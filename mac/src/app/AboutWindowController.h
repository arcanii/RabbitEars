// SPDX-License-Identifier: GPL-3.0-or-later
//
// AboutWindowController — the custom About box (peer of the Win32 About dialog in
// Win32/ui/Dialogs.cpp).
//
// WHY THIS EXISTS AT ALL, rather than +orderFrontStandardAboutPanelWithOptions:. mac used the
// SYSTEM About panel until 0.2.18, and that panel takes a credits string but CANNOT host controls
// — so the two tip buttons Win32 has ("Buy me a coffee" / "Ko-fi") had nowhere to live. Adding
// them means owning the window.
//
// ⚠ OWNING THE WINDOW MEANS OWNING TWO LICENCE OBLIGATIONS the system panel used to satisfy for
// free. Both are reproduced here and MUST NOT be dropped:
//   • the libVLC attribution (LGPL-2.1, © VideoLAN and the VLC contributors) — it lives in the
//     shared catalog as AboutMacCredits, which also carries the educational-purposes disclaimer;
//   • the GPL-3.0 notice, which is `NSHumanReadableCopyright` in Info.plist. It is READ FROM THE
//     BUNDLE here rather than copied into a literal, so the plist stays the single source of truth
//     and the two can never drift.
//
// Compiled with -fobjc-arc (listed in mac/CMakeLists.txt), like the other dialogs.
#pragma once

#import <Cocoa/Cocoa.h>

@interface AboutWindowController : NSObject
// Shows the About window, creating it on first use and re-using it afterwards. Building the
// contents on each show is what keeps it localized after a live language switch, which the
// built-once surfaces in MainWindowController have to be relabelled for by hand.
+ (void)showAbout;
@end
