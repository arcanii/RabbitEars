// SPDX-License-Identifier: GPL-3.0-or-later
// M3U / M3U8 (Extended M3U) parser for IPTV playlists.
//
// Handles the real-world IPTV dialect (validated against
// https://iptv-org.github.io/iptv/index.m3u):
//   #EXTM3U [x-tvg-url="..."] [url-tvg="..."]        -- header, optional EPG url
//   #EXTINF:<duration> <key="value" ...>,<display name>
//   #EXTGRP:<group>                                  -- group for the next entry
//   #EXTVLCOPT:<key>=<value>                          -- e.g. http-user-agent / http-referrer
//   <url>                                            -- the stream URL line
// plus bare-URL "simple" playlists (no #EXTINF).
#pragma once

#include <string>
#include <vector>

#include "models/ParsedChannel.h"

namespace rabbitears {

struct M3uDocument {
    std::wstring               epgUrl;    // x-tvg-url / url-tvg from #EXTM3U (may be empty)
    std::vector<ParsedChannel> channels;
};

// Parse a playlist from raw UTF-8 bytes (a BOM and CR/LF/CRLF are all tolerated).
M3uDocument parseM3u(const std::string& utf8Bytes);

// Read `path` (UTF-16 filesystem path) and parse it. On I/O failure returns an
// empty document and, if `error` is non-null, sets a human-readable message.
M3uDocument parseM3uFile(const std::wstring& path, std::wstring* error = nullptr);

}  // namespace rabbitears
