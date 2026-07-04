// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skin — the shared, platform-neutral skin model for the theme engine (0.2.x).
// This is the FIRST model to physically live in common/: the meter model it
// mirrors (MeterStyle/Palette/…) is still under Win32/ui/. It is the cross-platform
// contract both renderers consume — the Windows renderer (Direct2D-on-D3D11 + HLSL)
// and the eventual macOS renderer (Metal / Core Graphics). See
// Win32/docs/THEME_ENGINE.md §4.
//
// STRICTLY graphics-free: no <windows.h>, no COLORREF, no D2D/Metal. It compiles
// into RabbitEarsCore on BOTH platforms (the mac-core CI builds it on Apple clang),
// so keep it to standard C++ only. Each renderer converts SkinColor -> its native
// color type and resolves SkinFont -> its native font.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rabbitears {

// Platform-neutral color. `inherit` == "resolve against the base surface color" —
// the portable replacement for Win32's CLR_INVALID sentinel (used by widgets that
// layer on a skin, e.g. meter palettes). The base skin palette uses concrete colors.
struct SkinColor {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool    inherit = false;
};

constexpr bool operator==(const SkinColor& x, const SkinColor& y) {
    return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a && x.inherit == y.inherit;
}
constexpr bool operator!=(const SkinColor& x, const SkinColor& y) { return !(x == y); }

// Typography role (resolved to HFONT / NSFont by the renderer). `weight` is CSS-ish
// 100..900 (400 normal, 600 semibold). `sizePt` is points; the Win32 renderer scales
// to device pixels (pt * dpi / 72).
struct SkinFont {
    std::string family = "Segoe UI";
    float       sizePt = 10.5f;
    int         weight = 400;
    // `symbol` marks the platform icon/symbol font (the `glyph` role uses "Segoe MDL2
    // Assets" on Windows). A renderer that can't load `family` substitutes its own
    // icon font (mac: SF Symbols) rather than failing. (Italic is intentionally not
    // modelled — RabbitEars uses none; SkinFont isn't serialized, so it can be added
    // later without a format break.)
    bool        symbol = false;
};

// The named color roles a skin drives. Superset of today's *used* Theme fields (the
// four unused syntax-highlight colors carried from the sibling apps are dropped),
// plus dangerHover (was the hardcoded close-button hover red in drawCmdBar).
struct SkinPalette {
    SkinColor windowBg, panelBg, panelElevBg, altRowBg, hoverBg, border;
    SkinColor textPrimary, textSecondary, textMuted;
    SkinColor accent, accentText, selectionBg, selectionText;
    SkinColor dangerHover;
};

// Per-skin GPU-effect manifest — the strengths the Win32 HLSL skin effects (the
// transport-strip underglow + the dock-gutter neon, in Win32/ui/skin/SkinStrip.cpp)
// feed into the shader's `uIntensity`. Graphics-free: plain 0..1 floats resolved by
// each renderer (the shader saturate()s them, so out-of-range clamps). Defaults
// reproduce the pre-manifest hardcoded strengths, so a skin that leaves them unset
// keeps today's look; the macOS Metal renderer mirrors these. This is the "optional
// GPU-skin manifest" the Skin struct reserved for Phase 4 (see docs/SKIN_MODEL.md §2).
struct SkinGpu {
    float stripGlow = 1.0f;   // transport-strip underglow strength (0 = off .. 1 = full)
    float edgeGlow  = 0.9f;   // dock-gutter neon strength          (0 = off .. 1 = full)
};

// A complete skin: identity + palette + typography + GPU-effect strengths. `dark`
// hints the OS chrome (DWM immersive dark on Win32 / NSAppearance on mac).
struct Skin {
    std::string id;        // stable token, e.g. "dark" — this is the persisted selection
    std::string name;      // display name, e.g. "Dark"
    bool        dark = true;
    SkinGpu     gpu;       // per-skin GPU-effect strengths (glow); default == today's look
    SkinPalette palette;
    SkinFont    body, title, glyph;
};

// ---- built-in registry ------------------------------------------------------
// Phase 2 ships the two existing looks as skins (dark + light) to exercise the
// switch machinery; Steampunk / Cyberpunk are authored in Phase 4. Whether a skin
// carries light/dark *variants* (vs. being one or the other) is deferred to then.
Skin makeDarkSkin();
Skin makeLightSkin();
Skin makeCyberpunkSkin();                                 // neon-magenta-on-midnight (Phase 4, colours)
Skin makeSteampunkSkin();                                 // brass-on-dark-iron + serif title (Phase 4)
const std::vector<Skin>&  builtinSkins();                  // immortal; index 0 == dark
const Skin&               skinById(const std::string& id); // unknown -> builtinSkins()[0] (dark)
std::vector<std::string>  builtinSkinIds();
const char*               defaultSkinId();                 // "dark"

// The settings K/V key both platforms persist the active skin id under — shared so
// there is one source of truth for "which skin is active". The *active-state holder*
// and the OS dark/light detection that resolves a "follow system" choice are
// deliberately renderer-side (platform event models differ) — see THEME_ENGINE.md §4.3.
const char*               skinSettingKey();                // "skin"

// ---- (de)serialization (UTF-8) — mirrors the meter codec discipline ---------
// Color: "inherit", "RRGGBB", or "RRGGBBAA" (uppercase hex). Bad input -> fallback.
std::string skinColorToString(const SkinColor& c);
SkinColor   skinColorFromString(const std::string& s, const SkinColor& fallback);
// Palette: the 14 roles comma-joined, positionally. Requires EXACTLY 14 tokens or
// returns the whole fallback; within a 14-token string each field falls back
// individually. NOTE: this positional form is only round-trip data today (the
// persisted selection is just an id, §4.4) — so it is safe to freeze. BEFORE
// user-customizable skins persist a palette string (Phase 4), give it forward-compat
// (a version/arity prefix + accept >=14 tokens) so adding a 15th role doesn't discard
// every saved skin. Do NOT reorder roles — position is identity.
std::string skinPaletteToString(const SkinPalette& p);
SkinPalette skinPaletteFromString(const std::string& s, const SkinPalette& fallback);

// GPU manifest: "stripGlow,edgeGlow" (two %.3f 0..1 floats). Exact-arity or whole
// fallback (palette-codec discipline); values clamp to 0..1. Not persisted yet (only
// the id is, §4.4), so the format is free to change until user-customizable skins ship.
std::string skinGpuToString(const SkinGpu& g);
SkinGpu     skinGpuFromString(const std::string& s, const SkinGpu& fallback);

}  // namespace rabbitears
