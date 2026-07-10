// SPDX-License-Identifier: GPL-3.0-or-later
// WakeScheduler — Windows Task Scheduler integration so a scheduled recording fires even
// when RabbitEars isn't running (Recording Phase 3).
//
// The in-app scheduler (core/RecordingScheduler + the ~30 s WM_TIMER tick) can only record
// while the app is open. This registers ONE Windows scheduled task that wakes the machine
// from sleep and launches RabbitEars a couple of minutes before the next pending recording;
// the app then starts, its tick sees the airing schedule, and records as usual.
//
// Scope + limits (deliberate):
//  - Wakes from SLEEP (S1-S3). It cannot wake from hibernate/full shutdown — that depends on
//    firmware wake timers the app doesn't control. Windows power settings can also disable
//    wake timers entirely; then the task simply runs at the next boot (StartWhenAvailable).
//  - Runs with the interactive user's token, so it needs that user to be logged on (locked is
//    fine). This keeps it elevation-free: registering the task requires no admin rights.
#pragma once

#include <windows.h>

namespace rabbitears {

// Seconds before a recording's start that the machine is woken. Generous: a cold start pays
// libVLC's plugin scan (~3 s native, up to ~10 s on an x64 install without the cache) before
// the scheduler tick can even run.
inline constexpr int kWakeLeadSeconds = 120;

// Create-or-update the wake task to fire at `fireAtUtc` (an ABSOLUTE unix time — the caller
// subtracts kWakeLeadSeconds and clamps it into the future, so an imminent recording can't
// register a past boundary that Task Scheduler would fire immediately). Returns false if the
// task could not be registered (already logged); the app still records normally while open.
bool syncWakeTask(long long fireAtUtc);

// Remove the wake task — call when no pending schedules remain, so we never wake a machine
// for nothing.
void clearWakeTask();

// While `on`, ask Windows not to sleep (the display may still turn off). Called on the UI
// thread whenever the set of active recorders becomes non-empty / empty: the execution state
// is per-thread, so it must always be the same (UI) thread.
void setRecordingKeepAwake(bool on);

}  // namespace rabbitears
