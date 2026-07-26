// SPDX-License-Identifier: GPL-3.0-or-later
//
// DeadLinkSweep — the background half of the dead-link checker.
//
// Probes channel stream URLs off the UI thread and records dead_status, so "Hide unavailable"
// becomes something the app can actually populate instead of a filter over data nothing writes.
// The verdict logic — which is where the danger is — lives in common/core/DeadLinkCheck.h and is
// unit-tested; this file is only the plumbing around it.
//
// Graduated out of beta in 0.2.15. What makes that safe is that a sweep is USER-TRIGGERED (Settings
// ▸ Channels ▸ "Check for dead links…") and now UNDOABLE — "Clear dead-link results" resets every
// channel's dead_status, so a wrong verdict is one menu click to reverse rather than a channel that
// silently stays hidden. The verdict logic itself is unchanged.
//
// Design constraints, all of them load-bearing:
//   • Nothing here runs, allocates a thread, or opens a socket until the user asks for a sweep.
//   • Its OWN sqlite3 connection. The app's handle is shared with the UI, the scheduler tick and
//     import writes; a long-running worker must not sit in the middle of that.
//   • BOUNDED per run (kMaxPerSweep) and sequential with a delay between probes. A 12,000-channel
//     library would otherwise be a multi-hour hammering of one provider — and an Xtream account
//     with a connection limit can get the user's actual PLAYBACK kicked. Resumable via the TTL.
//   • Cancellable, and joined at shutdown so the process cannot outlive the thread.
#pragma once

#include <windows.h>

namespace rabbitears {
namespace mw {

struct AppState;

// Kick off a sweep on a worker thread. No-op (returns false) if a sweep is already running or the
// DB isn't open. Progress + completion are reported to the UI thread.
bool startDeadLinkSweep(AppState* st);

// Ask a running sweep to stop at the next probe boundary. Safe to call when none is running.
void cancelDeadLinkSweep();

// True while a sweep is in flight (drives the menu item's enabled/checked state).
bool deadLinkSweepRunning();

// Join any running sweep. Called from WM_DESTROY — the worker touches its own DB handle, so it
// must be finished before the app tears down.
void shutdownDeadLinkSweep();

}  // namespace mw
}  // namespace rabbitears
