// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/EpgStore.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "db/Database.h"
#include "platform/Log.h"
#include "ui/MainWindowInternal.h"
#include "ui/Tr.h"

namespace rabbitears {
namespace mw {
namespace {

// Between two of the store's transactions, long enough for a UI-thread write that is waiting on the
// lock to get it: SQLite's busy handler sleeps in steps of up to 100 ms (and Windows rounds a sleep up
// to its ~15.6 ms tick), so a lock released and retaken at once would slip past a waiter every time.
constexpr auto kYieldBetweenTransactions = std::chrono::milliseconds(250);

std::thread                g_worker;
std::atomic<bool>          g_running{false};
std::atomic<bool>          g_cancel{false};
std::mutex                 g_resultMx;
std::unique_ptr<EpgResult> g_result;  // the finished store, until takeEpgStoreResult()

void postProgress(HWND hwnd, const std::wstring& line) {
    auto* s = new std::wstring(line);  // the UI thread shows it in the loading box, then frees it
    if (!PostMessageW(hwnd, WM_APP_EPG_PROGRESS, 0, reinterpret_cast<LPARAM>(s))) delete s;
}

void storeBody(HWND hwnd, const std::wstring& dbPath, EpgResult& res) {
    using Clock = std::chrono::steady_clock;
    auto msSince = [](Clock::time_point t0) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    };
    // A second connection: the app's own already migrated the schema (or could not — which a worker
    // must not retry, holding the write lock; Database::open's upgradeSchema).
    Database db;
    std::wstring err;
    if (!db.open(dbPath, &err, /*upgradeSchema=*/false)) {
        res.storeError = err.empty() ? std::wstring(L"cannot open the database") : err;
        diag::error(L"EPG store: cannot open its own DB connection: " + res.storeError);
        return;
    }
    // One transaction per playlist, then one for the search index — not one for everything: a
    // UI-thread write waits for whichever is running (busy_timeout, 5 s, then it is LOST), so each is
    // kept as short as it can be. Searches meanwhile stay correct: until the index's own commit, its
    // stamp no longer matches, which the app's connection notices (Database::programmeSearchState)
    // and answers with LIKE.
    bool first = true;
    const long long now = static_cast<long long>(time(nullptr));
    for (EpgFetch& f : res.fetches) {
        if (!f.error.empty()) continue;
        if (!first) std::this_thread::sleep_for(kYieldBetweenTransactions);
        if (g_cancel.load()) return;  // after the pause too: a cancel that came during it
        first = false;
        postProgress(hwnd, trf(i18n::StringId::LoadingSavingProgrammes,
                               {std::to_wstring(f.programmes.size()), f.name}));
        const Clock::time_point t0 = Clock::now();
        f.stored = db.bulkInsertProgrammes(f.playlistId, f.programmes, now);
        f.storeMs = msSince(t0);
        f.storeDone = true;
        // 0 with an error: nothing landed, the old guide kept — a failure, reported as one (0 with
        // none: a guide with no valid programmes, stored). More than 0 with an error: some rows failed.
        if (f.stored == 0 && !db.lastError().empty()) {
            f.error = db.lastError();
            diag::error(L"EPG store: nothing stored for \"" + f.name + L"\": " + f.error);
        } else if (!db.lastError().empty()) {
            diag::warn(L"EPG store: " + std::to_wstring(f.stored) + L" of " + std::to_wstring(f.programmes.size()) +
                       L" programmes stored for \"" + f.name + L"\"; the first failure: " + db.lastError());
        }
    }
    std::set<std::wstring> chans;  // distinct guide channels across the guides stored
    for (const EpgFetch& f : res.fetches)
        if (f.storeDone && f.error.empty())
            for (const auto& p : f.programmes) chans.insert(p.channelId);
    res.channels = chans.size();
    // Re-index ONCE, after every playlist's guide — a rebuild re-indexes the whole table, so doing it
    // per playlist would repeat it (EPG_SEARCH.md §3). Only when a store changed something: if every
    // one failed, the index still matches, and a rebuild would only hold the lock they failed to get.
    if (first || g_cancel.load() ||  // first: no store ran
        db.programmeSearchState() != Database::ProgrammeSearchState::NeedsRebuild)
        return;
    std::this_thread::sleep_for(kYieldBetweenTransactions);
    if (g_cancel.load()) return;
    postProgress(hwnd, tr(i18n::StringId::LoadingIndexingGuide));
    const Clock::time_point t0 = Clock::now();
    const bool indexed = db.rebuildProgrammeIndex();
    res.indexMs = msSince(t0);
    res.index = indexed ? EpgResult::Index::Rebuilt : EpgResult::Index::Failed;
    if (!indexed) res.indexError = db.lastError();
}

void storeThread(HWND hwnd, const std::wstring& dbPath, std::unique_ptr<EpgResult> res) {
    // An exception escaping a std::thread is std::terminate, and this worker holds every parsed
    // programme (tens of MB), so bad_alloc is a real outcome. The handler allocates nothing — it
    // could be running out of memory — and a transaction it interrupts has rolled back (~Tx).
    try {
        storeBody(hwnd, dbPath, *res);
    } catch (...) {
        res->aborted = true;
    }
    // The parsed programmes are stored (or not): the UI thread needs only the counts.
    for (EpgFetch& f : res->fetches) {
        f.parsed = f.programmes.size();
        std::vector<Programme>().swap(f.programmes);
    }
    if (res->aborted) {
        try {
            diag::error(L"EPG store: stopped by an exception");
        } catch (...) {
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_resultMx);
        g_result = std::move(res);
    }
    g_running = false;
    // Carries nothing, so this message strands no allocation if the window is already tearing down
    // (a progress line posted then is simply leaked at exit).
    PostMessageW(hwnd, WM_APP_EPG_STORED, 0, 0);
}

}  // namespace

void startEpgStore(HWND hwnd, std::unique_ptr<EpgResult> res) {
    if (g_worker.joinable()) g_worker.join();  // the previous store, long since finished
    g_cancel = false;
    g_running = true;
    const std::wstring dbPath = Database::defaultDbPath();
    // The worker takes ownership only once it exists: std::thread's constructor would destroy a
    // moved-in unique_ptr if creating the thread threw, and the fallback below still needs `res`.
    EpgResult* raw = res.get();
    try {
        g_worker = std::thread([hwnd, dbPath, raw]() { storeThread(hwnd, dbPath, std::unique_ptr<EpgResult>(raw)); });
        res.release();
    } catch (...) {
        // Practically unreachable. The store then runs HERE, on the UI thread — frozen as it was
        // before this worker existed, and without its step names: those arrive after it returns.
        diag::error(L"EPG store: could not start the worker thread — storing on the UI thread");
        storeThread(hwnd, dbPath, std::move(res));
    }
}

std::unique_ptr<EpgResult> takeEpgStoreResult() {
    std::lock_guard<std::mutex> lock(g_resultMx);
    return std::move(g_result);
}

bool epgStoreRunning() { return g_running.load(); }

void cancelEpgStore() { g_cancel = true; }

void shutdownEpgStore() {
    g_cancel = true;
    if (g_worker.joinable()) g_worker.join();
}

}  // namespace mw
}  // namespace rabbitears
