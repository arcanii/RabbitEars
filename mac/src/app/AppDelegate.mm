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
    initUpdater();  // no-op stub until Sparkle is provisioned (RABBITEARS_HAVE_SPARKLE)

    _mainController = [[MainWindowController alloc] init];
    [_mainController showWindow];
}

- (void)applicationWillTerminate:(NSNotification*)__unused note {
    shutdownUpdater();
    diag::shutdown();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)__unused sender {
    return YES;
}

@end
