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
    // EdDSA public key — shared with the sibling apps' macOS Sparkle key pair (same
    // string as their SUPublicEDKey), so release packages are signed on macOS with
    // the existing private key via Sparkle's sign_update. See docs/RELEASING.md.
    // (To isolate RabbitEars, generate a dedicated key pair and paste its public
    // key here instead.)
    win_sparkle_set_eddsa_public_key("sKPprIa95Hw+DX3bMoxWMsyC0w9vc4MzEpgx7TBDP1I=");
    win_sparkle_init();
}

void checkForUpdates() { win_sparkle_check_update_with_ui(); }

void shutdownUpdater() { win_sparkle_cleanup(); }

}  // namespace rabbitears
