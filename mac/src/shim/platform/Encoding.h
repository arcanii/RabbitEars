// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS build shim for src/platform/Encoding.h.
//
// The real header (src/platform/Encoding.h) pulls in <windows.h> and calls
// WideCharToMultiByte/MultiByteToWideChar. This shim provides the SAME
// free-function contract (rabbitears::utf8FromWide / wideFromUtf8) with a
// portable implementation and NO <windows.h>, so the shared core sources
// (M3uParser.cpp, Database.cpp) compile UNCHANGED on clang.
//
// It is placed on an include dir that precedes src/, so `#include
// "platform/Encoding.h"` from a shared source resolves HERE on the mac build
// and to the real header on Windows.
//
// NOTE: wchar_t is 32-bit (UTF-32) on macOS and 16-bit (UTF-16) on Windows.
// This shim converts UTF-8 <-> the platform-native wide encoding, so every
// round-trip in the app (parse -> store -> read) stays self-consistent.
//
// This file is Phase-0 scaffolding. Phase 2 replaces both it and the real
// header with a single cross-platform Encoding seam (see docs/MACOS_PORT.md).
#pragma once

#include <cstring>
#include <string>

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
