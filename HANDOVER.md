# RabbitEars — Handover

A native **Windows (Win32 / C++17)** IPTV player built on **libVLC**, themed to
match its two sibling apps, **`G:\SQLTerminal-Win32`** and **`G:\ManorLords-SGE`**
(dark "Claude-desktop-style" look, coral accent `#D97757`, custom title bar,
CMake + Ninja + MSVC, dependencies vendored / NuGet-provisioned with **no Visual
Studio project**). This is the single starting point for anyone (human or agent)
continuing the work — read it before touching code.

## Stack decision (important)

The design doc (`IPTV Player Application Design.docx`) lists a "WinUI 3 / EF Core"
table. That is a boilerplate artifact and is **overridden** by the explicit
direction to leverage the two C++ reference apps' look. RabbitEars is therefore a
**custom-drawn native Win32 / C++17 app** (GDI + Direct2D), exactly like the
siblings — *not* WinUI 3, *not* .NET/EF Core. Storage is SQLite via the C API
(the vendored amalgamation), not EF Core.

| Component     | Choice                                                        |
|---------------|---------------------------------------------------------------|
| Language      | C++20, Windows SDK                                             |
| UI            | Custom Win32 chrome + Direct2D/GDI owner-draw (shared Theme.h) |
| Media engine  | libVLC 3.0.x (VideoLAN.LibVLC.Windows NuGet, provisioned)      |
| Storage       | SQLite (vendored amalgamation, C API)                          |
| M3U parsing   | Custom parser (`src/core/M3uParser`)                          |
| Build         | CMake + Ninja + MSVC (VS 2026 Community), deps vendored        |

## Current state — Layer A verified; Layer B1 built (needs a visual check)

**Layer B1 (GUI bring-up) is built and deploys** but has not been *run* yet: this
dev session's sandbox blocks launching a freshly-built exe (`Start-Process` hangs,
`cmd start` → "Access is denied"), so the owner must do the first launch. Static
analysis confirms it will start: builds clean at `/W4`, and the whole load-time
DLL chain resolves (`RabbitEars.exe → libvlc.dll → libvlccore.dll → system`), with
`libvlc.dll` + `libvlccore.dll` + 325 `plugins/` deployed next to the exe.

- `src/ui/VlcPlayer.{h,cpp}` — libVLC wrapper: instance with embed-friendly args,
  `libvlc_media_player_set_hwnd` into a child HWND (before play), per-channel
  `:http-user-agent`/`:http-referrer`, volume/aspect/audio-track, events marshaled
  to the UI thread via `PostMessage`.
- `src/ui/MainWindow.{h,cpp}` + `src/WinMain.cpp` — a themed window (dark DWM
  title bar) with a black video surface child, transport (pause/stop/volume) and a
  status line; on startup it plays a public HLS **test stream**
  (`test-streams.mux.dev`) to validate the pipeline. Set env `RABBITEARS_DEBUG=1`
  to get a startup trace at `rabbitears_debug.log` next to the exe.
- **Verified running:** the exe launches, creates its "RabbitEars" window, and
  reaches ~212 MB working set (libVLC actively decoding the test stream). Early
  builds linked the CRT dynamically and could raise a "missing MSVCP140/
  VCRUNTIME140 DLL" error depending on launch context; fixed by static-linking the
  CRT (`CMAKE_MSVC_RUNTIME_LIBRARY` = MultiThreaded / `/MT`) — the exe now needs
  **zero** external CRT DLLs, only the deployed `libvlc.dll` + Windows system DLLs.
- **To verify yourself:** run `build\RabbitEars.exe` — expect a dark ~1100×720
  window that buffers then plays video, with a working volume slider and pause/stop.
- **Branding:** the app icon (`packaging/app.ico`, generated from
  `art/RabbitEars_icon.png` by `scripts/make_ico.py`) is embedded via the `.rc`
  and set as the window/taskbar icon. The **About box** renders `art/RabbitEars3.png`
  (RCDATA + GDI+) with name/version and the libVLC (LGPL-2.1) attribution. The
  **README hero** uses `art/RabbitEars_logo.png`. (`art/RabbitEars.png` is the older
  mascot, no longer referenced by the build.)

## Layer B1b — real player (built)

The smoke-test window is replaced by the actual player (builds clean at `/W4`,
launches and loads the DB without crashing; verify the visuals by running it):

- **Custom title-bar chrome** (`MainWindow.cpp`): reclaims the non-client area via
  `WM_NCCALCSIZE` and paints an owner-draw command bar — app title, a coral
  "+ Add Playlist" pill, "Open File", "About", a search box, and hand-drawn
  min/max/close caption buttons (hover states, drag, double-click-maximize,
  top-edge resize).
- **Nav sidebar**: a dark TreeView — All Channels / ★ Favourites / Groups / Playlists;
  selecting a node filters the grid.
- **Channel grid** (`ChannelGridControl.{h,cpp}`): Direct2D owner-draw, windowed
  painting (smooth at 12k+ rows), columns # | ★ | Channel | Group, zebra rows,
  hover/selection, now-playing accent bar, vertical scroll, keyboard nav, live
  search filter. Single-click a row → play; click ★ → toggle favourite.
- **Add Playlist**: "+ Add Playlist" prompts for a URL (WinHTTP download on a
  worker thread, `core/Http`), "Open File" for a local `.m3u`; parse → store →
  refresh. Verified headlessly: `RabbitEarsCli --fetch <url>` and `--import <src>`.
- **Data flow**: opens the DB on startup, loads channels into the grid, plays the
  selected channel via `VlcPlayer` (with per-channel user-agent/referrer),
  persists `last_channel_id`. Fullscreen toggle (button / double-click video / Esc).
- CLI additions: `--fetch <url>` (test WinHTTP+parse) and `--import <url|file>`
  (import into the app's real DB — used to seed a first playlist).

## Layer B1c — polish (in progress)

- **Fluid buffer meter** (`BufferMeter.{h,cpp}`): a 2D Navier-Stokes "stable
  fluids" solver drives a little tank of liquid whose level tracks stream health
  (full = healthy buffer; drains as it goes chunky/low). It flows right→left with a
  wavy, splashy free surface — density-scaled gravity + hydrostatic buoyancy
  (travelling waves), vorticity confinement (swirls), splash bursts that intensify
  when the stream is chunky, and a soft surface-band-exempt level relaxation.
  Rendered as an iso-surface (bilinear sample, d=0.5 contour) with a coral foam
  crest and a left-drifting advected-phase shimmer, 2× supersampled to a DIB and
  HALFTONE-stretched into the strip. **Right-click to hide** (persisted in
  `buffer_hidden`); the sim runs only while there's fluid (no idle CPU). All knobs
  are the `constexpr` tunables block at the top of the .cpp.
- **Draggable sidebar splitter** (`ReVSplitter` in MainWindow): drag to resize the
  nav sidebar (clamped); width persisted in the `sidebar_w` setting.
- **Type-a-number jump**: type a channel number in the grid to select/scroll to
  the matching LCN (resets after ~0.9s idle).
- **Logo thumbnails** (`ChannelGridControl.cpp`): a LOGO column with async
  tvg-logo thumbnails — 3 background workers fetch (short-timeout `httpGet`),
  WIC-decode to premultiplied BGRA, disk-cache under `%LOCALAPPDATA%\RabbitEars\
  logos\`, and hand pixels to the UI thread which makes the `ID2D1Bitmap` lazily;
  only visible rows are fetched, deduped by URL. Placeholder = a chip with the
  channel's initial. Cache is a `shared_ptr` so detached workers can't outlive-crash.
- **Inline # editing**: double-click the # cell to overlay a numeric EDIT (Enter
  commits via `onSetNumber` → `setChannelNumber`, Esc cancels, commits on
  scroll/focus-loss).
- **Global search**: the command-bar box now searches the whole library
  (`db.searchChannels`, any name/group/tvg match), not just the current nav view.
- **Dead / geo-locked greying**: a channel that errors on playback is marked
  `dead_status=Dead` (persisted) and greyed in the grid; a successful play marks it
  Alive. Passive detection — no mass pinging.
- **Still to do:** resume-last-channel on launch; DPI-change re-layout; a proper
  background dead-link checker (roadmap). Then Layer C/D per docs/architecture.md.

## Infrastructure

- **C++20** (`CMAKE_CXX_STANDARD 20`; the siblings are C++17). Built clean at `/W4
  /permissive-`.
- **Auto-update (WinSparkle)**: vendored under `third_party/winsparkle/`;
  `src/platform/Updater.{h,cpp}` (init/check/shutdown) wired into `runApp` (startup
  background checks) with a **Check for Updates…** button in the About box.
  `src/version.h.in` → `build/generated/version.h` (APP_VERSION + git commit count).
  Appcast at `packaging/appcast.xml`; `scripts/make-appcast.ps1` generates a release
  entry. **Before shipping signed updates**: run WinSparkle's `generate_keys`,
  replace the placeholder EdDSA public key in `Updater.cpp`, set the real appcast
  URL, and sign each installer with `sign_update`. (An installer — Inno Setup like
  the sibling — is still TODO for Layer D.)
- **Modularization**: the About + text-prompt dialogs moved out of the (large)
  `MainWindow.cpp` into `src/ui/Dialogs.{h,cpp}`. Further candidates: the owner-draw
  command-bar chrome and the splitter (both couple to `AppState`, so they'd take a
  small context struct).

The engine (parser + store) and build system (Layer A) are complete, build clean
at `/W4`, and are proven end-to-end.

- **M3U/M3U8 parser** (`src/core/M3uParser.{h,cpp}`): full IPTV EXTINF dialect —
  `#EXTM3U` (+ `x-tvg-url`/`url-tvg` EPG url), `#EXTINF` attributes
  (`tvg-id`/`tvg-logo`/`tvg-name`/`group-title`/`tvg-chno` + inline
  `http-user-agent`/`http-referrer`), `#EXTGRP`, `#EXTVLCOPT`, bare-URL
  playlists. Correctly splits the display name on the **first *unquoted* comma**
  (not the last — titles and quoted attribute values both contain commas),
  strips BOM, tolerates CR/LF/CRLF.
- **SQLite store** (`src/db/Database.{h,cpp}`): a real typed DAO with a RAII
  `Stmt` (bound parameters — no injection from M3U-derived strings) and a `Tx`
  (one `BEGIN IMMEDIATE` for bulk insert), WAL + FK pragmas, schema created on
  open, `%LOCALAPPDATA%\RabbitEars\rabbitears.db` (env override
  `RABBITEARS_DATA_DIR`). Idempotent refresh via
  `ON CONFLICT(playlist_id,stream_url)` that preserves the user's favourite flag
  and custom LCN.
- **RabbitEarsCli** (`src/cli/RabbitEarsCli.cpp`): `--selftest` (30 assertions,
  all pass) and `<file.m3u>` to parse + store + dump. The GUI-free way to test
  the core, mirroring the siblings' `GvasCli`.
- **Verified**: `RabbitEarsCli --selftest` → ALL PASS. Parsing the real
  `https://iptv-org.github.io/iptv/index.m3u` → **12,905 channels, 177 groups**,
  EPG url captured, stored in one transaction.

## Architecture (bottom-up)

```
sqlite3         third_party/sqlite/   vendored public-domain amalgamation (copied
                                      from SQLTerminal-Win32; 3.53.2). Static lib.
RabbitEarsCore  src/core/, src/db/    engine: M3uParser (bytes -> ParsedChannel),
                src/models/           Database (SQLite DAO). No UI, no libVLC — so
                src/platform/         it and the CLI build/test with zero downloads.
                                      Encoding.h = UTF-8<->UTF-16 (copied verbatim).
RabbitEarsCli   src/cli/              headless core test/inspection tool
                                      (--selftest, --fetch, --import).
RabbitEars      src/ui/, src/WinMain  the Win32 GUI (BUILT). Gated behind
 (GUI)          src/platform/Updater  RABBITEARS_BUILD_GUI (provisions libVLC).
                                      MainWindow (chrome+layout+wiring),
                                      ChannelGridControl (D2D grid), BufferMeter
                                      (fluid), VlcPlayer (worker-thread libVLC),
                                      Dialogs (About/prompt), Updater (WinSparkle).
```

The `src/ui/*` current-state details are in the Layer B1/B1b/B1c sections above; the
sections below (Roadmap / Reuse map) are the original Layer-A plan, kept for
context — "Layer B" items marked "next"/"port" there are now **done**.

## Toolchain (same as the siblings — non-obvious)

- **Visual Studio 2026 Community** at
  `C:\Program Files\Microsoft Visual Studio\18\Community` (MSVC + bundled
  CMake/Ninja). `cmake`/`cl` are **not** on PATH by default.
- Build: **`scripts\build.cmd`** (sets vcvars64 + PATH, configures with
  `-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`, builds). Pass through extra CMake
  args, e.g. `scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON`.
- **RelWithDebInfo, not Debug**: the MSVC Debug CRT heap serializes allocations
  behind one process-wide lock, which stalls the UI thread during background work
  (documented pain in both siblings). Release CRT + PDBs keeps debugging usable.
- Expected build failure once the GUI exists: `LINK1168: cannot open
  RabbitEars.exe for writing` — the exe is still running from a manual test.
  `Stop-Process -Name RabbitEars -Force`, then rebuild.

## Build, test, verify

```
scripts\build.cmd
build\RabbitEarsCli.exe --selftest                 :: 30 parser + DB assertions
build\RabbitEarsCli.exe path\to\playlist.m3u       :: parse + store + dump a real file
```

## Roadmap

Layers B–D are speced in detail in **`docs/architecture.md`** (synthesized from a
deep analysis of both sibling apps + libVLC/M3U research). Summary:

- **Layer B — GUI shell (next).** `cmake/LibVlc.cmake` provisions libVLC; add
  `RabbitEars.exe`: custom title bar via `WM_NCCALCSIZE` + owner-draw command bar
  (port from `SQLTerminal-Win32`/`ManorLords-SGE` `MainWindow.cpp`), the 2-pane
  ManorLords layout (nav sidebar + content), a `VlcPlayer` wrapper
  (`libvlc_media_player_set_hwnd` into a child HWND — **set_hwnd before play**,
  marshal libVLC events off its thread with `PostMessage`), and
  `ChannelGridControl` (port `SqlGridControl.{h,cpp}` — D2D owner-draw grid) fed
  from the DB. Goal: window opens themed, channels list, click plays.
- **Layer C — features.** Add-Playlist dialog (URL via WinHTTP download on a
  worker thread / local file via `comdlg32`), live search filter, favourites star
  toggle, inline LCN edit + type-a-number jump, transport (volume/aspect/audio
  track/fullscreen), nav filtering (All/Favourites/group/playlist), settings
  persistence (resume last channel/volume).
- **Layer D — packaging.** `packaging/RabbitEars.rc` + icon, Inno Setup installer
  (model `SQLTerminal-Win32/packaging/installer.iss`), WinSparkle auto-update
  (`appcast.xml` + `scripts/make-appcast.ps1` + `src/platform/Updater`),
  `version.h.in`. Bundle LGPL license text + attribution for libVLC.

## Reuse map (what to copy from the siblings)

| Need                | Source (sibling)                                   | How        |
|---------------------|----------------------------------------------------|------------|
| SQLite amalgamation | `SQLTerminal-Win32/third_party/sqlite/`            | done (copied) |
| UTF-8/16 helpers    | `SQLTerminal-Win32/src/platform/Encoding.h`        | done (copied) |
| Palette             | `SQLTerminal-Win32/src/ui/Theme.h`                 | done (copied) |
| D2D factories       | `SQLTerminal-Win32/src/ui/D2DSupport.h`            | done (copied) |
| DPI manifest        | `SQLTerminal-Win32/packaging/app.manifest`         | done (copied) |
| Title-bar chrome    | `*/src/ui/MainWindow.cpp` (WM_NCCALCSIZE, cmd bar) | port (Layer B) |
| 2-pane layout       | `ManorLords-SGE/src/ui/MainWindow.cpp`             | port (Layer B) |
| Channel grid        | `SQLTerminal-Win32/src/ui/SqlGridControl.{h,cpp}`  | port (Layer B) |
| Themed dialogs      | `SQLTerminal-Win32/src/ui/ThemedDialog.{h,cpp}`    | port (Layer C) |
| libVLC provisioning | `ManorLords-SGE/cmake/WindowsAppSdk.cmake` pattern | done (`cmake/LibVlc.cmake`) |
| Installer/updater   | `SQLTerminal-Win32/packaging/`, `scripts/`, `third_party/winsparkle` | port (Layer D) |

## Gotchas to carry forward (from the siblings' hard-won experience)

- **`WM_CTLCOLORSTATIC` must return an opaque themed brush + matching
  `SetBkColor`** — a transparent/`NULL_BRUSH` ghosts old text and breaks ClearType.
- **`LVS_OWNERDATA` ListViews** don't reliably repaint the trailing area on column
  resize (stale pixels). This is a strong reason the grid is the **D2D
  `SqlGridControl` port**, which repaints its whole client each frame and has no
  such artifact.
- **`EnableWindow(mainHwnd, FALSE)` does not cascade** to the custom command/title
  bar (separate HWND) — track a `busy` flag explicitly during playlist fetch/parse.
- **libVLC 3.x `stop()`/`release()` are SYNCHRONOUS and block** until the stream
  tears down — seconds, on a stuck/dead stream. Calling them on the UI thread
  froze the app on channel switches. `VlcPlayer` now runs all media-player
  lifecycle on a dedicated worker thread (serialized command queue, with
  coalescing of rapid switches); the UI only enqueues and returns. Keep it that way.
- **libVLC event callbacks run on a libVLC thread** — never touch Win32/DB state
  from them; only atomics + `PostMessage` a `WM_APP+n` to the UI thread.
- **`libvlc_media_player_set_hwnd` must be called before `play()`** or libVLC opens
  its own top-level output window instead of rendering into the child HWND.
- **libVLC is LGPLv2.1**: dynamic-link + ship unmodified DLLs/plugins keeps
  RabbitEars proprietary; include the LGPL text + attribution, don't add GPL-only
  plugins (e.g. libdvdcss).

## Git state

Active development on `main` (the owner pushes). Engine + full GUI (Layers A, B1,
B1b, B1c) + infrastructure (WinSparkle, C++20) are committed. Commit or push only
when the owner asks; end commit messages with the Co-Authored-By trailer.

## Immediate next steps (pick up here)

- **Resume last channel** on launch (the `last_channel_id` setting is already
  written on play — read it in `WM_CREATE` and auto-play).
- **DPI-change re-layout** (`WM_DPICHANGED`): recreate fonts + relayout + push the
  new DPI to the grid/meter (`channelGridUpdateDpi`, `bufferMeterSetDpi`).
- **Further `MainWindow.cpp` modularization**: extract the owner-draw command-bar
  chrome and the splitter (both couple to `AppState`, so pass a small context).
- **Layer D packaging**: an Inno Setup installer (model
  `SQLTerminal-Win32/packaging/installer.iss`) — the last big infra piece. Then the
  WinSparkle ship-time TODOs (real EdDSA key, appcast URL, signing).
- **Roadmap features** (docs/architecture.md §7 / Layer C): XMLTV EPG now/next
  (tvg-id is already stored + indexed), background dead-link checker, recording via
  `--sout`, import/export favourites as `.m3u`.
- **Fine-tune the fluid meter** if the owner has feedback — all knobs are the
  `constexpr` block atop `src/ui/BufferMeter.cpp`.

## Seed prompt for a new session

Paste this verbatim to start a fresh session with working context restored:

> You are continuing work on **RabbitEars** (`G:\RabbitEars`), a native **Windows
> Win32 / C++20** IPTV player built on **libVLC**, themed to match its sibling apps
> `G:\SQLTerminal-Win32` and `G:\ManorLords-SGE` (dark "Claude-desktop" look, coral
> accent `#D97757`, custom title-bar chrome, CMake + Ninja + MSVC via **VS 2026
> Community**, dependencies vendored / NuGet-provisioned, **no Visual Studio
> project**). **Read `HANDOVER.md` and `docs/architecture.md` first** — they hold
> the full state, the reuse map from the siblings, and the design blueprints.
>
> Current state: the engine (M3U parser + SQLite DAO + `RabbitEarsCli`) and the full
> GUI are built and committed on `main`. The GUI has: custom `WM_NCCALCSIZE`
> title-bar chrome + owner-draw command bar, a nav TreeView sidebar with a draggable
> splitter, a Direct2D `ChannelGridControl` (logo thumbnails via async WIC + disk
> cache, `# | ★ | logo | name | group`, inline `#` editing, type-a-number jump,
> dead/geo-locked greying), a `VlcPlayer` that runs all libVLC lifecycle on a worker
> thread (so blocking `stop()`/`release()` never freeze the UI), a Navier-Stokes
> **fluid buffer meter**, Add-Playlist (WinHTTP worker or local file), global search,
> favourites, an About box + text-prompt in `src/ui/Dialogs.{h,cpp}`, and WinSparkle
> auto-update (`src/platform/Updater`).
>
> Build/verify:
> ```
> scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON      # GUI (downloads libVLC once)
> build\RabbitEarsCli.exe --selftest               # 30 core assertions
> build\RabbitEars.exe                              # the app
> ```
> Toolchain gotchas: `cmake`/`cl` aren't on PATH — always use `scripts\build.cmd`.
> `LINK1168: cannot open RabbitEars.exe` means an instance is still running —
> `Stop-Process -Name RabbitEars -Force` then rebuild. Static CRT (`/MT`); the exe
> needs no VC++ redist. **libVLC `stop()`/`release()` are blocking — keep them on the
> `VlcPlayer` worker thread.** libVLC event callbacks run on a libVLC thread — only
> `PostMessage` to the UI. The channel grid's D2D target is pinned to 96 DPI so
> drawing and mouse hit-testing share pixel space (don't remove that).
>
> This dev sandbox blocks *launching* the built exe (`Start-Process` hangs, `cmd
> start` → "Access is denied"); use `dangerouslyDisableSandbox` for a launch
> smoke-test (process alive + memory), and hand visual verification to the owner.
> Commit only when the owner asks; stage specific paths (the owner keeps adding
> `art/*.png` — don't sweep them in with `git add -A`). Immediate next steps are
> listed above in HANDOVER.md.
