// SPDX-License-Identifier: GPL-3.0-or-later
// See TvGuideWindowController.h. The behaviour mirrors Win32/ui/EpgGuideControl.cpp + the host side
// of Win32 onEpgGuide (MainWindowCommands.cpp); comments name the Win32 function each part follows.
#import "TvGuideWindowController.h"

#import "EpgGuideView.h"
#import "Tr.h"

#include "core/SearchFold.h"  // channel-name matching: searchFold (as the Win32 guide matches names)
#include "db/Database.h"
#include "models/Programme.h"
#include "platform/Encoding.h"
#include "platform/Log.h"
#include "ui/GuideModel.h"  // buildGuideModel — the guide's rows + coverage (shared with Win32)

#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using rabbitears::Database;
using rabbitears::GuideCoverage;
using rabbitears::GuideRow;
using namespace rabbitears;         // Tr / TrF
using namespace rabbitears::i18n;   // StringId

namespace {
NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:rabbitears::utf8FromWide(w).c_str()] ?: @"";
}
std::wstring ws(NSString* s) { return rabbitears::wideFromUtf8(s.UTF8String ?: ""); }
NSString* num(long long n) { return [NSString stringWithFormat:@"%lld", n]; }

// The same normalisation the guide builds rows with: strip an '@feed' suffix, ASCII-lowercase.
std::wstring normId(std::wstring s) {
    if (const size_t at = s.find(L'@'); at != std::wstring::npos) s.resize(at);
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return s;
}

// `s` through searchFold: case and accents folded as the programme search's index folds them,
// plus Ø Đ Ħ Ŀ Ł Ŧ, one character for one (Win32 foldCopy).
std::wstring foldCopy(const std::wstring& s) {
    std::wstring o(s);
    for (wchar_t& c : o) c = searchFold(c);
    return o;
}
// The one channel-name match, for the results' "Matching channels" count AND the filter the
// Channels item applies, so the two always agree (Win32 channelMatches).
bool channelMatches(const GuideRow& row, const std::wstring& foldedNeedle) {
    return foldCopy(row.channelName).find(foldedNeedle) != std::wstring::npos;
}
// `s` with each channelMatches-style occurrence of `foldedNeedle` wrapped in U+0002 … U+0003.
std::wstring markFolded(const std::wstring& s, const std::wstring& foldedNeedle) {
    if (foldedNeedle.empty()) return s;
    const std::wstring f = foldCopy(s);
    std::wstring o;
    size_t i = 0;
    for (size_t at; (at = f.find(foldedNeedle, i)) != std::wstring::npos; i = at + foldedNeedle.size()) {
        o.append(s, i, at - i);
        o += L'\x02';
        o.append(s, at, foldedNeedle.size());
        o += L'\x03';
    }
    return o.append(s, i, std::wstring::npos);
}
// Spaces off both ends — U+3000 too, the ideographic space a Japanese IME's Space types.
std::wstring trimmed(const std::wstring& s) {
    const size_t b = s.find_first_not_of(L" \t\r\n　");
    if (b == std::wstring::npos) return L"";
    return s.substr(b, s.find_last_not_of(L" \t\r\n　") - b + 1);
}

// Whether a programme can be played from an archive: it has started (aired, or is airing — "start
// over"), and the archive reaches back to its start (Win32 guideCanPlayFromStart).
bool canPlayFromStart(long long archiveChannel, int archiveDays, long long startUtc, long long nowUtc) {
    return archiveChannel > 0 && archiveDays > 0 && startUtc < nowUtc &&
           startUtc >= nowUtc - static_cast<long long>(archiveDays) * 86400;
}

// The coverage line and the explanation behind it (Win32 guideCoverageSummary/Explanation).
NSString* coverageSummary(const GuideCoverage& c) {
    return TrF(StringId::GuideCoverageLine, {num(c.shown), num(c.withId)});
}
NSString* coverageExplanation(const GuideCoverage& c) {
    NSMutableArray<NSString*>* paras = [NSMutableArray array];
    if (c.inNoLinkPlaylists > 0) [paras addObject:TrF(StringId::GuideCoverageNoLink, {num(c.inNoLinkPlaylists)})];
    if (c.noProgrammes > 0) [paras addObject:TrF(StringId::GuideCoverageNoProgrammes, {num(c.noProgrammes)})];
    if (c.guideUnmatched > 0) [paras addObject:TrF(StringId::GuideCoverageUnmatched, {num(c.guideUnmatched)})];
    [paras addObject:Tr(StringId::GuideCoverageNoId)];
    return [paras componentsJoinedByString:@"\n\n"];
}

long long localDay(long long epoch) {
    NSCalendar* cal = NSCalendar.currentCalendar;
    NSDate* d = [cal startOfDayForDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)epoch]];
    return (long long)d.timeIntervalSince1970;
}
NSString* dayLabel(long long epoch, long long nowUtc) {
    const long long d = localDay(epoch);
    if (d == localDay(nowUtc)) return Tr(StringId::GuideSearchToday);
    if (d == localDay(nowUtc + 24 * 3600)) return Tr(StringId::GuideSearchTomorrow);
    if (d == localDay(nowUtc - 24 * 3600)) return Tr(StringId::GuideSearchYesterday);  // catch-up
    static NSDateFormatter* fmt = nil;
    if (!fmt) {
        fmt = [[NSDateFormatter alloc] init];
        fmt.dateStyle = NSDateFormatterFullStyle;
        fmt.timeStyle = NSDateFormatterNoStyle;
    }
    return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)epoch]];
}

// A results-list line (Win32 ItemKind / ResultItem).
enum class ItemKind {
    Section,   // a block's heading, in the accent: channels, programmes found, description, catch-up
    Heading,   // a day ("Today", "Tomorrow", a date)
    Note,      // "Also in your channel list, not in the guide: N" — plain text
    Channels,  // "Show only channels matching …" — choosing it applies the channel filter
    Hit,       // one programme
};
struct ResultItem {
    ItemKind kind = ItemKind::Hit;
    std::wstring label;
    int hit = -1;  // Hit: index into _hits
};
bool selectable(const ResultItem& it) { return it.kind == ItemKind::Channels || it.kind == ItemKind::Hit; }

// `marked` (matches wrapped in U+0002 … U+0003) as attributed text: the matches bold in the accent.
NSAttributedString* markedText(const std::wstring& marked, NSFont* font, NSColor* base, NSColor* mark) {
    NSMutableAttributedString* out = [[NSMutableAttributedString alloc] init];
    NSFont* bold = [NSFontManager.sharedFontManager convertFont:font toHaveTrait:NSBoldFontMask];
    std::wstring run;
    bool inMark = false;
    auto flush = [&] {
        if (run.empty()) return;
        [out appendAttributedString:[[NSAttributedString alloc]
                                        initWithString:ns(run)
                                            attributes:@{NSFontAttributeName: inMark ? bold : font,
                                                         NSForegroundColorAttributeName: inMark ? mark : base}]];
        run.clear();
    };
    for (wchar_t c : marked) {
        if (c == L'\x02') { flush(); inMark = true; continue; }
        if (c == L'\x03') { flush(); inMark = false; continue; }
        run += c == L'\n' || c == L'\r' ? L' ' : c;
    }
    flush();
    return out;
}
}  // namespace

// The results table: Return opens the selected item (Win32: Enter on the list).
@interface REGuideResultsTable : NSTableView
@property (nonatomic, copy) void (^onReturn)(void);
@end
@implementation REGuideResultsTable
- (void)keyDown:(NSEvent*)e {
    if ((e.keyCode == 36 || e.keyCode == 76) && self.onReturn) { self.onReturn(); return; }  // Return / Enter
    [super keyDown:e];
}
@end

@interface TvGuideWindowController () <EpgGuideViewDelegate, NSTableViewDataSource, NSTableViewDelegate,
                                       NSSearchFieldDelegate, NSMenuDelegate>
@end

@implementation TvGuideWindowController {
    Database*     _db;
    NSWindow*     _window;
    NSView*       _toolbar;
    NSSearchField* _search;
    NSButton*     _nowBtn;
    NSButton*     _coverageBtn;
    EpgGuideView* _guideView;
    NSScrollView* _resultsScroll;
    REGuideResultsTable* _results;
    void (^_onPlay)(NSString*, NSString*);

    // ---- the rows (Win32 GuideState::allRows / rows / filter)
    std::vector<GuideRow> _allRows;
    NSArray<REGuideRow*>* _allViewRows;  // _allRows bridged once, same order
    GuideCoverage _coverage;
    std::wstring  _filter;               // the channel filter a search's Channels item applied
    unsigned      _rowsGeneration;       // bumped on every rebuild of _allRows
    unsigned      _hitsGeneration;       // the rows generation the current results were found against

    // ---- search (Win32 GuideState search fields)
    NSTimer*     _searchTimer;
    BOOL         _sessionReady;  // onSearchBegin ran for this burst of typing
    BOOL         _preparing;     // "Preparing search…" is showing while the session begins
    BOOL         _showingResults;
    std::wstring _searchedText;
    std::vector<Database::ProgrammeHit> _hits;
    bool         _hitsTruncated, _pastTruncated;
    long long    _searchNowUtc;
    int          _channelCount, _moreChannels;
    std::wstring _channelNames;  // the first matching names, marked, " · "-joined
    std::vector<ResultItem> _items;
}

- (instancetype)initWithDatabase:(Database*)db {
    if ((self = [super init])) _db = db;
    return self;
}

// ---- building the rows (Win32 onEpgGuide → buildGuideModel) -------------------------------------

// Rebuild _allRows + coverage from the DB. Returns NO when there is nothing to show.
- (BOOL)rebuildModel {
    if (!_db) return NO;
    const auto t0 = std::chrono::steady_clock::now();
    const long long now = (long long)time(nullptr);
    rabbitears::GuideModel model = rabbitears::buildGuideModel(*_db, now);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0).count();
    diag::info(L"TV guide: " + std::to_wstring(model.rows.size()) + L" rows in " + std::to_wstring(ms) +
               L" ms (" + model.coverageLog + L")");
    _coverage = model.coverage;
    _allRows = std::move(model.rows);
    ++_rowsGeneration;
    NSMutableArray<REGuideRow*>* out = [NSMutableArray arrayWithCapacity:_allRows.size()];
    NSString* noTitle = Tr(StringId::MacTvGuideNoTitle);
    for (const GuideRow& r : _allRows) {
        REGuideRow* row = [[REGuideRow alloc] init];
        row.channelId = ns(r.channelId);
        row.channelName = r.channelName.empty() ? row.channelId : ns(r.channelName);
        row.archiveChannel = r.archiveChannel;
        row.archiveDays = r.archiveDays;
        row.playlistId = r.playlistId;
        NSMutableArray<REGuideProgramme*>* progs = [NSMutableArray arrayWithCapacity:r.programmes.size()];
        for (const auto& p : r.programmes) {
            REGuideProgramme* gp = [[REGuideProgramme alloc] init];
            gp.title = p.title.empty() ? noTitle : ns(p.title);
            gp.descr = ns(p.descr);
            gp.startUtc = p.startUtc;
            gp.stopUtc = p.stopUtc;
            [progs addObject:gp];
        }
        row.programmes = progs;
        [out addObject:row];
    }
    _allViewRows = out;
    return !_allRows.empty();
}

// The rows the grid shows: all of them, or those the channel filter keeps (Win32 setFilter).
- (NSArray<REGuideRow*>*)visibleRows {
    if (_filter.empty()) return _allViewRows;
    const std::wstring needle = foldCopy(_filter);
    NSMutableArray<REGuideRow*>* out = [NSMutableArray array];
    for (size_t i = 0; i < _allRows.size(); ++i)
        if (channelMatches(_allRows[i], needle)) [out addObject:_allViewRows[i]];
    return out;
}

- (void)applyRowsKeepingScroll:(BOOL)keep {
    [_guideView setRows:[self visibleRows] nowUtc:(long long)time(nullptr) keepScroll:keep];
    _guideView.filterLabel = _filter.empty() ? nil : ns(_filter);
}

- (void)setFilter:(const std::wstring&)text {
    _filter = text;
    [self applyRowsKeepingScroll:YES];
}

- (void)updateCoverageButton {
    _coverageBtn.title = _coverage.valid ? [@"ⓘ  " stringByAppendingString:coverageSummary(_coverage)] : @"";
    _coverageBtn.hidden = !_coverage.valid;
}

// "No guide to show", with what the counts say about THIS library (Win32 onEpgGuide).
- (void)showNoGuideOver:(NSWindow*)parent {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = Tr(StringId::GuideNoGuideHeading);
    NSString* body = Tr(StringId::MacTvGuideNoGuideBody);
    if (_coverage.valid)
        body = [NSString stringWithFormat:@"%@\n\n%@\n\n%@", body, coverageSummary(_coverage),
                                          coverageExplanation(_coverage)];
    a.informativeText = body;
    [a addButtonWithTitle:Tr(StringId::ButtonOk)];
    [a beginSheetModalForWindow:(parent ?: NSApp.keyWindow) completionHandler:^(NSModalResponse __unused r) {}];
}

// ---- presenting ---------------------------------------------------------------------------------

- (void)presentRelativeTo:(NSWindow*)parent
                   onPlay:(void (^)(NSString*, NSString*))onPlay {
    [self presentRelativeTo:parent showChannel:nil onPlay:onPlay];
}

- (REGuideShowResult)presentRelativeTo:(NSWindow*)parent
                           showChannel:(NSString*)tvgId
                                onPlay:(void (^)(NSString*, NSString*))onPlay {
    _onPlay = [onPlay copy];
    if (![self rebuildModel]) {
        [self showNoGuideOver:parent];
        return REGuideShowNoGuide;
    }
    if (!_window) [self buildWindowNear:parent];
    // A re-open shows the grid, not any search results; the search box keeps its text and the next
    // search starts a fresh session. The new rows clear a channel filter (Win32 showEpgGuide).
    _filter.clear();
    [self showResultsList:NO];
    _sessionReady = NO;
    [_guideView setRows:[self visibleRows] nowUtc:(long long)time(nullptr)];
    _guideView.filterLabel = nil;
    [self updateCoverageButton];
    [_window makeKeyAndOrderFront:nil];

    if (tvgId.length == 0) return REGuideShowRevealed;
    const std::wstring want = normId(ws(tvgId));
    NSArray<REGuideRow*>* shown = _guideView.rows;
    for (NSInteger i = 0; i < (NSInteger)shown.count; ++i)
        if (normId(ws(shown[(NSUInteger)i].channelId)) == want) {
            [_guideView revealRowAtIndex:i];
            return REGuideShowRevealed;
        }
    return REGuideShowChannelMissing;
}

- (void)guideDidChange {
    if (!_window) return;
    _sessionReady = NO;  // the next search re-reads the channel set (and the index)
    if (![self rebuildModel]) {
        [_guideView setRows:@[] nowUtc:(long long)time(nullptr)];
        [self updateCoverageButton];
        return;
    }
    [self applyRowsKeepingScroll:YES];
    [self updateCoverageButton];
    if (_showingResults) [self runSearch];
}

- (void)archiveDidChange {
    if (!_window || !_db) return;
    const rabbitears::GuideArchivePicks picks(*_db);
    for (size_t i = 0; i < _allRows.size(); ++i) {
        const rabbitears::GuideArchivePick pk = picks.pick(_allRows[i].playlistId, _allRows[i].channelId);
        _allRows[i].archiveChannel = pk.channel;
        _allRows[i].archiveDays = pk.days;
        _allViewRows[i].archiveChannel = pk.channel;  // the same objects the grid draws from
        _allViewRows[i].archiveDays = pk.days;
    }
    for (auto& h : _hits) {
        const rabbitears::GuideArchivePick pk = picks.pick(h.playlistId, h.channelTvgId);
        h.archiveChannelId = pk.channel;
        h.archiveDays = pk.days;
    }
    _sessionReady = NO;  // the next search re-reads the channel set (the search's own picks)
    if (_showingResults && !_preparing) {
        __weak TvGuideWindowController* weak = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(200 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
            TvGuideWindowController* s = weak;
            if (!s || !s->_showingResults) return;
            const NSInteger sel = s->_results.selectedRow;
            const CGFloat y = s->_resultsScroll.contentView.bounds.origin.y;
            [s beginSearchSessionMayRebuild:NO];  // quietly: no "Preparing search…", no index rebuild
            s->_sessionReady = YES;
            [s runSearch];
            [s->_resultsScroll.contentView scrollToPoint:NSMakePoint(0, y)];
            if (sel >= 0 && sel < s->_results.numberOfRows)
                [s->_results selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)sel] byExtendingSelection:NO];
        });
    }
}

- (void)buildWindowNear:(NSWindow*)parent {
    const NSRect frame = NSMakeRect(0, 0, 980, 600);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = Tr(StringId::TvGuideTitle);
    _window.releasedWhenClosed = NO;  // we own it via the ivar; a later re-open reuses it
    _window.contentMinSize = NSMakeSize(620, 380);

    NSView* content = [[NSView alloc] initWithFrame:frame];
    _window.contentView = content;
    const CGFloat tbH = 40;

    // ---- toolbar: [search] [Now] … [ⓘ coverage]
    _toolbar = [[NSView alloc] initWithFrame:NSMakeRect(0, frame.size.height - tbH, frame.size.width, tbH)];
    _toolbar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:_toolbar];

    _search = [[NSSearchField alloc] initWithFrame:NSMakeRect(10, 8, 300, 24)];
    _search.placeholderString = Tr(StringId::GuideSearchCue);
    _search.sendsSearchStringImmediately = YES;  // our own 200 ms debounce below (Win32 kSearchDebounceMs)
    _search.sendsWholeSearchString = NO;
    _search.target = self;
    _search.action = @selector(searchChanged:);
    _search.delegate = self;
    [_toolbar addSubview:_search];

    _nowBtn = [NSButton buttonWithTitle:Tr(StringId::GuideNowButton) target:self action:@selector(nowPressed:)];
    _nowBtn.frame = NSMakeRect(318, 6, 70, 28);
    [_toolbar addSubview:_nowBtn];

    _coverageBtn = [NSButton buttonWithTitle:@"" target:self action:@selector(coveragePressed:)];
    _coverageBtn.bordered = NO;
    _coverageBtn.font = [NSFont systemFontOfSize:11];
    _coverageBtn.contentTintColor = NSColor.secondaryLabelColor;
    _coverageBtn.alignment = NSTextAlignmentRight;
    _coverageBtn.lineBreakMode = NSLineBreakByTruncatingTail;
    _coverageBtn.frame = NSMakeRect(400, 8, frame.size.width - 410, 24);
    _coverageBtn.autoresizingMask = NSViewWidthSizable;
    [_toolbar addSubview:_coverageBtn];

    // ---- the grid, and the results list in the same place
    const NSRect body = NSMakeRect(0, 0, frame.size.width, frame.size.height - tbH);
    _guideView = [[EpgGuideView alloc] initWithFrame:body];
    _guideView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _guideView.delegate = self;
    [content addSubview:_guideView];

    _resultsScroll = [[NSScrollView alloc] initWithFrame:body];
    _resultsScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _resultsScroll.hasVerticalScroller = YES;
    _results = [[REGuideResultsTable alloc] initWithFrame:body];
    // The one column spans the table. It was created at NSTableColumn's default 100 pt and only
    // ever resized on a LATER table resize, so every result wrapped at ~100 pt — "Upcoming pro…",
    // a title, a time and a channel each cut on its own line (found on device, 2026-11). Start it at
    // the full width and let it follow the table; -showResultsList: re-fits it too.
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"r"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    col.width = body.size.width;
    [_results addTableColumn:col];
    _results.headerView = nil;
    _results.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _results.dataSource = self;
    _results.delegate = self;
    _results.target = self;
    _results.action = @selector(resultClicked:);
    _results.doubleAction = @selector(resultDoubleClicked:);
    _results.intercellSpacing = NSMakeSize(0, 0);
    _results.style = NSTableViewStylePlain;
    __weak TvGuideWindowController* weak = self;
    _results.onReturn = ^{ [weak openSelectedResult]; };
    NSMenu* menu = [[NSMenu alloc] init];
    menu.delegate = self;
    _results.menu = menu;
    _resultsScroll.documentView = _results;
    _resultsScroll.hidden = YES;
    [content addSubview:_resultsScroll];

    _window.frameAutosaveName = @"RabbitEarsGuideWindow";  // remember size/position
    if (![_window setFrameUsingName:@"RabbitEarsGuideWindow"]) {
        if (parent) {  // offset from the main window on first open
            const NSRect pf = parent.frame;
            [_window setFrameOrigin:NSMakePoint(NSMinX(pf) + 30, NSMaxY(pf) - frame.size.height - 30)];
        } else {
            [_window center];
        }
    }
}

- (void)hide { [_window orderOut:nil]; }

// Live language switch: the toolbar's labels, then the rows (they bake in the translated "(no
// title)" placeholder) and any results list (its headings are translated text).
- (void)relabelForLanguageChange {
    if (!_window) return;
    _window.title = Tr(StringId::TvGuideTitle);
    _search.placeholderString = Tr(StringId::GuideSearchCue);
    _nowBtn.title = Tr(StringId::GuideNowButton);
    if ([self rebuildModel]) [self applyRowsKeepingScroll:YES];
    [self updateCoverageButton];
    if (_showingResults) {
        [self buildItems];
        [_results reloadData];
    }
}

// ---- toolbar ------------------------------------------------------------------------------------

- (void)nowPressed:(id)__unused sender {
    [self showResultsList:NO];
    [_guideView scrollToNow];
}

- (void)coveragePressed:(id)__unused sender {
    if (!_coverage.valid) return;
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = coverageSummary(_coverage);
    a.informativeText = coverageExplanation(_coverage);
    [a addButtonWithTitle:Tr(StringId::ButtonOk)];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse __unused r) {}];
}

// ---- search (Win32 runSearch / findChannels / buildItems) ----------------------------------------

- (void)searchChanged:(id)__unused sender {
    [_searchTimer invalidate];
    __weak TvGuideWindowController* weak = self;
    _searchTimer = [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:NO block:^(NSTimer* __unused t) {
        [weak runSearch];
    }];
}

// The search field's keys: Return opens the selected result (the Channels item filters — type +
// Return narrows the grid), ↓/↑ move the selection, Esc clears the box.
- (BOOL)control:(NSControl*)__unused control textView:(NSTextView*)__unused tv doCommandBySelector:(SEL)cmd {
    if (cmd == @selector(insertNewline:)) {
        [_searchTimer invalidate];
        if (!_showingResults || _searchedText != trimmed(ws(_search.stringValue))) [self runSearch];
        [self openSelectedResult];
        return YES;
    }
    if (cmd == @selector(moveDown:) || cmd == @selector(moveUp:)) {
        if (!_showingResults) return NO;
        const NSInteger dir = cmd == @selector(moveDown:) ? 1 : -1;
        NSInteger i = _results.selectedRow;
        for (NSInteger j = i + dir; j >= 0 && j < (NSInteger)_items.size(); j += dir)
            if (selectable(_items[(size_t)j])) { i = j; break; }
        if (i >= 0) {
            [_results selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
            [_results scrollRowToVisible:i];
        }
        return YES;
    }
    return NO;
}

- (void)showResultsList:(BOOL)show {
    _showingResults = show;
    _resultsScroll.hidden = !show;
    _guideView.hidden = show;
    if (show) [_results sizeLastColumnToFit];  // the window may have been resized while it was hidden
}

- (void)clearResults {
    _hits.clear();
    _channelCount = 0;
    _channelNames.clear();
    _moreChannels = 0;
    _items.clear();
    _searchedText.clear();
}

- (void)runSearch {
    if (!_db) return;
    const std::wstring text = trimmed(ws(_search.stringValue));
    if (text.empty()) {
        [self clearResults];
        _sessionReady = NO;  // the next burst re-reads the channel set (playlists may change)
        [_results reloadData];
        [self showResultsList:NO];
        return;
    }
    if (_preparing) return;  // a session is beginning; it searches for the box's text once it has
    if (!_sessionReady) {
        // Say so before the (possibly seconds-long) index rebuild blocks the main thread, then begin
        // on the next turn of the run loop, once "Preparing search…" is on screen.
        [self clearResults];
        _preparing = YES;
        [self buildItems];
        [_results reloadData];
        [self showResultsList:YES];
        [_window displayIfNeeded];
        __weak TvGuideWindowController* weak = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            TvGuideWindowController* s = weak;
            if (!s) return;
            [s beginSearchSession];
            s->_preparing = NO;
            s->_sessionReady = YES;
            [s runSearch];
        });
        return;
    }
    [self findChannels:text];
    _hitsTruncated = _pastTruncated = false;
    _searchNowUtc = (long long)time(nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    // With catch-up: programmes already aired, on channels whose archive still holds them, after the
    // upcoming ones (listed under their own heading).
    _hits = _db->searchProgrammes(text, _searchNowUtc, 200, &_hitsTruncated, /*withArchive=*/true, &_pastTruncated);
    if (diag::enabled(diag::Level::Debug))
        diag::debug(L"guide search: " + std::to_wstring(_hits.size()) + L" hits in " +
                    std::to_wstring(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - t0).count() / 1000.0) + L" ms");
    _hitsGeneration = _rowsGeneration;
    _searchedText = text;
    [self buildItems];
    [_results reloadData];
    // The first selectable item is selected: the Channels item when channels match (type + Return filters).
    for (size_t i = 0; i < _items.size(); ++i)
        if (selectable(_items[i])) {
            [_results selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
            break;
        }
    [_results scrollRowToVisible:0];
    [self showResultsList:YES];
}

// Begin a search session (Win32 cb.onSearchBegin): load the channel set and, if the index is stale
// (the first search after upgrading, a deleted playlist, a failed rebuild after a refresh), rebuild
// it — not while Refresh Guide's worker runs (it rebuilds the index itself once it has stored).
- (void)beginSearchSession { [self beginSearchSessionMayRebuild:YES]; }

// `mayRebuild` NO for the quiet re-search after new catch-up flags: no "Preparing search…" is up,
// so a stale index is searched by LIKE until the next session (Win32 onSearchBegin(mayRebuild)).
- (void)beginSearchSessionMayRebuild:(BOOL)mayRebuild {
    const auto t0 = std::chrono::steady_clock::now();
    bool rebuilt = false;
    const bool refreshing = self.isGuideRefreshing && self.isGuideRefreshing();
    if (mayRebuild && !refreshing && _db->programmeSearchState() == Database::ProgrammeSearchState::NeedsRebuild)
        rebuilt = _db->rebuildProgrammeIndex();
    const bool channels = _db->refreshProgrammeSearchChannels();
    diag::info(L"guide search session: " +
               std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0).count()) +
               L" ms" + (rebuilt ? L" (index rebuilt)" : L"") +
               (channels ? L"" : L" — channel set FAILED: " + _db->lastError()));
}

// The guide's rows whose name matches — what the Channels item would narrow the grid to — counted and
// listed by NAME (a channel in two playlists is one channel to the user); then how many more the
// channel list has by that name that the guide has no row for (Win32 findChannels).
- (void)findChannels:(const std::wstring&)text {
    constexpr int kNamesListed = 12;
    const std::wstring needle = foldCopy(text);
    std::unordered_set<std::wstring> seen;
    _channelCount = 0;
    _channelNames.clear();
    _moreChannels = 0;
    if (needle.empty()) return;
    for (const GuideRow& row : _allRows) {
        if (!channelMatches(row, needle) || !seen.insert(row.channelName).second) continue;
        if (_channelCount < kNamesListed) {
            if (!_channelNames.empty()) _channelNames += L"  ·  ";
            _channelNames += markFolded(row.channelName, needle);
        } else if (_channelCount == kNamesListed) {
            _channelNames += L"  ·  …";
        }
        ++_channelCount;
    }
    std::unordered_set<std::wstring> guideIds;
    for (const GuideRow& row : _allRows)
        if (!row.channelId.empty()) guideIds.insert(normId(row.channelId));
    const int n = _db->countUncoveredChannelNames(text, guideIds, 5000);
    if (n > 0) _moreChannels = n;
}

// Lay the results out (Win32 buildItems): the matching channels, then the upcoming programmes under
// their count, a heading per local day; then the description-only ones; then the aired ones (catch-up)
// under their own heading, their own title/description split inside.
- (void)buildItems {
    _items.clear();
    auto add = [&](ItemKind kind, const std::wstring& label, int hit) {
        ResultItem it;
        it.kind = kind;
        it.label = label;
        it.hit = hit;
        _items.push_back(std::move(it));
    };
    if (_preparing) {
        add(ItemKind::Section, ws(Tr(StringId::GuideSearchPreparing)), -1);
        return;
    }
    size_t upcoming = 0;
    for (const auto& h : _hits)
        if (h.programme.stopUtc > _searchNowUtc) ++upcoming;
    const size_t aired = _hits.size() - upcoming;
    if (_channelCount > 0) {
        add(ItemKind::Section, ws(TrF(StringId::GuideSearchChannelsHeading, {num(_channelCount)})), -1);
        add(ItemKind::Channels, L"", -1);
    }
    if (_moreChannels > 0)
        add(ItemKind::Note, ws(TrF(StringId::GuideSearchMoreChannels, {num(_moreChannels)})), -1);
    std::wstring status;
    if (upcoming == 0) status = ws(TrF(StringId::GuideSearchNone, {ns(_searchedText)}));
    else if (_hitsTruncated) status = ws(TrF(StringId::GuideSearchShowingFirst, {num((long long)upcoming)}));
    else status = ws(TrF(StringId::GuideSearchCount, {num((long long)upcoming)}));
    add(ItemKind::Section, status, -1);

    long long lastDay = -1;
    bool descrBlock = false, pastBlock = false;
    const long long now = _searchNowUtc;
    for (size_t i = 0; i < _hits.size(); ++i) {
        const auto& h = _hits[i];
        const bool past = h.programme.stopUtc <= now;
        if (past && !pastBlock) {
            add(ItemKind::Section, ws(Tr(StringId::GuideSearchCatchupHeading)), -1);
            if (_pastTruncated)
                add(ItemKind::Note, ws(TrF(StringId::GuideSearchShowingFirst, {num((long long)aired)})), -1);
            pastBlock = true;
            descrBlock = false;
            lastDay = -1;
        }
        if (!h.inTitle && !descrBlock) {
            add(ItemKind::Section, ws(Tr(StringId::GuideSearchInDescriptions)), -1);
            descrBlock = true;
            lastDay = -1;
        }
        // A programme already airing is filed under TODAY even if it began before midnight.
        const long long when = past ? h.programme.startUtc : std::max(h.programme.startUtc, now);
        const long long d = localDay(when);
        if (d != lastDay) {
            add(ItemKind::Heading, ws(dayLabel(when, now)), -1);
            lastDay = d;
        }
        add(ItemKind::Hit, L"", (int)i);
    }
}

// ---- results list -------------------------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView*)__unused tv { return (NSInteger)_items.size(); }

- (CGFloat)tableView:(NSTableView*)__unused tv heightOfRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_items.size()) return 24;
    const ResultItem& it = _items[(size_t)row];
    if (it.kind == ItemKind::Hit) {
        const auto& h = _hits[(size_t)it.hit];
        return (!h.inTitle && !h.snippet.empty()) ? 62 : 46;  // a description match: a third line
    }
    return it.kind == ItemKind::Channels ? 46 : 26;
}

- (BOOL)tableView:(NSTableView*)__unused tv shouldSelectRow:(NSInteger)row {
    return row >= 0 && row < (NSInteger)_items.size() && selectable(_items[(size_t)row]);
}

- (NSView*)tableView:(NSTableView*)tv viewForTableColumn:(NSTableColumn*)__unused col row:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_items.size()) return nil;
    const ResultItem& it = _items[(size_t)row];
    NSTextField* tf = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!tf) {
        tf = [NSTextField wrappingLabelWithString:@""];
        tf.identifier = @"cell";
        tf.maximumNumberOfLines = 3;
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
        tf.cell.truncatesLastVisibleLine = YES;
    }
    NSFont* f13 = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    NSFont* f11 = [NSFont systemFontOfSize:11];
    NSColor* accent = NSColor.controlAccentColor;
    NSMutableAttributedString* s = [[NSMutableAttributedString alloc] init];
    auto line = [&](NSAttributedString* a) {
        if (s.length) [s appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"]];
        [s appendAttributedString:a];
    };
    auto plain = [&](NSString* text, NSFont* font, NSColor* color) {
        return [[NSAttributedString alloc] initWithString:text
                                               attributes:@{NSFontAttributeName: font, NSForegroundColorAttributeName: color}];
    };
    switch (it.kind) {
        case ItemKind::Section:
            line(plain(ns(it.label), [NSFont systemFontOfSize:12 weight:NSFontWeightBold], accent));
            break;
        case ItemKind::Heading:
            line(plain(ns(it.label), [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold], NSColor.secondaryLabelColor));
            break;
        case ItemKind::Note:
            line(plain(ns(it.label), f11, NSColor.secondaryLabelColor));
            break;
        case ItemKind::Channels:
            line(plain(TrF(StringId::GuideSearchFilterChannels, {ns(_searchedText)}), f13, NSColor.labelColor));
            line(markedText(_channelNames, f11, NSColor.secondaryLabelColor, accent));
            break;
        case ItemKind::Hit: {
            const auto& h = _hits[(size_t)it.hit];
            const long long now = (long long)time(nullptr);
            NSMutableAttributedString* title = [markedText(h.markedTitle.empty() ? h.programme.title : h.markedTitle,
                                                           f13, NSColor.labelColor, accent) mutableCopy];
            if (h.programme.startUtc <= now && now < h.programme.stopUtc)  // "On now" badge
                [title appendAttributedString:plain([NSString stringWithFormat:@"   %@", Tr(StringId::GuideSearchOnNow)],
                                                    [NSFont systemFontOfSize:11 weight:NSFontWeightBold], accent)];
            line(title);
            const std::wstring chan = h.channelName.empty() ? h.programme.channelId : h.channelName;
            line(plain([NSString stringWithFormat:@"%@  ·  %@", REGuideTimeRange(h.programme.startUtc, h.programme.stopUtc),
                                                  ns(chan)], f11, NSColor.secondaryLabelColor));
            if (!h.inTitle && !h.snippet.empty())
                line(markedText(h.snippet, f11, NSColor.secondaryLabelColor, accent));
            break;
        }
    }
    tf.attributedStringValue = s;
    return tf;
}

- (const Database::ProgrammeHit*)hitAtRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_items.size() || _items[(size_t)row].kind != ItemKind::Hit) return nullptr;
    return &_hits[(size_t)_items[(size_t)row].hit];
}

- (void)resultClicked:(id)__unused sender {
    const NSInteger row = _results.clickedRow;
    if (row < 0 || row >= (NSInteger)_items.size()) return;
    if (_items[(size_t)row].kind == ItemKind::Channels) {
        [self applyChannelsItem];
    } else if (const auto* h = [self hitAtRow:row]) {
        [self jumpToHit:*h byClick:YES];
    }
}

- (void)resultDoubleClicked:(id)__unused sender {
    if (const auto* h = [self hitAtRow:_results.clickedRow]) [self openHit:*h];
}

- (void)openSelectedResult {
    const NSInteger row = _results.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.size()) return;
    if (_items[(size_t)row].kind == ItemKind::Channels) [self applyChannelsItem];
    else if (const auto* h = [self hitAtRow:row]) [self openHit:*h];
}

- (void)applyChannelsItem {
    [self showResultsList:NO];
    [self setFilter:_searchedText];
    [_window makeFirstResponder:_guideView];
}

- (void)guideViewDidClearFilter:(EpgGuideView*)__unused view { [self setFilter:L""]; }

// The visible row holding `h` (its channel AND a programme starting at its start), clearing a
// channel filter that hides it; -1 when the rows do not hold it (Win32 rowOfHit).
- (NSInteger)rowOfHit:(const Database::ProgrammeHit&)h {
    const std::wstring want = normId(h.channelTvgId);
    auto holds = [&](const GuideRow& row) {
        if (row.channelId != h.channelTvgId && normId(row.channelId) != want) return false;
        for (const auto& p : row.programmes)
            if (p.startUtc == h.programme.startUtc) return true;
        return false;
    };
    const std::wstring needle = foldCopy(_filter);
    NSInteger shownIndex = 0;
    for (size_t i = 0; i < _allRows.size(); ++i) {
        const bool shown = _filter.empty() || channelMatches(_allRows[i], needle);
        if (shown && holds(_allRows[i])) return shownIndex;
        if (shown) ++shownIndex;
    }
    for (size_t i = 0; i < _allRows.size(); ++i)
        if (holds(_allRows[i])) {  // filtered out: with the filter cleared, rows are _allRows in order
            [self setFilter:L""];
            return (NSInteger)i;
        }
    return -1;
}

// Show a result in the grid (Win32 jumpToHit). Not in the rows: rebuild them — but only when that
// could help (inside the window a rebuild covers, and not already rebuilt since these results).
- (void)jumpToHit:(Database::ProgrammeHit)hit byClick:(BOOL)byClick {
    NSInteger row = [self rowOfHit:hit];
    const long long now = (long long)time(nullptr);
    const bool rebuildCouldHelp = hit.programme.startUtc < now + rabbitears::kGuideWindowAheadSec &&
                                  hit.programme.stopUtc > now - rabbitears::kGuideWindowPastSec &&
                                  _rowsGeneration == _hitsGeneration;
    if (row < 0 && rebuildCouldHelp && [self rebuildModel]) {
        [self applyRowsKeepingScroll:YES];
        [self updateCoverageButton];
        [self findChannels:_searchedText];  // its channels must describe the NEW rows
        [self buildItems];
        [_results reloadData];
        row = [self rowOfHit:hit];
    }
    if (row < 0) {
        // Catch-up: most aired results are older than the grid's 6 h of history. A click on one the
        // archive can still play just selects it — double-click / Return opens it (Play from the start).
        const bool playable = hit.programme.stopUtc <= now && self.onPlayFromStart &&
                              canPlayFromStart(hit.archiveChannelId, hit.archiveDays, hit.programme.startUtc, now);
        if (!(byClick && playable)) NSBeep();
        return;
    }
    [_searchTimer invalidate];  // a search still pending must not bring the list back over it
    [self showResultsList:NO];
    [_guideView flashProgrammeInRow:row startUtc:hit.programme.startUtc];
    [_window makeFirstResponder:_guideView];
}

- (void)openHit:(const Database::ProgrammeHit&)h {
    const std::wstring chan = h.channelName.empty() ? h.programme.channelId : h.channelName;
    [self openProgrammeTitle:ns(h.programme.title) descr:ns(h.programme.descr) channelId:ns(h.channelTvgId)
                 channelName:ns(chan) start:h.programme.startUtc stop:h.programme.stopUtc
              archiveChannel:h.archiveChannelId archiveDays:h.archiveDays];
}

// Right-click on a result (Win32 onResultsContextMenu): the programme's actions + Show in guide.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    [menu removeAllItems];
    const NSInteger row = _results.clickedRow;
    const auto* hp = [self hitAtRow:row];
    if (!hp) return;
    [_results selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    const Database::ProgrammeHit h = *hp;
    const std::wstring chan = h.channelName.empty() ? h.programme.channelId : h.channelName;
    [self addProgrammeActionsTo:menu title:ns(h.programme.title) channelId:ns(h.channelTvgId) channelName:ns(chan)
                          start:h.programme.startUtc stop:h.programme.stopUtc
                 archiveChannel:h.archiveChannelId archiveDays:h.archiveDays];
    __weak TvGuideWindowController* weak = self;
    [self addItem:Tr(StringId::GuideSearchShowInGuide) to:menu do:^{ [weak jumpToHit:h byClick:NO]; }];
}

// ---- menus + the programme dialog ---------------------------------------------------------------

- (void)addItem:(NSString*)title to:(NSMenu*)menu do:(void (^)(void))block {
    NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:title action:@selector(runMenuBlock:) keyEquivalent:@""];
    mi.target = self;
    mi.representedObject = [block copy];
    [menu addItem:mi];
}
- (void)runMenuBlock:(NSMenuItem*)mi {
    void (^block)(void) = mi.representedObject;
    if (block) block();
}

- (void)playFromStart:(long long)archiveChannel title:(NSString*)title start:(long long)a stop:(long long)b {
    if (self.onPlayFromStart && self.onPlayFromStart(archiveChannel, title, a, b)) [self hide];
}

- (void)addProgrammeActionsTo:(NSMenu*)menu title:(NSString*)title channelId:(NSString*)cid
                  channelName:(NSString*)cname start:(long long)a stop:(long long)b
               archiveChannel:(long long)arch archiveDays:(int)days {
    __weak TvGuideWindowController* weak = self;
    const long long now = (long long)time(nullptr);
    if (self.onPlayFromStart && canPlayFromStart(arch, days, a, now))
        [self addItem:Tr(StringId::ProgrammePlayFromStartButton) to:menu do:^{
            [weak playFromStart:arch title:title start:a stop:b];
        }];
    if (cid.length) {
        [self addItem:Tr(StringId::ProgrammePlayButton) to:menu do:^{
            TvGuideWindowController* s = weak;
            if (s && s->_onPlay) s->_onPlay(cid, cname);
        }];
        if (self.onSchedule && b > now)
            [self addItem:Tr(StringId::ProgrammeScheduleButton) to:menu do:^{
                TvGuideWindowController* s = weak;
                if (s && s.onSchedule) s.onSchedule(cid, cname, title, a, b);
            }];
        if (self.onRecordSeries && title.length)
            [self addItem:Tr(StringId::RecordSeriesTitle) to:menu do:^{
                TvGuideWindowController* s = weak;
                if (s && s.onRecordSeries) s.onRecordSeries(cid, cname, title);
            }];
    }
}

// Right-click a grid row (Win32 onGuideContextMenu): "Play from the start" on a programme the row's
// archive reaches back to, and the channel's favourite toggle.
- (NSMenu*)guideView:(EpgGuideView*)__unused view menuForRow:(REGuideRow*)row programme:(REGuideProgramme*)p {
    NSMenu* menu = [[NSMenu alloc] init];
    __weak TvGuideWindowController* weak = self;
    const long long now = (long long)time(nullptr);
    if (p && self.onPlayFromStart && canPlayFromStart(row.archiveChannel, row.archiveDays, p.startUtc, now)) {
        const long long arch = row.archiveChannel, a = p.startUtc, b = p.stopUtc;
        NSString* title = p.title;
        [self addItem:Tr(StringId::ProgrammePlayFromStartButton) to:menu do:^{
            [weak playFromStart:arch title:title start:a stop:b];
        }];
    }
    if (row.channelId.length && self.isFavourite && self.onToggleFavourite) {
        NSString* cid = row.channelId;
        NSString* cname = row.channelName;
        const BOOL fav = self.isFavourite(cid);
        [self addItem:Tr(fav ? StringId::GuideRemoveFromFavourites : StringId::GuideAddToFavourites) to:menu do:^{
            TvGuideWindowController* s = weak;
            if (s && s.onToggleFavourite) s.onToggleFavourite(cid, cname);
        }];
    }
    return menu.numberOfItems ? menu : nil;
}

// A programme block was clicked: show its details with its actions.
- (void)guideView:(EpgGuideView*)__unused view
    didActivateProgramme:(REGuideProgramme*)programme
                   inRow:(REGuideRow*)row {
    [self openProgrammeTitle:programme.title descr:programme.descr channelId:row.channelId
                 channelName:row.channelName start:programme.startUtc stop:programme.stopUtc
              archiveChannel:row.archiveChannel archiveDays:row.archiveDays];
}

- (void)openProgrammeTitle:(NSString*)title descr:(NSString*)descr channelId:(NSString*)cid
               channelName:(NSString*)cname start:(long long)startUtc stop:(long long)stopUtc
            archiveChannel:(long long)arch archiveDays:(int)days {
    static NSDateFormatter* dayFmt = nil;
    if (!dayFmt) {
        dayFmt = [[NSDateFormatter alloc] init];
        dayFmt.dateStyle = NSDateFormatterMediumStyle;
        dayFmt.timeStyle = NSDateFormatterNoStyle;
    }
    NSString* day = startUtc ? [dayFmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)startUtc]] : @"";
    NSMutableString* info = [NSMutableString stringWithFormat:@"%@\n%@  %@", cname, day,
                                                              REGuideTimeRange(startUtc, stopUtc)];
    if (descr.length) [info appendFormat:@"\n\n%@", descr];

    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = title.length ? title : Tr(StringId::ProgrammeWindowTitle);
    a.informativeText = info;
    const BOOL hasChannel = cid.length > 0;
    const long long now = (long long)time(nullptr);
    const BOOL fromStart = self.onPlayFromStart && canPlayFromStart(arch, days, startUtc, now);
    // Buttons return NSAlertFirstButtonReturn (1000) + index, in add order; each slot's meaning is
    // recorded so an absent "Play from the start" shifts nothing.
    NSMutableArray<NSString*>* actions = [NSMutableArray array];
    NSButton* play = [a addButtonWithTitle:Tr(StringId::ProgrammePlayButton)];
    play.enabled = hasChannel;
    [actions addObject:@"play"];
    if (fromStart) {
        [a addButtonWithTitle:Tr(StringId::ProgrammePlayFromStartButton)];
        [actions addObject:@"start"];
    }
    NSButton* sched = [a addButtonWithTitle:Tr(StringId::ScheduleEditWindowTitle)];
    sched.enabled = hasChannel && stopUtc > now;  // can't record a finished airing
    [actions addObject:@"schedule"];
    NSButton* series = [a addButtonWithTitle:Tr(StringId::RecordSeriesTitle)];
    series.enabled = hasChannel && title.length > 0;
    [actions addObject:@"series"];
    NSButton* close = [a addButtonWithTitle:Tr(StringId::ButtonClose)];
    close.keyEquivalent = @"\033";  // Esc closes
    [actions addObject:@"close"];

    __weak TvGuideWindowController* weak = self;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        TvGuideWindowController* s = weak;
        const NSInteger k = resp - NSAlertFirstButtonReturn;
        if (!s || k < 0 || k >= (NSInteger)actions.count) return;
        NSString* act = actions[(NSUInteger)k];
        if ([act isEqualToString:@"play"] && hasChannel && s->_onPlay) s->_onPlay(cid, cname);
        else if ([act isEqualToString:@"start"]) [s playFromStart:arch title:title start:startUtc stop:stopUtc];
        else if ([act isEqualToString:@"schedule"] && hasChannel && s.onSchedule)
            s.onSchedule(cid, cname, title, startUtc, stopUtc);
        else if ([act isEqualToString:@"series"] && hasChannel && s.onRecordSeries)
            s.onRecordSeries(cid, cname, title);
    }];
}

@end
