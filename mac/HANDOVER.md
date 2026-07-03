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

## Current state (all merged to `main`)

The mac app **plays IPTV** via libVLC in a native window, with:
- a **rich channel grid** — columns (★ / # / name / group), live **search**, filter popup
  (All / ★ Favourites / groups / **countries**), **favourite toggle** + **LCN edit** (row
  context menu), **resume‑last‑played** on launch;
- **Sparkle auto‑update** wired — framework auto‑provisioned from the GitHub release by
  `mac/cmake/Mac.cmake` and embedded in the bundle; `SUFeedURL`/`SUPublicEDKey` in the
  Info.plist; menu‑bar *Check for Updates…*;
- a proper **menu bar** (App / Edit / …) so Cmd‑C/V/X/A/Z work in text fields;
- both platforms built in **CI** (`.github/workflows/{mac,windows}-core.yml`; the mac job
  builds the `.app`).

The mac `.mm` are compiled with **ARC** (`-fobjc-arc`); the shared core is portable C++
whose headers carry `#if defined(_WIN32)` branches (so Windows keeps its exact types/impls).

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

## Audio meters — tried on‑device, reverted, deferred (branch `mac-meters`, PR #9 — do NOT merge)

A native LED **audio meter** (`MeterView`) under the video, driven by a **libVLC audio tap** in
`VlcPlayerMac` (`libvlc_audio_set_callbacks` → peak metering + AudioQueue re‑output). It was
integrated onto `mac-ui` and **tested on‑device (2026‑07‑03): audio came out jerky and out of
A/V sync (audio led the video), so it was reverted.**

Root cause: taking over libVLC's audio output means pacing each PCM chunk to its `pts` **and**
reporting AudioQueue's output latency back into libVLC's master clock — but **libVLC 3.x (what
VLC.app ships) has no API to report that latency** (libVLC 4.x's timed audio callback fixes
exactly this). A fixed `audio-desync` offset is per‑stream and drifts, so there's no clean knob.

**Status:** the first release ships **without** meters (clean, in‑sync audio). The `MeterView`
LED code is good and preserved on `mac-meters`; only its level *source* (the tap) is the
problem. Redo post‑release by moving playback to **libVLC 4.x** (timed audio callbacks) or by
driving the meter from a **non‑invasive OS tap** (Core Audio process tap on macOS 14.4+, or
ScreenCaptureKit on 13+). **Don't re‑attempt the tap on libVLC 3.x.**

## Toward the first Mac release

1. ~~Land the meters~~ — tried on‑device and reverted (see above); the first release ships without them.
2. Cut a **signed + notarized** build (Developer ID) — required for Sparkle's live update
   flow. Sign the enclosure with the family Ed25519 key (Sparkle `sign_update`) and populate
   `mac/packaging/appcast-mac.xml`, then publish.

## Key files

```
mac/CMakeLists.txt                     # mac targets (app / self-test / play-probe); ARC; rpath; Sparkle embed
mac/cmake/Mac.cmake                    # libVLC + Sparkle provisioning
mac/src/app/main.mm                    # NSApplication entry
mac/src/app/AppDelegate.mm             # lifecycle + menu bar + Check-for-Updates
mac/src/app/MainWindowController.mm    # the UI: grid, search/filter, playback, meter wiring
mac/src/app/VlcPlayerMac.{h,mm}        # libVLC wrapper + audio tap (mac-meters)
mac/src/app/MeterView.{h,mm}           # LED level meter (mac-meters)
mac/platform/{Http,Log,Updater}.mm  mac/platform/Paths.cpp   # macOS platform layer
mac/src/tools/{selftest.cpp,playprobe.mm}
mac/packaging/{Info.plist.in, appcast-mac.xml}
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
Read mac/HANDOVER.md. RabbitEars is a cross-platform native IPTV player (Windows + macOS) in
one repo (common/ + Win32/ + mac/, unified root CMake); main is on 0.1.7. The macOS app plays
IPTV with a rich channel grid + Sparkle, built in CI. GOAL: first Mac release.
Next: (1) validate the audio meters on-device — `git checkout mac-meters && scripts/build-mac.sh
--app && open build-mac/mac/RabbitEars.app`, play a channel, confirm clean audio + meter tracks
+ smooth channel-switch; if good merge PR #9, if audio regresses revert the tap (keep MeterView,
consider ScreenCaptureKit). (2) Cut a signed+notarized build for Sparkle's update flow and
populate mac/packaging/appcast-mac.xml. Build mac: scripts/build-mac.sh [--app] (needs VLC.app);
headless libVLC check: build-mac/mac/RabbitEarsPlayProbe. GUI/audio need on-device testing.
Branch off main; keep common/ edits Windows-safe (windows-core CI validates).
```
