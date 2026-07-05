// SPDX-License-Identifier: GPL-3.0-or-later
// See TermsDialog.h.
#import "TermsDialog.h"

namespace {
// Verbatim peer of Win32 kTermsText (Win32/ui/Dialogs.cpp). Keep the two in sync.
NSString* const kTermsText =
    @"Please read these terms before using RabbitEars. By choosing “I Accept” you agree to them. "
    @"If you do not agree, choose “Decline” and the application will close.\n\n"
    @"1.  Educational purpose.  RabbitEars is a media player provided for educational and personal "
    @"use only. It is offered “as is”, without warranty of any kind, and you use it entirely at "
    @"your own risk.\n\n"
    @"2.  No content is included.  RabbitEars ships with no channels, playlists, or media of any "
    @"kind. It plays only the playlists that you choose to add. You are solely responsible for "
    @"obtaining your playlists from lawful sources and for ensuring that your use complies with all "
    @"applicable laws and the rights of content owners in your jurisdiction.\n\n"
    @"3.  No endorsement.  The authors of RabbitEars do not provide, host, recommend, or endorse "
    @"any stream or content, and have no knowledge of or control over what you choose to play. As "
    @"the project puts it: we don’t know, and we don’t care.\n\n"
    @"4.  Your responsibility.  Any illegal activity carried out with this software is yours alone "
    @"and is not supported, encouraged, or condoned by the authors.\n\n"
    @"5.  Open source.  RabbitEars plays media using libVLC, © VideoLAN and the VLC contributors, "
    @"under the GNU LGPL v2.1.\n\n"
    @"By clicking “I Accept”, you confirm that you have read, understood, and agree to these terms.";
}  // namespace

@implementation TermsDialog {
    NSWindow* _panel;
}

- (BOOL)runModal {
    [self build];
    // The gate runs from applicationDidFinishLaunching, before -showWindow activates the
    // app — so bring the app + panel frontmost, else on a non-Finder launch (login item,
    // `open -a`) the modal can appear behind the previously-active app.
    [NSApp activateIgnoringOtherApps:YES];
    const NSModalResponse r = [NSApp runModalForWindow:_panel];
    [_panel orderOut:nil];
    return r == NSModalResponseOK;
}

- (void)build {
    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 470)];

    NSTextField* title = [NSTextField labelWithString:@"Terms of Use"];
    title.font = [NSFont boldSystemFontOfSize:15];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:title];

    // Read-only, scrollable terms. Standard NSTextView-in-NSScrollView setup: the text
    // view tracks the clip width and grows vertically so long text scrolls.
    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = YES;
    scroll.borderType = NSBezelBorder;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextView* tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 500, 100)];
    tv.minSize = NSMakeSize(0, 0);
    tv.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    tv.verticallyResizable = YES;
    tv.horizontallyResizable = NO;
    tv.autoresizingMask = NSViewWidthSizable;
    tv.editable = NO;
    tv.selectable = YES;
    tv.textContainerInset = NSMakeSize(6, 8);
    tv.font = [NSFont systemFontOfSize:12];
    tv.textContainer.widthTracksTextView = YES;
    tv.string = kTermsText;
    scroll.documentView = tv;
    [content addSubview:scroll];

    NSButton* decline = [NSButton buttonWithTitle:@"Decline" target:self action:@selector(decline:)];
    decline.keyEquivalent = @"\033";  // Esc
    decline.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:decline];

    NSButton* accept = [NSButton buttonWithTitle:@"I Accept" target:self action:@selector(accept:)];
    accept.keyEquivalent = @"\r";
    accept.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:accept];

    [NSLayoutConstraint activateConstraints:@[
        [title.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
        [title.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],

        [scroll.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:10],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],

        [accept.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:12],
        [accept.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16],
        [accept.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [decline.centerYAnchor constraintEqualToAnchor:accept.centerYAnchor],
        [decline.trailingAnchor constraintEqualToAnchor:accept.leadingAnchor constant:-10],
    ]];

    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 560, 470)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    _panel.title = @"RabbitEars";
    _panel.contentView = content;
    [_panel center];
}

- (void)accept:(id)__unused sender  { [NSApp stopModalWithCode:NSModalResponseOK]; }
- (void)decline:(id)__unused sender { [NSApp stopModalWithCode:NSModalResponseCancel]; }

@end
