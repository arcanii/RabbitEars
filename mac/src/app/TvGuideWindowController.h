// SPDX-License-Identifier: GPL-3.0-or-later
//
// TvGuideWindowController — hosts the TV Guide in its own modeless window (the mac peer of Win32's
// showEpgGuide / EpgGuideControl). Its rows come from the SHARED build, buildGuideModel
// (common/ui/GuideModel.h — the LIVE channels carrying a guide id, joined to the stored programmes
// by normalised tvg-id, exactly as Win32's guide is built), and are drawn by an EpgGuideView.
//
// Above the grid, a toolbar: one search box for channels AND programmes (the shared
// Database::searchProgrammes — FTS5, docs/EPG_SEARCH.md — and the guide's own channel names), a
// Now button, and the coverage line ("Guide data for N of M channels with a guide ID"; a click
// explains the rest). While the box has text, a results list takes the grid's place: the matching
// channels (one item narrows the grid to them — the corner chip clears it), how many more the
// channel list has that the guide doesn't, then the programmes by day, the description-only ones,
// and the aired ones a channel's archive can still play (catch-up). A click on a programme shows
// it in the grid; a double-click (or Return) opens it.
//
// Clicking a programme in the grid opens its details with Play / Play from the start (catch-up) /
// Schedule / Record series; right-clicking a row offers Favourites and catch-up. The actions call
// back to the host (MainWindowController). Compiled with -fobjc-arc.
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
// Recording actions offered on a programme (set once by the host). onSchedule queues a one-off
// recording of this airing; onRecordSeries adds a rule for its title+channel.
@property (nonatomic, copy) void (^onSchedule)(NSString* tvgId, NSString* channelName,
                                               NSString* title, long long startUtc, long long stopUtc);
@property (nonatomic, copy) void (^onRecordSeries)(NSString* tvgId, NSString* channelName, NSString* title);
// Catch-up: play `title` (startUtc..stopUtc) out of `archiveChannel`'s archive. Returns YES once
// playback was asked for (the guide then hides); NO when the host explained why it cannot. nil =
// "Play from the start" is never offered.
@property (nonatomic, copy) BOOL (^onPlayFromStart)(long long archiveChannel, NSString* title,
                                                    long long startUtc, long long stopUtc);
// Favourites from a guide row's right-click (both nil = no such item).
@property (nonatomic, copy) BOOL (^isFavourite)(NSString* tvgId);
@property (nonatomic, copy) void (^onToggleFavourite)(NSString* tvgId, NSString* channelName);
// YES while Refresh Guide's worker is storing: a search session does not rebuild a stale index then
// (the worker is about to, and would only wait for the lock) — it searches by LIKE until the next one.
@property (nonatomic, copy) BOOL (^isGuideRefreshing)(void);

// Build the guide from the DB and show the window over `parent`. `onPlay` fires with a channel's
// full tvg-id (+ display name) when the user picks Play. If no row can be built, shows the "No
// guide to show" notice — with what the coverage counts say about this library — instead.
- (void)presentRelativeTo:(NSWindow*)parent
                   onPlay:(void (^)(NSString* tvgId, NSString* channelName))onPlay;
// Present the guide (same as above) and scroll+highlight the row for `tvgId` — the "Show in TV
// Guide" row action. Revealed (done), ChannelMissing (guide shown, caller should explain this channel
// isn't in it), or NoGuide (the controller already showed the notice — caller stays quiet). Matching
// uses the guide's own normalised tvg-id rule. A channel filter is cleared and the grid shown.
- (REGuideShowResult)presentRelativeTo:(NSWindow*)parent
                           showChannel:(NSString*)tvgId
                                onPlay:(void (^)(NSString* tvgId, NSString* channelName))onPlay;
- (void)hide;  // order the window out (kept alive for a later re-open)
- (void)relabelForLanguageChange;  // live language switch (window reused across opens)
// Refresh Guide stored new programmes: an open guide rebuilds its rows in place (keeping the time
// scroll, the filter and the search text), and the next search starts a new session.
- (void)guideDidChange;
// The catch-up flags changed (a provider sync, a playlist deleted): every row and listed result takes
// its new archive pick IN PLACE (no rebuild — the scroll and the filter stay); the search session
// ends, and a results list on screen searches again quietly (Win32 epgGuideUpdateArchive).
- (void)archiveDidChange;
@end
