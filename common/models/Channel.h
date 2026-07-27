// SPDX-License-Identifier: GPL-3.0-or-later
// Channel — a persisted channel row (mirrors the `channels` table). Plain data,
// safe to build off-thread and hand to the UI.
#pragma once

#include <optional>
#include <string>

namespace rabbitears {

// Result of a background ping of the stream URL (roadmap: dead-link checker).
enum class DeadStatus : int { Unknown = 0, Alive = 1, Dead = 2 };

struct Channel {
    long long          id = 0;
    long long          playlistId = 0;
    std::wstring       name;
    std::wstring       streamUrl;
    std::wstring       logoUrl;
    std::wstring       groupTitle;
    std::wstring       tvgId;
    std::wstring       tvgName;
    std::optional<int> lcn;                 // custom channel number; nullopt == unset
    bool               favourite = false;
    DeadStatus         deadStatus = DeadStatus::Unknown;
    long long          lastCheckedAt = 0;   // unix epoch seconds; 0 == never
    int                sortOrder = 0;        // playlist order as parsed

    // Playback hints (see ParsedChannel); empty when not specified.
    std::wstring       userAgent;
    std::wstring       referrer;

    // ---- VOD (schema v8) ----------------------------------------------------
    // A movie is a channel row with kind == Movie. That is deliberate rather than a second
    // table: it gives VOD the existing grid, search, favourites, dead-link handling and
    // playback path for free, and an Xtream "Movies" group already imports this way today —
    // what was missing was only the discriminator and the fields below.
    //
    // EVERY field here defaults to the live-TV answer, so a pre-v8 row read through this
    // struct is a live channel with no duration and no resume point, exactly as before.
    enum class Kind : int { Live = 0, Movie = 1, Episode = 2 };  // Episode: reserved for 0.3.0
    Kind      kind = Kind::Live;
    int       durationSec = 0;   // 0 == unknown, which is also every live channel
    int       resumeSec = 0;     // playback position to offer to resume from; 0 == start
    bool      watched = false;   // set when playback reaches the end (or near it)
    long long addedAt = 0;       // provider's "added" timestamp, for a recently-added sort

    bool isVod() const { return kind != Kind::Live; }
};

}  // namespace rabbitears
