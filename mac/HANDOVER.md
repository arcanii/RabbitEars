# RabbitEars — macOS Handover

The macOS team's living handover. (The root [`HANDOVER.md`](../HANDOVER.md) is the
Windows team's; the port rationale + history is [`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).)
Read this before touching the mac app.

## What RabbitEars is

A cross-platform native IPTV player in **one repo**: **`common/`** (portable core —
`M3uParser`, `Database`, `DockLayout`, models, platform seam *headers*), **`Win32/`** (the
Windows app), **`mac/`** (this — the Cocoa app), under a unified root `CMakeLists.txt`
(`common` → `Win32`/`mac` per‑OS). Playback is **libVLC**; storage **SQLite**. `main` is on
**0.2.0** (the theme-engine release; the Windows team ships from `main` — don't destabilize their build).

## Current state — v0.1.8-mac SHIPPED; `main` now on 0.2.0 (theme engine)

The mac app is **shipped and auto-updating**: **`v0.1.8-mac`** on GitHub — universal (Intel + Apple
Silicon), notarized, self-contained. The Sparkle auto-update path is **proven end-to-end** (0.1.7→
0.1.8 succeeded on-device; the one snag was an XML `--` in an appcast comment — **always `xmllint`
the appcast before publishing**). `main` has since advanced to **0.2.0**, the Windows-led
**theme-engine** release (`docs/SKIN_MODEL.md` = the shared, graphics-free skin model; the meter
*model* still lives under `Win32/ui/`). **App minimum is macOS 26** — a deliberate "latest is best"
policy (`LSMinimumSystemVersion`; the compile-time deployment target stays unpinned so CI's older
SDK still builds). All mac work stays **Windows-safe** (`mac/` + docs/scripts only).

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

The mac `.mm` are ObjC++ written **MRC-style** (ARC off target-wide; app-lifetime objects leak
benignly). `-fobjc-arc` is enabled **per-file** only where needed (e.g. `StatMeterView.mm`'s
weak-self timer). The shared core is portable C++ whose headers carry `#if defined(_WIN32)` branches.

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

## Meters — Core Audio tap tried & pulled; redone off libVLC stats (IN PROGRESS)

**History.** v0.1.8 shipped a `MeterView` LED strip fed by a **non‑invasive Core Audio process tap**
(`AudioLevelTap`, macOS 14.2+ — the mac peer of Win32's WASAPI `SpectrumTap`). Audio stayed in sync
(the tap only *observes*; it never takes over libVLC's output). But it hit **denied consent**: on a
denied audio‑capture prompt the tap still *succeeds* and just delivers silence → a dark, undetectable
strip. So the whole Core Audio meter was **removed** (`main` @ `c355feb`). Recover it from git history
before that commit if ever needed. (The older libVLC‑3.x `libvlc_audio_set_callbacks` tap — which
took over output and desynced — was a separate, earlier dead end; do NOT revisit it.)

**Redo — align with the Win32 meter set.** Win32 has `Win32/audio/SpectrumTap` (loopback + FFT),
`Win32/ui/MiniMeter` (Spectrum/Signal/Bitrate/Frames dot‑matrix), `Win32/ui/BufferMeter` (fluid sim).
**KEY: only Spectrum needs audio capture; Signal/Bitrate/Frames/Buffer run off
`libvlc_media_get_stats` — no consent, no desync.** So lead with those. Progress (branch
`mac-stats-meter` / **PR #22**):
- **A ✅** `VlcPlayerMac::sampleStats()` → `FlowStats` (peer of Win32 `VlcPlayer::sampleStats`: byte
  rates over wall‑clock, buffered‑ahead, fps, corruption/lost deltas).
- **B ✅** `StatMeterView` — a Win32‑style LED **dot‑matrix** meter (small square cells, 30fps easing,
  peak‑hold, mono readout).
- **C ✅** wired: a 250ms poll below the video → bar = throughput vs a rolling peak, readout
  `X Mb/s · Y fps · N drop`.
- **D ⏳** Spectrum meter (reuse a Core Audio tap + FFT) — **opt‑in** (the only consent‑needing one).
- **E ⏳** promote `FlowStats` + the meter model (`MeterKind/Style/Palette`, still Win32‑only) into
  `common/` for the shared skin (the theme‑engine boundary).

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
mac/src/app/VlcPlayerMac.{h,mm}        # libVLC wrapper (bundled PlugIns) + sampleStats()->FlowStats for the meters
mac/platform/{Http,Log,Updater}.mm  mac/platform/Paths.cpp   # macOS platform layer
mac/src/tools/{selftest.cpp,playprobe.mm}
mac/packaging/{Info.plist.in, appcast-mac.xml, RabbitEars.icns, RabbitEars.entitlements}
scripts/{build-mac.sh, package-mac.sh, make-icns.py, xcode.sh}  # build / bundle+sign+notarize / icon / Xcode-gen
mac/src/app/StatMeterView.{h,mm}                              # libVLC-stats LED dot-matrix meter (no audio capture); PR #22
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
(Windows + macOS) in ONE repo (common/ + Win32/ + mac/, unified root CMake; playback libVLC, storage
SQLite). main is on 0.2.0 (the Windows-led theme-engine release). The mac app is shipped and
auto-updating: v0.1.8-mac on GitHub (universal, notarized, self-contained); the Sparkle update path
is proven end-to-end. App minimum is macOS 26 ("latest is best" — LSMinimumSystemVersion only, the
deployment target stays unpinned so CI's older SDK builds). Build: scripts/build-mac.sh [--app]
(needs VLC.app). Release recipe: scripts/package-mac.sh + the mac-release-deployment memory (Developer
ID 386M76FV3K, notary profile SQLTerminal-notarize, sign_update --account SQLTerminal; universal needs
vlc-3.0.23-universal.dmg; ALWAYS xmllint mac/packaging/appcast-mac.xml before publishing — an XML '--'
in a comment broke the feed once). GUI/audio can't be verified headlessly — real Mac testing required;
branch off main, PR back, keep common/ edits Windows-safe (windows-core CI validates).

IN PROGRESS: the mac METER redo (branch mac-stats-meter / PR #22), aligned with the Win32 meter set
(Win32/audio/SpectrumTap + Win32/ui/{MiniMeter,BufferMeter}). The earlier Core Audio process-tap meter
shipped in 0.1.8 then was REMOVED (main @ c355feb) because denied audio-capture consent gave a dark,
undetectable strip. KEY: only the Spectrum meter needs audio capture; Signal/Bitrate/Frames/Buffer run
off libvlc_media_get_stats (no consent, no desync). DONE: (A) VlcPlayerMac::sampleStats()->FlowStats,
(B) StatMeterView (Win32-style LED dot-matrix), (C) wired 250ms poll below the video. NEXT: (D) Spectrum
meter (reuse a Core Audio tap + FFT, opt-in), (E) promote FlowStats + the meter model (MeterKind/Style/
Palette, still Win32-only) to common/ for the shared skin. Other open item: the universal build's x86_64
slice is untested on real Intel hardware (backlogged).
```
