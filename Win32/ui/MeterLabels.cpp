// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/MeterLabels.h"

#include <algorithm>
#include <vector>

#include "ui/Theme.h"
#include "ui/Tr.h"

namespace rabbitears {
namespace {

// The labels' face: the theme's body family (a Japanese / Chinese UI face in those languages —
// themeFontFamily), 10 px semibold. Made once per DPI and family rather than per paint — the skin strip
// repaints on every animation tick (a 16-ms timer) — and kept per DPI, so a bridge on a monitor of another
// scaling does not make the strip's font over again. UI thread only, as every caller is.
HFONT labelFont(UINT dpi) {
    struct Entry {
        UINT dpi;
        std::wstring family;
        HFONT font;
    };
    static std::vector<Entry> cache;
    const std::wstring family = themeFontFamily(FontRole::Body);
    for (Entry& e : cache)
        if (e.dpi == dpi) {
            if (e.family != family) {  // the language or the skin changed the face
                DeleteObject(e.font);
                e.font = themeFont(FontRole::Body, dpi, 10, FW_SEMIBOLD);
                e.family = family;
            }
            return e.font;
        }
    cache.push_back({dpi, family, themeFont(FontRole::Body, dpi, 10, FW_SEMIBOLD)});
    return cache.back().font;
}

}  // namespace

std::wstring meterLabelText(int slot) {
    using i18n::StringId;
    // Short names: a label must fit under a small meter ("Audio spectrum" / "Signal strength", the Meters
    // dialog's, do not under a Large needle meter). Bitrate and Data flow are the dialog's own.
    static constexpr StringId kNames[kMeterLabelSlots] = {
        StringId::MeterLabelSpectrum, StringId::MeterLabelSignal, StringId::MeterNameBitrate,
        StringId::MeterLabelFrames, StringId::MeterDataFlowCheckbox};
    return (slot >= 0 && slot < kMeterLabelSlots) ? tr(kNames[slot]) : std::wstring();
}

void paintMeterLabel(HDC dc, const RECT& cell, const std::wstring& text, UINT dpi) {
    if (!dc || text.empty() || cell.right <= cell.left || cell.bottom <= cell.top) return;
    // Capitals where the script has case — locale-neutral, so a render reads the same on every
    // machine; Japanese and Chinese pass through as they are.
    std::wstring caps(text.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.c_str(), static_cast<int>(text.size()),
                                caps.data(), static_cast<int>(caps.size()), nullptr, nullptr, 0);
    if (n > 0) caps.resize(static_cast<size_t>(n));
    else caps = text;

    const Theme& th = currentTheme();
    // The shadow falls below-right, away from the key light: dark on a dark strip, light on a light one.
    const int lum = (GetRValue(th.windowBg) * 30 + GetGValue(th.windowBg) * 59 + GetBValue(th.windowBg) * 11) / 100;
    const COLORREF shadow = lum < 128 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    const int px = std::max(1, MulDiv(1, static_cast<int>(dpi), 96));

    HGDIOBJ oldFont = SelectObject(dc, labelFont(dpi));
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = GetTextColor(dc);
    // Tracked out a little, like a panel legend — where that fits the cell. Else untracked, and cut with an
    // ellipsis if still too wide: DrawText's ellipsis does not allow for the tracking (it cut the "…" too).
    const int oldExtra = SetTextCharacterExtra(dc, px);
    SIZE ext{};
    GetTextExtentPoint32W(dc, caps.c_str(), static_cast<int>(caps.size()), &ext);
    UINT fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    if (ext.cx > cell.right - cell.left) {
        SetTextCharacterExtra(dc, 0);
        fmt |= DT_END_ELLIPSIS;
    }
    RECT r = cell;
    OffsetRect(&r, px, px);
    SetTextColor(dc, shadow);
    DrawTextW(dc, caps.c_str(), static_cast<int>(caps.size()), &r, fmt);
    r = cell;
    SetTextColor(dc, th.textMuted);
    DrawTextW(dc, caps.c_str(), static_cast<int>(caps.size()), &r, fmt);
    SetTextCharacterExtra(dc, oldExtra);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

}  // namespace rabbitears
