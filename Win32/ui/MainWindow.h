// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

namespace rabbitears {

// Create the main window and run the message loop. Returns the exit code.
// `scheduledWake` == launched by the Windows Scheduled Task that wakes this PC for a
// recording (see platform/WakeScheduler): the launch is unattended, so it must not steal
// focus and must not block on the post-update Terms re-prompt.
int runApp(HINSTANCE hInst, int nCmdShow, bool scheduledWake = false, bool restart = false);

}  // namespace rabbitears
