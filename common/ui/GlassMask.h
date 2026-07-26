// SPDX-License-Identifier: GPL-3.0-or-later
//
// GlassMask — the "dials behind a curved glass cover" look for the meters.
//
// Pure math, no graphics API: buildGlassMask() precomputes two per-pixel byte tables that a
// renderer applies over its FINISHED frame, so each platform keeps its own blitter. Lives in
// common/ because the maths is identical on Win32 (GDI DIB) and mac (CGBitmapContext).
//
// WHY A LUT: the effect is frame-INVARIANT — a pure function of (w, h, strength, chromePx). The
// meters repaint ~30x/s, so recomputing per pixel per frame would be silly; build once per size and
// the per-frame cost collapses to two byte ops per channel.
//
// THE MODEL: a FRAMED PANE. The reference is a Phase Linear 400, where the glass sits inside a dark
// machined surround — and it is the SURROUND, not the glass, that does most of the work of selling
// "instrument". So the mask draws three bands, measured inward from the edge:
//
//   band 0 — the outermost ONE device pixel. LEFT COMPLETELY ALONE (mul=255, add=0). This is the
//            renderer's own themed border, and it is the only thing separating the panel from a
//            chassis painted the SAME colour. Never darken it (that dissolves the outline), never
//            brighten it (add is achromatic — it would wash a brass or blue themed border toward
//            neutral, and the meter would read as "the border got fatter and hotter", which is the
//            exact failure the first two passes had). The theme owns this pixel; the glass does not.
//   band 1 — the rest of the chrome gutter. THE BEZEL: opaque, hard-stepped, directional. Because
//            v = v*mul/255 + add drives the pixel to exactly `add` as mul -> 0, this graphics-free
//            table can paint an ABSOLUTE achromatic tone — a manufactured part, not a tint — with
//            no colour type and no renderer change.
//   band 2 — the dial. Only the bezel's CAST SHADOW, and only as a multiply. `add` is exactly zero
//            across the whole dial: no additive term ever touches content. An early version spread
//            soft gradients over the face and read as BLUR; the pass after it still smeared a 5-row
//            additive ramp down from the lip. Confining every bright term to band 1 is what finally
//            makes the edge read as a drawn line rather than a filter.
//
// GEOMETRY IS DERIVED FROM `chromePx` — the width of the chrome band the renderer already reserves
// around its content (on Win32: MiniMeter's dpx(2, dpi) inset, which holds a 1px FrameRect plus a
// pad). Pass it and the bezel lands exactly on pixels that were never dial, so the frame costs
// NOTHING; the shadow is then a fixed multiple of it, so the whole effect scales with DPI without
// this file ever calling dpx() or knowing what a DPI is. It also means the mask contains NO
// axis-derived geometry at all: a 200x20 panel and a 20x200 panel get the same ring on both of
// their axes, which is what a machined part looks like. (Earlier passes sized the rim off the short
// axis, which made a 112x30 meter read as two horizontal bands rather than as a framed object.)
// Callers that reserve nothing may pass 0 and get a fraction-of-short-axis fallback.
//
// Explicitly NOT here:
//   • refraction — at ~26-30px tall with a 3px LED pitch the displacement is SUB-CELL, so it
//     reads as a rendering bug rather than as glass, and it would need a second scratch buffer;
//   • an animated highlight — it fights BufferMeter's own drifting specular, and there is no
//     shared frame clock (16ms parent vs 33ms meters);
//   • chromatic fringing — indistinguishable from colour noise on a 30px panel;
//   • broad gradients across the face — that was the blur.
#pragma once

#include <cstdint>
#include <vector>

namespace rabbitears {

// 0 = no glass at all (the tables come back neutral and the renderer can skip the pass entirely);
// 1 = the full effect. Values outside 0..1 are clamped.
//
// `chromePx` is GEOMETRY, not a setting: it is how many device pixels of non-content chrome the
// renderer has already reserved around its dial, so the bezel can be painted there instead of on
// top of the dial. It is never persisted, never per-meter, and never user-visible — the ONE user
// knob is still `strength`. 0 means "I reserved nothing, pick something sensible".
struct GlassParams {
    float strength = 0.0f;
    int   chromePx = 0;
};

// Fills `add` (added toward white — nonzero ONLY inside the bezel band) and `mul` (255 == leave the
// pixel alone), each w*h bytes, row-major top-down. Both are cleared to neutral and return
// immediately when strength <= 0 or the size is degenerate, so callers can treat "off" as a cheap
// no-op.
void buildGlassMask(int w, int h, const GlassParams& p, std::vector<uint8_t>& add,
                    std::vector<uint8_t>& mul);

// The chrome-band width buildGlassMask() will actually use for this size — `reserved` clamped down
// so the ring can never swallow the pane, or the fallback when `reserved` <= 0. Exported so a
// renderer whose content runs edge-to-edge (BufferMeter) can inset its own layout by exactly the
// same number, and so the selftest can pin the rule. Pure integer geometry; returns 0 when the
// panel is too small to carry a frame at all.
int glassChromePx(int w, int h, int reserved);

// True when the mask would do nothing — lets a renderer skip both the build and the apply.
inline bool glassIsNoop(const GlassParams& p) { return !(p.strength > 0.0f); }

// Stable settings token, shared by both platforms.
inline const char* glassStrengthSettingKey() { return "meter_glass"; }

}  // namespace rabbitears
