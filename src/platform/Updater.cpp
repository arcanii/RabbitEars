// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Updater.h"

#include <winsparkle.h>

#include "version.h"

namespace rabbitears {

void initUpdater() {
    // Windows appcast hosted on GitHub (same shape as the sibling apps). Update
    // this URL to the real RabbitEars repo when publishing releases.
    win_sparkle_set_appcast_url(
        "https://raw.githubusercontent.com/arcanii/RabbitEars/main/appcast.xml");
    // Report marketing.build (e.g. 0.1.0.42) so each committed build compares
    // correctly against the appcast's sparkle:version.
    win_sparkle_set_app_details(L"RabbitEars", L"RabbitEars", RE_VERSION_FULL_W);
    // EdDSA public key — REPLACE with your own from WinSparkle's generate_keys.exe
    // before shipping signed updates. The all-zero placeholder makes every update
    // fail signature verification (safe: nothing installs until this is set), and
    // sign each release enclosure with the matching private key via sign_update.
    win_sparkle_set_eddsa_public_key("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    win_sparkle_init();
}

void checkForUpdates() { win_sparkle_check_update_with_ui(); }

void shutdownUpdater() { win_sparkle_cleanup(); }

}  // namespace rabbitears
