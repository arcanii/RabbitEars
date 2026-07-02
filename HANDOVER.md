# RabbitEars — Handover

A native **Windows (Win32 / C++20)** IPTV player built on **libVLC**, themed to
match its two sibling apps, **`G:\SQLTerminal-Win32`** and **`G:\ManorLords-SGE`**
(dark "Claude-desktop-style" look, coral accent `#D97757`, custom title bar,
CMake + Ninja + MSVC, dependencies vendored / NuGet-provisioned with **no Visual
Studio project**). This is the single starting point for anyone (human or agent)
continuing the work — read it before touching code.

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

## Current state — v0.1.3 SHIPPED

**Released:** `v0.1.3` (2026-07-02), tag `v0.1.3` @ `ebd71a8`, public GitHub release
with a signed **`RabbitEars-0.1.3-setup.exe`** installer (full version `0.1.3.22`) and a
**live WinSparkle auto-update feed** (appcast published @ `5f43c64`). Earlier: `v0.1.2`
(tag @ `8c99254`, full `0.1.2.19`), `v0.1.1` (signed installer + live feed, the auto-update
baseline), `v0.1.0` (portable zip). 0.1.1 / 0.1.2 users get 0.1.3 automatically.

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

> **Sandbox note:** this dev environment **cannot launch the GUI exe**
> (`Start-Process` hangs even with `dangerouslyDisableSandbox`; `cmd start` →
> "Access is denied"). All GUI work is **build-verified + reasoned**, and the owner
> does the real runtime/visual verification. The CLI (`RabbitEarsCli`) *does* run
> here and is the way to exercise the core headlessly. The owner runs on the same
> machine (real DB at `%LOCALAPPDATA%\RabbitEars\`, ~12,905 channels from iptv-org).

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
  `src/platform/Updater.cpp` (`sKPprIa95Hw+…`) equals the macOS `SUPublicEDKey`, so
  installers are **signed on macOS** with the same private key.
- **Per release:** bump version in 4 places (`APP_VERSION` in `CMakeLists.txt`,
  `MyVer` in `packaging/installer.iss`, VERSIONINFO in `packaging/RabbitEars.rc`,
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
sqlite3         third_party/sqlite/   vendored public-domain amalgamation. Static lib.
RabbitEarsCore  src/core/, src/db/    engine: M3uParser, Http (WinHTTP), Database.
                src/models/           No UI/libVLC — CLI-testable with zero downloads.
RabbitEarsCli   src/cli/              headless core tool (--selftest/--fetch/--import).
RabbitEars      src/ui/, src/WinMain  Win32 GUI (gated: RABBITEARS_BUILD_GUI).
 (GUI)          src/platform/         MainWindow (chrome+layout+wiring), ChannelGrid-
                                      Control (D2D grid), BufferMeter (LED), VlcPlayer
                                      (worker libVLC + recorder), Dialogs (About/prompt),
                                      Splash (layered), Log (diagnostics), Updater
                                      (WinSparkle).
```

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

- ~~**Categories / countries include-filter**~~ — **DONE** (in the 0.1.2 batch; see
  above). Possible follow-ups: remember the *last* view when toggling it, or a
  per-view (not global) category filter.
- **Recording Phase 2 (scheduled)**: DB schedule table + dialog + a timer firing the
  headless recorder at set times (app must be running). Phase 3: Windows Task
  Scheduler wake-to-record + EPG-driven scheduling.
- **Recording formats**: MP4 (record `.ts`, remux on stop — MP4 isn't crash-safe if
  written live) and **transcoding** (format/quality/size presets; CPU-heavy).
- **Resume last channel** on launch (`last_channel_id` is already persisted).
- **DPI-change relayout** (`WM_DPICHANGED`): recreate fonts, relayout, push DPI to
  grid/meter (`channelGridUpdateDpi`, `bufferMeterSetDpi`).
- **Authenticode** code-signing (SmartScreen). **Portable-zip** artifact on releases.
- **EPG** (XMLTV now/next; `tvg-id` already stored) and a **background dead-link
  checker** (so "Hide unavailable" isn't purely passive). Import/export favourites.

## Git state

Active development on `main` (owner-owned repo `github.com/arcanii/RabbitEars`).
Tags `v0.1.0`, `v0.1.1`, `v0.1.2`, `v0.1.3` — **v0.1.3 released @ `ebd71a8`** (full
`0.1.3.22`; appcast published @ `5f43c64`). The 0.1.3 batch is committed + pushed, so the
working tree is **clean**. Build number = git commit count (the released 0.1.3 build was
stamped at count **22**; the appcast + this doc commit are post-release bookkeeping and
were not rebuilt — same as 0.1.2). Commit/push only when the owner asks; stage **specific
paths** (the owner keeps adding `art/*.png` — never `git add -A`); end commit messages
with the Co-Authored-By trailer.

## Immediate next steps (pick up here)

1. **Confirm the 0.1.2 → 0.1.3 auto-update in the wild** — on an installed 0.1.1/0.1.2,
   **About → Check for Updates** should offer **0.1.3.22** from the live appcast and apply
   the signed installer (the first end-to-end WinSparkle update test since 0.1.2). The
   owner still does the GUI/runtime pass on the 0.1.3 features (the sandbox can't launch
   the exe); `rabbitears.log` has `SpectrumTap:` lines if the audio meter looks dead.
2. Remaining **Phase-2 layout**: **named saved layouts**. The `DockLayout` engine already
   serializes/parses any tree, so this is a store (name → serialized tree, in settings) +
   a **Settings → Layout** submenu to save / apply / delete named layouts; the "move to
   edge" menu stays as a fallback.
3. Backlog: scheduled recording, resume-last-channel, DPI-change relayout
   (`WM_DPICHANGED`), Authenticode signing (SmartScreen), group-title country fallback for
   Xtream feeds, per-view (not global) category filter, EPG (XMLTV now/next).

## Seed prompt for a new session

Paste this verbatim to start a fresh session with working context restored:

> You are continuing work on **RabbitEars** (`G:\RabbitEars`), a native **Windows
> Win32 / C++20** IPTV player on **libVLC 3.0.23**, themed to match its sibling apps
> `G:\SQLTerminal-Win32` and `G:\ManorLords-SGE` (dark "Claude-desktop" look, coral
> `#D97757`, custom `WM_NCCALCSIZE` title-bar chrome, CMake + Ninja + MSVC via **VS
> 2026 Community**, deps vendored/NuGet, **no VS project**). **Read `HANDOVER.md` and
> `docs/architecture.md` first.**
>
> **State: v0.1.3 is SHIPPED** (tag `v0.1.3` @ `ebd71a8`, full `0.1.3.22`, signed
> installer + **live WinSparkle auto-update**; appcast @ `5f43c64`). The 0.1.3 batch below
> is committed, released, and its update feed is live; every piece is `/W4`-clean and
> adversarially reviewed (the owner does visual/runtime checks — the sandbox can't launch
> the exe). **Next: named saved layouts** (Phase-2), then backlog. 0.1.3 shipped:
> - transport + fullscreen **icon buttons** (Segoe MDL2 glyphs);
> - **channel-switch hang fix** — blocking libVLC `stop()`/`release()` moved off the
>   worker onto tracked "reaper" threads in `VlcPlayer` (drained in the dtor);
> - **Xtream / query-string fix** — `Http.cpp` was dropping the `?username=&password=`
>   (no `lpszExtraInfo` buffer); fixed + a VLC-style fetch User-Agent;
> - **SpectrumTap diagnostics** (activation/init HRESULTs → `rabbitears.log`);
> - **by-country nav filter** — `Database::listCountries/channelsByCountry` derive ISO
>   codes from tvg-id suffixes (`"CNN.us"`); 5 CLI selftest assertions;
> - **first-run T&C** that RE-PROMPTS on every version change (`tos_accepted` = full
>   version); **animated splash** (own thread, rotating captions);
> - **dockable layout** — NEW `ui/DockLayout.{h,cpp}` (a split-tree over Nav/Video/Grid,
>   serialize/parse, re-dock surgery; unit-tested by the CLI). `MainWindow` renders regions
>   from the tree via ONE atomic `BeginDeferWindowPos` pass (`SWP_NOCOPYBITS`), with resize
>   **gutters**, a **Settings → Layout** menu, and **drag-a-grip-to-redock** with a
>   translucent snap overlay. Remaining Phase-2: **named saved layouts**.
>
> The GUI: owner-draw command bar + Settings menu; three **dockable regions** (nav
> TreeView / video+transport strip / Direct2D `ChannelGridControl`) rearranged by dragging
> corner grips or the divider gutters (persisted `dock_layout`); a `VlcPlayer` (all libVLC
> lifecycle + a headless recorder on a worker thread); a blocky-LED buffer meter + modular
> mini-meters (spectrum/signal/bitrate/frames — `ui/MiniMeter` + WASAPI process-loopback
> `audio/SpectrumTap`); the log at `%LOCALAPPDATA%\RabbitEars\rabbitears.log`; themed dialogs.
>
> Build/verify:
> ```
> scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON      # from PowerShell: & "G:\RabbitEars\scripts\build.cmd" ...
> build\RabbitEarsCli.exe --selftest               # core + dock-layout + country assertions (headless)
> ```
> Gotchas: `cmake`/`cl` aren't on PATH — use `scripts\build.cmd`. `LINK1168` = an instance
> is running → `Stop-Process -Name RabbitEars -Force`, rebuild. Static CRT (`/MT`). **libVLC
> `stop()`/`release()` block** (now offloaded to reaper threads); event callbacks run on a
> libVLC thread → only `PostMessage`. **RECT fields are `LONG`** → cast to `int` for
> `std::max`/`MoveWindow` (or you hit ambiguous-overload errors). `themeBrush()` caches only
> 12 colors — LED/grip drawing uses the GDI **DC brush** instead. **`Grep` renders `/` as
> `\`** in this sandbox — trust `Read`, not `Grep`, for slash-containing lines. Channel grid
> D2D target pinned to 96 DPI. libVLC `i_read_bytes` is 0 for HLS. VLC sout single-quoted
> paths need `'` doubled. Modal dialogs must read controls before `DestroyWindow`.
>
> **This sandbox can't launch the GUI** (`Start-Process` hangs even with
> `dangerouslyDisableSandbox`); build-verify + reason, run `RabbitEarsCli` for headless
> checks (dock-tree + country logic live in `RabbitEarsCore` so they ARE selftested), and
> hand visual/runtime verification to the owner (who reads `rabbitears.log`). Commit only
> when asked; stage specific paths (never `git add -A` — the owner adds `art/*.png`); end
> commits with the Co-Authored-By trailer. Releases are signed on macOS with the shared
> family Ed25519 key — see `docs/RELEASING.md`.
