// SPDX-License-Identifier: GPL-3.0-or-later
//
// GlassMask — the "dials behind a curved glass cover" look for the meters.
//
// Pure math, no graphics API: buildGlassMask() precomputes two per-pixel byte tables that a
// renderer applies over its FINISHED frame, so each platform keeps its own blitter. Lives in
// common/ because the maths is identical on Win32 (GDI DIB) and mac (CGBitmapContext).
//
// WHY A LUT: the effect is frame-INVARIANT — a pure function of (w, h, strength). The meters
// repaint ~30×/s, so recomputing sin/pow per pixel per frame would be silly; build once per size
// and the per-frame cost collapses to two byte ops per channel.
//
// THE MODEL: a thick glass plate with a CURVED (beveled) rim, as on a vintage VU meter — the
// reference being a Phase Linear 400. The important observation from that hardware is that the
// glass is INVISIBLE across the middle: you read the dial through it perfectly. All the optical
// business happens in a narrow band at the rim, where the curve bends light — a bright fillet
// and highlight along the top/left lip, falling to shadow at the bottom/right.
//
// So: the centre is left exactly untouched (add=0, mul=255) and every term is squared to pack it
// hard against the edge. An earlier version spread soft gradients over the whole face and simply
// read as BLUR — the giveaway that it was a filter rather than a pane.
//
// Explicitly NOT here:
//   • refraction — at ~26-30px tall with a 3px LED pitch the displacement is SUB-CELL, so it
//     reads as a rendering bug rather than as glass, and it would need a second scratch buffer;
//   • an animated highlight — it fights BufferMeter's own drifting specular, and there is no
//     shared frame clock (16ms parent vs 33ms meters);
//   • chromatic fringing — indistinguishable from colour noise on a 30px panel.
//
// Geometry is expressed purely as FRACTIONS of w/h, never in device pixels, so a DPI-scaled meter
// gets a proportionally scaled highlight for free.
#pragma once

#include <cstdint>
#include <vector>

namespace rabbitears {

// 0 = no glass at all (the tables come back neutral and the renderer can skip the pass entirely);
// 1 = the full effect. Values outside 0..1 are clamped.
struct GlassParams {
    float strength = 0.0f;
};

// Fills `add` (the lit top/left bevel, added toward white) and `mul` (the shaded bottom/right
// bevel, 255 == unchanged), each
// w*h bytes, row-major top-down. Both are cleared to neutral and return immediately when
// strength <= 0 or the size is degenerate, so callers can treat "off" as a cheap no-op.
void buildGlassMask(int w, int h, const GlassParams& p, std::vector<uint8_t>& add,
                    std::vector<uint8_t>& mul);

// True when the mask would do nothing — lets a renderer skip both the build and the apply.
inline bool glassIsNoop(const GlassParams& p) { return !(p.strength > 0.0f); }

// Stable settings token, shared by both platforms.
inline const char* glassStrengthSettingKey() { return "meter_glass"; }

}  // namespace rabbitears
