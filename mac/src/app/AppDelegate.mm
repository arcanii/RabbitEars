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
    [appMenu addItemWithTitle:@"About RabbitEars"
                       action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [[appMenu addItemWithTitle:@"Check for Updates…"
                        action:@selector(checkForUpdates:) keyEquivalent:@""] setTarget:self];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit RabbitEars" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = menubar;
}

- (void)checkForUpdates:(id)__unused sender { rabbitears::checkForUpdates(); }

- (void)applicationWillTerminate:(NSNotification*)__unused note {
    shutdownUpdater();
    diag::shutdown();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)__unused sender {
    return YES;
}

@end
