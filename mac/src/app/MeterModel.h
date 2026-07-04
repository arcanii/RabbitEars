// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeterModel — the macOS meter model: kinds, look, palette, tuning + UTF-8 codecs.
//
// PROVISIONAL / mac-owned. This is the mac peer of the Win32 meter model
// (Win32/ui/MiniMeter.h: MeterKind/MeterStyle/MeterPalette/MeterTuning/MeterConfig).
// It deliberately lives under `rabbitears::mac` and in mac/ — NOT yet in common/ —
// so it stays out of the Windows build until the Win32 team reviews the shape and
// we PROMOTE it to common/ui/MeterModel.h under a neutral `rabbitears::meter`
// namespace (the shared theme-engine boundary, the peer of common/ui/Skin.h). At
// that point Win32 drops its own copy and resolves the palette to COLORREF the way
// Theme.h resolves SkinPalette. Until then Win32 keeps its own model — no clash,
// because this lives in a different namespace and never links into the .exe.
//
// Graphics-free (pure C++): colours are the portable rabbitears::SkinColor from the
// shared skin model, NOT COLORREF — a palette's `bg` uses SkinColor{inherit} in
// place of Win32's CLR_INVALID sentinel, so each renderer (mac StatMeterView today,
// Win32 GDI after promotion) converts SkinColor -> its native colour at draw time.
#pragma once

#include <string>

#include "ui/Skin.h"  // rabbitears::SkinColor (shared, from common/)

namespace rabbitears::mac {

// The meter varieties (mirrors Win32 MeterKind). Only Spectrum needs audio capture;
// Signal/Bitrate/Frames run off libVLC stream stats (FlowStats) — no consent prompt.
enum class MeterKind { Spectrum, Signal, Bitrate, Frames };

// The visual "look" of a meter. Led is the classic dot-matrix; the others are added
// incrementally — an unimplemented look renders as Led. (mirrors Win32 MeterStyle.)
enum class MeterStyle { Led, Tube, Lcd, Scope };

// Per-meter colour palette. The roles map onto the "how much is lit" ramp so every
// look can honour them. `bg.inherit` means "follow the theme's window background"
// (the portable replacement for Win32's CLR_INVALID). The default member initializers
// reproduce the classic Win32 look exactly, so MeterPalette{} == the classic palette.
struct MeterPalette {
    SkinColor bg{.inherit = true};   // panel background (inherit = theme windowBg)
    SkinColor off{38, 40, 44};       // unlit / dim cell
    SkinColor low{96, 205, 128};     // low band of the lit ramp        (green)
    SkinColor mid{232, 188, 86};     // mid band                         (amber)
    SkinColor high{232, 96, 86};     // high band + alert/trouble tint   (red)
    SkinColor accent{217, 119, 87};  // bitrate history fill / scope trace (coral)
    SkinColor peak{236, 236, 240};   // peak-hold cap / trace head        (near-white)
};

// Per-meter "feel" knobs, all normalized 0..1 with 0.5 the neutral default that
// reproduces the classic behaviour exactly. Which ones matter depends on the meter:
// glow -> Tube/Scope bloom; smoothing -> attack/decay easing; sensitivity -> input
// gain; peakHold -> spectrum peak linger; breathing -> bitrate ceiling ebb.
// (mirrors Win32 MeterTuning.)
struct MeterTuning {
    float glow        = 0.5f;
    float smoothing   = 0.5f;
    float sensitivity = 0.5f;
    float peakHold    = 0.5f;
    float breathing   = 0.5f;
};

// One meter's full configuration (enable + look + palette + knobs). (mirrors Win32
// MeterConfig.) The default is the classic look, enabled, LED.
struct MeterConfig {
    bool         enabled = true;
    MeterStyle   style   = MeterStyle::Led;
    MeterPalette palette{};
    MeterTuning  tuning{};
};

// Classic-look defaults (kept as functions for parity with the Win32 API surface, so
// the eventual promotion maps 1:1). All kinds share the classic palette/look today.
inline MeterPalette defaultMeterPalette(MeterKind /*kind*/) { return MeterPalette{}; }
inline MeterStyle   defaultMeterStyle(MeterKind /*kind*/) { return MeterStyle::Led; }
inline MeterTuning  defaultMeterTuning() { return MeterTuning{}; }

// ---- (de)serialization (UTF-8) — mirrors the common Skin codec discipline --------
// Each codec round-trips for the settings K/V store and, after promotion, feeds the
// shared skin. Exact-arity-or-whole-fallback with per-field fallback within a valid
// token count (the "meter discipline" — see Skin.cpp).

// Style: "led" | "tube" | "lcd" | "scope"; unknown -> fallback.
std::string meterStyleToString(MeterStyle s);
MeterStyle  meterStyleFromString(const std::string& s, MeterStyle fallback);

// Palette: 7 comma-joined SkinColor tokens (bg,off,low,mid,high,accent,peak). Each
// token is "inherit", "RRGGBB", or "RRGGBBAA" (skinColorToString form). The bg token
// also accepts the legacy Win32 "theme" spelling as inherit (forward-compat for when
// Win32 migrates). Requires exactly 7 tokens or returns the whole fallback; within 7
// each field falls back individually.
std::string  meterPaletteToString(const MeterPalette& p);
MeterPalette meterPaletteFromString(const std::string& s, const MeterPalette& fallback);

// Tuning: 5 comma-joined 0..1 floats (glow,smoothing,sensitivity,peakHold,breathing);
// exactly 5 tokens or whole fallback; each field falls back individually; values
// clamp to 0..1.
std::string meterTuningToString(const MeterTuning& t);
MeterTuning meterTuningFromString(const std::string& s, const MeterTuning& fallback);

}  // namespace rabbitears::mac
