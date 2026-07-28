// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/UrlCanon.h"

namespace rabbitears {
namespace {

// ASCII-only lowering. The scheme is ASCII by definition (RFC 3986 §3.1), and deliberately NOT
// std::towlower: the app never calls setlocale(), which is the same trap that made rule matching
// fold only A-Z until 0.2.14.
wchar_t lowerAscii(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c; }

bool schemeIs(const std::wstring& url, size_t len, const wchar_t* want) {
    size_t i = 0;
    for (; i < len && want[i]; ++i)
        if (lowerAscii(url[i]) != want[i]) return false;
    return i == len && want[i] == 0;  // both ended together
}

// The port substring is a run of digits, and only a run of digits. An empty port ("http://h:/x",
// which RFC 3986 permits) is deliberately NOT treated as the default: leaving a pathological
// spelling alone costs one duplicate row, and guessing at it is how a canonicaliser starts
// rewriting things it does not understand.
bool portEquals(const std::wstring& s, size_t from, size_t to, int want) {
    if (from >= to || to - from > 5) return false;  // empty, or too long to be a port
    int v = 0;
    for (size_t i = from; i < to; ++i) {
        if (s[i] < L'0' || s[i] > L'9') return false;
        v = v * 10 + (s[i] - L'0');
    }
    return v == want;
}

}  // namespace

std::wstring canonicalStreamUrl(const std::wstring& url) {
    const size_t sep = url.find(L"://");
    if (sep == std::wstring::npos || sep == 0) return url;  // no scheme: not ours to rewrite

    int defaultPort = 0;
    if (schemeIs(url, sep, L"http")) defaultPort = 80;
    else if (schemeIs(url, sep, L"https")) defaultPort = 443;
    else return url;  // rtsp/rtmp/udp/file/... — we do not know their defaults, so hands off

    const size_t authStart = sep + 3;
    size_t authEnd = url.find_first_of(L"/?#", authStart);
    if (authEnd == std::wstring::npos) authEnd = url.size();
    if (authStart >= authEnd) return url;  // "http:///path" — no authority at all

    // Userinfo may itself contain a colon ("user:password@host"), so the port search must start
    // AFTER the last '@' inside the authority. Without this, "http://u:pw@host/x" reads "pw@host"
    // as a port and the whole credential is destroyed.
    size_t hostStart = authStart;
    for (size_t i = authStart; i < authEnd; ++i)
        if (url[i] == L'@') hostStart = i + 1;

    // An IPv6 literal is bracketed and full of colons, none of which is the port: only a colon
    // AFTER the closing ']' is. "http://[::1]/x" must come back untouched.
    size_t colon = std::wstring::npos;
    if (hostStart < authEnd && url[hostStart] == L'[') {
        const size_t close = url.find(L']', hostStart);
        if (close == std::wstring::npos || close >= authEnd) return url;  // malformed — leave it
        if (close + 1 < authEnd && url[close + 1] == L':') colon = close + 1;
    } else {
        for (size_t i = hostStart; i < authEnd; ++i)
            if (url[i] == L':') colon = i;  // last colon wins; a bare host has none
    }
    if (colon == std::wstring::npos) return url;  // no explicit port — already canonical
    if (!portEquals(url, colon + 1, authEnd, defaultPort)) return url;  // non-default: keep it

    std::wstring out = url;
    out.erase(colon, authEnd - colon);
    return out;
}

}  // namespace rabbitears
