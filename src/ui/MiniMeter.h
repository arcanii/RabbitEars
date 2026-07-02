// SPDX-License-Identifier: GPL-3.0-or-later
// MiniMeter — a small, modular "LED dot-matrix" meter, a compact sibling of the
// big fluid BufferMeter. One control class renders several kinds so the user can
// mix-and-match which ones show (Settings → Meters):
//   * Spectrum — a live audio frequency analyser (fed from the WASAPI SpectrumTap;
//     bands ease up fast, fall slow, with peak-hold caps).
//   * Signal   — antenna-style strength bars (composite of stream health).
//   * Bitrate  — a scrolling history of stream throughput.
//   * Frames   — displayed frame-rate with a red flare on dropped frames.
// Each is drawn as small square LEDs (lit/dim cells) to match the family look.
#pragma once

#include <windows.h>

namespace rabbitears {

enum class MeterKind { Spectrum, Signal, Bitrate, Frames };

void registerMiniMeterClass(HINSTANCE hInst);
HWND createMiniMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi, MeterKind kind);
MeterKind miniMeterKind(HWND meter);

// Spectrum: push normalized band magnitudes (each 0..1). THREAD-SAFE — called from
// the audio capture thread; the values are latched under a lock and consumed by the
// control's animation timer on the UI thread.
void miniMeterPushSpectrum(HWND meter, const float* bands, int count);

// Signal: strength 0..1 (bars lit) and trouble 0..1 (tints the lit bars red). UI thread.
void miniMeterSetSignal(HWND meter, float strength, float trouble);

// Bitrate: append one throughput sample (bytes/sec) to the scrolling graph. UI thread.
void miniMeterPushBitrate(HWND meter, double bytesPerSec);

// Frames: displayed fps + dropped-frame count since the last sample (>0 flares red). UI thread.
void miniMeterSetFrames(HWND meter, int fps, int dropsDelta);

// Clear all animated state back to idle (e.g. on Stop). UI thread.
void miniMeterReset(HWND meter);

void miniMeterSetDpi(HWND meter, UINT dpi);

}  // namespace rabbitears
