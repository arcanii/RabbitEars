// SPDX-License-Identifier: GPL-3.0-or-later
//
// GuideRefresh — Refresh Guide's heavy half, run on a worker: fetch every playlist's guide(s),
// merge, store, re-index. The mac peer of Win32's onRefreshGuide fetch worker + ui/EpgStore
// (MainWindowCommands.cpp, EpgStore.cpp). Pure C++ declarations, so mac/src/tools/selftest.cpp
// runs the real thing against a local HTTP server and a real database; MainWindowController calls
// it from a GCD worker and keeps only the rule pass and the results on the main thread.
//
//  1. Fetch — a playlist's guide link is split into its guides (core/GuideSources.h: an M3U
//     header's x-tvg-url may list several); each is downloaded with a running byte count
//     (platform/HttpProgress.h), gunzipped if needed and parsed with a running programme count,
//     then the guides are MERGED (mergeGuideSources: a channel's programmes come from the first
//     guide listing it). A guide that fails while others load is reported beside what loaded.
//  2. Store — on the worker's OWN connection (opened with upgradeSchema=false: the app's own
//     already migrated), one short transaction per playlist with a 250 ms pause between them, so
//     a main-thread write waiting on the lock gets it (busy_timeout is 5 s, then it is LOST — the
//     scheduler's are retried, ScheduleStatusWriter; most others are not).
//  3. Index — the TV Guide search's index is rebuilt ONCE, after every playlist (docs/EPG_SEARCH.md
//     §3), and only when a store changed something.
// `cancel` is checked between downloads and transactions (set at quit).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "models/Programme.h"

namespace rabbitears::mac {

struct GuideTarget {
    long long    playlistId = 0;
    std::wstring name;
    std::wstring url;  // the playlist's guide link as stored (may name several guides)
};

struct GuideFetch {
    long long    playlistId = 0;
    std::wstring name;
    std::wstring error;                      // empty on success
    std::vector<std::wstring> sourceErrors;  // "guide 2 of 3: …" for a guide that failed beside others
    std::vector<std::wstring> sourceLog;     // what the merge kept of each guide that loaded
    std::vector<Programme> programmes;       // freed once stored (see `parsed`)
    size_t       parsed = 0;                 // how many programmes it was handed
    int          stored = 0;
    bool         storeDone = false;
    long long    downloadMs = 0, parseMs = 0, storeMs = 0;
};

struct GuideRefreshResult {
    std::vector<GuideFetch> fetches;
    size_t       channels = 0;  // distinct guide channels across the stored guides
    std::wstring storeError;    // the worker's connection would not open
    enum class Index { NotRun, Rebuilt, Failed } index = Index::NotRun;
    long long    indexMs = 0;
    std::wstring indexError;
};

// A progress line for the user (already translated). `force` marks a step name, which must be
// shown; the running counts (a call per network chunk, per 1,000 programmes) are not forced, and
// the caller may drop them to rate-limit.
using GuideProgressFn = std::function<void(const std::wstring& line, bool force)>;

// Blocks for the whole refresh — call from a worker. Never throws (an exception mid-store is caught,
// logged and reported as storeError; a transaction it interrupts has rolled back).
GuideRefreshResult refreshGuides(const std::vector<GuideTarget>& targets, const std::wstring& dbPath,
                                 const GuideProgressFn& progress, const std::atomic<bool>& cancel);

}  // namespace rabbitears::mac
