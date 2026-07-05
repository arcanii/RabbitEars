// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS app delegate (the Win32 peer is the MainWindow bring-up in
// Win32/WinMain.cpp / Win32/ui/MainWindow.cpp). Owns app lifecycle: logging,
// auto-update, and the MainWindowController (the actual UI).
#import "AppDelegate.h"
#import "MainWindowController.h"

#include "platform/Log.h"
#include "platform/Updater.h"

#if __has_include("generated/version.h")
#include "generated/version.h"
#else
#define RE_VERSION_DISPLAY_W L"dev"
#endif

using namespace rabbitears;

@implementation AppDelegate {
    MainWindowController* _mainController;
}

- (void)applicationDidFinishLaunching:(NSNotification*)__unused note {
    diag::init(RE_VERSION_DISPLAY_W);
    diag::info(L"macOS app starting");
    [self buildMenu];
    initUpdater();  // Sparkle background checks when provisioned; no-op otherwise

    _mainController = [[MainWindowController alloc] init];
    [_mainController showWindow];
}

// Minimal app menu: About / Check for Updates… / Quit. (About + Quit route to
// NSApp via the responder chain; Check for Updates calls into Sparkle.)
- (void)buildMenu {
    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [menubar addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [[appMenu addItemWithTitle:@"About RabbitEars"
                        action:@selector(showAboutPanel:) keyEquivalent:@""] setTarget:self];
    [[appMenu addItemWithTitle:@"Check for Updates…"
                        action:@selector(checkForUpdates:) keyEquivalent:@""] setTarget:self];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit RabbitEars" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    // Edit menu — REQUIRED for Cmd-X/C/V/A/Z to work in text fields: Cocoa routes
    // the standard editing commands through these menu items' key equivalents,
    // down the responder chain to the focused field editor. Without it, paste
    // (and cut/copy/select-all/undo) silently do nothing.
    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    [menubar addItem:editItem];
    NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem* redo = [editMenu addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"];
    redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [editMenu addItem:[NSMenuItem separatorItem]];
    [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = editMenu;

    // View menu — native full-screen (⌃⌘F). toggleFullScreen: routes down the
    // responder chain to the key window.
    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    [menubar addItem:viewItem];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem* fs = [viewMenu addItemWithTitle:@"Enter Full Screen"
                                         action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    // Hide the channel list / toolbar so the video can fill the window. Kept in the
    // menu bar (not the in-window toolbar) so they still work once the toolbar hides.
    [viewMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* hideList = [viewMenu addItemWithTitle:@"Hide Channel List"
                                               action:@selector(toggleChannelList:) keyEquivalent:@"l"];
    hideList.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    hideList.target = self;
    NSMenuItem* hideBar = [viewMenu addItemWithTitle:@"Hide Toolbar"
                                              action:@selector(toggleToolbar:) keyEquivalent:@"t"];
    hideBar.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    hideBar.target = self;
    NSMenuItem* spec = [viewMenu addItemWithTitle:@"Show Spectrum"
                                           action:@selector(toggleSpectrum:) keyEquivalent:@"m"];
    spec.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    spec.target = self;
    NSMenuItem* vidOnly = [viewMenu addItemWithTitle:@"Video Only"
                                              action:@selector(toggleVideoOnly:) keyEquivalent:@"f"];
    vidOnly.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    vidOnly.target = self;
    viewItem.submenu = viewMenu;

    NSApp.mainMenu = menubar;
}

- (void)checkForUpdates:(id)__unused sender { rabbitears::checkForUpdates(); }

// View-menu chrome toggles, forwarded to the window controller.
- (void)toggleChannelList:(id)__unused sender { [_mainController toggleChannelList]; }
- (void)toggleToolbar:(id)__unused sender { [_mainController toggleToolbar]; }
- (void)toggleSpectrum:(id)__unused sender { [_mainController toggleSpectrum]; }
- (void)toggleVideoOnly:(id)__unused sender { [_mainController toggleVideoOnly]; }

// Reflect current state in the menu titles (Hide ⇄ Show).
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(toggleChannelList:))
        item.title = _mainController.channelListHidden ? @"Show Channel List" : @"Hide Channel List";
    else if (item.action == @selector(toggleToolbar:))
        item.title = _mainController.toolbarHidden ? @"Show Toolbar" : @"Hide Toolbar";
    else if (item.action == @selector(toggleSpectrum:))
        item.title = _mainController.spectrumEnabled ? @"Hide Spectrum" : @"Show Spectrum";
    else if (item.action == @selector(toggleVideoOnly:))
        item.title = _mainController.videoOnly ? @"Exit Video Only" : @"Video Only";
    return YES;
}

// Custom About panel — the mac peer of the Win32 About box: libVLC attribution +
// the educational-use disclaimer (name/version come from the bundle Info.plist).
- (void)showAboutPanel:(id)__unused sender {
    NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
    ps.alignment = NSTextAlignmentCenter;
    NSString* credits =
        @"A simple IPTV viewer for macOS.\n\n"
        @"Plays media with libVLC (LGPL-2.1)\n"
        @"© VideoLAN and the VLC contributors.\n\n"
        @"RabbitEars is provided only for educational purposes, and does not "
        @"represent supporting any illegal activity that you do with it. "
        @"We don't know, we don't care.";
    NSAttributedString* attr = [[NSAttributedString alloc] initWithString:credits attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:11],
        NSParagraphStyleAttributeName: ps,
    }];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{NSAboutPanelOptionCredits: attr}];
}

- (void)applicationWillTerminate:(NSNotification*)__unused note {
    shutdownUpdater();
    diag::shutdown();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)__unused sender {
    return YES;
}

@end
