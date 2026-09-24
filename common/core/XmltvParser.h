// SPDX-License-Identifier: GPL-3.0-or-later
// XmltvParser — a targeted, tolerant pull-parser for XMLTV EPG feeds.
//
// No XML library is vendored; XMLTV is a constrained subset (`<tv>` containing
// `<channel>` and `<programme>` elements), so this hand-rolls a single-pass scanner
// over the UTF-8 bytes — mirroring core/M3uParser's byte-level approach. It extracts
// only what the guide needs (`<programme>` @start/@stop/@channel + title/sub-title/
// desc/category/episode-num/icon) and ignores everything else. Handles XML entities,
// CDATA sections, comments, the XML/DOCTYPE prologue, and self-closing tags.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "models/Programme.h"

namespace rabbitears {

struct XmltvDocument {
    std::vector<Programme> programmes;
};

// Parse an XMLTV document from raw UTF-8 bytes (already gunzipped — see core/Gzip).
// A UTF-8 BOM is tolerated. Malformed islands are skipped, not fatal.
XmltvDocument parseXmltv(const std::string& utf8Bytes);

// The same parse, reporting progress: `onProgress(programmesSoFar)` is called on the parsing
// thread each time the count reaches a multiple of kXmltvProgressEvery (never with 0, and not
// necessarily with the final count). An empty `onProgress` is the plain overload above.
// Added for the Win32 guide refresh's running count. The overload above keeps its signature and
// its results; it now forwards here with an empty callback.
constexpr std::size_t kXmltvProgressEvery = 1000;
XmltvDocument parseXmltv(const std::string& utf8Bytes,
                         const std::function<void(std::size_t)>& onProgress);

// Parse an XMLTV timestamp ("YYYYMMDDHHMMSS ±HHMM", with the seconds and/or zone
// optional) to unix epoch seconds (UTC). A missing zone is treated as UTC. Returns
// 0 when fewer than the leading 8 date digits (YYYYMMDD) are present. Exposed for
// unit testing.
long long parseXmltvTime(const std::string& xmltvTime);

}  // namespace rabbitears
