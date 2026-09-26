// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/XtreamClient.h"

#include <cwchar>

#include "platform/Encoding.h"

namespace rabbitears {
namespace {

std::wstring queryValue(const std::wstring& url, const std::wstring& key) {
    const size_t q = url.find(L'?');
    if (q == std::wstring::npos) return {};
    const std::wstring rest = url.substr(q + 1);
    for (size_t i = 0; i <= rest.size();) {
        const size_t amp = rest.find(L'&', i);
        const std::wstring pair =
            rest.substr(i, amp == std::wstring::npos ? std::wstring::npos : amp - i);
        const size_t eq = pair.find(L'=');
        if (eq != std::wstring::npos && pair.substr(0, eq) == key) return pair.substr(eq + 1);
        if (amp == std::wstring::npos) break;
        i = amp + 1;
    }
    return {};
}

// scheme://host[:port] — everything before the path. Userinfo is NOT stripped: an Xtream
// playlist URL never carries it, and silently rewriting the origin would change which server
// we talk to.
//
// Requires a real "://" scheme, and terminates the authority at '/', '?' OR '#'. Accepting a
// bare "//" was wrong twice over: `host//x/get.php?…` parsed as origin `host//x`, and
// `http://host?username=a&password=b` (no path) swallowed the whole query into the origin —
// both returned "valid Xtream playlist" and then built nonsense URLs like
// `http://host?username=a&password=b/movie/a/b/12.mp4`.
std::wstring originOf(const std::wstring& url) {
    const size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos || scheme == 0) return {};
    const size_t authStart = scheme + 3;
    const size_t e = url.find_first_of(L"/?#", authStart);
    if (e == authStart) return {};  // "http:///path" — no authority at all
    return e == std::wstring::npos ? url : url.substr(0, e);
}

// %XX / '+' decoding. Query values arrive encoded; we store the DECODED credential and
// re-encode per position on the way out (see encodeFor). '+' means space in a query only.
std::wstring percentDecodeQuery(const std::wstring& s) {
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    std::string bytes;  // decode into UTF-8 bytes, then widen once
    bytes.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'+') { bytes += ' '; continue; }
        if (s[i] == L'%' && i + 2 < s.size()) {
            const int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { bytes += static_cast<char>(h * 16 + l); i += 2; continue; }
        }
        const std::string u8 = utf8FromWide(std::wstring(1, s[i]));
        bytes += u8;
    }
    return wideFromUtf8(bytes);
}

// %XX decoding for a PATH segment ('+' is a literal '+' there, unlike in a query).
std::wstring percentDecodePath(const std::wstring& s) {
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    std::string bytes;
    bytes.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            const int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { bytes += static_cast<char>(h * 16 + l); i += 2; continue; }
        }
        bytes += utf8FromWide(std::wstring(1, s[i]));
    }
    return wideFromUtf8(bytes);
}

// Percent-encode everything outside the unreserved set. Deliberately the SAME conservative
// set for a query value and a path segment: it is valid in both, and the alternative — a
// per-position allow-list — is how a credential ends up correct in one URL and broken in the
// other. That divergence is the worst kind here: the auth probe round-trips through a query
// and succeeds, so --selftest and the login both pass, and only PLAYBACK breaks.
std::wstring encodeComponent(const std::wstring& s) {
    static const wchar_t* kHex = L"0123456789ABCDEF";
    const std::string u8 = utf8FromWide(s);
    std::wstring o;
    o.reserve(u8.size());
    for (unsigned char c : u8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            o += static_cast<wchar_t>(c);
        } else {
            o += L'%';
            o += kHex[c >> 4];
            o += kHex[c & 0x0F];
        }
    }
    return o;
}

// A container extension has to LOOK like one. `.empty()` alone let whitespace, a
// query-bearing value ("mp4?a=b") and junk straight into the play URL, producing exactly the
// 404-that-reads-as-broken this design promises to avoid — and without being counted.
bool looksLikeExtension(const std::wstring& e) {
    if (e.empty() || e.size() > 5) return false;
    for (wchar_t c : e)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')))
            return false;
    return true;
}

std::wstring toW(const JsonValue& v) { return v.asWString(); }

// Days since 1970-01-01 for a proleptic Gregorian date, and back (H. Hinnant's civil algorithms) —
// plain arithmetic, so no gmtime_s / gmtime_r / timegm platform split in the shared core.
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}
void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<long long>(yoe) + era * 400 + (m <= 2);
}

// "YYYY-MM-DD HH:MM:SS" read as if it were UTC (server_info.time_now); 0 unless it is exactly that.
long long epochOfWallClock(const std::wstring& s) {
    if (s.size() != 19 || s[4] != L'-' || s[7] != L'-' || s[10] != L' ' || s[13] != L':' || s[16] != L':')
        return 0;
    auto num = [&](size_t at, size_t n, long long& v) {
        v = 0;
        for (size_t i = at; i < at + n; ++i) {
            if (s[i] < L'0' || s[i] > L'9') return false;
            v = v * 10 + (s[i] - L'0');
        }
        return true;
    };
    long long y, mo, d, h, mi, se;
    if (!num(0, 4, y) || !num(5, 2, mo) || !num(8, 2, d) || !num(11, 2, h) || !num(14, 2, mi) || !num(17, 2, se))
        return 0;
    static const int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mo < 1 || mo > 12 || d < 1 || d > kDays[mo - 1] || h > 23 || mi > 59 || se > 60) return 0;
    if (mo == 2 && d == 29 && !(y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 0;
    return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + se;
}

// An epoch as the timeshift URL's "YYYY-MM-DD:HH-MM" (the epoch already shifted to the server's clock).
std::wstring wallClockMinute(long long t) {
    long long days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; --days; }
    long long y;
    unsigned m, d;
    civilFromDays(days, y, m, d);
    wchar_t b[32];
    swprintf(b, 32, L"%04lld-%02u-%02u:%02lld-%02lld", y, m, d, secs / 3600, (secs % 3600) / 60);
    return b;
}

}  // namespace

// ---------------------------------------------------------------------------

bool parseXtreamPlaylistUrl(const std::wstring& playlistUrl, XtreamCreds& out) {
    out = XtreamCreds{};
    const std::wstring origin = originOf(playlistUrl);
    if (origin.empty()) return false;
    // Both spellings: `username`/`password` is the get.php form, `user`/`pass` shows up on
    // a few panel forks. Anything else is not an Xtream playlist.
    std::wstring u = queryValue(playlistUrl, L"username");
    std::wstring p = queryValue(playlistUrl, L"password");
    if (u.empty()) u = queryValue(playlistUrl, L"user");
    if (p.empty()) p = queryValue(playlistUrl, L"pass");
    if (u.empty() || p.empty()) return false;
    out.origin = origin;
    // Store DECODED. The credentials are read out of a QUERY string but also have to go into
    // a PATH position (/movie/USER/PASS/), and the two encodings differ — '+' is a space in a
    // query and a literal '+' in a path. Decoding once here and re-encoding per position is
    // the only way both URLs are right; carrying the raw query spelling made the API URL work
    // and the play URL silently wrong.
    out.username = percentDecodeQuery(u);
    out.password = percentDecodeQuery(p);
    return true;
}

std::wstring xtreamApiUrl(const XtreamCreds& c, const std::wstring& action) {
    if (!c.valid()) return {};
    std::wstring url = c.origin + L"/player_api.php?username=" + encodeComponent(c.username) +
                       L"&password=" + encodeComponent(c.password);
    if (!action.empty()) url += L"&action=" + action;
    return url;
}

namespace {
std::wstring streamUrl(const XtreamCreds& c, const wchar_t* kind, long long id,
                       const std::wstring& ext) {
    // An empty extension is NOT filled in with a guess. See the header: a guessed suffix
    // turns "we do not know the container" into a 404 that reads as "playback is broken".
    if (!c.valid() || id <= 0 || !looksLikeExtension(ext)) return {};
    return c.origin + L"/" + kind + L"/" + encodeComponent(c.username) + L"/" +
           encodeComponent(c.password) + L"/" + std::to_wstring(id) + L"." + ext;
}
}  // namespace

std::wstring xtreamMovieUrl(const XtreamCreds& c, long long streamId, const std::wstring& ext) {
    return streamUrl(c, L"movie", streamId, ext);
}
std::wstring xtreamEpisodeUrl(const XtreamCreds& c, long long episodeId, const std::wstring& ext) {
    return streamUrl(c, L"series", episodeId, ext);
}

// ---------------------------------------------------------------------------

bool parseXtreamAccount(const std::string& body, XtreamAccount& out, std::wstring* err) {
    out = XtreamAccount{};
    JsonValue root;
    std::string perr;
    if (!parseJson(body, root, &perr)) {
        if (err) *err = wideFromUtf8(perr);
        return false;
    }
    if (!root.isObject() || !root.has("user_info")) {
        if (err) *err = L"no user_info block — this does not look like player_api.php";
        return false;
    }
    const JsonValue& ui = root["user_info"];
    out.status = toW(ui["status"]);
    // asBool() handles every spelling a panel uses for this flag: 1, "1", true, 0, "0",
    // false, "". The measured panel sent a bare number 1.
    //
    // ABSENT is not the same as 0. A fork that reports `status: "Active"` with no `auth`
    // member at all would otherwise be told its credentials were rejected — and "panels
    // differ from the spec, and from each other" is the whole premise of this file, so the
    // tolerance cannot stop here. Fall back to the status string.
    if (ui.has("auth"))
        out.authOk = ui["auth"].asBool(false);
    else
        out.authOk = (out.status == L"Active");
    out.expiresAt = ui["exp_date"].asInt64(0);           // quoted epoch
    out.maxConnections = ui["max_connections"].asInt(0);  // quoted number
    out.serverTime = root["server_info"]["timestamp_now"].asInt64(0);
    out.timezone = toW(root["server_info"]["timezone"]);
    out.serverLocalTime = epochOfWallClock(toW(root["server_info"]["time_now"]));
    return true;
}

bool xtreamServerUtcOffset(const XtreamAccount& a, int* offsetSec) {
    if (!offsetSec || a.serverTime <= 0 || a.serverLocalTime <= 0) return false;
    const long long d = a.serverLocalTime - a.serverTime;
    const long long q = 900;  // round to the quarter hour, half away from zero
    const long long r = (d >= 0 ? d + q / 2 : d - q / 2) / q * q;
    if (r < -12 * 3600 || r > 14 * 3600) return false;
    *offsetSec = static_cast<int>(r);
    return true;
}

std::wstring xtreamTimeshiftUrl(const XtreamCreds& c, long long streamId, long long startUtc, int minutes,
                                int serverUtcOffsetSec) {
    if (!c.valid() || streamId <= 0 || minutes <= 0 || startUtc <= 0) return {};
    return c.origin + L"/timeshift/" + encodeComponent(c.username) + L"/" + encodeComponent(c.password) + L"/" +
           std::to_wstring(minutes) + L"/" + wallClockMinute(startUtc + serverUtcOffsetSec) + L"/" +
           std::to_wstring(streamId) + L".ts";
}

long long xtreamLiveStreamId(const std::wstring& streamUrl, const XtreamCreds* creds) {
    const std::wstring origin = originOf(streamUrl);
    if (origin.empty()) return 0;
    std::wstring path = streamUrl.substr(origin.size());
    const size_t cut = path.find_first_of(L"?#");
    if (cut != std::wstring::npos) path.erase(cut);
    std::vector<std::wstring> seg;  // the non-empty path segments
    for (size_t i = 0; i < path.size();) {
        const size_t slash = path.find(L'/', i);
        const size_t end = slash == std::wstring::npos ? path.size() : slash;
        if (end > i) seg.push_back(path.substr(i, end - i));
        i = end + 1;
    }
    // {user}/{pass}/{id} or live/{user}/{pass}/{id} — and nothing else: movie/, series/ and timeshift/
    // URLs have more segments (a username that happens to be "movie" is still a username).
    if (!(seg.size() == 3 || (seg.size() == 4 && seg[0] == L"live"))) return 0;
    if (creds) {
        const size_t u = seg.size() - 3;
        if (percentDecodePath(seg[u]) != creds->username || percentDecodePath(seg[u + 1]) != creds->password)
            return 0;
    }
    std::wstring last = seg.back();
    const size_t dot = last.find(L'.');
    if (dot != std::wstring::npos) last.erase(dot);
    if (last.empty() || last.size() > 15) return 0;
    long long id = 0;
    for (wchar_t ch : last) {
        if (ch < L'0' || ch > L'9') return 0;
        id = id * 10 + (ch - L'0');
    }
    return id;
}

bool parseXtreamCategories(const std::string& body, std::vector<XtreamCategory>& out,
                           std::wstring* err) {
    out.clear();
    JsonValue root;
    std::string perr;
    if (!parseJson(body, root, &perr)) {
        if (err) *err = wideFromUtf8(perr);
        return false;
    }
    if (!root.isArray()) {
        if (err) *err = L"expected an array of categories";
        return false;
    }
    out.reserve(root.size());
    for (const JsonValue& v : root.elements()) {
        XtreamCategory c;
        // category_id is a quoted number on every panel seen, but asWString() renders a
        // bare number from its original token too, so either spelling yields the same key.
        c.id = toW(v["category_id"]);
        c.name = toW(v["category_name"]);
        if (c.id.empty()) continue;  // a category with no id cannot be referenced by an item
        out.push_back(std::move(c));
    }
    return true;
}

bool parseXtreamLiveArchive(const std::string& body, std::vector<XtreamArchive>& out, size_t* total,
                            std::wstring* err) {
    out.clear();
    if (total) *total = 0;
    JsonValue root;
    std::string perr;
    if (!parseJson(body, root, &perr)) {
        if (err) *err = wideFromUtf8(perr);
        return false;
    }
    if (!root.isArray()) {
        if (err) *err = L"expected an array of live streams";
        return false;
    }
    if (total) *total = root.size();
    for (const JsonValue& v : root.elements()) {
        // tv_archive: 1 / "1" / true (asBool reads every spelling); the duration is in days, quoted or not.
        if (!v["tv_archive"].asBool(false)) continue;
        XtreamArchive a;
        a.streamId = v["stream_id"].asInt64(0);
        a.days = v["tv_archive_duration"].asInt(0);
        if (a.streamId > 0 && a.days > 0) out.push_back(a);
    }
    return true;
}

bool parseXtreamVodStreams(const std::string& body, XtreamVodResult& out, std::wstring* err) {
    out = XtreamVodResult{};
    JsonValue root;
    std::string perr;
    if (!parseJson(body, root, &perr)) {
        if (err) *err = wideFromUtf8(perr);
        return false;
    }
    if (!root.isArray()) {
        if (err) *err = L"expected an array of VOD streams";
        return false;
    }
    out.total = static_cast<int>(root.size());
    out.movies.reserve(root.size());
    for (const JsonValue& v : root.elements()) {
        XtreamMovie m;
        // stream_id came back as a real number here; vod_id is the fallback other panels
        // use. asInt64 reads either spelling.
        m.streamId = v["stream_id"].asInt64(0);
        if (m.streamId <= 0) m.streamId = v["vod_id"].asInt64(0);
        if (m.streamId <= 0) { ++out.skippedNoId; continue; }

        m.containerExt = toW(v["container_extension"]);
        // Absent, null, "", whitespace and anything not extension-shaped all land here, and
        // all mean the same thing: no constructible URL. Counted rather than guessed at, and
        // shape-checked rather than merely non-empty — see looksLikeExtension().
        if (!looksLikeExtension(m.containerExt)) { ++out.skippedNoExt; continue; }

        m.name = toW(v["name"]);
        // A nameless row would pass the DAO's validity gate (which only tests the URL) and
        // land as a blank entry in the grid. Give it something selectable instead.
        if (m.name.empty()) m.name = L"#" + std::to_wstring(m.streamId);
        m.icon = toW(v["stream_icon"]);
        m.categoryId = toW(v["category_id"]);
        m.added = v["added"].asInt64(0);
        m.adult = v["is_adult"].asBool(false);
        out.movies.push_back(std::move(m));
    }
    return true;
}

// ---------------------------------------------------------------------------

std::vector<ParsedChannel> xtreamMoviesToChannels(const XtreamCreds& c,
                                                  const std::vector<XtreamMovie>& movies,
                                                  const std::vector<XtreamCategory>& categories,
                                                  const std::wstring& fallbackGroup) {
    std::vector<ParsedChannel> out;
    if (!c.valid()) return out;
    out.reserve(movies.size());
    for (const XtreamMovie& m : movies) {
        ParsedChannel p;
        p.streamUrl = xtreamMovieUrl(c, m.streamId, m.containerExt);
        if (p.streamUrl.empty()) continue;  // belt-and-braces; the parser already dropped these
        p.name = m.name;
        p.logoUrl = m.icon;
        // Category NAME, not id, so VOD lands in the existing Groups nav tree as readable
        // entries ("VOD - ACTIE [NL]") rather than as numbers. Linear lookup: category lists
        // are tens of entries, and building a map for 67 of them would cost more than it saves.
        p.groupTitle = fallbackGroup;
        if (!m.categoryId.empty()) {
            for (const XtreamCategory& cat : categories)
                if (cat.id == m.categoryId) {
                    if (!cat.name.empty()) p.groupTitle = cat.name;
                    break;
                }
        }
        p.kind = Channel::Kind::Movie;
        p.addedAt = m.added;
        out.push_back(std::move(p));
    }
    return out;
}

}  // namespace rabbitears
