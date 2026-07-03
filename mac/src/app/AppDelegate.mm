// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS app delegate (the Win32 peer is the MainWindow bring-up in
// src/WinMain.cpp / src/ui/MainWindow.cpp). SCAFFOLD STATE: it proves the
// shared core end-to-end at launch (opens the real SQLite DB) and shows a
// placeholder window. The native channel grid, meters, dialogs, and the
// VlcPlayerMac video surface are the Phase-1 work (see docs/MACOS_PORT.md).
#import "AppDelegate.h"

#include <string>

#include "db/Database.h"
#include "platform/Encoding.h"  // resolves to the mac shim on this build
#include "platform/Log.h"
#include "platform/Updater.h"

#if __has_include("generated/version.h")
#include "generated/version.h"
#else
#define RE_VERSION_DISPLAY_W L"dev"
#endif

using namespace rabbitears;

@implementation AppDelegate {
    NSWindow* _window;
}

- (void)applicationDidFinishLaunching:(NSNotification*)__unused note {
    diag::init(RE_VERSION_DISPLAY_W);
    diag::info(L"macOS spike starting");

    // Exercise the shared core at launch: open the real DB via the ported
    // Database::defaultDbPath() (~/Library/Application Support/RabbitEars).
    Database db;
    std::wstring err;
    const std::wstring dbPath = Database::defaultDbPath();
    const bool opened = db.open(dbPath, &err);
    diag::info((opened ? L"DB opened: " : L"DB open FAILED: ") + dbPath);

    initUpdater();

    const NSRect frame = NSMakeRect(0, 0, 720, 480);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"RabbitEars — macOS (spike)";
    [_window center];

    NSString* status = [NSString
        stringWithFormat:@"RabbitEars macOS spike\n\nShared core: OK\nDB: %@\nLog: %@",
                         [NSString stringWithUTF8String:utf8FromWide(dbPath).c_str()],
                         [NSString stringWithUTF8String:utf8FromWide(diag::filePath()).c_str()]];
    NSTextField* label = [NSTextField labelWithString:status];
    label.frame = NSMakeRect(20, 20, 680, 440);
    label.maximumNumberOfLines = 0;
    [_window.contentView addSubview:label];

    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)applicationWillTerminate:(NSNotification*)__unused note {
    shutdownUpdater();
    diag::shutdown();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)__unused sender {
    return YES;
}

@end
