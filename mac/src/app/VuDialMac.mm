// SPDX-License-Identifier: GPL-3.0-or-later
// See VuDialMac.h. The port of Win32/ui/VuDial.cpp: every constant and every geometric expression is
// that file's; the comments that explain WHY (measured off the reference photos, etc.) are there.
// MRC file with no retained Objective-C objects: CoreGraphics/CoreText objects are CFRelease'd here.
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include "VuDialMac.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "ui/VuLamp.h"  // vuLampIsUnset — "pick black to get the stock bulb back" (shared)

namespace rabbitears::mac {
namespace {

constexpr float kPi = 3.14159265f;

struct Rgb {
    float r, g, b;
};
Rgb rgbOf(COLORREF c) { return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f}; }
Rgb mix(const Rgb& a, const Rgb& b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgb scale(const Rgb& a, float k) { return {a.r * k, a.g * k, a.b * k}; }
uint32_t pack(const Rgb& c) {
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}
float smooth01(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float hashNoise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFF) / 32767.5f - 1.0f;
}

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

// ---- the printed scales (Win32 VuDial.cpp, measured off the reference instruments) -------------
struct ScaleTable {
    const float* db;
    const float* f;
    int          n;
    float        fullDb;
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

void setShadow(VuDialLayout& lay, float H) {
    const float grow = std::sqrt(std::max(1.0f, H / 26.0f));
    lay.shadowW = lay.needleW * (1.0f + 1.2f / grow);
    lay.shadowA = 52.0f / grow;
}

// ---- faceplates (pixel work — verbatim) ------------------------------------------------------
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

// ---- the vector half, in CoreGraphics (y-down: the context is flipped to GDI+'s convention) -----
struct Pt {
    float x, y;
};
Pt polar(float cx, float cy, float deg, float r) {
    const float a = deg * kPi / 180.0f;
    return {cx + r * std::cos(a), cy + r * std::sin(a)};
}
struct Sweep {
    float cx, cy, a0, span;
    float angle(float f) const { return a0 + span * f; }
    Pt at(float f, float r) const { return polar(cx, cy, angle(f), r); }
};
void setFill(CGContextRef g, COLORREF c, float a = 1.0f) {
    CGContextSetRGBFillColor(g, GetRValue(c) / 255.0, GetGValue(c) / 255.0, GetBValue(c) / 255.0, a);
}
void setStroke(CGContextRef g, COLORREF c, float width, float a = 1.0f) {
    CGContextSetRGBStrokeColor(g, GetRValue(c) / 255.0, GetGValue(c) / 255.0, GetBValue(c) / 255.0, a);
    CGContextSetLineWidth(g, width);
}
// An arc as GDI+ DrawArc draws it (start + sweep, clockwise in y-down), as a fine polyline so the
// geometry is exactly the one polar() gives — no reliance on CG's arc-direction convention.
void arcPath(CGContextRef g, float cx, float cy, float r, float startDeg, float sweepDeg) {
    const int n = std::max(16, static_cast<int>(std::fabs(sweepDeg) * 2.0f));
    for (int i = 0; i <= n; ++i) {
        const Pt p = polar(cx, cy, startDeg + sweepDeg * static_cast<float>(i) / n, r);
        if (i == 0) CGContextMoveToPoint(g, p.x, p.y);
        else CGContextAddLineToPoint(g, p.x, p.y);
    }
}
void arcStroke(CGContextRef g, const Sweep& s, float r, float f0, float f1, COLORREF c, float width) {
    setStroke(g, c, width);
    CGContextBeginPath(g);
    arcPath(g, s.cx, s.cy, r, s.angle(f0), s.span * (f1 - f0));
    CGContextStrokePath(g);
}
void tick(CGContextRef g, const Sweep& s, float f, float r0, float r1, COLORREF c, float width) {
    setStroke(g, c, width);
    const Pt a = s.at(f, r0), b = s.at(f, r1);
    CGContextBeginPath(g);
    CGContextMoveToPoint(g, a.x, a.y);
    CGContextAddLineToPoint(g, b.x, b.y);
    CGContextStrokePath(g);
}
void fillEllipse(CGContextRef g, float x, float y, float w, float h, COLORREF c, float a = 1.0f) {
    setFill(g, c, a);
    CGContextFillEllipseInRect(g, CGRectMake(x, y, w, h));
}
// One line of legend text centred on (x, y) — Arial (macOS ships it) or Helvetica, grey-scale
// antialiased, as Win32's legend(): the line box (ascent + descent) centred on y, nudged down 6 %.
void legend(CGContextRef g, const char* utf8, float px, float x, float y, COLORREF c) {
    CTFontRef font = CTFontCreateWithName(CFSTR("ArialMT"), px, nullptr);
    if (!font) font = CTFontCreateWithName(CFSTR("Helvetica"), px, nullptr);
    if (!font) return;
    CFStringRef str = CFStringCreateWithCString(nullptr, utf8, kCFStringEncodingUTF8);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat comps[4] = {GetRValue(c) / 255.0, GetGValue(c) / 255.0, GetBValue(c) / 255.0, 1.0};
    CGColorRef col = CGColorCreate(cs, comps);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void* vals[] = {font, col};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = str ? CFAttributedStringCreate(nullptr, str, attrs) : nullptr;
    if (as) {
        CTLineRef line = CTLineCreateWithAttributedString(as);
        CGFloat ascent = 0, descent = 0, leading = 0;
        const double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
        const float cy = y + px * 0.06f;
        const float baseline = cy + static_cast<float>(ascent - descent) * 0.5f;
        CGContextSaveGState(g);
        CGContextSetShouldSmoothFonts(g, false);       // grey-scale, never sub-pixel colour fringes
        CGContextSetTextMatrix(g, CGAffineTransformMakeScale(1, -1));  // upright in the y-down context
        CGContextSetTextPosition(g, x - static_cast<float>(width) * 0.5f, baseline);
        CTLineDraw(line, g);
        CGContextRestoreGState(g);
        CFRelease(line);
        CFRelease(as);
    }
    CFRelease(attrs);
    CGColorRelease(col);
    CGColorSpaceRelease(cs);
    if (str) CFRelease(str);
    CFRelease(font);
}

// A y-down CG context over `px` (w*h, 0x00RRGGBB): little-endian 32-bit, the first byte skipped.
CGContextRef contextOver(std::vector<uint32_t>& px, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef g = CGBitmapContextCreate(px.data(), static_cast<size_t>(w), static_cast<size_t>(h), 8,
                                           static_cast<size_t>(w) * 4, cs,
                                           kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!g) return nullptr;
    CGContextTranslateCTM(g, 0, h);
    CGContextScaleCTM(g, 1, -1);
    CGContextSetShouldAntialias(g, true);
    CGContextSetLineCap(g, kCGLineCapButt);
    return g;
}

// ---- the Backlit face -----------------------------------------------------------------------
void buildBacklit(const VuDialSpec& spec, int w, int h, float minLine, std::vector<uint32_t>& px,
                  VuDialLayout& lay) {
    const VuDialWindow win = vuDialWindow(VuFace::Backlit, w, h);
    const int my = win.my, openH = win.openH, openW = win.openW;
    if (openH < 6 || openW < 8) return;
    const int ox = (w - openW) / 2, oy = my;
    const float H = static_cast<float>(openH), W = static_cast<float>(openW);
    lay.card = RECT{ox, oy, ox + openW, oy + openH};

    const Lamp lamp = lampOf(spec.lamp);
    Rgb top, mid, hot, core;
    if (lamp.stock) {
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
    const float rx = H * 1.05f, ry = H * 0.72f;
    const float blobRx = H * 0.52f, blobRy = H * 0.36f;
    for (int y = 0; y < openH; ++y) {
        const float yy = static_cast<float>(oy + y) + 0.5f;
        const float dy = (yy - lampY) / ry;
        const float lip = y < H * 0.12f ? 0.60f + 0.40f * std::pow(y / (H * 0.12f), 0.7f) : 1.0f;
        for (int x = 0; x < openW; ++x) {
            const float xx = static_cast<float>(ox + x) + 0.5f;
            const float dx = (xx - lampX) / rx;
            const float g = 1.0f / std::pow(1.0f + 1.4f * (dx * dx + dy * dy), 1.6f);
            const float ex = (xx - lampX) / blobRx, ey = (yy - lampY) / blobRy;
            const float b = 1.0f / std::pow(1.0f + 1.6f * (ex * ex + ey * ey), 1.8f);
            Rgb c = mix(top, mid, smooth01(0.06f, 0.55f, g));
            c = mix(c, hot, smooth01(0.14f, 0.62f, b));
            c = mix(c, core, smooth01(0.70f, 1.0f, b) * 0.8f);
            const float edge = std::min(static_cast<float>(x), W - 1.0f - x) / (H * 0.06f);
            const float side = edge < 1.0f ? 0.86f + 0.14f * edge : 1.0f;
            c = scale(c, lip * side);
            if (x == 0 || y == 0 || x == openW - 1 || y == openH - 1) c = scale(c, 0.78f);
            px[static_cast<size_t>(oy + y) * w + (ox + x)] = pack(c);
        }
    }
    paintRim(px, w, h, ox, oy, openW, openH, {0.035f, 0.035f, 0.04f}, {0.26f, 0.26f, 0.27f}, true);

    CGContextRef g = contextOver(px, w, h);
    if (!g) return;
    CGContextSaveGState(g);
    CGContextClipToRect(g, CGRectMake(ox, oy, openW, openH));

    const COLORREF ink = RGB(34, 30, 26), red = spec.redZone;
    const float R = H * 1.20f;
    const float half = std::min(34.0f, std::asin(std::min(0.95f, 0.38f * W / R)) * 180.0f / kPi);
    const Sweep s{ox + W * 0.5f, oy + H * 1.63f, -90.0f - half, 2.0f * half};
    const ScaleTable& T = kBacklitScale;
    const float f0 = fOfDb(T, 0.0f);
    const float arcW = std::max(minLine, H * 0.024f);
    arcStroke(g, s, R, 0.0f, f0, ink, arcW);
    {  // the red zone: a wedge that thickens toward the top of the scale
        constexpr int n = 24;
        Pt pts[2 * n + 2];
        const float fEnd = 0.95f;
        for (int i = 0; i <= n; ++i) {
            const float t = static_cast<float>(i) / n;
            const float f = f0 + (fEnd - f0) * t;
            pts[i] = s.at(f, R + arcW * 0.5f + (H * 0.055f - arcW * 0.5f) * t);
            pts[2 * n + 1 - i] = s.at(f, R - arcW * 0.5f);
        }
        setFill(g, red);
        CGContextBeginPath(g);
        CGContextMoveToPoint(g, pts[0].x, pts[0].y);
        for (int i = 1; i < 2 * n + 2; ++i) CGContextAddLineToPoint(g, pts[i].x, pts[i].y);
        CGContextClosePath(g);
        CGContextEOFillPath(g);  // GDI+ FillPolygon's default (alternate) fill mode
    }
    struct Mark {
        float       db;
        const char* label;
    };
    static const Mark kMarks[] = {{-20, "20"}, {-10, "10"}, {-7, "7"},   {-5, "5"},  {-3, "3"},
                                  {-2, nullptr}, {-1, nullptr}, {0, "0"}, {1, nullptr}, {2, nullptr},
                                  {3, "3"},     {4, nullptr},  {5, "5"}};
    const float majL = H * 0.090f, minL = H * 0.052f;
    const float majW = std::max(minLine, H * 0.018f), minW = std::max(minLine * 0.8f, H * 0.013f);
    const float numPx = H * 0.12f;
    const bool numerals = numPx >= 5.0f;
    for (const Mark& m : kMarks) {
        const float f = fOfDb(T, m.db);
        const COLORREF c = m.db < 0.0f ? ink : red;
        tick(g, s, f, R, R + (m.label ? majL : minL), c, m.label ? majW : minW);
        if (m.label && numerals) {
            const Pt p = s.at(f, R + majL + numPx * 0.72f);
            legend(g, m.label, numPx, p.x, p.y, c);
        }
    }
    if (numerals) {
        const Pt lo = s.at(-0.03f, R + majL * 0.6f), hi = s.at(0.985f, R + majL * 0.6f);
        legend(g, "\xE2\x80\x93", numPx * 1.15f, lo.x, lo.y, ink);  // – (en dash)
        legend(g, "+", numPx * 1.15f, hi.x, hi.y, red);
    }
    const float vuPx = H * 0.17f;
    if (vuPx >= 7.0f) legend(g, "VU", vuPx, s.cx, oy + H * 0.70f, ink);

    lay.ledR = std::max(1.2f, H * 0.042f);
    lay.ledX = ox + W - H * 0.12f;
    lay.ledY = oy + H * 0.12f;
    {
        const float r = lay.ledR;
        fillEllipse(g, lay.ledX - r, lay.ledY - r, r * 2.0f, r * 2.0f, RGB(74, 14, 10));
        if (r >= 2.5f) fillEllipse(g, lay.ledX - r * 0.55f, lay.ledY - r * 0.6f, r * 0.55f, r * 0.45f,
                                   RGB(230, 190, 180), 150.0f / 255.0f);
        const float peakPx = H * 0.085f;
        if (peakPx >= 5.0f) {
            const float tw = peakPx * 2.8f;
            legend(g, "PEAK", peakPx, lay.ledX - r * 2.2f - H * 0.02f - tw * 0.5f, lay.ledY, ink);
        }
    }
    {  // the lamp's shroud: a dark brass half-disc on the bottom edge
        const float r = H * 0.085f, bx = s.cx, by = oy + H;
        fillEllipse(g, bx - r, by - r, r * 2.0f, r * 2.0f, RGB(70, 54, 24));
        setStroke(g, RGB(150, 118, 52), std::max(minLine * 0.8f, H * 0.012f), 200.0f / 255.0f);
        CGContextBeginPath(g);
        arcPath(g, bx, by, r, 180.0f, 180.0f);
        CGContextStrokePath(g);
    }
    CGContextRestoreGState(g);
    CGContextRelease(g);

    lay.pivotX = s.cx;
    lay.pivotY = s.cy;
    lay.a0 = s.a0;
    lay.sweep = s.span;
    lay.needleIn = (s.cy - (oy + H)) - H * 0.03f;
    lay.needleOut = R + majL * 0.85f;
    lay.needleW = std::max(0.9f, H * 0.012f);
    lay.shadowDeg = 1.1f;
    setShadow(lay, H);
    lay.face = VuFace::Backlit;
}

// ---- the Silver face ------------------------------------------------------------------------
void buildSilver(const VuDialSpec& spec, int w, int h, float minLine, std::vector<uint32_t>& px,
                 VuDialLayout& lay) {
    const VuDialWindow win = vuDialWindow(VuFace::Silver, w, h);
    const int my = win.my, openH = win.openH, openW = win.openW;
    if (openH < 6 || openW < 8) return;
    const int ox = (w - openW) / 2, oy = my;
    const float H = static_cast<float>(openH);
    const int wallR = std::max(1, static_cast<int>(std::lround(H * 0.20f)));
    const int wallL = std::max(1, static_cast<int>(std::lround(H * 0.035f)));
    const int cx0 = ox + wallL, cx1 = ox + openW - wallR;
    const float CW = static_cast<float>(cx1 - cx0);
    lay.card = RECT{cx0, oy, cx1, oy + openH};

    const Lamp lamp = lampOf(spec.lamp);
    const Rgb ivory = lamp.stock ? rgbOf(RGB(222, 208, 174)) : scale(mix(lamp.hue, {1, 1, 1}, 0.74f), 0.90f);
    for (int y = 0; y < openH; ++y) {
        const float fy = static_cast<float>(y) + 0.5f;
        const float lip = fy < H * 0.17f ? 0.46f + 0.54f * std::pow(fy / (H * 0.17f), 0.8f) : 1.0f;
        const float fall = 0.90f + 0.10f * smooth01(0.0f, H * 0.6f, fy);
        for (int x = ox; x < ox + openW; ++x) {
            Rgb c;
            if (x < cx0) {
                c = scale(ivory, 0.35f * lip);
            } else if (x < cx1) {
                const float u = (x - cx0 + 0.5f) / CW;
                c = scale(ivory, lip * fall * (1.0f - 0.10f * u * u));
                if (x == cx0 || x == cx1 - 1) c = scale(c, 0.82f);
            } else {
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

    CGContextRef g = contextOver(px, w, h);
    if (!g) return;
    CGContextSaveGState(g);
    CGContextClipToRect(g, CGRectMake(cx0, oy, cx1 - cx0, openH));

    const COLORREF ink = RGB(44, 40, 36), blue = RGB(34, 46, 134), red = spec.redZone;
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
        float       db;
        const char* label;
        bool        major;
    };
    static const Mark kBlue[] = {{-20, "\xE2\x88\x92" "20", true}, {-10, "10", true},  {-7, "7", true},
                                 {-5, "5", true},   {-4, nullptr, false}, {-3, "3", true},
                                 {-2, nullptr, false}, {-1, "1", true}};
    static const Mark kRed[] = {{0, "0", true}, {1, "1", true}, {2, "2", true},   {3, nullptr, true},
                                {4, "4", true}, {5, nullptr, true}, {6, "+6", true}};
    for (const Mark& m : kBlue) {
        const float f = fOfDb(T, m.db);
        tick(g, s, f, R, R + (m.major ? majL : minL), blue, m.major ? majW : minW);
        if (m.label && numerals) {
            Pt p = s.at(f, R + majL + numPx * 0.70f);
            if (m.db == -20.0f) p.x -= numPx * 0.85f;
            if (m.db == -10.0f) p.x -= numPx * 0.30f;
            legend(g, m.label, numPx, p.x, p.y, blue);
        }
    }
    for (const Mark& m : kRed) {
        const float f = fOfDb(T, m.db);
        tick(g, s, f, R, R - majL, red, majW);
        if (m.label && numerals) {
            const Pt p = s.at(f, R + arcW + numPx * 0.60f);
            legend(g, m.label, numPx, p.x, p.y, red);
        }
    }
    const float pctPx = H * 0.078f;
    if (pctPx >= 7.0f) {
        static const char* kPct[] = {"0", "20", "40", "60", "80", "100"};
        for (int i = 0; i <= 5; ++i) {
            const float f = i == 0 ? 0.0f : fOfDb(T, 20.0f * std::log10(i / 5.0f));
            tick(g, s, f, R - arcW * 0.5f, R - H * 0.065f, blue, minW);
            const Pt p = s.at(f, R - H * 0.065f - pctPx * 0.78f);
            legend(g, kPct[i], pctPx, p.x, p.y, blue);
        }
    }
    const float vuPx = H * 0.11f;
    if (vuPx >= 5.0f) {
        legend(g, "VU", vuPx, cx0 + H * 0.15f, oy + H * 0.13f, ink);
        legend(g, "VU", vuPx, cx1 - H * 0.17f, oy + H * 0.13f, ink);
        legend(g, "dB", vuPx, s.cx, oy + H * 0.72f, ink);
    }
    CGContextRestoreGState(g);
    CGContextRelease(g);

    lay.pivotX = s.cx;
    lay.pivotY = s.cy;
    lay.a0 = s.a0;
    lay.sweep = s.span;
    lay.needleIn = (s.cy - (oy + H)) - H * 0.05f;
    lay.needleOut = R + majL * 0.9f;
    lay.needleW = std::max(1.0f, H * 0.020f);
    lay.shadowDeg = -1.1f;
    setShadow(lay, H);
    lay.ledR = 0.0f;
    lay.face = VuFace::Silver;
}

}  // namespace

void buildVuDialStatic(const VuDialSpec& spec, int w, int h, float scaleFactor, std::vector<uint32_t>& px,
                       VuDialLayout& lay) {
    lay = VuDialLayout{};
    px.assign(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)), 0);
    if (w < 4 || h < 4) return;
    paintFaceplate(spec.face, w, h, px);
    const float minLine = std::max(1.0f, scaleFactor * 0.9f);  // Win32: dpi / 96 * 0.9
    if (spec.face == VuFace::Silver) buildSilver(spec, w, h, minLine, px, lay);
    else buildBacklit(spec, w, h, minLine, px, lay);
}

float vuDialPositionOfDb(VuFace face, float db) {
    if (!std::isfinite(db)) return db > 0.0f ? scaleOf(face).f[scaleOf(face).n - 1] : 0.0f;
    return fOfDb(scaleOf(face), db);
}
float vuDialFullScaleDb(VuFace face) { return scaleOf(face).fullDb; }
COLORREF vuDialNeedleColour(VuFace face) { return face == VuFace::Silver ? kSilverNeedle : kBacklitNeedle; }

void drawVuDialNeedle(CGContextRef g, int ox, int oy, const VuDialLayout& lay, float position, float peakGlow,
                      COLORREF needle) {
    if (!g || lay.card.right <= lay.card.left || lay.card.bottom <= lay.card.top) return;
    const float f = std::isfinite(position) ? std::clamp(position, 0.0f, 1.0f) : 0.0f;
    CGContextSaveGState(g);
    CGContextClipToRect(g, CGRectMake(ox + lay.card.left, oy + lay.card.top, lay.card.right - lay.card.left,
                                      lay.card.bottom - lay.card.top));
    CGContextSetLineCap(g, kCGLineCapRound);
    const float px = ox + lay.pivotX, py = oy + lay.pivotY;
    const float ang = lay.a0 + lay.sweep * f;
    auto line = [&](float deg, COLORREF c, float width, float alpha) {
        const Pt a = polar(px, py, deg, lay.needleIn), b = polar(px, py, deg, lay.needleOut);
        setStroke(g, c, width, alpha);
        CGContextBeginPath(g);
        CGContextMoveToPoint(g, a.x, a.y);
        CGContextAddLineToPoint(g, b.x, b.y);
        CGContextStrokePath(g);
    };
    // The shadow is the needle ROTATED about the pivot: fused at the root, opening toward the tip.
    line(ang + lay.shadowDeg, RGB(0, 0, 0), lay.shadowW, std::clamp(lay.shadowA, 0.0f, 255.0f) / 255.0f);
    line(ang, needle, lay.needleW, 1.0f);
    if (lay.ledR > 0.0f && peakGlow > 0.02f) {  // the PEAK lamp, lit: a hot core and a glow round it
        const float k = std::clamp(peakGlow, 0.0f, 1.0f);
        const float lx = ox + lay.ledX, ly = oy + lay.ledY, r = lay.ledR;
        auto disc = [&](float rr, float a, BYTE cr, BYTE cg, BYTE cb) {
            fillEllipse(g, lx - rr, ly - rr, rr * 2.0f, rr * 2.0f, RGB(cr, cg, cb), a * k / 255.0f);
        };
        disc(r * 2.6f, 70, 255, 70, 30);
        disc(r * 1.0f, 255, 255, 60, 36);
        disc(r * 0.5f, 210, 255, 205, 170);
    }
    CGContextRestoreGState(g);
}

}  // namespace rabbitears::mac
