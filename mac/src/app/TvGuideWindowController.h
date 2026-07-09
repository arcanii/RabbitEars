// SPDX-License-Identifier: GPL-3.0-or-later
//
// TvGuideWindowController — hosts the TV Guide in its own modeless window (the mac peer
// of Win32's showEpgGuide). It queries the shared Database for stored programmes, assembles
// them into channel rows (joining programmes to channels by tvg-id, exactly as the Win32
// onEpgGuide does), and hands them to an EpgGuideView. A clicked programme offers Play,
// which calls back to the host (MainWindowController) to resolve + play the channel.
// Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

namespace rabbitears { class Database; }

@interface TvGuideWindowController : NSObject
- (instancetype)initWithDatabase:(rabbitears::Database*)db;
// Build the guide from the DB and show the window over `parent`. `onPlay` fires with a
// channel's full tvg-id (+ display name) when the user picks Play on a programme. If no
// stored programmes match a playlist channel, shows an explanatory alert instead.
- (void)presentRelativeTo:(NSWindow*)parent
                   onPlay:(void (^)(NSString* tvgId, NSString* channelName))onPlay;
- (void)hide;  // order the window out (kept alive for a later re-open)
@end
