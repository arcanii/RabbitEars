// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/MiniMeter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "ui/Theme.h"

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReMiniMeter";
constexpr int kMaxBands = 32;
constexpr int kHist = 64;      // bitrate history ring length
constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 33;  // ~30fps animation

// LED palette (fixed — drawn with the DC brush, never themeBrush, whose 12-slot
// cache would leak if we pushed this many distinct colors through it).
constexpr COLORREF kOff = RGB(38, 40, 44);
constexpr COLORREF kGreen = RGB(96, 205, 128);
constexpr COLORREF kAmber = RGB(232, 188, 86);
constexpr COLORREF kRed = RGB(232, 96, 86);
constexpr COLORREF kCoral = RGB(217, 119, 87);
constexpr COLORREF kPeak = RGB(236, 236, 240);

int dpx(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

COLORREF lerpCol(COLORREF a, COLORREF b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](int x, int y) { return x + static_cast<int>((y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
              mix(GetBValue(a), GetBValue(b)));
}

struct MiniMeterState {
    MeterKind kind = MeterKind::Spectrum;
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

void paintSpectrum(HDC dc, const RECT& in, MiniMeterState* st) {
    const UINT dpi = st->dpi;
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    const int bands = st->bands;
    if (bands <= 0) return;
    const int cellP = std::max(dpx(3, dpi), 2);
    const int rows = std::max(1, (B - T) / cellP);
    const int colW = std::max(2, (R - L) / bands);
    const int gap = dpx(1, dpi);
    for (int c = 0; c < bands; ++c) {
        const int x0 = L + c * colW;
        const int x1 = x0 + colW - gap;
        const int litN = static_cast<int>(std::lround(st->level[c] * rows));
        const int peakRow = static_cast<int>(std::lround(st->peak[c] * rows));
        for (int r = 0; r < rows; ++r) {
            const int yb = B - r * cellP;
            RECT cell{x0, yb - cellP + gap, x1, yb};
            const float frac = static_cast<float>(r) / rows;
            COLORREF col = (r < litN) ? (frac < 0.60f ? kGreen : frac < 0.85f ? kAmber : kRed) : kOff;
            if (peakRow > 0 && r == peakRow) col = kPeak;
            fillCell(dc, cell, col);
        }
    }
}

void paintSignal(HDC dc, const RECT& in, MiniMeterState* st) {
    const UINT dpi = st->dpi;
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    const int bars = 5;
    const int colW = std::max(2, (R - L) / bars);
    const int cellP = std::max(dpx(3, dpi), 2);
    const int gap = dpx(1, dpi);
    const int litBars = static_cast<int>(std::ceil(st->sigLevel * bars - 0.001f));
    COLORREF base = st->sigLevel > 0.6f ? kGreen : (st->sigLevel > 0.3f ? kAmber : kRed);
    base = lerpCol(base, kRed, st->sigTrouble * 0.6f);
    for (int j = 0; j < bars; ++j) {
        const int x0 = L + j * colW;
        const int x1 = x0 + colW - gap;
        const int barTop = B - static_cast<int>(std::lround((B - T) * (j + 1) / float(bars)));
        const COLORREF col = (j < litBars) ? base : kOff;
        for (int y = B; y > barTop; y -= cellP) {
            RECT cell{x0, y - cellP + gap, x1, y};
            fillCell(dc, cell, col);
        }
    }
}

void paintBitrate(HDC dc, const RECT& in, MiniMeterState* st) {
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
        const float frac = std::clamp(st->hist[idx] / denom, 0.0f, 1.0f);
        const int litN = static_cast<int>(std::lround(frac * rows));
        const int x1 = R - k * colW;
        const int x0 = x1 - colW + gap;
        for (int r = 0; r < rows; ++r) {
            const int yb = B - r * cellP;
            RECT cell{x0, yb - cellP + gap, x1, yb};
            const COLORREF col = (r < litN) ? lerpCol(kCoral, kPeak, static_cast<float>(r) / rows * 0.5f)
                                            : kOff;
            fillCell(dc, cell, col);
        }
    }
}

void paintFrames(HDC dc, const RECT& in, MiniMeterState* st) {
    const UINT dpi = st->dpi;
    const int L = static_cast<int>(in.left), R = static_cast<int>(in.right);
    const int T = static_cast<int>(in.top), B = static_cast<int>(in.bottom);
    const int cellP = std::max(dpx(3, dpi), 2);
    const int rows = std::max(1, (B - T) / cellP);
    const int colW = std::max(dpx(4, dpi), 3);
    const int gap = dpx(1, dpi);
    const int cols = std::max(1, (R - L) / colW);
    const float frac = std::clamp(st->fps / 60.0f, 0.0f, 1.0f);
    const int lit = static_cast<int>(std::lround(frac * cols));
    COLORREF base = st->fps >= 24 ? kGreen : (st->fps >= 15 ? kAmber : kRed);
    base = lerpCol(base, kRed, st->flare);  // flash red on dropped frames
    for (int c = 0; c < cols; ++c) {
        const int x0 = L + c * colW;
        const int x1 = x0 + colW - gap;
        const COLORREF col = (c < lit) ? base : kOff;
        for (int r = 0; r < rows; ++r) {
            const int yb = B - r * cellP;
            RECT cell{x0, yb - cellP + gap, x1, yb};
            fillCell(dc, cell, col);
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
    fillCell(mem, rc, th.windowBg);
    SetDCBrushColor(mem, th.border);
    FrameRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    const int inset = dpx(2, st->dpi);
    RECT in{rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset};
    if (in.right > in.left && in.bottom > in.top) {
        switch (st->kind) {
            case MeterKind::Spectrum: paintSpectrum(mem, in, st); break;
            case MeterKind::Signal: paintSignal(mem, in, st); break;
            case MeterKind::Bitrate: paintBitrate(mem, in, st); break;
            case MeterKind::Frames: paintFrames(mem, in, st); break;
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
    constexpr float kSpecDecay = 0.80f, kPeakFall = 0.02f;
    for (int i = 0; i < st->bands; ++i) {
        const float target = (has && i < n) ? d[i] : 0.0f;
        st->level[i] = std::max(target, st->level[i] * kSpecDecay);
        st->peak[i] = std::max(st->level[i], st->peak[i] - kPeakFall);
    }
    // Signal easing + frame-drop flare decay.
    st->sigLevel += (st->sigTarget - st->sigLevel) * 0.25f;
    st->flare *= 0.90f;
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
    st->histMax = std::max(v, st->histMax * 0.985f);
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

}  // namespace rabbitears
