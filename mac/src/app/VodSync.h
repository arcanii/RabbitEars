// SPDX-License-Identifier: GPL-3.0-or-later
//
// VodSync — the Xtream VOD catalogue sync worker (macOS). The peer of Win32/ui/VodSync.{h,cpp}.
//
// WHAT IT IS. A user-triggered background pass that pulls a provider's whole movie catalogue over
// the Xtream `player_api.php` and upserts it into `channels` as `kind = Movie` rows, then retires
// the films the provider no longer lists. Every piece of the engine — credential parsing, the JSON
// reader, the row mapping, the DAO — already lives in `common/` and is already compiled into this
// app; this file is only the worker, the gate and the reporting around it.
//
// ⚠ IT OPENS ITS OWN `Database`. That deliberately breaks the app's "exactly one Database, main
// thread only" rule, and the reason is `retireMissingChannels()`: it stages its keep-set in a
// per-CONNECTION `TEMP TABLE` inside a transaction and is documented non-reentrant, so it cannot
// share the main connection with the 250 ms stats writer. The cost of that choice is real and is
// spelled out at the call sites: SQLite's `busy_timeout` BLOCKS, so a main-thread write issued
// while this worker holds its write transaction sleeps on the main queue. See the scheduler
// stand-down in MainWindowController -schedulerTick, which exists because of it.
//
// ⚠ CANCELLATION IS SOFT, AND IT IS NOT A STOP. `httpGet` has no cancellation handle (the mac
// implementation waits DISPATCH_TIME_FOREVER on its semaphore), so an in-flight body always
// finishes. Cancelling means: no FURTHER request is issued and nothing more is written. There is
// deliberately no join/shutdown entry point — waiting for this worker at quit would beachball
// termination for up to the full catalogue timeout.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/XtreamClient.h"
#include "db/Database.h"

namespace rabbitears {
namespace mac {

// Why a start attempt did not produce a worker. The refusal reasons that need app state (playback
// active, no Xtream playlist, DB closed) live in the CONTROLLER's refusal ladder, not here — this
// module only knows about itself, which is what keeps the running flag claimable last.
enum class VodSyncStart { Started, AlreadyRunning, DispatchFailed };

enum class VodSyncResult {
    Ok,
    Cancelled,
    NetworkError,
    AuthFailed,       // the panel answered, and said no (or the line is expired/banned)
    ParseError,
    EmptyCatalogue,   // every line answered with no usable movies AND nothing was committed
    DatabaseError,
};

enum class VodSyncPhase { Contacting, Fetching, Saving };

// The counts a completed run reports. `detail` is NOT localized — it is raw HTTP/JSON text for the
// log and the status tail, where a translated string would destroy the diagnostic.
struct VodSyncReport {
    VodSyncResult result = VodSyncResult::Ok;
    int  playlists = 0;    // lines that committed rows
    int  inserted  = 0;
    int  retired   = 0;
    int  unusable  = 0;    // items the provider listed that carried no id or no container
    bool retireRefused = false;
    std::wstring detail;
};

struct VodSyncTarget {
    long long    id = 0;   // playlist id
    std::wstring name;
    XtreamCreds  creds;
};

// Every ENABLED, URL-backed playlist whose stored URL carries Xtream credentials. A plain .m3u
// simply is not a target — that is not an error. MAIN THREAD ONLY: it reads the app's Database.
// Cheap (one URL parse per playlist, no network), so it is fine to call to decide menu presence.
std::vector<VodSyncTarget> xtreamTargets(Database& db);

struct VodSyncRequest {
    std::vector<VodSyncTarget> targets;  // resolved on the main thread by xtreamTargets()
    std::wstring dbPath;                 // Database::defaultDbPath(), resolved on the main thread
    // Both callbacks are invoked on the MAIN queue. onDone runs exactly once per started run.
    std::function<void(VodSyncPhase, int)> onProgress;
    std::function<void(VodSyncReport)>     onDone;
};

VodSyncStart startVodSync(VodSyncRequest req);
bool vodSyncRunning();
void cancelVodSync();  // soft — see the header note

}  // namespace mac
}  // namespace rabbitears
