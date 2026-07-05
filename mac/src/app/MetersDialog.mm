// SPDX-License-Identifier: GPL-3.0-or-later
// See MetersDialog.h.
#import "MetersDialog.h"

#include "MeterModel.h"
#include "db/Database.h"

#include <cmath>
#include <string>

using rabbitears::Database;
using namespace rabbitears::mac;

namespace {

// The four kinds in dialog order, their settings-key stems, and display labels.
const char* kKeys[4]   = {"spectrum", "signal", "bitrate", "frames"};
NSString*   kLabels[4] = {@"Spectrum", @"Signal", @"Bitrate", @"Frames"};
MeterKind   kKinds[4]  = {MeterKind::Spectrum, MeterKind::Signal, MeterKind::Bitrate, MeterKind::Frames};
NSString*   kColorTips[7] = {@"Background", @"Unlit", @"Low", @"Mid", @"High", @"Accent", @"Peak"};

NSColor* nscolor(const rabbitears::SkinColor& c) {
    return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:c.a / 255.0];
}
rabbitears::SkinColor skinFromNS(NSColor* col) {
    NSColor* c = [col colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    rabbitears::SkinColor s;
    s.r = (uint8_t)std::lround(c.redComponent * 255);
    s.g = (uint8_t)std::lround(c.greenComponent * 255);
    s.b = (uint8_t)std::lround(c.blueComponent * 255);
    s.a = (uint8_t)std::lround(c.alphaComponent * 255);
    s.inherit = false;
    return s;
}

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
    NSColorWell*   _well[4][7];  // bg, off, low, mid, high, accent, peak
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
        if (auto e = _db->getSetting(wkey(i, "")))       _cfg[i].enabled = (*e == L"1");
        if (auto s = _db->getSetting(wkey(i, "_style")))  _cfg[i].style = meterStyleFromString(narrow(*s), _cfg[i].style);
        if (auto c = _db->getSetting(wkey(i, "_colors"))) _cfg[i].palette = meterPaletteFromString(narrow(*c), _cfg[i].palette);
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
        [NSTextField labelWithString:@"Show, style, and colour each meter. Tuning + a live preview are next."];
    hdr.textColor = NSColor.secondaryLabelColor;
    hdr.font = [NSFont systemFontOfSize:11];
    [root addArrangedSubview:hdr];

    for (int i = 0; i < 4; ++i) [root addArrangedSubview:[self buildKindRow:i]];
    [root addArrangedSubview:[self buildButtonRow]];

    [root layoutSubtreeIfNeeded];
    NSSize fit = root.fittingSize;  // size the panel to its content
    if (fit.width  < 440) fit.width  = 440;
    if (fit.height < 180) fit.height = 180;
    NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, fit.width, fit.height)];
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor],
        [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];

    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, fit.width, fit.height)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
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

    NSStackView* row1 = [NSStackView stackViewWithViews:@[ en, [NSTextField labelWithString:@"Style:"], sp ]];
    row1.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row1.spacing = 10;
    row1.alignment = NSLayoutAttributeCenterY;

    // Colours: 7 wells (bg, off, low, mid, high, accent, peak).
    NSMutableArray<NSView*>* wells = [NSMutableArray arrayWithObject:[NSTextField labelWithString:@"Colours:"]];
    const MeterPalette& p = _cfg[i].palette;
    const rabbitears::SkinColor roles[7] = {p.bg, p.off, p.low, p.mid, p.high, p.accent, p.peak};
    for (int j = 0; j < 7; ++j) {
        NSColorWell* w = [[NSColorWell alloc] init];
        w.color = (j == 0 && roles[0].inherit) ? [NSColor colorWithWhite:0.08 alpha:1.0] : nscolor(roles[j]);
        w.toolTip = kColorTips[j];
        [w.widthAnchor constraintEqualToConstant:28].active = YES;
        [w.heightAnchor constraintEqualToConstant:18].active = YES;
        _well[i][j] = w;
        [wells addObject:w];
    }
    NSStackView* row2 = [NSStackView stackViewWithViews:wells];
    row2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row2.spacing = 4;
    row2.alignment = NSLayoutAttributeCenterY;

    NSStackView* col = [NSStackView stackViewWithViews:@[ row1, row2 ]];
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 5;
    return col;
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
        MeterPalette p = _cfg[i].palette;
        p.bg     = skinFromNS(_well[i][0].color);
        p.off    = skinFromNS(_well[i][1].color);
        p.low    = skinFromNS(_well[i][2].color);
        p.mid    = skinFromNS(_well[i][3].color);
        p.high   = skinFromNS(_well[i][4].color);
        p.accent = skinFromNS(_well[i][5].color);
        p.peak   = skinFromNS(_well[i][6].color);
        _cfg[i].palette = p;
        if (_db) {
            _db->setSetting(wkey(i, ""),        _cfg[i].enabled ? L"1" : L"0");
            _db->setSetting(wkey(i, "_style"),  widen(meterStyleToString(_cfg[i].style)));
            _db->setSetting(wkey(i, "_colors"), widen(meterPaletteToString(_cfg[i].palette)));
        }
    }
    if (_onApply) _onApply();
    [self dismiss];
}

- (void)cancel:(id)__unused sender { [self dismiss]; }

- (void)reset:(id)__unused sender {
    for (int i = 0; i < 4; ++i) {
        _cfg[i].style   = defaultMeterStyle(kKinds[i]);
        _cfg[i].palette = defaultMeterPalette(kKinds[i]);
        [_style[i] selectItemAtIndex:(NSInteger)_cfg[i].style];
        const MeterPalette& p = _cfg[i].palette;
        const rabbitears::SkinColor roles[7] = {p.bg, p.off, p.low, p.mid, p.high, p.accent, p.peak};
        for (int j = 0; j < 7; ++j)
            _well[i][j].color = (j == 0 && roles[0].inherit) ? [NSColor colorWithWhite:0.08 alpha:1.0] : nscolor(roles[j]);
    }
}

- (void)dismiss {
    [_panel.sheetParent endSheet:_panel];  // fires the completion handler, releasing self
}

@end
