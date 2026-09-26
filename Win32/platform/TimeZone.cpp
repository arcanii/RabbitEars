// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/TimeZone.h"

#include <chrono>
#include <exception>

namespace rabbitears {

bool utcOffsetAt(const std::wstring& ianaZone, long long utc, int* offsetSec) {
    if (ianaZone.empty() || !offsetSec) return false;
    std::string name;  // IANA names are ASCII; anything else is not one
    for (wchar_t c : ianaZone) {
        if (c <= 0 || c > 0x7E) return false;
        name += static_cast<char>(c);
    }
    try {
        // locate_zone throws for an unknown name, and when the database cannot be loaded (no ICU).
        const std::chrono::time_zone* tz = std::chrono::locate_zone(name);
        const std::chrono::sys_info info = tz->get_info(std::chrono::sys_seconds{std::chrono::seconds{utc}});
        *offsetSec = static_cast<int>(info.offset.count());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace rabbitears
