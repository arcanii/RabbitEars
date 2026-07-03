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
};

}  // namespace rabbitears
