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

## Current state — v0.1.7 SHIPPED · macOS Phase-2 restructure + Phase-1 app LANDED; audio meters pending

**Released:** `v0.1.7` (2026-07-03), tag `v0.1.7` @ `de8c571`, public GitHub release with a
signed **`RabbitEars-0.1.7-setup.exe`** installer (full version `0.1.7.52`) and a **live
WinSparkle auto-update feed** (appcast published @ `12be931`). Earlier: `v0.1.6` (`5d06958`,
`0.1.6.37`), `v0.1.5` (`ca945d1`, `0.1.5.29`), `v0.1.4` (`8622e8a`, `0.1.4.26`), `v0.1.3`
(`ebd71a8`, `0.1.3.22`), `v0.1.2` (`8c99254`, `0.1.2.19`), `v0.1.1` (auto-update baseline),
`v0.1.0` (portable zip). 0.1.1–0.1.6 users get 0.1.7 automatically.

**Repo restructured (macOS Phase-2 — LANDED):** the tree is now **`common/`** (portable core —
M3uParser, Database, DockLayout, models, platform *contracts*), **`Win32/`** (the Windows app, incl.
`platform/Paths.cpp` + `Http.cpp`), and **`mac/`**, under a unified root `CMakeLists.txt` (`common`
→ `Win32`/`mac` per-OS; marketing version now in `cmake/AppVersion.cmake`, shared by both). **The
Windows exe/DLLs/plugins now build to `build\Win32\`** (not `build\`) — `installer.iss` +
`build-installer.cmd` were fixed to match (0.1.7).

**macOS (all on `main`):** the app **plays IPTV** via libVLC in a native window with a rich channel
grid — search, group **and country** filter, favourites (★ toggle), **LCN edit**, and
**resume‑last‑played** on launch. **Sparkle auto‑update** is wired (framework auto‑provisioned from
the GitHub release by `mac/cmake/Mac.cmake` + embedded in the bundle; `SUFeedURL`/`SUPublicEDKey` in
`Info.plist`; menu‑bar *Check for Updates…*). Both platforms build in CI
(`.github/workflows/{mac,windows}-core.yml`, incl. the mac `.app`). Build/run the mac app:
`scripts/build-mac.sh --app` → `build-mac/mac/RabbitEars.app` (needs VLC.app for libVLC, auto‑detected).
Full detail: [`docs/MACOS_PORT.md`](docs/MACOS_PORT.md).

**macOS pending — branch `mac-meters` (NOT merged):** a native LED **audio meter** + a libVLC audio
tap (`libvlc_audio_set_callbacks` → peak metering + AudioQueue re‑output; mac start of the Win32
SpectrumTap). Also **enabled ARC** for the mac `.mm` (they were compiled MRC and leaking every
window/view). Held back because it changes the audio **output path** — needs an on‑device audio test
+ an adversarial‑review pass before merge. After that, the first Mac release needs a **signed +
notarized** build to light up the live Sparkle update flow (populate `mac/packaging/appcast-mac.xml`).

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
Tags `v0.1.0`…`v0.1.7`; **v0.1.7 released @ `de8c571`** (full `0.1.7.52`; appcast @ `12be931`).
`HEAD == origin/main` is this "mark 0.1.7 shipped" doc commit (commit count **54**; release
`de8c571` = 52, appcast-publish `12be931` = 53). **The macOS team pushes to `main` too** (mac
Phase-1), so **`git fetch` + rebase before a release** — 0.1.7 rebased onto their concurrent pushes
(count jumped 39→52 mid-cut, forcing a rebuild). Working tree clean.
Build number = git commit count, baked at CMake configure time **after** the commit — so a build
must follow the release commit to stamp the matching `0.1.5.<count>`. Commit/push only when the
owner asks; stage **specific paths** (the owner keeps adding `art/*.png` — never `git add -A`); end
commit messages with the Co-Authored-By trailer.

## Immediate next steps (pick up here)

1. **Nothing is blocking** — 0.1.7 is shipped and live. Pick the next item from
   **[`BACKLOG.md`](BACKLOG.md)** when ready; the headline is the **theme engine** (0.2.x epic —
   full reskin + selectable D3D11/shader skins). Before starting it: write `docs/THEME_ENGINE.md` and
   **flag the shared skin-model boundary to the macOS team** (skin *model* in `common/`, *renderer*
   per-platform — see `BACKLOG.md` + memory `rabbitears-cross-platform`).
2. **macOS Phase-1** continues on `main` (macOS team: native grid, playback, Sparkle, CI `.app`).
   Windows side: keep `common/` green (the `mac-core` CI is the drift alarm), review their PRs, and
   **`git fetch`/rebase before every release** — `main` is shared now (0.1.7's build count jumped
   39→52 mid-cut because of concurrent mac pushes).
3. **Easy point-release candidates** from the backlog: the dialog work-area clamp + shared
   `runModalLoop`, resume-last-channel, DPI-change relayout — small, ship as 0.1.x.

## Seed prompt for a new session

Paste this verbatim to start a fresh session with working context restored:

> You are continuing work on **RabbitEars** (`G:\RabbitEars`), a native **Windows
> Win32 / C++20** IPTV player on **libVLC 3.0.23**, themed to match its sibling apps
> `G:\SQLTerminal-Win32` and `G:\ManorLords-SGE` (dark "Claude-desktop" look, coral
> `#D97757`, custom `WM_NCCALCSIZE` title-bar chrome, CMake + Ninja + MSVC via **VS
> 2026 Community**, deps vendored/NuGet, **no VS project**). **Read `HANDOVER.md` and
> `docs/architecture.md` first.**
>
> **State: v0.1.7 is SHIPPED** (tag `v0.1.7` @ `de8c571`, full `0.1.7.52`, signed installer +
> **live WinSparkle auto-update**; appcast @ `12be931`, `origin/main` == the "mark shipped" doc
> commit, commit count 54). 0.1.7 = the **update-from-About fix** (the About box's modal loop
> swallowed WinSparkle's `WM_QUIT`, so the app lingered and the installer failed — the loop now
> re-posts `WM_QUIT`, plus a 2.5s force-exit net) + an **About-box bunny easter egg** + **restructure
> packaging fixes** (`build\Win32\` paths). Prior: 0.1.6 = the update-on-quit fix + meter feel knobs +
> random splash; 0.1.5 = the meters overhaul. **The macOS Phase-2 restructure has LANDED** — the tree
> is now `common/` + `Win32/` + `mac/` under a unified root CMake; the macOS team is mid **Phase-1**
> (native grid + playback + Sparkle + CI `.app`), and pushes to `main`, so **`git fetch`/rebase before
> a release**. **The theme engine** (full reskin + selectable D3D11/shader skins) is the headline
> **backlog** item — see `BACKLOG.md` (write `docs/THEME_ENGINE.md` + flag the shared skin-model
> boundary to the macOS team before starting). **JSON profiles** stay deferred. **Read the "0.1.7 — SHIPPED" + "Current state"
> sections of `HANDOVER.md` for full detail.**
>
> **Cross-platform (important, see memory `rabbitears-cross-platform`):** RabbitEars is going
> **macOS** — premium per platform, ~70% common core (engine + meter model/config) + platform-specific
> graphics. The macOS team is writing **`MACOS_PORT.md`**; the repo gets **restructured** once it
> lands — **do NOT preemptively restructure**; keep the meter *model ↔ renderer* seam clean. **JSON
> profiles** are deferred to a later version. (Prior: 0.1.4 fixed the audio meter [agile
> `IAgileObject`], meter animation, and splitter drag, and added the import-results dialog, rename
> playlists, and the splash version; 0.1.3 added the dockable layout + by-country filter.)
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
