// SPDX-License-Identifier: GPL-3.0-or-later
// EpgGuideControl — a modeless top-level "TV guide" window: a channels×time grid
// (a frozen channel column on the left, a frozen hour axis on top, programme blocks
// laid out along the time axis). A custom Direct2D control mirroring the device/
// paint/scroll idioms of ChannelGridControl, but 2-D (horizontal time scroll + a
// vertical channel scroll). It is a pure renderer over the rows it is handed —
// MainWindow assembles them from the DB (programmesInWindow joined to channels).
#pragma once

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
    std::wstring                channelName;
    std::vector<GuideProgramme> programmes;  // sorted by startUtc
};

// Open (or focus + refresh, if already open) the single modeless guide window over
// `owner`, populated with `rows` and marking "now" at `nowUtc`. Safe to call again to
// repopulate. `rows` may be empty (the window shows an empty guide).
void showEpgGuide(HWND owner, HINSTANCE hInst, UINT dpi, std::vector<GuideRow> rows, long long nowUtc);

}  // namespace rabbitears
