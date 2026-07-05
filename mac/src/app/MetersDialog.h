// SPDX-License-Identifier: GPL-3.0-or-later
//
// MetersDialog — the macOS "Meters" config panel (peer of Win32 Settings → Meters).
// Per meter kind (Spectrum/Signal/Bitrate/Frames): a Show toggle and a Style
// (LED/LCD/Tube/Scope). Loads from + persists to the settings DB under the
// Win32-compatible keys (meter_<kind> / meter_<kind>_style). Colour wells, tuning
// sliders, and a live preview are follow-up increments; the meters themselves grow
// to honour this config (M1). Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

namespace rabbitears { class Database; }

@interface MetersDialog : NSObject
- (instancetype)initWithDatabase:(rabbitears::Database*)db;
// Present as a modal sheet on `parent`. `onApply` runs after the user clicks OK and
// the config has been persisted, so the controller can refresh the live meters.
- (void)presentForWindow:(NSWindow*)parent onApply:(void (^)(void))onApply;
@end
