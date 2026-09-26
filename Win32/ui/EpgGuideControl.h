// SPDX-License-Identifier: GPL-3.0-or-later
// EpgGuideControl — a modeless top-level "TV guide" window: a channels×time grid
// (a frozen channel column on the left, a frozen hour axis on top, programme blocks
// laid out along the time axis). A custom Direct2D control mirroring the device/
// paint/scroll idioms of ChannelGridControl, but 2-D (horizontal time scroll + a
// vertical channel scroll). It is a pure renderer over the rows it is handed —
// the host builds them from the DB (buildGuideModel, ui/GuideModel.h).
#pragma once

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include "ui/GuideModel.h"  // GuideRow, GuideCoverage, the guide's time window

namespace rabbitears {

// One programme-search result as the guide lists it (the host converts Database::ProgrammeHit —
// docs/EPG_SEARCH.md). Marked text wraps each match in U+0002 … U+0003.
struct GuideSearchHit {
    std::wstring channelId;    // the channel's FULL tvg-id — the same id its GuideRow carries
    std::wstring channelName;
    std::wstring title, descr;
    std::wstring markedTitle;  // the title with the typed text marked; empty = show `title` plain
    std::wstring snippet;      // description-only matches: an excerpt with the words marked — or, when
                               // marking found nothing (a fold searchFold does not make), the
                               // description's start
    long long    startUtc = 0, stopUtc = 0;
    bool         inTitle = false;
    // Catch-up: the channel whose archive can play it, and how many days back it goes (0 = none) —
    // GuideRow::archiveChannel's counterpart. A hit that has already ended is found only with one (it
    // may lose it while listed: epgGuideUpdateArchive).
    long long    archiveChannel = 0;
    int          archiveDays = 0;
    long long    playlistId = 0;  // the playlist whose guide it is from (epgGuideUpdateArchive)
};

// Whether a programme can be played from an archive: it has started (aired, or is airing — "start
// over"), and the archive reaches back to its start. `archiveChannel` 0 = never.
bool guideCanPlayFromStart(long long archiveChannel, int archiveDays, long long startUtc, long long nowUtc);

// The coverage line itself ("Guide data for N of M channels with a guide ID"), and the explanation
// behind it: one paragraph per reason that applies (blank lines between), and the note that entries
// without a guide ID never appear. Both are also appended to the host's "No guide to show" notice.
std::wstring guideCoverageSummary(const GuideCoverage& c);
std::wstring guideCoverageExplanation(const GuideCoverage& c);

struct GuideCallbacks {
    // Clicking a programme opens a popup with Play / Schedule / Close. The host resolves
    // the channel (by tvg-id) and plays it / creates the schedule. Empty callbacks hide
    // the corresponding action.
    std::function<void(const std::wstring& channelId, const std::wstring& channelName,
                       const std::wstring& title, long long startUtc, long long stopUtc)>
        onSchedule;
    std::function<void(const std::wstring& channelId, const std::wstring& channelName)> onPlay;
    // "Play from the start" (catch-up): play the programme out of `archiveChannel`'s archive. Offered
    // only where guideCanPlayFromStart says so; empty -> never offered.
    std::function<void(long long archiveChannel, const std::wstring& title, long long startUtc, long long stopUtc)>
        onPlayFromStart;
    // "Record series": create a standing rule for every future airing of `title` on this
    // channel. Empty -> the popup's Record-series button does nothing.
    std::function<void(const std::wstring& channelId, const std::wstring& channelName,
                       const std::wstring& title)>
        onRecordSeries;
    // Right-click a channel row -> toggle it as a favourite (the host resolves the tvg-id to the
    // channel and flips its favourite flag). isFavourite reports the current state so the menu can
    // label "Add to" vs "Remove from" Favourites. Both empty -> no favourite action in the guide.
    std::function<void(const std::wstring& channelId, const std::wstring& channelName)> onToggleFavourite;
    std::function<bool(const std::wstring& channelId)> isFavourite;
    // Programme search, for the toolbar's search box (which also matches channel NAMES, itself, in the
    // guide's rows — the host is not involved). onSearchBegin runs once per search SESSION, before its
    // first search — a session starts when the box goes from empty to text, when
    // the guide's rows are (re)built, and when the guide is reopened: the host loads the channel set
    // and, if the index is stale and `mayRebuild`, rebuilds it (a second or two — the guide shows
    // "Preparing search…" first; the quiet re-search after new catch-up flags passes false, and the
    // search uses LIKE until the next session). onSearch returns the results for `text` as of `nowUtc`
    // (what has ended by then, the guide lists apart, after the rest) and sets *truncated when there
    // were more of those still to come, *truncatedPast when there were more that have ended. With no
    // onSearch, every search reports that nothing matches.
    std::function<void(bool mayRebuild)> onSearchBegin;
    std::function<std::vector<GuideSearchHit>(const std::wstring& text, long long nowUtc, bool* truncated,
                                              bool* truncatedPast)>
        onSearch;
    // How many distinct channel NAMES in the user's whole channel list match `text` (the main
    // window's channel search, live channels) with NO channel carrying one of `guideIds` — the
    // normalised ids of the guide's own rows, so another feed of a channel the guide shows (an FHD
    // beside the HD) is not counted — or -1 when that cannot be answered cheaply. Shown as "Also in
    // your channel list, not in the guide: N". Empty = no such note.
    std::function<int(const std::wstring& text, const std::unordered_set<std::wstring>& guideIds)>
        onCountChannelNames;
    // Rebuild the guide's rows from the database (re-entering showEpgGuide on the same window), for a
    // search result that is not in this window's rows. Tried only when it could help — the result lies
    // inside the window a rebuild covers (kGuideWindowPastSec/AheadSec around now) and the rows have not
    // been rebuilt since those results were fetched — e.g. the guide was refreshed since, or its rows
    // were built long enough ago that the window has moved on.
    std::function<void()> onRebuild;
};

// Open (or focus + refresh, if already open) the single modeless guide window over
// `owner`, populated with `rows` and marking "now" at `nowUtc`. Safe to call again to
// repopulate: a re-open shows the grid (not any search results; the search box keeps its text), with
// every channel (the new rows clear a channel filter), except the re-entry a search result's jump
// makes through GuideCallbacks::onRebuild, which keeps the results list and the channel filter.
// `rows` may be empty (the window shows an empty guide). `cb.onSchedule`, if set, adds a right-click
// "Schedule recording" action on programme blocks. `coverage` (when valid) is the toolbar's
// coverage line.
void showEpgGuide(HWND owner, HINSTANCE hInst, UINT dpi, std::vector<GuideRow> rows, long long nowUtc,
                  GuideCallbacks cb = {}, GuideCoverage coverage = {});

// Hide the guide window if it is open. The window is kept alive (not destroyed), so a later
// showEpgGuide re-reveals and repopulates it. Called when the host starts playing a channel
// from the guide, so the picked show isn't left playing behind the guide window.
void hideEpgGuide();

bool epgGuideOpen();  // true if the guide window exists (open or hidden)

// Re-apply the guide's chrome for a LIVE skin switch (Settings ▸ Theme) — the caption, border, scroll
// bars and search box — and repaint it (the grid and the results read the theme at paint time).
// No-op if the guide window doesn't exist (open or hidden).
void epgGuideRefreshTheme();
// Re-reveal an already-built guide WITHOUT re-querying the DB — instant reopen after a
// play-from-guide hid it. Moves the "now" line + airing highlight to `nowUtc` (the stored
// programmes don't change), shows the grid rather than any search results (the search box keeps its
// text, and a channel filter stays on — the rows are the same), and starts a fresh search session.
// A full rebuild (showEpgGuide via onEpgGuide) happens the first time, on an explicit reopen from the
// menu, and when a search result's jump needs it (GuideCallbacks::onRebuild). No-op if the guide
// isn't open.
void revealEpgGuide(long long nowUtc);

// Reveal the guide scrolled to a channel's row ("Show in TV Guide" from the channel grid):
// clears any channel filter, leaves any search results for the grid, top-aligns the row matching
// `tvgId` (matched on the normalised base id — '@feed' suffix stripped, case-folded — like the
// guide's own row join), and re-centres the time axis on `nowUtc`. Returns false — changing nothing
// — when the guide isn't built yet or the channel has no guide row; the caller decides what to tell
// the user. Build the guide first via the epgGuideOpen()/onEpgGuide pattern.
bool epgGuideShowChannel(const std::wstring& tvgId, long long nowUtc);

// Refresh the guide for a LIVE UI-language change: re-set the (translated) window caption, rebuild
// the cached Direct2D text formats and the search box's font so they pick up the new font family
// (the CJK UI faces differ from Segoe UI), re-size the box to it, re-set its cue banner and the
// results' headings, then repaint. The programme rows are unchanged, so this skips the DB rebuild
// showEpgGuide would do. No-op if the guide window doesn't exist (open or hidden).
void epgGuideRefreshLanguage();

// The catch-up flags changed while the guide exists, open or hidden (a provider sync, a playlist added
// or deleted — refreshArchiveMarkers): every row and every listed search result takes its new archive
// pick from `picks`, IN PLACE — no rebuild, so the scroll and the channel filter stay as they are, and
// nothing takes the focus — and the search session ends, so the next search re-reads the channel set
// (the search's own picks). A results list on screen searches again, quietly, ~0.2 s later (later still
// after a click on it: never between a double-click's two clicks), keeping the scroll and the selection
// — a selected programme no longer listed gives way to the first item if that is in view, else to
// nothing. Until then an aired result that lost its archive stays listed (without "Play from the
// start"), and one that gained it is not there yet. A popup or menu already open keeps what it showed
// (playCatchup checks again). No-op if the guide window doesn't exist.
void epgGuideUpdateArchive(const GuideArchivePicks& picks);

}  // namespace rabbitears
