// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeterBridge — the pop-out meter bridge (Settings ▸ Meters ▸ Meter bridge window): the tray's meters and
// the buffer tank again, as large as the window they are in — the size at which the photoreal detail
// (the VU dials' numerals, LED lenses) exists at all. The owner's decision (PHOTOREAL.md, 2026-09-25):
// all three routes to bigger meters — a taller tray, a resizable tray, and this.
//
// Its meters are MIRRORS of the tray's (miniMeterSetMirror / bufferMeterSetMirror): whatever feeds a tray
// meter — data, resets, a new look, palette or tuning from the Meters dialog — reaches its twin here too,
// so nothing that feeds the meters had to learn about this window. Which meters it shows follows the
// tray's own on/off settings; the window's place and size are remembered (meter_bridge_rect), and so is
// whether it was open (meter_bridge) — it reopens at the next launch.
#pragma once

#include <windows.h>

namespace rabbitears {
namespace mw {

struct AppState;

void toggleMeterBridge(AppState* st);      // open / close, remembering the choice
bool meterBridgeOpen();
void restoreMeterBridge(AppState* st);     // at startup: reopen it if it was open when the app last closed
void meterBridgeRelayout(AppState* st);    // the tray's meters were switched on/off (Meters dialog)
void meterBridgeRefreshTheme();            // a skin switch: frame + background
void meterBridgeRefreshLanguage();         // a UI-language switch: the caption

}  // namespace mw
}  // namespace rabbitears
