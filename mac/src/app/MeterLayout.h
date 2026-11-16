// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeterLayout — the mac meters' sizes, shared by the floating tray (MainWindowController
// -applyMeterLayout) and the meter bridge: the mac reading of Win32/ui/MeterTray.h (stage A).
//
// ONE meter height, the setting `meter_height` in dp (Win32's key and values): 30 = the standard size
// (mac's 24-pt tray, exactly as it always was), Large 50, Extra large 72, any height between by dragging
// the tray's top edge, up to 120. mac draws a height h dp as kMeterBasePt * h / 30 points. At the
// standard height every meter keeps its kind's width and the 8-pt gap; taller, the widths scale with the
// height, needle looks take their instrument's own width (two of one face match, whatever their kind),
// and the meters stand side by side — the Win32 own row's rules.
#pragma once

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>

#include "MeterModel.h"
#include "VuDialMac.h"
#import "Tr.h"

namespace rabbitears::mac {

constexpr int kMeterHeightStd = 30, kMeterHeightLarge = 50, kMeterHeightXLarge = 72;
constexpr int kMeterHeightMin = kMeterHeightStd, kMeterHeightMax = 120;
constexpr CGFloat kMeterBasePt = 24;   // the standard tray's meter height (pt)
constexpr CGFloat kMeterGapPt = 8;     // the standard tray's gap between meters
constexpr CGFloat kMeterLabelRowPt = 14;  // the labels' row under a taller row (Win32 kMeterLabelDp)
constexpr CGFloat kMeterBaseW[4] = {180, 64, 130, 96};  // the kinds' widths at the standard height
constexpr CGFloat kTankBaseW = 92;  // the data-flow tank at the standard height (Win32's 115 dp at 30 dp)
// The tank's width at height `h` pt: its standard width, scaled with the height in a taller row.
inline CGFloat tankWidthPt(CGFloat h, bool ownRow) {
    return ownRow ? std::round(kTankBaseW * h / kMeterBasePt) : kTankBaseW;
}

inline int clampMeterHeightDp(int dp) { return std::clamp(dp, kMeterHeightMin, kMeterHeightMax); }
inline CGFloat meterHeightPt(int dp) { return kMeterBasePt * clampMeterHeightDp(dp) / kMeterHeightStd; }
inline int meterHeightDpForPt(CGFloat pt) {
    return clampMeterHeightDp(static_cast<int>(std::lround(pt * kMeterHeightStd / kMeterBasePt)));
}

// A meter's width at height `h` pt. `ownRow`: taller than the standard (or the bridge).
inline CGFloat meterWidthPt(int kind, MeterStyle style, CGFloat h, bool ownRow) {
    const int k = std::clamp(kind, 0, 3);
    if (!ownRow) return kMeterBaseW[k];
    if (isVuLook(style)) {
        const CGFloat chrome = 2;  // meterChromePx, in points
        const int dial = static_cast<int>(std::lround(h - 2 * chrome));
        if (dial >= 8)
            return vuDialNaturalWidth(style == MeterStyle::VuSilver ? VuFace::Silver : VuFace::Backlit, dial) + 2 * chrome;
    }
    return std::round(kMeterBaseW[k] * h / kMeterBasePt);
}

// A meter's short name as its label shows it: capitals, tracked out, the muted text with a hairline
// shadow below-right (the key light is above-left) — Win32 MeterLabels.
// `kind` 0..3 = MeterKind order, 4 = the data-flow tank (Win32 MeterLabels' slots).
inline NSAttributedString* meterLabelText(int kind) {
    static const i18n::StringId kNames[5] = {i18n::StringId::MeterLabelSpectrum, i18n::StringId::MeterLabelSignal,
                                              i18n::StringId::MeterNameBitrate, i18n::StringId::MeterLabelFrames,
                                              i18n::StringId::MeterDataFlowCheckbox};
    NSString* text = [Tr(kNames[std::clamp(kind, 0, 4)]) uppercaseStringWithLocale:nil];
    NSShadow* sh = [[NSShadow alloc] init];
    sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.9];
    sh.shadowOffset = NSMakeSize(1, -1);
    sh.shadowBlurRadius = 0;
    NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
    ps.alignment = NSTextAlignmentCenter;
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSAttributedString* a = [[NSAttributedString alloc]
        initWithString:text
            attributes:@{NSFontAttributeName: [NSFont systemFontOfSize:9 weight:NSFontWeightSemibold],
                         NSForegroundColorAttributeName: [NSColor colorWithWhite:0.62 alpha:1.0],
                         NSKernAttributeName: @1.0,
                         NSShadowAttributeName: sh,
                         NSParagraphStyleAttributeName: ps}];
#if !__has_feature(objc_arc)
    [sh release];
    [ps release];
    [a autorelease];
#endif
    return a;
}

}  // namespace rabbitears::mac
