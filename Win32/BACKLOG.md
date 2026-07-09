# RabbitEars — Backlog

Parked / not-yet-scheduled work, split out of `Win32/HANDOVER.md` (which stays the single entry
point for **current state**; this is the parking lot for **what's next**). Ship small items as 0.1.x
point releases; the **theme engine** is the big 0.2.x epic. Windows-team doc — kept under `Win32/`
so it doesn't collide with the macOS team's root-level edits (they own `mac/`).

---

## 🎨 Theme engine — the big 0.2.x epic

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

## 🖥️ Native ARM64 build (Windows-on-ARM)

**Owner-directed (backlogged 2026-07-08).** Today the app is **x64-only** (`scripts/build.cmd` →
`vcvars64.bat`, `/machine:x64`; `cmake/LibVlc.cmake:31` hardcodes `build/x64`). It runs fine on
Windows-on-ARM via the OS's **x64 emulation** (owner runs it on ARM Windows, "works well") — but a
**native ARM64 build** would give native speed + better battery, no emulation.

**Design (owner's idea): one x64 launcher that self-selects the native binaries.** Ship a small
**x86-64 launcher** (runs everywhere — natively on x64, emulated on ARM). At startup it detects the
**native** machine arch (`IsWow64Process2` → `NativeMachine == IMAGE_FILE_MACHINE_ARM64`, or
`GetNativeSystemInfo`) and launches the matching binary set — **ARM64 binaries on ARM**, x64
otherwise. One distributable, native on both. (Alternative: an arch-specific installer per download,
simpler but two artifacts.)

**Dependency status (checked 2026-07-08):**
- ✅ **libVLC 3.0.23** — the NuGet already unpacks `build/arm64` (full `libvlc.dll` + `libvlccore.dll`
  + `plugins/`). Usually *the* WoA blocker; already solved.
- ✅ **SQLite, miniz** — vendored C source, compile for any target.
- ❌ **WinSparkle** — `third_party/winsparkle/lib` has only an x64 `WinSparkle.lib`; need an ARM64
  build (upstream ships ARM64 in recent releases) — vendor it alongside.
- ⚙️ **Toolchain / CMake** — add an ARM64 configure (ARM64 MSVC cross-tools, e.g.
  `vcvarsamd64_arm64.bat`) + make `LibVlc.cmake`'s `build/x64` arch-conditional (`build/arm64`).
  Installer (`packaging/installer.iss`) + `build-installer.cmd` bundle both binary sets + the launcher.

**Scope:** self-contained, not blocking any feature work. Own point release when picked up.

---

## ⏱️ Slow first startup — ship a pre-generated libVLC `plugins.dat`

**Diagnosed 2026-07-09 (owner reported "first startup is very slow").** libVLC scans all **323 plugin
DLLs** at startup because there is **no `plugins.dat` cache**. Log evidence: the whole delay is inside
libVLC init (session-start → "libVLC initialized"), and it is **cold-cache-bound** — ~10.6 s right after a
rebuild re-copies the plugins (cold on disk) vs ~0.8 s once the DLLs are warm in the OS file cache. The DB
open right after is fast (0.2 s / 13 k channels), so it is neither the DB nor the playlist.

**Worse for installed users:** `packaging/installer.iss` ships plugins into `{app}\plugins` (Program Files,
**read-only** for a standard user), so libVLC can never write a cache there and **rescans all 323 plugins on
every launch** — first launch (cold) is the ~10 s worst case.

**Fix:** ship a pre-generated **`plugins.dat`** next to the plugins. With it present libVLC loads the cache
instead of scanning → fast startup everywhere (dev + installed, cold or warm).
- Generate it with libVLC's **`vlc-cache-gen`** as a post-plugin-copy build step (`cmake/LibVlc.cmake`) →
  writes `build/Win32/plugins/plugins.dat`; the installer's `plugins\*` glob then ships it automatically.
- **Blocker:** `vlc-cache-gen.exe` is NOT in the `VideoLAN.LibVLC.Windows` NuGet (it ships no tools) — source
  the one matching libVLC **3.0.23** from the VLC Windows build and vendor it (small binary). Must match the
  libvlccore ABI exactly or the cache is rejected and libVLC silently rescans.
- Secondary lever: **trim** the plugin set (RabbitEars only needs HTTP/HLS access, common demuxers/codecs,
  the Direct3D11 vout, mmdevice aout) — shrinks the scan but doesn't eliminate it; risky (removing a needed
  plugin breaks playback), so cache-gen is the primary fix.

**Scope:** own point release (candidate 0.2.5). `VlcEngine::init` args (`Win32/ui/VlcEngine.cpp:37`) don't
disable caching, so no code change there — purely a build/packaging change.

---

## 📺 TV Guide ↔ channel enhancements (owner-requested 2026-07-09)

Two follow-ups off the 0.2.4 TV-Guide polish:
- **Favourite a channel from the guide.** Right-click a guide row → **"Add to Favourites"** (toggle), so you
  can star channels without leaving the guide. The guide already carries each row's full tvg-id
  (`GuideRow::channelId`) → resolve to the `Channel` (`Database::channelByTvgId`) and toggle its favourite flag
  (same one the grid's `COL_FAV` uses). Add the action to `EpgGuideControl`'s right-click menu, next to the
  existing Play / Schedule (`GuideCallbacks`).
- **"Show in TV Guide" from a channel.** Right-click a channel in the grid → jump to that channel's row in the
  guide: open/reveal the guide (reuse `revealEpgGuide`) and scroll it to that channel (vertical to the row,
  horizontal to "now"). Hook the channel grid's context menu (`ChannelGridCallbacks::onContextMenu`) → a new
  `EpgGuideControl` "scroll to channel <tvg-id>" entry point.

**Scope:** small UX features in the guide + channel-grid context menus; no schema change. Bundle into a
TV-Guide polish point release.

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

- **Recording Phase 2 (scheduled)** — DB schedule table + dialog + a timer firing the headless
  recorder at set times (app must be running). **Phase 3** — Windows Task Scheduler wake-to-record +
  EPG-driven scheduling.
- **Recording formats** — MP4 (record `.ts`, remux on stop — MP4 isn't crash-safe if written live)
  and **transcoding** (format/quality/size presets; CPU-heavy).
- **EPG** (XMLTV now/next; `tvg-id` already stored) + a **background dead-link checker** (so "Hide
  unavailable" isn't purely passive). **Import/export favourites.**
- **Resume last channel** on launch (`last_channel_id` is already persisted).
- **Named saved layouts** (`DockLayout` already serializes any tree).
- **Group-title country fallback** for Xtream feeds (whose channels lack `tvg-id` country codes).

## Polish / cleanup

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

- **Authenticode** code-signing of the exe + installer (silences SmartScreen; separate from the EdDSA
  update signature).
- **Portable-zip** artifact on releases (alongside the auto-updating installer).
