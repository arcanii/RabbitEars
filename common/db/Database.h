// SPDX-License-Identifier: GPL-3.0-or-later
// Database — the SQLite-backed persistence layer for RabbitEars.
//
// Wraps a single sqlite3 connection with parameterized statements (the piece the
// sibling SQLTerminal-Win32 SqliteProvider intentionally lacks — it is a generic
// SQL terminal). All app data (playlists, channels, favourites, LCN numbering,
// settings) lives in one DB file, by default %LOCALAPPDATA%\RabbitEars\rabbitears.db.
#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "models/Channel.h"
#include "models/ParsedChannel.h"
#include "models/Playlist.h"
#include "models/Programme.h"
#include "models/RecordingRule.h"
#include "models/ScheduledRecording.h"

struct sqlite3;  // forward-declared; sqlite3.h is included only in the .cpp

namespace rabbitears {

// ⚠ THE FILTERS LIVE HERE, IN SQL, AND THAT IS THE WHOLE POINT. They used to be applied in
// C++ over the returned vector, which composes the wrong way round the moment a limit exists:
// "the matches among the first 5,000 rows" instead of "the first 5,000 matches". Those are not
// the same set and the difference is not cosmetic — with a 20,000-channel playlist whose Sports
// block sits at position ~15,000, an include-filter of {Sports} would return an EMPTY grid while
// Groups ▸ Sports (its own query) showed the same channels perfectly. Pushing the predicates
// down means the limit applies to the filtered set, which is what a user means by "show me the
// first N".
//
// Every field defaults to "no restriction", so a default-constructed GridFilter reproduces the
// pre-0.2.17 query exactly — which is what leaves the macOS app, and every non-grid caller
// here, byte-for-byte unaffected.
//
// ⚠ SCOPE: this is deliberately at NAMESPACE scope, not nested in Database. Nested, its default
// member initializers belong to Database's complete-class context, so the `= {}` default
// arguments below could not use them until Database was complete — which MSVC accepts but Clang
// correctly rejects ("default member initializer for 'limit' needed within definition of
// enclosing class"), breaking every macOS build. `Database::GridFilter` still resolves via the
// member alias inside the class, so all existing Win32 call sites are unchanged.
struct GridFilter {
    // 0 == unlimited. Callers that need a COMPLETE list (favourites export, the recording
    // scheduler, the rule/schedule editors, the dead-link sweep) must leave it at 0: a
    // truncated list there silently drops the user's data rather than merely hiding rows.
    int  limit = 0;
    bool hideDead = false;                  // drop DeadStatus::Dead
    std::vector<std::wstring> categories;   // empty == no category restriction
};

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Open (creating if needed) the DB at `path`, apply pragmas, and create the
    // schema. Returns false and sets `error` (if non-null) on failure.
    // `upgradeSchema` false: take the database at whatever version it is (read, not migrated) — for a
    // worker's second connection, opened after the app's own. A schema step the app's connection
    // could not complete must not be retried from a worker: the v11 build holds the write lock for
    // ~4 s on a large library, long enough for the UI's own writes to time out behind it.
    bool open(const std::wstring& path, std::wstring* error = nullptr, bool upgradeSchema = true);
    void close();
    bool isOpen() const { return db_ != nullptr; }

    // Default DB location: %LOCALAPPDATA%\RabbitEars\rabbitears.db, honoring the
    // RABBITEARS_DATA_DIR env override (used by tests). Creates the directory.
    static std::wstring defaultDbPath();

    // ---- Playlists ---------------------------------------------------------
    // Returns the new playlist id, or 0 on failure. `epgUrl` is the playlist's XMLTV
    // guide URL (defaulted so existing/mac callers are unaffected).
    long long addPlaylist(const std::wstring& name, const std::wstring& source, bool isUrl,
                          long long nowEpoch, const std::wstring& epgUrl = {});
    std::vector<Playlist> listPlaylists();
    void deletePlaylist(long long playlistId);
    // Change a playlist's friendly display name (its channels/source are untouched).
    void renamePlaylist(long long playlistId, const std::wstring& name);
    // Override a playlist's XMLTV guide URL (the M3U x-tvg-url is used as the default,
    // but some feeds ship a stub/empty guide — this points Refresh Guide at a better one).
    // Pass an empty string to clear the override back to "no guide for this playlist".
    void setPlaylistEpgUrl(long long playlistId, const std::wstring& epgUrl);
    // Enable/disable a playlist. A disabled playlist keeps its rows but is excluded
    // from every cross-playlist query (all-channels, favourites, groups, countries,
    // search, LCN lookup); channelsByPlaylist() still returns it verbatim.
    void setPlaylistEnabled(long long playlistId, bool enabled);

    // ---- Channels ----------------------------------------------------------
    // Insert/refresh a freshly-parsed batch under `playlistId` in one transaction.
    // Existing rows (same playlist_id + stream_url) are updated in place, preserving
    // the user's favourite flag and custom LCN. Updates the playlist channel_count /
    // last_refreshed_at. Returns the number of rows inserted or updated.
    int bulkInsertChannels(long long playlistId, const std::vector<ParsedChannel>& channels,
                           long long nowEpoch);

    // Delete rows of ONE kind in one playlist whose stream_url is not in `keepUrls` — the
    // other half of a VOD sync, since a provider's catalogue churns and without this the
    // library only ever grows. Scoped by kind so a VOD sync can never touch live channels.
    // An EMPTY keepUrls deletes nothing (a failed/partial fetch must not wipe a library);
    // the caller is responsible for only passing a set it actually trusts. Any failure to
    // stage the keep-set aborts and rolls back rather than deleting a partial difference.
    //
    // Returns the number of rows removed, or **-1 if the operation FAILED** and rolled back —
    // following clearDeadStatuses(). The distinction matters more here than anywhere else in this
    // class: without it a rolled-back retirement is indistinguishable from "the provider dropped
    // nothing", so the caller reports a clean sync and the library silently never converges. That
    // is exactly the "harder to spot because it looks like it worked" failure the warning below is
    // about. An empty keepUrls still returns 0 — a no-op is not a failure.
    // Recomputes playlists.channel_count.
    //
    // ⚠ NOT REENTRANT, and NOT safe to run concurrently on ONE Database handle. It stages
    // the keep-set in a per-CONNECTION temp table inside a transaction, so two overlapping
    // calls would clear each other's set mid-flight and the inner commit would end the outer
    // transaction early — mass deletion. Database::open uses SQLITE_OPEN_FULLMUTEX, so
    // sharing a handle across threads is otherwise fine; this one function is the exception.
    // Run the VOD sync on a worker with its OWN connection, per Win32/docs/XTREAM_VOD.md §4.
    int retireMissingChannels(long long playlistId, int kind,
                              const std::vector<std::wstring>& keepUrls);

    // ---- Grid queries ------------------------------------------------------
    // What the channel GRID displays, and the only place a row cap belongs.
    //
    // The grid filter/limit bundle. Defined at namespace scope (see above for why); this alias
    // keeps the established `Database::GridFilter` spelling working at every call site.
    using GridFilter = ::rabbitears::GridFilter;

private:
    std::vector<std::wstring> listGroupsOfKind(int kind);
    std::vector<Channel> channelsByGroupOfKind(const std::wstring& group, int kind,
                                               const GridFilter& g);

public:

    // ---- Grid queries ------------------------------------------------------
    std::vector<Channel> allChannels(const GridFilter& g = {});
    std::vector<Channel> channelsByPlaylist(long long playlistId, const GridFilter& g = {});
    // LIVE channels in `group`. Kind-scoped: the live tree and the Movies tree are separate
    // namespaces, so a VOD category sharing a live group's name cannot cross-contaminate.
    std::vector<Channel> channelsByGroup(const std::wstring& group, const GridFilter& g = {});
    // ---- VOD (schema v8) ----------------------------------------------------
    // Movies live under their own "Movies" nav root rather than as ~67 extra siblings in the
    // live group tree — see listGroups().
    std::vector<Channel> moviesByGroup(const std::wstring& group, const GridFilter& g = {});
    std::vector<Channel> allMovies(const GridFilter& g = {});  // newest-first (provider `added`)
    std::vector<std::wstring> listVodGroups();  // the VOD categories
    std::vector<Channel> favourites(const GridFilter& g = {});
    // Channels (any kind) in enabled playlists whose NAME contains `term` — a substring, case- and
    // accent-insensitive for Latin script ("quebec" finds "TVA QUÉBEC"); typed characters are
    // literal (no wildcards). Through the channel index (schema v11 — docs/CHANNEL_SEARCH.md):
    // 0.2–1 ms for most words on the owner's 410k channels, ~45 ms for the commonest ("the"). Under
    // 3 characters (trigram's minimum — which also covers most CJK searches), or without the index
    // (v11 not landed), a LIKE scan of the names: ~110–150 ms there, ASCII-only case folding. Before
    // v11 this also matched group titles and tvg-names; names only is the owner's decision (2026-09-25).
    std::vector<Channel> searchChannels(const std::wstring& term, const GridFilter& g = {});
    // The TV Guide search's "Also in your channel list, not in the guide: N": how many DISTINCT names
    // of LIVE channels in enabled playlists match `text` the way searchChannels does, NONE of whose
    // channels carries one of `coveredIds` — the guide rows' guide ids, normalised ('@feed' stripped,
    // ASCII-lower-cased) — so an HD/FHD sibling of a channel the guide shows is not counted. -1 when
    // it cannot be answered cheaply: under 3 characters, no channel index, or more than `maxRows`
    // matching channels (a broad word, where the note would be noise anyway).
    int countUncoveredChannelNames(const std::wstring& text, const std::unordered_set<std::wstring>& coveredIds,
                                   int maxRows);
    // True once the channel index exists (schema v11 landed on this connection).
    bool channelSearchIndexed() const { return schemaVersion_ >= 11; }
    std::optional<Channel> channelByLcn(int lcn);
    // First enabled channel carrying this tvg-id (the EPG join key); nullopt if none.
    // Used to resolve a guide programme back to a recordable stream.
    std::optional<Channel> channelByTvgId(const std::wstring& tvgId);
    // The channel row by primary key (enabled playlists only); nullopt if it no longer
    // exists — e.g. a persisted last_channel_id whose playlist was deleted.
    std::optional<Channel> channelById(long long id);

    std::vector<std::wstring> listGroups();
    // Distinct ISO country codes (lowercase) derived from tvg-id suffixes
    // (iptv-org convention: "<name>.<cc>", e.g. "CNN.us"); + channels for one code.
    std::vector<std::wstring> listCountries();
    std::vector<Channel> channelsByCountry(const std::wstring& code, const GridFilter& g = {});

    void setFavourite(long long channelId, bool favourite);
    void toggleFavourite(long long channelId);
    void setChannelNumber(long long channelId, std::optional<int> lcn);
    void setDeadStatus(long long channelId, DeadStatus status, long long nowEpoch);
    // Undo for a dead-link sweep: reset every channel to Unknown + clear last_checked_at.
    // Returns rows affected, or -1 if the write failed (0 means "nothing needed clearing").
    int clearDeadStatuses();

    // ---- EPG (programmes) --------------------------------------------------
    // Replace this playlist's stored guide with a freshly-parsed batch, in one
    // transaction (a refresh is authoritative — old rows are cleared first). Also
    // records an `epg_refreshed_<id>` settings timestamp. Returns rows stored.
    int bulkInsertProgrammes(long long playlistId, const std::vector<Programme>& programmes,
                             long long nowEpoch);
    // The programme airing at `nowEpoch` plus the one after it (0–2 rows) for a
    // channel's tvg-id; empty when the guide has no coverage there.
    std::vector<Programme> nowNext(long long playlistId, const std::wstring& channelId,
                                   long long nowEpoch);
    // Every programme overlapping [windowStartUtc, windowEndUtc), ordered by channel
    // then start — the timeline-guide query.
    std::vector<Programme> programmesInWindow(long long playlistId, long long windowStartUtc,
                                              long long windowEndUtc);
    // The same window across ALL enabled playlists — recording rules are library-wide, not
    // playlist-scoped (see core/RecordingRules).
    std::vector<Programme> programmesInWindowAll(long long windowStartUtc, long long windowEndUtc);

    // ---- Programme search (schema v10, docs/EPG_SEARCH.md) ------------------
    // Two external-content FTS5 tables over epg_programmes: titles (trigram — any substring) and
    // descriptions (unicode61 — words and prefixes), both case- and accent-insensitive. NOTHING
    // maintains them automatically (no triggers, and bulkInsertProgrammes does not touch them): a
    // caller that wants search calls rebuildProgrammeIndex() after storing guides, and search checks
    // a stamp first so a stale index is never read. Until then, nothing here costs anything.
    enum class ProgrammeSearchState {
        Ready,         // the index matches epg_programmes
        NeedsRebuild,  // it does not (never built, a refresh or playlist delete since, an older
                       // build's refresh) — call rebuildProgrammeIndex() before relying on search
        Unavailable,   // no index (schema v10 did not land): search answers with a LIKE scan
    };
    // Cheap after the first call: the stamp (COUNT + MAX(id) over epg_programmes plus the
    // epg_refreshed_* settings — 7 ms at 193k rows, measured) is compared once, then remembered until
    // this object changes epg_programmes (bulkInsertProgrammes, deletePlaylist) or is reopened. Nothing
    // else in the process writes epg_programmes (the sync workers' connections do not).
    ProgrammeSearchState programmeSearchState();
    // Rebuild both tables from epg_programmes and record the stamp, in ONE transaction of its own.
    // Seconds on a large guide (~1.5 s at the owner's 193k programmes, measured through this code on
    // a copy of their library — RabbitEarsCli --epgsearch): call it where the user has been
    // told, e.g. behind a loading box. False = nothing changed (no index, a contended writer, or a
    // failed statement — lastError()); the old stamp then still says whether the index is current.
    bool rebuildProgrammeIndex();

    struct ProgrammeHit {
        long long    id = 0;            // epg_programmes.id
        long long    playlistId = 0;
        Programme    programme;         // channelId is the GUIDE's id (not normalised)
        // The user's channel it airs on — the one a TV Guide row shows for it: the first of that
        // playlist's channels sharing the normalised tvg-id, in channelsByPlaylist's order.
        // channelTvgId is that channel's FULL tvg-id, which is what Play / Schedule resolve;
        // channelName may be empty (a nameless channel — show the id, as the guide does).
        std::wstring channelName, channelTvgId;
        bool         inTitle = false;   // false = matched only in the description
        // For display, each marked occurrence wrapped in U+0002 … U+0003: `markedTitle` = the title
        // with the typed text marked (title matches); `snippet` = an excerpt of the description around
        // the EARLIEST occurrence of any typed word (description-only matches). Marking is simpler
        // than the index's matching, so it can differ: it compares characters through searchFold
        // (core/SearchFold.h — case, and Latin accents: "quebec" marks "Québec"), one for one, so a
        // match the index found through some other folding stays unmarked (markedTitle empty,
        // snippet = the description's start); it marks EVERY typed word as a prefix (the index takes
        // only the last), and compares typed punctuation literally.
        std::wstring markedTitle, snippet;
    };
    // (Re)load which channels a search may return: every channel with a tvg-id in an ENABLED
    // playlist, per playlist, by normalised tvg-id — the channels a TV Guide row can be built for
    // (the guide itself only builds rows for programmes in its −6 h..+72 h window). Results are
    // filtered to these BEFORE the limit. Kept on this connection (a TEMP table) until the next call;
    // call it when a search session starts (~90 ms at the owner's 410k channels, measured).
    bool refreshProgrammeSearchChannels();
    // Programmes still airing or upcoming at `fromUtc` (stop_utc > fromUtc) on a channel
    // refreshProgrammeSearchChannels() loaded, whose title contains `text` or whose description has
    // its words (the last one as a prefix): title matches first, then description-only matches, each
    // soonest first; at most `limit`. The text is searched for literally — FTS5 syntax typed by the
    // user is never interpreted. Case is folded for every cased script, accents only for LATIN (the
    // tokenizers do not strip e.g. Greek tonos). Under 3 characters (trigram's minimum), or unless
    // the index is Ready, it falls back to a LIKE scan for the whole text as one substring — of the
    // titles, and also of the descriptions unless the index is Ready: slower, ASCII-only case folding,
    // accents exact (its results are still marked through searchFold, which ignores accents). A term
    // with CJK in it searches descriptions by LIKE too, since the description tokenizer cannot split
    // CJK into words. `truncated` (optional) is set when more than `limit` matched. Measured on the owner's
    // real guide (193k programmes): 0.2–0.4 ms for most words, 20–40 ms for the commonest ("news",
    // "the"); the LIKE fallback ~50 ms.
    std::vector<ProgrammeHit> searchProgrammes(const std::wstring& text, long long fromUtc, int limit,
                                               bool* truncated = nullptr);
    // Per playlist (enabled or not): how many distinct guide ids its LIVE channels carry — tvg-ids
    // normalised the way the guide joins them ('@feed' stripped, ASCII-lower-cased), so channels
    // sharing an id count once. Playlists with none are absent. The TV Guide's coverage line ("guide
    // data for N of M channels with a guide ID"). ONE pass over the tvg-id index's id-carrying rows,
    // whatever the number of playlists: ~3 ms on the owner's library, whose 366k live rows are mostly
    // series episodes carrying no id at all.
    std::vector<std::pair<long long, int>> distinctLiveGuideIds();
    // Those ids themselves, across ENABLED playlists — the coverage line's "guide channels matching
    // none of yours" is the guide's channels minus these. Same index, ~3 ms.
    std::vector<std::wstring> liveGuideIds();

    // ---- Scheduled recordings ----------------------------------------------
    long long addSchedule(const ScheduledRecording& s);  // returns the new id, or 0 on failure
    std::vector<ScheduledRecording> listSchedules();     // ordered by start_utc
    // True only when the row now carries `status`: the UPDATE completed AND matched a row. False
    // means nothing was written — SQLITE_BUSY past busy_timeout, a failed prepare, or no such id (the
    // bool cannot tell those apart; read the row back if it matters). ⚠ planScheduler learns that a
    // SCHEDULE holds the recorder only from this column (its manualRecordingActive input covers
    // recordings no schedule owns), so a caller that changes recorder state on the assumption the
    // write landed MUST check it — see beginScheduledStart / PendingStatusWrite in
    // core/RecordingScheduler. Was void until 2026-09; callers that ignore the result compile
    // unchanged, mac's included.
    bool updateScheduleStatus(long long id, ScheduleStatus status, const std::wstring& filePath = {});
    // True when the DELETE completed (the row is gone, or never existed). False = LOST, and the row
    // survives with whatever status it had. Was void until 2026-09; ignoring it compiles unchanged.
    bool deleteSchedule(long long id);

    // ---- Recording rules (EPG-driven series recording, schema v5) -----------
    // A rule is a recipe; core/RecordingRules expands it against the stored EPG into ordinary
    // scheduled_recordings rows (tagged with rule_id).
    long long addRule(const RecordingRule& r);  // returns the new id, or 0 on failure
    void updateRule(const RecordingRule& r);    // overwrite the editable fields of the rule r.id
    std::vector<RecordingRule> listRules();     // ordered by created_at
    void setRuleEnabled(long long id, bool enabled);
    // Deletes the rule and its still-Pending schedules; recordings that already ran (or were
    // cancelled/missed) are kept as history. Both deletes run in ONE transaction: false = LOST, and
    // NEITHER table changed (nothing orphaned, nothing unqueued). Was void until 2026-09
    // (source-compatible). ⚠ Opens its own BEGIN IMMEDIATE — do not call it inside a transaction.
    bool deleteRule(long long id);
    // Drop a rule's still-Pending schedules without touching the rule (used when a rule is edited:
    // its old predictions no longer match, so they are cleared and the rule re-expanded). History
    // (Done/Recording/Cancelled/…) is kept. False = LOST, and the old predictions are still queued.
    // Was void until 2026-09 (source-compatible).
    bool clearPendingForRule(long long ruleId);

    // ---- Settings (key/value blob) ----------------------------------------
    std::optional<std::wstring> getSetting(const std::wstring& key);
    void setSetting(const std::wstring& key, const std::wstring& value);

    const std::wstring& lastError() const { return lastError_; }

private:
    bool exec(const char* sql);
    bool createSchema();
    void migrate();  // incremental, idempotent schema upgrades keyed off PRAGMA user_version
    bool hasColumn(const char* table, const char* column);  // PRAGMA table_info membership test

    // Runs the v9 step: rewrite every stream_url to its canonical spelling and merge the rows that
    // become equal. Returns true when the DB is at v9 afterwards. Separate from migrate() because
    // it is the only step that REWRITES existing rows rather than adding columns, and the only one
    // whose failure has to be reported rather than probed for structurally.
    bool canonicalizeStreamUrls();
    // The v10 step: create the two (empty) programme-search tables and record v10 — one
    // transaction. True when the DB is at v10 afterwards.
    bool createProgrammeSearchTables();
    bool programmeSearchTablesExist();       // both tables present AND genuinely FTS5
    // The v11 step: the channel search index — table, triggers, the build from existing rows and
    // v11 — in one transaction. True when the DB is at v11 afterwards.
    bool createChannelSearchIndex();
    bool channelSearchTableExists();         // channels_fts present AND genuinely FTS5
    std::wstring programmeIndexStamp();      // what the index must have been built from
    // programmeSearchState()'s memory: -1 = not yet checked since this connection last changed
    // epg_programmes, 0 = stale, 1 = current. Reset by bulkInsertProgrammes and deletePlaylist.
    int          programmeIndexKnown_ = -1;

    sqlite3*     db_ = nullptr;
    std::wstring lastError_;
    // The schema version this connection actually left the database at, captured by migrate().
    // ⚠ LOAD-BEARING: the canonicalising write path is gated on it being >= 9. Storing canonical
    // URLs into a database whose existing rows are still un-canonical is the destructive
    // combination — the next playlist refresh would miss every stored row on idx_channels_dedupe
    // and insert a duplicate of the ENTIRE library. If the migration does not land, this stays at
    // 8 and writes keep the old literal behaviour, which merely duplicates movies: bad, survivable,
    // and repaired by the next successful open.
    int          schemaVersion_ = 0;
    bool         upgradeSchema_ = true;  // open()'s upgradeSchema: false = read the version, migrate nothing
};

}  // namespace rabbitears
