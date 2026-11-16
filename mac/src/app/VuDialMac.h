// SPDX-License-Identifier: GPL-3.0-or-later
//
// VuDialMac — the analog VU instruments the mac MeterView draws for its two needle looks: the mac
// port of Win32/ui/VuDial.{h,cpp} (read that header for the whole design). Same faces, same MEASURED
// scale tables, the same proportions and the same detail-by-size thresholds:
//
//   * Backlit — a cream card behind a black brushed faceplate, lit from a bulb behind the pivot at
//     the bottom centre; a -20..+5 scale whose red zone thickens toward +5, "VU", a PEAK lamp.
//   * Silver — an evenly lit ivory card set deep in a brushed-silver faceplate with a chrome inner
//     wall; blue scale to -1, red 0..+6, a 0-100 % modulation scale. No maker's name or logo.
//
// The pixel work (faceplate grain, the card's lighting, the rim) is the Win32 code verbatim; the
// GDI+ vector work (arcs, ticks, the red wedge, discs, legends, the needle) is CoreGraphics in a y-down
// context over the same 0x00RRGGBB buffer, with the geometry computed by the same expressions — so the
// two platforms draw the same instrument. Keep the tables and constants in step with VuDial.cpp (a
// candidate for promotion to common/ once its GDI+ half is split from its pixel half on Win32).
//
// Split in two because only the needle moves: buildVuDialStatic renders everything else once per
// (spec, size) and the meter blits that cache every frame, then drawVuDialNeedle on top.
#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "platform/WinTypes.h"  // COLORREF / RGB / RECT, as the Win32 original is written

namespace rabbitears::mac {

enum class VuFace { Backlit, Silver };

struct VuDialSpec {
    VuFace   face = VuFace::Backlit;
    COLORREF lamp = CLR_INVALID;          // the bulb's hue (the palette's Bg); CLR_INVALID / too dark = stock
    COLORREF redZone = RGB(232, 96, 86);  // the palette's High
    bool operator==(const VuDialSpec&) const = default;
};

// Where the static layer put things (dial-relative device pixels, y down).
struct VuDialLayout {
    VuFace face = VuFace::Backlit;
    RECT  card{};
    float pivotX = 0.0f, pivotY = 0.0f;
    float a0 = 0.0f, sweep = 0.0f;            // degrees: 0 = +x, increasing clockwise (y down)
    float needleIn = 0.0f, needleOut = 0.0f;
    float needleW = 1.0f;
    float shadowDeg = 0.0f, shadowW = 2.0f, shadowA = 52.0f;
    float ledX = 0.0f, ledY = 0.0f, ledR = 0.0f;  // PEAK lamp; ledR == 0: the face has none
};

float    vuDialPositionOfDb(VuFace face, float db);
float    vuDialFullScaleDb(VuFace face);
COLORREF vuDialNeedleColour(VuFace face);

constexpr float kVuBacklitAspect = 1.62f;
constexpr float kVuSilverAspect = 2.45f;
struct VuDialWindow {
    int mx = 0, my = 0;
    int openW = 0, openH = 0;
};
inline VuDialWindow vuDialWindow(VuFace face, int w, int h) {
    const bool silver = face == VuFace::Silver;
    VuDialWindow r;
    r.my = std::max(1, static_cast<int>(std::lround(h * (silver ? 0.07f : 0.08f))));
    r.openH = h - 2 * r.my;
    r.mx = std::max(1, static_cast<int>(std::lround(h * (silver ? 0.09f : 0.10f))));
    r.openW = std::min(w - 2 * r.mx,
                       static_cast<int>(std::lround(r.openH * (silver ? kVuSilverAspect : kVuBacklitAspect))));
    return r;
}
inline int vuDialNaturalWidth(VuFace face, int h) {
    const VuDialWindow r = vuDialWindow(face, 1 << 20, h);
    return r.openW + 2 * r.mx;
}

// Render the frame-invariant layer into `px` (w*h, 0x00RRGGBB, top-down) and fill `lay`. `scale` is
// the backing scale (pixels per point) — it only picks the thinnest line drawn, as Win32's dpi does.
void buildVuDialStatic(const VuDialSpec& spec, int w, int h, float scale, std::vector<uint32_t>& px,
                       VuDialLayout& lay);

// The moving parts, into `ctx` (a y-down context over the meter's pixels, the static layer already
// in it with its top-left at (ox, oy)): the needle at `position` (0..1 along the arc) with its shadow,
// and the PEAK lamp lit to `peakGlow` (0..1). A non-finite position draws the needle at rest.
void drawVuDialNeedle(CGContextRef ctx, int ox, int oy, const VuDialLayout& lay, float position, float peakGlow,
                      COLORREF needle);

}  // namespace rabbitears::mac
