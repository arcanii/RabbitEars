// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS implementation of platform/Updater.h (the WinSparkle peer is
// Updater.cpp). This is the macOS side of the SAME update mechanism: WinSparkle
// on Windows, Sparkle on macOS, one shared appcast/EdDSA scheme and one family
// Ed25519 key.
//
// Sparkle.framework is auto-provisioned by mac/cmake/Mac.cmake (downloaded from
// the GitHub release, embedded in the bundle) and RABBITEARS_HAVE_SPARKLE is
// then defined. The SUFeedURL (packaging/appcast-mac.xml) + SUPublicEDKey (the
// family Ed25519 key, same as Win32/platform/Updater.cpp) live in Info.plist.in.
// When Sparkle isn't provisioned the three entry points are safe no-ops.
//
// NOTE: the live update flow (check -> download -> install) needs a signed +
// notarized release and a real signed enclosure in the appcast; that is release
// infra, not code. Sparkle initializes and checks here regardless.
#import <Foundation/Foundation.h>

#include "platform/Updater.h"

#if defined(RABBITEARS_HAVE_SPARKLE)
#import <Sparkle/Sparkle.h>
#endif

namespace rabbitears {

#if defined(RABBITEARS_HAVE_SPARKLE)

static SPUStandardUpdaterController* g_updater = nil;

void initUpdater() {
    g_updater = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                             updaterDelegate:nil
                                                          userDriverDelegate:nil];
}

void checkForUpdates() { [g_updater checkForUpdates:nil]; }

void shutdownUpdater() { g_updater = nil; }

#else  // stub build (no Sparkle framework yet)

void initUpdater() { NSLog(@"[RabbitEars] auto-update disabled (Sparkle not provisioned)"); }
void checkForUpdates() { NSLog(@"[RabbitEars] Check for Updates: Sparkle not provisioned"); }
void shutdownUpdater() {}

#endif

}  // namespace rabbitears
