// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/DeadLinkSweep.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/DeadLinkCheck.h"
#include "core/FeatureFlags.h"
#include "core/Http.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "platform/Log.h"
#include "ui/MainWindowInternal.h"

namespace rabbitears {
namespace mw {
namespace {

// Bounded per run, and gentle. A full library sweep is explicitly NOT the goal: 12,000 sequential
// probes would take hours and read to a provider like a scraper — an Xtream account with a
// connection cap can respond by kicking the user's actual playback. The TTL makes runs resume, so
// repeated use converges on the whole library without any single run being abusive.
constexpr int kMaxPerSweep = 250;
constexpr int kProbeTimeoutMs = 8000;
constexpr int kPauseBetweenProbesMs = 120;

std::thread        g_worker;
std::atomic<bool>  g_running{false};
std::atomic<bool>  g_cancel{false};

void sweepThread(HWND hwnd, std::wstring dbPath) {
    // The worker's OWN connection — never the app's. The app handle is shared with the UI thread,
    // the 30s scheduler tick and import writes; parking a long sweep in the middle of that is how
    // you get SQLITE_BUSY (or, worse with our discard-the-result writers, a silently lost write).
    Database db;
    std::wstring err;
    if (!db.open(dbPath, &err)) {
        diag::error(L"dead-link sweep: cannot open its own DB connection: " + err);
        g_running = false;
        PostMessageW(hwnd, WM_APP_DEADLINK_DONE, 0, 0);
        return;
    }

    const long long now = static_cast<long long>(time(nullptr));
    std::vector<Channel> todo;
    for (const Channel& c : db.allChannels()) {
        if (c.streamUrl.empty()) continue;
        if (!needsProbe(c.lastCheckedAt, now)) continue;  // still fresh — this is what resumes
        todo.push_back(c);
        if (static_cast<int>(todo.size()) >= kMaxPerSweep) break;
    }
    diag::info(L"dead-link sweep: " + std::to_wstring(todo.size()) + L" channel(s) to probe");

    // Collect verdicts FIRST, write nothing yet. The whole batch has to be judged before any of it
    // is trusted (see sweepIsTrustworthy) — that is the guard that stops a dropped network from
    // marking the library dead.
    struct Verdict { long long id; ProbeOutcome outcome; };
    std::vector<Verdict> results;
    int alive = 0, dead = 0, inconclusive = 0;

    for (size_t i = 0; i < todo.size(); ++i) {
        if (g_cancel.load()) {
            diag::info(L"dead-link sweep: cancelled after " + std::to_wstring(i) + L" probe(s)");
            break;
        }
        int status = 0;
        const ProbeTransport tr = httpProbe(todo[i].streamUrl, status, kProbeTimeoutMs);
        const ProbeOutcome out = classifyProbe(tr, status);
        switch (out) {
            case ProbeOutcome::Alive: ++alive; break;
            case ProbeOutcome::Dead: ++dead; break;
            case ProbeOutcome::Inconclusive: ++inconclusive; break;
        }
        results.push_back({todo[i].id, out});
        diag::debug(L"probe: " + todo[i].name + L"  http=" + std::to_wstring(status) + L"  -> " +
                    (out == ProbeOutcome::Alive     ? L"alive"
                     : out == ProbeOutcome::Dead    ? L"DEAD"
                                                    : L"inconclusive"));
        if (i + 1 < todo.size()) {
            PostMessageW(hwnd, WM_APP_DEADLINK_PROGRESS, static_cast<WPARAM>(i + 1),
                         static_cast<LPARAM>(todo.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(kPauseBetweenProbesMs));
        }
    }

    // The guard. If too little of the batch actually reached a server, the fault is far more likely
    // to be at our end than at every provider's at once — so throw the whole thing away.
    if (!sweepIsTrustworthy(alive, dead, inconclusive)) {
        diag::warn(L"dead-link sweep DISCARDED as untrustworthy (alive=" + std::to_wstring(alive) +
                   L" dead=" + std::to_wstring(dead) + L" inconclusive=" +
                   std::to_wstring(inconclusive) + L") — network problem, not dead channels");
        g_running = false;
        PostMessageW(hwnd, WM_APP_DEADLINK_DONE, 0, 0);
        return;
    }

    int wrote = 0;
    for (const Verdict& v : results) {
        // Only a definite verdict is recorded. An Inconclusive probe deliberately leaves both the
        // status AND the timestamp alone, so it is retried next run rather than being cached as
        // "checked, no idea" for a day.
        if (v.outcome == ProbeOutcome::Inconclusive) continue;
        db.setDeadStatus(v.id, v.outcome == ProbeOutcome::Dead ? DeadStatus::Dead : DeadStatus::Alive,
                         now);
        ++wrote;
    }
    diag::info(L"dead-link sweep done: alive=" + std::to_wstring(alive) + L" dead=" +
               std::to_wstring(dead) + L" inconclusive=" + std::to_wstring(inconclusive) +
               L" written=" + std::to_wstring(wrote));
    g_running = false;
    PostMessageW(hwnd, WM_APP_DEADLINK_DONE, static_cast<WPARAM>(dead), static_cast<LPARAM>(wrote));
}

}  // namespace

bool startDeadLinkSweep(AppState* st) {
    if (!st || !st->db.isOpen()) return false;
    if (!betaEnabled(BetaFeature::DeadLinkChecker)) return false;  // inert unless opted in
    if (g_running.exchange(true)) return false;                    // one at a time
    g_cancel = false;
    if (g_worker.joinable()) g_worker.join();  // reap the previous, finished, worker
    g_worker = std::thread(sweepThread, st->hwnd, Database::defaultDbPath());
    return true;
}

void cancelDeadLinkSweep() { g_cancel = true; }

bool deadLinkSweepRunning() { return g_running.load(); }

void shutdownDeadLinkSweep() {
    g_cancel = true;
    if (g_worker.joinable()) g_worker.join();
}

}  // namespace mw
}  // namespace rabbitears
