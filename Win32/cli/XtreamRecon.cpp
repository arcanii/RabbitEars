// SPDX-License-Identifier: GPL-3.0-or-later
#include "XtreamRecon.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include "core/Http.h"
#include "db/Database.h"
#include "platform/Encoding.h"

namespace rabbitears {
namespace {

// ---------------------------------------------------------------------------
// Output plumbing — everything goes to stdout AND into the saved report, so the
// thing the owner reads on screen is byte-identical to the thing they hand back.
// ---------------------------------------------------------------------------

std::string g_report;

void say(const std::string& s) {
    fwrite(s.data(), 1, s.size(), stdout);
    g_report += s;
}
void sayln(const std::string& s = {}) { say(s + "\n"); }

// 12431 -> "12,431". Item counts here run to tens of thousands and the whole point
// of the report is that a human eyeballs them.
std::string commas(long long v) {
    std::string d = std::to_string(v < 0 ? -v : v), o;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) o += ',';
        o += d[i];
    }
    return (v < 0 ? "-" : "") + o;
}

std::string pad(const std::string& s, size_t w) {
    return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}

// Bytes as a human size. The transfer size of get_vod_streams is a design input
// (it decides whether a full sync is viable or the client must page by category).
std::string sizeStr(size_t bytes) {
    char buf[64];
    if (bytes < 1024) sprintf_s(buf, "%zu B", bytes);
    else if (bytes < 1024 * 1024) sprintf_s(buf, "%.1f KB", bytes / 1024.0);
    else sprintf_s(buf, "%.2f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

// ---------------------------------------------------------------------------
// Redaction. A recon report is worthless if it cannot be shared, and an Xtream
// response leaks credentials in three separate places: the URL we echo, the
// `user_info` block player_api.php returns, and every absolute stream/icon URL
// embedded in the item lists. So redaction happens ONCE, on the raw bytes, before
// anything is scanned, printed or saved — there is no path by which an
// unredacted byte reaches the report.
// ---------------------------------------------------------------------------

// %40 -> '@', '+' -> ' '. The credential appears PERCENT-ENCODED in the playlist URL but
// DECODED inside the JSON (a panel's own `direct_source` field embeds it in cleartext), so
// both spellings have to be registered or the decoded one ships.
std::string percentDecode(const std::string& s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { o += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            const int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { o += static_cast<char>(h * 16 + l); i += 2; continue; }
        }
        o += s[i];
    }
    return o;
}

struct Redactor {
    bool enabled = true;

    // Every spelling of every secret, longest first (so a credential that is a prefix of
    // another cannot be half-replaced), each with the tag that replaces it.
    std::vector<std::pair<std::string, std::string>> tokens;
    std::wstring wuser, wpass;  // wide originals, for the structural URL segment rewrite
    std::string  winUser;       // the OS account name, which is in every saved path

    void add(const std::string& v, const char* tag) {
        if (v.empty()) return;
        for (const auto& t : tokens)
            if (t.first == v) return;
        tokens.emplace_back(v, tag);
    }
    void finish() {  // longest first
        std::sort(tokens.begin(), tokens.end(),
                  [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    }

    // ---- URLs: rewritten STRUCTURALLY, by POSITION, never by matching the secret's text.
    // This is the difference that matters: a 2-character password is exactly as safe as a
    // 20-character one, which no text replace can promise. Query values are replaced by
    // PARAMETER NAME and path segments by WHOLE-SEGMENT equality.
    std::wstring urlW(const std::wstring& u) const {
        if (!enabled) return u;
        const size_t q = u.find(L'?');
        std::wstring path = (q == std::wstring::npos) ? u : u.substr(0, q);
        const std::wstring query = (q == std::wstring::npos) ? std::wstring() : u.substr(q + 1);

        // authority: scheme://[userinfo@]HOST[:port]
        // The userinfo split is not pedantry. `http://joe:hunter2@panel.tld/…` put a basic-auth
        // password AND the real hostname into the report in cleartext, because everything from
        // the first ':' onward was assumed to be ":port" and copied through verbatim.
        std::wstring head, rest;
        const size_t slashes = path.find(L"//");
        if (slashes != std::wstring::npos) {
            const size_t authStart = slashes + 2;
            size_t authEnd = path.find(L'/', authStart);
            if (authEnd == std::wstring::npos) authEnd = path.size();
            const std::wstring auth = path.substr(authStart, authEnd - authStart);

            const size_t at = auth.rfind(L'@');  // LAST '@' — userinfo may contain one
            const size_t hostStart = (at == std::wstring::npos) ? 0 : at + 1;
            // An IPv6 literal is bracketed and full of colons, so the port is the ':' AFTER
            // the ']', not the first one.
            size_t portAt;
            if (hostStart < auth.size() && auth[hostStart] == L'[') {
                const size_t close = auth.find(L']', hostStart);
                portAt = (close == std::wstring::npos) ? std::wstring::npos : auth.find(L':', close);
            } else {
                portAt = auth.find(L':', hostStart);
            }
            const std::wstring port =
                (portAt == std::wstring::npos) ? std::wstring() : auth.substr(portAt);
            // Userinfo is dropped entirely rather than tagged: it is a credential we were never
            // asked to preserve the shape of, and keeping a placeholder invites reading it as data.
            head = path.substr(0, authStart) + L"<HOST>" + port;
            rest = path.substr(authEnd);
        } else {
            rest = path;
        }
        // path segments: /movie/USER/PASS/1234.mp4 — exact whole-segment compare, so a
        // credential that happens to be a substring of a segment is never mangled.
        std::wstring outPath = head;
        for (size_t p = 0;;) {
            const size_t s = rest.find(L'/', p);
            const std::wstring seg =
                rest.substr(p, s == std::wstring::npos ? std::wstring::npos : s - p);
            if (!wpass.empty() && seg == wpass) outPath += L"<PASS>";
            else if (!wuser.empty() && seg == wuser) outPath += L"<USER>";
            else outPath += seg;
            if (s == std::wstring::npos) break;
            outPath += L'/';
            p = s + 1;
        }

        if (query.empty()) return outPath;
        std::wstring outQ;
        for (size_t p = 0;;) {
            const size_t amp = query.find(L'&', p);
            std::wstring pair =
                query.substr(p, amp == std::wstring::npos ? std::wstring::npos : amp - p);
            const size_t eq = pair.find(L'=');
            if (eq != std::wstring::npos) {
                const std::wstring k = pair.substr(0, eq);
                if (k == L"username" || k == L"user") pair = k + L"=<USER>";
                else if (k == L"password" || k == L"pass") pair = k + L"=<PASS>";
            }
            outQ += pair;
            if (amp == std::wstring::npos) break;
            outQ += L'&';
            p = amp + 1;
        }
        return outPath + L"?" + outQ;
    }
    // Belt AND braces: the structural pass by position, THEN the guarded text pass. The
    // structural rules are exact but positional, so a credential carrying a URL delimiter
    // ('/' or '?') splits across two positions and slips through — and scrub() catches
    // exactly that, because both halves end up delimiter-bounded.
    std::string url(const std::wstring& u) const { return scrub(utf8FromWide(urlW(u))); }

    // A secret is only replaced where it is delimited on BOTH sides by a URL/JSON
    // separator. Without that guard, a credential that happens to be an ordinary word
    // ("stream", "series", "player", "movies") rewrites the very FIELD NAMES the census is
    // measuring — and the result does not look like corruption in the report, it looks
    // like a provider that has no stream_id and no container_extension.
    static bool sep(char c) {
        return c == '/' || c == '=' || c == '&' || c == '"' || c == '?' || c == '\\' ||
               c == ':' || c == '\'' || c == '@' || c == ',' || c == '{' || c == '}';
    }
    // An ALL-DIGIT credential needs a narrower rule. Some panels issue numeric usernames, and
    // with the general set a password of "12345678" matches inside `"stream_id":12345678,` —
    // bounded by ':' and ','. Rewriting that yields `"stream_id":<PASS>,`, the scanner bails,
    // and EVERY response is reported unparseable with the headline verdict flipped to NO.
    // So a numeric token is only replaced in string/URL context, never in JSON value position.
    static bool sepNumeric(char c) {
        return c == '/' || c == '=' || c == '&' || c == '"' || c == '?' || c == '\\' || c == '@';
    }
    static bool allDigitsTok(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        return true;
    }

    // One pass, building a new string. The previous in-place version was O(hits × body),
    // which on a 30 MB list whose every item embeds the origin is minutes of silent memmove.
    std::string scrub(const std::string& in) const {
        if (!enabled || tokens.empty()) return in;
        std::string o;
        o.reserve(in.size());
        for (size_t i = 0; i < in.size();) {
            bool hit = false;
            for (const auto& t : tokens) {
                if (i + t.first.size() > in.size()) continue;
                if (in.compare(i, t.first.size(), t.first) != 0) continue;
                const size_t after = i + t.first.size();
                const bool numeric = allDigitsTok(t.first);
                if (i != 0 && !(numeric ? sepNumeric(in[i - 1]) : sep(in[i - 1]))) continue;
                if (after != in.size() && !(numeric ? sepNumeric(in[after]) : sep(in[after]))) continue;
                o += t.second;
                i = after;
                hit = true;
                break;
            }
            if (!hit) o += in[i++];
        }
        return o;
    }

    // Blank the VALUE of any "password"/"username" member whatever it holds — exact, and
    // independent of length or encoding, so it covers what the guarded pass cannot.
    static void blankCredMembers(std::string& s) {
        static const char* kKeys[] = {"\"password\"", "\"username\"", "\"user\"", "\"pass\""};
        for (const char* key : kKeys) {
            const std::string k = key;
            const std::string tag = (k == "\"password\"" || k == "\"pass\"") ? "<PASS>" : "<USER>";
            for (size_t i = s.find(k); i != std::string::npos; i = s.find(k, i + 1)) {
                size_t j = i + k.size();
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                if (j >= s.size() || s[j] != ':') continue;
                ++j;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                if (j >= s.size() || s[j] != '"') continue;  // non-string value: leave it
                const size_t vs = j + 1;
                size_t ve = vs;
                while (ve < s.size() && s[ve] != '"') ve += (s[ve] == '\\' ? 2 : 1);
                if (ve > s.size()) break;
                s.replace(vs, ve - vs, tag);
            }
        }
    }

    std::string body(std::string s) const {
        if (!enabled) return s;
        blankCredMembers(s);
        return scrub(s);
    }
    // Free text (a playlist name, an error message, a filesystem path). Also strips the OS
    // account name, which is otherwise baked into every path this report prints.
    std::string text(const std::wstring& w) const {
        std::string s = utf8FromWide(w);
        if (!enabled) return s;
        s = scrub(s);
        if (!winUser.empty()) {
            for (size_t i = s.find(winUser); i != std::string::npos;
                 i = s.find(winUser, i + 6))
                s.replace(i, winUser.size(), "<USER>");
        }
        return s;
    }
};

// ---------------------------------------------------------------------------
// The census scanner.
//
// A streaming, permissive JSON walk that never materializes a tree — a 30 MB
// get_vod_streams with 40k items must not cost 40k x 15 nodes of RAM — and records,
// per PATH rather than per value:
//   * how many times the path carried a value (=> which fields are OPTIONAL),
//   * the type histogram (=> numbers-as-strings, the flagged provider risk),
//   * up to three sample values.
//
// Object keys that are entirely digits collapse to '#'. That is not a shortcut: it is
// how the awkward Xtream `episodes` shape — a map keyed by season NUMBER, not an array —
// becomes visible as one path instead of N, and it bounds the map on a hostile response.
// ---------------------------------------------------------------------------

struct PathInfo {
    std::string parent;
    long long count = 0;
    long long nString = 0, nNumber = 0, nBool = 0, nNull = 0, nObject = 0, nArray = 0;
    long long nNumericString = 0;  // string whose CONTENT is a number: "7.8", "1234"
    long long nEmpty = 0;          // "" or null — the other way a field is absent
    std::vector<std::string> samples;
    // A separate example of a QUOTED NUMBER. samples[] holds the first three DISTINCT
    // values of any kind, so quoting `e.g. samples.front()` next to a numbers-as-strings
    // finding can print a non-numeric example ("Inception") beside the claim.
    std::string numericSample;
    int order = 0;  // first-seen order, so the report reads in document order
};

struct Census {
    std::map<std::string, PathInfo> paths;
    std::map<std::string, std::set<std::string>> numericKeys;  // parent path -> the digit keys seen
    // Every direct member of the FIRST element of a root array, key -> sample. The census
    // is otherwise per-PATH and carries no per-item correlation, so without this the play
    // URL would be built from item #1's stream_id and item #2's container_extension —
    // producing a confident, wrong "NOT reachable".
    std::map<std::string, std::string> firstItem;
    int nextOrder = 0;
    bool truncated = false;      // hit the distinct-path cap
    std::string trailing;        // non-empty: junk AFTER the root value (a PHP notice, say)
    static constexpr size_t kMaxPaths = 4000;
    static constexpr size_t kMaxSampleLen = 48;

    bool rootIsArray() const {
        const auto it = paths.find("");
        return it != paths.end() && it->second.nArray > 0;
    }
    bool hasRoot() const { return paths.find("") != paths.end(); }

    void record(const std::string& path, const std::string& parent, char type,
                const std::string& sample, bool numericString, bool empty) {
        auto it = paths.find(path);
        if (it == paths.end()) {
            if (paths.size() >= kMaxPaths) { truncated = true; return; }
            it = paths.emplace(path, PathInfo{}).first;
            it->second.parent = parent;
            it->second.order = nextOrder++;
        }
        PathInfo& p = it->second;
        ++p.count;
        switch (type) {
            case 's': ++p.nString; break;
            case 'n': ++p.nNumber; break;
            case 'b': ++p.nBool;   break;
            case 'z': ++p.nNull;   break;
            case 'o': ++p.nObject; break;
            case 'a': ++p.nArray;  break;
            default: break;
        }
        if (numericString) {
            ++p.nNumericString;
            if (p.numericSample.empty()) p.numericSample = sample;
        }
        if (empty) ++p.nEmpty;
        if (!sample.empty() && p.samples.size() < 3 &&
            std::find(p.samples.begin(), p.samples.end(), sample) == p.samples.end())
            p.samples.push_back(sample);

        // Direct members of the first root-array element. "[]" is recorded when that
        // element's object is ENTERED, so its count is still 1 for the whole of item #1
        // and 2 onwards for every later item — which is exactly the window we want.
        // Only STRING and NUMBER values are usable identities. A JSON null records the sample
        // text "null", and storing that made `container_extension: null` — a MySQL NULL through
        // json_encode, entirely routine — look like the string "null": the verdict then reported
        // the field present "on all 12,431 items, e.g. \"null\"" and probed
        // /movie/u/p/1234.null, printing a confident "NOT reachable" for a working provider.
        if ((type == 's' || type == 'n') && !empty && !sample.empty() &&
            path.compare(0, 3, "[].") == 0 && path.find_first_of(".[", 3) == std::string::npos) {
            const auto root = paths.find("[]");
            if (root != paths.end() && root->second.count == 1)
                firstItem.emplace(path.substr(3), sample);
        }
    }
};

// Cap on the numeric keys remembered per parent. Reported as "200+" when hit, never as a
// bare count — a capped number printed as fact is a lie the reader cannot detect.
constexpr size_t kMaxNumericKeys = 200;

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Is this STRING's content a number? The single most design-relevant question in the
// whole report: Xtream panels routinely quote numeric fields, inconsistently, and a
// strict parser that expects a number will hard-fail on exactly those providers.
bool looksNumeric(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    bool digit = false, dot = false;
    for (; i < s.size(); ++i) {
        if (s[i] >= '0' && s[i] <= '9') { digit = true; continue; }
        if (s[i] == '.' && !dot) { dot = true; continue; }
        return false;
    }
    return digit;
}

class Scanner {
public:
    // Holds the body by reference — it is up to 30 MB and is never copied. The reference must
    // outlive the Scanner, so never construct one from a temporary (a string literal builds
    // one that dies at the end of the construction statement).
    Scanner(const std::string& s, Census& c) : s_(s), c_(c) {}
    Scanner(std::string&&, Census&) = delete;  // make that mistake a compile error

    // Returns false and fills `err` (with a byte offset and surrounding context) on a
    // parse failure. A failure is a FINDING, not a crash — "this provider emits
    // something we cannot read, here is exactly where" is a useful recon answer.
    bool run(std::string& err) {
        skipWs();
        if (i_ >= s_.size()) { err = "empty body"; return false; }
        if (!value("", "", 0)) { err = fail_; return false; }
        skipWs();
        // Junk AFTER the root value. A PHP notice APPENDED to the response parses as a
        // clean document up to that point, so without this check the census is printed as
        // authoritative and the warning is invisible. (A PREPENDED notice is caught earlier,
        // by the not-JSON check in probe().)
        if (i_ < s_.size()) {
            std::string tail = s_.substr(i_, 80);
            for (char& c : tail)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            c_.trailing = "trailing junk after the root value at byte " +
                          commas(static_cast<long long>(i_)) + ": " + tail;
        }
        return true;
    }

private:
    const std::string& s_;
    Census& c_;
    size_t i_ = 0;
    std::string fail_;

    void skipWs() {
        // A UTF-8 BOM is skipped as whitespace. A PHP file saved with a BOM is a textbook
        // panel misconfiguration, and treating it as "not JSON" would flip the headline
        // verdict to NO on a provider that works perfectly.
        if (i_ + 3 <= s_.size() && static_cast<unsigned char>(s_[i_]) == 0xEF &&
            static_cast<unsigned char>(s_[i_ + 1]) == 0xBB &&
            static_cast<unsigned char>(s_[i_ + 2]) == 0xBF)
            i_ += 3;
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool bail(const char* what) {
        const size_t from = i_ > 30 ? i_ - 30 : 0;
        std::string ctx = s_.substr(from, 60);
        for (char& c : ctx)
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        fail_ = std::string(what) + " at byte " + commas(static_cast<long long>(i_)) + " near: ..." +
                ctx + "...";
        return false;
    }

    static void appendUtf8(std::string& o, unsigned long cp) {
        if (cp <= 0x7F) { o += static_cast<char>(cp); }
        else if (cp <= 0x7FF) {
            o += static_cast<char>(0xC0 | (cp >> 6));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned long& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + k];
            unsigned long d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned long>(c - 'A' + 10);
            else return false;
            out = (out << 4) | d;
        }
        i_ += 4;
        return true;
    }

    bool str(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return bail("expected string");
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '"') { ++i_; return true; }
            if (c != '\\') { out += c; ++i_; continue; }
            if (++i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!hex4(cp)) return bail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() && s_[i_] == '\\' &&
                        s_[i_ + 1] == 'u') {
                        const size_t save = i_;
                        i_ += 2;
                        unsigned long lo = 0;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            i_ = save;  // lone high surrogate — handled just below
                    }
                    // An unpaired surrogate has no UTF-8 encoding. Emitting one anyway
                    // would put invalid bytes into a report the owner is asked to read and
                    // forward, where it is indistinguishable from a real encoding bug.
                    // U+FFFD is the honest rendering. NUL likewise cannot go into a text
                    // file that is written with an explicit byte count.
                    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp == 0) cp = 0xFFFD;
                    appendUtf8(out, cp);
                    break;
                }
                // Permissive on purpose: an unknown escape is passed through rather than
                // rejected. We are measuring providers, not validating them.
                default: out += e; break;
            }
        }
        return bail("unterminated string");
    }

    // Back off to a UTF-8 lead byte so a truncated sample is never cut mid-sequence —
    // otherwise an accented title turns into mojibake in the very report that exists to
    // show the owner whether the PROVIDER's encoding is sound.
    static size_t utf8Boundary(const std::string& v, size_t n) {
        if (n >= v.size()) return v.size();
        while (n > 0 && (static_cast<unsigned char>(v[n]) & 0xC0) == 0x80) --n;
        return n;
    }

    static std::string trim(std::string v) {
        for (char& c : v)
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (v.size() > Census::kMaxSampleLen)
            v = v.substr(0, utf8Boundary(v, Census::kMaxSampleLen)) + "\xE2\x80\xA6";
        return v;
    }

    // path/parent are the census keys; depth guards a pathological nesting bomb.
    bool value(const std::string& path, const std::string& parent, int depth) {
        if (depth > 64) return bail("nesting deeper than 64");
        skipWs();
        if (i_ >= s_.size()) return bail("truncated");
        const char c = s_[i_];

        if (c == '{') {
            c_.record(path, parent, 'o', {}, false, false);
            ++i_;
            skipWs();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            for (;;) {
                skipWs();
                std::string key;
                if (!str(key)) return false;
                skipWs();
                if (i_ >= s_.size() || s_[i_] != ':') return bail("expected ':'");
                ++i_;
                std::string kp = key;
                if (allDigits(key)) {
                    kp = "#";
                    // Bound the outer map too, not just each set: `paths` has a cap but
                    // this one did not, so a body of a million distinct numeric-keyed
                    // objects grew it without limit.
                    if (c_.numericKeys.size() < Census::kMaxPaths ||
                        c_.numericKeys.count(path)) {
                        auto& set = c_.numericKeys[path];
                        if (set.size() < kMaxNumericKeys) set.insert(key);
                    }
                }
                if (!value(path.empty() ? kp : path + "." + kp, path, depth + 1)) return false;
                skipWs();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                return bail("expected ',' or '}'");
            }
        }

        if (c == '[') {
            c_.record(path, parent, 'a', {}, false, false);
            ++i_;
            skipWs();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            const std::string ep = path + "[]";
            for (;;) {
                if (!value(ep, path, depth + 1)) return false;
                skipWs();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                return bail("expected ',' or ']'");
            }
        }

        if (c == '"') {
            std::string v;
            if (!str(v)) return false;
            const bool num = looksNumeric(v);
            c_.record(path, parent, 's', "\"" + trim(v) + "\"", num, v.empty());
            return true;
        }

        if (s_.compare(i_, 4, "true") == 0)  { i_ += 4; c_.record(path, parent, 'b', "true", false, false);  return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; c_.record(path, parent, 'b', "false", false, false); return true; }
        if (s_.compare(i_, 4, "null") == 0)  { i_ += 4; c_.record(path, parent, 'z', "null", false, true);   return true; }

        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            const size_t start = i_;
            if (s_[i_] == '-' || s_[i_] == '+') ++i_;
            bool digit = false;
            while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
                                      s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) {
                if (s_[i_] >= '0' && s_[i_] <= '9') digit = true;
                ++i_;
            }
            // Permissive lexing must not become permissive LABELLING: a bare "-" would
            // otherwise be recorded as type `number`, and the report's job is to surface
            // non-standard shapes, not to launder them into valid ones.
            if (!digit) return bail("'-' with no digits is not a number");
            c_.record(path, parent, 'n', s_.substr(start, i_ - start), false, false);
            return true;
        }
        return bail("unexpected character");
    }
};

// Path column width. Real paths reach ~45 chars on a series_info ffprobe block
// (episodes.#[].info.video.disposition.hearing_impaired); anything longer wraps.
constexpr size_t kPathCol = 38;

// The rendered census table. `label` names the container whose children we list.
void printCensus(const Census& c) {
    if (c.paths.empty()) { sayln("    (no values)"); return; }

    std::vector<const std::pair<const std::string, PathInfo>*> rows;
    for (const auto& kv : c.paths) rows.push_back(&kv);
    std::sort(rows.begin(), rows.end(), [](auto* a, auto* b) { return a->second.order < b->second.order; });

    sayln("    " + pad("path", kPathCol) + pad("present", 16) + pad("types", 22) + "samples");
    sayln("    " + std::string(kPathCol + 16 + 22 + 20, '-'));
    for (const auto* r : rows) {
        const std::string& path = r->first;
        const PathInfo& p = r->second;

        // "12,431/12,431" is the useful form — it is how an OPTIONAL field announces
        // itself. Only worth printing when the parent container occurred more than once;
        // "1/1" under a root object is noise.
        std::string present = commas(p.count);
        const auto par = c.paths.find(p.parent);
        if (par != c.paths.end() && par->second.count > 1)
            present = commas(p.count) + "/" + commas(par->second.count);

        std::string types;
        auto add = [&](long long n, const char* t) {
            if (!n) return;
            if (!types.empty()) types += "+";
            types += t;
        };
        add(p.nObject, "object"); add(p.nArray, "array"); add(p.nString, "string");
        add(p.nNumber, "number"); add(p.nBool, "bool");   add(p.nNull, "null");

        std::string samples;
        for (const auto& s : p.samples) {
            if (!samples.empty()) samples += " · ";
            samples += s;
        }

        std::string flags;
        if (p.nNumericString == p.nString && p.nString > 0)
            flags = "  <-- NUMERIC STRING";
        else if (p.nNumericString > 0)
            flags = "  <-- NUMERIC STRING (" + commas(p.nNumericString) + " of " + commas(p.nString) + ")";
        if (p.nEmpty > 0 && p.count > 0 && p.nEmpty * 4 >= p.count)
            flags += "  [empty x" + commas(p.nEmpty) + "]";

        // A path longer than its column used to run straight into the next one
        // ("…codec_long_name3/3"), which is unreadable exactly where the nesting is deepest
        // and the reader needs it most. Overflow wraps onto its own line instead.
        const std::string label = path.empty() ? "(root)" : path;
        const std::string cols = pad(present, 16) + pad(types, 22) + samples + flags;
        if (label.size() > kPathCol)
            sayln("    " + label + "\n    " + std::string(kPathCol, ' ') + cols);
        else
            sayln("    " + pad(label, kPathCol) + cols);

        const auto nk = c.numericKeys.find(path);
        if (nk != c.numericKeys.end() && !nk->second.empty()) {
            std::string keys;
            int shown = 0;
            for (const auto& k : nk->second) {
                if (shown++ >= 12) { keys += ", …"; break; }
                if (!keys.empty()) keys += ", ";
                keys += k;
            }
            const bool capped = nk->second.size() >= kMaxNumericKeys;
            sayln("      \xE2\x86\xB3 MAP KEYED BY NUMBER, not an array — " +
                  (capped ? commas(static_cast<long long>(kMaxNumericKeys)) + "+ (capped)"
                          : commas(static_cast<long long>(nk->second.size()))) +
                  " distinct numeric keys: " + keys);
        }
    }
    if (c.truncated)
        sayln("    *** DISTINCT-PATH CAP HIT — the response is unusually irregular; some paths are");
    if (c.truncated)
        sayln("        MISSING from this table, so every count below it understates the truth.");
    if (!c.trailing.empty()) sayln("    *** " + c.trailing);
}

// ---------------------------------------------------------------------------
// One endpoint probe: fetch, redact, scan, report. Returns the redacted body.
// ---------------------------------------------------------------------------

struct ProbeResult {
    bool ok = false;
    std::string body;       // redacted
    Census census;
    std::string parseErr;   // non-empty when the body was not parseable JSON
    size_t bytes = 0;
    long long ms = 0;
    std::wstring httpErr;
};

ProbeResult probe(int step, int steps, const std::string& label, const std::wstring& url,
                  const Redactor& red, int timeoutMs) {
    ProbeResult r;
    sayln();
    sayln("[" + std::to_string(step) + "/" + std::to_string(steps) + "] " + label);
    sayln("    GET " + red.url(url));

    const ULONGLONG t0 = GetTickCount64();
    std::string bytes;
    std::wstring err;
    const bool ok = httpGet(url, bytes, err, timeoutMs);
    r.ms = static_cast<long long>(GetTickCount64() - t0);
    r.bytes = bytes.size();

    if (!ok) {
        // httpGet folds the HTTP status into the message for >=400, which is exactly the
        // distinction that matters here: 404 = endpoint absent, 403 = blocked, 401 = creds.
        r.httpErr = err;
        sayln("    FAILED: " + red.text(err) + "   (" + std::to_string(r.ms) + " ms)");
        return r;
    }
    r.body = red.body(std::move(bytes));
    sayln("    OK  " + sizeStr(r.bytes) + " in " + std::to_string(r.ms) + " ms");

    if (r.body.empty()) { r.parseErr = "empty body"; sayln("    EMPTY BODY"); return r; }

    // A panel with the API turned off typically answers with an HTML error page or a
    // login redirect, not JSON. Catch that before the scanner so the report says the
    // useful thing instead of "unexpected character at byte 0".
    size_t f = 0;
    if (r.body.size() >= 3 && static_cast<unsigned char>(r.body[0]) == 0xEF &&
        static_cast<unsigned char>(r.body[1]) == 0xBB && static_cast<unsigned char>(r.body[2]) == 0xBF)
        f = 3;  // a BOM'd panel is misconfigured, not broken — do not call it "not JSON"
    const size_t nb = r.body.find_first_not_of(" \t\r\n", f);
    f = (nb == std::string::npos) ? f : nb;
    if (f >= r.body.size() || (r.body[f] != '{' && r.body[f] != '[')) {
        r.parseErr = "not JSON";
        std::string head = r.body.substr(f, 200);
        for (char& ch : head)
            if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        sayln("    NOT JSON — body starts: " + head);
        return r;
    }

    Scanner sc(r.body, r.census);
    if (!sc.run(r.parseErr)) {
        sayln("    PARSE FAILED: " + r.parseErr);
        sayln("    (this IS a finding — the raw body is saved; the production parser must handle it)");
        return r;
    }
    r.ok = true;
    printCensus(r.census);
    return r;
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// First sample recorded at `path`, unquoted.
std::string firstSample(const Census& c, const std::string& path) {
    const auto it = c.paths.find(path);
    if (it == c.paths.end() || it->second.samples.empty()) return {};
    return unquote(it->second.samples.front());
}

// A named member OF THE FIRST ITEM. Use this — not firstSample — whenever two values must
// belong to the same object (a stream_id and the container_extension the play URL pairs it
// with); firstSample would happily take them from different items.
std::string firstItemField(const Census& c, const std::string& key) {
    const auto it = c.firstItem.find(key);
    return it == c.firstItem.end() ? std::string() : unquote(it->second);
}

long long pathCount(const Census& c, const std::string& path) {
    const auto it = c.paths.find(path);
    return it == c.paths.end() ? 0 : it->second.count;
}

// ---------------------------------------------------------------------------
// URL handling
// ---------------------------------------------------------------------------

std::wstring queryValue(const std::wstring& url, const std::wstring& key) {
    const size_t q = url.find(L'?');
    if (q == std::wstring::npos) return {};
    std::wstring rest = url.substr(q + 1);
    for (size_t i = 0; i < rest.size();) {
        const size_t amp = rest.find(L'&', i);
        const std::wstring pair = rest.substr(i, amp == std::wstring::npos ? amp : amp - i);
        const size_t eq = pair.find(L'=');
        if (eq != std::wstring::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
        if (amp == std::wstring::npos) break;
        i = amp + 1;
    }
    return {};
}

// scheme://host[:port] — everything before the path. The Xtream API and the movie/series
// stream URLs all hang off this.
std::wstring originOf(const std::wstring& url) {
    const size_t s = url.find(L"//");
    if (s == std::wstring::npos) return {};
    const size_t e = url.find(L'/', s + 2);
    return e == std::wstring::npos ? url : url.substr(0, e);
}

// The HOST alone — no userinfo, no port. Must agree with urlW's authority parsing or the
// wrong token gets registered for redaction: on `http://joe:pw@panel.tld/` the naive version
// returned "joe", so the REAL hostname was never registered and survived in every body.
std::wstring hostOf(const std::wstring& url) {
    const std::wstring o = originOf(url);
    const size_t s = o.find(L"//");
    if (s == std::wstring::npos) return {};
    std::wstring h = o.substr(s + 2);
    const size_t at = h.rfind(L'@');
    if (at != std::wstring::npos) h = h.substr(at + 1);
    if (!h.empty() && h[0] == L'[') {  // IPv6 literal: keep the brackets out of the token
        const size_t close = h.find(L']');
        return close == std::wstring::npos ? h.substr(1) : h.substr(1, close - 1);
    }
    const size_t colon = h.find(L':');
    return colon == std::wstring::npos ? h : h.substr(0, colon);
}

std::wstring dirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

void ensureDir(const std::wstring& dir) {
    if (dir.empty()) return;
    ensureDir(dirOf(dir));
    CreateDirectoryW(dir.c_str(), nullptr);  // ERROR_ALREADY_EXISTS is fine
}

bool writeFile(const std::wstring& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

std::string utcStamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[64];
    sprintf_s(buf, "%04u-%02u-%02u %02u:%02u UTC", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// Pacing between requests. The dead-link checker's lesson applies: a provider's
// connection cap can kick the owner's ACTUAL playback, and a burst reads as a scraper.
// Nine paced requests is nothing; nine simultaneous ones might not be.
void pace() { Sleep(400); }

}  // namespace

// ---------------------------------------------------------------------------

int xtreamRecon(const std::wstring& source, bool raw) {
    g_report.clear();

    // --- resolve the playlist -------------------------------------------------
    std::wstring url = source, plName;
    if (url.empty()) {
        std::wstring err;
        Database db;
        if (!db.open(Database::defaultDbPath(), &err)) {
            sayln("DB open failed: " + utf8FromWide(err));
            sayln("Pass the playlist URL explicitly: RabbitEarsCli --xtream \"http://host:port/get.php?username=…&password=…\"");
            return 1;
        }
        std::vector<Playlist> cands;
        for (const auto& pl : db.listPlaylists())
            if (!pl.sourceUrl.empty() && !queryValue(pl.sourceUrl, L"username").empty() &&
                !queryValue(pl.sourceUrl, L"password").empty())
                cands.push_back(pl);
        if (cands.empty()) {
            sayln("No Xtream-looking playlist found in " + utf8FromWide(Database::defaultDbPath()) + ".");
            sayln("(Looking for a source URL carrying both username= and password=.)");
            sayln("Pass one explicitly: RabbitEarsCli --xtream \"http://host:port/get.php?username=…&password=…\"");
            return 1;
        }
        url = cands.front().sourceUrl;
        plName = cands.front().name;
        if (cands.size() > 1)
            sayln("(" + commas(static_cast<long long>(cands.size())) +
                  " Xtream playlists in the DB; probing the first. Pass a URL to choose another.)");
    }

    const std::wstring user = queryValue(url, L"username");
    const std::wstring pass = queryValue(url, L"password");
    const std::wstring origin = originOf(url);
    if (user.empty() || pass.empty() || origin.empty()) {
        sayln("That URL does not look like an Xtream playlist — it needs a scheme, and username=");
        sayln("and password= in the query, e.g.");
        sayln("  http://host:port/get.php?username=…&password=…&type=m3u_plus");
        // Deliberately NOT echoing the URL: redaction is not configured yet on this path
        // (the credentials are what we failed to find), so echoing it could print a
        // password in cleartext into the console transcript the owner pastes back.
        return 1;
    }

    Redactor red;
    red.enabled = !raw;
    red.wuser = user;
    red.wpass = pass;
    // Register BOTH the encoded and decoded spelling of each credential: the playlist URL
    // carries them percent-encoded, while the panel's own JSON (a `direct_source` field,
    // say) embeds the DECODED form — so registering only one ships the other.
    for (const std::wstring* w : {&pass, &user}) {
        const std::string enc = utf8FromWide(*w);
        red.add(enc, w == &pass ? "<PASS>" : "<USER>");
        red.add(percentDecode(enc), w == &pass ? "<PASS>" : "<USER>");
    }
    red.add(utf8FromWide(hostOf(url)), "<HOST>");
    {   // the OS account name is baked into every path this report prints
        const wchar_t* un = _wgetenv(L"USERNAME");
        // Guard the length: a 2-character account name blind-replaced across the report
        // would corrupt unrelated text, and a path is not worth that.
        if (un && wcslen(un) >= 4) red.winUser = utf8FromWide(un);
    }
    red.finish();

    const std::wstring creds = L"username=" + user + L"&password=" + pass;
    const std::wstring api = origin + L"/player_api.php?" + creds;

    // --- header ---------------------------------------------------------------
    sayln("RabbitEars — Xtream reconnaissance");
    sayln("==================================");
    sayln("Run:        " + utcStamp());
    sayln("Purpose:    gate 1 of the Xtream VOD + Series epic — does player_api.php answer,");
    sayln("            and what shape does it return? (Win32/BACKLOG.md)");
    if (!plName.empty()) sayln("Playlist:   \"" + red.text(plName) + "\"  (from the app's DB)");
    sayln("Origin:     " + red.url(origin));
    sayln("API base:   " + red.url(api));
    if (red.enabled) {
        sayln("Redaction:  ON — host/username/password replaced with <HOST>/<USER>/<PASS>.");
        sayln("            URLs are rewritten by POSITION (query key, path segment), not by");
        sayln("            matching the secret's text, so a 2-character password is as safe as");
        sayln("            a 20-character one. Bodies get that plus an exact \"password\"/");
        sayln("            \"username\" member pass. Applies to every saved file too.");
    } else {
        sayln("Redaction:  *** OFF (--raw) — this report CONTAINS YOUR CREDENTIALS. Do not share,");
        sayln("            and delete the saved folder afterwards. ***");
    }
    sayln("Requests:   up to 9, sequential, ~400 ms apart. Read-only — nothing is written to");
    sayln("            the provider. NOTE the last one opens a movie URL to check reachability");
    sayln("            and closes it after the headers; on a max_connections=1 account that");
    sayln("            momentarily uses your one slot, so avoid running this mid-programme.");

    // --- the probes -----------------------------------------------------------
    const int kSteps = 9;
    int step = 0;

    const ProbeResult auth = probe(++step, kSteps, "auth — player_api.php with no action", api, red, 20000);
    pace();

    // The single most important yes/no in the report.
    const bool apiLives = auth.ok && (pathCount(auth.census, "user_info") > 0 ||
                                      pathCount(auth.census, "server_info") > 0);

    const ProbeResult liveCats = probe(++step, kSteps, "get_live_categories (small — proves the action verb works)",
                                       api + L"&action=get_live_categories", red, 20000);
    pace();
    const ProbeResult vodCats = probe(++step, kSteps, "get_vod_categories",
                                      api + L"&action=get_vod_categories", red, 20000);
    pace();
    // The big one. Timed and sized deliberately: whether a full sync is viable, or the
    // client must page per category, is decided by these two numbers.
    const ProbeResult vod = probe(++step, kSteps, "get_vod_streams (the whole movie library — expect this to be large)",
                                  api + L"&action=get_vod_streams", red, 120000);
    pace();
    const ProbeResult serCats = probe(++step, kSteps, "get_series_categories",
                                      api + L"&action=get_series_categories", red, 20000);
    pace();
    const ProbeResult series = probe(++step, kSteps, "get_series (the whole series library)",
                                     api + L"&action=get_series", red, 120000);
    pace();

    // --- chained probes: they need an id harvested from the lists above --------
    // Both of these come from the SAME item (firstItemField), because the play URL pairs
    // them: taking the id from item #1 and the extension from item #2 builds a URL that
    // 404s, and the report would print a confident "VOD is not reachable".
    std::string vodId = firstItemField(vod.census, "stream_id");
    if (vodId.empty()) vodId = firstItemField(vod.census, "vod_id");
    const std::string vodExt = firstItemField(vod.census, "container_extension");
    const std::string seriesId = firstItemField(series.census, "series_id");

    ProbeResult vodInfo, seriesInfo;
    if (!vodId.empty()) {
        vodInfo = probe(++step, kSteps, "get_vod_info&vod_id=" + vodId + " (one movie's detail record)",
                        api + L"&action=get_vod_info&vod_id=" + wideFromUtf8(vodId), red, 20000);
        pace();
    } else {
        sayln();
        sayln("[" + std::to_string(++step) + "/" + std::to_string(kSteps) +
              "] get_vod_info — SKIPPED: no stream_id/vod_id found in get_vod_streams.");
    }
    if (!seriesId.empty()) {
        seriesInfo = probe(++step, kSteps,
                           "get_series_info&series_id=" + seriesId + " (one series: seasons + episodes)",
                           api + L"&action=get_series_info&series_id=" + wideFromUtf8(seriesId), red, 20000);
        pace();
    } else {
        sayln();
        sayln("[" + std::to_string(++step) + "/" + std::to_string(kSteps) +
              "] get_series_info — SKIPPED: no series_id found in get_series.");
    }

    // --- can we actually build a playable URL? --------------------------------
    // Xtream VOD plays from {origin}/movie/{user}/{pass}/{id}.{ext}. If container_extension
    // is missing this is guesswork, and that changes the design — so probe it for real.
    ++step;
    sayln();
    pace();
    int movieStatus = 0;
    ProbeTransport movieTransport = ProbeTransport::NoConnect;
    bool movieProbed = false;
    std::wstring movieUrl;
    sayln("[" + std::to_string(step) + "/" + std::to_string(kSteps) +
          "] play-URL reachability (headers only, body never read)");
    if (!vodId.empty() && !vodExt.empty()) {
        movieUrl = origin + L"/movie/" + user + L"/" + pass + L"/" + wideFromUtf8(vodId) + L"." +
                   wideFromUtf8(vodExt);
        sayln("    " + red.url(movieUrl));
        movieTransport = httpProbe(movieUrl, movieStatus, 15000);
        movieProbed = true;
        const char* t = movieTransport == ProbeTransport::Ok      ? "reached"
                        : movieTransport == ProbeTransport::Timeout ? "TIMEOUT"
                                                                    : "NO CONNECT";
        sayln(std::string("    ") + t +
              (movieTransport == ProbeTransport::Ok ? "  HTTP " + std::to_string(movieStatus) : ""));
    } else if (vodId.empty()) {
        sayln("    SKIPPED: the first movie item carried no stream_id/vod_id to build a URL from.");
    } else {
        // Deliberately NOT guessing ".mp4" here. A guessed suffix produces a 404 that reads
        // as "VOD playback is broken", when the actual finding is "this panel does not tell
        // us the container" — a completely different design consequence.
        sayln("    SKIPPED: the first movie item carried no container_extension, and guessing a");
        sayln("    suffix would turn an unknown into a false 'not reachable'. See verdict 3.");
    }

    // --- verdict --------------------------------------------------------------
    // Deliberately answers the BACKLOG's questions in the BACKLOG's words, so the
    // design doc can be written straight off this block.
    sayln();
    sayln();
    sayln("VERDICT");
    sayln("=======");

    sayln("1. Does player_api.php respond?          " +
          std::string(apiLives ? "YES" : "NO  <-- this changes the estimate completely"));
    if (!apiLives && !auth.httpErr.empty()) sayln("   auth failure: " + red.text(auth.httpErr));
    // A panel that ANSWERED but sent something unreadable is a different diagnosis from one
    // that never answered; a bare "NO" would send the reader looking for a network fault.
    if (!apiLives && auth.httpErr.empty() && !auth.parseErr.empty())
        sayln("   the panel DID answer, but not with usable JSON: " + auth.parseErr);
    if (!apiLives && auth.ok) sayln("   answered, but with no user_info/server_info block.");
    // An Xtream panel answers BAD CREDENTIALS with HTTP 200 and {"user_info":{"auth":0}} —
    // so "it responded" and "it let us in" are different questions, and conflating them
    // produces a report that says the API is healthy while every list comes back empty.
    const std::string authFlag = firstSample(auth.census, "user_info.auth");
    // Panels spell the rejection as 0, "0", false or null depending on the build.
    const bool credsRejected =
        apiLives && (authFlag == "0" || authFlag == "false" || authFlag == "null");
    if (apiLives) {
        const std::string st = firstSample(auth.census, "user_info.status");
        const std::string exp = firstSample(auth.census, "user_info.exp_date");
        const std::string cons = firstSample(auth.census, "user_info.max_connections");
        sayln("   credentials accepted:" +
              std::string(credsRejected
                              ? " NO  <-- user_info.auth = 0. EVERYTHING BELOW IS MEANINGLESS;"
                                "\n                        the empty lists are the rejection, not the library."
                              : authFlag.empty() ? " (no user_info.auth field to check)" : " yes"));
        if (!st.empty())   sayln("   account status:      " + st);
        if (!exp.empty())  sayln("   expires (epoch):     " + exp);
        if (!cons.empty()) sayln("   max_connections:     " + cons +
                                 "   <-- the sync must stay under this or it kicks playback");
    }

    auto verbSummary = [&](const char* name, const ProbeResult& p) {
        std::string s = "   " + pad(name, 24);
        if (p.ok) {
            // "0 items" and "not a list" are completely different answers — an empty VOD
            // library vs. a response shaped unlike the one we expected — and collapsing
            // them into one string made a disabled/empty library read as a shape surprise.
            if (p.census.rootIsArray()) {
                const long long n = pathCount(p.census, "[]");
                s += (n > 0 ? commas(n) + " items" : std::string("EMPTY list ([])"));
            } else {
                s += "an object, not a list";
            }
            s += "  · " + sizeStr(p.bytes) + " · " + std::to_string(p.ms) + " ms";
            if (p.census.truncated) s += "  [PATH CAP HIT — counts understate]";
            if (!p.census.trailing.empty()) s += "  [TRAILING JUNK]";
        } else if (!p.httpErr.empty()) {
            s += "FAILED — " + red.text(p.httpErr);
        } else {
            s += "FAILED — " + (p.parseErr.empty() ? std::string("unknown") : p.parseErr);
        }
        sayln(s);
    };
    sayln("2. What the action verbs returned:");
    verbSummary("get_live_categories", liveCats);
    verbSummary("get_vod_categories", vodCats);
    verbSummary("get_vod_streams", vod);
    verbSummary("get_series_categories", serCats);
    verbSummary("get_series", series);
    if (!vodId.empty()) verbSummary("get_vod_info", vodInfo);
    if (!seriesId.empty()) verbSummary("get_series_info", seriesInfo);
    sayln("   (get_live_streams deliberately NOT fetched — the M3U import already covers live,");
    sayln("    and it would be the largest response of the run for no new information.)");

    sayln("3. Is a playable VOD URL constructible?");
    // Three-way, not boolean. "Absent" and "we never got a list to look at" are different
    // findings, and the previous version asserted the first when it meant the second —
    // straight into the section of the design doc about URL construction.
    {
        const long long items = pathCount(vod.census, "[]");
        const auto extPath = vod.census.paths.find("[].container_extension");
        const long long withExt = extPath == vod.census.paths.end() ? 0 : extPath->second.count;
        // Present-but-EMPTY (or null) is not present for our purposes: the play URL cannot be
        // built from "". Counting those as present made the headline answer to the epic's
        // third question read YES while step 9 was simultaneously skipping the probe.
        const long long blankExt = extPath == vod.census.paths.end() ? 0 : extPath->second.nEmpty;
        const long long usableExt = withExt - blankExt;
        if (!vod.ok || !vod.census.rootIsArray()) {
            sayln("   container_extension:  UNKNOWN — get_vod_streams did not return a list, so no");
            sayln("                         item was ever examined. This is NOT evidence of absence.");
        } else if (items == 0) {
            sayln("   container_extension:  UNKNOWN — the movie library came back empty ([]).");
        } else if (usableExt <= 0) {
            sayln("   container_extension:  " +
                  std::string(blankExt > 0 ? "present but EMPTY/null on every item"
                                           : "ABSENT from all " + commas(items) + " items") +
                  "  <-- the suffix would have to be guessed");
        } else if (usableExt < items) {
            sayln("   container_extension:  usable on " + commas(usableExt) + " of " + commas(items) +
                  " items  <-- OPTIONAL; the client needs a fallback");
            if (blankExt > 0)
                sayln("                         (" + commas(blankExt) + " are present but empty/null)");
            if (vodExt.empty())
                sayln("                         (the FIRST item is one of the ones without it,");
            if (vodExt.empty())
                sayln("                          which is why the play-URL probe was skipped)");
        } else {
            sayln("   container_extension:  present and non-empty on all " + commas(items) +
                  " items, e.g. \"" +
                  (vodExt.empty() ? firstSample(vod.census, "[].container_extension") : vodExt) + "\"");
        }
    }
    if (movieProbed) {
        if (movieTransport == ProbeTransport::Ok)
            sayln("   play URL probe:       HTTP " + std::to_string(movieStatus) +
                  (movieStatus < 400 ? "  (reachable)" : "  <-- NOT reachable"));
        else
            sayln("   play URL probe:       transport failure (no connect / timeout)");
    } else {
        sayln("   play URL probe:       not run — see step " + std::to_string(kSteps) + " above.");
    }

    // The flagged provider risk, answered with counts rather than a guess.
    sayln("4. Numbers-as-strings (the flagged non-standard risk):");
    {
        int found = 0, scanned = 0;
        auto scanNumeric = [&](const char* what, const Census& c) {
            if (c.hasRoot()) ++scanned;  // did we actually have a body to look at?
            for (const auto& kv : c.paths) {
                if (kv.second.nNumericString == 0) continue;
                ++found;
                sayln("   " + pad(std::string(what) + " " + kv.first, 40) +
                      commas(kv.second.nNumericString) + " of " + commas(kv.second.nString) +
                      " quoted numbers  e.g. " + kv.second.numericSample);
            }
        };
        // The *_categories responses are scanned too: category_id is quoted on essentially
        // every panel, and leaving them out under-reported the risk on a healthy run.
        scanNumeric("live_categories", liveCats.census);
        scanNumeric("vod_categories", vodCats.census);
        scanNumeric("vod_streams", vod.census);
        scanNumeric("series_categories", serCats.census);
        scanNumeric("series", series.census);
        scanNumeric("vod_info", vodInfo.census);
        scanNumeric("series_info", seriesInfo.census);
        if (found == 0 && scanned == 0)
            sayln("   UNKNOWN — no response was parsed, so nothing was examined. NOT a clean bill.");
        else if (found == 0)
            sayln("   none across the " + std::to_string(scanned) +
                  " response(s) examined — every numeric field was a real JSON number.");
    }

    sayln("5. Parse failures (a failure here is a REQUIREMENT for the production parser):");
    {
        int found = 0, bodies = 0;
        auto pf = [&](const char* what, const ProbeResult& p) {
            if (p.census.hasRoot()) ++bodies;
            if (!p.parseErr.empty()) { ++found; sayln("   " + pad(what, 24) + p.parseErr); }
            if (!p.census.trailing.empty())
                { ++found; sayln("   " + pad(what, 24) + p.census.trailing); }
            if (p.census.truncated)
                { ++found; sayln("   " + pad(what, 24) + "distinct-path cap hit — shape is irregular"); }
        };
        pf("auth", auth); pf("get_live_categories", liveCats); pf("get_vod_categories", vodCats);
        pf("get_vod_streams", vod); pf("get_series_categories", serCats); pf("get_series", series);
        pf("get_vod_info", vodInfo); pf("get_series_info", seriesInfo);
        // Same guard section 4 already has. A run where every request failed at the transport
        // leaves parseErr empty everywhere, which is NOT a clean bill of health — nothing was
        // parsed at all.
        if (!found && bodies == 0)
            sayln("   UNKNOWN — no body was received, so nothing was parsed. NOT a clean bill.");
        else if (!found)
            sayln("   none — all " + std::to_string(bodies) +
                  " body/bodies received parsed cleanly, end to end.");
    }

    // --- save -----------------------------------------------------------------
    const std::wstring outDir = dirOf(Database::defaultDbPath()) + L"\\xtream-recon";
    ensureDir(outDir);
    // Wipe first. Without this, a raw-*.json left by an earlier --raw run survives whenever
    // this run's matching probe fails (nothing overwrites it), and the closing line below
    // then certifies the whole folder as redacted — which would be false.
    struct Save { const char* name; const ProbeResult* p; };
    const Save saves[] = {
        {"auth", &auth}, {"live_categories", &liveCats}, {"vod_categories", &vodCats},
        {"vod_streams", &vod}, {"series_categories", &serCats}, {"series", &series},
        {"vod_info", &vodInfo}, {"series_info", &seriesInfo},
    };
    // 512 KB per body is plenty to see the shape and keeps the folder hand-backable;
    // the census above already counted the FULL body, so nothing is lost by truncating.
    constexpr size_t kMaxSave = 512 * 1024;
    for (const auto& s : saves)  // clear BEFORE writing, so a failed probe leaves no stale file
        DeleteFileW((outDir + L"\\raw-" + wideFromUtf8(s.name) + L".json").c_str());
    std::vector<std::string> written;
    for (const auto& s : saves) {
        if (s.p->body.empty()) continue;
        std::string body = s.p->body;
        if (body.size() > kMaxSave) {
            // Back off to a UTF-8 lead byte: a mid-sequence cut renders as mojibake and
            // would be mistaken for a provider encoding fault.
            size_t cut = kMaxSave;
            while (cut > 0 && (static_cast<unsigned char>(body[cut]) & 0xC0) == 0x80) --cut;
            body = body.substr(0, cut) + "\n\n/* TRUNCATED for the report at " +
                   commas(static_cast<long long>(cut)) + " of " +
                   commas(static_cast<long long>(s.p->body.size())) +
                   " redacted bytes — the census above counted the WHOLE body, and this file\n" +
                   "   is deliberately not valid JSON past this point. */\n";
        }
        if (writeFile(outDir + L"\\raw-" + wideFromUtf8(s.name) + L".json", body))
            written.push_back(std::string("raw-") + s.name + ".json");
    }

    sayln();
    sayln("Saved to: " + red.text(outDir));
    for (const auto& w : written) sayln("   " + w);
    sayln("   xtream-recon.txt   <-- this report; send this one back");
    if (red.enabled)
        sayln("Every saved byte went through the same redaction as this report, and any file from");
    if (red.enabled)
        sayln("an earlier run was deleted first, so nothing unredacted can survive in this folder.");

    const std::wstring reportPath = outDir + L"\\xtream-recon.txt";
    // g_report is still growing as we say() — snapshot it, then write.
    const std::string finalReport = g_report;
    if (!writeFile(reportPath, finalReport)) {
        say("\nWARNING: could not write the report file to " + red.text(reportPath) + "\n");
        return 1;
    }
    // 0 = usable · 2 = the API answered but is not usable (dead, or credentials rejected —
    // which is NOT a success, however cleanly the panel replied).
    return (apiLives && !credsRejected) ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Selftest. Fixtures are modelled on real Xtream-Codes responses, including the two
// shapes that are the whole reason recon exists: numeric fields delivered as QUOTED
// strings, and `get_series_info`'s `episodes` — a map keyed by season NUMBER rather
// than an array.
// ---------------------------------------------------------------------------

int xtreamReconSelftest(void (*expect)(bool, const std::string&)) {
    int before = 0;
    (void)before;

    auto scan = [](const std::string& json, Census& c, std::string& err) {
        Scanner s(json, c);
        return s.run(err);
    };
    auto count = [](const Census& c, const char* path) { return pathCount(c, path); };
    auto info = [](const Census& c, const char* path) -> PathInfo {
        const auto it = c.paths.find(path);
        return it == c.paths.end() ? PathInfo{} : it->second;
    };

    // --- 1. get_vod_streams: optional fields + numbers-as-strings --------------
    {
        // Item 2 deliberately omits container_extension (providers really do this, and
        // it is the field the play URL is built from); rating is quoted everywhere.
        const std::string json =
            "[{\"num\":1,\"name\":\"Inception\",\"stream_id\":1234,\"rating\":\"8.8\","
            "\"container_extension\":\"mp4\",\"added\":\"1690000000\"},"
            "{\"num\":2,\"name\":\"Ar\\u00e8s\",\"stream_id\":1235,\"rating\":\"6.1\","
            "\"added\":\"1690000001\"},"
            "{\"num\":3,\"name\":\"Dune\",\"stream_id\":1236,\"rating\":\"7.9\","
            "\"container_extension\":\"mkv\",\"added\":\"1690000002\"}]";
        Census c;
        std::string err;
        expect(scan(json, c, err), "xtream recon: vod_streams fixture parses (" + err + ")");
        expect(count(c, "[]") == 3, "xtream recon: 3 array elements counted");
        expect(count(c, "[].name") == 3, "xtream recon: every item has name");
        // THE optional-field signal: 2-of-3 is what tells the design doc the play URL
        // suffix cannot be assumed.
        expect(count(c, "[].container_extension") == 2,
               "xtream recon: container_extension seen on 2 of 3 items (optional detected)");
        expect(info(c, "[].rating").nNumericString == 3,
               "xtream recon: quoted rating flagged as NUMERIC STRING on all 3");
        expect(info(c, "[].added").nNumericString == 3,
               "xtream recon: quoted epoch flagged as NUMERIC STRING");
        expect(info(c, "[].stream_id").nNumber == 3 && info(c, "[].stream_id").nNumericString == 0,
               "xtream recon: a REAL json number is not flagged as a numeric string");
        expect(info(c, "[].name").parent == "[]", "xtream recon: parent path recorded for the ratio");
        // *** The id/extension pair MUST come from the same item, or the play URL pairs
        // item #1's id with item #3's "mkv" and 404s — which the report would print as a
        // confident "VOD NOT reachable". ***
        expect(firstItemField(c, "stream_id") == "1234",
               "xtream recon: firstItemField reads item #1's stream_id");
        expect(firstItemField(c, "container_extension") == "mp4",
               "xtream recon: ...and item #1's OWN container_extension, not item #3's mkv");
        expect(firstItemField(c, "nonesuch").empty(),
               "xtream recon: a field item #1 lacks reads as absent, not as another item's value");
        expect(info(c, "[].rating").numericSample == "\"8.8\"",
               "xtream recon: the numbers-as-strings example is a QUOTED NUMBER, not just any sample");
        // è must survive as UTF-8 (0xC3 0xA8), or every accented title in the report is
        // mojibake and the owner cannot tell a real encoding bug from ours.
        bool utf8ok = false;
        for (const auto& s : info(c, "[].name").samples)
            if (s.find("\xC3\xA8") != std::string::npos) utf8ok = true;
        expect(utf8ok, "xtream recon: \\u00e8 decoded to UTF-8 in the sample");
    }

    // --- 1b. the item that decides the play URL is the one MISSING the extension ---
    {
        // Item #1 has no container_extension, item #2 does. The old code took the id from
        // #1 and the extension from #2 and probed a URL that cannot exist.
        const std::string json =
            "[{\"name\":\"A\",\"stream_id\":7},"
            "{\"name\":\"B\",\"stream_id\":8,\"container_extension\":\"mkv\"}]";
        Census c;
        std::string err;
        expect(scan(json, c, err), "xtream recon: mixed-extension fixture parses");
        expect(firstItemField(c, "stream_id") == "7", "xtream recon: item #1's id read");
        expect(firstItemField(c, "container_extension").empty(),
               "xtream recon: item #1's MISSING extension reads empty (=> probe is skipped, not guessed)");
        expect(count(c, "[].container_extension") == 1 && count(c, "[]") == 2,
               "xtream recon: the census still reports 1-of-2 so the verdict can say OPTIONAL");
    }

    // --- 1c. shapes that used to flip a headline verdict -----------------------
    {
        Census c;
        std::string err;
        // A PHP file saved with a BOM is a classic panel misconfiguration; it used to
        // report as "NOT JSON", flipping "does player_api.php respond" to NO.
        expect(scan("\xEF\xBB\xBF{\"user_info\":{\"auth\":1}}", c, err),
               "xtream recon: a UTF-8 BOM does not make a good body 'not JSON'");
        expect(count(c, "user_info.auth") == 1, "xtream recon: ...and the body behind it is scanned");

        // A PHP notice APPENDED after valid JSON parses cleanly up to that point, so
        // without the trailing check the census reads as authoritative.
        Census c2;
        std::string err2;
        expect(scan("[1,2]<br />Fatal error", c2, err2), "xtream recon: trailing junk still parses");
        expect(!c2.trailing.empty(), "xtream recon: ...but is reported as a finding");

        // Root shape: "empty list" and "not a list" are different answers about a provider.
        Census c3, c4;
        std::string e3, e4;
        expect(scan("[]", c3, e3) && c3.rootIsArray() && pathCount(c3, "[]") == 0,
               "xtream recon: an empty array is an EMPTY LIST, not a shape surprise");
        expect(scan("{\"a\":1}", c4, e4) && !c4.rootIsArray(),
               "xtream recon: an object root is reported as not-a-list");

        // Permissive lexing must not launder junk into a valid type.
        Census c5;
        std::string e5;
        expect(!scan("[-]", c5, e5), "xtream recon: a bare '-' is not accepted as a number");

        // An unpaired surrogate has no UTF-8 encoding; emitting one would put invalid
        // bytes into a text file the owner is asked to read and forward.
        Census c6;
        std::string e6;
        expect(scan("[\"\\ud800x\"]", c6, e6), "xtream recon: a lone surrogate does not fail the parse");
        expect(!c6.paths["[]"].samples.empty() &&
                   c6.paths["[]"].samples.front().find("\xEF\xBF\xBD") != std::string::npos,
               "xtream recon: ...it becomes U+FFFD rather than invalid UTF-8 in the report");
    }

    // --- 2. get_series_info: the season-keyed episodes MAP ---------------------
    {
        const std::string json =
            "{\"info\":{\"name\":\"Show\",\"cover\":\"\"},"
            "\"seasons\":[{\"season_number\":1},{\"season_number\":2}],"
            "\"episodes\":{\"1\":[{\"id\":\"11\",\"title\":\"E1\",\"container_extension\":\"mkv\"},"
            "{\"id\":\"12\",\"title\":\"E2\",\"container_extension\":\"mkv\"}],"
            "\"2\":[{\"id\":\"21\",\"title\":\"E1\",\"container_extension\":\"mkv\"}]}}";
        Census c;
        std::string err;
        expect(scan(json, c, err), "xtream recon: series_info fixture parses (" + err + ")");
        expect(count(c, "episodes") == 1, "xtream recon: episodes is an object");
        // The collapse is the point: without it this is N paths, one per season, and the
        // shape is invisible in the report.
        expect(count(c, "episodes.#") == 2, "xtream recon: season keys collapsed to a single '#' path");
        expect(c.numericKeys["episodes"].size() == 2,
               "xtream recon: both numeric season keys recorded for the report");
        expect(count(c, "episodes.#[]") == 3, "xtream recon: 3 episodes across 2 seasons");
        expect(count(c, "episodes.#[].title") == 3, "xtream recon: episode titles counted");
        expect(info(c, "episodes.#[].id").nNumericString == 3,
               "xtream recon: quoted episode ids flagged (the play URL is built from these)");
        expect(count(c, "seasons[]") == 2, "xtream recon: seasons array counted separately");
        expect(info(c, "info.cover").nEmpty == 1, "xtream recon: empty string counted as empty");
    }

    // --- 3. failures are FINDINGS, so they must report usefully ----------------
    {
        Census c;
        std::string err;
        expect(!scan("{\"a\":1,", c, err), "xtream recon: truncated JSON fails");
        expect(err.find("byte") != std::string::npos,
               "xtream recon: parse failure names a byte offset (" + err + ")");
        Census c2;
        std::string err2;
        expect(!scan("<html>Not authorised</html>", c2, err2), "xtream recon: an HTML error page fails");
        Census c3;
        std::string err3;
        expect(scan("[]", c3, err3), "xtream recon: an empty array is valid, not an error");
        expect(count(c3, "[]") == 0, "xtream recon: empty array reports zero elements");
        // A nesting bomb must be refused rather than blowing the stack on a hostile panel.
        std::string deep(200, '[');
        Census c4;
        std::string err4;
        expect(!scan(deep, c4, err4), "xtream recon: 200-deep nesting is refused");
    }

    // --- 4. looksNumeric, the flag the whole risk section rests on -------------
    {
        expect(looksNumeric("7.8") && looksNumeric("-5") && looksNumeric("1690000000"),
               "xtream recon: numeric strings detected");
        expect(!looksNumeric("") && !looksNumeric("abc") && !looksNumeric("1.2.3") &&
                   !looksNumeric("-") && !looksNumeric("8.8 / 10"),
               "xtream recon: non-numeric strings not flagged");
    }

    // --- 5. redaction — the property that makes the report shareable -----------
    // Every case below is a defect an adversarial review actually found in the first
    // version of this file. They are regression tests, not hypotheticals.
    {
        auto make = [](const char* u, const char* p, const char* h) {
            Redactor r;
            r.wuser = wideFromUtf8(u);
            r.wpass = wideFromUtf8(p);
            r.add(p, "<PASS>");
            r.add(percentDecode(p), "<PASS>");
            r.add(u, "<USER>");
            r.add(percentDecode(u), "<USER>");
            r.add(h, "<HOST>");
            r.finish();
            return r;
        };

        const Redactor r = make("bryanmark", "s3cretpassword", "panel.example.com");
        const std::string body =
            "{\"user_info\":{\"username\":\"bryanmark\",\"password\":\"s3cretpassword\"},"
            "\"url\":\"http://panel.example.com:8080/movie/bryanmark/s3cretpassword/1.mp4\"}";
        const std::string out = r.body(body);
        expect(out.find("s3cretpassword") == std::string::npos,
               "xtream recon: password gone from the redacted body");
        expect(out.find("bryanmark") == std::string::npos,
               "xtream recon: username gone from the redacted body");
        expect(out.find("panel.example.com") == std::string::npos,
               "xtream recon: host gone from the redacted body");
        expect(out.find("8080") != std::string::npos,
               "xtream recon: the PORT survives redaction (it is shape, not identity)");

        // *** The critical one. URLs are NOT JSON, so the member pass does nothing to them
        // and a short credential used to print verbatim on every single GET line. ***
        const Redactor s = make("ab", "abc", "panel.example.com");
        const std::string u = s.url(L"http://panel.example.com:8080/get.php?username=ab&password=abc&type=m3u_plus");
        expect(u.find("password=<PASS>") != std::string::npos,
               "xtream recon: a 3-char password is redacted IN A URL (structural, not textual)");
        expect(u.find("username=<USER>") != std::string::npos,
               "xtream recon: a 2-char username is redacted in a URL");
        expect(u.find("=ab&") == std::string::npos && u.find("=abc") == std::string::npos,
               "xtream recon: no short credential survives anywhere in the URL");
        expect(u.find("type=m3u_plus") != std::string::npos,
               "xtream recon: unrelated query params are untouched");
        expect(u.find("<HOST>:8080") != std::string::npos,
               "xtream recon: host redacted in a URL, port kept");
        const std::string mu = s.url(L"http://panel.example.com:8080/movie/ab/abc/1234.mkv");
        expect(mu.find("/<USER>/<PASS>/1234.mkv") != std::string::npos,
               "xtream recon: /movie/USER/PASS/ path segments redacted by whole-segment match");

        // *** The other critical one: a credential that is an ordinary word must not eat
        // the field names the census is measuring. "stream" is 6 chars — it passed the old
        // length gate — and would have rewritten "stream_id" to "<PASS>_id", blanking the
        // verdict's stream_id and container_extension findings with no visible symptom. ***
        const Redactor w = make("movies", "stream", "panel.example.com");
        const std::string fields =
            "[{\"stream_id\":1,\"stream_icon\":\"x\",\"container_extension\":\"mkv\","
            "\"direct_source\":\"http://panel.example.com/movie/movies/stream/1.mkv\"}]";
        const std::string fOut = w.body(fields);
        expect(fOut.find("\"stream_id\"") != std::string::npos,
               "xtream recon: a dictionary-word password does NOT rewrite the field name stream_id");
        expect(fOut.find("\"container_extension\"") != std::string::npos,
               "xtream recon: ...nor container_extension");
        expect(fOut.find("/movie/<USER>/<PASS>/") != std::string::npos,
               "xtream recon: ...while still being redacted inside an embedded stream URL");

        // Percent-encoding: the URL carries p%40ss1234, the panel's JSON carries p@ss1234.
        // Registering only the encoded spelling shipped the decoded one in direct_source.
        const Redactor e = make("bob", "p%40ss1234", "panel.example.com");
        const std::string enc =
            "{\"direct_source\":\"http://panel.example.com/movie/bob/p@ss1234/9.mkv\"}";
        expect(e.body(enc).find("p@ss1234") == std::string::npos,
               "xtream recon: the DECODED spelling of a %-encoded password is redacted too");

        // *** Userinfo. `http://joe:hunter2@panel.tld/` put the basic-auth password AND the
        // real hostname in the report verbatim, because everything after the first ':' was
        // assumed to be ":port". hostOf() also returned "joe", so the true host was never
        // even registered as a redaction token. ***
        expect(hostOf(L"http://joe:hunter2@panel.example.com:8080/x") == L"panel.example.com",
               "xtream recon: hostOf skips userinfo (or the real host is never redacted)");
        const Redactor ui = make("bob", "pw123456", "panel.example.com");
        const std::string uiOut = ui.url(L"http://joe:hunter2@panel.example.com:8080/get.php?username=bob&password=pw123456");
        expect(uiOut.find("hunter2") == std::string::npos,
               "xtream recon: a userinfo password never reaches the report");
        expect(uiOut.find("panel.example.com") == std::string::npos,
               "xtream recon: ...and the host behind userinfo is still redacted");
        expect(uiOut.find("<HOST>:8080") != std::string::npos,
               "xtream recon: ...with the port preserved");
        expect(hostOf(L"http://[2001:db8::1]:8080/x") == L"2001:db8::1",
               "xtream recon: an IPv6 literal is not cut at its first colon");
        expect(ui.url(L"http://[2001:db8::1]:8080/x").find("db8") == std::string::npos,
               "xtream recon: an IPv6 host is fully redacted, not mangled");

        // A credential carrying a URL delimiter splits across two positions, so the
        // structural pass alone misses it — the scrub() fallback is what catches it.
        const Redactor dp = make("bob", "a/bcdef", "panel.example.com");
        expect(dp.url(L"http://panel.example.com/movie/bob/a/bcdef/1.mkv").find("bcdef") ==
                   std::string::npos,
               "xtream recon: a password containing '/' is still redacted in a URL");

        // *** An ALL-DIGIT credential must not be substituted in JSON value position, or it
        // rewrites `"stream_id":12345678,` and every response reports as unparseable. ***
        const Redactor num = make("11112222", "12345678", "panel.example.com");
        const std::string numBody = "{\"stream_id\":12345678,\"u\":\"12345678\"}";
        const std::string numOut = num.body(numBody);
        expect(numOut.find("\"stream_id\":12345678,") != std::string::npos,
               "xtream recon: a numeric password does NOT corrupt a JSON number (would break every parse)");
        expect(numOut.find("\"<PASS>\"") != std::string::npos,
               "xtream recon: ...but is still redacted inside a quoted string");

        Redactor off;
        off.enabled = false;
        off.add("s3cretpassword", "<PASS>");
        off.finish();
        expect(off.body("x s3cretpassword") == "x s3cretpassword",
               "xtream recon: --raw disables redaction");
    }

    // --- 7. a JSON null is not an identity ------------------------------------
    {
        // container_extension null (a MySQL NULL through json_encode) used to read as the
        // 4-character string "null", so the verdict claimed the field was present on every
        // item and the probe built /movie/u/p/1234.null and reported "NOT reachable".
        const std::string json =
            "[{\"stream_id\":1234,\"container_extension\":null,\"name\":\"A\"},"
            "{\"stream_id\":1235,\"container_extension\":\"mp4\",\"name\":\"B\"}]";
        Census c;
        std::string err;
        // NB use the scan() helper, never `Scanner s(literal, …)`: Scanner holds the body by
        // CONST REFERENCE, so a temporary built from a literal dies at the end of the
        // construction statement and run() then walks freed memory.
        expect(scan(json, c, err), "xtream recon: null-extension fixture parses");
        expect(firstItemField(c, "container_extension").empty(),
               "xtream recon: a JSON null is NOT usable as a container extension");
        expect(firstItemField(c, "stream_id") == "1234",
               "xtream recon: ...while a real value beside it is still read");
        const auto it = c.paths.find("[].container_extension");
        expect(it != c.paths.end() && it->second.nEmpty == 1,
               "xtream recon: the null is counted as empty so the verdict can discount it");
        // Same for an empty string, the other way a panel says "no extension".
        const std::string emptyExt = "[{\"container_extension\":\"\"}]";
        Census c2;
        std::string e2;
        expect(scan(emptyExt, c2, e2) && firstItemField(c2, "container_extension").empty(),
               "xtream recon: an EMPTY container_extension is not usable either");
    }

    // --- 6. URL dissection ----------------------------------------------------
    {
        const std::wstring u = L"http://panel.example.com:8080/get.php?username=abc&password=d&f=1";
        expect(queryValue(u, L"username") == L"abc", "xtream recon: username parsed from the query");
        expect(queryValue(u, L"password") == L"d", "xtream recon: password parsed from the query");
        expect(queryValue(u, L"nope").empty(), "xtream recon: a missing query key is empty");
        expect(originOf(u) == L"http://panel.example.com:8080", "xtream recon: origin keeps the port");
        expect(hostOf(u) == L"panel.example.com", "xtream recon: host drops the port");
        // A trailing key with no '&' after it must still be found (the real playlist URLs
        // end in &type=m3u_plus, but not always).
        expect(queryValue(L"http://h/g.php?password=zz", L"password") == L"zz",
               "xtream recon: a final query param is parsed");
    }
    return 0;
}

}  // namespace rabbitears
