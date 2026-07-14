// SPDX-License-Identifier: GPL-3.0-or-later
//
// RecordingsWindowController — a modeless window managing the recording queue: a "Scheduled"
// tab (one-off + rule-generated schedules, with their status) and a "Series Rules" tab
// (standing EPG rules). The mac peer of Win32's Scheduled Recordings / Rules dialogs. It reads
// the shared Database and lets the user cancel a pending schedule or enable/delete a rule; the
// actual firing is the MainWindowController scheduler tick. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

namespace rabbitears { class Database; }

@interface RecordingsWindowController : NSObject
- (instancetype)initWithDatabase:(rabbitears::Database*)db;
// Present over `parent` and refresh. `onChange` fires after a cancel/delete/enable so the host
// can nudge the scheduler (re-expand rules, re-derive keep-awake).
- (void)presentRelativeTo:(NSWindow*)parent onChange:(void (^)(void))onChange;
- (void)reload;  // re-query the DB into both tables (called from the ~30s tick when open)
- (void)relabelForLanguageChange;  // live language switch (window reused across opens)
@end
