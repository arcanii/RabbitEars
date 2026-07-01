// SPDX-License-Identifier: GPL-3.0-or-later
// A 2D Navier-Stokes "stable fluids" solver (Jos Stam) driving a little tank of
// liquid whose level tracks stream health. Density relaxes toward a fill profile
// (level = health); velocity advects it into sloshing waves; gravity + injected
// gusts keep it alive. Renders to a DIB stretched into the widget.
#include "ui/BufferMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "ui/Theme.h"

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReBufferMeter";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 33;

// Fluid grid (interior NX x NY, with a 1-cell border for boundaries).
constexpr int NX = 72, NY = 20;
constexpr int GW = NX + 2, GH = NY + 2;
inline int IX(int i, int j) { return i + GW * j; }

// Tunables (feel free to tweak).
constexpr int   ITER = 8;
constexpr float SIM_DT = 0.12f;
constexpr float VISC = 1e-5f;
constexpr float DIFF = 2e-6f;
constexpr float RELAX = 4.0f;   // density -> fill-profile pull
constexpr float GRAV = 9.0f;    // downward pull on liquid
constexpr int   MENU_HIDE = 1;

int dpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

struct Fluid {
    std::vector<float> u, v, u0, v0, d, d0;
    Fluid() {
        const int n = GW * GH;
        u.assign(n, 0); v.assign(n, 0); u0.assign(n, 0); v0.assign(n, 0);
        d.assign(n, 0); d0.assign(n, 0);
    }
};

void set_bnd(int b, float* x) {
    for (int i = 1; i <= NX; ++i) {
        x[IX(i, 0)] = (b == 2) ? -x[IX(i, 1)] : x[IX(i, 1)];
        x[IX(i, NY + 1)] = (b == 2) ? -x[IX(i, NY)] : x[IX(i, NY)];
    }
    for (int j = 1; j <= NY; ++j) {
        x[IX(0, j)] = (b == 1) ? -x[IX(1, j)] : x[IX(1, j)];
        x[IX(NX + 1, j)] = (b == 1) ? -x[IX(NX, j)] : x[IX(NX, j)];
    }
    x[IX(0, 0)] = 0.5f * (x[IX(1, 0)] + x[IX(0, 1)]);
    x[IX(0, NY + 1)] = 0.5f * (x[IX(1, NY + 1)] + x[IX(0, NY)]);
    x[IX(NX + 1, 0)] = 0.5f * (x[IX(NX, 0)] + x[IX(NX + 1, 1)]);
    x[IX(NX + 1, NY + 1)] = 0.5f * (x[IX(NX, NY + 1)] + x[IX(NX + 1, NY)]);
}

void lin_solve(int b, float* x, const float* x0, float a, float c) {
    const float invc = 1.0f / c;
    for (int k = 0; k < ITER; ++k) {
        for (int j = 1; j <= NY; ++j)
            for (int i = 1; i <= NX; ++i)
                x[IX(i, j)] = (x0[IX(i, j)] + a * (x[IX(i - 1, j)] + x[IX(i + 1, j)] +
                                                   x[IX(i, j - 1)] + x[IX(i, j + 1)])) *
                              invc;
        set_bnd(b, x);
    }
}

void diffuse(int b, float* x, float* x0, float diff, float dt) {
    const float a = dt * diff * NX * NY;
    lin_solve(b, x, x0, a, 1 + 4 * a);
}

void advect(int b, float* d, const float* d0, const float* u, const float* v, float dt) {
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
            float x = i - dt * u[IX(i, j)];
            float y = j - dt * v[IX(i, j)];
            x = std::clamp(x, 0.5f, NX + 0.5f);
            y = std::clamp(y, 0.5f, NY + 0.5f);
            const int i0 = static_cast<int>(x), i1 = i0 + 1;
            const int j0 = static_cast<int>(y), j1 = j0 + 1;
            const float s1 = x - i0, s0 = 1 - s1, t1 = y - j0, t0 = 1 - t1;
            d[IX(i, j)] = s0 * (t0 * d0[IX(i0, j0)] + t1 * d0[IX(i0, j1)]) +
                          s1 * (t0 * d0[IX(i1, j0)] + t1 * d0[IX(i1, j1)]);
        }
    set_bnd(b, d);
}

void project(float* u, float* v, float* p, float* div) {
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
            div[IX(i, j)] = -0.5f * (u[IX(i + 1, j)] - u[IX(i - 1, j)] + v[IX(i, j + 1)] -
                                     v[IX(i, j - 1)]);
            p[IX(i, j)] = 0;
        }
    set_bnd(0, div);
    set_bnd(0, p);
    lin_solve(0, p, div, 1, 4);
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
            u[IX(i, j)] -= 0.5f * (p[IX(i + 1, j)] - p[IX(i - 1, j)]);
            v[IX(i, j)] -= 0.5f * (p[IX(i, j + 1)] - p[IX(i, j - 1)]);
        }
    set_bnd(1, u);
    set_bnd(2, v);
}

void vel_step(Fluid& f, float dt) {
    diffuse(1, f.u0.data(), f.u.data(), VISC, dt);
    diffuse(2, f.v0.data(), f.v.data(), VISC, dt);
    project(f.u0.data(), f.v0.data(), f.u.data(), f.v.data());
    advect(1, f.u.data(), f.u0.data(), f.u0.data(), f.v0.data(), dt);
    advect(2, f.v.data(), f.v0.data(), f.u0.data(), f.v0.data(), dt);
    project(f.u.data(), f.v.data(), f.u0.data(), f.v0.data());
}

void dens_step(Fluid& f, float dt) {
    // f.d0 holds the source term (set by the caller).
    const int n = GW * GH;
    for (int i = 0; i < n; ++i) f.d[i] += dt * f.d0[i];
    diffuse(0, f.d0.data(), f.d.data(), DIFF, dt);
    advect(0, f.d.data(), f.d0.data(), f.u.data(), f.v.data(), dt);
    for (int i = 0; i < n; ++i) f.d[i] = std::clamp(f.d[i], 0.0f, 1.4f);
}

struct MeterState {
    Fluid fluid;
    float target = 0.0f;   // fill level 0..1 (buffer health)
    bool  hidden = false;
    bool  timerOn = false;
    UINT  dpi = 96;
    uint32_t rng = 0x1234abcdu;
    HBITMAP dib = nullptr;
    HDC     dibDC = nullptr;
    uint32_t* bits = nullptr;
    std::function<void(bool)> onHidden;
};
MeterState* stateOf(HWND h) { return reinterpret_cast<MeterState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

float frand(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xFFFFFF) / static_cast<float>(0x1000000);
}

float totalFluid(const Fluid& f) {
    float t = 0;
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) t += f.d[IX(i, j)];
    return t;
}

void step(MeterState* st) {
    Fluid& f = st->fluid;
    const float fill = st->target;

    // Gusts + gravity keep the surface alive; liveliness scales with fill.
    if (fill > 0.02f) {
        for (int g = 0; g < 3; ++g) {
            const int gx = 1 + static_cast<int>(frand(st->rng) * NX);
            const float sy = (1.0f - fill) * NY;
            const int gy = std::clamp(static_cast<int>(sy + (frand(st->rng) - 0.5f) * 4), 1, NY);
            f.u[IX(std::clamp(gx, 1, NX), gy)] -= (2.5f + 5.0f * fill);      // flow toward the left
            f.v[IX(std::clamp(gx, 1, NX), gy)] += (frand(st->rng) - 0.5f) * 8.0f * fill;
        }
    }
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i)
            if (f.d[IX(i, j)] > 0.1f) f.v[IX(i, j)] += GRAV * SIM_DT;

    // Density source: pull toward the fill profile (liquid below the water line).
    for (int j = 1; j <= NY; ++j) {
        const float depthFromTop = (j - 0.5f) / NY;  // 0 top .. 1 bottom
        float tgt = (depthFromTop - (1.0f - fill)) * NY / 1.5f + 0.5f;
        tgt = std::clamp(tgt, 0.0f, 1.0f);
        for (int i = 1; i <= NX; ++i) f.d0[IX(i, j)] = RELAX * (tgt - f.d[IX(i, j)]);
    }

    vel_step(f, SIM_DT);
    dens_step(f, SIM_DT);
}

uint32_t packRGB(int r, int g, int b) {
    r = std::clamp(r, 0, 255); g = std::clamp(g, 0, 255); b = std::clamp(b, 0, 255);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

void ensureDib(HWND hwnd, MeterState* st) {
    if (st->dib) return;
    HDC hdc = GetDC(hwnd);
    st->dibDC = CreateCompatibleDC(hdc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = NX;
    bmi.bmiHeader.biHeight = -NY;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    st->dib = CreateDIBSection(st->dibDC, &bmi, DIB_RGB_COLORS,
                               reinterpret_cast<void**>(&st->bits), nullptr, 0);
    if (st->dib) SelectObject(st->dibDC, st->dib);
    ReleaseDC(hwnd, hdc);
}

void render(HWND hwnd, MeterState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const Theme& th = currentTheme();

    if (st->hidden) {
        FillRect(hdc, &rc, themeBrush(th.windowBg));
        // A faint baseline so it stays right-clickable to re-show.
        RECT line{0, rc.bottom - dpx(st->dpi, 2), rc.right, rc.bottom};
        FillRect(hdc, &line, themeBrush(th.border));
        EndPaint(hwnd, &ps);
        return;
    }

    ensureDib(hwnd, st);
    if (st->bits) {
        const int bgR = GetRValue(th.windowBg), bgG = GetGValue(th.windowBg), bgB = GetBValue(th.windowBg);
        for (int j = 0; j < NY; ++j)
            for (int i = 0; i < NX; ++i) {
                const float d = st->fluid.d[IX(i + 1, j + 1)];
                const float a = std::clamp(d, 0.0f, 1.0f);
                const float alpha = a * a * (3 - 2 * a);  // smoothstep
                const float depthT = static_cast<float>(j) / (NY - 1);
                // liquid: bright violet near the surface, deeper toward the bottom
                const int lr = static_cast<int>(196 - depthT * 120);
                const int lg = static_cast<int>(150 - depthT * 110);
                const int lb = static_cast<int>(246 - depthT * 150);
                int r = static_cast<int>(bgR + (lr - bgR) * alpha);
                int g = static_cast<int>(bgG + (lg - bgG) * alpha);
                int b = static_cast<int>(bgB + (lb - bgB) * alpha);
                const float glow = 4.0f * a * (1 - a) * alpha;  // glowing meniscus at the surface
                r = static_cast<int>(r + (235 - r) * glow * 0.6f);
                g = static_cast<int>(g + (215 - g) * glow * 0.6f);
                b = static_cast<int>(b + (255 - b) * glow * 0.6f);
                st->bits[j * NX + i] = packRGB(r, g, b);
            }
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, nullptr);
        StretchBlt(hdc, 0, 0, rc.right, rc.bottom, st->dibDC, 0, 0, NX, NY, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
}

void startTimer(HWND hwnd, MeterState* st) {
    if (!st->hidden && !st->timerOn) {
        SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
        st->timerOn = true;
    }
}
void stopTimer(HWND hwnd, MeterState* st) {
    if (st->timerOn) {
        KillTimer(hwnd, kTimerId);
        st->timerOn = false;
    }
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
            render(hwnd, st);
            return 0;
        case WM_TIMER:
            if (wParam == kTimerId) {
                step(st);
                InvalidateRect(hwnd, nullptr, FALSE);
                if (st->target <= 0.0f && totalFluid(st->fluid) < 0.4f) stopTimer(hwnd, st);
            }
            return 0;
        case WM_CONTEXTMENU: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, MENU_HIDE,
                        st->hidden ? L"Show buffer fluid" : L"Hide buffer fluid");
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.x == -1 && pt.y == -1) {  // keyboard menu
                RECT rc;
                GetWindowRect(hwnd, &rc);
                pt = {rc.left + 8, rc.top + 8};
            }
            const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd,
                                           nullptr);
            DestroyMenu(menu);
            if (cmd == MENU_HIDE) {
                const bool nowHidden = !st->hidden;
                bufferMeterSetHidden(hwnd, nowHidden);
                if (st->onHidden) st->onHidden(nowHidden);
            }
            return 0;
        }
        case WM_NCDESTROY:
            stopTimer(hwnd, st);
            if (st->dibDC) DeleteDC(st->dibDC);
            if (st->dib) DeleteObject(st->dib);
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

void bufferMeterSetHealth(HWND meter, int percent) {
    MeterState* st = stateOf(meter);
    if (!st) return;
    st->target = std::clamp(percent, 0, 100) / 100.0f;
    if (!st->hidden) startTimer(meter, st);
}

void bufferMeterSetHidden(HWND meter, bool hidden) {
    MeterState* st = stateOf(meter);
    if (!st || st->hidden == hidden) return;
    st->hidden = hidden;
    if (hidden) stopTimer(meter, st);
    else if (st->target > 0 || totalFluid(st->fluid) > 0.4f) startTimer(meter, st);
    InvalidateRect(meter, nullptr, FALSE);
}

void bufferMeterSetOnHiddenChanged(HWND meter, std::function<void(bool)> cb) {
    if (MeterState* st = stateOf(meter)) st->onHidden = std::move(cb);
}

void bufferMeterSetDpi(HWND meter, UINT dpi) {
    if (MeterState* st = stateOf(meter)) {
        st->dpi = dpi;
        InvalidateRect(meter, nullptr, FALSE);
    }
}

}  // namespace rabbitears
