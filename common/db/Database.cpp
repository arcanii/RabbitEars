// SPDX-License-Identifier: GPL-3.0-or-later
#include "db/Database.h"

#include <sqlite3.h>

#include <set>

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
    explicit Tx(sqlite3* d) : db(d) { sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr); }
    void commit() {
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        done = true;
    }
    ~Tx() {
        if (!done) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};

// SELECT column order shared by every channel query below.
constexpr const char* kChannelCols =
    "id,playlist_id,name,stream_url,logo_url,group_title,tvg_id,tvg_name,lcn,"
    "is_favourite,dead_status,last_checked_at,sort_order,user_agent,referrer";

// Predicate restricting a channel query to enabled playlists. Disabled playlists
// keep their rows but vanish from every cross-playlist view; channelsByPlaylist()
// deliberately omits it (an explicit "show me exactly this playlist" accessor).
constexpr const char* kEnabledOnly =
    "playlist_id IN (SELECT id FROM playlists WHERE enabled=1)";

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

std::vector<Channel> runChannelQuery(sqlite3* db, const std::string& sql,
                                     const std::wstring* bindText = nullptr,
                                     std::optional<long long> bindInt = std::nullopt) {
    std::vector<Channel> out;
    Stmt q(db, sql.c_str());
    if (!q) return out;
    int idx = 1;
    if (bindText) q.bindText(idx++, *bindText);
    if (bindInt) q.bindInt(idx++, *bindInt);
    while (q.step()) out.push_back(readChannel(q));
    return out;
}

// The 2-letter country code from an iptv-org-style tvg-id ("<name>.<cc>"): the last
// dot-segment iff it is exactly two ASCII letters, lowercased. "" otherwise.
std::wstring countryFromTvgId(const std::wstring& tvgId) {
    const size_t dot = tvgId.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 3 != tvgId.size()) return L"";
    auto isAlpha = [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); };
    auto lower = [](wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c; };
    const wchar_t a = tvgId[dot + 1], b = tvgId[dot + 2];
    if (!isAlpha(a) || !isAlpha(b)) return L"";
    return std::wstring{lower(a), lower(b)};
}

}  // namespace

Database::~Database() { close(); }

void Database::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
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

bool Database::open(const std::wstring& path, std::wstring* error) {
    close();
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
// A fresh DB starts at user_version 0 (kSchema built the v1 shape), an existing
// 0.1.x DB is at 1, a 0.1.9+ DB at 2; each open applies whatever steps are missing.
void Database::migrate() {
    long long v = 0;
    {
        Stmt q(db_, "PRAGMA user_version");
        if (q && q.step()) v = q.intCol(0);
    }
    if (v >= 3) return;

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

    // Advance user_version to reflect exactly what actually landed, so a partial
    // failure retries the missing step next open instead of skipping it.
    const bool haveV2 = hasColumn("playlists", "enabled");
    const bool haveV3 = haveV2 && hasColumn("playlists", "epg_url");
    if (haveV3) exec("PRAGMA user_version=3");
    else if (haveV2) exec("PRAGMA user_version=2");
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
}

void Database::renamePlaylist(long long playlistId, const std::wstring& name) {
    Stmt q(db_, "UPDATE playlists SET name=? WHERE id=?");
    if (!q) return;
    q.bindText(1, name);
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
    Stmt ins(db_,
             "INSERT INTO channels("
             "playlist_id,name,stream_url,logo_url,group_title,tvg_id,tvg_name,lcn,sort_order,"
             "user_agent,referrer) VALUES(?,?,?,?,?,?,?,?,?,?,?) "
             "ON CONFLICT(playlist_id,stream_url) DO UPDATE SET "
             "name=excluded.name,logo_url=excluded.logo_url,group_title=excluded.group_title,"
             "tvg_id=excluded.tvg_id,tvg_name=excluded.tvg_name,sort_order=excluded.sort_order,"
             "user_agent=excluded.user_agent,referrer=excluded.referrer");
    if (!ins) return 0;
    int n = 0, order = 0;
    for (const ParsedChannel& c : channels) {
        if (!c.isValid()) continue;
        ins.reset();
        ins.bindInt(1, playlistId);
        ins.bindText(2, c.name);
        ins.bindText(3, c.streamUrl);
        ins.bindText(4, c.logoUrl);
        ins.bindText(5, c.groupTitle);
        ins.bindText(6, c.tvgId);
        ins.bindText(7, c.tvgName);
        ins.bindOptInt(8, c.chno >= 0 ? std::optional<int>(c.chno) : std::nullopt);
        ins.bindInt(9, order++);
        ins.bindText(10, c.userAgent);
        ins.bindText(11, c.referrer);
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
    tx.commit();
    return n;
}

std::vector<Channel> Database::allChannels() {
    return runChannelQuery(
        db_, std::string("SELECT ") + kChannelCols + " FROM channels WHERE " + kEnabledOnly +
                 " ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE");
}

std::vector<Channel> Database::channelsByPlaylist(long long playlistId) {
    return runChannelQuery(db_,
                           std::string("SELECT ") + kChannelCols +
                               " FROM channels WHERE playlist_id=? "
                               "ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE",
                           nullptr, playlistId);
}

std::vector<Channel> Database::channelsByGroup(const std::wstring& group) {
    return runChannelQuery(db_,
                           std::string("SELECT ") + kChannelCols +
                               " FROM channels WHERE group_title=? AND " + kEnabledOnly +
                               " ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE",
                           &group);
}

std::vector<Channel> Database::favourites() {
    return runChannelQuery(db_, std::string("SELECT ") + kChannelCols +
                                    " FROM channels WHERE is_favourite=1 AND " + kEnabledOnly +
                                    " ORDER BY (lcn IS NULL), lcn, name COLLATE NOCASE");
}

std::vector<Channel> Database::searchChannels(const std::wstring& term) {
    // Case-insensitive substring match on name/group/tvg_name.
    const std::wstring pattern = L"%" + term + L"%";
    std::vector<Channel> out;
    Stmt q(db_, (std::string("SELECT ") + kChannelCols +
                 " FROM channels WHERE (name LIKE ?1 COLLATE NOCASE "
                 "OR group_title LIKE ?1 COLLATE NOCASE OR tvg_name LIKE ?1 COLLATE NOCASE) AND " +
                 kEnabledOnly + " ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE")
                    .c_str());
    if (!q) return out;
    q.bindText(1, pattern);
    while (q.step()) out.push_back(readChannel(q));
    return out;
}

std::optional<Channel> Database::channelByLcn(int lcn) {
    auto rows = runChannelQuery(db_, std::string("SELECT ") + kChannelCols + " FROM channels WHERE lcn=? AND " +
                                         kEnabledOnly + " ORDER BY sort_order LIMIT 1",
                                nullptr, lcn);
    if (rows.empty()) return std::nullopt;
    return rows.front();
}

std::vector<std::wstring> Database::listGroups() {
    std::vector<std::wstring> out;
    Stmt q(db_, (std::string("SELECT DISTINCT group_title FROM channels WHERE group_title IS NOT NULL "
                             "AND group_title<>'' AND ") +
                 kEnabledOnly + " ORDER BY group_title COLLATE NOCASE")
                    .c_str());
    if (!q) return out;
    while (q.step()) out.push_back(q.textCol(0));
    return out;
}

std::vector<std::wstring> Database::listCountries() {
    std::set<std::wstring> codes;  // sorted + distinct
    Stmt q(db_, (std::string("SELECT tvg_id FROM channels WHERE tvg_id IS NOT NULL AND tvg_id<>'' AND ") +
                 kEnabledOnly)
                    .c_str());
    if (!q) return {};
    while (q.step()) {
        std::wstring cc = countryFromTvgId(q.textCol(0));
        if (!cc.empty()) codes.insert(std::move(cc));
    }
    return std::vector<std::wstring>(codes.begin(), codes.end());
}

std::vector<Channel> Database::channelsByCountry(const std::wstring& code) {
    // Match tvg-ids ending in ".<code>". LIKE is ASCII-case-insensitive, and the code
    // is exactly two letters, so this mirrors countryFromTvgId's last-segment rule.
    const std::wstring pattern = L"%." + code;
    return runChannelQuery(db_,
                           std::string("SELECT ") + kChannelCols +
                               " FROM channels WHERE tvg_id LIKE ? AND " + kEnabledOnly +
                               " ORDER BY (lcn IS NULL), lcn, sort_order, name COLLATE NOCASE",
                           &pattern);
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

// ---- EPG (programmes) ------------------------------------------------------

int Database::bulkInsertProgrammes(long long playlistId, const std::vector<Programme>& programmes,
                                   long long nowEpoch) {
    if (!db_) return 0;
    Tx tx(db_);
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
    tx.commit();
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
