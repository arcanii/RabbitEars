# RabbitEars — Handover

A native **Windows (Win32 / C++20)** IPTV player built on **libVLC**, themed to
match its two sibling apps, **`G:\SQLTerminal-Win32`** and **`G:\ManorLords-SGE`**
(dark "Claude-desktop-style" look, coral accent `#D97757`, custom title bar,
CMake + Ninja + MSVC, dependencies vendored / NuGet-provisioned with **no Visual
Studio project**). This is the single starting point for anyone (human or agent)
continuing the work — read it before touching code.

> **Location:** this is the **Windows team's** handover, kept under **`Win32/`** (with its
> companion `Win32/BACKLOG.md` and design docs in `Win32/docs/`) so it doesn't collide with the
> macOS team's edits on shared root-level files — they own **`mac/`** (`mac/README.md`) and share
> `common/` + root `docs/`. Moved here from the repo root in the 0.2.x theme-engine stream.

## Stack decision (important)

The design doc (`IPTV Player Application Design.docx`) lists a "WinUI 3 / EF Core"
table. That is a boilerplate artifact and is **overridden** by the explicit
direction to leverage the two C++ reference apps' look. RabbitEars is therefore a
**custom-drawn native Win32 / C++20 app** (GDI + Direct2D), exactly like the
siblings — *not* WinUI 3, *not* .NET/EF Core. Storage is SQLite via the C API.

| Component     | Choice                                                        |
|---------------|---------------------------------------------------------------|
| Language      | C++20, Windows SDK                                             |
| UI            | Custom Win32 chrome + Direct2D/GDI owner-draw (shared Theme.h) |
| Media engine  | libVLC 3.0.23 (VideoLAN.LibVLC.Windows NuGet, provisioned)     |
| Storage       | SQLite (vendored amalgamation, C API)                         |
| M3U parsing   | Custom parser (`src/core/M3uParser`)                          |
| Build         | CMake + Ninja + MSVC (VS 2026 Community), deps vendored        |
| Installer     | Inno Setup 6 (`packaging/installer.iss`)                       |
| Auto-update   | WinSparkle, EdDSA-signed appcast on GitHub (LIVE as of 0.1.1) |

## Current state — **v0.2.10 SHIPPED (Chinese language-selection hotfix)** · v0.2.9 · macOS 0.2.7

**Released:** **`v0.2.10`** (2026-07-13), tag `v0.2.10` @ `3da1096`, full version **`0.2.10.253`** (the
installers were built at commit `7c3727e`/count 253; the tag sits a couple of doc commits later — the
usual cosmetic drift; installers + version.h + appcast all agree on 0.2.10.253, cleanly `> 0.2.9.243`).
Three signed installers on GitHub release `v0.2.10` (x64 / native ARM64 / universal), **two appcasts**
(`0.2.10.253`) committed @ `1e1b2e4` and LIVE (raw feeds serve 0.2.10.253; all three enclosures HTTP 200).
Owner-signed on the Mac. **0.2.10 HOTFIX.** Fixes the two Traditional
Chinese / Hong Kong **language-selection** bugs an owner runtime pass caught in shipped 0.2.9 — both
GUI-wiring, so invisible to the headless selftest + the i18n review (they only exercise the catalog, not
the Win32 language plumbing). **(1)** The `WM_CREATE` startup loader whitelisted `ui_language` to only
`system`/`en`/`ja`, so a persisted `zh-Hant`/`zh-HK` was silently dropped and the app reset to the system
language (Chinese "restarted but did nothing"). *Fix:* **removed the whitelist — `resolveLang()` is the
single validator** now (`Win32/ui/MainWindow.cpp`; retroactive — an already-persisted Chinese choice now
applies without re-selecting). **(2)** `ID_LANG_ZH_HK` was `2062`, the LAST id of the **computed**
`ID_DOCK_BASE` range (`2051 + panel*4 + side` → 2051..2062), so picking 繁體中文（香港） fired a dock command
("move channels to bottom") instead of switching language. *Fix:* moved to **`2049`**, a genuine single-id
gap (`Win32/ui/MainWindowInternal.h`). **⚠️ NEW GOTCHA — computed command-id ranges** (`ID_DOCK_BASE`
2051..2062, `ID_LAYOUT_APPLY_BASE` 2079..2088, `ID_LAYOUT_DELETE_BASE` 2089..2098, `ID_THEME_SKIN_BASE`
2100+) do NOT appear as literal `= 20xx` assignments, so a plain grep misses them — check every `*_BASE + n`
range before allocating a command id. Version bumped 0.2.9 → **0.2.10** (commit `7c3727e`, full
**`0.2.10.253`**); both arches built (x64 BOTH theme flags + native ARM64) + selftest ALL PASS; **3
installers built** — x64 `35,309,768` / arm64 `30,167,080` / universal `63,171,660` bytes. **Owner-verified
on-device: zh-Hant + zh-HK now apply.** The `main` rebase also pulled the **mac team's localization merge**:
the shared i18n catalog is now **531 keys × 4 languages** (EN / 日本語 / 繁體中文 / 繁體中文（香港）) — it builds +
passes placeholder parity on the Windows side, so it ships along. **Owner runtime pass: ✅ zh-Hant + zh-HK
now apply on-device**, and auto-update `0.2.9 → 0.2.10` is live for both arches.

**Released:** **`v0.2.9`** (2026-07-12), tag `v0.2.9` @ `75f8d16`, full version **`0.2.9.243`** (tag count ==
shipped build number), three signed installers on GitHub release `v0.2.9` (x64 / native ARM64 / universal),
**two appcasts** (`0.2.9.243`) committed @ `fb9b030` and LIVE (raw feeds serve 0.2.9.243; all three enclosures
HTTP 200). Owner-signed on the Mac (`sign_update --account SQLTerminal`). **Headline features:**
(1) **Localization → Traditional Chinese** — ships **繁體中文 (`zh-Hant`)** + **繁體中文（香港） (`zh-HK`)** on top of
EN + 日本語. A new **base/overlay** mechanism in `gen_i18n.py` lets a *variant* locale carry only its DELTAS
(`languages.json` gets `"base": "zh-Hant"`; `zh-HK.json` is just the HK-vocab deltas) — the generator merges
base+overlay before validation, so completeness/parity see the effective catalog and the table stays a full
per-language row (no runtime fallback). `Tr.h` routes CJK sublangs (Taiwan→`zh-Hant`, Hong Kong/Macau→`zh-HK`);
`themeFontFamily()` uses **Microsoft JhengHei UI** for both. (2) **Series-rule episode dedup** — schema **v6**
`scheduled_recordings.episode_key`; `episodeKey()` = folded `<episode-num>`**+**`<sub-title>` combined (a blank /
partial xmltv_ns num alone would collapse a whole season onto one key), title-scoped, and the caller
(`expandRecordingRules`) pre-filters programmes to **recordable** (enabled-library) channels so dedup never claims
an episode on an EPG-only channel it can't record. (3) **Recording rule editor** — Settings ▸ Recording Rules… ▸
**New… / Edit…** (or double-click a row): title + **Exact/Contains** + a channel or **(any channel)** +
**lead/trail padding**; `ruleDialog` in `Dialogs.cpp`, backed by new `Database::updateRule` +
`clearPendingForRule` (an edit clears the rule's now-stale Pending predictions, then re-expands). Plus a **license
cleanup**: the repo `LICENSE` was mislabeled LGPL-2.1 (it's the whole project's **GPL-3.0**) — fixed, `licenses/`
+ `THIRD-PARTY-NOTICES.txt` added, and the installer now ships them. Built green (x64 BOTH theme flags + native
ARM64 + selftest ALL PASS). **Adversarially reviewed — the DVR pass caught 4 confirmed bugs incl. a HIGH
ship-blocker:** the migration early-return `if (v >= 5) return` never got bumped for v6, so it would have skipped
the `episode_key` ALTER on every existing 0.2.7/0.2.8 DB (`user_version=5`) → every scheduled-recording query
fails on the missing column, schedules vanish, none can be added; fixed to `>= 6`, and a **v5→v6 migration
selftest** now guards it (the v2-based migration test can't — `2 < 5` never early-returns). **⚠️ Traditional
Chinese + Hong Kong (like Japanese) are machine drafts** pending a native pass
(`gen_i18n.py --review zh-Hant` / `zh-HK` / `ja`) — the gate before *advertising* CJK support. **Deferred
(LOW, in `BACKLOG.md`):** editing a rule's lead time while one of its airings is actively Recording can leave a
phantom Missed row. **Gotcha:** Windows Defender silently quarantined the freshly-built **unsigned** installers
(rebuilt byte-identical to re-ship + upload in one shot) — a concrete case for the **Authenticode** backlog item.

**Released:** **`v0.2.8`** (2026-07-11), tag `v0.2.8` @ `e45adb6`, full version **`0.2.8.217`** (tag count ==
shipped build number), three signed installers on GitHub release `v0.2.8` (x64 / native ARM64 / universal),
**two appcasts** (`0.2.8.217`) committed @ `5404004` and LIVE (raw feeds serve 0.2.8.217; all three enclosures
HTTP 200). Owner-signed on the Mac (`sign_update --account SQLTerminal`). **Three headline features:**
(1) **Localization** — the app is fully localizable and ships **English + 日本語**, defaulting to the system
language with a Settings ▸ Language toggle (gear-icon menu, regrouped; Yu Gothic UI for CJK; restart-to-apply
via a themed prompt). The source of truth is JSON under `common/i18n/` → `tools/i18n/gen_i18n.py` generates
`common/core/Strings.{h,cpp}` (**never hand-edit them**); CI `--check` + selftest guard drift. (2) **Wake-timer
preflight** — warns when the power plan won't wake the PC for a recording, + "Run wake task now". (3) **TV Guide
loading box.** Built green (x64 BOTH theme flags + native ARM64 + selftest); each feature adversarially reviewed
(the localization pass alone fixed 3 confirmed bugs). **Owner pass:** ✅ language shift confirmed on-device;
the wake-preflight banner + guide loading box are **low-risk, delegated to testers** (owner's call).
**⚠️ Japanese is a machine draft** pending a native review (`gen_i18n.py --review ja`) — the gate before
*advertising* JP support; it ships now as an initial translation.

**Released:** **`v0.2.7`** (2026-07-10), tag `v0.2.7` @ `cc9f9e4`, full version **`0.2.7.184`** (tag count ==
shipped build number), three signed installers on GitHub release `v0.2.7` (x64 / native ARM64 / universal),
**two appcasts** (`0.2.7.184`) committed @ `438c83d` and LIVE (raw feeds serve 0.2.7.184; all three enclosures
HTTP 200). **Recording Phase 3** — the app is now a real DVR: a Windows scheduled task **wakes the PC and
records with RabbitEars closed**, sleep is suppressed *during* a recording, and **EPG-driven series rules**
("Record series" in the guide) queue every future airing. Built green (x64 BOTH theme flags + selftest ALL
PASS + native ARM64); adversarially reviewed — **2 confirmed bugs + 2 self-caught, all fixed pre-ship** (see
the 0.2.7 block under "Immediate next steps" — especially the two invariants: *never re-add an empty-queue
early return to `onSchedulerTick`*, and *tombstones are load-bearing* because `expandRules` dedups against
rows of ANY status).

**Owner runtime pass — PARTIAL (2026-07-10).** ✅ **Scheduled recording works** (owner-confirmed). ⚠️ The
**wake-from-sleep** leg is **unverified and unverifiable on this dev box**: it is a **Parallels ARM64 VM**
(`powercfg /a` ⇒ only `Standby (S0 Low Power Idle)`, hibernate off), whose standby is virtualised and whose
host Mac suspends the whole guest — no guest RTC timer can fire. **That test needs real hardware.** Owner
judged it non-critical for now. Still unexercised: "Record series" from the guide, the Recording Rules…
manager, and that an auto-update to 0.2.7 doesn't break a queued recording (the Terms-deferral path).

**0.2.8-dev (uncommitted→committed, UNRELEASED): the wake-timer preflight** — the investigation above found a
real, silent DVR bug (Windows won't ARM the RTC timer when the power plan says so, **per power source**), so
the app now detects and says it instead of failing at 3am. See the 0.2.8-dev block under "Immediate next steps".

**Released:** **`v0.2.6`** (2026-07-10), tag `v0.2.6` @ `4382395`, full version **`0.2.6.180`** (tag count ==
shipped build number this time — `main` was level with origin, no rebase drift), three signed installers on
GitHub release `v0.2.6` (x64 / native ARM64 / universal), **two appcasts** (`0.2.6.180`) committed @ `b2b7180`
and LIVE (raw feeds serve 0.2.6.180; all three enclosures HTTP 200). The **owner-directed "do all of these"**
batch: concurrent per-pane recording, MP4, PIP resize + view-mode/PIP persistence, TV-Guide "Show in Guide",
resume-last-channel, named saved layouts, import/export favourites, and the x64 install-time `plugins.dat` fix
— all built green (x64 BOTH theme flags + selftest ALL PASS + native ARM64), adversarially reviewed (3 confirmed
recording regressions + 1 export nit fixed pre-ship). See the 0.2.6 block under "Immediate next steps" for the
full feature map. **✅ Auto-update `0.2.5 → 0.2.6` CONFIRMED IN THE WILD on the native ARM64 build** (the one the
universal installer laid down) — so the whole per-arch chain is proven end-to-end: `appcast-arm64.xml` → ARM64
WinSparkle → the arm64 installer. **Owner runtime pass still wanted** for the 0.2.6 features (sandbox can't
launch the GUI): record in two panes at once + switch panes mid-record; MP4 plays back; PIP resize/position/mode
survive a restart; resume-last; Show-in-Guide; saved layouts; favourites export→import; and the faster x64 cold
start from the installed cache.

**Released:** **`v0.2.5`** (2026-07-09), tag `v0.2.5` @ `fbebcc7`, full version **`0.2.5.168`**, signed **three**
installers — **`RabbitEars-0.2.5-setup.exe`** (x64), **`RabbitEars-0.2.5-arm64-setup.exe`** (native ARM64), and
**`RabbitEars-0.2.5-universal-setup.exe`** (bundles both, installs the native arch) — on GitHub release `v0.2.5`;
**two appcasts** (`appcast.xml` x64 + `appcast-arm64.xml` ARM64, both `0.2.5.168`) committed @ `5c7073e` and LIVE
(raw feeds serve 0.2.5.168; all three enclosures return HTTP 200). **The 0.2.5 headline = the NATIVE ARM64 build**
(owner-measured ~4× faster than emulated x64) with per-arch auto-update, a universal installer, an About-box arch
readout, and an ARM64 "which build?" update chooser — see "Immediate next steps". **Build-number note:** shipped as
`0.2.5.168` (the commit count when built + signed); the tag commit `fbebcc7` is count 175 after rebasing onto the
mac team's concurrent v0.1.10 pushes — cosmetic drift (installer + appcast + the app's self-report all agree on
`0.2.5.168`, cleanly `> 0.2.4.166`), the same concurrent-push behaviour the handover has long noted. **✅ Universal
installer owner-verified in the wild** — running `RabbitEars-0.2.5-universal-setup.exe` on the ARM device installed
the **native ARM64** build and it runs (confirms the install-time `ProcessorArchitecture = paArm64` Check), and the
**About-box arch line reads `ARM64`** (owner-confirmed on-device). **Still to runtime-verify:** the Check-for-Updates
chooser, and a live 0.2.4→0.2.5
auto-update.

**Released:** **`v0.2.4`** (2026-07-09), tag `v0.2.4` @ `f30fcc2`, full version `0.2.4.166`, signed
**`RabbitEars-0.2.4-setup.exe`** (appcast @ `6e2b4ae`) — the **per-pane vout-host pool** that ends the "VLC
(Direct3D11 output)" popout on rapid channel-surf (libVLC renders into a proven-free host child window, never
the reused pane HWND; hosts freed by the reaper done-flag, grown deadlock-safe via SendMessageTimeout), plus
**TV Guide reopen + instant reopen** (`NM_CLICK` + `revealEpgGuide`) and **"Video only" from fullscreen** now
dropping into windowed video-only. Owner-surf-verified; auto-updates from 0.2.3. Prior:
**`v0.2.3`** (2026-07-08), tag `v0.2.3` @ `d6ad80a`, full version `0.2.3.162`, signed
**`RabbitEars-0.2.3-setup.exe`** (appcast @ `ca7b682`) — **the multi-view fix batch**: **track-based per-pane
audio** (only the active tile is audible, via `libvlc_audio_set_track(mp,-1)` — holds through HLS quality
switches where a volume mute leaked), the **mode-switch AppHang fix** (async pane + recorder teardown),
**video-only/fullscreen 2×2 grid + clickable tile focus**, **single-collapse keeps the selected stream**, and
the active-pane highlight. Owner-runtime-verified; **auto-update `0.2.2 → 0.2.3` confirmed in the wild.** Prior:
**`v0.2.2`** (2026-07-07), tag `v0.2.2` @ `059b632`, full version `0.2.2.153`, signed
**`RabbitEars-0.2.2-setup.exe`** (appcast @ `fcdac10`) — **the EPG `@feed` tvg-id matching fix** (iptv-org
`CNN.us@SD` now matches XMLTV's `CNN.us`, so large guides populate — owner runtime-verified the TV Guide
loads channels), **the artist's clockwork app icon + splash** (`art/clockwork_icon3.png`, trimmed to the
rounded tile — the earlier `clockwork_icon`/`_icon2` inputs were deleted), **About artwork +25% with a
clickable GitHub link** (owner-confirmed), and an **empty-PIP highlight** (accent frame + hint until a
channel loads). **First release cut entirely on the owner's machine** — `gh` CLI + Inno Setup are BOTH
present here now (not the old sandbox); only the EdDSA signing still happens on the Mac. Now on **0.2.3 dev**.
Prior: **`v0.2.1`** (2026-07-06), tag `v0.2.1` @ `79ab12c`, full version `0.2.1.148`, signed
**`RabbitEars-0.2.1-setup.exe`** (appcast @ `a361b99`) — **EPG/TV-Guide + Scheduled Recordings + the
multi-view (Split 2×2 / floating PIP) engine + the clockwork icon** (see the sections below); owner
runtime-verified Split + PIP live, and **auto-update `0.2.0 → 0.2.1` confirmed in the wild** (About ▸ Check
for Updates). The GitHub release was created via the **REST API** (no `gh` CLI in the
sandbox — a cached git credential authenticated it); the appcast points 0.1.1–0.2.0 users at it. Prior:
**`v0.2.0`** (2026-07-04), tag `v0.2.0` @ `343aa0e`, full version `0.2.0.107`, signed
**`RabbitEars-0.2.0-setup.exe`** (appcast @ `7b3946a`) — **the theme engine** (see the section below);
**auto-update `0.1.7 → 0.2.0` verified in the wild.** Prior: `v0.1.7` (2026-07-03), tag `v0.1.7` @
`de8c571`, signed **`RabbitEars-0.1.7-setup.exe`** (full `0.1.7.52`, appcast @ `12be931`). Earlier: `v0.1.6` (`5d06958`,
`0.1.6.37`), `v0.1.5` (`ca945d1`, `0.1.5.29`), `v0.1.4` (`8622e8a`, `0.1.4.26`), `v0.1.3`
(`ebd71a8`, `0.1.3.22`), `v0.1.2` (`8c99254`, `0.1.2.19`), `v0.1.1` (auto-update baseline),
`v0.1.0` (portable zip). 0.1.1–0.1.6 users get 0.1.7 automatically.

**Repo restructured (macOS Phase-2 — LANDED):** the tree is now **`common/`** (portable core —
M3uParser, Database, DockLayout, models, platform *contracts*), **`Win32/`** (the Windows app, incl.
`platform/Paths.cpp` + `Http.cpp`), and **`mac/`**, under a unified root `CMakeLists.txt` (`common`
→ `Win32`/`mac` per-OS; marketing version now in `cmake/AppVersion.cmake`, shared by both). **The
Windows exe/DLLs/plugins now build to `build\Win32\`** (not `build\`) — `installer.iss` +
`build-installer.cmd` were fixed to match (0.1.7). The macOS team is moving fast on `main`: Phase-1
(playback + native channel grid + Sparkle + CI `.app` build) is in progress.

### 🔲 Multi-view (Split 2×2 / floating PIP) + TV Guide overhaul (0.2.1 — **shipped**)

The session after the EPG/recordings merge added the **multi-player engine** (the roadmap keystone) and
overhauled the TV Guide — all shipped in **0.2.1** (commits `27fac06` engine+guide, `79ab12c` floating PIP):

- **Multi-player engine.** `Win32/ui/VlcEngine` owns the ONE shared libVLC instance (a single `libvlc_new`);
  `VlcPlayer::init(engine)` borrows it, so N players are cheap. A **`VideoPane`** = its own video HWND +
  `VlcPlayer` + channel, held in an `AppState` vector with an `active` index + a `ViewMode`. Players tag
  events with their pane index (HIWORD of `wParam`) so only the active pane drives the transport/meters. The
  pane geometry is a pure, headless-tested **`common/ui/VideoGrid`** (shared with mac; new `--selftest` cases).
- **Split (2×2)** — child-window tiles; click one to make it active (audio + channel selection + transport
  follow it, accent border). **Settings ▸ View** and the video right-click menu switch modes.
- **Picture-in-picture** — a **top-level `WS_EX_TOPMOST` popup OWNED by the main window**, NOT a child: a
  child sibling composites UNDER the big pane's libVLC D3D surface and is invisible (this bit us first — the
  fix was topmost + top-level). `positionFloatingPip()` places it in screen coords and tracks the main window
  on move/resize; **drag it** to reposition (kept in `pipPos`); **right-click a channel ▸ Play in PIP** pushes
  it to the corner muted (`playChannelInPane`, via `ChannelGridCallbacks::onContextMenu`). Owner-verified live.
- **TV Guide overhaul.** A **📺 TV Guide** sidebar node (`ViewKind::Guide`) opens the guide; right-click it
  for Refresh / Set Guide URL. **Per-playlist custom EPG-URL override** (`Database::setPlaylistEpgUrl`;
  right-click a playlist ▸ Set Guide URL…). The guide shows **only channels present in a playlist** (every
  row is playable). **Type-to-search** filters channels (a highlighted corner field). Clicking **Play**
  surfaces the viewer + **hides the guide** (`hideEpgGuide`; it played hidden behind the big guide window
  before). A **modeless "Loading TV guide…" box** shows live download/parse progress (the fetch was silent).

Build-verified BOTH theme flags; nothing here is theme-gated. **Carry-forward gotcha:** a floating/overlay
window over libVLC playback must be a **top-level** window (child siblings lose to the D3D vout — same root
cause as the 0.1.3 grip-occlusion note).

### 📺 EPG + ⏺ Scheduled Recordings (0.2.1 — **shipped**; merged @ `85c7ec6`)

The 0.2.1 feature pair, **merged to `main` @ `85c7ec6`** (the `epg-xmltv` branch — 11 commits + the merge —
is deleted; it branched off `main` @ `bc74015`). All new **core** lands in `common/` and is
**headless-tested** via `RabbitEarsCli --selftest` (42 assertions incl. gzip, XMLTV parse, the v2→v4
migration, and the pure scheduler); the **GUI is build-verified BOTH theme flags but NOT runtime-verified**
(sandbox can't launch it) — the owner's runtime pass, and the `mac-core` CI check on `main` for the
`common/` additions, are still to confirm (the direct merge skipped the pre-merge CI gate).

- **EPG (XMLTV):** vendored **miniz** (`third_party/miniz`, a `miniz` static lib in the root CMake) +
  `common/core/Gzip` gunzip `.xml.gz` (WinHTTP/NSURLSession only auto-decompress *transfer*-encoded gzip);
  a hand-rolled `common/core/XmltvParser` (+ `common/models/Programme`) mirroring `M3uParser`; **schema
  v3** (`playlists.epg_url` + a playlist-scoped `epg_programmes` table, `ON DELETE CASCADE`) via the
  `migrate()` step-wise pattern + DAO (`bulkInsertProgrammes`, `nowNext`, `programmesInWindow`). The M3U
  `x-tvg-url` is now **persisted** (was parsed then dropped).
  - `Settings ▸ Refresh Guide…` — off-thread fetch → gunzip → parse → store (mirrors `startPlaylistWorker`).
  - `Settings ▸ TV Guide` — a **new modeless Direct2D control** `Win32/ui/EpgGuideControl` (channels×time,
    frozen channel column + hour axis, "now" line, 2-D scroll; borrows `ChannelGridControl`'s D2D scaffolding).
    Click an entry → a **Play channel / Schedule… / Close** popup (`programmeDialog`, `Dialogs.cpp`).
  - CLI: `--epg <url|file>` (fetch → gunzip → parse → summary) + `--tvgids [epg]` (per-playlist tvg-id ↔ EPG
    match report: exact / case-insensitive / `@`-stripped). **EPG matching caveat (important):** the guide
    joins programmes to channels by tvg-id. iptv-org's own `x-tvg-url` is a tiny stub, but the real gotcha is
    that iptv-org tvg-ids carry an **`@feed` quality suffix** (`CNN.us@SD`) while XMLTV feeds key on the base
    (`CNN.us`) — so 0.2.x (post-0.2.1 fix, see "Immediate next steps") matches on the base id, `@…` stripped +
    case-folded; a playlist with **no tvg-ids** (`uslg.m3u`) can never match. The **custom EPG-URL override**
    (once a follow-up) shipped in 0.2.1.
- **Scheduled recordings:** **schema v4** (`scheduled_recordings` — self-contained rows: stream URL/UA/
  referrer captured at schedule time, standalone/not playlist-scoped) + DAO + `channelByTvgId`; a **pure,
  unit-tested `common/core/RecordingScheduler::planScheduler(schedules, now, busy)`** decision core applied
  by a ~30s `kSchedulerTimer=0xA2` tick in `MainWindow` (this **ungated the theme-gated `WM_TIMER`** — keep
  the scheduler case outside the `#ifdef`). `AppState::activeScheduleId` gives the single shared `rec_`
  recorder explicit ownership so the manual Record button + the scheduler never stomp; a one-time startup
  reconcile resets stale `Recording` rows. `Settings ▸ Scheduled Recordings…` = a manager (list +
  New/Cancel/Delete); the *New…* `scheduleDialog` is a type-ahead channel combo + start/stop
  DateTimePickers (needs `ICC_DATE_CLASSES`, added to `InitCommonControlsEx`) — also for no-EPG channels.
  **v1 limits:** one recording at a time; **app-must-be-running** (Task-Scheduler wake is a later phase);
  concurrent recording ⇒ the multi-player roadmap.
- **App icon → clockwork** (`packaging/app.ico` from `art/clockwork_icon3.png` as of 0.2.2 —
  `scripts/make_ico.py` reads it; **Pillow is now installed**, so `python scripts/make_ico.py` works
  directly) + README badge; two more studies (`happy`/`style`) checked in.
  Marketing **version bumped to 0.2.1** in the 4 places (AppVersion.cmake / installer.iss / RabbitEars.rc /
  app.manifest); mac keeps its 0.1.9 `APPLE` override.
- Both big UI surfaces (the guide control + the schedule dialogs) passed an **adversarial review**; fixes
  applied — notably a `scheduleDialog` OK-path **read-after-destroy** (it read its controls after IDOK
  destroyed the window; now captured in the Proc) and the **WM_QUIT-under-modal** use-after-free (the new
  modal loops + `showInfoDialog` now `DestroyWindow` + re-post the quit). Run the same adversarial pass on
  any new Win32/D2D UI — it keeps catching real bugs.

### 🎨 Theme engine (0.2.x epic) — SHIPPED in v0.2.0 (merged to `main`; theme-ON by default)

The theme engine **shipped in v0.2.0** — the `theme-engine` branch was **merged to `main` + deleted**
(2026-07-04, PR #16 superseded), and the CMake flag **`RABBITEARS_THEME_ENGINE` now defaults ON**, so a
standard build ships the theme chrome (the flag stays an option — `-DRABBITEARS_THEME_ENGINE=OFF` still
builds the parity path). ⚠️ **Build-dir CACHE trap:** a plain rebuild reuses the *cached* flag regardless
of the default — a stale theme-OFF cache shipped a Theme-menu-less exe during the 0.2.0 live pass. **Any
release/verify build must pass `-DRABBITEARS_THEME_ENGINE=ON` explicitly.** Design docs:
**`Win32/docs/THEME_ENGINE.md`** (§6 = the D3D11/HLSL renderer) + the shared skin-model contract
**[`docs/SKIN_MODEL.md`](../docs/SKIN_MODEL.md)** (for the mac team). Every part was adversarially reviewed
+ build-verified both flags. **Owner-verified live in 0.2.0:** all four skins (Dark / Light / Cyberpunk /
Steampunk), the strip / gutter / button glow + Steampunk heat-haze, Video-only, and the DPI first-resize
fix; **auto-update `0.1.7 → 0.2.0` confirmed in the wild.** Commits (newest first):

- **Steampunk heat-haze** (`0609bb6`) — the last authored GPU effect: a procedural brass heat-shimmer on
  the transport-strip underglow, hung off the `SkinGpu` manifest (`heatHaze` param). `underglow.hlsl`
  `PSMain` gains a time-scrolling sine **wobble** that ripples the underglow band + a rising **"boiling"
  plume** fading upward, both scaled by `hz = saturate(uParams.x)` — so `heatHaze==0` is a strict no-op and
  every other skin's strip is byte-identical. New `float4 uParams` in the cbuffer (C++ `StripConstants`
  mirrored → 64 B). Steampunk `heatHaze=0.70`. Reviewed SHIP; pending the owner's live look.
- **Per-skin glow manifest — `SkinGpu`** (`e77dd3c`) — moved the hardcoded HLSL glow strengths (strip
  underglow + gutter neon) into a per-skin **`SkinGpu {stripGlow, edgeGlow, heatHaze}`** on the shared
  `common/` Skin model; `SkinStrip` reads `currentSkin().gpu.*` instead of the `1.0f`/`0.9f` literals.
  Per-skin: dark `{1.0,0.9}` (unchanged), light `{0.35,0.30}` (subtle), cyberpunk `{1.0,1.0}` (full neon),
  steampunk `{0.85,0.70}` (softer). `skinGpu*String` codec (arity-or-fallback, clamp, nan/inf-guarded) +
  selftests. No shader/`.cso` churn — the cbuffer already carried `uIntensity`. Reviewed SHIP.
- **Shared skin-model doc extracted** (`4897824`) — `THEME_ENGINE.md` §4 → standalone
  **[`docs/SKIN_MODEL.md`](../docs/SKIN_MODEL.md)** (canonical cross-team contract; §4 is now a pointer +
  summary). Mac-team coordination: PR #16 carries the `main`-side root-docs relocation.
- **UI iteration — meters Data-flow row + windowed Video-only mode** (`e6b83d7`) — **NOT theme-gated**
  (ships in both flag states). **Meters:** Settings→Meters is now a single "Meters…" item (inline
  quick-toggles removed); the setup dialog gains a 5th **Data flow** row for the buffer/fluid meter
  (enable + live preview; no Look/palette); the buffer meter is half-width in the strip with `LED_PITCH
  5→3` to match the mini-meters. **Video only** (Settings→Video only / **Ctrl+Shift+V**): collapse the
  window to just the video (hide nav/grid/title/strip) *without* leaving the window — reuses the
  fullscreen layout/paint path minus the window-style change; exit via double-click / Esc (also handled
  in MainProc, survives a resize) / a right-click view menu; drag the window by the video
  (SM_CXDRAG-thresholded); `libvlc_video_set_mouse_input`/`set_key_input` off so input reaches VideoProc
  while a stream plays. Each part reviewed SHIP.
- **Phase 4b-2** (`1ddba13`) — per-skin **neon glow on the dock gutters**: a new `PSEdge` HLSL pixel
  shader (reusing the fullscreen-triangle VS) renders an accent "neon tube" down each gutter via the
  Phase-1 windowless offscreen-texture + `BitBlt` technique (an `EdgeState` surface in `SkinStrip`, no
  D2D pass, static — rendered on WM_PAINT). Device-loss fix: the strip's loss path also drops the edge's
  resources. Owner-verified on Steampunk + Cyberpunk. Reviewed SHIP after the device-loss fix.
- **Phase 4b-1** (`49b3993`) — accent **glow** on the owner-draw transport buttons: a GDI+ bloom behind
  the glyph when *lit* (hover, or the record button while recording), glyph brightened to a bright core.
  Uses the meters' `drawTubeGlow` GDI+ technique, **not** a GPU surface — a swapchain `Present` would hit
  the sibling-clipping wall behind the child-window buttons. Reviewed SHIP (Light-skin contrast ~9:1).
- **Phase 4a** (`05a70df`) — the **Steampunk** skin (brass/copper on dark aged-iron, oxidised-rust danger).
  FIRST skin to diverge typography — a **Georgia serif title** via the 3b seam (body stays Segoe UI for
  grid legibility). The accent-driven Phase-1 strip underglow renders **brass under it for free** (it
  already reads `currentTheme().accent`/`windowBg`). `common/ui/Skin.{h,cpp}`; selftest now asserts 4 skins.
- **Phase 3c** (`1d2c3d7`) — the play/stop/record/fullscreen buttons converted from classic `BS_PUSHBUTTON`
  to skin-native **`BS_OWNERDRAW`** (`drawTransportButton` + `WM_DRAWITEM` in MainProc; hover tracked via a
  subclass in the button's `GWLP_USERDATA`). Flat into the strip band; clicks/tooltips/glyph-swaps
  unchanged. Reviewed SHIP after fixing a self-referential `#else` initializer (caught by the flag-off build).
- **Phase 3b** (`2206ac4`) — migrated the ~14 ad-hoc `CreateFontW`/`CreateTextFormat` sites onto one
  typography seam: a 4-arg `themeFont(role,dpi,px96,weight)` + a new `themeTextFormat(role,…)` in
  `D2DSupport.h`, so a skin swaps the *typeface* while each site keeps its own size/weight. The ★ grid
  dingbat stays pinned to Segoe UI Symbol; Splash's 2 GDI+ fonts left. Flag-off byte-identical. Reviewed SHIP.
- **Cyberpunk skin + registry-driven Theme menu** (`0611794`) — first *authored* skin (neon magenta on
  midnight, **colours only, no shaders yet**). Settings→Theme auto-lists `builtinSkins()`. Owner: "looks
  ok, adjust later once we see it in the app."
- **Phase 3a** (`2176bb1`) — `themeFont(role,dpi)` typography seam (skin-driven flag-on, hardwired Segoe
  UI flag-off) + `dangerHover` (the last hardcoded colour) wired into the skin. Parity-preserving.
- **Phase 2c** (`4c5df33`) — live **Settings→Theme** switch (Follow System / Dark / Light), persisted
  under `skinSettingKey()`, whole-app repaint broadcast (`applyActiveSkin`, MainWindow.cpp).
- **Phase 2b** (`3320334`) — `currentTheme()`/`themeBrush()` in `Theme.h` re-backed by the active skin
  (cached COLORREF Theme; per-skin brush cache). Parity: the dark skin == `makeDarkTheme` exactly.
- **Phase 2a** (`5ac563c`) — the shared model **`common/ui/Skin.{h,cpp}`** (SkinColor/SkinPalette/
  SkinFont/Skin + registry + UTF-8 codecs), 14 CLI selftests. **First model physically in `common/`.**
- **Phase 1 spike** (`ae02206`) — the D3D11⇄D2D1.1 interop device (`Win32/ui/skin/SkinDevice`) + a
  transport-strip **underglow** (HLSL shader → offscreen GDI-compatible texture → `BitBlt` in the
  parent's child-clipped paint DC — a swapchain `Present` bypasses GDI sibling-clipping and hid the
  transport controls, so it's windowless). Shaders: `fxc` → `.cso` → embedded C header via `bin2h.cmake`.

**Ratified contract decisions** ([`docs/SKIN_MODEL.md`](../docs/SKIN_MODEL.md)): skins define their OWN colours (no OS
`GetSysColor` inherit); active-selection + OS dark/light detection are renderer-side, only the id +
settings key are shared; the positional 14-role palette codec is frozen until user-customizable skins
ship (then add a version prefix).

**All shipped in v0.2.0** — 3b fonts, 3c owner-draw buttons, 4a Steampunk, 4b-1 button glow, 4b-2 gutter
neon glow, the per-skin **`SkinGpu` manifest**, and **Steampunk heat-haze** — the complete authored-effect
set (strip underglow · gutter neon · button glow · heat-haze), owner-verified live and auto-updated to
users. The shared contract lives in [`docs/SKIN_MODEL.md`](../docs/SKIN_MODEL.md) (canonical; THEME_ENGINE.md
§4 is a pointer); the mac Metal renderer mirrors it later.

**Next (0.2.x point releases):** **0.2.1 — the macOS app icon:** `packaging/app.ico` was rebuilt from
`art/macos_icon.png` (the cleaner mac icon, 1024² → a multi-res `.ico`: 16–64 as 32-bit BMP + 128/256 as
PNG) — a **single-file swap**, since the window/taskbar/Alt-Tab/dialog/exe *and* the installer's
`SetupIconFile` all reference `app.ico`. Owner-verified; on `main` as 0.2.1 dev (ships at the next 0.2.1
cut). Optional 0.2.x follow-ups: per-skin glow/heat-haze *tuning* (`SkinGpu` in `common/ui/Skin.cpp` +
the wobble/plume magnitudes in `underglow.hlsl`); Steampunk palette/serif polish; extend `SkinGpu` to the
GDI+ button glow (still a separate hardcoded strength); refresh the About/Splash *logo* art to match the
new icon; or reskin a new surface (nav / grid / dialogs — Appendix A).

### 0.1.7 — SHIPPED (tag `v0.1.7` @ `de8c571`, full `0.1.7.52`; all `/W4` clean)
The update fix + easter egg + restructure packaging fixes (10 paths incl. `art/BadAss_RabbitEars.png`),
rebased onto the macOS team's concurrent `main`, built, signed on macOS, released, appcast live @
`12be931`. 0.1.1–0.1.6 users get it via WinSparkle. Contents:
- **Update-from-About fix** — "Check for Updates" lives only in the About box, whose nested modal
  `GetMessage` loop **swallowed the `WM_QUIT`** that WinSparkle's `shutdown_request` triggers (it
  posts `WM_CLOSE` → `WM_DESTROY` → `PostQuitMessage`), so `runApp`'s outer loop never exited, the
  process lingered, and the installer couldn't overwrite the locked exe → update failed. Fix: the
  About loop **re-posts `WM_QUIT`** so the outer loop also exits (clean + fast), plus the
  `onUpdaterShutdownRequest` callback arms a **2.5s guaranteed force-exit** safety net.
  (`Win32/ui/Dialogs.cpp`, `Win32/platform/Updater.cpp`.) NB: the other themed dialogs share the
  swallow-`WM_QUIT` pattern (backlog: extract a shared `runModalLoop`); the 4s `WM_DESTROY` watchdog
  covers them and none can trigger updates.
- **About-box easter egg** — click the bunny to swap to `BadAss_RabbitEars.png` (new embedded
  resource `IDR_ABOUT_ALT_PNG`, lazy-loaded on first click, toggles; hit-tested against the drawn
  image rect `AboutState::imgRect`). (`Win32/ui/Dialogs.cpp`, `Win32/resource.h`,
  `packaging/RabbitEars.rc`, `art/BadAss_RabbitEars.png`.)
- **Packaging fixes for the restructure** — the exe moved to `build\Win32\`, so `installer.iss`
  `[Files]` **and** `build-installer.cmd`'s pre-flight check (both still pointed at `build\`) were
  updated, else the installer packaged a **stale pre-restructure exe** or failed. Stale top-level
  `build\` copies cleared. **Carry forward: build/verify the Windows exe path is `build\Win32\`.**

### 0.1.6 — SHIPPED (tag `v0.1.6` @ `5d06958`, full `0.1.6.37`; all `/W4` clean)
Committed as `5d06958` (13 paths; version bumped in the four places), built, signed on macOS,
released, appcast live @ `ebcbc2f`. 0.1.1–0.1.5 users get it via WinSparkle. Three items:
- **Auto-update-on-quit fix** (the important one) — updates failed intermittently because a lingering
  RabbitEars process locked the exe/DLLs so the installer couldn't overwrite them (a shutdown-
  coordination race — reproduced even with nothing playing). Full bundle: a WinSparkle
  `shutdown_request` callback → `PostMessage(WM_CLOSE)` so WinSparkle closes + waits for the process
  to exit before installing (`Updater.{h,cpp}`, `initUpdater(HWND)`); `VlcPlayer::shutdown()` joins the
  worker + reaper threads + releases libVLC synchronously in `WM_DESTROY` (was a fire-and-forget
  `stop()`); a bounded **force-exit watchdog** (`armExitWatchdog(4000)` in `WM_DESTROY`) that
  `ExitProcess`es if teardown hangs; a **single-instance mutex** (`RabbitEars.SingleInstance`, focuses
  the existing window); and installer **Restart Manager** (`AppMutex` + `CloseApplications` +
  `RestartApplications` in `installer.iss`, mutex name matched). Owner verifies via a live upgrade.
- **Per-meter "feel" knobs** — a `MeterTuning` struct (glow / smoothing / sensitivity / peak-hold /
  breathing) on `MeterConfig`, all normalized 0..1 with **0.5 = the classic behaviour exactly**
  (behavior-preserving, verified by the mapping math). Threaded into `drawTubeGlow` (glow), `drawScope`
  (glow + gain), `onTick` (smoothing → decay/ease/flare, peakHold → peakFall), the `paint*` sensitivity
  gain, and `miniMeterPushBitrate` (breathing). Setters/getters + `meterTuning{To,From}String`. The
  Meters → Setup… dialog gains **inline trackbar sliders per row** (relevant knobs per meter — spectrum
  also gets Peak, bitrate gets Breathe) with live preview via `WM_HSCROLL`, persisted per meter
  (`meter_<kind>_knobs`). Dialog widened to 720×~738 and **clamped to the monitor work area**
  (`clampToWorkArea`) so it can't clip off-screen. (`MiniMeter.{h,cpp}`, `Dialogs.cpp`, `MainWindow.cpp`)
- **Random splash captions** — the splash shuffles its caption order per launch (Fisher-Yates,
  re-shuffle on wrap) so the sequence differs each run. (`Splash.cpp`)
- **Backlog noted:** the other themed dialogs (About / prompt / Categories / Terms / info) share the
  same centre-on-parent positioning and could clip near a screen edge — reuse `clampToWorkArea`.

### 0.1.5 — SHIPPED (tag `v0.1.5` @ `ca945d1`, full `0.1.5.29`; all `/W4` clean)
Committed as `ca945d1` (10 paths; version bumped in the four places), built, signed on macOS,
released, appcast live @ `873c73a`. **0.1.5 is a METERS OVERHAUL** — the owner pivoted 0.1.5 here;
**JSON profiles are DEFERRED** to a later version. Changes:
- **Per-meter look + palette engine** (`MiniMeter.{h,cpp}`) — every meter carries a
  `MeterStyle {Led,Tube,Lcd,Scope}` + a fully custom `MeterPalette` (`bg/off/low/mid/high/accent/
  peak`; `bg == CLR_INVALID` follows the theme's windowBg). Defaults reproduce the classic LED look
  exactly (behavior-preserving). Rendering is palette-driven via `rampColor` + a style-aware
  `drawCell`; `drawScope` is a separate trace path. API: `miniMeterSetStyle/SetPalette` setters,
  `miniMeterStyle/Palette` getters, `meter{Style,Palette}{To,From}String` (de)serialization, and a
  `MeterConfig {enabled,style,palette}` POD.
- **The four looks** — **LED** (flat GDI, unchanged), **LCD** (GDI; off-cells ghost the lit colour),
  **Vacuum tube** (muted GDI base cells + a **GDI+ soft phosphor halo** — `drawTubeGlow` blooms each
  lit cell with layered antialiased ellipses [wide dim halo → inner glow → peak-bright core] that
  bleed across cell borders into a glowing column; replaced the old hard bright-core), **Oscilloscope**
  (**GDI+** antialiased trace with a phosphor bloom — two wide low-alpha accent underlays beneath a
  peak-bright core, on a faint graticule). An unimplemented look falls back to LED. GDI+ is already started
  globally by `runApp` (`MainWindow.cpp` GdiplusStartup/Shutdown) so MiniMeter just uses it — it
  needs `<objidl.h>` before `<gdiplus.h>` (the min/max-in-Gdiplus trick).
- **Meters… setup dialog** (`Dialogs.cpp` `chooseMeters` + `MetersProc`, declared in `Dialogs.h`;
  opened from **Settings → Meters → Setup…**, `ID_METERS_SETUP` → `onMeters` in `MainWindow.cpp`) —
  4 rows (one per meter), each a **live preview** MiniMeter fed synthetic data via a dialog timer, a
  **Look** combobox, and **7 owner-draw colour swatches** (Bg/Dim/Low/Mid/High/Accent/Peak) that open
  Win32 `ChooseColor`. Enable checkboxes + OK/Cancel/**Reset to defaults**. On OK it applies live +
  persists per meter (`meter_<kind>_style`, `meter_<kind>_colors`, and the existing `meter_<kind>`
  enable); loaded at startup after the meters are created. Reviewed clean by a background agent
  (no lifetime/teardown/leak/modal-loop bug).
- **Owner design decisions (locked):** per-meter looks (all 4 available on each), full per-meter
  palette, and **keep the bitrate adaptive "breathing" scale** (the "changing shape as it scrolls"
  is expected — the ceiling re-normalizes each sample — not a bug).
- **Sign-off + ship (done):** the four looks (incl. the GDI+ tube soft-halo `drawTubeGlow`) were
  visually approved by the owner; 0.1.5 was cut per `docs/RELEASING.md` (built `0.1.5.29` → signed on
  macOS → `v0.1.5` GitHub release with the installer asset → appcast `873c73a`). 0.1.1–0.1.4 users
  get it via WinSparkle.

**Cross-platform direction (2026-07-03) — see memory `rabbitears-cross-platform`:** RabbitEars is
going **macOS**. **Premium experience per platform** (Windows: GDI/GDI+/Direct2D; macOS: Core
Graphics/Metal). **~70% common code** (engine — M3uParser, Database, Http, DockLayout tree — plus the
meter *model/config/palette/style*); **~30% platform-specific** (rendering, windowing, chrome). The
macOS team is writing **`MACOS_PORT.md`**; the repo will be **restructured** (common vs. platform
dirs) once it lands — **do NOT preemptively restructure**; keep the meter *model ↔ renderer* seam
clean so the split is a move, not a rewrite.

### 0.1.4 — SHIPPED (tag `v0.1.4` @ `8622e8a`, full `0.1.4.26`; all `/W4` clean)
Two commits: `47dc0fe` (agile audio-loopback handler + meters reset on switch) + `8622e8a`
(the batch below). Built, signed on macOS, released, appcast live. A fix-and-polish batch:
- **Audio spectrum meter fixed** — the process-loopback completion handler now implements
  `IAgileObject`, so `ActivateAudioInterfaceAsync` no longer fails with `E_ILLEGAL_METHOD_CALL`
  (`0x8000000E`) on the MTA capture thread. (`SpectrumTap.cpp`)
- **Mini-meters animate reliably** — the ~30 fps timer syncs to real visibility
  (`WM_WINDOWPOSCHANGED` + `WM_PAINT`), not just `WM_SHOWWINDOW` (which `DeferWindowPos`
  never sends), so they no longer sit frozen until a minimize/restore. Meters also reset on
  channel switch so a dead stream can't freeze the previous channel's readings. (`MiniMeter.cpp`)
- **Transport-strip repaint** — `WM_PAINT` paints the strip band itself (not via
  `WM_ERASEBKGND`), so relayouts / meter toggles leave no stale "blank grid" footprints or
  top-edge seams. (`MainWindow.cpp`)
- **Smooth splitter drag** — transport controls bit-copy to their new spot (no repaint → no
  button flicker or video black-flash), nav/grid get a **paced** synchronous flush (no
  streaks; `gutterFlushTick`), and a full settle repaint fires on release. (`MainWindow.cpp`)
- **Import results dialog** — adding a playlist shows a themed `showInfoDialog` (channels
  parsed / imported / skipped, group count, or the error). (`Dialogs.{h,cpp}`, `MainWindow.cpp`)
- **Rename playlists** — right-click a playlist → **Rename…** (`Database::renamePlaylist`;
  friendly display name only). **Categories…** now shows a "no categories" notice via
  `showInfoDialog` when the library has no group titles (the owner's FAST/LG library has 0).
- **Splash** shows the version string (+ one more caption); **About…** moved last in Settings.

### 0.1.3 — SHIPPED (tag `v0.1.3` @ `ebd71a8`, full `0.1.3.22`; all `/W4` clean)
Committed as `ebd71a8` (16 paths; version bumped in the four places), built, signed on
macOS, released, appcast live. Changes:
- **Transport/fullscreen icons** (`MainWindow.cpp`) — play/pause/stop/record + fullscreen
  are Segoe MDL2 glyph buttons (`kGlyph*`), square + tooltipped; play↔pause and
  record↔stop swap with state. Narrower buttons free strip width (helps the meter tray).
- **Channel-switch hang fixed** (`VlcPlayer.cpp`) — the blocking `stop()`/`release()`
  runs on a tracked reaper thread (`reapers_`) so a stuck stream can't wedge the next
  channel; the destructor drains reapers before `libvlc_release`. Reviewed sound; one
  cosmetic residual (two vouts briefly share the video HWND during a stuck stop —
  follow-up: give each player its own child video HWND).
- **Xtream / query-string URLs fixed** (`Http.cpp`) — `httpGet` was DROPPING the
  `?query` (no `lpszExtraInfo` buffer) so `?username=&password=` never reached the
  server; fixed (verified the query is sent), and the fetch User-Agent is now VLC-style.
- **Spectrum tap diagnostics** (`SpectrumTap.cpp`) — logs activation/init HRESULTs and
  "first audio window analysed" so `rabbitears.log` pinpoints the "audio meter doesn't
  work" report (also: the icon change frees space so the spectrum meter is more likely
  to be visible — the responsive tray had been hiding it on narrow windows).
- **Dockable layout, Phase 1** — NEW `ui/DockLayout.{h,cpp}`: a pure split-tree over the
  Nav/Video/Grid panels (serialize/parse with fallback, re-dock surgery), built into
  **RabbitEarsCore** and covered by **9 CLI `--selftest` assertions**. `MainWindow`
  renders the three regions from the tree (Video panel = video + the transport strip at
  its bottom), with parent-painted resize **gutters** (drag persists to `dock_layout`,
  `WM_CAPTURECHANGED`-safe) and a **Settings → Layout** menu (reset + move any region to
  any edge). The old single nav splitter (`ReVSplitter`/`VSplitterProc`, `sidebar_w`) is
  now dead code. 5-agent review → 1 high fixed (lost-capture sticky drag). `layout()`
  moves every child in one atomic **`BeginDeferWindowPos`** pass with **`SWP_NOCOPYBITS`**
  (the ManorLords-SGE fix) — this killed splitter-drag artifacts + stale transport-button
  pixels on a panel move.
- **Drag-to-redock (Phase 2, partial)** — each region has a small **grip** child (class
  `ReDockGrip`, top-right corner, `Panel` id in `GWLP_USERDATA`); dragging it shows a
  translucent coral **drop-zone overlay** (`ReDropOverlay`, layered popup) over the
  target half and, on release, `dock()`s the region there (`beginPanelDrag`/
  `updateDockTarget`/`endPanelDrag`; parent captures the mouse, `WM_CAPTURECHANGED`
  cancels). Known caveat to verify: the **video** region's grip is a sibling of the
  libVLC surface, so D3D/DWM may visually occlude it during playback (still clickable).
  Remaining Phase 2: named saved layouts. The Settings → Layout "move to edge" menu is
  kept as a fallback.
- **T&C gate** (`runApp`) — the user must accept the Terms, and **re-accept on every
  version change** (new install or update). `tos_accepted` stores the **full** version
  (`RE_VERSION_FULL_W`, marketing.build) it was accepted for, so any bump re-prompts;
  declining exits. (NB: build number = git commit count, so during dev this re-prompts
  once per commit, not per rebuild.)
- **Animated splash** (`Splash.cpp`) — the splash now runs on its **own thread** (owns
  the window so UpdateLayeredWindow/DestroyWindow stay on the creating thread while the
  UI thread blocks in libVLC init) and cycles tongue-in-cheek captions (`kMessages`:
  "Finding the power plug…", "Bending the left ear to the right…", …) every 1.2 s.
  `closeSplash` signals + joins the thread.
- **By-country nav filter** — a **Countries** node in the sidebar (next to Groups). Since
  the model has no country field, it's derived from the **tvg-id suffix** (iptv-org
  `"<name>.<cc>"`): `Database::listCountries()`/`channelsByCountry()` (+ `countryFromTvgId`)
  in Core, with a `ViewKind::Country`/`ViewFilter::country` + `countryLabel()` name map in
  `MainWindow`. **5 CLI selftest assertions** cover the derivation. Caveat: playlists
  whose channels lack `tvg-id` country codes (e.g. some Xtream feeds) won't populate it.

The engine + full GUI are complete and proven end-to-end. **Auto-update is confirmed
working** (About → Check for Updates reports "up to date" against the live appcast).

> **Sandbox note:** this dev environment **cannot launch the GUI exe** (`Start-Process` hangs even with
> `dangerouslyDisableSandbox`; `cmd start` → "Access is denied"). All GUI work is **build-verified +
> reasoned**; the owner does the real runtime/visual verification. Handy pattern used across 0.2.1: kick off a
> **background wait-loop** that polls for `RabbitEars.exe` to exit, then rebuilds — the owner just closes the
> app and the relink + verify happens hands-off. The CLI (`RabbitEarsCli`) *does* run here to exercise the
> core headlessly. **As of the 0.2.1 EPG work the machine also has `python` + `sqlite3`** — so you can query
> the real DB directly for EPG/tvg-id debugging
> (`sqlite3 %LOCALAPPDATA%\RabbitEars\rabbitears.db "SELECT tvg_id,name FROM channels LIMIT 20"`), alongside
> the `RabbitEarsCli --tvgids` diagnostic. Owner runs on the same machine (real DB at
> `%LOCALAPPDATA%\RabbitEars\`, ~13k iptv-org channels in `index.m3u` + a 444-channel `uslg.m3u`).

### Shipped in 0.1.2 (committed @ `8c99254`, tag `v0.1.2`)
All `/W4` clean; committed + released. (These were the working-tree batch; now on `main`.)
- **Real fullscreen** — Fullscreen (button / double-click video / Esc) now saves
  the window placement+style, switches to a **borderless popup covering the whole
  monitor** (taskbar hidden), and restores on exit. Frame-inset (`WM_NCCALCSIZE`)
  and top-edge resize (`HTTOP`) are suppressed while fullscreen.
- **Recording (Phase 1, manual)** — a **headless second libVLC player** (`rec_` in
  `VlcPlayer`, worker-thread only, shared instance) records the current channel to
  a **`.ts`/`.mkv` lossless stream copy** via `:sout=#std{access=file,mux=…,dst='…'}`
  — independent of playback (you keep watching). **Record** button in the transport
  strip toggles it; files go to `%USERPROFILE%\Videos\RabbitEars\<name> - <ts>.<ext>`.
  Finalized on Stop *and* on app quit. NB: the sout `dst` path is single-quoted and
  **any literal `'` is doubled** (VLC chain-parser requirement — the `%USERPROFILE%`
  dir can contain one, e.g. `C:\Users\O'Brien`).
- **Settings menu** — command bar is now **`+ Add Playlist` · `Settings ▾`**; the
  popup holds **Open File**, **About**, **Recording format** (TS/MKV radio), and
  **Hide unavailable channels** (toggle). Settings persist (`rec_format`,
  `hide_dead`).
- **Hide unavailable** — filters out `dead_status=Dead` across all views + search
  via `applyChannelFilters()` (the shared hook the categories filter also uses).
- **Categories… include-filter** (`Dialogs.cpp` `chooseCategories()` + `MainWindow.cpp`
  `onCategories()`) — **Settings → Categories…** opens a dark checkbox `ListView` over
  the distinct group titles (`db.listGroups()`) with a live "Filter categories…" box,
  **Select All / Clear**, and an "N of M selected" count. Include set is applied in
  `applyChannelFilters()` (nav views **and** global search); channels with a blank
  group are never hidden (unselectable). Normalized so all-checked / none-checked ==
  filter off. Persisted as newline-joined `category_filter`; the menu item shows a
  count badge + check when active. Built `/W4` clean; 5-agent adversarial review clean.
- **Modular meters + audio spectrum** (`ui/MiniMeter.{h,cpp}`, `audio/SpectrumTap.{h,cpp}`)
  — a small LED dot-matrix control `MiniMeter` renders 4 selectable meters in a
  right-to-left tray left of the fluid BufferMeter, each toggled + persisted via
  **Settings → Meters** (`meter_spectrum/signal/bitrate/frames`):
  * **Audio spectrum** — a real FFT analyser of *this app's own* audio, captured
    read-only via **WASAPI process-loopback** (`AUDCLNT_PROCESS_LOOPBACK` on our PID,
    `SpectrumTap`). It NEVER touches libVLC's audio path (chosen over the amem-takeover
    route for exactly that safety); on any failure it just sits idle. 1024-pt Hann FFT
    → 16 log bands; attack-fast/decay-slow with peak caps. `syncSpectrumTap()` runs the
    capture thread only while the spectrum meter is shown.
  * **Signal strength** / **Bitrate** / **Frame rate** — driven from the existing
    `PlayerEvent::Stats` snapshot (FrameMeter needed a new `FlowStats.displayedPerSec`
    from libVLC `i_displayed_pictures`). Defaults: spectrum + signal on, bitrate +
    frames off.
  Built `/W4` clean; 4-area adversarial review → 1 medium fixed (`running_` now clears
  on every `SpectrumTap::run()` exit so a failed capture can auto-retry). **Owner must
  verify visually/aurally — the sandbox can't launch the GUI or capture audio.**
- **Legal / first-run gating** — the About box carries a disclaimer footnote, and a
  **first-run Terms-of-Use dialog** (`Dialogs.cpp` `showTerms()`, gated in `runApp()`
  on a persisted `tos_accepted` setting) must be accepted before the main window is
  shown; **Decline** tears down and exits. The **default iptv-org playlist URL was
  removed** from Add-Playlist (the box starts empty) — RabbitEars ships with **no
  bundled playlist/content**; users add their own source. (Existing users see the
  T&C once on their next launch, since `tos_accepted` isn't set yet.)

### Shipped GUI (committed, in v0.1.1)
- **Custom title-bar chrome** (`MainWindow.cpp`): `WM_NCCALCSIZE` reclaims the NC
  area; owner-draw command bar (title, coral "+ Add Playlist", Settings menu,
  search box, hand-drawn min/max/close), drag-move, double-click-maximize.
- **Nav sidebar** (dark TreeView): All / ★ Favourites / Groups / Playlists.
  **Right-click a playlist → Delete Playlist** (confirm → `db.deletePlaylist`).
  Draggable splitter (`ReVSplitter`, width persisted `sidebar_w`).
- **Channel grid** (`ChannelGridControl`): Direct2D owner-draw, smooth at 12k+ rows,
  `# | ★ | logo | name | group`, async WIC logo thumbnails (disk-cached under
  `…\RabbitEars\logos\`), inline `#` edit, type-a-number jump, dead/geo greying,
  live search, click-to-play, ★ toggles favourite. **D2D target pinned to 96 DPI**
  so draw + hit-testing share pixel space — don't remove that.
- **VlcPlayer** (`src/ui/VlcPlayer.{h,cpp}`): all libVLC lifecycle on a dedicated
  **worker thread** (blocking `stop()`/`release()` never touch the UI thread),
  `set_hwnd` before play, per-channel UA/referrer, events marshaled via
  `PostMessage`. Samples `libvlc_media_get_stats` every 250 ms → a `FlowStats`
  snapshot (throughput, packet loss, buffered bytes) posted as `PlayerEvent::Stats`.
- **Buffer meter** (`src/ui/BufferMeter.{h,cpp}`): a Navier-Stokes "stable fluids"
  sim rendered as a **blocky LED dot-matrix** (client-sized DIB, per-cell squares).
  Motion is **honest** — inflow-current speed + wave energy track real demux
  throughput (a stalled stream goes still); corruption/discontinuity/dropped-frame
  deltas drive turbulence + splashes; a healthy stream rests ~half-full with a
  "pouring-in" top-right fill. Overlay shows the consumption rate; **hover tooltip**
  shows consumption + buffer latency + recent loss. Right-click hides it
  (`buffer_hidden`). Tunables are the `constexpr` block atop the .cpp
  (`kVisibleFill`, `NORMAL_FILL`, `POUR_VY`, …); UI-side knobs `kFlowRef`/`kTroubleRef`
  in `MainWindow.cpp`.
- **Buffer slider + latency read-outs** — a "Buffer N.N s" slider in the transport
  strip sets `network-caching` (persisted `buffer_ms`; re-buffers the current stream
  on change). NB: libVLC's **`i_read_bytes` is 0 for HLS/adaptive**, so the
  *received rate* and *measured delay* are only shown when actually reported; the
  reliable **consumption** rate + **configured buffer latency** are always shown.
- **Startup splash** (`src/ui/Splash.{h,cpp}`): a layered (per-pixel-alpha) branded
  window shown during the ~10 s libVLC init (mostly `libvlc_new` loading 325
  plugins). It's a layered window, so DWM keeps compositing it while the UI thread
  is blocked in `WM_CREATE`.
- **Volume** slider with a Segoe MDL2 speaker glyph + tooltip.

### Diagnostics — a real log now exists
`src/platform/Log.{h,cpp}` — thread-safe, always-on log at
**`%LOCALAPPDATA%\RabbitEars\rabbitears.log`** (previous run kept as `.log.1`, every
line flushed). Captures the session banner (app/OS/exe), DB open, playlist
download/parse/import, channel selection + stream URL, all playback events, and
**libVLC's own warnings/errors** (routed via `libvlc_log_set`; `--quiet` was
dropped). This is the first thing to ask a tester for.

## Engine (Layer A — complete, /W4 clean, proven)

- **M3U/M3U8 parser** (`src/core/M3uParser`): full EXTINF dialect — `#EXTM3U`
  (+ `x-tvg-url`/`url-tvg`), `#EXTINF` attrs (`tvg-id`/`-logo`/`-name`/`group-title`/
  `tvg-chno` + inline `http-user-agent`/`http-referrer`), `#EXTGRP`, `#EXTVLCOPT`,
  bare-URL playlists. Splits the display name on the **first *unquoted* comma**;
  strips BOM; tolerates CR/LF/CRLF.
- **SQLite store** (`src/db/Database`): typed DAO, RAII `Stmt` (bound params) + `Tx`
  (one `BEGIN IMMEDIATE` bulk insert), WAL + FK, schema on open,
  `%LOCALAPPDATA%\RabbitEars\rabbitears.db` (env `RABBITEARS_DATA_DIR`). Idempotent
  refresh via `ON CONFLICT(playlist_id,stream_url)` preserving favourite + LCN.
  `deletePlaylist`, `channelsByGroup/Playlist`, `favourites`, `searchChannels`,
  `setDeadStatus`, settings K/V.
- **RabbitEarsCli** (`src/cli/RabbitEarsCli.cpp`): `--selftest` (30 assertions),
  `--fetch <url>` (WinHTTP + parse), `--import <url|file>` (into the DB; respects
  `RABBITEARS_DATA_DIR`), `<file.m3u>` dump. Runs headlessly in the sandbox — use it
  to repro core/parse/store issues.

## Release / auto-update (LIVE — see `docs/RELEASING.md`)

- Shares the **family Ed25519 key** with the siblings: the WinSparkle public key in
  `Win32/platform/Updater.cpp` (`sKPprIa95Hw+…`) equals the macOS `SUPublicEDKey`, so
  installers are **signed on macOS** with the same private key.
- **Per release:** bump version in 4 places (`APP_VERSION` in `cmake/AppVersion.cmake`
  — now the single source shared with the macOS build, `MyVer` in
  `packaging/installer.iss`, VERSIONINFO in `packaging/RabbitEars.rc`,
  `assemblyIdentity` in `packaging/app.manifest`) → commit → `scripts\build.cmd
  -DRABBITEARS_BUILD_GUI=ON` → `scripts\build-installer.cmd` (Inno at
  `%LOCALAPPDATA%\Programs\Inno Setup 6`) → **sign on the Mac** (`./bin/sign_update
  --account SQLTerminal RabbitEars-<ver>-setup.exe`, wrapped by
  `scripts/sign-release.sh`) → `scripts\make-appcast.ps1 -Version A.B.C.<build>
  -SetupExe … -Signature <sig> -Tag v<ver>` → `gh release create` with the installer
  → commit/push `appcast.xml` (repo root). Build number = git commit count (baked
  after the commit).
- **Caveat:** 0.1.0 shipped before signing, so **0.1.0 users can't auto-update** —
  0.1.1 is the baseline; 0.1.1 users get 0.1.2+ automatically. **Authenticode**
  signing (to silence SmartScreen) is still not set up.

## Architecture (bottom-up)

```
sqlite3               third_party/sqlite/  vendored public-domain amalgamation. Static lib.
RabbitEarsCore        common/core, db,     platform-neutral engine: M3uParser, Database,
                      models, ui/DockLayout DockLayout. Links only sqlite3 (no UI/HTTP/OS
                                           paths). Built on BOTH Windows and macOS.
RabbitEarsPlatformWin Win32/platform/      Windows platform layer: Http (WinHTTP) + Paths
                                           (%LOCALAPPDATA% db path). Linked by CLI + GUI.
RabbitEarsCli         Win32/cli/           headless core tool (--selftest/--fetch/--import).
RabbitEars            Win32/ (ui, WinMain, Win32 GUI (gated: RABBITEARS_BUILD_GUI).
 (GUI)                audio, platform/)    MainWindow (chrome+layout+wiring), ChannelGrid-
                                           Control (D2D grid), BufferMeter (LED), VlcPlayer
                                           (worker libVLC + recorder), Dialogs (About/prompt),
                                           Splash (layered), Log (diagnostics), Updater
                                           (WinSparkle).
```

> Layout note: the tree is split into `common/` (shared engine, both OSes), `Win32/` (the
> Windows app), and `mac/` (the macOS app), built by one unified root `CMakeLists.txt`. Some
> inline `src/...` paths elsewhere in this doc predate that split.

## Toolchain (non-obvious)

- **VS 2026 Community** at `C:\Program Files\Microsoft Visual Studio\18\Community`
  (MSVC + bundled CMake/Ninja). `cmake`/`cl` are **not** on PATH.
- Build: **`scripts\build.cmd`** (vcvars64 + PATH; `-G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo`). Pass extra args, e.g.
  `scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON`. From PowerShell, invoke it as
  `& "G:\RabbitEars\scripts\build.cmd" …` (a bare `scripts\build.cmd` after `;` can
  be mis-parsed as a module).
- **RelWithDebInfo, not Debug** (Debug CRT heap lock stalls the UI thread).
- **`LINK1168: cannot open RabbitEars.exe`** = an instance is running →
  `Stop-Process -Name RabbitEars -Force`, rebuild.
- Static CRT (`/MT`) — the exe needs no VC++ redist.

## Build, test, verify

```
scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON      :: GUI (provisions libVLC once)
build\RabbitEarsCli.exe --selftest               :: 30 parser + DB assertions
build\RabbitEarsCli.exe --import <url|file>       :: exercise fetch+parse+store headlessly
build\RabbitEars.exe                              :: the app (owner runs; sandbox can't)
scripts\build-installer.cmd                       :: -> build\installer\RabbitEars-<ver>-setup.exe
```

## Gotchas to carry forward

- **libVLC 3.x `stop()`/`release()` are SYNCHRONOUS/blocking** — keep ALL media-player
  lifecycle on the `VlcPlayer` worker thread (both `mp_` playback and `rec_` recorder).
- **libVLC event callbacks run on a libVLC thread** — only atomics + `PostMessage`.
- **`set_hwnd` before `play()`** or libVLC opens its own top-level output window.
- **libVLC `i_read_bytes` is 0 for HLS/adaptive** — don't trust the input-byte
  counter for those; consumption (`i_demux_read_bytes`) is the reliable rate.
- **VLC sout single-quoted values**: a literal `'` must be **doubled** (`''`), else
  the chain parser truncates the path. Sanitize filenames; double quotes in the dir.
- **Playback uses the GPU by default** (DXVA2/D3D11VA decode + Direct3D11 vout — we
  don't override `--avcodec-hw`/`--vout`). Recording is a **stream copy** → no
  decode/encode → no GPU.
- **`WM_CTLCOLORSTATIC` must return an opaque themed brush + `SetBkColor`** (else
  ghosting / broken ClearType).
- **`EnableWindow(mainHwnd, FALSE)` doesn't cascade** to the custom command bar —
  track `busy` explicitly during playlist fetch.
- **Modal dialogs must read their controls BEFORE `DestroyWindow`** — the Add-Playlist
  prompt bug was reading the edit box after destroy → empty URL → silent no-op.
  (Fixed; watch for the pattern in `Dialogs.cpp`.)
- **libVLC is LGPLv2.1** — dynamic-link + ship unmodified DLLs/plugins; include the
  attribution; no GPL-only plugins.
- **WASAPI process-loopback needs a Win11-era NTDDI** — `AUDIOCLIENT_ACTIVATION_PARAMS`
  et al. are `#if`'d out at the project-wide `NTDDI_VERSION=0x0A000006`, so
  `SpectrumTap.cpp` `#undef`+`#define`s `NTDDI_VERSION 0x0A00000C` **before** the first
  Windows header. Runtime still degrades gracefully on older Windows (meter idle).
- **`themeBrush()` caches only 12 colors and LEAKS beyond that** — the LED meters draw
  every cell with the **GDI DC brush** (`SetDCBrushColor` + `GetStockObject(DC_BRUSH)`),
  never `themeBrush`, so many per-cell colors cost no allocations.
- **Stop the `SpectrumTap` before the meter HWNDs die** — its capture thread pushes to
  `meterSpectrum`, so `WM_DESTROY` calls `spectrumTap.stop()` (joins the thread) first;
  child windows are destroyed only after the parent's `WM_DESTROY` returns.

## Backlog

Moved to **[`BACKLOG.md`](BACKLOG.md)** — the parked work, headlined by the **theme engine** (0.2.x
epic: full reskin + selectable D3D11/shader skins). Also there: JSON profiles, scheduled recording,
recording formats, EPG + dead-link checker, resume-last-channel, named saved layouts, group-title
country fallback, the dialog work-area clamp + shared-`runModalLoop` cleanup, DPI-change relayout,
Authenticode + portable-zip. `HANDOVER.md` stays focused on **current state**.

## Git state

Active development on `main` (owner-owned repo `github.com/arcanii/RabbitEars`).
Tags `v0.1.0`…`v0.2.2`; **v0.2.2 released @ `059b632`** (full `0.2.2.153`; appcast @ `fcdac10`) — EPG `@feed`
fix + clockwork icon + About/PIP polish; now on **0.2.3 dev**. **Release-tooling note (0.2.2):** this machine now
has **`gh` CLI (2.96) AND Inno Setup**, so the whole release ran locally: commit → push → build →
`build-installer.cmd` (Inno) → `gh release create v0.2.2` + upload → `make-appcast.ps1` → commit/push
`appcast.xml`. **Only EdDSA signing stays on the Mac** (`scripts/sign-release.sh` → `sign_update` + the key).
The `raw.githubusercontent.com` feed caches ~5 min (`max-age=300`) — an installed app won't see the new appcast
until that expires (looked like "0.2.1 doesn't detect the update" for a few min; not a bug). Prior: **v0.2.1 @
`79ab12c`** (`0.2.1.148`). Earlier: **v0.2.0
@ `343aa0e`** (`0.2.0.107`; appcast @ `7b3946a`), the theme engine, theme-ON by default. The `theme-engine`
branch was **merged to `main` + deleted** (only
`main` remains; PR #16 superseded + closed). **The macOS team pushes to `main` too** (mac Phase-1), so
**`git fetch` + rebase before a release** — the 0.2.0 push integrated a concurrent mac commit mid-flight
(the first push was rejected until re-fetched). Working tree otherwise clean (the owner's
`art/logo_basic*.png` stay untracked). Build number = git commit count, baked at CMake configure time
**after** the commit — so a build must follow the release commit to stamp the matching `0.2.0.<count>`. Commit/push only when the
owner asks; stage **specific paths** (the owner keeps adding `art/*.png` — never `git add -A`); end
commit messages with the Co-Authored-By trailer.

## Immediate next steps (pick up here)

✅ **0.2.8 SHIPPED** (2026-07-11) — tag `v0.2.8` @ `e45adb6`, `0.2.8.217`, three signed installers on GitHub
release `v0.2.8`, two appcasts LIVE @ `5404004`. Three features: the **wake-timer preflight**, a **TV Guide
loading box**, and **localization (English + 日本語)**. Built green x64 BOTH theme flags + native ARM64,
selftest ALL PASS, adversarially reviewed. Owner pass: ✅ language shift confirmed on-device; the wake
preflight banner + guide loading box are low-risk, **delegated to testers**. Japanese is a machine draft —
native review is the gate before advertising JP support. (Details of the three features below.)

**Localization — Japanese first (commit `c9c2504`).** The app is fully localizable and ships English + 日本語,
defaulting to the system language with a Settings ▸ Language toggle (System / English / 日本語). Changing it
prompts a themed **TaskDialog** (Restart now / Later, localized to the chosen language); "Restart now"
self-relaunches — a new instance is spawned with **`--restart`** (which WAITS on the single-instance mutex
for the outgoing instance to exit instead of bouncing), then the current one tears down. The active language
is left unchanged until the restart, so the session never shows a half-translated mix.
- **The pipeline is the point (sustainable).** Source of truth = JSON under **`common/i18n/`** (`languages.json`
  + `keys.json` + one `<code>.json` per language). **`tools/i18n/gen_i18n.py`** generates the pure catalog
  `common/core/Strings.{h,cpp}` (enum `StringId` indexing a per-language table → "every key in every language"
  is generator- AND compile-time enforced). **Never hand-edit `Strings.{h,cpp}`** — edit the JSON, run the
  generator, commit both. Adding a language = drop a `<code>.json` + a `languages.json` entry + wire
  `Tr.h`/menu; it won't build until every one of the ~374 keys is filled. Translator confirmation:
  `gen_i18n.py --review ja` → a side-by-side `review-ja.md`. See `common/i18n/README.md`.
- **Three drift guards:** `gen_i18n.py --check` (wired into `.github/workflows/windows-core.yml`), the CLI
  `--selftest` (completeness + placeholder parity), and the compile-time array-vs-enum check.
- **Call sites:** `Win32/ui/Tr.h` gives `tr(StringId)`→wstring and `trf(id,{args})` (fills `{0}`,`{1}`…);
  ~430 hardcoded `L"..."` across `Win32/ui/*.cpp` were converted (much of it by parallel agents, build-gated +
  a completeness scan). Two COMDLG filter strings (embedded `\0`) stay literal in code, by design.
- **Selector/chrome:** system default via `GetUserDefaultUILanguage`, read at WM_CREATE **before** the chrome
  builds (and before the splash thread starts — see the atomic below); ids 2066–2068. The command-bar
  "Settings ▾" is now an **MDL2 gear glyph** (`kGlyphSettings` U+E713, mac parity) and the flat Settings menu
  is **regrouped** (Channels / Recording / View / Layout submenus; Language + Theme top-level).
- **CJK font:** `themeFontFamily()` (the single GDI+DirectWrite choke point) switches non-symbol roles to
  **Yu Gothic UI** when Japanese; the MDL2 glyph role is exempt so the gear/transport icons stay.
- **Two review-caught bugs fixed:** the JP view-switch confirm leaked an English `%s` plural "s" (now baked
  "(s)" into the string); and the language global is a **relaxed `std::atomic`** set **before** the splash
  worker thread starts — fixing a data race + an English flash on the localized splash.
- **⚠️ Japanese is a glossary-consistent MACHINE DRAFT** pending a native review (Terms-of-Use especially).
  The pipeline exists precisely so that review is a clean `ja.json` diff.
- **Owner pass:** ✅ **language shift confirmed** (2026-07-11) — switching language + the Restart-now prompt
  brings the app back up in the chosen language, on-device. Still worth an eyeball: the gear/menu look, that
  Japanese reads naturally (no tofu), and that a Japanese-Windows install comes up Japanese with no toggle.
  The **native Japanese review** (`gen_i18n.py --review ja`) remains the gate before advertising JP support.

**TV Guide loading box** (`onEpgGuide`, `MainWindowCommands.cpp`). The FIRST guide open is slow (reopen via
`revealEpgGuide` is instant) — a synchronous per-playlist `programmesInWindow` build on the UI thread, with
the window appearing only at the end, so the click looked hung. It now shows the existing modeless
"Loading TV guide…" box (busy-spinner cursor) first, then builds, then opens. **The box is a LOCAL HWND, not
`st->loadingDlg`** — that slot belongs to the async EPG fetch; an early version reused it plus an
`if (st->busy) return;` guard, and review caught the guard as a **regression** (`st->busy` is also set by the
playlist download worker, which shows no box and leaves the grid clickable → "Show in TV Guide" bailed and
printed a misleading "try Refresh Guide…"). No guard now: the build only READS the DB on the UI thread, so
it's safe during a fetch/playlist load. A `diag::info` logs `TV guide first-open: DB+build … ms, window … ms`
— **the owner should read that number**; if builds run past a few seconds, thread the build with its OWN
sqlite connection (see `Win32/BACKLOG.md`, "build off the UI thread").

**Marketing version bumped `0.2.7` → `0.2.8`** in the four places that must move together —
`cmake/AppVersion.cmake` (`APP_VERSION`), `packaging/app.manifest` (`assemblyIdentity version`),
`packaging/installer.iss` (`MyVer`), `packaging/RabbitEars.rc` (`FILEVERSION`/`PRODUCTVERSION` + the two
version strings). Build number = git commit count, stamped at configure, so **build AFTER committing** or the
stamp trails `HEAD`. **No tag, no appcast, no installers yet** — bumping the version does not release
anything; the tag + appcast still gate the rollout, and 0.2.7 users stay on 0.2.7 until then.

> ⚠️ **`cmake/AppVersion.cmake` is shared, and the two platforms stay DECOUPLED.** Line 8's `APP_VERSION`
> is **Windows**; the `if(APPLE)` override below it is **mac** — never touch the mac line from Windows work,
> and never collapse them into one version. On `main` the override still reads `0.1.10`, but the mac team has
> moved to **mac `0.2.0`** (multi-view + PIP + TV Guide parity) on the unmerged branch
> `origin/mac-multiview-tvguide`, which rewrites that block. **Their merge will conflict with this bump on
> line 8** — resolve by keeping BOTH: `APP_VERSION "0.2.8"` (Windows) *and* their `if(APPLE) "0.2.0"` (mac).

**Why.** 0.2.7 registers the wake task — but Windows only **arms** the underlying RTC wake timer when the
active power plan's "Allow wake timers" (`GUID_ALLOW_RTC_WAKE`) permits it, and **that setting is stored PER
POWER SOURCE**. The dev box (and a great many real laptops — the same `AC=0x1 / DC=0x0` shows up verbatim in
Microsoft's own Q&A on this symptom) reads **AC = Enable, DC = Disable**. So an unplugged laptop **silently
misses every scheduled recording**: the task looks healthy in Task Scheduler, `StartWhenAvailable` runs it
only when the user next wakes the PC themselves, and nothing warns anyone. Worst failure mode a DVR has.

- **`common/core/PowerPolicy.{h,cpp}` (NEW, pure, mac-safe, selftested)** — `decideWakePolicy(inputs)` maps
  (`rtcWakeCapable`, `hasBattery`, `onBattery`, `ac`, `dc`) → `{willWake, reason, otherSourceBlocked}`. It
  returns a **reason enum, never UI copy** (the mac app compiles this file). `Unknown` indices **fail OPEN** —
  never nag a machine whose policy we couldn't read. **Invariant:** `discharging = onBattery` and
  `hasBattery = in.hasBattery || in.onBattery` — *you cannot run on battery without having one*; trusting a
  gauge that says otherwise would read the AC index and promise a wake the DC policy is about to refuse.
- **`Win32/platform/PowerPolicy.{h,cpp}` (NEW)** — `queryWakePolicy()` probes `PowerGetActiveScheme` +
  `PowerRead{AC,DC}ValueIndex` on `GUID_ALLOW_RTC_WAKE` (GUIDs defined **locally**: `winnt.h` only *declares*
  them and no lib exports them), `GetSystemPowerStatus` for the source, and `CallNtPowerInformation(
  SystemPowerCapabilities)` for `SystemBatteriesPresent` + RTC capability. `wakeVerdictText()` renders the copy.
  Links `powrprof.lib` via `#pragma comment` (house style; **present on ARM64**, link-verified).
  - ⚠️ **The `AoAc` guard is load-bearing.** A **Modern Standby** machine reports
    `RtcWake == PowerSystemUnspecified` (no S1–S3 states) *even though timer wakes work fine* through the S0
    resiliency phase. Verified on this box: `AoAc=1, RtcWake=0`. So `rtcWakeCapable = caps.AoAc ||
    caps.RtcWake != PowerSystemUnspecified`. **Drop the `AoAc` term and every modern laptop is told, wrongly,
    that it cannot wake.**
  - **Not** on the ~30 s tick — it's a registry + capability read. Called on dialog open and the Settings toggle.
- **Surfaces** — an accent warning **banner** atop Settings ▸ Scheduled Recordings… (`ScheduleManagerCallbacks::
  wakeWarning`, pre-rendered by the host so `Dialogs.cpp` stays pure UI), and a status line when
  **Wake this PC to record** is switched on. `manageSchedules` gained the `clampToWorkArea` it was missing.
  The banner's height is **measured** (`DrawTextW DT_CALCRECT|DT_WORDBREAK`) and the window grown to fit —
  an `SS_LEFT` static wraps then **clips**, and a hard-coded 52 px truncated the longest verdict (needs 57 px
  at the real 592 px width; review caught it, GDI measurement confirmed it). Never re-hard-code that height.
- **`runWakeTaskNow()`** (`WakeScheduler.cpp`, reusing its COM RAII + `openRootFolder`) → `ITaskFolder::GetTask`
  + `IRegisteredTask::Run` (`AllowDemandStart` was already `VARIANT_TRUE`). New **Settings ▸ Run wake task now**
  (`ID_WAKE_RUN_NOW = 2065`, from the 2065–2069 gap; **never ≥2100**), greyed unless `wakeToRecord &&
  wakeTaskFor > 0`. **This is how you exercise the whole `--scheduled-wake` path without sleeping the machine** —
  which is the only way to test it on this VM at all.
- **Adversarial review (6 lenses × 3 refute-by-default skeptics): 2 confirmed, 2 refuted 0/3.**
  1. `BatteryFlag == 255` ("gauge unreadable") has bit 7 set exactly like 128 ("no battery"), so it was read as
     *no battery* → on a discharging laptop the AC index was consulted → the status bar **affirmatively lied**
     ("This PC will wake…"). Fixed in the pure core by the `onBattery ⇒ hasBattery` invariant, and in the probe
     by preferring `caps.SystemBatteriesPresent`. Pinned by 2 selftests.
  2. The banner clipped its last line (above).
- **New selftests (14)** — the full decision table: source selection, both blocking settings, the
  AC-enabled/DC-disabled mismatch flag, desktop ignores DC, no-RTC-capability, `Unknown` fails open, and the
  `onBattery ⇒ hasBattery` invariant.
- **Owner pass wanted:** the banner renders un-clipped and reads sensibly; Settings ▸ Run wake task now fires
  the scheduled-wake launch (minimised, no focus steal, picks up the queued row); toggling wake-to-record on an
  unplugged laptop shows the "wake timers are off" warning.

✅ **0.2.7 SHIPPED** (2026-07-10) — tag `v0.2.7` @ `cc9f9e4`, `0.2.7.184`, three signed installers on GitHub
release `v0.2.7`, two appcasts LIVE @ `438c83d`. Code landed in one commit (`cc9f9e4`, count 184 == the tag,
no rebase drift), appcasts in a follow-up (`438c83d`); both pushed. **Recording Phase 3** (owner-directed):
the DVR-completing feature — recordings now fire when the app ISN'T running, and a series records itself.
**Owner runtime pass still owed** (see the current-state header).

- **✅ Wake-to-record** — `Win32/platform/WakeScheduler.{h,cpp}` (new): a Task Scheduler 2.0 COM wrapper
  registering ONE user task, "**RabbitEars Recording Wake**" (root folder), with `WakeToRun` +
  `StartWhenAvailable` + no battery stops, whose action is `RabbitEars.exe --scheduled-wake`. It is
  create-or-updated to fire at `earliestPendingStart − kWakeLeadSeconds (120 s)`, clamped to `now+30 s`
  so a past boundary can't make Task Scheduler fire instantly against the single-instance mutex.
  `syncWakeFromSchedules(st)` is the choke point (tick, EPG refresh, rule add/enable/delete, the
  Settings toggle) **and runs at `WM_DESTROY`** — registering the task on the way out is the entire
  point. Settings ▸ **Wake this PC to record** (`wake_to_record`, default ON) clears the task when off.
  Also `setRecordingKeepAwake()` (`ES_CONTINUOUS|ES_SYSTEM_REQUIRED`, screen may still sleep) is
  re-derived from "any pane recording?" by `syncKeepAwake(st)` on every record start/stop + tick.
  **Limits (deliberate, documented in the header):** wakes from SLEEP only (not hibernate/shutdown —
  firmware wake timers, not ours); needs the user logged on (interactive token ⇒ no elevation to
  register); Windows power settings can disable wake timers, in which case `StartWhenAvailable` runs it
  at the next boot instead.
- **✅ `--scheduled-wake` launch path** — `wWinMain` now reads `lpCmdLine` (the app's only flag) and
  `runApp(hInst, nCmdShow, scheduledWake)`. Three behaviours: (1) an already-running instance is NOT
  yanked to the foreground; (2) the window comes up `SW_SHOWMINNOACTIVE`; (3) **the post-update Terms
  re-prompt is deferred** when a PRIOR version was accepted — otherwise an auto-update would silently
  break every scheduled recording until the user next opened the app. It does NOT write `tos_accepted`,
  so the next interactive launch still prompts; a first-ever run (no acceptance on record) still gates.
  `runApp` also calls `onSchedulerTick` once immediately (the first `WM_TIMER` is 30 s out — a woken
  machine has a deadline).
- **✅ EPG-driven series rules** — schema **v5**: `recording_rules` + `scheduled_recordings.rule_id`
  (deliberately NOT an FK: `deleteRule()` drops only the rule's still-**Pending** rows and keeps the
  Done/Missed/Cancelled history). New model `common/models/RecordingRule.h` + the **pure, headless-tested**
  `common/core/RecordingRules.{h,cpp}`: `expandRules(rules, programmes, existing, now, horizon)` matches on
  the normalised base tvg-id (`@feed` stripped, case-folded — `normaliseTvgId`, now shared) + `Exact`/
  `Contains` title match, applies lead/trail padding, and **dedups by (channel, paddedStart) against ALL
  existing rows whatever their status** — so a Cancelled or Done airing is never resurrected, and two rules
  matching one airing collapse to one recording. Horizon `kRuleHorizonSeconds` = 14 days. The Win32 side
  (`expandRecordingRules`) only resolves each match to a playable stream (the core has no DB) and inserts;
  it runs on the tick, after an EPG refresh, at startup, and on rule add/enable.
- **✅ UI** — the TV-Guide programme popup gains a 4th button **"Record series"** (`ProgrammeAction::RecordSeries`
  → `GuideCallbacks::onRecordSeries` → `recordSeriesFromGuide`, which refuses an exact duplicate rule and
  reports how many airings it queued). New **Settings ▸ Recording Rules…** manager (`manageRules` in
  `Dialogs.cpp`, modelled on `manageSchedules`): Show / Channel / Match / State columns, Enable-Disable,
  Delete (with a confirm that says pending recordings will go and history will stay). New ids
  `ID_WAKE_RECORD=2063`, `ID_RULES=2064` (from the free 2063–2069 gap; 2074–2098 went to 0.2.6, and
  **never allocate at/above 2100** — the open-ended skin range).
- **New selftests**: expander (match/normalise/horizon/padding/dedup/degenerate), rule DAO round-trip +
  `deleteRule` history-preservation, and the **v4→v5 migration** (`recording_rules` created, `rule_id`
  added, pre-v5 rows read back as `ruleId==0`).
- **Adversarially reviewed — 2 CONFIRMED bugs (one root cause) + 2 self-caught, all fixed:**
  1. **(root, medium)** `onSchedulerTick` had a pre-existing `if (schedules.empty()) return;` and the new
     Phase-3 tail was appended BELOW it — so with an empty queue, rules never re-expanded and a stale wake
     task was never cleared (a still-enabled series would silently stop recording; the PC would wake for a
     recording that no longer existed). **The early return is now gone** — `planScheduler({})` yields empty
     plans, so the loops no-op. **Never re-add that early return.**
  2. **(low)** the Scheduled-Recordings manager's Cancel/Delete didn't refresh the wake task (the Rules
     manager did) → both now call `syncWakeFromSchedules`.
  3. **(self-caught, perf)** removing the early return made `syncWakeFromSchedules` hit COM on every ~30 s
     tick → it now keys on `AppState::wakeTaskFor` (the UNCLAMPED earliest pending start; `0` = no task,
     `-1` = never synced) and only re-registers when the target actually changes. Likewise
     `expandRecordingRules` is throttled to `kRuleExpandIntervalSeconds` (15 min) — its 14-day, all-playlist
     EPG query is far too heavy for the tick — and is `force`d by a guide refresh / new / re-enabled rule.
  4. **(self-caught, correctness trap)** with the tail always running, **hard-deleting** a rule-generated
     *Pending* row left no dedup anchor, so the next expansion recreated it (the user's delete undid itself).
     The manager's Delete now **Cancels** such rows instead — a `Cancelled` tombstone is exactly what tells
     the rule "skip this airing". One-off rows, and rows that already ran, still delete for real.
     **Consequence to remember: `expandRules` dedups against ALL statuses, so tombstones are load-bearing.**
  5. `expandRules` also now folds each programme's title/channel-id **once** instead of once per rule
     (it was O(rules × programmes) allocations on a large guide).
- **Owner runtime pass — PARTIAL (2026-07-10):** ✅ scheduled recording works. ⚠️ wake-from-sleep is
  **untestable on the dev box** (Parallels ARM64 VM, S0-only, host suspends the guest) — needs real hardware;
  use **Settings ▸ Run wake task now** (0.2.8-dev) to exercise everything except the sleep itself. Still
  unexercised: "Record series" from the guide surviving a refresh; Recording Rules… enable/disable/delete;
  **and that an auto-update to 0.2.7 doesn't break a queued recording** (the Terms-deferral path).

✅ **0.2.6 SHIPPED** (2026-07-10) — tag `v0.2.6` @ `4382395`, `0.2.6.180`, three signed installers on GitHub
release `v0.2.6`, two appcasts LIVE @ `b2b7180`. Code landed in one commit (`4382395`, count 180 == the tag,
no rebase drift), appcasts in a follow-up (`b2b7180`); both pushed. **This time the shipped build number matches
the tag's commit count** (`main` was level with origin at cut time). The owner-directed "do all of these" batch —
every feature below built green on x64 BOTH theme flags + selftest ALL PASS + native ARM64, all three installers
compile, and the batch was **adversarially reviewed**: 3 CONFIRMED regressions in the per-pane recording change +
1 plausible export
nit, **all four fixed** — (1) the Scheduled-Recordings manager's Cancel/Delete stopped the ACTIVE pane's
recorder instead of the pinned pane's (could cut a user's manual recording and leave the scheduled one
running unowned) → shared `stopScheduledRecorder(st)` helper (tick + manager); (2) the Record glyph read
`isRecording()` right after the async stop ENQUEUE (still true) and stuck on Stop — the next "stop" click
would silently START a recording → glyph forced to Record when the stopped recorder is the active pane's;
(3) plan.stop's active-pane fallback could kill an unrelated manual recording when the pin went stale → a
gone pinned pane now stops nothing (fallback only for pin-less pre-0.2.6 rows); (4) favourites export now
flushes + checks before the success dialog (close() swallows flush failures). NB: the review's UI lens +
one verifier died on a subagent spend limit — that lens was re-covered by a careful inline pass (restore
ordering, PIP resize state machine, id math, guide scroll; nothing further found). **Owner runtime pass still
owed** (see the current-state header for the checklist).

- **✅ Concurrent per-pane recording.** The engine was already per-pane (each pane's `VlcPlayer` owns its own
  `rec_`); the UI now matches: the Record button toggles the **ACTIVE pane's** recorder (glyph follows pane
  switches via `setActivePane`), N panes record at once, a scheduled recording **pins to the pane it started
  on** (`AppState::schedulePane`) and only blocks manual record there, and `applyViewMode` **confirms before
  killing background (pane>0) recordings** (MessageBox; closes a scheduled one's DB row as Done). Scheduler
  still runs one scheduled recording at a time (`planScheduler` unchanged).
- **✅ MP4 recording — direct mp4 mux** (no ts+remux: libVLC's mp4 mux finalizes the moov on stop, and every
  stop path — manual/scheduled/quit — goes through `doRecordStop`). Settings ▸ Recording format gains MP4;
  `formatToExtMux` (Commands.cpp) is the single ext/mux mapping; the load whitelist accepts "mp4". Caveat
  (documented, accepted): a hard crash mid-record leaves an unplayable .mp4, unlike .ts.
- **✅ View mode + PIP persistence + PIP resize.** `view_mode` persists in `applyViewMode` (the choke point)
  and restores in WM_CREATE; `pip_pos` saves on drag-release and restores AFTER `applyViewMode` (which resets
  `pipMoved`); `positionFloatingPip` clamps a stale pos into the client. **PIP resize**: drag the bottom-right
  dp(18) corner (`resizingPip`), routed through `layout()` (`applyUserPipSize` clamps dp(120)…60% of region),
  EFFECTIVE size persisted as `pip_size` from `paneBounds`. Cursor feedback only on an empty PIP (vout hosts
  don't forward WM_SETCURSOR) — the drag itself always works.
- **✅ TV Guide "Show in Guide".** Right-click a grid channel ▸ Show in TV Guide → `epgGuideShowChannel(tvgId,
  now)` (EpgGuideControl export): clears the type-to-search filter, matches the row on the **normalised base
  id** (@feed stripped + case-folded, same as `onEpgGuide`'s join), top-aligns the row + re-centres on "now".
  Builds the guide first via the `epgGuideOpen() ? … : onEpgGuide(st)` pattern; explains via status if the
  channel has no row.
- **✅ Resume last channel** (Settings toggle, default ON, `resume_last`): WM_CREATE auto-plays
  `last_channel_id` via the new `Database::channelById` (nullopt-safe if deleted). Win32 deliberately
  AUTO-PLAYS (the mac port only highlights).
- **✅ Named saved layouts** (Settings ▸ Layout ▸ Save layout as… / Apply / Delete): `promptText` names it;
  stored as `layout_saved_<name>` + a `layout_names` newline index; cap 10 (menu ids 2079–2098 — NEVER
  allocate at/above 2100, the open-ended skin range).
- **✅ Import/export favourites** (Settings): export = the new **`common/core/M3uWriter`** (symmetric with the
  parser, CRLF, quote-degrading escaping; **selftest round-trip coverage**) via the app's first
  `GetSaveFileNameW`; import parses any M3U and stars library channels matching by **exact stream URL, then
  tvg-id** (all matching rows, de-duped), with an import-results info dialog. `Database::channelById` +
  selftests also new in `common/` (mac-safe).
- **✅ Slow first startup FIXED for x64 installs — install-time `vlc-cache-gen`** (the VLC-installer approach;
  supersedes the CI-generation idea, whose zip-mtime/timezone skew would go stale): vendored
  `third_party/vlc-tools/x64/vlc-cache-gen.exe` (3.0.23, **byte-identical libvlccore** to our NuGet — see the
  README there), shipped + run by `installer.iss` post-install **gated `IsX64Native`** (never under ARM
  emulation — it silently writes an EMPTY cache ⇒ libVLC loads 0 plugins ⇒ no playback), `[UninstallDelete]`
  cleans `plugins.dat`. CI verifier `.github/workflows/plugins-cache-verify.yml` proves the exe+NuGet produce
  a valid cache on a native-x64 runner (auto-runs when the vendored tool changes; also workflow_dispatch).
  ARM64 cache stays backlogged (native scan ~3s, no ARM64 cache-gen exists).
- **Authenticode** — scaffolded in `docs/RELEASING.md` (signtool recipe, sign exe→installer→EdDSA order);
  **owner-gated on a cert purchase**.
- **Owner runtime pass wanted:** concurrent recording (record in 2 panes at once; switch panes mid-record;
  the mode-switch confirm), MP4 recording plays back, PIP resize + position/size/mode surviving a restart,
  resume-last on launch, Show in Guide, saved layouts, favourites export→import round-trip, and (on an x64
  machine or after the next x64 release) the faster cold start from the installed `plugins.dat`.

✅ **0.2.5 SHIPPED** (2026-07-09) — tag `v0.2.5` @ `fbebcc7`, `0.2.5.168`, three signed installers on GitHub release
`v0.2.5`, two appcasts LIVE @ `5c7073e`. The code landed in one commit (`d9b0840`, rebased to `fbebcc7` onto the mac
team's v0.1.10), the appcasts in a follow-up (`5c7073e`); both pushed. The 0.2.5 feature set below is the changelog.
**✅ Universal installer + About-box arch readout owner-verified in the wild** —
`RabbitEars-0.2.5-universal-setup.exe` on the ARM device installed the **native ARM64** build, it runs, and its
**About box reads `ARM64`** (so the install-time `ProcessorArchitecture = paArm64` Check + the `IsWow64Process2`
arch readout both work on real hardware). **Runtime pass still owed** (sandbox can't launch the GUI): the
Check-for-Updates chooser appears + points at the right feed, and a live `0.2.4 → 0.2.5` auto-update lands.

- **✅ Favourite-a-channel from the TV Guide — DONE, owner-verified.** Right-click a channel row in the guide
  → "★ Add to Favourites" / "Remove from Favourites". `GuideCallbacks` gained `onToggleFavourite` + `isFavourite`;
  `EpgGuideControl`'s `onGuideContextMenu` (new `WM_RBUTTONUP`) builds the menu; `onEpgGuide` wires it
  (`Database::channelByTvgId` → `toggleFavourite` → `loadForFilter` to refresh the grid). Rows with no tvg-id can't
  be favourited.
- **✅ Native ARM64 build SHIP-READY — perf-verified + auto-update wired + installer builds clean (0.2.5).** The
  owner runs Windows-on-ARM. `scripts/build-arm64.cmd` (native `vcvarsarm64`, output in `build-arm64/`) builds a
  genuine **PE32+ ARM64** exe with **WinSparkle auto-update LINKED** (dumpbin confirms the import; not stubbed).
  **Perf (owner-run, this session):** native cold-start **~3.4 s vs ~13 s emulated (~4×)** and playback **~6% of one
  core vs ~28% emulated (~4.4×)** on the same HLS stream — decisive; owner: "arm build works perfectly."
  **Auto-update wiring:** WinSparkle **0.9.3 vendored for BOTH arches** (`third_party/winsparkle/{lib,bin}/{x64,arm64}`
  — the old flat `lib/WinSparkle.lib`+`bin/WinSparkle.dll` were moved into `x64/`); `Win32/CMakeLists.txt` picks the
  slice by **`CMAKE_CXX_COMPILER_ARCHITECTURE_ID`** (mirrors `cmake/LibVlc.cmake`, which picks `build/arm64` vs
  `build/x64` the same way — **NOT `CMAKE_SYSTEM_PROCESSOR`**, the HOST, `ARM64` even for the x64 build). `RABBITEARS_UPDATER`
  now defaults **ON for both** (`Updater.cpp` still stubs under `#ifdef RABBITEARS_HAVE_WINSPARKLE` if forced OFF).
  **Packaging = Option B (per-arch installers, owner-chosen):** `packaging/installer.iss` is arch-parameterized
  (`/DSrcDir /DOutSuffix /DArchAllowed`, x64 defaults byte-identical to before); `scripts/build-installer.cmd [arm64]`
  builds either — **both build clean** (`RabbitEars-0.2.5-setup.exe` x64 35 MB + `RabbitEars-0.2.5-arm64-setup.exe`
  30 MB, ISCC 6.7.3, arm64 sourced from `build-arm64/`). Per-arch auto-update: the arm64 build reads
  **`appcast-arm64.xml`** (`Updater.cpp` keys on `_M_ARM64`), x64 keeps `appcast.xml` — an ARM user always gets the
  native build; x64 users' feed is untouched. `scripts/make-appcast.ps1 -Arch arm64` writes the arm64 feed; the
  two-arch release flow is in `docs/RELEASING.md`. **REMAINING to ship:** cut 0.2.5 for both arches (build both
  installers → sign both on the Mac → one GitHub release with two assets → populate both appcasts). Optional/minor:
  an ARM64 `plugins.dat` (native scan is already ~3 s, so low value now). After editing shared CMake, **always
  re-verify the x64 build BOTH flags** (arch bugs only show at link). **Gotcha fixed:** `scripts/build-arm64.cmd`
  had **LF-only line endings** → cmd.exe mis-parsed it (`REM` stopped suppressing comments, a `(`-led comment turned
  fatal); rewrote it CRLF. **Keep `.cmd` scripts CRLF** — now enforced in `.gitattributes` (`*.cmd/*.bat text eol=crlf`,
  a review-caught durable fix so git checkouts don't re-introduce LF).
- **✅ Universal installer + About-box arch readout + ARM64 update chooser (0.2.5, owner-requested).**
  (1) **Universal installer** — `scripts/build-installer.cmd universal` (`/DUniversal` in `installer.iss`) bundles BOTH
  binary sets and installs only the NATIVE arch at install time (Inno `[Code] ProcessorArchitecture = paArm64` Check);
  the on-disk result matches a per-arch install, so it **reuses the per-arch feeds with no extra appcast/launcher**
  (`RabbitEars-0.2.5-universal-setup.exe`, ~63 MB). (2) **About box** shows the running architecture — `ARM64` / `x64`,
  and `x64 (emulated on ARM64)` — via `IsWow64Process2` (`Win32/ui/Dialogs.cpp` `runningArchLabel()`). (3) **Check for
  Updates on ARM64 hardware** pops a TaskDialog (native ARM64 recommended / x64 emulated) and points WinSparkle at the
  chosen arch's feed (`Win32/platform/Updater.cpp` `chooseUpdateArch`/`machineIsArm64`); the feed reverts to the build's
  native default after each check via the did-find/did-not-find/cancelled callbacks (review-caught, so the per-check
  choice can't leak into a later check). NB: WinSparkle compares by VERSION, so the chooser selects the update *channel*
  — it doesn't force a same-version arch swap. All three build clean on both arches, both theme flags, selftest green;
  adversarially reviewed (0 confirmed, 2 plausible both fixed).
- **⏳ Slow first startup (`plugins.dat`) — DIAGNOSED, DEFERRED → `BACKLOG.md`.** libVLC never auto-writes
  `plugins.dat`; it rescans all 323 plugins each launch; "fast vs slow" is OS-file-cache warmth. The fix
  (`vlc-cache-gen`) is blocked from this ARM-emulated agent context — the x64 `vlc-cache-gen` under emulation makes
  an EMPTY 24-byte cache (even for VLC's own plugins), which must NEVER ship (libVLC would read 0 plugins → no
  playback). Needs a **native x64 env / CI** (or the ARM64-native path) to generate + verify. `vlc-cache-gen.exe`
  (3.0.23, byte-matching our NuGet `libvlccore`) is in the session scratchpad, not vendored.

0. **✅ 0.2.3 SHIPPED** (2026-07-08) — tag `v0.2.3` @ `d6ad80a`, `0.2.3.162`, appcast @ `ca7b682`, auto-updating
   from 0.2.2. The multi-view fix batch below is live and owner-runtime-verified (audio follows the active tile;
   video-only/fullscreen + focus; single-collapse keeps the selection; no mode-switch hang). **Next up: 0.2.4
   (item 1) — the "VLC (Direct3D11 output)" popout on rapid channel-surf.** The bullets below are the 0.2.3
   changelog.
   - **Multi-view mode-switch HANG — FIXED + owner-verified.** `applyViewMode` tore panes down with a blocking
     `player.shutdown()` on the UI thread → a stuck stream's libVLC `stop()` froze the UI (Windows `AppHangB1`).
     Now async: `VlcPlayer::beginTeardown()` hands the blocking stop to a reaper + joins only the worker; the
     pane parks in `AppState::dyingPanes`, and `reapDyingPanes()` reaps it once its stop finishes (each mode
     switch, the ~30 s scheduler tick, force-drained at WM_DESTROY before `engine.shutdown()`).
   - **Recorder teardown no longer blocks the mode switch — review-caught, FIXED (runtime-verify).** The hang fix
     above offloaded only the *playback* stop; the *recorder* (`rec_`) was still stopped **synchronously** on the
     joined worker (`Cmd::Quit → doRecordStop()`), so recording a stuck feed into a **background split tile** and
     then switching modes re-froze the UI — a residual hole in the "UI never blocks on a mode switch" invariant (not
     a regression vs 0.2.2, which blocked on both). `doRecordStop(bool async)` now hands the recorder's blocking
     `stop()/release()` to the same reaper vector, symmetric with `doStop(async)`; `beginTeardown()` enqueues an
     async recorder-stop (a `Cmd::RecordStop` with `ivalue=1`) between the playback stop and the quit;
     `teardownComplete()`/`shutdown()` already drain the whole reaper vector, so no lifecycle change. Manual
     stop + shutdown stay synchronous. Built BOTH theme flags + selftest green. **Verify at runtime:** Split →
     record into a *background* (non-active) tile → switch view modes → no hang, the `.ts` finalizes cleanly.
   - **Video-only / fullscreen shows the 2×2 grid + clickable tile focus — owner-verified.** `layout()`'s
     fullscreen/video-only branch (`MainWindowChrome.cpp`) tiles the panes per view mode across the whole client;
     the active-pane border paints in these modes; and a click on a tile in video-only now **activates** it
     (`VideoProc` previously only armed the window-drag and never called `setActivePane`; a real drag still moves
     the window). Owner: "video only and full screen work perfectly."
   - **Multi-view audio → only the active pane — TRACK-BASED mute, owner-verified.** First tried `volume=0` for
     background panes, but libVLC 3.x **resets a player's output volume to 100% whenever it recreates the audio
     output** (an HLS low→high quality switch, no event fired), so a volume mute leaked and *pulsed* ("jumpy") on
     adaptive feeds — every command returned `rc=0` yet audio leaked. Fixed by **deselecting a background pane's
     audio track** (`libvlc_audio_set_track(mp,-1)`): a pane with no audio ES has no aout to reset.
     `VlcPlayer::setMuted`/`applyAudioState` (worker) apply it on the Playing transition + re-assert each 250 ms
     poll; the saved track id is validated against the live stream and reset on `doPlay`, so a channel change
     can't strand a pane silent. Callers (`addPane`/`playChannelInPane`/`setActivePane`) mute via
     `setMuted(i!=active)`; the active pane keeps its track + volume slider. A 4-lens adversarial review caught +
     fixed two edge bugs pre-ship (force-selecting the first track over libVLC's preferred audio language; a stale
     track id silencing a pane). Owner-verified: audio follows the active tile through quality ramps. (Known edge:
     one channel's quirky audio ES needed a channel re-select — stream-specific, self-heals.)
   - **Single-collapse keeps the selected stream — owner-verified.** Leaving Split/PIP no longer snaps to the
     top-left tile: `applyViewMode` captures the active pane's channel before teardown and replays it into the
     persistent pane 0 (log-confirmed).
   - **Active-pane highlight → only the active pane — owner-verified.** `setActivePane` now `InvalidateRect(…,
     TRUE)` so the gap-drawn border erases before the new one paints (WS_CLIPCHILDREN keeps the gap-fill off the
     video, no flicker).
1. **✅ 0.2.4 SHIPPED** (2026-07-09) — tag `v0.2.4` @ `f30fcc2`, `0.2.4.166`, appcast @ `6e2b4ae`,
   auto-updating from 0.2.3. **"VLC (Direct3D11 output)" popout FIXED**, owner-surf-verified. Rapid
   channel-surf reused the pane HWND while the old stream's D3D11 vout (async reaper) still owned it → libVLC
   spawned its own output window (the 0.1.3 "two vouts share the HWND" note). Fixed with a **per-pane self-cleaning
   pool of vout-host child windows** (`kVoutHostClass`): libVLC renders into a host, never the pane HWND; a new
   stream attaches to a **proven-free** host (selected by the reaper's done-flag, NOT parity — an initial design
   review showed a fixed 2-slot ping-pong still recurs under ≥2 stuck reapers), the old stream keeps its host until
   its reaper releases it, then it returns to the free set. `VlcPlayer` owns the pool + `Reaper.host` association +
   `currentHost_`; hosts are created/sized/shown/hidden on the UI thread (`makeVoutHost`, `WM_APP_MAKE_VOUT_HOST`),
   grown on demand via `SendMessageTimeout` (deadlock-safe vs `worker_.join()`); `VoutHostProc` forwards clicks to
   the pane so activate/drag/dblclick/menu still work; the host is shown on `PlayerEvent::Playing` (no black gap).
   Also in 0.2.4: **TV Guide reopen** (an `NM_CLICK` handler reopens the guide when its already-selected node is
   re-clicked — `TVN_SELCHANGEDW` misses that) + **instant reopen** (`revealEpgGuide` re-reveals the built guide
   without a DB rebuild); **"Video only" from fullscreen** now drops into windowed video-only instead of no-op.
   Both flags + selftest green, adversarial review clean, owner-surf-verified (no popout on healthy + dead-feed
   surfing). **Backlogged from the surf-testing:** the slow first startup (libVLC rescans 323 plugins — ship a
   `plugins.dat`) → `Win32/BACKLOG.md`.
2. **MainWindow.cpp split — DONE** (`7656750`→`a2c0118`, both flags green): header + `rabbitears::mw` + 5 `.cpp`
   (core / chrome / dock / data / commands); 3283→~1425-line core. File map: memory
   `mainwindow-modularization-plan`. **0.2.2 SHIPPED** (`v0.2.2` @ `059b632`, `0.2.2.153`, appcast `fcdac10`;
   feed live + auto-updating). Next feature work after 0.2.3/0.2.4: multi-player polish — **concurrent recording**
   (each pane already has its own recorder), per-pane recording ownership, persist the view mode.
2. **Multi-player polish** — the engine EXISTS now, so build on `VideoPane` / `common/ui/VideoGrid` / the
   shared `VlcEngine`, NOT the old one-`VlcPlayer` assumption (memory `rabbitears-feature-roadmap`). The big
   unlock is **concurrent recording** (each pane's player already carries its own recorder); also per-pane
   recording ownership (today the manual + scheduled recorder follow the *active* pane), persisting the view
   mode across launches, and an optional PIP "always-on-top over other apps" toggle (`WS_EX_TOPMOST` now) + a
   resize grip. The **custom EPG-URL override** and **split/PIP** items from the old roadmap are DONE.
3. **macOS Phase-1** continues on `main`; keep `common/` green (the `mac-core` CI is the drift alarm) and
   **`git fetch`/rebase before every release** — `main` is shared (0.1.7's build count jumped 39→52 mid-cut
   from concurrent mac pushes). Aside: the mac `HANDOVER.md` is stale — PR #22 is merged; its "E3"
   `MeterModel`→`common/ui` promotion is now post-merge backlog.

## Seed prompt for a new session

Paste this verbatim to start a fresh session with working context restored:

> You are continuing **RabbitEars**, a native **Windows Win32 / C++20** IPTV player on **libVLC 3.0.23**
> with a shared **`common/`** core (also feeds the macOS app), dark "Claude-desktop" chrome (coral
> `#D97757`, custom `WM_NCCALCSIZE` title bar), CMake + Ninja + MSVC (VS 2026), deps vendored/NuGet.
> **Read `Win32/HANDOVER.md` first — the top "Current state" + "Immediate next steps" (the 0.2.8 block)
> — plus `Win32/BACKLOG.md` and the recalled memories.**
>
> **State: last SHIPPED = `v0.2.8`** (2026-07-11, tag @ `e45adb6`, `0.2.8.217`, appcasts @ `5404004`).
> `main` is clean. Everything shipped is LIVE and auto-updating (raw feeds serve 0.2.8.217, enclosures
> HTTP 200). 0.2.8 = **localization (EN + 日本語)** + wake-timer preflight + TV Guide loading box. i18n
> source of truth is `common/i18n/*.json` → `tools/i18n/gen_i18n.py` generates `common/core/Strings.*`
> (never hand-edit). **Japanese is a machine draft** — native review (`gen_i18n.py --review ja`) is the
> gate before advertising JP support.
>
> **The app.** `Win32/ui/VlcEngine` owns ONE shared libVLC instance across N `VideoPane`s (each = a video
> HWND + `VlcPlayer` + channel; `AppState` holds the vector + `active` + `ViewMode`). Single / Split (2×2
> child tiles) / floating `WS_EX_TOPMOST` PIP popup — the PIP **must** be a top-level window (a child
> sibling loses to libVLC's D3D vout). Each pane owns a **self-cleaning pool of vout-host child windows**
> (libVLC renders into a proven-free host, never the reused pane HWND). Multi-view audio = **track-based**
> mute (`libvlc_audio_set_track(mp,-1)` for background panes; a volume mute leaks through HLS quality
> switches). `MainWindow.cpp` is a header + `rabbitears::mw` + 5 `.cpp` (memory `mainwindow-modularization-plan`).
>
> **Shipped highlights.** 0.2.0 theme engine (4 skins + HLSL effects). 0.2.1 EPG/TV-Guide + scheduled
> recordings + multi-view. 0.2.4 vout-host pool. **0.2.5** native **ARM64** (~4× faster than emulated) with
> **per-arch auto-update** (x64 reads `appcast.xml`, ARM64 reads `appcast-arm64.xml` — `Updater.cpp` keys on
> `_M_ARM64`) and **three installers** (x64 / arm64 / **universal**, which installs the native arch via an
> Inno `ProcessorArchitecture` Check and needs no feed of its own). **0.2.6** concurrent **per-pane
> recording** (the Record button drives the ACTIVE pane; a schedule pins to `AppState::schedulePane`), MP4,
> PIP resize + view-mode/PIP persistence, Show-in-Guide, resume-last-channel, saved layouts, favourites
> import/export (`common/core/M3uWriter`), and the **x64 install-time `plugins.dat`** (vendored
> `third_party/vlc-tools/x64/vlc-cache-gen.exe`, run post-install gated `IsX64Native` — **NEVER under ARM
> emulation: it silently writes an EMPTY cache ⇒ libVLC loads 0 plugins ⇒ no playback**).
>
> **0.2.7 = Recording Phase 3 (DVR foundation).** `Win32/platform/WakeScheduler.{h,cpp}` registers ONE Task
> Scheduler task ("RabbitEars Recording Wake", `WakeToRun`+`StartWhenAvailable`) running
> `RabbitEars.exe --scheduled-wake` at `earliestPendingStart − 120s` (clamped to `now+30s`).
> `syncWakeFromSchedules(st)` is the choke point (tick / guide refresh / rule change / Settings toggle)
> **and runs at `WM_DESTROY`** — registering the task on the way out is the whole point; it keys on
> `AppState::wakeTaskFor` so an unchanged queue costs no COM. `syncKeepAwake(st)` suppresses sleep while any
> pane records. `--scheduled-wake` = no focus steal, `SW_SHOWMINNOACTIVE`, and it **defers the post-update
> Terms re-prompt** when a prior version was accepted (without writing `tos_accepted`) — otherwise an
> auto-update silently kills every queued recording. **Series rules:** schema **v5** (`recording_rules` +
> `scheduled_recordings.rule_id`, deliberately not an FK — `deleteRule()` drops only Pending rows, keeps
> history) + the pure `common/core/RecordingRules::expandRules()`; "Record series" in the guide's programme
> popup; Settings ▸ Recording Rules… manager.
>
> **Two 0.2.7 invariants you must not break** (both review-caught): (1) `onSchedulerTick` has **no
> empty-queue early return** — the tail (rule expansion, keep-awake, wake-task upkeep) must run even with an
> empty queue, or a series silently stops recording and a stale wake task never clears. (2) **Tombstones are
> load-bearing:** `expandRules` dedups against existing rows of **ANY** status, so the schedules manager
> *cancels* (never hard-deletes) a rule-generated Pending airing — a hard delete would let the expander
> resurrect it. Both are pinned by selftests.
>
> **0.2.8 = localization + wake preflight + guide loading box (SHIPPED, newest).**
> **Localization (English + 日本語).** SOURCE OF TRUTH = JSON under `common/i18n/` (`languages.json` +
> `keys.json` + one `<code>.json` per language); **`tools/i18n/gen_i18n.py`** generates the pure catalog
> `common/core/Strings.{h,cpp}` — **NEVER hand-edit Strings.\***; edit the JSON, run the generator, commit
> both. Win32 call sites use `Win32/ui/Tr.h`: `tr(StringId)`→`std::wstring`, `trf(id,{wideArgs})` fills
> `{0}`,`{1}`… (UTF-8→wide at the boundary). Enum-indexed table ⇒ "every key in every language" is compile-
> time enforced; drift guarded by `gen_i18n.py --check` (in `windows-core.yml` CI) + the CLI selftest
> (completeness + placeholder parity). System default via `GetUserDefaultUILanguage`, read at `WM_CREATE`
> **before** `createChildren` **and** before the splash thread — `i18n::g_lang` is a **relaxed
> `std::atomic`** (the splash worker reads it). Settings ▸ Language ids **2066–2068**; restart-to-apply via a
> themed TaskDialog that self-relaunches with **`--restart`** (waits on the single-instance mutex for the old
> instance to exit, instead of bouncing). **Gear-icon** Settings button (`kGlyphSettings` MDL2 U+E713) +
> regrouped menu. **CJK font:** `themeFontFamily()` (the single GDI+DirectWrite choke point) → **Yu Gothic
> UI** when Japanese; the symbol/glyph role is exempt (gear/transport icons stay). **⚠️ Japanese is a machine
> draft** — `gen_i18n.py --review ja` emits an EN/JA side-by-side for a native pass (the gate before
> advertising JP support). Adding a language: drop a `<code>.json` + `languages.json` entry + wire
> `Tr.h`/menu — the generator refuses to build until every key is filled.
> **Wake-timer preflight.** 0.2.7's task registers, but Windows only **arms** the RTC timer if the power
> plan's `GUID_ALLOW_RTC_WAKE` allows it — **per power source** (`AC=Enable, DC=Disable` is a common laptop
> default ⇒ an unplugged PC silently misses recordings). Pure `common/core/PowerPolicy` (mac-safe, selftested;
> invariant *`onBattery` implies `hasBattery`*) + `Win32/platform/PowerPolicy` (powrprof probe) warn in the
> schedules manager + Settings toggle. **`AoAc` guard is load-bearing:** Modern Standby reports
> `RtcWake=Unspecified` yet wakes fine — drop it and every modern laptop is wrongly told it can't wake.
> **Settings ▸ Run wake task now** (`ID_WAKE_RUN_NOW=2065`) demand-starts the task so `--scheduled-wake` is
> testable *without sleeping*. The schedules-manager banner height is **measured** (`DT_CALCRECT`) — never
> hard-code it, it clipped.
> **TV Guide loading box.** The first `onEpgGuide` build is synchronous (reopen via `revealEpgGuide` is
> instant); it shows a **LOCAL** "Loading TV guide…" box (NOT `st->loadingDlg`, which is the async fetch's) —
> no `st->busy` guard (an early version's guard was a review-caught regression on Show-in-Guide).
>
> **Immediate next:** 0.2.8 owner pass — ✅ **language shift confirmed on-device**; the wake-preflight banner
> + guide loading box are low-risk, **delegated to testers**. Owed on 0.2.7 (real hardware): wake-from-sleep
> (untestable on this Parallels ARM64 VM, S0-only), "Record series", auto-update over a queued recording.
> Then pick from `Win32/BACKLOG.md`: the **native Japanese review** (release gate for advertising JP; the
> Terms-of-Use text especially), **more languages** / **live language switch** (i18n follow-ups); **series-rule
> follow-ups** (episode dedup via `Programme::episodeNum`/`subTitle`; a rule editor — `Contains` + lead/trail
> padding already exist in model/DB/expander, only the UI is missing); **transcoding on record**; a
> **background dead-link checker**; **Authenticode** (recipe in `docs/RELEASING.md`; blocked on the owner
> buying a cert — silences SmartScreen); portable-zip artifact; or the deferred **JSON profiles** epic.
>
> **Build/verify** (PowerShell): `& "<repo>\scripts\build.cmd" -DRABBITEARS_BUILD_GUI=ON -DRABBITEARS_THEME_ENGINE=ON`
> then `build\Win32\RabbitEarsCli.exe --selftest`. **Build BOTH theme flags** (ON and OFF) — flag-off must
> stay byte-identical to shipping. **ARM64:** `scripts\build-arm64.cmd` (native `vcvarsarm64`, output in
> `build-arm64\`). `cmake/LibVlc.cmake` + the WinSparkle slice pick the arch by
> **`CMAKE_CXX_COMPILER_ARCHITECTURE_ID`** (the MSVC **TARGET**) — **NOT `CMAKE_SYSTEM_PROCESSOR`** (the
> HOST, which is `ARM64` here even for the x64 build). **After any shared-CMake edit, re-verify the x64 build
> BOTH flags** — that arch bug only surfaces at link.
>
> **Gotchas.** `cmake`/`cl` are NOT on PATH — use `scripts\build.cmd`; outputs in `build\Win32\`.
> `LINK1168` = a running `RabbitEars.exe` locks the exe (the owner runs it — **ask them to close it, don't
> loop**). Static CRT (`/MT`, no redist). **The sandbox cannot launch the GUI** — build-verify + reason;
> the owner does every runtime/visual pass. `common/` must stay mac-safe (the `mac-core` CI compiles it on
> clang — no Win32 APIs). **`.cmd` scripts must be CRLF** (enforced in `.gitattributes`; LF made cmd.exe
> mis-parse `build-arm64.cmd`). Command ids: 2074–2098 used by 0.2.6, 2063/2064 by 0.2.7, **2065–2068 by
> 0.2.8** (2065 wake-run-now, 2066–2068 language; 2069 free) — **never allocate at/above 2100**
> (`ID_THEME_SKIN_BASE` is open-ended). libVLC `stop()`/`release()` block → offloaded to
> reaper threads; event callbacks → only `PostMessage`; `i_read_bytes` is 0 for HLS; VLC sout single-quoted
> paths need `'` doubled; modal dialogs must read their controls **before** `DestroyWindow`.
>
> **Process.** Every substantive change gets an **adversarial multi-lens review** (a background `Workflow`)
> + a both-flags build before it ships — it keeps catching real bugs (3 in 0.2.6, 2 in 0.2.7). **Commit only
> when the owner asks**; stage **specific paths** (never `git add -A` — the owner adds `art/*.png`); end
> commits with the `Co-Authored-By` trailer. `main` is shared with the mac team → **`git fetch`/rebase before
> pushing** (push the code commit early to lock the build number = git commit count, so the tag matches).
> Release flow (`docs/RELEASING.md`): commit+push → **force-reconfigure so `version.h` picks up the new
> commit count** (delete `build/generated/version.h`, rebuild — else the stamp lags HEAD by a commit) →
> rebuild both arches → 3 installers (`scripts\build-installer.cmd [x64|arm64|universal]`) → **owner signs on
> the Mac** (`sign_update --account SQLTerminal <exe>`, only EdDSA signing is there — the printed `length`
> must equal the local file's bytes exactly) → `pwsh scripts\make-appcast.ps1 [-Arch arm64] -Version
> <ver.build> -Tag v<ver> -SetupExe … -Signature …` → `gh release create v<ver> --target main …` with 3
> assets (a bare SHA `--target` 422s; use `main`) → commit+push both appcasts → verify raw feeds serve
> `<ver.build>` + enclosures HTTP 200. **`pwsh` (PowerShell 7) is now installed** (was absent for the 0.2.8
> cut, which fell back to `& .\scripts\make-appcast.ps1` under Windows PowerShell 5.1). `gh` CLI + Inno Setup
> are on this machine, so everything except signing runs locally. **v0.2.8 tag = `main`** (`e45adb6`, 217).
