// SPDX-License-Identifier: GPL-3.0-or-later
// BufferMeter — a fun, physical buffering visualizer: a little tank of liquid
// simulated with a 2D Navier-Stokes "stable fluids" solver. The fill level tracks
// the stream's health (full = healthy buffer); as the stream gets chunky/low the
// liquid drains. It sloshes and flows while data is arriving. Can be hidden
// (right-click) since motion can be distracting.
#pragma once

#include <functional>

#include <windows.h>

namespace rabbitears {

void registerBufferMeterClass(HINSTANCE hInst);
HWND createBufferMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi);

// Buffer health 0..100 -> target liquid level (and flow liveliness).
void bufferMeterSetHealth(HWND meter, int percent);
// Hide/show the visualizer; while hidden the sim is paused (no CPU).
void bufferMeterSetHidden(HWND meter, bool hidden);
// Notified when the user toggles hide via the right-click menu (for persistence).
void bufferMeterSetOnHiddenChanged(HWND meter, std::function<void(bool)> cb);
void bufferMeterSetDpi(HWND meter, UINT dpi);

}  // namespace rabbitears
