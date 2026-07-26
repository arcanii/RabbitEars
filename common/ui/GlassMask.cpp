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
    // Edge ramp as a fraction of the SHORT axis, so a wide-but-short meter doesn't get a vignette
    // that swallows it. Clamped to a sane band: below ~2px it stops reading, past ~18% it starts
    // looking like a bad drop shadow.
    const float shortAxis = fw < fh ? fw : fh;
    float ramp = shortAxis * 0.18f;
    if (ramp < 2.0f) ramp = 2.0f;

    for (int y = 0; y < h; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / fh;  // 0..1 top->bottom
        for (int x = 0; x < w; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / fw;

            // --- specular streak: a soft diagonal band across the upper-left, the classic
            // "light source off to one side" cue. Weighted 0.7/0.3 so it leans horizontal, which
            // suits these wide, short panels better than a 45° streak.
            const float t = 0.7f * fx + 0.3f * fy;
            const float band = smoothstep(0.02f, 0.16f, t) - smoothstep(0.22f, 0.42f, t);

            // --- a gentle dome: brightest across the upper third, feathering downward, so the
            // pane reads as curved rather than flat.
            const float domeY = 0.30f + 0.18f * std::sin(3.14159265f * fx);
            const float dome = 1.0f - smoothstep(0.0f, 0.55f, std::fabs(fy - domeY));

            float spec = band * (0.55f + 0.45f * dome);
            if (spec < 0.0f) spec = 0.0f;

            // --- ENVIRONMENT REFLECTION: what actually makes glass read as glass. A real pane
            // mirrors the room, so bake in a stylised one. It costs nothing extra — like everything
            // here it is a pure function of position, so it folds into the same LUT.
            //
            // Three elements, in the order a viewer reads them:
            //   1. the overhead light — a broad soft bar across the top third, the dominant
            //      reflection in any lit room, bowed slightly to suggest a curved pane;
            //   2. a window — a soft-edged bright rectangle off to one side. This is the cue that
            //      says "reflection" rather than "gradient", because it has EDGES;
            //   3. the room falloff — everything below the horizon reflects darker floor, so the
            //      lower half stays clean and the dials underneath remain readable.
            const float ceilY = 0.16f + 0.05f * std::sin(3.14159265f * fx);
            const float ceiling = 1.0f - smoothstep(0.0f, 0.22f, std::fabs(fy - ceilY));

            // Window pane, right-of-centre, with a soft mullion splitting it — the giveaway detail.
            const float wx = 1.0f - smoothstep(0.0f, 0.10f, std::fabs(fx - 0.72f) - 0.10f);
            const float wy = 1.0f - smoothstep(0.0f, 0.10f, std::fabs(fy - 0.30f) - 0.12f);
            float window = wx * wy;
            const float mullion = 1.0f - smoothstep(0.0f, 0.018f, std::fabs(fx - 0.72f));
            window *= (1.0f - 0.55f * mullion);

            // Below the horizon the room is darker, so reflections fade out fast.
            const float horizon = 1.0f - smoothstep(0.34f, 0.62f, fy);

            const float reflection = (ceiling * 0.55f + window * 0.75f) * horizon;

            add[static_cast<size_t>(y) * w + x] = toByte((spec * 0.42f + reflection * 0.30f) * s);

            // --- edge darkening: an inward ramp from all four edges. This is the element that
            // actually sells "recessed behind something" — more so than the highlight.
            const float dx = static_cast<float>(x < w - 1 - x ? x : w - 1 - x);
            const float dy = static_cast<float>(y < h - 1 - y ? y : h - 1 - y);
            const float d = dx < dy ? dx : dy;
            const float dark = 1.0f - smoothstep(0.0f, ramp, d);  // 1 at the very edge -> 0 inside
            mul[static_cast<size_t>(y) * w + x] = toByte(1.0f - dark * 0.38f * s);
        }
    }
}

}  // namespace rabbitears
