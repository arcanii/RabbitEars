// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/VideoGrid.h"

#include <algorithm>
#include <cmath>

namespace rabbitears {

std::vector<PaneBox> computeVideoPanes(ViewMode mode, int paneCount, int cx, int cy, int cw,
                                       int ch, const VideoGridOpts& o) {
    if (paneCount < 1) paneCount = 1;
    std::vector<PaneBox> out;
    out.reserve(static_cast<size_t>(paneCount));

    if (mode == ViewMode::Single || paneCount == 1) {
        out.push_back({cx, cy, cw, ch});
        for (int i = 1; i < paneCount; ++i) out.push_back({cx, cy, 0, 0});  // surplus -> hidden
        return out;
    }

    if (mode == ViewMode::Pip) {
        out.push_back({cx, cy, cw, ch});  // pane 0 fills the region
        const int pw = std::min(o.pipW, cw);
        const int ph = std::min(o.pipH, ch);
        int py = cy + ch - ph - o.pipMargin;  // bottom-right, stacking upward if >1 inset
        for (int i = 1; i < paneCount; ++i) {
            out.push_back({cx + cw - pw - o.pipMargin, py, std::max(0, pw), std::max(0, ph)});
            py -= ph + o.pipMargin;
        }
        return out;
    }

    // Split: a grid sized cols = ceil(sqrt(n)), rows = ceil(n/cols) (so 2->1x2, 4->2x2,
    // 9->3x3). The (cw+gap)*c/cols partition tiles the content exactly: cell c spans
    // [x0,x1] with a `gap` trimmed off the right, and the last column's x1 lands on
    // cx+cw regardless of rounding. Same in y.
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(paneCount))));
    if (cols < 1) cols = 1;
    const int rows = (paneCount + cols - 1) / cols;
    for (int i = 0; i < paneCount; ++i) {
        const int c = i % cols, r = i / cols;
        const int x0 = cx + (cw + o.gap) * c / cols;
        const int x1 = cx + (cw + o.gap) * (c + 1) / cols - o.gap;
        const int y0 = cy + (ch + o.gap) * r / rows;
        const int y1 = cy + (ch + o.gap) * (r + 1) / rows - o.gap;
        out.push_back({x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)});
    }
    return out;
}

}  // namespace rabbitears
