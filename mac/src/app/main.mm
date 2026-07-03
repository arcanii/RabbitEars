// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS entry point (the Win32 peer is src/WinMain.cpp). Creates the
// NSApplication, installs the delegate, and runs the event loop.
#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

int main(int /*argc*/, const char* /*argv*/[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
