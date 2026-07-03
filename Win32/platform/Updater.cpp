// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Updater.h"

#include <thread>

#include <winsparkle.h>

#include "version.h"

namespace rabbitears {

namespace {
// The main window, so WinSparkle's shutdown-request callback (which fires on a
// WinSparkle worker thread) can ask the UI to close via PostMessage.
HWND g_mainWnd = nullptr;

// WinSparkle needs us to quit before it installs an update. Post WM_CLOSE so the app
// tears down on its own (UI) thread; WinSparkle waits for the process to exit before
// running the installer. Without this the installer races a still-running RabbitEars and
// fails to overwrite the locked exe/DLLs — the reported "update fails" bug.
void onUpdaterShutdownRequest() {
    // Ask the UI to close cleanly (WM_DESTROY joins threads + releases libVLC)...
    if (g_mainWnd) PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
    // ...but the clean close can be blocked by a modal dialog's own message loop — and the ONLY
    // path to "Check for Updates" is the About box, so it's open during every user-triggered
    // update. A nested loop that swallows the WM_QUIT leaves the process alive, locking the exe so
    // WinSparkle's installer fails. Guarantee a fast exit so WinSparkle (waiting on our process
    // handle) can proceed; the clean path normally exits well under this, so this only wins if a
    // modal loop or a stuck teardown holds us.
    std::thread([] {
        Sleep(2500);
        ExitProcess(0);
    }).detach();
}
int onUpdaterCanShutdown() { return 1; }  // always OK to close for an update
}  // namespace

void initUpdater(HWND mainWnd) {
    g_mainWnd = mainWnd;
    // Windows appcast hosted on GitHub (same shape as the sibling apps).
    win_sparkle_set_appcast_url(
        "https://raw.githubusercontent.com/arcanii/RabbitEars/main/appcast.xml");
    // Report marketing.build (e.g. 0.1.0.42) so each committed build compares
    // correctly against the appcast's sparkle:version.
    win_sparkle_set_app_details(L"RabbitEars", L"RabbitEars", RE_VERSION_FULL_W);
    // EdDSA public key — shared with the sibling apps' macOS Sparkle key pair (same
    // string as their SUPublicEDKey), so release packages are signed on macOS with
    // the existing private key via Sparkle's sign_update. See docs/RELEASING.md.
    win_sparkle_set_eddsa_public_key("sKPprIa95Hw+DX3bMoxWMsyC0w9vc4MzEpgx7TBDP1I=");
    // Coordinate shutdown so an update never races a still-running instance.
    win_sparkle_set_can_shutdown_callback(onUpdaterCanShutdown);
    win_sparkle_set_shutdown_request_callback(onUpdaterShutdownRequest);
    win_sparkle_init();
}

void checkForUpdates() { win_sparkle_check_update_with_ui(); }

void shutdownUpdater() { win_sparkle_cleanup(); }

}  // namespace rabbitears
