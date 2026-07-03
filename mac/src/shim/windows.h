// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal <windows.h> build shim for macOS.
//
// On the mac build, exactly one shared source still reaches for <windows.h>:
// src/ui/DockLayout.h includes it for `RECT`, and src/ui/DockLayout.cpp calls
// the MSVC CRT functions swprintf_s / _wtof. There is no system <windows.h> on
// macOS, so this file (placed ahead of the system search path) supplies JUST
// those three symbols and nothing else.
//
// This is deliberately tiny and clearly-labelled. Phase 2 de-Win32's
// DockLayout (RECT -> POD Rect, swprintf_s/_wtof -> std CRT) and deletes this
// file. See docs/MACOS_PORT.md, Section 5.
#pragma once

#include <cstdarg>
#include <cwchar>

// The only Win32 type DockLayout uses. Field order/names match windows.h.
struct RECT {
    long left;
    long top;
    long right;
    long bottom;
};

// MSVC secure/wide CRT shims used by DockLayout.cpp. The array-reference
// template mirrors MSVC's size-deducing swprintf_s overload.
template <size_t N>
inline int swprintf_s(wchar_t (&buf)[N], const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vswprintf(buf, N, fmt, ap);
    va_end(ap);
    return n;
}

inline double _wtof(const wchar_t* s) { return std::wcstod(s, nullptr); }
