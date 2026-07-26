// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VuLamp.h"

#include <cmath>

namespace rabbitears {
namespace {

// ---- The card -----------------------------------------------------------------------------
// The dial's pigment, taken VERBATIM from the cream the VU look shipped with (the old `faceTop`).
// Keeping the literal identical is the whole of "the default must still be a VU": under the stock
// bulb this file reproduces that colour family, and the brightest pixel on the finished dial lands
// within ~1 luma level of the brightest pixel the flat gradient used to produce.
constexpr float kPigment[3] = {246.0f, 232.0f, 176.0f};

// Rec.601 luma of that pigment (229.8). Used as the neutral the pigment is bleached TOWARD — see
// kBleach below. Written as a literal rather than recomputed so the two can never drift.
constexpr float kPigmentLuma = 229.8f;

// ---- The bulb -----------------------------------------------------------------------------
// ~2700K seen through a cream plate. Its blue channel at 0.776 of red is what gives the default
// dial its tungsten cast; a flat white lamp (which a user CAN pick) is a legitimate, cooler dial.
constexpr float kStock[3] = {255.0f, 236.0f, 198.0f};

// Below this peak channel a colour is a background, not a lamp. 64 clears the darkest thing that
// can arrive here by accident — the theme's own window background, which is 24 on the dark skin, 26
// on sepia and 18 on midnight — by two and a half times, while sitting far under any colour a
// person picks meaning "the lamp is blue". Because only chromaticity is read (see vuDialColours),
// nothing is lost by rejecting the dim end: RGB(20,40,85) and RGB(60,120,255) are the SAME LAMP to
// this file, so a user who wants that navy simply picks the legible version of it.
constexpr int kLampFloor = 64;

// ---- Photometry ---------------------------------------------------------------------------
// How far the pigment is dragged toward its own luma as the lamp gets more saturated. This is the
// term that makes ONE swatch serve both references, and it is the only non-obvious line in the file.
//
// You cannot see "cream" under an electric-blue bulb — you see blue. Multiply the cream by a blue
// lamp WITHOUT this and you get RGB(58,127,176): a dead slate, the classic "it just looks dirty"
// failure. Bleaching the pigment in proportion to the lamp's saturation removes the cream's own hue
// exactly as fast as the lamp overwhelms it, so the stock bulb (saturation 0.22) leaves the card
// essentially the cream it is printed as, while an electric blue (saturation 0.76) neutralises it
// three-quarters of the way and the dial comes out genuinely blue.
constexpr float kBleach = 1.0f;

// The two ends of the face, as multiples of nominal illumination.
//
// kFaceHi 1.09 is THE SINGLE KNOB'S PARTNER, and it is pinned rather than chosen: it is the value
// that puts the finished hotspot at luma ~231, within about one level of the 229.8 the old flat
// face carried at its brightest. That matters more than it sounds — it is the difference between
// this change reading as "lit" and reading as "someone turned my meters down". It also sits
// deliberately between the stock lamp's two clip points (red clips above 1.052, green not until
// 1.19), so exactly one channel saturates. Push it past 1.19 and green clips too, the hotspot
// flat-tops to a hue-less white, and the complaint that started all this is back.
constexpr float kFaceHi = 1.09f;

// kFaceLo 0.80 IS THE KNOB — the one number to turn if the lamp is too strong or too weak. It sets
// the far rim and nothing else: the hotspot does not move when you turn it, because the hot end is
// pinned above, so the dial gets flatter or deeper rather than brighter or darker overall. On a
// 108x26 tray meter under the stock bulb it maps like this (dimmest corner / total range across the
// face, against the old flat face's 205..230, i.e. 25 levels):
//     0.72 -> 162 / 69     0.76 -> 170 / 61     0.80 -> 178 / 53  (shipping)     0.88 -> 193 / 38
// 0.80 is where the dial still reads as lit from below — better than twice the old modelling, and
// with a lateral term the old face had none of — while its dimmest corner, RGB(200,177,121), is
// still recognisably the cream this look is named for. Below about 0.76 that corner turns olive,
// and a VU that isn't cream stops reading as a VU.
constexpr float kFaceLo = 0.80f;

// Where the printed scale sits in the field, as a mask value 0..1. Every tick and the whole arc
// live between 0.14h and 0.333h from the top — the DIMMEST third of a bottom-lit dial. Sampled at
// 241 points along the arc drawVu actually draws, the field's mean over that band is 0.283 on the
// narrowest tray meter, 0.311 on the widest and 0.284 in the settings preview. 0.31 is therefore
// the WIDEST tray meter's value, not a universal one — one ink colour serving the lot is an
// approximation, and a deliberate one: per-tick shading would mean a brush per tick for a spread
// that lands inside a single byte at these sizes.
constexpr float kScaleField = 0.31f;

// The ink's reflectance — and, because both the ink and the ground it sits on are the same pigment
// under the same lamp, THE INVERSE OF THE TICK CONTRAST RATIO. 0.24 is nominally 4.17:1.
//
// That ratio is EXACT across lamp colours and across glass strengths — the first because ink and
// ground share one lamp, the second because the glass overlay is a pure multiply on the dial — and
// those are the two invariances that matter, because they are what let the blue option ship with no
// second knob. It is NOT exact across positions: the ink is one flat colour while the ground under
// it varies with the field, so measured along the arc the real ratio runs 3.88–4.30 (108x26) and
// 3.95–4.30 (146x82 preview). The old hard-coded brown gave 3.83:1 on its flat cream, so even the
// bottom of that range is an improvement — but note ABSOLUTE contrast still falls (164 -> 142),
// because the ground under the ticks is genuinely darker than the old flat face.
constexpr float kInkFrac = 0.24f;

// ---- The bulb's position and reach ----------------------------------------------------------
// Left of centre. Not centred, because a symmetric pool has no findable source and is just the
// "too pure" problem restated; not further left, because the arc's own left end sits at 0.5w-0.84h
// and the hotspot must stay under the dial rather than beside it.
constexpr float kLampX = 0.42f;   // x, as a fraction of the dial's width

// Just BELOW the bottom edge — 1.06 of the height, i.e. 6% of h past it. Far enough that the peak
// of the pool is off-canvas (so no blob), close enough that the bottom rows still sit on the
// shoulder rather than the tail. This is also the number that decides how the lamp meets the glass
// overlay's rim shadow: at 1.06 the last five rows of the face are within 6 luma of the peak, so
// the glass can carve its bezel lip into the front of the pool without eating the pool.
constexpr float kLampY = 1.06f;

// Reach. The vertical radius is a multiple of h so the falloff is the same shape at every DPI. The
// horizontal radius takes the LARGER of a width term and a height term, and the reason is the four
// tray meters: the widest (Spectrum, 108px) needs the pool to spread or its right third goes dead,
// while the narrowest (Signal, 54px) would get a pinprick from a width-only term. With this floor
// the dimmest corner lands within 5 luma of itself across all four widths and the whole tray reads
// as one row of instruments lit by one row of bulbs.
constexpr float kLampRx = 0.62f;      // of width...
constexpr float kLampRxFloor = 1.55f; // ...but never less than this multiple of height
constexpr float kLampRy = 1.35f;      // of height

// Falloff steepness. The curve is (1 + k*d^2)^-1.5, which is Lambert's cosine on the receiving card
// folded into an inverse square — the cos(theta) is what makes it steeper than plain 1/r^2 and it
// is the reason this reads as a source rather than a vignette: a fast knee and then a long, flat
// tail, where a smoothstep is symmetric at both ends and looks like a lens effect. 2.6 puts the
// far top corner at 0.10 of the core with no visible edge anywhere on the panel, because the curve
// never actually reaches zero and so has nothing to terminate at.
constexpr float kFall = 2.6f;

float max3(float a, float b, float c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }
float min3(float a, float b, float c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }

uint8_t toByte(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

}  // namespace

VuLampRgb vuStockLamp() { return VuLampRgb{kStock[0], kStock[1], kStock[2]}; }

bool vuLampIsUnset(int r, int g, int b) {
    const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    return mx < kLampFloor;
}

VuLampRgb vuLampFrom(int r, int g, int b) {
    if (vuLampIsUnset(r, g, b)) return vuStockLamp();
    return VuLampRgb{static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
}

VuDial vuDialColours(VuLampRgb lamp) {
    // Reject a non-finite or negative channel BEFORE taking the max, not just an all-zero lamp.
    // max3 cannot see a partial NaN — every NaN comparison is false, so max3({NaN,200,100}) returns
    // 200 and the NaN escapes through lampN into the renderer's static_cast<int>, which is UB. A
    // negative channel is the same story via chroma > 1. Unreachable from today's callers (they all
    // feed GetRValue-derived 0..255), but this function is documented as total, so it is.
    if (!(lamp.r >= 0.0f) || !(lamp.g >= 0.0f) || !(lamp.b >= 0.0f)) lamp = vuStockLamp();
    float mx = max3(lamp.r, lamp.g, lamp.b);
    if (!(mx > 0.0f)) {  // an all-zero lamp
        lamp = vuStockLamp();
        mx = kStock[0];
    }
    const float mn = min3(lamp.r, lamp.g, lamp.b);

    // ONLY the lamp's chromaticity is taken: its peak channel is renormalised to 1 and its
    // brightness discarded. A lamp's job is to be bright — how bright is this file's decision, not
    // the swatch's — and without this a user who once set a dark panel colour on an LED meter and
    // then switched that meter to Vu would be handed an unlit dial.
    const float lampN[3] = {lamp.r / mx, lamp.g / mx, lamp.b / mx};
    const float chroma = (mx - mn) / mx;  // HSV saturation; 0 for any grey, 1 for a pure hue

    float pig[3];
    for (int i = 0; i < 3; ++i)
        pig[i] = kPigment[i] + (kPigmentLuma - kPigment[i]) * (chroma * kBleach);

    auto lit = [&](float k) {
        return VuLampRgb{pig[0] * lampN[0] * k, pig[1] * lampN[1] * k, pig[2] * lampN[2] * k};
    };

    VuDial d;
    d.faceDim = lit(kFaceLo);
    d.faceHot = lit(kFaceHi);  // deliberately left unclamped; see VuLampRgb's note
    // The illumination the scale actually sits in, followed through from the knob — so the ink is
    // always the same pigment under the same light as the ground it is printed on.
    d.ink = lit((kFaceLo + (kFaceHi - kFaceLo) * kScaleField) * kInkFrac);
    return d;
}

void buildVuLampMask(int w, int h, std::vector<uint8_t>& mask) {
    const size_t n = (w > 0 && h > 0) ? static_cast<size_t>(w) * static_cast<size_t>(h) : 0;
    mask.assign(n, 0);
    if (!n) return;

    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const float lx = fw * kLampX, ly = fh * kLampY;
    const float rx = (fw * kLampRx > fh * kLampRxFloor) ? fw * kLampRx : fh * kLampRxFloor;
    const float ry = fh * kLampRy;

    for (int y = 0; y < h; ++y) {
        // Pixel CENTRES, not corners: the bulb sits a fraction of a pixel below the bottom row, and
        // sampling at the corner would put the nearest sample half a pixel closer than it is.
        const float v = (static_cast<float>(y) + 0.5f - ly) / ry;
        const float vv = v * v;
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f - lx) / rx;
            // (1 + k*d^2)^-1.5 — note d^2 is what the curve wants, so there is no sqrt for the
            // distance itself and the one below is the power, not the radius.
            const float t = 1.0f + kFall * (u * u + vv);
            mask[static_cast<size_t>(y) * w + x] = toByte(1.0f / (t * std::sqrt(t)));
        }
    }
}

}  // namespace rabbitears
