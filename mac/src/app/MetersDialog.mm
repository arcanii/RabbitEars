// SPDX-License-Identifier: GPL-3.0-or-later
// See MetersDialog.h.
#import "MetersDialog.h"

#import "MeterView.h"
#include "MeterModel.h"
#include "db/Database.h"
#import "Tr.h"

#include <algorithm>
#include <cmath>
#include <string>

using rabbitears::Database;
using namespace rabbitears::mac;
using namespace rabbitears;
using namespace rabbitears::i18n;  // StringId

namespace {

// The four kinds in dialog order, their settings-key stems, and display labels. Labels are
// StringIds (not NSStrings): these are load-time globals, so they resolve through Tr() at the
// use sites — after the active language is set — rather than at static-init time.
const char* kKeys[4]   = {"spectrum", "signal", "bitrate", "frames"};
StringId    kLabels[4] = {StringId::MacMeterNameSpectrum, StringId::MacMeterNameSignal,
                          StringId::MeterNameBitrate, StringId::MacMeterNameFrames};
MeterKind   kKinds[4]  = {MeterKind::Spectrum, MeterKind::Signal, MeterKind::Bitrate, MeterKind::Frames};
StringId    kColorTips[7] = {StringId::MacMetersTipBackground, StringId::MacMetersTipUnlit,
                             StringId::MeterRoleLow, StringId::MeterRoleMid, StringId::MeterRoleHigh,
                             StringId::MeterRoleAccent, StringId::MeterPeak};
// Tuning knobs in MeterTuning order (glow, smoothing, sensitivity, peakHold, breathing).
StringId    kTuneLabels[5] = {StringId::MeterKnobGlow, StringId::MeterKnobSmooth, StringId::MeterKnobSens,
                              StringId::MeterPeak, StringId::MeterKnobBreathe};

NSColor* nscolor(const rabbitears::SkinColor& c) {
    return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:c.a / 255.0];
}
rabbitears::SkinColor skinFromNS(NSColor* col) {
    NSColor* c = [col colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: NSColor.blackColor;
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
    NSSlider*      _tune[4][5];  // glow, smoothing, sensitivity, peakHold, breathing
    MeterView*     _preview[4];  // live preview, fed synthetic data by _previewTimer
    NSTimer*       _previewTimer;
    double         _phase;
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
        _cfg[i].tuning  = defaultMeterTuning();
        if (!_db) continue;
        if (auto e = _db->getSetting(wkey(i, "")))        _cfg[i].enabled = (*e == L"1");
        if (auto s = _db->getSetting(wkey(i, "_style")))  _cfg[i].style   = meterStyleFromString(narrow(*s), _cfg[i].style);
        if (auto c = _db->getSetting(wkey(i, "_colors"))) _cfg[i].palette = meterPaletteFromString(narrow(*c), _cfg[i].palette);
        if (auto t = _db->getSetting(wkey(i, "_tuning"))) _cfg[i].tuning  = meterTuningFromString(narrow(*t), _cfg[i].tuning);
    }
}

- (void)presentForWindow:(NSWindow*)parent onApply:(void (^)(void))onApply {
    _onApply = [onApply copy];
    [self buildPanel];
    // Animate the previews with synthetic data so style/colour/tuning changes read live.
    __weak MetersDialog* weak = self;
    _previewTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 20.0 repeats:YES block:^(NSTimer* t) {
        MetersDialog* s = weak;
        if (s) [s pumpPreviews];
        else   [t invalidate];
    }];
    // The completion handler captures self, keeping the dialog alive until the sheet
    // closes (nothing else retains it once the caller's local is released).
    [parent beginSheet:_panel completionHandler:^(NSModalResponse __unused r) { (void)self; }];
}

- (void)buildPanel {
    NSStackView* root = [NSStackView stackViewWithViews:@[]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 10;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);
    root.translatesAutoresizingMaskIntoConstraints = NO;

    NSTextField* hdr =
        [NSTextField labelWithString:Tr(StringId::MacMetersDialogSubheading)];
    hdr.textColor = NSColor.secondaryLabelColor;
    hdr.font = [NSFont systemFontOfSize:11];
    [root addArrangedSubview:hdr];

    for (int i = 0; i < 4; ++i) [root addArrangedSubview:[self buildKindRow:i]];
    [root addArrangedSubview:[self buildButtonRow]];

    [root layoutSubtreeIfNeeded];
    NSSize fit = root.fittingSize;  // size the panel to its content
    if (fit.width  < 520) fit.width  = 520;
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
    _panel.title = Tr(StringId::MetersWindowTitle);
    _panel.contentView = content;
}

- (NSView*)buildKindRow:(int)i {
    NSButton* en = [NSButton checkboxWithTitle:Tr(kLabels[i]) target:self action:@selector(controlChanged:)];
    en.state = _cfg[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
    en.tag = i;
    [en.widthAnchor constraintEqualToConstant:100].active = YES;  // align the popups in a column
    _enable[i] = en;

    NSPopUpButton* sp = [[NSPopUpButton alloc] init];
    [sp addItemsWithTitles:@[ Tr(StringId::MeterLookLed), Tr(StringId::MeterLookLcd),
                              Tr(StringId::MeterLookVacuumTube), Tr(StringId::MeterLookOscilloscope) ]];  // index == (int)MeterStyle
    [sp selectItemAtIndex:(NSInteger)_cfg[i].style];
    sp.target = self;
    sp.action = @selector(controlChanged:);
    sp.tag = i;
    _style[i] = sp;

    // Live preview at the right of the header row.
    MeterView* pv = [[MeterView alloc] initWithKind:kKinds[i]];
    [pv.widthAnchor constraintEqualToConstant:120].active = YES;
    [pv.heightAnchor constraintEqualToConstant:22].active = YES;
    if (kKinds[i] == MeterKind::Spectrum) [pv setAvailable:YES];  // synthetic feed, never denied
    [pv setConfig:_cfg[i]];
    _preview[i] = pv;

    NSView* hspacer = [[NSView alloc] init];
    [hspacer setContentHuggingPriority:NSLayoutPriorityDefaultLow
                        forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView* row1 = [NSStackView stackViewWithViews:@[ en, [NSTextField labelWithString:Tr(StringId::MacMetersStyleLabel)],
                                                           sp, hspacer, pv ]];
    row1.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row1.spacing = 10;
    row1.alignment = NSLayoutAttributeCenterY;

    // Colours: 7 wells (bg, off, low, mid, high, accent, peak).
    NSMutableArray<NSView*>* wells = [NSMutableArray arrayWithObject:[NSTextField labelWithString:Tr(StringId::MacMetersColoursLabel)]];
    const MeterPalette& p = _cfg[i].palette;
    const rabbitears::SkinColor roles[7] = {p.bg, p.off, p.low, p.mid, p.high, p.accent, p.peak};
    for (int j = 0; j < 7; ++j) {
        NSColorWell* w = [[NSColorWell alloc] init];
        w.color = (j == 0 && roles[0].inherit) ? [NSColor colorWithWhite:0.08 alpha:1.0] : nscolor(roles[j]);
        w.toolTip = Tr(kColorTips[j]);
        w.target = self;
        w.action = @selector(controlChanged:);
        w.tag = i;
        [w.widthAnchor constraintEqualToConstant:28].active = YES;
        [w.heightAnchor constraintEqualToConstant:18].active = YES;
        _well[i][j] = w;
        [wells addObject:w];
    }
    NSStackView* row2 = [NSStackView stackViewWithViews:wells];
    row2.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row2.spacing = 4;
    row2.alignment = NSLayoutAttributeCenterY;

    // Tuning: 5 sliders (0..1). Label + slider per knob.
    const float knobs[5] = {_cfg[i].tuning.glow, _cfg[i].tuning.smoothing, _cfg[i].tuning.sensitivity,
                            _cfg[i].tuning.peakHold, _cfg[i].tuning.breathing};
    NSMutableArray<NSView*>* tune = [NSMutableArray arrayWithObject:[NSTextField labelWithString:Tr(StringId::MacMetersTuningLabel)]];
    for (int j = 0; j < 5; ++j) {
        NSTextField* lbl = [NSTextField labelWithString:Tr(kTuneLabels[j])];
        lbl.font = [NSFont systemFontOfSize:10];
        lbl.textColor = NSColor.secondaryLabelColor;
        NSSlider* sl = [NSSlider sliderWithValue:knobs[j] minValue:0.0 maxValue:1.0
                                          target:self action:@selector(controlChanged:)];
        sl.continuous = YES;
        sl.tag = i * 10 + j;  // decode: kind = tag/10
        sl.toolTip = TrF(StringId::MacMetersKnobTooltip, {Tr(kTuneLabels[j])});
        [sl.widthAnchor constraintEqualToConstant:56].active = YES;
        _tune[i][j] = sl;
        [tune addObject:lbl];
        [tune addObject:sl];
    }
    NSStackView* row3 = [NSStackView stackViewWithViews:tune];
    row3.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row3.spacing = 5;
    row3.alignment = NSLayoutAttributeCenterY;

    NSStackView* col = [NSStackView stackViewWithViews:@[ row1, row2, row3 ]];
    col.orientation = NSUserInterfaceLayoutOrientationVertical;
    col.alignment = NSLayoutAttributeLeading;
    col.spacing = 5;
    return col;
}

- (NSView*)buildButtonRow {
    NSButton* reset = [NSButton buttonWithTitle:Tr(StringId::MetersResetButton) target:self action:@selector(reset:)];
    NSButton* cancel = [NSButton buttonWithTitle:Tr(StringId::ButtonCancel) target:self action:@selector(cancel:)];
    cancel.keyEquivalent = @"\033";  // Esc
    NSButton* ok = [NSButton buttonWithTitle:Tr(StringId::ButtonOk) target:self action:@selector(ok:)];
    ok.keyEquivalent = @"\r";
    NSView* spacer = [[NSView alloc] init];
    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow
                       forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView* row = [NSStackView stackViewWithViews:@[ reset, spacer, cancel, ok ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 8;
    return row;
}

// Read the current control state for one kind into a MeterConfig.
- (MeterConfig)configForKind:(int)i {
    MeterConfig c;
    c.enabled = (_enable[i].state == NSControlStateValueOn);
    c.style   = (MeterStyle)[_style[i] indexOfSelectedItem];
    MeterPalette p;
    p.bg     = skinFromNS(_well[i][0].color);
    p.off    = skinFromNS(_well[i][1].color);
    p.low    = skinFromNS(_well[i][2].color);
    p.mid    = skinFromNS(_well[i][3].color);
    p.high   = skinFromNS(_well[i][4].color);
    p.accent = skinFromNS(_well[i][5].color);
    p.peak   = skinFromNS(_well[i][6].color);
    c.palette = p;
    MeterTuning t;
    t.glow        = (float)_tune[i][0].doubleValue;
    t.smoothing   = (float)_tune[i][1].doubleValue;
    t.sensitivity = (float)_tune[i][2].doubleValue;
    t.peakHold    = (float)_tune[i][3].doubleValue;
    t.breathing   = (float)_tune[i][4].doubleValue;
    c.tuning = t;
    return c;
}

// Any control changed — push the live config into that kind's preview. Sliders carry
// kind*10+knob in their tag; every other control carries the kind directly. Decode by
// class so a kind-0 slider (tag 0..4) isn't mistaken for another kind.
- (void)controlChanged:(NSControl*)sender {
    const int kind = [sender isKindOfClass:[NSSlider class]] ? (int)(sender.tag / 10) : (int)sender.tag;
    if (kind < 0 || kind >= 4) return;
    [_preview[kind] setConfig:[self configForKind:kind]];
    if (kKinds[kind] == MeterKind::Spectrum) [_preview[kind] setAvailable:YES];
}

// Feed each preview synthetic, animated data so the look is visible at rest.
- (void)pumpPreviews {
    _phase += 0.09;
    for (int i = 0; i < 4; ++i) {
        MeterView* v = _preview[i];
        if (!v) continue;
        switch (kKinds[i]) {
            case MeterKind::Spectrum: {
                float bands[24];
                for (int b = 0; b < 24; ++b) {
                    const float e = 0.45f + 0.4f * std::sin((float)_phase + b * 0.35f) *
                                                     std::cos((float)_phase * 0.5f + b * 0.12f);
                    bands[b] = std::clamp(e, 0.0f, 1.0f);
                }
                [v pushSpectrum:bands count:24];
                break;
            }
            case MeterKind::Signal:
                [v setSignal:std::clamp(0.55f + 0.35f * (float)std::sin(_phase * 0.7), 0.0f, 1.0f)
                     trouble:0.15f];
                break;
            case MeterKind::Bitrate:
                [v pushBitrate:(2.0e6 + 1.2e6 * std::sin(_phase))];
                break;
            case MeterKind::Frames:
                [v setFrames:(int)std::lround(45.0 + 14.0 * std::sin(_phase)) dropsDelta:0];
                break;
        }
    }
}

- (void)ok:(id)__unused sender {
    for (int i = 0; i < 4; ++i) {
        _cfg[i] = [self configForKind:i];
        if (_db) {
            _db->setSetting(wkey(i, ""),        _cfg[i].enabled ? L"1" : L"0");
            _db->setSetting(wkey(i, "_style"),  widen(meterStyleToString(_cfg[i].style)));
            _db->setSetting(wkey(i, "_colors"), widen(meterPaletteToString(_cfg[i].palette)));
            _db->setSetting(wkey(i, "_tuning"), widen(meterTuningToString(_cfg[i].tuning)));
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
        _cfg[i].tuning  = defaultMeterTuning();
        [_style[i] selectItemAtIndex:(NSInteger)_cfg[i].style];
        const MeterPalette& p = _cfg[i].palette;
        const rabbitears::SkinColor roles[7] = {p.bg, p.off, p.low, p.mid, p.high, p.accent, p.peak};
        for (int j = 0; j < 7; ++j)
            _well[i][j].color = (j == 0 && roles[0].inherit) ? [NSColor colorWithWhite:0.08 alpha:1.0] : nscolor(roles[j]);
        const float knobs[5] = {_cfg[i].tuning.glow, _cfg[i].tuning.smoothing, _cfg[i].tuning.sensitivity,
                                _cfg[i].tuning.peakHold, _cfg[i].tuning.breathing};
        for (int j = 0; j < 5; ++j) _tune[i][j].doubleValue = knobs[j];
        [_preview[i] setConfig:[self configForKind:i]];
        if (kKinds[i] == MeterKind::Spectrum) [_preview[i] setAvailable:YES];
    }
}

- (void)dismiss {
    [_previewTimer invalidate];
    _previewTimer = nil;
    [_panel.sheetParent endSheet:_panel];  // fires the completion handler, releasing self
}

@end
