// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS app delegate (the Win32 peer is the MainWindow bring-up in
// Win32/WinMain.cpp / Win32/ui/MainWindow.cpp). Owns app lifecycle: logging,
// auto-update, and the MainWindowController (the actual UI).
#import "AppDelegate.h"
#import "MainWindowController.h"
#import "Tr.h"

#include "platform/Log.h"
#include "platform/Updater.h"

#if __has_include("generated/version.h")
#include "generated/version.h"
#else
#define RE_VERSION_DISPLAY_W L"dev"
#endif

using namespace rabbitears;
using namespace rabbitears::i18n;  // StringId

@implementation AppDelegate {
    MainWindowController* _mainController;
}

- (void)applicationDidFinishLaunching:(NSNotification*)__unused note {
    diag::init(RE_VERSION_DISPLAY_W);
    diag::info(L"macOS app starting");
    applyStartupLanguage();  // resolve the display language (NSUserDefaults) BEFORE building any UI
    [self buildMenu];
    initUpdater();  // Sparkle background checks when provisioned; no-op otherwise

    _mainController = [[MainWindowController alloc] init];
    [_mainController showWindow];
}

// Minimal app menu: About / Check for Updates… / Quit. (About + Quit route to
// NSApp via the responder chain; Check for Updates calls into Sparkle.)
- (void)buildMenu {
    // buildMenu is re-run on every live language switch, so autorelease the alloc'd bar/items/
    // submenus (each is retained by its parent, and NSApp.mainMenu retains the whole tree) — else
    // ~11 objects would leak per switch. The old bar is reclaimed when NSApp.mainMenu is reassigned.
    NSMenu* menubar = [[[NSMenu alloc] init] autorelease];
    NSMenuItem* appItem = [[[NSMenuItem alloc] init] autorelease];
    [menubar addItem:appItem];
    NSMenu* appMenu = [[[NSMenu alloc] init] autorelease];
    [[appMenu addItemWithTitle:Tr(StringId::AboutWindowTitle)
                        action:@selector(showAboutPanel:) keyEquivalent:@""] setTarget:self];
    [[appMenu addItemWithTitle:Tr(StringId::AboutCheckForUpdatesButton)
                        action:@selector(checkForUpdates:) keyEquivalent:@""] setTarget:self];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self addLanguageSubmenuTo:appMenu];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:Tr(StringId::MenuQuit) action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    // File menu — playlist import + management. The Mac-native home for these commands
    // (they're also on the in-window Settings ▾ pull-down). Items target self and forward
    // to _mainController, which is nil until the window loads just after buildMenu.
    NSMenuItem* fileItem = [[[NSMenuItem alloc] init] autorelease];
    [menubar addItem:fileItem];
    NSMenu* fileMenu = [[[NSMenu alloc] initWithTitle:Tr(StringId::MenuFile)] autorelease];
    [[fileMenu addItemWithTitle:Tr(StringId::MenuAddPlaylist)
                         action:@selector(addPlaylist:) keyEquivalent:@"n"] setTarget:self];
    [[fileMenu addItemWithTitle:Tr(StringId::MenuOpenPlaylistFile)
                         action:@selector(openFile:) keyEquivalent:@"o"] setTarget:self];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [[fileMenu addItemWithTitle:Tr(StringId::MenuManagePlaylists)
                         action:@selector(showPlaylists:) keyEquivalent:@""] setTarget:self];
    [fileMenu addItem:[NSMenuItem separatorItem]];
    [[fileMenu addItemWithTitle:Tr(StringId::MenuImportFavourites)
                         action:@selector(importFavourites:) keyEquivalent:@""] setTarget:self];
    [[fileMenu addItemWithTitle:Tr(StringId::MenuExportFavourites)
                         action:@selector(exportFavourites:) keyEquivalent:@""] setTarget:self];
    fileItem.submenu = fileMenu;

    // Edit menu — REQUIRED for Cmd-X/C/V/A/Z to work in text fields: Cocoa routes
    // the standard editing commands through these menu items' key equivalents,
    // down the responder chain to the focused field editor. Without it, paste
    // (and cut/copy/select-all/undo) silently do nothing.
    NSMenuItem* editItem = [[[NSMenuItem alloc] init] autorelease];
    [menubar addItem:editItem];
    NSMenu* editMenu = [[[NSMenu alloc] initWithTitle:Tr(StringId::MenuEdit)] autorelease];
    [editMenu addItemWithTitle:Tr(StringId::MenuUndo) action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem* redo = [editMenu addItemWithTitle:Tr(StringId::MenuRedo) action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:Tr(StringId::MenuCut) action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:Tr(StringId::MenuCopy) action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:Tr(StringId::MenuPaste) action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:Tr(StringId::MenuSelectAll) action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = editMenu;

    // View menu — native full-screen (⌃⌘F). toggleFullScreen: routes down the
    // responder chain to the key window.
    NSMenuItem* viewItem = [[[NSMenuItem alloc] init] autorelease];
    [menubar addItem:viewItem];
    NSMenu* viewMenu = [[[NSMenu alloc] initWithTitle:Tr(StringId::MenuView)] autorelease];
    NSMenuItem* fs = [viewMenu addItemWithTitle:Tr(StringId::MenuEnterFullScreen)
                                         action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    // Hide the channel list / toolbar so the video can fill the window. Kept in the
    // menu bar (not the in-window toolbar) so they still work once the toolbar hides.
    [viewMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* hideList = [viewMenu addItemWithTitle:Tr(StringId::MenuHideChannelList)
                                               action:@selector(toggleChannelList:) keyEquivalent:@"l"];
    hideList.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    hideList.target = self;
    NSMenuItem* hideBar = [viewMenu addItemWithTitle:Tr(StringId::MenuHideToolbar)
                                              action:@selector(toggleToolbar:) keyEquivalent:@"t"];
    hideBar.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    hideBar.target = self;
    NSMenuItem* vidOnly = [viewMenu addItemWithTitle:Tr(StringId::MenuVideoOnlyPlain)
                                              action:@selector(toggleVideoOnly:) keyEquivalent:@"f"];
    vidOnly.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    vidOnly.target = self;

    // Multi-view layout — Single vs Split (2×2). ⌃⌘1 / ⌃⌘2.
    [viewMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* single = [viewMenu addItemWithTitle:Tr(StringId::MenuSingleView)
                                             action:@selector(setViewSingle:) keyEquivalent:@"1"];
    single.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    single.target = self;
    NSMenuItem* split = [viewMenu addItemWithTitle:Tr(StringId::MenuSplitView)
                                            action:@selector(setViewSplit:) keyEquivalent:@"2"];
    split.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    split.target = self;
    NSMenuItem* pip = [viewMenu addItemWithTitle:Tr(StringId::MenuPictureInPicture)
                                          action:@selector(setViewPip:) keyEquivalent:@"3"];
    pip.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    pip.target = self;

    // TV Guide (EPG) — open the channels×time guide (⌘G) + download the guide data.
    [viewMenu addItem:[NSMenuItem separatorItem]];
    [[viewMenu addItemWithTitle:Tr(StringId::TvGuideTitle)
                         action:@selector(showGuide:) keyEquivalent:@"g"] setTarget:self];
    [[viewMenu addItemWithTitle:Tr(StringId::MenuRefreshGuide)
                         action:@selector(refreshGuide:) keyEquivalent:@""] setTarget:self];

    [viewMenu addItem:[NSMenuItem separatorItem]];
    [[viewMenu addItemWithTitle:Tr(StringId::MenuMeters)
                         action:@selector(showMeters:) keyEquivalent:@""] setTarget:self];

    viewItem.submenu = viewMenu;

    NSApp.mainMenu = menubar;
}

- (void)checkForUpdates:(id)__unused sender { rabbitears::checkForUpdates(); }

// Language submenu (System default / English / 日本語 / 繁體中文 / 繁體中文（香港）). Selecting a language persists
// the preference to NSUserDefaults and applies it LIVE — no restart (see -selectLanguage:). The
// menu is rebuilt on each language change, so the ✓ tracks the active language.
- (void)addLanguageSubmenuTo:(NSMenu*)parent {
    NSMenuItem* langItem = [[[NSMenuItem alloc] init] autorelease];
    langItem.title = Tr(StringId::MenuLanguage);
    NSMenu* langMenu = [[[NSMenu alloc] initWithTitle:langItem.title] autorelease];
    struct LangDef { NSString* code; StringId sid; };
    const LangDef defs[] = {
        { @"system",  StringId::LangSystemDefault },
        { @"en",      StringId::LangEnglish },
        { @"ja",      StringId::LangJapanese },
        { @"zh-Hant", StringId::LangTraditionalChinese },
        { @"zh-HK",   StringId::LangTraditionalChineseHK },
    };
    NSString* cur = currentLanguagePref();
    for (const LangDef& d : defs) {
        NSMenuItem* it = [langMenu addItemWithTitle:Tr(d.sid)
                                             action:@selector(selectLanguage:) keyEquivalent:@""];
        it.target = self;
        it.representedObject = d.code;
        it.state = [cur isEqualToString:d.code] ? NSControlStateValueOn : NSControlStateValueOff;
    }
    langItem.submenu = langMenu;
    [parent addItem:langItem];  // one-time, app-lifetime (same MRC-leak convention as the rest of buildMenu)
}

// Apply the chosen language LIVE — no restart (the mac peer of Win32 setLanguageSelection).
- (void)selectLanguage:(NSMenuItem*)sender {
    NSString* code = sender.representedObject;
    if (!code.length || [code isEqualToString:currentLanguagePref()]) return;  // no change
    setLanguagePref(code);                  // persist (NSUserDefaults) — BEFORE buildMenu so the
                                            //   Language checkmark reads the new pref
    setActiveLang(macResolveLang(code));    // flip the RUNNING language: every Tr()/TrF() now reads it
    [self buildMenu];                       // rebuild the menu bar in the new language (+ moves the checkmark)
    [_mainController applyLanguageLive];     // relabel the open window (fans out to meters + modeless windows)
}

// View-menu chrome toggles, forwarded to the window controller.
- (void)toggleChannelList:(id)__unused sender { [_mainController toggleChannelList]; }
- (void)toggleToolbar:(id)__unused sender { [_mainController toggleToolbar]; }
- (void)toggleVideoOnly:(id)__unused sender { [_mainController toggleVideoOnly]; }

// File/View command actions, forwarded to the window controller (same handlers the
// in-window Settings ▾ pull-down targets). _mainController is nil until just after
// buildMenu, so an early invocation would no-op safely.
- (void)addPlaylist:(id)sender { [_mainController addPlaylist:sender]; }
- (void)openFile:(id)sender { [_mainController openFile:sender]; }
- (void)showPlaylists:(id)sender { [_mainController showPlaylists:sender]; }
- (void)showMeters:(id)sender { [_mainController showMeters:sender]; }
- (void)showGuide:(id)sender { [_mainController showGuide:sender]; }
- (void)refreshGuide:(id)sender { [_mainController refreshGuide:sender]; }
- (void)importFavourites:(id)sender { [_mainController importFavourites:sender]; }
- (void)exportFavourites:(id)sender { [_mainController exportFavourites:sender]; }
- (void)setViewSingle:(id)sender { [_mainController setViewSingle:sender]; }
- (void)setViewSplit:(id)sender { [_mainController setViewSplit:sender]; }
- (void)setViewPip:(id)sender { [_mainController setViewPip:sender]; }

// Reflect current state in the menu titles (Hide ⇄ Show).
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(toggleChannelList:))
        item.title = Tr(_mainController.channelListHidden ? StringId::MenuShowChannelList : StringId::MenuHideChannelList);
    else if (item.action == @selector(toggleToolbar:))
        item.title = Tr(_mainController.toolbarHidden ? StringId::MenuShowToolbar : StringId::MenuHideToolbar);
    else if (item.action == @selector(toggleVideoOnly:))
        item.title = Tr(_mainController.videoOnly ? StringId::MenuExitVideoOnly : StringId::MenuVideoOnlyPlain);
    else if (item.action == @selector(setViewSingle:))
        item.state = (!_mainController.isSplitView && !_mainController.isPipView)
                         ? NSControlStateValueOn : NSControlStateValueOff;
    else if (item.action == @selector(setViewSplit:))
        item.state = _mainController.isSplitView ? NSControlStateValueOn : NSControlStateValueOff;
    else if (item.action == @selector(setViewPip:))
        item.state = _mainController.isPipView ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

// Custom About panel — the mac peer of the Win32 About box: libVLC attribution +
// the educational-use disclaimer (name/version come from the bundle Info.plist).
- (void)showAboutPanel:(id)__unused sender {
    NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
    ps.alignment = NSTextAlignmentCenter;
    NSString* credits = Tr(StringId::AboutMacCredits);
    NSAttributedString* attr = [[NSAttributedString alloc] initWithString:credits attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:11],
        NSParagraphStyleAttributeName: ps,
    }];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{NSAboutPanelOptionCredits: attr}];
}

- (void)applicationWillTerminate:(NSNotification*)__unused note {
    [_mainController finalizeRecordingsForQuit];  // flush + index any open recording before exit
    shutdownUpdater();
    diag::shutdown();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)__unused sender {
    return YES;
}

@end
