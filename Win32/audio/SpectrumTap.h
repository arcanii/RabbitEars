// SPDX-License-Identifier: GPL-3.0-or-later
// SpectrumTap — a read-only audio frequency analyser for the spectrum MiniMeter.
//
// It captures THIS process's rendered audio via WASAPI *process-loopback*
// (AUDCLNT_PROCESS_LOOPBACK targeting our own PID), windows it, runs an FFT, and
// hands normalized log-spaced frequency bands to a sink callback. Because it is a
// loopback *capture*, it never touches libVLC's audio output — playback is wholly
// unaffected, and if activation fails (older Windows, no audio) it simply delivers
// nothing and the meter sits idle. Requires Windows 10 2004+ for process loopback.
#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include <windows.h>

namespace rabbitears {

class SpectrumTap {
public:
    static constexpr int kBands = 16;

    SpectrumTap() = default;
    ~SpectrumTap();
    SpectrumTap(const SpectrumTap&) = delete;
    SpectrumTap& operator=(const SpectrumTap&) = delete;

    // `sink` receives a pointer to kBands floats (0..1) each analysis window. It is
    // invoked on the capture thread, so it must be cheap and thread-safe
    // (miniMeterPushSpectrum is exactly that). Safe to call start() twice (no-op if
    // already running).
    void start(std::function<void(const float*)> sink);
    void stop();  // signals + joins the capture thread; idempotent
    bool running() const { return running_.load(); }

private:
    void run();

    std::function<void(const float*)> sink_;
    std::thread                       thread_;
    std::atomic<bool>                 running_{false};
    HANDLE                            stopEvt_ = nullptr;
};

}  // namespace rabbitears
