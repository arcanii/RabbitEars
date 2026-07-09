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

// One channel's row: its resolvable tvg-id + display name + programmes (sorted by start).
@interface REGuideRow : NSObject
@property (nonatomic, copy) NSString* channelId;    // full tvg-id — resolves to a stream (may be empty)
@property (nonatomic, copy) NSString* channelName;
@property (nonatomic, strong) NSArray<REGuideProgramme*>* programmes;
@end

@class EpgGuideView;

@protocol EpgGuideViewDelegate <NSObject>
// A programme block was clicked (its row carries the channel to play).
- (void)guideView:(EpgGuideView*)view
    didActivateProgramme:(REGuideProgramme*)programme
                   inRow:(REGuideRow*)row;
@end

@interface EpgGuideView : NSView
@property (nonatomic, weak) id<EpgGuideViewDelegate> delegate;
// Populate the grid and mark "now". Resets scroll to put "now" near the left.
- (void)setRows:(NSArray<REGuideRow*>*)rows nowUtc:(long long)nowUtc;
@end
