// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/BufferMeter.h"

#include <algorithm>
#include <cmath>

#include "ui/Theme.h"

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReBufferMeter";
int dpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

struct MeterState {
    int  percent = 0;
    UINT dpi = 96;
};
MeterState* stateOf(HWND h) { return reinterpret_cast<MeterState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

COLORREF lerp(COLORREF a, COLORREF b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto mix = [&](int ca, int cb) { return static_cast<int>(ca + (cb - ca) * t + 0.5); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
              mix(GetBValue(a), GetBValue(b)));
}

void paint(HWND hwnd, MeterState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    const Theme& th = currentTheme();
    FillRect(mem, &rc, themeBrush(th.windowBg));

    const int padY = dpx(st->dpi, 4);
    const int segW = dpx(st->dpi, 4), gap = dpx(st->dpi, 3), unit = segW + gap;
    const int avail = rc.right;
    const int count = std::max(1, (avail + gap) / unit);
    const int lit = static_cast<int>(std::lround(st->percent / 100.0 * count));

    const COLORREF dark = RGB(74, 46, 36);      // deep coral
    const COLORREF bright = th.accent;          // coral
    const COLORREF dim = th.panelElevBg;        // unlit
    const COLORREF knobC = th.selectionText;    // light edge

    const int top = padY, bot = rc.bottom - padY;
    for (int i = 0; i < count; ++i) {
        const int x = i * unit;
        RECT seg{x, top, x + segW, bot};
        COLORREF col;
        if (i < lit) {
            const double t = (lit > 1) ? static_cast<double>(i) / (lit - 1) : 1.0;
            col = lerp(dark, bright, t);
        } else {
            col = dim;
        }
        HBRUSH br = CreateSolidBrush(col);
        FillRect(mem, &seg, br);
        DeleteObject(br);
    }
    // Knob at the fill edge while actively buffering.
    if (st->percent > 0 && st->percent < 100 && lit > 0 && lit < count) {
        const int x = (lit - 1) * unit;
        RECT knob{x - dpx(st->dpi, 1), 0, x + segW + dpx(st->dpi, 1), rc.bottom};
        HBRUSH br = CreateSolidBrush(knobC);
        FillRect(mem, &knob, br);
        DeleteObject(br);
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK MeterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* st = new MeterState();
        st->dpi = GetDpiForWindow(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    MeterState* st = stateOf(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(hwnd, st);
            return 0;
        case WM_NCDESTROY:
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void registerBufferMeterClass(HINSTANCE hInst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MeterProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

HWND createBufferMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi) {
    HWND h = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    if (h)
        if (MeterState* st = stateOf(h)) st->dpi = dpi;
    return h;
}

void bufferMeterSet(HWND meter, int percent) {
    MeterState* st = stateOf(meter);
    if (!st) return;
    percent = std::clamp(percent, 0, 100);
    if (percent == st->percent) return;
    st->percent = percent;
    InvalidateRect(meter, nullptr, FALSE);
}

void bufferMeterSetDpi(HWND meter, UINT dpi) {
    if (MeterState* st = stateOf(meter)) {
        st->dpi = dpi;
        InvalidateRect(meter, nullptr, FALSE);
    }
}

}  // namespace rabbitears
