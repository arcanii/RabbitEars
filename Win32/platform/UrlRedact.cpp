// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/UrlRedact.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

#include "platform/Encoding.h"

namespace rabbitears {
namespace {

constexpr size_t kMinSecretLen = 4;
const wchar_t* const kMask = L"***";

bool isUrlEnd(wchar_t c) {
    return std::iswspace(c) || c == L'"' || c == L'\'' || c == L'<' || c == L'>';
}
bool isSchemeChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
           c == L'+' || c == L'-' || c == L'.';
}
bool isAlpha(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

std::wstring lowerAscii(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

// Query parameter names whose value is a credential. Compared lower-case.
bool isCredentialParam(const std::wstring& name) {
    static const wchar_t* const kNames[] = {
        L"username", L"user",   L"password",     L"pass", L"passwd", L"pwd",
        L"token",    L"access_token", L"auth",   L"key",  L"apikey", L"api_key", L"secret"};
    const std::wstring n = lowerAscii(name);
    for (const wchar_t* k : kNames)
        if (n == k) return true;
    return false;
}

int hexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// %XX-decode. '+' is left alone here; urlCredentials() also registers the '+'-as-space form of a
// query value separately, because XtreamClient reads a '+' in a query as a space (and re-encodes
// the credential for a stream path, a space as %20 — that spelling is picked up when the stream is
// played; see streamLoginToRegister). Undecodable escapes are kept verbatim. Bytes are UTF-8.
std::wstring percentDecode(const std::wstring& s) {
    std::string bytes;
    bool any = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            const int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                any = true;
                continue;
            }
        }
        if (s[i] < 0x80) bytes.push_back(static_cast<char>(s[i]));
        else bytes += utf8FromWide(std::wstring(1, s[i]));
    }
    return any ? wideFromUtf8(bytes) : s;
}

// A MIRROR of common/core/XtreamClient.cpp encodeComponent (file-local there, so not callable
// here): every UTF-8 byte outside A-Z a-z 0-9 - . _ ~ as %XX, upper-case hex. It is how the app
// writes a stored credential into a stream path; keep the two in step.
std::wstring encodeLikeXtream(const std::wstring& s) {
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

// A path segment that is an Xtream stream id: digits, optionally with an extension (123, 123.ts).
bool isStreamId(const std::wstring& seg) {
    size_t i = 0;
    while (i < seg.size() && seg[i] >= L'0' && seg[i] <= L'9') ++i;
    if (i == 0) return false;
    if (i == seg.size()) return true;
    if (seg[i] != L'.' || i + 1 == seg.size()) return false;
    for (size_t j = i + 1; j < seg.size(); ++j)
        if (!isAlpha(seg[j]) && !(seg[j] >= L'0' && seg[j] <= L'9')) return false;
    return true;
}

// The pieces of one URL token that we mask. Offsets are into the token; ranges are [begin, end).
struct UrlParts {
    size_t userInfoBegin = std::wstring::npos, userInfoEnd = 0;  // before '@'
    std::vector<std::pair<size_t, size_t>> credValues;            // credential query values
    std::vector<std::pair<size_t, size_t>> pathCreds;             // Xtream stream-path login
};

// `tok` is a whole URL ("scheme://…"). `authorityBegin` is just past "://".
UrlParts analyse(const std::wstring& tok, size_t authorityBegin) {
    UrlParts p;
    size_t authEnd = tok.find_first_of(L"/?#", authorityBegin);
    if (authEnd == std::wstring::npos) authEnd = tok.size();
    const size_t at = tok.rfind(L'@', authEnd == 0 ? 0 : authEnd - 1);
    // at > authorityBegin: an EMPTY user-info is the multicast form (udp://@239.1.1.1:1234), not
    // a credential.
    if (at != std::wstring::npos && at > authorityBegin && at < authEnd) {
        p.userInfoBegin = authorityBegin;
        p.userInfoEnd = at;
    }
    // The Xtream stream-URL shapes, which carry the login as path segments:
    //   /USER/PASS/123[.ext]    and    /(live|movie|series|timeshift)/USER/PASS/…/123[.ext]
    // (XtreamClient writes a '#' or '?' in a credential into a path as %23 / %3F.)
    size_t pathEnd = tok.find_first_of(L"?#", authEnd);
    if (pathEnd == std::wstring::npos) pathEnd = tok.size();
    std::vector<std::pair<size_t, size_t>> segs;
    for (size_t i = authEnd; i < pathEnd;) {
        if (tok[i] == L'/') { ++i; continue; }
        size_t e = tok.find(L'/', i);
        if (e == std::wstring::npos || e > pathEnd) e = pathEnd;
        segs.emplace_back(i, e);
        i = e;
    }
    auto seg = [&tok, &segs](size_t k) {
        return tok.substr(segs[k].first, segs[k].second - segs[k].first);
    };
    // The last segment may carry the sentence around the URL ("…/123.ts," or "…/123.ts)"): the
    // token only ends at whitespace, a quote or an angle bracket.
    std::wstring last = segs.empty() ? std::wstring() : seg(segs.size() - 1);
    while (!last.empty() && std::wcschr(L",.;:!?)]", last.back())) last.pop_back();
    if (segs.size() >= 3 && isStreamId(last)) {
        const std::wstring kind = lowerAscii(seg(0));
        const bool isKind =
            kind == L"live" || kind == L"movie" || kind == L"series" || kind == L"timeshift";
        if (isKind && segs.size() >= 4)
            p.pathCreds = { segs[1], segs[2] };
        else if (!isKind && segs.size() == 3)  // /live/chan1/123.m3u8 is not /USER/PASS/123
            p.pathCreds = { segs[0], segs[1] };
    }
    const size_t q = tok.find(L'?', authEnd);
    if (q == std::wstring::npos) return p;
    // The query runs to the token's end — a '#' does NOT end it here. XtreamClient::queryValue
    // reads a value past a raw '#', so a password containing one ("ab#cd") logs in, and ending
    // the value at the '#' would leave its tail in clear. (A real fragment is then read as part
    // of the last value, which only ever masks more.)
    const size_t qEnd = tok.size();
    // Parameters split on '&' only, as the providers' PHP (and XtreamClient::queryValue) do — a
    // ';' is part of the value. A name written HTML-escaped ("&amp;password=") still counts.
    size_t i = q + 1;
    while (i < qEnd) {
        size_t amp = tok.find(L'&', i);
        if (amp == std::wstring::npos || amp > qEnd) amp = qEnd;
        const size_t eq = tok.find(L'=', i);
        if (eq != std::wstring::npos && eq < amp && amp > eq + 1) {
            std::wstring name = tok.substr(i, eq - i);
            if (lowerAscii(name).rfind(L"amp;", 0) == 0) name.erase(0, 4);
            if (isCredentialParam(name)) p.credValues.emplace_back(eq + 1, amp);
        }
        i = amp + 1;
    }
    return p;
}

// Find the next URL token at or after `from`: returns [begin, end) and the authority start, or
// begin == npos when there is none.
struct Token { size_t begin = std::wstring::npos, end = 0, authority = 0; };
Token nextUrl(const std::wstring& s, size_t from) {
    for (size_t sep = s.find(L"://", from); sep != std::wstring::npos; sep = s.find(L"://", sep + 3)) {
        size_t b = sep;
        while (b > from && isSchemeChar(s[b - 1])) --b;
        while (b < sep && !isAlpha(s[b])) ++b;  // a scheme starts with a letter
        if (b == sep) continue;                 // "://" with no scheme in front
        size_t e = sep + 3;
        while (e < s.size() && !isUrlEnd(s[e])) ++e;
        // Two URLs run together ("…/e.xml,http://…", "?url=http://…") are two tokens: end this
        // one where the next scheme begins, so the second is analysed on its own.
        for (size_t in = s.find(L"://", sep + 3); in != std::wstring::npos && in < e;
             in = s.find(L"://", in + 3)) {
            size_t b2 = in;
            while (b2 > sep + 3 && isSchemeChar(s[b2 - 1])) --b2;
            while (b2 < in && !isAlpha(s[b2])) ++b2;
            if (b2 < in) {
                e = b2;
                break;
            }
        }
        return {b, e, sep + 3};
    }
    return {};
}

bool isWordChar(wchar_t c) { return std::iswalnum(c) || c == L'_'; }

// `s` with each occurrence of `what` that stands as a whole token replaced by `with` — so a short
// login ("test") is not masked inside other words. "Whole" is judged only at an edge where `what`
// itself begins/ends with a letter, digit or '_': there the neighbour must not be one (a %XX
// escape just before it counts as a boundary — a login inside a URL-encoded URL, "%2FUSER%2FPASS");
// an edge that is punctuation ("!abc1") matches whatever is beside it. One pass into a new
// string (only once there is a hit), so the cost stays linear however many hits there are (the
// startup scrub of a large old log can have hundreds of thousands).
void replaceWhole(std::wstring& s, const std::wstring& what, const wchar_t* with) {
    if (what.empty()) return;
    const bool checkLeft = isWordChar(what.front()), checkRight = isWordChar(what.back());
    std::wstring out;
    size_t copied = 0;
    bool any = false;
    for (size_t pos = s.find(what); pos != std::wstring::npos;) {
        const size_t end = pos + what.size();
        const bool escapedLeft = pos >= 3 && s[pos - 3] == L'%' && hexVal(s[pos - 2]) >= 0 &&
                                 hexVal(s[pos - 1]) >= 0;
        const bool leftOk = !checkLeft || pos == 0 || !isWordChar(s[pos - 1]) || escapedLeft;
        const bool rightOk = !checkRight || end >= s.size() || !isWordChar(s[end]);
        if (leftOk && rightOk) {
            if (!any) {
                out.reserve(s.size());
                any = true;
            }
            out.append(s, copied, pos - copied);
            out += with;
            copied = end;
            pos = s.find(what, end);
        } else {
            pos = s.find(what, pos + 1);
        }
    }
    if (!any) return;
    out.append(s, copied, std::wstring::npos);
    s.swap(out);
}

}  // namespace

std::wstring extractHttpUrl(const std::wstring& text) {
    // Already just an address (the usual case): keep it exactly as typed. A password may end in
    // '.', '!' or ')', and trimming one off would silently break the login on every refresh.
    const size_t tb = text.find_first_not_of(L" \t\r\n");
    if (tb != std::wstring::npos) {
        const size_t te = text.find_last_not_of(L" \t\r\n");
        const std::wstring whole = text.substr(tb, te - tb + 1);
        const std::wstring wl = lowerAscii(whole);
        bool single = wl.rfind(L"http://", 0) == 0 || wl.rfind(L"https://", 0) == 0;
        for (wchar_t c : whole)
            if (isUrlEnd(c)) single = false;
        if (single) return whole.size() > wl.find(L"://") + 3 ? whole : L"";
    }
    // Otherwise the address was pulled out of other text: trim what reads as that text's
    // punctuation.
    const std::wstring low = lowerAscii(text);
    size_t b = low.find(L"http://");
    const size_t bs = low.find(L"https://");
    if (bs != std::wstring::npos && (b == std::wstring::npos || bs < b)) b = bs;
    if (b == std::wstring::npos) return L"";
    size_t e = b;
    while (e < text.size() && !isUrlEnd(text[e])) ++e;
    std::wstring url = text.substr(b, e - b);
    for (;;) {  // drop trailing sentence punctuation, and a ')' that closes nothing in the URL
        if (url.empty()) break;
        const wchar_t c = url.back();
        if (c == L'.' || c == L',' || c == L';' || c == L':' || c == L'!' || c == L'?') {
            url.pop_back();
            continue;
        }
        if (c == L')' && url.find(L'(') == std::wstring::npos) {
            url.pop_back();
            continue;
        }
        break;
    }
    const size_t sep = url.find(L"://");
    if (sep == std::wstring::npos || sep + 3 >= url.size()) return L"";  // "http://" alone
    return url;
}

std::vector<std::wstring> urlCredentials(const std::wstring& url) {
    std::vector<std::wstring> out;
    auto add = [&out](const std::wstring& v) {
        if (v.size() < kMinSecretLen) return;
        for (const auto& have : out)
            if (have == v) return;
        out.push_back(v);
    };
    auto addBoth = [&add](const std::wstring& raw) {
        add(raw);
        add(percentDecode(raw));
    };
    for (Token t = nextUrl(url, 0); t.begin != std::wstring::npos; t = nextUrl(url, t.end)) {
        const std::wstring tok = url.substr(t.begin, t.end - t.begin);
        const UrlParts p = analyse(tok, t.authority - t.begin);
        if (p.userInfoBegin != std::wstring::npos) {
            const std::wstring ui = tok.substr(p.userInfoBegin, p.userInfoEnd - p.userInfoBegin);
            const size_t colon = ui.find(L':');
            addBoth(ui.substr(0, colon));
            if (colon != std::wstring::npos) addBoth(ui.substr(colon + 1));
        }
        for (const auto& [vb, ve] : p.credValues) {
            const std::wstring raw = tok.substr(vb, ve - vb);
            addBoth(raw);
            std::wstring plusAsSpace = raw;  // as a query reader (XtreamClient) decodes it
            std::replace(plusAsSpace.begin(), plusAsSpace.end(), L'+', L' ');
            const std::wstring decoded = percentDecode(plusAsSpace);
            add(decoded);
            // ...and exactly as XtreamClient then writes it into a stream path, so last session's
            // log is masked at startup even where it quotes that path without its scheme.
            add(encodeLikeXtream(decoded));
        }
    }
    return out;
}

std::vector<std::wstring> streamLoginToRegister(const std::wstring& url,
                                                const std::vector<std::wstring>& known) {
    std::vector<std::wstring> login;
    auto add = [&login](const std::wstring& v) {
        if (v.size() < kMinSecretLen) return;
        if (std::find(login.begin(), login.end(), v) == login.end()) login.push_back(v);
    };
    const Token t = nextUrl(url, 0);
    if (t.begin == std::wstring::npos) return {};
    const std::wstring tok = url.substr(t.begin, t.end - t.begin);
    const UrlParts p = analyse(tok, t.authority - t.begin);
    for (const auto& [pb, pe] : p.pathCreds) {
        const std::wstring raw = tok.substr(pb, pe - pb);
        add(raw);
        add(percentDecode(raw));
    }
    for (const auto& v : login)
        if (std::find(known.begin(), known.end(), v) != known.end()) return login;
    return {};  // nothing already known: not (provably) this account's login — register nothing
}

std::wstring redactUrls(const std::wstring& line, const std::vector<std::wstring>& secrets) {
    std::wstring out;
    if (line.find(L"://") == std::wstring::npos) {
        out = line;
    } else {
        out.reserve(line.size());
        size_t copied = 0;
        for (Token t = nextUrl(line, 0); t.begin != std::wstring::npos; t = nextUrl(line, t.end)) {
            out.append(line, copied, t.begin - copied);
            std::wstring tok = line.substr(t.begin, t.end - t.begin);
            const UrlParts p = analyse(tok, t.authority - t.begin);
            // Every range to mask (they never overlap: user-info, path and query are disjoint),
            // replaced right-to-left so the earlier offsets stay valid.
            std::vector<std::pair<size_t, size_t>> ranges = p.credValues;
            ranges.insert(ranges.end(), p.pathCreds.begin(), p.pathCreds.end());
            if (p.userInfoBegin != std::wstring::npos) ranges.emplace_back(p.userInfoBegin, p.userInfoEnd);
            std::sort(ranges.begin(), ranges.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            for (const auto& [b, e] : ranges) tok.replace(b, e - b, kMask);
            out += tok;
            copied = t.end;
        }
        out.append(line, copied, std::wstring::npos);
    }
    for (const auto& s : secrets)
        if (s.size() >= kMinSecretLen) replaceWhole(out, s, kMask);
    return out;
}

}  // namespace rabbitears
