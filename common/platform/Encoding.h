// SPDX-License-Identifier: GPL-3.0-or-later
// UTF-8 <-> UTF-16/UTF-32 conversion helpers. The core/UI work in std::wstring
// (UTF-16 on Windows, UTF-32 on macOS); SQLite's C API and network/M3U bytes are
// UTF-8. The free-function contract (utf8FromWide / wideFromUtf8) is identical on
// both platforms; only the implementation differs.
//
// Windows branch ported verbatim from the sibling app SQLTerminal-Win32.
#pragma once

#include <cstring>
#include <string>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace rabbitears {

inline std::string utf8FromWide(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

inline std::wstring wideFromUtf8(const char* s, int len = -1) {
    if (!s) return {};
    if (len < 0) len = static_cast<int>(std::strlen(s));
    if (len == 0) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, len, w.data(), n);
    return w;
}

inline std::wstring wideFromUtf8(const std::string& s) {
    return wideFromUtf8(s.c_str(), static_cast<int>(s.size()));
}

}  // namespace rabbitears

#else  // non-Windows (macOS/other): wchar_t is 32-bit (UCS-4), so no surrogates.

namespace rabbitears {

inline std::string utf8FromWide(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        const unsigned long cp = static_cast<unsigned long>(wc);
        if (cp <= 0x7F) {
            s.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.append("\xEF\xBF\xBD");  // U+FFFD replacement
        }
    }
    return s;
}

inline std::wstring wideFromUtf8(const char* s, int len = -1) {
    if (!s) return {};
    if (len < 0) len = static_cast<int>(std::strlen(s));
    std::wstring w;
    int i = 0;
    while (i < len) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned long cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1Fu; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0Fu; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07u; extra = 3; }
        else { cp = 0xFFFD; extra = 0; }  // invalid lead byte
        ++i;
        for (int k = 0; k < extra; ++k) {
            if (i >= len || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
                cp = 0xFFFD;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3Fu);
            ++i;
        }
        w.push_back(static_cast<wchar_t>(cp));
    }
    return w;
}

inline std::wstring wideFromUtf8(const std::string& s) {
    return wideFromUtf8(s.c_str(), static_cast<int>(s.size()));
}

}  // namespace rabbitears

#endif
