// SPDX-License-Identifier: GPL-3.0-or-later
// BufferMeter — a fun, physical buffering visualizer: a little tank of liquid
// simulated with a 2D Navier-Stokes "stable fluids" solver and drawn as a blocky
// LED dot-matrix. The fill level tracks the stream's health (a healthy stream
// rests ~half-full); as the stream gets chunky/low the liquid drains. It sloshes
// and flows while data is arriving. Can be hidden (right-click) since motion can
// be distracting.
#pragma once

#include <functional>

#include <windows.h>

namespace rabbitears {

void registerBufferMeterClass(HINSTANCE hInst);
HWND createBufferMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi);

// Buffer health 0..100 -> target liquid level (a healthy stream rests ~half-full).
void bufferMeterSetHealth(HWND meter, int percent);
// Real-stream honesty: flowRate 0..1 = throughput (drives the right->left current
// speed + wave energy; 0 = stalled, current freezes), trouble 0..1 = packet
// corruption/loss pressure (drives turbulence + violent splashes). Fed from
// libVLC media stats; both ease smoothly so the surface responds without jitter.
void bufferMeterSetFlow(HWND meter, float flowRate, float trouble);
// Compact throughput readout drawn in the meter's top-right (e.g. L"12.4 Mb/s").
// Pass L"" to clear it. Cleared automatically when health drops to 0.
void bufferMeterSetMetrics(HWND meter, const wchar_t* text);
// Hide/show the visualizer; while hidden the sim is paused (no CPU).
void bufferMeterSetHidden(HWND meter, bool hidden);
// Notified when the user toggles hide via the right-click menu (for persistence).
void bufferMeterSetOnHiddenChanged(HWND meter, std::function<void(bool)> cb);
void bufferMeterSetDpi(HWND meter, UINT dpi);

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
