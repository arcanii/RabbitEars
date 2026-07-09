// SPDX-License-Identifier: GPL-3.0-or-later
// M3U writer — the exporter symmetric with M3uParser (used by favourites export).
// Emits the same Extended-M3U dialect the parser reads, so an exported file
// round-trips through parseM3u() and imports cleanly into any IPTV player.
#pragma once

#include <string>

#include "core/M3uParser.h"

namespace rabbitears {

// Serialize `doc` as UTF-8 Extended-M3U bytes: an #EXTM3U header (with x-tvg-url
// when the document carries an EPG url), then per channel an #EXTINF line with the
// non-empty tvg-* / group-title attributes, any #EXTVLCOPT playback hints, and the
// stream URL. CRLF line endings (friendliest for Windows edits; the parser takes any).
std::string writeM3u(const M3uDocument& doc);

}  // namespace rabbitears
