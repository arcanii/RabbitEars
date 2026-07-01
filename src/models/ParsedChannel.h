// SPDX-License-Identifier: GPL-3.0-or-later
// ParsedChannel — one channel as produced by the M3U/M3U8 parser, before it is
// assigned a database id / playlist_id. Plain data, safe to build off-thread.
#pragma once

#include <string>

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

    bool isValid() const { return !streamUrl.empty(); }
};

}  // namespace rabbitears
