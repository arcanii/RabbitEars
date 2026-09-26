// SPDX-License-Identifier: GPL-3.0-or-later
// MeterTray — the transport strip's meter geometry, in one place for the app (MainWindowChrome
// layout(), the strip paints, the top-edge drag, the meter bridge), for RabbitEarsRender, which
// composites the tray at exactly these positions, and for --selftest. Pure arithmetic, header-only.
//
// ONE meter height (setting `meter_height`, dp) sizes the tray (Win32/docs/PHOTOREAL.md, stage A). At the
// standard 30 dp the meters sit INLINE, right of the transport controls, in the strip's one 50-dp row —
// every number here then is the one the tray has always used. Taller, they get a ROW OF THEIR OWN above
// the transport row, the video panel's whole width (the owner's choice, 2026-09-26): beside the controls,
// most of them would not fit. Every meter's width and the tank's scale with the height from their 30-dp
// design widths.
#pragma once

#include <algorithm>

#include <windows.h>

namespace rabbitears {

// Meter heights, dp. The presets are Settings ▸ Meters ▸ Standard / Large / Extra large; the drag on
// the strip's top edge sets any height from kMeterHeightMin to kMeterHeightMax.
constexpr int kMeterHeightStd = 30;
constexpr int kMeterHeightLarge = 50;
constexpr int kMeterHeightXLarge = 72;
constexpr int kMeterHeightMin = kMeterHeightStd;
constexpr int kMeterHeightMax = 120;

// The meters' widths at the standard 30-dp height (96-dpi design values), in MeterKind order —
// Spectrum, Signal, Bitrate, Frames — and the buffer tank's.
constexpr int kTrayMeterW96[4] = {112, 58, 96, 72};
constexpr int kTrayTankW96 = 115;

constexpr int kStripMinDp = 50;     // the transport row — and the whole strip at the standard height
constexpr int kMeterRowTopDp = 10;  // own row: the space above the meters (the transport row's own
                                    // 10 dp above its buttons separates them below)

inline int trayDp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

inline int clampMeterHeightDp(int h) { return std::clamp(h, kMeterHeightMin, kMeterHeightMax); }

// A tray width (`w96`: its 96-dpi width at the standard height) scaled with the meters' height.
inline int trayWidth(int w96, int meterPx, UINT dpi) {
    return MulDiv(trayDp(w96, dpi), meterPx, trayDp(kMeterHeightStd, dpi));
}

struct StripMetrics {
    int  stripPx = 0;     // the whole strip
    int  meterPx = 0;     // the meters' height
    bool ownRow = false;  // the meters in a row of their own above the transport row
};

// The standard strip: 50 dp, the meters 30 dp and inline.
inline StripMetrics standardStrip(UINT dpi) { return {trayDp(kStripMinDp, dpi), trayDp(kMeterHeightStd, dpi), false}; }

// Own row: the strip's height around meters `meterPx` tall.
inline int ownRowStripPx(int meterPx, UINT dpi) {
    return trayDp(kMeterRowTopDp, dpi) + meterPx + trayDp(kStripMinDp, dpi);
}

// The tallest meters (px) the limits allow in an own row: `maxStripPx` > 0 caps the strip (half the
// video panel — the video keeps the rest), `maxRowPx` > 0 is the row's width, which the tank (always
// shown) must fit. 0 = no limit (INT_MAX-like).
inline int ownRowMeterLimitPx(UINT dpi, int maxStripPx, int maxRowPx) {
    int limit = 1 << 30;
    if (maxStripPx > 0) limit = std::min(limit, maxStripPx - ownRowStripPx(0, dpi));
    if (maxRowPx > 0)  // the tank's width at h is ~ w115 * h / w30: floor, so it never exceeds the row
        limit = std::min(limit, maxRowPx * trayDp(kMeterHeightStd, dpi) / trayDp(kTrayTankW96, dpi));
    return limit;
}

// The tallest meter height (dp) whose own-row strip fits the limits whole (for the edge drag's range);
// kMeterHeightStd when not even one taller than the standard fits.
inline int maxMeterHeightDp(UINT dpi, int maxStripPx, int maxRowPx) {
    const int limit = ownRowMeterLimitPx(dpi, maxStripPx, maxRowPx);
    if (limit <= trayDp(kMeterHeightStd, dpi)) return kMeterHeightStd;
    int d = std::min(kMeterHeightMax, MulDiv(limit, 96, static_cast<int>(dpi)));
    while (d > kMeterHeightStd && trayDp(d, dpi) > limit) --d;
    return std::max(d, kMeterHeightStd);
}

// The strip for a meter height of `meterDp` at `dpi`, within the limits above — capped to a whole-dp
// height (maxMeterHeightDp), so what is shown is always a height the edge drag can reach and keep. When
// the limits leave no room for meters taller than the standard ones, it is the standard strip (so a
// small window keeps the tray it always had).
inline StripMetrics stripMetrics(int meterDp, UINT dpi, int maxStripPx = 0, int maxRowPx = 0) {
    meterDp = clampMeterHeightDp(meterDp);
    const StripMetrics standard = standardStrip(dpi);
    if (meterDp <= kMeterHeightStd) return standard;
    const int d = std::min(meterDp, maxMeterHeightDp(dpi, maxStripPx, maxRowPx));
    if (d <= kMeterHeightStd) return standard;
    const int meterPx = trayDp(d, dpi);
    return {ownRowStripPx(meterPx, dpi), meterPx, true};
}

// The edge drag: the meter height for a strip `stripPx` tall — the standard one until the strip is tall
// enough for an own row of meters taller than it (so the edge stays put, then follows the cursor).
inline int meterHeightForStripPx(int stripPx, UINT dpi) {
    const int meterPx = stripPx - ownRowStripPx(0, dpi);
    if (meterPx <= trayDp(kMeterHeightStd, dpi)) return kMeterHeightStd;
    return clampMeterHeightDp(MulDiv(meterPx, 96, static_cast<int>(dpi)));
}

// ...with hysteresis where the layout switches (the strip jumps ~41 dp between the standard strip and the
// smallest own row): from the standard strip (`ownRowNow` false) the cursor must go kDragHysteresisDp
// past the smallest own-row strip to switch, and from an own row as far below it to switch back — so a
// cursor jittering on the boundary does not flip the whole layout every pixel.
constexpr int kDragHysteresisDp = 6;
inline int meterHeightForDrag(int stripPx, bool ownRowNow, UINT dpi) {
    const int enterPx = ownRowStripPx(trayDp(kMeterHeightStd + 1, dpi), dpi);  // the smallest own-row strip
    const int hyst = trayDp(kDragHysteresisDp, dpi);
    if (ownRowNow ? stripPx < enterPx - hyst : stripPx < enterPx + hyst) return kMeterHeightStd;
    return std::max(kMeterHeightStd + 1, meterHeightForStripPx(stripPx, dpi));
}

}  // namespace rabbitears
