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
    // --restart marks a self-relaunch: it tells runApp to WAIT for the outgoing instance to release
    // the single-instance mutex instead of bouncing. No feature launches it today (the language
    // switch now applies live, no restart), but the facility is kept for any future self-restart.
    const bool restart = lpCmdLine && wcsstr(lpCmdLine, L"--restart") != nullptr;
    return rabbitears::runApp(hInst, nCmdShow, scheduledWake, restart);
}
