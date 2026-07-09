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
benignly). `-fobjc-arc` is enabled **per-file** only where needed (`MeterView.mm`, `MetersDialog.mm`,
`SpectrumTap.mm` — weak-self timers, blocks, an RT latch). The shared core is portable C++ whose
headers carry `#if defined(_WIN32)` branches.

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

## Meters — full Win32-parity meter system (branch `mac-stats-meter` / PR #22, NOT merged)

**History.** A non-invasive Core Audio process tap (`AudioLevelTap`, macOS 14.2+) shipped in v0.1.8 then
was **removed** (`main` @ `c355feb`): on *denied* audio-capture consent the tap still succeeds and just
delivers silence → a dark, undetectable strip. (The older libVLC-3.x `libvlc_audio_set_callbacks` tap —
took over output, desynced — was an earlier dead end; do NOT revisit it.) Both were rebuilt properly.

**Current architecture** (on `mac-stats-meter`; on-device-validated in pieces; passed a clean adversarial
ARC/threading/CoreAudio review). **KEY: only Spectrum needs audio capture; Signal/Bitrate/Frames run off
`FlowStats` — no consent, no desync.**
- **`common/models/FlowStats.h`** — the shared stream-health snapshot (both `VlcPlayerMac::sampleStats()`
  and Win32). Byte rates over wall-clock, buffered-ahead, fps, corruption/lost deltas.
- **`MeterModel.{h,cpp}`** (`rabbitears::mac`, **MAC-LOCAL**) — `MeterKind` (Spectrum/Signal/Bitrate/
  Frames), `MeterStyle` (Led/Lcd/Tube/Scope), `MeterPalette` (7 `SkinColor` roles; `inherit` = theme bg),
  `MeterTuning` (5 knobs), `MeterConfig` + UTF-8 codecs (mirror `common/ui/Skin.cpp`). Kept out of
  `common/` + the Windows binary until the Win32 team reviews it — **that promotion (to `common/ui` under
  a neutral `rabbitears::meter` ns) is the deferred E3**; `Win32/ui/MiniMeter`'s model stays as-is until then.
- **`MeterView.{h,mm}`** (ARC) — ONE view renders any kind × style from a `MeterConfig` (peer of Win32
  `MiniMeter`). Spectrum folds in the RT-thread `os_unfair_lock` latch + energy probe + "grant permission"
  placeholder. 30fps easing. **All four styles render for real now: LED (dot-matrix), LCD (ghosted matrix),
  Tube (translucent-halo glow on lit cells, sized by the glow knob), Scope (a phosphor `NSBezierPath` trace
  of each kind's level series with an `NSShadow` bloom).** The 5 tuning knobs are wired: sensitivity→gain,
  smoothing→easing inertia, peakHold→spectrum peak decay, breathing→bitrate ceiling ebb, glow→Tube/Scope
  bloom (all centred so 0.5 ≈ the pre-tuning behaviour). Tube/Scope + the knob curves are **first-draft —
  expect on-device tuning** (`fillCell`/`strokeScope` in `MeterView.mm`).
- **`SpectrumTap.{h,mm}`** (ARC) — the recovered process tap + a **vDSP FFT** → 24 log bands (fixed
  preallocated buffers, no RT alloc). Opt-in: creating it triggers the one-time consent prompt.
- **`MetersDialog.{h,mm}`** (ARC) — Settings ▸ Meters… config sheet: per-kind **Show + Style + 7 colour
  wells + 5 tuning sliders + a live preview**. The preview is a real `MeterView` per kind fed synthetic
  animated data by a weak-self `_previewTimer`, updated live as any control changes (`controlChanged:` →
  `configForKind:` → `[preview setConfig:]`). Persists `meter_<kind>` / `_style` / `_colors` / `_tuning`
  (Win32-compatible keys); `loadMeterConfig` reads `_tuning` back into the live meters too.
- **MainWindowController** (MRC) glue: `loadMeterConfig`/`applyMeterConfig` (launch + dialog OK; Spectrum
  is toggled via the Meters dialog's per-kind Show — the old standalone ⌥⌘M "Show Spectrum" menu was removed as redundant);
  `tickStats` feeds Bitrate/Frames/Signal from `FlowStats` + Spectrum from the tap; the FlowStats consent
  cross-check is **gated on `VlcPlayerMac::hasAudioTrack()`** so a silent/video-only/muted stream isn't
  mistaken for denied capture; a **`DraggableMeterBar`** floats the meters over a full-bleed video (drag
  anywhere; position persisted `meter_pos_x/y`); a bottom-bar **show/hide button** (`meters_hidden`); and
  **Video Only** mode (⌥⌘F / Esc / double-click). `StatMeterView` + `SpectrumMeterView` were retired.

**Remaining:** the **E3** promotion above (`MeterModel` → `common/ui` under a neutral `rabbitears::meter` ns
once Win32 reviews) is the last pre-merge item, then merge PR #22 to `main`. **Backlog (not blocking):**
fine-tuning the Tube glow radius / Scope trace weight / knob response curves — built blind but on-device-validated
as ship-quality (Scope + LED confirmed by the owner 2026-07-05); tweak the constants in `fillCell`/`strokeScope`
if a later pass wants them dialed in.

## Playlists — enable / disable / rename / refresh / delete (branch `mac-stats-meter` / PR #22, alongside the meters)

A **Settings ▸ Manage Playlists…** sheet (`PlaylistsDialog`, ARC) lists every imported playlist with an
**Enabled** checkbox + per-row **⟳ Refresh** / **✎ Rename** (SF-Symbol icon buttons) + a **Delete** button
(destructive confirm). Changes apply live and refresh the grid.
- **Data layer (shared `common/db/Database`)** — this added the repo's **first schema migration**: a
  `user_version`-gated `Database::migrate()` steps 1→2 and `ALTER`s in
  `playlists.enabled INTEGER NOT NULL DEFAULT 1` (idempotent via a `hasColumn` check; SQLite backfills
  existing rows to 1, so no playlist vanishes on upgrade). `setPlaylistEnabled()` toggles it,
  `listPlaylists()` reads it. **Future schema changes extend `migrate()` with the next `if (v < N)` step.**
- **Disabled = hidden from every cross-playlist view** — `allChannels`, `favourites`, `channelsByGroup`,
  `listGroups`, `listCountries`, `channelsByCountry`, `searchChannels`, and `channelByLcn` all gained a
  `playlist_id IN (SELECT id FROM playlists WHERE enabled=1)` predicate (`kEnabledOnly`). `channelsByPlaylist()`
  stays literal — an explicit "show me exactly this playlist" accessor. **Windows-safe**: default-enabled +
  no Win32 disable UI ⇒ the predicate is a no-op there (windows-core CI validates).
- **Controller glue (MRC)** — `showPlaylists:` opens the sheet; `reloadAfterPlaylistChange` re-points the
  active playlist when you disable/delete the one you're viewing (falls back to the last enabled one) and
  **preserves the grid's active filter** across the menu rebuild; `restoreLastPlaylist` now restores the last
  *enabled* playlist. 12 self-test assertions cover the DB behaviour. On-device-validated (the list needed a
  flipped `RETopClipView` so a short list top-anchors instead of sinking to the bottom of the scroll box).
- **Rename / Refresh / friendly names** — **Rename** (`✎`) prompts a sheet → `Database::renamePlaylist`.
  **Refresh** (`⟳`) re-downloads (URL) or re-reads (file) the source off the main queue and upserts via
  `bulkInsertChannels` (favourites + custom LCNs preserved; new/changed channels appear — it does **not**
  prune removed ones), weak-self so a dismissed sheet is a no-op. Imports now derive a **friendly default
  name** (`friendlyName()` — file/last-path-segment stem, else host) instead of the raw URL; the full
  URL/path is still kept as `source`, and users can rename after.
- **Menu-bar parity** — the import/management/meter commands now live in the macOS menu bar too, not just
  the in-window Settings ▾ pull-down: a new **File** menu (Add Playlist ⌘N / Open Playlist File ⌘O /
  Manage Playlists…) and **View ▸ Meters…**. `AppDelegate.buildMenu` builds them; items target the app
  delegate and forward to `MainWindowController` (its action selectors are now in the header), matching the
  existing View-toggle pattern (the controller isn't in the responder chain). The in-window bar stays for now.

## Terms of Use gate (branch `mac-terms-of-use`, off main — for the next release)

A modal **`TermsDialog`** (ARC) shown once on **first launch and after any version change**,
mirroring the Win32 gate (`Win32/ui` `showTerms`) with the **verbatim terms text**. In
`MainWindowController -showWindow`, right after the DB opens: if `tos_accepted` ≠ the current full
version (`RE_VERSION_FULL_W`, marketing.build) → run the modal. **Accept** persists the version +
continues; **Decline** quits (`[NSApp terminate:]`). Every other launch is silent. Same `tos_accepted`
key + full-version scheme as Win32 (so a build-number bump re-prompts too). The dialog header shows the
version being accepted (`RE_VERSION_W`); this branch also bumps the mac version to **0.1.10** (the next
release). Passed an adversarial
ObjC++ review — clean (one fix applied: activate the app before the modal so it's frontmost on
login-item / `open -a` launches). **Needs on-device validation** (modal at launch + terms rendering)
before it ships.

## Releasing (v0.1.7-mac shipped this way — full recipe in the `mac-release-deployment` memory)

**Version:** the mac marketing version is now **decoupled from Windows** — `cmake/AppVersion.cmake` keeps
`APP_VERSION 0.2.0` for Windows but overrides it to **`0.1.9`** when `APPLE` (the mac port lacks the 0.2.0
skin/theme engine, so it deliberately trails). That feeds both `CFBundleShortVersionString` and the generated
`version.h`; the Windows `.exe` / `windows-core` CI are unaffected. Bump the mac line in `AppVersion.cmake`.

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
mac/src/app/AppDelegate.mm             # lifecycle + menu bar (App/File/Edit/View) + custom About + Check-for-Updates
mac/src/app/MainWindowController.mm    # the UI: top bar, grid, search/filter, split, volume, fullscreen, playback
mac/src/app/VlcPlayerMac.{h,mm}        # libVLC wrapper (bundled PlugIns) + sampleStats()->FlowStats for the meters
mac/platform/{Http,Log,Updater}.mm  mac/platform/Paths.cpp   # macOS platform layer
mac/src/tools/{selftest.cpp,playprobe.mm}
mac/packaging/{Info.plist.in, appcast-mac.xml, RabbitEars.icns, RabbitEars.entitlements}
scripts/{build-mac.sh, package-mac.sh, make-icns.py, xcode.sh}  # build / bundle+sign+notarize / icon / Xcode-gen
common/models/FlowStats.h                                    # shared stream-health snapshot (Win32 + mac)
mac/src/app/MeterModel.{h,cpp}                               # mac-local meter model (rabbitears::mac) + UTF-8 codecs
mac/src/app/MeterView.{h,mm}                                 # unified meter renderer (4 kinds x styles); peer of Win32 MiniMeter
mac/src/app/SpectrumTap.{h,mm}                               # Core Audio process tap + vDSP FFT -> bands (opt-in)
mac/src/app/MetersDialog.{h,mm}                              # Settings > Meters config sheet (Show/Style/Colours)
mac/src/app/PlaylistsDialog.{h,mm}                          # Settings > Manage Playlists sheet (enable/disable/delete)
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
SQLite). main is on 0.2.0 (Windows-led theme engine). The mac app is shipped + auto-updating (v0.1.8-mac,
universal, notarized, self-contained; Sparkle path proven end-to-end). App min macOS 26
(LSMinimumSystemVersion only; deployment target unpinned so CI's older SDK builds). Build:
scripts/build-mac.sh [--app] (needs VLC.app). Release recipe: scripts/package-mac.sh + the
mac-release-deployment memory (Dev ID 386M76FV3K, notary profile SQLTerminal-notarize, sign_update
--account SQLTerminal; universal needs vlc-3.0.23-universal.dmg; ALWAYS xmllint
mac/packaging/appcast-mac.xml before publishing — an XML '--' in a comment broke the feed once). GUI/audio
can't be verified headlessly — real Mac testing required. Branch off main, PR back; keep common/ edits
Windows-safe (windows-core CI validates). mac .mm are MRC-style (app-lifetime leaks OK); -fobjc-arc
per-file only (MeterView/MetersDialog/SpectrumTap). Run an adversarial ObjC++ review before merging.

IN PROGRESS: the mac METER system (branch mac-stats-meter / PR #22, NOT merged to main; validated
on-device in pieces; adversarial-review-clean). Full Win32-MiniMeter parity: FlowStats -> common/models;
a MAC-LOCAL meter model (rabbitears::mac, mac/src/app/MeterModel) = MeterKind/Style/Palette(SkinColor)/
Tuning/Config + UTF-8 codecs; a unified MeterView (renders the 4 kinds Spectrum/Signal/Bitrate/Frames x
styles from a MeterConfig); SpectrumTap (Core Audio process tap + vDSP FFT, opt-in — the only
consent-needing one; the "denied" placeholder is gated on VlcPlayerMac::hasAudioTrack so silent/video-only
streams don't false-flag); a Settings > Meters config dialog (per-kind Show + Style + 7 colour wells,
persists meter_<kind>/_style/_colors); a draggable floating meter bar over full-bleed video (meter_pos_x/y);
a bottom-bar show/hide button (meters_hidden); and Video Only mode (⌥⌘F/Esc/dbl-click). StatMeterView +
SpectrumMeterView were retired. KEY: only Spectrum needs audio capture; Signal/Bitrate/Frames run off
FlowStats (no consent). NEXT: (M1b/c) Tube + Scope styles — they render as LED today, the hardest to get
right blind (expect on-device tuning); (M2c/d) tuning sliders wired into the renderer + a live preview in
the dialog; (E3) promote MeterModel to common/ui under a neutral rabbitears::meter ns once the Win32 team
reviews it (Win32/ui/MiniMeter stays as-is until then). Backlog: universal build's x86_64 slice untested
on real Intel hardware.
```
