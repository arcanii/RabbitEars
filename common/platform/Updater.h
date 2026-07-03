// SPDX-License-Identifier: GPL-3.0-or-later
// Thin wrapper over the auto-update engine — WinSparkle on Windows, Sparkle on
// macOS. Checks run against an EdDSA-signed appcast; the public key is set in the
// platform impl (Win32/platform/Updater.cpp, mac/platform/Updater.mm).
#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rabbitears {

#if defined(_WIN32)
// mainWnd receives WM_CLOSE when WinSparkle needs the app to quit to apply an
// update (the 0.1.6 shutdown-coordination fix).
void initUpdater(HWND mainWnd);  // configure + start background checks
#else
void initUpdater();              // macOS: Sparkle coordinates its own restart
#endif
void checkForUpdates();  // user-triggered "Check for Updates…"
void shutdownUpdater();  // on app exit

}  // namespace rabbitears
