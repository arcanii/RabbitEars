// SPDX-License-Identifier: GPL-3.0-or-later
//
// VuDial — the analog VU instruments drawn by MiniMeter's two needle looks.
//
//   * Backlit — a cream card behind a black brushed-aluminium faceplate, lit from a bulb behind the
//     needle's pivot at the bottom centre: the amber glow, a -20..+5 scale whose red zone thickens
//     toward +5, "VU", and a PEAK lamp. Modelled on a modern desktop VU meter (the owner's reference
//     photo, 2026-09).
//   * Silver — an evenly lit ivory card set deep in a brushed-silver faceplate, a chrome inner wall
//     on one side; the pivot hidden below the window; a two-colour scale (blue to -1, red 0..+6)
//     with a 0-100 % modulation scale under it. Modelled on a 1970s cassette-deck meter. No maker's
//     name or logo is drawn, on either face, deliberately.
//
// The instrument is drawn at its OWN proportions, centred in the meter, with faceplate either side:
// a real VU window has a fixed aspect, and a card stretched across a 4:1 tray meter stops reading as
// one. Detail comes in by size — the tray's 26-40 px dials get light, arc, red zone, ticks, needle
// and lamp; numerals and legends appear only where they would be at least ~5 px tall (the Settings
// preview; a bigger tray, if the meters ever grow). Text smaller than that is a smudge, not detail.
//
// Split in two because only the needle moves: buildVuDialStatic renders everything else ONCE per
// (spec, size) — faceplate, card, lamp field, scale, legends, the unlit PEAK lamp — and the meter
// blits that cache every frame and adds drawVuDialNeedle on top.
#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>

namespace rabbitears {

enum class VuFace { Backlit, Silver };

// Everything the static layer depends on. A change to any field rebuilds it.
struct VuDialSpec {
    VuFace   face = VuFace::Backlit;
    COLORREF lamp = CLR_INVALID;  // the bulb's hue: the palette's Bg on a VU look (CLR_INVALID, or
                                  // anything too dark to be a lamp, = the face's stock bulb)
    COLORREF redZone = RGB(232, 96, 86);  // the palette's High
    bool operator==(const VuDialSpec&) const = default;
};

// Where the static layer put things, for the needle pass. Coordinates are relative to the dial
// rect's top-left, in device pixels.
struct VuDialLayout {
    VuFace face = VuFace::Backlit;
    RECT  card{};             // the visible card: the needle is clipped to it
    float pivotX = 0.0f, pivotY = 0.0f;
    float a0 = 0.0f, sweep = 0.0f;         // degrees, GDI+ convention (0 = +x, clockwise, +y down)
    float needleIn = 0.0f, needleOut = 0.0f;  // the needle's extent, as radii from the pivot
    float needleW = 1.0f;
    float shadowDeg = 0.0f;                // the shadow is the needle rotated this far about the pivot
    float shadowW = 2.0f;                  // ...this wide, at this opacity — both easing off as the
    float shadowA = 52.0f;                 // dial grows, or it reads as a second needle
    float ledX = 0.0f, ledY = 0.0f, ledR = 0.0f;  // PEAK lamp; ledR == 0 means the face has none
};

// The printed scale, for whoever drives the needle. Position is along the arc: 0 = its left end,
// 1 = its right end. The marks sit where the REFERENCE instruments print them (measured, not a
// textbook law); below -20 the position falls linearly in voltage to the left end.
float    vuDialPositionOfDb(VuFace face, float db);
float    vuDialFullScaleDb(VuFace face);   // the scale's top mark: +5 (Backlit), +6 (Silver)
COLORREF vuDialNeedleColour(VuFace face);  // the face's own needle — what the stock Accent means

// Render the frame-invariant layer into `px` (w*h, 0x00RRGGBB, row-major, top-down — the same layout
// as a 32bpp top-down DIB) and fill `lay`. `dpi` only picks the thinnest line it will draw.
void buildVuDialStatic(const VuDialSpec& spec, int w, int h, UINT dpi, std::vector<uint32_t>& px,
                       VuDialLayout& lay);

// The moving parts, over the blitted static layer whose top-left is (ox, oy) in `dc`: the needle at
// `position` along the arc (0..1; see vuDialPositionOfDb — the caller decides what its signal reads
// as) with its shadow, and the PEAK lamp lit to `peakGlow` (0..1). `needle` is its colour. A
// non-finite position draws the needle at rest rather than indexing anything with it.
void drawVuDialNeedle(HDC dc, int ox, int oy, const VuDialLayout& lay, float position, float peakGlow,
                      COLORREF needle);

}  // namespace rabbitears
