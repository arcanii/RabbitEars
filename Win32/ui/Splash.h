// SPDX-License-Identifier: GPL-3.0-or-later
// A lightweight startup splash: a centered, per-pixel-alpha (layered) window
// showing the branded logo, put up before the main window's (slow) creation so
// the user sees something during libVLC init + DB load. Because it's a layered
// window painted via UpdateLayeredWindow, DWM keeps compositing it even while the
// UI thread is blocked in WM_CREATE. Requires GDI+ to be started first.
#pragma once

#include <windows.h>

namespace rabbitears {

HWND showSplash(HINSTANCE hInst);   // returns the splash HWND (or nullptr)
void closeSplash(HWND splash);      // destroys it

}  // namespace rabbitears
