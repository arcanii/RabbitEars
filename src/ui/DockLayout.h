// SPDX-License-Identifier: GPL-3.0-or-later
// DockLayout — a tiny binary split-tree describing where the three main regions
// (nav sidebar, video+transport, channel grid) sit, so the user can rearrange them
// and save named layouts. This is PURE geometry/logic with no HWNDs, so it is
// unit-tested headlessly by RabbitEarsCli (--selftest) even though the GUI can't be.
//
// Serialized form (compact, human-readable): leaves are 'N' (Nav), 'V' (Video),
// 'G' (Grid); splits are '|<ratio>(a,b)' (side-by-side, left=a) or '-<ratio>(a,b)'
// (stacked, top=a). E.g. the default:  |0.220(N,-0.600(V,G))
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace rabbitears {

enum class Panel { Nav = 0, Video = 1, Grid = 2 };
inline constexpr int kPanelCount = 3;

enum class DockSide { Left, Right, Top, Bottom };

struct DockNode;  // opaque; defined in the .cpp. Used as a stable handle for gutter drags.

class DockLayout {
public:
    DockLayout();
    ~DockLayout();
    DockLayout(DockLayout&&) noexcept;
    DockLayout& operator=(DockLayout&&) noexcept;
    DockLayout(const DockLayout&) = delete;
    DockLayout& operator=(const DockLayout&) = delete;

    // The classic arrangement: Nav on the left, Video over Grid on the right.
    static DockLayout makeDefault();
    // Parse a serialized layout; returns makeDefault() if the string is malformed or
    // does not contain exactly the three panels once each.
    static DockLayout parse(const std::wstring& s);
    std::wstring serialize() const;

    // A draggable divider between two siblings. `node` is a stable handle to pass to
    // setRatio(); `vertical` = the split stacks top/bottom (so the gutter is a
    // horizontal bar the user drags up/down).
    struct Gutter {
        RECT      rc;        // the draggable divider rect
        RECT      nodeRect;  // the full rect this split occupies (for ratio math on drag)
        DockNode* node;
        bool      vertical;  // split stacks top/bottom (gutter is a horizontal bar)
    };

    // Lay the tree out inside `content`, leaving `gutterPx` between siblings and never
    // letting a panel get smaller than `minPx`. Fills rects[(int)panel] for each panel
    // and appends the divider gutters.
    void computeRects(const RECT& content, int gutterPx, int minPx,
                      RECT rects[kPanelCount], std::vector<Gutter>& gutters) const;

    // Move `moving` so it docks on `side` of `target` (no-op if moving == target).
    void dock(Panel moving, DockSide side, Panel target);

    // Set a split node's first-child fraction (0..1), clamped away from 0/1. `node`
    // comes from a Gutter; valid until the next dock() reshapes the tree.
    void setRatio(DockNode* node, double ratio);

    bool valid() const { return root_ != nullptr; }

private:
    std::unique_ptr<DockNode> root_;
};

}  // namespace rabbitears
