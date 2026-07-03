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

#include <string>

#include <windows.h>

namespace rabbitears {

enum class MeterKind { Spectrum, Signal, Bitrate, Frames };

// The visual "look" of a meter (Settings → Meters…). LED is the classic dot-matrix;
// the others are added incrementally — an unimplemented look renders as LED.
enum class MeterStyle { Led, Tube, Lcd, Scope };

// Fully customizable per-meter colour palette. The roles map onto the "how much is
// lit" math so every look can honour them. `bg == CLR_INVALID` means "follow the theme's
// window background". Defaults reproduce the classic look exactly.
struct MeterPalette {
    COLORREF bg;      // panel background (CLR_INVALID = theme windowBg)
    COLORREF off;     // unlit / dim cell
    COLORREF low;     // low band of the lit ramp        (default green)
    COLORREF mid;     // mid band                         (default amber)
    COLORREF high;    // high band + alert/trouble tint   (default red)
    COLORREF accent;  // bitrate history fill / scope trace (default coral)
    COLORREF peak;    // peak-hold cap / trace head        (default near-white)
};

inline MeterPalette defaultMeterPalette(MeterKind /*kind*/) {
    return MeterPalette{CLR_INVALID, RGB(38, 40, 44), RGB(96, 205, 128), RGB(232, 188, 86),
                        RGB(232, 96, 86), RGB(217, 119, 87), RGB(236, 236, 240)};
}
inline MeterStyle defaultMeterStyle(MeterKind /*kind*/) { return MeterStyle::Led; }

// One meter's full configuration (enable + look + palette). Indexed by MeterKind in
// the Settings → Meters… dialog and in persistence.
struct MeterConfig {
    bool         enabled = true;
    MeterStyle   style = MeterStyle::Led;
    MeterPalette palette = defaultMeterPalette(MeterKind::Spectrum);
};

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

// ---- Look & palette (Settings → Meters…) -----------------------------------
void miniMeterSetStyle(HWND meter, MeterStyle style);
void miniMeterSetPalette(HWND meter, const MeterPalette& palette);
MeterStyle   miniMeterStyle(HWND meter);
MeterPalette miniMeterPalette(HWND meter);

// Serialize a style/palette for the settings K/V store (persisted per meter). The
// palette is 7 comma-joined tokens (bg first — "theme" for CLR_INVALID, else RRGGBB);
// parsing falls back to `fallback` for any missing/garbled field.
std::wstring meterStyleToString(MeterStyle style);
MeterStyle   meterStyleFromString(const std::wstring& s, MeterStyle fallback);
std::wstring meterPaletteToString(const MeterPalette& p);
MeterPalette meterPaletteFromString(const std::wstring& s, const MeterPalette& fallback);

}  // namespace rabbitears
