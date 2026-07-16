// SPDX-License-Identifier: GPL-3.0-or-later
//
// LogoLoader — async channel-logo image cache for the channel grid. A cell-based
// NSTableView asks -imageForURL: for the logo column of each VISIBLE row; this returns
// the cached image immediately, or nil while it loads the image off the main thread
// (memory cache -> disk cache -> network), then invokes `onReady` on the main thread so
// the controller can reload the visible logo cells. Keyed by the logo URL, so it is
// independent of row indices (which shift on filter/search/refresh) and defeats the
// cell-reuse "wrong image" problem for free.
//
// Networking obeys the app's normal App Transport Security policy (https + localhost);
// cleartext-http logos fail and are negative-cached to the placeholder — the same policy
// the rest of the app's networking already follows. Compiled with -fobjc-arc.
#pragma once

#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

@interface LogoLoader : NSObject

// `cacheDir` is the on-disk logo cache directory (created if absent; pruned to a cap on
// init). `onReady` is invoked on the MAIN thread whenever a newly loaded image becomes
// available — coalesce a visible-rows reload there.
- (instancetype)initWithCacheDir:(NSString*)cacheDir onReady:(void (^)(void))onReady;

// The cached image for `url`, or nil if not (yet) available. Main-thread only. A nil
// return with a nonempty, not-yet-failed, not-in-flight URL kicks off an async load.
- (nullable NSImage*)imageForURL:(NSString*)url;

- (instancetype)init NS_UNAVAILABLE;

@end

NS_ASSUME_NONNULL_END
