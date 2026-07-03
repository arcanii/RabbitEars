// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeterView — a small native LED-style audio level meter (the macOS start of the
// Win32 MiniMeter family). Feed it a 0..1 level with -pushLevel:; it smooths the
// bar (fast attack / slow release) and holds a decaying peak marker, redrawing on
// an internal ~30fps timer. Purely a view — the audio level comes from
// VlcPlayerMac's audio tap.
#pragma once

#import <Cocoa/Cocoa.h>

@interface MeterView : NSView
- (void)pushLevel:(float)level;   // 0..1, thread-safe (may be called off the main thread)
- (void)reset;                    // drop to silence (e.g. on stop)
@end
