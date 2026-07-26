# RabbitEars — Backlog

Parked / not-yet-scheduled work, split out of `Win32/HANDOVER.md` (which stays the single entry
point for **current state**; this is the parking lot for **what's next**). Ship small items as 0.1.x
point releases; the **theme engine** is the big 0.2.x epic. Windows-team doc — kept under `Win32/`
so it doesn't collide with the macOS team's root-level edits (they own `mac/`).

---

## 🌍 Shared-core addition: Xtream group-title→country fallback · flagged by the macOS team, 2026-07-16

`common/db/Database.cpp` (PR #41): `listCountries()` / `channelsByCountry()` now derive a channel's
country from an **Xtream-style group-title prefix** (`US| NEWS`, `[UK] SPORTS`, `FR - CINEMA`) when the
tvg-id carries no `".cc"` suffix — so **your Countries nav-tree gains entries for Xtream playlists** that
previously showed none. The tvg-id stays authoritative; the fallback needs an explicit delimiter (a bare
space never counts: "IT MOVIES" ≠ Italy) and deny-lists HD/SD/TV/EN/XX. 3-letter tokens ("USA|") are
deliberately rejected — say the word if you want a small alias map and we'll extend the shared rule.
The rule runs as a registered SQLite scalar (`effective_country(tvg_id, group_title)`, deterministic,
registered in `Database::open`) so `channelsByCountry` filters **server-side** — the adversarial review
benchmarked a C++-side materialize-all filter at ~30 ms/call at 14k channels on a per-keystroke mac path;
the scalar restores the sub-millisecond shape, and the list and the filter share one rule so they can't
drift. Deny-list: HD/SD/TV/EN/XX + **EX** ("EX-YU|" Balkan groups) + **ON** ("ON-DEMAND"). **Known-wrong,
kept deliberately:** "AR|" on pan-Arabic groups files under Argentina in your nav tree (`countryLabel`
renders the localized name) — 'ar' is genuinely Argentina on Latino panels, so neither denying nor keeping
is right for both; an ISO-whitelist or playlist-majority disambiguation would be follow-ups. No schema
change. Twin selftests extended (the CLI "By country" block). Any pushback → ping the macOS team.

---

## 🎬 Shared-core fix landed: padding-proof series-rule dedup + **SCHEMA v7** · flagged by the macOS team, 2026-07-16

`common/` (PR #40) fixes a **Windows bug too**: editing a rule's lead mid-recording (your edit flow:
`updateRule` + `clearPendingForRule` + re-expand, `MainWindowCommands.cpp:~1174`) used to spawn a
duplicate Pending row for the in-progress airing — the slot key is the PADDED start, which the edit
moved — that could never start (single recorder busy) and rotted into a **phantom Missed**; a
**Cancelled** future airing's tombstone was resurrected by the same edit; and two rules with
different padding defeated the "two rules → one recording" collapse.

**⚠ This adds a schema migration — v6 → v7:** `scheduled_recordings.prog_start_utc` (the programme's
UNPADDED start, set by `expandRules`; 0 for manual/pre-v7 rows). Rule rows now dedup on
`(channel, prog_start_utc)` — exact and immune to padding edits. An adversarial review showed why a
heuristic can't do it: a rule row's padded window also *contains* adjacent/nested airings whenever
trail ≥ the next airing's duration, so window containment silently swallowed every second
back-to-back bulletin. Pre-v7 rows fall back to title-scoped containment for the transition (they
age out of the 14-day horizon); manual rows keep slot-only dedup, unchanged. The migrate() gate is
bumped (`v>=7`), the v6→v7 path is empirically verified (hand-built v6 DB → open → v7, rows intact),
and regression tests are in `RabbitEarsCli --selftest` (the "Padding-proof dedup (v7)" block). The
mac rule editor now clamps lead/trail to 0..240 min matching your `readMinutes`. Windows needs no
source change — `addSchedule`/`readSchedule` carry the column via the shared DAO. Any pushback →
ping the macOS team.

---

## 🌐 i18n — native CJK translation review (shared `common/i18n`) · flagged by the macOS team, 2026-07-15

The macOS team ran an AI-assisted, adversarially-verified quality pass over the machine-draft
**JA / zh-Hant / zh-HK** catalog and opened a PR (branch **`i18n-cjk-quality-fixes`**) applying
**36 verified consistency fixes** — *no* mistranslations and *no* placeholder breaks (all terminology,
native-punctuation, or regional word-choice polish). Nothing structural: **no ids added or removed**,
placeholder parity preserved, `core-selftest` green on both platforms. Most fixes touch `Mac*`-prefixed
ids (mac-only display), **but 14 changed ids are Windows-facing** (referenced in `Win32/` source), so they
change what Windows 日本語 / 繁體中文 / 香港 users see — **please eyeball the Windows rendering:**

- **JA "wake" strings — a *Windows* feature (mac can't wake):** `MenuRunWakeTaskNow`,
  `StatusWakeTaskStarted`, `StatusWakeTaskFailed` — unified 復帰 → **スリープ解除** to match the rest of the
  wake-to-record UI (`MenuWakeToRecord`, `StatusWakeToRecordOff`). Worth a native glance since these are
  primarily Windows-facing.
- **JA shared:** `StatusAiringCancelledRule` (シリーズのルール→シリーズルール); `DialogDeletePlaylistBody`
  (half-width `?` → full-width `？`); `AboutLibVlcCredit` (VLC 貢献者 → VLC の貢献者);
  `ExportFavouritesNoneBody`, `ImportFavouritesSkippedLine` (half-width → full-width `（）`).
- **zh-Hant shared:** `RuleColMatch` (相符 → 比對方式); `EpgRulesQueuedDetail` (個新的播出 → 個新的播出項目);
  `TooltipMeterFrames` (掉幀 → 掉影格, the Taiwan form, matching 影格率 in the same string);
  `TermsBodyText` (clause-5 parens → full-width `（）`, body text otherwise unchanged).
- **zh-HK new overrides on shared ids** (these now differ from zh-Hant on Windows too): `RecordSeriesTitle`,
  `RecordSeriesAlreadyHeading`, `StatusAiringCancelledRule` — 影集 → **劇集** (HK reads 影集 as "photo album").

Full per-string rationale + verifier verdicts are in the PR. Any pushback → ping the macOS team and they'll
adjust the JSON (the shared catalog is edited via `common/i18n/*.json` + `tools/i18n/gen_i18n.py`, never the
generated `Strings.cpp`). A separate small macOS PR (`prune-dead-catalog-ids`) removes 6 dead ids
(`LangRestart*` + 2 unused mac ids) — independent, also Windows-safe.

---

## 🎨 Theme engine — ✅ SHIPPED in v0.2.0 (this section is kept as the design record)

All four skins (Dark / Light / Cyberpunk / Steampunk) and the complete authored GPU-effect set
(strip underglow · gutter neon · button glow · Steampunk heat-haze) shipped in **v0.2.0**, theme-ON
by default. **Optional follow-ups still open:** per-skin glow/heat-haze *tuning* (`SkinGpu` in
`common/ui/Skin.cpp` + the wobble/plume magnitudes in `underglow.hlsl`); Steampunk palette/serif
polish; extend `SkinGpu` to the GDI+ button glow (still a hardcoded strength); refresh the
About/Splash *logo* art to match the clockwork icon; or reskin a further surface (nav / grid /
dialogs — Appendix A of `docs/THEME_ENGINE.md`). The design/architecture below is the record.

**Goal (owner-directed):** a **full-app reskin** with **runtime-selectable skins**
(Dark / Steampunk / Cyberpunk — mockups exist) powered by **Direct3D 11 + HLSL shaders** for
animated GPU effects. Write a **`Win32/docs/THEME_ENGINE.md`** design doc first, and **flag the
shared skin-model boundary to the macOS team** before any engine code lands (they move fast on
`mac/`). → **Design doc written: [`docs/THEME_ENGINE.md`](docs/THEME_ENGINE.md).**

**Architecture:**
- **One shared GPU device — Direct2D-on-D3D11 interop.** D2D 1.1 runs on a D3D11 device, so you get
  crisp GPU 2D (geometry, text, gradients, `ID2D1Effect` blur/glow/shadow) *and* can drop to **HLSL
  pixel shaders** for the "living" bits (flowing neon, heat-haze, reactive glows). The channel grid
  is *already* Direct2D (`Win32/ui/ChannelGridControl.cpp`, `Win32/ui/D2DSupport.h`), so it folds
  into the same device rather than fighting it. Do **not** rewrite everything in raw D3D.
- **Skin = shared model + per-platform renderer** (mirrors the meter model↔renderer seam): the skin
  **model** (theme id, asset/param/animation refs, layout) lives in **`common/`** (shared); the
  **renderer** is platform-specific — **`Win32/`** in D3D11/D2D+HLSL, **`mac/`** in Metal/Core
  Graphics later. Keeps the shared core unforked.
- **Runtime-selectable + persisted** (Settings → Theme), switchable live.

**Phased build** (each phase owner-verified — GUI rendering can't be checked in the dev sandbox):
1. **Foundation spike** — stand up the D3D11+D2D interop device + swapchain on ONE surface (the
   command/transport bar), render the *current* look at parity + prove ONE HLSL effect. De-risks the
   whole engine before committing to reskin everything. Behind a build flag.
2. **Skin abstraction** — grow `Win32/ui/Theme.h` from a flat palette into a `Skin` system (palette
   + assets + shaders + per-widget draw) + a skin registry + the runtime switch + Settings UI; define
   the skin *model* in `common/`.
3. **Reskin surfaces incrementally** — transport + meters first (the mockups), then command-bar
   chrome, then nav + grid, then dialogs.
4. **Author the sets** — Steampunk + Cyberpunk art + shaders. macOS renderer mirrors in Metal.

**Risks / coordination:**
- **D3D11 is Windows-only** and the macOS port is active → keep the skin *model* shared, the
  *renderer* per-platform; flag the boundary to the mac team early (Metal is their side).
- **libVLC owns the video surface** (its own D3D11 vout in a child HWND) → skin *around* video;
  shader-over-video needs care and probably isn't worth it early.
- **Real-time swapchain hygiene** — flip-model present, animate only when visible (don't peg GPU/CPU
  when idle), DPI/resize/occlusion handling.
- **Not a point release** — realistically a 0.2.x epic across several versions.

---

## 🖥️ Native ARM64 build (Windows-on-ARM) — ✅ SHIPPED in v0.2.5

**Owner-directed (backlogged 2026-07-08); implemented 2026-07-09.** The app now has a **native ARM64
build** with **auto-update** — see `Win32/HANDOVER.md` "Immediate next steps → 0.2.5" for the full
wiring. Perf-verified: native cold-start **~4×** the emulated x64 and playback **~4.4×** lighter CPU.

**Delivery — Option B (per-arch installers), chosen over the earlier launcher sketch.** The original
idea was one x64 launcher that detects arch (`IsWow64Process2` → `IMAGE_FILE_MACHINE_ARM64`) and runs
the native set — one download, but a **doubled** installer (both libVLC plugin trees) + a new component
+ relaunch wiring. Owner chose **B**: two lean per-arch installers + **two appcast feeds** (x64 keeps
`appcast.xml`, arm64 reads `appcast-arm64.xml`; the build selects its feed via `_M_ARM64` in
`Updater.cpp`). Keeps x64 users byte-for-byte unchanged; each arch auto-updates natively.

**Dependency status (all resolved):**
- ✅ **libVLC 3.0.23** — NuGet unpacks `build/arm64` (full DLLs + `plugins/`); `LibVlc.cmake` picks it by
  `CMAKE_CXX_COMPILER_ARCHITECTURE_ID`.
- ✅ **SQLite, miniz** — vendored C source, compile for any target.
- ✅ **WinSparkle** — **0.9.3 vendored for both arches** (`third_party/winsparkle/{lib,bin}/{x64,arm64}`);
  CMake selects the slice by target arch. Auto-update linked into the ARM64 exe (dumpbin-confirmed).
- ✅ **Toolchain / CMake / packaging** — `scripts/build-arm64.cmd` (native `vcvarsarm64`);
  `packaging/installer.iss` arch-parameterized; `scripts/build-installer.cmd [arm64]` builds either;
  `scripts/make-appcast.ps1 -Arch arm64` writes the arm64 feed; two-arch flow in `docs/RELEASING.md`.

**Shipped 2026-07-09** (v0.2.5 @ `fbebcc7`, three installers incl. universal, two live appcasts;
universal + About-arch owner-verified on the ARM device). Only remnant: an ARM64 `plugins.dat`
(low value — native scan ~3 s; no ARM64 `vlc-cache-gen` exists, would need building the tiny tool
against the NuGet's arm64 `libvlccore`).

---

## ⏱️ Slow first startup — ✅ FIXED for x64 installs (0.2.6 batch, install-time `vlc-cache-gen`)

libVLC rescanned all 323 plugins each launch (no writable `plugins.dat` under Program Files); ~10 s
cold. **Fix (0.2.6):** vendored `third_party/vlc-tools/x64/vlc-cache-gen.exe` (3.0.23, byte-identical
`libvlccore` to our NuGet — provenance in the README there), shipped + run by `installer.iss`
post-install **on native-x64 machines only** (`IsX64Native` gate — NEVER under ARM emulation, which
silently writes an EMPTY cache ⇒ libVLC loads 0 plugins ⇒ no playback); `[UninstallDelete]` cleans the
generated `plugins.dat`. This is VLC's own installer approach — install-time generation keeps the
cache's per-plugin path/mtime/size records exact, where a CI-pre-generated file would go stale
(zip-extraction mtime/timezone skew). CI verifier: `.github/workflows/plugins-cache-verify.yml`
(native-x64 runner, fails hard on the empty-cache mode; auto-runs when the vendored tool changes).
Remnants: the ARM64 cache (see the ARM64 section; low value) and the optional plugin-set trim
(risky, low value now). Dev builds still scan (only the installer generates) — the owner dev-runs
the native ARM64 build (~3 s scan), so that's moot.

---

## 📺 TV Guide ↔ channel enhancements — ✅ BOTH DONE

- **Favourite a channel from the guide** — SHIPPED in v0.2.5 (right-click a guide row).
- **"Show in TV Guide" from a channel** — in the 0.2.6 batch: grid right-click →
  `epgGuideShowChannel(tvgId, now)` (normalised-tvg-id row match, filter cleared, row top-aligned,
  time axis re-centred on "now"; builds the guide first when needed).

---

## Deferred epics

- **JSON profiles** (deferred since 0.1.5): per-profile settings + playlist sources, channel cache
  rebuilt per profile; `%LOCALAPPDATA%\RabbitEars\profiles\*.json` + an active-profile pointer; keep
  the ~197 MB channel cache OUT of the JSON.

## macOS

- **Validate the x86_64 (Intel) slice on real Intel hardware** — the shipped `v0.1.7-mac` is a
  universal (arm64 + x86_64) notarized DMG. The **arm64 slice is confirmed on-device (2026-07-04,
  runs + looks good)**; the **x86_64 slice has never run on a physical Intel Mac**. When an Intel
  machine is available: launch the app + run `RabbitEarsPlayProbe` (headless play smoke test) + a
  quick GUI/playback pass. Arranging hardware may take a while — parked, not blocking.
- **Faster update propagation — move the Sparkle feed off `raw.githubusercontent.com`** — the
  `SUFeedURL` (in `mac/packaging/Info.plist.in`) points at the raw appcast, which is served with
  `Cache-Control: max-age=300`, so a published or fixed appcast takes up to ~5 min to reach
  clients (a stale cache re-served the broken feed once during the 0.1.8 rollout). Move the feed to
  **GitHub Pages** (or another low-cache host) and update `SUFeedURL`. Gotcha: already-shipped
  clients keep polling the OLD URL baked into their Info.plist, so keep the raw appcast alive (or
  301-redirect it) — the switch only speeds up builds cut after the change.

## Features

*(Shipped and removed from this list: EPG/XMLTV + scheduled recording Phase 2 (0.2.1); multi-view
Split/PIP (0.2.1); concurrent per-pane recording, MP4, PIP resize, view-mode/PIP persistence,
resume-last-channel, named saved layouts, import/export favourites, Show-in-Guide (0.2.6).)*

- **Recording Phase 3** — ✅ DONE in the 0.2.7 batch: **wake-to-record** (a Windows Scheduled Task
  wakes the PC + launches RabbitEars before a recording; `Win32/platform/WakeScheduler`, sleep also
  suppressed *during* a recording) + **EPG-driven series rules** (schema v5 `recording_rules`, the
  pure `common/core/RecordingRules` expander, "Record series" in the TV Guide, Settings ▸ Recording
  Rules…). Remaining Phase-3-adjacent ideas: **season/episode dedup** (use `Programme::episodeNum` /
  `subTitle` so a repeat airing of an already-recorded episode is skipped — today a rule records
  every airing of a matching title), and richer rule editing (lead/trail padding + `Contains` rules
  are in the model + DB but only `Exact` rules are creatable from the UI).
- **Wake-timer preflight** — ✅ DONE (0.2.8-dev, unreleased): `common/core/PowerPolicy` (pure) +
  `Win32/platform/PowerPolicy` (probe) detect that Windows will not ARM the RTC wake timer for the
  current power source (`GUID_ALLOW_RTC_WAKE` is per-source; `AC=Enable, DC=Disable` is a common
  laptop default ⇒ unplugged = every recording silently missed), warn in the schedules manager +
  the Settings toggle, and add **Settings ▸ Run wake task now** to exercise the `--scheduled-wake`
  path without sleeping. See `Win32/HANDOVER.md` (0.2.8-dev block). Possible follow-ups: a "Fix this"
  button that deep-links Power Options (`control.exe powercfg.cpl` / `powercfg /SETDCVALUEINDEX`
  needs elevation — probably just open the page), and re-querying the verdict after New… in the manager.
- **Verify wake-to-record on real hardware** — the dev box is a **Parallels ARM64 VM** (S0 Modern
  Standby only, hibernate off, host suspends the guest), so a genuine wake-from-sleep test is
  impossible there. On a physical PC: queue a recording, close the app, sleep the machine, confirm
  it wakes and records. Parked (owner: non-critical for now); everything *except* the sleep is
  already exercisable via Settings ▸ Run wake task now.
- **Transcoding on record** — format/quality/size presets (CPU-heavy). Today recording is always a
  lossless stream copy (`ts`/`mkv`/`mp4` mux, no re-encode).
- **Background dead-link checker** — so "Hide unavailable" isn't purely passive: probe channels
  off-thread, write `dead_status`, throttle + cache. Pairs with the existing `setDeadStatus` DAO.
- **⛔ TV Guide off-thread — DEFERRED, and the backlog's premise below is WRONG.** Two findings from
  the 0.2.14 investigation, both verified against the code and the live DB:
  1. **The "cheaper first win" described below does not exist.** `programmesInWindow` ALREADY bounds
     by the window — `WHERE playlist_id=? AND start_utc<? AND stop_utc>?` (`common/db/Database.cpp`).
     It does not scan the whole playlist partition. Index variants were benchmarked during the
     design pass and every one REGRESSED (262 → 430 → 1045 ms; an unrelated count 52 → 570 ms).
  2. **There is no evidence the problem exists at all.** The owner's library has **0 rows in
     `epg_programmes`** (442 channels, one playlist, no XMLTV ever fetched), so the diag timer
     `"TV guide first-open: DB+build … ms"` has never once fired. Optimising an unmeasured path —
     by adding a second sqlite connection and an async rewrite of `onEpgGuide`, in the same area
     where a review already caught one regression — is how you buy risk with no return.
  **To revisit:** set a guide URL (Settings ▸ Set Guide URL…), run Settings ▸ Refresh Guide, open the
  TV Guide once, and read that diag line. Only if the number is genuinely bad is the threading work
  justified — and it must then use its OWN sqlite3 connection (never `st->db`, which is FULLMUTEX
  and shared with the scheduler tick + fetch writes) and post rows back via `WM_APP+8` (the next
  free id: +6/+7 went to the dead-link sweep, +10 is ChannelGridControl).
  *(Original entry, retained for context — note its premise is disproven by (1) above.)*
- **TV Guide first-open: build off the UI thread** (0.2.8-dev added a *loading box* only). The first
  `onEpgGuide` build is synchronous on the UI thread, so the window is frozen behind the "Loading TV
  guide…" box while it runs; if a build exceeds ~5 s, DWM ghosts it as "Not Responding". A diag timer
  now logs `TV guide first-open: DB+build … ms, window … ms` — **check that number before doing this.**
  The cost is dominated by `programmesInWindow` + per-row `wstring` materialization (7 allocs/row), and
  the query over-fetches: it scans the whole `playlist_id` partition, not the 78 h window it then
  filters to (`common/db/Database.cpp` `programmesInWindow`) — so a **cheaper first win is bounding the
  query by `stop_utc > winStart AND start_utc < winEnd`** before threading anything. If still slow,
  move the build to a worker — but it **must use its OWN `sqlite3` connection**, NOT `st->db`: that one
  `SQLITE_OPEN_FULLMUTEX` handle would serialize/interleave a worker read with the scheduler tick's and
  a fetch's write transactions (risking `SQLITE_LOCKED`/`BUSY` or an inconsistent snapshot). Post the
  built rows back via a new `WM_APP_*` message (WM_APP+1..+5 and +10 are taken).
- **Series-rule follow-ups:** ✅ **episode dedup** and the ✅ **rule editor** landed on `main`
  (0.2.9-dev, unreleased). *Episode dedup:* `episodeKey()` combines `Programme::episodeNum` + `subTitle`
  (schema **v6** `scheduled_recordings.episode_key`) so a repeat airing of an already-queued/recorded
  episode is skipped — title-scoped, and the caller filters to recordable channels so dedup never claims an
  episode on an EPG-only channel it can't record. *Rule editor:* Settings ▸ Recording Rules… ▸
  **New…/Edit…** (or double-click a row) opens a dialog for title + **Exact/Contains** + channel (or **any
  channel**) + **lead/trail padding** in minutes; backed by `Database::updateRule` + `clearPendingForRule`.
  **Still open:** a **"skip this airing"** affordance that reads better than the manager's Cancel.
  *(The phantom-**Missed**-after-a-lead-time-edit edge case listed here is ✅ **FIXED in 0.2.12** — `975fb3b`
  + `82fdba8` persist the **unpadded** airing start as `scheduled_recordings.prog_start_utc`, **schema v7**,
  giving dedup a padding-proof identity. That was the "seed dedup from the unpadded programme window"
  option suggested here.)*
- ~~**Group-title country fallback** for Xtream feeds~~ — ✅ **SHIPPED in 0.2.12** (`641e57f`; the
  mac team's shared-core change, plus `76617f0` moving country derivation into a SQLite scalar).
- **🔍 "Glass cover" over the meters** (owner idea, 2026-07-25 — make the LED/tube/scope mini-meters +
  the fluid buffer meter look like dials behind a curved glass pane). **Researched against the real
  rendering code; feasible, ~1 day, low-medium risk.** Findings worth keeping:
  - **⚠️ SCOPE FORK — settle this first.** *Per-meter* glass (each meter gets its own pane) is the easy
    version below. **One continuous pane spanning all five meter child HWNDs** (including the gaps
    between them) is a different, much harder project: the only path is a `WS_EX_LAYERED` +
    `UpdateLayeredWindow` popup over the tray (the repo does this twice already — `Splash.cpp:173`,
    `MainWindowDock.cpp:139-144`), which has no partial-update path and must mirror the parent's
    move/size/z-order/show/minimize/fullscreen/DPI. Defer unless the owner specifically wants seamless.
  - **Approach: a shared mask helper writing into each meter's back-buffer.** New graphics-free
    **`common/ui/GlassMask.{h,cpp}`** (mirrors the `Skin.h` discipline so mac can reuse the math):
    `buildGlassMask(W, H, params, add[], mul[])` precomputes two `uint8_t` W×H LUTs, applied as a
    multiply-add over the finished frame. Geometry must be expressed as **fractions of W/H, never
    `dpx()`**. Plug-in points: `MiniMeter::onPaint` (content ends `MiniMeter.cpp:373`, blit `:374`) and
    `BufferMeter::renderLedBits` (cells end `:460`, blit `:525`).
  - **Free prerequisite win:** `MiniMeter` creates *and destroys* a `CreateCompatibleBitmap` DDB **every
    paint** (`MiniMeter.cpp:345-347`, `:375-377`) — **120 GDI object create/destroy pairs per second**
    across the 4 tray meters at 30 fps. Converting it to the cached 32bpp DIB pattern `BufferMeter`
    already uses (`BufferMeter.cpp:463-487`) is required for mask caching and is a win on its own.
  - **❌ NOT the GPU/theme-engine path.** The SkinStrip technique works *because* every destination DC is
    child-clipped — which is exactly what puts it **behind** the children; glass must be **in front**.
    Also: alpha is destroyed in three places (`D2D1_ALPHA_MODE_IGNORE` `SkinStrip.cpp:155`, shaders
    returning `1.0`, `BitBlt SRCCOPY`), there is one global strip texture that fully rebuilds on any size
    change (five meters at five widths ⇒ ~150 `CreateTexture2D`/sec), and it must still work with
    `RABBITEARS_THEME_ENGINE` **OFF**.
  - **Do the 80% version:** a static **specular streak + edge darkening**, two terms, one 0..1 strength.
    **Skip refraction entirely** — at ~26-30px tall with a 3px LED pitch the displacement is *sub-cell*
    and reads as a rendering bug. Skip the animated highlight (it fights BufferMeter's existing drifting
    specular, and there is no shared frame clock: 16ms parent vs 33ms meters).
  - **Global 0..1 toggle, NOT a per-meter knob** — three hard blockers: the **buffer meter has no
    `MeterConfig` at all** (the Data-flow row deliberately omits combo/swatches/sliders,
    `Dialogs.cpp:1804-1834`), there is **no room for a 5th slider** (`kMtrKnobs = 4` and Spectrum uses all
    four; the band already ends at x≈692 in a `dp(720)` dialog), and — **the important one** — a 6th
    `MeterTuning` field **breaks the mac side**: Win32's parser falls back per field, but
    `mac/src/app/MeterModel.cpp:94` is `if (tok.size() != 5) return fallback;`, **exact arity**.
  - **Perf is a non-issue.** The whole tray is ~13,590 px @96dpi (~0.7% of a single 1080p frame); at 30 fps
    that's ~408k px/s with a precomputed LUT — tens of µs/frame. For scale, `drawTubeGlow` already does
    ~384 antialiased ellipse fills per frame on a busy spectrum meter.
- **🔍 Glass cover — further refinement** (owner: "looks better, still needs more work", 2026-07-26).
  Shipped in 0.2.14 as `common/ui/GlassMask.{h,cpp}` — a beveled-plate model (clear centre, all the
  optics in a rim band: lit top/left fillet, shaded bottom/right), replacing a first attempt whose
  full-face gradients just read as blur. Ideas for the next pass, roughly in value order:
  (a) **a bezel/frame** around the panel — in the Phase Linear reference the glass sits inside a
  dark surround, and that frame does a lot of the work of selling "instrument"; the meters are
  currently flush to the strip; (b) a **thin bright top edge + dark bottom edge** on the panel
  itself (a 1px rim), which reads as thickness far more cheaply than any gradient; (c) revisit the
  bevel WIDTH per meter aspect — the tray meters are ~30px tall and very wide, so a rim sized off
  the short axis is proportionally huge on the long one; consider sizing X and Y independently;
  (d) a faint, very tight corner highlight where two bevels meet. **Do NOT reintroduce**: broad
  soft gradients across the face (that was the blur), refraction (sub-cell at this size), or an
  animated highlight (fights BufferMeter's own drifting specular; no shared frame clock).
- **📻 VU needle look on macOS** — Win32 0.2.14 added `MeterStyle::Vu` (analog needle gauge with
  true ~300ms symmetric ballistics). `mac/src/app/MeterModel.h` carries its own copy of the
  MeterStyle enum WITHOUT `Vu`; its parser falls back on the unknown `"vu"` token, so a shared DB is
  safe, but a mac user switching a meter Win32 set to VU silently gets LED. Mac-team change: add the
  enumerator + a Core Graphics renderer.
- **🐞 PIP resize letterboxes (black bars left/right)** — owner-reported 2026-07-26, on 0.2.14-dev.
  Resizing the PIP popup leaves black bars down one side: the window is free-form but the video
  inside keeps its own aspect ratio, so the leftover client area shows the `BLACK_BRUSH` letterbox.
  Decide the intended behaviour first — either (a) **constrain the resize to the video's aspect**
  (handle `WM_SIZING` on the PIP popup, snapping the dragged edge so w/h matches the stream's
  `libvlc_video_get_size`, which is the usual PIP convention and makes bars impossible), or
  (b) keep free-form and centre the video, accepting bars on one axis. (a) is almost certainly what
  the owner wants. NB the PIP popup uses `kVideoClass` (`addPane`, `MainWindowCommands.cpp:652`) and
  its size persists as `pip_size`, so a snap must apply on restore too, not just during the drag.
- **🐞 Leaving PIP view moves the PIP stream into the main pane** — owner-reported 2026-07-26.
  Switching PIP → Single takes whatever was playing in the PIP and plays it in the main view; it
  should leave the main view's channel alone and simply close the PIP. Look at the `ViewMode::Pip` →
  `ViewMode::Single` transition (`ID_VIEW_SINGLE`/`ID_VIEW_PIP` in `MainWindowCommands.cpp`) and how
  panes are torn down — the floating pane's channel is presumably being promoted to pane 0 rather
  than discarded.
- **PIP: right-click context menu** (owner request, 2026-07-26) — the PIP popup has no context menu
  today. Wants at least: close PIP, and the swap below. `kVideoClass`'s WndProc already handles
  right-click for the main video's view menu (see `VideoProc`), so this is mostly deciding the item
  set and reusing that path for a `floating` pane.
- **PIP ⇄ main swap** (owner request, 2026-07-26) — a deliberate "promote the PIP to the main view
  and demote the main view into the PIP" action, from the PIP right-click menu above. Note this is
  the *intentional* version of the bug two entries up: the swap should be something the user asks
  for, never a side-effect of changing view mode.
- **PIP "always on top of other apps" toggle** — ✅ **SHIPPED in 0.2.14** (`9774418`): Settings ▸ View ▸
  "Keep PIP above other apps", default ON. Off ⇒ PIP is topmost only while RabbitEars is the
  foreground app (re-raised on `WM_ACTIVATEAPP`) — it can never be left plain non-topmost, because
  an owned popup then composites UNDER the main window's libVLC D3D surface. Owner-verified.
  *(Original entry: the PIP popup is `WS_EX_TOPMOST` today — it must be, to composite over libVLC's
  D3D vout; a user toggle to drop it out of topmost when the main window isn't focused. Its resize
  grip + position/size persistence shipped in 0.2.6.)*

## Localization (i18n) follow-ups

The engine shipped in 0.2.8 (English + Japanese; JSON source in `common/i18n/`, generator
`tools/i18n/gen_i18n.py`, `common/i18n/README.md`). **Traditional Chinese (繁體中文, `zh-Hant`) added on
`main`** (machine draft, unreleased) — proved the "add a language" path end-to-end: `zh-Hant.json` +
`languages.json`/`keys.json` entries, `Tr.h` (`systemLang` now maps Chinese-Traditional sublangs,
Simplified falls back to English), the Settings ▸ Language item (`ID_LANG_ZH_HANT=2069`), the
`Microsoft JhengHei UI` CJK font in `themeFontFamily()`, and the CLI selftest generalized to check
completeness + placeholder parity across ALL shipped languages. Remaining:
- **Native Japanese review** — the current `ja.json` is a glossary-consistent machine draft. A native
  speaker should pass over it (via `gen_i18n.py --review ja`), the **Terms-of-Use** text especially
  (provisional legal wording). This is the gate before advertising JP support in a release.
- **Native Traditional Chinese review** — `zh-Hant.json` is a machine draft (glossary-consistent,
  Taiwan conventions, adversarially reviewed — one 配對→對應 fix applied; no Simplified chars, country
  names Taiwan-standard). A native Taiwan speaker should still pass over it (via
  `gen_i18n.py --review zh-Hant`), the **Terms-of-Use** especially, before advertising 繁體中文 support.
- **Live language switch** — ✅ DONE (0.2.11-dev, UNRELEASED). Settings ▸ Language now applies the
  choice **live, with no restart**: `setLanguageSelection` calls `i18n::setActiveLang(resolveLang(pref))`
  then a new `applyLanguageChange(st)` (`MainWindowCommands.cpp`) that remakes the CJK-aware chrome
  fonts (`remakeUiFonts`, hoisted OUT of the `#ifdef RABBITEARS_THEME_ENGINE` so it works flag-off),
  re-sends the search cue banner, re-renders the buffer label, rebuilds the nav tree (`refreshNav`),
  recreates the Direct2D grid + guide text formats (`channelGridUpdateDpi` + new
  `epgGuideRefreshLanguage`), refreshes the status line, and `RedrawWindow`s. The old restart
  TaskDialog + `restartApp()` are gone (the `--restart` self-relaunch plumbing in `runApp`/`WinMain`
  is kept, now unused). Built green all three configs (x64 both theme flags + native ARM64) +
  selftest ALL PASS; adversarially reviewed (3 findings refuted, 2 confirmed-cosmetic). **Two minor
  deferrals:** (1) a live switch overwrites the status line's channel-count only when idle — a
  background download/EPG-fetch banner is left intact (guarded on `busy`/`loadingDlg`), but a
  non-busy transient (Paused/Buffering/Unavailable) briefly stays in the old language until the next
  player event (the transport button glyph still indicates play/pause/record state). (2) `refreshNav`
  drops the nav sidebar's selection highlight + collapses expanded Groups/Countries — its established
  behavior at every call site (playlist add/rename/delete), so the switch just returns the sidebar to
  the app's normal no-selection default; a proper fix (restore selection by matching `st->filter`)
  would benefit all `refreshNav` callers and is a separate enhancement.
- **More languages** — drop a `<code>.json` + `languages.json` entry + wire `Tr.h`/menu; the generator
  refuses to build until every key is filled. The lookup already scales (kTables indexed by `Lang`).
- **Localize the 2 COMDLG filter strings** ("Playlists (*.m3u)…") — kept literal because of their
  embedded `\0` separators; needs a small special-cased builder, not the flat catalog.
- **macOS adoption** — the catalog is pure `common/`, so mac can consume `trU8()` (wrap with its `ns()`
  UTF-8 helper) and gain the same completeness selftest; currently Win32-only.
- **Date/time format strings** in the schedule dialog/columns are left numeric (yyyy-MM-dd) — revisit if
  locale-aware date formatting is wanted.

## Polish / cleanup

- **About box: give the tip buttons their own labelled section** (owner-requested, 2026-07-25). Today
  **Buy me a coffee** and **Ko-fi** sit in the bottom button row next to *Check for Updates* and *OK*,
  with **no explanation** — a user has no idea what they are for, and "Ko-fi" in particular means nothing
  to someone who hasn't met the brand. Group them into a visually distinct block (a separator/hairline
  above, or a small framed area) with **one short line of copy** — something like *"RabbitEars is free and
  open source. If it's useful to you, you can support development:"* — and the two buttons beneath it.
  Notes for whoever does this: (a) the copy must be a **new i18n key**, not a literal — see
  `common/i18n/README.md`, and remember `gen_i18n.py` must be re-run; (b) the box is `dp(620)×dp(470)` and
  the bottom row is already **four** buttons wide (see the measured-geometry comment in `showAbout`), so
  this most likely means **moving the two tip buttons OUT of that row** into the new section rather than
  adding more width — which also frees the row for the planned "Licenses…" button; (c) the same wording
  problem does *not* apply to the support prompt, which already explains itself. `Win32/ui/Dialogs.cpp`
  `showAbout` / `AboutProc` (the artwork + text are owner-drawn in `WM_PAINT`, so a section header can be
  drawn there rather than added as a control).
- **Shared `runModalLoop` helper** — About / prompt / Categories / Terms / info / Meters each run a
  hand-rolled modal `GetMessage` loop. 0.1.7 fixed the **About** loop to re-post `WM_QUIT` (so a quit
  mid-dialog exits cleanly); the others still swallow it (benign — the 4s `WM_DESTROY` watchdog covers
  them, and none can trigger updates). Extract one correct helper and use it everywhere.
- **Clamp the remaining themed dialogs to the work area** — About / prompt / Categories / Terms / info
  centre on the parent (`Win32/ui/Dialogs.cpp`) and can clip off-screen near a screen edge. Reuse
  `clampToWorkArea()` after each dialog's centred x/y (the Meters dialog already does, since 0.1.6).
- **DPI-change relayout** (`WM_DPICHANGED`): recreate fonts, relayout, push DPI to grid/meter
  (`channelGridUpdateDpi`, `bufferMeterSetDpi`).
- **Category filter follow-ups** — remember the *last* view when toggling it, or a per-view (not
  global) category filter.

## Release / infra

- **About-box "Licenses…" viewer** (follow-up to the 0.2.9 licensing cleanup) — the cleanup fixed the
  repo `LICENSE` (now real **GPL-3.0**, was mislabeled LGPL-2.1), relocated libVLC's LGPL-2.1 text to
  `licenses/`, added `THIRD-PARTY-NOTICES.txt`, corrected the "keeps RabbitEars proprietary" comment
  in `cmake/LibVlc.cmake`, and wired `installer.iss` to ship the notices + `licenses\*` beside
  `LICENSE.txt`. **Remaining:** an in-app viewer — add a third **"Licenses…"** button to `showAbout`
  (`Win32/ui/Dialogs.cpp`, next to Check-for-Updates/OK) opening a scrollable, read-only themed panel
  listing each bundled component (libVLC LGPL-2.1 + its GPL plugins, SQLite public-domain,
  miniz/WinSparkle/Sparkle MIT) with copyright + source links, sourced from the i18n catalog so it
  localizes. Low-risk, self-contained. **NB (0.2.12):** that button row now holds **three** buttons —
  Check-for-Updates `dp(140)` · Buy-me-a-coffee `dp(160)` · OK `dp(90)` — in a `dp(530)` client, so a
  fourth needs the About box widened or the row re-laid-out (don't just wedge it in). Also (small): vendor the WinSparkle/Sparkle upstream `LICENSE`
  files into `third_party/` for picture-perfect MIT notices, and have the **mac** DMG ship the same
  `THIRD-PARTY-NOTICES.txt` + `licenses/` (mac-team packaging).
- **Authenticode** code-signing of the exe + installer (silences SmartScreen; separate from the EdDSA
  update signature). **Recipe + where it slots into the release flow is documented in
  `docs/RELEASING.md` ("Not yet covered")** — blocked only on the owner buying/provisioning a
  code-signing cert. ⚠️ **Azure Artifact Signing (formerly Trusted Signing, ~$10/mo) is NOT available to
  individual developers in Japan** — Microsoft lists individuals in the **USA and Canada only** (orgs:
  US/CA/EU/UK). So the realistic options are **[SignPath Foundation](https://signpath.org/terms.html)**
  (free OV-level signing for OSS, and recommended by Microsoft's own code-signing doc — RabbitEars'
  GPL-3.0 + libVLC looks eligible; caveat: the cert subject reads "SignPath Foundation", not the owner's
  name) or a ~$200/yr OV cert (Sectigo/DigiCert; HSM/token required since 2023). **Do NOT pay for EV
  expecting instant SmartScreen trust — that bypass was removed in 2024.**
- **Microsoft Store listing** (researched 2026-07-25, not started). **Registration is now FREE** — the $19
  individual / $99 company fees were waived (Sept 2025 / May 2026); sign up **only** via
  `https://storedeveloper.microsoft.com` (entering through Partner Center can still show the legacy paid
  flow). Verification is government ID + selfie. **Recommended path: the EXE/MSI submission (Store Policy
  10.2.9)** — the Store downloads and runs *your* Inno installer from a versioned HTTPS URL (GitHub
  Releases), which **keeps WinSparkle**: Policy 10.2.5's "update only through the Store" explicitly
  exempts 10.2.9, and Microsoft documents updates on this path as the app's own responsibility. It also
  keeps the GPL/LGPL story clean (the user gets the byte-identical binary you published source for).
  **Do NOT use MSIX** — free Microsoft signing, but you lose WinSparkle, seal libVLC behind an
  unmodifiable package (weakening LGPL-2.1 §6's relink guarantee), and ship a re-signed binary.
  **Blocker: 10.2.9 requires Authenticode** (self-signed rejected) — see the entry above; that is the only
  hard prerequisite. Other gotchas: the policy says the installer **and every PE inside it** must be
  signed (that's the whole libVLC `plugins\` tree — though the certification page softens it to "highly
  recommended"; ask support); a **privacy policy URL is mandatory** for Win32 products (10.5.1) and we
  have none; certification will fail as "not testable" unless the **cert notes carry a public free-to-air
  M3U URL + steps through the T&C gate**; screenshots must not show real broadcast streams or
  broadcaster logos (11.2); set **"Applicable license terms" to GPL-3.0-or-later** (the default Store
  terms forbid redistribution and are GPL-incompatible — your own terms override); and check the name
  against **RabbitEars.info** (an established US OTA-TV database) before reserving it. Publishing solves
  SmartScreen *for Store installs only*. GPL apps are accepted (VLC itself ships there) and several
  M3U/IPTV players are already listed.
- **Portable-zip** artifact on releases (alongside the auto-updating installer).
- **ARM64 `plugins.dat`** — no ARM64 `vlc-cache-gen` exists upstream; would mean building the tiny
  tool against the NuGet's arm64 `libvlccore`. LOW value (native ARM64 scan is already ~3 s). The x64
  side is solved (install-time cache-gen, 0.2.6).
