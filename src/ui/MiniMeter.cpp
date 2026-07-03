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
};

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

void onPaint(HWND hwnd, MiniMeterState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    const Theme& th = currentTheme();
    const COLORREF bg = (st->palette.bg == CLR_INVALID) ? th.windowBg : st->palette.bg;
    fillCell(mem, rc, bg);
    SetDCBrushColor(mem, th.border);
    FrameRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    const int inset = dpx(2, st->dpi);
    RECT in{rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset};
    if (in.right > in.left && in.bottom > in.top) {
        if (st->style == MeterStyle::Scope) {
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
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
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
        case MeterStyle::Led:
        default:                return L"led";
    }
}

MeterStyle meterStyleFromString(const std::wstring& s, MeterStyle fallback) {
    if (s == L"led") return MeterStyle::Led;
    if (s == L"tube") return MeterStyle::Tube;
    if (s == L"lcd") return MeterStyle::Lcd;
    if (s == L"scope") return MeterStyle::Scope;
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

}  // namespace rabbitears
