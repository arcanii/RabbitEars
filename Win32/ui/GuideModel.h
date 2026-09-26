// SPDX-License-Identifier: GPL-3.0-or-later
// GuideModel — the TV Guide's rows and coverage counts, built from the database. No window code, so
// the GUI (onEpgGuide, which hands the result to showEpgGuide) and the CLI (--guidebench, --selftest)
// run the same build.
#pragma once

#include <string>
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
