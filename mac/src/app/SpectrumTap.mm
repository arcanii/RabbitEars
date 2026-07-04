// SPDX-License-Identifier: GPL-3.0-or-later
// See SpectrumTap.h.
//
// Design: libVLC plays audio normally through its own Core Audio output; we do NOT
// sit in that path. Instead we create a Core Audio *process tap* (macOS 14.2+) on
// our own process, wrap it in a private aggregate device, and install an IOProc
// that receives a COPY of the output audio. On the real-time IOProc we mono-mix the
// samples into a fixed FIFO and, once a window has filled, run a windowed vDSP FFT
// and hand the resulting frequency bands to the meter. Because the tap is
// CATapUnmuted the real audio still reaches the speakers in sync — the tap only
// observes. That is what makes it desync-proof, unlike taking over libVLC's output.
//
// Real-time discipline: every buffer the FFT touches (fifo/window/scratch/bands) is
// a FIXED-SIZE member allocated once at init, so the IOProc never allocates, never
// locks, and never messages ObjC — it only calls the plain C++ SpectrumFft and the
// caller's handler block (whose job is a brief lock-guarded copy).
#import "SpectrumTap.h"

#import <Accelerate/Accelerate.h>
#import <CoreAudio/AudioHardware.h>
#import <CoreAudio/AudioHardwareTapping.h>   // AudioHardwareCreate/DestroyProcessTap (macOS 14.2+)
#import <CoreAudio/CATapDescription.h>

#include "platform/Log.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unistd.h>

namespace {

constexpr int   kFFTLog2   = 10;              // 1024-point FFT
constexpr int   kFFTN      = 1 << kFFTLog2;   // 1024 samples per window
constexpr int   kMaxBands  = 64;
constexpr float kFloorDb   = -66.0f;          // magnitude at/below this maps to 0 (tunable)

// A self-contained, allocation-free real-FFT spectrum analyser. Fed mono-ish PCM in
// arbitrary chunk sizes; every kFFTN samples it emits `bandCount` magnitudes (0..1,
// low→high, dB-scaled) through `handler`. All buffers are fixed members — no malloc,
// no locks on the audio thread. Owned by SpectrumTap (heap; new in init, delete in
// stop, after the IOProc is guaranteed stopped).
struct SpectrumFft {
    FFTSetup fftSetup = nullptr;
    int      bandCount = 24;
    int      fifoCount = 0;

    float    fifo[kFFTN];
    float    window[kFFTN];
    float    windowed[kFFTN];
    float    realp[kFFTN / 2];
    float    imagp[kFFTN / 2];
    float    mags[kFFTN / 2];
    float    bands[kMaxBands];
    int      bandLo[kMaxBands];
    int      bandHi[kMaxBands];

    void (^handler)(const float*, int) = nil;  // ARC-managed (this .mm is -fobjc-arc)

    bool init(int bc) {
        bandCount = std::clamp(bc, 1, kMaxBands);
        fftSetup = vDSP_create_fftsetup(kFFTLog2, kFFTRadix2);
        if (!fftSetup) return false;
        vDSP_hann_window(window, kFFTN, vDSP_HANN_NORM);
        computeBandEdges();
        return true;
    }

    ~SpectrumFft() {
        if (fftSetup) vDSP_destroy_fftsetup(fftSetup);
    }

    // Log-spaced band edges over the usable bins [1, N/2] (bin 0 = DC, skipped).
    void computeBandEdges() {
        const int nyq = kFFTN / 2;
        int edges[kMaxBands + 1];
        for (int b = 0; b <= bandCount; ++b) {
            const double frac = (double)b / bandCount;
            int e = (int)std::lround(std::pow((double)nyq, frac));  // 1 .. nyq
            edges[b] = std::clamp(e, 1, nyq);
        }
        for (int b = 0; b < bandCount; ++b) {
            bandLo[b] = edges[b];
            bandHi[b] = std::min(nyq, std::max(edges[b + 1], edges[b] + 1));  // ≥1 bin wide
        }
    }

    // RT thread: mono-mix a tapped buffer list and stream it through the FIFO.
    void feed(const AudioBufferList* abl) {
        if (!abl || abl->mNumberBuffers == 0) return;
        const AudioBuffer& b0 = abl->mBuffers[0];
        const int ch0 = (int)b0.mNumberChannels;
        if (ch0 == 0 || b0.mDataByteSize == 0) return;  // reject empty/format-less callbacks (we don't pin an ASBD)
        if (abl->mNumberBuffers == 1 && ch0 >= 1) {
            // One buffer: mono, or interleaved multichannel — average the channels.
            const float* s = (const float*)b0.mData;
            if (!s) return;
            const int frames = (int)(b0.mDataByteSize / sizeof(float) / (UInt32)ch0);
            for (int f = 0; f < frames; ++f) {
                float acc = 0.0f;
                for (int c = 0; c < ch0; ++c) acc += s[f * ch0 + c];
                pushSample(acc / (float)ch0);
            }
        } else {
            // Planar: one buffer per channel — average across buffers, frame by frame.
            const UInt32 nb = abl->mNumberBuffers;
            const int frames = (int)(b0.mDataByteSize / sizeof(float));
            for (int f = 0; f < frames; ++f) {
                float acc = 0.0f;
                int cnt = 0;
                for (UInt32 bi = 0; bi < nb; ++bi) {
                    const float* s = (const float*)abl->mBuffers[bi].mData;
                    if (s && (UInt32)f < abl->mBuffers[bi].mDataByteSize / sizeof(float)) {
                        acc += s[f];
                        ++cnt;
                    }
                }
                if (cnt) pushSample(acc / (float)cnt);
            }
        }
    }

    void pushSample(float x) {
        fifo[fifoCount++] = x;
        if (fifoCount >= kFFTN) {
            runWindow();
            fifoCount = 0;  // non-overlapping windows (~47 fps @ 48kHz — plenty for a meter)
        }
    }

    void runWindow() {
        vDSP_vmul(fifo, 1, window, 1, windowed, 1, kFFTN);
        DSPSplitComplex split = {realp, imagp};
        vDSP_ctoz((const DSPComplex*)windowed, 2, &split, 1, kFFTN / 2);
        vDSP_fft_zrip(fftSetup, &split, 1, kFFTLog2, kFFTDirection_Forward);
        split.imagp[0] = 0.0f;  // drop the packed Nyquist term that rides in imagp[0]
        vDSP_zvabs(&split, 1, mags, 1, kFFTN / 2);

        for (int b = 0; b < bandCount; ++b) {
            float m = 0.0f;
            for (int k = bandLo[b]; k < bandHi[b]; ++k)
                if (mags[k] > m) m = mags[k];
            const float amp = m * (2.0f / (float)kFFTN);       // undo vDSP_fft_zrip's 2N scaling
            const float db = 20.0f * std::log10(amp + 1e-9f);
            const float lvl = (db - kFloorDb) / (0.0f - kFloorDb);  // [floor .. 0] dB -> [0 .. 1]
            // A NaN/Inf sample (underrun/garbage) would survive std::clamp and poison the
            // meter's easing state permanently (and make lround() UB downstream) — floor it.
            bands[b] = std::isfinite(lvl) ? std::clamp(lvl, 0.0f, 1.0f) : 0.0f;
        }
        if (handler) handler(bands, bandCount);
    }
};

}  // namespace

@interface SpectrumTap ()
- (AudioObjectID)processObjectForCurrentPID;
- (BOOL)start API_AVAILABLE(macos(14.2));
@end

@implementation SpectrumTap {
    AudioObjectID       _tapID;      // the process tap
    AudioObjectID       _aggregate;  // private aggregate device wrapping the tap
    AudioDeviceIOProcID _ioProc;     // installed on _aggregate
    SpectrumFft*        _fft;        // RT-thread FFT state (owned)
    BOOL                _running;
}

- (nullable instancetype)initWithBandCount:(int)bandCount
                           spectrumHandler:(void (^)(const float*, int))handler {
    if (!(self = [super init])) return nil;
    _tapID = kAudioObjectUnknown;
    _aggregate = kAudioObjectUnknown;
    _ioProc = NULL;
    _running = NO;
    _fft = new SpectrumFft();
    if (!_fft->init(bandCount)) {
        rabbitears::diag::error(L"SpectrumTap: FFT setup failed");
        [self stop];
        return nil;
    }
    _fft->handler = [handler copy];
    if (@available(macOS 14.2, *)) {
        if ([self start]) return self;
    }
    // Older macOS, or the tap couldn't be built — clean up and report no meter.
    [self stop];
    return nil;
}

// Translate our PID into the Core Audio process AudioObjectID a tap references.
- (AudioObjectID)processObjectForCurrentPID {
    pid_t pid = getpid();
    AudioObjectID obj = kAudioObjectUnknown;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyTranslatePIDToProcessObject,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(obj);
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                             sizeof(pid), &pid, &size, &obj);
    return (st == noErr) ? obj : kAudioObjectUnknown;
}

- (BOOL)start {
    // 1. A stereo-mixdown tap of THIS process, UNMUTED so playback still reaches the
    //    speakers — we only observe.
    AudioObjectID proc = [self processObjectForCurrentPID];
    if (proc == kAudioObjectUnknown) {
        rabbitears::diag::error(L"SpectrumTap: could not resolve this process's audio object");
        return NO;
    }

    CATapDescription* desc =
        [[CATapDescription alloc] initStereoMixdownOfProcesses:@[ @(proc) ]];
    desc.name = @"RabbitEars Spectrum Meter";
    desc.privateTap = YES;             // visible only to us
    desc.muteBehavior = CATapUnmuted;  // keep audio audible (also the default)

    OSStatus st = AudioHardwareCreateProcessTap(desc, &_tapID);
    if (st != noErr || _tapID == kAudioObjectUnknown) {
        rabbitears::diag::error(L"SpectrumTap: AudioHardwareCreateProcessTap failed (status "
                                + std::to_wstring(st) + L") — possibly an audio-capture consent issue");
        return NO;
    }

    // 2. The tap's UID, to reference it from a private aggregate device.
    CFStringRef tapUID = NULL;
    AudioObjectPropertyAddress uidAddr = {
        kAudioTapPropertyUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 uidSize = sizeof(tapUID);
    st = AudioObjectGetPropertyData(_tapID, &uidAddr, 0, NULL, &uidSize, &tapUID);
    if (st != noErr || !tapUID) {
        rabbitears::diag::error(L"SpectrumTap: could not read the tap UID (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }
    NSString* tapUIDStr = (__bridge_transfer NSString*)tapUID;

    // 3. A PRIVATE aggregate device containing only this tap, so we can install an
    //    IOProc and receive the tapped audio as its input.
    NSString* aggUID = [NSString stringWithFormat:@"com.rabbitears.spectrumtap.%d", getpid()];
    NSDictionary* aggDesc = @{
        @(kAudioAggregateDeviceNameKey):      @"RabbitEars Spectrum Aggregate",
        @(kAudioAggregateDeviceUIDKey):       aggUID,
        @(kAudioAggregateDeviceIsPrivateKey): @YES,
        @(kAudioAggregateDeviceTapListKey):   @[ @{@(kAudioSubTapUIDKey): tapUIDStr} ],
    };
    st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggDesc, &_aggregate);
    if (st != noErr || _aggregate == kAudioObjectUnknown) {
        rabbitears::diag::error(L"SpectrumTap: AudioHardwareCreateAggregateDevice failed (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }

    // 4. IOProc: FFT the tapped buffers → the band handler. Runs on a real-time audio
    //    thread; the block captures only the plain C++ SpectrumFft pointer (no ObjC
    //    messaging, no retain cycle). stop() calls AudioDeviceStop (synchronous)
    //    before deleting _fft, so no IOProc is ever in flight against a freed FFT.
    SpectrumFft* fft = _fft;
    st = AudioDeviceCreateIOProcIDWithBlock(&_ioProc, _aggregate, NULL,
        ^(const AudioTimeStamp* inNow, const AudioBufferList* inInputData,
          const AudioTimeStamp* inInputTime, AudioBufferList* outOutputData,
          const AudioTimeStamp* inOutputTime) {
            (void)inNow; (void)inInputTime; (void)outOutputData; (void)inOutputTime;
            if (inInputData) fft->feed(inInputData);
        });
    if (st != noErr || !_ioProc) {
        rabbitears::diag::error(L"SpectrumTap: AudioDeviceCreateIOProcIDWithBlock failed (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }

    st = AudioDeviceStart(_aggregate, _ioProc);
    if (st != noErr) {
        rabbitears::diag::error(L"SpectrumTap: AudioDeviceStart failed (status "
                                + std::to_wstring(st) + L")");
        return NO;
    }
    _running = YES;
    rabbitears::diag::info(L"SpectrumTap: spectrum metering started");
    return YES;
}

- (void)stop {
    if (@available(macOS 14.2, *)) {
        if (_aggregate != kAudioObjectUnknown && _ioProc) {
            if (_running) AudioDeviceStop(_aggregate, _ioProc);  // synchronous: no IOProc after this
            AudioDeviceDestroyIOProcID(_aggregate, _ioProc);
        }
        if (_aggregate != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(_aggregate);
        if (_tapID != kAudioObjectUnknown)     AudioHardwareDestroyProcessTap(_tapID);
    }
    _ioProc = NULL;
    _aggregate = kAudioObjectUnknown;
    _tapID = kAudioObjectUnknown;
    _running = NO;
    // Safe now — AudioDeviceStop above guarantees no IOProc is running against _fft.
    delete _fft;
    _fft = nullptr;
}

- (void)dealloc {
    [self stop];
}

@end
