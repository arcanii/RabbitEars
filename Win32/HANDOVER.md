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

## Current state — **0.2.17 IN PROGRESS on `main`** · v0.2.16 SHIPPED (2026-07-28) · macOS 0.2.15

### 🚧 0.2.17 — unreleased, `APP_VERSION` = `0.2.17`

Everything below is committed to `main` and **owner-verified on device** unless marked otherwise.
The theme is *"the library is ten times bigger than anyone assumed"* — every item traces back to the
0.2.16 pass discovering a real 411k-row library where the design assumed 44k.

| what | why |
|---|---|
| **v8 migration fix** (`aad57f5`) | five `ALTER`s shared ONE guard, so a partial failure latched `user_version=8` over a half-migrated table and every channel query then failed to prepare — **library silently empty, permanently**. Ships in v0.2.16 binaries. |
| **`canonicalStreamUrl()` + schema v9** (`22253b6`, `9eed4ff`, `986df48`) | the m3u writes `host:80`, the VOD sync builds `host` → the dedupe index saw two strings and **every film was stored twice**. v9 rewrites stored URLs and merges. Verified on a copy of the real DB: 454,195 → 410,596 rows, 0 duplicates, 14/14 favourites kept. ✅ ran on the owner's live DB. |
| **Grid row cap** (`04b4d22`) | All Channels 1485 ms → **108 ms**; search 1626 ms → ~134 ms **per keystroke**. The filters moved into SQL so the cap composes with them. ✅ "all channels appears instantly, search much more responsive". |
| **Skip back / forward** | ±10 s buttons flanking the scrub bar, same visibility as it. ✅ "works good, looks good". |

**Still to decide before a cut:** whether this is `0.2.17` or folds into `0.3.0` with series. `APP_VERSION`
is already bumped to `0.2.17`, and the bump commit must precede the installer build (build number =
commit count).

## Previous release — **v0.2.16 SHIPPED (2026-07-28)** · macOS 0.2.15

### ✅ 0.2.16 — SHIPPED (2026-07-28) — the Xtream VOD release

**Released:** tag **`v0.2.16`** @ `d83b002`, full version **`0.2.16.377`**; three installers on GitHub
release `v0.2.16` — x64 `35,370,751` / arm64 `30,218,567` / universal `63,289,025` bytes — **two
appcasts** (`0.2.16.377`) committed @ `0344b54` and LIVE (both feeds HTTP 200, and **both enclosure
URLs downloaded end-to-end with the byte count matching the signed length**, which is the check that
catches a bad `-Tag`). Owner-signed on the Mac; the two feeds cross-checked so the x64 and arm64
signatures cannot be swapped.

⚠️ **The pre-release verification returned NO-GO once, on the same class of blocker as 0.2.15** — the
docs commit `d83b002` had not reached the remote when the release was called for. `gh release create`
tags the REMOTE head, so building then would have stamped `0.2.16.377` while the tag pointed at
count 376. Caught by `git ls-remote origin refs/heads/main` before anything was built. **That check
has now saved two consecutive releases; run it before you build, not after.**

**✅ OWNER-VERIFIED ON-DEVICE before the cut** — the two things the sandbox cannot check:
- **The scrub bar is INVISIBLE on live TV and works on seekable content.** Confirmed on the real
  provider: absent on `NL - SPONGEBOB` (a 24/7 feed), present and tracking on a series episode
  (`0:49 / 43:30`). This was the release's top risk item and it closed as GO.
- **Glass bezel and the empty-tank fix: "look fine"** — clearing the one reservation carried over
  from 0.2.15 ("needs work — but ok for this release").

⛔ **What shipped UNVERIFIED, and it is the riskiest thing in the release: the VOD sync itself.** The
owner's Xtream line expired before a real sync could run, so insert + `retireMissingChannels` has
never executed against a live provider — only against selftests. Mitigated by design rather than by
luck: it is user-triggered, invisible without an Xtream playlist, gated on playback/recording, and
refuses to delete anything when too much of the response was unusable. **First real sync is still
outstanding — see "What still needs the owner".**

> ### ⚠️ Version note — the two PLANNED releases merged into one shipped 0.2.16 (owner's call, 2026-07-28)
>
> The plan was 0.2.16 = groundwork, 0.2.17 = the Xtream client. **0.2.16 was version-bumped but never
> tagged and never given an appcast, so no user ever had it** — so both halves shipped together as a
> single **0.2.16**. What merged is the *release plan*, not the version numbering: **0.2.17 is simply
> the next version, unused and available.** Practical consequences: no bump was needed for this cut
> (`APP_VERSION` already read `0.2.16`); and text elsewhere that says "0.2.17" where it means "the
> Xtream VOD work" predates this call — commit messages (`538f0b2`, `e4e01a7`) say it too and cannot
> be rewritten. Read those as *the VOD half of 0.2.16*.

> **`main` is clean and fully released.** `APP_VERSION` is `0.2.16` (`cmake/AppVersion.cmake:11`; the
> `if(APPLE)` override is untouched at `0.2.15`) — **the next release bumps it to `0.2.17`**, or to
> `0.3.0` if series lands first and you want the minor bump to mark the capability.
>
> The Xtream VOD epic's two gates are both CLOSED and **movies have SHIPPED**. The design doc —
> written off a REAL provider's measured answers, not estimates — is
> **[`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md)**; read it before touching the epic. **Series
> (0.3.0) is the remaining half**, and §1 already has the measured shape for it.

### ✅ 0.2.15 — SHIPPED (2026-07-26)

**Released:** tag **`v0.2.15`** @ `1324f5f`, full version **`0.2.15.365`**; three installers on GitHub
release `v0.2.15` — x64 `35,350,517` / arm64 `30,194,643` / universal `63,242,002` bytes — **two
appcasts** (`0.2.15.365`) committed @ `77035ed` and LIVE (both feeds HTTP 200 **and both enclosure URLs
verified HTTP 200**, which is the check that actually catches a bad `-Tag`). Owner-signed on the Mac;
every signature byte-length-verified against its file, the two appcasts cross-checked against each
other, and all three installer hashes confirmed unchanged across the round trip to the Mac.

⚠️ **The pre-release verification returned NO-GO first, on a real blocker** — worth reading before the
next release, because the failure was invisible and permanent. The last commit (`1324f5f`, docs only)
was **unpushed**, so `origin/main` was at commit count 364 while the binaries were already stamped
`0.2.15.365`. `gh release create` tags the REMOTE head, so the tag would have pointed at source that
did not contain what shipped, and anyone rebuilding from the tag would get `0.2.15.364` — a *lower*
WinSparkle version than the release. **Push before tagging, and verify `git ls-remote origin
refs/heads/main` matches HEAD.** (Also: push promptly — the mac team pushes to `main` too, and a
commit landing between the build and the tag moves the count and permanently mismatches the
already-built installers.)

`APP_VERSION` is **`0.2.15`** (`cmake/AppVersion.cmake:11`; the `if(APPLE)` override is separate and
untouched). The `0.2.15-dev` branch was merged and pruned; everything is on `main`.

**✅ OWNER-VERIFIED ON-DEVICE (2026-07-26, x64 build `0.2.15 (362)`)** — the whole line was checked in
one pass: *"tank looks better … VU meters better … VU color change good … About box looks good"*.
The one reservation: **glass bezel "needs work — but ok for this release"** (see BACKLOG). This
matters because 0.2.15 is almost entirely VISUAL, and visual work is the one thing the dev sandbox
cannot check at all.

- **About-box tip section** (`e02e140`) ✅ — the Buy-Me-a-Coffee / Ko-fi buttons finally say what they
  are. Deliberately silent on how the two backends differ (owner's 0.2.13 call). The box grew
  dp(470)→dp(530) and is now **capped to the work area**: dp(530) is 795px at 150%, and a 1366×768
  laptop has ~708px of work area, so uncapped the entire button row sat under the taskbar.
- **PIP right-click menu + PIP⇄main swap** (`e02e140`) — the floating PIP gets its own menu instead of
  the main window's view menu. The swap is **allowed while recording**: recording runs on a separate
  headless player (`VlcPlayer::rec_`) and `doStop()` never touches it. *Not exercised in the pass.*
- **Dead-link checker graduated** (`e02e140`) — flag *and* branch deleted. Safe because a sweep is
  user-triggered and now **undoable**: Settings ▸ Channels ▸ "Clear dead-link results".
- **Splash**: the "Arch cookies" line became `SplashWrappingTinFoil`.
- **Glass "framed pane"** (`5f447b7`) ⚠️ — three bands: the theme's own 1px border left bit-identical,
  an opaque hard-stepped bezel in the chrome gutter, and the dial carrying only the bezel's cast
  shadow (`add` is exactly 0 over content — the invariant that forecloses the old "reads as blur"
  failure). Costs **zero dial pixels**; footprint went *down* 42.4%→38.2%. **Owner: needs work.**
- **Look-aware meter knobs** (`80e3a38`, `9bb9f7a`) — the slider band is now a function of
  (kind, LOOK), not kind alone. Two of VU's three sliders were DEAD, and `Sens` was a real bug
  (`scalarLevel` never applied it). Completing it also removed four dead `Glow` sliders that shipped
  in the DEFAULT configuration, since `glow` is only read by Tube and Scope.
- **VU bottom-lamp lighting** (`b07ef8c`) ✅ — `common/ui/VuLamp.{h,cpp}`, pigment × light. The face
  was a gradient brighter at the TOP (i.e. a lamp *above* the dial) and the needle's shadow offset
  down-right. Now a bulb below the bottom edge and left of centre, a needle shadow that is the needle
  ROTATED 2.6° about the pivot (a fixed offset gives constant separation, which reads as two
  needles), and **blue via `palette.bg`** — hue only, black restores the stock bulb.
- **Data-flow tank: top-centre pour, floor drain** (`e4d24e5`, `ad2d738`) ✅ — the drain is a dish in
  the level controller's OWN target rather than a velocity sink, so it shares the controller's fixed
  point instead of fighting the buffer-health readout. Unclaimed win found by review: per-column
  waterline spread collapsed **7.66 → 1.72 rows** (the old right→left drift piled water 9 rows deep
  on the left wall and 1.3 on the right). Resting level moved 5→6 of 10 LED rows at full health;
  `kVisibleFill` 0.68 kept, owner-accepted.
- **User-selectable fluid colour** (`2071aa4`) ✅ — one global swatch in the Data-flow row (that row
  has no `MeterConfig`). Depth shading is Beer-Lambert and reproduces the old default **byte-identically
  at every depth**.

**Two review lessons worth carrying forward.** (1) A design panel's *reasoning* can be wrong even when
its *conclusion* is right: the buffer drain's "a velocity sink is unsolvable here" was disproven by
experiment (`ad2d738`) — a sink survives projection at 68% because `ITER = 8` is nowhere near
converged. (2) A test can pin the flattering pixel: the VU lamp's original assertions covered only the
hotspot, which sits in the bottom rows furthest from every marking, while the scale band the user
actually reads had gone 16% darker. Both were caught by adversarial review, not by the build.

### v0.2.14 — previous release

**Released:** **`v0.2.14`** (2026-07-26), tag `v0.2.14` @ `aa8580f`, full version **`0.2.14.349`**; three
installers on GitHub release `v0.2.14` — x64 `35,334,214` / arm64 `30,189,007` / universal `63,226,356`
bytes — **two appcasts** (`0.2.14.349`) committed @ `d594fd0` and LIVE (both enclosures HTTP 200).
Owner-signed on the Mac; each signature byte-length-verified against the built file, and the two
appcasts cross-checked so the x64/arm64 signatures cannot be swapped.

⚠️ **The biggest single release of the 0.2.x line, and the least runtime-verified** — six features plus a
background network worker. Mitigated by design, not by luck: the dead-link checker is **inert unless its
beta flag is ticked**, and the glass overlay is **off at strength 0**, so a user who touches neither gets
0.2.13 plus bug fixes. Owner did verify the System dialog, the PIP toggle, VU, glass and **a real
dead-link sweep** (31 alive / 22 dead / 53 written on a 442-channel library) before the cut.

**Features**
- **⚙️ Settings ▸ System…** (`0ff649e`) — the first "organise the settings" dialog, built to grow:
  titled sections, themed hairlines. Carries the **log level** and the **beta-feature switchboard**.
- **📊 Log levels** (`0ff649e`, instrumented `61773c4`) — standard Trace/Debug/Info/Warn/Error with a
  runtime threshold. **Header-only by necessity**: mac implements the same `common/platform/Log.h`
  (`mac/platform/Log.mm`), so adding a function to it would break their link — the levels, threshold and
  codec are inline over a function-local atomic, and **neither platform sink changed**. Deliberately **no
  "Off"**: the log exists so a tester can send it after a failure. Instrumented in the same release
  because levels with no call sites are a setting that lies — DEBUG carries every libVLC state
  transition + the scheduler verdict, TRACE the 250ms flow snapshot.
- **🧪 Beta feature flags** (`common/core/FeatureFlags.h`) — opt-in switches persisted as `beta_<id>`,
  default OFF, with the rules written into the header (inert when off; the id is stable because it is
  the settings key; delete the flag *with* its branch when a feature graduates). This is what let the
  dead-link checker ship at all.
- **🔗 Dead-link checker, BETA** (`081faf9` core + `57d4fbb` sweep) — `common/core/DeadLinkCheck` holds
  the verdict logic (17 selftests) because that is where the danger is: a bad sweep marks thousands of
  channels dead and, with "Hide unavailable" on, **the library disappears**. Hence: a failure to CONNECT
  is *Inconclusive*, never Dead; **an entire sweep is discarded unless enough of it reached a server**;
  5xx is inconclusive (an overloaded provider recovers); only 401/403/404/410 mark Dead. New
  **`httpProbe()`** seam — `httpGet` is unusable here (a live stream's body never ends), so it reads
  headers and closes; GET not HEAD (edges mishandle HEAD), redirects NOT followed (a 3xx is healthy).
  Bounded at 250/run, sequential, paced — a greedy 12k sweep reads as a scraper and an Xtream
  connection cap can kick the user's actual playback. Own sqlite connection; joined in `WM_DESTROY`.
- **📻 VU needle meter** (`10dae97`, thinned `c87a6e9`) — fifth `MeterStyle`. **Real ballistics**: ~300ms
  and *symmetric*, because a VU lags going down as much as up and that weight is the instrument; reusing
  the LED attack/decay envelope would make it a peak meter. Integrates every tick regardless of the
  active look, so switching to it shows a needle already tracking.
- **🔍 Glass overlay** (`6c7ec62`, reworked `d3ce334`) — `common/ui/GlassMask` precomputes two LUTs
  applied over the finished frame (frame-invariant ⇒ zero per-frame maths). Now a **beveled plate**:
  clear centre, all the optics in a rim band. A first attempt spread gradients over the whole face and
  read as *blur* — the owner's Phase Linear 400 photo is why. Free win alongside: `MiniMeter` now caches
  a DIB instead of creating+destroying one **every paint** (~240 GDI ops/sec across the tray).
  **Global** strength, not per-meter: the buffer meter has no `MeterConfig`, the knob band is full, and a
  6th `MeterTuning` field would break mac's exact-arity parser.
- **🖼️ PIP "Keep above other apps"** (`9774418`) — off ⇒ topmost only while RabbitEars is foreground,
  re-raised on `WM_ACTIVATEAPP`. It can never be left plain non-topmost: an owned popup then composites
  UNDER the main window's libVLC D3D surface.
- **⏭️ `ScheduleStatus::Skipped`** (`af6592d`) — cancelling one airing of a series rule already behaved
  correctly (any-status row is a tombstone); it just *said* "Cancelled", which reads as if the series was
  stopped. The mac `switch` case shipped in the SAME commit — it is `-Wswitch -Werror` there.

**Fixes**
- **PIP no longer hijacks the main view on exit** (`a4b93aa`) — `applyViewMode` carried the active pane's
  stream into pane 0, right for Split (equal tiles) but wrong for PIP (a secondary overlay).
- **PIP resize snaps to the video aspect** (same commit) — needed a new worker-sampled
  `VlcPlayer::videoSize()` (libVLC calls stay off the UI thread), published as one atomic.
- **Unicode case-fold for rule matching** (`c7ec7b0`) — `foldTitle` used `towlower` under a comment
  claiming it handled "any language". The app never calls `setlocale()`, so it folded **only A-Z**: a
  Contains rule for "café" never matched "CAFÉ", "тв" never matched "ТВ" — silently. Explicit simple-fold
  table (Latin-1, Latin Ext-A, Greek, both Cyrillic blocks); `common/` cannot use `CharLowerW`/ICU.
- **`PRAGMA busy_timeout`** (`bb110c3`) — five writers discard `stepDone()`, so a contended write was a
  *silently lost setting*. Also the prerequisite for any second connection.

**Deferred with evidence — TV Guide off-thread.** Both premises fail: `programmesInWindow` **already**
bounds by the window (the backlog's "cheap win" is a no-op, and index variants all regressed in
benchmarking), and the owner's library has **zero `epg_programmes`**, so the diag timer has never fired.
See `BACKLOG.md` for how to get a real number before anyone tries again.

---

📚 **Release history for v0.2.13 and older lives in [`HANDOVER-ARCHIVE.md`](HANDOVER-ARCHIVE.md).**
Split out 2026-07-26 (this file was 1,503 lines). The archive holds the contemporaneous per-release
accounts back to v0.1.1 — read it before re-litigating an old decision or re-trying an idea that may
already have been reverted. Everything needed to work on the app *today* is in this file.

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
- **`make-appcast.ps1 -Tag` defaults to `v<Version>` — i.e. `v0.2.12.325`, NOT `v0.2.12`.** Omit it and the
  enclosure URL points at a release tag that doesn't exist, so every auto-update 404s. **Always pass
  `-Tag v<marketing.version>` explicitly** (bitten twice; `docs/RELEASING.md` step 5 now shows it).
- **`build-arm64.cmd` needs the right vcvars for the HOST** — `vcvarsarm64.bat` is the ARM64-**native**
  toolchain and exists only on a Windows-on-ARM box; on an x64 dev machine the same VS component installs
  `vcvarsamd64_arm64.bat` (x64→ARM64 **cross**). The script prefers native and falls back to cross (0.2.12).
  Both target ARM64 — `LibVlc.cmake` keys on `CMAKE_CXX_COMPILER_ARCHITECTURE_ID` (the **target**), not the
  host, so the arm64 libVLC/WinSparkle slices are picked either way. **Verify the output is really ARM64**:
  PE machine at `e_lfanew+4` should be `0xAA64`.
- **"Every modal disables the main window" is ALMOST true — two surfaces are MODELESS**: the TV Guide
  (`epgGuideOpen()`) and the topmost "please wait" box (`st->loadingDlg`, with `st->busy` for its worker).
  Anything gating on `IsWindowEnabled(hwnd)` must name those explicitly. Worse, a dialog opened *from* the
  live guide calls `EnableWindow(mainWnd, TRUE)` on exit, which can **re-enable the main window underneath
  an unrelated nested modal loop** and silently break its modality.
- **`KillTimer` does not purge a `WM_TIMER` already posted** — if the handler then runs a nested modal
  loop, that stale message can re-enter it. Use a re-entry latch (see `maybeShowSupportPrompt`).

## Backlog

Moved to **[`BACKLOG.md`](BACKLOG.md)** — the parked work, headlined by the **theme engine** (0.2.x
epic: full reskin + selectable D3D11/shader skins). Also there: JSON profiles, scheduled recording,
recording formats, EPG + dead-link checker, resume-last-channel, named saved layouts, group-title
country fallback, the dialog work-area clamp + shared-`runModalLoop` cleanup, DPI-change relayout,
Authenticode + portable-zip. `HANDOVER.md` stays focused on **current state**.

## Git state

Owner-owned repo `github.com/arcanii/RabbitEars`. **Development is on `main`** — `0.2.15-dev` was
merged and deleted, and the four stale mac-side PR branches were pruned with it, so `main` is now the
only branch local and remote. (All five were verified fully merged with zero unmerged commits and no
open PRs before deletion; four of them belonged to already-merged macOS PRs #33/#34/#35/#42.)
Tags `v0.1.0`…**`v0.2.16`**; **the next tag is `v0.2.17`** (or `v0.3.0`) — nothing is reserved or
burned. **v0.2.16 released @ `d83b002`** (full `0.2.16.377`; both appcasts @ `0344b54`) —
the Xtream VOD release: movies sync into a 🎬 Movies root, player seek + scrub bar, buffer-meter
glass. Prior: **v0.2.15 released @ `1324f5f`** (full `0.2.15.365`; both appcasts @
`77035ed`) — the instruments release: VU relit from a bottom bulb + optional blue lamp, framed glass,
the data-flow tank reoriented to pour in at the top and drain at the floor, user-selectable fluid
colour, look-aware meter knobs, About tip section, PIP menu + swap, dead-link checker out of beta.
Prior: **v0.2.14 @ `aa8580f`** (`0.2.14.349`, appcasts @ `d594fd0`) — System settings, beta flags, the
beta dead-link checker, VU + glass meters, two PIP fixes. Prior: **v0.2.13 @ `93dea6f`** (`0.2.13.329`, appcasts @ `d57997f`) — Ko-fi; **v0.2.12 @
`76c6a46`** (`0.2.12.325`) — Buy Me a Coffee + CJK QA + Xtream countries + schema v7. The **mac line is
decoupled** (`if(APPLE)` in `cmake/AppVersion.cmake`, currently **0.2.15**) and the mac team pushes to
`main` too, so **`git fetch` + rebase before every release** — the 0.2.0 cut had a push rejected mid-flight
by a concurrent mac commit. **Release-tooling note (0.2.2):** this machine now
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
(the first push was rejected until re-fetched). Working tree clean.
Build number = git commit count, baked at CMake configure time
**after** the commit — so a build must follow the release commit to stamp the matching `0.2.0.<count>`. Commit/push only when the
owner asks; stage **specific paths** (the owner keeps adding `art/*.png` — never `git add -A`); end
commit messages with the Co-Authored-By trailer.

## Immediate next steps (pick up here)

### 📦 What went into 0.2.16 (2026-07-27 → 28) — six commits, all now released

`APP_VERSION` was already `0.2.16` when these landed, so the cut needed no bump. Both theme flags
build clean at /W4 and `--selftest` is ALL PASS after every one of them.

| commit | what |
|---|---|
| `ffb69dc` | groundwork: player seek + scrub bar, `common/core/Json`, schema v8, buffer-meter glass, the `--xtream` recon probe |
| `1a486be` | gate 2 — [`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md), written off the real provider run |
| `538f0b2` | the Xtream client core: `common/core/XtreamClient`, DAO plumbing, **APP_VERSION → 0.2.16** (its message says "0.2.17 core" — that was the pre-collapse plan) |
| `1d59783` + `7751cdc` | the perf work: country views made immune to VOD size, then the **Movies nav root** data model |
| `e4e01a7` | **the VOD UI**: Movies nav root wired, `Win32/ui/VodSync.{h,cpp}`, Settings action, 15 i18n keys (message says "0.2.17" — same reason) |

#### The Xtream client core (`538f0b2`)

`common/core/XtreamClient.{h,cpp}` — **pure, no network by design.** The caller fetches and hands
bytes in, because with `max_connections: 1` whether a sync may run at all depends on whether a pane
is playing, and only the caller knows that. Burying the fetch would hide that decision somewhere it
cannot be made correctly.

`Database` gained `retireMissingChannels()` (the other half of a sync — a catalogue churns, so
without it the library only ever grows) and `bulkInsertChannels` now carries `kind`/`added_at`.

#### The Movies nav root (`7751cdc`) — owner's call, data model only

Movies live under **one "Movies" root**, not as ~67 sibling categories in the live tree.
`listGroups()` is now live-only; `listVodGroups()` / `moviesByGroup()` / `allMovies()` serve the new
root. The two trees are separate namespaces — a category name present on both sides returns only its
own rows to each accessor, which is pinned by selftests.

**⚠️ The nav is NOT wired yet.** `ViewKind` has no `Movies`/`MovieGroup`, `refreshNav` does not build
the root, and `loadForFilter` cannot select it. The DB is ready; the UI is not.

#### Perf — measured, not guessed (`1d59783`, `7751cdc`)

New **`RabbitEarsCli --benchdb [movies] [live]`** (defaults to the owner's real 43,599 / 442). The
design doc called this the epic's biggest risk and said measure BEFORE the sync ships. It was real:

| query | live-only | +43,599 movies | after |
|---|---|---|---|
| `listCountries()` | 0.12 ms | **13.01 ms** | **0.09 ms** |
| `channelsByCountry()` | 0.13 ms | **6.38 ms** | **0.12 ms** |
| `listGroups()` | 0.11 ms | **8.21 ms** | **0.07 ms** |

**Every live-TV path is now immune to VOD library size.** The bigger half was CORRECTNESS, not speed:
a movie has no country, but its category name goes through the country-prefix fallback — a provider
with `NL - FILMS` categories would have filed all 43,599 films under the Netherlands. The owner's real
categories dodge it only because `VOD` is three letters, which is luck, not design.

Left as measured, all on non-interactive paths: `moviesByGroup()` 2.38 ms (the real browse path),
`listVodGroups()` 9.7 ms (nav refresh), `allMovies()` **77 ms** (materializes 43,599 rows),
`searchChannels()` 7.9 ms (movies stay searchable on purpose), bulk insert ~260 ms.

**A dead end, recorded so nobody retries it:** widening the VOD partial index to
`(kind, group_title)` and inlining `kind` as a literal did NOT fix `listVodGroups` (8.9 → 9.7 ms,
noise). The composite index is kept because it does serve `moviesByGroup`. Not worth chasing further
on a once-per-nav-refresh path.

- **Player seek + scrub bar + time readout** — `VlcPlayer` gains `timeMs/lengthMs/isSeekable/seekTo`,
  sampled on the worker beside `videoSize()` and published as atomics. The bar appears only when
  libVLC reports the media seekable with a real length; seeks commit on `TB_ENDTRACK` only (one seek
  per gesture, not per drag tick — a network stream would re-buffer continuously).
  ⚠️ **"Invisible on live IPTV" is libVLC's judgement, not an invariant we enforce.** An HLS feed with
  a DVR window can legitimately report seekable, and a fair number of IPTV channels are exactly that.
  Showing a scrub bar there is arguably CORRECT — but it means the "strip is unchanged for existing
  users" claim needs an eye on a real channel list. **This is the top thing to check on device.**
- **`common/core/Json.{h,cpp}`** — hand-rolled JSON reader (house style). Tolerant exactly where
  panels are non-standard, strict on structure. 32 selftests.
- **Schema v8** — `channels.kind/duration_sec/resume_sec/watched/added_at`. A movie is a channel row,
  not a second table. All columns default to the live-TV answer; verified by migrating a hand-built
  v2 DB and asserting a pre-v8 row comes back `kind=Live`.
- **Buffer-meter glass** — the backlog's likeliest answer to the 0.2.15 "bezel needs work". Grid
  geometry lifted into `BufferMeter.h` so `--selftest` can pin the one property that cannot be
  eyeballed: glass OFF is bit-identical to the pre-glass renderer at every size/DPI.
- **`RabbitEarsCli --xtream`** — the reconnaissance probe (below).

**Three defects an adversarial review caught in the seek work, worth remembering:** pressing **Stop**
stranded the scrub bar on screen forever, because `doStop()` DETACHES its libVLC callbacks before
teardown (deliberately, so a dying stream cannot post stale events) — so **no event follows a stop**
and `sampleStats()` has already quit. Nothing existed to retire the bar. Also: a pane switch between
two *seekable* panes never flipped visibility, so pane A's seek target drove pane B's thumb; and a
Seek queued behind a Play landed the old film's position on the new stream (now stamped with a stream
generation). The pattern in all three: **state cleared only on a visibility TRANSITION is not cleared
at all when the transition doesn't happen.**

### ✅ The VOD UI is DONE and COMMITTED (`e4e01a7`, pushed) — ▶️ PICK UP AT "What still needs the owner"

All three remaining items landed. Both theme flags build clean at /W4 and `--selftest` is ALL PASS.
**None of it has been seen running** — see "What still needs the owner". This completes 0.2.16;
nothing is left to build before the release except the owner's on-device pass.

1. **Movies nav root — settled as CATEGORIES-ONLY.** `ViewKind` gained `Movies` + `MovieGroup`.
   `refreshNav` builds the root from `listVodGroups()` and **omits it entirely when there are no
   movies**, so the sidebar is byte-identical for every existing live-TV-only user. Unlike
   Groups/Countries the root IS selectable (`ViewKind::Guide`'s precedent): selecting it clears the
   grid, expands its category list and says so, rather than leaving the previous view's rows sitting
   under a heading that says Movies. `MovieGroup` → `moviesByGroup()` (2.4 ms). **`allMovies()` is
   never called from the UI** — 43,599 rows is not a browsable view.
2. **`Win32/ui/VodSync.{h,cpp}`** — own sqlite connection, joined in `WM_DESTROY`; three requests per
   playlist (auth probe + categories + streams, ~13.5 MB); `bulkInsertChannels` then
   `retireMissingChannels` behind a trust guard that INSERTS but refuses to RETIRE below 50% usable
   items. Syncs every enabled Xtream playlist in one run.
3. **Settings ▸ Channels ▸ "Sync movies from provider"** — `ID_VOD_SYNC = 2027`, `WM_APP+8/+9`,
   15 i18n keys × en/ja/zh-Hant (zh-HK inherits).

**The three constraints, and how they are actually met** (all three needed more than the obvious):

- 🔴 **`max_connections: 1`.** The gate refuses on `isPlaying()` **OR `isRecording()` OR
  `nowPlayingId != 0`**, across `panes` *and* `dyingPanes`. All three terms are load-bearing:
  `rec_` is a **second libVLC player with its own socket** that `doStop()` never touches, so a
  scheduled recording runs with nothing "playing"; and `playing_` is set from the
  `libvlc_MediaPlayerPlaying` **event**, so it is false for the whole open/buffer window while the
  socket is already claimed. The gate is also **not start-only** — `playChannelInPane`,
  `onToggleRecord` and `onSchedulerTick` all call `cancelVodSync()`, because the user's stream
  always wins. Finally the sync and the **dead-link sweep now exclude each other** (both refuse, and
  the menu greys both while either runs): they are the two provider-facing workers, and a sweep
  whose probes get refused for capacity can persist `dead_status=Dead` on live TV.
- 🔴 **`retireMissingChannels()` reentrancy** — worker has its own connection; joined at
  `WM_DESTROY` **after** the players, see the shutdown-order note below.
- 🔴 **Trust** — the guard reads `XtreamVodResult`'s counts in 64-bit arithmetic (`total` is
  provider-controlled, and the multiplication is the one place a broken panel could overflow into
  *deleting* the library). A run that refuses retirement says so in its own status message.

**Three defects three review rounds caught that are worth carrying forward as patterns:**

1. **A fix can be a regression.** Moving `armExitWatchdog` ahead of the worker joins (to stop them
   skipping the wake-task registration) put two *unbounded* joins inside a 4 s budget sized for
   libVLC teardown — so `ExitProcess` would fire mid-join and skip `player.shutdown()`, **the only
   thing that finalizes an in-progress recording** (an mp4 with no moov atom is unplayable). The
   order is now: wake-task registration → cancel both workers → arm watchdog → libVLC teardown →
   *then* join the network workers. **Recordings finalize before anything unbounded is waited on.**
2. **"Survive the error" can be worse than failing.** An unparseable `get_vod_categories` was
   originally survived by falling back to one group — but `bulkInsertChannels` writes
   `group_title=excluded.group_title` **unconditionally**, so that would have rewritten all 43,599
   stored movies to "Movies" in one committed transaction and flattened the whole tree, on a
   transient hiccup, with no undo. It now aborts the sync.
3. **A modal loop pumps posted messages.** The playlist context menu bounds-checked its `navFilters`
   index before `TrackPopupMenu` and re-read it after. `WM_APP_VOD_DONE` is the app's first posted
   message that calls `refreshNav()` — so the menu's Rename/**Delete** could name a different
   playlist than the one right-clicked. The id is now read before the menu.

Also still open from the design doc, and **deliberately not decided here** — they are owner calls,
and the design doc's "success = … resume works" line depends on all three: where duration comes from
(the API has none for movies — cache `VlcPlayer::lengthMs()` at play time), the `watched` threshold,
and resume-prompt vs silent-resume. **As it stands the VOD feature is browse-and-play, not resume**,
and resume is listed under 0.3.0 anyway ("resume everywhere"). Decide whether 0.2.16 ships without
it — the recommendation is yes, since resume cannot be designed against a library nobody has used.

#### ⚠️ One measured regression this release ships with — an owner decision

**The search box is now 0.63 → 80.00 ms on the FIRST KEYSTROKE** (126×, `--benchdb`). `EN_CHANGE`
runs `searchChannels()` synchronously on the UI thread with no debounce, no minimum length and no
`LIMIT`, and the query carries no `kind` predicate — so one letter matches and materializes most of
a 43,599-film library. The 7.9 ms figure previously signed off here was measured with the term
`"Channel 1"`, which matches **zero** movies, so it timed the scan and none of the materialization;
`--benchdb` now reports both. Movies staying searchable is a deliberate design-doc position, and how
to degrade it (LIMIT / debounce / minimum length) changes what the user gets — so it is left for the
owner rather than settled here. See BACKLOG.

`allChannels()` (0.73 → **80.4 ms**) and `channelsByPlaylist()` (0.59 → **80.0 ms**) also grow with
VOD size. That one is a deliberate position — "the All view legitimately grows with the row count" —
and the post-sync UI is careful never to land on it. What was NOT deliberate and is now fixed: those
two views ORDER BY `kind` first, because an Xtream playlist and its VOD sync write to the **same
playlist row** and `bulkInsertChannels` restarts `sort_order` at 0 per batch, so 43,599 movies
numbered 0..43598 interleaved straight through 442 live channels numbered 0..441. For a live-only
library every row is `kind=0`, so the ordering is byte-identical — macOS included.

### 🎬 Xtream VOD — both gates CLOSED, and what reconnaissance found

`RabbitEarsCli --xtream` ran against the owner's real provider (2026-07-27): `player_api.php` **works**,
**43,599 movies** / **13,152 series**, `container_extension` on 100% of movies, play URL **HTTP 302
reachable**, all 8 bodies parsed cleanly. The design doc is
**[`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md)** — written off measured numbers, with the shared-core
boundary flagged to the macOS team in its §2.

**Three findings changed the plan** (full detail in BACKLOG + the doc): **`max_connections: 1`** is the
governing constraint; **movies carry NO metadata** (`get_vod_info` returns 204 bytes with an empty
`info`, so duration must be cached from `lengthMs()` at play time); and **poster art inverts** —
`stream_icon` is empty on ~90% of movies but series `cover` is populated, so **movies stay in the text
grid and posters become a SERIES feature**.

⚠️ **The biggest risk is not VOD** — 43,599 rows is ~4× the current library, in the same `channels`
table that already needed a SQLite scalar to keep the country filter under ~30 ms/keystroke at 14k
rows. ✅ **Re-measured with `--benchdb` before shipping — but AGAINST THE WRONG SHAPE.** At the design
doc's 43,599 movies + 442 live, the country/group paths came out immune and only the search box
degraded (0.63 → 80.00 ms per keystroke). Then the on-device pass showed the owner's real library is
**410,147 rows, ALL `kind=Live`** — the provider lists movies and series flat in the m3u — where the
same queries cost **0.6–1.4 SECONDS**. That is pre-existing and ships in v0.2.15 today, so it does not
block 0.2.16, but it retires the claim that the live-TV paths are safe at scale. **Read the numbers
and the analysis in BACKLOG before doing any more perf work on this table.**

### What still needs the owner

**0.2.16 SHIPPED with its headline feature unexercised.** The scrub bar, glass and tank were all
owner-verified before the cut (below); the VOD sync was not, because the line expired first. In
priority order:

0. ✅ **THE FIRST REAL VOD SYNC RAN (2026-07-28) — and it works.** Owner's live provider:
   **43,606 movies, 67 categories**, all with group titles and `added_at` populated, the 🎬 Movies
   root appeared in the sidebar when it finished, films play, and seek/pause/scrub work on them.
   The headline feature of 0.2.16 is no longer unexercised.

   🔴🔴🔴 **…but it DOUBLED the movie library instead of upgrading it in place — see BACKLOG,
   "EVERY FILM IS STORED TWICE".** The provider's m3u emits `http://host:80/movie/…` while
   `xtreamMovieUrl()` builds `http://host/movie/…` from the port-less playlist URL, so the
   `(playlist_id, stream_url)` dedupe never fires. `bulkInsertChannels`' comment claims that
   collision happens "BY DESIGN"; a real provider has falsified it. 43,599 `kind=0` duplicates now
   sit in the LIVE tree beside 43,606 `kind=1` rows. **Not destructive, does not self-heal, and a
   second sync is safe** (the keep-set matches the `kind=1` rows exactly). The fix is a canonical
   `stream_url` plus a deduping migration — scoped in BACKLOG, and the migration is the half that
   can lose user state, so it wants care rather than speed.

   **Still worth doing: run the sync a SECOND time.** The first can only insert; the second is the
   one that calls `retireMissingChannels` in anger and can DELETE. "Movie sync done — N added or
   updated, **0 removed**" is the healthy answer. A removal count out of proportion to what the
   provider actually dropped is the failure to watch for. If it ever goes wrong the library is
   rebuildable: delete the playlist and re-import.

1. ✅ **ANSWERED ON DEVICE (2026-07-28) — and the FIRST answer here was WRONG. Read the correction.**
   The initial pass sampled ONE live channel (`NL - SPONGEBOB`), saw no scrub bar, and this entry
   recorded "absent on live channels — 0.2.16 is invisible to existing users". **A wider look
   disproved it the same day:** BBC News shows `0:36 / 0:36`, another channel `1:00`, another
   `1:54` — and **all of them actually rewind.** These are real HLS DVR windows, so libVLC's
   `is_seekable` is telling the truth and the bar is doing something useful.
   **Verdict: leave it exactly as it is.** A short rewind buffer on live TV is a capability, not a
   glitch, and suppressing it would throw away working functionality. What is wrong is only the
   CLAIM: 0.2.16 is *not* invisible on live channels — on any provider offering a DVR window it
   adds a working rewind, which is a nicer surprise than the one we were braced for.
   ⚠️ **The methodological lesson is the point of keeping this entry.** One channel was treated as
   the answer to a question about a whole class of streams, and the wrong conclusion was written
   into three documents before a second sample was taken. `is_seekable` varies PER CHANNEL on one
   provider — sample several before concluding anything about it.
2. **The rest of the 0.2.16 pass** — the glass bezel (does a framed tank beside four framed
   mini-meters resolve the 0.2.15 "needs work"?), the scrub bar on something actually seekable, and
   the two 0.2.15 leftovers below (the empty-tank fix, the bezel).
3. **A real VOD sync**, once the worker exists — against a 1-connection line, not during playback.

⏳ **The recon account expired ~2026-07-28** (`exp_date` 1785276000 against a server clock of
1785155974 — ~33 h after the run). Anything needing a live VOD stream needs a renewed line. The
recon report itself is saved and re-runnable: `RabbitEarsCli --xtream` against any provider.

---

✅ **0.2.15 SHIPPED** (2026-07-26) — tag `v0.2.15` @ `1324f5f`, `0.2.15.365`, three signed installers on
GitHub release `v0.2.15`, both appcasts LIVE @ `77035ed` (feeds AND enclosure URLs verified HTTP 200).
See the "Current state" block for the full feature list, the NO-GO blocker the pre-release check caught,
and the two review lessons worth carrying forward.

**Owner-verified on-device before the cut** — unusually thorough for this line, and it mattered because
0.2.15 is almost entirely visual: tank, VU meters, VU colour change, About box, PIP swap all confirmed
on the real x64 build. That leaves **only two things unseen** in the shipped release:
- **the empty-tank fix** (`200e5bc`) — landed after the pass. Play a channel, stop it, and the tank
  should drain COMPLETELY rather than leaving a dim bottom stripe; the sim should also go idle a
  couple of seconds later, where before it ran forever.
- **the glass bezel**, which the owner explicitly accepted as-is: *"needs work — but ok for this
  release"*. Deferred to **pass 4** in `BACKLOG.md`, written as an investigation with a shortlist of
  suspects rather than a fix, because "needs work" does not say which way it is wrong.

**🎬 The next EPIC, if it is wanted: Xtream VOD + Series (`BACKLOG.md`, scoped 2026-07-26).**
RabbitEars has **no VOD support at all** today — no seek API in the player, no scrub bar, no
duration/resume/watched in the `Channel` model, and no Xtream `player_api.php` client (the Xtream
handling that exists treats those playlists as flat lists of live channels). Also: **there is no JSON
parser in the repo**, and `player_api.php` is JSON-only. Estimated ~15–22 focused days; BACKLOG holds
the full breakdown, the risks, and a three-release plan that lands at **0.3.0** rather than starting
there. **Two things must happen first, in order:** a 30-minute reconnaissance of the owner's real
provider (panels are wildly non-standard and some disable the API), then a design doc
`Win32/docs/XTREAM_VOD.md` written BEFORE any code — exactly as `THEME_ENGINE.md` was for the one
prior epic of this size, and with the same obligation to flag the shared-core boundary to the macOS
team, since the client, parser, schema and models all land in `common/`.

**Candidates for 0.2.16** (as judged before the work started; ✅ = now landed in `ffb69dc`):
✅ **wire the glass into the buffer meter** — done, and it was indeed the item with the clearest
rationale; whether it resolves the bezel reservation is still an owner call. Glass bezel **pass 4**
remains open and is now better informed (the tank is no longer the unframed odd one out, so if the
bezel still reads wrong, the suspects narrow to the three luminance steps / the light skin / the
corner pip). Still open and untouched: the **buffer-depth tester task** (does `bufferedBytes` report
at all on IPTV?); the About box's **"Bg" swatch not saying it is the VU lamp**; the **x86-64 arch
label**; and Authenticode signing (still owner-gated, also unblocks the Microsoft Store track).

**Still open from 0.2.14:** the 22 dead-link verdicts from that release's one real sweep have never
been confirmed as true positives — much less pressing now that "Clear dead-link results" makes a wrong
verdict one click to undo, which is precisely what made graduating the checker safe.

✅ **0.2.14 SHIPPED** (2026-07-26) — tag `v0.2.14` @ `aa8580f`, `0.2.14.349`, three signed installers on
GitHub release `v0.2.14`, both appcasts LIVE @ `d594fd0`. Six features + a background worker — see the
"Current state" block above for the full list and the reasoning behind each.

**Pending the owner's on-device pass** (0.2.14 shipped with less runtime verification than usual):
- **"Skip this airing"** has never been exercised on a real series rule.
- **The Unicode case-fold fix** has never been tested against a non-English guide (the owner's library
  has no EPG at all — see the deferred TV-Guide item).
- **The dead-link checker** got one real sweep (31 alive / 22 dead / 53 written). Worth confirming those
  **22 are genuinely dead** rather than false positives — with "Hide unavailable" on they vanish from the
  grid, and a wrong verdict is invisible until a user goes looking for a channel.
- Everything else (System dialog, log levels, PIP toggle + both PIP fixes, VU, glass) was owner-seen.

**Candidates for 0.2.15:** the About-box tip section (owner-requested — the tip buttons have no
explanation); further **glass** work (a bezel/frame is the top idea — see BACKLOG for the explicit "do
NOT reintroduce" list); the **PIP right-click menu + PIP⇄main swap**; graduating the dead-link checker
out of beta once it has mileage; Authenticode signing (also unblocks the Microsoft Store track).

📚 **The shipped-release blocks for v0.2.13 and older moved to
[`HANDOVER-ARCHIVE.md`](HANDOVER-ARCHIVE.md)** — including the long write-ups for localization,
wake-to-record, the recording scheduler, saved layouts, favourites I/O and the ARM64 port.

## Seed prompt for a new session

Paste this verbatim to start a fresh session with working context restored:

> You are continuing **RabbitEars**, a native **Windows Win32 / C++20** IPTV player on **libVLC
> 3.0.23** with a shared **`common/`** core (also feeds the macOS app), dark "Claude-desktop" chrome
> (coral `#D97757`, custom `WM_NCCALCSIZE` title bar), CMake + Ninja + MSVC (VS 2026), deps
> vendored/NuGet. Repo `G:\RabbitEars`.
>
> **Read `Win32/HANDOVER.md` first** — the top "Current state" block and "Immediate next steps" —
> plus `Win32/BACKLOG.md`. Per-release history older than the current line is in
> `Win32/HANDOVER-ARCHIVE.md`; you rarely need it, but check it before re-trying an idea, because
> several have already been tried and reverted.
>
> **State:** last SHIPPED = **`v0.2.15`** (2026-07-26, `0.2.15.365`, tag @ `1324f5f`, both appcasts
> LIVE @ `77035ed`) — the instruments release. `main` is the ONLY branch, local and remote, and it is
> clean. `APP_VERSION` is `0.2.15`; the next release bumps it. **Bumping ≠ releasing** — the tag and
> the two appcasts are what gate the rollout.
>
> **Traps that have already cost real time, in order of cost:**
> * **`common/` is shared with mac, and mac keeps its own copies of some of it.** `Log.h` is
>   implemented by BOTH `Win32/platform/Log.cpp` and `mac/platform/Log.mm` — adding a function to that
>   header breaks their link (which is why log levels are header-only inline). `mac/src/app/
>   MeterModel.cpp` parses `MeterTuning` and `MeterPalette` with **exact arity** (`!= 5`, `!= 7`), so a
>   new field silently resets every mac user's meters. A new `ScheduleStatus` needs a case in mac's
>   `RecordingsWindowController.mm` in the SAME commit (`-Wswitch -Werror`). **Always grep `mac/`
>   before changing anything under `common/`.**
> * **Command ids: pick from a genuine gap.** The computed ranges (`ID_DOCK_BASE` 2051–2062,
>   `ID_LAYOUT_*_BASE` 2079–2098, `ID_THEME_SKIN_BASE` 2100+) have no literal to grep — one collision
>   already shipped as a bug. `WM_APP+1..+8` and `+10` are taken.
> * **Release:** bump ONLY `APP_VERSION` in `cmake/AppVersion.cmake` (everything derives), leaving the
>   `if(APPLE)` mac override alone. Three installers, two appcasts; always pass `-Tag v<ver>` to
>   `make-appcast.ps1` — it defaults to `v<full.version>`, which 404s. Only EdDSA signing happens on
>   the owner's Mac. **PUSH BEFORE TAGGING and verify `git ls-remote origin refs/heads/main` equals
>   HEAD** — `gh release create` tags the REMOTE head, so an unpushed commit tags source that does not
>   contain what you built, and the build number (= commit count) is baked into the binaries. This
>   returned NO-GO on the 0.2.15 cut. Verify the enclosure URLs resolve HTTP 200, not just the feeds.
> * **Build with `-DRABBITEARS_THEME_ENGINE=ON` explicitly** — the default is ON but build dirs cache
>   it, and a stale OFF cache once shipped a Theme-menu-less exe. Verify BOTH flags before committing.
>   Occasional `LNK1104`/`LNK1168` on `G:\` is the exe being locked (the owner runs it) or SMB
>   flakiness — retry once, don't loop.
> * **i18n:** `common/i18n/*.json` → `tools/i18n/gen_i18n.py` generates `common/core/Strings.*` (never
>   hand-edit). ~566 keys × 4 languages; `zh-HK` is an override layer over `zh-Hant`, so most new keys
>   need no HK entry. CJK is a machine draft.
>
> **Working rules:** every change adversarially reviewed (background agent) + build-verified BOTH
> theme flags + `--selftest` before committing. **This sandbox cannot launch the GUI** — hand every
> visual/runtime pass to the owner. Commit only when asked; stage specific paths (never `git add -A`).

