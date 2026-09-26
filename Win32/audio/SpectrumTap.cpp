// SPDX-License-Identifier: GPL-3.0-or-later
// Process-loopback capture (AUDCLNT_PROCESS_LOOPBACK) needs a Windows-11-era NTDDI
// to expose AUDIOCLIENT_ACTIVATION_PARAMS and friends; the project-wide NTDDI is
// older, so raise it just for this translation unit before any Windows header is
// pulled in (SpectrumTap.h includes <windows.h>). Activation still fails gracefully
// at runtime on older Windows — the meter simply stays idle.
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000C
#include "audio/SpectrumTap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>  // IAudioSessionManager2 — our session's volume (the audio meter's level)
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objidl.h>  // IAgileObject — the async-activation completion handler must be agile
#include <propidl.h>

#include "platform/Log.h"

#pragma comment(lib, "mmdevapi.lib")

namespace rabbitears {
namespace {

constexpr int    kFftSize = 1024;   // ~21ms window at 48kHz
constexpr int    kRate = 48000;
constexpr int    kChannels = 2;
constexpr double kPi = 3.14159265358979323846;

// The volume of an audio session (0..1; 0 while muted), or -1 when it cannot be read.
float volumeOf(ISimpleAudioVolume* vol) {
    float v = 1.0f;
    BOOL mute = FALSE;
    if (!vol || FAILED(vol->GetMasterVolume(&v)) || FAILED(vol->GetMute(&mute)) || !std::isfinite(v)) return -1.0f;
    return mute ? 0.0f : std::clamp(v, 0.0f, 1.0f);
}

// THIS process's audio session on the default render device — the console role, as libVLC's output
// picks its device — whose volume Windows applies to what the loopback captures: its ISimpleAudioVolume
// (the caller releases it), or null when there is none (then nothing is compensated). Of several sessions
// of ours, the loudest ACTIVE one (else the loudest). Found every ~2 s on the capture thread (COM is
// initialised there); the volume itself is read from it every window, so a moved slider counts at once.
ISimpleAudioVolume* findOurSession(IMMDeviceEnumerator* en) {
    ISimpleAudioVolume* best = nullptr;
    IMMDevice* dev = nullptr;
    if (!en || FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) return best;
    IAudioSessionManager2* mgr = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) &&
        mgr) {
        IAudioSessionEnumerator* sessions = nullptr;
        if (SUCCEEDED(mgr->GetSessionEnumerator(&sessions)) && sessions) {
            int count = 0;
            if (FAILED(sessions->GetCount(&count))) count = 0;
            const DWORD pid = GetCurrentProcessId();
            float bestVol = -1.0f;
            bool bestActive = false;
            for (int i = 0; i < count; ++i) {
                IAudioSessionControl* ctl = nullptr;
                if (FAILED(sessions->GetSession(i, &ctl)) || !ctl) continue;
                IAudioSessionControl2* ctl2 = nullptr;
                DWORD spid = 0;
                if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctl2))) &&
                    ctl2) {
                    if (SUCCEEDED(ctl2->GetProcessId(&spid)) && spid == pid) {
                        ISimpleAudioVolume* vol = nullptr;
                        if (SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&vol))) &&
                            vol) {
                            const float v = volumeOf(vol);
                            AudioSessionState state = AudioSessionStateInactive;
                            const bool active = SUCCEEDED(ctl->GetState(&state)) && state == AudioSessionStateActive;
                            // An active session beats any inactive one; among equals, the louder.
                            if (v >= 0.0f && ((active && !bestActive) || (active == bestActive && v > bestVol))) {
                                if (best) best->Release();
                                best = vol;  // keeps the QueryInterface reference
                                vol = nullptr;
                                bestVol = v;
                                bestActive = active;
                            }
                            if (vol) vol->Release();
                        }
                    }
                    ctl2->Release();
                }
                ctl->Release();
            }
            sessions->Release();
        }
        mgr->Release();
    }
    dev->Release();
    return best;
}

// In-place iterative radix-2 Cooley-Tukey FFT.
void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / len;
        const float wr = static_cast<float>(std::cos(ang));
        const float wi = static_cast<float>(std::sin(ang));
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0f, cwi = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float ur = re[a], ui = im[a];
                const float vr = re[b] * cwr - im[b] * cwi;
                const float vi = re[b] * cwi + im[b] * cwr;
                re[a] = ur + vr;
                im[a] = ui + vi;
                re[b] = ur - vr;
                im[b] = ui - vi;
                const float ncwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = ncwr;
            }
        }
    }
}

// A minimal hand-rolled completion handler so we can wait for the async activation
// synchronously on the capture thread.
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE       done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT      resultHr = E_FAIL;
    IAudioClient* client = nullptr;  // ownership transferred out by the caller

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activateHr = E_FAIL;
        IUnknown* punk = nullptr;
        const HRESULT hr = op->GetActivateResult(&activateHr, &punk);
        if (SUCCEEDED(hr) && SUCCEEDED(activateHr) && punk)
            punk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
        resultHr = FAILED(hr) ? hr : activateHr;
        if (punk) punk->Release();
        SetEvent(done);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        // Answer IAgileObject: ActivateAudioInterfaceAsync marshals the completion
        // handler across apartments and rejects a non-agile handler *synchronously*
        // with E_ILLEGAL_METHOD_CALL (0x8000000E) when invoked from our MTA capture
        // thread — which is exactly why the spectrum meter was dead. IAgileObject is a
        // pure marker (IUnknown vtable), so returning our own IUnknown head is agile.
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IAgileObject) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            if (done) CloseHandle(done);
            delete this;
        }
        return r;
    }

private:
    LONG ref_ = 1;
};

// Activate a process-loopback IAudioClient for our own process tree. Returns null
// on any failure (older Windows, denied, no default device, etc.).
IAudioClient* activateProcessLoopback() {
    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    ap.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);

    ActivateHandler* handler = new ActivateHandler();
    if (!handler->done) {
        handler->Release();
        return nullptr;
    }
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                             __uuidof(IAudioClient), &pv, handler, &op);
    IAudioClient* client = nullptr;
    if (SUCCEEDED(hr)) {
        WaitForSingleObject(handler->done, INFINITE);
        if (SUCCEEDED(handler->resultHr)) {
            client = handler->client;  // transfer ownership
            handler->client = nullptr;
        } else {
            wchar_t b[96];
            swprintf_s(b, L"SpectrumTap: process-loopback activation failed (hr=0x%08lX)",
                       static_cast<unsigned long>(handler->resultHr));
            diag::warn(b);
        }
    } else {
        wchar_t b[96];
        swprintf_s(b, L"SpectrumTap: ActivateAudioInterfaceAsync failed (hr=0x%08lX)",
                   static_cast<unsigned long>(hr));
        diag::warn(b);
    }
    if (op) op->Release();
    handler->Release();
    return client;
}

}  // namespace

SpectrumTap::~SpectrumTap() { stop(); }

void SpectrumTap::start(std::function<void(const float*, float)> sink) {
    if (running_.load()) return;             // a worker is already active
    if (thread_.joinable()) thread_.join();  // reap a previously-finished (e.g. failed) thread
    if (stopEvt_) {
        CloseHandle(stopEvt_);
        stopEvt_ = nullptr;
    }
    sink_ = std::move(sink);
    stopEvt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void SpectrumTap::stop() {
    if (stopEvt_) SetEvent(stopEvt_);
    if (thread_.joinable()) thread_.join();
    if (stopEvt_) {
        CloseHandle(stopEvt_);
        stopEvt_ = nullptr;
    }
    running_.store(false);
}

void SpectrumTap::run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    diag::info(L"SpectrumTap: capture thread starting");

    IAudioClient*        client = activateProcessLoopback();
    IAudioCaptureClient* capture = nullptr;
    HANDLE               sampleEvt = nullptr;
    bool                 started = false;

    if (client) {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wfx.nChannels = kChannels;
        wfx.nSamplesPerSec = kRate;
        wfx.wBitsPerSample = 32;
        wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        wfx.cbSize = 0;

        // 200ms shared capture buffer, event-driven. Process-loopback requires the
        // LOOPBACK + EVENTCALLBACK flags and a periodicity of 0.
        HRESULT hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, &wfx,
            nullptr);
        if (SUCCEEDED(hr)) {
            sampleEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            hr = client->SetEventHandle(sampleEvt);
        }
        if (SUCCEEDED(hr))
            hr = client->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void**>(&capture));
        if (SUCCEEDED(hr)) hr = client->Start();
        started = SUCCEEDED(hr);
        if (!started) {
            wchar_t b[96];
            swprintf_s(b, L"SpectrumTap: WASAPI init failed (hr=0x%08lX) — spectrum meter idle",
                       static_cast<unsigned long>(hr));
            diag::warn(b);
        } else {
            diag::info(L"SpectrumTap: capturing this process's audio (process loopback, 48kHz stereo)");
        }
    } else {
        diag::warn(L"SpectrumTap: process-loopback unavailable (older Windows?) — spectrum meter idle");
    }

    if (started) {
        // Precompute the Hann window.
        std::vector<float> hann(kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            hann[i] = 0.5f * (1.0f - static_cast<float>(std::cos(2.0 * kPi * i / (kFftSize - 1))));

        std::vector<float> win(kFftSize, 0.0f);  // mono accumulation window
        int wpos = 0;
        // The window's level for the audio meter's needle: each channel's sum of squares (the
        // louder one's RMS is what a VU meter on the programme would read — before any Hann
        // weighting, which only the FFT wants).
        double sumL = 0.0, sumR = 0.0;
        // Our session's volume (programmeDbfs adds back what it took off the level).
        IMMDeviceEnumerator* devices = nullptr;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                         reinterpret_cast<void**>(&devices));
        ISimpleAudioVolume* ourSession = nullptr;
        ULONGLONG nextSessionFind = 0;
        float loggedVolDb = 1.0f;  // (no volume is > 0 dB: the first reading is logged)
        std::vector<float> re(kFftSize), im(kFftSize);
        float bands[SpectrumTap::kBands];
        bool firstWindow = true;

        HANDLE waits[2] = {stopEvt_, sampleEvt};
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) break;  // stop signaled

            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE*  data = nullptr;
                UINT32 frames = 0;
                DWORD  flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const float* f = (silent || !data) ? nullptr : reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    const float l = f ? f[i * 2] : 0.0f, r = f ? f[i * 2 + 1] : 0.0f;
                    const float mono = 0.5f * (l + r);
                    sumL += static_cast<double>(l) * l;
                    sumR += static_cast<double>(r) * r;
                    win[wpos++] = mono;
                    if (wpos >= kFftSize) {
                        for (int k = 0; k < kFftSize; ++k) {
                            re[k] = win[k] * hann[k];
                            im[k] = 0.0f;
                        }
                        fft(re.data(), im.data(), kFftSize);
                        // Log-spaced bands from 40Hz..16kHz, peak magnitude per band,
                        // normalized from dB (-72..-12 -> 0..1).
                        const double binHz = static_cast<double>(kRate) / kFftSize;
                        for (int b = 0; b < SpectrumTap::kBands; ++b) {
                            const double f0 = 40.0 * std::pow(16000.0 / 40.0,
                                                              static_cast<double>(b) / SpectrumTap::kBands);
                            const double f1 = 40.0 * std::pow(16000.0 / 40.0,
                                                              static_cast<double>(b + 1) / SpectrumTap::kBands);
                            int lo = std::clamp(static_cast<int>(f0 / binHz), 1, kFftSize / 2);
                            int hi = std::clamp(static_cast<int>(f1 / binHz), lo + 1, kFftSize / 2);
                            float mx = 0.0f;
                            for (int k = lo; k < hi; ++k) {
                                const float mag = std::sqrt(re[k] * re[k] + im[k] * im[k]) / kFftSize;
                                if (mag > mx) mx = mag;
                            }
                            const float db = 20.0f * std::log10(mx + 1e-9f);
                            bands[b] = std::clamp((db + 72.0f) / 60.0f, 0.0f, 1.0f);
                        }
                        // Our session: found every ~2 s (device or session changes), its volume read every
                        // window — a moved slider counts at once, never a stale volume kicking the needle.
                        const ULONGLONG now = GetTickCount64();
                        if (now >= nextSessionFind) {
                            nextSessionFind = now + 2000;
                            if (ourSession) ourSession->Release();
                            ourSession = findOurSession(devices);
                        }
                        const float v = volumeOf(ourSession);
                        const float sessionVol = v < 0.0f ? 1.0f : v;  // none found: nothing compensated
                        const float volDb = sessionVol > kQuietestVolume ? 20.0f * std::log10(sessionVol) : -999.0f;
                        if (std::fabs(volDb - loggedVolDb) >= 0.5f) {  // a change of half a dB or more
                            loggedVolDb = volDb;
                            wchar_t b[128];
                            if (volDb > -999.0f)
                                swprintf_s(b, L"SpectrumTap: our audio session's volume is %.4f — the audio meter "
                                              L"adds back %.1f dB", sessionVol, -volDb);
                            else
                                swprintf_s(b, L"SpectrumTap: our audio session is muted or silent — the audio "
                                              L"meter's needle rests");
                            diag::info(b);
                        }
                        // (fmax: a NaN on one channel does not silence the other.)
                        if (sink_)
                            sink_(bands, programmeDbfs(rmsDbfs(std::fmax(sumL, sumR), kFftSize), sessionVol));
                        sumL = sumR = 0.0;
                        if (firstWindow) {
                            diag::info(L"SpectrumTap: first audio window analysed (audio is flowing)");
                            firstWindow = false;
                        }
                        wpos = 0;
                    }
                }
                capture->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        if (ourSession) ourSession->Release();
        if (devices) devices->Release();
    }

    if (capture) capture->Release();
    if (client) client->Release();
    if (sampleEvt) CloseHandle(sampleEvt);
    if (SUCCEEDED(co)) CoUninitialize();
    diag::info(L"SpectrumTap: capture thread stopped");
    // Clear on EVERY exit (incl. early activation/init failure) so running() stays
    // honest and syncSpectrumTap() can retry later instead of believing a dead tap
    // is still alive.
    running_.store(false);
}

}  // namespace rabbitears
