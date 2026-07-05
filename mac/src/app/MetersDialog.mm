// SPDX-License-Identifier: GPL-3.0-or-later
// See MetersDialog.h.
#import "MetersDialog.h"

#include "MeterModel.h"
#include "db/Database.h"

#include <string>

using rabbitears::Database;
using namespace rabbitears::mac;

namespace {

// The four kinds in dialog order, their settings-key stems, and display labels.
const char* kKeys[4]   = {"spectrum", "signal", "bitrate", "frames"};
NSString*   kLabels[4] = {@"Spectrum", @"Signal", @"Bitrate", @"Frames"};
MeterKind   kKinds[4]  = {MeterKind::Spectrum, MeterKind::Signal, MeterKind::Bitrate, MeterKind::Frames};

// ASCII widen/narrow — settings keys and codec values are all ASCII (hex, "led", "1").
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string  narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); }
std::wstring wkey(int i, const char* suffix) {
    return widen(std::string("meter_") + kKeys[i] + suffix);  // e.g. L"meter_spectrum_style"
}

}  // namespace

@implementation MetersDialog {
    Database*      _db;
    NSWindow*      _panel;
    void (^_onApply)(void);
    MeterConfig    _cfg[4];
    NSButton*      _enable[4];
    NSPopUpButton* _style[4];
}

- (instancetype)initWithDatabase:(Database*)db {
    if ((self = [super init])) {
        _db = db;
        [self loadConfig];
    }
    return self;
}

- (void)loadConfig {
    for (int i = 0; i < 4; ++i) {
        _cfg[i] = MeterConfig{};
        _cfg[i].enabled = false;  // meters are opt-in until the user enables them
        _cfg[i].style   = defaultMeterStyle(kKinds[i]);
        _cfg[i].palette = defaultMeterPalette(kKinds[i]);
        if (!_db) continue;
        if (auto e = _db->getSetting(wkey(i, "")))      _cfg[i].enabled = (*e == L"1");
        if (auto s = _db->getSetting(wkey(i, "_style"))) _cfg[i].style = meterStyleFromString(narrow(*s), _cfg[i].style);
    }
}

- (void)presentForWindow:(NSWindow*)parent onApply:(void (^)(void))onApply {
    _onApply = [onApply copy];
    [self buildPanel];
    // The completion handler captures self, keeping the dialog alive until the sheet
    // closes (nothing else retains it once the caller's local is released).
    [parent beginSheet:_panel completionHandler:^(NSModalResponse __unused r) { (void)self; }];
}

- (void)buildPanel {
    NSStackView* root = [NSStackView stackViewWithViews:@[]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 8;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    root.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* hdr =
        [NSTextField labelWithString:@"Show and style each meter. Colours + tuning are coming; "
                                      "the meters themselves wire up next."];
    hdr.textColor = NSColor.secondaryLabelColor;
    hdr.font = [NSFont systemFontOfSize:11];
    [root addArrangedSubview:hdr];

    for (int i = 0; i < 4; ++i) [root addArrangedSubview:[self buildKindRow:i]];
    [root addArrangedSubview:[self buildButtonRow]];

    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 250)];
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor],
    ]];

    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 460, 250)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    _panel.title = @"Meters";
    _panel.contentView = content;
}

- (NSView*)buildKindRow:(int)i {
    NSButton* en = [NSButton checkboxWithTitle:kLabels[i] target:nil action:nil];
    en.state = _cfg[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [en.widthAnchor constraintEqualToConstant:110].active = YES;  // align the popups in a column
    _enable[i] = en;

    NSPopUpButton* sp = [[NSPopUpButton alloc] init];
    [sp addItemsWithTitles:@[ @"LED", @"LCD", @"Tube", @"Scope" ]];  // index == (int)MeterStyle
    [sp selectItemAtIndex:(NSInteger)_cfg[i].style];
    _style[i] = sp;

    NSStackView* row = [NSStackView stackViewWithViews:@[ en, [NSTextField labelWithString:@"Style:"], sp ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 10;
    row.alignment = NSLayoutAttributeCenterY;
    return row;
}

- (NSView*)buildButtonRow {
    NSButton* reset = [NSButton buttonWithTitle:@"Reset to Defaults" target:self action:@selector(reset:)];
    NSButton* cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    cancel.keyEquivalent = @"\033";  // Esc
    NSButton* ok = [NSButton buttonWithTitle:@"OK" target:self action:@selector(ok:)];
    ok.keyEquivalent = @"\r";
    NSView* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView* row = [NSStackView stackViewWithViews:@[ reset, spacer, cancel, ok ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 8;
    return row;
}

- (void)ok:(id)__unused sender {
    for (int i = 0; i < 4; ++i) {
        _cfg[i].enabled = (_enable[i].state == NSControlStateValueOn);
        _cfg[i].style   = (MeterStyle)[_style[i] indexOfSelectedItem];
        if (_db) {
            _db->setSetting(wkey(i, ""),       _cfg[i].enabled ? L"1" : L"0");
            _db->setSetting(wkey(i, "_style"), widen(meterStyleToString(_cfg[i].style)));
        }
    }
    if (_onApply) _onApply();
    [self dismiss];
}

- (void)cancel:(id)__unused sender { [self dismiss]; }

- (void)reset:(id)__unused sender {
    for (int i = 0; i < 4; ++i) {
        _cfg[i].style = defaultMeterStyle(kKinds[i]);
        [_style[i] selectItemAtIndex:(NSInteger)_cfg[i].style];
    }
}

- (void)dismiss {
    [_panel.sheetParent endSheet:_panel];  // fires the completion handler, releasing self
}

@end
