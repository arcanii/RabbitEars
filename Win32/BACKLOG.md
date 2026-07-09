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
