// SPDX-License-Identifier: GPL-3.0-or-later
// See MainWindowController.h.
#import "MainWindowController.h"

#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "core/Http.h"
#include "core/M3uParser.h"
#include "db/Database.h"
#include "models/Channel.h"
#include "platform/Encoding.h"
#include "platform/Log.h"

#import "VlcPlayerMac.h"

using namespace rabbitears;

@implementation MainWindowController {
    NSWindow*    _window;
    NSTextField* _urlField;
    NSTableView* _table;
    NSView*      _videoView;
    NSTextField* _status;

    std::unique_ptr<Database>     _db;
    std::unique_ptr<VlcPlayerMac> _player;
    std::vector<Channel>          _channels;
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

- (void)showWindow {
    const NSRect frame = NSMakeRect(0, 0, 960, 600);
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

    // ---- top bar: URL field + Load + Open File + Stop ----
    const CGFloat barH = 44;
    NSView* bar = [[NSView alloc]
        initWithFrame:NSMakeRect(0, frame.size.height - barH, frame.size.width, barH)];
    bar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;

    _urlField = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 9, frame.size.width - 320, 26)];
    _urlField.placeholderString = @"M3U / M3U8 playlist URL…";
    _urlField.autoresizingMask = NSViewWidthSizable;
    _urlField.target = self;
    _urlField.action = @selector(loadURL:);  // Enter in the field loads
    [bar addSubview:_urlField];

    NSButton* loadBtn = [NSButton buttonWithTitle:@"Load URL" target:self action:@selector(loadURL:)];
    loadBtn.frame = NSMakeRect(frame.size.width - 298, 8, 92, 28);
    loadBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:loadBtn];

    NSButton* openBtn = [NSButton buttonWithTitle:@"Open File…" target:self action:@selector(openFile:)];
    openBtn.frame = NSMakeRect(frame.size.width - 202, 8, 104, 28);
    openBtn.autoresizingMask = NSViewMinXMargin;
    [bar addSubview:openBtn];

    NSButton* stopBtn = [NSButton buttonWithTitle:@"Stop" target:self action:@selector(stop:)];
    stopBtn.frame = NSMakeRect(frame.size.width - 92, 8, 80, 28);
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

    // ---- split: channel list | video ----
    NSSplitView* split = [[NSSplitView alloc]
        initWithFrame:NSMakeRect(0, statusH, frame.size.width, frame.size.height - barH - statusH)];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 300, 100)];
    scroll.hasVerticalScroller = YES;
    _table = [[NSTableView alloc] initWithFrame:scroll.bounds];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    col.title = @"Channels";
    col.width = 280;
    [_table addTableColumn:col];
    _table.headerView = [[NSTableHeaderView alloc] init];
    _table.dataSource = self;
    _table.delegate = self;
    _table.usesAlternatingRowBackgroundColors = YES;
    scroll.documentView = _table;
    [split addSubview:scroll];

    _videoView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 660, 100)];
    _videoView.wantsLayer = YES;
    _videoView.layer.backgroundColor = NSColor.blackColor.CGColor;
    [split addSubview:_videoView];

    [content addSubview:split];
    [split setPosition:300 ofDividerAtIndex:0];

    _player->attachTo(_videoView);  // hand libVLC the NSView to render into

    [self restoreLastPlaylist];

    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)setStatus:(NSString*)s { _status.stringValue = s; }

static NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:utf8FromWide(w).c_str()] ?: @"";
}

- (void)restoreLastPlaylist {
    if (!_db) { [self setStatus:@"Database unavailable — check disk permissions and restart."]; return; }
    const auto playlists = _db->listPlaylists();
    if (playlists.empty()) {
        [self setStatus:@"Ready — load an M3U URL or open a file to begin."];
        return;
    }
    [self loadChannelsForPlaylist:playlists.back().id];
}

- (void)loadChannelsForPlaylist:(long long)pid {
    if (!_db) return;
    _channels = _db->channelsByPlaylist(pid);
    // Clear any stale selection BEFORE reloading, and suppress the resulting
    // selection-change notification so swapping playlists never auto-plays a
    // channel the user didn't pick.
    _suppressSelectionPlay = YES;
    [_table deselectAll:nil];
    [_table reloadData];
    _suppressSelectionPlay = NO;
    [self setStatus:[NSString stringWithFormat:@"%lu channels — select one to play.",
                     (unsigned long)_channels.size()]];
}

// ---- actions ----

- (void)loadURL:(id)__unused sender {
    NSString* u = _urlField.stringValue;
    if (!u.length) return;
    const std::wstring url = wideFromUtf8(u.UTF8String);
    const uint64_t token = ++_loadToken;  // newest request wins; supersedes any in flight
    [self setStatus:@"Downloading playlist…"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        std::string bytes;
        std::wstring derr;
        const bool ok = httpGet(url, bytes, derr);
        auto doc = std::make_shared<M3uDocument>(ok ? parseM3u(bytes) : M3uDocument{});
        dispatch_async(dispatch_get_main_queue(), ^{
            if (token != self->_loadToken) return;  // a newer load superseded this one
            if (!ok) {
                [self setStatus:[NSString stringWithFormat:@"Download failed: %@", ns(derr)]];
                return;
            }
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
        const std::wstring path = wideFromUtf8(panel.URL.path.UTF8String);
        std::wstring perr;
        const M3uDocument doc = parseM3uFile(path, &perr);
        if (!perr.empty()) {
            [self setStatus:[NSString stringWithFormat:@"Read failed: %@", ns(perr)]];
            return;
        }
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
    [self loadChannelsForPlaylist:pid];
}

- (void)stop:(id)__unused sender {
    _player->stop();
    [self setStatus:@"Stopped."];
}

- (void)playRow:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_channels.size()) return;
    const Channel& c = _channels[(size_t)row];
    _player->play(c.streamUrl, c.userAgent, c.referrer);
    [self setStatus:[NSString stringWithFormat:@"Playing: %@", ns(c.name)]];
}

// ---- NSTableView data source / delegate ----

- (NSInteger)numberOfRowsInTableView:(NSTableView*)__unused t {
    return (NSInteger)_channels.size();
}

- (id)tableView:(NSTableView*)__unused t
    objectValueForTableColumn:(NSTableColumn*)__unused col
                          row:(NSInteger)row {
    if (row < 0 || row >= (NSInteger)_channels.size()) return @"";
    return ns(_channels[(size_t)row].name);
}

- (void)tableViewSelectionDidChange:(NSNotification*)__unused n {
    if (_suppressSelectionPlay) return;  // programmatic (deselect/reload) change, not a user pick
    [self playRow:_table.selectedRow];
}

@end
