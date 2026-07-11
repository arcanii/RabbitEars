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

// Outcome of a "Show in TV Guide" request (see -presentRelativeTo:showChannel:onPlay:).
typedef NS_ENUM(NSInteger, REGuideShowResult) {
    REGuideShowRevealed,       // guide is open and scrolled to the requested channel
    REGuideShowChannelMissing, // guide is open, but the requested channel has no row
    REGuideShowNoGuide,        // nothing to show — the controller already alerted the user
};

@interface TvGuideWindowController : NSObject
- (instancetype)initWithDatabase:(rabbitears::Database*)db;
// Recording actions offered on a programme's detail dialog (set once by the host). onSchedule
// queues a one-off recording of this airing; onRecordSeries adds a rule for its title+channel.
@property (nonatomic, copy) void (^onSchedule)(NSString* tvgId, NSString* channelName,
                                               NSString* title, long long startUtc, long long stopUtc);
@property (nonatomic, copy) void (^onRecordSeries)(NSString* tvgId, NSString* channelName, NSString* title);
// Build the guide from the DB and show the window over `parent`. `onPlay` fires with a
// channel's full tvg-id (+ display name) when the user picks Play on a programme. If no
// stored programmes match a playlist channel, shows an explanatory alert instead.
- (void)presentRelativeTo:(NSWindow*)parent
                   onPlay:(void (^)(NSString* tvgId, NSString* channelName))onPlay;
// Present the guide (same as above) and scroll+highlight the row for `tvgId` — the "Show in
// TV Guide" row action. The result tells the caller what happened: Revealed (done),
// ChannelMissing (guide shown, caller should explain this channel isn't in it), or NoGuide
// (the controller already showed the standard "no guide" alert — caller stays quiet, so the
// user never sees two alerts). Matching uses the same lowercased-base tvg-id rule as the
// programme→channel join.
- (REGuideShowResult)presentRelativeTo:(NSWindow*)parent
                           showChannel:(NSString*)tvgId
                                onPlay:(void (^)(NSString* tvgId, NSString* channelName))onPlay;
- (void)hide;  // order the window out (kept alive for a later re-open)
@end
