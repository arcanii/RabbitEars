# RabbitEars — macOS Handover

The macOS team's living handover. (The root [`HANDOVER.md`](../HANDOVER.md) is the
Windows team's; the port rationale + history is [`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).)
Read this before touching the mac app.

## What RabbitEars is

A cross-platform native IPTV player in **one repo**: **`common/`** (portable core —
`M3uParser`, `Database`, `DockLayout`, models, platform seam *headers*), **`Win32/`** (the
Windows app), **`mac/`** (this — the Cocoa app), under a unified root `CMakeLists.txt`
(`common` → `Win32`/`mac` per‑OS). Playback is **libVLC**; storage **SQLite**. `main` is on
**0.1.7** (the Windows team ships from `main` — don't destabilize their build).

## Current state — first release SHIPPED + MERGED to `main` (PR #11, 2026-07-03)

The mac app is **feature-complete for a first release** and **published**: **`v0.1.7-mac`** on
GitHub — a **universal (Intel + Apple Silicon), notarized, self-contained** DMG that opens with a
normal double-click. **PR #11 was rebase-merged to `main` on 2026-07-03** (`main` @ `5f57118`), landing the code **and**
`mac/packaging/appcast-mac.xml`; the `mac-ui` branch is now deleted. The Sparkle feed URL serves
from `main`, so in-app auto-updates are **live** — the feed advertises `sparkle:version=68` (= the
shipped build), so existing users aren't re-prompted; the channel is simply armed for the next
release. The mac work was **Windows-safe** (`mac/` + docs/scripts only; no `common/`/`Win32/`
changes), so the Windows team's build from `main` was unaffected.

The app **plays IPTV** via libVLC in a native window:
- **rich channel grid** — ★ / # / name / group columns, live **search**, filter popup
  (All / ★ Favourites / groups / **countries**), **favourite** toggle + **LCN edit** (row menu),
  **resume-last-played**; a single click selects, **double-click / Return plays**;
- **top bar** — accent **`+ Add Playlist`** (URL prompt → import-results dialog) + a **`Settings`**
  menu (Open File / Check for Updates / About) + search + filter — the Win32 command-bar peer;
- **split view** (grid | video) that fills correctly and **remembers window size/position**;
- **volume + mute** (bottom bar), native **fullscreen** (⌃⌘F), a **custom About** (libVLC
  attribution + educational-use disclaimer, matching Win32);
- **Sparkle auto-update** (framework embedded; `SUFeedURL`/`SUPublicEDKey` in Info.plist);
- **self-contained** — `scripts/package-mac.sh` bundles libvlc + libvlccore + ~343 plugins into the
  app, so it runs with **no VLC.app installed**;
- an **app icon** (`RabbitEars.icns`), a menu bar (Cmd-C/V/X/A/Z), CI on both platforms.

The mac `.mm` are ObjC++ written **ARC-style** (no manual retain/release); note `-fobjc-arc` is
**not** currently enabled in the mac build (it was only on the reverted, since-deleted `mac-meters` branch). The
shared core is portable C++ whose headers carry `#if defined(_WIN32)` branches.

## Build & run

```sh
scripts/build-mac.sh                 # shared core + self-test (no external deps)
scripts/build-mac.sh --app           # + RabbitEars.app  (needs VLC.app for libVLC)
open build-mac/mac/RabbitEars.app
build-mac/mac/RabbitEarsPlayProbe    # headless libVLC smoke test (exit 0 = Playing)
```
`Mac.cmake` auto‑detects VLC.app (or `-DLIBVLC_MAC_PREFIX=<dir>`) and downloads Sparkle. The
unified root build also works directly: `cmake -S . -B build-mac -DRABBITEARS_BUILD_MAC=ON`.
Unsigned dev builds trip Gatekeeper — right‑click → Open, or
`xattr -dr com.apple.quarantine build-mac/mac/RabbitEars.app`.

## Audio meters — tried on‑device, reverted, deferred (branch `mac-meters` / PR #9 now DELETED)

A native LED **audio meter** (`MeterView`) under the video, driven by a **libVLC audio tap** in
`VlcPlayerMac` (`libvlc_audio_set_callbacks` → peak metering + AudioQueue re‑output). It was
integrated onto `mac-ui` and **tested on‑device (2026‑07‑03): audio came out jerky and out of
A/V sync (audio led the video), so it was reverted.**

Root cause: taking over libVLC's audio output means pacing each PCM chunk to its `pts` **and**
reporting AudioQueue's output latency back into libVLC's master clock — but **libVLC 3.x (what
VLC.app ships) has no API to report that latency** (libVLC 4.x's timed audio callback fixes
exactly this). A fixed `audio-desync` offset is per‑stream and drifts, so there's no clean knob.

**Status:** the first release ships **without** meters (clean, in‑sync audio). The `MeterView`
LED code is good; only its level *source* (the tap) was the problem. **The `mac-meters` branch was
deleted on 2026‑07‑04 (PR #9 auto‑closed); its tip was `eb730ea`** — restore it via GitHub's
"Restore branch" on the closed PR #9 (or check out that SHA) to get the `MeterView` code back,
else re‑create it. Redo post‑release by moving playback to **libVLC 4.x** (timed audio callbacks) or by
driving the meter from a **non‑invasive OS tap** (Core Audio process tap on macOS 14.4+, or
ScreenCaptureKit on 13+). **Don't re‑attempt the tap on libVLC 3.x.**

## Releasing (v0.1.7-mac shipped this way — full recipe in the `mac-release-deployment` memory)

Deployed like the sibling **SQLTerminal** (`~/Desktop/github_repos/SQLTerminal/scripts/build.sh`),
**reusing the family credentials — no new setup**: Developer ID **`386M76FV3K`** signs, notarize
via the **`SQLTerminal-notarize`** keychain profile, and the Sparkle EdDSA key is in the login
keychain under account **`SQLTerminal`**. (Bryan's other team `28BMLNV6CR` is Apple-Development
only — it can't notarize.)

```sh
# 1. universal build — a stock VLC.app is single-arch, so point at vlc-3.0.23-universal.dmg's app
scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DLIBVLC_MAC_PREFIX="<universal VLC.app>/Contents/MacOS"
# 2. bundle libVLC + sign inside-out (app, plugins, AND Sparkle's Updater.app/Autoupdate/XPC —
#    which ship without a secure timestamp and otherwise FAIL notarization)
scripts/package-mac.sh <app> --vlc "<VLC.app>" \
    --sign "Developer ID Application: Matthew Mark (386M76FV3K)" \
    --entitlements mac/packaging/RabbitEars.entitlements
# 3. dmg → notarize → staple → Sparkle-sign
create-dmg … <dmg> <app>
xcrun notarytool submit <dmg> --keychain-profile SQLTerminal-notarize --wait
xcrun stapler staple <dmg>
build-mac*/sparkle/bin/sign_update --account SQLTerminal <dmg>   # prints edSignature + length
# 4. paste the enclosure into mac/packaging/appcast-mac.xml (sparkle:version = CFBundleVersion),
#    gh release create v<ver>-mac <dmg>, then MERGE the appcast to main (feed serves from main).
```

## Key files

```
mac/CMakeLists.txt                     # mac targets (app / self-test / play-probe); rpath; Sparkle embed; icon
mac/cmake/Mac.cmake                    # libVLC + Sparkle provisioning (-DLIBVLC_MAC_PREFIX overrides VLC.app)
mac/src/app/AppDelegate.mm             # lifecycle + menu bar (App/Edit/View) + custom About + Check-for-Updates
mac/src/app/MainWindowController.mm    # the UI: top bar, grid, search/filter, split, volume, fullscreen, playback
mac/src/app/VlcPlayerMac.{h,mm}        # libVLC wrapper (prefers bundled Contents/PlugIns); NO audio tap on mac-ui
mac/platform/{Http,Log,Updater}.mm  mac/platform/Paths.cpp   # macOS platform layer
mac/src/tools/{selftest.cpp,playprobe.mm}
mac/packaging/{Info.plist.in, appcast-mac.xml, RabbitEars.icns, RabbitEars.entitlements}
scripts/{build-mac.sh, package-mac.sh, make-icns.py, xcode.sh}  # build / bundle+sign+notarize / icon / Xcode-gen
mac/src/app/MeterView.{h,mm} + the VlcPlayerMac audio tap       # deferred; branch mac-meters DELETED — restore from closed PR #9 / eb730ea
../common/ …                           # the shared engine (edit carefully — feeds Windows too)
```

## Working rules

- **Can't test GUI/audio headlessly** — real Mac testing is required for anything visual or
  audible (that's how the "can't paste" / meter bugs surface).
- **Branch off `main`, PR back**; CI validates both platforms. Keep any shared‑file
  (`common/`) edit behavior‑preserving on Windows and let `windows-core` CI confirm.
- Run an adversarial review on new ObjC++ (ARC/threading/Cocoa) before merging — it has
  repeatedly caught real bugs here.

## Seed prompt for a fresh session

```
Read mac/HANDOVER.md and the recalled memory. RabbitEars is a cross-platform native IPTV player
(Windows + macOS) in one repo (common/ + Win32/ + mac/, unified root CMake). The macOS app is
feature-complete, SHIPPED, and MERGED: v0.1.7-mac on GitHub (universal, notarized, self-contained);
PR #11 is merged to main (main @ 5f57118), so the code + mac/packaging/appcast-mac.xml are on main and
the Sparkle feed (served from main) is LIVE. mac-ui is deleted.
Build: scripts/build-mac.sh [--app] (needs VLC.app). Release recipe: scripts/package-mac.sh + the
mac-release-deployment memory (Developer ID 386M76FV3K, notary profile SQLTerminal-notarize,
sign_update --account SQLTerminal; universal needs vlc-*-universal.dmg). GUI/audio can't be verified
headlessly — real Mac testing required. Open items: (1) audio meters are deferred — the libVLC 3.x
tap caused A/V desync, do NOT retry (use a non-invasive OS tap or libVLC 4.x); the mac-meters branch
was DELETED (restore from closed PR #9 / SHA eb730ea if you revisit it); (2) the x86_64 slice of the
universal build is untested on real Intel hardware. Branch off main;
keep common/ edits Windows-safe (windows-core CI validates).
```
