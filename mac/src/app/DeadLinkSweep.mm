// SPDX-License-Identifier: GPL-3.0-or-later
//
// See DeadLinkSweep.h. Like VodSync.mm, this file holds NO Objective-C objects — pure C++ + GCD +
// std::function — which is why it is deliberately ABSENT from the -fobjc-arc list in
// mac/CMakeLists.txt. Keep it that way: the moment an NSString or an NSAlert appears in here, that
// whole question has to be asked again.
//
// The behaviour is a faithful port of Win32/ui/DeadLinkSweep.cpp. Where a comment there explains
// WHY a guard exists, the guard is reproduced here with the same reasoning rather than re-derived.

#import <dispatch/dispatch.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <utility>
#include <vector>

#include "DeadLinkSweep.h"

#include "core/DeadLinkCheck.h"
#include "core/Http.h"
#include "db/Database.h"
#include "platform/Log.h"

namespace rabbitears {
namespace mac {
namespace {

// Budget per run. The sweep RESUMES: needsProbe() skips anything checked inside the 24 h TTL, so
// a second run continues where this one stopped instead of re-probing the same head of the list.
constexpr int kMaxPerSweep = 250;
constexpr int kProbeTimeoutMs = 8000;
constexpr int kPauseBetweenProbesMs = 120;

std::atomic<bool> g_running{false};
std::atomic<bool> g_cancel{false};

void runSweep(const std::wstring& dbPath,
              const std::function<void(int, int)>& progress,
              DeadLinkReport& rep) {
    Database db;
    std::wstring err;
    if (!db.open(dbPath, &err)) {
        diag::error(L"dead-link sweep: cannot open its own DB connection: " + err);
        rep.dbFailed = true;
        return;
    }

    const long long now = static_cast<long long>(time(nullptr));
    std::vector<Channel> todo;
    for (const Channel& c : db.allChannels()) {
        if (c.streamUrl.empty()) continue;
        // LIVE channels only. Once a VOD sync has run, `channels` holds several times more movies
        // than live channels, all interleaved by the same ordering — so without this the budget
        // gets spent on films and the feature the user actually asked for silently stops covering
        // their TV list. It is also right on its own terms: a vanished film is retired by the
        // catalogue sync, which costs two requests instead of tens of thousands.
        if (c.isVod()) continue;
        if (!needsProbe(c.lastCheckedAt, now)) continue;   // still fresh — this is what resumes
        todo.push_back(c);
        if (static_cast<int>(todo.size()) >= kMaxPerSweep) break;
    }
    diag::info(L"dead-link sweep: " + std::to_wstring(todo.size()) + L" channel(s) to probe");
    if (todo.empty()) return;

    // Collect verdicts FIRST, write NOTHING yet. The whole batch has to be judged before any of it
    // is trusted — that is the guard that stops a dropped network from marking the library dead.
    struct Verdict { long long id; ProbeOutcome outcome; };
    std::vector<Verdict> results;
    results.reserve(todo.size());

    for (size_t i = 0; i < todo.size(); ++i) {
        if (g_cancel.load()) {
            rep.cancelled = true;
            diag::info(L"dead-link sweep: cancelled after " + std::to_wstring(i) + L" probe(s)");
            break;
        }
        int status = 0;
        const ProbeTransport tr = httpProbe(todo[i].streamUrl, status, kProbeTimeoutMs);
        const ProbeOutcome out = classifyProbe(tr, status);
        switch (out) {
            case ProbeOutcome::Alive:        ++rep.alive; break;
            case ProbeOutcome::Dead:         ++rep.dead; break;
            case ProbeOutcome::Inconclusive: ++rep.inconclusive; break;
        }
        results.push_back({todo[i].id, out});
        ++rep.probed;
        diag::debug(L"probe: " + todo[i].name + L"  http=" + std::to_wstring(status) + L"  -> " +
                    (out == ProbeOutcome::Alive  ? L"alive"
                     : out == ProbeOutcome::Dead ? L"DEAD"
                                                 : L"inconclusive"));
        if (i + 1 < todo.size()) {
            if (progress) progress(static_cast<int>(i + 1), static_cast<int>(todo.size()));
            // Pace the provider. Also the only place a cancel can take effect promptly, since the
            // probe itself has no cancellation handle.
            std::this_thread::sleep_for(std::chrono::milliseconds(kPauseBetweenProbesMs));
        }
    }

    // THE GUARD. If too little of the batch actually reached a server, the fault is far more likely
    // to be at our end than at every provider's at once — so throw the whole thing away. This is
    // the single most important rule in the feature: a dead network fails every probe identically,
    // and a naive checker would read that as "the entire library is gone".
    if (!sweepIsTrustworthy(rep.alive, rep.dead, rep.inconclusive)) {
        rep.trustworthy = false;
        diag::warn(L"dead-link sweep DISCARDED as untrustworthy (alive=" +
                   std::to_wstring(rep.alive) + L" dead=" + std::to_wstring(rep.dead) +
                   L" inconclusive=" + std::to_wstring(rep.inconclusive) +
                   L") — network problem, not dead channels");
        return;
    }

    for (const Verdict& v : results) {
        // Only a definite verdict is recorded. An Inconclusive probe deliberately leaves BOTH the
        // status and the timestamp alone, so it is retried on the next run rather than cached as
        // "checked, no idea" for a day.
        if (v.outcome == ProbeOutcome::Inconclusive) continue;
        db.setDeadStatus(v.id, v.outcome == ProbeOutcome::Dead ? DeadStatus::Dead : DeadStatus::Alive,
                         now);
        ++rep.written;
    }
    diag::info(L"dead-link sweep done: alive=" + std::to_wstring(rep.alive) + L" dead=" +
               std::to_wstring(rep.dead) + L" inconclusive=" + std::to_wstring(rep.inconclusive) +
               L" written=" + std::to_wstring(rep.written));
}

}  // namespace

DeadLinkStart startDeadLinkSweep(DeadLinkRequest req) {
    // Claimed LAST of all the checks (the controller's refusal ladder runs first), so no refusal
    // path can leave the flag set and wedge the menu item at "Checking…" for the session.
    if (g_running.exchange(true)) return DeadLinkStart::AlreadyRunning;
    g_cancel = false;

    // Copied into the block: the worker must not read anything the main thread still owns.
    const std::wstring dbPath = req.dbPath;
    auto onProgress = req.onProgress;
    auto onDone = req.onDone;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        DeadLinkReport rep;
        // Progress hops to the main queue; the worker never touches UI state itself.
        auto progress = [onProgress](int done, int total) {
            if (!onProgress) return;
            dispatch_async(dispatch_get_main_queue(), ^{ onProgress(done, total); });
        };
        runSweep(dbPath, progress, rep);

        // The single exit. Drop the flag BEFORE handing the report to the main queue: the
        // completion re-enables the menu item, and a completion that ran while the flag was still
        // set would leave "Checking…" showing over a finished run.
        g_running = false;
        if (onDone) dispatch_async(dispatch_get_main_queue(), ^{ onDone(rep); });
    });
    return DeadLinkStart::Started;
}

void cancelDeadLinkSweep() { g_cancel = true; }

bool deadLinkSweepRunning() { return g_running.load(); }

}  // namespace mac
}  // namespace rabbitears
