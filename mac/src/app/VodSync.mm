// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Xtream VOD sync worker — see VodSync.h for what it is and the two things about it that are
// not obvious (its own DB connection, and cancellation being soft).
//
// ⚠ THIS FILE HOLDS NO OBJECTIVE-C OBJECTS. It is pure C++ + GCD + std::function, which is why it
// is deliberately NOT in the -fobjc-arc list in mac/CMakeLists.txt: with no Obj-C object to own,
// ARC-vs-MRC has nothing to decide here, and no block escapes into MRC code. Keep it that way —
// the moment an NSString or an NSAlert appears in here, the file needs that whole question asked.
//
// The behaviour is a faithful port of Win32/ui/VodSync.cpp. Where a comment there explains WHY a
// guard exists, the guard is reproduced here with the same reasoning rather than re-derived.

#import <dispatch/dispatch.h>

#include <atomic>
#include <ctime>
#include <utility>

#include "VodSync.h"

#include "core/Http.h"
#include "platform/Log.h"

namespace rabbitears {
namespace mac {

namespace {

// A film is only evidence that films are gone if most of what the provider listed was usable.
constexpr int kMinUsablePercent   = 50;
constexpr int kAccountTimeoutMs   = 15000;
constexpr int kCatalogueTimeoutMs = 30000;

std::atomic<bool> g_running{false};
std::atomic<bool> g_cancel{false};

// The body of one run. Returns through `rep`; every early exit sets a result FIRST.
void runSync(const std::wstring& dbPath,
             const std::vector<VodSyncTarget>& targets,
             const std::function<void(VodSyncPhase, int)>& progress,
             VodSyncReport& rep) {
    // Its OWN connection — see the header. Opening it here also means the v8/v9 migration can run
    // on THIS queue rather than the main one if the app has never opened the DB, which is a bonus
    // rather than the reason.
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

    for (const VodSyncTarget& t : targets) {
        if (g_cancel.load()) { rep.result = VodSyncResult::Cancelled; break; }

        // ---- 1. Who are we? -------------------------------------------------------------
        // A panel answers BAD CREDENTIALS with HTTP 200 and auth:0, so "it responded" and "it let
        // us in" are different questions. Worth one ~1 KB request before pulling tens of MB: an
        // expired line otherwise surfaces as a JSON parse error on an error page, which reads as a
        // bug in us rather than a bill the user has to pay.
        progress(VodSyncPhase::Contacting, 0);
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
                   std::to_wstring(acct.maxConnections));
        // authOk alone is not enough — it is true for a line that is authenticated and banned or
        // expired, which is exactly the case a user hits and needs told plainly. An empty status
        // is not a verdict, so it does not refuse.
        if (!acct.authOk || (!acct.status.empty() && acct.status != L"Active")) {
            rep.result = VodSyncResult::AuthFailed;
            rep.detail = acct.status.empty() ? L"auth rejected" : acct.status;
            break;
        }
        if (g_cancel.load()) { rep.result = VodSyncResult::Cancelled; break; }

        // ---- 2. Two requests for the whole catalogue ------------------------------------
        // Not one call per category, and emphatically not one get_vod_info per film: under
        // max_connections:1 the request COUNT is the cost.
        progress(VodSyncPhase::Fetching, 0);
        std::vector<XtreamCategory> cats;
        if (!httpGet(xtreamApiUrl(t.creds, L"get_vod_categories"), body, err, kCatalogueTimeoutMs)) {
            diag::error(L"VOD sync: get_vod_categories failed: " + err);
            rep.result = VodSyncResult::NetworkError;
            rep.detail = err;
            break;
        }
        if (!parseXtreamCategories(body, cats, &err)) {
            // NOT survivable, though it reads like it should be. bulkInsertChannels writes
            // `group_title=excluded.group_title` UNCONDITIONALLY, so carrying on with an empty
            // category list would rewrite every stored movie to the "Movies" fallback in one
            // committed transaction and flatten the whole tree — on a transient parse hiccup,
            // with no undo. A sync that cannot name its categories has to stop.
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
        body.shrink_to_fit();  // tens of MB, and the row build below is not small either

        rep.unusable += res.skippedNoId + res.skippedNoExt;
        diag::info(L"VOD sync: \"" + t.name + L"\" " + std::to_wstring(res.movies.size()) +
                   L" usable of " + std::to_wstring(res.total) + L" (no id: " +
                   std::to_wstring(res.skippedNoId) + L", no extension: " +
                   std::to_wstring(res.skippedNoExt) + L"), " + std::to_wstring(cats.size()) +
                   L" categories");

        // The other half of the category guard, and it can only be checked HERE because it needs
        // BOTH responses. parseXtreamCategories returns TRUE with an empty vector for a body that
        // is a valid empty array — so the abort above (which only catches a parse FAILURE) leaves
        // the identical disaster reachable. The precise test is not "cats is empty" but "the
        // movies reference categories we do not have": a provider that genuinely files nothing
        // into categories sends no category_id either, and for them one "Movies" group is the
        // correct answer, not a failure.
        if (cats.empty()) {
            bool wantsCategories = false;
            for (const XtreamMovie& m : res.movies)
                if (!m.categoryId.empty()) { wantsCategories = true; break; }
            if (wantsCategories) {
                diag::error(L"VOD sync: movies reference categories but get_vod_categories "
                            L"returned none — refusing rather than flattening the Movies tree");
                rep.result = VodSyncResult::ParseError;
                rep.detail = L"no categories returned";
                break;
            }
        }
        if (g_cancel.load()) { rep.result = VodSyncResult::Cancelled; break; }

        // ---- 3. Write --------------------------------------------------------------------
        // Past this point cancellation is ignored: bulkInsertChannels is one transaction, and
        // stopping between the insert and the retire leaves the library consistent anyway
        // (retirement is idempotent and the next sync redoes it).
        std::vector<ParsedChannel> rows =
            xtreamMoviesToChannels(t.creds, res.movies, cats, L"Movies");
        res.movies.clear();
        res.movies.shrink_to_fit();  // two full-catalogue vectors alive at once is the peak
        // `rows`, not res.movies, is what the counts below are about — it is both what gets
        // inserted AND what becomes the keep-set, so the trust guard has to reason about the set
        // that actually reaches the DELETE.
        const long long kept = static_cast<long long>(rows.size());
        if (kept == 0) {
            // `continue`, not `break`: one dud line must not stop the others, and the verdict is
            // only promoted to EmptyCatalogue after the loop if NOTHING was committed.
            diag::warn(L"VOD sync: no usable movies for \"" + t.name + L"\" — nothing written");
            anyEmpty = true;
            continue;
        }
        progress(VodSyncPhase::Saving, static_cast<int>(rows.size()));

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

        // ---- 4. Retire, but only on evidence we believe -----------------------------------
        // 64-bit arithmetic, not int: `total` is provider-controlled, so this multiplication is
        // the one place a hostile or broken panel could push past INT_MAX and flip the comparison
        // — into deleting the library.
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
            // MOVED out of `rows`, not copied — a second full-catalogue vector of URLs at the
            // sync's memory peak is exactly what this avoids.
            for (ParsedChannel& p : rows) keep.push_back(std::move(p.streamUrl));
            rows.clear();
            rows.shrink_to_fit();
            const int gone =
                db.retireMissingChannels(t.id, static_cast<int>(Channel::Kind::Movie), keep);
            if (gone < 0) {
                // A rolled-back retirement, NOT "the provider dropped nothing". Reporting a clean
                // sync here looks like it worked while the library silently never converges.
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

    // Only now is "the provider returned nothing" the story of the run: if any line committed
    // rows, a second line coming back empty is a detail, not the verdict.
    if (anyEmpty && rep.result == VodSyncResult::Ok && rep.inserted == 0)
        rep.result = VodSyncResult::EmptyCatalogue;
}

}  // namespace

std::vector<VodSyncTarget> xtreamTargets(Database& db) {
    std::vector<VodSyncTarget> out;
    if (!db.isOpen()) return out;
    for (const Playlist& p : db.listPlaylists()) {
        if (!p.enabled || !p.isUrl || p.sourceUrl.empty()) continue;
        VodSyncTarget t;
        if (!parseXtreamPlaylistUrl(p.sourceUrl, t.creds)) continue;  // a plain .m3u — not an error
        t.id = p.id;
        t.name = p.name;
        out.push_back(std::move(t));
    }
    return out;
}

VodSyncStart startVodSync(VodSyncRequest req) {
    // Claimed LAST of all the checks (the controller's refusal ladder runs first), so no refusal
    // path can leave the flag set and wedge the menu item at "Syncing…" for the session.
    if (g_running.exchange(true)) return VodSyncStart::AlreadyRunning;
    g_cancel = false;

    // Copied into the block: the worker must not read anything the main thread still owns.
    const std::wstring dbPath = req.dbPath;
    const std::vector<VodSyncTarget> targets = req.targets;
    auto onProgress = req.onProgress;
    auto onDone = req.onDone;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        VodSyncReport rep;
        // Progress hops to the main queue; the worker never touches UI state itself.
        auto progress = [onProgress](VodSyncPhase ph, int n) {
            if (!onProgress) return;
            dispatch_async(dispatch_get_main_queue(), ^{ onProgress(ph, n); });
        };
        runSync(dbPath, targets, progress, rep);

        // The single exit. Drop the flag BEFORE handing the report to the main queue: the
        // completion re-enables the menu item, and a completion that ran while the flag was still
        // set would leave "Syncing…" showing over a finished run.
        g_running = false;
        if (onDone) dispatch_async(dispatch_get_main_queue(), ^{ onDone(rep); });
    });
    return VodSyncStart::Started;
}

bool vodSyncRunning() { return g_running.load(); }

void cancelVodSync() { g_cancel = true; }

}  // namespace mac
}  // namespace rabbitears
