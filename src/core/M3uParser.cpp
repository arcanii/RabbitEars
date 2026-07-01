// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/M3uParser.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>

#include "platform/Encoding.h"

namespace rabbitears {
namespace {

// All tokenizing is done on the UTF-8 bytes: every delimiter we care about
// (" , = # : whitespace) is ASCII, and UTF-8 continuation bytes are always
// >= 0x80, so multibyte sequences can never be mistaken for a delimiter.

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (isSpace(s[b]) || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (isSpace(s[e - 1]) || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

// Case-insensitive prefix test for directive keywords (#EXTINF etc.).
bool startsWithCI(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Split a byte buffer into lines on \n, \r\n, or lone \r. The trailing \r of a
// \r\n pair is dropped here; per-line trimming handles any stragglers.
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\n') {
            out.push_back(std::move(cur));
            cur.clear();
        } else if (c == '\r') {
            out.push_back(std::move(cur));
            cur.clear();
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;  // swallow the \n of \r\n
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// Parse `key="value"` and `key=value` attribute pairs out of an EXTINF/EXTM3U
// metadata run. Quoted values may contain spaces, commas and semicolons; an
// unquoted value runs to the next whitespace. Keys are lowercased. Any leading
// duration token (which has no '=') is naturally skipped.
std::map<std::string, std::string> parseAttributes(const std::string& meta) {
    std::map<std::string, std::string> attrs;
    size_t i = 0;
    const size_t n = meta.size();
    while (i < n) {
        // Advance to the start of an identifier.
        while (i < n && !(std::isalnum(static_cast<unsigned char>(meta[i])) || meta[i] == '_'))
            ++i;
        const size_t keyStart = i;
        while (i < n && (std::isalnum(static_cast<unsigned char>(meta[i])) || meta[i] == '_' ||
                         meta[i] == '-'))
            ++i;
        if (i == keyStart) {  // no identifier char consumed; skip one and retry
            ++i;
            continue;
        }
        if (i >= n || meta[i] != '=') continue;  // not a key=value token
        std::string key = toLowerAscii(meta.substr(keyStart, i - keyStart));
        ++i;  // consume '='
        std::string value;
        if (i < n && meta[i] == '"') {
            ++i;  // opening quote
            const size_t vs = i;
            while (i < n && meta[i] != '"') ++i;
            value = meta.substr(vs, i - vs);
            if (i < n) ++i;  // closing quote
        } else {
            const size_t vs = i;
            while (i < n && !isSpace(meta[i])) ++i;
            value = meta.substr(vs, i - vs);
        }
        if (!key.empty()) attrs[key] = value;
    }
    return attrs;
}

// Index of the first comma NOT inside a double-quoted run, or npos. This is the
// separator between the EXTINF metadata and the display name — NOT the last
// comma (attribute values and titles can both contain commas).
size_t firstUnquotedComma(const std::string& s) {
    bool inQuotes = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') inQuotes = !inQuotes;
        else if (s[i] == ',' && !inQuotes) return i;
    }
    return std::string::npos;
}

int parseIntOr(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || errno != 0) return fallback;
    return static_cast<int>(v);
}

void applyExtinf(const std::string& afterColon, ParsedChannel& ch) {
    const size_t comma = firstUnquotedComma(afterColon);
    const std::string meta = (comma == std::string::npos) ? afterColon : afterColon.substr(0, comma);
    std::string title = (comma == std::string::npos) ? std::string() : afterColon.substr(comma + 1);
    title = trim(title);

    const auto attrs = parseAttributes(meta);
    auto get = [&](const char* k) -> std::string {
        auto it = attrs.find(k);
        return it == attrs.end() ? std::string() : it->second;
    };

    ch.name = wideFromUtf8(title);
    ch.logoUrl = wideFromUtf8(get("tvg-logo"));
    ch.groupTitle = wideFromUtf8(get("group-title"));
    ch.tvgId = wideFromUtf8(get("tvg-id"));
    ch.tvgName = wideFromUtf8(get("tvg-name"));
    // Channel number: tvg-chno is the common key; channel-number / tvg-channel-number seen too.
    std::string chno = get("tvg-chno");
    if (chno.empty()) chno = get("channel-number");
    if (chno.empty()) chno = get("tvg-channel-number");
    ch.chno = parseIntOr(chno, -1);
    // Inline playback hints (also may arrive via #EXTVLCOPT lines below).
    if (!get("http-user-agent").empty()) ch.userAgent = wideFromUtf8(get("http-user-agent"));
    if (!get("http-referrer").empty()) ch.referrer = wideFromUtf8(get("http-referrer"));

    if (ch.name.empty()) ch.name = ch.tvgName;  // fall back to tvg-name
}

// Derive a readable name for a bare-URL entry (last path segment, no query).
std::wstring nameFromUrl(const std::string& url) {
    std::string u = url;
    const size_t q = u.find_first_of("?#");
    if (q != std::string::npos) u = u.substr(0, q);
    while (!u.empty() && u.back() == '/') u.pop_back();
    const size_t slash = u.find_last_of('/');
    std::string seg = (slash == std::string::npos) ? u : u.substr(slash + 1);
    return seg.empty() ? wideFromUtf8(url) : wideFromUtf8(seg);
}

}  // namespace

M3uDocument parseM3u(const std::string& utf8Bytes) {
    M3uDocument doc;

    // Strip a UTF-8 BOM if present.
    std::string bytes = utf8Bytes;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);

    ParsedChannel pending;
    bool havePending = false;

    for (const std::string& raw : splitLines(bytes)) {
        const std::string line = trim(raw);
        if (line.empty()) continue;

        if (startsWithCI(line, "#EXTM3U")) {
            const auto attrs = parseAttributes(line.substr(7));
            auto it = attrs.find("x-tvg-url");
            if (it == attrs.end()) it = attrs.find("url-tvg");
            if (it != attrs.end()) doc.epgUrl = wideFromUtf8(it->second);
            continue;
        }
        if (startsWithCI(line, "#EXTINF:")) {
            pending = ParsedChannel{};
            havePending = true;
            applyExtinf(line.substr(8), pending);
            continue;
        }
        if (startsWithCI(line, "#EXTGRP:")) {
            if (havePending && pending.groupTitle.empty())
                pending.groupTitle = wideFromUtf8(trim(line.substr(8)));
            continue;
        }
        if (startsWithCI(line, "#EXTVLCOPT:")) {
            if (havePending) {
                const std::string opt = trim(line.substr(11));
                const size_t eq = opt.find('=');
                if (eq != std::string::npos) {
                    const std::string k = toLowerAscii(trim(opt.substr(0, eq)));
                    const std::string v = trim(opt.substr(eq + 1));
                    if (k == "http-user-agent") pending.userAgent = wideFromUtf8(v);
                    else if (k == "http-referrer") pending.referrer = wideFromUtf8(v);
                }
            }
            continue;
        }
        if (!line.empty() && line[0] == '#') continue;  // any other directive/comment

        // A non-comment line is the stream URL that closes the current entry.
        if (havePending) {
            pending.streamUrl = wideFromUtf8(line);
            if (pending.name.empty()) pending.name = nameFromUrl(line);
            doc.channels.push_back(std::move(pending));
            pending = ParsedChannel{};
            havePending = false;
        } else {
            // Bare URL with no preceding #EXTINF (simple playlist).
            ParsedChannel ch;
            ch.streamUrl = wideFromUtf8(line);
            ch.name = nameFromUrl(line);
            doc.channels.push_back(std::move(ch));
        }
    }
    return doc;
}

M3uDocument parseM3uFile(const std::wstring& path, std::wstring* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = L"Could not open file: " + path;
        return {};
    }
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseM3u(bytes);
}

}  // namespace rabbitears
