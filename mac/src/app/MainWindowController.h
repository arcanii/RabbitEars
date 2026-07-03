// SPDX-License-Identifier: GPL-3.0-or-later
//
// MainWindowController — the native macOS UI (the Phase-1 peer of the Win32
// src/ui/MainWindow). A split window: a channel list (NSTableView backed by the
// shared Database) on the left, a libVLC video surface (VlcPlayerMac) on the
// right, and a bar to load an M3U playlist by URL or file. Selecting a channel
// plays it. This is the MVP; the richer grid (favourites, LCN, search, country
// filters) + meters are follow-ups.
#pragma once

#import <Cocoa/Cocoa.h>

@interface MainWindowController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate>
- (void)showWindow;
@end
