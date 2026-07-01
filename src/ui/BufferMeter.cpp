// SPDX-License-Identifier: GPL-3.0-or-later
// A 2D Navier-Stokes "stable fluids" solver (Jos Stam) driving a little tank of
// liquid whose level tracks stream health. Data streams in on the RIGHT and
// drains LEFT; the surface waves and splashes (buoyancy + travelling waves +
// vorticity confinement), and a chunky/low stream turns turbulent and drains.
// Rendered as an iso-surface with a foam crest and a left-drifting shimmer.
// Right-click hides it (motion can distract). Tunables are grouped below.
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
constexpr UINT kTimerMs = 33;  // ~30 fps
constexpr int MENU_HIDE = 1;

// Fluid grid (interior NX x NY, +1-cell border) and 2x supersampled DIB.
constexpr int NX = 96, NY = 28;
constexpr int GW = NX + 2, GH = NY + 2;
inline int IX(int i, int j) { return i + GW * j; }
constexpr int RMUL = 2, RW = NX * RMUL, RH = NY * RMUL;

// ---- tunables --------------------------------------------------------------
constexpr int   ITER      = 8;
constexpr float SIM_DT    = 0.12f;
constexpr float VISC      = 8e-6f;
constexpr float DIFF      = 1e-6f;
constexpr float RELAX     = 0.6f;    // soft level pull (surface band exempt)
constexpr float GRAV      = 12.0f;   // density-scaled gravity
constexpr float BUOY      = 14.0f;   // hydrostatic restoring force -> waves
constexpr float VORT      = 0.18f;   // vorticity confinement (swirls)
constexpr float INFLOW_VX = -6.0f;   // baseline right->left current (cells/s, u<0 = left)
constexpr float WAVE_AMP  = 7.0f;    // travelling-wave impulse
constexpr float SPLASH_VY = -22.0f;  // upward droplet ejection (v<0 = up)
constexpr float VMAX      = 26.0f;   // velocity clamp (stability at dt=0.12)

int dpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

struct Fluid {
    std::vector<float> u, v, u0, v0, d, d0, curl, phase, phase0;
    Fluid() {
        const int n = GW * GH;
        u.assign(n, 0); v.assign(n, 0); u0.assign(n, 0); v0.assign(n, 0);
        d.assign(n, 0); d0.assign(n, 0); curl.assign(n, 0);
        phase.assign(n, 0); phase0.assign(n, 0);
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
            float x = std::clamp(i - dt * u[IX(i, j)], 0.5f, NX + 0.5f);
            float y = std::clamp(j - dt * v[IX(i, j)], 0.5f, NY + 0.5f);
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

// Vorticity confinement: re-inject the small-scale rotation that advection
// dissipates, so the fluid curls/ripples instead of going flat.
void vorticity_confine(float* u, float* v, float* curl, float eps, float dt) {
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i)
            curl[IX(i, j)] = 0.5f * (v[IX(i + 1, j)] - v[IX(i - 1, j)]) -
                             0.5f * (u[IX(i, j + 1)] - u[IX(i, j - 1)]);
    for (int i = 0; i <= NX + 1; ++i) { curl[IX(i, 0)] = 0; curl[IX(i, NY + 1)] = 0; }
    for (int j = 0; j <= NY + 1; ++j) { curl[IX(0, j)] = 0; curl[IX(NX + 1, j)] = 0; }
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
            float nx = 0.5f * (std::fabs(curl[IX(i + 1, j)]) - std::fabs(curl[IX(i - 1, j)]));
            float ny = 0.5f * (std::fabs(curl[IX(i, j + 1)]) - std::fabs(curl[IX(i, j - 1)]));
            const float len = std::sqrt(nx * nx + ny * ny) + 1e-5f;
            nx /= len;
            ny /= len;
            const float w = curl[IX(i, j)];
            u[IX(i, j)] += eps * dt * (ny * w);
            v[IX(i, j)] += eps * dt * (-nx * w);
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
    vorticity_confine(f.u.data(), f.v.data(), f.curl.data(), VORT, dt);
    project(f.u.data(), f.v.data(), f.u0.data(), f.v0.data());
}

void dens_step(Fluid& f, float dt) {
    const int n = GW * GH;
    for (int i = 0; i < n; ++i) f.d[i] += dt * f.d0[i];
    diffuse(0, f.d0.data(), f.d.data(), DIFF, dt);
    advect(0, f.d.data(), f.d0.data(), f.u.data(), f.v.data(), dt);
    for (int i = 0; i < n; ++i) f.d[i] = std::clamp(f.d[i], 0.0f, 1.4f);
}

// Advect a phase field along the flow (seeded on the right, drifts left) — drives
// the render's directional shimmer.
void advect_phase(Fluid& f, float dt, float t) {
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
            const float base = (static_cast<float>(i) / NX) * 6.2831853f;
            f.phase[IX(i, j)] = 0.92f * f.phase[IX(i, j)] + 0.08f * (base + 3.0f * t);
        }
    set_bnd(0, f.phase.data());
    advect(0, f.phase0.data(), f.phase.data(), f.u.data(), f.v.data(), dt);
    std::swap(f.phase, f.phase0);
    set_bnd(0, f.phase.data());
}

// ---- state -----------------------------------------------------------------

struct MeterState {
    Fluid fluid;
    float target = 0.0f;   // fill 0..1 (buffer health)
    float phase = 0.0f;    // sim time (wave + shimmer clock)
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
inline float waterlineJ(float fill) { return (1.0f - std::clamp(fill, 0.0f, 1.0f)) * NY + 0.5f; }
inline void addVel(Fluid& f, int i, int j, float du, float dv) {
    i = std::clamp(i, 1, NX);
    j = std::clamp(j, 1, NY);
    f.u[IX(i, j)] += du;
    f.v[IX(i, j)] += dv;
}
inline void splash(Fluid& f, uint32_t& rng, int i, int j, float power) {
    i = std::clamp(i, 1, NX);
    j = std::clamp(j, 1, NY);
    f.d[IX(i, j)] = std::min(1.4f, f.d[IX(i, j)] + 0.6f * power);
    f.v[IX(i, j)] += SPLASH_VY * power;
    f.u[IX(i, j)] += (frand(rng) - 0.5f) * 10.0f * power;
    if (j > 1) {
        f.d[IX(i, j - 1)] = std::min(1.4f, f.d[IX(i, j - 1)] + 0.35f * power);
        f.v[IX(i, j - 1)] += SPLASH_VY * 0.7f * power;
    }
}

void step(MeterState* st) {
    Fluid& f = st->fluid;
    const float fill = std::clamp(st->target, 0.0f, 1.0f);
    st->phase += SIM_DT;
    const float surf = waterlineJ(fill);
    // "chunkiness": a struggling (low but non-zero) stream turns turbulent.
    const float chunk = (fill > 0.05f) ? std::clamp((0.55f - fill) / 0.55f, 0.0f, 1.0f) : 0.0f;

    // (1) inflow current on the RIGHT + gentle global left drift (right -> left).
    if (fill > 0.02f) {
        for (int j = 1; j <= NY; ++j) {
            if (j + 0.5f < surf) continue;
            for (int i = NX - 8; i <= NX; ++i) {
                const float w = (i - (NX - 8)) / 8.0f;
                f.u[IX(i, j)] += INFLOW_VX * (0.4f + 0.6f * w) * SIM_DT * 6.0f;
            }
            for (int i = 1; i <= NX; ++i) f.u[IX(i, j)] += INFLOW_VX * 0.15f * SIM_DT * 6.0f;
        }
    }
    // (2) density-scaled gravity + hydrostatic buoyancy -> a real wavy surface.
    for (int j = 1; j <= NY; ++j)
        for (int i = 1; i <= NX; ++i) {
            const float d = f.d[IX(i, j)];
            if (d <= 0.02f) continue;
            f.v[IX(i, j)] += GRAV * SIM_DT * std::min(d, 1.0f);
            const float restDepth = (j + 0.5f) - surf;
            const float want = std::clamp(restDepth, 0.0f, 1.0f);
            f.v[IX(i, j)] -= BUOY * SIM_DT * (want - d) * 0.5f;
        }
    // (3) travelling surface waves (crests move right -> left) + noisy ripples.
    if (fill > 0.03f) {
        const int sj = std::clamp(static_cast<int>(surf), 1, NY);
        for (int i = 1; i <= NX; ++i) {
            const float ph = st->phase * 3.2f + i * 0.55f;
            const float wv = std::sin(ph) + 0.4f * std::sin(ph * 2.13f + 1.7f);
            for (int dj = -1; dj <= 1; ++dj) {
                const int j = std::clamp(sj + dj, 1, NY);
                const float env = 1.0f - std::min(1.0f, std::abs(dj) * 0.5f);
                f.v[IX(i, j)] += WAVE_AMP * SIM_DT * wv * env;
            }
        }
        for (int g = 0; g < 4; ++g) {
            const int gi = 1 + static_cast<int>(frand(st->rng) * NX);
            const int gj = std::clamp(sj + static_cast<int>((frand(st->rng) - 0.5f) * 3), 1, NY);
            addVel(f, gi, gj, (frand(st->rng) - 0.7f) * 6.0f, (frand(st->rng) - 0.5f) * 6.0f);
        }
    }
    // (4) splashes: steady where the stream lands on the right; violent when chunky.
    if (fill > 0.05f && frand(st->rng) < 0.6f) {
        const int sj = std::clamp(static_cast<int>(surf), 1, NY);
        splash(f, st->rng, NX - 1 - static_cast<int>(frand(st->rng) * 4), sj, 0.5f + 0.5f * fill);
    }
    if (chunk > 0.0f) {
        const int bursts = 1 + static_cast<int>(chunk * 4.0f);
        const int sj = std::clamp(static_cast<int>(surf), 1, NY);
        for (int b = 0; b < bursts; ++b) {
            const int bi = 1 + static_cast<int>(frand(st->rng) * NX);
            const int bj = std::clamp(sj + static_cast<int>((frand(st->rng) - 0.6f) * 4), 1, NY);
            splash(f, st->rng, bi, bj, 0.5f + chunk);
        }
    }
    // (5) SOFT level relaxation — firm far from the surface, almost none within a
    // ~1.5-cell band of it, so the level is stable but waves survive.
    for (int j = 1; j <= NY; ++j) {
        const float dist = (j + 0.5f) - surf;
        float tgt, rate;
        if (dist > 1.5f) { tgt = 1.0f; rate = RELAX; }
        else if (dist < -1.5f) { tgt = 0.0f; rate = RELAX * 1.5f; }
        else { tgt = 0.5f; rate = RELAX * 0.05f; }
        for (int i = 1; i <= NX; ++i) f.d0[IX(i, j)] = rate * (tgt - f.d[IX(i, j)]);
    }
    // (6) stability clamp (essential at dt=0.12 on this grid).
    const int n = GW * GH;
    for (int k = 0; k < n; ++k) {
        f.u[k] = std::clamp(f.u[k], -VMAX, VMAX);
        f.v[k] = std::clamp(f.v[k], -VMAX, VMAX);
    }

    vel_step(f, SIM_DT);
    dens_step(f, SIM_DT);
    advect_phase(f, SIM_DT, st->phase);
}

// ---- rendering (iso-surface + foam crest + left-drift shimmer) --------------

uint32_t packRGB(int r, int g, int b) {
    r = std::clamp(r, 0, 255); g = std::clamp(g, 0, 255); b = std::clamp(b, 0, 255);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}
float sampleField(const float* x, float fi, float fj) {
    const float ci = std::clamp(fi + 0.5f, 1.0f, static_cast<float>(NX));
    const float cj = std::clamp(fj + 0.5f, 1.0f, static_cast<float>(NY));
    const int i0 = static_cast<int>(ci), j0 = static_cast<int>(cj);
    const int i1 = std::min(i0 + 1, NX), j1 = std::min(j0 + 1, NY);
    const float sx = ci - i0, sy = cj - j0;
    const float a = x[IX(i0, j0)], b = x[IX(i1, j0)];
    const float c = x[IX(i0, j1)], dd = x[IX(i1, j1)];
    return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + dd * sx) * sy;
}
float smoothstep01(float e0, float e1, float v) {
    const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

void renderFluidBits(MeterState* st, const Theme& th) {
    const Fluid& f = st->fluid;
    const float bgR = GetRValue(th.windowBg), bgG = GetGValue(th.windowBg), bgB = GetBValue(th.windowBg);
    const float coR = GetRValue(th.accent), coG = GetGValue(th.accent);
    const float SURF = 0.5f, invRMUL = 1.0f / RMUL;
    for (int py = 0; py < RH; ++py) {
        const float fj = (py + 0.5f) * invRMUL;
        for (int px = 0; px < RW; ++px) {
            const float fi = (px + 0.5f) * invRMUL;
            const float d = sampleField(f.d.data(), fi, fj);
            const float gy = sampleField(f.d.data(), fi, fj + 0.6f) - sampleField(f.d.data(), fi, fj - 0.6f);
            const float aa = std::clamp(0.35f * std::fabs(gy) + 0.06f, 0.03f, 0.30f);
            const float cover = smoothstep01(SURF - aa, SURF + aa, d);
            if (cover <= 0.004f) {
                st->bits[py * RW + px] = packRGB(static_cast<int>(bgR), static_cast<int>(bgG),
                                                 static_cast<int>(bgB));
                continue;
            }
            const float depth = std::clamp((d - SURF) * 1.35f, 0.0f, 1.0f);
            float lr = 190 - depth * 120, lg = 158 - depth * 116, lb = 244 - depth * 128;
            const float band = 4.0f * cover * (1.0f - cover);
            const float foam = band * band;
            lr += (255 - lr) * foam * 0.85f + (coR - lr) * foam * 0.15f;
            lg += (240 - lg) * foam * 0.85f + (coG - lg) * foam * 0.10f;
            lb += (255 - lb) * foam * 0.85f;
            const float ph = sampleField(f.phase.data(), fi, fj);
            const float uu = sampleField(f.u.data(), fi, fj);
            float shim = 0.5f + 0.5f * std::sin(ph);
            shim = shim * shim * shim;
            const float leftGate = std::clamp(-uu * 0.10f, 0.0f, 1.0f);
            const float spec = shim * (0.25f + 0.75f * leftGate) * (0.30f + 0.70f * band);
            lr += (255 - lr) * spec * 0.55f;
            lg += (250 - lg) * spec * 0.55f;
            lb += (255 - lb) * spec * 0.55f;
            const int r = static_cast<int>(bgR + (lr - bgR) * cover);
            const int g = static_cast<int>(bgG + (lg - bgG) * cover);
            const int b = static_cast<int>(bgB + (lb - bgB) * cover);
            st->bits[py * RW + px] = packRGB(r, g, b);
        }
    }
}

void ensureDib(HWND hwnd, MeterState* st) {
    if (st->dib) return;
    HDC hdc = GetDC(hwnd);
    st->dibDC = CreateCompatibleDC(hdc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = RW;
    bmi.bmiHeader.biHeight = -RH;  // top-down
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
        RECT line{0, rc.bottom - dpx(st->dpi, 2), rc.right, rc.bottom};
        FillRect(hdc, &line, themeBrush(th.border));
        EndPaint(hwnd, &ps);
        return;
    }
    ensureDib(hwnd, st);
    if (st->bits) {
        renderFluidBits(st, th);
        SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, nullptr);
        StretchBlt(hdc, 0, 0, rc.right, rc.bottom, st->dibDC, 0, 0, RW, RH, SRCCOPY);
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
            if (pt.x == -1 && pt.y == -1) {
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
