// SPDX-License-Identifier: GPL-3.0-or-later
// Playlist — a persisted playlist row (mirrors the `playlists` table).
#pragma once

#include <string>

namespace rabbitears {

struct Playlist {
    long long    id = 0;
    std::wstring name;
    std::wstring sourceUrl;         // set when isUrl; else empty
    std::wstring sourcePath;        // set for local-file imports; else empty
    std::wstring epgUrl;            // XMLTV guide URL (from the M3U x-tvg-url); may be empty
    bool         isUrl = true;
    long long    addedAt = 0;       // unix epoch seconds
    long long    lastRefreshedAt = 0;
    int          channelCount = 0;
    bool         enabled = true;    // disabled playlists are hidden from every cross-playlist view
};

}  // namespace rabbitears
