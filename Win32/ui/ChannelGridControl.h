// SPDX-License-Identifier: GPL-3.0-or-later
// ChannelGridControl — a custom Direct2D owner-drawn grid for the channel list
// (# | ★ | Channel Name | Group). Windowed painting (only visible rows) keeps it
// smooth at 10k+ channels. Modeled on SQLTerminal-Win32's SqlGridControl.
//
// Column layout: [ # (LCN) | ★ favourite toggle | Channel Name | Group ].
// (A logo thumbnail column is planned once async logo fetching lands.)
#pragma once

#include <functional>
#include <vector>

#include <windows.h>

#include "models/Channel.h"

namespace rabbitears {

struct ChannelGridCallbacks {
    std::function<void(const Channel&)> onActivate;         // play this channel
    std::function<void(const Channel&)> onToggleFavourite;  // star toggled
    std::function<void(const Channel&)> onSetNumber;        // inline # (LCN) edited
    std::function<void(const Channel&, POINT screenPt)> onContextMenu;  // right-click a row -> menu
};

void registerChannelGridClass(HINSTANCE hInst);
HWND createChannelGrid(HWND parent, HINSTANCE hInst, int id, UINT dpi);

void channelGridSetChannels(HWND grid, std::vector<Channel> channels);
void channelGridSetFilter(HWND grid, const std::wstring& text);
void channelGridSetCallbacks(HWND grid, ChannelGridCallbacks cb);
void channelGridUpdateDpi(HWND grid, UINT dpi);
void channelGridApplyTheme(HWND grid);
void channelGridGetCounts(HWND grid, int* shown, int* total);
// Mark a channel id as now-playing (highlighted); pass 0 to clear.
void channelGridSetNowPlaying(HWND grid, long long channelId);
// Update a channel's dead/geolocked status (greys it out); no-op if not loaded.
void channelGridSetDeadStatus(HWND grid, long long channelId, DeadStatus status);

}  // namespace rabbitears
