// SPDX-License-Identifier: GPL-3.0-or-later
// BufferMeter — a small segmented gradient bar that visualizes libVLC's buffering
// progress (0..100%) in real time, in the coral theme. Lit segments fade from a
// dark coral to the bright accent; unlit segments are dim; a light knob marks the
// current fill edge while actively buffering.
#pragma once

#include <windows.h>

namespace rabbitears {

void registerBufferMeterClass(HINSTANCE hInst);
HWND createBufferMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi);
void bufferMeterSet(HWND meter, int percent);  // 0..100
void bufferMeterSetDpi(HWND meter, UINT dpi);

}  // namespace rabbitears
