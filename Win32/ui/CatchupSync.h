// SPDX-License-Identifier: GPL-3.0-or-later
// CatchupSync — the decisions "Sync movies from provider" makes about catch-up (ui/VodSync
// syncArchive), without the network or a window: which of a playlist's channels keep an archive,
// whether that answer may replace the stored flags, and which server time zone to keep. Compiled into
// the CLI too, so --selftest pins them.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/XtreamClient.h"  // XtreamArchive, XtreamCreds

namespace rabbitears {

// get_live_streams' archived streams matched to a playlist's live channels (`liveUrls` = (channel id,
// stream URL), Database::liveChannelUrls) by stream id — only URLs carrying the playlist's own login
// (`creds`): another shape in the same playlist is not stream N.
struct ArchiveMatch {
    std::vector<std::pair<long long, int>> flags;  // (channel id, days of archive)
    // False = do NOT replace the stored flags: the list named archived streams, yet NONE of the
    // playlist's URLs reads as a live stream of its login — a mismatch of spellings (a panel writing
    // the user name with '+' for a space in its paths, not "%20"), not evidence that every archive is
    // gone. When the URLs do read, the list's answer stands, "none of yours" included: the provider
    // may simply have dropped the archive on this playlist's channels.
    bool usable = true;
};
ArchiveMatch matchArchiveFlags(const std::vector<std::pair<long long, std::wstring>>& liveUrls,
                               const std::vector<XtreamArchive>& archives, const XtreamCreds& creds);

// Whether `zone` (IANA) agrees with the offset a probe measured at `utc` (xtreamServerUtcOffset) — its
// offset at `utc`, or up to two minutes either side of it (the probe's two readings can straddle a
// daylight-saving change) — and this PC's time-zone database knows it. False for "".
bool serverZoneAgrees(const std::wstring& zone, long long utc, int measuredOffsetSec);

// What to store in the setting archive_tz_<playlist> after an account probe — std::nullopt: leave it
// as it is. `probedZone` = the probe's zone ("" = none), `storedZone` = the setting now (nullopt = never
// stored), `utc` = the probe's timestamp, `measuredOffsetSec` = its measured offset (null = none).
// With a measurement: the probe's zone if it agrees; else the stored one if IT agrees (a panel naming
// a wrong zone, or none, does not undo a right one); else "" when there was a name to refuse — a panel
// set to "UTC" whose clock reads CEST would put every catch-up URL two hours off, so the measured
// offset decides then. Without one, the probe's name cannot be checked against a clock, so it is
// stored only where nothing usable was: never over a stored name this PC knows (an earlier probe kept
// it) nor over "" (one refused). A name this PC's time-zone database does not know is no use to
// playCatchup: probed where nothing usable is stored, it is stored as "" (which then keeps later
// unmeasured names out too); stored, it counts as nothing.
std::optional<std::wstring> serverZoneToStore(const std::wstring& probedZone,
                                              const std::optional<std::wstring>& storedZone, long long utc,
                                              const int* measuredOffsetSec);

}  // namespace rabbitears
