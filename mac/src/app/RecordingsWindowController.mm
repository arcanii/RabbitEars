// SPDX-License-Identifier: GPL-3.0-or-later
// See RecordingsWindowController.h.
#import "RecordingsWindowController.h"

#include "db/Database.h"
#include "models/RecordingRule.h"
#include "models/ScheduledRecording.h"
#include "platform/Encoding.h"

#include <ctime>
#include <vector>

using rabbitears::Database;
using rabbitears::RecordingRule;
using rabbitears::ScheduledRecording;
using rabbitears::ScheduleStatus;

namespace {
NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:rabbitears::utf8FromWide(w).c_str()] ?: @"";
}
NSString* statusText(ScheduleStatus s) {
    switch (s) {
        case ScheduleStatus::Pending:   return @"Pending";
        case ScheduleStatus::Recording: return @"● Recording";
        case ScheduleStatus::Done:      return @"Done";
        case ScheduleStatus::Missed:    return @"Missed";
        case ScheduleStatus::Failed:    return @"Failed";
        case ScheduleStatus::Cancelled: return @"Cancelled";
    }
    return @"";
}
NSString* whenText(long long startUtc, long long stopUtc) {
    // Cached (main-thread only): the ~30s reload re-renders every row, so per-row formatter
    // allocation would churn. dispatch_once keeps two shared formatters.
    static NSDateFormatter* df; static NSDateFormatter* tf; static dispatch_once_t once;
    dispatch_once(&once, ^{
        df = [[NSDateFormatter alloc] init];
        df.dateStyle = NSDateFormatterMediumStyle; df.timeStyle = NSDateFormatterShortStyle;
        tf = [[NSDateFormatter alloc] init];
        tf.timeStyle = NSDateFormatterShortStyle;
    });
    NSString* a = [df stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)startUtc]];
    NSString* b = [tf stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)stopUtc]];
    return [NSString stringWithFormat:@"%@ – %@", a, b];
}
}  // namespace

@interface RecordingsWindowController () <NSTableViewDataSource, NSTableViewDelegate>
@end

@implementation RecordingsWindowController {
    Database*    _db;
    NSWindow*    _window;
    NSTableView* _schedTable;
    NSTableView* _ruleTable;
    NSTextField* _hint;
    void (^_onChange)(void);
    std::vector<ScheduledRecording> _schedules;
    std::vector<RecordingRule>      _rules;
}

- (instancetype)initWithDatabase:(Database*)db {
    if ((self = [super init])) _db = db;
    return self;
}

- (void)presentRelativeTo:(NSWindow*)parent onChange:(void (^)(void))onChange {
    _onChange = [onChange copy];
    if (!_window) [self buildWindowNear:parent];
    [self reload];
    [_window makeKeyAndOrderFront:nil];
}

- (void)reload {
    // Preserve each table's selection by IDENTITY across a background (~30s tick) reload — a
    // reordering insert would otherwise leave the highlight on a DIFFERENT row than the user
    // picked, so a later Cancel/Delete could hit the wrong item.
    long long selSched = 0, selRule = 0;
    if (_schedTable.selectedRow >= 0 && _schedTable.selectedRow < (NSInteger)_schedules.size())
        selSched = _schedules[(size_t)_schedTable.selectedRow].id;
    if (_ruleTable.selectedRow >= 0 && _ruleTable.selectedRow < (NSInteger)_rules.size())
        selRule = _rules[(size_t)_ruleTable.selectedRow].id;

    if (_db) { _schedules = _db->listSchedules(); _rules = _db->listRules(); }
    [_schedTable reloadData];
    [_ruleTable reloadData];

    if (selSched)
        for (size_t i = 0; i < _schedules.size(); ++i)
            if (_schedules[i].id == selSched) {
                [_schedTable selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
                break;
            }
    if (selRule)
        for (size_t i = 0; i < _rules.size(); ++i)
            if (_rules[i].id == selRule) {
                [_ruleTable selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
                break;
            }
}

- (NSTableView*)makeTableInScroll:(NSScrollView*)scroll columns:(NSArray<NSString*>*)titles
                            widths:(NSArray<NSNumber*>*)widths ids:(NSArray<NSString*>*)ids {
    NSTableView* t = [[NSTableView alloc] initWithFrame:scroll.bounds];
    t.usesAlternatingRowBackgroundColors = YES;
    t.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    for (NSUInteger i = 0; i < titles.count; ++i) {
        NSTableColumn* c = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
        c.title = titles[i];
        c.width = widths[i].doubleValue;
        [t addTableColumn:c];
    }
    t.dataSource = self;
    t.delegate = self;
    scroll.documentView = t;
    scroll.hasVerticalScroller = YES;
    return t;
}

- (void)buildWindowNear:(NSWindow*)parent {
    const NSRect frame = NSMakeRect(0, 0, 720, 460);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                     NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                                            backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"Recordings";
    _window.releasedWhenClosed = NO;
    _window.contentMinSize = NSMakeSize(520, 320);

    NSView* cv = _window.contentView;
    NSTabView* tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(12, 78, frame.size.width - 24, frame.size.height - 90)];
    tabs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [cv addSubview:tabs];

    // Scheduled tab.
    NSTabViewItem* schedItem = [[NSTabViewItem alloc] initWithIdentifier:@"sched"];
    schedItem.label = @"Scheduled";
    NSScrollView* schedScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 660, 320)];
    _schedTable = [self makeTableInScroll:schedScroll
                                  columns:@[ @"Channel", @"Title", @"When", @"Format", @"Status" ]
                                   widths:@[ @150, @200, @210, @60, @100 ]
                                      ids:@[ @"chan", @"title", @"when", @"fmt", @"status" ]];
    schedItem.view = schedScroll;
    [tabs addTabViewItem:schedItem];

    // Series rules tab.
    NSTabViewItem* ruleItem = [[NSTabViewItem alloc] initWithIdentifier:@"rules"];
    ruleItem.label = @"Series Rules";
    NSScrollView* ruleScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 660, 320)];
    _ruleTable = [self makeTableInScroll:ruleScroll
                                 columns:@[ @"Enabled", @"Channel", @"Matches title", @"Format" ]
                                  widths:@[ @70, @180, @280, @70 ]
                                     ids:@[ @"on", @"chan", @"title", @"fmt" ]];
    ruleItem.view = ruleScroll;
    [tabs addTabViewItem:ruleItem];

    // Bottom bar: action buttons + the honest wake note (Phase 7).
    NSButton* cancelSched = [NSButton buttonWithTitle:@"Cancel Selected"
                                               target:self action:@selector(cancelSchedule:)];
    cancelSched.frame = NSMakeRect(12, 44, 150, 26);
    cancelSched.autoresizingMask = NSViewMaxYMargin;
    [cv addSubview:cancelSched];
    NSButton* toggleRule = [NSButton buttonWithTitle:@"Enable/Disable Rule"
                                              target:self action:@selector(toggleRule:)];
    toggleRule.frame = NSMakeRect(170, 44, 170, 26);
    [cv addSubview:toggleRule];
    NSButton* delRule = [NSButton buttonWithTitle:@"Delete Rule"
                                           target:self action:@selector(deleteRule:)];
    delRule.frame = NSMakeRect(348, 44, 120, 26);
    [cv addSubview:delRule];

    _hint = [NSTextField labelWithString:
        @"RabbitEars records only while it is running and your Mac is awake. It can’t wake a "
        @"sleeping Mac to record — keep the Mac on (lid open, plugged in) for scheduled recordings."];
    _hint.frame = NSMakeRect(12, 10, frame.size.width - 24, 28);
    _hint.autoresizingMask = NSViewWidthSizable;
    _hint.textColor = NSColor.secondaryLabelColor;
    _hint.font = [NSFont systemFontOfSize:11];
    _hint.maximumNumberOfLines = 2;
    [cv addSubview:_hint];

    _window.frameAutosaveName = @"RabbitEarsRecordingsWindow";
    if (![_window setFrameUsingName:@"RabbitEarsRecordingsWindow"]) {
        if (parent) [_window setFrameOrigin:NSMakePoint(NSMinX(parent.frame) + 40, NSMaxY(parent.frame) - frame.size.height - 40)];
        else [_window center];
    }
}

// ---- table data ----------------------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView*)t {
    return t == _schedTable ? (NSInteger)_schedules.size() : (NSInteger)_rules.size();
}

- (NSView*)tableView:(NSTableView*)t viewForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    NSString* text = @"";
    if (t == _schedTable && row < (NSInteger)_schedules.size()) {
        const auto& s = _schedules[(size_t)row];
        NSString* cid = col.identifier;
        if      ([cid isEqualToString:@"chan"])   text = ns(s.channelName);
        else if ([cid isEqualToString:@"title"])  text = ns(s.title);
        else if ([cid isEqualToString:@"when"])   text = whenText(s.startUtc, s.stopUtc);
        else if ([cid isEqualToString:@"fmt"])    text = ns(s.mux);
        else if ([cid isEqualToString:@"status"]) text = statusText(s.status);
    } else if (t == _ruleTable && row < (NSInteger)_rules.size()) {
        const auto& r = _rules[(size_t)row];
        NSString* cid = col.identifier;
        if      ([cid isEqualToString:@"on"])    text = r.enabled ? @"✓" : @"—";
        else if ([cid isEqualToString:@"chan"])  text = r.channelName.empty() ? @"(any channel)" : ns(r.channelName);
        else if ([cid isEqualToString:@"title"]) text = ns(r.titleMatch);
        else if ([cid isEqualToString:@"fmt"])   text = ns(r.mux);
    }
    NSTextField* tf = [NSTextField labelWithString:text];
    tf.lineBreakMode = NSLineBreakByTruncatingTail;
    return tf;
}

// ---- actions -------------------------------------------------------------------------------

- (void)notifyChange { if (_onChange) _onChange(); }

- (void)cancelSchedule:(id)__unused sender {
    const NSInteger row = _schedTable.selectedRow;
    if (row < 0 || row >= (NSInteger)_schedules.size() || !_db) return;
    const auto s = _schedules[(size_t)row];  // copy: reload below mutates _schedules
    // A recording in progress can't be cancelled from here (the scheduler owns it).
    if (s.status == ScheduleStatus::Recording) return;
    if (s.status == ScheduleStatus::Pending) {
        // Mark Cancelled (a tombstone) rather than delete, so a series rule can't re-queue the slot.
        _db->updateScheduleStatus(s.id, ScheduleStatus::Cancelled);
    } else if (s.ruleId != 0 && s.stopUtc > (long long)time(nullptr)) {
        // Rule-generated, still-future, already history (Cancelled/Failed): this row is the dedup
        // anchor. Hard-deleting it lets the next rule expansion recreate the airing — silently
        // undoing the cancel (a review-caught bug; Win32 guards the same case). Keep it; the user
        // must delete/disable the rule to stop it entirely.
        NSAlert* a = [[NSAlert alloc] init];
        a.messageText = @"This airing was queued by a series rule";
        a.informativeText = @"It’s cancelled and won’t record. The entry stays in the list because "
                            @"removing it now would let its series rule re-queue the same airing; "
                            @"you can remove it once the airing’s scheduled time has passed.";
        [a addButtonWithTitle:@"OK"];
        [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse __unused r) {}];
        return;
    } else {
        _db->deleteSchedule(s.id);  // one-off, or a past airing a rule won't recreate: remove outright
    }
    [self reload];
    [self notifyChange];
}

- (void)toggleRule:(id)__unused sender {
    const NSInteger row = _ruleTable.selectedRow;
    if (row < 0 || row >= (NSInteger)_rules.size() || !_db) return;
    const auto& r = _rules[(size_t)row];
    _db->setRuleEnabled(r.id, !r.enabled);
    [self reload];
    [self notifyChange];  // re-enabling should force a rule expansion (host does it)
}

- (void)deleteRule:(id)__unused sender {
    const NSInteger row = _ruleTable.selectedRow;
    if (row < 0 || row >= (NSInteger)_rules.size() || !_db) return;
    const long long id = _rules[(size_t)row].id;
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"Delete this series rule?";
    a.informativeText = @"Its still-pending queued recordings are removed too. Recordings that "
                        @"already ran are kept.";
    [a addButtonWithTitle:@"Delete"];
    [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn || !self->_db) return;
        self->_db->deleteRule(id);
        [self reload];
        [self notifyChange];
    }];
}

@end
