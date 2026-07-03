// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars — Win32 GUI entry point. Trivial: all real work is in runApp().
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ui/MainWindow.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    return rabbitears::runApp(hInst, nCmdShow);
}
