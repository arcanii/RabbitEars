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
    // Returns the new playlist id, or 0 on failure.
    long long addPlaylist(const std::wstring& name, const std::wstring& source, bool isUrl,
                          long long nowEpoch);
    std::vector<Playlist> listPlaylists();
    void deletePlaylist(long long playlistId);

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

    std::vector<std::wstring> listGroups();

    void setFavourite(long long channelId, bool favourite);
    void toggleFavourite(long long channelId);
    void setChannelNumber(long long channelId, std::optional<int> lcn);
    void setDeadStatus(long long channelId, DeadStatus status, long long nowEpoch);

    // ---- Settings (key/value blob) ----------------------------------------
    std::optional<std::wstring> getSetting(const std::wstring& key);
    void setSetting(const std::wstring& key, const std::wstring& value);

    const std::wstring& lastError() const { return lastError_; }

private:
    bool exec(const char* sql);
    bool createSchema();

    sqlite3*     db_ = nullptr;
    std::wstring lastError_;
};

}  // namespace rabbitears
