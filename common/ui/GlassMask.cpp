// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/GlassMask.h"

#include <cmath>

namespace rabbitears {
namespace {

// Hermite smoothstep, clamped. The whole mask is built from this one curve so the highlight and
// the vignette share a falloff and can't disagree about what "soft" means.
float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

uint8_t toByte(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// Saturate at 1. The bezel's shadow term sums four squared edge distances, and at a corner two of
// them are both at full — without this the corner blows past 1 and clips. (The previous shipped
// mask had exactly that bug: at strength 1 the top-left `bright` computed to 1.0608, so every
// meter shipped a pure-white pixel in its corner.)
float sat1(float v) { return v > 1.0f ? 1.0f : v; }

// ---- BAND 1: the bezel, as ABSOLUTE achromatic tones ------------------------------------------
// Composited values below are for the DEFAULT DARK skin, where the gutter pixel band 1 lands on is
// windowBg 22 and the strip BEHIND the meter is the SAME 22, with the untouched themed border at
// 48 one pixel further out. That identity is the whole design problem: you cannot separate a panel
// from its chassis with darkness when there are only 22 levels of headroom, so the frame announces
// itself with a BRIGHT lip and closes itself against the DIAL with a near-black body.

// How much of the pixel underneath survives. 0.10 rather than 0: 10% keeps a trace of theme tint
// (and of a very bright dial element, as a faint sheen) so the frame doesn't read as a dead row,
// while still forcing an absolute tone — 22 -> 2 on dark, 255 -> 25 on light. Going to 0.0 flattens
// the frame to identical black on every skin and loses the anchor the lip needs.
constexpr float kBodyMul = 0.10f;

// The lit top chamfer: 22 -> 112. Chosen as the largest value that still reads as a LINE rather
// than as a light source: 112 is a 64-level step over the 48 border outside it and a 97-level step
// over the shadowed dial row inside it, so it cannot merge with either neighbour. It is also
// deliberately far BELOW the 211 the previous mask put on the border row — the brightest pixel of a
// meter must not be its own outline, or the panel reads as glowing rather than as thick.
constexpr float kLipTop = 0.431f;

// The left chamfer at 0.62 of the top (22 -> 70). 0.62 is inherited verbatim from the bevel this
// replaces — same light direction (above-left), one fewer number to defend.
constexpr float kLipLeft = 0.267f;

// The corner where the two chamfers meet: +18 over the top lip (112 -> 130). Enough to read as a
// node, under the ~+25 where a lone pixel starts to look like a stuck sub-pixel. It lives on the
// OPAQUE frame, not on the dial, so unlike a dial-side glint it cannot fight the cast shadow.
// Exactly 1px at 96dpi, 2x2 at 144, 3x3 at 192 — it scales with the frame, by construction.
constexpr float kLipCorner = 0.070f;

// The underside. 22 -> 4 (bottom) and 22 -> 7 (right): 15-18 levels BELOW the surrounding strip, so
// the shadow sides are visible dark lines rather than merely "less bright". The bottom is deepest
// because it faces directly away from an above-left key; the right only grazes it.
constexpr float kUnderBot = 0.008f;
constexpr float kUnderRight = 0.018f;

// ---- BAND 2: the shadow the bezel casts onto the dial behind it --------------------------------
// Reach, as a multiple of the frame's own thickness — because that is physically what sets it: how
// far the bezel stands proud of the dial. 2.4 gives 4.8px on a 30px meter and 9.6px on a 60px one,
// i.e. a constant 16% of height at every DPI, and (the point) the SAME absolute geometry on the
// 150x86 settings preview as on the 112x30 tray meter, so the preview shows what will ship.
constexpr float kShadowRun = 2.4f;

// Depth. A multiply, so it darkens dial content and leaves empty background nearly alone — which is
// correct, and is why it is loudest over lit LEDs, the VU face and the scope trace. 0.30 takes a
// lit cell from 205 to 143: plainly visible, never crushed. 0.45 crushed dim cells; 0.20 vanished.
constexpr float kShadowDeep = 0.30f;

}  // namespace

int glassChromePx(int w, int h, int reserved) {
    if (w <= 0 || h <= 0) return 0;
    const int shortAxis = w < h ? w : h;
    // 0.0667 (~1/15) is the FALLBACK only, for a caller that reserves no chrome: about a fifteenth
    // of the short axis is as much frame as a small instrument can afford — 2px on a 30px panel,
    // 4px on a 60px one. Floored at 2 because band 0 takes one pixel for the theme's border and the
    // bezel needs at least one more to exist. A renderer that DOES reserve chrome must pass it, and
    // then no fraction is involved at all.
    int c = (reserved > 0) ? reserved : static_cast<int>(shortAxis * 0.0667f + 0.5f);
    if (reserved <= 0 && c < 2) c = 2;
    // Never let the ring swallow the pane. Shrinking (rather than bailing) means a tiny panel
    // degrades to border-only + a shallow shadow instead of becoming a solid block.
    while (c > 0 && (w - 2 * c < 2 || h - 2 * c < 2)) --c;
    return c;
}

void buildGlassMask(int w, int h, const GlassParams& p, std::vector<uint8_t>& add,
                    std::vector<uint8_t>& mul) {
    const size_t n = (w > 0 && h > 0) ? static_cast<size_t>(w) * static_cast<size_t>(h) : 0;
    add.assign(n, 0);      // 0 == add nothing
    mul.assign(n, 255);    // 255 == leave the pixel alone
    if (!n || glassIsNoop(p)) return;

    float s = p.strength;
    if (s > 1.0f) s = 1.0f;

    const int chrome = glassChromePx(w, h, p.chromePx);
    if (chrome <= 0) return;   // a pane too small to carry a frame keeps its dial instead

    const float fchrome = static_cast<float>(chrome);
    const float shadowW = fchrome * kShadowRun;
    // Band 1's opacity is constant, so hoist it: one toByte for the whole ring.
    const uint8_t bezelMul = toByte(1.0f - (1.0f - kBodyMul) * s);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const int eT = y, eB = h - 1 - y, eL = x, eR = w - 1 - x;
            // Chebyshev ring index: distance to the NEAREST edge. This is what makes the frame a
            // uniform-thickness rectangle on both axes for free, rather than a fraction of each.
            int ring = eT;
            if (eB < ring) ring = eB;
            if (eL < ring) ring = eL;
            if (eR < ring) ring = eR;

            if (ring < chrome) {
                // BAND 0 — the theme's FrameRect. Hands off, at every strength, on every skin.
                if (ring == 0) continue;

                // BAND 1 — the bezel. NO smoothstep anywhere in here: at 26-30px a 1px luminance
                // STEP survives and a ramp does not, and a ramp is what read as blur. One flat
                // value per side, so each run of the frame is a single byte and a lone anomalous
                // pixel can never look like a blit artifact.
                const int litD = (eT < eL) ? eT : eL;
                const int shD = (eB < eR) ? eB : eR;
                float a;
                if (shD <= litD) {
                    // Ties go to the shadow, which leaves exactly ONE lit corner (top-left) and so
                    // gives the light a single unambiguous direction. The other three corners are
                    // 1px mitres — which is how a butt-jointed 3D edge is drawn.
                    a = (eB <= eR) ? kUnderBot : kUnderRight;
                } else if (eT < chrome && eL < chrome) {
                    a = kLipTop + kLipCorner;   // the corner pip
                } else if (eT <= eL) {
                    a = kLipTop;                // a 1px bright top edge...
                } else {
                    a = kLipLeft;               // ...over a subordinate left one
                }
                mul[i] = bezelMul;
                add[i] = toByte(a * s);
                continue;
            }

            // BAND 2 — the dial, seen behind the glass. The bezel is opaque and in FRONT, so it
            // throws a shadow onto what is behind it; that, and not a bright line, is the cue that
            // says "an opaque thing is in front of a recessed thing". Measured from the bezel's
            // INNER face and squared, so it packs hard against the rim instead of drifting.
            const float dT = 1.0f - smoothstep(0.0f, shadowW, static_cast<float>(eT) - fchrome);
            const float dL = 1.0f - smoothstep(0.0f, shadowW, static_cast<float>(eL) - fchrome);
            const float dB = 1.0f - smoothstep(0.0f, shadowW, static_cast<float>(eB) - fchrome);
            const float dR = 1.0f - smoothstep(0.0f, shadowW, static_cast<float>(eR) - fchrome);
            // 0.34 on the bottom/right, not 0: the inner wall of a recessed panel bounces some
            // light back. Zero there and the dial looks like it is sliding out of its frame;
            // equal there and the light loses its direction.
            const float shade = sat1(dT * dT + 0.62f * dL * dL +
                                     0.34f * (dB * dB + 0.62f * dR * dR));
            mul[i] = toByte(1.0f - kShadowDeep * shade * s);
            // add[i] stays 0. Nothing brightens the dial. That is the invariant.
        }
    }
}

}  // namespace rabbitears
