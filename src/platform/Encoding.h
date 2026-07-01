// SPDX-License-Identifier: GPL-3.0-or-later
// UTF-8 <-> UTF-16 conversion helpers. The core/UI work in UTF-16 (wchar_t);
// SQLite's C API and network/M3U bytes are UTF-8.
//
// Ported verbatim from the sibling app SQLTerminal-Win32
// (src/platform/Encoding.h); only the namespace differs.
#pragma once

#include <cstring>
#include <string>

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
