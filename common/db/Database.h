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
#include <vector>

#include "models/Channel.h"
#include "models/ParsedChannel.h"
#include "models/Playlist.h"
#include "models/Programme.h"
#include "models/ScheduledRecording.h"

struct sqlite3;  // forward-declared; sqlite3.h is included only in the .cpp

namespace rabbitears {

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Open (creating if needed) the DB at `path`, apply pragmas, and create the
    // schema. Returns false and sets `error` (if non-null) on failure.
    bool open(const std::wstring& path, std::wstring* error = nullptr);
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

    std::vector<Channel> allChannels();
    std::vector<Channel> channelsByPlaylist(long long playlistId);
    std::vector<Channel> channelsByGroup(const std::wstring& group);
    std::vector<Channel> favourites();
    std::vector<Channel> searchChannels(const std::wstring& term);
    std::optional<Channel> channelByLcn(int lcn);
    // First enabled channel carrying this tvg-id (the EPG join key); nullopt if none.
    // Used to resolve a guide programme back to a recordable stream.
    std::optional<Channel> channelByTvgId(const std::wstring& tvgId);

    std::vector<std::wstring> listGroups();
    // Distinct ISO country codes (lowercase) derived from tvg-id suffixes
    // (iptv-org convention: "<name>.<cc>", e.g. "CNN.us"); + channels for one code.
    std::vector<std::wstring> listCountries();
    std::vector<Channel> channelsByCountry(const std::wstring& code);

    void setFavourite(long long channelId, bool favourite);
    void toggleFavourite(long long channelId);
    void setChannelNumber(long long channelId, std::optional<int> lcn);
    void setDeadStatus(long long channelId, DeadStatus status, long long nowEpoch);

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

    // ---- Scheduled recordings ----------------------------------------------
    long long addSchedule(const ScheduledRecording& s);  // returns the new id, or 0 on failure
    std::vector<ScheduledRecording> listSchedules();     // ordered by start_utc
    void updateScheduleStatus(long long id, ScheduleStatus status, const std::wstring& filePath = {});
    void deleteSchedule(long long id);

    // ---- Settings (key/value blob) ----------------------------------------
    std::optional<std::wstring> getSetting(const std::wstring& key);
    void setSetting(const std::wstring& key, const std::wstring& value);

    const std::wstring& lastError() const { return lastError_; }

private:
    bool exec(const char* sql);
    bool createSchema();
    void migrate();  // incremental, idempotent schema upgrades keyed off PRAGMA user_version
    bool hasColumn(const char* table, const char* column);  // PRAGMA table_info membership test

    sqlite3*     db_ = nullptr;
    std::wstring lastError_;
};

}  // namespace rabbitears
