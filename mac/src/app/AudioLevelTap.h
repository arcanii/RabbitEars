// SPDX-License-Identifier: GPL-3.0-or-later
//
// AudioLevelTap — a NON-INVASIVE audio level source for MeterView. It installs a
// Core Audio *process tap* (macOS 14.2+) on THIS process's own audio output and
// reports a 0..1 peak level WITHOUT touching the playback path: libVLC keeps its
// own audio output untouched, so there is no A/V desync (the reason the earlier
// libvlc_audio_set_callbacks tap was reverted). On macOS < 14.2 the initializer
// returns nil and the meter simply stays dark; playback is unaffected either way.
#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface AudioLevelTap : NSObject

// Start observing this process's audio output. `handler` is called with a 0..1
// peak on a real-time audio thread — keep it trivial (MeterView's -pushLevel: is a
// lock-free atomic store and is safe to call here). Returns nil on macOS < 14.2 or
// if the tap can't be created.
- (nullable instancetype)initWithLevelHandler:(void (^)(float level))handler;

- (void)stop;   // idempotent; also invoked from -dealloc

@end

NS_ASSUME_NONNULL_END
