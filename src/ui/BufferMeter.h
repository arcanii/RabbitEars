// SPDX-License-Identifier: GPL-3.0-or-later
// BufferMeter — a segmented "data flow" indicator: while a stream is active, small
// purple blocks stream from right to left across the track, representing data
// arriving. Idle = dim static segments.
#pragma once

#include <windows.h>

namespace rabbitears {

void registerBufferMeterClass(HINSTANCE hInst);
HWND createBufferMeter(HWND parent, HINSTANCE hInst, int id, UINT dpi);
void bufferMeterSetActive(HWND meter, bool active);  // animate while a stream is live
void bufferMeterSetDpi(HWND meter, UINT dpi);

}  // namespace rabbitears
