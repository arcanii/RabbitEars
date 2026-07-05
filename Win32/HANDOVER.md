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

## Current state — v0.2.0 SHIPPED · **0.2.1 in dev** (EPG + Scheduled Recordings, branch `epg-xmltv` / PR pending) · macOS Phase-1

**Released:** **`v0.2.0`** (2026-07-04), tag `v0.2.0` @ `343aa0e`, full version `0.2.0.107`, signed
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

### 📺 EPG + ⏺ Scheduled Recordings (0.2.1 dev — branch `epg-xmltv`, **PR pending**)

The 0.2.1 feature pair, built + committed + pushed on **`epg-xmltv`** (11 commits; branched off `main` @
`bc74015`, `main` unchanged since). Open the PR at `github.com/arcanii/RabbitEars/pull/new/epg-xmltv`
(the `pr_body.md` draft is in the session scratchpad; **`gh` here is installed but not authenticated** —
needs `gh auth login`). All new **core** lands in `common/` and is **headless-tested** via
`RabbitEarsCli --selftest` (42 assertions incl. gzip, XMLTV parse, the v2→v4 migration, and the pure
scheduler); the **GUI is build-verified BOTH theme flags but NOT runtime-verified** (sandbox can't launch
it) — the owner's runtime pass is the last gate before merge.

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
  - CLI: `--epg <url|file>` (fetch → gunzip → parse → summary). **EPG-source caveat (important):** the guide
    only shows channels the *feed* covers. iptv-org's `index.m3u` (13,069 channels) declares
    `x-tvg-url=…worker-9dd4.onrender.com/guide.xml.gz`, which is a **2-channel / 114-programme stub** — so the
    guide shows ~2 rows. **Not a bug.** **Follow-up:** a per-playlist or global **custom EPG-URL override**
    (today the x-tvg-url is used as-is, no override UI).
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
- **App icon → clockwork** (`packaging/app.ico` regenerated from `art/clockwork_icon.png`;
  `scripts/make_ico.py` repointed at it — needs Pillow, absent here so the .ico was built via a
  System.Drawing PowerShell one-off) + README badge; two more studies (`happy`/`style`) checked in.
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
Tags `v0.1.0`…`v0.2.0`; **v0.2.0 released @ `343aa0e`** (full `0.2.0.107`; appcast @ `7b3946a`) — the
theme engine, theme-ON by default. The `theme-engine` branch was **merged to `main` + deleted** (only
`main` remains; PR #16 superseded + closed). **The macOS team pushes to `main` too** (mac Phase-1), so
**`git fetch` + rebase before a release** — the 0.2.0 push integrated a concurrent mac commit mid-flight
(the first push was rejected until re-fetched). Working tree otherwise clean (the owner's
`art/logo_basic*.png` stay untracked). Build number = git commit count, baked at CMake configure time
**after** the commit — so a build must follow the release commit to stamp the matching `0.2.0.<count>`. Commit/push only when the
owner asks; stage **specific paths** (the owner keeps adding `art/*.png` — never `git add -A`); end
commit messages with the Co-Authored-By trailer.

## Immediate next steps (pick up here)

1. **0.2.1 IN DEV — EPG + Scheduled Recordings + clockwork icon, branch `epg-xmltv` (pushed, PR pending).**
   See the "📺 EPG + ⏺ Scheduled Recordings" section above. Core is headless-tested; the GUI is
   build-verified both flags but **needs the owner's runtime pass** (sandbox can't launch it): confirm the
   clockwork icon, the guide renders, click → Play / Schedule, the manager + manual dialog, and an actual
   near-future recording. **Then:** open + merge the PR (`gh auth login` first, or the compare link) → cut
   **0.2.1** (version bump is done; build → sign-on-mac → appcast per `docs/RELEASING.md`). ⚠️ **Build with
   `-DRABBITEARS_THEME_ENGINE=ON` explicitly** — build dirs cache the flag (a stale theme-OFF cache once
   shipped a Theme-menu-less exe during the 0.2.0 live pass).
2. **Roadmap after 0.2.1** (memory `rabbitears-feature-roadmap`): a **custom EPG-URL override** (the guide
   only shows what the *feed* covers — iptv-org's index.m3u ships a 2-channel stub EPG), then the owner's
   video roadmap — **PIP**, **multiple simultaneous views**, **split view** — all of which sit on the
   **multi-player engine** keystone (today one `VlcPlayer` = one worker + one video HWND; N players unlock
   split-view / PIP / **concurrent recording**).
3. **macOS Phase-1** continues on `main`; keep `common/` green (the `mac-core` CI is the drift alarm) and
   **`git fetch`/rebase before every release** — `main` is shared (0.1.7's build count jumped 39→52 mid-cut
   from concurrent mac pushes). Aside: the mac `HANDOVER.md` is stale — PR #22 is merged; its "E3"
   `MeterModel`→`common/ui` promotion is now post-merge backlog.

## Seed prompt for a new session

Paste this verbatim to start a fresh session with working context restored:

> You are continuing **RabbitEars**, a native **Windows Win32 / C++20** IPTV player on **libVLC 3.0.23**
> with a shared **`common/`** core (also feeds the macOS app), dark "Claude-desktop" chrome (coral
> `#D97757`, custom `WM_NCCALCSIZE` title bar), CMake + Ninja + MSVC (VS 2026), deps vendored/NuGet.
> **Read `Win32/HANDOVER.md` first — the "📺 EPG + ⏺ Scheduled Recordings" + "🎨 Theme engine" sections —
> plus the recalled memories.**
>
> **State:** last SHIPPED = **v0.2.0** (theme engine, theme-ON by default). **In dev = 0.2.1** on branch
> **`epg-xmltv`** (pushed, **PR pending** — 11 commits off `main`@`bc74015`): the **EPG** (XMLTV — vendored
> miniz gunzip + `common/core/XmltvParser` + schema v3 `epg_programmes`; a Direct2D **TV Guide** window
> `Win32/ui/EpgGuideControl` + `Settings ▸ Refresh Guide`; click an entry → Play/Schedule popup) and
> **Scheduled recordings** (schema v4 `scheduled_recordings` + a **pure, unit-tested**
> `common/core/RecordingScheduler::planScheduler` applied by a ~30s `kSchedulerTimer` tick;
> `Settings ▸ Scheduled Recordings…` manager + a manual `scheduleDialog`; **one-at-a-time,
> app-must-be-running**). Plus the **clockwork app icon** (`packaging/app.ico`) and the **0.2.1** version
> bump (4 places; mac keeps 0.1.9). Core is headless-tested (`RabbitEarsCli --selftest`, 42 assertions incl.
> the v2→v4 migration + planScheduler); the **GUI is build-verified both theme flags but NOT
> runtime-verified** — the owner's runtime pass is the gate before merge. Both big UI surfaces were
> adversarially reviewed.
>
> **Immediate next:** owner runtime-verifies the GUI → open/merge the `epg-xmltv` PR
> (`github.com/arcanii/RabbitEars/pull/new/epg-xmltv`; `gh` here needs `gh auth login`) → cut **0.2.1**
> (bump done; build → sign-on-mac → appcast per `docs/RELEASING.md`). **EPG caveat:** the guide shows only
> channels the *feed* covers — iptv-org's index.m3u ships a 2-channel stub EPG; a **custom EPG-URL override**
> is a good follow-up. **Roadmap** (memory `rabbitears-feature-roadmap`): the **multi-player engine** is the
> keystone that unlocks split-view / PIP / concurrent recording.
>
> **Build/verify** (PowerShell): `& "<repo>\scripts\build.cmd" -DRABBITEARS_BUILD_GUI=ON -DRABBITEARS_THEME_ENGINE=ON`
> then `build\Win32\RabbitEarsCli.exe --selftest`; core-only headless (no libVLC): `& "<repo>\scripts\build.cmd"`.
> Gotchas: `cmake`/`cl` NOT on PATH — use `scripts\build.cmd`; Windows outputs in `build\Win32\`; **LINK1168 =
> a running RabbitEars.exe locks the exe** → `Stop-Process -Name RabbitEars -Force`, rebuild; ⚠️ build with
> `-DRABBITEARS_THEME_ENGINE=ON` explicitly (build dirs cache the flag); static CRT (`/MT`, no redist); the
> sandbox **can't launch the GUI** — build-verify + reason, owner runtime-verifies; `common/` stays mac-safe
> (`mac-core` CI compiles it on clang). Commit only when asked; stage **specific paths** (never `git add -A` —
> the owner adds `art/*.png`); end commits with the `Co-Authored-By` trailer; branch off `main`, PR back.
>
> ---
> *The prior theme-engine-era seed prompt follows (still-useful build gotchas + the 0.2.0 phase history):*
>
> You are continuing work on **RabbitEars** (`G:\RabbitEars`), a native **Windows Win32 / C++20** IPTV
> player on **libVLC 3.0.23**, themed to match its sibling apps `G:\SQLTerminal-Win32` and
> `G:\ManorLords-SGE` (dark "Claude-desktop" look, coral `#D97757`, custom `WM_NCCALCSIZE` title-bar
> chrome, CMake + Ninja + MSVC via **VS 2026 Community**, deps vendored/NuGet, **no VS project**).
> **Read `Win32/HANDOVER.md` first — especially the "🎨 Theme engine" section — plus
> `Win32/docs/THEME_ENGINE.md`.** (Team docs live under `Win32/` now, not the repo root, so they don't
> collide with the mac team; the mac team owns `mac/` + shares `common/` and root `docs/`.)
>
> **State:** last SHIPPED release is **v0.2.0** (`343aa0e`, `0.2.0.107`, appcast `7b3946a`) — **the theme
> engine**, merged to `main` and **theme-ON by default** (the `RABBITEARS_THEME_ENGINE` flag default was
> flipped ON; `-DRABBITEARS_THEME_ENGINE=OFF` still builds the parity path). ⚠️ **Build the GUI with
> `-DRABBITEARS_THEME_ENGINE=ON` explicitly** — build dirs cache the flag (a stale OFF cache once shipped a
> Theme-menu-less exe). The `theme-engine` branch was merged + deleted (only `main` remains); auto-update
> `0.1.7 → 0.2.0` is verified; and `packaging/app.ico` was swapped to the macOS icon on `main` for 0.2.1.
> The theme engine, phase by phase (all shipped in 0.2.0) — committed +
> reviewed (0 findings) + build-verified both flags: **Phase 1** — the D3D11⇄D2D1.1 interop device
> `Win32/ui/skin/SkinDevice` + a **windowless** transport-strip HLSL **underglow** (offscreen
> GDI-compatible texture → child-clipped `BitBlt`; a swapchain `Present` bypasses GDI sibling-clipping so
> it CAN'T be used over the controls); **Phase 2a** — shared model **`common/ui/Skin.{h,cpp}`**
> (SkinColor/SkinPalette/SkinFont/Skin + registry + UTF-8 codecs, 14 CLI selftests — the FIRST model
> physically in `common/`); **Phase 2b** — `currentTheme()`/`themeBrush()` in `Theme.h` re-backed by the
> active skin (parity: the dark skin == `makeDarkTheme` exactly); **Phase 2c** — live **Settings→Theme**
> switch (Follow System/Dark/Light), persisted, whole-app repaint broadcast (`applyActiveSkin`); **Phase
> 3a** — `themeFont(role,dpi)` typography seam + `dangerHover` wiring; and a **Cyberpunk** skin (neon
> magenta on midnight, **colours only, no shaders**) with a registry-driven Theme menu. **Done since then:**
> 3b (fonts → a `themeFont`/`themeTextFormat` seam), 3c (owner-draw transport buttons), 4a (**Steampunk**
> skin + Georgia serif title), 4b-1 (GDI+ **button glow**), 4b-2 (per-skin **neon glow on the dock gutters**
> — a new `PSEdge` shader via the windowless SkinStrip/`BitBlt`), the per-skin **`SkinGpu` manifest**
> (`{stripGlow, edgeGlow, heatHaze}` on the `common/` Skin model — `SkinStrip` reads `currentSkin().gpu.*`
> instead of hardcoded intensities; codec + selftests), and **Steampunk heat-haze** (a procedural brass
> shimmer on the strip underglow — `underglow.hlsl` `PSMain` wobble + rising plume gated by `heatHaze`, a
> no-op for every other skin; new `uParams` cbuffer field). Plus **ungated UI** (`e6b83d7`, ships both
> flags): a meters **Data-flow row** + "Meters…" menu cleanup + buffer-meter half-width/`LED_PITCH 5→3`, and
> a windowed **Video-only** mode (Settings→Video only / **Ctrl+Shift+V**; hide all chrome; drag the video to
> move; double-click/Esc/right-click to exit; libVLC input passthrough so it works while playing). All
> committed + reviewed SHIP + both-flag build-verified; **all owner-verified live in 0.2.0**. The
> **authored GPU-effect set is complete** (strip, gutter, button glow, heat-haze). **Next (0.2.x):** cut
> **0.2.1** (the macOS-icon swap is already on `main`; bump the version + build → sign-on-mac → appcast per
> `docs/RELEASING.md`); optional per-skin glow/heat-haze *tuning* (`SkinGpu` in `common/ui/Skin.cpp` +
> `underglow.hlsl`); Steampunk palette/serif polish; extend `SkinGpu` to the button glow; refresh the
> About/Splash logo art; or reskin a new surface. **Coordination:** the shared contract is
> `docs/SKIN_MODEL.md` (the mac Metal renderer mirrors it); `main` is shared with the mac team, so
> **`git fetch`/rebase before every release**. **JSON profiles** stay deferred.
>
> **Cross-platform (memory `rabbitears-cross-platform`):** premium per platform, ~70% common core; the
> **skin MODEL is shared in `common/`, the RENDERER is per-platform** (Win32 D3D11/D2D+HLSL; mac Metal
> later). Ratified: skins define their OWN colours (no OS `GetSysColor`); active-selection + OS dark/light
> detection are renderer-side, only the id + `skinSettingKey()` are shared; the positional palette codec
> is frozen until user skins ship. Keep `common/` graphics-free (the `mac-core` CI compiles it on clang).
>
> The GUI: owner-draw command bar + Settings menu; three **dockable regions** (nav TreeView /
> video+transport strip / Direct2D `ChannelGridControl`, persisted `dock_layout`); a `VlcPlayer` (libVLC
> lifecycle + headless recorder on a worker thread); a blocky-LED buffer meter + modular mini-meters
> (`ui/MiniMeter` + WASAPI process-loopback `audio/SpectrumTap`); log at `%LOCALAPPDATA%\RabbitEars\rabbitears.log`.
>
> **Build/verify** (from PowerShell):
> ```
> & "G:\RabbitEars\scripts\build.cmd" -DRABBITEARS_BUILD_GUI=ON -DRABBITEARS_THEME_ENGINE=ON   # theme-engine dev
> & "G:\RabbitEars\scripts\build.cmd" -DRABBITEARS_BUILD_GUI=ON -DRABBITEARS_THEME_ENGINE=OFF  # verify shipping path
> build\Win32\RabbitEarsCli.exe --selftest    # core + dock + country + skin-model/SkinGpu assertions (headless)
> ```
> Every theme-engine change: adversarially reviewed via a background **Workflow** (independent lenses) +
> build-verified **BOTH flags** before committing. **flag-off must stay byte-identical to shipping** —
> every engine addition is `#ifdef RABBITEARS_THEME_ENGINE`-gated. Gotchas: `cmake`/`cl` aren't on PATH —
> use `scripts\build.cmd`; Windows outputs are in `build\Win32\`. `LINK1168` = an instance running →
> `Stop-Process -Name RabbitEars -Force`, rebuild. Static CRT (`/MT`, **no redist** — shaders are
> precompiled `.cso` embedded as C headers via `bin2h.cmake`, never a runtime DLL). `themeBrush()` is a
> per-skin `unordered_map` flag-on (the 12-slot array flag-off). **This sandbox CANNOT launch the GUI** —
> build-verify + reason + run `RabbitEarsCli` headless; hand visual verification to the owner. Commit only
> when asked; stage specific paths (never `git add -A` — owner adds `art/*.png`); end commits with the
> Co-Authored-By trailer. Also: libVLC `stop()`/`release()` block (offloaded to reaper threads); event
> callbacks → only `PostMessage`; RECT fields are `LONG` (cast to int); channel grid D2D pinned to 96 DPI;
> libVLC `i_read_bytes` is 0 for HLS; VLC sout single-quoted paths need `'` doubled; modal dialogs read
> controls before `DestroyWindow`. Releases signed on macOS with the shared family Ed25519 key —
> `docs/RELEASING.md`.
