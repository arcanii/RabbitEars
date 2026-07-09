// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Updater.h"

#ifdef RABBITEARS_HAVE_WINSPARKLE

#include <thread>

#include <windows.h>
#include <commctrl.h>  // TaskDialogIndirect — the ARM64 "which build?" chooser (comctl32 v6)

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

// Per-architecture appcast feeds. WinSparkle can't pick an enclosure by arch, so each
// architecture reads its own appcast + downloads its own installer. See docs/RELEASING.md.
constexpr char kAppcastX64[] =
    "https://raw.githubusercontent.com/arcanii/RabbitEars/main/appcast.xml";
constexpr char kAppcastArm64[] =
    "https://raw.githubusercontent.com/arcanii/RabbitEars/main/appcast-arm64.xml";
// The feed this build ships with, matching its own architecture (the ARM64 chooser can
// override it at check time). x64 users' feed is thus unchanged; a native ARM64 build
// defaults to the ARM64 feed so it never self-updates to the emulated x64 build.
#ifdef _M_ARM64
constexpr const char* kDefaultAppcast = kAppcastArm64;
#else
constexpr const char* kDefaultAppcast = kAppcastX64;
#endif

// Once a manual check resolves, revert WinSparkle's single global feed to this build's native
// default, so a later check (in particular an automatic background one) never inherits the arch
// the user picked in the ARM64 chooser. Registered on the did-find / did-not-find / cancelled
// callbacks — it runs the moment the check resolves, and it's safe there because an in-progress
// update downloads from the appcast's enclosure URL, not from this setting.
void resetAppcastToDefault() { win_sparkle_set_appcast_url(kDefaultAppcast); }

// True on Windows-on-ARM — the machine runs BOTH the native ARM64 build and the x64 build
// (under emulation), so the user can meaningfully choose which one to update to. On x64
// hardware ARM64 can't run, so there is no choice. Resolved via IsWow64Process2 (Win10
// 1709+), falling back to "not ARM64" on older Windows.
bool machineIsArm64() {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    if (auto fn = reinterpret_cast<IsWow64Process2Fn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"))) {
        USHORT procMach = 0, nativeMach = 0;
        if (fn(GetCurrentProcess(), &procMach, &nativeMach))
            return nativeMach == IMAGE_FILE_MACHINE_ARM64;
    }
    return false;
}

// Ask which architecture's build to update to. Returns 1 = native ARM64, 0 = x64 (emulated),
// -1 = cancel. A TaskDialog with command-link buttons (comctl32 v6 — already active via the
// app manifest). Native is the default: it is ~4x faster and lower-power on this hardware.
int chooseUpdateArch(HWND owner) {
    static const int kArm64Btn = 101, kX64Btn = 102;
    const TASKDIALOG_BUTTON buttons[] = {
        {kArm64Btn, L"Native ARM64  (recommended)\n"
                    L"Runs directly on this device's processor — faster and lower-power."},
        {kX64Btn, L"x64  (emulated)\n"
                  L"Runs under Windows' built-in x64 emulation — for maximum compatibility."},
    };
    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = owner;
    cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION |
                  TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.pszWindowTitle = L"RabbitEars";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = L"Which build do you want to update to?";
    cfg.pszContent = L"This PC has an ARM processor and can run either build. The native ARM64 "
                     L"build is faster and uses less power; the x64 build runs under emulation.";
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.pButtons = buttons;
    cfg.nDefaultButton = kArm64Btn;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)))
        return 1;  // couldn't show the chooser — fall back to the recommended native build
    if (pressed == kArm64Btn) return 1;
    if (pressed == kX64Btn) return 0;
    return -1;  // Cancel / closed
}
}  // namespace

void initUpdater(HWND mainWnd) {
    g_mainWnd = mainWnd;
    // Start on this build's own architecture feed (see kDefaultAppcast); on ARM64 the
    // "Check for Updates" chooser can re-point it to the other arch before each check.
    win_sparkle_set_appcast_url(kDefaultAppcast);
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
    // Revert the feed to this build's native default after any check resolves, so the ARM64
    // chooser's per-check choice never leaks into a later (e.g. automatic) check. No-op on x64,
    // where kDefaultAppcast is the only feed. See checkForUpdates().
    win_sparkle_set_did_find_update_callback(resetAppcastToDefault);
    win_sparkle_set_did_not_find_update_callback(resetAppcastToDefault);
    win_sparkle_set_update_cancelled_callback(resetAppcastToDefault);
    win_sparkle_init();
}

void checkForUpdates() {
    // On Windows-on-ARM the user can pick which build to update to (native ARM64 or emulated
    // x64); point WinSparkle at that architecture's feed first. On x64 hardware there is only
    // one runnable architecture, so check directly. NB: WinSparkle compares by VERSION, so a
    // chosen channel only offers a download when it is NEWER than the running build — this
    // selects the update channel, it does not force a same-version architecture swap.
    if (machineIsArm64()) {
        HWND owner = GetActiveWindow();  // the About box (the only caller); falls back to main
        if (!owner) owner = g_mainWnd;
        const int pick = chooseUpdateArch(owner);
        if (pick < 0) return;  // cancelled — don't check
        win_sparkle_set_appcast_url(pick == 1 ? kAppcastArm64 : kAppcastX64);
    }
    win_sparkle_check_update_with_ui();
}

void shutdownUpdater() { win_sparkle_cleanup(); }

}  // namespace rabbitears

#else  // ---- no WinSparkle (RABBITEARS_UPDATER=OFF) --------------------------------------------
// Auto-update is compiled out — the functions become no-ops so the rest of the app builds and runs
// unchanged (it just never checks for / installs updates). RABBITEARS_HAVE_WINSPARKLE is defined by
// the build only when RABBITEARS_UPDATER is ON — now the default for BOTH x64 and ARM64, since the
// ARM64 WinSparkle slice is vendored. See Win32/CMakeLists.txt.

namespace rabbitears {
void initUpdater(HWND) {}
void checkForUpdates() {}
void shutdownUpdater() {}
}  // namespace rabbitears

#endif
