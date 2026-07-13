// SPDX-License-Identifier: GPL-3.0-or-later
// EpgGuideControl — a modeless top-level "TV guide" window: a channels×time grid
// (a frozen channel column on the left, a frozen hour axis on top, programme blocks
// laid out along the time axis). A custom Direct2D control mirroring the device/
// paint/scroll idioms of ChannelGridControl, but 2-D (horizontal time scroll + a
// vertical channel scroll). It is a pure renderer over the rows it is handed —
// MainWindow assembles them from the DB (programmesInWindow joined to channels).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace rabbitears {

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

struct GuideCallbacks {
    // Clicking a programme opens a popup with Play / Schedule / Close. The host resolves
    // the channel (by tvg-id) and plays it / creates the schedule. Empty callbacks hide
    // the corresponding action.
    std::function<void(const std::wstring& channelId, const std::wstring& channelName,
                       const std::wstring& title, long long startUtc, long long stopUtc)>
        onSchedule;
    std::function<void(const std::wstring& channelId, const std::wstring& channelName)> onPlay;
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
};

// Open (or focus + refresh, if already open) the single modeless guide window over
// `owner`, populated with `rows` and marking "now" at `nowUtc`. Safe to call again to
// repopulate. `rows` may be empty (the window shows an empty guide). `cb.onSchedule`, if
// set, adds a right-click "Schedule recording" action on programme blocks.
void showEpgGuide(HWND owner, HINSTANCE hInst, UINT dpi, std::vector<GuideRow> rows, long long nowUtc,
                  GuideCallbacks cb = {});

// Hide the guide window if it is open. The window is kept alive (not destroyed), so a later
// showEpgGuide re-reveals and repopulates it. Called when the host starts playing a channel
// from the guide, so the picked show isn't left playing behind the guide window.
void hideEpgGuide();

bool epgGuideOpen();  // true if the guide window exists (open or hidden)
// Re-reveal an already-built guide WITHOUT re-querying the DB — instant reopen after a
// play-from-guide hid it. Only moves the "now" line + airing highlight to `nowUtc` (the stored
// programmes don't change); a full rebuild (showEpgGuide via onEpgGuide) is only needed the first
// time or on an explicit Refresh. No-op if the guide isn't open.
void revealEpgGuide(long long nowUtc);

// Reveal the guide scrolled to a channel's row ("Show in TV Guide" from the channel grid):
// clears any type-to-search filter, top-aligns the row matching `tvgId` (matched on the
// normalised base id — '@feed' suffix stripped, case-folded — like the guide's own row
// join), and re-centres the time axis on `nowUtc`. Returns false (without revealing) when
// the guide isn't built yet or the channel has no guide row — the caller decides what to
// tell the user. Build the guide first via the epgGuideOpen()/onEpgGuide pattern.
bool epgGuideShowChannel(const std::wstring& tvgId, long long nowUtc);

// Refresh the guide for a LIVE UI-language change: re-set the (translated) window caption and
// rebuild the cached Direct2D text formats so they pick up the new font family (the CJK UI faces
// differ from Segoe UI), then repaint. The programme rows are unchanged, so this skips the DB
// rebuild showEpgGuide would do. No-op if the guide window doesn't exist (open or hidden).
void epgGuideRefreshLanguage();

}  // namespace rabbitears
