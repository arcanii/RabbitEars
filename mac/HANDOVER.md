# RabbitEars — macOS Handover

The macOS team's living handover. (The Windows team's is [`Win32/HANDOVER.md`](../Win32/HANDOVER.md);
the port rationale + history is [`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).) Read this before
touching the mac app.

## What RabbitEars is

A cross-platform native IPTV player in **one repo**: **`common/`** (portable core — `M3uParser`,
`Database`, `DockLayout`, `FlowStats`, XMLTV/EPG + recording-scheduler cores, models, platform seam
*headers*), **`Win32/`** (the Windows app), **`mac/`** (this — the Cocoa app), under a unified root
`CMakeLists.txt` (`common` → `Win32`/`mac` per‑OS). Playback is **libVLC**; storage **SQLite**.

`main` carries **both platforms at decoupled versions**: **Windows 0.2.4** (theme engine + EPG/TV Guide,
scheduled recordings, multi-view Split/PIP — the Windows team ships from `main`) and **mac 0.1.10**. The
version split lives in `cmake/AppVersion.cmake` (`APP_VERSION` = Windows; an `if(APPLE)` override = mac).
Keep all mac work **Windows-safe** and let `windows-core` / `macOS core` CI confirm.

## Current state — v0.1.10-mac SHIPPED (everything merged to main)

The mac app is **shipped and auto-updating**: **`v0.1.10-mac`** on GitHub — universal (arm64 + x86_64),
notarized, self-contained. **App minimum is macOS 26** ("latest is best"; `LSMinimumSystemVersion` only,
deployment target unpinned so CI's older SDK still builds — note macOS 26 is Apple-Silicon-only, so the
x86_64 slice is effectively dead weight but shipped for parity). The Sparkle path is **proven end-to-end**
(0.1.7→0.1.8→0.1.9→0.1.10 auto-updates confirmed in the wild; the one historical snag was an XML `--` in
an appcast comment — **always `xmllint` the appcast before publishing**).

The app **plays IPTV** via libVLC in a native window:
- **rich channel grid** — ★ / # / name / group columns, live **search**, filter popup
  (All / ★ Favourites / groups / **countries**), **favourite** toggle + **LCN edit** (row menu),
  **resume-last-played**; single click selects, **double-click / Return plays**;
- **Terms-of-Use gate** on first launch + after any version change (see below);
- **playlist management** — Settings ▸ Manage Playlists… (enable/disable/rename/refresh/delete);
- **audio/stream meters** — 4 kinds × 4 styles + a config dialog (see below);
- **top bar** — accent **`+ Add Playlist`** + a **⚙ gear** (Open File / Manage Playlists / Meters /
  Updates / About) + search + filter; plus a full **menu bar** (App / File / Edit / View);
- **split view** (grid | video) that fills correctly + **remembers window size/position**;
- **volume + mute**, native **fullscreen** (⌃⌘F) + **Video Only** (⌥⌘F), a **custom About**;
- **Sparkle auto-update**; **self-contained** (`scripts/package-mac.sh` bundles libvlc + ~343 plugins,
  so it runs with **no VLC.app installed**); an app icon; CI on both platforms.

The mac `.mm` are ObjC++ written **MRC-style** (ARC off target-wide; app-lifetime objects leak benignly).
`-fobjc-arc` is enabled **per-file** only where needed: `MeterView.mm`, `MetersDialog.mm`, `SpectrumTap.mm`,
`PlaylistsDialog.mm`, `TermsDialog.mm` (weak-self timers, blocks, an RT latch, block-captured self). The
shared core is portable C++ whose headers carry `#if defined(_WIN32)` branches.

## Build & run

```sh
scripts/build-mac.sh                 # shared core + self-test (no external deps)
scripts/build-mac.sh --app           # + RabbitEars.app  (needs VLC.app for libVLC)
open build-mac/mac/RabbitEars.app
build-mac/mac/RabbitEarsPlayProbe    # headless libVLC smoke test (exit 0 = Playing)
```
`Mac.cmake` auto‑detects VLC.app (or `-DLIBVLC_MAC_PREFIX=<dir>`) and downloads Sparkle. Unsigned dev
builds trip Gatekeeper — right‑click → Open, or `xattr -dr com.apple.quarantine build-mac/mac/RabbitEars.app`.

## Terms-of-Use gate (SHIPPED in v0.1.10)

`TermsDialog.{h,mm}` (ARC) — a modal shown once on **first launch and after any version change**, mirroring
the Win32 `showTerms` gate with the **verbatim terms text**. In `MainWindowController -showWindow`, right
after the DB opens: if `tos_accepted` ≠ the current full version (`RE_VERSION_FULL_W`, marketing.build) →
run the modal (via `-initWithVersion:`, which shows the version in the header). **Accept** persists the
version + continues; **Decline** quits. Every other launch is silent. Same `tos_accepted` key + full-version
scheme as Win32. On-device-validated. **To re-trigger for testing:**
`sqlite3 "$HOME/Library/Application Support/RabbitEars/rabbitears.db" "DELETE FROM settings WHERE key='tos_accepted'"` then relaunch.

## Meters — full Win32-parity meter system (SHIPPED in v0.1.9)

**KEY: only Spectrum needs audio capture; Signal/Bitrate/Frames run off `FlowStats` — no consent, no desync.**
(History: an earlier libVLC-3.x `libvlc_audio_set_callbacks` tap took over output + desynced — **do NOT
revisit it**; a Core Audio process tap that shipped in 0.1.8 was removed because denied consent delivered
undetectable silence, then rebuilt properly with a `hasAudioTrack`-gated placeholder.)
- **`common/models/FlowStats.h`** — shared stream-health snapshot (both `VlcPlayerMac::sampleStats()` + Win32).
- **`MeterModel.{h,cpp}`** (`rabbitears::mac`, **MAC-LOCAL**) — `MeterKind`/`MeterStyle`/`MeterPalette`(7
  `SkinColor` roles)/`MeterTuning`(5 knobs)/`MeterConfig` + UTF-8 codecs. Kept out of `common/` + the Windows
  binary until the Win32 team reviews it — **that promotion (→ `common/ui` under a neutral `rabbitears::meter`
  ns) is the deferred E3**; the owner chose to leave it mac-local and let the Win32 team drive E3.
- **`MeterView.{h,mm}`** (ARC) — ONE view renders any kind × style from a `MeterConfig`. All four styles are
  real: **LED** (dot-matrix), **LCD** (ghosted), **Tube** (translucent-halo glow, sized by the glow knob),
  **Scope** (a phosphor `NSBezierPath` trace + `NSShadow` bloom). The 5 tuning knobs are wired (sensitivity→
  gain, smoothing→easing, peakHold→spectrum peak decay, breathing→bitrate ceiling ebb, glow→Tube/Scope bloom;
  centred so 0.5 == the pre-tuning behaviour). Spectrum folds in the RT-thread `os_unfair_lock` latch + energy
  probe + "grant permission" placeholder.
- **`SpectrumTap.{h,mm}`** (ARC) — the process tap + **vDSP FFT** → 24 log bands (preallocated, no RT alloc).
  Opt-in: creating it triggers the one-time consent prompt.
- **`MetersDialog.{h,mm}`** (ARC) — Settings ▸ Meters…: per-kind **Show + Style + 7 colour wells + 5 tuning
  sliders + a live preview** (a real `MeterView` fed synthetic data by a weak-self timer, updated as any
  control changes). Persists `meter_<kind>` / `_style` / `_colors` / `_tuning`; `loadMeterConfig` reads them back.
- **MainWindowController** glue: a **`DraggableMeterBar`** floats the meters over full-bleed video
  (`meter_pos_x/y`); a bottom-bar show/hide button (`meters_hidden`); **Video Only** (⌥⌘F/Esc/dbl-click).

**Backlog (not blocking):** on-device fine-tuning of the Tube glow radius / Scope trace weight / knob curves
(built blind, ship-quality per owner) — tweak the constants in `fillCell`/`strokeScope`. And **E3** (the
`MeterModel` promotion, owned by the Win32 team).

## Playlists — enable / disable / rename / refresh / delete (SHIPPED in v0.1.9)

A **Settings ▸ Manage Playlists…** sheet (`PlaylistsDialog`, ARC): per-playlist **Enabled** checkbox +
**⟳ Refresh** / **✎ Rename** icon buttons + a **Delete** button (confirmed). Live-applies + refreshes the grid.
- **Data layer (shared `common/db/Database`)** — the repo's **first schema migration** pattern:
  `Database::migrate()` is `user_version`-gated (`if (v < N)` steps). It added `playlists.enabled` (v1→2,
  idempotent via `hasColumn`, backfills existing rows to 1). *The Windows team has since extended `migrate()`
  through v3/v4 for EPG/recordings — the mac app runs the full chain and it's verified clean on a real DB.*
  `setPlaylistEnabled()`/`renamePlaylist()`; disabled playlists are hidden from every cross-playlist query via
  a shared `kEnabledOnly` predicate (`allChannels`/`favourites`/groups/countries/search/`channelByLcn`);
  `channelsByPlaylist()` stays literal. **Windows-safe** (default-enabled, no Win32 disable UI).
- **Refresh / friendly names** — Refresh re-downloads (URL) / re-reads (file) off the main queue + upserts via
  `bulkInsertChannels` (favourites + LCNs kept; does **not** prune removed channels), weak-self. Imports derive
  a **friendly name** (`friendlyName()` — stem/host, not the raw URL); the full URL/path stays as `source`.
- **Controller glue** — `reloadAfterPlaylistChange` re-points the active playlist if you disable/delete the
  current one + preserves the grid filter. The list uses a flipped `RETopClipView` so a short list top-anchors.

## Menu bar + gear (SHIPPED in v0.1.9)

Import/management/meter commands live in the macOS **menu bar** (a **File** menu: Add Playlist ⌘N / Open
Playlist File ⌘O / Manage Playlists…; **View ▸ Meters…**) as well as the in-window **⚙ gear**. `AppDelegate`
builds the menu bar; items target the app delegate and forward to `MainWindowController` (whose action
selectors are in the header), matching the View-toggle pattern (the controller isn't in the responder chain).

## Releasing (v0.1.10-mac shipped this way — full recipe in the `mac-release-deployment` memory)

**Version:** decoupled per-platform in `cmake/AppVersion.cmake` — Windows `APP_VERSION 0.2.4`, mac overridden
to **`0.1.10`** under `if(APPLE)`. Feeds both `CFBundleShortVersionString` + the generated `version.h`.
**Bump the mac line there** for a mac release; Windows is unaffected.

Deployed like the sibling **SQLTerminal**, **reusing the family credentials**: Developer ID **`386M76FV3K`**
signs, notarize via the **`SQLTerminal-notarize`** keychain profile, Sparkle EdDSA key in the login keychain
under account **`SQLTerminal`**.

```sh
# 1. universal build — a stock VLC.app is single-arch; get vlc-3.0.23-universal.dmg from videolan
#    (https://get.videolan.org/vlc/3.0.23/macosx/vlc-3.0.23-universal.dmg), mount, copy VLC.app out
scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DLIBVLC_MAC_PREFIX="<universal VLC.app>/Contents/MacOS"
# 2. bundle libVLC + sign inside-out (app, plugins, AND Sparkle's Updater.app/Autoupdate/XPC)
scripts/package-mac.sh <app> --vlc "<VLC.app>" \
    --sign "Developer ID Application: Matthew Mark (386M76FV3K)" \
    --entitlements mac/packaging/RabbitEars.entitlements
# 3. dmg → notarize → staple → Sparkle-sign
create-dmg … <dmg> <app>
xcrun notarytool submit <dmg> --keychain-profile SQLTerminal-notarize --wait
xcrun stapler staple <dmg>
build-mac*/sparkle/bin/sign_update --account SQLTerminal <dmg>   # prints edSignature + length
# 4. gh release create v<ver>-mac <dmg> --target main --latest=false   (keep Windows as "Latest")
#    then add the <item> to mac/packaging/appcast-mac.xml (sparkle:version = CFBundleVersion) ON MAIN.
#    xmllint FIRST. The feed serves from main; git push HANGS intermittently, so land the appcast via:
#    gh api --method PUT repos/OWNER/REPO/contents/mac/packaging/appcast-mac.xml \
#        -f message=… -f content=<base64> -f sha=<current-file-sha> -f branch=main
```

## Key files

```
mac/CMakeLists.txt                     # mac targets; rpath; Sparkle embed; icon; per-file -fobjc-arc list
mac/cmake/Mac.cmake                    # libVLC + Sparkle provisioning (-DLIBVLC_MAC_PREFIX overrides VLC.app)
mac/src/app/AppDelegate.mm             # lifecycle + menu bar (App/File/Edit/View) + custom About + Updates
mac/src/app/MainWindowController.mm    # the UI: top bar, grid, split, playback, meters glue, ToU gate
mac/src/app/VlcPlayerMac.{h,mm}        # libVLC wrapper + sampleStats()->FlowStats
mac/src/app/TermsDialog.{h,mm}         # first-launch / version-change Terms-of-Use gate (ARC)
mac/src/app/PlaylistsDialog.{h,mm}     # Settings > Manage Playlists (enable/disable/rename/refresh/delete)
mac/src/app/MetersDialog.{h,mm}        # Settings > Meters (Show/Style/Colours/Tuning + live preview)
mac/src/app/MeterView.{h,mm}           # unified meter renderer (4 kinds × 4 styles)
mac/src/app/MeterModel.{h,cpp}         # mac-local meter model (rabbitears::mac) + UTF-8 codecs
mac/src/app/SpectrumTap.{h,mm}         # Core Audio process tap + vDSP FFT -> bands (opt-in)
mac/platform/{Http,Log,Updater}.mm  mac/platform/Paths.cpp   # macOS platform layer
mac/packaging/{Info.plist.in, appcast-mac.xml, RabbitEars.icns, RabbitEars.entitlements}
scripts/{build-mac.sh, package-mac.sh, make-icns.py}         # build / bundle+sign+notarize / icon
cmake/AppVersion.cmake                 # per-platform version (Windows APP_VERSION + APPLE override)
common/models/FlowStats.h              # shared stream-health snapshot (Win32 + mac)
../common/ …                           # the shared engine (edit carefully — feeds Windows too)
```

## Working rules

- **Can't test GUI/audio headlessly** — real Mac testing is required for anything visual or audible (drive it
  with the computer-use MCP: `open` the app + screenshot; that's how the "can't paste" / meter / list-anchor
  bugs surfaced).
- **Branch off `main`, PR back**; CI validates both platforms. Keep any shared‑file (`common/`) edit
  behavior‑preserving on Windows.
- Run an **adversarial review on new ObjC++** (ARC/threading/Cocoa) before merging — it has repeatedly caught
  real bugs here.
- **`git push` hangs intermittently** this machine/session — clear stuck `git-remote-https` procs + retry, or
  use `gh pr merge` / `gh api` (REST works fine) for anything targeting `main`.
- Windows `gui-build` CI is **pre-existing red** (the theme engine needs `fxc`, absent in CI) — unrelated to mac.

## Seed prompt for a fresh session

```
Read mac/HANDOVER.md and the recalled memory. RabbitEars is a cross-platform native IPTV player
(Windows + macOS) in ONE repo (common/ + Win32/ + mac/, unified root CMake; playback libVLC, storage
SQLite). main carries BOTH platforms at decoupled versions: Windows 0.2.4 (theme engine + EPG/TV Guide,
scheduled recordings, multi-view Split/PIP) and mac 0.1.10 — the split lives in cmake/AppVersion.cmake
(APP_VERSION = Windows; an if(APPLE) override = mac). The mac app is SHIPPED + auto-updating (v0.1.10-mac,
universal, notarized, self-contained; Sparkle path proven end-to-end). App min macOS 26 (LSMinimumSystemVersion
only; deployment target unpinned so CI's older SDK builds; macOS 26 is Apple-Silicon-only). Build:
scripts/build-mac.sh [--app] (needs VLC.app). Release recipe: scripts/package-mac.sh + the
mac-release-deployment memory (Dev ID 386M76FV3K, notary profile SQLTerminal-notarize, sign_update
--account SQLTerminal; universal needs vlc-3.0.23-universal.dmg; ALWAYS xmllint appcast-mac.xml — an XML
'--' in a comment broke the feed once; land the appcast on main via `gh api` PUT because git push hangs).
GUI/audio can't be verified headlessly — real Mac testing required (computer-use MCP: open + screenshot).
Branch off main, PR back; keep common/ edits Windows-safe. mac .mm are MRC-style (app-lifetime leaks OK);
-fobjc-arc per-file only (MeterView/MetersDialog/SpectrumTap/PlaylistsDialog/TermsDialog). Run an
adversarial ObjC++ review before merging.

SHIPPED + MERGED to main (all mac features live): the METER system (unified MeterView, 4 kinds
Spectrum/Signal/Bitrate/Frames × 4 styles LED/LCD/Tube/Scope from a mac-local MeterModel; SpectrumTap =
Core Audio tap + vDSP FFT, opt-in; MetersDialog with Show/Style/7 colours/5 tuning sliders/live preview;
draggable meter bar; Video Only); PLAYLIST management (Settings > Manage Playlists: enable/disable/rename/
refresh/delete, backed by a user_version-gated Database::migrate() that added playlists.enabled + a
kEnabledOnly predicate; friendly import names); MENU BAR (File/View) + a ⚙ gear; and the TERMS-OF-USE gate
(TermsDialog, first-launch + version-change, tos_accepted keyed on RE_VERSION_FULL_W, mirrors Win32).
KEY meter fact: only Spectrum needs audio capture; Signal/Bitrate/Frames run off FlowStats (no consent).
BACKLOG (not blocking): on-device fine-tuning of the Tube glow / Scope trace / knob curves (fillCell/
strokeScope in MeterView.mm); E3 = promote MeterModel -> common/ui under a neutral rabbitears::meter ns,
owned by the Win32 team. NOTE: the Windows team added shared common/ code (XmltvParser, RecordingScheduler,
Gzip, VideoGrid, miniz, Database schema v3/v4) — the mac build compiles + migrates cleanly against it.
```
```
