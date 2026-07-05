// SPDX-License-Identifier: GPL-3.0-or-later
//
// TermsDialog — the first-run / version-change Terms-of-Use gate (peer of Win32
// showTerms in Win32/ui/Dialogs.cpp). App-modal: -runModal returns YES if the user
// accepted, NO if they declined (the caller then quits). The terms text mirrors the
// Win32 wording verbatim. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

@interface TermsDialog : NSObject
// `version` (e.g. the marketing version being accepted) is shown in the dialog header.
- (instancetype)initWithVersion:(NSString*)version;
// Runs the modal gate. Returns YES on "I Accept", NO on "Decline".
- (BOOL)runModal;
@end
