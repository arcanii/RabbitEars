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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

#include "ui/VuDial.h"  // VuFace, vuDialNaturalWidth — the needle looks' faces

namespace rabbitears {

enum class MeterKind { Spectrum, Signal, Bitrate, Frames };

// The visual "look" of a meter (Settings → Meters…). LED is the classic dot-matrix;
// the others are added incrementally — an unimplemented look renders as LED.
// Vu is the classic analog needle gauge — since 0.2.18 a backlit desktop VU meter (ui/VuDial.h;
// it was a Phase Linear 400-style full-width dial before): a lamp-lit card, a damped needle. Appended LAST — the codec is token-based so order is not persisted, but
// mac carries its own copy of this enum (mac/src/app/MeterModel.h) WITHOUT Vu, and its parser falls
// back on the unknown "vu" token. Adding it there is a mac-team change, flagged in BACKLOG.md.
// VuSilver is the second analog instrument (a silver cassette-deck meter; ui/VuDial.h) — appended
// LAST for the same reason, and mac's parser falls back on its "vu_silver" token the same way.
enum class MeterStyle { Led, Tube, Lcd, Scope, Vu, VuSilver };

// The two needle looks share everything but their face: `bg` is their lamp, they have no cells, and
// the Meters dialog gives them the same knobs.
inline bool isVuLook(MeterStyle s) { return s == MeterStyle::Vu || s == MeterStyle::VuSilver; }
inline VuFace vuFaceOf(MeterStyle s) { return s == MeterStyle::VuSilver ? VuFace::Silver : VuFace::Backlit; }

// The chrome band a meter reserves around its dial: a 1px themed FrameRect plus the rest of the
// pad. Used for BOTH the content inset AND the glass mask's frame width, so the two can never
// disagree — the bezel is painted exactly here, which is why it costs zero dial pixels at any DPI
// and at any meter size (the standard tray's meters are dp(30) tall, taller ones up to dp(120); the
// Settings previews are dp(86)).
inline int meterChromePx(UINT dpi) { return MulDiv(2, static_cast<int>(dpi), 96); }

// The width (px) a meter `meterPx` tall takes when its look has a width of its own — a needle look: its
// instrument at the face's own proportions (vuDialNaturalWidth) inside the meter's chrome — else 0, and
// it fills whatever width its kind gets (the cell looks, Scope). The own meter row (layout()) and the
// meter bridge size needle meters by it, so two meters of one face match whatever their kind; the
// standard tray keeps every meter at its kind's width, as it always has.
inline int miniMeterNaturalWidth(MeterStyle style, int meterPx, UINT dpi) {
    const int chrome = meterChromePx(dpi), h = meterPx - 2 * chrome;
    if (!isVuLook(style) || h < 8) return 0;  // drawVu draws nothing in a dial under 8 px
    return vuDialNaturalWidth(vuFaceOf(style), h) + 2 * chrome;
}

// Fully customizable per-meter colour palette. The roles map onto the "how much is
// lit" math so every look can honour them. `bg == CLR_INVALID` means "follow the theme's
// window background". Defaults reproduce the classic look exactly.
struct MeterPalette {
    COLORREF bg;      // panel background (CLR_INVALID = theme windowBg); on the Vu look this is
                      // instead the LAMP behind the dial — hue only, CLR_INVALID (or any colour
                      // too dark to be a lamp, so: pick black to get back here) = the stock bulb
    COLORREF off;     // unlit / dim cell (stock value adapts to a light panel — meterDrawnPalette)
    COLORREF low;     // low band of the lit ramp        (default green)
    COLORREF mid;     // mid band                         (default amber)
    COLORREF high;    // high band + alert/trouble tint + the Vu red zone (default red)
    COLORREF accent;  // bitrate history fill / scope trace (default coral)
    COLORREF peak;    // peak-hold cap / trace head        (default near-white; adapts, like `off`)
};

inline MeterPalette defaultMeterPalette(MeterKind /*kind*/) {
    return MeterPalette{CLR_INVALID, RGB(38, 40, 44), RGB(96, 205, 128), RGB(232, 188, 86),
                        RGB(232, 96, 86), RGB(217, 119, 87), RGB(236, 236, 240)};
}
inline MeterStyle defaultMeterStyle(MeterKind /*kind*/) { return MeterStyle::Led; }

// Per-meter "feel" knobs (Settings → Meters…), all normalized 0..1 with 0.5 as the
// neutral default that reproduces the classic behaviour exactly. Which ones matter
// depends on the meter/look: glow → Tube/Scope bloom; smoothing → attack/decay easing
// (spectrum/signal/frames); sensitivity → input gain (all); peakHold → spectrum peak
// linger; breathing → bitrate adaptive-ceiling ebb.
struct MeterTuning {
    float glow;         // Tube/Scope bloom intensity
    float smoothing;    // attack/decay easing (higher = smoother)
    float sensitivity;  // input gain (0.5 = unity; on the audio needle ±12 dB — vuReadingOfDbfs)
    float peakHold;     // spectrum peak-cap linger
    float breathing;    // bitrate ceiling re-normalization speed
};
inline MeterTuning defaultMeterTuning() { return MeterTuning{0.5f, 0.5f, 0.5f, 0.5f, 0.5f}; }

// One meter's full configuration (enable + look + palette + knobs). Indexed by MeterKind
// in the Settings → Meters… dialog and in persistence.
struct MeterConfig {
    bool         enabled = true;
    MeterStyle   style = MeterStyle::Led;
    MeterPalette palette = defaultMeterPalette(MeterKind::Spectrum);
    MeterTuning  tuning = defaultMeterTuning();
};

void registerMiniMeterClass(HINSTANCE hInst);
HWND createMiniMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi, MeterKind kind);
MeterKind miniMeterKind(HWND meter);

// Spectrum: push normalized band magnitudes (each 0..1). THREAD-SAFE — called from
// the audio capture thread; the values are latched under a lock and consumed by the
// control's animation timer on the UI thread.
void miniMeterPushSpectrum(HWND meter, const float* bands, int count);

// Spectrum, too: the programme's level in dBFS (SpectrumTap::programmeDbfs), for the audio meter's NEEDLE —
// a needle look on the Spectrum meter reads it as a VU meter does (vuReadingOfDbfs); the cell looks keep
// drawing the bands. Without it (a feed that never pushes one) the needle falls back to the bands' mean.
// THREAD-SAFE, like miniMeterPushSpectrum: the loudest level pushed between two ticks is the one read.
void miniMeterPushLevel(HWND meter, float dbfs);

// The audio needle's reading in VU for a level in dBFS: 0 VU at kVuReferenceDbfs (the EBU digital
// alignment level), the Sens knob shifting it evenly in dB — ±kVuSensSpanDb at its ends, 0 at 0.5 — so a
// quiet source can be brought up as far as a loud one brought down.
constexpr float kVuReferenceDbfs = -18.0f;
constexpr float kVuSensSpanDb = 12.0f;
inline float vuReadingOfDbfs(float dbfs, float sensitivity) {
    return dbfs - kVuReferenceDbfs + (sensitivity - 0.5f) * 2.0f * kVuSensSpanDb;
}

// Signal: strength 0..1 (bars lit) and trouble 0..1 (tints the lit bars red). UI thread.
void miniMeterSetSignal(HWND meter, float strength, float trouble);

// Bitrate: append one throughput sample (bytes/sec) to the scrolling graph. UI thread.
void miniMeterPushBitrate(HWND meter, double bytesPerSec);

// Frames: displayed fps + dropped-frame count since the last sample (>0 flares red). UI thread.
void miniMeterSetFrames(HWND meter, int fps, int dropsDelta);

// Clear all animated state back to idle (e.g. on Stop). UI thread.
void miniMeterReset(HWND meter);

void miniMeterSetDpi(HWND meter, UINT dpi);

// Mirror everything this meter is given — its data (every push/set above, the spectrum included),
// resets, and its look, palette and tuning — onto `mirror`, a second meter of the same kind elsewhere
// (the pop-out meter bridge), so no caller feeds two meters; linking also starts it with this meter's
// Bitrate history so far. Not its DPI (each window has its own).
// nullptr unlinks. THREAD-SAFE against the spectrum push: set and cleared under the meter's lock, so
// once this returns with nullptr no forward to the old mirror is in flight — unlink BEFORE destroying it.
void miniMeterSetMirror(HWND meter, HWND mirror);

// ---- Look & palette (Settings → Meters…) -----------------------------------
void miniMeterSetStyle(HWND meter, MeterStyle style);
void miniMeterSetPalette(HWND meter, const MeterPalette& palette);
void miniMeterSetTuning(HWND meter, const MeterTuning& tuning);
MeterStyle   miniMeterStyle(HWND meter);
MeterPalette miniMeterPalette(HWND meter);
MeterTuning  miniMeterTuning(HWND meter);

// What a meter with this palette + look ACTUALLY draws on theme `th` — the same resolution onPaint
// uses, so the Meters dialog's swatches can show the colour on screen rather than the stored one.
// meterPanelColor: the panel behind the cells (`bg`, or the theme's window when it is CLR_INVALID;
// always the theme's on the Vu look, where `bg` is the lamp). meterDrawnPalette: the palette
// verbatim, except that the STOCK `off` and `peak` — dark-panel colours — are re-derived from the
// panel when that panel is light, and on a needle look the stock `accent` is the face's own needle
// (ui/VuDial.h). Paint-time only: never write a resolved colour back to settings
// (a stock value saved as its light-panel resolution would stop adapting, and be wrong on a dark skin).
struct Theme;
COLORREF     meterPanelColor(const MeterPalette& p, MeterStyle style, const Theme& th);
MeterPalette meterDrawnPalette(const MeterPalette& p, MeterStyle style, const Theme& th);

// Serialize a style/palette for the settings K/V store (persisted per meter). The
// palette is 7 comma-joined tokens (bg first — "theme" for CLR_INVALID, else RRGGBB);
// parsing falls back to `fallback` for any missing/garbled field.
std::wstring meterStyleToString(MeterStyle style);
MeterStyle   meterStyleFromString(const std::wstring& s, MeterStyle fallback);
std::wstring meterPaletteToString(const MeterPalette& p);
MeterPalette meterPaletteFromString(const std::wstring& s, const MeterPalette& fallback);

// Tuning is 5 comma-joined 0..1 floats (glow,smoothing,sensitivity,peakHold,breathing);
// any missing/garbled field falls back to `fallback`.
std::wstring meterTuningToString(const MeterTuning& t);
MeterTuning  meterTuningFromString(const std::wstring& s, const MeterTuning& fallback);

// "Glass cover" overlay strength for EVERY meter, 0 (off, the default) .. 1. Global rather than a
// per-meter MeterTuning field on purpose: the buffer meter has no MeterConfig, the Meters dialog's
// knob band is full, and a 6th MeterTuning field would break mac's exact-arity parser. Setting it
// only stores the value — the caller repaints. Persisted under glassStrengthSettingKey().
void  miniMeterSetGlass(float strength);
float miniMeterGlass();

// ---- Tooling (RabbitEarsRender) --------------------------------------------
// Copy the frame this meter last painted — its cached 32bpp back-buffer, which onPaint BitBlts 1:1
// (SRCCOPY) to the screen — as 0x00RRGGBB per pixel, row-major, top-down. READ-ONLY: it never paints
// and never calls into the paint path, so it cannot change what the app draws. False before the first
// paint. Used by Win32/render/RabbitEarsRender.cpp, which drives the meter's REAL WndProc (WM_TIMER,
// WM_PAINT) on a hidden window; the app itself never calls it.
bool miniMeterSnapshot(HWND meter, std::vector<uint32_t>& pixels, int& w, int& h);

}  // namespace rabbitears
