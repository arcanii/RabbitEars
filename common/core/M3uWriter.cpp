// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/M3uWriter.h"

#include "platform/Encoding.h"

namespace rabbitears {
namespace {

// Attribute values sit inside key="…" and the dialect has no escape for an embedded
// quote — replace it with an apostrophe (and line breaks with spaces) so the emitted
// line always re-parses. Lossy only for degenerate names that couldn't round-trip anyway.
std::string attrSafe(const std::wstring& w) {
    std::string s = utf8FromWide(w);
    for (char& c : s) {
        if (c == '"') c = '\'';
        else if (c == '\r' || c == '\n') c = ' ';
    }
    return s;
}

// Free-text fields (display name, URLs, #EXTVLCOPT values) are line-oriented: the
// only character that must not appear is a line break.
std::string lineSafe(const std::wstring& w) {
    std::string s = utf8FromWide(w);
    for (char& c : s) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return s;
}

}  // namespace

std::string writeM3u(const M3uDocument& doc) {
    std::string out = "#EXTM3U";
    if (!doc.epgUrl.empty()) out += " x-tvg-url=\"" + attrSafe(doc.epgUrl) + "\"";
    out += "\r\n";
    for (const ParsedChannel& c : doc.channels) {
        if (!c.isValid()) continue;  // a channel without a stream URL is unplayable — skip
        out += "#EXTINF:-1";
        if (!c.tvgId.empty()) out += " tvg-id=\"" + attrSafe(c.tvgId) + "\"";
        if (!c.tvgName.empty()) out += " tvg-name=\"" + attrSafe(c.tvgName) + "\"";
        if (!c.logoUrl.empty()) out += " tvg-logo=\"" + attrSafe(c.logoUrl) + "\"";
        if (!c.groupTitle.empty()) out += " group-title=\"" + attrSafe(c.groupTitle) + "\"";
        if (c.chno >= 0) out += " tvg-chno=\"" + std::to_string(c.chno) + "\"";
        // The parser splits the display name on the first UNQUOTED comma — every comma
        // before this one sits inside a quoted attribute, so a name containing commas
        // still round-trips intact.
        out += "," + lineSafe(c.name) + "\r\n";
        if (!c.userAgent.empty())
            out += "#EXTVLCOPT:http-user-agent=" + lineSafe(c.userAgent) + "\r\n";
        if (!c.referrer.empty())
            out += "#EXTVLCOPT:http-referrer=" + lineSafe(c.referrer) + "\r\n";
        out += lineSafe(c.streamUrl) + "\r\n";
    }
    return out;
}

}  // namespace rabbitears
