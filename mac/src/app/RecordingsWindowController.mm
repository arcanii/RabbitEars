// SPDX-License-Identifier: GPL-3.0-or-later
// See RecordingsWindowController.h.
#import "RecordingsWindowController.h"
#import "Tr.h"

#include "core/RecordingRules.h"  // normaliseTvgId — resolve a rule's channel to the library
#include "db/Database.h"
#include "models/Channel.h"
#include "models/RecordingRule.h"
#include "models/ScheduledRecording.h"
#include "platform/Encoding.h"

#include <algorithm>
#include <ctime>
#include <vector>

using rabbitears::Database;
using rabbitears::RecordingRule;
using rabbitears::ScheduledRecording;
using rabbitears::ScheduleStatus;
using namespace rabbitears;
using namespace rabbitears::i18n;  // StringId

namespace {
NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:rabbitears::utf8FromWide(w).c_str()] ?: @"";
}
std::wstring ws(NSString* s) { return rabbitears::wideFromUtf8(s.UTF8String ?: ""); }
NSString* statusText(ScheduleStatus s) {
    switch (s) {
        case ScheduleStatus::Pending:   return Tr(StringId::ScheduleStatusPending);
        case ScheduleStatus::Recording: return Tr(StringId::ScheduleStatusRecording);
        case ScheduleStatus::Done:      return Tr(StringId::ScheduleStatusDone);
        case ScheduleStatus::Missed:    return Tr(StringId::ScheduleStatusMissed);
        case ScheduleStatus::Failed:    return Tr(StringId::ScheduleStatusFailed);
        case ScheduleStatus::Cancelled: return Tr(StringId::ScheduleStatusCancelled);
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
    return TrF(StringId::GuideTimeRange, {a, b});
}
}  // namespace

@interface RecordingsWindowController () <NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate>
@end

@implementation RecordingsWindowController {
    Database*    _db;
    NSWindow*    _window;
    NSTableView* _schedTable;
    NSTableView* _ruleTable;
    NSTextField* _hint;
    // Tab items + bottom-bar buttons — held so a live language switch can relabel them (ARC file).
    NSTabViewItem* _schedTabItem;
    NSTabViewItem* _ruleTabItem;
    NSButton*    _cancelSchedBtn;
    NSButton*    _toggleRuleBtn;
    NSButton*    _delRuleBtn;
    NSButton*    _newRuleBtn;
    NSButton*    _editRuleBtn;
    NSButton*    _ruleEditOk;  // the rule-editor sheet's OK button, while it is up (else nil)
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

// Live language switch (this window is built once + reused). Relabel every built-once string in
// place — safe whether the window is open or closed, and it preserves the frame/selection/scroll
// and any in-progress rule edit (nil-and-rebuild would dangle self-referencing dataSource/delegate/
// target back-refs). Cell text (status labels, "(any channel)", time ranges) re-reads Tr on reload;
// the rule-editor sheet + all NSAlerts are built on demand and auto-update.
- (void)relabelForLanguageChange {
    if (!_window) return;
    _window.title = Tr(StringId::MacRecordingsWindowTitle);
    _schedTabItem.label = Tr(StringId::MacRecordingsTabScheduled);
    _ruleTabItem.label  = Tr(StringId::MacRecordingsTabSeriesRules);

    [_schedTable tableColumnWithIdentifier:@"chan"].title   = Tr(StringId::LabelChannel);
    [_schedTable tableColumnWithIdentifier:@"title"].title  = Tr(StringId::ScheduleColTitle);
    [_schedTable tableColumnWithIdentifier:@"when"].title   = Tr(StringId::ScheduleColWhen);
    [_schedTable tableColumnWithIdentifier:@"fmt"].title    = Tr(StringId::MacRecordingsColFormat);
    [_schedTable tableColumnWithIdentifier:@"status"].title = Tr(StringId::ScheduleColStatus);

    [_ruleTable tableColumnWithIdentifier:@"on"].title    = Tr(StringId::RuleStateEnabled);
    [_ruleTable tableColumnWithIdentifier:@"chan"].title  = Tr(StringId::LabelChannel);
    [_ruleTable tableColumnWithIdentifier:@"title"].title = Tr(StringId::MacRecordingsColMatchesTitle);
    [_ruleTable tableColumnWithIdentifier:@"fmt"].title   = Tr(StringId::MacRecordingsColFormat);

    _cancelSchedBtn.title = Tr(StringId::MacRecordingsCancelSelectedButton);
    _toggleRuleBtn.title  = Tr(StringId::MacRecordingsToggleRuleButton);
    _delRuleBtn.title     = Tr(StringId::MacRecordingsDeleteRuleButton);
    _newRuleBtn.title     = Tr(StringId::ScheduleManagerNewButton);
    _editRuleBtn.title    = Tr(StringId::RuleEditButton);
    _hint.stringValue     = Tr(StringId::MacRecordingsWakeHint);

    [_schedTable reloadData];  // re-Tr the cell text (status/format/time-range)
    [_ruleTable reloadData];
    [_schedTable.headerView setNeedsDisplay:YES];
    [_ruleTable.headerView setNeedsDisplay:YES];
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
    _window.title = Tr(StringId::MacRecordingsWindowTitle);
    _window.releasedWhenClosed = NO;
    _window.contentMinSize = NSMakeSize(520, 320);

    NSView* cv = _window.contentView;
    NSTabView* tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(12, 78, frame.size.width - 24, frame.size.height - 90)];
    tabs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [cv addSubview:tabs];

    // Scheduled tab.
    NSTabViewItem* schedItem = [[NSTabViewItem alloc] initWithIdentifier:@"sched"];
    schedItem.label = Tr(StringId::MacRecordingsTabScheduled);
    NSScrollView* schedScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 660, 320)];
    _schedTable = [self makeTableInScroll:schedScroll
                                  columns:@[ Tr(StringId::LabelChannel), Tr(StringId::ScheduleColTitle),
                                             Tr(StringId::ScheduleColWhen), Tr(StringId::MacRecordingsColFormat),
                                             Tr(StringId::ScheduleColStatus) ]
                                   widths:@[ @150, @200, @210, @60, @100 ]
                                      ids:@[ @"chan", @"title", @"when", @"fmt", @"status" ]];
    schedItem.view = schedScroll;
    [tabs addTabViewItem:schedItem];
    _schedTabItem = schedItem;

    // Series rules tab.
    NSTabViewItem* ruleItem = [[NSTabViewItem alloc] initWithIdentifier:@"rules"];
    ruleItem.label = Tr(StringId::MacRecordingsTabSeriesRules);
    NSScrollView* ruleScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 660, 320)];
    _ruleTable = [self makeTableInScroll:ruleScroll
                                 columns:@[ Tr(StringId::RuleStateEnabled), Tr(StringId::LabelChannel),
                                            Tr(StringId::MacRecordingsColMatchesTitle), Tr(StringId::MacRecordingsColFormat) ]
                                  widths:@[ @70, @180, @280, @70 ]
                                     ids:@[ @"on", @"chan", @"title", @"fmt" ]];
    ruleItem.view = ruleScroll;
    [tabs addTabViewItem:ruleItem];
    _ruleTabItem = ruleItem;

    // Bottom bar: action buttons + the honest wake note (Phase 7).
    NSButton* cancelSched = [NSButton buttonWithTitle:Tr(StringId::MacRecordingsCancelSelectedButton)
                                               target:self action:@selector(cancelSchedule:)];
    cancelSched.frame = NSMakeRect(12, 44, 150, 26);
    cancelSched.autoresizingMask = NSViewMaxYMargin;
    [cv addSubview:cancelSched];
    _cancelSchedBtn = cancelSched;
    NSButton* toggleRule = [NSButton buttonWithTitle:Tr(StringId::MacRecordingsToggleRuleButton)
                                              target:self action:@selector(toggleRule:)];
    toggleRule.frame = NSMakeRect(170, 44, 150, 26);
    [cv addSubview:toggleRule];
    _toggleRuleBtn = toggleRule;
    NSButton* delRule = [NSButton buttonWithTitle:Tr(StringId::MacRecordingsDeleteRuleButton)
                                           target:self action:@selector(deleteRule:)];
    delRule.frame = NSMakeRect(324, 44, 110, 26);
    [cv addSubview:delRule];
    _delRuleBtn = delRule;
    // New… / Edit… a series rule (the mac peer of Win32's Recording-Rules manager). Rules could
    // previously only be born from the guide's "Record Series"; these create/edit them directly.
    NSButton* newRule = [NSButton buttonWithTitle:Tr(StringId::ScheduleManagerNewButton)
                                           target:self action:@selector(newRule:)];
    newRule.frame = NSMakeRect(444, 44, 100, 26);
    [cv addSubview:newRule];
    _newRuleBtn = newRule;
    NSButton* editRule = [NSButton buttonWithTitle:Tr(StringId::RuleEditButton)
                                            target:self action:@selector(editRule:)];
    editRule.frame = NSMakeRect(548, 44, 100, 26);
    [cv addSubview:editRule];
    _editRuleBtn = editRule;
    // Double-clicking a rule row edits it.
    _ruleTable.target = self;
    _ruleTable.doubleAction = @selector(editRule:);

    _hint = [NSTextField labelWithString:Tr(StringId::MacRecordingsWakeHint)];
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
        else if ([cid isEqualToString:@"chan"])  text = r.channelName.empty() ? Tr(StringId::RuleAnyChannelPlaceholder) : ns(r.channelName);
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
        a.messageText = Tr(StringId::MacRecordingsAiringQueuedByRuleHeading);
        a.informativeText = Tr(StringId::MacRecordingsAiringQueuedByRuleBody);
        [a addButtonWithTitle:Tr(StringId::ButtonOk)];
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
    a.messageText = Tr(StringId::MacRecordingsDeleteRuleConfirmHeading);
    a.informativeText = Tr(StringId::MacRecordingsDeleteRuleConfirmBody);
    [a addButtonWithTitle:Tr(StringId::ButtonDelete)];
    [a addButtonWithTitle:Tr(StringId::ButtonCancel)];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn || !self->_db) return;
        self->_db->deleteRule(id);
        [self reload];
        [self notifyChange];
    }];
}

// ---- rule editor (New… / Edit…) ------------------------------------------------------------
// The mac peer of Win32's ruleDialog. Rules were previously born only from the guide's "Record
// Series"; this lets the user create one from scratch or change an existing rule's criteria.

- (NSString*)trimmed:(NSString*)s {
    return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

- (void)newRule:(id)__unused sender {
    RecordingRule r;  // id==0 ⇒ new; defaults from the model (Exact, enabled, no padding)
    r.mux = L"ts";    // the crash-safe default container (mp4 loses its index on a hard crash)
    r.createdAt = (long long)time(nullptr);  // addRule binds this verbatim (no auto-stamp)
    [self presentRuleEditorForRule:r isNew:YES];
}

- (void)editRule:(id)__unused sender {
    const NSInteger row = _ruleTable.selectedRow;
    if (row < 0 || row >= (NSInteger)_rules.size()) return;  // nothing selected
    [self presentRuleEditorForRule:_rules[(size_t)row] isNew:NO];
}

- (void)presentRuleEditorForRule:(RecordingRule)rule isNew:(BOOL)isNew {
    if (!_db) return;

    // Library channels for the picker, sorted by name. Popup index 0 == "(any channel)";
    // a menu item's tag is the 1-based library index (0 == any) so a duplicate channel name
    // can't collapse the mapping (NSPopUpButton's addItemWithTitle: dedups by title — building
    // the menu with explicit items + tags avoids that).
    std::vector<Channel> chans = _db->allChannels();
    std::sort(chans.begin(), chans.end(),
              [](const Channel& a, const Channel& b) { return a.name < b.name; });

    NSInteger preSelTag = 0;  // default "(any channel)"
    if (!rule.channelId.empty()) {
        const std::wstring want = normaliseTvgId(rule.channelId);
        for (size_t i = 0; i < chans.size(); ++i)
            if (!chans[i].tvgId.empty() && normaliseTvgId(chans[i].tvgId) == want) {
                preSelTag = (NSInteger)i + 1;
                break;
            }
        if (preSelTag == 0) {  // the rule's channel has left the library — keep it selectable so
            Channel keep;      // editing another field can't silently turn it into "(any channel)"
            keep.tvgId = rule.channelId;
            keep.name  = rule.channelName.empty() ? rule.channelId : rule.channelName;
            chans.push_back(keep);
            preSelTag = (NSInteger)chans.size();
        }
    }

    // Accessory: 5 labelled rows (Channel / Title / Match / Start early / Stop late).
    const CGFloat W = 380, rowH = 26, gap = 10, labelW = 92, fieldX = 100, fieldW = W - fieldX, numW = 56;
    const CGFloat H = 5 * rowH + 4 * gap;
    NSView* form = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    auto rowY = [&](int i) { return H - (CGFloat)(i + 1) * rowH - (CGFloat)i * gap; };
    auto addLabel = [&](StringId sid, int i) {
        NSTextField* l = [NSTextField labelWithString:Tr(sid)];
        l.frame = NSMakeRect(0, rowY(i) + 4, labelW, rowH - 6);
        l.alignment = NSTextAlignmentRight;
        [form addSubview:l];
    };
    addLabel(StringId::ScheduleFieldChannel, 0);
    addLabel(StringId::ScheduleFieldTitle,   1);
    addLabel(StringId::RuleFieldMatch,       2);
    addLabel(StringId::RuleFieldLead,        3);
    addLabel(StringId::RuleFieldTrail,       4);

    NSPopUpButton* chanPop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(fieldX, rowY(0), fieldW, rowH)
                                                        pullsDown:NO];
    NSMenuItem* anyItem = [[NSMenuItem alloc] initWithTitle:Tr(StringId::RuleAnyChannelPlaceholder)
                                                     action:nil keyEquivalent:@""];
    anyItem.tag = 0;
    [chanPop.menu addItem:anyItem];
    for (size_t i = 0; i < chans.size(); ++i) {
        NSString* t = chans[i].name.empty() ? ns(chans[i].tvgId) : ns(chans[i].name);
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:t action:nil keyEquivalent:@""];
        it.tag = (NSInteger)i + 1;
        [chanPop.menu addItem:it];
    }
    [chanPop selectItemWithTag:preSelTag];

    NSTextField* titleField = [[NSTextField alloc] initWithFrame:NSMakeRect(fieldX, rowY(1), fieldW, rowH)];
    titleField.stringValue = ns(rule.titleMatch);
    titleField.delegate = self;

    NSPopUpButton* matchPop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(fieldX, rowY(2), fieldW, rowH)
                                                         pullsDown:NO];
    [matchPop.menu addItem:[[NSMenuItem alloc] initWithTitle:Tr(StringId::RuleMatchExact) action:nil keyEquivalent:@""]];
    [matchPop.menu addItem:[[NSMenuItem alloc] initWithTitle:Tr(StringId::RuleMatchContains) action:nil keyEquivalent:@""]];
    [matchPop selectItemAtIndex:(rule.match == RuleMatch::Contains ? 1 : 0)];

    NSTextField* leadField = [[NSTextField alloc] initWithFrame:NSMakeRect(fieldX, rowY(3), numW, rowH)];
    leadField.stringValue = [NSString stringWithFormat:@"%d", rule.leadSec / 60];
    leadField.alignment = NSTextAlignmentRight;
    NSTextField* trailField = [[NSTextField alloc] initWithFrame:NSMakeRect(fieldX, rowY(4), numW, rowH)];
    trailField.stringValue = [NSString stringWithFormat:@"%d", rule.trailSec / 60];
    trailField.alignment = NSTextAlignmentRight;
    auto addMin = [&](int i) {
        NSTextField* m = [NSTextField labelWithString:Tr(StringId::RuleMinutesSuffix)];
        m.frame = NSMakeRect(fieldX + numW + 6, rowY(i) + 4, 80, rowH - 6);
        m.textColor = NSColor.secondaryLabelColor;
        [form addSubview:m];
    };
    addMin(3);
    addMin(4);
    for (NSView* v in @[ chanPop, titleField, matchPop, leadField, trailField ]) [form addSubview:v];

    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = isNew ? Tr(StringId::RuleEditNewTitle) : Tr(StringId::RuleEditWindowTitle);
    alert.accessoryView = form;
    NSButton* ok = [alert addButtonWithTitle:Tr(StringId::ButtonOk)];
    [alert addButtonWithTitle:Tr(StringId::ButtonCancel)];
    _ruleEditOk = ok;
    ok.enabled = [self trimmed:titleField.stringValue].length > 0;  // a rule needs a title
    alert.window.initialFirstResponder = titleField;

    const long long ruleId = rule.id;
    __block RecordingRule carried = rule;  // preserve enabled / mux / createdAt across the edit
    [alert beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        titleField.delegate = nil;   // stop controlTextDidChange: before the controls go away
        self->_ruleEditOk = nil;
        if (resp != NSAlertFirstButtonReturn || !self->_db) return;
        NSString* title = [self trimmed:titleField.stringValue];
        if (title.length == 0) return;  // OK was gated on this; re-check defensively

        RecordingRule r = carried;
        r.id = ruleId;
        r.titleMatch = ws(title);
        r.match = (matchPop.indexOfSelectedItem == 1) ? RuleMatch::Contains : RuleMatch::Exact;
        // Clamp to 0..240 min, matching the Win32 editor (Dialogs.cpp readMinutes) — the rule
        // engine's padding invariants assume non-negative, bounded lead/trail.
        r.leadSec  = std::clamp((int)leadField.intValue, 0, 240) * 60;
        r.trailSec = std::clamp((int)trailField.intValue, 0, 240) * 60;
        const NSInteger tag = chanPop.selectedItem.tag;  // 0 == any; k == chans[k-1]
        if (tag <= 0) {
            r.channelId.clear();
            r.channelName.clear();
        } else if ((size_t)(tag - 1) < chans.size()) {
            r.channelId   = chans[(size_t)(tag - 1)].tvgId;
            r.channelName = chans[(size_t)(tag - 1)].name;
        }

        if (isNew) {
            self->_db->addRule(r);
        } else {
            self->_db->updateRule(r);
            self->_db->clearPendingForRule(ruleId);  // stale predictions no longer match the criteria
        }
        [self reload];
        [self notifyChange];  // host re-expands the rules against the new/edited criteria
    }];
}

- (void)controlTextDidChange:(NSNotification*)note {
    // Gate the rule editor's OK on a non-empty (trimmed) title.
    if (_ruleEditOk)
        _ruleEditOk.enabled = [self trimmed:((NSTextField*)note.object).stringValue].length > 0;
}

@end
