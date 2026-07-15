// SPDX-License-Identifier: GPL-3.0-or-later
// See CategoriesDialog.h.
#import "CategoriesDialog.h"
#import "Tr.h"

using rabbitears::Tr;
using rabbitears::TrF;
using namespace rabbitears::i18n;  // StringId

// A flipped clip view top-anchors a short checklist (see PlaylistsDialog's RETopClipView); named
// uniquely so the two files don't declare the same ObjC class.
@interface RECategoriesClipView : NSClipView
@end
@implementation RECategoriesClipView
- (BOOL)isFlipped { return YES; }
@end

@implementation CategoriesDialog {
    NSWindow*                 _panel;
    NSStackView*              _rows;      // one checkbox per group-title
    NSArray<NSString*>*       _groups;    // sorted group-titles
    NSMutableSet<NSString*>*  _sel;       // currently-checked (included) groups
    NSTextField*              _count;     // "N of M categories selected"
    void (^_onApply)(NSSet<NSString*>*);
}

- (instancetype)initWithGroups:(NSArray<NSString*>*)groups selected:(NSSet<NSString*>*)selected {
    if ((self = [super init])) {
        _groups = [groups sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
        // Constrain the saved selection to groups that STILL EXIST — a persisted category can point at
        // a group whose playlist was since deleted, and such a ghost (no checkbox is ever drawn for it)
        // would corrupt the "N of M" count and the all-checked/none-checked -> "show all" normalization,
        // silently wiping a real filter. Keep the invariant _sel ⊆ _groups. Empty (or all-stale) result
        // means "no filter" — open with EVERYTHING checked so unchecking some builds a restriction.
        NSMutableSet<NSString*>* s = [selected mutableCopy];
        [s intersectSet:[NSSet setWithArray:_groups]];
        _sel = s.count ? s : [NSMutableSet setWithArray:_groups];
    }
    return self;
}

- (void)presentForWindow:(NSWindow*)parent onApply:(void (^)(NSSet<NSString*>*))onApply {
    _onApply = [onApply copy];
    [self buildPanel];
    // The completion handler captures self, keeping the dialog alive until the sheet closes.
    [parent beginSheet:_panel completionHandler:^(NSModalResponse __unused r) { (void)self; }];
}

- (void)buildPanel {
    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 420, 460)];

    NSTextField* prompt = [NSTextField wrappingLabelWithString:Tr(StringId::CategoriesPrompt)];
    prompt.textColor = NSColor.secondaryLabelColor;
    prompt.font = [NSFont systemFontOfSize:11];
    prompt.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:prompt];

    _rows = [NSStackView stackViewWithViews:@[]];
    _rows.orientation = NSUserInterfaceLayoutOrientationVertical;
    _rows.alignment = NSLayoutAttributeLeading;
    _rows.spacing = 2;
    _rows.translatesAutoresizingMaskIntoConstraints = NO;

    NSScrollView* scroll = [[NSScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.borderType = NSBezelBorder;
    RECategoriesClipView* clipView = [[RECategoriesClipView alloc] init];
    clipView.drawsBackground = NO;
    scroll.contentView = clipView;
    scroll.documentView = _rows;
    [content addSubview:scroll];

    NSButton* all = [NSButton buttonWithTitle:Tr(StringId::CategoriesSelectAllButton)
                                       target:self action:@selector(selectAll:)];
    NSButton* clear = [NSButton buttonWithTitle:Tr(StringId::CategoriesClearButton)
                                         target:self action:@selector(clearAll:)];
    _count = [NSTextField labelWithString:@""];
    _count.textColor = NSColor.secondaryLabelColor;
    _count.font = [NSFont systemFontOfSize:11];
    NSStackView* tools = [NSStackView stackViewWithViews:@[ all, clear, _count ]];
    tools.spacing = 8;
    tools.alignment = NSLayoutAttributeCenterY;
    tools.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:tools];

    NSButton* done = [NSButton buttonWithTitle:Tr(StringId::MacPlaylistsDoneButton)
                                        target:self action:@selector(apply:)];
    done.keyEquivalent = @"\r";
    done.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:done];
    NSButton* cancel = [NSButton buttonWithTitle:Tr(StringId::ButtonCancel)
                                          target:self action:@selector(cancel:)];
    cancel.keyEquivalent = @"\033";  // Esc
    cancel.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:cancel];

    NSClipView* clip = scroll.contentView;
    [NSLayoutConstraint activateConstraints:@[
        [prompt.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
        [prompt.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16],
        [prompt.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],

        [scroll.topAnchor constraintEqualToAnchor:prompt.bottomAnchor constant:10],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],

        [tools.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:10],
        [tools.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16],

        [done.topAnchor constraintEqualToAnchor:tools.bottomAnchor constant:12],
        [done.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16],
        [done.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],
        [cancel.centerYAnchor constraintEqualToAnchor:done.centerYAnchor],
        [cancel.trailingAnchor constraintEqualToAnchor:done.leadingAnchor constant:-8],

        [_rows.topAnchor constraintEqualToAnchor:clip.topAnchor],
        [_rows.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor constant:4],
        [_rows.trailingAnchor constraintEqualToAnchor:clip.trailingAnchor],
        [_rows.widthAnchor constraintEqualToAnchor:clip.widthAnchor constant:-4],
    ]];

    [self reloadRows];

    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 420, 460)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    _panel.title = Tr(StringId::CategoriesTitle);
    _panel.contentView = content;
}

- (void)reloadRows {
    for (NSView* v in [_rows.arrangedSubviews copy]) {
        [_rows removeArrangedSubview:v];
        [v removeFromSuperview];
    }
    if (_groups.count == 0) {
        NSTextField* empty = [NSTextField labelWithString:Tr(StringId::CategoriesNoneHeading)];
        empty.textColor = NSColor.secondaryLabelColor;
        [_rows addArrangedSubview:empty];
    } else {
        for (NSString* g in _groups) {
            NSButton* cb = [NSButton checkboxWithTitle:g target:self action:@selector(toggleGroup:)];
            cb.state = [_sel containsObject:g] ? NSControlStateValueOn : NSControlStateValueOff;
            [_rows addArrangedSubview:cb];
        }
    }
    [self updateCount];
}

- (void)toggleGroup:(NSButton*)sender {
    if (sender.state == NSControlStateValueOn) [_sel addObject:sender.title];
    else                                       [_sel removeObject:sender.title];
    [self updateCount];
}

- (void)selectAll:(id)__unused sender {
    [_sel addObjectsFromArray:_groups];
    for (NSView* v in _rows.arrangedSubviews)
        if ([v isKindOfClass:[NSButton class]]) ((NSButton*)v).state = NSControlStateValueOn;
    [self updateCount];
}

- (void)clearAll:(id)__unused sender {
    [_sel removeAllObjects];
    for (NSView* v in _rows.arrangedSubviews)
        if ([v isKindOfClass:[NSButton class]]) ((NSButton*)v).state = NSControlStateValueOff;
    [self updateCount];
}

- (void)updateCount {
    // CategoriesSelectedCount is a "%d of %d" (Win32 printf-style) shared string — fill it directly.
    _count.stringValue = [NSString stringWithFormat:Tr(StringId::CategoriesSelectedCount),
                                                    (int)_sel.count, (int)_groups.count];
}

- (void)apply:(id)__unused sender {
    // Checking ALL (or none via Clear) means "no filter" — hand back an empty set for that.
    NSSet<NSString*>* result = (_sel.count == 0 || _sel.count == _groups.count)
                                   ? [NSSet set]
                                   : [_sel copy];
    if (_onApply) _onApply(result);
    [_panel.sheetParent endSheet:_panel];
}

- (void)cancel:(id)__unused sender {
    [_panel.sheetParent endSheet:_panel];  // no callback — leave the filter unchanged
}

@end
