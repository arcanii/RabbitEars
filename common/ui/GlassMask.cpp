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

}  // namespace

void buildGlassMask(int w, int h, const GlassParams& p, std::vector<uint8_t>& add,
                    std::vector<uint8_t>& mul) {
    const size_t n = (w > 0 && h > 0) ? static_cast<size_t>(w) * static_cast<size_t>(h) : 0;
    add.assign(n, 0);      // 0 == add nothing
    mul.assign(n, 255);    // 255 == leave the pixel alone
    if (!n || glassIsNoop(p)) return;

    float s = p.strength;
    if (s > 1.0f) s = 1.0f;

    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    // The bevel: the width of the CURVED band around the rim. Everything inside it is left
    // perfectly clear — that is the whole idea (see the header). Sized off the short axis so a
    // wide, short meter doesn't get a bevel that swallows it, and floored so it survives at 96dpi.
    // 0.20, not more: these panels are only ~30px tall, so a bevel taken from BOTH edges eats the
    // face fast — at 0.28 it met in the middle and the result read as blur again. A fifth of the
    // short axis leaves a clear central band you can still read the dial through.
    const float shortAxis = fw < fh ? fw : fh;
    float bevel = shortAxis * 0.20f;
    if (bevel < 3.0f) bevel = 3.0f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Distance to each edge, normalised across the bevel. 1 == at/outside the rim,
            // 0 == the inner boundary where the glass goes flat and clear.
            const float dT = 1.0f - smoothstep(0.0f, bevel, static_cast<float>(y));
            const float dL = 1.0f - smoothstep(0.0f, bevel, static_cast<float>(x));
            const float dB = 1.0f - smoothstep(0.0f, bevel, static_cast<float>(h - 1 - y));
            const float dR = 1.0f - smoothstep(0.0f, bevel, static_cast<float>(w - 1 - x));

            // Squaring concentrates every term hard against the rim. This is what stops the effect
            // reading as blur: the transition is quick and the middle of the pane is untouched.
            const float lit = dT * dT + 0.62f * dL * dL;   // light from above-left catches these
            const float shade = dB * dB + 0.62f * dR * dR;  // the far side of the curve falls away

            // A thin bright fillet right at the very top/left edge — the hard glint along the lip
            // of a thick plate. Narrow (the outer ~18% of the bevel) so it reads as a line.
            const float fillet = smoothstep(0.82f, 1.0f, dT) + 0.7f * smoothstep(0.82f, 1.0f, dL);

            float bright = lit * 0.34f + fillet * 0.30f;
            if (bright > 1.0f) bright = 1.0f;
            add[static_cast<size_t>(y) * w + x] = toByte(bright * s);

            float dark = shade * 0.42f;
            if (dark > 1.0f) dark = 1.0f;
            mul[static_cast<size_t>(y) * w + x] = toByte(1.0f - dark * s);
        }
    }
}

}  // namespace rabbitears
