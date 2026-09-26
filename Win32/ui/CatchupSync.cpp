// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/CatchupSync.h"

#include <ctime>
#include <unordered_map>

#include "platform/TimeZone.h"

namespace rabbitears {

ArchiveMatch matchArchiveFlags(const std::vector<std::pair<long long, std::wstring>>& liveUrls,
                               const std::vector<XtreamArchive>& archives, const XtreamCreds& creds) {
    ArchiveMatch m;
    std::unordered_map<long long, int> daysByStream;
    for (const XtreamArchive& a : archives)
        if (a.streamId > 0 && a.days > 0) daysByStream[a.streamId] = a.days;
    bool anyRead = false;  // a URL that reads as a live stream of this login
    for (const auto& [id, url] : liveUrls) {
        const long long sid = xtreamLiveStreamId(url, &creds);  // 0 = not a live URL with this login
        if (sid <= 0) continue;
        anyRead = true;
        const auto it = daysByStream.find(sid);
        if (it != daysByStream.end()) m.flags.emplace_back(id, it->second);
    }
    m.usable = daysByStream.empty() || anyRead;
    return m;
}

bool serverZoneAgrees(const std::wstring& zone, long long utc, int measuredOffsetSec) {
    for (long long at : {utc, utc - 120, utc + 120}) {
        int zoneOffset = 0;
        if (!utcOffsetAt(zone, at, &zoneOffset)) return false;  // "", or unknown here
        if (zoneOffset == measuredOffsetSec) return true;
    }
    return false;
}

std::optional<std::wstring> serverZoneToStore(const std::wstring& probedZone,
                                              const std::optional<std::wstring>& storedZone, long long utc,
                                              const int* measuredOffsetSec) {
    if (measuredOffsetSec) {
        if (!probedZone.empty() && serverZoneAgrees(probedZone, utc, *measuredOffsetSec)) return probedZone;
        // A probe naming a wrong zone (or none) does not undo a stored one the clock still agrees with.
        if (storedZone && !storedZone->empty() && serverZoneAgrees(*storedZone, utc, *measuredOffsetSec))
            return *storedZone;
        if (probedZone.empty() && storedZone.value_or(L"").empty()) return std::nullopt;  // nothing to refuse
        return std::wstring();
    }
    // Whether this PC's time-zone database knows a name — asked at the current time, not at the probe's
    // timestamp, which nothing checked here (defensive: a date past the database's range could make
    // every name "unknown"; MSVC's answers even at a microsecond stamp). A stored name it does not know
    // (an older build stored names unchecked) counts as none: playCatchup cannot use it either.
    const long long now = static_cast<long long>(std::time(nullptr));
    int unused = 0;
    const bool storedUsable = storedZone && (storedZone->empty() || utcOffsetAt(*storedZone, now, &unused));
    if (probedZone.empty() || storedUsable) return std::nullopt;
    return utcOffsetAt(probedZone, now, &unused) ? probedZone : std::wstring();  // unknown here: no use
}

}  // namespace rabbitears
