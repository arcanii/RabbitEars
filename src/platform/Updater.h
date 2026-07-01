// SPDX-License-Identifier: GPL-3.0-or-later
// Thin wrapper over WinSparkle (the Windows analogue of Sparkle). Auto-update
// checks run against a Windows appcast; release packages must be EdDSA-signed with
// the private key matching the public key set in Updater.cpp.
#pragma once

namespace rabbitears {

void initUpdater();      // configure + start background checks
void checkForUpdates();  // user-triggered "Check for Updates…"
void shutdownUpdater();  // on app exit

}  // namespace rabbitears
