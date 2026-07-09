// SPDX-License-Identifier: GPL-3.0-or-later
// See PlaylistsDialog.h.
#import "PlaylistsDialog.h"

#include "core/Http.h"
#include "core/M3uParser.h"
#include "db/Database.h"
#include "platform/Encoding.h"

#include <ctime>
#include <memory>
#include <string>
#include <vector>

using rabbitears::Database;
using rabbitears::Playlist;

namespace {
NSString* ns(const std::wstring& w) {
    return [NSString stringWithUTF8String:rabbitears::utf8FromWide(w).c_str()] ?: @"";
}
std::wstring ws(NSString* s) { return rabbitears::wideFromUtf8(s.UTF8String ?: ""); }
}  // namespace

// A non-flipped NSClipView anchors an under-sized document view to the BOTTOM, so a
// short playlist list sinks to the bottom of the scroll box. A flipped clip view
// top-anchors it (and still scrolls normally once the list overflows).
@interface RETopClipView : NSClipView
@end
@implementation RETopClipView
- (BOOL)isFlipped { return YES; }
@end

@implementation PlaylistsDialog {
    Database*             _db;
    NSWindow*             _panel;
    NSStackView*          _rows;      // the scrollable per-playlist list
    void (^_onChange)(void);
    std::vector<Playlist> _playlists;  // backs the row controls' tags (playlist id)
}

- (instancetype)initWithDatabase:(Database*)db {
    if ((self = [super init])) _db = db;
    return self;
}

- (void)presentForWindow:(NSWindow*)parent onChange:(void (^)(void))onChange {
    _onChange = [onChange copy];
    [self buildPanel];
    // The completion handler captures self, keeping the dialog alive until the sheet
    // closes (nothing else retains it once the caller's local is released).
    [parent beginSheet:_panel completionHandler:^(NSModalResponse __unused r) { (void)self; }];
}

- (void)buildPanel {
    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 540, 420)];

    NSTextField* hdr = [NSTextField labelWithString:
        @"Turn a playlist off to hide its channels from the grid and filters, or delete it entirely."];
    hdr.textColor = NSColor.secondaryLabelColor;
    hdr.font = [NSFont systemFontOfSize:11];
    hdr.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:hdr];

    _rows = [NSStackView stackViewWithViews:@[]];
    _rows.orientation = NSUserInterfaceLayoutOrientationVertical;
    _rows.alignment = NSLayoutAttributeLeading;
    _rows.spacing = 4;
    _rows.translatesAutoresizingMaskIntoConstraints = NO;

    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.borderType = NSBezelBorder;
    RETopClipView* clipView = [[RETopClipView alloc] init];  // top-anchor short lists
    clipView.drawsBackground = NO;
    scroll.contentView = clipView;
    scroll.documentView = _rows;
    [content addSubview:scroll];

    NSButton* done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(done:)];
    done.keyEquivalent = @"\r";
    done.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:done];

    NSClipView* clip = scroll.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [hdr.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
        [hdr.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16],
        [hdr.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],

        [scroll.topAnchor constraintEqualToAnchor:hdr.bottomAnchor constant:10],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],

        [done.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:10],
        [done.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16],
        [done.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],

        // Row stack fills the clip width; its height stays intrinsic so the list scrolls.
        [_rows.topAnchor constraintEqualToAnchor:clip.topAnchor],
        [_rows.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
        [_rows.trailingAnchor constraintEqualToAnchor:clip.trailingAnchor],
        [_rows.widthAnchor constraintEqualToAnchor:clip.widthAnchor],
    ]];

    [self reloadRows];

    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 540, 420)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    _panel.title = @"Manage Playlists";
    _panel.contentView = content;
}

// (Re)populate the list from the DB. Called on open and after each delete.
- (void)reloadRows {
    for (NSView* v in [_rows.arrangedSubviews copy]) {
        [_rows removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    _playlists = _db ? _db->listPlaylists() : std::vector<Playlist>{};

    if (_playlists.empty()) {
        NSTextField* empty = [NSTextField labelWithString:
            @"No playlists yet — close this window, then use “＋ Add Playlist”."];
        empty.textColor = NSColor.secondaryLabelColor;
        [_rows addArrangedSubview:empty];
        return;
    }
    for (const auto& p : _playlists) {
        NSView* row = [self rowForPlaylist:p];
        [_rows addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:_rows.widthAnchor].active = YES;  // full-width rows
    }
}

- (NSView*)rowForPlaylist:(const Playlist&)p {
    NSButton* en = [NSButton checkboxWithTitle:ns(p.name) target:self action:@selector(toggleEnabled:)];
    en.state = p.enabled ? NSControlStateValueOn : NSControlStateValueOff;
    en.tag = (NSInteger)p.id;
    en.toolTip = @"Show or hide this playlist’s channels";
    [en.widthAnchor constraintLessThanOrEqualToConstant:280].active = YES;  // long names truncate
    [en setContentHuggingPriority:NSLayoutPriorityDefaultHigh
                   forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSString* src = ns(p.isUrl ? p.sourceUrl : p.sourcePath);
    NSString* detail =
        [NSString stringWithFormat:@"%d ch · %@", p.channelCount, src.length ? src : @"—"];
    NSTextField* info = [NSTextField labelWithString:detail];
    info.textColor = NSColor.secondaryLabelColor;
    info.font = [NSFont systemFontOfSize:11];
    info.lineBreakMode = NSLineBreakByTruncatingTail;
    info.toolTip = src;
    [info setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                   forOrientation:NSLayoutConstraintOrientationHorizontal];
    [info setContentHuggingPriority:NSLayoutPriorityDefaultHigh
                     forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSView* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSButton* refresh = [self iconButton:@"arrow.clockwise" tip:@"Re-download / re-read from source"
                                  action:@selector(refreshPlaylist:) pid:p.id];
    NSButton* guide = [self iconButton:@"calendar" tip:@"Set the XMLTV guide (EPG) URL"
                                action:@selector(setGuideUrl:) pid:p.id];
    NSButton* rename = [self iconButton:@"pencil" tip:@"Rename"
                                 action:@selector(renamePlaylist:) pid:p.id];

    NSButton* del = [NSButton buttonWithTitle:@"Delete" target:self action:@selector(deletePlaylist:)];
    del.tag = (NSInteger)p.id;
    [del setContentHuggingPriority:NSLayoutPriorityRequired
                    forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView* row = [NSStackView stackViewWithViews:@[ en, info, spacer, refresh, guide, rename, del ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 8;
    row.alignment = NSLayoutAttributeCenterY;
    row.edgeInsets = NSEdgeInsetsMake(2, 6, 2, 6);
    return row;
}

// A compact bordered SF-Symbol button carrying a playlist id in its tag.
- (NSButton*)iconButton:(NSString*)symbol tip:(NSString*)tip action:(SEL)action pid:(long long)pid {
    NSImage* img = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:tip];
    NSButton* b = img ? [NSButton buttonWithImage:img target:self action:action]
                      : [NSButton buttonWithTitle:tip target:self action:action];
    b.tag = (NSInteger)pid;
    b.toolTip = tip;
    [b setContentHuggingPriority:NSLayoutPriorityRequired
                  forOrientation:NSLayoutConstraintOrientationHorizontal];
    return b;
}

// A checkbox toggle enables/disables the playlist immediately; the grid refreshes
// via onChange. No dialog rebuild needed — the row stays put.
- (void)toggleEnabled:(NSButton*)sender {
    if (_db) _db->setPlaylistEnabled((long long)sender.tag, sender.state == NSControlStateValueOn);
    if (_onChange) _onChange();
}

- (void)deletePlaylist:(NSButton*)sender {
    const long long pid = (long long)sender.tag;
    NSString* name = @"this playlist";
    int count = 0;
    for (const auto& p : _playlists)
        if (p.id == pid) { name = ns(p.name); count = p.channelCount; }

    NSAlert* a = [[NSAlert alloc] init];
    a.alertStyle = NSAlertStyleWarning;
    a.messageText = [NSString stringWithFormat:@"Delete “%@”?", name];
    a.informativeText = [NSString stringWithFormat:
        @"This permanently removes the playlist and its %d channel%@ from RabbitEars, including "
        @"any favourites and custom channel numbers you set on them. This cannot be undone.",
        count, count == 1 ? @"" : @"s"];
    NSButton* del = [a addButtonWithTitle:@"Delete"];
    del.hasDestructiveAction = YES;
    del.keyEquivalent = @"";  // don't let Return fire the destructive action
    NSButton* cancel = [a addButtonWithTitle:@"Cancel"];
    cancel.keyEquivalent = @"\033";  // Esc cancels

    [a beginSheetModalForWindow:_panel completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;  // Delete is the first button
        if (_db) _db->deletePlaylist(pid);
        if (_onChange) _onChange();
        [self reloadRows];
    }];
}

- (void)renamePlaylist:(NSButton*)sender {
    const long long pid = (long long)sender.tag;
    NSString* current = @"";
    for (const auto& p : _playlists) if (p.id == pid) current = ns(p.name);

    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"Rename Playlist";
    a.informativeText = @"This changes only the display name; the channels and source are untouched.";
    [a addButtonWithTitle:@"Rename"];
    NSButton* cancel = [a addButtonWithTitle:@"Cancel"];
    cancel.keyEquivalent = @"\033";  // Esc cancels
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    input.stringValue = current;
    a.accessoryView = input;
    [a.window setInitialFirstResponder:input];

    [a beginSheetModalForWindow:_panel completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;  // Rename is the first button
        NSString* name = [input.stringValue
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!name.length) return;  // ignore an empty name
        if (_db) _db->renamePlaylist(pid, ws(name));
        if (_onChange) _onChange();
        [self reloadRows];
    }];
}

// Set/clear a playlist's XMLTV guide (EPG) URL — seeded with its current one (the M3U
// x-tvg-url or a prior override). The View ▸ Refresh Guide command downloads it. Peer of
// the Win32 promptSetGuideUrl.
- (void)setGuideUrl:(NSButton*)sender {
    const long long pid = (long long)sender.tag;
    NSString* current = @"";
    for (const auto& p : _playlists) if (p.id == pid) current = ns(p.epgUrl);

    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"Set Guide URL";
    a.informativeText = @"XMLTV guide URL (.xml or .xml.gz). Leave blank to clear it. Then use "
                        @"“Refresh Guide” (the View menu or the ⚙ menu) to download it.";
    [a addButtonWithTitle:@"Save"];
    NSButton* cancel = [a addButtonWithTitle:@"Cancel"];
    cancel.keyEquivalent = @"\033";  // Esc cancels
    NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 360, 24)];
    input.stringValue = current;
    input.placeholderString = @"https://…/guide.xml.gz";
    a.accessoryView = input;
    [a.window setInitialFirstResponder:input];

    [a beginSheetModalForWindow:_panel completionHandler:^(NSModalResponse resp) {
        if (resp != NSAlertFirstButtonReturn) return;  // Save is the first button
        NSString* url = [input.stringValue
            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (_db) _db->setPlaylistEpgUrl(pid, ws(url));  // empty clears the override
        if (_onChange) _onChange();
        [self reloadRows];  // refresh the (unchanged) list; keeps the panel in sync
    }];
}

// Re-download (URL) or re-read (file) the source and upsert its channels into the
// existing playlist — new/changed channels appear; favourites + custom LCNs are kept
// (bulkInsertChannels upserts by playlist_id+stream_url). Runs off the main queue;
// a weak self-ref makes it a no-op if the dialog is dismissed mid-fetch.
- (void)refreshPlaylist:(NSButton*)sender {
    const long long pid = (long long)sender.tag;
    std::wstring source;
    bool isUrl = true;
    for (const auto& p : _playlists)
        if (p.id == pid) { source = p.isUrl ? p.sourceUrl : p.sourcePath; isUrl = p.isUrl; }
    if (source.empty() || !_db) return;

    sender.enabled = NO;
    sender.toolTip = @"Refreshing…";
    const long long now = (long long)time(nullptr);
    __weak PlaylistsDialog* weak = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        auto doc = std::make_shared<rabbitears::M3uDocument>();
        bool ok = false;
        if (isUrl) {
            std::string bytes;
            std::wstring err;
            ok = rabbitears::httpGet(source, bytes, err);
            if (ok) *doc = rabbitears::parseM3u(bytes);
        } else {
            std::wstring err;
            *doc = rabbitears::parseM3uFile(source, &err);
            ok = err.empty();
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            PlaylistsDialog* s = weak;
            if (!s) return;
            if (ok && s->_db) s->_db->bulkInsertChannels(pid, doc->channels, now);
            if (s->_onChange) s->_onChange();
            [s reloadRows];  // rebuilds rows (updated channel count) + re-enables the button
        });
    });
}

- (void)done:(id)__unused sender {
    if (_panel.attachedSheet) return;  // a delete-confirm is up — don't tear the panel out from under it
    [_panel.sheetParent endSheet:_panel];
}

@end
