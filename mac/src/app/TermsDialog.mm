// SPDX-License-Identifier: GPL-3.0-or-later
// See TermsDialog.h.
#import "TermsDialog.h"
#import "Tr.h"

using namespace rabbitears;
using namespace rabbitears::i18n;  // StringId

@implementation TermsDialog {
    NSWindow* _panel;
    NSString* _version;
}

- (instancetype)initWithVersion:(NSString*)version {
    if ((self = [super init])) _version = [version copy];
    return self;
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

    NSTextField* title = [NSTextField labelWithString:Tr(StringId::TermsHeading)];
    title.font = [NSFont boldSystemFontOfSize:15];
    title.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:title];

    NSTextField* ver = [NSTextField labelWithString:
        _version.length ? TrF(StringId::MacTermsVersionLabel, {_version}) : @""];
    ver.font = [NSFont systemFontOfSize:11];
    ver.textColor = NSColor.secondaryLabelColor;
    ver.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:ver];

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
    tv.string = Tr(StringId::TermsBodyText);
    scroll.documentView = tv;
    [content addSubview:scroll];

    NSButton* decline = [NSButton buttonWithTitle:Tr(StringId::TermsDeclineButton) target:self action:@selector(decline:)];
    decline.keyEquivalent = @"\033";  // Esc
    decline.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:decline];

    NSButton* accept = [NSButton buttonWithTitle:Tr(StringId::TermsAcceptButton) target:self action:@selector(accept:)];
    accept.keyEquivalent = @"\r";
    accept.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:accept];

    [NSLayoutConstraint activateConstraints:@[
        [title.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
        [title.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [ver.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:2],
        [ver.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],

        [scroll.topAnchor constraintEqualToAnchor:ver.bottomAnchor constant:10],
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
    _panel.title = Tr(StringId::AppName);
    _panel.contentView = content;
    [_panel center];
}

- (void)accept:(id)__unused sender  { [NSApp stopModalWithCode:NSModalResponseOK]; }
- (void)decline:(id)__unused sender { [NSApp stopModalWithCode:NSModalResponseCancel]; }

@end
