// SPDX-License-Identifier: GPL-3.0-or-later
// See MainWindowController.h.
#import "MainWindowController.h"

#include <cwchar>
#include <ctime>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/Http.h"
#include "core/M3uParser.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "platform/Encoding.h"
#include "platform/Log.h"
#include "platform/Updater.h"

#import "AppDelegate.h"
#import "VlcPlayerMac.h"

using namespace rabbitears;

// NSTableView that plays the selected row on Return/Enter (the keyboard peer of the
// double-click action), so the grid is fully keyboard-navigable.
@interface RETableView : NSTableView
@end
@implementation RETableView
- (void)keyDown:(NSEvent*)e {
    const unichar ch = e.charactersIgnoringModifiers.length
        ? [e.charactersIgnoringModifiers characterAtIndex:0] : 0;
    if ((ch == NSCarriageReturnCharacter || ch == NSEnterCharacter)
        && self.selectedRow >= 0 && self.doubleAction) {
        [NSApp sendAction:self.doubleAction to:self.target from:self];
        return;
    }
    [super keyDown:e];
}
@end

// Filter popup tags.
enum { kFilterAll = 0, kFilterFavourites = 1, kFilterGroup = 2, kFilterCountry = 3 };

@implementation MainWindowController {
    NSWindow*      _window;
    NSSearchField* _search;
    NSPopUpButton* _filter;
    RETableView*   _table;
    NSView*        _videoView;
    NSTextField*   _emptyHint;   // "no channels yet" hint centered over the empty video pane
    NSTextField*   _status;
    NSSlider*      _volume;      // bottom-bar volume (0..100)
    NSButton*      _muteBtn;     // 🔊 / 🔇 toggle

    std::unique_ptr<Database>     _db;
    std::unique_ptr<VlcPlayerMac> _player;
    std::vector<Channel>          _channels;
    long long                     _currentPid;             // loaded playlist (0 = none/all)
    uint64_t                      _loadToken;              // only the newest URL load's result applies
    CGFloat                       _gridWidth;              // channel-grid width; the video pane fills the rest
    int                           _preMuteVolume;          // volume to restore when un-muting
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
    _window.contentMinSize = NSMakeSize(560, 360);  // keep both split panes usable
    _window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;  // ⌃⌘F / green button
    _window.frameAutosaveName = @"RabbitEarsMainWindow";  // remember size + position across launches
    NSView* content = _window.contentView;

    std::wstring err;
    if (!_db->open(Database::defaultDbPath(), &err)) {
        diag::error(L"DB open failed: " + err);
        _db.reset();  // make the unusable state explicit; the guards below surface it
    }

    // ---- top bar (one row): [+ Add Playlist] [Settings ▾] … [search] [filter] [Stop]
    //      — the mac peer of the Win32 command bar (kCmdBtns = + Add Playlist, Settings).
    const CGFloat barH = 46;
    NSView* bar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, frame.size.height - barH, frame.size.width, barH)];
    bar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;

    // Left: the accent "+ Add Playlist" (prompts for a URL) and the Settings menu.
    NSButton* addBtn = [NSButton buttonWithTitle:@"+  Add Playlist"
                                          target:self action:@selector(addPlaylist:)];
    addBtn.frame = NSMakeRect(12, 9, 138, 28);
    addBtn.bezelColor = NSColor.controlAccentColor;  // accent, like the Win32 button
    [bar addSubview:addBtn];

    NSButton* setBtn = [NSButton buttonWithTitle:@"Settings  ▾"
                                          target:self action:@selector(showSettings:)];
    setBtn.frame = NSMakeRect(158, 9, 116, 28);
    [bar addSubview:setBtn];

    // Right (pinned): Stop, the filter popup, and the stretchy search field between.
    NSButton* stopBtn = [NSButton buttonWithTitle:@"Stop" target:self action:@selector(stop:)];
    stopBtn.frame = NSMakeRect(frame.size.width - 92, 9, 80, 28);
    stopBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:stopBtn];

    _filter = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(frame.size.width - 270, 9, 170, 28) pullsDown:NO];
    _filter.target = self;
    _filter.action = @selector(filterChanged:);
    _filter.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:_filter];

    _search = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(284, 10, frame.size.width - 562, 26)];
    _search.placeholderString = @"Search channels…";
    _search.autoresizingMask = NSViewWidthSizable;
    _search.delegate = self;  // controlTextDidChange: -> live filter
    [bar addSubview:_search];

    [content addSubview:bar];

    // ---- bottom bar: status line (left) + volume (right) ----
    const CGFloat statusH = 22;
    _status = [NSTextField labelWithString:@"Ready."];
    _status.frame = NSMakeRect(12, 3, frame.size.width - 24 - 150, statusH - 5);
    _status.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    _status.textColor = NSColor.secondaryLabelColor;
    [content addSubview:_status];

    int vol0 = 100;
    if (_db) {
        const auto v = _db->getSetting(L"volume");
        if (v && !v->empty()) vol0 = (int)std::wcstol(v->c_str(), nullptr, 10);
    }
    _preMuteVolume = vol0 > 0 ? vol0 : 100;
    _muteBtn = [NSButton buttonWithTitle:(vol0 == 0 ? @"🔇" : @"🔊")
                                  target:self action:@selector(toggleMute:)];
    _muteBtn.bordered = NO;
    _muteBtn.frame = NSMakeRect(frame.size.width - 132, 1, 24, 20);
    _muteBtn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [content addSubview:_muteBtn];

    _volume = [NSSlider sliderWithValue:vol0 minValue:0 maxValue:100
                                 target:self action:@selector(volumeChanged:)];
    _volume.frame = NSMakeRect(frame.size.width - 104, 3, 92, 16);
    _volume.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    _volume.continuous = YES;
    [content addSubview:_volume];
    _player->setVolume(vol0);

    // ---- split: channel grid | video ----
    NSSplitView* split = [[NSSplitView alloc]
        initWithFrame:NSMakeRect(0, statusH, frame.size.width, frame.size.height - barH - statusH)];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 380, 100)];
    scroll.hasVerticalScroller = YES;
    _table = [[RETableView alloc] initWithFrame:scroll.bounds];
    _table.target = self;
    _table.doubleAction = @selector(playSelectedRow:);  // double-click / Return plays
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

    _videoView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 600, 100)];
    _videoView.wantsLayer = YES;
    _videoView.layer.backgroundColor = NSColor.blackColor.CGColor;
    [split addSubview:_videoView];

    // Empty-state hint, centered over the (black) video pane until channels exist.
    _emptyHint = [NSTextField labelWithString:@"No channels yet — click “＋ Add Playlist” to begin."];
    _emptyHint.textColor = NSColor.secondaryLabelColor;
    _emptyHint.font = [NSFont systemFontOfSize:15];
    _emptyHint.translatesAutoresizingMaskIntoConstraints = NO;
    [_videoView addSubview:_emptyHint];
    [NSLayoutConstraint activateConstraints:@[
        [_emptyHint.centerXAnchor constraintEqualToAnchor:_videoView.centerXAnchor],
        [_emptyHint.centerYAnchor constraintEqualToAnchor:_videoView.centerYAnchor],
    ]];

    split.delegate = self;  // keep the grid a fixed width; the video pane fills the rest
    [content addSubview:split];
    _gridWidth = 380;
    [self layoutSplitPanes:split];  // fill now — no blank strip before the first resize

    _player->attachTo(_videoView);  // hand libVLC the NSView to render into

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

// ---- split layout (channel grid | video) ----

// The channel grid keeps _gridWidth; the video pane fills the rest. Called on every
// split resize and once at setup, so the video always fills — no blank strip before
// the first resize.
- (void)layoutSplitPanes:(NSSplitView*)sv {
    if (sv.subviews.count < 2) return;
    const CGFloat W = NSWidth(sv.bounds), H = NSHeight(sv.bounds), d = sv.dividerThickness;
    const CGFloat left = MAX(160, MIN(_gridWidth, W - d - 200));  // keep both panes usable
    sv.subviews[0].frame = NSMakeRect(0, 0, left, H);
    sv.subviews[1].frame = NSMakeRect(left + d, 0, W - left - d, H);
}

- (void)splitView:(NSSplitView*)sv resizeSubviewsWithOldSize:(NSSize)__unused oldSize {
    [self layoutSplitPanes:sv];  // window resize grows the video, not the grid
}

// Remember a divider the user dragged, so later window resizes preserve it.
- (void)splitViewDidResizeSubviews:(NSNotification*)note {
    NSSplitView* sv = note.object;
    if (sv.subviews.count >= 1) _gridWidth = NSWidth(sv.subviews[0].frame);
}

- (CGFloat)splitView:(NSSplitView*)__unused sv
    constrainMinCoordinate:(CGFloat)__unused m ofSubviewAt:(NSInteger)__unused i { return 160; }

- (CGFloat)splitView:(NSSplitView*)sv
    constrainMaxCoordinate:(CGFloat)__unused m ofSubviewAt:(NSInteger)__unused i {
    return NSWidth(sv.bounds) - 200;
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
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];
        [_table scrollRowToVisible:(NSInteger)i];
        [self setStatus:[NSString stringWithFormat:
                         @"Last played: %@ — double-click or press Return to resume.",
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
    [_table deselectAll:nil];
    [_table reloadData];
    [self setStatus:[NSString stringWithFormat:@"%lu channels%@.", (unsigned long)_channels.size(),
                     q.empty() ? @"" : @" (search)"]];
    _emptyHint.hidden = !_channels.empty();
}

// ---- actions ----

// "+ Add Playlist": prompt for a URL, then load it (peer of the Win32 onAddUrl).
- (void)addPlaylist:(id)__unused sender {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"Add Playlist";
    a.informativeText = @"Playlist URL (.m3u / .m3u8):";
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    input.placeholderString = @"https://…";
    a.accessoryView = input;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;  // OK is the first button
        NSString* u = [input.stringValue
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (u.length) [self loadPlaylistFromURL:u];
    }];
}

// Download + parse + import a playlist URL off the main queue; newest request wins
// (an in-flight load is superseded, never interleaved).
- (void)loadPlaylistFromURL:(NSString*)u {
    const std::wstring url = ws(u);
    const uint64_t token = ++_loadToken;
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
    if (pid == 0) {
        [self setStatus:@"Add playlist failed: could not save to the database."];
        [self showResults:@"Could not import the playlist"
                     info:[NSString stringWithFormat:
                           @"Source: %@\n\nThe playlist could not be saved to the database.", ns(source)]];
        return;
    }
    const int imported = _db->bulkInsertChannels(pid, doc.channels, now);
    const int parsed = (int)doc.channels.size();
    std::set<std::wstring> groups;
    for (const auto& c : doc.channels)
        if (!c.groupTitle.empty()) groups.insert(c.groupTitle);
    [self showPlaylist:pid];  // make it the active view (updates the grid + status)

    // Import results — the mac peer of the Win32 onPlaylistDone dialog.
    if (parsed == 0) {
        [self showResults:@"No channels found"
                     info:[NSString stringWithFormat:
                           @"Source: %@\n\nThe playlist contained no channels.", ns(source)]];
        return;
    }
    NSMutableString* info = [NSMutableString stringWithFormat:
        @"Source: %@\n\nChannels parsed: %d\nChannels imported: %d\n", ns(source), parsed, imported];
    if (parsed - imported > 0)
        [info appendFormat:@"Skipped (blank or duplicate URLs): %d\n", parsed - imported];
    [info appendFormat:@"Groups: %lu", (unsigned long)groups.size()];
    [self showResults:[NSString stringWithFormat:@"Imported %d channels", imported] info:info];
}

- (void)showResults:(NSString*)title info:(NSString*)info {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = info;
    [a addButtonWithTitle:@"OK"];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse __unused r) {}];
}

- (void)filterChanged:(id)__unused sender { [self refreshChannels]; }

- (void)controlTextDidChange:(NSNotification*)note {
    if (note.object == _search) [self refreshChannels];  // live search
}

- (void)stop:(id)__unused sender {
    _player->stop();
    _window.title = @"RabbitEars";
    [self setStatus:@"Stopped."];
}

// Settings pull-down (peer of the Win32 "Settings ▾" command-bar menu). Small for
// now — file import + updates/about; real preferences will hang off here.
- (void)showSettings:(NSButton*)sender {
    NSMenu* m = [[NSMenu alloc] init];
    [[m addItemWithTitle:@"Open Playlist File…" action:@selector(openFile:) keyEquivalent:@""] setTarget:self];
    [m addItem:[NSMenuItem separatorItem]];
    [[m addItemWithTitle:@"Check for Updates…" action:@selector(checkForUpdates:) keyEquivalent:@""] setTarget:self];
    [[m addItemWithTitle:@"About RabbitEars" action:@selector(showAbout:) keyEquivalent:@""] setTarget:self];
    [m popUpMenuPositioningItem:nil
                     atLocation:NSMakePoint(0, NSHeight(sender.bounds) + 4)
                         inView:sender];
}

- (void)checkForUpdates:(id)__unused sender { rabbitears::checkForUpdates(); }
- (void)showAbout:(id)sender { [(AppDelegate*)NSApp.delegate showAboutPanel:sender]; }

- (void)playClicked:(id)__unused sender { [self playRow:_table.clickedRow]; }

// Double-click a row (or Return) plays it; a single click just selects.
- (void)playSelectedRow:(id)__unused sender {
    [self playRow:_table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow];
}

// ---- volume (bottom bar) ----

- (void)volumeChanged:(id)__unused sender {
    const int v = (int)_volume.doubleValue;
    _player->setVolume(v);
    if (v > 0) _preMuteVolume = v;
    _muteBtn.title = (v == 0) ? @"🔇" : @"🔊";
    if (_db) _db->setSetting(L"volume", std::to_wstring(v));
}

- (void)toggleMute:(id)__unused sender {
    _volume.doubleValue = (_volume.doubleValue == 0) ? _preMuteVolume : 0;
    [self volumeChanged:nil];
}

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
    _player->setVolume((int)_volume.doubleValue);  // libVLC resets volume per media
    if (_db) _db->setSetting(L"last_channel_id", std::to_wstring(c.id));  // resume this on next launch
    _window.title = [NSString stringWithFormat:@"RabbitEars — %@", ns(c.name)];
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

@end
