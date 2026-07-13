# RabbitEars — macOS Handover

The macOS team's living handover. (The Windows team's is [`Win32/HANDOVER.md`](../Win32/HANDOVER.md);
the port rationale + history is [`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).) Read this before
touching the mac app.

## What RabbitEars is

A cross-platform native IPTV player in **one repo**: **`common/`** (portable core — `M3uParser`,
`Database`, `DockLayout`, `FlowStats`, XMLTV/EPG + recording-scheduler cores, models, platform seam
*headers*), **`Win32/`** (the Windows app), **`mac/`** (this — the Cocoa app), under a unified root
`CMakeLists.txt` (`common` → `Win32`/`mac` per‑OS). Playback is **libVLC**; storage **SQLite**.

`main` carries **both platforms at decoupled versions**: **Windows 0.2.10** (theme engine + EPG/TV Guide,
scheduled recordings incl. wake-to-record + EPG series rules, multi-view Split/PIP, saved layouts, per-pane
recording — the Windows team ships from `main`) and **mac 0.2.9** (the parity line). The version split lives in
`cmake/AppVersion.cmake` (`APP_VERSION` = Windows; an `if(APPLE)` override = mac). **That file is the one
recurring merge conflict** between the two teams — keep the Windows line and the `if(APPLE)` override intact.
Keep all mac work **Windows-safe** and let `windows-core` / `macOS core` CI confirm.

> **Scope:** the SHIPPED mac **0.2.9** (build 261, universal, notarized — released 2026-07-13) reaches parity
> with the Windows **0.2.9** set: a **recording-rule editor** (New…/Edit… series rules), **series-rule episode
> dedup** (schema v6, already shared), **Traditional Chinese** (zh-Hant + zh-HK) in the Language selector, and
> the **GPL-3.0 notices** bundled in `Resources/`. It builds on 0.2.8 (localization EN + 日本語 + gear regrouped),
> 0.2.7 (the #25→#29 stack: favourites I/O, PiP resize/persist, saved layouts, per-pane recording, the recording
> scheduler + series rules) and 0.2.0 (TV Guide, multi-view, PiP). Unported **by design**: the Windows **theme
> engine** (mac uses the native system appearance) and **wake-to-record** (a non-root mac app can't arm a wake).
> Windows **0.2.10** is a Win32-only Traditional-Chinese *language-selection* hotfix, N/A to mac.

## SHIPPED — the 0.2.6/0.2.7 parity stack (v0.2.7-mac, build 234, 2026-07-11)

The five-PR stack **merged to `main` in order #25→#29** (merge commits `f387ad0`→`de240fd`), the mac version was
bumped to **0.2.7** (`cmake/AppVersion.cmake` APPLE override → build 234, commit `f9f7404`), and **`v0.2.7-mac`
shipped** — universal, notarized, appcast live on `main` (`3c832cf`). All merges were `gh pr merge --merge`; the
version bump + appcast landed via `gh api` PUT (the git REST path — `git push` still hangs this session). Zero
`common/`/`Win32/` edits across the whole stack — every core (`VideoGrid`, `M3uWriter`, `RecordingScheduler`,
`RecordingRules`, the schema-v5 `Database` methods) was **already compiled into the mac binary**; wiring, not porting.

| PR | Branch | Phase | On-device |
|----|--------|-------|-----------|
| [#25](https://github.com/arcanii/RabbitEars/pull/25) | `mac-favourites-io-guide` | Favourites import/export + Show in TV Guide | ✅ verified |
| [#26](https://github.com/arcanii/RabbitEars/pull/26) | `mac-pip-resize-persist` | PiP inset resize + persist (size/pos) | ✅ verified |
| [#27](https://github.com/arcanii/RabbitEars/pull/27) | `mac-saved-layouts` | Named saved multi-view layouts | ✅ verified |
| [#28](https://github.com/arcanii/RabbitEars/pull/28) | `mac-recording` | Per-pane recording to file (ts/mp4) | ⚠️ **shipped unverified** |
| [#29](https://github.com/arcanii/RabbitEars/pull/29) | `mac-recording-scheduler` | Scheduled recordings + EPG series rules + honest wake | ⚠️ **shipped unverified** |

> **⚠ 0.2.7 shipped WITHOUT on-device verification of P4 recording + P5-7 scheduler.** The owner chose "ship it"
> on 2026-07-11; the recording/scheduler **file-muxing paths never ran on real hardware** (green mac-core CI +
> the adversarial reviews were deemed sufficient). "Does a real `.ts`/`.mp4` play?" and "does the ~30s tick fire
> a playable scheduled file?" remain **unproven on device** — if a recording/scheduler bug surfaces in the wild,
> start here (→ a 0.2.8-mac patch). P1-3 (favourites/PiP/layouts) WERE device-verified before merge.

Each phase got an **adversarial ObjC++ review** (multi-agent workflow) that found + fixed real bugs before
merge — the reviews repeatedly earned their cost:
- **P1** import used a last-wins map (a channel duplicated across playlists marked only one row) → set-based.
- **P2** resize-taller-near-top overflowed the container, persisting `_pipPosY>1` (jumps on relaunch) → clamp to free space at the pinned corner.
- **P3** empty-pane wasn't cleared on apply (a carried/pre-existing stream kept playing) + NaN survived the geometry clamp.
- **P4** the **MKV format would ship broken** (bundled VLC has `libmux_ts`+`libmux_mp4` but **no mkv muxer** — `libmkv_plugin` is the DEMUXER; dropped mkv); a main-thread record-stop hung the UI on a stalled feed → `stopRecordingAsync` (reaper-thread peer); non-POSIX-locale timestamp; MRC formatter leak; same-second filename collision.
- **P5-7** cancelling a rule-generated future airing **hard-deleted the dedup tombstone → next expansion resurrected it** (kept as Cancelled anchor, Win32-parity); series channels resolve by NORMALISED tvg-id (else `@feed` channels record nothing); Recordings table preserves selection by id across the ~30s reload.

**Recording design (P4/P5):** each `VlcPlayerMac` gets a recorder = a SECOND headless libVLC media player
muxing to `~/Movies/RabbitEars` via `:sout=#std{access=file,mux=ts|mp4,dst='…'}` + `:sout-keep` (stream-copy,
no set_nsobject) — mac is PER-PANE (Win32 uses one shared recorder), so scheduled recordings run on a
**dedicated headless recorder** driven by a ~30s `NSTimer` tick over the shared `planScheduler()`. A ~2nd
connection per recording (breaks 1-connection IPTV accounts) is surfaced; ts is the crash-safe default (mp4
loses its index on a hard crash). `stopRecordingAsync` is off-main; `-finalizeRecordingsForQuit` (from
`applicationWillTerminate:`) flushes open recordings because the MRC app-lifetime objects don't destruct on quit.

**Honest wake (P7):** an `IOPMAssertion(PreventUserIdleSystemSleep)` is held while any recording runs, but a
non-root, hardened, Developer-ID app **CANNOT wake a sleeping Mac** — `IOPMSchedulePowerEvent` requires root
(verified in the macOS 26 SDK `IOPMLib.h`). So scheduling shows the caveat up front and the Recordings window
states it: records only while running + Mac awake (lid open). That is the deliberate degraded design, NOT a TODO.

> **PiP-switch freeze fix — SHIPPED in 0.2.7** (`0ab8618`, already on `main` before the stack landed). "Play in
> PiP" while already in PiP (any re-play into a running pane) called `set_media` without stopping the current
> input — on some live IPTV feeds the old inset wedged (frozen) and the new never started. Fix:
> `libvlc_media_player_stop()` before `set_media` in `VlcPlayerMac::play()`. **Could NOT reproduce with VOD test
> streams** (identical code to 208 switched them cleanly); it targets the likely cause but is **still unconfirmed
> on the affected IPTV channels** — now released, so a persistent freeze there would be a 0.2.8-mac follow-up.

**NEXT (on-device, post-ship validation):** 0.2.7 is out, so this now validates *shipped* code — record a real
HLS stream (confirm the `.ts`/`.mp4` plays), schedule ~1 min out (watch the ~30s tick fire a playable file), and
confirm the PiP-switch fix on real IPTV. A failure here means a 0.2.8-mac patch, not a blocked merge. **On-device
traps that cost hours are listed under Working rules.**

## Current state — v0.2.9-mac SHIPPED (2026-07-13)

**Latest: `v0.2.9-mac`** (build 261, universal, notarized, appcast live @ `cb14d56`) — **Windows-0.2.9 parity**:
a **recording-rule editor** (New…/Edit… + double-click in the Recordings window's *Series Rules* tab — channel-or-
`(any channel)`, title, Exact/Contains, lead/trail minutes; OK gated on a non-empty title; New→`addRule`,
Edit→`updateRule`+`clearPendingForRule`+re-expand — in `RecordingsWindowController`, **zero new catalog strings**,
reuses the shared Win32 rule ids); **series-rule episode dedup** (the shared schema-v6 `episode_key` +
`episodeKey`/`expandRules` were already compiled in — the mac fix is the 0.2.9 **pre-filter**: restrict the
channel-blind expander to *recordable* programmes BEFORE dedup, normalised @feed-safe, in
`-expandRecordingRules:`); **Traditional Chinese** zh-Hant + zh-HK in the Language selector + `Tr.h` routing
(Simplified/bare-`zh` fall through to English); and the **GPL-3.0** LICENSE + THIRD-PARTY-NOTICES.txt + `licenses/`
bundled into `Contents/Resources` (via `mac/CMakeLists.txt`; deep-codesign covers them) + an About-box copyright
line (`NSHumanReadableCopyright`). **ZERO `common/`/`Win32/` source changes** (PR #31). **Two fixes rode along:**
(1) **a shipped-since-0.2.7 CRASH** — `recordingPathFor:` cached its filename-scrub `NSCharacterSet` in a `static`
from the AUTORELEASED `characterSetWithCharactersInString:` without `retain` (MRC), so the **2nd** recording of a
session (a 2nd manual record, or the scheduler tick) messaged a freed object → `EXC_BAD_ACCESS`; caught by the
0.2.9 on-device pass, fixed with a retain (`474004d`); (2) **a shared-catalog `\r\n` bug** (PR #32, `common/i18n`)
— 27 strings were double-escaped (`\\r\\n`) so both platforms rendered a literal `\r\n`; collapsed to real
newlines + regenerated `Strings.cpp`. **On-device VERIFIED** (isolated `RABBITEARS_DATA_DIR` + a `127.0.0.1`
m3u/XMLTV fixture of public HLS streams): playback, manual recording→valid h264+aac `.ts`, rule editor CRUD +
validation + field round-trip, episode dedup (3 airings→2 schedules), the scheduler (tick→Recording→auto-Done,
playable `.ts`, no crash), and 繁體中文 rendering + selector. Adversarial ObjC++/ARC + logic reviews: 0 code
defects (1 low zh-Hans-region routing edge fixed). ⚠ **Trap:** the installed app + a dev build share the bundle id,
so a dual-instance screen composites both windows — clicks are unsafe for real data; verify the running PID's DB
via `lsof`/`ps eww` and prefer a HEADLESS scheduler re-test (sqlite-arm a schedule + monitor the log/DB).
**Before it: `v0.2.8-mac`** (build 248, universal, notarized, appcast @ `03048ec`) — **localization
(English + 日本語)** over the shared `common/i18n` catalog (a `Tr`/`TrF` AppKit layer = peer of `Win32/ui/Tr.h`;
Language selector System/English/日本語 + restart-to-apply; ~290 UI strings wrapped; **+145 mac-only ids** incl.
machine-draft JA + zh-Hant), and the **gear menu regrouped to match Win32** (Channels/Recording/View/Layout/
Language submenus). PR #30, on-device verified (switch→restart→JA across menus/dialogs/About/Terms; one meter-
label overflow found + fixed). **The mac branch now merges the shared catalog to `main`** — additive +
generator-validated (531 ids × 4 langs: en/ja/zh-Hant/zh-HK; zh-HK inherits zh-Hant via `base`; the mac-only
Chinese is never displayed — the mac selector offers only System/English/日本語 — it exists for catalog
completeness). See `mac/src/app/Tr.h`. **Before it: `v0.2.7-mac`** (build 234) — the 0.2.6/0.2.7 parity stack
(#25→#29) + the PiP-switch fix. Note recording/scheduler shipped **without**
on-device verification. The prior 0.2.0 milestone (still accurate for the multi-view/EPG internals) follows.

The mac app is **shipped and auto-updating**: **`v0.2.0-mac`** on GitHub — universal (arm64 + x86_64),
notarized, self-contained, `0.2.0` build `208`. It lands the three Windows-parity features — **TV Guide (EPG)**,
**multi-view Split/2×2** and **Picture-in-Picture** — plus the unified app icon.
**Key discovery: this was wiring, not porting** — every shared core it needed (`VideoGrid`, `XmltvParser`,
`Gzip`, `Programme`, the `Database` EPG methods) **already compiled into the mac binary**; PR #24 touched
**no `common/`, `Win32/` or `third_party/` file at all**.
The 0.2.0 release was verified end-to-end before the appcast went live: `spctl` → "Notarized Developer ID",
staple validates, the `edSignature` verifies against the **downloaded** GitHub asset under the key that matches
the app's embedded `SUPublicEDKey`, `sparkle:version` 208 > 172, `length` byte-exact, and the launched bundle's
banner read `0.2.0 (208)`.

Older: **`v0.1.10-mac`** — universal (arm64 + x86_64),
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

## TV Guide / EPG (branch `mac-multiview-tvguide`, 0.2.0)

The whole data pipeline was **already compiled in** — this is wiring, not porting. **No `common/` edits.**
- **Refresh** — `MainWindowController -refreshGuide:` (View ▸ Refresh Guide, or the ⚙ menu): for every
  *enabled* playlist with a non-empty `epgUrl`, off the main queue → `httpGet(url,…,60000)` →
  `gunzipIfNeeded` (XMLTV is usually served as `.xml.gz`) → `parseXmltv(...).programmes` → back on the main
  queue → `Database::bulkInsertProgrammes`. Newest-refresh-wins via `_epgToken`. **DB is main-thread only.**
- **Guide window** — `TvGuideWindowController` (own modeless `NSWindow`) assembles rows from
  `programmesInWindow(pid, now-6h, now+72h)`, grouping consecutive same-`channelId` programmes and joining to
  channels by the **lowercased base tvg-id** (`normId` strips iptv-org's `@feed` suffix: `CNN.us@SD` → `cnn.us`).
  Channels absent from the playlist are skipped, so **every row is playable**. `EpgGuideView` is a *flipped*
  custom `NSView` drawing 4 clipped regions (programme blocks + now-line, frozen channel column, frozen hour
  axis, corner) with its own scroll offsets — the AppKit peer of Win32's Direct2D three-clip control.
  Clicking a programme opens details + **Play Channel** (`channelByTvgId` → play → hide the guide).
- **Guide URL** — parsed from the M3U `x-tvg-url`, or set per playlist via **Manage Playlists ▸ 📅**
  (`setPlaylistEpgUrl`). *(A real bug fixed here: `importDoc` used to drop `doc.epgUrl`, so Refresh Guide
  had nothing to fetch.)*
- **Gotchas:** hour-axis ticks are aligned to **local** hour boundaries via `NSCalendar` (raw UTC stepping
  mislabels fractional-hour zones like UTC+5:30). All programme text goes through the `Encoding.h` UTF seam —
  **never** the naive `widen`/`narrow` byte-cast (titles are routinely non-ASCII).

## Multi-view — Split / 2×2 + Picture-in-Picture (branch `mac-multiview-tvguide`, 0.2.0)

- **`VlcEngineMac`** owns the ONE `libvlc_instance_t` (`libvlc_new` loads ~325 plugins — once per process).
  Every `VlcPlayerMac` borrows it via `init(engine)`; an Nth pane is now cheap. **Destroy all players before
  the engine.**
- **Pane model** — `MacVideoPane { NSView* view; unique_ptr<VlcPlayerMac> player; Channel channel; long long
  channelId; }` in `_panes`. `_player` / `_videoView` are **raw ALIASES to the active pane**, so all the
  pre-existing playback/meter/stats code kept working untouched. Single view = exactly one pane.
- **Layout** — `-applyVideoPaneLayout` feeds the shared `common/ui/VideoGrid::computeVideoPanes` and
  **y-flips** each box (`VideoGrid` is top-down; AppKit is bottom-up): `y = ich - b.y - b.h`, flipping against
  the *same integer* height passed in. Split(4) ⇒ 2×2; Pip(2) ⇒ full backdrop + draggable bottom-right inset.
- **Single-audio active pane** — only the active pane is audible; background panes are muted by
  **audio-track deselect** (`libvlc_audio_set_track(mp,-1)`), NOT volume=0 (libVLC resets volume to 100% when
  it recreates the aout on an HLS quality switch). `tickStats` **re-asserts** the mute every 250ms because the
  track may not exist yet at `play()` time. Clicking a pane activates it (accent `CALayer` border).
- **Async teardown (do NOT regress this)** — `stop()` is synchronous and blocks on a stuck stream. On collapse,
  panes are torn down on a GCD background queue. The pane's **`NSView` is retained across the async stop and
  released back on the main thread**: libVLC holds it via `set_nsobject` and its vout renders into it until the
  player is released. `applyViewMode` also re-points `_player`/`_videoView` at a **surviving** pane *before*
  any teardown. Collapsing carries the active stream into pane 0 (`carryStreamFromPane`, skipped when it's
  already the same channel); `Stop` clears `channelId` so a stopped stream is not resurrected by that carry.
- **Triggers** — View ▸ Single (⌃⌘1) / Split 2×2 (⌃⌘2) / Picture-in-Picture (⌃⌘3), with checkmarks; row
  context menu ▸ **Play in PiP** (plays the inset, backdrop stays active + audible — Win32 parity).
- **mac is SIMPLER than Win32 here:** no vout-host pool. That whole Win32 apparatus exists only to dodge a
  Direct3D11 "VLC (Direct3D11 output)" popout; AVFoundation composites sibling `NSView`s fine (validated with
  4 live streams). Persisting view mode is deliberately NOT done (Win32 doesn't either).

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

**⚠ The Spectrum meter needs the `com.apple.security.device.audio-input` entitlement.** It taps the app's own
audio output with `AudioHardwareCreateProcessTap`, and the **hardened runtime silently blocks audio capture
without it**: `AudioHardwareCreateProcessTap`, the aggregate device and the IOProc all return success, the tap
just delivers **zeros**, so the meter sits flat and *nothing* appears in the log. That entitlement is now in
`mac/packaging/RabbitEars.entitlements` (which `scripts/package-mac.sh` passes to `codesign`), alongside the
`NSAudioCaptureUsageDescription` Info.plist key that supplies the consent prompt's text.
**Consequence for testing:** a plain `scripts/build-mac.sh --app` dev build is *ad-hoc, linker-signed* with no
entitlements, so macOS never even prompts and the Spectrum meter is dead. To test it you must sign the dev
bundle first:
```sh
codesign --force --deep --options runtime --timestamp=none \
    --entitlements mac/packaging/RabbitEars.entitlements \
    -s "Developer ID Application: Matthew Mark (386M76FV3K)" build-mac/mac/RabbitEars.app
open build-mac/mac/RabbitEars.app     # now macOS prompts for audio recording; grant it
```
Signal/Bitrate/Frames never need any of this — they run off `FlowStats`.

**The "grant permission" placeholder** (`updateSpectrumAvailability:` → `MeterView -drawUnavailable:`) used to
be unreachable: it required **32 _consecutive_** audible-but-tap-silent 250 ms polls, and the counter was zeroed
both by `tickStats` whenever `libvlc_media_player_is_playing()` momentarily dipped false and by the
`demuxBytesPerSec == 0` branch — both happen routinely at HLS segment boundaries, so on a real stream it never
reached the threshold and a denied tap just looked like a dead meter. It now **accumulates** audible, tap-silent
polls (never reset by a transient dip) and trips after ~10 s of genuinely audible playback with zero tap energy;
`_spectrumEverHadEnergy` still latches "granted" so a quiet passage can't false-flag. The message was also wider
(43 chars @ 9 pt) than the 180 pt strip — it is now a short auto-shrinking *"Spectrum needs audio permission"*
with the full instruction in the view's tooltip. If the tap is hard-denied, `AudioDeviceStart` fails outright
(`SpectrumTap` init returns nil) and `startSpectrumTap` shows the placeholder immediately.

**Backlog (not blocking):** on-device fine-tuning of the Tube glow radius / Scope trace weight / knob curves
(built blind, ship-quality per owner) — tweak the constants in `fillCell`/`strokeScope`. And **E3** (the
`MeterModel` promotion, owned by the Win32 team). Also: `startSpectrumTap` runs inside `-showWindow` and
`AudioHardwareCreateProcessTap` **blocks on the consent prompt**, so on first launch the main window doesn't
appear until the user answers — worth deferring the tap off the critical path.

**Open low-severity findings (0.2.0 adversarial review, deliberately NOT fixed before the release cut** —
each needs on-device verification that wasn't available at the time, and none is user-visible enough to
justify an untested change to shipped audio/memory paths):
- **MRC dialog leaks.** `addPlaylist:` (~L714), `showSettings:` (~L1223) and `editChannelNumber:` (~L1457) in
  `MainWindowController.mm` `alloc` an `NSAlert`/`NSTextField`/`NSMenu` with `+1` and never release it. This
  is an MRC file, so each invocation leaks. The fix is an `autorelease` on each — but it depends on AppKit
  retaining the alert for the sheet's lifetime and the block retaining the accessory view, so **verify on
  device before shipping it**; a wrong call here is an over-release crash, not a leak.
- **Transient background-pane audio bleed (~250 ms).** `playChannel:intoPane:` calls `setMuted(true)`
  immediately, but at that moment no audio track exists yet, so `setMuted` sees `cur == -1` ("already
  silent") and no-ops. libVLC then auto-selects the track when it appears and the background pane is briefly
  audible until the next `tickStats` re-assert. A real fix needs a libVLC event callback
  (`libvlc_MediaPlayerESSelected`) rather than the 250 ms poll.

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
#    --entitlements now DEFAULTS to mac/packaging/RabbitEars.entitlements whenever --sign is given, and the
#    script refuses to sign a hardened binary whose entitlements lack audio-input. (It used to sign a
#    hardened build with an EMPTY entitlement set if you forgot the flag — which silently kills Spectrum.)
scripts/package-mac.sh <app> --vlc "<VLC.app>" \
    --sign "Developer ID Application: Matthew Mark (386M76FV3K)"
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
mac/src/app/MainWindowController.mm    # the UI: top bar, grid, split, playback, meters glue, ToU gate,
                                       #   video PANE model (Single/Split-2×2/PiP) + EPG orchestration
mac/src/app/VlcEngineMac.{h,mm}        # the ONE shared libvlc_instance_t; players borrow handle()
mac/src/app/VlcPlayerMac.{h,mm}        # libVLC wrapper: init(engine), setMuted (track-deselect), sampleStats()
mac/src/app/EpgGuideView.{h,mm}        # TV Guide renderer: flipped NSView, channels×time grid (ARC)
mac/src/app/TvGuideWindowController.{h,mm}  # guide window; DB->rows (normId @feed join), play-from-guide (ARC)
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
common/ui/VideoGrid.{h,cpp}            # SHARED pane geometry (Single/Split/Pip) — mac y-flips the boxes
common/core/{XmltvParser,Gzip}.{h,cpp} # SHARED EPG parse + gunzip (already compiled into mac; called as-is)
../common/ …                           # the shared engine (edit carefully — feeds Windows too)
```

## Working rules

- **Can't test GUI/audio headlessly** — real Mac testing is required for anything visual or audible (drive it
  with the computer-use MCP: `open` the app + screenshot; that's how the "can't paste" / meter / list-anchor
  bugs surfaced).
- **On-device testing recipe.** Launch the dev binary with an isolated DB so you never touch the user's data:
  `RABBITEARS_DATA_DIR=/tmp/redb build-mac/mac/RabbitEars.app/Contents/MacOS/RabbitEars &` (`defaultDbPath()`
  honors it). A local `python3 -m http.server` serving a hand-made `.m3u` + XMLTV fixture makes the whole
  import→refresh→guide→playback path deterministic and offline; use **`http://127.0.0.1:…`** (the loopback IP
  literal is ATS-exempt, so `NSURLSession` won't block cleartext HTTP). Public HLS streams that work for
  multi-view testing: `test-streams.mux.dev/x36xhzz/x36xhzz.m3u8`, Apple's `bipbop_4x3_variant.m3u8`.
- **Dev builds must be native arm64.** `build-mac/CMakeCache.txt` can hold a stale
  `CMAKE_OSX_ARCHITECTURES=arm64;x86_64` from a release build; a stock VLC.app is arm64-only, so the x86_64
  slice fails to link libvlc. Pass `-DCMAKE_OSX_ARCHITECTURES=arm64` to `scripts/build-mac.sh --app`.
- The **ToU gate is keyed on the FULL version incl. build number**, so every rebuild after a commit re-prompts
  in dev. That's expected, not a bug.
- **`open` can launch the WRONG bundle.** Several `RabbitEars.app`s share the bundle id
  (`/Applications`, `build-mac/`, `build-mac-universal/`), and LaunchServices may resolve to any of them —
  a stale `build-mac-universal` copy silently hijacked `open build-mac/mac/RabbitEars.app` for a whole
  debugging session. **Always confirm the version banner in `rabbitears.log`** (`==== RabbitEars (macOS) X.Y.Z (build) ====`)
  before trusting an on-device result, and `lsregister -f` the bundle you mean (or move the others aside).
- Launching the raw binary (`.../Contents/MacOS/RabbitEars`) instead of the bundle makes the *shell* the TCC
  "responsible process", so audio-capture permission is attributed to the terminal — another way to get a
  silently dead Spectrum meter. Use `open <bundle>` for anything permission-related.
- **Branch off `main`, PR back**; CI validates both platforms. Keep any shared‑file (`common/`) edit
  behavior‑preserving on Windows.
- Run an **adversarial review on new ObjC++** (ARC/threading/Cocoa) before merging — it has repeatedly caught
  real bugs here.
- **`git push` hangs intermittently** this machine/session — clear stuck `git-remote-https` procs + retry, or
  use `gh pr merge` / `gh api` (REST works fine) for anything targeting `main`.
- **The repo lives under `~/Desktop`, which macOS TCC protects.** Access can be revoked *mid-session* — every
  read, including `git`'s own cwd probe, starts returning `EPERM: operation not permitted` while `~/Documents`
  and `~/Downloads` still work. An agent's `request_directory` grant does **not** lift it (that's Claude's
  permission layer, not the kernel's). Fix: `tccutil reset SystemPolicyDesktopFolder com.anthropic.claude-code`
  restores it immediately, no relaunch. **Any review or build that ran during an EPERM window is void, not
  green** — the tools were reading nothing.
- Windows `gui-build` CI is **pre-existing red** (the theme engine needs `fxc`, absent in CI) — unrelated to mac.

## Seed prompt for a fresh session

```
Read mac/HANDOVER.md and the recalled memory. RabbitEars is a cross-platform native IPTV player
(Windows + macOS) in ONE repo (common/ + Win32/ + mac/, unified root CMake; playback libVLC, storage
SQLite). main carries BOTH platforms at decoupled versions via cmake/AppVersion.cmake (APP_VERSION =
Windows 0.2.10; an if(APPLE) override = mac 0.2.9) — that file is the recurring cross-team merge conflict,
keep both lines. mac is SHIPPED + auto-updating: v0.2.9-mac (build 261, universal, notarized,
self-contained, LOCALIZED EN + 日本語 + 繁體中文/香港; Sparkle proven end-to-end). App min macOS 26 (Apple-Silicon-only). Build:
scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES=arm64 (a stock VLC.app is arm64-only; the build-mac
CMakeCache can hold a stale universal arch). Release: scripts/package-mac.sh + the mac-release-deployment
memory (Dev ID 386M76FV3K, notary profile SQLTerminal-notarize, sign_update --account SQLTerminal;
universal needs vlc-3.0.23-universal.dmg; ALWAYS xmllint appcast-mac.xml). GUI/audio can't be verified
headlessly — real Mac testing required (computer-use MCP). mac .mm are MRC-style (app-lifetime leaks OK);
-fobjc-arc PER-FILE only (list in mac/CMakeLists.txt: MeterView/MetersDialog/SpectrumTap/PlaylistsDialog/
TermsDialog/EpgGuideView/TvGuideWindowController/RecordingsWindowController) — MainWindowController.mm &
VlcPlayerMac.mm are MRC. Run an adversarial ObjC++ review before merging (it has caught a real bug in
EVERY native phase). Dev testing: launch with RABBITEARS_DATA_DIR=<scratch> for an isolated DB, serve a
local m3u/XMLTV fixture over http://127.0.0.1 (ATS-exempt loopback).

STATE: v0.2.9-mac SHIPPED 2026-07-13 (build 261, universal, notarized; appcast @ cb14d56; PRs #31 code + #32 the
i18n fix, both merged by the user) = WINDOWS-0.2.9 PARITY: a recording-rule editor (New…/Edit… + double-click in
the Recordings window's Series Rules tab — channel/(any channel), title, Exact/Contains, lead/trail min; OK gated
on a non-empty title; New→addRule, Edit→updateRule+clearPendingForRule+re-expand; in RecordingsWindowController;
ZERO new catalog strings — reuses the shared Win32 rule ids); series-rule EPISODE DEDUP (shared schema-v6
episode_key + episodeKey/expandRules were already compiled in; the mac 0.2.9 change is the PRE-FILTER — restrict
the channel-blind expander to RECORDABLE programmes BEFORE dedup, normalised @feed-safe, in expandRecordingRules:);
TRADITIONAL CHINESE zh-Hant+zh-HK in the selector + Tr.h routing (Simplified/bare-zh → English); GPL-3.0 notices
(LICENSE+THIRD-PARTY-NOTICES.txt+licenses/) bundled into Contents/Resources + About-box NSHumanReadableCopyright.
ZERO common/Win32 SOURCE changes (PR #31). TWO fixes rode along: (1) a SHIPPED-SINCE-0.2.7 CRASH — recordingPathFor:
cached its filename-scrub NSCharacterSet in a `static` from the AUTORELEASED characterSetWithCharactersInString:
without retain (MRC file) → the 2nd recording of a session (2nd manual record OR the scheduler tick) messaged a
freed object → EXC_BAD_ACCESS; fixed with a retain (474004d); (2) a SHARED-CATALOG \r\n bug (PR #32, common/i18n) —
27 strings were double-escaped (\\r\\n) so BOTH platforms rendered a literal \r\n; collapsed to real newlines +
regenerated Strings.cpp. ON-DEVICE VERIFIED (isolated RABBITEARS_DATA_DIR + a 127.0.0.1 m3u/XMLTV fixture of PUBLIC
HLS streams — mux.dev x36xhzz + Apple bipbop): playback, manual recording→valid h264+aac .ts (ffprobe), rule editor
CRUD+validation+round-trip, episode dedup (3 airings→2 schedules), the scheduler (tick→Recording→auto-Done, playable
.ts, NO crash), and 繁體中文 rendering + selector (系統預設/English/日本語/✓繁體中文/繁體中文（香港）). Adversarial ObjC++/ARC +
logic reviews: 0 code defects (1 low zh-Hans-region routing edge fixed). RELEASE: universal VLC (vlc-3.0.23-universal.dmg)
→ package-mac.sh --sign → hdiutil+ditto dmg → sign dmg → notarytool --keychain-profile SQLTerminal-notarize --wait
(Accepted) → stapler staple → sign_update --account SQLTerminal (PROMPTS keychain — click Allow; the first run
blocks/times-out) → gh release create v0.2.9-mac --target main --latest=false → appcast via git push (WORKED this
session). GOTCHA: a STALE build-mac-universal/ from a prior release held a signed .app whose sealed Info.plist EPERM'd
the cmake regen ("Operation not permitted", NOT TCC) → rm -rf build-mac-universal before the universal configure.
GOTCHA: the installed app + a dev build SHARE the bundle id → a dual-instance screen composites both windows, clicks
unsafe for real data → verify the running PID's DB via lsof/ps eww + prefer a HEADLESS scheduler re-test.
BEFORE it — v0.2.8-mac SHIPPED 2026-07-12 (build 248, universal, notarized; appcast @ 03048ec, PR #30) = LOCALIZATION
(English + 日本語 over the shared common/i18n catalog; a Tr/TrF AppKit layer = peer of Win32/ui/Tr.h; Language
selector System/English/日本語 + restart-to-apply, pref in NSUserDefaults read BEFORE buildMenu; ~290 UI strings
wrapped across all 10 mac .mm; +145 mac-only ids Mac*-prefixed with machine-draft JA + zh-Hant) + the GEAR MENU
regrouped to match Win32 (Channels/Recording/View/Layout/Language submenus; dropped Theme + wake, N/A on mac).
FIRST mac change to touch common/ since 0.2.0 — mac now MERGES the shared catalog to main. main advanced hard
mid-work (Windows added zh-Hant/zh-HK + went 0.2.9); merged cleanly to 531 ids × 4 langs (zh-HK inherits zh-Hant
via "base"; the mac-only Chinese is NEVER displayed — the mac selector offers only System/English/日本語 — it
exists only for catalog completeness so the Windows core-selftest CI passes). On-device VERIFIED (switch→restart→
JA across menus/dialogs/About/Terms; one meter-label overflow found+fixed). Adversarial review: 292 wrap sites,
0 findings. The i18n bulk ran via workflows (per-file wrap+review agents); the first heavy-schema INVENTORY
workflow STALLED on giant structured output — fixed by edit-in-place agents returning only new-ids. PR #30
needed the USER to merge (auto-mode BLOCKS self-merging an agent-authored PR); branch pushed via the git-data
REST API (git push hangs); the shared-catalog PR CI runs on the head+base MERGE, which is what caught the
missing zh-Hant. i18n = mac/src/app/Tr.h (Tr/TrF) + common/i18n/*.json + common/core/Strings.{h,cpp} (GENERATED
by tools/i18n/gen_i18n.py — edit the JSON, never the .cpp; run gen_i18n.py + it validates completeness/parity).
BEFORE it — v0.2.7-mac SHIPPED 2026-07-11 (build 234, universal, notarized; appcast @ 3c832cf). The
0.2.6/0.2.7 PARITY STACK MERGED to main in order #25->#29 (merge commits f387ad0->de240fd), the mac version
bumped to 0.2.7 (f9f7404), and the PiP-switch freeze fix (0ab8618) rode in too. ZERO common/Win32 edits — every
core was already compiled into the mac binary (wiring, not porting). What shipped:
 #25 favourites import/export (M3uWriter) + Show in TV Guide
 #26 PiP inset resize (top-left grip, pin bottom-right) + persist size/pos
 #27 named saved multi-view layouts (settings K/V, mac-local serialization)
 #28 per-pane recording (2nd headless libVLC player, :sout=#std, ts/mp4; NO mkv — bundled VLC has no mkv muxer)
 #29 dedicated headless recorder + ~30s NSTimer tick over planScheduler(); schedule-from-guide; EPG series
     rules (expandRules); Recordings window; honest wake messaging
 +   PiP-switch fix: libvlc_media_player_stop() before set_media in VlcPlayerMac::play()
CAUTION: #28 recording + #29 scheduler shipped WITHOUT on-device verification (owner chose "ship it") — the
file-muxing paths never ran on real hardware; the PiP-switch fix is likewise unconfirmed on the affected IPTV
channels. P1-3 WERE device-verified. A recording/scheduler/PiP bug in the wild → a 0.2.8-mac patch, not a
blocked merge. git push STILL HANGS this session — the 5 PRs were merged with gh pr merge --merge, and the
version bump + appcast landed via gh api PUT contents (git REST works; push does not). Release recipe +
gotchas: the mac-release-deployment memory.

DO NOT REGRESS (multi-view): the pane's NSView must be RETAINED across the async player stop (libVLC's vout
renders into it via set_nsobject); applyViewMode re-points the _player/_videoView aliases at a SURVIVING pane
BEFORE any teardown; Stop clears the pane's channelId or a later collapse resurrects the stream. DB is
MAIN-THREAD ONLY. Recording: destroy players (incl. recorders) before the engine; the borrowed libVLC
instance + the engine LEAK on quit (so a detached async stop is safe past termination, but an open recording
must be finalized in -finalizeRecordingsForQuit).

ON-DEVICE TRAPS (each cost hours): `open` can launch a DIFFERENT RabbitEars.app (several share the bundle id
— /Applications, build-mac/, build-mac-universal/) — ALWAYS confirm the banner "==== RabbitEars (macOS)
X.Y.Z (build) ====" in rabbitears.log. Codesign a dev build (Dev ID + --options runtime + the entitlements
file with com.apple.security.device.audio-input) to test Spectrum. The repo is under ~/Desktop which macOS
TCC protects — access can be revoked MID-SESSION (every read incl. git EPERMs); fix:
`tccutil reset SystemPolicyDesktopFolder com.anthropic.claude-code` (no relaunch). Closing the main window
QUITS the app. The BUILD_NUMBER only refreshes on cmake RECONFIGURE, not per-build. The dev build SHARES the
app's NSUserDefaults domain (com.rabbitears.RabbitEars) with any installed RabbitEars — RABBITEARS_DATA_DIR
isolates the DB but NOT the defaults (ui_language, window frames), so switching language while testing writes
the USER's real defaults. Bumping the version re-triggers the ToU gate (keyed on full version incl. build).

NEXT: mac is at 0.2.9 (parity with Windows 0.2.9); Windows is at 0.2.10 (a Win32-only Chinese-selection hotfix,
already N/A to mac). The 0.2.7 recording/scheduler is now ON-DEVICE VERIFIED (+ its 2nd-recording crash fixed in
0.2.9), so that long-open item is CLOSED. Candidate targets: (a) **NATIVE TRANSLATION REVIEW — now the priority:**
0.2.9 EXPOSES Traditional Chinese to users, so JA *and* zh-Hant *and* zh-HK are all shipping as machine drafts —
`gen_i18n.py --review ja` / `zh-Hant` / `zh-HK` is the gate before truly advertising CJK; (b) the `MenuVideoOnly`
`\t` follow-up — same double-escape root cause as PR #32 but a Win32 menu accelerator (a literal `\t` on the
Windows menu), left for a Windows-side check; (c) whatever the Windows team ships next past 0.2.10 (read
Win32/HANDOVER.md; skip the theme engine + wake, N/A on mac). Backlog: promote MeterModel to common/ui (E3,
Win32-team owned); on-device meter fine-tuning (fillCell/strokeScope); prune two now-unused catalog ids
(MacMainWindowLayoutsMenu, MacMainWindowFormatHeader).
```
```
