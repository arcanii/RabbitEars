// SPDX-License-Identifier: GPL-3.0-or-later
// MeterLabels — the optional labels under the meters (Settings ▸ Meters ▸ Meter labels, setting
// meter_labels, off by default): each meter's name printed on the strip under it, like a legend on a
// panel — in the row MeterTray.h keeps for them under an own row of meters (Large and up; the standard
// tray has no room), and under the meters of the meter bridge. Shared by the main window's strip, the
// bridge and RabbitEarsRender, so a render prints exactly what the app prints.
#pragma once

#include <string>

#include <windows.h>

namespace rabbitears {

// The tray's meters in label order: the four MiniMeter kinds (MeterKind order), then the buffer tank.
constexpr int kMeterLabelSlots = 5;
constexpr int kMeterLabelTank = 4;

// Slot `slot`'s label in the UI language — the meters' short names ("Spectrum", "Signal", "Bitrate",
// "Frames", "Data flow"), so one fits under a small meter; empty for a slot out of range.
std::wstring meterLabelText(int slot);

// Print `text` centred in `cell` (the row under a meter) in capitals, tracked out like a panel legend
// where that fits: the theme's muted text over a hairline shadow below-right (the key light is above-left —
// docs/PHOTOREAL.md); a label too wide for the cell is cut with an ellipsis. Transparent: the strip's
// material shows through. The DC's font, colours and modes are restored.
void paintMeterLabel(HDC dc, const RECT& cell, const std::wstring& text, UINT dpi);

}  // namespace rabbitears
