// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/XmltvParser.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "platform/Encoding.h"

namespace rabbitears {
namespace {

// All scanning is on the UTF-8 bytes: every XML delimiter we care about
// (< > / = " ' & and ASCII whitespace) is < 0x80, and UTF-8 continuation bytes are
// always >= 0x80, so a multibyte character can never be mistaken for a delimiter.

bool isXmlSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

char lc(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive compare of a parsed element/attr name against a literal.
bool ieq(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t k = 0; k < n; ++k)
        if (lc(a[k]) != lc(b[k])) return false;
    return true;
}

bool starts(const std::string& s, size_t i, const char* p) {
    const size_t n = std::strlen(p);
    return i + n <= s.size() && std::memcmp(s.data() + i, p, n) == 0;
}

void appendCodepoint(std::string& out, unsigned cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;  // out of range / surrogate
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decode one entity beginning at s[i]=='&' into `out` (UTF-8), advancing i past it.
// `limit` bounds the search for the terminating ';' (so an unescaped '&' can't reach
// across an attribute quote or the whole document). Unknown entities pass literally.
void appendEntity(std::string& out, const std::string& s, size_t& i, size_t limit) {
    size_t semi = std::string::npos;
    for (size_t k = i + 1; k < limit; ++k) {
        if (s[k] == ';') { semi = k; break; }
        if (!(s[k] == '#' || (s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'z') ||
              (s[k] >= 'A' && s[k] <= 'Z')))
            break;  // not a well-formed entity name — bail
    }
    if (semi == std::string::npos) {
        out.push_back('&');
        ++i;
        return;
    }
    const std::string e = s.substr(i + 1, semi - i - 1);
    i = semi + 1;
    if (ieq(e, "amp")) {
        out.push_back('&');
    } else if (ieq(e, "lt")) {
        out.push_back('<');
    } else if (ieq(e, "gt")) {
        out.push_back('>');
    } else if (ieq(e, "quot")) {
        out.push_back('"');
    } else if (ieq(e, "apos")) {
        out.push_back('\'');
    } else if (!e.empty() && e[0] == '#') {
        unsigned cp = 0;
        bool ok = e.size() > 1;
        if (e.size() > 1 && (e[1] == 'x' || e[1] == 'X')) {
            ok = e.size() > 2;
            for (size_t k = 2; k < e.size() && ok; ++k) {
                const char c = e[k];
                if (c >= '0' && c <= '9') cp = cp * 16 + static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f') cp = cp * 16 + static_cast<unsigned>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp = cp * 16 + static_cast<unsigned>(c - 'A' + 10);
                else ok = false;
            }
        } else {
            for (size_t k = 1; k < e.size() && ok; ++k) {
                const char c = e[k];
                if (c >= '0' && c <= '9') cp = cp * 10 + static_cast<unsigned>(c - '0');
                else ok = false;
            }
        }
        if (ok && cp != 0) appendCodepoint(out, cp);
        else { out.push_back('&'); out.append(e); out.push_back(';'); }
    } else {
        out.push_back('&');  // unknown named entity — keep verbatim
        out.append(e);
        out.push_back(';');
    }
}

// Decode a plain byte range [from,to) (entities, no CDATA) — used for attribute values.
std::string decodeRange(const std::string& s, size_t from, size_t to) {
    std::string out;
    size_t i = from;
    while (i < to) {
        if (s[i] == '&') appendEntity(out, s, i, to);
        else { out.push_back(s[i]); ++i; }
    }
    return out;
}

// Read the text content of a leaf element whose opening tag has just been consumed,
// stopping at its matching close tag. Handles CDATA (verbatim), entities, comments,
// and (best-effort) shallow nested markup via depth tracking. Advances i past the
// close tag.
std::string readLeafText(const std::string& s, size_t& i, const std::string& /*name*/) {
    std::string out;
    const size_t n = s.size();
    int depth = 0;
    while (i < n) {
        if (starts(s, i, "<![CDATA[")) {
            const size_t e = s.find("]]>", i + 9);
            const size_t end = (e == std::string::npos) ? n : e;
            out.append(s, i + 9, end - (i + 9));
            i = (e == std::string::npos) ? n : e + 3;
        } else if (starts(s, i, "<!--")) {
            const size_t e = s.find("-->", i + 4);
            i = (e == std::string::npos) ? n : e + 3;
        } else if (starts(s, i, "</")) {
            const size_t gt = s.find('>', i);
            const size_t next = (gt == std::string::npos) ? n : gt + 1;
            if (depth > 0) { --depth; i = next; }  // close of a nested element
            else { i = next; break; }              // our own close tag
        } else if (s[i] == '<') {
            const size_t gt = s.find('>', i);
            const bool selfClose = (gt != std::string::npos && gt > i && s[gt - 1] == '/');
            if (!selfClose) ++depth;  // nested open element
            i = (gt == std::string::npos) ? n : gt + 1;
        } else if (s[i] == '&') {
            appendEntity(out, s, i, n);
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

int digits2(const std::string& d, size_t off) {
    return (d[off] - '0') * 10 + (d[off + 1] - '0');
}

// Days since 1970-01-01 for a proleptic-Gregorian date (Howard Hinnant's algorithm) —
// timezone-independent, so it avoids mktime/localtime pitfalls.
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

}  // namespace

long long parseXmltvTime(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && isXmlSpace(s[i])) ++i;
    std::string digits;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits.size() < 14) {
        digits.push_back(s[i]);
        ++i;
    }
    if (digits.size() < 8) return 0;          // need at least YYYYMMDD
    while (digits.size() < 14) digits.push_back('0');  // pad missing HH/MM/SS

    const long long year = digits2(digits, 0) * 100LL + digits2(digits, 2);
    const unsigned mon = static_cast<unsigned>(digits2(digits, 4));
    const unsigned day = static_cast<unsigned>(digits2(digits, 6));
    const int hh = digits2(digits, 8), mm = digits2(digits, 10), ss = digits2(digits, 12);
    if (mon < 1 || mon > 12 || day < 1 || day > 31) return 0;

    // Optional " ±HHMM" (or "±HH") timezone offset; absent ⇒ UTC.
    int offMin = 0;
    while (i < s.size() && isXmlSpace(s[i])) ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        const int sign = (s[i] == '-') ? -1 : 1;
        ++i;
        std::string tz;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { tz.push_back(s[i]); ++i; }
        if (tz.size() >= 4) offMin = sign * (digits2(tz, 0) * 60 + digits2(tz, 2));
        else if (tz.size() >= 2) offMin = sign * digits2(tz, 0) * 60;
    }

    const long long days = daysFromCivil(year, mon, day);
    return days * 86400LL + hh * 3600LL + mm * 60LL + ss - static_cast<long long>(offMin) * 60LL;
}

XmltvDocument parseXmltv(const std::string& utf8Bytes) {
    XmltvDocument doc;
    const std::string& s = utf8Bytes;
    const size_t n = s.size();
    size_t i = 0;
    if (n >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
        i = 3;  // strip UTF-8 BOM

    Programme cur;
    bool inProg = false;

    while (i < n) {
        if (s[i] != '<') { ++i; continue; }

        if (starts(s, i, "<!--")) {
            const size_t e = s.find("-->", i + 4);
            i = (e == std::string::npos) ? n : e + 3;
            continue;
        }
        if (starts(s, i, "<![CDATA[")) {  // stray CDATA outside a leaf — skip
            const size_t e = s.find("]]>", i + 9);
            i = (e == std::string::npos) ? n : e + 3;
            continue;
        }
        if (starts(s, i, "<?")) {  // <?xml ... ?> processing instruction
            const size_t e = s.find("?>", i + 2);
            i = (e == std::string::npos) ? n : e + 2;
            continue;
        }
        if (starts(s, i, "<!")) {  // <!DOCTYPE ...> (may carry a [ internal subset ])
            size_t j = i + 2;
            int bracket = 0;
            while (j < n) {
                const char c = s[j];
                if (c == '[') ++bracket;
                else if (c == ']') { if (bracket > 0) --bracket; }
                else if (c == '>' && bracket == 0) { ++j; break; }
                ++j;
            }
            i = j;
            continue;
        }
        if (starts(s, i, "</")) {  // end tag
            size_t j = i + 2;
            std::string name;
            while (j < n && s[j] != '>' && !isXmlSpace(s[j])) { name.push_back(s[j]); ++j; }
            const size_t gt = s.find('>', j);
            i = (gt == std::string::npos) ? n : gt + 1;
            if (inProg && ieq(name, "programme")) {
                doc.programmes.push_back(std::move(cur));
                cur = Programme{};
                inProg = false;
            }
            continue;
        }

        // Start tag: '<' name (attr)* ('/')? '>'
        size_t j = i + 1;
        std::string name;
        while (j < n && s[j] != '>' && s[j] != '/' && !isXmlSpace(s[j])) { name.push_back(s[j]); ++j; }

        std::vector<std::pair<std::string, std::string>> attrs;
        bool selfClose = false;
        while (j < n && s[j] != '>') {
            if (s[j] == '/') { selfClose = true; ++j; continue; }
            if (isXmlSpace(s[j])) { ++j; continue; }
            std::string an;
            while (j < n && s[j] != '=' && s[j] != '>' && s[j] != '/' && !isXmlSpace(s[j])) {
                an.push_back(s[j]);
                ++j;
            }
            while (j < n && isXmlSpace(s[j])) ++j;
            std::string av;
            if (j < n && s[j] == '=') {
                ++j;
                while (j < n && isXmlSpace(s[j])) ++j;
                if (j < n && (s[j] == '"' || s[j] == '\'')) {
                    const char q = s[j];
                    ++j;
                    const size_t vs = j;
                    while (j < n && s[j] != q) ++j;
                    av = decodeRange(s, vs, j);
                    if (j < n) ++j;  // past the closing quote
                } else {
                    const size_t vs = j;
                    while (j < n && !isXmlSpace(s[j]) && s[j] != '>' && s[j] != '/') ++j;
                    av = decodeRange(s, vs, j);
                }
            }
            if (!an.empty()) attrs.emplace_back(std::move(an), std::move(av));
        }
        if (j < n && s[j] == '>') ++j;
        i = j;  // positioned just past the start tag

        if (ieq(name, "programme")) {
            cur = Programme{};
            for (const auto& kv : attrs) {
                if (ieq(kv.first, "channel")) cur.channelId = wideFromUtf8(kv.second);
                else if (ieq(kv.first, "start")) cur.startUtc = parseXmltvTime(kv.second);
                else if (ieq(kv.first, "stop")) cur.stopUtc = parseXmltvTime(kv.second);
            }
            inProg = true;
            if (selfClose) {  // empty <programme .../> — unusual, but don't lose it
                doc.programmes.push_back(std::move(cur));
                cur = Programme{};
                inProg = false;
            }
        } else if (inProg) {
            if (ieq(name, "icon")) {
                for (const auto& kv : attrs)
                    if (ieq(kv.first, "src")) cur.iconUrl = wideFromUtf8(kv.second);
                if (!selfClose) readLeafText(s, i, name);  // icon is normally empty; consume any body
            } else if (!selfClose) {
                const std::wstring w = wideFromUtf8(readLeafText(s, i, name));
                if (ieq(name, "title")) { if (cur.title.empty()) cur.title = w; }
                else if (ieq(name, "sub-title")) { if (cur.subTitle.empty()) cur.subTitle = w; }
                else if (ieq(name, "desc")) { if (cur.descr.empty()) cur.descr = w; }
                else if (ieq(name, "category")) { if (cur.category.empty()) cur.category = w; }
                else if (ieq(name, "episode-num")) { if (cur.episodeNum.empty()) cur.episodeNum = w; }
            }
        }
        // Tags outside a <programme> (tv, channel, display-name, …) are ignored; their
        // text is skipped harmlessly by the top-of-loop non-'<' advance.
    }
    return doc;
}

}  // namespace rabbitears
