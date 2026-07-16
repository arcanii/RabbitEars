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

@interface MainWindowController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate, NSSplitViewDelegate, NSWindowDelegate, NSMenuDelegate>
- (void)showWindow;
// Re-label every built-once UI surface in the CURRENT active language (Settings ▸ Language applies
// live, no restart). The caller must flip i18n::setActiveLang(...) BEFORE calling this. Main thread.
- (void)applyLanguageLive;

// View-menu chrome toggles — hide the channel list / the top toolbar so the video
// can fill the window. Driven from the app menu bar so they still work when the
// in-window toolbar is hidden. The getters back the menu titles (Hide ⇄ Show).
- (void)toggleChannelList;
- (void)toggleToolbar;
- (void)toggleVideoOnly;  // View ▸ Video Only — collapse all chrome to just the video
- (BOOL)channelListHidden;
- (BOOL)toolbarHidden;
- (BOOL)videoOnly;

// YES once -showWindow has finished startup: the Terms-of-Use sheet (if shown) was accepted and
// -finishStartup ran. AppDelegate's -validateMenuItem: disables app commands until then, so the
// ToU sheet gates the WHOLE app — a sheet alone blocks only its own window, not the menu bar.
- (BOOL)startupFinished;

// Multi-view layout (View menu): Single / Split (2×2) / Picture-in-Picture. The getters
// back the menu checkmarks.
- (void)setViewSingle:(id)sender;
- (void)setViewSplit:(id)sender;
- (void)setViewPip:(id)sender;
- (BOOL)isSplitView;
- (BOOL)isPipView;

// Command actions surfaced in BOTH the in-window Settings ▾ pull-down and the app
// menu bar (the menu-bar items forward here from AppDelegate).
- (void)addPlaylist:(id)sender;
- (void)openFile:(id)sender;
- (void)showPlaylists:(id)sender;
- (void)showMeters:(id)sender;

// TV Guide (EPG). refreshGuide: downloads + stores the XMLTV guide for every enabled
// playlist that carries a guide URL; showGuide: opens the channels×time guide window.
- (void)refreshGuide:(id)sender;
- (void)showGuide:(id)sender;
- (void)exportFavourites:(id)sender;
- (void)importFavourites:(id)sender;
// Finalize any in-progress recordings on quit. The MRC app-lifetime objects (this controller,
// its panes) are not destroyed at termination, so the VlcPlayerMac destructors that finalize a
// recording never run — without this, an mp4/mkv recording open at quit loses its index and is
// unplayable. Called from -applicationWillTerminate:.
- (void)finalizeRecordingsForQuit;
@end
