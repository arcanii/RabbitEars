// SPDX-License-Identifier: GPL-3.0-or-later
//
// StatMeterView — a compact native LED-style meter driven by libVLC *stream stats*
// (throughput / buffered-ahead / frames), NOT by audio capture. So there's no
// audio-capture consent prompt and no A/V desync — the reasons the Core Audio tap
// meter was pulled. The mac starting point of the Win32 MiniMeter / BufferMeter
// family; fed from VlcPlayerMac's FlowStats via MainWindowController's stats timer.
//
// It smooths a 0..1 "level" (fast attack / slow release, with a decaying peak cap)
// on an internal ~30fps timer, and shows a small right-aligned text readout.
#pragma once

#import <Cocoa/Cocoa.h>

#include "MeterModel.h"  // rabbitears::mac::MeterPalette — the meter's colour roles

@interface StatMeterView : NSView
- (void)setLevel:(float)level;       // 0..1 (e.g. normalized throughput)
- (void)setReadout:(NSString*)text;  // small mono readout (e.g. "12.4 Mb/s · 25 fps")
- (void)reset;                       // drop to idle (e.g. on stop)
// Recolour the LEDs from the shared meter model. Defaults to the classic palette
// (defaultMeterPalette) at init; call this to follow a skin. `bg.inherit` resolves
// to the dark panel background (the mac stand-in for the theme window background).
- (void)setPalette:(const rabbitears::mac::MeterPalette&)palette;
@end
