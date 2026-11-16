// SPDX-License-Identifier: GPL-3.0-or-later
//
// EpgGuideView — a native Cocoa "TV guide": a channels×time grid with a frozen channel
// column on the left, a frozen hour axis on top, and programme blocks laid out along the
// time axis. The mac peer of Win32's EpgGuideControl, but pure AppKit (no windows.h / no
// Direct2D): a single custom NSView that manages its own horizontal (time) + vertical
// (channel) scroll offsets and draws all four regions with CoreGraphics/CoreText, exactly
// as the Win32 control does with its three clip passes. It is a pure renderer over the rows
// it is handed — TvGuideWindowController assembles them from the DB. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

// One show on one channel. NSString-based so drawing needs no per-frame bridging (the
// window controller bridges the DB's std::wstring once, at assembly time).
@interface REGuideProgramme : NSObject
@property (nonatomic, copy) NSString* title;
@property (nonatomic, copy) NSString* descr;
@property (nonatomic) long long startUtc;  // unix epoch seconds (UTC); rendered in local time
@property (nonatomic) long long stopUtc;
@end

// One channel's row: its resolvable tvg-id + display name + programmes (sorted by start). Built
// from the shared GuideRow (common/ui/GuideModel.h) by TvGuideWindowController.
@interface REGuideRow : NSObject
@property (nonatomic, copy) NSString* channelId;    // full tvg-id — resolves to a stream (may be empty)
@property (nonatomic, copy) NSString* channelName;
@property (nonatomic, strong) NSArray<REGuideProgramme*>* programmes;
// Catch-up: the channel whose archive plays this row's past programmes, and how many days back it
// goes (0 = none) — GuideRow::archiveChannel / archiveDays.
@property (nonatomic) long long archiveChannel;
@property (nonatomic) int archiveDays;
@property (nonatomic) long long playlistId;
@end

// "13:00 – 14:30" in local time — or, for a programme of 24 hours or more (a provider's all-day
// filler would read the same time twice), "13:00 – 13:00 (24 h)". Win32 EpgGuideControl timeRange.
NSString* REGuideTimeRange(long long startUtc, long long stopUtc);

@class EpgGuideView;

@protocol EpgGuideViewDelegate <NSObject>
// A programme block was clicked (its row carries the channel to play).
- (void)guideView:(EpgGuideView*)view
    didActivateProgramme:(REGuideProgramme*)programme
                   inRow:(REGuideRow*)row;
@optional
// Right-click on a row (`programme` = the block under the pointer, or nil): the menu to show, or nil.
- (NSMenu*)guideView:(EpgGuideView*)view menuForRow:(REGuideRow*)row programme:(REGuideProgramme*)programme;
// The corner cell's channel-filter chip ("toronto ✕") was clicked: clear the filter.
- (void)guideViewDidClearFilter:(EpgGuideView*)view;
@end

@interface EpgGuideView : NSView
@property (nonatomic, weak) id<EpgGuideViewDelegate> delegate;
// The rows currently laid out, in display order (sorted by channel name).
@property (nonatomic, readonly) NSArray<REGuideRow*>* rows;
// Populate the grid and mark "now". Resets scroll to put "now" near the left.
- (void)setRows:(NSArray<REGuideRow*>*)rows nowUtc:(long long)nowUtc;
// The same, but keeping the time scroll (a channel filter, a rebuild after a refresh) when
// `keepScroll`; the channel scroll returns to the top either way (the rows' indices changed).
- (void)setRows:(NSArray<REGuideRow*>*)rows nowUtc:(long long)nowUtc keepScroll:(BOOL)keepScroll;
// The channel filter shown as a chip in the corner cell ("toronto ✕"); nil/empty = none.
@property (nonatomic, copy) NSString* filterLabel;
// Scroll the time axis back to "now" (the toolbar's Now button).
- (void)scrollToNow;
// A search result's jump: bring the programme starting at `startUtc` in row `index` into view (the
// row a third of the way down, the start ~80 pt in) and outline its block for 2.5 s.
- (void)flashProgrammeInRow:(NSInteger)index startUtc:(long long)startUtc;
// Scroll row `index` into view (vertically centred) and tint it until the next scroll or
// click. Used by "Show in TV Guide". Out-of-range indices are ignored.
- (void)revealRowAtIndex:(NSInteger)index;
@end
