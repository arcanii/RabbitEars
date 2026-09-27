// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/PhotoCells.h"

#include <array>
#include <cmath>

namespace rabbitears {
namespace {

// ---- colour helpers (sRGB bytes; the glow alone is added in linear light) ----------------------------

struct Rgb {
    float r, g, b;
};
Rgb rgbOf(COLORREF c) {
    return {static_cast<float>(GetRValue(c)), static_cast<float>(GetGValue(c)), static_cast<float>(GetBValue(c))};
}
Rgb mix(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
Rgb scale(const Rgb& a, float k) { return {a.r * k, a.g * k, a.b * k}; }
uint32_t pack(const Rgb& c) {
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}
const Rgb kWhite{255.0f, 255.0f, 255.0f};

// sRGB <-> linear light, 16-bit. The glow is light ADDED to what is there, and light adds in linear space;
// added in sRGB bytes a halo is too strong near the cell and too weak further out (PHOTOREAL.md lists
// gamma-space blending as a likely — not proven — reason the old scope bloom reads muddy).
struct LinearLut {
    std::array<uint16_t, 256> toLin{};
    std::vector<uint8_t> toSrgb;  // 65536 entries
    LinearLut() : toSrgb(65536) {
        for (int i = 0; i < 256; ++i) {
            const double s = i / 255.0;
            const double l = s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
            toLin[i] = static_cast<uint16_t>(std::lround(l * 65535.0));
        }
        for (int i = 0; i < 65536; ++i) {
            const double l = i / 65535.0;
            const double s = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
            toSrgb[i] = static_cast<uint8_t>(std::clamp(std::lround(s * 255.0), 0L, 255L));
        }
    }
};
const LinearLut& lut() {
    static const LinearLut l;
    return l;
}

// Signed distance (px) from a point (a pixel centre, `px, py`, in the rectangle's own frame — its top-left
// at 0, 0) to a w x h rounded rectangle with corner radius `rad`: < 0 inside.
float roundRectSdf(float px, float py, float w, float h, float rad) {
    const float hx = w * 0.5f, hy = h * 0.5f;
    const float qx = std::fabs(px - hx) - (hx - rad), qy = std::fabs(py - hy) - (hy - rad);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
}

// Each finish's field — the window the cells sit in, over the dial — for a panel colour.
//   LED: the housing the LEDs are set in, a shade under the panel (on a dark panel, near black);
//   LCD: dark, the polariser's black, a shade deeper still — on a light panel, a reflective LCD's grey-green;
//   VFD: always dark teal glass (a VFD window is dark on any front panel), with a little of a dark panel in it.
Rgb fieldOf(const PhotoScene& s) {
    const Rgb p = rgbOf(s.panel);
    const bool dark = photoPanelIsDark(s.panel);
    switch (s.finish) {
        case CellFinish::Led: return scale(p, dark ? 0.78f : 0.94f);
        case CellFinish::Lcd: return dark ? scale(p, 0.70f) : mix(p, Rgb{168.0f, 178.0f, 160.0f}, 0.45f);
        case CellFinish::Vfd: {
            const Rgb glass{5.0f, 16.0f, 19.0f};
            return dark ? mix(glass, p, 0.30f) : glass;
        }
    }
    return p;
}

// The glow a lit cell throws round itself: its reach (px) and strength (fraction of its colour, in linear
// light) — none on a light field, where light added to white is lost (and a halo clipped to white reads as
// a smudge): the Studio LED and the Backlit LCD on a light panel. The VFD's glass is always dark.
struct GlowSpec {
    int   m = 0;
    float strength = 0.0f;
};
GlowSpec glowOf(const PhotoScene& s, int w, int h) {
    GlowSpec g;
    const bool fieldDark = s.finish == CellFinish::Vfd || photoPanelIsDark(s.panel);
    if (!fieldDark) return g;
    const int e = std::min(w, h);
    const float knob = std::clamp(s.glow, 0.0f, 1.0f) * 2.0f;  // 0.5 = stock
    switch (s.finish) {
        case CellFinish::Led:
            g.m = std::max(1, static_cast<int>(e * 0.7f + 0.5f));
            g.strength = 0.26f * knob;
            break;
        case CellFinish::Lcd:  // the backlight spilling round a segment: faint, and no Glow knob
            g.m = std::max(1, static_cast<int>(e * 0.5f + 0.5f));
            g.strength = 0.12f;
            break;
        case CellFinish::Vfd:
            g.m = std::max(2, static_cast<int>(e * 0.7f + 0.5f));
            g.strength = 0.24f * knob;
            break;
    }
    if (g.strength <= 0.0f) g.m = 0;
    return g;
}

// One cell's pixels (w x h, opaque over the field).
void shadeCell(const PhotoScene& s, int w, int h, COLORREF litC, bool on, std::vector<uint32_t>& out) {
    out.assign(static_cast<size_t>(w) * h, 0);
    const Rgb field = fieldOf(s);
    const Rgb lit = rgbOf(litC), off = rgbOf(s.off), hot = rgbOf(s.hot);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const float e = static_cast<float>(std::min(w, h));
    // Detail by size, as the VU faces do it: a rim, a core and a glint only where there are pixels for them.
    const bool detail = e >= 5.0f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float u = px / fw, v = py / fh;  // 0..1 across the cell
            Rgb c{};
            float d = 0.0f, cov = 1.0f;
            switch (s.finish) {
                case CellFinish::Led: {
                    const float rad = detail ? e * 0.22f : 0.0f;
                    d = roundRectSdf(px, py, fw, fh, rad);
                    cov = std::clamp(0.5f - d, 0.0f, 1.0f);
                    const float inner = std::clamp(-d / std::max(1.0f, e * 0.2f), 0.0f, 1.0f);
                    // The key light's glint (above-left): a 1-px line of light one pixel inside the lens's rim,
                    // along its top from the left, fading out past the middle of the top edge and half-way down
                    // the left side — where a light above-left catches a domed lens.
                    const float ring = std::clamp(1.0f - std::fabs(d + 1.5f) / 0.8f, 0.0f, 1.0f);
                    const float glint = detail ? ring * std::clamp((0.55f - v) / 0.35f, 0.0f, 1.0f) *
                                                     std::clamp((0.75f - u) / 0.5f, 0.0f, 1.0f)
                                               : 0.0f;
                    if (on) {
                        // A diffused lens: its own colour, hotter toward the middle (a band along a wide
                        // segment), and a darker rim.
                        const float ex = (u - 0.5f) * 2.0f, ey = (v - 0.5f) * 2.0f;
                        const float core = detail ? std::pow(std::clamp(1.0f - (ex * ex * 0.45f + ey * ey), 0.0f, 1.0f), 1.3f) : 0.0f;
                        c = mix(lit, mix(lit, hot, 0.85f), 0.45f * core);
                        c = scale(c, detail ? 0.72f + 0.28f * inner : 1.0f);
                        c = mix(c, kWhite, 0.55f * glint);
                    } else {
                        // Off, the lens is still coloured plastic: its hue, dark, domed (lighter at the top).
                        c = mix(off, lit, 0.22f);
                        if (detail) c = scale(c, (1.12f - 0.28f * v) * (0.78f + 0.22f * inner));
                        c = mix(c, kWhite, 0.22f * glint);
                    }
                    break;
                }
                case CellFinish::Lcd: {
                    d = roundRectSdf(px, py, fw, fh, detail ? 0.6f : 0.0f);
                    cov = std::clamp(0.5f - d, 0.0f, 1.0f);
                    const bool edge = detail && d > -1.0f;  // the outermost ring: the segment's seal
                    // Flat, as an LCD segment is — lit, the light through it; unlit, a faint ghost: half-way
                    // from the field to the Dim colour tinted with its own.
                    c = on ? lit : mix(field, mix(off, lit, 0.30f), 0.5f);
                    if (edge) c = scale(c, on ? 0.80f : 0.88f);
                    break;
                }
                case CellFinish::Vfd: {
                    d = roundRectSdf(px, py, fw, fh, detail ? e * 0.14f : 0.0f);
                    const float soft = detail ? 1.4f : 1.0f;  // phosphor edges are soft
                    cov = std::clamp(0.5f - d / soft, 0.0f, 1.0f);
                    const float inner = std::clamp(-d / std::max(1.0f, e * 0.25f), 0.0f, 1.0f);
                    if (on) {
                        c = mix(lit, kWhite, 0.12f);
                        if (detail) c = scale(c, 0.84f + 0.16f * inner);
                    } else {
                        // An unlit anode: its phosphor (the Dim colour, tinted with its own) seen through the
                        // dark glass, barely there.
                        c = mix(field, mix(off, lit, 0.25f), 0.35f);
                    }
                    break;
                }
            }
            out[static_cast<size_t>(y) * w + x] = pack(mix(field, c, cov));
        }
}

// The glow round a lit cell: (w+2m) x (h+2m), linear light per channel, zero inside the cell — an exponential
// falloff windowed so it reaches nothing at the sprite's edge (cut off there, it left a visible rectangle).
void shadeGlow(const PhotoScene& s, int w, int h, COLORREF litC, const GlowSpec& g, std::vector<uint16_t>& out) {
    const int gw = w + 2 * g.m, gh = h + 2 * g.m;
    out.assign(static_cast<size_t>(gw) * gh * 3, 0);
    if (g.m <= 0) return;
    const LinearLut& L = lut();
    const Rgb lit = rgbOf(litC);
    const float lr = L.toLin[static_cast<int>(lit.r)], lg = L.toLin[static_cast<int>(lit.g)],
                lb = L.toLin[static_cast<int>(lit.b)];
    const float e = static_cast<float>(std::min(w, h));
    const float rad = s.finish == CellFinish::Lcd ? 0.6f : e * 0.2f;
    const float sigma = std::max(0.6f, g.m / 2.3f);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const float d = roundRectSdf(x - g.m + 0.5f, y - g.m + 0.5f, static_cast<float>(w),
                                         static_cast<float>(h), rad);
            if (d <= 0.0f) continue;
            const float win = std::max(0.0f, 1.0f - d / (g.m + 0.5f));
            const float a = g.strength * std::exp(-d / sigma) * win * win;
            const size_t i = (static_cast<size_t>(y) * gw + x) * 3;
            out[i + 0] = static_cast<uint16_t>(std::min(65535.0f, lr * a));
            out[i + 1] = static_cast<uint16_t>(std::min(65535.0f, lg * a));
            out[i + 2] = static_cast<uint16_t>(std::min(65535.0f, lb * a));
        }
}

}  // namespace

void PhotoCellCache::newFrame(const PhotoScene& scene) {
    if (!valid_ || !(scene == scene_)) {
        sprites_.clear();
        scene_ = scene;
        valid_ = true;
    }
    ++frame_;
}

const PhotoCellCache::Sprite& PhotoCellCache::get(const PhotoScene& scene, int w, int h, COLORREF lit, bool on) {
    for (Sprite& sp : sprites_)
        if (sp.w == w && sp.h == h && sp.lit == lit && sp.on == on) {
            sp.used = frame_;
            return sp;
        }
    Sprite* slot = nullptr;
    if (sprites_.size() < cap_) {
        slot = &sprites_.emplace_back();
    } else {
        slot = &*std::min_element(sprites_.begin(), sprites_.end(),
                                  [](const Sprite& a, const Sprite& b) { return a.used < b.used; });
    }
    Sprite& sp = *slot;
    sp.w = w;
    sp.h = h;
    sp.lit = lit;
    sp.on = on;
    sp.used = frame_;
    shadeCell(scene, w, h, lit, on, sp.px);
    const GlowSpec g = on ? glowOf(scene, w, h) : GlowSpec{};
    sp.m = g.m;
    if (g.m > 0) shadeGlow(scene, w, h, lit, g, sp.glow);
    else sp.glow.clear();
    return sp;
}

void paintPhotoCells(uint32_t* px, int stride, int bufH, const RECT& dialIn, const PhotoScene& scene,
                     const std::vector<PhotoCell>& cells, PhotoCellCache& cache) {
    if (!px || stride <= 0 || bufH <= 0) return;
    RECT dial = dialIn;
    dial.left = std::max<LONG>(dial.left, 0);
    dial.top = std::max<LONG>(dial.top, 0);
    dial.right = std::min<LONG>(dial.right, stride);
    dial.bottom = std::min<LONG>(dial.bottom, bufH);
    if (dial.right <= dial.left || dial.bottom <= dial.top) return;
    cache.newFrame(scene);

    // The field.
    const uint32_t field = pack(fieldOf(scene));
    for (LONG y = dial.top; y < dial.bottom; ++y)
        std::fill(px + static_cast<size_t>(y) * stride + dial.left, px + static_cast<size_t>(y) * stride + dial.right,
                  field);

    // The cells, each a straight copy of its sprite (clipped to the dial).
    for (const PhotoCell& c : cells) {
        const int w = c.r.right - c.r.left, h = c.r.bottom - c.r.top;
        if (w <= 0 || h <= 0) continue;
        const PhotoCellCache::Sprite& sp = cache.get(scene, w, h, c.lit, c.on);
        const int x0 = std::max<int>(c.r.left, dial.left), x1 = std::min<int>(c.r.right, dial.right);
        if (x1 <= x0) continue;
        for (int y = std::max<int>(c.r.top, dial.top); y < std::min<int>(c.r.bottom, dial.bottom); ++y)
            std::copy(sp.px.begin() + static_cast<size_t>(y - c.r.top) * w + (x0 - c.r.left),
                      sp.px.begin() + static_cast<size_t>(y - c.r.top) * w + (x1 - c.r.left),
                      px + static_cast<size_t>(y) * stride + x0);
    }

    // The lit cells' glow, added in linear light over everything drawn so far (so light from one cell
    // brightens its lit neighbours' edges too, as it does through a real diffuser).
    const LinearLut& L = lut();
    for (const PhotoCell& c : cells) {
        if (!c.on) continue;
        const int w = c.r.right - c.r.left, h = c.r.bottom - c.r.top;
        if (w <= 0 || h <= 0) continue;
        const PhotoCellCache::Sprite& sp = cache.get(scene, w, h, c.lit, c.on);
        if (sp.m <= 0) continue;
        const int gw = w + 2 * sp.m;
        const int gx0 = c.r.left - sp.m, gy0 = c.r.top - sp.m;
        const int x0 = std::max<int>(gx0, dial.left), x1 = std::min<int>(gx0 + gw, dial.right);
        const int y0 = std::max<int>(gy0, dial.top), y1 = std::min<int>(gy0 + h + 2 * sp.m, dial.bottom);
        if (x1 <= x0 || y1 <= y0) continue;  // clipped away (never, for cells inside the dial)
        for (int y = y0; y < y1; ++y) {
            uint32_t* row = px + static_cast<size_t>(y) * stride;
            const uint16_t* g = sp.glow.data() + (static_cast<size_t>(y - gy0) * gw + (x0 - gx0)) * 3;
            for (int x = x0; x < x1; ++x, g += 3) {
                if ((g[0] | g[1] | g[2]) == 0) continue;
                const uint32_t p = row[x];
                const uint32_t r = std::min<uint32_t>(65535u, L.toLin[(p >> 16) & 0xFF] + g[0]);
                const uint32_t gg = std::min<uint32_t>(65535u, L.toLin[(p >> 8) & 0xFF] + g[1]);
                const uint32_t b = std::min<uint32_t>(65535u, L.toLin[p & 0xFF] + g[2]);
                row[x] = (static_cast<uint32_t>(L.toSrgb[r]) << 16) | (static_cast<uint32_t>(L.toSrgb[gg]) << 8) |
                         L.toSrgb[b];
            }
        }
    }

    // The VFD's own structure, over the whole window, only where there are pixels for it: the filament wires
    // stretched across the window — thin lines a little darker than what is behind them, with a faint warm
    // sheen — once the pitch is 6 px or more (Extra large at 100 %, Large at 150 %), and the control grid — a
    // fine mesh just in front of the anodes — once it is 7 or more (Extra large at 150 %).
    if (scene.finish == CellFinish::Vfd && scene.pitch >= 6) {
        const bool mesh = scene.pitch >= 7;
        const int period = std::max(3, scene.pitch / 3);
        const int H = dial.bottom - dial.top;
        const int nFil = H >= 120 ? 3 : 2;
        for (LONG y = dial.top; y < dial.bottom; ++y) {
            uint32_t* row = px + static_cast<size_t>(y) * stride;
            const bool meshRow = (y - dial.top) % period == 0;
            bool filament = false;
            for (int i = 0; i < nFil; ++i)
                if (y - dial.top == (H * (i + 1)) / (nFil + 1)) filament = true;
            // In integer steps of 1/256 (the whole window, every frame): a filament keeps ~78 % of what is
            // behind it plus a warm twelfth, the mesh ~88 %.
            if (filament) {
                for (LONG x = dial.left; x < dial.right; ++x) {
                    const uint32_t p = row[x];
                    const uint32_t r = (((p >> 16) & 0xFF) * 200 + 70 * 21) >> 8,
                                   g = (((p >> 8) & 0xFF) * 200 + 44 * 21) >> 8, b = ((p & 0xFF) * 200 + 30 * 21) >> 8;
                    row[x] = (r << 16) | (g << 8) | b;
                }
                continue;
            }
            if (!mesh) continue;
            auto shade = [](uint32_t p) {
                return ((((p >> 16) & 0xFF) * 225 >> 8) << 16) | ((((p >> 8) & 0xFF) * 225 >> 8) << 8) |
                       ((p & 0xFF) * 225 >> 8);
            };
            if (meshRow) {
                for (LONG x = dial.left; x < dial.right; ++x) row[x] = shade(row[x]);
            } else {
                for (LONG x = dial.left; x < dial.right; x += period) row[x] = shade(row[x]);
            }
        }
    }
}

}  // namespace rabbitears
