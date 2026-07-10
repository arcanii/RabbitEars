// SPDX-License-Identifier: GPL-3.0-or-later
// PowerPolicy (Win32) — the OS probe behind the wake-timer preflight.
//
// WakeScheduler registers the task; this answers the question the task cannot: will Windows
// actually arm the RTC wake timer that fires it? It reads the active power plan's "Allow wake
// timers" setting for BOTH power sources, the source currently in use, and whether the machine
// can RTC-wake at all, then hands them to the pure core (common/core/PowerPolicy).
//
// The peer of this file on macOS would read IOPMrootDomain; nothing here belongs in common/.
#pragma once

#include <string>

#include "core/PowerPolicy.h"

namespace rabbitears {

// Probe Windows and decide. Every individual query that fails leaves its input at the permissive
// default, so a machine whose policy we cannot read is never nagged. Costs a registry read plus a
// power-capability call: fine on dialog open or a menu toggle — do NOT put it on the ~30 s tick.
WakeVerdict queryWakePolicy();

// The one user-facing line for a verdict, or an empty string when there is nothing to say.
std::wstring wakeVerdictText(const WakeVerdict& v);

}  // namespace rabbitears
