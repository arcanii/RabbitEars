// SPDX-License-Identifier: GPL-3.0-or-later
// See MainWindowController.h.
#import "MainWindowController.h"

#include <cwchar>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Http.h"
#include "core/M3uParser.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "platform/Encoding.h"
#include "platform/Log.h"

#import "MeterView.h"
#import "VlcPlayerMac.h"

using namespace rabbitears;

// Filter popup tags.
enum { kFilterAll = 0, kFilterFavourites = 1, kFilterGroup = 2, kFilterCountry = 3 };

@implementation MainWindowController {
    NSWindow*      _window;
    NSTextField*   _urlField;
    NSSearchField* _search;
    NSPopUpButton* _filter;
    NSTableView*   _table;
    NSView*        _videoView;
    MeterView*     _meter;
    NSTextField*   _status;

    std::unique_ptr<Database>     _db;
    std::unique_ptr<VlcPlayerMac> _player;
    std::vector<Channel>          _channels;
    long long                     _currentPid;             // loaded playlist (0 = none/all)
    BOOL                          _suppressSelectionPlay;  // ignore programmatic selection changes
    uint64_t                      _loadToken;              // only the newest URL load's result applies
}

- (instancetype)init {
    if ((self = [super init])) {
        _db = std::make_unique<Database>();
        _player = std::make_unique<VlcPlayerMac>();
    }
    return self;
}

static NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:utf8FromWide(w).c_str()] ?: @"";
}
static std::wstring ws(NSString* s) { return wideFromUtf8(s.UTF8String ?: ""); }

- (void)showWindow {
    const NSRect frame = NSMakeRect(0, 0, 980, 640);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"RabbitEars";
    [_window center];
    _window.releasedWhenClosed = NO;  // we own it via the ivar
    NSView* content = _window.contentView;

    std::wstring err;
    if (!_db->open(Database::defaultDbPath(), &err)) {
        diag::error(L"DB open failed: " + err);
        _db.reset();  // make the unusable state explicit; the guards below surface it
    }

    // ---- top bar: two rows (load controls; search/filter/stop) ----
    const CGFloat barH = 80;
    NSView* bar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, frame.size.height - barH, frame.size.width, barH)];
    bar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;

    // row 1: URL field + Load + Open File
    _urlField = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 45, frame.size.width - 330, 26)];
    _urlField.placeholderString = @"M3U / M3U8 playlist URL…";
    _urlField.autoresizingMask = NSViewWidthSizable;
    _urlField.target = self;
    _urlField.action = @selector(loadURL:);
    [bar addSubview:_urlField];

    NSButton* loadBtn = [NSButton buttonWithTitle:@"Load URL" target:self action:@selector(loadURL:)];
    loadBtn.frame = NSMakeRect(frame.size.width - 310, 44, 92, 28);
    loadBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:loadBtn];

    NSButton* openBtn = [NSButton buttonWithTitle:@"Open File…" target:self action:@selector(openFile:)];
    openBtn.frame = NSMakeRect(frame.size.width - 214, 44, 104, 28);
    openBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:openBtn];

    // row 2: search field + filter popup + Stop
    _search = [[NSSearchField alloc] initWithFrame:NSMakeRect(12, 10, 260, 26)];
    _search.placeholderString = @"Search channels…";
    _search.delegate = self;  // controlTextDidChange: -> live filter
    [bar addSubview:_search];

    _filter = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(280, 10, 190, 26) pullsDown:NO];
    _filter.target = self;
    _filter.action = @selector(filterChanged:);
    [bar addSubview:_filter];

    NSButton* stopBtn = [NSButton buttonWithTitle:@"Stop" target:self action:@selector(stop:)];
    stopBtn.frame = NSMakeRect(frame.size.width - 92, 9, 80, 28);
    stopBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:stopBtn];
    [content addSubview:bar];

    // ---- status line (bottom) ----
    const CGFloat statusH = 22;
    _status = [NSTextField labelWithString:@"Ready."];
    _status.frame = NSMakeRect(12, 3, frame.size.width - 24, statusH - 5);
    _status.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    _status.textColor = NSColor.secondaryLabelColor;
    [content addSubview:_status];

    // ---- split: channel grid | video ----
    NSSplitView* split = [[NSSplitView alloc]
        initWithFrame:NSMakeRect(0, statusH, frame.size.width, frame.size.height - barH - statusH)];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 380, 100)];
    scroll.hasVerticalScroller = YES;
    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    [self addColumn:@"fav" title:@"★" width:26];
    [self addColumn:@"num" title:@"#" width:46];
    [self addColumn:@"name" title:@"Channel" width:200];
    [self addColumn:@"group" title:@"Group" width:120];
    _table.headerView = [[NSTableHeaderView alloc] init];
    _table.dataSource = self;
    _table.delegate = self;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.allowsColumnResizing = YES;
    _table.menu = [self makeRowMenu];  // right-click: Play / Toggle Favourite
    scroll.documentView = _table;
    [split addSubview:scroll];

    // Right pane: the video surface with a thin audio-level meter strip below it.
    NSView* rightPane = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 100)];
    const CGFloat meterH = 26;
    _videoView = [[NSView alloc] initWithFrame:NSMakeRect(0, meterH, 600, 100 - meterH)];
    _videoView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _videoView.wantsLayer = YES;
    _videoView.layer.backgroundColor = NSColor.blackColor.CGColor;
    [rightPane addSubview:_videoView];
    _meter = [[MeterView alloc] initWithFrame:NSMakeRect(0, 0, 600, meterH)];
    _meter.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;  // pinned to the bottom
    [rightPane addSubview:_meter];
    [split addSubview:rightPane];

    [content addSubview:split];
    [split setPosition:380 ofDividerAtIndex:0];

    _player->attachTo(_videoView);  // hand libVLC the NSView to render into
    __weak MeterView* wm = _meter;
    _player->setLevelCallback([wm](float lv) { [wm pushLevel:lv]; });  // pushLevel is thread-safe

    [self restoreLastPlaylist];

    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)addColumn:(NSString*)ident title:(NSString*)title width:(CGFloat)w {
    NSTableColumn* c = [[NSTableColumn alloc] initWithIdentifier:ident];
    c.title = title;
    c.width = w;
    c.minWidth = 20;
    [_table addTableColumn:c];
}

- (NSMenu*)makeRowMenu {
    NSMenu* m = [[NSMenu alloc] init];
    [[m addItemWithTitle:@"Play" action:@selector(playClicked:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Toggle Favourite" action:@selector(toggleFavourite:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"Set Channel Number…" action:@selector(editChannelNumber:) keyEquivalent:@""] setTarget:self];
    return m;
}

- (void)setStatus:(NSString*)s { _status.stringValue = s; }

// ---- playlist load / filter model ----

- (void)restoreLastPlaylist {
    if (!_db) { [self setStatus:@"Database unavailable — check disk permissions and restart."]; return; }
    const auto playlists = _db->listPlaylists();
    if (playlists.empty()) {
        [self setStatus:@"Ready — load an M3U URL or open a file to begin."];
        [self rebuildFilterMenu];
        return;
    }
    [self showPlaylist:playlists.back().id];
    [self selectLastPlayed];
}

// Highlight (don't auto-play) the channel that was playing when the app last quit.
- (void)selectLastPlayed {
    if (!_db) return;
    const auto s = _db->getSetting(L"last_channel_id");
    if (!s || s->empty()) return;
    const long long cid = std::wcstoll(s->c_str(), nullptr, 10);
    for (size_t i = 0; i < _channels.size(); ++i) {
        if (_channels[i].id != cid) continue;
        _suppressSelectionPlay = YES;
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
        [_table scrollRowToVisible:(NSInteger)i];
        _suppressSelectionPlay = NO;
        [self setStatus:[NSString stringWithFormat:@"Last played: %@ — click to resume.",
                         ns(_channels[i].name)]];
        return;
    }
}

// Make `pid` the active playlist: reset search/filter, rebuild groups, reload.
- (void)showPlaylist:(long long)pid {
    _currentPid = pid;
    _search.stringValue = @"";
    [self rebuildFilterMenu];       // selects "All channels"
    [self refreshChannels];
}

// Build items manually (NOT addItemWithTitle, which de-dupes by title) so a real
// M3U group named "All channels"/"★ Favourites" can't clobber a reserved item;
// the true group name rides on representedObject and routing keys off tag +
// representedObject, never the display title.
- (void)addFilterItem:(NSString*)title tag:(NSInteger)tag group:(NSString*)group {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    it.tag = tag;
    it.representedObject = group;
    [_filter.menu addItem:it];
}

- (void)rebuildFilterMenu {
    [_filter removeAllItems];
    [self addFilterItem:@"All channels" tag:kFilterAll group:nil];
    [self addFilterItem:@"★ Favourites" tag:kFilterFavourites group:nil];
    if (_db) {
        const auto groups = _db->listGroups();
        if (!groups.empty()) [_filter.menu addItem:[NSMenuItem separatorItem]];
        for (const auto& g : groups) {
            if (g.empty()) continue;
            [self addFilterItem:ns(g) tag:kFilterGroup group:ns(g)];
        }
        const auto countries = _db->listCountries();
        if (!countries.empty()) [_filter.menu addItem:[NSMenuItem separatorItem]];
        for (const auto& cc : countries) {
            if (cc.empty()) continue;
            [self addFilterItem:[@"Country: " stringByAppendingString:ns(cc).uppercaseString]
                            tag:kFilterCountry group:ns(cc)];
        }
    }
    // Rebuild happens only on playlist load — always reset to "All channels" so a
    // switch/import shows the new playlist in full, never a stale group filter.
    [_filter selectItemAtIndex:0];
}

// Apply the current search text + filter selection and reload the table.
- (void)refreshChannels {
    if (!_db) { _channels.clear(); [_table reloadData]; return; }
    const std::wstring q = ws(_search.stringValue);
    if (!q.empty()) {
        _channels = _db->searchChannels(q);
    } else {
        NSMenuItem* sel = _filter.selectedItem;
        switch (sel.tag) {
            case kFilterFavourites: _channels = _db->favourites(); break;
            case kFilterGroup:      _channels = _db->channelsByGroup(ws(sel.representedObject)); break;
            case kFilterCountry:    _channels = _db->channelsByCountry(ws(sel.representedObject)); break;
            default:                _channels = _currentPid ? _db->channelsByPlaylist(_currentPid)
                                                            : _db->allChannels(); break;
        }
    }
    _suppressSelectionPlay = YES;      // programmatic reload must not auto-play
    [_table deselectAll:nil];
    [_table reloadData];
    _suppressSelectionPlay = NO;
    [self setStatus:[NSString stringWithFormat:@"%lu channels%@.", (unsigned long)_channels.size(),
                     q.empty() ? @"" : @" (search)"]];
}

// ---- actions ----

- (void)loadURL:(id)__unused sender {
    NSString* u = _urlField.stringValue;
    if (!u.length) return;
    const std::wstring url = ws(u);
    const uint64_t token = ++_loadToken;  // newest request wins; supersedes any in flight
    [self setStatus:@"Downloading playlist…"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        std::string bytes;
        std::wstring derr;
        const bool ok = httpGet(url, bytes, derr);
        auto doc = std::make_shared<M3uDocument>(ok ? parseM3u(bytes) : M3uDocument{});
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != self->_loadToken) return;  // a newer load superseded this one
            if (!ok) { [self setStatus:[NSString stringWithFormat:@"Download failed: %@", ns(derr)]]; return; }
            [self importDoc:*doc name:url source:url isUrl:true];
        });
    });
}

- (void)openFile:(id)__unused sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowedFileTypes = @[ @"m3u", @"m3u8" ];
    panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSModalResponseOK || !panel.URL) return;
        const std::wstring path = ws(panel.URL.path);
        std::wstring perr;
        const M3uDocument doc = parseM3uFile(path, &perr);
        if (!perr.empty()) { [self setStatus:[NSString stringWithFormat:@"Read failed: %@", ns(perr)]]; return; }
        [self importDoc:doc name:path source:path isUrl:false];
    }];
}

- (void)importDoc:(const M3uDocument&)doc
             name:(const std::wstring&)name
           source:(const std::wstring&)source
            isUrl:(bool)isUrl {
    if (!_db) { [self setStatus:@"Database unavailable — cannot save playlist."]; return; }
    const long long now = (long long)time(nullptr);
    const long long pid = _db->addPlaylist(name, source, isUrl, now);
    _db->bulkInsertChannels(pid, doc.channels, now);
    [self showPlaylist:pid];
}

- (void)filterChanged:(id)__unused sender { [self refreshChannels]; }

- (void)controlTextDidChange:(NSNotification*)note {
    if (note.object == _search) [self refreshChannels];  // live search
}

- (void)stop:(id)__unused sender { _player->stop(); [_meter reset]; [self setStatus:@"Stopped."]; }

- (void)playClicked:(id)__unused sender { [self playRow:_table.clickedRow]; }

- (void)toggleFavourite:(id)__unused sender {
    const NSInteger row = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_channels.size() || !_db) return;
    _db->toggleFavourite(_channels[(size_t)row].id);
    [self refreshChannels];  // re-query so the ★ column + a Favourites filter stay correct
}

// Set/clear a channel's LCN (the # column) via a small prompt.
- (void)editChannelNumber:(id)__unused sender {
    const NSInteger row = _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_channels.size() || !_db) return;
    const Channel& c = _channels[(size_t)row];
    const long long cid = c.id;  // capture by value; _channels can change before the sheet returns
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = [NSString stringWithFormat:@"Channel number for “%@”", ns(c.name)];
    a.informativeText = @"Enter a number, or leave blank to clear it.";
    [a addButtonWithTitle:@"Set"];
    [a addButtonWithTitle:@"Cancel"];
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 220, 24)];
    input.stringValue = c.lcn ? [NSString stringWithFormat:@"%d", *c.lcn] : @"";
    a.accessoryView = input;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;
        NSString* t = [input.stringValue
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        std::optional<int> lcn;
        if (t.length) lcn = (int)t.integerValue;
        self->_db->setChannelNumber(cid, lcn);
        [self refreshChannels];
    }];
}

- (void)playRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_channels.size()) return;
    const Channel& c = _channels[(size_t)row];
    _player->play(c.streamUrl, c.userAgent, c.referrer);
    if (_db) _db->setSetting(L"last_channel_id", std::to_wstring(c.id));  // resume this on next launch
    [self setStatus:[NSString stringWithFormat:@"Playing: %@", ns(c.name)]];
}

// ---- NSTableView data source / delegate ----

- (NSInteger)numberOfRowsInTableView:(NSTableView*)__unused t { return (NSInteger)_channels.size(); }

- (id)tableView:(NSTableView*)__unused t
    objectValueForTableColumn:(NSTableColumn*)col
                          row:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_channels.size()) return @"";
    const Channel& c = _channels[(size_t)row];
    NSString* id_ = col.identifier;
    if ([id_ isEqualToString:@"fav"])   return c.favourite ? @"★" : @"";
    if ([id_ isEqualToString:@"num"])   return c.lcn ? [NSString stringWithFormat:@"%d", *c.lcn] : @"";
    if ([id_ isEqualToString:@"group"]) return ns(c.groupTitle);
    return ns(c.name);  // "name"
}

- (void)tableViewSelectionDidChange:(NSNotification*)__unused n {
    if (_suppressSelectionPlay) return;  // programmatic (deselect/reload) change, not a user pick
    [self playRow:_table.selectedRow];
}

@end
