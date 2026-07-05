// SPDX-License-Identifier: GPL-3.0-or-later
//
// MainWindowController — the native macOS UI (the Phase-1 peer of the Win32
// src/ui/MainWindow). A split window: a channel list (NSTableView backed by the
// shared Database) on the left, a libVLC video surface (VlcPlayerMac) on the
// right, and a top bar (+ Add Playlist / Settings / search + filter). Selecting a
// channel plays it. The richer grid (favourites, LCN, search, country filters) is
// in; audio meters are a follow-up.
#pragma once

#import <Cocoa/Cocoa.h>

@interface MainWindowController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate, NSSplitViewDelegate>
- (void)showWindow;

// View-menu chrome toggles — hide the channel list / the top toolbar so the video
// can fill the window. Driven from the app menu bar so they still work when the
// in-window toolbar is hidden. The getters back the menu titles (Hide ⇄ Show).
- (void)toggleChannelList;
- (void)toggleToolbar;
- (void)toggleVideoOnly;  // View ▸ Video Only — collapse all chrome to just the video
- (BOOL)channelListHidden;
- (BOOL)toolbarHidden;
- (BOOL)videoOnly;

// Command actions surfaced in BOTH the in-window Settings ▾ pull-down and the app
// menu bar (the menu-bar items forward here from AppDelegate).
- (void)addPlaylist:(id)sender;
- (void)openFile:(id)sender;
- (void)showPlaylists:(id)sender;
- (void)showMeters:(id)sender;
@end
