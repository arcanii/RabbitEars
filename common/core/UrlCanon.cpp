// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/UrlCanon.h"

namespace rabbitears {
namespace {

// ASCII-only lowering. The scheme is ASCII by definition (RFC 3986 §3.1), and deliberately NOT
// std::towlower: the app never calls setlocale(), which is the same trap that made rule matching
// fold only A-Z until 0.2.14.
template <class Ch>
Ch lowerAscii(Ch c) { return (c >= Ch('A') && c <= Ch('Z')) ? static_cast<Ch>(c + 32) : c; }

template <class View>
bool schemeIs(const View& url, size_t len, const char* want) {
    size_t i = 0;
    for (; i < len && want[i]; ++i)
        if (lowerAscii(url[i]) != static_cast<typename View::value_type>(want[i])) return false;
    return i == len && want[i] == 0;  // both ended together
}

// The port substring is a run of digits, and only a run of digits. An empty port ("http://h:/x",
// which RFC 3986 permits) is deliberately NOT treated as the default: leaving a pathological
// spelling alone costs one duplicate row, and guessing at it is how a canonicaliser starts
// rewriting things it does not understand.
template <class View>
bool portEquals(const View& s, size_t from, size_t to, int want) {
    if (from >= to || to - from > 5) return false;  // empty, or too long to be a port
    int v = 0;
    for (size_t i = from; i < to; ++i) {
        if (s[i] < typename View::value_type('0') || s[i] > typename View::value_type('9'))
            return false;
        v = v * 10 + static_cast<int>(s[i] - typename View::value_type('0'));
    }
    return v == want;
}

// Returns the [begin, end) byte/char range to erase, or {0,0} for "leave it alone". Shared by both
// overloads: every delimiter examined is ASCII, and UTF-8 never puts an ASCII byte inside a
// multi-byte sequence, so the same index arithmetic is correct for chars and for bytes.
template <class View>
std::pair<size_t, size_t> defaultPortRange(const View& url) {
    using Ch = typename View::value_type;
    const auto npos = View::npos;

    size_t sep = npos;
    for (size_t i = 0; i + 2 < url.size(); ++i)
        if (url[i] == Ch(':') && url[i + 1] == Ch('/') && url[i + 2] == Ch('/')) { sep = i; break; }
    if (sep == npos || sep == 0) return {0, 0};  // no scheme: not ours to rewrite

    int defaultPort = 0;
    if (schemeIs(url, sep, "http")) defaultPort = 80;
    else if (schemeIs(url, sep, "https")) defaultPort = 443;
    else return {0, 0};  // rtsp/rtmp/udp/file/... — we do not know their defaults, so hands off

    const size_t authStart = sep + 3;
    size_t authEnd = url.size();
    for (size_t i = authStart; i < url.size(); ++i)
        if (url[i] == Ch('/') || url[i] == Ch('?') || url[i] == Ch('#')) { authEnd = i; break; }
    if (authStart >= authEnd) return {0, 0};  // "http:///path" — no authority at all

    // Userinfo may itself contain a colon ("user:password@host"), so the port search must start
    // AFTER the last '@' inside the authority. Without this, "http://u:pw@host/x" reads "pw@host"
    // as a port and the whole credential is destroyed.
    size_t hostStart = authStart;
    for (size_t i = authStart; i < authEnd; ++i)
        if (url[i] == Ch('@')) hostStart = i + 1;

    // An IPv6 literal is bracketed and full of colons, none of which is the port: only a colon
    // AFTER the closing ']' is. "http://[::1]/x" must come back untouched.
    size_t colon = npos;
    if (hostStart < authEnd && url[hostStart] == Ch('[')) {
        size_t close = npos;
        for (size_t i = hostStart; i < authEnd; ++i)
            if (url[i] == Ch(']')) { close = i; break; }
        if (close == npos) return {0, 0};  // malformed — leave it
        if (close + 1 < authEnd && url[close + 1] == Ch(':')) colon = close + 1;
    } else {
        for (size_t i = hostStart; i < authEnd; ++i)
            if (url[i] == Ch(':')) colon = i;  // last colon wins; a bare host has none
    }
    if (colon == npos) return {0, 0};                                   // no explicit port
    if (!portEquals(url, colon + 1, authEnd, defaultPort)) return {0, 0};  // non-default: keep it
    return {colon, authEnd};
}

}  // namespace

std::wstring canonicalStreamUrl(const std::wstring& url) {
    const auto r = defaultPortRange(std::wstring_view(url));
    if (r.first == r.second) return url;
    std::wstring out = url;
    out.erase(r.first, r.second - r.first);
    return out;
}

std::string canonicalStreamUrlU8(std::string_view url) {
    const auto r = defaultPortRange(url);
    if (r.first == r.second) return std::string(url);
    std::string out(url);
    out.erase(r.first, r.second - r.first);
    return out;
}

}  // namespace rabbitears
