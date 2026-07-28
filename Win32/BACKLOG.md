# RabbitEars — Backlog

Parked / not-yet-scheduled work, split out of `Win32/HANDOVER.md` (which stays the single entry
point for **current state**; this is the parking lot for **what's next**). Ship small items as 0.1.x
point releases; the **theme engine** is the big 0.2.x epic. Windows-team doc — kept under `Win32/`
so it doesn't collide with the macOS team's root-level edits (they own `mac/`).

---

## 🍎 Shared-core changes in v0.2.16 · **for the macOS team**, 2026-07-28

Windows shipped **v0.2.16** (Xtream VOD movies, player seek, scrub bar). Everything below the UI
landed in `common/`, so mac inherits it on the next merge. **The full change-by-change audit — every
claim checked against actual `mac/` call sites — is [`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md) §2.**

**mac needs no source change to build or run.** Two items are not cosmetic, though:

1. 🔴 **A schema-v8 migration bug that could silently empty a mac library — fixed AFTER the v0.2.16
   tag, so pick up that commit.** The five v8 `ALTER TABLE channels` statements shared one
   `hasColumn("channels","kind")` guard, and the `user_version` probe asked the same single
   question. A partial failure (a disk filling mid-sequence; `exec()` reports nothing) left `kind`
   present, so later opens skipped the other four forever while `user_version` still latched to 8 —
   and because `kChannelCols` names all twenty columns, every channel query then fails to prepare
   and `runChannelQuery` returns empty. Library gone, silently, with no retry. Now individually
   guarded, with all five columns checked.
2. ⚠️ **`Tx::commit()` changed semantics for every transaction**, mac included: a failed COMMIT now
   ROLLS BACK instead of leaving the transaction open. Safe on mac *only because* mac has one
   `Database` and marshals both background writers to the main queue — **revisit if mac ever adds a
   second connection or writes off the main queue.** `bulkInsertChannels`/`bulkInsertProgrammes` now
   return 0 rather than a positive count when their commit fails.

Everything else is behaviour-preserving on mac because mac cannot produce a `kind != 0` row
(`M3uParser` is byte-identical between the tags and never sets `kind`; mac has no `XtreamClient`
caller). The latent divergence to remember: `listGroups()` is now LIVE-only, so **if mac adopts the
VOD sync, movie categories silently vanish from the nav filter and the Categories checklist** unless
mac also builds a Movies root on `listVodGroups()`/`moviesByGroup()`/`allMovies()`.

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
  → ✅ **DONE in the 0.2.15 "framed pane" pass** — (a),(b),(d) shipped; (c) was discharged the OTHER
  way, see below. The mask is now three bands measured inward from the edge: band 0 = the theme's own
  1px `FrameRect`, left **bit-identical** at every strength on every skin (`add` is achromatic, so
  brightening it washed a brass/blue border toward neutral — that was the "border got fatter and
  hotter" failure); band 1 = an **opaque** hard-stepped bezel (lit top/left, near-black bottom/right,
  one corner pip) painted by driving `mul→0.10` so `v = v*mul/255 + add` lands on an absolute tone —
  no colour type, no renderer change, still graphics-free; band 2 = the dial carrying **only the
  bezel's cast shadow**, with `add` exactly 0 — nothing brightens content anywhere, which is the
  invariant that forecloses the blur failure for good. Geometry comes from a new `chromePx` the
  renderer passes (`meterChromePx()` = the same `dpx(2)` inset `onPaint` already reserves), so the
  frame costs **zero dial pixels at every DPI and every size**, and the dp(86) Settings preview shows
  the byte-identical edge the dp(30) tray meter will get. Touched footprint went **down** (42.4% →
  38.2% on a 112×30) because a 5-row additive ramp became a 1px line. **(c) was rejected on purpose:**
  a real chamfer is constant width, and axis-derived rims are what made a 112×30 meter read as two
  horizontal bands — the fix was removing axis-derived geometry entirely (Chebyshev ring), not
  splitting X from Y. Trim knob if it reads heavy: `kLipTop` (0.431), then `kShadowDeep` (0.30).
- **🔍 Glass cover — wire the buffer meter** — ✅ **DONE in the 0.2.16 groundwork** (`ffb69dc`), to the
  recipe below. The grid geometry was lifted into `BufferMeter.h` as an inline `bufferGrid()` so
  `--selftest` can finally reach it (the objection below was that it could not): it now pins that with
  glass OFF the grid is **bit-identical** to the pre-glass renderer at every size and DPI, and that
  with glass ON it costs exactly one row and one column (38×10 → 37×9 at 115×30), containment inside
  the bezel band verified across a size/DPI sweep. `drawMetrics` and the `ID_MTR_GLASS` preview
  invalidation were handled as prescribed. **Still needs the owner's eye** — whether four framed
  mini-meters beside a now-framed tank actually resolves the "bezel needs work" reservation is a
  visual question the sandbox cannot answer. Original recipe kept below as the record. `BufferMeter.cpp`
  has no glass code and no border at all, so it is now the only meter still literally flush to the strip —
  four framed mini-meters beside one unframed fluid tank may read as half-done. It was deferred because it
  is the only place real dial would be spent: its LED grid is genuinely edge-to-edge (at 115×30 the layout
  is `gap=1, pitch=3, cellPx=2, cols=38, rows=10, ox=1, oy=0`, so the top LED row starts at row 0 and a
  frame would swallow it whole), `--selftest` cannot reach it (`renderLedBits`/`render` are static in an
  anonymous namespace of a GUI TU), and ⚠️ **its `dpx` arguments are REVERSED** vs MiniMeter's
  (`dpx(dpi, v)` at `BufferMeter.cpp:62` vs `dpx(v, dpi)` at `MiniMeter.cpp:41`). Recipe: cache
  `glassAdd/glassMul/glassBuilt/glassChrome` on `MeterState`; reset on the `ensureDib` size path; build
  with `GlassParams{miniMeterGlass(), dpx(st->dpi, 2)}` in `render()` before `renderLedBits`; apply the
  same byte loop between `renderLedBits` and the `BitBlt` (no `GdiFlush` needed — it writes `st->bits`
  directly). **Inset the LED grid by `glassChromePx(W, H, dpx(st->dpi, 2))` and GATE that inset on
  `miniMeterGlass() > 0.0f`** — glass defaults to 0, and a permanent 38×10 → 37×9 grid for a feature
  nobody enabled is a default-path regression. `drawMetrics` draws after the blit with `tr.top = dpx(1)`,
  which would cross the frame — bump to `dpx(3)`. Also add `InvalidateRect(st->bufPreview, …)` to the
  `ID_MTR_GLASS` handler (`Dialogs.cpp`, which only invalidates `preview[0..3]`), or the buffer preview
  shows stale glass while the slider drags — and the buffer meter kills its timer when the tank drains,
  so it will not self-refresh.
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

## 🎬 Xtream VOD + Series — the 0.3.0 epic (scoped 2026-07-26 · **both gates CLOSED 2026-07-27**)

> **STATUS: both gates DONE, and MOVIES ARE CODE COMPLETE** (`ffb69dc`, `1a486be`, `538f0b2`,
> `1d59783`, `7751cdc`, `e4e01a7` — all unreleased). The design doc is
> [`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md), written off MEASURED numbers from the owner's real
> provider — read it before touching this epic.
>
> ⚠️ **The two PLANNED releases merged into one shipped 0.2.16 (owner's call, 2026-07-28).** 0.2.16
> was never tagged and never given an appcast, so no user ever had it — both halves shipped together
> as **v0.2.16**. What merged is the release plan, not the numbering: **0.2.17 is the next version,
> unused and available.** Text below (and the commit messages) that says "0.2.17" predates that call
> and means *the Xtream VOD work*.
>
> **Nothing is left to BUILD for movies** — the parser, client, schema, DAO, the Movies nav root, the
> sync worker, the Settings action and the i18n are all done, selftested and committed. What is left
> is the **owner's on-device pass**, which nothing in the dev sandbox can substitute for.
>
> **Resolved since scoping:** the perf risk called out below was measured with the new
> `RabbitEarsCli --benchdb` and FIXED — every live-TV path is now immune to VOD library size. Open
> question #2 (where VOD lives in the nav) was decided by the owner: **a single "Movies" root**, not
> ~67 sibling categories. Three reconnaissance findings changed the plan; they are inline below.

**Answering "does RabbitEars support VOD?" — no, not at all, and the gap is bigger than it looks.**
Every "Movies" string in the tree is group-title *test data*. Investigated and confirmed:

- **No seek anywhere.** `VlcPlayer` exposes zero position/duration/seek API — no `set_position`,
  `get_length`, `get_time`. The transport is Play · Stop · Fullscreen · Record with no scrub bar,
  because a live stream has nothing to scrub. **This is the single biggest prerequisite.**
- **No content model.** `Channel` has no duration, watched flag, resume position, or live-vs-VOD
  discriminator. An Xtream movie today imports as just another channel in a group called "Movies".
- **No Xtream API client.** What exists is narrow and unrelated: a VLC-style User-Agent so panels
  don't reject the fetch (`Win32/platform/Http.cpp`), credentials tolerated in the query string, and
  the country-from-group-title fallback. All of it treats an Xtream playlist as a **flat M3U of live
  channels**.
- **⚠️ NO JSON PARSER IN THE REPO.** `player_api.php` is JSON-only. House style is hand-rolled
  parsers (M3U, XMLTV) with vendoring reserved for hard things (SQLite, miniz) — so this is probably
  ~300 lines of subset parser plus `--selftest` coverage, not a dependency decision. Decide in the
  design doc.
- **Already reusable, and the biggest head start:** the channel-logo loader (off-thread,
  disk-cached, bomb-safe) is exactly the poster-art problem again. Schema migrations are incremental
  and idempotent on `PRAGMA user_version` (at **v7**), so VOD's v8 is routine.

**Estimate: ~15–22 focused days (3–5 weeks)** plus this project's usual ~25–30% for adversarial
review + both-flag verification. Breakdown: JSON subset parser 1–1.5 · Xtream client 2–3 · schema v8
+ DAO + migration tests 2–3 · player seek/position (the worker-thread atomic pattern `videoSize()`
already uses) 1 · scrub bar + time readout 1–2 · resume/watched 1 · poster grid (a Direct2D sibling
to the 862-line `ChannelGridControl`) 3–5 · series→seasons→episodes browser 2–3 · nav + i18n (~50
strings × 4 langs) 1.5.

**Biggest risk, and it is un-simulatable here:** Xtream panels are wildly non-standard — differing
shapes, missing fields, numbers-as-strings, and some disable `player_api.php` outright. Second risk:
VOD libraries run 10k–50k items, so the poster grid inherits the perf discipline the country filter
already needed (a C++-side filter benchmarked at ~30 ms/call and was pushed into a SQLite scalar).

### ✅ Both gates are closed — what reconnaissance actually found

1. ✅ **Reconnaissance — DONE 2026-07-27** via `RabbitEarsCli --xtream` against the owner's real
   provider. `player_api.php` **works** (`auth=1`, Active). **43,599 movies** (13.46 MB / 5.2 s) and
   **13,152 series** (13.33 MB / 4.7 s); 67 VOD + 39 series categories, flat. `container_extension`
   present and non-empty on **all** 43,599; the constructed play URL probed **HTTP 302 (reachable)**.
   All 8 bodies parsed cleanly. Full numbers in the design doc §1; the report itself is
   credential-redacted and re-runnable against any future provider.
2. ✅ **Design doc — DONE**, [`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md). The shared-core boundary is
   flagged to the macOS team in its §2, including the two mac-side traps (an older mac build opening
   a v8 DB; `Channel` having gained fields).

**Three findings the estimate above did not anticipate:**

- 🔴 **`max_connections: 1`.** One connection is the entire budget. A sync can never overlap
  playback, and the 0.3.0 poster fetcher gets NO concurrency. This is the dead-link checker's lesson
  (a connection cap kicks the user's actual playback) arriving early enough to design around.
- 🔴 **Movies have NO metadata.** `get_vod_info` returns 204 bytes with `info` as an empty array — no
  duration, plot, cast or year. Series *episodes* carry a full ffprobe block; movies carry nothing.
  So `durationSec` cannot be an import field: it must be cached from `VlcPlayer::lengthMs()` at play
  time, which makes the 0.2.16 seek work a harder prerequisite than assumed. And `get_vod_info` must
  NOT be called per item (43,599 requests, for nothing, against a 1-connection cap).
- 🔴 **Poster art inverts the 0.3.0 plan.** `stream_icon` is empty on ~90% of movies (39,048 of
  43,599) while series `cover` is well populated. A movie poster grid would be nine-tenths blank and
  read as broken. **Movies stay in the existing text grid; posters become a SERIES feature.**

Also confirmed: `episodes` really is a **map keyed by season number**; `direct_source` is empty and
`custom_sid` null on every movie (so the play URL must be constructed, never taken from the payload);
and types are mixed **within a single field** — `rating_5based` and `tmdb` are `string+number`,
`category_id` is `string+null`, `backdrop_path` is `array+null`. A strict parser dies on this
provider, which is exactly why `common/core/Json.h`'s scalar accessors are tolerant. **Never add a
strict mode.**

### Ship it in three, landing at 0.3.0 — not as one branch

A 3–5 week monolith contradicts how this project ships well: small, owner-verified, corrected fast.
The sandbox cannot see the GUI, so long feedback loops are unusually expensive here.

⚠️ The first two bullets **collapsed into one 0.2.16 release** (see the status block above); they are
kept as the plan of record because the shape of the work is unchanged.

- **0.2.16 — ✅ GROUNDWORK LANDED** (`ffb69dc`): player seek + scrub bar + time readout,
  `common/core/Json.{h,cpp}` (the JSON reader moved EARLIER than planned — the recon tool needed the
  shape questions answered first, and the parser is the cheapest thing to test headlessly), schema
  v8, plus the buffer-meter glass. *Pending the owner's on-device pass.*
- **✅ SHIPPED in v0.2.16 (the planned "0.2.17" half)** — the **Xtream client**, movies only, existing
  grid, no posters. `common/core/XtreamClient` (`538f0b2`) + the sync worker, Movies nav root and Settings
  action (`e4e01a7`). **Code complete and pushed; unseen on a device.**
  ⚠️ Success was written as "43,599 movies in the grid, one plays, **resume works**" — the first two
  are built, the third is NOT: resume depends on the three §6 open questions (where `durationSec` is
  cached from `lengthMs()`, the `watched` threshold, prompt-vs-silent resume) and all three are
  owner calls that are still open. **As built it is browse-and-play.** Recommendation: ship it that
  way and let resume land in 0.3.0, where it is listed anyway ("resume everywhere") — resume cannot
  be designed sensibly against a library nobody has used yet.
- **0.3.0** — series → seasons → episodes, **posters for SERIES** (not movies — see the finding
  above), resume everywhere. The minor bump marks the capability's ARRIVAL, not the start of the work.

✅ **The "biggest regression risk" above was measured and FIXED** (`1d59783`, `7751cdc`). New
`RabbitEarsCli --benchdb [movies] [live]` (defaults: the owner's real 43,599 / 442) times the queries
`loadForFilter()` runs, live-only then again with the movies present. It was real —
`listCountries()` 0.12 → **13.01 ms**, `channelsByCountry()` 0.13 → **6.38 ms**, `listGroups()` 0.11 →
**8.21 ms**, all on per-nav-click or per-keystroke paths. All three are now **at or below their
live-only baseline** (0.09 / 0.12 / 0.07 ms) and immune to VOD size, via kind-scoped queries plus a
partial index per side of the discriminator.

The bigger half was **correctness**: a movie has no country, but its category name goes through the
group-title country fallback, so a provider with `NL - FILMS` categories would have filed all 43,599
films under the Netherlands. The owner's real categories dodge it only because `VOD` is three letters
— luck, not design. Pinned by a selftest that inserts exactly that trap row.

Remaining VOD-side costs are inherent and on non-interactive paths: `moviesByGroup()` 2.38 ms,
`listVodGroups()` 9.7 ms (nav refresh), `allMovies()` **77 ms** (materializes 43,599 rows — which is
why the Movies root shows categories only, as shipped), bulk insert ~260 ms.

### 🔴🔴 THE BENCHMARK'S DEFAULT SHAPE IS NOT THE OWNER'S LIBRARY — measured on-device 2026-07-28

`--benchdb` defaults to **43,599 movies + 442 live**, straight from the design doc. The owner's real
library is **410,147 rows in one playlist** (seen in the nav during the 0.2.16 verification pass) and
**every one of them is `kind=Live`**, because no VOD sync has ever run — the provider's `m3u_plus`
lists movies AND series episodes flat, as ordinary channels. Re-measured at `--benchdb 1 410147`:

| query | owner's library TODAY |
|---|---|
| `allChannels()` | **1266 ms** |
| `channelsByPlaylist()` | **1403 ms** |
| `searchChannels()` first keystroke | **1256 ms** |
| `listGroups()` / `listCountries()` | **629 / 679 ms** |
| `channelsByGroup()` / `channelsByCountry()` | **632 / 602 ms** |

⚠️ **This is PRE-EXISTING and ships in v0.2.15 today — 0.2.16 does not cause it.** But it retires the
comfortable reading of "every live-TV path is now immune to VOD library size". That claim is true as
written (adding `kind=Movie` rows does not hurt those queries) and **irrelevant at this shape**: the
kind-scoping only engages for rows actually marked `kind=Movie`, and here nothing is. A VOD sync
would reclassify ~43,599 of the 410,147 and buy back roughly 10%; the remaining ~366k are series
episodes and live channels that stay `kind=Live`.

The real problem is **materialization**, not the scan: these queries build a `std::vector<Channel>`
of every matching row and hand it to the grid. At 410k rows that is hundreds of MB of `wstring`
churn per nav click. The fix is almost certainly a `LIMIT` + windowing on the grid, not more indexes
— and it wants its own measured investigation, because the last index-widening attempt here was a
dead end (see the note above). Until then, note that `listVodGroups()` runs on every `refreshNav`,
so the Movies root adds ~600-800 ms to a nav rebuild at this scale once VOD is synced.

### 🔴 The search box is the one per-keystroke path VOD still breaks — OWNER DECISION

**Measured 0.63 → 80.00 ms on the first keystroke** (126×), added to `--benchdb` as
`searchChannels() 1ch` in the VOD UI work (`e4e01a7`). **The 7.9 ms previously recorded above was wrong** —
not mismeasured, but measured with the term `"Channel 1"`, which matches **zero** of the benchmark's
movies (they are named `Film Title Number N`) and no VOD category. It timed the table scan and none
of the materialization. A real user's first keystroke is one letter, which matches most of the
library. `--benchdb` now prints both figures so the distinction cannot be lost again.

The mechanics: the search box's `EN_CHANGE` calls `Database::searchChannels()` **synchronously on
the UI thread**, with no debounce, no minimum length and no `LIMIT`, and the query has no `kind`
predicate. So every keystroke scans 44k rows and materializes ~40k `Channel` structs.

This is left undecided on purpose. "Movies stay searchable" is an explicit design-doc position, and
every way of fixing it changes what the user gets:

- **`LIMIT` (say 500)** — kills the materialization outright, which is where the 80 ms is. But a
  silently truncated result set is its own lie: "I searched and it wasn't there." Needs the count
  line to say it truncated.
- **Debounce (~200 ms)** — no semantic change at all, purely fewer queries. But each search still
  costs 80 ms, so it reduces the frequency of the stall, not the stall.
- **Minimum length before movies join the search** — cheap, and matches how people actually search a
  40k catalogue, but it is a second, invisible rule about what "search" means.

`LIMIT` + an honest count line is the likeliest right answer; it is a product call, not a perf one.

### Recorded, not fixed — measured while finishing the VOD work

- **`allChannels()` 0.73 → 80.4 ms and `channelsByPlaylist()` 0.59 → 80.0 ms.** Deliberate, per
  `--benchdb`'s own note: "allChannels() legitimately grows with the row count — it is the 'All'
  view." Both are per-nav-click, not per-keystroke, and the post-sync UI is careful never to land on
  All Channels. Worth revisiting only if the owner finds the All view unusable with VOD loaded.
- **UI-thread DB writes contend with the sync's two big transactions.** The sync holds
  `BEGIN IMMEDIATE` for ~43.6k inserts (~265 ms) and then again for the temp-table stage + the
  anti-join delete. WAL keeps readers clear, but a UI-thread write landing in those windows blocks —
  and past `busy_timeout=5000` it returns `SQLITE_BUSY`, which every writer here discards, i.e. a
  **silently lost write**. Measured hold times are far under 5 s so this is a freeze, not a loss, at
  the current library size. Chunking the insert would cap it. The dead-link sweep never exposed this
  because its writes are single-row.

## Tester tasks (need a real stream — the dev sandbox cannot do these)

- **📊 Does `bufferedBytes` actually report on IPTV? — 10 minutes, decides a feature**
  (owner idea, 2026-07-26). The data-flow tank's level is currently a STATE indicator, not a buffer
  gauge: it starts empty, fills through `Opening`(15) → `Buffering`(real %) → `Playing`(100), then
  **pins at 100 and is never updated again** for the rest of the stream. Only `flow` and `trouble`
  are fed periodically (`MainWindow.cpp`, the `PlayerEvent::Stats` case). Making the level *breathe*
  with how much data is actually buffered ahead would be a strictly better meter, and the signal is
  already sampled — `FlowStats::bufferedBytes` ("read minus demux"), delivered on that same event
  and currently unused.
  **The doubt, and why this is a test rather than a task:** the comment at `MainWindow.cpp` (the
  Stats case) warns that libVLC's input-byte counter **stays 0 for HLS/adaptive**, which is most
  IPTV. `bufferedBytes` derives from `readBytesPerSec`, so it may be flat zero on exactly the
  streams RabbitEars plays — in which case feeding it to the level would leave the tank sitting
  empty while a channel plays perfectly, which is far worse than today.
  **How to run it — no code change needed, this is already logged:** Settings ▸ System… → log level
  **Trace** → OK (takes effect immediately). Play a channel ~60s; if possible interrupt the network
  briefly so the trace contains a stall. Settings ▸ System… → **Open log folder**. The 250ms flow
  snapshot (`VlcPlayer.cpp`, gated on `diag::enabled(Trace)`) prints
  `pane N flow: demux … read … fps … buffered … B  lost … corrupt … disc …`.
  **What decides it:** `read` non-zero and `buffered` *moving* ⇒ the signal is real, build it.
  `read 0.00 Mb/s` + `buffered 0 B` ⇒ dead end, close this item. Set the level back to **Info**
  afterwards — Trace is deliberately the loudest thing in the app (~4 lines/sec per playing pane).
  **If it lives**, note `buffered` is a CUMULATIVE difference, not an instantaneous fullness, so
  turning it into a level needs a reference for "full" — the configured network-caching depth
  (already a user setting, on the transport slider) is the obvious candidate.

## Polish / cleanup

- **🔍 Glass bezel — pass 4** (owner, 2026-07-26, on the shipped 0.2.15 build: *"needs work — but ok
  for this release"*). The framed-pane model (`5f447b7`) is a keeper structurally — band 0 is the
  theme's own border left bit-identical, band 1 an opaque hard-stepped bezel in the chrome gutter,
  band 2 the dial with only a cast shadow and `add` exactly 0 — so **do not go back to gradients on
  the face**; that was the blur, twice. What is not yet right is unspecified, so start by pinning it
  down. The design panel's own "cannot verify headlessly" list is the shortlist of suspects, in order:
  (a) **three luminance steps inside 2px** — border 48 / lip 112 / shadow 15 on consecutive rows may
  be reading as banding rather than as a frame; (b) the **light skin**, where a near-black 1px ring
  sits inside a 214 border around a white face — much the biggest swing of any skin; (c) the **corner
  pip** (130 vs 112, a single pixel), the design's only decorative term and the most likely to read as
  a stuck sub-pixel — `kLipCorner = 0.0f` deletes it and changes nothing else; (d) whether four framed
  mini-meters beside the *unframed* buffer tank reads as half-done (see the separate "wire the buffer
  meter" entry — that is the likeliest single answer). Knobs, in the order to reach for them:
  `kLipTop` (0.431) carries the whole look; then `kShadowDeep` (0.30) if the *shadow* rather than the
  lip reads heavy. Do NOT reach for `kBodyMul` — that changes what the frame *is*. All in
  `common/ui/GlassMask.cpp`, all covered by the 27 selftest assertions, which will tell you loudly if
  a change breaks the band-0-untouched invariant.

- **Meters dialog: the "Bg" swatch means something different on a VU, and nothing says so**
  (flagged while landing the VU lamp, 0.2.15). On every cell look `palette.bg` is the panel
  background; on the **Vu** look it is the **LAMP** behind the dial (hue only — brightness is the
  model's business, and `CLR_INVALID`/any too-dark colour = the stock warm bulb). That is documented
  in `MiniMeter.h` and `drawVu`, but the dialog still just says **Bg**, so the one control that turns
  the meter electric blue is undiscoverable. Fix = per-style swatch labels (`kRoles[]` in
  `Dialogs.cpp` is currently a flat 7-entry array built once), which also means a new i18n key and
  re-labelling on `CBN_SELCHANGE` — the same machinery `meterSyncKnobs` already does for the
  sliders, so it is a natural follow-on rather than new plumbing. NB the roles that are simply
  UNUSED by a look are a different problem: `off/low/mid/high` do nothing on a VU and could be
  hidden the way the dead knobs now are.

- **About box: the arch label should read "x86-64", not "x64"** (owner, 2026-07-26). `AboutArchX64`
  in `common/i18n/en.json` renders the running-architecture suffix in the About box's version line
  (`runningArchLabel()`, `Win32/ui/Dialogs.cpp`). "x86-64" is the correct name for the ISA; "x64" is
  Microsoft's shorthand. ⚠️ **Change ONLY the display string.** `x64` is also the arch *token* baked
  into installer filenames (`RabbitEars-<ver>-setup.exe` vs `-arm64-setup.exe`), the appcast
  enclosures, and `scripts\build.cmd` / `build-installer.cmd` — renaming any of those breaks
  auto-update for every existing x64 install. The three sibling ids (`AboutArchArm64`,
  `AboutArchX86`, `AboutArchUnknown`) are worth a consistency glance at the same time; note
  `AboutArchX86` means genuine 32-bit x86, so it must NOT also become "x86-64". One-line change ×
  4 languages (the token is identical in all of them), then `gen_i18n.py`.

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
