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

## Current state — **v0.2.14 SHIPPED (System settings · beta flags · dead-link checker · VU + glass meters)** · v0.2.13 · macOS 0.2.15

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

### 🔨 0.2.15 — IN DEVELOPMENT (branch `0.2.15-dev`, marketing version bumped, NOT released)

`APP_VERSION` is **`0.2.15`** (`cmake/AppVersion.cmake:11`; the `if(APPLE)` override is separate and
untouched). Bumping the version releases nothing — the git tag + the appcasts still gate the rollout,
and 0.2.14 users stay on 0.2.14 until then. Landed on the branch so far:

- **About-box tip section** (`e02e140`) — the Buy-Me-a-Coffee / Ko-fi buttons finally say what they
  are. Deliberately silent on how the two backends differ (owner's 0.2.13 call). The box grew
  dp(470)→dp(530) and is now **capped to the work area**: dp(530) is 795px at 150%, and a 1366×768
  laptop has ~708px of work area, so uncapped the entire button row sat under the taskbar.
- **PIP right-click menu + PIP⇄main swap** (`e02e140`) — the floating PIP gets its own menu instead of
  the main window's view menu. The swap is **allowed while recording**: recording runs on a separate
  headless player (`VlcPlayer::rec_`) and `doStop()` never touches it.
- **Dead-link checker graduated** (`e02e140`) — flag *and* branch deleted. Safe because a sweep is
  user-triggered and now **undoable**: Settings ▸ Channels ▸ "Clear dead-link results".
- **Splash**: the "Arch cookies" line became `SplashWrappingTinFoil`.
- **Glass "framed pane" (uncommitted)** — the meter glass is now three bands: the theme's own 1px
  border left bit-identical, an opaque hard-stepped bezel in the chrome gutter, and the dial carrying
  only the bezel's cast shadow (`add` is exactly 0 over content — the invariant that forecloses the
  old "reads as blur" failure). Costs **zero dial pixels**; footprint went *down* 42.4%→38.2%.
- **VU lamp lighting** — design panel run; not yet implemented. Owner's direction: the lighting is
  "too pure", the lamp belongs at the **bottom**, the needle needs a **shadow**, and the light "could
  also be blue". The blue option must ride an **existing** palette role (`bg`, unused by the VU look
  and defaulting to `CLR_INVALID` = "no override") — mac's parsers are exact-arity on both
  `MeterTuning` (5) and `MeterPalette` (7), so no new field is possible.

**None of 0.2.15 is runtime-verified** — this sandbox cannot launch the GUI.

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

Owner-owned repo `github.com/arcanii/RabbitEars`. **0.2.15 development is on the branch
`0.2.15-dev`**, not on `main` — `main` is at `ea1bc32` (the 0.2.14 docs commit) and fast-forwards
cleanly (`git switch main && git merge --ff-only 0.2.15-dev`).
Tags `v0.1.0`…**`v0.2.14`**; **v0.2.14 released @ `aa8580f`** (full `0.2.14.349`; both appcasts @ `d594fd0`)
— System settings, beta flags, the beta dead-link checker, VU + glass meters, two PIP fixes. Prior: **v0.2.13 @ `93dea6f`** (`0.2.13.329`, appcasts @ `d57997f`) — Ko-fi; **v0.2.12 @
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
> **State:** last SHIPPED = `v0.2.14` (2026-07-26, `0.2.14.349`). Development is on branch
> **`0.2.15-dev`** with `APP_VERSION` already bumped to `0.2.15` — **bumped ≠ released**; the tag and
> the two appcasts gate the rollout.
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
>   the owner's Mac.
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

