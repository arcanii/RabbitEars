// SPDX-License-Identifier: GPL-3.0-or-later
//
// PlaylistsDialog — the macOS "Manage Playlists" panel (Settings ▸ Manage
// Playlists…). Lists every imported playlist with an Enabled toggle and a Delete
// button. Disabling a playlist hides its channels from the grid + filters without
// removing them (Database::setPlaylistEnabled); Delete removes it and its channels
// after a confirmation. Mutations apply live and fire the onChange block so the
// main window can rebuild its filter menu + grid. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

namespace rabbitears { class Database; }

@interface PlaylistsDialog : NSObject
- (instancetype)initWithDatabase:(rabbitears::Database*)db;
// Present as a modal sheet on `parent`. `onChange` runs after each enable/disable/
// delete so the caller can re-sync the channel grid.
- (void)presentForWindow:(NSWindow*)parent onChange:(void (^)(void))onChange;
// Set Guide URL's "Download the guide now?" — Yes runs this (the host's Refresh Guide). nil = not asked.
@property (nonatomic, copy) void (^onRefreshGuide)(void);
@end
