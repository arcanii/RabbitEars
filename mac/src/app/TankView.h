// SPDX-License-Identifier: GPL-3.0-or-later
//
// TankView — the data-flow tank (Win32's buffering visualizer, ui/BufferMeter): a little tank of liquid
// whose level tracks the stream's buffer health, its motion fed from real libVLC stats (TankSim does the
// physics and the LED render). A compact throughput readout ("12.4 Mb/s") sits top-right. Right-click
// offers "Hide buffer fluid". Mirrors onto the meter bridge's twin like MeterView. Main thread only.
// Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

@interface TankView : NSView
- (void)setHealth:(int)percent;                       // 0..100 (Win32 bufferMeterSetHealth); < 0 = keep
- (void)setFlow:(float)flowRate trouble:(float)trouble;  // 0..1 each (Win32 bufferMeterSetFlow)
- (void)setMetrics:(NSString*)text;                   // the readout; nil/@"" clears it
- (void)setMirror:(TankView*)mirror;                  // the bridge's twin (seeded on link); nil unlinks
@property (nonatomic, copy) void (^onHideRequested)(void);  // the right-click "Hide buffer fluid"
- (void)relabelForLanguageChange;
@end

// The app-wide tank settings (Win32: globals too — the tank has no MeterConfig): the fluid's surface
// colour (buffer_fluid_color, RRGGBB) and the opt-in scaled dots (buffer_scaled_dots).
void REDataFlowSetFluidColor(unsigned r, unsigned g, unsigned b);
void REDataFlowFluidColor(unsigned* r, unsigned* g, unsigned* b);
void REDataFlowSetScaledDots(bool on);
bool REDataFlowScaledDots();
