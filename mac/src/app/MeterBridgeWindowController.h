// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeterBridgeWindowController — the meter bridge (Win32/ui/MeterBridge, PHOTOREAL.md stage A): a
// separate, resizable window with the enabled meters in ONE row, as large as the window lets them be
// (needle looks at their instrument's own width, cell looks at their kind's width scaled with the
// height, side by side like a rack), optionally labelled. Its meters are MIRRORS of the tray's
// (MeterView -setMirror:): data, resets and looks come from the tray, so nothing that feeds meters
// changes. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

#include "MeterModel.h"

@class MeterView;

@interface MeterBridgeWindowController : NSObject
- (instancetype)init;
- (MeterView*)meterForKind:(int)k;  // 0..3, MeterKind order
// The looks (and which meters show) — the tray's configs; `labels` prints each meter's short name.
- (void)applyConfigs:(const rabbitears::mac::MeterConfig*)cfgs labels:(BOOL)labels;
- (void)showNear:(NSWindow*)parent;
- (void)close;               // orders the window out (onClose is NOT called)
- (BOOL)isVisible;
@property (nonatomic, copy) void (^onClose)(void);  // the user closed the window
- (void)relabelForLanguageChange;
@end
