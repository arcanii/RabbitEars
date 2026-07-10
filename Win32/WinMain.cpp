// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars — Win32 GUI entry point. Trivial: all real work is in runApp().
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>

#include "ui/MainWindow.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    // The ONLY command-line flag: the Windows Scheduled Task registered by
    // platform/WakeScheduler passes --scheduled-wake when it wakes the machine for a
    // recording. It tells runApp this launch is unattended (don't steal focus, don't sit
    // at a modal Terms re-prompt). A plain substring test is enough — the flag is ours and
    // the app takes no other arguments.
    const bool scheduledWake = lpCmdLine && wcsstr(lpCmdLine, L"--scheduled-wake") != nullptr;
    return rabbitears::runApp(hInst, nCmdShow, scheduledWake);
}
