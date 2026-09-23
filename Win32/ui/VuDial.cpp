// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VuDial.h"

#include <algorithm>
#include <cmath>

#include <objidl.h>  // IStream — required by gdiplus.h below
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the std versions into
// the Gdiplus namespace before including it (same dance as MiniMeter.cpp).
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "ui/VuLamp.h"  // vuLampIsUnset — "pick black to get the stock bulb back", as since 0.2.15

namespace rabbitears {
namespace {

constexpr float kPi = 3.14159265f;

struct Rgb {
    float r, g, b;  // 0..1
};
Rgb rgbOf(COLORREF c) { return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f}; }
Rgb mix(const Rgb& a, const Rgb& b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
Rgb scale(const Rgb& a, float k) { return {a.r * k, a.g * k, a.b * k}; }
uint32_t pack(const Rgb& c) {
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}
float smooth01(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Deterministic per-pixel noise in [-1, 1] — a static texture must come out identical every time it
// is built (and on every render-tool run), so no rand().
float hashNoise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFF) / 32767.5f - 1.0f;
}

// The lamp as a pure hue (peak channel 1), or `stock` when the swatch means "the face's own bulb":
// CLR_INVALID, or too dark to be a lamp (VuLamp's rule, so picking black still restores the stock
// bulb exactly as it has since 0.2.15).
struct Lamp {
    bool stock = true;
    Rgb  hue{1.0f, 1.0f, 1.0f};
};
Lamp lampOf(COLORREF c) {
    Lamp l;
    if (c == CLR_INVALID || vuLampIsUnset(GetRValue(c), GetGValue(c), GetBValue(c))) return l;
    const Rgb v = rgbOf(c);
    const float mx = std::max({v.r, v.g, v.b});
    l.stock = false;
    l.hue = scale(v, 1.0f / mx);
    return l;
}

// ---- the printed scales ---------------------------------------------------------------------------
// Where each dB mark sits along the arc (0 = the arc's left end, 1 = its right end), MEASURED off the
// reference instruments rather than computed. A textbook movement is linear in voltage, which
// crowds -20..-3 into the left third; neither reference prints that — both space the low marks out —
// so a computed scale looked wrong beside them. The caller positions the needle through the same
// table (vuDialPositionOfDb). Below -20 the position falls linearly in voltage to the arc's left end.
struct ScaleTable {
    const float* db;
    const float* f;
    int          n;
    float        fullDb;  // the scale's top mark
};
constexpr float kBacklitDb[] = {-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3, 4, 5};
constexpr float kBacklitF[] = {0.13f, 0.26f, 0.36f, 0.44f, 0.53f, 0.575f, 0.62f,
                               0.67f, 0.71f, 0.75f, 0.79f, 0.84f, 0.89f};
constexpr float kSilverDb[] = {-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6};
constexpr float kSilverF[] = {0.05f, 0.14f, 0.22f, 0.29f, 0.40f, 0.47f, 0.56f,
                              0.64f, 0.71f, 0.78f, 0.84f, 0.90f, 0.95f, 1.00f};
constexpr ScaleTable kBacklitScale{kBacklitDb, kBacklitF, 13, 5.0f};
constexpr ScaleTable kSilverScale{kSilverDb, kSilverF, 14, 6.0f};

float fOfDb(const ScaleTable& t, float db) {
    if (db <= t.db[0]) return t.f[0] * std::pow(10.0f, (db - t.db[0]) / 20.0f);
    for (int i = 1; i < t.n; ++i)
        if (db <= t.db[i]) {
            const float u = (db - t.db[i - 1]) / (t.db[i] - t.db[i - 1]);
            return t.f[i - 1] + (t.f[i] - t.f[i - 1]) * u;
        }
    return t.f[t.n - 1];
}
const ScaleTable& scaleOf(VuFace face) { return face == VuFace::Silver ? kSilverScale : kBacklitScale; }

constexpr COLORREF kBacklitNeedle = RGB(24, 22, 20);
constexpr COLORREF kSilverNeedle = RGB(18, 18, 20);

// The needle's shadow eases off as the dial grows: tuned at the 26-px tray dial, and scaled up
// linearly it becomes a hard band as wide as the needle — the "second needle" MiniMeter's old look
// already learned about. Same square-root rule.
void setShadow(VuDialLayout& lay, float H) {
    const float grow = std::sqrt(std::max(1.0f, H / 26.0f));
    lay.shadowW = lay.needleW * (1.0f + 1.2f / grow);
    lay.shadowA = 52.0f / grow;
}

// ---- faceplates -----------------------------------------------------------------------------------
// Brushed aluminium: the grain runs horizontally, so the noise is mostly a function of the ROW (long
// streaks), a little of short runs along the row, and a whisper per pixel. Lit from above, so the
// plate is a touch brighter at the top — the key light the glass bezel already implies.
void paintFaceplate(VuFace face, int w, int h, std::vector<uint32_t>& px) {
    const bool silver = face == VuFace::Silver;
    const Rgb base = silver ? Rgb{0.765f, 0.755f, 0.730f} : Rgb{0.118f, 0.118f, 0.128f};
    const float row = silver ? 0.028f : 0.014f, run = silver ? 0.016f : 0.008f,
                grain = silver ? 0.008f : 0.004f, sheen = silver ? 0.07f : 0.035f;
    for (int y = 0; y < h; ++y) {
        const float ny = hashNoise(0, static_cast<uint32_t>(y), 11);
        const float lit = sheen * (1.0f - static_cast<float>(y) / static_cast<float>(h)) - sheen * 0.35f;
        for (int x = 0; x < w; ++x) {
            const float nr = hashNoise(static_cast<uint32_t>(x) / 9u, static_cast<uint32_t>(y), 12);
            const float ng = hashNoise(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 13);
            const float d = ny * row + nr * run + ng * grain + lit;
            px[static_cast<size_t>(y) * w + x] = pack({base.r + d, base.g + d, base.b + d});
        }
    }
}

// The window's rim in the faceplate: a dark cut on three sides and one lit edge.
void paintRim(std::vector<uint32_t>& px, int w, int h, int ox, int oy, int ow, int oh, const Rgb& dark,
              const Rgb& lit, bool litBottom) {
    for (int x = ox - 1; x <= ox + ow; ++x) {
        if (x < 0 || x >= w) continue;
        if (oy - 1 >= 0) px[static_cast<size_t>(oy - 1) * w + x] = pack(dark);
        if (oy + oh < h) px[static_cast<size_t>(oy + oh) * w + x] = pack(litBottom ? lit : dark);
    }
    for (int y = oy; y < oy + oh; ++y) {
        if (ox - 1 >= 0) px[static_cast<size_t>(y) * w + (ox - 1)] = pack(dark);
        if (ox + ow < w) px[static_cast<size_t>(y) * w + (ox + ow)] = pack(litBottom ? dark : lit);
    }
}

// ---- drawing helpers ------------------------------------------------------------------------------
Gdiplus::Color gc(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}
Gdiplus::PointF polar(float cx, float cy, float deg, float r) {
    const float a = deg * kPi / 180.0f;
    return Gdiplus::PointF(cx + r * std::cos(a), cy + r * std::sin(a));
}
// One line of legend text centred on (x, y). Arial: every Windows install has it, and a VU scale is
// set in a plain grotesque. Grey-scale antialiasing — ClearType's colour fringes would sit on a
// coloured card and read as a printing fault.
void legend(Gdiplus::Graphics& g, const wchar_t* s, float px, float x, float y, COLORREF c) {
    Gdiplus::FontFamily fam(L"Arial");
    const Gdiplus::FontFamily* f = fam.IsAvailable() ? &fam : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::Font font(f, px, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush b(gc(c));
    // The line box centres ascent+descent; digits and capitals sit high in it, so nudge down a hair.
    g.DrawString(s, -1, &font, Gdiplus::PointF(x, y + px * 0.06f), &sf, &b);
}

// Ticks, numerals and arc segments all place by position f along one sweep.
struct Sweep {
    float cx, cy, a0, span;
    float angle(float f) const { return a0 + span * f; }
    Gdiplus::PointF at(float f, float r) const { return polar(cx, cy, angle(f), r); }
};
void arcStroke(Gdiplus::Graphics& g, const Sweep& s, float r, float f0, float f1, COLORREF c,
               float width) {
    Gdiplus::Pen pen(gc(c), width);
    g.DrawArc(&pen, Gdiplus::RectF(s.cx - r, s.cy - r, r * 2.0f, r * 2.0f), s.angle(f0),
              s.span * (f1 - f0));
}
void tick(Gdiplus::Graphics& g, const Sweep& s, float f, float r0, float r1, COLORREF c, float width) {
    Gdiplus::Pen pen(gc(c), width);
    g.DrawLine(&pen, s.at(f, r0), s.at(f, r1));
}
void setupGraphics(Gdiplus::Graphics& g) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
}

// ---- the Backlit face -----------------------------------------------------------------------------
// Proportions are fractions of the card height H, measured off the reference: a 1.62:1 card, the
// scale's crown at ~0.43H with the numerals above it, "VU" at ~0.7H, PEAK in the top-right corner.
constexpr float kBacklitAspect = 1.62f;

void buildBacklit(const VuDialSpec& spec, int w, int h, float minLine, std::vector<uint32_t>& px,
                  VuDialLayout& lay) {
    const int my = std::max(1, static_cast<int>(std::lround(h * 0.08f)));
    const int openH = h - 2 * my;
    const int mx = std::max(1, static_cast<int>(std::lround(h * 0.10f)));
    const int openW = std::min(w - 2 * mx, static_cast<int>(std::lround(openH * kBacklitAspect)));
    if (openH < 6 || openW < 8) return;
    const int ox = (w - openW) / 2, oy = my;
    const float H = static_cast<float>(openH), W = static_cast<float>(openW);
    lay.card = RECT{ox, oy, ox + openW, oy + openH};

    // -- the light: a bulb behind the bottom-centre shroud, glowing up through the card -------------
    const Lamp lamp = lampOf(spec.lamp);
    Rgb top, mid, hot, core;
    if (lamp.stock) {
        // Medians sampled off the reference photo: the card's top edge, mid-card, the saturated
        // yellow ring round the bulb, and its pale core.
        top = rgbOf(RGB(226, 190, 134));
        mid = rgbOf(RGB(252, 214, 141));
        hot = rgbOf(RGB(255, 236, 52));
        core = rgbOf(RGB(255, 250, 150));
    } else {
        top = scale(mix(lamp.hue, {1, 1, 1}, 0.62f), 0.86f);
        mid = scale(mix(lamp.hue, {1, 1, 1}, 0.45f), 0.98f);
        hot = lamp.hue;
        core = mix(lamp.hue, {1, 1, 1}, 0.55f);
    }
    const float lampX = ox + W * 0.5f, lampY = oy + H * 1.02f;
    const float rx = H * 1.05f, ry = H * 0.72f;      // the broad field: the card warming toward the bulb
    const float blobRx = H * 0.52f, blobRy = H * 0.36f;  // the tight one: the bulb's own blob
    for (int y = 0; y < openH; ++y) {
        const float yy = static_cast<float>(oy + y) + 0.5f;
        const float dy = (yy - lampY) / ry;
        // The window's top lip shades the card below it; the sides a little.
        const float lip = y < H * 0.12f ? 0.60f + 0.40f * std::pow(y / (H * 0.12f), 0.7f) : 1.0f;
        for (int x = 0; x < openW; ++x) {
            const float xx = static_cast<float>(ox + x) + 0.5f;
            const float dx = (xx - lampX) / rx;
            const float g = 1.0f / std::pow(1.0f + 1.4f * (dx * dx + dy * dy), 1.6f);
            const float ex = (xx - lampX) / blobRx, ey = (yy - lampY) / blobRy;
            const float b = 1.0f / std::pow(1.0f + 1.6f * (ex * ex + ey * ey), 1.8f);
            // Four stops over two fields, not a two-colour ramp: the reference's card warms steadily
            // toward the bulb (the broad field), and turns a saturated yellow only in a tight blob
            // round it, with a paler core (the tight one).
            Rgb c = mix(top, mid, smooth01(0.06f, 0.55f, g));
            c = mix(c, hot, smooth01(0.14f, 0.62f, b));
            c = mix(c, core, smooth01(0.70f, 1.0f, b) * 0.8f);
            const float edge = std::min(static_cast<float>(x), W - 1.0f - x) / (H * 0.06f);
            const float side = edge < 1.0f ? 0.86f + 0.14f * edge : 1.0f;
            c = scale(c, lip * side);
            if (x == 0 || y == 0 || x == openW - 1 || y == openH - 1) c = scale(c, 0.78f);  // card edge
            px[static_cast<size_t>(oy + y) * w + (ox + x)] = pack(c);
        }
    }
    paintRim(px, w, h, ox, oy, openW, openH, {0.035f, 0.035f, 0.04f}, {0.26f, 0.26f, 0.27f}, true);

    // -- the printed scale -------------------------------------------------------------------------
    Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(px.data()));
    Gdiplus::Graphics g(&bmp);
    setupGraphics(g);
    g.SetClip(Gdiplus::Rect(ox, oy, openW, openH));

    const COLORREF ink = RGB(34, 30, 26), red = spec.redZone;
    // Fitted to the reference's arc (three points on it, corrected for the photo's angle): its crown
    // at ~0.43H, its ends ~0.16H lower, spanning ~3/4 of the card — so its centre, the real pivot,
    // lies ~0.6H BELOW the window, and the brass boss on the bottom edge is the lamp's shroud.
    const float R = H * 1.20f;
    const float half = std::min(34.0f, std::asin(std::min(0.95f, 0.38f * W / R)) * 180.0f / kPi);
    const Sweep s{ox + W * 0.5f, oy + H * 1.63f, -90.0f - half, 2.0f * half};
    const ScaleTable& T = kBacklitScale;
    const float f0 = fOfDb(T, 0.0f);
    const float arcW = std::max(minLine, H * 0.024f);
    arcStroke(g, s, R, 0.0f, f0, ink, arcW);
    // The red zone is a wedge that THICKENS toward the top of the scale, as the reference prints it.
    {
        constexpr int n = 24;
        Gdiplus::PointF pts[2 * n + 2];
        const float fEnd = 0.95f;
        for (int i = 0; i <= n; ++i) {
            const float t = static_cast<float>(i) / n;
            const float f = f0 + (fEnd - f0) * t;
            pts[i] = s.at(f, R + arcW * 0.5f + (H * 0.055f - arcW * 0.5f) * t);
            pts[2 * n + 1 - i] = s.at(f, R - arcW * 0.5f);
        }
        Gdiplus::SolidBrush b(gc(red));
        g.FillPolygon(&b, pts, 2 * n + 2);
    }
    struct Mark {
        float          db;
        const wchar_t* label;  // nullptr = a minor tick
    };
    static const Mark kMarks[] = {{-20, L"20"}, {-10, L"10"}, {-7, L"7"},   {-5, L"5"},  {-3, L"3"},
                                  {-2, nullptr}, {-1, nullptr}, {0, L"0"},   {1, nullptr}, {2, nullptr},
                                  {3, L"3"},     {4, nullptr},  {5, L"5"}};
    const float majL = H * 0.090f, minL = H * 0.052f;
    const float majW = std::max(minLine, H * 0.018f), minW = std::max(minLine * 0.8f, H * 0.013f);
    const float numPx = H * 0.12f;
    const bool numerals = numPx >= 5.0f;
    for (const Mark& m : kMarks) {
        const float f = fOfDb(T, m.db);
        const COLORREF c = m.db < 0.0f ? ink : red;
        tick(g, s, f, R, R + (m.label ? majL : minL), c, m.label ? majW : minW);
        if (m.label && numerals) {
            const Gdiplus::PointF p = s.at(f, R + majL + numPx * 0.72f);
            legend(g, m.label, numPx, p.X, p.Y, c);
        }
    }
    if (numerals) {
        const Gdiplus::PointF lo = s.at(-0.03f, R + majL * 0.6f), hi = s.at(0.985f, R + majL * 0.6f);
        legend(g, L"–", numPx * 1.15f, lo.X, lo.Y, ink);
        legend(g, L"+", numPx * 1.15f, hi.X, hi.Y, red);
    }
    const float vuPx = H * 0.17f;
    if (vuPx >= 7.0f) legend(g, L"VU", vuPx, s.cx, oy + H * 0.70f, ink);

    // PEAK: the lamp (unlit here — drawVuDialNeedle lights it) and, where it can be read, its legend.
    lay.ledR = std::max(1.2f, H * 0.042f);
    lay.ledX = ox + W - H * 0.12f;
    lay.ledY = oy + H * 0.12f;
    {
        const float r = lay.ledR;
        Gdiplus::SolidBrush dome(Gdiplus::Color(255, 74, 14, 10));
        g.FillEllipse(&dome, lay.ledX - r, lay.ledY - r, r * 2.0f, r * 2.0f);
        if (r >= 2.5f) {
            Gdiplus::SolidBrush glint(Gdiplus::Color(150, 230, 190, 180));
            g.FillEllipse(&glint, lay.ledX - r * 0.55f, lay.ledY - r * 0.6f, r * 0.55f, r * 0.45f);
        }
        const float peakPx = H * 0.085f;
        if (peakPx >= 5.0f) {
            const float tw = peakPx * 2.8f;
            legend(g, L"PEAK", peakPx, lay.ledX - r * 2.2f - H * 0.02f - tw * 0.5f, lay.ledY, ink);
        }
    }
    // The lamp's shroud: a dark brass half-disc on the bottom edge, the bulb glowing round it.
    {
        const float r = H * 0.085f, bx = s.cx, by = oy + H;
        Gdiplus::SolidBrush boss(Gdiplus::Color(255, 70, 54, 24));
        g.FillEllipse(&boss, bx - r, by - r, r * 2.0f, r * 2.0f);
        Gdiplus::Pen rim(Gdiplus::Color(200, 150, 118, 52), std::max(minLine * 0.8f, H * 0.012f));
        g.DrawArc(&rim, Gdiplus::RectF(bx - r, by - r, r * 2.0f, r * 2.0f), 180.0f, 180.0f);
    }
    g.ResetClip();

    lay.pivotX = s.cx;
    lay.pivotY = s.cy;
    lay.a0 = s.a0;
    lay.sweep = s.span;
    lay.needleIn = (s.cy - (oy + H)) - H * 0.03f;  // from just below the card's bottom edge
    lay.needleOut = R + majL * 0.85f;
    lay.needleW = std::max(0.9f, H * 0.012f);
    lay.shadowDeg = 1.1f;
    setShadow(lay, H);
    lay.face = VuFace::Backlit;
}

// ---- the Silver face ------------------------------------------------------------------------------
constexpr float kSilverAspect = 2.45f;

void buildSilver(const VuDialSpec& spec, int w, int h, float minLine, std::vector<uint32_t>& px,
                 VuDialLayout& lay) {
    const int my = std::max(1, static_cast<int>(std::lround(h * 0.07f)));
    const int openH = h - 2 * my;
    const int mx = std::max(1, static_cast<int>(std::lround(h * 0.09f)));
    const int openW = std::min(w - 2 * mx, static_cast<int>(std::lround(openH * kSilverAspect)));
    if (openH < 6 || openW < 8) return;
    const int ox = (w - openW) / 2, oy = my;
    const float H = static_cast<float>(openH);
    // The window is deep: a polished inner wall shows down its right side, a shadowed sliver down its
    // left, and the card sits between them.
    const int wallR = std::max(1, static_cast<int>(std::lround(H * 0.20f)));
    const int wallL = std::max(1, static_cast<int>(std::lround(H * 0.035f)));
    const int cx0 = ox + wallL, cx1 = ox + openW - wallR;
    const float CW = static_cast<float>(cx1 - cx0);
    lay.card = RECT{cx0, oy, cx1, oy + openH};

    const Lamp lamp = lampOf(spec.lamp);
    const Rgb ivory = lamp.stock ? rgbOf(RGB(222, 208, 174)) : scale(mix(lamp.hue, {1, 1, 1}, 0.74f), 0.90f);
    for (int y = 0; y < openH; ++y) {
        const float fy = static_cast<float>(y) + 0.5f;
        // Evenly lit, but the deep window's top wall throws a real shadow onto the card's top.
        const float lip = fy < H * 0.17f ? 0.46f + 0.54f * std::pow(fy / (H * 0.17f), 0.8f) : 1.0f;
        const float fall = 0.90f + 0.10f * smooth01(0.0f, H * 0.6f, fy);
        for (int x = ox; x < ox + openW; ++x) {
            Rgb c;
            if (x < cx0) {
                c = scale(ivory, 0.35f * lip);  // the left wall, in shadow
            } else if (x < cx1) {
                const float u = (x - cx0 + 0.5f) / CW;
                c = scale(ivory, lip * fall * (1.0f - 0.10f * u * u));
                if (x == cx0 || x == cx1 - 1) c = scale(c, 0.82f);
            } else {
                // The polished right wall: vertical brushing, brightest where it faces the light.
                const float u = (x - cx1 + 0.5f) / static_cast<float>(wallR);
                const float n = hashNoise(static_cast<uint32_t>(x), 0, 21) * 0.05f;
                const float lum =
                    0.58f + 0.34f * std::pow(std::sin(kPi * std::min(0.999f, u * 0.9f + 0.05f)), 0.7f) + n;
                c = scale(Rgb{0.97f, 0.97f, 0.95f}, lum * (0.70f + 0.30f * lip));
            }
            px[static_cast<size_t>(oy + y) * w + x] = pack(c);
        }
    }
    paintRim(px, w, h, ox, oy, openW, openH, {0.30f, 0.29f, 0.28f}, {0.93f, 0.92f, 0.90f}, true);

    Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(px.data()));
    Gdiplus::Graphics g(&bmp);
    setupGraphics(g);
    g.SetClip(Gdiplus::Rect(cx0, oy, cx1 - cx0, openH));

    const COLORREF ink = RGB(44, 40, 36), blue = RGB(34, 46, 134), red = spec.redZone;
    // Fitted to the reference's arc: the pivot is well below the window, so the scale is a long,
    // shallow arc — crown at ~0.37H, ends ~0.2H lower, from ~17% to ~88% of the card.
    const float R = H * 1.70f;
    const float half = std::asin(std::min(0.9f, 0.355f * CW / R)) * 180.0f / kPi;
    const Sweep s{cx0 + CW * 0.525f, oy + H * 2.07f, -90.0f - half, 2.0f * half};
    const ScaleTable& T = kSilverScale;
    const float f0 = fOfDb(T, 0.0f), fm1 = fOfDb(T, -1.0f);
    const float arcW = std::max(minLine, H * 0.038f);
    arcStroke(g, s, R, 0.0f, fm1 + 0.012f, blue, arcW);
    arcStroke(g, s, R, f0 - 0.010f, 1.005f, red, arcW);
    const float majL = H * 0.10f, minL = H * 0.06f;
    const float majW = std::max(minLine, H * 0.019f), minW = std::max(minLine * 0.8f, H * 0.013f);
    const float numPx = H * 0.108f;
    const bool numerals = numPx >= 5.0f;
    struct Mark {
        float          db;
        const wchar_t* label;
        bool           major;
    };
    // Blue ticks stand above the arc; the red zone's hang below it, as on the reference.
    static const Mark kBlue[] = {{-20, L"−20", true}, {-10, L"10", true},    {-7, L"7", true},
                                 {-5, L"5", true},         {-4, nullptr, false},  {-3, L"3", true},
                                 {-2, nullptr, false},     {-1, L"1", true}};
    static const Mark kRed[] = {{0, L"0", true},     {1, L"1", true}, {2, L"2", true}, {3, nullptr, true},
                                {4, L"4", true},     {5, nullptr, true}, {6, L"+6", true}};
    for (const Mark& m : kBlue) {
        const float f = fOfDb(T, m.db);
        tick(g, s, f, R, R + (m.major ? majL : minL), blue, m.major ? majW : minW);
        if (m.label && numerals) {
            Gdiplus::PointF p = s.at(f, R + majL + numPx * 0.70f);
            // "-20" is printed to the LEFT of its tick on the reference: centred on it, it runs into
            // the "10" a few degrees along.
            if (m.db == -20.0f) p.X -= numPx * 0.85f;
            if (m.db == -10.0f) p.X -= numPx * 0.30f;  // and "10" gives "7" room, as printed
            legend(g, m.label, numPx, p.X, p.Y, blue);
        }
    }
    for (const Mark& m : kRed) {
        const float f = fOfDb(T, m.db);
        tick(g, s, f, R, R - majL, red, majW);
        if (m.label && numerals) {
            const Gdiplus::PointF p = s.at(f, R + arcW + numPx * 0.60f);
            legend(g, m.label, numPx, p.X, p.Y, red);
        }
    }
    // The 0-100 % modulation scale under the blue arc: voltage, so 100 % = 0 VU and 50 % = -6 dB.
    // Smaller type than the main scale, so it waits for more room: under ~7 px its labels run together.
    const float pctPx = H * 0.078f;
    if (pctPx >= 7.0f) {
        static const wchar_t* kPct[] = {L"0", L"20", L"40", L"60", L"80", L"100"};
        for (int i = 0; i <= 5; ++i) {
            const float f = i == 0 ? 0.0f : fOfDb(T, 20.0f * std::log10(i / 5.0f));
            tick(g, s, f, R - arcW * 0.5f, R - H * 0.065f, blue, minW);
            const Gdiplus::PointF p = s.at(f, R - H * 0.065f - pctPx * 0.78f);
            legend(g, kPct[i], pctPx, p.X, p.Y, blue);
        }
    }
    const float vuPx = H * 0.11f;
    if (vuPx >= 5.0f) {
        legend(g, L"VU", vuPx, cx0 + H * 0.15f, oy + H * 0.13f, ink);
        legend(g, L"VU", vuPx, cx1 - H * 0.17f, oy + H * 0.13f, ink);
        legend(g, L"dB", vuPx, s.cx, oy + H * 0.72f, ink);
    }
    g.ResetClip();

    lay.pivotX = s.cx;
    lay.pivotY = s.cy;
    lay.a0 = s.a0;
    lay.sweep = s.span;
    lay.needleIn = (s.cy - (oy + H)) - H * 0.05f;  // from just below the window's bottom edge
    lay.needleOut = R + majL * 0.9f;
    lay.needleW = std::max(1.0f, H * 0.020f);
    lay.shadowDeg = -1.1f;  // lit from the right here: the shadow falls to the left, as photographed
    setShadow(lay, H);
    lay.ledR = 0.0f;
    lay.face = VuFace::Silver;
}

}  // namespace

void buildVuDialStatic(const VuDialSpec& spec, int w, int h, UINT dpi, std::vector<uint32_t>& px,
                       VuDialLayout& lay) {
    lay = VuDialLayout{};
    px.assign(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)), 0);
    if (w < 4 || h < 4) return;
    paintFaceplate(spec.face, w, h, px);
    const float minLine = std::max(1.0f, static_cast<float>(dpi) / 96.0f * 0.9f);
    if (spec.face == VuFace::Silver) buildSilver(spec, w, h, minLine, px, lay);
    else buildBacklit(spec, w, h, minLine, px, lay);
}

float vuDialPositionOfDb(VuFace face, float db) {
    if (!std::isfinite(db)) return db > 0.0f ? scaleOf(face).f[scaleOf(face).n - 1] : 0.0f;
    return fOfDb(scaleOf(face), db);
}
float vuDialFullScaleDb(VuFace face) { return scaleOf(face).fullDb; }
COLORREF vuDialNeedleColour(VuFace face) { return face == VuFace::Silver ? kSilverNeedle : kBacklitNeedle; }

void drawVuDialNeedle(HDC dc, int ox, int oy, const VuDialLayout& lay, float position, float peakGlow,
                      COLORREF needle) {
    if (lay.card.right <= lay.card.left || lay.card.bottom <= lay.card.top) return;
    // std::clamp passes NaN straight through; a NaN angle would draw nothing sensible.
    const float f = std::isfinite(position) ? std::clamp(position, 0.0f, 1.0f) : 0.0f;

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetClip(Gdiplus::Rect(ox + lay.card.left, oy + lay.card.top, lay.card.right - lay.card.left,
                            lay.card.bottom - lay.card.top),
              Gdiplus::CombineModeIntersect);
    const float px = ox + lay.pivotX, py = oy + lay.pivotY;
    const float ang = lay.a0 + lay.sweep * f;
    // The shadow is the needle ROTATED about the pivot (MiniMeter's lesson from the "double needle"):
    // fused at the root, opening toward the tip — what a pointer standing off the card really throws.
    {
        const float sa = ang + lay.shadowDeg;
        Gdiplus::Pen sh(Gdiplus::Color(static_cast<BYTE>(std::clamp(lay.shadowA, 0.0f, 255.0f)), 0, 0, 0),
                        lay.shadowW);
        sh.SetStartCap(Gdiplus::LineCapRound);
        sh.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&sh, polar(px, py, sa, lay.needleIn), polar(px, py, sa, lay.needleOut));
    }
    Gdiplus::Pen pen(gc(needle), lay.needleW);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, polar(px, py, ang, lay.needleIn), polar(px, py, ang, lay.needleOut));

    // The PEAK lamp, lit: a hot core and a glow spilling onto the card around it.
    if (lay.ledR > 0.0f && peakGlow > 0.02f) {
        const float k = std::clamp(peakGlow, 0.0f, 1.0f);
        const float lx = ox + lay.ledX, ly = oy + lay.ledY, r = lay.ledR;
        auto disc = [&](float rr, BYTE a, BYTE cr, BYTE cg, BYTE cb) {
            Gdiplus::SolidBrush b(Gdiplus::Color(static_cast<BYTE>(a * k), cr, cg, cb));
            g.FillEllipse(&b, lx - rr, ly - rr, rr * 2.0f, rr * 2.0f);
        };
        disc(r * 2.6f, 70, 255, 70, 30);
        disc(r * 1.0f, 255, 255, 60, 36);
        disc(r * 0.5f, 210, 255, 205, 170);
    }
    g.ResetClip();
}

}  // namespace rabbitears
