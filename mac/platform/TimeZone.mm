// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS implementation of platform/TimeZone.h (the Windows peer is TimeZone.cpp, on the C++20
// tzdb — which Apple's libc++ does not ship). NSTimeZone reads the system's IANA database, so a
// panel's "Europe/Amsterdam" gives its offset on the PROGRAMME's day, daylight saving included.
#import <Foundation/Foundation.h>

#include "platform/TimeZone.h"

namespace rabbitears {

bool utcOffsetAt(const std::wstring& ianaZone, long long utc, int* offsetSec) {
    if (ianaZone.empty() || !offsetSec) return false;
    std::string name;  // IANA names are ASCII; anything else is not one
    for (wchar_t c : ianaZone) {
        if (c <= 0 || c > 0x7E) return false;
        name += static_cast<char>(c);
    }
    @autoreleasepool {
        // timeZoneWithName: resolves IANA names and their links ("UTC", "US/Eastern", "EST" — all
        // tzdb zones, as the Win32 peer's locate_zone accepts them); nil for an unknown one.
        NSString* n = [NSString stringWithUTF8String:name.c_str()];
        NSTimeZone* tz = n ? [NSTimeZone timeZoneWithName:n] : nil;
        if (!tz) return false;
        *offsetSec = (int)[tz secondsFromGMTForDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)utc]];
        return true;
    }
}

}  // namespace rabbitears
