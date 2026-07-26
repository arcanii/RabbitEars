// SPDX-License-Identifier: GPL-3.0-or-later
//
// VuLamp — the bulb behind the VU dial, and the colours it makes.
//
// Pure math, no graphics API: the renderer gets one byte table (the bulb's field over the dial) and
// three colours (the face at its dimmest, the face at the bulb's core, and the printed ink), then
// composites them itself. Lives in common/ because the maths is identical on Win32 (GDI DIB) and
// mac (CGBitmapContext) — mac carries no Vu style yet, so today only Win32 calls it.
//
// WHY A LUT — the same reasoning as GlassMask: the field is frame-INVARIANT, a pure function of
// (w, h). The meters repaint ~30x/s and the needle is the only thing on the dial that moves, so the
// field is built once per size and the per-frame cost collapses to one lerp per pixel. It is also
// why this is testable at all: the hardest part of the look to eyeball is the part --selftest can
// pin exactly.
//
// THE MODEL — a printed cream card lit by ONE small bulb tucked behind the bottom rim of the bezel:
//
//   * WHAT YOU SEE IS PIGMENT x LIGHT. Every colour on the dial — the face, the arc, the ticks — is
//     the card's own pigment multiplied by the lamp. That single product is the whole file, and it
//     is what lets one colour swatch turn the instrument electric blue with nothing ever
//     disagreeing: the markings go dark navy because they are the SAME INK UNDER THE SAME LAMP, not
//     because someone picked a second colour to match.
//
//   * THE BULB'S CENTRE IS OFF-CANVAS, just below the bottom edge. A closed bright blob inside a
//     26px panel reads as a smudge or a blit artifact rather than as light; what belongs on screen
//     is the upper shoulder of a pool that carries on past the rim, which is what you actually see
//     when a bulb is hidden behind a bezel lip.
//
//   * AND IT IS OFF-AXIS, left of centre. Perfect bilateral symmetry is the tell of a gradient —
//     the eye can find no source in it. Moving the bulb sideways puts ~49 luma of variation ACROSS
//     the bottom edge, on an axis where a vertical gradient has exactly none, and that lateral term
//     is what makes the dial read as lit by a lamp instead of painted with a ramp.
//
// Explicitly NOT here:
//   * the needle, its shadow, the scale geometry — all still the renderer's, all still per-frame;
//   * any blur. At 26px a kernel wide enough to soften anything eats a fifth of the panel, and a
//     soft ramp across the face is exactly what read as blur when GlassMask tried it. Every soft
//     edge in this file is the falloff curve itself, evaluated per pixel.
//   * gamma. The dial is composited in the same 8-bit display space the rest of the meter tray
//     lives in; going linear for this one surface would put it out of step with the glass mask,
//     which is a straight byte multiply sitting directly on top of it.
#pragma once

#include <cstdint>
#include <vector>

namespace rabbitears {

// A colour as plain floats, 0..255 per channel, deliberately NOT clamped: `faceHot` is the
// photometric value and a warm lamp overshoots 255 in red. The renderer clamps that ENDPOINT and
// lerps toward the clamped value, which turns what would be a hard contour part-way up the dial
// into a smooth roll into the highlight — so the overshoot has to survive the trip out of here.
// Not COLORREF and not Gdiplus::Color: this file must not know what platform it is on.
struct VuLampRgb {
    float r, g, b;
};

// The three colours one lamp makes on the dial. The face is a per-pixel LERP from `faceDim` to
// `faceHot` driven by buildVuLampMask(); `ink` is flat, because every tick and the whole arc live
// inside the top third of the face, where the field barely varies (see kScaleField in the .cpp).
struct VuDial {
    VuLampRgb faceDim;  // the face where the mask reads 0   — the far rim
    VuLampRgb faceHot;  // the face where the mask reads 255 — the bulb's core
    VuLampRgb ink;      // the printed scale: arc + ticks
};

// The stock bulb: a ~2700K grain-of-wheat lamp behind a cream plate. This is what the dial is lit
// by unless the user says otherwise, and every constant in the .cpp is calibrated against it.
VuLampRgb vuStockLamp();

// True when this colour is too dark to be meant as a lamp. A lamp cannot be dark, and this file
// uses only a lamp's HUE AND SATURATION (never its brightness), so refusing the bottom of the range
// costs no expressible colours at all — every hue is still reachable, just at a legible value.
// What it buys is two things the renderer needs: the theme's own near-black window background
// cannot be mistaken for a lamp, and picking black in a colour picker is a way back to the stock
// bulb from a platform sentinel that a picker can never return.
bool vuLampIsUnset(int r, int g, int b);

// A user's colour pick resolved to the lamp it means — the stock bulb when vuLampIsUnset().
VuLampRgb vuLampFrom(int r, int g, int b);

// Pigment x light. Pure, total, and the same on both platforms.
VuDial vuDialColours(VuLampRgb lamp);

// Fills `mask` with w*h bytes, row-major top-down: the bulb's relative illumination at each pixel
// of the DIAL rect (not the client rect — the renderer passes the face it is about to paint).
// 0 == the far rim, 255 == the bulb's core. Cleared to 0 and returned immediately for a degenerate
// size, so a caller can treat that as "paint nothing".
void buildVuLampMask(int w, int h, std::vector<uint8_t>& mask);

}  // namespace rabbitears
