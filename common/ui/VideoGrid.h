// SPDX-License-Identifier: GPL-3.0-or-later
// VideoGrid — pure geometry for the multi-player video area: given a view mode and a
// pane count, compute where each video pane sits inside the video region. Like
// DockLayout this is PURE math with no HWNDs, so it is unit-tested headlessly by
// RabbitEarsCli (--selftest) even though the GUI can't be, and it is shared with the
// macOS renderer (the split/PIP arithmetic is identical across platforms; only the
// window/surface plumbing differs).
#pragma once

#include <vector>

namespace rabbitears {

// How the video region is divided across live players.
//   Single — one pane fills the region (the classic single-player view).
//   Split  — an NxN-ish grid, one cell per pane (multiple simultaneous views).
//   Pip    — pane 0 fills the region; the rest are small insets in the corner.
enum class ViewMode { Single = 0, Split = 1, Pip = 2 };

// A pane rectangle in position+size form (what SetWindowPos/DeferWindowPos want).
struct PaneBox {
    int x = 0, y = 0, w = 0, h = 0;
};

// Tunables: the gap between split panes and the size/margin of a PIP inset.
struct VideoGridOpts {
    int gap = 0;        // space left between adjacent split panes (px)
    int pipW = 0;       // PIP inset width (px)
    int pipH = 0;       // PIP inset height (px)
    int pipMargin = 0;  // PIP inset offset from the region's bottom-right corner (px)
};

// Compute one box per pane (exactly `paneCount`, clamped to >= 1) for `mode` laid out
// inside the content box (cx, cy, cw, ch). Split tiles the content exactly (the last
// row/column lands on the content's far edge — no rounding seams). Pip returns pane 0
// full then corner insets stacked upward. A zero-area box marks a pane the caller
// should hide (e.g. surplus panes in Single).
std::vector<PaneBox> computeVideoPanes(ViewMode mode, int paneCount, int cx, int cy, int cw,
                                       int ch, const VideoGridOpts& opts);

}  // namespace rabbitears
