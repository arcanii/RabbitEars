// SPDX-License-Identifier: GPL-3.0-or-later
#include "db/Database.h"

#include <sqlite3.h>

#include <algorithm>
#include <cwchar>  // std::wcstoll — NOT _wtoll, which is MSVC-only and breaks the macOS build
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "core/SearchFold.h"
#include "core/UrlCanon.h"
#include "platform/Encoding.h"

namespace rabbitears {
namespace {

// RAII wrapper over a prepared statement with bound parameters (1-based) and
// typed column reads (0-based). SQLITE_TRANSIENT copies bound buffers so the
// temporary UTF-8 strings can die immediately after the bind call.
struct Stmt {
    sqlite3_stmt* s = nullptr;

    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    explicit operator bool() const { return s != nullptr; }

    void bindText(int i, const std::wstring& w) {
        const std::string u = utf8FromWide(w);
        sqlite3_bind_text(s, i, u.c_str(), static_cast<int>(u.size()), SQLITE_TRANSIENT);
    }
    void bindInt(int i, long long v) { sqlite3_bind_int64(s, i, v); }
    void bindNull(int i) { sqlite3_bind_null(s, i); }
    void bindOptInt(int i, std::optional<int> v) {
        if (v) sqlite3_bind_int64(s, i, *v);
        else sqlite3_bind_null(s, i);
    }

    bool step() { return sqlite3_step(s) == SQLITE_ROW; }
    int stepDone() { return sqlite3_step(s); }  // SQLITE_DONE on success

    bool isNull(int col) { return sqlite3_column_type(s, col) == SQLITE_NULL; }
    long long intCol(int col) { return sqlite3_column_int64(s, col); }
    std::optional<int> optIntCol(int col) {
        if (isNull(col)) return std::nullopt;
        return static_cast<int>(sqlite3_column_int64(s, col));
    }
    std::wstring textCol(int col) {
        const auto* p = sqlite3_column_text(s, col);
        const int n = sqlite3_column_bytes(s, col);
        return p ? wideFromUtf8(reinterpret_cast<const char*>(p), n) : std::wstring();
    }
    void reset() {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
};

// Scoped IMMEDIATE transaction; rolls back if not committed (e.g. on exception
// or early return). Bulk-inserting thousands of channels autocommit-per-row is
// ~100x slower, so every batch write is wrapped in one of these.
struct Tx {
    sqlite3* db;
    bool done = false;
    // ⚠ Whether the transaction actually OPENED. Discarding this was a silent hole under the whole
    // class: BEGIN IMMEDIATE takes the WAL writer lock and runs the busy handler, so with
    // busy_timeout=5000 it returns SQLITE_BUSY whenever another connection holds a write
    // transaction for longer than 5 s — which the v9 migration (6.6 s measured) and a 410k-row
    // playlist upsert both do. The connection then stays in AUTOCOMMIT, so every statement in the
    // body commits individually, the destructor's ROLLBACK is a no-op, and commit() fails — i.e.
    // the work lands with no atomicity and is then reported as a FAILURE. Every guarantee written
    // in terms of "inside one transaction" evaluates to nothing in that state, so callers check.
    bool began = false;
    explicit Tx(sqlite3* d) : db(d) {
        began = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    explicit operator bool() const { return began; }
    // Returns false when the COMMIT itself failed. Most callers ignore it — a lost settings write
    // is survivable — but retireMissingChannels does NOT: it is the one operation here that
    // DELETES in bulk, and reporting a plausible "n removed" for a transaction that never landed
    // is the "looks like it worked" failure its own documentation warns about.
    //
    // ⚠ `done` is set to the RESULT, not unconditionally to true. A COMMIT that fails with
    // SQLITE_BUSY leaves the transaction OPEN — SQLite's contract is that the application must
    // then retry or roll back — so swallowing the failure here would skip the destructor's
    // ROLLBACK and strand a write transaction on the connection. Leaving `done` false makes the
    // destructor clean up. In the other failure modes the statement has already released the
    // transaction and the compensating ROLLBACK is a harmless "no transaction is active".
    bool commit() {
        if (!began) return false;  // never opened: there is nothing to commit and nothing landed
        const bool ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
        done = ok;
        return ok;
    }
    ~Tx() {
        // Only roll back a transaction we actually opened — otherwise this fires a stray ROLLBACK
        // in autocommit, which at best errors harmlessly and at worst aborts an unrelated
        // transaction a future caller opened on the same connection.
        if (began && !done) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};

// SELECT column order shared by every channel query below.
constexpr const char* kChannelCols =
    "id,playlist_id,name,stream_url,logo_url,group_title,tvg_id,tvg_name,lcn,"
    "is_favourite,dead_status,last_checked_at,sort_order,user_agent,referrer,"
    // v8 (VOD). APPENDED, never inserted: readChannel() reads by ordinal, so inserting a
    // column here would silently shift every field after it.
    "kind,duration_sec,resume_sec,watched,added_at";

// Predicate restricting a channel query to enabled playlists. Disabled playlists
// keep their rows but vanish from every cross-playlist view; channelsByPlaylist()
// deliberately omits it (an explicit "show me exactly this playlist" accessor).
constexpr const char* kEnabledOnly =
    "playlist_id IN (SELECT id FROM playlists WHERE enabled=1)";

// Predicate restricting a query to LIVE channels (schema v8). Used by the two COUNTRY
// queries, for two independent reasons — and the correctness one is the important half:
//
//   • CORRECTNESS: a movie has no country. It carries no tvg-id, and its group_title is a
//     VOD category name. Feeding those to effective_country() invites a false positive —
//     a provider whose categories read "NL - FILMS" would file 43,599 films under the
//     Netherlands and swamp the country tree. Excluding VOD by kind removes the whole class
//     rather than hoping the deny-list keeps up with category naming.
//   • SPEED: measured with `RabbitEarsCli --benchdb` at the owner's real shape (43,599
//     movies beside 442 live channels), effective_country() over every row cost
//     listCountries 0.13 -> 13.01 ms and channelsByCountry 0.15 -> 6.38 ms. Both are
//     per-nav-click / per-keystroke paths, and channelsByCountry is the one that already
//     had to become a SQL scalar to survive 14k channels.
//
// Deliberately NOT applied to searchChannels(): a user searching for a FILM must be able to
// find it, so search keeps the VOD rows. Their price (one LIKE keystroke 0.63 -> 80.00 ms at the
// owner's shape — BACKLOG) is gone for 3+ characters since the v11 channel index (0.2–1 ms,
// docs/CHANNEL_SEARCH.md); 1–2 characters still scan the names.
//
// listGroups() is NOT in that category any more: it went kind-scoped in its own right (via
// listGroupsOfKind(0)) when movies got their own "Movies" nav root, so VOD categories are a
// separate namespace rather than 67 extra siblings in the live tree.
constexpr const char* kLiveOnly = "kind=0";

Channel readChannel(Stmt& q) {
    Channel c;
    c.id = q.intCol(0);
    c.playlistId = q.intCol(1);
    c.name = q.textCol(2);
    c.streamUrl = q.textCol(3);
    c.logoUrl = q.textCol(4);
    c.groupTitle = q.textCol(5);
    c.tvgId = q.textCol(6);
    c.tvgName = q.textCol(7);
    c.lcn = q.optIntCol(8);
    c.favourite = q.intCol(9) != 0;
    c.deadStatus = static_cast<DeadStatus>(q.intCol(10));
    c.lastCheckedAt = q.intCol(11);
    c.sortOrder = static_cast<int>(q.intCol(12));
    c.userAgent = q.textCol(13);
    c.referrer = q.textCol(14);
    // v8. All five carry NOT NULL DEFAULTs, so a row written before v8 reads as a live
    // channel with no duration and no resume point — identical to pre-v8 behaviour.
    c.kind = static_cast<Channel::Kind>(q.intCol(15));
    c.durationSec = static_cast<int>(q.intCol(16));
    c.resumeSec = static_cast<int>(q.intCol(17));
    c.watched = q.intCol(18) != 0;
    c.addedAt = q.intCol(19);
    return c;
}

// SELECT column order shared by every programme query below.
constexpr const char* kProgrammeCols =
    "channel_id,start_utc,stop_utc,title,sub_title,descr,category,episode_num,icon_url";

Programme readProgramme(Stmt& q) {
    Programme p;
    p.channelId = q.textCol(0);
    p.startUtc = q.intCol(1);
    p.stopUtc = q.intCol(2);
    p.title = q.textCol(3);
    p.subTitle = q.textCol(4);
    p.descr = q.textCol(5);
    p.category = q.textCol(6);
    p.episodeNum = q.textCol(7);
    p.iconUrl = q.textCol(8);
    return p;
}

// SELECT column order shared by every scheduled-recording query below.
constexpr const char* kScheduleCols =
    "id,channel_id,channel_name,stream_url,user_agent,referrer,title,start_utc,stop_utc,mux,"
    "status,file_path,created_at,rule_id,episode_key,prog_start_utc";

ScheduledRecording readSchedule(Stmt& q) {
    ScheduledRecording s;
    s.id = q.intCol(0);
    s.channelId = q.textCol(1);
    s.channelName = q.textCol(2);
    s.streamUrl = q.textCol(3);
    s.userAgent = q.textCol(4);
    s.referrer = q.textCol(5);
    s.title = q.textCol(6);
    s.startUtc = q.intCol(7);
    s.stopUtc = q.intCol(8);
    s.mux = q.textCol(9);
    s.status = static_cast<ScheduleStatus>(q.intCol(10));
    s.filePath = q.textCol(11);
    s.createdAt = q.intCol(12);
    s.ruleId = q.intCol(13);  // NULL (pre-v5 / one-off rows) reads back as 0
    s.episodeKey = q.textCol(14);  // '' for manual / pre-v6 rows (no episode dedup)
    s.progStartUtc = q.intCol(15);  // 0 for manual / pre-v7 rows (containment fallback)
    return s;
}

constexpr const char* kRuleCols =
    "id,channel_id,channel_name,title_match,match_kind,enabled,lead_sec,trail_sec,mux,created_at";

RecordingRule readRule(Stmt& q) {
    RecordingRule r;
    r.id = q.intCol(0);
    r.channelId = q.textCol(1);
    r.channelName = q.textCol(2);
    r.titleMatch = q.textCol(3);
    r.match = static_cast<RuleMatch>(q.intCol(4));
    r.enabled = q.intCol(5) != 0;
    r.leadSec = static_cast<int>(q.intCol(6));
    r.trailSec = static_cast<int>(q.intCol(7));
    r.mux = q.textCol(8);
    r.createdAt = q.intCol(9);
    return r;
}

// The grid filters as a SQL fragment, appended to the caller's own WHERE and BEFORE its ORDER BY,
// with the values it needs pushed onto `binds` (in placeholder order, after the caller's own).
//
// Both predicates mirror, exactly, the C++ filtering this replaced:
//   hideDead  — DeadStatus::Dead is 2 (models/Channel.h).
//   categories — keep a row when it is VOD, or has no group at all, or its group is in the set.
//                The first two exemptions are not incidental: the Categories dialog is built from
//                listGroups(), which is LIVE-only and skips blank groups, so neither a movie nor an
//                ungrouped channel can ever be *offered* for ticking — hiding them behind a filter
//                that cannot express them would make them unreachable.
std::string gridWhere(const Database::GridFilter& g, std::vector<std::wstring>& binds) {
    std::string sql;
    if (g.hideDead) sql += " AND dead_status<>2";
    if (!g.categories.empty()) {
        sql += " AND (kind<>0 OR group_title IS NULL OR group_title='' OR group_title IN (";
        for (size_t i = 0; i < g.categories.size(); ++i) {
            sql += (i ? ",?" : "?");
            binds.push_back(g.categories[i]);
        }
        sql += "))";
    }
    return sql;
}

// Appended AFTER the ORDER BY. Guarded on > 0 for two independent reasons: a default-constructed
// GridFilter must reproduce the original SQL byte-for-byte (mac, and every non-grid caller), and
// SQLite's `LIMIT 0` returns ZERO ROWS — so an unguarded append would silently empty every view.
std::string gridLimit(const Database::GridFilter& g) {
    return g.limit > 0 ? " LIMIT " + std::to_string(g.limit) : std::string();
}

std::vector<Channel> runChannelQuery(sqlite3* db, const std::string& sql,
                                     const std::wstring* bindText = nullptr,
                                     std::optional<long long> bindInt = std::nullopt,
                                     const std::vector<std::wstring>* extraBinds = nullptr) {
    std::vector<Channel> out;
    Stmt q(db, sql.c_str());
    if (!q) return out;
    int idx = 1;
    if (bindText) q.bindText(idx++, *bindText);
    if (bindInt) q.bindInt(idx++, *bindInt);
    // The grid-filter values, in the order gridWhere() emitted their placeholders. They come last
    // because gridWhere() appends its fragment after the caller's own predicates.
    if (extraBinds)
        for (const std::wstring& b : *extraBinds) q.bindText(idx++, b);
    while (q.step()) out.push_back(readChannel(q));
    return out;
}

// Country derivation operates on raw UTF-8 bytes: every significant character in the grammar
// is ASCII, and any non-ASCII byte (>= 0x80) simply fails the letter/delimiter tests exactly
// as the equivalent wide-char check would — so byte-wise parsing is semantically identical and
// lets the SQL scalar below run with zero encoding conversions per row.

// The 2-letter country code from an iptv-org-style tvg-id ("<name>.<cc>"): the last
// dot-segment iff it is exactly two ASCII letters, lowercased. "" otherwise.
std::string countryFromTvgIdU8(std::string_view tvgId) {
    const size_t dot = tvgId.find_last_of('.');
    if (dot == std::string_view::npos || dot + 3 != tvgId.size()) return {};
    auto isAlpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
    const char a = tvgId[dot + 1], b = tvgId[dot + 2];
    if (!isAlpha(a) || !isAlpha(b)) return {};
    return std::string{lower(a), lower(b)};
}

// FALLBACK country derivation for Xtream-style playlists, whose channels typically carry an
// opaque/empty tvg-id but prefix the country onto the group-title: "US| NEWS", "US - NEWS",
// "[UK] SPORTS", "|FR| CINEMA", "es:CINE". Accepted shape: optional leading decoration
// (spaces / '|' / '[' / '('), exactly TWO ASCII letters, optional spaces, then an EXPLICIT
// delimiter ('|' '-' ':' ']' ')'). A bare space is deliberately NOT a delimiter — "IT MOVIES"
// is a genre, not Italy, and "TV SHOWS" is not Tuvalu. A short deny-list drops prefixes that
// would otherwise pollute the country list: HD/EN/XX/EX/ON are not assigned ISO 3166 codes
// (EX-YU| and ON-DEMAND are real group idioms), and SD (Sudan) / TV (Tuvalu) are assigned but
// virtually always mean "standard definition" / "television" in a group-title — the sacrifice
// is deliberate. KNOWN-WRONG, kept: "AR|" on pan-Arabic groups reads as Argentina — 'ar' is
// genuinely Argentina on Latino panels, so neither denying nor keeping it is right for both;
// keeping it preserves the true positives. 3+-letter tokens ("USA|", "FRANCE|") are
// structurally rejected; an alpha-3 alias table is a possible follow-up (see Win32/BACKLOG).
// Lowercased 2-letter code, or "" when the title doesn't open with a recognisable prefix.
std::string countryFromGroupTitleU8(std::string_view group) {
    auto isAlpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
    size_t i = 0;
    while (i < group.size() && group[i] == ' ') ++i;
    if (i < group.size() && (group[i] == '|' || group[i] == '[' || group[i] == '(')) ++i;
    while (i < group.size() && group[i] == ' ') ++i;
    if (i + 2 > group.size() || !isAlpha(group[i]) || !isAlpha(group[i + 1])) return {};
    const std::string cc{lower(group[i]), lower(group[i + 1])};
    i += 2;
    while (i < group.size() && group[i] == ' ') ++i;
    if (i >= group.size()) return {};  // just a 2-letter group name — no delimiter, no claim
    const char d = group[i];
    if (d != '|' && d != '-' && d != ':' && d != ']' && d != ')') return {};
    if (cc == "hd" || cc == "sd" || cc == "tv" || cc == "en" || cc == "xx" || cc == "ex" ||
        cc == "on")
        return {};
    return cc;
}

// The country a channel belongs to: the tvg-id suffix is authoritative (machine-authored);
// the group-title prefix is consulted only when the tvg-id yields nothing.
std::string effectiveCountryU8(std::string_view tvgId, std::string_view group) {
    std::string cc = countryFromTvgIdU8(tvgId);
    return cc.empty() ? countryFromGroupTitleU8(group) : cc;
}

// SQL scalar `effective_country(tvg_id, group_title)` — the same rule, callable from SQL so
// channelsByCountry can filter WITHOUT materializing every channel row (the C++-side filter
// this replaces cost ~30 ms per call at 14k channels, and the mac search path evaluates the
// country filter per keystroke). SQLITE_DETERMINISTIC: same inputs, same answer.
void effectiveCountrySqlFn(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc != 2) {
        sqlite3_result_text(ctx, "", 0, SQLITE_STATIC);
        return;
    }
    auto sv = [](sqlite3_value* v) -> std::string_view {
        const auto* p = sqlite3_value_text(v);  // NULL column -> nullptr
        return p ? std::string_view(reinterpret_cast<const char*>(p),
                                    static_cast<size_t>(sqlite3_value_bytes(v)))
                 : std::string_view();
    };
    const std::string cc = effectiveCountryU8(sv(argv[0]), sv(argv[1]));
    sqlite3_result_text(ctx, cc.c_str(), static_cast<int>(cc.size()), SQLITE_TRANSIENT);
}

// canonical_url(stream_url) — see common/core/UrlCanon.h. A NULL argument comes back as NULL
// rather than as "": stream_url is NOT NULL so this cannot happen on the real column, but a
// canonicaliser that turns NULL into a value is exactly the kind of surprise a bulk UPDATE should
// not contain.
void canonicalUrlSqlFn(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc != 1 || sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    const auto* p = sqlite3_value_text(argv[0]);
    const std::string out = canonicalStreamUrlU8(
        p ? std::string_view(reinterpret_cast<const char*>(p),
                             static_cast<size_t>(sqlite3_value_bytes(argv[0])))
          : std::string_view());
    sqlite3_result_text(ctx, out.c_str(), static_cast<int>(out.size()), SQLITE_TRANSIENT);
}

}  // namespace

Database::~Database() { close(); }

void Database::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
    programmeIndexKnown_ = -1;  // belongs to the connection: a reopen re-checks the stamp
}

bool Database::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        lastError_ = err ? wideFromUtf8(err) : L"exec failed";
        sqlite3_free(err);
        return false;
    }
    return true;
}

// Database::defaultDbPath() is platform-specific and lives in the platform layer:
//   Win32/platform/Paths.cpp   (%LOCALAPPDATA%\RabbitEars)
//   mac/platform/Paths.cpp     (~/Library/Application Support/RabbitEars)
// so this file (the shared core) depends only on sqlite3 — no shell32/ole32.

bool Database::open(const std::wstring& path, std::wstring* error, bool upgradeSchema) {
    close();
    upgradeSchema_ = upgradeSchema;
    const std::string utf8 = utf8FromWide(path);
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(utf8.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        lastError_ = db_ ? wideFromUtf8(sqlite3_errmsg(db_)) : L"sqlite3_open_v2 failed";
        if (error) *error = lastError_;
        close();
        return false;
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA foreign_keys=ON;");
    // Wait for a contended write lock instead of failing instantly with SQLITE_BUSY. This matters
    // because most writers here DISCARD the step result (setSetting/setDeadStatus/setFavourite/…
    // all just call stepDone()), so a BUSY is not an error the user ever sees — it is a SILENTLY
    // LOST WRITE. WAL keeps readers off the writer's back, but writers still serialise, and the
    // app already writes from the scheduler tick, the UI, and playlist/EPG import. It is also the
    // prerequisite for any second connection (a future off-thread guide build or link checker).
    exec("PRAGMA busy_timeout=5000;");
    // The country-derivation rule as a SQL scalar (see effectiveCountrySqlFn) — lets the
    // Countries queries filter server-side instead of materializing every channel row.
    sqlite3_create_function(db_, "effective_country", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                            nullptr, &effectiveCountrySqlFn, nullptr, nullptr);
    // canonical_url() — the v9 migration rewrites 410k rows with it, and doing that as one SQL
    // UPDATE rather than a C++ round trip per row is the difference between ~2 s and minutes.
    // ⚠ If this registration ever fails, every v9 statement using it fails to PREPARE — which is
    // exactly the case the migration's per-statement checking exists to catch, because a
    // transaction full of failed statements still COMMITs cleanly.
    sqlite3_create_function(db_, "canonical_url", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                            &canonicalUrlSqlFn, nullptr, nullptr);
    if (!createSchema()) {
        if (error) *error = lastError_;
        close();
        return false;
    }
    return true;
}

bool Database::createSchema() {
    static const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS playlists(
  id                INTEGER PRIMARY KEY,
  name              TEXT NOT NULL,
  source_url        TEXT,
  source_path       TEXT,
  is_url            INTEGER NOT NULL DEFAULT 1,
  added_at          INTEGER NOT NULL,
  last_refreshed_at INTEGER,
  channel_count     INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS channels(
  id              INTEGER PRIMARY KEY,
  playlist_id     INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
  name            TEXT NOT NULL,
  stream_url      TEXT NOT NULL,
  logo_url        TEXT,
  group_title     TEXT,
  tvg_id          TEXT,
  tvg_name        TEXT,
  lcn             INTEGER,
  is_favourite    INTEGER NOT NULL DEFAULT 0,
  dead_status     INTEGER NOT NULL DEFAULT 0,
  last_checked_at INTEGER NOT NULL DEFAULT 0,
  sort_order      INTEGER NOT NULL DEFAULT 0,
  user_agent      TEXT,
  referrer        TEXT
);
CREATE TABLE IF NOT EXISTS settings(
  key   TEXT PRIMARY KEY,
  value TEXT
);
CREATE INDEX IF NOT EXISTS idx_channels_playlist ON channels(playlist_id);
CREATE INDEX IF NOT EXISTS idx_channels_group    ON channels(group_title);
CREATE INDEX IF NOT EXISTS idx_channels_fav      ON channels(is_favourite) WHERE is_favourite=1;
CREATE INDEX IF NOT EXISTS idx_channels_lcn      ON channels(lcn) WHERE lcn IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_channels_tvgid    ON channels(tvg_id);
CREATE INDEX IF NOT EXISTS idx_channels_name     ON channels(name COLLATE NOCASE);
CREATE UNIQUE INDEX IF NOT EXISTS idx_channels_dedupe ON channels(playlist_id, stream_url);
)SQL";
    if (!exec(kSchema)) return false;
    migrate();
    return true;
}

// Incremental, idempotent schema upgrades, gated on PRAGMA user_version.
//   v1: the baseline schema above.
//   v2: playlists.enabled — per-playlist on/off toggle.
//   v3: EPG — playlists.epg_url + the epg_programmes table (XMLTV now/next + guide).
//   v4: scheduled_recordings — the recording scheduler queue.
//   v5: recording_rules (EPG-driven series rules) + scheduled_recordings.rule_id.
//   v6: scheduled_recordings.episode_key — per-episode identity for series-rule episode dedup.
//   v7: scheduled_recordings.prog_start_utc — the unpadded programme start; the padding-proof
//       airing identity for series-rule slot dedup (start_utc moves when a rule's lead is edited).
//   v8: channels.kind/duration_sec/resume_sec/watched/added_at — VOD. A movie is a channel row
//       with kind=1, so it inherits the grid, search, favourites and playback path; every column
//       defaults to the live-TV answer, so pre-v8 rows and older builds are unaffected.
//   v9: channels.stream_url rewritten to its CANONICAL spelling, merging the rows that become
//       equal. The first step here that rewrites existing DATA rather than adding columns — see
//       canonicalizeStreamUrls().
//   v10: two empty FTS5 tables for the TV Guide's programme search — see
//       createProgrammeSearchTables() and docs/EPG_SEARCH.md.
//   v11: the channel search index — an FTS5 table over channels.name kept in step by triggers —
//       see createChannelSearchIndex() and docs/CHANNEL_SEARCH.md.
// A fresh DB starts at user_version 0 (kSchema built the v1 shape), an existing
// 0.1.x DB is at 1, a 0.1.9+ DB at 2; each open applies whatever steps are missing.
void Database::migrate() {
    long long v = 0;
    {
        Stmt q(db_, "PRAGMA user_version");
        if (q && q.step()) v = q.intCol(0);
    }
    schemaVersion_ = static_cast<int>(v);
    if (v >= 11) return;  // newest schema; bump in lockstep with the highest step below
    // A worker's second connection takes the version as it is (open()'s upgradeSchema).
    if (!upgradeSchema_) return;
    if (v == 10) {
        createChannelSearchIndex();
        return;
    }
    // ⚠ A v9 database goes STRAIGHT on to v10 (and v11). The v2–v9 block below has no per-step
    // version gate: it rewrites user_version from the column checks (8 at most) and re-runs v9's
    // channel-URL rewrite whenever v8's columns exist — so letting a v9 database into it would redo
    // that rewrite (seconds on a large library: the original v9 run took 6.6 s on the owner's) and set
    // user_version back to 8 — for good, if the re-run then failed.
    if (v == 9) {
        if (createProgrammeSearchTables()) createChannelSearchIndex();
        return;
    }

    // v2: playlists.enabled.
    if (!hasColumn("playlists", "enabled"))
        exec("ALTER TABLE playlists ADD COLUMN enabled INTEGER NOT NULL DEFAULT 1");

    // v3: EPG storage. epg_url is an ALTER; epg_programmes is a fresh playlist-scoped
    // table (ON DELETE CASCADE, exactly like channels).
    if (!hasColumn("playlists", "epg_url"))
        exec("ALTER TABLE playlists ADD COLUMN epg_url TEXT");
    exec(
        "CREATE TABLE IF NOT EXISTS epg_programmes("
        "  id          INTEGER PRIMARY KEY,"
        "  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
        "  channel_id  TEXT NOT NULL,"
        "  start_utc   INTEGER NOT NULL,"
        "  stop_utc    INTEGER NOT NULL,"
        "  title       TEXT NOT NULL,"
        "  sub_title   TEXT,"
        "  descr       TEXT,"
        "  category    TEXT,"
        "  episode_num TEXT,"
        "  icon_url    TEXT"
        ");");
    exec("CREATE INDEX IF NOT EXISTS idx_epg_lookup "
         "ON epg_programmes(playlist_id, channel_id, start_utc);");

    // v4: the recording-scheduler queue. Self-contained rows (the stream URL + hints are
    // captured at schedule time), standalone (not playlist-scoped — a schedule outlives
    // its source playlist).
    exec(
        "CREATE TABLE IF NOT EXISTS scheduled_recordings("
        "  id           INTEGER PRIMARY KEY,"
        "  channel_id   TEXT,"
        "  channel_name TEXT NOT NULL,"
        "  stream_url   TEXT NOT NULL,"
        "  user_agent   TEXT,"
        "  referrer     TEXT,"
        "  title        TEXT,"
        "  start_utc    INTEGER NOT NULL,"
        "  stop_utc     INTEGER NOT NULL,"
        "  mux          TEXT NOT NULL DEFAULT 'ts',"
        "  status       INTEGER NOT NULL DEFAULT 0,"
        "  file_path    TEXT,"
        "  created_at   INTEGER NOT NULL"
        ");");
    exec("CREATE INDEX IF NOT EXISTS idx_sched_start ON scheduled_recordings(start_utc);");

    // v5: EPG-driven "series" rules. A rule is a recipe (channel + title pattern); core/
    // RecordingRules expands it against epg_programmes into ordinary scheduled_recordings
    // rows, so the scheduler gains no new timing logic. rule_id back-links a generated row
    // to its rule (NULL/0 == a one-off schedule). Deliberately NOT a foreign key: rule
    // deletion is handled explicitly in deleteRule() (drop only the still-Pending rows), so
    // a completed recording keeps its provenance even after its rule is gone.
    exec(
        "CREATE TABLE IF NOT EXISTS recording_rules("
        "  id           INTEGER PRIMARY KEY,"
        "  channel_id   TEXT,"                        // tvg-id; NULL/'' == any channel
        "  channel_name TEXT,"
        "  title_match  TEXT NOT NULL,"
        "  match_kind   INTEGER NOT NULL DEFAULT 0,"  // 0 = exact, 1 = contains
        "  enabled      INTEGER NOT NULL DEFAULT 1,"
        "  lead_sec     INTEGER NOT NULL DEFAULT 0,"
        "  trail_sec    INTEGER NOT NULL DEFAULT 0,"
        "  mux          TEXT NOT NULL DEFAULT 'ts',"
        "  created_at   INTEGER NOT NULL"
        ");");
    if (!hasColumn("scheduled_recordings", "rule_id"))
        exec("ALTER TABLE scheduled_recordings ADD COLUMN rule_id INTEGER");
    exec("CREATE INDEX IF NOT EXISTS idx_sched_rule ON scheduled_recordings(rule_id, start_utc);");

    // v6: episode_key on scheduled_recordings — a stable per-episode identity (from the EPG
    // <episode-num>, else <sub-title>) set by core/RecordingRules, so a series rule skips a REPEAT
    // airing of an episode it already queued/recorded, not merely a duplicate of the same airing
    // slot. Empty for manual and pre-v6 rows: they just don't participate in episode dedup.
    if (!hasColumn("scheduled_recordings", "episode_key"))
        exec("ALTER TABLE scheduled_recordings ADD COLUMN episode_key TEXT");

    // v7: prog_start_utc on scheduled_recordings — the programme's UNPADDED start, set by
    // core/RecordingRules. start_utc is the PADDED recording start, which moves when a rule's
    // lead is edited, so it cannot identify the airing across edits: a mid-recording lead edit
    // re-created the airing as a duplicate Pending row that rotted into a phantom Missed, and
    // resurrected Cancelled tombstones. (chan, prog_start_utc) is the padding-proof identity.
    // 0 for manual and pre-v7 rows: those fall back to a containment heuristic in the expander.
    if (!hasColumn("scheduled_recordings", "prog_start_utc"))
        exec("ALTER TABLE scheduled_recordings ADD COLUMN prog_start_utc INTEGER NOT NULL DEFAULT 0");

    // v8: VOD on channels — kind (0 live / 1 movie / 2 episode), plus duration, resume
    // position, watched and the provider's "added" timestamp. A movie is a channel row, not a
    // second table, so it inherits the grid, search, favourites and playback path unchanged.
    //
    // Every column is NOT NULL with a default that IS the live-TV answer, which is what makes
    // this safe in both directions: an existing row becomes a live channel with no VOD data,
    // and an OLDER build (including the macOS app, which shares this file) opening a v8 DB
    // takes the `v >= 7` early return, ignores the extra columns, and is unharmed.
    // ⚠ Each ALTER is guarded INDIVIDUALLY, and the v8 probe below checks ALL FIVE columns.
    // Wrapping the five in one `if (!hasColumn("channels","kind"))` was wrong in a way that only
    // appears after a failure, and then permanently: if the sequence died partway — the disk
    // filling between two statements is enough, and exec() does not report failure — then `kind`
    // would exist, so every later open would skip the remaining four forever, while `haveV8`
    // (which asked the same single question) still latched `user_version=8`. Since kChannelCols
    // now names all twenty columns, EVERY channel query would then fail to prepare, and
    // runChannelQuery returns an empty vector on a prepare failure: the user's entire library
    // would come back EMPTY, silently, with no error and no way to retry.
    //
    // v2..v7 never needed this because each of those steps adds exactly ONE column, so guarding
    // on it is the same question as "did this step land". v8 is the first multi-column step, and
    // it quietly broke the invariant the comment below promises.
    if (!hasColumn("channels", "kind"))
        exec("ALTER TABLE channels ADD COLUMN kind INTEGER NOT NULL DEFAULT 0");
    if (!hasColumn("channels", "duration_sec"))
        exec("ALTER TABLE channels ADD COLUMN duration_sec INTEGER NOT NULL DEFAULT 0");
    if (!hasColumn("channels", "resume_sec"))
        exec("ALTER TABLE channels ADD COLUMN resume_sec INTEGER NOT NULL DEFAULT 0");
    if (!hasColumn("channels", "watched"))
        exec("ALTER TABLE channels ADD COLUMN watched INTEGER NOT NULL DEFAULT 0");
    if (!hasColumn("channels", "added_at"))
        exec("ALTER TABLE channels ADD COLUMN added_at INTEGER NOT NULL DEFAULT 0");
    // TWO partial indexes, one per side of the discriminator, because the two directions
    // have different users and neither index serves the other:
    //   kind<>0 — "show me the VOD" (the future poster/series views).
    //   kind=0  — "show me the live channels", which is the one that MATTERS TODAY. The
    //             country queries walk every row and evaluate effective_country() on it;
    //             measured with `--benchdb` at the owner's shape (43,599 movies beside 442
    //             live), that took listCountries 0.13 -> 13.01 ms. `kind=0` as a plain
    //             predicate only halved it, because it still scanned 44k rows to find 442.
    //             The partial index holds ONLY the live rowids, so the scan is 442 entries.
    // A live-only library (every existing user) pays for neither: both are partial, and the
    // kind<>0 index is empty until a VOD sync runs.
    // The VOD side indexes (kind, group_title) rather than kind alone: the Movies root's two
    // hot queries are "DISTINCT group_title WHERE kind=1" (the category list) and "WHERE
    // kind=1 AND group_title=?" (one category), and a composite serves both — the first as an
    // index-only scan. On kind alone the category list cost 8.81 ms at 43,599 movies.
    exec("DROP INDEX IF EXISTS idx_channels_kind;");  // superseded by the composite below
    exec("CREATE INDEX IF NOT EXISTS idx_channels_vod ON channels(kind, group_title) WHERE kind<>0;");
    exec("CREATE INDEX IF NOT EXISTS idx_channels_live ON channels(kind) WHERE kind=0;");

    // Advance user_version to reflect exactly what actually landed, so a partial
    // failure retries the missing step next open instead of skipping it. (hasColumn on a
    // column of a table doubles as a table-exists check — table_info is empty if absent.)
    const bool haveV2 = hasColumn("playlists", "enabled");
    const bool haveV3 = haveV2 && hasColumn("playlists", "epg_url");
    const bool haveV4 = haveV3 && hasColumn("scheduled_recordings", "id");
    const bool haveV5 =
        haveV4 && hasColumn("recording_rules", "id") && hasColumn("scheduled_recordings", "rule_id");
    const bool haveV6 = haveV5 && hasColumn("scheduled_recordings", "episode_key");
    const bool haveV7 = haveV6 && hasColumn("scheduled_recordings", "prog_start_utc");
    // ALL FIVE v8 columns, not just the first. With the ALTERs individually guarded above, a
    // failure on any one of them leaves the others applied — so asking about `kind` alone (or
    // about `added_at` alone) would still report v8 for a half-migrated table. Five extra
    // table_info reads at open is nothing; latching v8 over a missing column is unrecoverable.
    const bool haveV8 = haveV7 && hasColumn("channels", "kind") &&
                        hasColumn("channels", "duration_sec") &&
                        hasColumn("channels", "resume_sec") && hasColumn("channels", "watched") &&
                        hasColumn("channels", "added_at");
    if (haveV8) exec("PRAGMA user_version=8");
    else if (haveV7) exec("PRAGMA user_version=7");
    else if (haveV6) exec("PRAGMA user_version=6");
    else if (haveV5) exec("PRAGMA user_version=5");
    else if (haveV4) exec("PRAGMA user_version=4");
    else if (haveV3) exec("PRAGMA user_version=3");
    else if (haveV2) exec("PRAGMA user_version=2");
    schemaVersion_ = haveV8 ? 8 : haveV7 ? 7 : haveV6 ? 6 : haveV5 ? 5 : haveV4 ? 4
                              : haveV3   ? 3
                              : haveV2   ? 2
                                         : 1;

    // v9 — the DATA migration. Gated on haveV8 because it reads channels.kind: running it against
    // a table that never got the v8 columns would fail every statement, and "every statement
    // failed" is indistinguishable from "there was nothing to do" at the COMMIT.
    //
    // It sets schemaVersion_ = 9 itself, and ONLY when the work provably landed. Everything
    // downstream that writes a canonical URL is gated on that, so a failure here degrades to the
    // pre-v9 literal behaviour rather than to the destructive one.
    if (haveV8 && canonicalizeStreamUrls()) schemaVersion_ = 9;

    // v10 — programme search; v11 — channel search. Each gated on the step before having landed in
    // this same open, so the version only ever advances one proven step at a time.
    if (schemaVersion_ == 9) createProgrammeSearchTables();
    if (schemaVersion_ == 10) createChannelSearchIndex();
}

// ---------------------------------------------------------------------------
// v11 — the channel search index (docs/CHANNEL_SEARCH.md): an external-content FTS5 trigram table
// over channels.name ("quebec" finds "TVA QUÉBEC", a substring, case- and accent-blind), built here
// from the rows already there and kept in step by three triggers.
//
// TRIGGERS, unlike v10 — the owner's decision (2026-09-25), with its price stated: channels are
// written by several paths on both platforms (a playlist add on the UI thread, the VOD sync on its
// own connection, retirement, a playlist delete's cascade), and a trigger keeps every one of them in
// step with no caller involved and nothing to go stale. It costs ~5 ms per 1,000 channel rows written
// (+2 s on a 410k-row playlist, measured), and the build below ~4 s ONCE on the owner's 410k
// channels. THE PRICE: a build WITHOUT FTS5 — every Windows release through 0.2.18, mac through
// 0.2.17 — can no longer insert, update or delete channels in this database ("no such module:
// fts5"): installing one of those over this breaks adding, refreshing and deleting playlists (the
// delete silently — its statement fails to prepare) and the VOD sync; reads, favourites and dead-link
// marks still work. 0.2.19+ has FTS5; auto-update only moves forward.
//
// The update trigger fires only when the name actually changed: bulkInsertChannels' upsert rewrites
// name=excluded.name on every row of a refresh. Everything — table, triggers, build, version — in ONE
// transaction: a failure leaves v10 with NO triggers (so channel writes keep working) and search on
// LIKE; the next open retries.
bool Database::createChannelSearchIndex() {
    Tx tx(db_);
    if (!tx) return false;
    // From clean, whatever an earlier life of this database left: the index is rebuilt below anyway,
    // and IF NOT EXISTS would keep a stale trigger body, or an older FTS5 channels_fts with other
    // columns that the name-only delete trigger would then corrupt. An ORDINARY table squatting on
    // the name is not dropped — the CREATE below fails on it, and v11 waits (it is not ours to drop).
    if (!exec("DROP TRIGGER IF EXISTS channels_fts_ai") || !exec("DROP TRIGGER IF EXISTS channels_fts_ad") ||
        !exec("DROP TRIGGER IF EXISTS channels_fts_au"))
        return false;
    if (channelSearchTableExists() && !exec("DROP TABLE channels_fts")) return false;
    if (!exec("CREATE VIRTUAL TABLE channels_fts USING fts5("
              "name, content='channels', content_rowid='id', tokenize='trigram remove_diacritics 1')"))
        return false;
    if (!channelSearchTableExists()) return false;  // belt and braces, before any trigger writes into it
    if (!exec("CREATE TRIGGER channels_fts_ai AFTER INSERT ON channels BEGIN"
              " INSERT INTO channels_fts(rowid, name) VALUES (new.id, new.name); END") ||
        !exec("CREATE TRIGGER channels_fts_ad AFTER DELETE ON channels BEGIN"
              " INSERT INTO channels_fts(channels_fts, rowid, name) VALUES ('delete', old.id, old.name); END") ||
        !exec("CREATE TRIGGER channels_fts_au AFTER UPDATE OF name ON channels"
              " WHEN old.name IS NOT new.name BEGIN"
              " INSERT INTO channels_fts(channels_fts, rowid, name) VALUES ('delete', old.id, old.name);"
              " INSERT INTO channels_fts(rowid, name) VALUES (new.id, new.name); END"))
        return false;
    if (!exec("INSERT INTO channels_fts(channels_fts) VALUES ('rebuild')")) return false;
    if (!exec("PRAGMA user_version=11")) return false;  // transactional: rolls back with the rest
    if (!tx.commit()) return false;
    schemaVersion_ = 11;
    return true;
}

bool Database::channelSearchTableExists() {
    Stmt q(db_, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='channels_fts'"
                " AND sql LIKE 'CREATE VIRTUAL TABLE%USING fts5(%'");
    return q && q.step() && q.intCol(0) == 1;
}

// ---------------------------------------------------------------------------
// v10 — two EMPTY external-content FTS5 tables for programme search (docs/EPG_SEARCH.md §3).
//
// Deliberately NO triggers: a trigger would make every write to epg_programmes depend on FTS5, so a
// build without it (every release through 0.2.18, and an older mac build) would fail every guide
// refresh with "no such module". Without triggers such a build still opens, refreshes and
// integrity-checks this database (measured with an FTS5-less build of this amalgamation) — it only
// leaves the index stale, which the stamp catches. Nothing is indexed here, so the upgrade is
// instant, and nothing in this class builds the index on its own: a CALLER runs
// rebuildProgrammeIndex() — Win32 after each guide refresh, and before the first search whenever
// programmeSearchState() says NeedsRebuild. Until then searchProgrammes() answers with LIKE.
//
// Both tables and the version in ONE transaction: a failure (SQLITE_BUSY, a full disk) leaves v9
// and nothing half-made, and the next open retries; search answers with LIKE meanwhile.
bool Database::createProgrammeSearchTables() {
    Tx tx(db_);
    if (!tx) return false;
    // remove_diacritics: trigram accepts 1 (its values 1 and 2 select the same folding), unicode61
    // takes 2 (also folds the diacritics of letters that carry several).
    if (!exec("CREATE VIRTUAL TABLE IF NOT EXISTS epg_fts_title USING fts5("
              "title, content='epg_programmes', content_rowid='id',"
              " tokenize='trigram remove_diacritics 1')"))
        return false;
    if (!exec("CREATE VIRTUAL TABLE IF NOT EXISTS epg_fts_descr USING fts5("
              "descr, content='epg_programmes', content_rowid='id',"
              " tokenize='unicode61 remove_diacritics 2')"))
        return false;
    // IF NOT EXISTS succeeds over an ordinary table of the same name, so check what is there.
    if (!programmeSearchTablesExist()) return false;
    // Tables just made are empty: a stamp left from an earlier life of this database (a downgrade and
    // back) must not vouch for them, or search would call an empty index Ready.
    if (!exec("DELETE FROM settings WHERE key='epg_fts_stamp'")) return false;
    if (!exec("PRAGMA user_version=10")) return false;  // transactional: rolls back with the tables
    if (!tx.commit()) return false;
    schemaVersion_ = 10;
    return true;
}

bool Database::programmeSearchTablesExist() {
    Stmt q(db_, "SELECT COUNT(*) FROM sqlite_master WHERE type='table'"
                " AND name IN ('epg_fts_title','epg_fts_descr')"
                " AND sql LIKE 'CREATE VIRTUAL TABLE%USING fts5(%'");
    return q && q.step() && q.intCol(0) == 2;
}

// ---------------------------------------------------------------------------
// v9 — canonicalise channels.stream_url and merge the rows that become equal.
//
// The problem, measured rather than imagined (Win32/BACKLOG.md): a provider's m3u_plus emits
// `http://host:80/movie/U/P/1.mp4` while the Xtream VOD sync constructs `http://host/movie/...`
// from a port-less playlist URL. `idx_channels_dedupe` is UNIQUE on the LITERAL string, so those
// are two channels, and the first sync stored 43,606 movies beside the 43,599 the m3u already had.
//
// Three properties this is built around, in the order they matter:
//
//  1. IT CANNOT LIE ABOUT SUCCEEDING. `Tx::commit()` reports only whether COMMIT ran — and `Stmt`
//     swallows a prepare failure while `exec()`'s bool is ignored everywhere else in migrate() —
//     so a transaction in which EVERY statement failed still commits cleanly. That matters more
//     here than anywhere else in this file, because the caller latches the version off the return
//     value and the canonicalising write path is gated on it. So every statement is checked, and
//     `PRAGMA user_version=9` is written INSIDE the transaction: user_version lives in the database
//     header and is fully transactional, so it rolls back WITH the data instead of being a separate
//     write that can disagree with it.
//  2. IT CANNOT LEAVE DUPLICATES BEHIND. The UNIQUE dedupe index is dropped for the bulk rewrite
//     and recreated at the end, inside the same transaction. That is a 4x speedup (7.9 s -> 1.9 s
//     on 366k rows, measured) but the real reason is that the recreate FAILS if the merge missed a
//     collision — turning "the merge was incomplete" from a silent corruption into a rollback.
//  3. IT KEEPS THE USER'S DATA. The survivor inherits favourite / LCN / resume / watched /
//     dead-status from every row it absorbs. On the owner's library no collision involved any of
//     those, so this path ships having never met a real conflict — which is exactly why it is
//     pinned by a --selftest fixture where the LOSER carries all of it.
bool Database::canonicalizeStreamUrls() {
    if (!db_) return false;
    Tx tx(db_);
    // No transaction, no guarantees: everything this function claims about atomicity and about
    // user_version rolling back with the data is only true inside one. Retry next open.
    if (!tx) return false;

    // Stage every row's canonical key once. A temp table + index rather than a correlated
    // subquery: the grouping below is otherwise quadratic over 454k rows.
    if (!exec("CREATE TEMP TABLE IF NOT EXISTS _canon("
              "id INTEGER PRIMARY KEY, pid INTEGER, cu TEXT, kind INTEGER)") ||
        !exec("DELETE FROM _canon") ||
        !exec("INSERT INTO _canon(id,pid,cu,kind) "
              "SELECT id, playlist_id, canonical_url(stream_url), kind FROM channels") ||
        !exec("CREATE INDEX IF NOT EXISTS _canon_key ON _canon(pid, cu)"))
        return false;

    // Groups that will collide once the rewrite lands. Note this is over the FULL post-canonical
    // key space, so it catches a canonicalised row colliding with one that was ALREADY canonical
    // (the m3u/sync case) just as well as two canonicalised rows colliding with each other.
    struct Group { long long pid; std::wstring cu; };
    std::vector<Group> groups;
    {
        Stmt q(db_, "SELECT pid, cu FROM _canon GROUP BY pid, cu HAVING COUNT(*)>1");
        if (!q) return false;
        while (q.step()) groups.push_back({q.intCol(0), q.textCol(1)});
    }

    std::vector<long long> losers;
    std::vector<std::pair<long long, long long>> remap;  // loser id -> survivor id
    for (const Group& g : groups) {
        // Survivor = highest kind, then lowest id. Deterministic (so a re-run after a rollback
        // makes the same choice) and it prefers the Movie row, which is the one carrying the
        // provider's added_at and the correct kind.
        struct Row {
            long long id, kind, fav, resume, watched, duration, added, dead, checked;
            bool hasLcn; long long lcn;
        };
        std::vector<Row> rows;
        {
            Stmt q(db_,
                   "SELECT c.id,c.kind,c.is_favourite,c.resume_sec,c.watched,c.duration_sec,"
                   "c.added_at,c.dead_status,c.last_checked_at,c.lcn IS NOT NULL,COALESCE(c.lcn,0) "
                   "FROM _canon k JOIN channels c ON c.id=k.id "
                   "WHERE k.pid=?1 AND k.cu=?2 ORDER BY c.kind DESC, c.id ASC");
            if (!q) return false;
            q.bindInt(1, g.pid);
            q.bindText(2, g.cu);
            while (q.step())
                rows.push_back({q.intCol(0), q.intCol(1), q.intCol(2), q.intCol(3), q.intCol(4),
                                q.intCol(5), q.intCol(6), q.intCol(7), q.intCol(8),
                                q.intCol(9) != 0, q.intCol(10)});
        }
        if (rows.size() < 2) continue;  // raced away, or the group query lied — nothing to merge

        // Fold every loser's user data into the survivor. MAX throughout, because for all of these
        // "someone set it" beats "nobody did"; dead_status travels WITH last_checked_at so a stale
        // verdict cannot overwrite a fresher one.
        Row s = rows[0];
        long long dead = rows[0].dead, checked = rows[0].checked;
        for (size_t i = 1; i < rows.size(); ++i) {
            const Row& r = rows[i];
            s.fav = std::max(s.fav, r.fav);
            s.resume = std::max(s.resume, r.resume);
            s.watched = std::max(s.watched, r.watched);
            s.duration = std::max(s.duration, r.duration);
            s.added = std::max(s.added, r.added);
            s.kind = std::max(s.kind, r.kind);
            if (!s.hasLcn && r.hasLcn) { s.hasLcn = true; s.lcn = r.lcn; }
            if (r.checked > checked) { checked = r.checked; dead = r.dead; }
            losers.push_back(r.id);
            remap.emplace_back(r.id, s.id);
        }
        Stmt u(db_,
               "UPDATE channels SET is_favourite=?1, resume_sec=?2, watched=?3, duration_sec=?4,"
               " added_at=?5, kind=?6, dead_status=?7, last_checked_at=?8, lcn=?9 WHERE id=?10");
        if (!u) return false;
        u.bindInt(1, s.fav);
        u.bindInt(2, s.resume);
        u.bindInt(3, s.watched);
        u.bindInt(4, s.duration);
        u.bindInt(5, s.added);
        u.bindInt(6, s.kind);
        u.bindInt(7, dead);
        u.bindInt(8, checked);
        u.bindOptInt(9, s.hasLcn ? std::optional<int>(static_cast<int>(s.lcn)) : std::nullopt);
        u.bindInt(10, s.id);
        if (u.stepDone() != SQLITE_DONE) return false;
    }

    if (!losers.empty()) {
        Stmt del(db_, "DELETE FROM channels WHERE id=?1");
        if (!del) return false;
        for (long long id : losers) {
            del.reset();
            del.bindInt(1, id);
            if (del.stepDone() != SQLITE_DONE) return false;
        }
        // A persisted last_channel_id naming a row we just deleted would silently stop resuming —
        // channelById() returns nullopt and the caller shrugs, so the user simply never gets their
        // last channel back and nothing says why. Point it at the survivor of its own group.
        // Deliberately done in C++ off the map built above: the SQL equivalent is a correlated
        // triple-subquery that nobody could review, and this runs once over a 1-row lookup.
        if (auto lc = getSetting(L"last_channel_id"); lc && !lc->empty()) {
            // std::wcstoll, not _wtoll: this file is compiled into RabbitEarsCore, which the macOS
            // app links too, and _wtoll is an MSVC CRT extension that does not exist there.
            const long long was = std::wcstoll(lc->c_str(), nullptr, 10);
            for (const auto& [loser, survivor] : remap)
                if (loser == was) {
                    setSetting(L"last_channel_id", std::to_wstring(survivor));
                    break;
                }
        }
    }

    // The bulk rewrite, with the UNIQUE index out of the way — see property 2 above. Predicated on
    // an actual difference so a re-run over an already-canonical table touches nothing.
    if (!exec("DROP INDEX IF EXISTS idx_channels_dedupe") ||
        !exec("UPDATE channels SET stream_url=canonical_url(stream_url) "
              "WHERE stream_url<>canonical_url(stream_url)") ||
        !exec("CREATE UNIQUE INDEX idx_channels_dedupe ON channels(playlist_id, stream_url)"))
        return false;

    // Every other bulk mutator recomputes this and says why: without it the nav tree keeps showing
    // the pre-merge count until some later import happens to correct it — and that count is the one
    // number a user can check to see whether the merge did anything. Unscoped: any playlist can
    // have lost rows.
    if (!exec("UPDATE playlists SET channel_count="
              "(SELECT COUNT(*) FROM channels WHERE playlist_id=playlists.id)"))
        return false;

    if (!exec("DELETE FROM _canon")) return false;
    if (!exec("PRAGMA user_version=9")) return false;  // transactional: rolls back with the data
    return tx.commit();
}

bool Database::hasColumn(const char* table, const char* column) {
    // `table` is always a compile-time literal here, so the concatenation is injection-safe.
    Stmt q(db_, (std::string("PRAGMA table_info(") + table + ")").c_str());
    if (!q) return false;
    const std::wstring want = wideFromUtf8(column);
    while (q.step())
        if (q.textCol(1) == want) return true;  // column 1 of table_info is the name
    return false;
}

// ---- Playlists -------------------------------------------------------------

long long Database::addPlaylist(const std::wstring& name, const std::wstring& source, bool isUrl,
                                long long nowEpoch, const std::wstring& epgUrl) {
    Stmt q(db_,
           "INSERT INTO playlists(name,source_url,source_path,is_url,added_at,channel_count,epg_url) "
           "VALUES(?,?,?,?,?,0,?)");
    if (!q) return 0;
    q.bindText(1, name);
    if (isUrl) { q.bindText(2, source); q.bindNull(3); }
    else { q.bindNull(2); q.bindText(3, source); }
    q.bindInt(4, isUrl ? 1 : 0);
    q.bindInt(5, nowEpoch);
    q.bindText(6, epgUrl);
    if (q.stepDone() != SQLITE_DONE) return 0;
    return sqlite3_last_insert_rowid(db_);
}

std::vector<Playlist> Database::listPlaylists() {
    std::vector<Playlist> out;
    Stmt q(db_,
           "SELECT id,name,source_url,source_path,is_url,added_at,last_refreshed_at,channel_count,"
           "enabled,epg_url FROM playlists ORDER BY added_at");
    if (!q) return out;
    while (q.step()) {
        Playlist p;
        p.id = q.intCol(0);
        p.name = q.textCol(1);
        p.sourceUrl = q.textCol(2);
        p.sourcePath = q.textCol(3);
        p.isUrl = q.intCol(4) != 0;
        p.addedAt = q.intCol(5);
        p.lastRefreshedAt = q.intCol(6);
        p.channelCount = static_cast<int>(q.intCol(7));
        p.enabled = q.intCol(8) != 0;
        p.epgUrl = q.textCol(9);
        out.push_back(std::move(p));
    }
    return out;
}

void Database::deletePlaylist(long long playlistId) {
    Stmt q(db_, "DELETE FROM playlists WHERE id=?");
    if (!q) return;
    q.bindInt(1, playlistId);
    q.stepDone();
    programmeIndexKnown_ = -1;  // ON DELETE CASCADE removed its programmes: re-check the stamp
}

void Database::renamePlaylist(long long playlistId, const std::wstring& name) {
    Stmt q(db_, "UPDATE playlists SET name=? WHERE id=?");
    if (!q) return;
    q.bindText(1, name);
    q.bindInt(2, playlistId);
    q.stepDone();
}

void Database::setPlaylistEpgUrl(long long playlistId, const std::wstring& epgUrl) {
    Stmt q(db_, "UPDATE playlists SET epg_url=? WHERE id=?");
    if (!q) return;
    q.bindText(1, epgUrl);
    q.bindInt(2, playlistId);
    q.stepDone();
}

void Database::setPlaylistEnabled(long long playlistId, bool enabled) {
    Stmt q(db_, "UPDATE playlists SET enabled=? WHERE id=?");
    if (!q) return;
    q.bindInt(1, enabled ? 1 : 0);
    q.bindInt(2, playlistId);
    q.stepDone();
}

// ---- Channels --------------------------------------------------------------

int Database::bulkInsertChannels(long long playlistId, const std::vector<ParsedChannel>& channels,
                                 long long nowEpoch) {
    if (!db_) return 0;
    Tx tx(db_);
    if (!tx) return 0;  // contended writer: report nothing imported rather than a partial batch
    // v8 adds kind + added_at. Both are updated ON CONFLICT only when the incoming row
    // ASSERTS a value, never downward to the default — the two directions are NOT
    // symmetric. An Xtream `m3u_plus` playlist already carries /movie/USER/PASS/id.ext
    // rows, and the VOD sync constructs the identical URL, so they collide on
    // idx_channels_dedupe BY DESIGN. But ParsedChannel from the M3U parser cannot express
    // `kind` — it defaults to Live — so a plain "kind=excluded.kind" would let a routine
    // playlist refresh silently revert every movie to a live channel, zero the provider's
    // added_at, and (worst) leave zero kind=1 rows so retireMissingChannels becomes a
    // permanent no-op and the library grows forever. "The M3U didn't say" is not "it is
    // Live".
    //
    // resume_sec / watched / duration_sec / is_favourite / lcn / dead_status are absent
    // from the SET list entirely: those are the USER's data, not the provider's, and a
    // refresh must never discard how far someone got into a film.
    Stmt ins(db_,
             "INSERT INTO channels("
             "playlist_id,name,stream_url,logo_url,group_title,tvg_id,tvg_name,lcn,sort_order,"
             "user_agent,referrer,kind,added_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) "
             "ON CONFLICT(playlist_id,stream_url) DO UPDATE SET "
             "name=excluded.name,logo_url=excluded.logo_url,group_title=excluded.group_title,"
             "tvg_id=excluded.tvg_id,tvg_name=excluded.tvg_name,sort_order=excluded.sort_order,"
             "user_agent=excluded.user_agent,referrer=excluded.referrer,"
             "kind=CASE WHEN excluded.kind<>0 THEN excluded.kind ELSE kind END,"
             "added_at=CASE WHEN excluded.added_at<>0 THEN excluded.added_at ELSE added_at END");
    if (!ins) return 0;
    int n = 0, order = 0;
    for (const ParsedChannel& c : channels) {
        if (!c.isValid()) continue;
        ins.reset();
        ins.bindInt(1, playlistId);
        ins.bindText(2, c.name);
        // 🔴 CANONICAL, and gated on the v9 migration having actually landed. Storing canonical
        // URLs into a table whose existing rows are still literal is the destructive combination:
        // every incoming row would miss its stored counterpart on idx_channels_dedupe and insert a
        // duplicate of the ENTIRE library (410,147 rows on the owner's, measured). If v9 did not
        // land, this keeps the old literal behaviour — which merely duplicates movies, and which
        // the next successful open repairs.
        ins.bindText(3, schemaVersion_ >= 9 ? canonicalStreamUrl(c.streamUrl) : c.streamUrl);
        ins.bindText(4, c.logoUrl);
        ins.bindText(5, c.groupTitle);
        ins.bindText(6, c.tvgId);
        ins.bindText(7, c.tvgName);
        ins.bindOptInt(8, c.chno >= 0 ? std::optional<int>(c.chno) : std::nullopt);
        ins.bindInt(9, order++);
        ins.bindText(10, c.userAgent);
        ins.bindText(11, c.referrer);
        ins.bindInt(12, static_cast<long long>(c.kind));
        ins.bindInt(13, c.addedAt);
        if (ins.stepDone() == SQLITE_DONE) ++n;
    }
    {
        Stmt upd(db_,
                 "UPDATE playlists SET channel_count=(SELECT COUNT(*) FROM channels WHERE "
                 "playlist_id=?1), last_refreshed_at=?2 WHERE id=?1");
        if (upd) {
            upd.bindInt(1, playlistId);
            upd.bindInt(2, nowEpoch);
            upd.stepDone();
        }
    }
    // A failed COMMIT rolled the whole batch back, so NOTHING was imported. Returning `n` here
    // would tell the caller "added 43,599 channels" about rows that are not in the table — the
    // same lie retireMissingChannels' -1 exists to prevent, on the path a user sees most often.
    if (!tx.commit()) return 0;
    return n;
}

// Retire VOD rows the provider has dropped. A catalogue churns constantly, so without this
// the library only ever GROWS: a film removed upstream keeps a row whose URL 404s forever.
//
// Scoped to (playlist_id, kind) so a VOD sync can never touch live channels — the failure
// this guards against is a partial or empty VOD response wiping someone's TV list, the same
// class of disaster the dead-link checker's "discard the whole sweep unless enough of it
// reached a server" rule exists to prevent. The CALLER owes that judgement: pass only a
// successfully-parsed, non-empty set. An empty `keepUrls` deletes NOTHING, by design.
//
// A temp-table anti-join rather than one giant NOT IN (...) literal: at 43,599 URLs the
// literal would be megabytes of SQL and would hit SQLITE_MAX_SQL_LENGTH.
int Database::retireMissingChannels(long long playlistId, int kind,
                                    const std::vector<std::wstring>& keepUrls) {
    if (!db_) return -1;
    if (keepUrls.empty()) return 0;  // nothing to compare against: a no-op, not a failure
    Tx tx(db_);
    // -1, not 0: a contended BEGIN means the retirement did not happen, and 0 would read as
    // "the provider dropped nothing" — the one lie this function must never tell.
    if (!tx) return -1;
    exec("CREATE TEMP TABLE IF NOT EXISTS _keep_urls(url TEXT PRIMARY KEY)");
    exec("DELETE FROM _keep_urls");
    {
        Stmt ins(db_, "INSERT OR IGNORE INTO _keep_urls(url) VALUES(?)");
        if (!ins) return -1;
        for (const std::wstring& u : keepUrls) {
            ins.reset();
            // 🔴 THE SAME canonicalisation as the INSERT, and it MUST live here rather than at the
            // call site. This function anti-joins the keep-set against the stored column, so the
            // two have to be spelled the same way — and VodSync builds its keep-set from the RAW
            // constructed URL. A user whose playlist URL carries an explicit `:80` (which is how
            // this very provider spells it) would otherwise have every stored movie miss every
            // keep entry, and the DELETE below would retire the ENTIRE catalogue the sync just
            // inserted, on every run, for ever. Putting it in the DAO makes it impossible for a
            // caller — including future ones — to get this wrong. INSERT OR IGNORE already absorbs
            // the duplicate keys canonicalisation can produce.
            ins.bindText(1, schemaVersion_ >= 9 ? canonicalStreamUrl(u) : u);
            // ⚠ EVERY row must land. A keep-URL that failed to insert is indistinguishable
            // from one the provider dropped, so the DELETE below would retire a film the
            // caller explicitly asked to keep. One SQLITE_FULL on the temp store partway
            // through 43,599 inserts would wipe the library from that point on — the exact
            // disaster the empty-set guard above is written to prevent, but harder to spot
            // because it looks like it worked. Bail and let ~Tx roll back.
            if (ins.stepDone() != SQLITE_DONE) return -1;
        }
    }
    int removed = 0;
    {
        Stmt del(db_,
                 "DELETE FROM channels WHERE playlist_id=?1 AND kind=?2 "
                 "AND stream_url NOT IN (SELECT url FROM _keep_urls)");
        if (!del) return -1;
        del.bindInt(1, playlistId);
        del.bindInt(2, kind);
        // Read changes() ONLY after a confirmed DELETE: on failure it still holds the count
        // from the last successful statement (the final INSERT), so an unchecked read would
        // report "1 retired" when nothing was deleted.
        if (del.stepDone() != SQLITE_DONE) return -1;
        removed = sqlite3_changes(db_);
    }
    // Keep playlists.channel_count honest — bulkInsertChannels recomputes it, and a sync
    // runs insert-then-retire, so without this the playlist list shows the pre-retirement
    // count until some later import happens to correct it.
    if (removed > 0) {
        Stmt upd(db_,
                 "UPDATE playlists SET channel_count=(SELECT COUNT(*) FROM channels WHERE "
                 "playlist_id=?1) WHERE id=?1");
        if (upd) {
            upd.bindInt(1, playlistId);
            upd.stepDone();
        }
    }
    exec("DELETE FROM _keep_urls");
    // A failed COMMIT means the deletion did not land. Returning `removed` here would hand the
    // caller a believable positive for rows that are still in the table — the one lie this
    // function must never tell.
    if (!tx.commit()) return -1;
    return removed;
}

// The two views that are NOT kind-scoped, and so are the only two where live channels and movies
// share an ordering. `kind` leads the sort for exactly that reason: an Xtream m3u_plus playlist
// and its VOD sync write to the SAME playlist row, and bulkInsertChannels restarts sort_order at 0
// for each batch — so 43,599 movies numbered 0..43598 would interleave straight through 442 live
// channels numbered 0..441, shuffling films into the middle of the user's TV list. Live first
// keeps these views usable without hiding anything.
//
// For a live-only library every row is kind=0, so this term is constant and the ordering is
// byte-identical to before — including on macOS, which calls allChannels() in four places.
std::vector<Channel> Database::allChannels(const GridFilter& g) {
    std::vector<std::wstring> binds;
    return runChannelQuery(
        db_,
        std::string("SELECT ") + kChannelCols + " FROM channels WHERE " + kEnabledOnly +
            gridWhere(g, binds) +
            " ORDER BY kind, (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE" + gridLimit(g),
        nullptr, std::nullopt, &binds);
}

std::vector<Channel> Database::channelsByPlaylist(long long playlistId, const GridFilter& g) {
    std::vector<std::wstring> binds;
    return runChannelQuery(
        db_,
        std::string("SELECT ") + kChannelCols + " FROM channels WHERE playlist_id=?" +
            gridWhere(g, binds) +
            // `, id` only breaks ties the keys before it leave unspecified — so the TV Guide's row
            // join (which keeps the first channel per tvg-id in this order) and the programme search's
            // channel table (refreshProgrammeSearchChannels, same order + id) pick the SAME channel.
            " ORDER BY kind, (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE, id" + gridLimit(g),
        nullptr, playlistId, &binds);
}

// Kind-scoped, so a VOD category that happens to share a name with a live group cannot pull
// 43,599 films into a live group view (or the reverse). The live tree and the Movies tree
// are separate namespaces.
std::vector<Channel> Database::channelsByGroup(const std::wstring& group, const GridFilter& g) {
    return channelsByGroupOfKind(group, 0, g);
}

std::vector<Channel> Database::moviesByGroup(const std::wstring& group, const GridFilter& g) {
    return channelsByGroupOfKind(group, static_cast<int>(Channel::Kind::Movie), g);
}

std::vector<Channel> Database::channelsByGroupOfKind(const std::wstring& group, int kind,
                                                     const GridFilter& g) {
    // kind first: the partial indexes make it the cheap discriminator.
    std::vector<std::wstring> binds;
    std::string sql = std::string("SELECT ") + kChannelCols +
                      " FROM channels WHERE kind=" + std::to_string(kind) +
                      " AND group_title=? AND " + kEnabledOnly + gridWhere(g, binds) +
                      " ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE" + gridLimit(g);
    return runChannelQuery(db_, sql, &group, std::nullopt, &binds);
}

// Every movie, for the "Movies" root itself. Sorted newest-first on the provider's `added`
// stamp — with 43,599 items an alphabetical wall is useless, and "what's new" is the one
// ordering a VOD library is actually browsed by. Ties fall back to name so the order is
// stable (added_at is 0 for anything imported before v8 or from a panel that omits it).
std::vector<Channel> Database::allMovies(const GridFilter& g) {
    std::vector<std::wstring> binds;
    return runChannelQuery(db_,
                           std::string("SELECT ") + kChannelCols +
                               " FROM channels WHERE kind=1 AND " + kEnabledOnly +
                               gridWhere(g, binds) +
                               " ORDER BY added_at DESC, name COLLATE NOCASE" + gridLimit(g),
                           nullptr, std::nullopt, &binds);
}

std::vector<Channel> Database::favourites(const GridFilter& g) {
    std::vector<std::wstring> binds;
    return runChannelQuery(db_,
                           std::string("SELECT ") + kChannelCols +
                               " FROM channels WHERE is_favourite=1 AND " + kEnabledOnly +
                               gridWhere(g, binds) +
                               " ORDER BY (lcn IS NULL), lcn, name COLLATE NOCASE" + gridLimit(g),
                           nullptr, std::nullopt, &binds);
}

namespace {
// Defined with programme search further down (the same anonymous namespace).
size_t codePoints(const std::string& u8);
std::wstring ftsQuoted(const std::wstring& s);
std::wstring likeContains(const std::wstring& text);

// searchChannels' channel-name match, as a WHERE term binding its text at ?1: the channel index when
// it can answer (v11, 3+ characters), else a LIKE scan of the names. `bound` gets the text to bind.
// (countUncoveredChannelNames matches the same way, joining from the index instead.)
std::string channelNameMatch(bool indexed, const std::wstring& term, std::wstring& bound) {
    if (indexed && codePoints(utf8FromWide(term)) >= 3) {
        bound = ftsQuoted(term);  // the whole text as ONE phrase — a substring, as LIKE was
        return "id IN (SELECT rowid FROM channels_fts WHERE channels_fts MATCH ?1)";
    }
    bound = likeContains(term);  // LIKE is ASCII-case-insensitive by default; typed % and _ literal
    return "name LIKE ?1 ESCAPE '\\'";
}
}  // namespace

std::vector<Channel> Database::searchChannels(const std::wstring& term, const GridFilter& g) {
    // Channel NAMES (see the header): through the v11 index when it can answer, else LIKE.
    //
    // ⚠ Routed through runChannelQuery like every other grid query, and that is deliberate: this
    // used to build its own Stmt, which meant a "central" limit added to the shared helper would
    // have silently skipped THE hottest path in the app. It runs on the search box's debounce tick,
    // synchronously on the UI thread — 1,626 ms on a 411k-row library before the cap, 134 ms after,
    // ~0.2 ms through the index. The text is bound once at ?1; the grid filter's bare `?`
    // placeholders take the next free indexes after it.
    std::wstring bound;
    const std::string match = channelNameMatch(channelSearchIndexed(), term, bound);
    std::vector<std::wstring> binds;
    return runChannelQuery(
        db_,
        std::string("SELECT ") + kChannelCols + " FROM channels WHERE " + match + " AND " + kEnabledOnly +
            gridWhere(g, binds) + " ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE" + gridLimit(g),
        &bound, std::nullopt, &binds);
}

std::optional<Channel> Database::channelByLcn(int lcn) {
    auto rows = runChannelQuery(db_, std::string("SELECT ") + kChannelCols + " FROM channels WHERE lcn=? AND " +
                                         kEnabledOnly + " ORDER BY sort_order LIMIT 1",
                                nullptr, lcn);
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

std::optional<Channel> Database::channelByTvgId(const std::wstring& tvgId) {
    // Live first (kind 0): a TV Guide row — built from live channels only — whose tvg-id a movie also
    // carries must play the channel, not a film that happens to sort earlier.
    auto rows = runChannelQuery(db_,
                                std::string("SELECT ") + kChannelCols +
                                    " FROM channels WHERE tvg_id=? AND " + kEnabledOnly +
                                    " ORDER BY kind, sort_order LIMIT 1",
                                &tvgId);
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

std::optional<Channel> Database::channelById(long long id) {
    auto rows = runChannelQuery(db_,
                                std::string("SELECT ") + kChannelCols +
                                    " FROM channels WHERE id=? AND " + kEnabledOnly + " LIMIT 1",
                                nullptr, id);
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

// LIVE groups only. VOD categories are browsed under their own "Movies" nav root rather
// than as ~67 extra siblings in the live tree — a decision that is as much about the tree
// staying recognisable as about cost, though it fixes both (measured with --benchdb: this
// query went 0.09 -> 8.21 ms once 43,599 movies shared the table). Movies are still findable
// by search, which deliberately keeps them.
//
// No behaviour change for a live-only library, mac included: every pre-v8 row is kind=0.
std::vector<std::wstring> Database::listGroups() { return listGroupsOfKind(0); }

// The VOD categories, for the Movies root.
std::vector<std::wstring> Database::listVodGroups() {
    return listGroupsOfKind(static_cast<int>(Channel::Kind::Movie));
}

std::vector<std::wstring> Database::listGroupsOfKind(int kind) {
    std::vector<std::wstring> out;
    // `kind` is inlined as a LITERAL, not bound. A partial index is only usable when SQLite
    // can prove at PREPARE time that the query's WHERE implies the index's condition, and it
    // cannot prove `?1 <> 0` about a parameter it has not seen — so binding here silently
    // cost a full 43,599-row scan (8.9 ms) while the composite VOD index sat unused. `kind`
    // is an int from a fixed enum, so the concatenation is injection-safe.
    Stmt q(db_, (std::string("SELECT DISTINCT group_title FROM channels WHERE kind=") +
                 std::to_string(kind) +
                 " AND group_title IS NOT NULL AND group_title<>'' AND " + kEnabledOnly +
                 " ORDER BY group_title COLLATE NOCASE")
                    .c_str());
    if (!q) return out;
    while (q.step()) out.push_back(q.textCol(0));
    return out;
}

std::vector<std::wstring> Database::listCountries() {
    // Effective country per channel: the tvg-id suffix, else the Xtream-style group-title
    // prefix (the effective_country SQL scalar) — so Xtream playlists, whose tvg-ids carry
    // no ".<cc>", still populate the Countries filter. Codes are ASCII, so ORDER BY's binary
    // sort matches the old std::set ordering.
    std::vector<std::wstring> out;
    Stmt q(db_, (std::string("SELECT DISTINCT effective_country(tvg_id, group_title) "
                             "FROM channels WHERE ") +
                 kLiveOnly + " AND " + kEnabledOnly + " ORDER BY 1")
                    .c_str());
    if (!q) return out;
    while (q.step()) {
        std::wstring cc = q.textCol(0);
        if (!cc.empty()) out.push_back(std::move(cc));
    }
    return out;
}

std::vector<Channel> Database::channelsByCountry(const std::wstring& code, const GridFilter& g) {
    // Filter on the same effective country as listCountries (one rule, the SQL scalar), so
    // the two stay in lockstep. Filtering server-side matters: the mac search path evaluates
    // the active country filter per keystroke, and materializing every channel row here
    // (allChannels-shaped) cost ~30 ms at 14k channels; with the scalar only matches are
    // materialized. The code is lowercased to match the scalar's output (the old LIKE was
    // ASCII-case-insensitive).
    std::wstring want;
    want.reserve(code.size());
    for (wchar_t c : code)
        want.push_back((c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c);
    // kLiveOnly comes FIRST so SQLite discards a VOD row before ever calling the scalar —
    // that is where the 43,599-row cost actually lived. See kLiveOnly for why movies are
    // excluded on correctness grounds too, not merely for speed.
    std::vector<std::wstring> binds;
    return runChannelQuery(db_,
                           std::string("SELECT ") + kChannelCols + " FROM channels WHERE " +
                               kLiveOnly +
                               " AND effective_country(tvg_id, group_title)=? AND " + kEnabledOnly +
                               gridWhere(g, binds) +
                               " ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE" +
                               gridLimit(g),
                           &want, std::nullopt, &binds);
}

void Database::setFavourite(long long channelId, bool favourite) {
    Stmt q(db_, "UPDATE channels SET is_favourite=? WHERE id=?");
    if (!q) return;
    q.bindInt(1, favourite ? 1 : 0);
    q.bindInt(2, channelId);
    q.stepDone();
}

void Database::toggleFavourite(long long channelId) {
    Stmt q(db_, "UPDATE channels SET is_favourite=1-is_favourite WHERE id=?");
    if (!q) return;
    q.bindInt(1, channelId);
    q.stepDone();
}

void Database::setChannelNumber(long long channelId, std::optional<int> lcn) {
    Stmt q(db_, "UPDATE channels SET lcn=? WHERE id=?");
    if (!q) return;
    q.bindOptInt(1, lcn);
    q.bindInt(2, channelId);
    q.stepDone();
}

void Database::setDeadStatus(long long channelId, DeadStatus status, long long nowEpoch) {
    Stmt q(db_, "UPDATE channels SET dead_status=?, last_checked_at=? WHERE id=?");
    if (!q) return;
    q.bindInt(1, static_cast<int>(status));
    q.bindInt(2, nowEpoch);
    q.bindInt(3, channelId);
    q.stepDone();
}

// Reset EVERY channel back to "not checked" — the undo for a dead-link sweep. Also clears
// last_checked_at, so the next sweep re-probes from scratch instead of skipping rows it considers
// freshly checked (the TTL is what makes a sweep resumable, and it would otherwise defeat the
// re-check the user is asking for by clearing).
//
// Returns rows affected, or -1 on failure. The -1 is the point, and the reason this deviates from
// the `return 0` convention elsewhere in this file: sqlite3_changes() reports the last SUCCESSFUL
// change count on the connection, so swallowing a failed step would return some earlier
// setSetting's count and let the caller announce "cleared N channels" having cleared nothing —
// while the grid keeps hiding them. 0 is a legitimate result (nothing was ever marked), so it
// cannot double as the error code.
int Database::clearDeadStatuses() {
    if (!db_) return -1;
    Stmt q(db_, "UPDATE channels SET dead_status=0, last_checked_at=0 WHERE dead_status!=0 OR "
                "last_checked_at!=0");
    if (!q) return -1;
    if (q.stepDone() != SQLITE_DONE) return -1;  // e.g. SQLITE_BUSY past the busy_timeout
    return sqlite3_changes(db_);
}

// ---- EPG (programmes) ------------------------------------------------------

int Database::bulkInsertProgrammes(long long playlistId, const std::vector<Programme>& programmes,
                                   long long nowEpoch) {
    if (!db_) return 0;
    programmeIndexKnown_ = -1;  // whatever happens below, the search index must be re-checked
    Tx tx(db_);
    if (!tx) return 0;  // contended writer: the guide was not replaced
    {  // A refresh replaces this playlist's guide wholesale — the feed is authoritative.
        Stmt del(db_, "DELETE FROM epg_programmes WHERE playlist_id=?");
        if (del) { del.bindInt(1, playlistId); del.stepDone(); }
    }
    Stmt ins(db_,
             "INSERT INTO epg_programmes("
             "playlist_id,channel_id,start_utc,stop_utc,title,sub_title,descr,category,"
             "episode_num,icon_url) VALUES(?,?,?,?,?,?,?,?,?,?)");
    if (!ins) return 0;
    int n = 0;
    for (const Programme& p : programmes) {
        if (!p.isValid()) continue;
        ins.reset();
        ins.bindInt(1, playlistId);
        ins.bindText(2, p.channelId);
        ins.bindInt(3, p.startUtc);
        ins.bindInt(4, p.stopUtc);
        ins.bindText(5, p.title);
        ins.bindText(6, p.subTitle);
        ins.bindText(7, p.descr);
        ins.bindText(8, p.category);
        ins.bindText(9, p.episodeNum);
        ins.bindText(10, p.iconUrl);
        if (ins.stepDone() == SQLITE_DONE) ++n;
    }
    // Record the refresh time so the UI can show "guide updated N ago" (part of the Tx).
    setSetting(L"epg_refreshed_" + std::to_wstring(playlistId), std::to_wstring(nowEpoch));
    if (!tx.commit()) return 0;  // rolled back: no programmes stored, and no refresh timestamp
    return n;
}

std::vector<Programme> Database::nowNext(long long playlistId, const std::wstring& channelId,
                                         long long nowEpoch) {
    std::vector<Programme> out;
    if (!db_) return out;
    Stmt q(db_, (std::string("SELECT ") + kProgrammeCols +
                 " FROM epg_programmes WHERE playlist_id=? AND channel_id=? AND stop_utc>? "
                 "ORDER BY start_utc LIMIT 2")
                    .c_str());
    if (!q) return out;
    q.bindInt(1, playlistId);
    q.bindText(2, channelId);
    q.bindInt(3, nowEpoch);
    while (q.step()) out.push_back(readProgramme(q));
    return out;
}

std::vector<Programme> Database::programmesInWindow(long long playlistId, long long windowStartUtc,
                                                    long long windowEndUtc) {
    std::vector<Programme> out;
    if (!db_) return out;
    Stmt q(db_, (std::string("SELECT ") + kProgrammeCols +
                 " FROM epg_programmes WHERE playlist_id=? AND start_utc<? AND stop_utc>? "
                 "ORDER BY channel_id, start_utc")
                    .c_str());
    if (!q) return out;
    q.bindInt(1, playlistId);
    q.bindInt(2, windowEndUtc);
    q.bindInt(3, windowStartUtc);
    while (q.step()) out.push_back(readProgramme(q));
    return out;
}

std::vector<Programme> Database::programmesInWindowAll(long long windowStartUtc,
                                                       long long windowEndUtc) {
    std::vector<Programme> out;
    if (!db_) return out;
    // Across every ENABLED playlist — a recording rule is library-wide, not playlist-scoped.
    // Overlap test (start<end AND stop>start) matches programmesInWindow. A subquery rather
    // than a JOIN so kScheduleCols-style bare column lists stay unqualified and unambiguous.
    Stmt q(db_, (std::string("SELECT ") + kProgrammeCols +
                 " FROM epg_programmes"
                 " WHERE playlist_id IN (SELECT id FROM playlists WHERE enabled=1)"
                 "   AND start_utc<? AND stop_utc>?"
                 " ORDER BY channel_id, start_utc")
                    .c_str());
    if (!q) return out;
    q.bindInt(1, windowEndUtc);
    q.bindInt(2, windowStartUtc);
    while (q.step()) out.push_back(readProgramme(q));
    return out;
}

// ---- Programme search (schema v10) — docs/EPG_SEARCH.md ----------------------

// What the index must have been built from: the programme count and max id, plus every
// epg_refreshed_<playlist> setting — which bulkInsertProgrammes rewrites on EVERY refresh, in every
// build since the one that created schema v3, so an older build's refresh changes it too. Count and
// max id alone would not do: a same-sized re-import reuses the same ids. Empty = could not read.
std::wstring Database::programmeIndexStamp() {
    std::wstring s;
    {
        Stmt q(db_, "SELECT COUNT(*), IFNULL(MAX(id),0) FROM epg_programmes");
        if (!q || !q.step()) return L"";
        s = std::to_wstring(q.intCol(0)) + L":" + std::to_wstring(q.intCol(1));
    }
    Stmt q(db_, "SELECT key, value FROM settings WHERE key GLOB 'epg_refreshed_*' ORDER BY key");
    if (!q) return L"";
    while (q.step()) s += L";" + q.textCol(0) + L"=" + q.textCol(1);
    return s;
}

Database::ProgrammeSearchState Database::programmeSearchState() {
    if (!db_ || schemaVersion_ < 10) return ProgrammeSearchState::Unavailable;
    if (programmeIndexKnown_ < 0) {
        const std::wstring now = programmeIndexStamp();
        const auto saved = getSetting(L"epg_fts_stamp");
        programmeIndexKnown_ = (!now.empty() && saved && *saved == now) ? 1 : 0;
    }
    return programmeIndexKnown_ == 1 ? ProgrammeSearchState::Ready
                                     : ProgrammeSearchState::NeedsRebuild;
}

bool Database::rebuildProgrammeIndex() {
    if (!db_ || schemaVersion_ < 10) return false;
    Tx tx(db_);
    if (!tx) return false;  // contended writer: nothing changed, the old stamp still stands
    if (!exec("INSERT INTO epg_fts_title(epg_fts_title) VALUES('rebuild')")) return false;
    if (!exec("INSERT INTO epg_fts_descr(epg_fts_descr) VALUES('rebuild')")) return false;
    const std::wstring stamp = programmeIndexStamp();  // same transaction: what was just indexed
    if (stamp.empty()) return false;
    {
        Stmt q(db_, "INSERT INTO settings(key,value) VALUES('epg_fts_stamp',?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        if (!q) return false;
        q.bindText(1, stamp);
        if (q.stepDone() != SQLITE_DONE) return false;
    }
    if (!tx.commit()) return false;
    programmeIndexKnown_ = 1;
    return true;
}

namespace {

// The search's channel table: one row per (playlist, normalised tvg-id) the user can play, carrying
// the channel a TV Guide row would show for it.
constexpr const char* kSearchChannelTable =
    "CREATE TEMP TABLE IF NOT EXISTS guide_channels("
    "playlist_id INTEGER NOT NULL, cid TEXT NOT NULL, name TEXT NOT NULL, tvg_id TEXT NOT NULL,"
    " PRIMARY KEY(playlist_id, cid)) WITHOUT ROWID";

// A tvg-id normalised as core/RecordingRules normaliseTvgId does — the part before '@',
// ASCII-lower-cased (SQLite's lower() is ASCII-only too). Used on BOTH sides of the join.
#define RE_NORM_TVG(col) "lower(substr(" col ",1,instr(" col "||'@','@')-1))"

}  // namespace

bool Database::refreshProgrammeSearchChannels() {
    if (!db_) return false;
    if (!exec(kSearchChannelTable)) return false;
    // A DEFERRED transaction that WRITES only the TEMP schema takes no write lock on the main
    // database (it only reads `channels`), so this never contends with the guide store or the sync
    // workers. One statement, not a row loop: ~90 ms at the owner's 410k channels, where listing
    // them through channelsByPlaylist took 1.1 s.
    if (!exec("BEGIN")) return false;
    // Per (playlist, id) keep the FIRST LIVE channel in channelsByPlaylist's own order — the one the
    // guide's row join keeps (onEpgGuide's byBase.emplace, live channels only), so a result names and
    // plays the same channel its guide row does.
    const bool ok =
        exec("DELETE FROM temp.guide_channels") &&
        exec("INSERT INTO temp.guide_channels(playlist_id,cid,name,tvg_id)"
             " SELECT playlist_id, cid, name, tvg_id FROM ("
             "  SELECT playlist_id, " RE_NORM_TVG("tvg_id") " AS cid, name, tvg_id,"
             "   ROW_NUMBER() OVER (PARTITION BY playlist_id, " RE_NORM_TVG("tvg_id")
             "    ORDER BY kind, (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE, id) AS rn"
             "  FROM channels WHERE tvg_id>'' AND +kind=0"  // + : the tvg-id index, not every live row
             "   AND +playlist_id IN (SELECT id FROM playlists WHERE enabled=1))"
             " WHERE rn=1");
    if (!ok || !exec("COMMIT")) {
        exec("ROLLBACK");  // a failed COMMIT leaves the transaction open — close it
        return false;
    }
    return true;
}

std::vector<std::pair<long long, int>> Database::distinctLiveGuideIds() {
    std::vector<std::pair<long long, int>> out;
    if (!db_) return out;
    // The unary + on kind AND on the GROUP BY keeps the planner on idx_channels_tvgid — a range over
    // just the rows that HAVE a tvg-id — instead of idx_channels_live or idx_channels_playlist (which
    // it picks for a plain GROUP BY playlist_id, to skip a sort): 2.8 ms vs 81 ms on the owner's
    // library (measured, vendored 3.53.2). kind 0 = live, as everywhere in this file.
    Stmt q(db_, "SELECT playlist_id, COUNT(DISTINCT " RE_NORM_TVG("tvg_id") ") FROM channels"
                " WHERE tvg_id>'' AND +kind=0 GROUP BY +playlist_id");
    if (!q) return out;
    while (q.step()) out.emplace_back(q.intCol(0), static_cast<int>(q.intCol(1)));
    return out;
}

std::vector<std::wstring> Database::liveGuideIds() {
    std::vector<std::wstring> out;
    if (!db_) return out;
    Stmt q(db_, (std::string("SELECT DISTINCT " RE_NORM_TVG("tvg_id") " FROM channels"
                             " WHERE tvg_id>'' AND +kind=0 AND +") + kEnabledOnly)
                    .c_str());
    if (!q) return out;
    while (q.step()) out.push_back(q.textCol(0));
    return out;
}

int Database::countUncoveredChannelNames(const std::wstring& text,
                                         const std::unordered_set<std::wstring>& coveredIds, int maxRows) {
    if (!db_ || !channelSearchIndexed() || maxRows <= 0 || codePoints(utf8FromWide(text)) < 3) return -1;
    // Driven FROM the index (CROSS JOIN fixes the order), so LIMIT stops it early: "id IN (SELECT rowid
    // …)" would collect every match first — 10.6 ms for "the" (68k) even under LIMIT 1, vs 3.5 ms.
    Stmt q(db_, (std::string("SELECT c.name, " RE_NORM_TVG("c.tvg_id") " FROM channels_fts"
                             " CROSS JOIN channels c ON c.id=channels_fts.rowid"
                             " WHERE channels_fts MATCH ?1 AND c.kind=0 AND c.") +
                 kEnabledOnly + " LIMIT ?2")
                    .c_str());
    if (!q) return -1;
    q.bindText(1, ftsQuoted(text));  // the same phrase searchChannels' index path uses
    q.bindInt(2, static_cast<long long>(maxRows) + 1);  // one extra says "too many to be worth it"
    // A name counts when NONE of its channels is covered: "CBC Toronto HD" and "… FHD" sharing the
    // guide's row id are both covered, a same-named channel elsewhere does not uncover them.
    std::unordered_map<std::wstring, bool> nameCovered;
    int rows = 0;
    while (q.step()) {
        if (++rows > maxRows) return -1;
        const std::wstring id = q.textCol(1);
        bool& covered = nameCovered.emplace(q.textCol(0), false).first->second;
        if (!id.empty() && coveredIds.count(id)) covered = true;
    }
    int n = 0;
    for (const auto& [name, covered] : nameCovered)
        if (!covered) ++n;
    return n;
}

namespace {

// Unicode code points in a UTF-8 string (portable: wchar_t is UTF-16 on Windows, UTF-32 on mac).
size_t codePoints(const std::string& u8) {
    size_t n = 0;
    for (unsigned char b : u8)
        if ((b & 0xC0) != 0x80) ++n;
    return n;
}

std::wstring trimSpace(const std::wstring& s) {
    // U+3000 too: a Japanese IME's Space types the ideographic space.
    const size_t b = s.find_first_not_of(L" \t\r\n\f\v\u3000");
    if (b == std::wstring::npos) return L"";
    const size_t e = s.find_last_not_of(L" \t\r\n\f\v\u3000");
    return s.substr(b, e - b + 1);
}

// "Could be part of a word": an ASCII letter or digit, or anything non-ASCII outside the common
// punctuation and space blocks — a portable stand-in for iswalnum, which on macOS's C locale says no
// to every non-ASCII letter. The excluded blocks are the ones real guide text puts right against a
// word, and that the unicode61 tokenizer treats as separators: U+00A0–U+00BF (no-break space, « »,
// ¡ ¿), U+2000–U+206F (curly quotes ‘ ’ “ ”, dashes, …), U+3000–U+303F (CJK 「」、。).
bool wordish(wchar_t c) {
    const unsigned long u = static_cast<unsigned long>(c);
    if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return true;
    if (u < 0x80) return false;
    return !((u >= 0x00A0 && u <= 0x00BF) || (u >= 0x2000 && u <= 0x206F) || (u >= 0x3000 && u <= 0x303F));
}

// Does the text contain CJK? The unicode61 tokenizer has no word segmentation for it — a run of
// Chinese or Japanese without spaces is ONE token — so descriptions in those scripts are searched
// with LIKE instead (the titles' trigram index is fine: trigram needs no word boundaries).
bool isCjkChar(wchar_t c) {
    const unsigned long u = static_cast<unsigned long>(c);
    return (u >= 0x3040 && u <= 0x30FF) ||   // hiragana, katakana
           (u >= 0x3400 && u <= 0x4DBF) ||   // CJK extension A
           (u >= 0x4E00 && u <= 0x9FFF) ||   // CJK unified ideographs
           (u >= 0xAC00 && u <= 0xD7AF) ||   // hangul syllables
           (u >= 0xF900 && u <= 0xFAFF) ||   // CJK compatibility ideographs
           (u >= 0xFF66 && u <= 0xFF9F);     // half-width katakana
}
bool hasCjk(const std::wstring& s) { return std::any_of(s.begin(), s.end(), isCjkChar); }

// The typed text split on whitespace, keeping only words with something a tokenizer could keep.
std::vector<std::wstring> searchWords(const std::wstring& text) {
    std::vector<std::wstring> words;
    std::wstring cur;
    auto flush = [&] {
        for (wchar_t c : cur)
            if (wordish(c)) {
                words.push_back(cur);
                break;
            }
        cur.clear();
    };
    for (wchar_t c : text) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\u3000') flush();
        else cur += c;
    }
    flush();
    return words;
}

// One FTS5 string: "…" with every inner " doubled — the only escaping FTS5's syntax has, and it
// makes whatever the user typed (*, (, -, AND, OR, NEAR, :) a literal to search for.
std::wstring ftsQuoted(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') o += L'"';
        o += c;
    }
    return o + L"\"";
}

// The description query: each word quoted and simply juxtaposed — FTS5's implicit AND, which
// (unlike an explicit AND) drops a word that yields no tokens — with the last one a prefix:
// "doctor" "who"*. Empty = no description search.
std::wstring descrQuery(const std::vector<std::wstring>& words) {
    std::wstring q;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) q += L' ';
        q += ftsQuoted(words[i]);
        if (i + 1 == words.size()) q += L'*';
    }
    return q;
}

// %text% for LIKE … ESCAPE '\'.
std::wstring likeContains(const std::wstring& text) {
    std::wstring o = L"%";
    for (wchar_t c : text) {
        if (c == L'%' || c == L'_' || c == L'\\') o += L'\\';
        o += c;
    }
    return o + L"%";
}

// Does `needle` occur in `hay` at `pos`, comparing through searchFold (case, and Latin accents — so
// the "Québec" a search for "quebec" found gets marked)? With `wordStart`, only where a word begins
// (the tokenizer's view: a description word matches at its start, never in the middle of another
// word) — except for a CJK needle, whose words have no boundaries to start at (see hasCjk).
bool matchesAt(const std::wstring& hay, size_t pos, const std::wstring& needle, bool wordStart) {
    if (needle.empty() || pos + needle.size() > hay.size()) return false;
    if (wordStart && pos > 0 && wordish(hay[pos - 1]) && wordish(needle[0]) && !isCjkChar(needle[0]))
        return false;
    for (size_t k = 0; k < needle.size(); ++k)
        if (searchFold(hay[pos + k]) != searchFold(needle[k])) return false;
    return true;
}

// `text` with every occurrence of any `needles` entry wrapped in U+0002 … U+0003 (the longest one
// where several start at the same place). Empty when nothing occurs — the caller shows it plain.
std::wstring markAll(const std::wstring& text, const std::vector<std::wstring>& needles, bool wordStart) {
    std::wstring o;
    bool any = false;
    for (size_t i = 0; i < text.size();) {
        size_t len = 0;
        for (const auto& n : needles)
            if (n.size() > len && matchesAt(text, i, n, wordStart)) len = n.size();
        if (len) {
            o += L'\x02';
            o.append(text, i, len);
            o += L'\x03';
            i += len;
            any = true;
        } else {
            o += text[i++];
        }
    }
    return any ? o : std::wstring();
}

// Never cut between the two halves of a UTF-16 surrogate pair (an emoji or other astral character on
// Windows, where wchar_t is 16-bit): move a cut off a low surrogate. A no-op with 32-bit wchar_t.
size_t offLowSurrogate(const std::wstring& s, size_t i, bool forward) {
    while (i > 0 && i < s.size() && static_cast<unsigned long>(s[i]) >= 0xDC00 &&
           static_cast<unsigned long>(s[i]) <= 0xDFFF) {
        if (forward) ++i;
        else --i;
    }
    return i;
}

// An excerpt of a description around the EARLIEST occurrence of any typed word, the words marked;
// the description's start when searchFold's comparison finds none (a match the tokenizer found
// through some other folding — the match is still real, it just cannot be located here).
std::wstring snippetAround(const std::wstring& descr, const std::vector<std::wstring>& words) {
    constexpr size_t kBefore = 30, kAfter = 90, kPlain = 110;
    size_t at = std::wstring::npos;
    for (size_t i = 0; i < descr.size() && at == std::wstring::npos; ++i)
        for (const auto& w : words)
            if (matchesAt(descr, i, w, true)) {
                at = i;
                break;
            }
    if (at == std::wstring::npos)
        return descr.size() > kPlain ? descr.substr(0, offLowSurrogate(descr, kPlain, false)) + L"…"
                                     : descr;
    size_t b = offLowSurrogate(descr, at > kBefore ? at - kBefore : 0, true);
    size_t e = offLowSurrogate(descr, std::min(descr.size(), at + kAfter), false);
    if (b > 0) {  // start on a word boundary
        const size_t sp = descr.find(L' ', b);
        if (sp != std::wstring::npos && sp < at) b = sp + 1;
    }
    if (e < descr.size()) {
        const size_t sp = descr.rfind(L' ', e);
        if (sp != std::wstring::npos && sp > at) e = sp;
    }
    const std::wstring cut = descr.substr(b, e - b);
    const std::wstring marked = markAll(cut, words, true);
    return (b > 0 ? L"…" : L"") + (marked.empty() ? cut : marked) +
           (e < descr.size() ? L"…" : L"");
}

// The join that keeps only programmes the caller can show (refreshProgrammeSearchChannels). CROSS
// JOIN fixes the join ORDER: SQLite's planner otherwise drives from epg_programmes through
// idx_epg_lookup (playlist_id IN …) and probes the matches per row — 47 ms for a rare word on the
// owner's 193k programmes, against 0.2 ms matches-first (measured, vendored 3.53.2).
constexpr const char* kSearchChannelJoin =
    " CROSS JOIN temp.guide_channels g ON g.playlist_id=p.playlist_id AND g.cid="
    RE_NORM_TVG("p.channel_id");

}  // namespace

#undef RE_NORM_TVG

std::vector<Database::ProgrammeHit> Database::searchProgrammes(const std::wstring& text,
                                                               long long fromUtc, int limit,
                                                               bool* truncated) {
    std::vector<ProgrammeHit> out;
    if (truncated) *truncated = false;
    if (!db_ || limit <= 0) return out;
    const std::wstring term = trimSpace(text);
    if (term.empty()) return out;
    // The search needs its channel table even when the caller never loaded one (then: no results).
    if (!exec(kSearchChannelTable)) return out;

    // Trigram cannot match under 3 characters, and a stale index must never be READ: a reused rowid
    // would return the wrong programme (a single-guide refresh restarts ids at 1), and reading a
    // column or snippet() through FTS5 on a stale rowid fails with SQLITE_CORRUPT_VTAB.
    const bool ready = programmeSearchState() == ProgrammeSearchState::Ready;
    const bool useIndex = ready && codePoints(utf8FromWide(term)) >= 3;
    const bool cjk = hasCjk(term);  // descriptions by LIKE: unicode61 does not segment CJK
    const std::vector<std::wstring> words = searchWords(term);
    const std::wstring titleQ = ftsQuoted(term);
    const std::wstring descrQ = (useIndex && !cjk) ? descrQuery(words) : L"";

    // 1. Match, keep the caller's channels, rank, limit — ROWIDS ONLY from the index: fetching a
    //    column, highlight() or snippet() through FTS5 reads (and re-tokenises) the content row of
    //    EVERY match before the LIMIT applies — measured 700 ms for "the" with snippet().
    std::string sql;
    if (useIndex) {
        sql = "SELECT p.id, MAX(m.t) AS inTitle, p.start_utc, g.name, g.tvg_id FROM ("
              "SELECT rowid AS id, 1 AS t FROM epg_fts_title WHERE epg_fts_title MATCH ?1";
        if (!descrQ.empty())
            sql += " UNION ALL SELECT rowid, 0 FROM epg_fts_descr WHERE epg_fts_descr MATCH ?2";
        else if (cjk)  // the whole term as a substring of the description (a table scan, ~50 ms)
            sql += " UNION ALL SELECT id, 0 FROM epg_programmes WHERE descr LIKE ?5 ESCAPE '\\'";
        sql += ") m CROSS JOIN epg_programmes p ON p.id=m.id";
        sql += kSearchChannelJoin;
        sql += " WHERE p.stop_utc>?3";
        sql += std::string(" AND p.") + kEnabledOnly;
        sql += " GROUP BY p.id ORDER BY inTitle DESC, p.start_utc LIMIT ?4";
    } else {
        // LIKE, matching the whole term as one substring: titles always; descriptions too unless
        // the index is Ready (a 1–2 character Latin term WITH a Ready index searches titles only — a
        // one-letter description scan matches nearly everything), and always for CJK, where two
        // characters are already a whole word.
        sql = "SELECT p.id, (p.title LIKE ?1 ESCAPE '\\') AS inTitle, p.start_utc, g.name, g.tvg_id"
              " FROM epg_programmes p";
        sql += kSearchChannelJoin;
        sql += " WHERE p.stop_utc>?3";
        sql += std::string(" AND p.") + kEnabledOnly;
        sql += (ready && !cjk) ? " AND p.title LIKE ?1 ESCAPE '\\'"
                               : " AND (p.title LIKE ?1 ESCAPE '\\' OR p.descr LIKE ?1 ESCAPE '\\')";
        sql += " ORDER BY inTitle DESC, p.start_utc LIMIT ?4";
    }
    struct Ranked {
        long long    id;
        bool         inTitle;
        std::wstring name, tvgId;
    };
    std::vector<Ranked> ranked;
    {
        Stmt q(db_, sql.c_str());
        if (!q) {
            lastError_ = wideFromUtf8(sqlite3_errmsg(db_));
            return out;
        }
        q.bindText(1, useIndex ? titleQ : likeContains(term));
        if (useIndex && !descrQ.empty()) q.bindText(2, descrQ);
        if (useIndex && descrQ.empty() && cjk) q.bindText(5, likeContains(term));
        q.bindInt(3, fromUtc);
        q.bindInt(4, static_cast<long long>(limit) + 1);  // one extra says "there are more"
        while (q.step()) ranked.push_back({q.intCol(0), q.intCol(1) != 0, q.textCol(3), q.textCol(4)});
    }
    if (static_cast<int>(ranked.size()) > limit) {
        ranked.resize(static_cast<size_t>(limit));
        if (truncated) *truncated = true;
    }
    if (ranked.empty()) return out;

    // 2. The winners' rows. A separate statement, so in principle another connection could commit
    //    between the two — none in this process writes epg_programmes (only bulkInsertProgrammes and
    //    deletePlaylist's cascade do, on the caller's connection), and a missing id is skipped below.
    std::string idList;
    for (const auto& r : ranked) {
        if (!idList.empty()) idList += ',';
        idList += std::to_string(r.id);
    }
    std::unordered_map<long long, ProgrammeHit> byId;
    {
        Stmt q(db_, (std::string("SELECT id, playlist_id, ") + kProgrammeCols +
                     " FROM epg_programmes WHERE id IN (" + idList + ")")
                        .c_str());
        if (!q) return out;
        while (q.step()) {
            ProgrammeHit h;
            h.id = q.intCol(0);
            h.playlistId = q.intCol(1);
            // readProgramme reads kProgrammeCols from column 0; here they start at column 2.
            h.programme.channelId = q.textCol(2);
            h.programme.startUtc = q.intCol(3);
            h.programme.stopUtc = q.intCol(4);
            h.programme.title = q.textCol(5);
            h.programme.subTitle = q.textCol(6);
            h.programme.descr = q.textCol(7);
            h.programme.category = q.textCol(8);
            h.programme.episodeNum = q.textCol(9);
            h.programme.iconUrl = q.textCol(10);
            byId.emplace(h.id, std::move(h));
        }
    }
    // 3. Mark the matches for display, in C++ over at most `limit` rows (see ProgrammeHit).
    out.reserve(ranked.size());
    for (auto& r : ranked) {
        auto it = byId.find(r.id);
        if (it == byId.end()) continue;
        ProgrammeHit& h = it->second;
        h.inTitle = r.inTitle;
        h.channelName = std::move(r.name);
        h.channelTvgId = std::move(r.tvgId);
        if (h.inTitle) h.markedTitle = markAll(h.programme.title, {term}, false);
        else h.snippet = snippetAround(h.programme.descr, words);
        out.push_back(std::move(h));
    }
    return out;
}

// ---- Scheduled recordings --------------------------------------------------

long long Database::addSchedule(const ScheduledRecording& s) {
    Stmt q(db_,
           "INSERT INTO scheduled_recordings("
           "channel_id,channel_name,stream_url,user_agent,referrer,title,start_utc,stop_utc,mux,"
           "status,file_path,created_at,rule_id,episode_key,prog_start_utc)"
           " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!q) return 0;
    q.bindText(1, s.channelId);
    q.bindText(2, s.channelName);
    q.bindText(3, s.streamUrl);
    q.bindText(4, s.userAgent);
    q.bindText(5, s.referrer);
    q.bindText(6, s.title);
    q.bindInt(7, s.startUtc);
    q.bindInt(8, s.stopUtc);
    q.bindText(9, s.mux);
    q.bindInt(10, static_cast<int>(s.status));
    q.bindText(11, s.filePath);
    q.bindInt(12, s.createdAt);
    q.bindInt(13, s.ruleId);  // 0 == a one-off schedule (no owning rule)
    q.bindText(14, s.episodeKey);  // '' for manual rows (no episode dedup)
    q.bindInt(15, s.progStartUtc);  // 0 for manual rows (no airing identity)
    if (q.stepDone() != SQLITE_DONE) return 0;
    return sqlite3_last_insert_rowid(db_);
}

// ---- Recording rules (EPG-driven series recording) --------------------------

long long Database::addRule(const RecordingRule& r) {
    Stmt q(db_,
           "INSERT INTO recording_rules("
           "channel_id,channel_name,title_match,match_kind,enabled,lead_sec,trail_sec,mux,"
           "created_at) VALUES(?,?,?,?,?,?,?,?,?)");
    if (!q) return 0;
    q.bindText(1, r.channelId);
    q.bindText(2, r.channelName);
    q.bindText(3, r.titleMatch);
    q.bindInt(4, static_cast<int>(r.match));
    q.bindInt(5, r.enabled ? 1 : 0);
    q.bindInt(6, r.leadSec);
    q.bindInt(7, r.trailSec);
    q.bindText(8, r.mux);
    q.bindInt(9, r.createdAt);
    if (q.stepDone() != SQLITE_DONE) return 0;
    return sqlite3_last_insert_rowid(db_);
}

void Database::updateRule(const RecordingRule& r) {
    Stmt q(db_,
           "UPDATE recording_rules SET channel_id=?,channel_name=?,title_match=?,match_kind=?,"
           "enabled=?,lead_sec=?,trail_sec=?,mux=? WHERE id=?");
    if (!q) return;
    q.bindText(1, r.channelId);
    q.bindText(2, r.channelName);
    q.bindText(3, r.titleMatch);
    q.bindInt(4, static_cast<int>(r.match));
    q.bindInt(5, r.enabled ? 1 : 0);
    q.bindInt(6, r.leadSec);
    q.bindInt(7, r.trailSec);
    q.bindText(8, r.mux);
    q.bindInt(9, r.id);
    q.stepDone();
}

std::vector<RecordingRule> Database::listRules() {
    std::vector<RecordingRule> out;
    if (!db_) return out;
    Stmt q(db_, (std::string("SELECT ") + kRuleCols + " FROM recording_rules ORDER BY created_at")
                    .c_str());
    if (!q) return out;
    while (q.step()) out.push_back(readRule(q));
    return out;
}

void Database::setRuleEnabled(long long id, bool enabled) {
    Stmt q(db_, "UPDATE recording_rules SET enabled=? WHERE id=?");
    if (!q) return;
    q.bindInt(1, enabled ? 1 : 0);
    q.bindInt(2, id);
    q.stepDone();
}

bool Database::deleteRule(long long id) {
    // Drop the rule's still-PENDING rows (they were only ever a materialised prediction), but
    // keep anything Recording/Done/Missed/Failed/Cancelled — that history is a record of what
    // actually happened and must survive the recipe that produced it. Not an FK cascade for
    // exactly this reason; rule_id on the surviving rows becomes a dangling id, which is fine
    // (nothing joins on it — it exists only for dedup + this cleanup).
    //
    // ⚠ ONE transaction, so false means NEITHER table changed. As two autocommit DELETEs it could
    // split either way under contention: a lost pending-drop followed by a landed rule delete orphans
    // Pending rows nothing ever sweeps (they record, and arm unattended wakes, for a rule the user
    // removed); a landed drop followed by a lost rule delete silently unqueues the rule's airings
    // until its next expansion, and loses any not-yet-landed Skip on them. BEGIN IMMEDIATE takes the
    // writer lock up front, so the two statements cannot be separated by another connection.
    Tx tx(db_);
    if (!tx) return false;  // writer lock not obtained: nothing ran
    {
        Stmt q(db_, "DELETE FROM scheduled_recordings WHERE rule_id=? AND status=?");
        if (!q) return false;  // ~Tx rolls back
        q.bindInt(1, id);
        q.bindInt(2, static_cast<int>(ScheduleStatus::Pending));
        if (q.stepDone() != SQLITE_DONE) return false;
    }
    {
        Stmt q(db_, "DELETE FROM recording_rules WHERE id=?");
        if (!q) return false;
        q.bindInt(1, id);
        if (q.stepDone() != SQLITE_DONE) return false;
    }
    return tx.commit();  // a failed COMMIT leaves done=false, so ~Tx rolls back
}

bool Database::clearPendingForRule(long long ruleId) {
    // Same rationale as deleteRule's pending-drop, but the rule stays: when a rule is edited its
    // old predictions no longer match the new criteria, so clear the still-Pending rows and let
    // the caller re-expand. History (Recording/Done/Missed/Failed/Cancelled) is untouched.
    Stmt q(db_, "DELETE FROM scheduled_recordings WHERE rule_id=? AND status=?");
    if (!q) return false;
    q.bindInt(1, ruleId);
    q.bindInt(2, static_cast<int>(ScheduleStatus::Pending));
    return q.stepDone() == SQLITE_DONE;
}

std::vector<ScheduledRecording> Database::listSchedules() {
    std::vector<ScheduledRecording> out;
    if (!db_) return out;
    Stmt q(db_,
           (std::string("SELECT ") + kScheduleCols + " FROM scheduled_recordings ORDER BY start_utc")
               .c_str());
    if (!q) return out;
    while (q.step()) out.push_back(readSchedule(q));
    return out;
}

bool Database::updateScheduleStatus(long long id, ScheduleStatus status, const std::wstring& filePath) {
    // Set file_path only when a non-empty one is given, so a later status change (Done,
    // Failed…) doesn't clobber the path captured when recording started.
    //
    // changes() is read ONLY after a confirmed SQLITE_DONE: on a failed step it still holds the
    // count from this connection's previous successful statement, so an unchecked read would report
    // a lost write as landed (same trap retireMissingChannels documents).
    if (filePath.empty()) {
        Stmt q(db_, "UPDATE scheduled_recordings SET status=? WHERE id=?");
        if (!q) return false;
        q.bindInt(1, static_cast<int>(status));
        q.bindInt(2, id);
        if (q.stepDone() != SQLITE_DONE) return false;
    } else {
        Stmt q(db_, "UPDATE scheduled_recordings SET status=?, file_path=? WHERE id=?");
        if (!q) return false;
        q.bindInt(1, static_cast<int>(status));
        q.bindText(2, filePath);
        q.bindInt(3, id);
        if (q.stepDone() != SQLITE_DONE) return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool Database::deleteSchedule(long long id) {
    Stmt q(db_, "DELETE FROM scheduled_recordings WHERE id=?");
    if (!q) return false;
    q.bindInt(1, id);
    return q.stepDone() == SQLITE_DONE;  // a row that was already gone is still "gone": success
}

// ---- Settings --------------------------------------------------------------

std::optional<std::wstring> Database::getSetting(const std::wstring& key) {
    Stmt q(db_, "SELECT value FROM settings WHERE key=?");
    if (!q) return std::nullopt;
    q.bindText(1, key);
    if (!q.step()) return std::nullopt;
    return q.textCol(0);
}

void Database::setSetting(const std::wstring& key, const std::wstring& value) {
    Stmt q(db_, "INSERT INTO settings(key,value) VALUES(?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!q) return;
    q.bindText(1, key);
    q.bindText(2, value);
    q.stepDone();
}

}  // namespace rabbitears
