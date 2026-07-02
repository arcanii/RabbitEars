// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/DockLayout.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace rabbitears {

struct DockNode {
    bool   leaf = true;
    Panel  panel = Panel::Nav;   // when leaf
    bool   vertical = false;     // when split: true = stacked (top/bottom)
    double ratio = 0.5;          // when split: first child's fraction
    std::unique_ptr<DockNode> a, b;
};

namespace {

std::unique_ptr<DockNode> makeLeaf(Panel p) {
    auto n = std::make_unique<DockNode>();
    n->leaf = true;
    n->panel = p;
    return n;
}

std::unique_ptr<DockNode> makeSplit(bool vertical, double ratio, std::unique_ptr<DockNode> a,
                                    std::unique_ptr<DockNode> b) {
    auto n = std::make_unique<DockNode>();
    n->leaf = false;
    n->vertical = vertical;
    n->ratio = ratio;
    n->a = std::move(a);
    n->b = std::move(b);
    return n;
}

void serializeNode(const DockNode* n, std::wstring& out) {
    if (!n) return;
    if (n->leaf) {
        out += (n->panel == Panel::Nav) ? L'N' : (n->panel == Panel::Video) ? L'V' : L'G';
        return;
    }
    wchar_t r[16];
    swprintf_s(r, L"%.3f", n->ratio);
    out += n->vertical ? L'-' : L'|';
    out += r;
    out += L'(';
    serializeNode(n->a.get(), out);
    out += L',';
    serializeNode(n->b.get(), out);
    out += L')';
}

// Recursive-descent parse; sets ok=false on any malformed input.
std::unique_ptr<DockNode> parseNode(const std::wstring& s, size_t& i, bool& ok) {
    if (!ok || i >= s.size()) {
        ok = false;
        return nullptr;
    }
    const wchar_t c = s[i];
    if (c == L'N' || c == L'V' || c == L'G') {
        ++i;
        return makeLeaf(c == L'N' ? Panel::Nav : c == L'V' ? Panel::Video : Panel::Grid);
    }
    if (c == L'|' || c == L'-') {
        const bool vertical = (c == L'-');
        ++i;
        const size_t start = i;
        while (i < s.size() && (iswdigit(s[i]) || s[i] == L'.')) ++i;
        if (i == start || i >= s.size() || s[i] != L'(') {
            ok = false;
            return nullptr;
        }
        const double ratio = _wtof(s.substr(start, i - start).c_str());
        ++i;  // '('
        auto a = parseNode(s, i, ok);
        if (!ok || i >= s.size() || s[i] != L',') {
            ok = false;
            return nullptr;
        }
        ++i;  // ','
        auto b = parseNode(s, i, ok);
        if (!ok || i >= s.size() || s[i] != L')') {
            ok = false;
            return nullptr;
        }
        ++i;  // ')'
        return makeSplit(vertical, ratio, std::move(a), std::move(b));
    }
    ok = false;
    return nullptr;
}

void countPanels(const DockNode* n, int counts[kPanelCount]) {
    if (!n) return;
    if (n->leaf) {
        counts[static_cast<int>(n->panel)]++;
        return;
    }
    countPanels(n->a.get(), counts);
    countPanels(n->b.get(), counts);
}

void computeNode(const DockNode* n, RECT rect, int gutterPx, int minPx, RECT rects[kPanelCount],
                 std::vector<DockLayout::Gutter>& gutters) {
    if (!n) return;
    if (n->leaf) {
        rects[static_cast<int>(n->panel)] = rect;
        return;
    }
    if (n->vertical) {
        const int total = rect.bottom - rect.top;
        const int avail = std::max(0, total - gutterPx);
        const int lo = std::min(minPx, avail), hi = std::max(lo, avail - minPx);
        const int aH = std::clamp(static_cast<int>(avail * n->ratio + 0.5), lo, hi);
        RECT ra{rect.left, rect.top, rect.right, rect.top + aH};
        RECT rg{rect.left, rect.top + aH, rect.right, rect.top + aH + gutterPx};
        RECT rb{rect.left, rect.top + aH + gutterPx, rect.right, rect.bottom};
        gutters.push_back({rg, rect, const_cast<DockNode*>(n), true});
        computeNode(n->a.get(), ra, gutterPx, minPx, rects, gutters);
        computeNode(n->b.get(), rb, gutterPx, minPx, rects, gutters);
    } else {
        const int total = rect.right - rect.left;
        const int avail = std::max(0, total - gutterPx);
        const int lo = std::min(minPx, avail), hi = std::max(lo, avail - minPx);
        const int aW = std::clamp(static_cast<int>(avail * n->ratio + 0.5), lo, hi);
        RECT ra{rect.left, rect.top, rect.left + aW, rect.bottom};
        RECT rg{rect.left + aW, rect.top, rect.left + aW + gutterPx, rect.bottom};
        RECT rb{rect.left + aW + gutterPx, rect.top, rect.right, rect.bottom};
        gutters.push_back({rg, rect, const_cast<DockNode*>(n), false});
        computeNode(n->a.get(), ra, gutterPx, minPx, rects, gutters);
        computeNode(n->b.get(), rb, gutterPx, minPx, rects, gutters);
    }
}

// Remove the leaf for `p`, collapsing its parent split into the sibling. Returns the
// extracted leaf, or nullptr if not found in this subtree.
std::unique_ptr<DockNode> removePanel(std::unique_ptr<DockNode>& slot, Panel p) {
    if (!slot || slot->leaf) return nullptr;
    if (slot->a && slot->a->leaf && slot->a->panel == p) {
        auto extracted = std::move(slot->a);
        slot = std::move(slot->b);  // collapse split -> surviving sibling
        return extracted;
    }
    if (slot->b && slot->b->leaf && slot->b->panel == p) {
        auto extracted = std::move(slot->b);
        slot = std::move(slot->a);
        return extracted;
    }
    if (auto e = removePanel(slot->a, p)) return e;
    return removePanel(slot->b, p);
}

// Wrap the leaf for `target` in a new split with `movingNode` on `side`. `movingNode`
// is consumed only when the target is found.
bool wrapTarget(std::unique_ptr<DockNode>& slot, Panel target, DockSide side,
                std::unique_ptr<DockNode>& movingNode) {
    if (!slot) return false;
    if (slot->leaf) {
        if (slot->panel != target) return false;
        const bool vertical = (side == DockSide::Top || side == DockSide::Bottom);
        const bool movingFirst = (side == DockSide::Left || side == DockSide::Top);
        auto targetNode = std::move(slot);
        slot = movingFirst
                   ? makeSplit(vertical, 0.5, std::move(movingNode), std::move(targetNode))
                   : makeSplit(vertical, 0.5, std::move(targetNode), std::move(movingNode));
        return true;
    }
    if (wrapTarget(slot->a, target, side, movingNode)) return true;
    return wrapTarget(slot->b, target, side, movingNode);
}

}  // namespace

DockLayout::DockLayout() = default;
DockLayout::~DockLayout() = default;
DockLayout::DockLayout(DockLayout&&) noexcept = default;
DockLayout& DockLayout::operator=(DockLayout&&) noexcept = default;

DockLayout DockLayout::makeDefault() {
    DockLayout d;
    d.root_ = makeSplit(false, 0.220, makeLeaf(Panel::Nav),
                        makeSplit(true, 0.600, makeLeaf(Panel::Video), makeLeaf(Panel::Grid)));
    return d;
}

DockLayout DockLayout::parse(const std::wstring& s) {
    size_t i = 0;
    bool ok = true;
    auto root = parseNode(s, i, ok);
    if (!ok || i != s.size() || !root) return makeDefault();
    int counts[kPanelCount] = {0, 0, 0};
    countPanels(root.get(), counts);
    for (int k = 0; k < kPanelCount; ++k)
        if (counts[k] != 1) return makeDefault();  // exactly one of each panel required
    DockLayout d;
    d.root_ = std::move(root);
    return d;
}

std::wstring DockLayout::serialize() const {
    std::wstring out;
    serializeNode(root_.get(), out);
    return out;
}

void DockLayout::computeRects(const RECT& content, int gutterPx, int minPx, RECT rects[kPanelCount],
                              std::vector<Gutter>& gutters) const {
    for (int k = 0; k < kPanelCount; ++k) rects[k] = RECT{0, 0, 0, 0};
    gutters.clear();
    computeNode(root_.get(), content, gutterPx, minPx, rects, gutters);
}

void DockLayout::dock(Panel moving, DockSide side, Panel target) {
    if (moving == target || !root_) return;
    auto extracted = removePanel(root_, moving);
    if (!extracted) return;
    if (!wrapTarget(root_, target, side, extracted))
        root_ = makeSplit(false, 0.5, std::move(extracted), std::move(root_));  // never drop a panel
}

void DockLayout::setRatio(DockNode* node, double ratio) {
    if (node && !node->leaf) node->ratio = std::clamp(ratio, 0.05, 0.95);
}

}  // namespace rabbitears
