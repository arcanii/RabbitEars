// SPDX-License-Identifier: GPL-3.0-or-later
//
// TermsDialog — the first-run / version-change Terms-of-Use gate (peer of Win32
// showTerms in Win32/ui/Dialogs.cpp). Presented as a document-modal SHEET on the main
// window, so it descends from the title bar and can never be buried independently of its
// window. (The old app-modal panel ran before the main window was shown and before the app
// was activated; on a non-activating launch — e.g. a Sparkle post-update relaunch — it could
// come up hidden behind other apps with no visible window, which read as a hang/beachball.)
// The terms text mirrors the Win32 wording verbatim. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

@interface TermsDialog : NSObject
// `version` (e.g. the marketing version being accepted) is shown in the dialog header.
- (instancetype)initWithVersion:(NSString*)version;
// Presents the gate as a sheet on `parent`; `completion` is called with YES on "I Accept",
// NO on "Decline". The dialog keeps ITSELF alive until the sheet is dismissed, so the caller
// may release its reference immediately after this call.
- (void)beginSheetForWindow:(NSWindow*)parent completion:(void (^)(BOOL accepted))completion;
@end
