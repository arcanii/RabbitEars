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

// Parse an XMLTV timestamp ("YYYYMMDDHHMMSS ±HHMM", with the seconds and/or zone
// optional) to unix epoch seconds (UTC). A missing zone is treated as UTC. Returns
// 0 when fewer than the leading 8 date digits (YYYYMMDD) are present. Exposed for
// unit testing.
long long parseXmltvTime(const std::string& xmltvTime);

}  // namespace rabbitears
