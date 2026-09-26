// SPDX-License-Identifier: GPL-3.0-or-later
// SpectrumTap — a read-only audio frequency analyser for the spectrum MiniMeter.
//
// It captures THIS process's rendered audio via WASAPI *process-loopback*
// (AUDCLNT_PROCESS_LOOPBACK targeting our own PID), windows it, runs an FFT, and
// hands normalized log-spaced frequency bands — and the window's level, for the
// audio meter's needle — to a sink callback. Because it is a
// loopback *capture*, it never touches libVLC's audio output — playback is wholly
// unaffected, and if activation fails (older Windows, no audio) it simply delivers
// nothing and the meter sits idle. Requires Windows 10 2004+ for process loopback.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

#include <windows.h>

namespace rabbitears {

class SpectrumTap {
public:
    static constexpr int kBands = 16;

    // A window's level: the RMS of its louder channel (`sumSquares` over `n` samples) in dBFS,
    // sine-calibrated (+3.01 dB, so a full-scale sine reads 0 dBFS, as a VU alignment tone does);
    // silence, or anything under it, reads kSilenceDbfs. Pure, so --selftest pins it.
    static constexpr float kSilenceDbfs = -120.0f;
    static float rmsDbfs(double sumSquares, int n) {
        if (n <= 0 || !(sumSquares > 0.0)) return kSilenceDbfs;
        const double db = 20.0 * std::log10(std::sqrt(sumSquares / n)) + 3.0103;
        return static_cast<float>(std::max(db, static_cast<double>(kSilenceDbfs)));
    }

    // The PROGRAMME's level, not what is heard: the owner saw the needle drop with the app's volume slider,
    // so the capture comes after the volume Windows applies to our audio session (which libVLC's output is
    // believed to set from the slider — the code reads the session's real volume, so it does not depend on
    // that). The captured level `db` is raised by what `sessionVolume` (0..1) took off, never past full
    // scale (a full-scale square wave, +3.01 dBFS, is as loud as a programme can be). A session muted or
    // under -100 dB leaves nothing to raise: the floor, as silence is. Pure, so --selftest pins it.
    static constexpr float kFullScaleDbfs = 3.0103f;
    static constexpr float kQuietestVolume = 1e-5f;  // -100 dB
    static float programmeDbfs(float db, float sessionVolume) {
        if (db <= kSilenceDbfs || !(sessionVolume > kQuietestVolume)) return kSilenceDbfs;  // (a NaN volume too)
        if (sessionVolume >= 1.0f) return db;
        return std::min(db - 20.0f * std::log10(sessionVolume), kFullScaleDbfs);
    }

    SpectrumTap() = default;
    ~SpectrumTap();
    SpectrumTap(const SpectrumTap&) = delete;
    SpectrumTap& operator=(const SpectrumTap&) = delete;

    // `sink` receives a pointer to kBands floats (0..1) and the window's level — the
    // programme's (rmsDbfs, then programmeDbfs by our session's volume, read every window)
    // — each analysis window (~21 ms). The bands are as captured (after the volume). It is invoked on the capture thread, so it must be
    // cheap and thread-safe (miniMeterPushSpectrum / miniMeterPushLevel are exactly
    // that). Safe to call start() twice (no-op if already running).
    void start(std::function<void(const float* bands, float levelDbfs)> sink);
    void stop();  // signals + joins the capture thread; idempotent
    bool running() const { return running_.load(); }

private:
    void run();

    std::function<void(const float*, float)> sink_;
    std::thread                              thread_;
    std::atomic<bool>                        running_{false};
    HANDLE                                   stopEvt_ = nullptr;
};

}  // namespace rabbitears
