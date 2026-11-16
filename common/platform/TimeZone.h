// SPDX-License-Identifier: GPL-3.0-or-later
// TimeZone — a named (IANA) time zone's UTC offset at a given moment, for catch-up URLs: an Xtream
// panel reads a timeshift start on its own wall clock (XtreamClient xtreamTimeshiftUrl), and its
// account probe names the zone ("Europe/Amsterdam"). A seam each platform implements — so daylight
// saving on the day of the PROGRAMME is right, not the offset measured on the day of the sync:
// Win32/platform/TimeZone.cpp through the C++20 time-zone database (the MSVC runtime backs it with
// Windows' ICU, Windows 10 1903 and later); mac/platform/TimeZone.mm through NSTimeZone (Apple's
// libc++ does not ship the tzdb). common/ui/CatchupSync uses it.
#pragma once

#include <string>

namespace rabbitears {

// The offset in seconds (local minus UTC) of `ianaZone` at `utc`; false — `*offsetSec` untouched —
// when the name is empty or unknown, or the database is unavailable.
bool utcOffsetAt(const std::wstring& ianaZone, long long utc, int* offsetSec);

}  // namespace rabbitears
