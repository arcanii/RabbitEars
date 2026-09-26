// SPDX-License-Identifier: GPL-3.0-or-later
// GuideModel — the TV Guide's rows and coverage counts, built from the database. No window code, so
// the GUI (onEpgGuide, which hands the result to showEpgGuide) and the CLI (--guidebench, --selftest)
// run the same build.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace rabbitears {

class Database;

// The time window the host builds guide rows for, relative to "now" (buildGuideModel). Shared so the
// guide can tell whether rebuilding its rows could ever bring a search result into them.
constexpr long long kGuideWindowPastSec = 6 * 3600;    // a little history
constexpr long long kGuideWindowAheadSec = 72 * 3600;  // three days ahead

struct GuideProgramme {
    std::wstring title;
    std::wstring descr;         // shown when the block is clicked
    long long    startUtc = 0;  // unix epoch seconds (UTC); rendered in local time
    long long    stopUtc = 0;
};

struct GuideRow {
    std::wstring                channelId;    // tvg-id — resolves to a recordable stream (may be empty)
    std::wstring                channelName;
    std::vector<GuideProgramme> programmes;  // sorted by startUtc
    // Catch-up: the channel whose archive plays this row's past programmes — of the playlist's live
    // channels sharing the row's guide id, the one keeping the LONGEST archive, the first in
    // channelsByPlaylist's order on a tie (it need not be the channel the row is named after; the
    // search picks the same, Database::refreshProgrammeSearchChannels) — and how many days back it
    // goes. 0 = no archive.
    long long                   archiveChannel = 0;
    int                         archiveDays = 0;
    long long                   playlistId = 0;  // the playlist the row's channels are in
};

// A catch-up pick (GuideRow::archiveChannel / archiveDays) for one playlist's guide id.
struct GuideArchivePick {
    long long channel = 0;
    int       days = 0;
};
// Every playlist's picks, re-read from the database (liveGuideChannels, ~4 ms) — for a guide that is
// already open when the archive flags change (a provider sync): `pick(playlistId, tvgId)` with a row's
// or a search result's FULL tvg-id gives what buildGuideModel would now pick for it.
class GuideArchivePicks {
public:
    explicit GuideArchivePicks(Database& db);
    GuideArchivePick pick(long long playlistId, const std::wstring& tvgId) const;

private:
    struct Key {
        long long    playlistId;
        std::wstring base;  // the normalised guide id
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept;
    };
    std::unordered_map<Key, GuideArchivePick, KeyHash> picks_;
};

// What the toolbar's coverage line says — "Guide data for {shown} of {withId} channels with a guide
// ID" — and the explanation a click on it opens. Counted while building the rows (buildGuideModel),
// per enabled playlist — a channel in two playlists counts in each — and the parts add up:
// withId = shown + inNoLinkPlaylists + noProgrammes.
struct GuideCoverage {
    bool valid = false;         // false = no coverage line
    int  shown = 0;             // guide ids with a row (programmes in the window), per playlist
    int  withId = 0;            // the live channels' distinct guide ids (normalised tvg-ids)
    int  inNoLinkPlaylists = 0; // ids without a row, in playlists with no guide link
    int  noProgrammes = 0;      // ids without a row, in playlists that HAVE a guide link
    int  guideUnmatched = 0;    // the guide's channels (programmes in the window) matching NONE of
                                // the user's channels, in any playlist
};

struct GuideModel {
    std::vector<GuideRow> rows;         // sorted by channel name
    GuideCoverage         coverage;     // always valid
    std::wstring          coverageLog;  // the coverage counts in words, for the diag log
};

// The guide around `nowUtc` (kGuideWindowPastSec before it to kGuideWindowAheadSec after), over every
// ENABLED playlist. Programmes join to the playlist's LIVE channels by tvg-id, normalised ('@feed'
// suffix stripped, ASCII-lower-cased — iptv-org ids carry "@SD"/"@HD" feeds, XMLTV keys on the base);
// the first channel per id in channelsByPlaylist's order names the row, and its FULL tvg-id becomes
// the row's channelId (what Play and Schedule resolve). A guide channel matching none of the
// playlist's channels gets no row. Reads only, in several statements with no transaction of its own:
// if another connection stores a guide meanwhile, a playlist's rows may be from before it and
// another's from after.
GuideModel buildGuideModel(Database& db, long long nowUtc);

}  // namespace rabbitears
