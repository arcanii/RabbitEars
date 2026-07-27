// SPDX-License-Identifier: GPL-3.0-or-later
// ParsedChannel — one channel as produced by the M3U/M3U8 parser, before it is
// assigned a database id / playlist_id. Plain data, safe to build off-thread.
#pragma once

#include <string>

#include "models/Channel.h"  // Channel::Kind — a parsed row carries its own kind

namespace rabbitears {

// The value produced for one #EXTINF entry (+ its following URL and any
// #EXTVLCOPT lines). All text is UTF-16. Missing attributes are empty / -1.
struct ParsedChannel {
    std::wstring name;         // display name (text after the EXTINF metadata comma)
    std::wstring streamUrl;    // the URL line following #EXTINF
    std::wstring logoUrl;      // tvg-logo
    std::wstring groupTitle;   // group-title (may be a ';'-joined multi-category)
    std::wstring tvgId;        // tvg-id (EPG join key)
    std::wstring tvgName;      // tvg-name
    int          chno = -1;    // tvg-chno / channel-number (LCN); -1 == absent

    // Playback hints captured from inline http-* attributes and #EXTVLCOPT lines.
    // Passed to libVLC as per-media options (":http-user-agent=", ":http-referrer=").
    std::wstring userAgent;    // http-user-agent
    std::wstring referrer;     // http-referrer

    // ---- VOD (schema v8) ----------------------------------------------------
    // Defaults are the live-TV answer, so the M3U parser — which never sets either —
    // keeps producing exactly the rows it always has.
    Channel::Kind kind = Channel::Kind::Live;
    long long     addedAt = 0;  // provider's "added" epoch; 0 == unknown
    // NOTE there is deliberately no duration here. The Xtream API does not expose one for
    // a movie (get_vod_info returns an empty `info`), so duration is cached from libVLC at
    // play time instead — see Win32/docs/XTREAM_VOD.md §1 F2.

    bool isValid() const { return !streamUrl.empty(); }
};

}  // namespace rabbitears
