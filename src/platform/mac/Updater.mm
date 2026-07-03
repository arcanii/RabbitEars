// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS implementation of platform/Updater.h (the WinSparkle peer is
// Updater.cpp). This is the macOS side of the SAME update mechanism: WinSparkle
// on Windows, Sparkle on macOS, one shared appcast/EdDSA scheme and one family
// Ed25519 key.
//
// SCAFFOLD STATE: the Sparkle wiring is stubbed until Sparkle.framework is
// provisioned (see mac/cmake/Mac.cmake). When RABBITEARS_HAVE_SPARKLE is
// defined, the framework calls below activate; otherwise the three entry points
// are safe no-ops so the app builds and runs without auto-update.
//
// TODO(phase-1): provision Sparkle.framework, embed the family SUPublicEDKey
// (the same Ed25519 public key hard-coded in src/platform/win/Updater.cpp), point
// SUFeedURL at packaging/appcast-mac.xml, and replace the stubs below.
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
