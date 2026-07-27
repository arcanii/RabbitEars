// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VodSync.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Http.h"
#include "core/XtreamClient.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "models/Playlist.h"
#include "platform/Log.h"
#include "ui/DeadLinkSweep.h"  // deadLinkSweepRunning() — the other provider-facing worker
#include "ui/MainWindowInternal.h"
#include "ui/VlcPlayer.h"

namespace rabbitears {
namespace mw {
namespace {

// Per-PHASE WinHTTP timeouts (resolve/connect/send/receive), not whole-transfer budgets — the
// receive timeout bounds the wait for the next chunk. The catalogue is ~13.5 MB over ~5 s on the
// measured provider, so a generous per-chunk window costs nothing when the line is healthy and
// still gives up on a line that has genuinely stalled.
constexpr int kAccountTimeoutMs = 15000;
constexpr int kCatalogueTimeoutMs = 30000;

// 🔴 The trust guard. A response can parse perfectly and still be worthless: if most of it was
// skipped for want of a stream id or a container extension, the keep-set we would hand
// retireMissingChannels() is missing films that are still in the catalogue — and retiring on that
// evidence deletes a library because a panel changed a field. So: below this share of usable
// items, INSERT but do not RETIRE. Adding a film we can play is safe; removing one is not.
//
// The same asymmetry the dead-link sweep encodes (a failure to connect is never a Dead verdict),
// applied to the one operation in this app that deletes rows in bulk.
constexpr int kMinUsablePercent = 50;

std::thread       g_worker;
std::atomic<bool> g_running{false};
std::atomic<bool> g_cancel{false};
std::mutex        g_reportMx;
VodSyncReport     g_report;

// One playlist to sync: its row id (retirement is scoped to it) and the credentials lifted out of
// its stored URL. Resolved on the UI thread so the worker never touches AppState.
struct SyncTarget {
    long long    id = 0;
    std::wstring name;
    XtreamCreds  creds;
};

void publish(const VodSyncReport& r) {
    std::lock_guard<std::mutex> lock(g_reportMx);
    g_report = r;
}

// Every ENABLED playlist whose stored URL carries Xtream credentials. Disabled playlists are
// skipped deliberately: their channels are excluded from every view, so syncing one would spend
// ~27 MB and the line's only connection on rows nobody can see.
std::vector<SyncTarget> xtreamTargets(AppState* st) {
    std::vector<SyncTarget> out;
    if (!st || !st->db.isOpen()) return out;
    for (const Playlist& p : st->db.listPlaylists()) {
        if (!p.enabled || !p.isUrl || p.sourceUrl.empty()) continue;
        SyncTarget t;
        if (!parseXtreamPlaylistUrl(p.sourceUrl, t.creds)) continue;  // a plain .m3u — not an error
        t.id = p.id;
        t.name = p.name;
        out.push_back(std::move(t));
    }
    return out;
}

// Finish: publish, drop the running flag, tell the UI. Every exit from syncThread goes through
// this — a path that forgets g_running would wedge the menu item as "Syncing…" forever.
void finish(HWND hwnd, VodSyncReport r) {
    publish(r);
    g_running = false;
    PostMessageW(hwnd, WM_APP_VOD_DONE, 0, 0);
}

void syncBody(HWND hwnd, const std::wstring& dbPath, const std::vector<SyncTarget>& targets,
              VodSyncReport& rep) {

    // 🔴 The worker's OWN connection, never AppState::db. retireMissingChannels() stages its
    // keep-set in a per-CONNECTION temp table inside a transaction and is documented as not
    // reentrant; sharing the app's handle with the UI thread while it runs is the one genuinely
    // unsafe thing to do with this database.
    Database db;
    std::wstring err;
    if (!db.open(dbPath, &err)) {
        diag::error(L"VOD sync: cannot open its own DB connection: " + err);
        rep.result = VodSyncResult::DatabaseError;
        rep.detail = err;
        return;
    }

    const long long now = static_cast<long long>(time(nullptr));
    bool anyEmpty = false;  // at least one line answered with no usable movies

    for (const SyncTarget& t : targets) {
        if (g_cancel.load()) { rep.result = VodSyncResult::Cancelled; break; }

        // ---- 1. Who are we? -------------------------------------------------
        // A panel answers BAD CREDENTIALS with HTTP 200 and auth:0, so "it responded" and "it let
        // us in" are different questions. Worth one ~1 KB request before pulling 27 MB: an expired
        // line otherwise surfaces as a JSON parse error on an error page, which reads as a bug in
        // us rather than a bill the user has to pay.
        PostMessageW(hwnd, WM_APP_VOD_PROGRESS, kVodPhaseContacting, 0);
        std::string body;
        if (!httpGet(xtreamApiUrl(t.creds), body, err, kAccountTimeoutMs)) {
            diag::error(L"VOD sync: account probe failed for \"" + t.name + L"\": " + err);
            rep.result = VodSyncResult::NetworkError;
            rep.detail = err;
            break;
        }
        XtreamAccount acct;
        if (!parseXtreamAccount(body, acct, &err)) {
            diag::error(L"VOD sync: account probe unparseable for \"" + t.name + L"\": " + err);
            rep.result = VodSyncResult::ParseError;
            rep.detail = err;
            break;
        }
        diag::info(L"VOD sync: \"" + t.name + L"\" auth=" + (acct.authOk ? L"1" : L"0") +
                   L" status=\"" + acct.status + L"\" max_connections=" +
                   std::to_wstring(acct.maxConnections) + L" exp_date=" +
                   std::to_wstring(acct.expiresAt));
        // authOk alone is not enough — it is true for a line that is authenticated and banned or
        // expired, which is exactly the case a user hits and needs told about plainly. An empty
        // status is not a verdict, so it does not refuse.
        if (!acct.authOk || (!acct.status.empty() && acct.status != L"Active")) {
            rep.result = VodSyncResult::AuthFailed;
            rep.detail = acct.status.empty() ? L"auth rejected" : acct.status;
            break;
        }
        if (g_cancel.load()) { rep.result = VodSyncResult::Cancelled; break; }

        // ---- 2. Two requests for the whole catalogue ------------------------
        // Not 67 per-category calls and emphatically not 43,599 get_vod_info calls: under
        // max_connections:1 the request COUNT is the cost, and get_vod_info has nothing in it for
        // a movie anyway (XTREAM_VOD.md §1 F2).
        PostMessageW(hwnd, WM_APP_VOD_PROGRESS, kVodPhaseFetching, 0);
        std::vector<XtreamCategory> cats;
        if (!httpGet(xtreamApiUrl(t.creds, L"get_vod_categories"), body, err,
                     kCatalogueTimeoutMs)) {
            diag::error(L"VOD sync: get_vod_categories failed: " + err);
            rep.result = VodSyncResult::NetworkError;
            rep.detail = err;
            break;
        }
        if (!parseXtreamCategories(body, cats, &err)) {
            // NOT survivable, though it reads like it should be. Categories look like a naming
            // convenience, but bulkInsertChannels writes `group_title=excluded.group_title`
            // UNCONDITIONALLY — so carrying on with an empty category list would rewrite all
            // 43,599 stored movies to the "Movies" fallback in one committed transaction and
            // flatten the entire Movies tree, on a transient parse hiccup, with no undo. A sync
            // that cannot name its categories has to stop.
            diag::error(L"VOD sync: get_vod_categories unparseable: " + err);
            rep.result = VodSyncResult::ParseError;
            rep.detail = err;
            break;
        }
        if (g_cancel.load()) { rep.result = VodSyncResult::Cancelled; break; }

        if (!httpGet(xtreamApiUrl(t.creds, L"get_vod_streams"), body, err, kCatalogueTimeoutMs)) {
            diag::error(L"VOD sync: get_vod_streams failed: " + err);
            rep.result = VodSyncResult::NetworkError;
            rep.detail = err;
            break;
        }
        XtreamVodResult res;
        if (!parseXtreamVodStreams(body, res, &err)) {
            // A truncated body dies HERE, on structure, which is why the guard below only has to
            // reason about a response that parsed.
            diag::error(L"VOD sync: get_vod_streams unparseable: " + err);
            rep.result = VodSyncResult::ParseError;
            rep.detail = err;
            break;
        }
        body.clear();
        body.shrink_to_fit();  // ~13.5 MB, and the row build below is not small either

        rep.unusable += res.skippedNoId + res.skippedNoExt;
        diag::info(L"VOD sync: \"" + t.name + L"\" " + std::to_wstring(res.movies.size()) +
                   L" usable of " + std::to_wstring(res.total) + L" (no id: " +
                   std::to_wstring(res.skippedNoId) + L", no extension: " +
                   std::to_wstring(res.skippedNoExt) + L"), " + std::to_wstring(cats.size()) +
                   L" categories");
        if (g_cancel.load()) { rep.result = VodSyncResult::Cancelled; break; }

        // ---- 3. Write ------------------------------------------------------
        // Past this point cancellation is ignored: bulkInsertChannels is one transaction, and
        // stopping between the insert and the retire would leave the library consistent anyway
        // (retirement is idempotent and the next sync redoes it).
        std::vector<ParsedChannel> rows =
            xtreamMoviesToChannels(t.creds, res.movies, cats, L"Movies");
        res.movies.clear();
        res.movies.shrink_to_fit();  // two 43,599-element vectors alive at once is the peak
        // `rows`, not res.movies, is what the counts below are about — it is both what gets
        // inserted AND what becomes the keep-set, so the trust guard has to reason about the
        // set that actually reaches the DELETE. (They agree today; making the guard depend on
        // the other one is how they quietly stop agreeing.)
        const long long kept = static_cast<long long>(rows.size());
        if (kept == 0) {
            // Nothing written and nothing retired for THIS line. An empty keep-set deletes nothing
            // by design, but we do not even get that far — "the provider answered with no movies"
            // is far more likely to be a provider hiccup than a catalogue that genuinely emptied.
            // `continue`, not `break`: one dud line must not stop the others being synced, and the
            // verdict is only promoted to EmptyCatalogue after the loop if NOTHING was committed.
            diag::warn(L"VOD sync: no usable movies for \"" + t.name + L"\" — nothing written");
            anyEmpty = true;
            continue;
        }
        PostMessageW(hwnd, WM_APP_VOD_PROGRESS, kVodPhaseSaving,
                     static_cast<LPARAM>(rows.size()));

        const int wrote = db.bulkInsertChannels(t.id, rows, now);
        rep.inserted += wrote;
        if (wrote == 0) {
            // Every row carries a non-empty constructed URL, so a zero here is the DB refusing to
            // write, not the catalogue being empty. Say so instead of reporting a clean sync.
            diag::error(L"VOD sync: bulkInsertChannels wrote 0 of " + std::to_wstring(rows.size()) +
                        L" rows for \"" + t.name + L"\"");
            rep.result = VodSyncResult::DatabaseError;
            rep.detail = L"bulk insert wrote nothing";
            break;
        }

        // ---- 4. Retire, but only on evidence we believe ---------------------
        // 64-bit arithmetic, not int: `total` is provider-controlled and only ever read back from
        // a JSON array's length, so the multiplication is the one place a hostile or broken panel
        // could push this past INT_MAX and flip the comparison — into deleting the library.
        const bool trusted =
            res.total > 0 && kept * 100 >= static_cast<long long>(res.total) * kMinUsablePercent;
        if (!trusted) {
            rep.retireRefused = true;
            diag::warn(L"VOD sync: RETIREMENT SKIPPED for \"" + t.name + L"\" — only " +
                       std::to_wstring(kept) + L" of " + std::to_wstring(res.total) +
                       L" items were usable, which is not evidence a film is gone");
        } else {
            std::vector<std::wstring> keep;
            keep.reserve(rows.size());
            // MOVED out of `rows`, not copied: the insert above is done with them, and a second
            // 43,599-element vector of URLs is ~10 MB duplicated at the sync's memory peak.
            for (ParsedChannel& p : rows) keep.push_back(std::move(p.streamUrl));
            rows.clear();
            rows.shrink_to_fit();
            const int gone =
                db.retireMissingChannels(t.id, static_cast<int>(Channel::Kind::Movie), keep);
            if (gone < 0) {
                // A rolled-back retirement, NOT "the provider dropped nothing". Reporting a clean
                // sync here is the failure Database.h warns about — it looks like it worked, and
                // the library silently never converges.
                diag::error(L"VOD sync: retirement FAILED and rolled back for \"" + t.name + L"\"");
                rep.result = VodSyncResult::DatabaseError;
                rep.detail = L"retirement rolled back";
                break;
            }
            rep.retired += gone;
            diag::info(L"VOD sync: retired " + std::to_wstring(gone) + L" movie(s) from \"" +
                       t.name + L"\"");
        }
        ++rep.playlists;
    }
    // Only now is "the provider returned nothing" the story of the run: if any line did commit
    // rows, a second line coming back empty is a detail, not the verdict.
    if (anyEmpty && rep.result == VodSyncResult::Ok && rep.inserted == 0)
        rep.result = VodSyncResult::EmptyCatalogue;
}

void syncThread(HWND hwnd, std::wstring dbPath, std::vector<SyncTarget> targets) {
    VodSyncReport rep;
    // The one catch-all in the app, and it earns its place: this worker allocates a ~13.5 MB
    // response plus two 43,599-element vectors, so bad_alloc is a real outcome rather than a
    // theoretical one — and an exception escaping a std::thread is std::terminate, i.e. the whole
    // app dies while the user is browsing. Whatever went wrong, the run is reported and the
    // running flag is cleared through the single exit below.
    try {
        syncBody(hwnd, dbPath, targets, rep);
    } catch (const std::exception& e) {
        diag::error(L"VOD sync: aborted by an exception");
        rep.result = VodSyncResult::DatabaseError;
        rep.detail = std::wstring(L"internal error: ") +
                     std::wstring(e.what(), e.what() + strlen(e.what()));
    } catch (...) {
        diag::error(L"VOD sync: aborted by an unknown exception");
        rep.result = VodSyncResult::DatabaseError;
        rep.detail = L"internal error";
    }
    // Every non-Ok outcome must carry SOMETHING: a run that committed work and then stopped is
    // reported with StatusVodSyncPartial, which interpolates the detail, and Cancelled /
    // EmptyCatalogue set none of their own — leaving an empty pair of parentheses on screen.
    if (rep.result != VodSyncResult::Ok && rep.detail.empty()) {
        rep.detail = rep.result == VodSyncResult::Cancelled       ? L"cancelled"
                     : rep.result == VodSyncResult::EmptyCatalogue ? L"no usable movies"
                                                                   : L"unknown error";
    }

    diag::info(L"VOD sync done: result=" + std::to_wstring(static_cast<int>(rep.result)) +
               L" playlists=" + std::to_wstring(rep.playlists) + L" inserted=" +
               std::to_wstring(rep.inserted) + L" retired=" + std::to_wstring(rep.retired) +
               L" unusable=" + std::to_wstring(rep.unusable) +
               (rep.retireRefused ? L" (retirement refused)" : L""));
    finish(hwnd, rep);
}

}  // namespace

bool vodSyncAvailable(AppState* st) { return !xtreamTargets(st).empty(); }

VodSyncStart startVodSync(AppState* st) {
    if (!st || !st->db.isOpen()) return VodSyncStart::DatabaseClosed;

    // 🔴 The max_connections gate, and the reason this is a user-triggered action rather than a
    // timer. One connection is the whole budget on the measured line, so a sync that runs during
    // playback does not merely queue — it gets the user's own stream kicked. Checked across ALL
    // panes (Split shows four) and across panes that are still tearing down: a dying pane's
    // libVLC stop() is asynchronous, so its socket can outlive the user's decision to close it.
    //
    // isRecording() is checked too, and it is NOT redundant. A VlcPlayer owns TWO libVLC players:
    // `mp_` (playback, which drives isPlaying) and the headless `rec_` recorder, which has its own
    // socket and which doStop() deliberately never touches — so "recording with nothing playing"
    // is an ordinary documented state, and it is the state a SCHEDULED recording runs in. Without
    // this the 20:00 wake-to-record job is exactly what a sync would kick, and the scheduler would
    // then mark the truncated file Done.
    // nowPlayingId is checked as well as isPlaying(), and it is the one that closes the real hole:
    // `playing_` is set from the libvlc_MediaPlayerPlaying EVENT, so it is false for the whole
    // open+buffer window — seconds, on an IPTV line, which is why StatusOpening exists — while the
    // socket is already claimed. nowPlayingId is set synchronously when a channel is loaded into a
    // pane and cleared on Stop, so it covers exactly that gap. dyingPanes have no id but do have a
    // player that may still be draining.
    auto busy = [](const std::unique_ptr<VideoPane>& p) {
        return p && (p->player.isPlaying() || p->player.isRecording() || p->nowPlayingId != 0);
    };
    for (const std::unique_ptr<VideoPane>& p : st->panes)
        if (busy(p)) return VodSyncStart::PlaybackActive;
    for (const std::unique_ptr<VideoPane>& p : st->dyingPanes)
        if (busy(p)) return VodSyncStart::PlaybackActive;

    // The dead-link sweep is the OTHER worker that opens a connection to the same provider.
    // Neither knew about the other, so both could run at once against a one-connection line — and
    // the failure mode is not merely slow: a sweep whose probes get refused for capacity
    // classifies channels and can persist dead_status=Dead on live TV. Refuse rather than contend.
    if (deadLinkSweepRunning()) return VodSyncStart::SweepActive;

    std::vector<SyncTarget> targets = xtreamTargets(st);
    if (targets.empty()) return VodSyncStart::NoXtreamPlaylist;

    if (g_running.exchange(true)) return VodSyncStart::AlreadyRunning;  // one at a time
    g_cancel = false;
    // The previous run's report is deliberately NOT cleared here. Its WM_APP_VOD_DONE may still be
    // sitting in the queue undispatched, and that message is ABOUT the old run — wiping the counts
    // out from under it would render "0 added, 0 removed" for a sync that did real work.
    if (g_worker.joinable()) g_worker.join();  // reap the previous, finished, worker
    try {
        g_worker = std::thread(syncThread, st->hwnd, Database::defaultDbPath(), std::move(targets));
    } catch (...) {
        // Thread creation is the one thing here that can fail after the flag is claimed, and a
        // stuck flag wedges the menu item at "Syncing…" for the rest of the session.
        g_running = false;
        diag::error(L"VOD sync: could not start the worker thread");
        return VodSyncStart::ThreadFailed;
    }
    return VodSyncStart::Started;
}

bool vodSyncRunning() { return g_running.load(); }

void cancelVodSync() { g_cancel = true; }

void shutdownVodSync() {
    g_cancel = true;
    if (g_worker.joinable()) g_worker.join();
}

VodSyncReport vodSyncReport() {
    std::lock_guard<std::mutex> lock(g_reportMx);
    return g_report;
}

}  // namespace mw
}  // namespace rabbitears
