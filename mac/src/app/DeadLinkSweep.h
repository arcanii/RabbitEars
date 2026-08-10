// SPDX-License-Identifier: GPL-3.0-or-later
//
// DeadLinkSweep — the background dead-link checker (macOS). The peer of
// Win32/ui/DeadLinkSweep.cpp, and a sibling of VodSync in shape: a GCD worker with its own
// Database connection, a soft cancel, and callbacks marshalled back to the main queue.
//
// The decision core is NOT here. `common/core/DeadLinkCheck.{h,cpp}` — classifyProbe,
// sweepIsTrustworthy, needsProbe — is shared, selftested, and already compiled into this app; this
// file is only the worker and the reporting around it. The dangerous part of this feature was never
// the probing, it is deciding what a probe MEANS: get it wrong and the checker marks thousands of
// working channels dead, and with "Hide unavailable" on, the user's library simply vanishes.
//
// ⚠ IT OPENS ITS OWN `Database`, for the same reason VodSync does: the app's connection is shared
// with the UI, the ~30 s scheduler tick and import writes, and parking a 250-probe sweep in the
// middle of that invites SQLITE_BUSY. The cost is the same too — SQLite's busy_timeout BLOCKS, so
// a main-thread write issued while this worker holds its write transaction sleeps on the main
// queue. The sweep's writes are deliberately a short burst at the very end, not a long transaction.
//
// ⚠ CANCELLATION IS SOFT. An in-flight probe always finishes (up to its timeout). Cancelling means
// no FURTHER probe is issued, and — because the trustworthiness gate is evaluated over whatever was
// collected — a cancelled sweep still writes the verdicts it had already earned, exactly as the
// Win32 worker does.
#pragma once

#include <functional>
#include <string>

namespace rabbitears {
namespace mac {

// Why a start attempt did not produce a worker. The refusals that need app state (a VOD sync
// running, the provider connection busy, the DB closed) live in the CONTROLLER's refusal ladder,
// which is what lets this module claim the running flag LAST.
enum class DeadLinkStart { Started, AlreadyRunning, DispatchFailed };

struct DeadLinkReport {
    int  probed = 0;         // probes actually issued
    int  alive = 0;
    int  dead = 0;
    int  inconclusive = 0;
    int  written = 0;        // rows whose dead_status was updated
    bool trustworthy = true; // false => the ENTIRE sweep was discarded, nothing written
    bool cancelled = false;
    bool dbFailed = false;   // the worker could not open its own connection
};

struct DeadLinkRequest {
    std::wstring dbPath;
    std::function<void(int done, int total)> onProgress;  // delivered on the MAIN queue
    std::function<void(DeadLinkReport)> onDone;           // delivered on the MAIN queue
};

// Claims the running flag last; call the controller's refusal ladder first.
DeadLinkStart startDeadLinkSweep(DeadLinkRequest req);

// Soft cancel — see the header note. Safe to call from any thread, and safe when nothing is
// running. The recording scheduler calls this: a due recording outranks a link check, exactly as
// it outranks a catalogue refresh.
void cancelDeadLinkSweep();

bool deadLinkSweepRunning();

}  // namespace mac
}  // namespace rabbitears
