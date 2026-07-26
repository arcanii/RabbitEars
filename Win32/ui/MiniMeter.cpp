// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/MiniMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include <objidl.h>  // IStream — required by gdiplus.h below
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the std
// versions into the Gdiplus namespace before including it (used for the AA scope trace).
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <atomic>

#include "ui/GlassMask.h"  // shared "glass cover" mask math (common/)
#include "ui/VuLamp.h"     // shared VU dial illumination math (common/)
#include "ui/Theme.h"

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReMiniMeter";
constexpr int kMaxBands = 32;
constexpr int kHist = 64;      // bitrate history ring length
constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 33;  // ~30fps animation

// Meter colours come from each meter's MeterPalette (customizable via Settings →
// Meters…); the defaults reproduce the classic look. Cells are drawn with the stock
// DC brush (SetDCBrushColor), never themeBrush — its 12-slot cache would leak once
// the per-cell ramp pushes this many distinct colours through it.
int dpx(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

// The chrome band a meter reserves around its dial: a 1px themed FrameRect plus the rest of the
// pad. Used for BOTH the content inset AND the glass mask's frame width, so the two can never
// disagree — the bezel is painted exactly here, which is why it costs zero dial pixels at any DPI
// and at any meter size (the tray meters are dp(30) tall; the Settings previews are dp(86)).
int meterChromePx(UINT dpi) { return dpx(2, dpi); }

COLORREF lerpCol(COLORREF a, COLORREF b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](int x, int y) { return x + static_cast<int>((y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
              mix(GetBValue(a), GetBValue(b)));
}

// The lit-ramp colour for a 0..1 fraction (same thresholds as the classic look).
COLORREF rampColor(const MeterPalette& p, float frac) {
    return frac < 0.60f ? p.low : frac < 0.85f ? p.mid : p.high;
}

// One lit cell captured during the base paint pass so the Tube look can add a soft
// GDI+ phosphor halo over it in a second pass (see drawTubeGlow). `c` is the cell's
// lit ramp colour.
struct GlowCell {
    RECT     r;
    COLORREF c;
};

struct MiniMeterState {
    MeterKind    kind = MeterKind::Spectrum;
    MeterStyle   style = MeterStyle::Led;
    MeterPalette palette = defaultMeterPalette(MeterKind::Spectrum);
    MeterTuning  tuning = defaultMeterTuning();
    UINT      dpi = 96;
    bool      timerOn = false;

    // Spectrum (fed off the audio thread through `pending`, under `mtx`).
    std::mutex mtx;
    float      pending[kMaxBands] = {};
    int        pendingN = 0;
    bool       hasPending = false;
    float      level[kMaxBands] = {};  // UI-thread only (timer + paint)
    float      peak[kMaxBands] = {};
    int        bands = 0;

    // Signal
    float sigTarget = 0.0f, sigLevel = 0.0f, sigTrouble = 0.0f;

    // Bitrate history ring (newest at head-1)
    float hist[kHist] = {};
    int   histHead = 0, histCount = 0;
    float histMax = 1.0f;

    // Frames
    int   fps = 0;
    float flare = 0.0f;  // red flash on dropped frames, decays

    // VU needle: its own damped position, deliberately NOT the same value the cell looks draw.
    // A real VU movement is a mass on a spring — the standard is ~300ms to settle, symmetric on
    // rise and fall — and that lag IS the instrument's character. Sharing the LED meters'
    // attack-fast/decay-slow envelope would make it twitch like a peak meter, which is the one
    // thing a VU is not. Held per-meter so switching looks doesn't fling the needle.
    float vuNeedle = 0.0f;

    // Cached 32bpp top-down back-buffer. Previously onPaint created AND destroyed a
    // CreateCompatibleBitmap every paint — with 4 tray meters at ~30fps that is ~240 GDI object
    // operations per second for a surface whose size almost never changes. Caching it is a win on
    // its own, and it is what makes the glass mask cacheable (we need addressable pixels).
    HDC       backDC = nullptr;
    HBITMAP   backBmp = nullptr;
    void*     backBits = nullptr;   // BGRA, row-major top-down (biHeight negative)
    HGDIOBJ   backOld = nullptr;
    int       backW = 0, backH = 0;
    // Glass overlay LUTs, rebuilt with the back-buffer whenever the size or strength changes.
    std::vector<uint8_t> glassAdd, glassMul;
    float                glassBuilt = -1.0f;  // strength the LUTs were built for (-1 = never)
    int                  glassChrome = -1;    // chrome width they were built for (DPI can change
                                              // without a resize — miniMeterSetDpi only sets dpi)
    // Vu: the bulb's field over the dial, cached exactly like the glass LUTs above. Keyed on the
    // FACE size only, not the client size and not the palette — the field is geometry, the lamp's
    // colour is only the two ends of the lerp, so recolouring costs nothing.
    std::vector<uint8_t> lampMask;
    int                  lampW = 0, lampH = 0;  // size lampMask was built for (0 = never)
};

// Global "glass cover" strength for every meter (0 = off). A single app-wide value rather than a
// per-meter MeterTuning knob, deliberately: the buffer meter has no MeterConfig at all, the Meters
// dialog's knob band is already full at 4 sliders, and a 6th MeterTuning field would break mac's
// exact-arity parser. Read by MiniMeter; BufferMeter does not consume it yet (see BACKLOG).
std::atomic<float>& meterGlassRef() {
    static std::atomic<float> g{0.0f};
    return g;
}

MiniMeterState* stateOf(HWND h) {
    return reinterpret_cast<MiniMeterState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

void fillCell(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// Draw one meter cell in the chosen look. `lit` is the cell's colour when on; the
// styler decides the off/dim appearance and any decoration. (Scope is a trace, not
// cells — handled by drawScope, so it falls back to LED cells here.)
void drawCell(HDC dc, const RECT& r, COLORREF lit, bool on, const MeterPalette& pal,
              MeterStyle style, std::vector<GlowCell>* glow) {
    switch (style) {
        case MeterStyle::Lcd:
            // Backlit LCD: off segments are a faint ghost of the lit colour, not black.
            fillCell(dc, r, on ? lit : lerpCol(pal.off, lit, 0.16f));
            break;
        case MeterStyle::Tube:
            // Warm vacuum tube: lay down a muted base cell now; the soft phosphor halo
            // is added in a second GDI+ pass (lit cells collected in `glow`) so the
            // bloom can bleed across cell borders into a continuous column of light —
            // something a hard GDI square core can't. See drawTubeGlow.
            fillCell(dc, r, on ? lerpCol(pal.off, lit, 0.55f) : pal.off);
            if (on && glow) glow->push_back({r, lit});
            break;
        case MeterStyle::Led:
        case MeterStyle::Scope:
        default:
            fillCell(dc, r, on ? lit : pal.off);
            break;
    }
}

// Oscilloscope look: draw the kind's data as a continuous trace on a faint graticule
// instead of cells. Spectrum → band curve; Bitrate → its scroll history; Signal /
// Frames → a level line (they keep no history).
void drawScope(HDC dc, const RECT& in, MiniMeterState* st) {
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    if (R <= L || B <= T) return;

    float vals[kHist];
    int n = 0;
    switch (st->kind) {
        case MeterKind::Spectrum:
            n = std::clamp(st->bands, 1, kMaxBands);
            for (int i = 0; i < n; ++i) vals[i] = std::clamp(st->level[i], 0.0f, 1.0f);
            break;
        case MeterKind::Bitrate: {
            const float denom = std::max(st->histMax, 1.0f);
            n = std::min(kHist, st->histCount);
            for (int k = 0; k < n; ++k) {  // oldest → newest, left → right
                const int idx = (st->histHead - n + k + kHist) % kHist;
                vals[k] = std::clamp(st->hist[idx] / denom, 0.0f, 1.0f);
            }
            break;
        }
        case MeterKind::Signal:
            n = 12;
            for (int i = 0; i < n; ++i) vals[i] = std::clamp(st->sigLevel, 0.0f, 1.0f);
            break;
        case MeterKind::Frames:
            n = 12;
            for (int i = 0; i < n; ++i) vals[i] = std::clamp(st->fps / 60.0f, 0.0f, 1.0f);
            break;
    }
    if (n < 2) {
        vals[1] = (n == 1) ? vals[0] : 0.0f;
        if (n == 0) vals[0] = 0.0f;
        n = 2;
    }

    const float gain = st->tuning.sensitivity * 2.0f;  // sensitivity knob (0.5 = unity)
    for (int i = 0; i < n; ++i) vals[i] = std::clamp(vals[i] * gain, 0.0f, 1.0f);

    auto gcol = [](COLORREF c, BYTE a) {
        return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
    };
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // Faint graticule.
    const Gdiplus::REAL Lf = static_cast<Gdiplus::REAL>(L), Rf = static_cast<Gdiplus::REAL>(R);
    const Gdiplus::REAL Tf = static_cast<Gdiplus::REAL>(T), Bf = static_cast<Gdiplus::REAL>(B);
    Gdiplus::Pen gridPen(gcol(st->palette.off, 150), 1.0f);
    for (int gi = 1; gi < 4; ++gi) {
        const Gdiplus::REAL gy = Tf + (Bf - Tf) * gi / 4.0f;
        g.DrawLine(&gridPen, Lf, gy, Rf, gy);
    }

    // The trace, antialiased, with a phosphor bloom: wide low-alpha accent underlays
    // then a crisp bright core.
    Gdiplus::PointF pts[kHist];
    for (int i = 0; i < n; ++i) {
        pts[i].X = Lf + (Rf - Lf) * i / static_cast<Gdiplus::REAL>(n - 1);
        pts[i].Y = Bf - vals[i] * (Bf - Tf - 1.0f);
    }
    const Gdiplus::REAL pw = static_cast<Gdiplus::REAL>(std::max(1, dpx(2, st->dpi)));
    auto stroke = [&](COLORREF c, BYTE a, Gdiplus::REAL w) {
        Gdiplus::Pen p(gcol(c, a), w);
        p.SetLineJoin(Gdiplus::LineJoinRound);
        p.SetStartCap(Gdiplus::LineCapRound);
        p.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLines(&p, pts, n);
    };
    const float gs = st->tuning.glow * 2.0f;  // glow knob scales the phosphor bloom (0.5 = default)
    stroke(st->palette.accent, static_cast<BYTE>(std::clamp(45.0f * gs, 0.0f, 255.0f)), pw * 3.2f);
    stroke(st->palette.accent, static_cast<BYTE>(std::clamp(95.0f * gs, 0.0f, 255.0f)), pw * 1.8f);
    stroke(st->palette.peak, 255, pw);
}

// Second pass for the Tube look: soft phosphor halos over the lit cells (collected in
// the base pass), drawn with GDI+ so they're antialiased and additively bleed across
// cell borders. Layered like the scope bloom — a wide dim halo, an inner glow, then a
// bright core toward the peak colour. (Alphas/radii are aesthetic — tune to taste.)
// The 0..1 quantity this meter is currently showing, as ONE number. The cell looks render a
// spectrum/history as many values; a needle can only point at one, so each kind collapses to its
// headline figure. Shared by the VU needle (and anything else single-valued later).
float scalarLevel(const MiniMeterState* st) {
    float raw = 0.0f;
    switch (st->kind) {
        case MeterKind::Spectrum: {  // loudness ≈ the mean of the bands
            const int n = std::clamp(st->bands, 1, kMaxBands);
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) sum += std::clamp(st->level[i], 0.0f, 1.0f);
            raw = sum / static_cast<float>(n);
            break;
        }
        case MeterKind::Bitrate: {
            const float denom = std::max(st->histMax, 1.0f);
            if (st->histCount <= 0) break;
            const int idx = (st->histHead - 1 + kHist) % kHist;  // newest sample
            raw = st->hist[idx] / denom;
            break;
        }
        case MeterKind::Signal: raw = st->sigLevel; break;
        case MeterKind::Frames: raw = st->fps / 60.0f; break;
    }
    // Sensitivity applies HERE, not only in the cell painters. Every cell look scales by this in
    // its own paint function (paintSpectrum/paintSignal/paintBitrate/paintFrames), so a needle that
    // skipped it made the Sens slider silently STOP WORKING the moment a row was switched to VU —
    // it worked on LED/Tube/LCD/Scope and did nothing on the same row's VU. 0.5 is unity, so this
    // is a bit-exact no-op at the default tuning and only bites for a user who moved the slider.
    const float gain = st->tuning.sensitivity * 2.0f;
    return std::clamp(raw * gain, 0.0f, 1.0f);
}

// Rounds a photometric colour to something GDI+ can paint. `faceHot` is allowed to run past 255
// (see VuLamp.h) and this is where that stops.
Gdiplus::Color vuArgb(const VuLampRgb& c) {
    auto q = [](float v) {
        if (!(v > 0.0f)) return static_cast<BYTE>(0);
        return static_cast<BYTE>(v >= 255.0f ? 255.0f : v + 0.5f);
    };
    return Gdiplus::Color(255, q(c.r), q(c.g), q(c.b));
}

// Classic analog VU gauge: a lamp-lit cream face, a swept scale with ticks, a red zone over the
// last fifth, and a damped needle that casts a real shadow onto the dial. Modelled on a Phase
// Linear 400 — pair it with the glass overlay (Settings ▸ Meters… ▸ Glass) for the full instrument
// look.
//
// Unlike the other looks this one owns its face colours: a VU that isn't cream-on-black stops
// reading as a VU, the same way the Tube look owns its phosphor glow. The palette still drives what
// it sensibly can — `peak` tints the red zone, `accent` the needle, and `bg` IS THE LAMP (see
// MiniMeter.h; its CLR_INVALID default means the stock warm bulb) — so a user keeps meaningful
// control without being able to break the idiom.
//
// The face is NOT painted with GDI+. It is a per-pixel lerp between two colours driven by a cached
// byte field from common/ui/VuLamp.h — the same LUT trick, for the same reason, as the glass mask:
// the lighting is frame-invariant, so it is built once per size and costs one lerp per pixel per
// frame. It also puts the part of this look that is hardest to eyeball inside --selftest.
//
// The pivot sits BELOW the panel so the arc sweeps the full width: these meters are only ~30px
// tall, and a needle rooted inside that would be a stub. That geometry is also what makes the
// needle's shadow work at this size — see the note above it. GDI+ for everything on top of the
// face: a jaggy needle at this size looks broken.
void drawVu(HDC dc, const RECT& in, MiniMeterState* st) {
    const float L = static_cast<float>(in.left), R = static_cast<float>(in.right);
    const float T = static_cast<float>(in.top), B = static_cast<float>(in.bottom);
    const float w = R - L, h = B - T;
    if (w < 8.0f || h < 8.0f) return;
    const int fw = in.right - in.left, fh = in.bottom - in.top;

    // ---- The lamp ---------------------------------------------------------------------------
    // CLR_INVALID is 0xFFFFFFFF, so GetRValue/GetGValue/GetBValue would unpack the "follow the
    // theme" sentinel as pure WHITE. Test for it before touching the channels.
    const VuLampRgb lamp = (st->palette.bg == CLR_INVALID)
                               ? vuStockLamp()
                               : vuLampFrom(GetRValue(st->palette.bg), GetGValue(st->palette.bg),
                                            GetBValue(st->palette.bg));
    const VuDial dial = vuDialColours(lamp);

    // ---- The face ---------------------------------------------------------------------------
    // The bulb's field is a pure function of the dial's SIZE, so it is cached and rebuilt only on
    // resize — deliberately not keyed on the lamp colour, because the field is geometry and the
    // colour is only the lerp's two endpoints. A palette change therefore costs nothing.
    if (st->lampW != fw || st->lampH != fh) {
        buildVuLampMask(fw, fh, st->lampMask);
        st->lampW = fw;
        st->lampH = fh;
    }
    const bool direct = st->backBits && !st->lampMask.empty() && in.left >= 0 && in.top >= 0 &&
                        in.right <= st->backW && in.bottom <= st->backH;
    if (direct) {
        // onPaint's fillCell()/FrameRect() are ordinary GDI and are BATCHED. They have to land
        // before we write these pixels ourselves, or the batch flushes afterwards and paints over
        // the finished dial. Same hazard, opposite direction, as the GdiFlush() onPaint already
        // does before applyGlass() reads the bits back.
        GdiFlush();
        // Clamp the ENDPOINTS and lerp toward the clamped value, rather than lerping in float and
        // clamping the result: the hot end of a warm lamp overshoots 255 in red, and clamping last
        // would put a hard contour across the dial where red pins. Clamping first turns that same
        // overshoot into a smooth roll into the highlight.
        int lo[3], span3[3];
        const float dimC[3] = {dial.faceDim.b, dial.faceDim.g, dial.faceDim.r};  // DIB order: BGRA
        const float hotC[3] = {dial.faceHot.b, dial.faceHot.g, dial.faceHot.r};
        for (int c = 0; c < 3; ++c) {
            lo[c] = static_cast<int>(std::clamp(dimC[c], 0.0f, 255.0f) + 0.5f);
            span3[c] = static_cast<int>(std::clamp(hotC[c], 0.0f, 255.0f) + 0.5f) - lo[c];
        }
        auto* px = static_cast<uint8_t*>(st->backBits);
        for (int y = 0; y < fh; ++y) {
            const uint8_t* mrow = st->lampMask.data() + static_cast<size_t>(y) * fw;
            uint8_t* prow = px + (static_cast<size_t>(in.top + y) * st->backW + in.left) * 4;
            for (int x = 0; x < fw; ++x) {
                const int mm = mrow[x];
                // Alpha (byte 3) is left exactly as GDI wrote it: the DIB is BI_RGB, nothing reads
                // that byte, and writing it risks disagreeing with what GDI+ expects underneath.
                for (int c = 0; c < 3; ++c)
                    prow[x * 4 + c] = static_cast<uint8_t>(lo[c] + (span3[c] * mm + 127) / 255);
            }
        }
    }

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (!direct) {
        // Belt and braces — ensureBack() guarantees backBits, so this cannot normally happen. A
        // flat dial at the dim end is a degraded VU rather than a hole in the tray.
        Gdiplus::SolidBrush flat(vuArgb(dial.faceDim));
        g.FillRectangle(&flat, Gdiplus::RectF(L, T, w, h));
    }
    // The needle is rooted 0.95h BELOW the dial, so without a clip it paints out across the bezel
    // gutter and over the theme's own border row — which it did. Now that the dial has a lit rim of
    // its own, a pointer running over the frame breaks the "instrument behind a bezel" read that
    // the rim and the glass overlay are both working to build; clipped, the needle emerges from
    // under the bezel instead, which is what the real thing does. The clip lives on this stack-local
    // Graphics, NOT on the cached backDC — `dc` here is st->backDC, which never carries
    // BeginPaint's update region — so it cannot leak into the next paint whatever happens.
    g.SetClip(Gdiplus::RectF(L, T, w, h), Gdiplus::CombineModeIntersect);

    // Geometry: pivot below the bottom edge, radius chosen so the arc clears the top with margin.
    const float cx = L + w * 0.5f, cy = B + h * 1.05f;
    const float rad = (cy - T) - h * 0.14f;
    const float span = 52.0f;               // total sweep, degrees
    const float a0 = -90.0f - span * 0.5f;  // 0 == full left
    auto ptAt = [&](float frac, float rr) {
        const float ang = (a0 + span * std::clamp(frac, 0.0f, 1.0f)) * 3.14159265f / 180.0f;
        return Gdiplus::PointF(cx + rr * std::cos(ang), cy + rr * std::sin(ang));
    };

    // Scale arc + ticks. Seven majors; the last fifth is the red zone, as on the real thing. The
    // ink is no longer the hard-coded brown ARGB(74,58,34) — it is the same pigment under the same
    // lamp as the face, which is what keeps the markings legible when the lamp is not warm: a fixed
    // brown on a blue dial is mud, whereas this comes out dark navy. Because ink and ground share a
    // lamp their contrast is a fixed RATIO (4.17:1, set by kInkFrac in VuLamp.cpp) rather than a
    // fixed difference — and since the glass overlay is a pure multiply on the dial, that ratio
    // survives the glass at any strength too.
    const Gdiplus::Color ink = vuArgb(dial.ink);
    Gdiplus::Pen arcPen(ink, std::max(1.0f, h * 0.035f));
    // `peak` stays raw: the red zone is a translucent filter laid over the dial, a real hardware
    // pattern, so it reads at its own colour whatever the lamp behind it is doing.
    Gdiplus::Pen redPen(Gdiplus::Color(255, GetRValue(st->palette.peak), GetGValue(st->palette.peak),
                                       GetBValue(st->palette.peak)),
                        std::max(1.5f, h * 0.055f));
    Gdiplus::RectF arcBox(cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
    g.DrawArc(&arcPen, arcBox, a0, span * 0.80f);
    g.DrawArc(&redPen, arcBox, a0 + span * 0.80f, span * 0.20f);

    for (int i = 0; i <= 6; ++i) {
        const float f = static_cast<float>(i) / 6.0f;
        const float inner = rad - h * (i % 3 == 0 ? 0.20f : 0.12f);
        const Gdiplus::PointF p1 = ptAt(f, rad), p2 = ptAt(f, inner);
        Gdiplus::Pen tick(ink, std::max(1.0f, h * (i % 3 == 0 ? 0.045f : 0.03f)));
        g.DrawLine(&tick, p2, p1);
    }

    // ---- The needle, and the shadow it throws ------------------------------------------------
    const float v = std::clamp(st->vuNeedle, 0.0f, 1.0f);
    const Gdiplus::PointF tip = ptAt(v, rad - h * 0.06f);
    const Gdiplus::PointF root(cx, cy - h * 0.10f);

    // The shadow is the needle ROTATED ABOUT THE PIVOT, not displaced by a fixed (dx, dy), and that
    // is the whole reason it reads as a shadow at 26px instead of as a second needle.
    //
    // A bulb at the bottom is the awkward case: it sits nearly on the needle's own axis, so a
    // literal point-source projection throws the shadow radially — straight along the needle, where
    // it hides — and worst of all near mid-scale, which is where the needle lives. What rescues it
    // is how a pointer is actually built: it is cranked, standing further off the card at the tip
    // than at the boss. With the light effectively parallel (the bulb is ~1.8h away while the
    // pointer floats a fraction of a pixel above the card) a gap that grows along the needle makes
    // the displacement grow along it too, and to first order that IS a small rotation about the
    // pivot. Because the pivot sits 1.05h BELOW the panel, the visible span of the needle is all
    // far from it, so the wedge opens where it is wanted: at h=26 the separation runs 1.2px where
    // the needle emerges from the bezel to 2.2px at the tip. Fused at the root, plainly separate at
    // the tip. A constant offset gives constant separation, and constant separation is exactly what
    // looks like two needles.
    //
    // 2.6 degrees, CLOCKWISE — toward higher readings — because the bulb is left of centre, so the
    // light crosses the needle from the left at every deflection and the shadow always falls to the
    // right. That is the handedness the old fixed (+0.8,+0.8) had, so this reads as "better lit"
    // rather than as "something moved". And being an ANGLE it is size- and DPI-invariant for free:
    // the shadow holds the same proportion to the needle in the tray and in the settings preview,
    // which is not true of anything measured in pixels.
    const float shRad = 2.6f * 3.14159265f / 180.0f;
    const float sc = std::cos(shRad), ss = std::sin(shRad);
    auto spin = [&](const Gdiplus::PointF& p) {  // about the pivot; +y is down, so this is clockwise
        const float dx = p.X - cx, dy = p.Y - cy;
        return Gdiplus::PointF(cx + dx * sc - dy * ss, cy + dx * ss + dy * sc);
    };
    const Gdiplus::PointF sRoot = spin(root), sTip = spin(tip);
    // Two passes, because a bulb behind a bezel lip is an AREA source and throws nothing crisp: a
    // wide faint penumbra with a narrower, darker umbra inside it. BOTH sit on the shadow line, so
    // the pair stays directional — a soft halo drawn on the needle itself would only fatten it,
    // which is the opposite of the hairline that was asked for one commit ago. Compounded they take
    // 43% out of the face, i.e. a shadow near luma 120 on the ~210 the dial carries where the
    // needle spends its time, against the old single alpha-60 stroke that after antialiasing was a
    // dark fringe ON the needle rather than a cast shadow at all.
    //
    // Pure black on purpose: alpha-blending black is a straight multiply, so the shadowed pixels
    // keep the dial's own hue and simply have less light on them, which is what a shadow is. A
    // tinted shadow would have to be re-derived for every lamp colour and would be wrong for all
    // but one of them.
    //
    // The widths are fractions of h that meet their floors at exactly h=26 — the real tray size —
    // so nothing thins below a pixel on the meters that actually ship.
    Gdiplus::Pen penumbra(Gdiplus::Color(36, 0, 0, 0), std::max(2.0f, h * 0.078f));
    penumbra.SetStartCap(Gdiplus::LineCapRound);
    penumbra.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&penumbra, sRoot, sTip);
    Gdiplus::Pen umbra(Gdiplus::Color(86, 0, 0, 0), std::max(1.15f, h * 0.045f));
    umbra.SetStartCap(Gdiplus::LineCapRound);
    umbra.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&umbra, sRoot, sTip);

    // Thin on purpose (owner call): a real VU needle is a hairline, and a fat one at this size
    // reads as a bar rather than a pointer. Unchanged — only its shadow moved.
    Gdiplus::Pen needle(Gdiplus::Color(255, GetRValue(st->palette.accent),
                                       GetGValue(st->palette.accent),
                                       GetBValue(st->palette.accent)),
                        std::max(0.9f, h * 0.028f));
    needle.SetStartCap(Gdiplus::LineCapRound);
    needle.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&needle, root, tip);

    g.ResetClip();
}

void drawTubeGlow(HDC dc, const std::vector<GlowCell>& cells, const MeterPalette& pal, float glow) {
    if (cells.empty()) return;
    const float gs = glow * 2.0f;  // glow knob: 0.5 (default) -> 1.0 = current intensity
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (const GlowCell& gc : cells) {
        const float cw = static_cast<float>(gc.r.right - gc.r.left);
        const float ch = static_cast<float>(gc.r.bottom - gc.r.top);
        const float cx = (gc.r.left + gc.r.right) * 0.5f;
        const float cy = (gc.r.top + gc.r.bottom) * 0.5f;
        auto ell = [&](float rx, float ry, COLORREF c, float a) {
            const BYTE aa = static_cast<BYTE>(std::clamp(a * gs, 0.0f, 255.0f));
            Gdiplus::SolidBrush b(Gdiplus::Color(aa, GetRValue(c), GetGValue(c), GetBValue(c)));
            g.FillEllipse(&b, cx - rx, cy - ry, rx * 2.0f, ry * 2.0f);
        };
        ell(cw * 0.72f, ch * 1.5f, gc.c, 48.0f);                            // wide soft halo (merges the column)
        ell(cw * 0.50f, ch * 0.85f, gc.c, 120.0f);                          // inner glow
        ell(cw * 0.38f, ch * 0.50f, lerpCol(gc.c, pal.peak, 0.6f), 235.0f); // bright core
    }
}

void paintSpectrum(HDC dc, const RECT& in, MiniMeterState* st, std::vector<GlowCell>* glow) {
    const UINT dpi = st->dpi;
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    const int bands = st->bands;
    if (bands <= 0) return;
    const int cellP = std::max(dpx(3, dpi), 2);
    const int rows = std::max(1, (B - T) / cellP);
    const int colW = std::max(2, (R - L) / bands);
    const int gap = dpx(1, dpi);
    const float gain = st->tuning.sensitivity * 2.0f;  // sensitivity knob (0.5 = unity)
    for (int c = 0; c < bands; ++c) {
        const int x0 = L + c * colW;
        const int x1 = x0 + colW - gap;
        const int litN =
            static_cast<int>(std::lround(std::clamp(st->level[c] * gain, 0.0f, 1.0f) * rows));
        const int peakRow =
            static_cast<int>(std::lround(std::clamp(st->peak[c] * gain, 0.0f, 1.0f) * rows));
        for (int r = 0; r < rows; ++r) {
            const int yb = B - r * cellP;
            RECT cell{x0, yb - cellP + gap, x1, yb};
            const float frac = static_cast<float>(r) / rows;
            bool on = (r < litN);
            COLORREF lit = rampColor(st->palette, frac);
            if (peakRow > 0 && r == peakRow) {
                on = true;
                lit = st->palette.peak;
            }
            drawCell(dc, cell, lit, on, st->palette, st->style, glow);
        }
    }
}

void paintSignal(HDC dc, const RECT& in, MiniMeterState* st, std::vector<GlowCell>* glow) {
    const UINT dpi = st->dpi;
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    const int bars = 5;
    const int colW = std::max(2, (R - L) / bars);
    const int cellP = std::max(dpx(3, dpi), 2);
    const int gap = dpx(1, dpi);
    const float sl = std::clamp(st->sigLevel * st->tuning.sensitivity * 2.0f, 0.0f, 1.0f);
    const int litBars = static_cast<int>(std::ceil(sl * bars - 0.001f));
    COLORREF base = sl > 0.6f ? st->palette.low : (sl > 0.3f ? st->palette.mid : st->palette.high);
    base = lerpCol(base, st->palette.high, st->sigTrouble * 0.6f);
    for (int j = 0; j < bars; ++j) {
        const int x0 = L + j * colW;
        const int x1 = x0 + colW - gap;
        const int barTop = B - static_cast<int>(std::lround((B - T) * (j + 1) / float(bars)));
        const bool on = (j < litBars);
        for (int y = B; y > barTop; y -= cellP) {
            RECT cell{x0, y - cellP + gap, x1, y};
            drawCell(dc, cell, base, on, st->palette, st->style, glow);
        }
    }
}

void paintBitrate(HDC dc, const RECT& in, MiniMeterState* st, std::vector<GlowCell>* glow) {
    const UINT dpi = st->dpi;
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    const int cellP = std::max(dpx(3, dpi), 2);
    const int rows = std::max(1, (B - T) / cellP);
    const int colW = std::max(dpx(3, dpi), 2);
    const int gap = dpx(1, dpi);
    const int maxCols = std::max(1, (R - L) / colW);
    const int cols = std::min(maxCols, st->histCount);
    const float denom = std::max(st->histMax, 1.0f);
    for (int k = 0; k < cols; ++k) {  // k=0 -> newest (rightmost)
        const int idx = (st->histHead - 1 - k + kHist) % kHist;
        const float frac =
            std::clamp(st->hist[idx] / denom * st->tuning.sensitivity * 2.0f, 0.0f, 1.0f);
        const int litN = static_cast<int>(std::lround(frac * rows));
        const int x1 = R - k * colW;
        const int x0 = x1 - colW + gap;
        for (int r = 0; r < rows; ++r) {
            const int yb = B - r * cellP;
            RECT cell{x0, yb - cellP + gap, x1, yb};
            const bool on = (r < litN);
            const COLORREF lit = lerpCol(st->palette.accent, st->palette.peak,
                                         static_cast<float>(r) / rows * 0.5f);
            drawCell(dc, cell, lit, on, st->palette, st->style, glow);
        }
    }
}

void paintFrames(HDC dc, const RECT& in, MiniMeterState* st, std::vector<GlowCell>* glow) {
    const UINT dpi = st->dpi;
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    const int cellP = std::max(dpx(3, dpi), 2);
    const int rows = std::max(1, (B - T) / cellP);
    const int colW = std::max(dpx(4, dpi), 3);
    const int gap = dpx(1, dpi);
    const int cols = std::max(1, (R - L) / colW);
    const float frac = std::clamp(st->fps / 60.0f * st->tuning.sensitivity * 2.0f, 0.0f, 1.0f);
    const int lit = static_cast<int>(std::lround(frac * cols));
    COLORREF base = st->fps >= 24 ? st->palette.low
                                  : (st->fps >= 15 ? st->palette.mid : st->palette.high);
    base = lerpCol(base, st->palette.high, st->flare);  // flash toward the alert (high) colour
    for (int c = 0; c < cols; ++c) {
        const int x0 = L + c * colW;
        const int x1 = x0 + colW - gap;
        const bool on = (c < lit);
        for (int r = 0; r < rows; ++r) {
            const int yb = B - r * cellP;
            RECT cell{x0, yb - cellP + gap, x1, yb};
            drawCell(dc, cell, base, on, st->palette, st->style, glow);
        }
    }
}

// Ensure the cached back-buffer matches (w,h) and the glass LUTs match the active strength.
// Returns false if the surface couldn't be created (caller falls back to painting nothing).
bool ensureBack(MiniMeterState* st, HDC ref, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (!st->backDC) st->backDC = CreateCompatibleDC(ref);
    if (!st->backDC) return false;
    if (!st->backBmp || st->backW != w || st->backH != h) {
        if (st->backBmp) {
            SelectObject(st->backDC, st->backOld);
            DeleteObject(st->backBmp);
            st->backBmp = nullptr;
            st->backBits = nullptr;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down, so row y is at bits + y*w
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        st->backBmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &st->backBits, nullptr, 0);
        if (!st->backBmp) return false;
        st->backOld = SelectObject(st->backDC, st->backBmp);
        st->backW = w;
        st->backH = h;
        st->glassBuilt = -1.0f;  // force a LUT rebuild at the new size
    }
    const float want = meterGlassRef().load(std::memory_order_relaxed);
    const int chrome = meterChromePx(st->dpi);
    if (st->glassBuilt != want || st->glassChrome != chrome) {
        buildGlassMask(w, h, GlassParams{want, chrome}, st->glassAdd, st->glassMul);
        st->glassBuilt = want;
        st->glassChrome = chrome;
    }
    return true;
}

// Apply the cached glass LUTs over the finished frame: darken toward the edges, then add the
// specular. Two byte ops per channel per pixel — the whole meter tray is ~0.7% of a 1080p frame.
void applyGlass(MiniMeterState* st) {
    if (st->glassBuilt <= 0.0f || !st->backBits) return;
    const size_t n = static_cast<size_t>(st->backW) * static_cast<size_t>(st->backH);
    if (st->glassAdd.size() != n || st->glassMul.size() != n) return;
    auto* px = static_cast<uint8_t*>(st->backBits);  // BGRA
    for (size_t i = 0; i < n; ++i) {
        const unsigned mul = st->glassMul[i], add = st->glassAdd[i];
        for (int ch = 0; ch < 3; ++ch) {
            unsigned v = px[i * 4 + ch];
            v = (v * mul) / 255u + add;
            px[i * 4 + ch] = static_cast<uint8_t>(v > 255u ? 255u : v);
        }
    }
}

void onPaint(HWND hwnd, MiniMeterState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (!ensureBack(st, hdc, rc.right, rc.bottom)) {
        EndPaint(hwnd, &ps);
        return;
    }
    HDC mem = st->backDC;

    const Theme& th = currentTheme();
    // For the Vu look `bg` is the LAMP behind the dial, not the panel colour (see drawVu), so the
    // hairline matte around the dial stays the theme's — an electric-blue lamp must not also put a
    // bright outline round the bezel. Both reference instruments have a dark surround doing much of
    // the work of looking like an instrument.
    const COLORREF bg = (st->style == MeterStyle::Vu || st->palette.bg == CLR_INVALID)
                            ? th.windowBg
                            : st->palette.bg;
    fillCell(mem, rc, bg);
    SetDCBrushColor(mem, th.border);
    FrameRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    const int inset = meterChromePx(st->dpi);
    RECT in{rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset};
    if (in.right > in.left && in.bottom > in.top) {
        if (st->style == MeterStyle::Vu) {
            drawVu(mem, in, st);
        } else if (st->style == MeterStyle::Scope) {
            drawScope(mem, in, st);
        } else {
            // Tube collects its lit cells for a soft GDI+ halo pass afterwards; the
            // other looks pass nullptr, so there's no collection and no overhead.
            std::vector<GlowCell> glow;
            std::vector<GlowCell>* glowPtr = (st->style == MeterStyle::Tube) ? &glow : nullptr;
            switch (st->kind) {
                case MeterKind::Spectrum: paintSpectrum(mem, in, st, glowPtr); break;
                case MeterKind::Signal: paintSignal(mem, in, st, glowPtr); break;
                case MeterKind::Bitrate: paintBitrate(mem, in, st, glowPtr); break;
                case MeterKind::Frames: paintFrames(mem, in, st, glowPtr); break;
            }
            if (glowPtr) drawTubeGlow(mem, glow, st->palette, st->tuning.glow);
        }
    }
    // Glass goes on LAST, over the finished dials — including the tube halo and the scope trace,
    // which is the point: the highlight must sit on the pane, not under the phosphor. GDI+ (used by
    // those two) writes through the same DIB, so the pixels are all there by now. GdiFlush()
    // guarantees that: the batched GDI calls above must land before we read the bits directly.
    if (st->glassBuilt > 0.0f) {
        GdiFlush();
        applyGlass(st);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

void onTick(MiniMeterState* st) {
    // Spectrum — consume the latest pushed frame (or zeros if none arrived, so the
    // bars fall to silence when audio stops), then attack-fast / decay-slow + peaks.
    float d[kMaxBands];
    bool has = false;
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (st->hasPending) {
            has = true;
            n = st->pendingN;
            std::memcpy(d, st->pending, sizeof(float) * std::min(n, kMaxBands));
            st->hasPending = false;
        }
    }
    if (has) st->bands = std::min(n, kMaxBands);
    // Knob-derived easing — 0.5 knobs reproduce the classic constants (decay .80,
    // peakFall .02, signal ease .25, flare decay .90).
    const float decay = std::clamp(0.60f + st->tuning.smoothing * 0.40f, 0.60f, 0.97f);
    const float peakFall = std::clamp(0.04f * (1.0f - st->tuning.peakHold), 0.002f, 0.04f);
    for (int i = 0; i < st->bands; ++i) {
        const float target = (has && i < n) ? d[i] : 0.0f;
        st->level[i] = std::max(target, st->level[i] * decay);
        st->peak[i] = std::max(st->level[i], st->peak[i] - peakFall);
    }
    // Signal easing + frame-drop flare decay.
    const float sigEase = std::clamp(0.45f - st->tuning.smoothing * 0.40f, 0.05f, 0.45f);
    const float flareDecay = std::clamp(0.80f + st->tuning.smoothing * 0.20f, 0.80f, 0.97f);
    st->sigLevel += (st->sigTarget - st->sigLevel) * sigEase;
    st->flare *= flareDecay;
    if (st->flare < 0.01f) st->flare = 0.0f;

    // VU needle ballistics. Integrated EVERY tick regardless of the current look, so switching to
    // Vu shows a needle already tracking the signal instead of one sweeping up from zero.
    //
    // A real VU movement reaches ~99% in 300ms and is SYMMETRIC — it is a damped mechanical
    // movement, not an envelope follower, so it lags in both directions. At 33ms/tick that is a
    // per-tick coefficient of ~1-exp(-33/65) ≈ 0.40 for a 300ms settle (t99 ≈ 4.6 time constants).
    // The smoothing knob still scales it, so a user can have a livelier or lazier needle, but the
    // centre of the range is the real instrument.
    const float vuBase = 0.40f;
    const float vuCoef = std::clamp(vuBase * (1.35f - st->tuning.smoothing * 0.70f), 0.06f, 0.75f);
    st->vuNeedle += (scalarLevel(st) - st->vuNeedle) * vuCoef;
}

// Run the ~30fps animation timer only while the meter is actually shown.
void syncTimer(HWND hwnd, MiniMeterState* st, bool run) {
    if (run && !st->timerOn) {
        SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
        st->timerOn = true;
    } else if (!run && st->timerOn) {
        KillTimer(hwnd, kTimerId);
        st->timerOn = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MiniMeterState* st = stateOf(hwnd);
    switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (st) {
                // Also sync here: on a fresh launch every layout() runs BEFORE the
                // top-level window is shown, so the SWP_SHOWWINDOW WM_WINDOWPOSCHANGED
                // below sees IsWindowVisible()==FALSE (hidden parent) and the parent's
                // later ShowWindow sends its children nothing. A WM_PAINT only arrives
                // once the meter is genuinely on screen — the cheap check is a no-op
                // on every subsequent paint.
                syncTimer(hwnd, st, IsWindowVisible(hwnd) != FALSE);
                onPaint(hwnd, st);
            }
            return 0;
        case WM_TIMER:
            if (st && wParam == kTimerId) {
                onTick(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_WINDOWPOSCHANGED:
            // layout() shows/hides the meters via DeferWindowPos(SWP_SHOWWINDOW /
            // SWP_HIDEWINDOW), which does NOT send WM_SHOWWINDOW (documented SetWindowPos
            // behavior) — so gating the timer on WM_SHOWWINDOW alone left the meters
            // frozen (empty spectrum/bitrate, signal stuck at 0, fps 0) until something
            // like a minimize/restore finally delivered one. Sync to real visibility on
            // every positional change; WM_SHOWWINDOW below still covers parent-minimize.
            if (st) syncTimer(hwnd, st, IsWindowVisible(hwnd) != FALSE);
            break;  // let DefWindowProc do its WM_SIZE/WM_MOVE bookkeeping
        case WM_SHOWWINDOW:
            if (st) syncTimer(hwnd, st, wParam != FALSE);
            return 0;
        case WM_DESTROY:
            if (st && st->timerOn) {
                KillTimer(hwnd, kTimerId);
                st->timerOn = false;
            }
            if (st && st->backDC) {  // release the cached back-buffer (see ensureBack)
                if (st->backBmp) {
                    SelectObject(st->backDC, st->backOld);
                    DeleteObject(st->backBmp);
                }
                DeleteDC(st->backDC);
                st->backDC = nullptr;
                st->backBmp = nullptr;
                st->backBits = nullptr;
                st->backW = st->backH = 0;
            }
            return 0;
        case WM_NCDESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void registerMiniMeterClass(HINSTANCE hInst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

HWND createMiniMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi, MeterKind kind) {
    auto* st = new MiniMeterState();
    st->kind = kind;
    st->style = defaultMeterStyle(kind);
    st->palette = defaultMeterPalette(kind);
    st->dpi = dpi;
    HWND h = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    if (!h) {
        delete st;
        return nullptr;
    }
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    return h;
}

MeterKind miniMeterKind(HWND meter) {
    MiniMeterState* st = stateOf(meter);
    return st ? st->kind : MeterKind::Spectrum;
}

void miniMeterPushSpectrum(HWND meter, const float* bands, int count) {
    MiniMeterState* st = stateOf(meter);
    if (!st || !bands || count <= 0) return;
    count = std::min(count, kMaxBands);
    std::lock_guard<std::mutex> lk(st->mtx);
    std::memcpy(st->pending, bands, sizeof(float) * count);
    st->pendingN = count;
    st->hasPending = true;
}

void miniMeterSetSignal(HWND meter, float strength, float trouble) {
    MiniMeterState* st = stateOf(meter);
    if (!st) return;
    st->sigTarget = std::clamp(strength, 0.0f, 1.0f);
    st->sigTrouble = std::clamp(trouble, 0.0f, 1.0f);
}

void miniMeterPushBitrate(HWND meter, double bytesPerSec) {
    MiniMeterState* st = stateOf(meter);
    if (!st) return;
    const float v = static_cast<float>(std::max(0.0, bytesPerSec));
    st->hist[st->histHead] = v;
    st->histHead = (st->histHead + 1) % kHist;
    if (st->histCount < kHist) ++st->histCount;
    // Adaptive ceiling: rise instantly to a new peak, ebb slowly so the graph
    // re-scales after a spike instead of flat-lining.
    const float ebb = std::clamp(0.995f - st->tuning.breathing * 0.02f, 0.95f, 0.999f);
    st->histMax = std::max(v, st->histMax * ebb);  // breathing knob (0.5 = classic 0.985)
    if (st->histMax < 1.0f) st->histMax = 1.0f;
}

void miniMeterSetFrames(HWND meter, int fps, int dropsDelta) {
    MiniMeterState* st = stateOf(meter);
    if (!st) return;
    st->fps = std::max(0, fps);
    if (dropsDelta > 0) st->flare = 1.0f;
}

void miniMeterReset(HWND meter) {
    MiniMeterState* st = stateOf(meter);
    if (!st) return;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->hasPending = false;
        st->pendingN = 0;
    }
    for (int i = 0; i < kMaxBands; ++i) st->level[i] = st->peak[i] = 0.0f;
    st->sigTarget = st->sigLevel = st->sigTrouble = 0.0f;
    st->histHead = st->histCount = 0;
    st->histMax = 1.0f;
    for (int i = 0; i < kHist; ++i) st->hist[i] = 0.0f;
    st->fps = 0;
    st->flare = 0.0f;
    if (IsWindow(meter)) InvalidateRect(meter, nullptr, FALSE);
}

void miniMeterSetDpi(HWND meter, UINT dpi) {
    MiniMeterState* st = stateOf(meter);
    if (st) st->dpi = dpi;
}

void miniMeterSetStyle(HWND meter, MeterStyle style) {
    MiniMeterState* st = stateOf(meter);
    if (!st) return;
    st->style = style;
    if (IsWindow(meter)) InvalidateRect(meter, nullptr, FALSE);
}

void miniMeterSetPalette(HWND meter, const MeterPalette& palette) {
    MiniMeterState* st = stateOf(meter);
    if (!st) return;
    st->palette = palette;
    if (IsWindow(meter)) InvalidateRect(meter, nullptr, FALSE);
}

MeterStyle miniMeterStyle(HWND meter) {
    MiniMeterState* st = stateOf(meter);
    return st ? st->style : MeterStyle::Led;
}

MeterPalette miniMeterPalette(HWND meter) {
    MiniMeterState* st = stateOf(meter);
    return st ? st->palette : defaultMeterPalette(MeterKind::Spectrum);
}

void miniMeterSetTuning(HWND meter, const MeterTuning& tuning) {
    MiniMeterState* st = stateOf(meter);
    if (!st) return;
    st->tuning = tuning;
    if (IsWindow(meter)) InvalidateRect(meter, nullptr, FALSE);
}

MeterTuning miniMeterTuning(HWND meter) {
    MiniMeterState* st = stateOf(meter);
    return st ? st->tuning : defaultMeterTuning();
}

// ---- style / palette (de)serialization for the settings K/V store ----------
namespace {
std::wstring hexColor(COLORREF c) {
    wchar_t b[8];
    swprintf_s(b, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}
COLORREF parseHexColor(const std::wstring& s, COLORREF fallback) {
    if (s.size() != 6) return fallback;
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (!end || *end != L'\0') return fallback;
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}
}  // namespace

std::wstring meterStyleToString(MeterStyle style) {
    switch (style) {
        case MeterStyle::Tube:  return L"tube";
        case MeterStyle::Lcd:   return L"lcd";
        case MeterStyle::Scope: return L"scope";
        case MeterStyle::Vu:    return L"vu";
        case MeterStyle::Led:
        default:                return L"led";
    }
}

MeterStyle meterStyleFromString(const std::wstring& s, MeterStyle fallback) {
    if (s == L"led") return MeterStyle::Led;
    if (s == L"tube") return MeterStyle::Tube;
    if (s == L"lcd") return MeterStyle::Lcd;
    if (s == L"scope") return MeterStyle::Scope;
    if (s == L"vu") return MeterStyle::Vu;
    return fallback;
}

std::wstring meterPaletteToString(const MeterPalette& p) {
    const std::wstring bg = (p.bg == CLR_INVALID) ? std::wstring(L"theme") : hexColor(p.bg);
    return bg + L"," + hexColor(p.off) + L"," + hexColor(p.low) + L"," + hexColor(p.mid) + L"," +
           hexColor(p.high) + L"," + hexColor(p.accent) + L"," + hexColor(p.peak);
}

MeterPalette meterPaletteFromString(const std::wstring& s, const MeterPalette& fallback) {
    std::vector<std::wstring> tok;
    size_t start = 0;
    for (;;) {
        const size_t comma = s.find(L',', start);
        if (comma == std::wstring::npos) {
            tok.push_back(s.substr(start));
            break;
        }
        tok.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    if (tok.size() != 7) return fallback;
    MeterPalette p = fallback;
    p.bg = (tok[0] == L"theme") ? CLR_INVALID : parseHexColor(tok[0], fallback.bg);
    p.off = parseHexColor(tok[1], fallback.off);
    p.low = parseHexColor(tok[2], fallback.low);
    p.mid = parseHexColor(tok[3], fallback.mid);
    p.high = parseHexColor(tok[4], fallback.high);
    p.accent = parseHexColor(tok[5], fallback.accent);
    p.peak = parseHexColor(tok[6], fallback.peak);
    return p;
}

std::wstring meterTuningToString(const MeterTuning& t) {
    wchar_t b[96];
    swprintf_s(b, L"%.3f,%.3f,%.3f,%.3f,%.3f", t.glow, t.smoothing, t.sensitivity, t.peakHold,
               t.breathing);
    return b;
}

MeterTuning meterTuningFromString(const std::wstring& s, const MeterTuning& fallback) {
    const float fb[5] = {fallback.glow, fallback.smoothing, fallback.sensitivity, fallback.peakHold,
                         fallback.breathing};
    float v[5];
    size_t start = 0;
    for (int i = 0; i < 5; ++i) {
        const size_t comma = s.find(L',', start);
        const std::wstring tok =
            (comma == std::wstring::npos) ? s.substr(start) : s.substr(start, comma - start);
        wchar_t* end = nullptr;
        const double d = wcstod(tok.c_str(), &end);
        v[i] = (end && end != tok.c_str()) ? std::clamp(static_cast<float>(d), 0.0f, 1.0f) : fb[i];
        if (comma == std::wstring::npos) {  // short string — remaining fields fall back
            for (int k = i + 1; k < 5; ++k) v[k] = fb[k];
            break;
        }
        start = comma + 1;
    }
    return MeterTuning{v[0], v[1], v[2], v[3], v[4]};
}

void miniMeterSetGlass(float strength) {
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    meterGlassRef().store(strength, std::memory_order_relaxed);
    // Each meter rebuilds its LUTs lazily in ensureBack() when it next paints (it compares the
    // strength it built against this one), so there is nothing to invalidate here.
}

float miniMeterGlass() { return meterGlassRef().load(std::memory_order_relaxed); }

}  // namespace rabbitears
