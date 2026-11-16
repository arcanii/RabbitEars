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
namespace rabbitears::mac { class ScheduleStatusWriter; }

@interface RecordingsWindowController : NSObject
// `writer` is the scheduler's (MainWindowController owns both it and this window): every terminal
// status and rule change here goes through it, so a write lost to DB contention is remembered and
// replayed instead of silently undone (ScheduleStatusWriter.h).
- (instancetype)initWithDatabase:(rabbitears::Database*)db
                    statusWriter:(rabbitears::mac::ScheduleStatusWriter*)writer;
// Present over `parent` and refresh. `onChange` fires after a cancel/delete/enable so the host
// can nudge the scheduler (re-expand rules, re-derive keep-awake).
- (void)presentRelativeTo:(NSWindow*)parent onChange:(void (^)(void))onChange;
- (void)reload;  // re-query the DB into both tables (called from the ~30s tick when open)
// Cancel on a row that is RECORDING asks the scheduler (which owns the recorder) to stop it before the
// row is marked — the peer of Win32's cb.cancel calling stopScheduledRecorder. May be nil.
@property (nonatomic, copy) void (^onStopRecording)(long long scheduleId);
- (void)relabelForLanguageChange;  // live language switch (window reused across opens)
@end
