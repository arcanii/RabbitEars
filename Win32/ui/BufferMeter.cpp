// SPDX-License-Identifier: GPL-3.0-or-later
// A 2D Navier-Stokes "stable fluids" solver (Jos Stam) driving a little tank of
// liquid whose level tracks stream health. The motion is *honest*, fed from real
// libVLC media stats: data pours in at the TOP CENTRE and leaves through a drain in
// the FLOOR, and the pour rate + circulation speed + shimmer + wave energy all track
// the stream's actual throughput (demux bytes/s) — a stalled stream's pour stops and
// its surface goes still — while real packet corruption/loss/dropped frames drive
// turbulence + violent splashes. A depleted buffer also turns turbulent. The plunging
// inflow drives the circulation a real tank develops: down the middle, out along the
// floor, up the walls, back inward at the surface. Rendered as a blocky LED
// dot-matrix: the fluid field is quantized onto a grid of small lit squares (foam
// crest + a shimmer that falls with the flow), and a healthy stream rests ~half-full
// so there's visible drain headroom. Right-click hides it. Tunables are grouped below.
#include "ui/BufferMeter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "ui/Theme.h"
#include "ui/Tr.h"

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReBufferMeter";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 33;  // ~30 fps
constexpr int MENU_HIDE = 1;

// Fluid grid (interior NX x NY, +1-cell border).
constexpr int NX = 96, NY = 28;
constexpr int GW = NX + 2, GH = NY + 2;
inline int IX(int i, int j) { return i + GW * j; }

// LED dot-matrix render: each cell is a small lit square with a dark gap around
// it. Sizes are 96-dpi design values (DPI-scaled at draw time).
constexpr int LED_PITCH = 3;  // cell + gap — matches the mini-meters' cell size (was 5)
constexpr int LED_GAP   = 1;  // dark gap between cells
// The grid shows only the bottom kVisibleFill fraction of the simulated tank, so a
// healthy stream (rests at NORMAL_FILL) reaches near the top with a little splash
// headroom instead of leaving dead unlit rows above. Lower = tighter crop.
constexpr float kVisibleFill = 0.68f;

// ---- tunables --------------------------------------------------------------
constexpr int   ITER      = 8;
constexpr float SIM_DT    = 0.12f;
constexpr float VISC      = 8e-6f;
constexpr float DIFF      = 1e-6f;
constexpr float RELAX     = 0.6f;    // soft level pull (surface band exempt)
constexpr float GRAV      = 12.0f;   // density-scaled gravity
constexpr float BUOY      = 14.0f;   // hydrostatic restoring force -> waves
constexpr float VORT      = 0.18f;   // vorticity confinement (swirls)
constexpr float WAVE_AMP  = 7.0f;    // travelling-wave impulse
constexpr float SPLASH_VY = -22.0f;  // upward droplet ejection (v<0 = up)
constexpr float POUR_VY   = 18.0f;   // downward pour-in velocity at the top-centre mouth (v>0 = down)
constexpr float POUR_MASS = 0.55f;   // density laid down per drop — THE knob for pour strength
constexpr float POUR_HALF = 5.0f;    // half-width of the pour mouth (cells; ~4 LED columns)
constexpr float VMAX      = 26.0f;   // velocity clamp (stability at dt=0.12)
constexpr float NORMAL_FILL = 0.5f;  // a healthy (100%) stream rests ~half-full

constexpr float kPi = 3.14159265f;
// The tank's centreline, and the render's EXACT mirror line. Not "about the middle":
// renderLedBits maps LED column c to fi = (c+0.5)/cols*NX and sampleField adds 0.5, so
// ci(c) + ci(cols-1-c) == NX+1 for *every* value of cols. A field symmetric about
// (NX+1)/2 therefore renders symmetric at 38, 36 and 35 columns alike, with no
// odd/even artefact and no DPI-dependent skew. Do not round this to 48.
constexpr float kCx = (NX + 1) * 0.5f;  // 48.5

// ---- the fountain: pour at the top centre, drain in the floor ---------------
// A top-centre inflow into a CLOSED box develops one roll per half: down the middle,
// out along the floor, up both walls, back inward at the surface. That topology is not
// a choice — project() drives divergence to zero everywhere, so the field has to close,
// and a convergent floor current under a centre downdraft is exactly the bottom-centre
// sink it cannot resolve. Prescribed from a streamfunction (divergence-free by
// construction) and applied as a nudge, which doubles as the only real damping in this
// sim: VISC = 8e-6 gives lin_solve a = 2.6e-3, i.e. nothing.
constexpr float ROLL_V     = 4.0f;   // peak centre downdraft (cells/s) — crosses the tank in ~3.5s
constexpr float ROLL_UMAX  = 14.0f;  // cap on the much faster horizontal legs (cells/s)
constexpr float ROLL_K     = 0.20f;  // per-frame pull toward the roll (tau = 5 frames = 0.17s,
                                     // matching the `flow` easing so both respond together)
constexpr float ROLL_HMIN  = 3.0f;   // divide-by-zero guard on the water-column depth
// The floor drain, expressed as a dish in the RESTING density rather than as a sink.
// DEEPER, never paler: the render's body shade saturates at d = 1.24 and its lit/foam
// thresholds sit at 0.36..0.64, so deepening can only ever darken — it cannot punch an
// unlit hole that reads as dead pixels, nor grow a white foam blob. See step (5).
constexpr float DRAIN_D    = 0.28f;  // how far the drain deepens the floor's resting density
constexpr float DRAIN_W    = 13.0f;  // drain half-width (cells; ~5 LED columns of core)
constexpr float DRAIN_PULL = 8.0f;   // extra relaxation authority inside the drain

int dpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

float smoothstep01(float e0, float e1, float v) {
    const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// The user-selectable fluid colour (see BufferMeter.h). An atomic because the Meters dialog writes
// it from the UI thread while this meter and its dialog preview read it during their own paints.
std::atomic<uint32_t>& fluidColorRef() {
    static std::atomic<uint32_t> c{static_cast<uint32_t>(kDefaultFluidColor)};
    return c;
}

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

// Advect a phase field along the flow — drives the render's directional shimmer. Seeded
// as a top->bottom ramp so the fronts FALL with the pour, then get sheared by the tank's
// circulation: fastest down the middle, spreading along the floor, riding back up the
// walls. It draws the roll, and on a 10-row panel a band of light moving down is a far
// stronger "the flow goes DOWN" cue than 2-5 drops in the top two rows. Two cycles over
// the tank height = ~7 LED rows per cycle, comfortably resolved at both 10 and 9 rows.
// `t` is the FLOW-SCALED shimmer clock, not sim time: this is now the panel's loudest
// motion, so it must nearly freeze when the stream does, or it keeps saying "data is
// moving" over a dead stream.
void advect_phase(Fluid& f, float dt, float t) {
    for (int j = 1; j <= NY; ++j) {
        const float base = (static_cast<float>(NY - j) / NY) * 2.0f * 6.2831853f;
        for (int i = 1; i <= NX; ++i)
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
    // Honest stream signals (eased toward the *Target each step for smoothness).
    float flow = 0.0f, flowTarget = 0.0f;        // 0..1 throughput -> current speed / wave energy
    float trouble = 0.0f, troubleTarget = 0.0f;  // 0..1 packet loss -> turbulence / splashes
    float shimT = 0.0f;                          // flow-scaled shimmer clock (see advect_phase)
    bool  hidden = false;
    bool  timerOn = false;
    UINT  dpi = 96;
    uint32_t rng = 0x1234abcdu;
    HBITMAP dib = nullptr;
    HDC     dibDC = nullptr;
    uint32_t* bits = nullptr;
    int     dibW = 0, dibH = 0;  // current DIB size (client px); recreated on resize
    wchar_t metrics[40] = L"";   // compact throughput/delay readout drawn over the grid
    HFONT   metricsFont = nullptr;
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

// Fixed functions of the column index: the roll streamfunction's horizontal terms and the
// drain's floor profile. Nothing here depends on anything that changes at runtime, so the
// roll costs no per-frame trig across the grid — only two calls per row for the vertical
// term. Same function-local-static idiom as fluidColorRef().
struct ColLut { float rs[GW], rc[GW], dw[GW]; };
const ColLut& colLut() {
    static const ColLut t = [] {
        ColLut c{};
        for (int i = 0; i < GW; ++i) {
            const float sx = (static_cast<float>(i) - kCx) / kCx;  // -1 .. +1 at the ghost cells
            c.rs[i] = std::sin(kPi * sx);
            c.rc[i] = std::cos(kPi * sx);
            const float r = std::fabs(static_cast<float>(i) - kCx) / DRAIN_W;
            c.dw[i] = (r < 1.0f) ? (1.0f - r) * (1.0f - r) : 0.0f;
        }
        return c;
    }();
    return t;
}

// How much the drain DEEPENS the floor's resting density on row j — zero everywhere but
// the bottom ~4 grid rows, and zero unless there is real water standing over it. Multiply
// by colLut().dw[i] for the per-cell amount.
//
// Used by BOTH step (2) (as the hydrostatic rest state) and step (5) (as the relaxation
// target), and they MUST be the same field. With a flat rest state the buoyancy spring
// spends every frame trying to undo the dish, which is a sustained vertical force parked
// on a solid floor — the drain would grow a permanent standing jet instead of reading as
// a drain. Sharing one function is what stops the two from drifting apart.
//
// `open` gates it on throughput, and the dist term is Torricelli: suction tracks the head
// of water standing over the floor, so a nearly-empty tank barely drains and never has its
// last two LED rows eaten by its own drain (fully off below ~19% health).
inline float drainRowAmp(int j, float surf, float open) {
    const float dist = (j + 0.5f) - surf;
    if (dist <= 2.5f) return 0.0f;  // a full cell outside step (5)'s +-1.5 surface band
    const float vprof = smoothstep01(static_cast<float>(NY) - 4.5f, static_cast<float>(NY),
                                     static_cast<float>(j));
    return DRAIN_D * open * vprof * smoothstep01(2.5f, 5.0f, dist);
}
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
    // Ease the honest stream signals toward their latest targets (stats arrive
    // ~4x/s; easing keeps the surface smooth between samples). Flow is sustained;
    // trouble is bursty, so its target self-decays unless a new loss refreshes it.
    st->flow += (st->flowTarget - st->flow) * 0.18f;
    st->troubleTarget *= 0.90f;
    st->trouble += (st->troubleTarget - st->trouble) * 0.30f;
    const float flow = std::clamp(st->flow, 0.0f, 1.0f);
    // "chunkiness": a badly depleted buffer (well below the healthy resting level)
    // turns turbulent. Combined with real packet loss -> `struggle` drives thrash.
    const float lowThresh = NORMAL_FILL * 0.5f;
    const float chunk = (fill > 0.05f) ? std::clamp((lowThresh - fill) / lowThresh, 0.0f, 1.0f) : 0.0f;
    const float struggle = std::clamp(std::max(chunk, st->trouble), 0.0f, 1.0f);
    // The shimmer clock runs on THROUGHPUT, not on wall time (see advect_phase). The 0.15
    // baseline keeps it from looking switched off rather than stalled.
    st->shimT += SIM_DT * (0.15f + 0.85f * flow);
    // The pour thread wanders slowly: a perfectly symmetric plunging jet is an unstable
    // equilibrium in real life, and a rigid centred column can park between two LED sample
    // points and strobe. +-2.5 cells = +-1 LED column, ~2.5s per cycle, scaled by throughput
    // so a barely-alive stream's thread is dead still.
    const float sway = std::sin(st->phase * 0.7f) * 2.5f * flow;
    const float drainOpen = 0.35f + 0.65f * flow;  // the drain works harder when data moves
    const ColLut& lut = colLut();

    // (1) TANK CIRCULATION. The top-centre plunge drives the roll pair a real tank
    // develops: DOWN the middle, OUT along the floor, UP both walls, back INWARD at the
    // surface. Prescribed from a streamfunction psi = -sin(pi*sx)*sin(pi*sy) — zero on all
    // four walls and on the free surface — so u = dpsi/dj, v = -dpsi/di is divergence-free
    // by construction and satisfies the same no-through conditions set_bnd enforces.
    // project() therefore passes it through instead of cancelling it, which is exactly what
    // it would do to a prescribed sink.
    //
    // Applied as a NUDGE toward the target, not as an impulse. That is a strict contraction
    // (x <- (1-k)x + k*T can never exceed max(|x|,|T|)) and it is the first genuine damping
    // this sim has ever had; the old step (1) added up to 4.32/frame to u with no restoring
    // term at all, so the right-hand jet simply pinned at the VMAX clamp and stayed there.
    const float H = std::max(NY + 0.5f - surf, ROLL_HMIN);  // water column depth, cells
    const float rollDepth = std::clamp((H - 3.0f) / 6.0f, 0.0f, 1.0f);
    if (rollDepth > 0.0f) {
        const float aspect = kCx / H;  // the box is ~3.5x wider than deep -> fast floor legs
        // Scale the WHOLE streamfunction (both components together) to cap the horizontal
        // legs and to calm a shallow puddle. Scaling psi keeps div == 0 exactly; capping
        // `aspect` on its own would NOT, and that is a real trap — it silently breaks the
        // single property this entire approach is built on.
        const float Vc = std::min(ROLL_V * (0.18f + 0.82f * flow), ROLL_UMAX / aspect) * rollDepth;
        for (int j = 1; j <= NY; ++j) {
            const float dist = (j + 0.5f) - surf;
            if (dist <= 1.0f) continue;  // dry rows + the wave/splash band: left untouched
            const float gate = smoothstep01(1.0f, 3.5f, dist);
            const float sy = std::clamp(dist / H, 0.0f, 1.0f);
            const float sss = std::sin(kPi * sy), cs = std::cos(kPi * sy);
            const float k = ROLL_K * gate;
            const float au = -Vc * aspect * cs, av = Vc * sss;
            for (int i = 1; i <= NX; ++i) {
                f.u[IX(i, j)] += (au * lut.rs[i] - f.u[IX(i, j)]) * k;
                f.v[IX(i, j)] += (av * lut.rc[i] - f.v[IX(i, j)]) * k;
            }
        }
    }
    // (1b) POUR-IN: data falls from the TOP CENTRE (scaled by throughput) onto the pool
    // below — reads as the buffer being filled from above. Origin is the top of the
    // *visible* tank. The second, lighter deposit one row down is not decoration: LED row 0
    // samples grid row ~10.4, so a drop written only at topJ = 9 lands in a row NO LED
    // reads and is invisible until it has already fallen a third of the way (a real defect
    // in the old right-edge pour too). Column picks are triangular — two uniforms summed,
    // sd 2.0 cells — so the thread is dense in the middle and feathers at the edges instead
    // of reading as a slab; each drop also wets its two neighbours, putting the wetted span
    // at 13 cells (~5 LED columns) against a ~3-cell aliasing floor (the render samples
    // every 2.53 cells, so a 1-cell column would strobe). lround, not a cast: truncation
    // would bias the mean to 48.0 and put the thread half a cell off the mirror line.
    if (flow > 0.02f && fill > 0.05f) {
        const int topJ = std::clamp(static_cast<int>((1.0f - kVisibleFill) * NY) + 1, 1, NY);
        const int j2 = std::min(topJ + 1, NY);
        const int drops = 2 + static_cast<int>(flow * 3.0f);
        for (int c = 0; c < drops; ++c) {
            const float tri = frand(st->rng) + frand(st->rng) - 1.0f;  // triangular, -1..1
            const int pi = std::clamp(
                static_cast<int>(std::lround(kCx + sway + tri * POUR_HALF)), 1, NX);
            const float pw = 0.35f + 0.65f * flow;
            for (int k = -1; k <= 1; ++k) {
                const int ii = std::clamp(pi + k, 1, NX);
                const float wk = (k == 0) ? 1.0f : 0.5f;
                f.d[IX(ii, topJ)] = std::min(1.4f, f.d[IX(ii, topJ)] + POUR_MASS * pw * wk);
                f.d[IX(ii, j2)]   = std::min(1.4f, f.d[IX(ii, j2)] + POUR_MASS * 0.55f * pw * wk);
            }
            f.v[IX(pi, topJ)] += POUR_VY * pw;                     // downward (v>0 = down)
            f.v[IX(pi, j2)]   += POUR_VY * 0.6f * pw;
            f.u[IX(pi, topJ)] += (frand(st->rng) - 0.5f) * 4.0f;   // slight horizontal scatter
        }
    }
    // (2) density-scaled gravity + hydrostatic buoyancy -> a real wavy surface. The resting
    // density carries the drain's dish (step (5)) because the spring and the level
    // controller must agree on what "at rest" means — see drainRowAmp's note.
    //
    // Note gravity uses min(d, 1): the drain is DENSER than the bulk, so that existing cap
    // means it gets exactly the same gravity as everything else and there is NO buoyancy
    // differential at all. A drain built the other way — thinning the fluid — would sit at
    // d ~ 0.3 and receive only 0.43/frame against the bulk's 1.44, i.e. a sustained ~1.0
    // relative UPDRAFT parked on the floor. That is the second independent reason this
    // drain deepens rather than thins.
    for (int j = 1; j <= NY; ++j) {
        const float restDepth = (j + 0.5f) - surf;
        const float base = std::clamp(restDepth, 0.0f, 1.0f);
        const float dvj = drainRowAmp(j, surf, drainOpen);
        for (int i = 1; i <= NX; ++i) {
            const float d = f.d[IX(i, j)];
            if (d <= 0.02f) continue;
            f.v[IX(i, j)] += GRAV * SIM_DT * std::min(d, 1.0f);
            const float want = base + dvj * lut.dw[i];
            f.v[IX(i, j)] -= BUOY * SIM_DT * (want - d) * 0.5f;
        }
    }
    // (3) travelling surface waves — crests radiate OUTWARD from where the pour lands —
    // + noisy ripples. The minus sign is what reverses them: holding the phase constant now
    // needs the distance from the impact to GROW with time. The impact point carries the
    // pour's sway so the pattern is not a rigid mirror-symmetric chevron. Wave energy
    // tracks throughput too: a stalled stream's surface goes calm.
    if (fill > 0.03f) {
        const float waveScale = 0.25f + 0.75f * flow;
        const int sj = std::clamp(static_cast<int>(surf), 1, NY);
        for (int i = 1; i <= NX; ++i) {
            const float ph = st->phase * 3.2f -
                             std::fabs(static_cast<float>(i) - (kCx + sway)) * 0.55f;
            const float wv = std::sin(ph) + 0.4f * std::sin(ph * 2.13f + 1.7f);
            for (int dj = -1; dj <= 1; ++dj) {
                const int j = std::clamp(sj + dj, 1, NY);
                const float env = 1.0f - std::min(1.0f, std::abs(dj) * 0.5f);
                f.v[IX(i, j)] += WAVE_AMP * SIM_DT * wv * env * waveScale;
            }
        }
        for (int g = 0; g < 4; ++g) {
            const int gi = 1 + static_cast<int>(frand(st->rng) * NX);
            const int gj = std::clamp(sj + static_cast<int>((frand(st->rng) - 0.5f) * 3), 1, NY);
            // Spray scatters outward from the impact. Same bias magnitude the old leftward
            // drift carried (mean 1.2), re-aimed rather than removed, so the surface keeps
            // exactly today's energy budget.
            const float bias = (static_cast<float>(gi) < kCx) ? -1.2f : 1.2f;
            addVel(f, gi, gj, ((frand(st->rng) - 0.5f) * 6.0f + bias) * waveScale,
                   (frand(st->rng) - 0.5f) * 6.0f * waveScale);
        }
    }
    // (4) splashes: droplets where the falling data LANDS, under the top-centre thread and
    //     following its sway — scaled by throughput, so a stalled stream stops splashing;
    //     violent bursts anywhere when the stream struggles (depleted buffer or real packet
    //     loss/corruption). Those bursts stay uniformly random in i on purpose: against an
    //     otherwise symmetric tank, "the symmetry broke" is now a legible trouble tell that
    //     the old right-to-left drift could never give.
    if (fill > 0.05f && flow > 0.02f && frand(st->rng) < 0.6f * flow) {
        const int sj = std::clamp(static_cast<int>(surf), 1, NY);
        const int si = std::clamp(
            static_cast<int>(std::lround(kCx + sway + (frand(st->rng) - 0.5f) * 6.0f)), 1, NX);
        splash(f, st->rng, si, sj, (0.4f + 0.6f * fill) * flow);
    }
    if (struggle > 0.0f && fill > 0.03f) {
        const int bursts = 1 + static_cast<int>(struggle * 5.0f);
        const int sj = std::clamp(static_cast<int>(surf), 1, NY);
        for (int b = 0; b < bursts; ++b) {
            const int bi = 1 + static_cast<int>(frand(st->rng) * NX);
            const int bj = std::clamp(sj + static_cast<int>((frand(st->rng) - 0.6f) * 4), 1, NY);
            splash(f, st->rng, bi, bj, 0.5f + struggle);
        }
    }
    // (5) SOFT level relaxation — firm far from the surface, almost none within a ~1.5-cell
    // band of it, so the level is stable but waves survive. THE FLOOR DRAIN LIVES HERE, and
    // that is the whole answer to "how does a visible drain coexist with the level control".
    //
    // It is not a second force added beside this controller — it is a reshaping of this
    // controller's OWN target. The fixed point of d += dt*rate*(tgt - d) is d == tgt by
    // definition, so the steady state simply IS the dished floor. There are not two
    // authorities, therefore nothing to fight and no standstill to reach.
    //
    // The level is untouched, structurally: `surf` still comes straight from buffer health,
    // the waterline is the d~0.5 crossing that `surf` imposes (it is never computed from
    // mass — this simulator manufactures density below the line and destroys it above,
    // unconditionally, every frame), and drainRowAmp() returns 0 for dist <= 2.5, a full
    // cell outside this loop's +-1.5 surface band and >= 10 cells below the waterline at
    // healthy fill. It also self-disables below ~19% health, so a dying buffer's last two
    // LED rows are never eaten by their own drain.
    //
    // The dish DEEPENS (raises d), the one direction with no threshold anywhere near it:
    // renderLedBits saturates its depth shade at d = 1.24 and puts lit/foam at 0.36..0.64,
    // so a deeper drain can only get darker — it can never punch an unlit hole that reads
    // as dead pixels, nor drift into the foam band and glow white. dens_step clamps d to
    // [0, 1.4] and the peak target is 1.26, so there is headroom either way.
    for (int j = 1; j <= NY; ++j) {
        const float dist = (j + 0.5f) - surf;
        float tgt, rate;
        if (dist > 1.5f) { tgt = 1.0f; rate = RELAX; }
        else if (dist < -1.5f) { tgt = 0.0f; rate = RELAX * 1.5f; }
        else { tgt = 0.5f; rate = RELAX * 0.05f; }
        const float dvj = drainRowAmp(j, surf, drainOpen);
        if (dvj <= 0.0f) {
            for (int i = 1; i <= NX; ++i) f.d0[IX(i, j)] = rate * (tgt - f.d[IX(i, j)]);
        } else {
            for (int i = 1; i <= NX; ++i) {
                // Deepen the target AND pull harder toward it: the plunge keeps sweeping
                // ordinary fluid across the floor, and only extra authority holds the dish
                // against that. Worst case rate = RELAX*(1 + 8*0.259) = 1.84, a per-frame
                // gain of 0.221 — 4.5x inside the explicit-relaxation limit.
                const float a = dvj * lut.dw[i];
                f.d0[IX(i, j)] = rate * (1.0f + DRAIN_PULL * a) * ((tgt + a) - f.d[IX(i, j)]);
            }
        }
    }
    // (6) stability clamp (essential at dt=0.12 on this grid).
    const int n = GW * GH;
    for (int k = 0; k < n; ++k) {
        f.u[k] = std::clamp(f.u[k], -VMAX, VMAX);
        f.v[k] = std::clamp(f.v[k], -VMAX, VMAX);
    }

    vel_step(f, SIM_DT);
    dens_step(f, SIM_DT);
    advect_phase(f, SIM_DT, st->shimT);
}

// ---- rendering (LED dot-matrix + foam crest + falling shimmer) --------------

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

// Quantize the fluid field onto an LED grid: one small lit square per cell,
// coloured by the density (depth-shaded body, whitish foam at the surface, a
// specular shimmer that falls with the flow). Dry cells show a faint "off" dot so
// the matrix reads even above the waterline; gaps stay the panel background.
void renderLedBits(MeterState* st, const Theme& th, int W, int H) {
    const Fluid& f = st->fluid;
    const int bgR = GetRValue(th.windowBg), bgG = GetGValue(th.windowBg), bgB = GetBValue(th.windowBg);
    const float coR = GetRValue(th.accent), coG = GetGValue(th.accent);
    // Unlit LED: a faint neutral dot (bg nudged toward the border colour).
    const int offR = bgR + (GetRValue(th.border) - bgR) * 6 / 10;
    const int offG = bgG + (GetGValue(th.border) - bgG) * 6 / 10;
    const int offB = bgB + (GetBValue(th.border) - bgB) * 6 / 10;
    const uint32_t bg = packRGB(bgR, bgG, bgB);
    const uint32_t off = packRGB(offR, offG, offB);

    // Flood the panel with background; the gaps between LEDs keep this colour.
    const int total = W * H;
    for (int k = 0; k < total; ++k) st->bits[k] = bg;

    // LED geometry in device pixels; grid centred in the panel.
    const int gap = std::max(1, dpx(st->dpi, LED_GAP));
    const int pitch = std::max(gap + 2, dpx(st->dpi, LED_PITCH));
    const int cellPx = pitch - gap;
    const int cols = std::max(1, (W + gap) / pitch);
    const int rows = std::max(1, (H + gap) / pitch);
    const int ox = (W - (cols * pitch - gap)) / 2;
    const int oy = (H - (rows * pitch - gap)) / 2;

    const float SURF = 0.5f;
    // Map rows onto the bottom kVisibleFill of the tank, dropping the never-lit
    // zone above the healthy waterline + splash headroom.
    const float loJ = (1.0f - kVisibleFill) * NY;
    for (int row = 0; row < rows; ++row) {
        const float fj = loJ + (row + 0.5f) / rows * (NY - loJ);
        for (int col = 0; col < cols; ++col) {
            const float fi = (col + 0.5f) / cols * NX;
            const float d = sampleField(f.d.data(), fi, fj);
            const float lit = smoothstep01(SURF - 0.14f, SURF + 0.14f, d);

            uint32_t cell;
            if (lit <= 0.02f) {
                cell = off;
            } else {
                const float depth = std::clamp((d - SURF) * 1.35f, 0.0f, 1.0f);
                // Body colour = the user's surface colour, shaded darker with depth. The three
                // channels darken at DIFFERENT rates on purpose: red fastest, blue slowest. That is
                // Beer-Lambert — water absorbs long wavelengths first, which is why deep water goes
                // blue — and it is also exactly what the previous hardcoded ramp did
                // (190,158,244 -> 70,42,116), so the default look is reproduced byte-for-byte while
                // an arbitrary user colour still deepens plausibly instead of just going grey.
                const float kR = 0.368f, kG = 0.266f, kB = 0.475f;  // survival at full depth
                const COLORREF fc = static_cast<COLORREF>(
                    fluidColorRef().load(std::memory_order_relaxed));
                const float sR = static_cast<float>(GetRValue(fc));
                const float sG = static_cast<float>(GetGValue(fc));
                const float sB = static_cast<float>(GetBValue(fc));
                float lr = sR * (1.0f - depth * (1.0f - kR));
                float lg = sG * (1.0f - depth * (1.0f - kG));
                float lb = sB * (1.0f - depth * (1.0f - kB));
                const float band = 4.0f * lit * (1.0f - lit);  // peaks at the surface row
                const float foam = band * band;
                lr += (255 - lr) * foam * 0.80f + (coR - lr) * foam * 0.20f;
                lg += (240 - lg) * foam * 0.80f + (coG - lg) * foam * 0.15f;
                lb += (255 - lb) * foam * 0.80f;
                const float ph = sampleField(f.phase.data(), fi, fj);
                const float uu = sampleField(f.u.data(), fi, fj);
                const float vv2 = sampleField(f.v.data(), fi, fj);
                float shim = 0.5f + 0.5f * std::sin(ph);
                shim = shim * shim * shim;
                // The current runs down the middle, out along the floor and back up the
                // walls, so no single signed axis describes it any more — the old gate on
                // leftward `u` would light the left-hand floor and the right-hand wall and
                // nothing else. Gate on flow SPEED and let the advected phase carry the
                // direction (that is what the phase field is for). Saturating at 8 cells/s
                // makes brightness itself an honest throughput readout: the floor legs reach
                // ~14 at full throughput and ~2.5 when stalled — strictly more honest than
                // the old gate, which stayed lit on the baseline drift over a dead stream.
                // It also leaves the drain's throat, where the streamfunction puts BOTH
                // components at ~0, as a still dark spot in fast bright water.
                const float flowGate = std::clamp(std::sqrt(uu * uu + vv2 * vv2) * 0.125f,
                                                  0.0f, 1.0f);
                // 0.45 (was 0.30) off the surface band: `band` is zero in the body, and the
                // body is exactly where the descending shimmer now lives. At 0.30 the one
                // cue that says "the flow goes DOWN" was invisible everywhere it matters.
                const float spec = shim * (0.25f + 0.75f * flowGate) * (0.45f + 0.55f * band);
                lr += (255 - lr) * spec * 0.55f;
                lg += (250 - lg) * spec * 0.55f;
                lb += (255 - lb) * spec * 0.55f;
                const int r = static_cast<int>(offR + (lr - offR) * lit);
                const int g = static_cast<int>(offG + (lg - offG) * lit);
                const int b = static_cast<int>(offB + (lb - offB) * lit);
                cell = packRGB(r, g, b);
            }

            const int x0 = ox + col * pitch, y0 = oy + row * pitch;
            for (int yy = 0; yy < cellPx; ++yy) {
                const int py = y0 + yy;
                if (py < 0 || py >= H) continue;
                uint32_t* dst = st->bits + py * W + x0;
                for (int xx = 0; xx < cellPx; ++xx) {
                    const int px = x0 + xx;
                    if (px >= 0 && px < W) dst[xx] = cell;
                }
            }
        }
    }
}

void ensureDib(HWND hwnd, MeterState* st, int W, int H) {
    if (st->dib && st->dibW == W && st->dibH == H) return;
    if (!st->dibDC) {
        HDC hdc = GetDC(hwnd);
        st->dibDC = CreateCompatibleDC(hdc);
        ReleaseDC(hwnd, hdc);
    }
    if (st->dib) {
        DeleteObject(st->dib);
        st->dib = nullptr;
        st->bits = nullptr;
    }
    st->dibW = W;
    st->dibH = H;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    st->dib = CreateDIBSection(st->dibDC, &bmi, DIB_RGB_COLORS,
                               reinterpret_cast<void**>(&st->bits), nullptr, 0);
    if (st->dib) SelectObject(st->dibDC, st->dib);
}

// Overlay a small throughput readout (e.g. "12.4 Mb/s") in the top-right, with a
// 1px shadow so it stays legible over lit LEDs. Font is cached (DPI-scaled).
void drawMetrics(HDC hdc, MeterState* st, const Theme& th, int W, int H) {
    if (!st->metricsFont)
        st->metricsFont = themeFont(FontRole::Body, st->dpi, 11, FW_SEMIBOLD);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, st->metricsFont));
    const int oldMode = SetBkMode(hdc, TRANSPARENT);
    RECT tr{0, dpx(st->dpi, 1), W - dpx(st->dpi, 4), H};
    RECT sh = tr;
    OffsetRect(&sh, dpx(st->dpi, 1), dpx(st->dpi, 1));
    SetTextColor(hdc, RGB(0, 0, 0));
    DrawTextW(hdc, st->metrics, -1, &sh, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
    SetTextColor(hdc, th.textPrimary);
    DrawTextW(hdc, st->metrics, -1, &tr, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
    SetBkMode(hdc, oldMode);
    SelectObject(hdc, oldFont);
}

void render(HWND hwnd, MeterState* st) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const Theme& th = currentTheme();
    if (st->hidden) {
        FillRect(hdc, &rc, themeBrush(th.windowBg));
        RECT line{0, rc.bottom - dpx(st->dpi, 2), rc.right, rc.bottom};
        FillRect(hdc, &line, themeBrush(th.border));
        EndPaint(hwnd, &ps);
        return;
    }
    if (W > 0 && H > 0) {
        ensureDib(hwnd, st, W, H);
        if (st->bits) {
            renderLedBits(st, th, W, H);
            BitBlt(hdc, 0, 0, W, H, st->dibDC, 0, 0, SRCCOPY);
            if (st->metrics[0]) drawMetrics(hdc, st, th, W, H);
        }
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
                        st->hidden ? tr(i18n::StringId::BufferMeterShow).c_str()
                                   : tr(i18n::StringId::BufferMeterHide).c_str());
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
            if (st->metricsFont) DeleteObject(st->metricsFont);
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
    st->target = std::clamp(percent, 0, 100) / 100.0f * NORMAL_FILL;
    if (percent <= 0) {  // stopped / ended / error: no stream, so no flow / metrics
        st->flowTarget = 0.0f;
        st->troubleTarget = 0.0f;
        st->metrics[0] = L'\0';
    }
    if (!st->hidden) startTimer(meter, st);
}

void bufferMeterSetFlow(HWND meter, float flowRate, float trouble) {
    MeterState* st = stateOf(meter);
    if (!st) return;
    st->flowTarget = std::clamp(flowRate, 0.0f, 1.0f);
    // Latch the strongest recent trouble; step() decays it between samples.
    st->troubleTarget = std::max(st->troubleTarget, std::clamp(trouble, 0.0f, 1.0f));
    if (!st->hidden) startTimer(meter, st);
}

void bufferMeterSetMetrics(HWND meter, const wchar_t* text) {
    MeterState* st = stateOf(meter);
    if (!st) return;
    lstrcpynW(st->metrics, text ? text : L"", 40);
    if (!st->hidden) InvalidateRect(meter, nullptr, FALSE);
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
        if (st->metricsFont) {  // recreate at the new DPI on next paint
            DeleteObject(st->metricsFont);
            st->metricsFont = nullptr;
        }
        InvalidateRect(meter, nullptr, FALSE);
    }
}

// Global, HWND-less (mirrors miniMeterSetGlass): there is one fluid colour for the app, and both
// the real meter and the Meters dialog's preview read it on their next paint. Storing only — the
// caller repaints, which is what lets the dialog preview the choice live on the REAL meter.
void bufferMeterSetFluidColor(COLORREF c) {
    fluidColorRef().store(static_cast<uint32_t>(c), std::memory_order_relaxed);
}
COLORREF bufferMeterFluidColor() {
    return static_cast<COLORREF>(fluidColorRef().load(std::memory_order_relaxed));
}

}  // namespace rabbitears
