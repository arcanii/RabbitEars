// SPDX-License-Identifier: GPL-3.0-or-later
//
// WinTypes — the handful of Windows types and macros the shared PIXEL code (common/ui/PhotoCells)
// was written with: COLORREF/RGB/Get?Value, RECT/LONG, UINT/BYTE/WORD/DWORD, MulDiv. On Windows it is
// <windows.h> itself, so that code compiles exactly as it did in Win32/ui; elsewhere it is a minimal
// portable definition with the SAME values (COLORREF is 0x00BBGGRR, MulDiv rounds half away from
// zero and returns -1 on a zero divisor), so the mac app compiles the identical rasteriser. Nothing
// here draws — that is the point: only code that writes pixels itself may include this.
#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#else

#include <cstdint>
#include <cstdlib>  // llabs

using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
using UINT = unsigned int;
using LONG = std::int32_t;  // 32-bit, as on Windows (LLP64) — not `long`, which is 64-bit here
using COLORREF = DWORD;

struct RECT {
    LONG left, top, right, bottom;
};

constexpr COLORREF RGB(int r, int g, int b) {
    return static_cast<COLORREF>(static_cast<BYTE>(r)) | (static_cast<COLORREF>(static_cast<BYTE>(g)) << 8) |
           (static_cast<COLORREF>(static_cast<BYTE>(b)) << 16);
}
constexpr BYTE GetRValue(COLORREF c) { return static_cast<BYTE>(c & 0xFF); }
constexpr BYTE GetGValue(COLORREF c) { return static_cast<BYTE>((c >> 8) & 0xFF); }
constexpr BYTE GetBValue(COLORREF c) { return static_cast<BYTE>((c >> 16) & 0xFF); }
constexpr COLORREF CLR_INVALID = 0xFFFFFFFF;

// a * b / c in 64 bits, rounded half away from zero — Windows' MulDiv; -1 when c == 0.
inline int MulDiv(int a, int b, int c) {
    if (c == 0) return -1;
    const long long p = static_cast<long long>(a) * b;
    const long long q = (p >= 0) == (c > 0) ? (llabs(p) + llabs(c) / 2) / llabs(c)
                                            : -((llabs(p) + llabs(c) / 2) / llabs(c));
    return static_cast<int>(q);
}

#endif
