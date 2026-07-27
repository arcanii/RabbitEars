// SPDX-License-Identifier: GPL-3.0-or-later
// BufferMeter — a fun, physical buffering visualizer: a little tank of liquid
// simulated with a 2D Navier-Stokes "stable fluids" solver and drawn as a blocky
// LED dot-matrix. Data pours in at the TOP CENTRE and leaves through a drain in the
// FLOOR. The fill level tracks the stream's health (a healthy stream rests
// ~half-full); as the stream gets chunky/low the level falls toward the drain. It
// sloshes and circulates while data is arriving. Can be hidden (right-click) since
// motion can be distracting.
#pragma once

#include <functional>

#include <windows.h>

namespace rabbitears {

void registerBufferMeterClass(HINSTANCE hInst);
HWND createBufferMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi);

// Buffer health 0..100 -> target liquid level (a healthy stream rests ~half-full).
void bufferMeterSetHealth(HWND meter, int percent);
// Real-stream honesty: flowRate 0..1 = throughput (drives the top-centre pour rate, the
// tank's circulation speed, the falling shimmer and the wave energy; 0 = stalled, the
// pour stops and the shimmer nearly freezes), trouble 0..1 = packet corruption/loss
// pressure (drives turbulence + violent splashes). Fed from libVLC media stats; both
// ease smoothly so the surface responds without jitter.
void bufferMeterSetFlow(HWND meter, float flowRate, float trouble);
// Compact throughput readout drawn in the meter's top-right (e.g. L"12.4 Mb/s").
// Pass L"" to clear it. Cleared automatically when health drops to 0.
void bufferMeterSetMetrics(HWND meter, const wchar_t* text);
// Hide/show the visualizer; while hidden the sim is paused (no CPU).
void bufferMeterSetHidden(HWND meter, bool hidden);
// Notified when the user toggles hide via the right-click menu (for persistence).
void bufferMeterSetOnHiddenChanged(HWND meter, std::function<void(bool)> cb);
void bufferMeterSetDpi(HWND meter, UINT dpi);

// ---- LED grid geometry -----------------------------------------------------
// Factored out of the renderer, and header-inline, for ONE reason: the renderer lives in an
// anonymous namespace inside a GUI translation unit that --selftest cannot link, and the
// property that matters here is not visual — it is that turning the glass overlay OFF costs
// the tank exactly ZERO dial pixels.
//
// This meter's grid is genuinely edge-to-edge (at 115x30 the top LED row starts at y=0), so
// unlike the mini-meters a bezel here really does eat a row and a column. That is acceptable
// only because it is GATED: `inset` is 0 whenever glass strength is 0, which is the default,
// and at inset == 0 every value below is bit-identical to the pre-glass renderer. A test can
// pin that; an eyeball on a 115x30 panel cannot.
struct BufferGrid {
    int cols = 1, rows = 1, ox = 0, oy = 0;
};
inline BufferGrid bufferGrid(int W, int H, int gap, int pitch, int inset) {
    BufferGrid g;
    const int gridW = (W - 2 * inset) > 1 ? (W - 2 * inset) : 1;
    const int gridH = (H - 2 * inset) > 1 ? (H - 2 * inset) : 1;
    g.cols = (gridW + gap) / pitch; if (g.cols < 1) g.cols = 1;
    g.rows = (gridH + gap) / pitch; if (g.rows < 1) g.rows = 1;
    g.ox = inset + (gridW - (g.cols * pitch - gap)) / 2;
    g.oy = inset + (gridH - (g.rows * pitch - gap)) / 2;
    return g;
}

// ---- fluid colour ----------------------------------------------------------
// The liquid's body colour, as seen at the SURFACE; depth shades it darker (see the note in
// renderLedBits on why the three channels darken at different rates). Foam and the specular
// highlight stay near-white on purpose — a crest is scattered light, not tinted liquid, and
// tinting them made the whole panel read as one flat colour.
//
// GLOBAL, exactly like the glass strength and for the same reason: this meter has no MeterConfig
// at all (the Meters dialog's "Data flow" row deliberately has no Look/palette/knobs, because the
// fluid look is internal), so there is nowhere per-meter to hang it. One app-wide value, one
// swatch, persisted under bufferFluidColorSettingKey(). Setting it only stores the value —
// the caller repaints.
constexpr COLORREF kDefaultFluidColor = RGB(190, 158, 244);
void     bufferMeterSetFluidColor(COLORREF c);
COLORREF bufferMeterFluidColor();
inline const char* bufferFluidColorSettingKey() { return "buffer_fluid_color"; }

}  // namespace rabbitears
