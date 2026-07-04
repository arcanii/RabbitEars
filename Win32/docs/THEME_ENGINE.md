# RabbitEars — Theme Engine (0.2.x epic)

**Design doc — Windows team.** Kept under `Win32/docs/` so it doesn't collide with the macOS
team's root-level edits. The **shared skin-model boundary** — extracted to the standalone
[`docs/SKIN_MODEL.md`](../../docs/SKIN_MODEL.md), summarized in [§4](#4-the-shared-skin-model-boundary--macos-team-read-this)
— is the one part both teams must agree on before any engine code lands.

- **Status:** DESIGN — no engine code yet. Branch `theme-engine` (off `main` @ `c6a0cf2`).
- **Scope:** a 0.2.x epic across several point releases, not one PR.
- **Verification:** every phase is **owner-verified at runtime** — GUI rendering cannot be checked
  in the dev sandbox (it can't launch the exe). Land each phase behind a build flag, off by default.
- **Grounding:** the "current state" claims below were mapped directly from the code (file:line
  anchors are real). Read them before proposing changes — several contradict older assumptions.

---

## 1. Goal & non-goals

**Goal (owner-directed, from [`Win32/BACKLOG.md`](../BACKLOG.md)):** a **full-app reskin** with
**runtime-selectable skins** — **Dark** (today's Claude-desktop look, coral `#D97757`), **Steampunk**,
**Cyberpunk** (mockups exist) — powered by **Direct2D-on-D3D11 interop + HLSL pixel shaders** for
animated GPU effects (flowing neon, heat-haze, reactive glow). Switchable live from Settings → Theme,
persisted.

**Non-goals (for now):**
- **Shader-over-video.** libVLC owns its own D3D11 vout in a child HWND; we skin *around* video, not
  over it (see [§6.6](#66-the-libvlc-video-surface-constraint)).
- **Rewriting everything in raw D3D.** The grid is already Direct2D; it folds into the interop device.
  D2D 1.1 stays the primary 2D API; raw D3D11 is only for full-surface background shader fills.
- **A cross-platform *renderer*.** Only the skin **model** is shared. Windows renders in D3D11/D2D+HLSL;
  macOS mirrors later in Metal/Core Graphics. Neither team's renderer is a shared artifact.
- **JSON profiles** and other backlog items — orthogonal, still deferred.

---

## 2. Current architecture — the honest baseline

Five subsystems were mapped from source. The reality is messier than the handover implied; the doc
plans around what's actually there.

### 2.1 Theming is a flat GDI palette, Win32-only
[`Win32/ui/Theme.h`](../ui/Theme.h) (header-only, ~169 lines) is the whole system: a POD `struct Theme`
of **17 `COLORREF` fields + a `bool dark`** (`Theme.h:16-35`). There is **no runtime skin concept** —
exactly two immortal instances (`makeDarkTheme()` `Theme.h:37`, `makeLightTheme()` `Theme.h:60`) selected
by `currentTheme()` (`Theme.h:100`) on a 3-state global `themeOverride()` (`-1` follow-system / `0` light
/ `1` dark, `Theme.h:95`). `common/` has **zero** theme/color code.

- **Access surface is load-bearing:** `currentTheme()` is called **~28×** across 7 files (MainWindow 10,
  Dialogs 11, + BufferMeter/ChannelGrid/MiniMeter/Splash); the `currentTheme().field` idiom is deeply
  embedded. `themeBrush()` (`Theme.h:114`) is used **~15×**. **Any refactor must keep these shapes.**
- **`themeBrush()` caches only 12 colors** (`static COLORREF colors[12]`, `Theme.h:115`) and silently
  **leaks past 12** (creates but doesn't cache). Three skins' palettes will blow this.
- **Fonts are NOT in `Theme`.** ~14 sites ad-hoc `CreateFontW("Segoe UI"...)` / `CreateTextFormat` with
  hardwired px sizes (MainWindow.cpp:1305-1313, Dialogs ×7, ChannelGrid, BufferMeter, Splash). There is
  **no font access pattern to intercept** — adding typography to a skin is net-new surface.
- **High-cardinality bypass:** per-cell meter ramps deliberately avoid `themeBrush()` and use
  `SetDCBrushColor` + `GetStockObject(DC_BRUSH)` (MiniMeter.cpp:94-97, rationale at 34-37). The skin
  engine must keep exposing raw colors (not only `HBRUSH`) for this path and for D2D.
- **Platform glue lives in Theme.h too** and stays Win32-side: `systemUsesDarkMode()` (registry read,
  `Theme.h:85`), `applyDialogDarkMode()` (DWM, `Theme.h:141`), `dialogCtlColor()` (`Theme.h:130`).

### 2.2 The only D2D surface is the grid — and it's Direct2D **1.0**
[`Win32/ui/ChannelGridControl.cpp`](../ui/ChannelGridControl.cpp) draws to an **`ID2D1HwndRenderTarget`**
(`GridState::rt`, line 221) created via `ID2D1Factory::CreateHwndRenderTarget` (`ensureDevice()`,
lines 277-295). **There is no D3D11 device, no DXGI swapchain, no `ID2D1DeviceContext`, no interop
anywhere in the app.** [`Win32/ui/D2DSupport.h`](../ui/D2DSupport.h) provides only device-*independent*
singletons: a plain `ID2D1Factory` (`D2D1_FACTORY_TYPE_SINGLE_THREADED`, includes `d2d1.h` only — **no**
`d2d1_1.h`/`d3d11.h`/`dxgi.h`) and an `IDWriteFactory`, plus `colorToD2D(COLORREF)→D2D1_COLOR_F` (`:38`).

- **96-DPI pin is load-bearing** (`ChannelGridControl.cpp:284-288`): the target is forced to 96 dpi so
  1 D2D unit == 1 physical pixel, because draw and hit-testing (`rowAtY`/`colAtX`, lines 384-397) share
  raw client-pixel space; the app scales itself via `dpx()`. **Any migrated device context must
  `SetDpi(96,96)` or all mouse math silently breaks** — the single highest-risk part of the migration.
- Frame: `BeginDraw`→`Clear(panelBg)`→windowed rows→`EndDraw`; **no explicit `Present`** (the HwndRT
  presents in `EndDraw`, line 575). Device loss: `D2DERR_RECREATE_TARGET`→`discardDevice()` (line 271).
- DirectWrite (`recreateFormats()`, WIC logo decode on 3 worker threads) is device-independent and
  **ports unchanged**.

### 2.3 The meter model↔renderer seam — the precedent, with a caveat
The meter config (`MeterStyle`/`MeterPalette`/`MeterTuning`/`MeterConfig` + `meter*To/FromString`
codecs) is the shape the skin model should mirror: POD structs + free-function string codecs targeting
the SQLite settings K/V store, an unknown-token→fallback discipline, a `MeterStyle` discriminant the
renderer switches on.

> ⚠️ **Correction to the handover.** This "shared meter model" is **NOT physically in `common/`** — every
> type and codec is in [`Win32/ui/MiniMeter.h`](../ui/MiniMeter.h) / `MiniMeter.cpp`, the *same*
> translation unit as the GDI/GDI+ renderer. It is `COLORREF`-typed (`MeterPalette`, MiniMeter.h:28) and
> `swprintf_s`/`wcstod`-based; it **cannot** be included from `common/` without `windows.h`. The
> physical split the handover describes was never executed. **The skin model will be the first model to
> actually live in `common/`** — it executes the split the meter seam only gestures at. (Lifting the
> meter model into `common/` alongside it is a sensible, optional follow-on.)

Reusable lessons from the meter codec: stable token for the discriminant; fixed-arity comma-joined
field lists; hex `RRGGBB` for colors; `%.3f`-clamped floats for 0..1 knobs; a `"theme"` sentinel for
follow-the-base-color; **exact field count required or return whole fallback, but per-field graceful
fallback**; the palette couples to the app theme at exactly one resolve point (`bg==CLR_INVALID →
currentTheme().windowBg`, MiniMeter.cpp:350) — kept in the renderer, not the model.

### 2.4 Owner-draw surfaces to reskin (full inventory in [Appendix A](#appendix-a-surface-inventory))
[`Win32/ui/MainWindow.cpp`](../ui/MainWindow.cpp) (2188 lines) hosts the shell chrome — all **raw GDI**,
no D2D: title-bar reclaim (`WM_NCCALCSIZE:1497`), command bar (`drawCmdBar:298`, GDI into an offscreen
`HBITMAP`, BitBlt in `WM_PAINT:1532`), transport-strip band fill (`WM_PAINT:1542`, raw `FillRect`), dock
gutters (`paintGutters:507`), drag grips (`DockGripProc:626`). Two bands use **different** theme colors
(command bar = `panelElevBg`; strip = `windowBg`) and there's a **hardcoded** close-hover
`RGB(196,43,28)` (`drawCmdBar:344`) — both must become skin-driven. Transport buttons are **classic
`BUTTON` children** with MDL2 glyphs (createChildren:1336-1355), not owner-draw — reskinning them to
glow needs `NM_CUSTOMDRAW`/owner-draw conversion. The video surface (`VideoProc:2028`, `BLACK_BRUSH`)
stays a plain letterbox — leave it alone.

### 2.5 Build & the common↔platform contract pattern
Root [`CMakeLists.txt`](../../CMakeLists.txt) (C++20, `/MT` static CRT, no redist) always builds
`RabbitEarsCore` from `common/`, then `Win32/` (WIN32) or `mac/` (APPLE). The GUI is gated behind the
OFF-by-default `RABBITEARS_BUILD_GUI` option (`:33`) — the exact pattern a new flag follows. Today the
GUI links only `d2d1 dwrite windowscodecs gdiplus`; **`d3d11`, `dxgi`, `d3dcompiler`, `dxguid` appear
nowhere** (all net-new). No `.hlsl`/`.cso`/`fxc` path exists — greenfield.

**The contract precedent to copy:** `common/` *declares* a symbol with no body — `Database::defaultDbPath()`
(`common/db/Database.h:37`), `rabbitears::httpGet` (`common/core/Http.h`), `initUpdater` (`common/platform/Updater.h`,
`#if`-branched signature) — and each platform lib *implements* it: `RabbitEarsPlatformWin`
(`Win32/platform/Paths.cpp`, `Http.cpp`) and `RabbitEarsPlatformMac` (`mac/platform/Paths.cpp`, `Http.mm`).
**The skin model↔renderer split mirrors this exactly.**

---

## 3. Target architecture: shared **model** ↔ per-platform **renderer**

```
                    common/  (RabbitEarsCore — graphics-free, links only sqlite3)
                    ┌───────────────────────────────────────────────────────┐
                    │  SKIN MODEL                                            │
                    │   • SkinColor (platform-neutral RGBA, no windows.h)   │
                    │   • Skin: palette roles + typography roles + id/name  │
                    │   • SkinRegistry: the N skins + active-skin selection │
                    │   • codecs: skin(Palette|Font|*)To/FromString (UTF-8) │
                    └───────────────────────────────────────────────────────┘
                           ▲  shared, agreed with mac team (§4)  ▲
             ┌─────────────┘                                     └─────────────┐
   Win32/  (RABBITEARS_THEME_ENGINE)                       mac/  (later, Metal/Core Graphics)
   ┌───────────────────────────────────┐                  ┌───────────────────────────────┐
   │ RENDERER (Windows)                │                  │ RENDERER (macOS)              │
   │  • resolve Skin → COLORREF Theme  │                  │  • resolve Skin → NSColor     │
   │    + HFONTs (keeps currentTheme())│                  │  • Metal/CoreImage shaders    │
   │  • per-skin brush cache           │                  │  • mirrors the same roles     │
   │  • SkinDevice: D3D11⇄D2D1.1 interop│                 └───────────────────────────────┘
   │  • HLSL effects (.cso via RCDATA) │
   └───────────────────────────────────┘
```

**The minimal-churn principle.** `currentTheme()` stays and keeps returning `const Theme&` (COLORREF).
Its *backing* changes: instead of two static instances, it returns a **resolved view of the active
skin**, rebuilt from the `common/` `Skin` model on every skin switch. The ~28 `currentTheme().field`
call sites and ~15 `themeBrush()` sites **do not change** — `currentTheme().accent` still reads a
`COLORREF`. This is the cheapest possible path from "flat palette" to "N runtime skins": the shared
model gains the color *values*; the Win32 side keeps the exact access idiom.

What genuinely changes on Win32:
1. `themeBrush()`'s global 12-slot cache → a **per-skin brush set**, cleared/rebuilt on skin switch
   (brushes are color-derived, so they're skin-scoped). Keep the `SetDCBrushColor` bypass for per-cell.
2. A **skin-switch broadcast**: `InvalidateRect(nullptr)` every window + re-run `applyDarkChrome`
   (DWM) + rebuild cached GDI/D2D resources (grid's `ID2D1SolidColorBrush`, ad-hoc fonts). Today the
   only "switch" is `applyDialogDarkMode`'s per-dialog DWM re-theme — generalize it.
3. Typography becomes skin-carried (a `SkinFont` per role → resolved `HFONT`s). This can lag to Phase 3
   since it's net-new — Phase 1/2 can keep the hardwired Segoe UI fonts.
4. The **`SkinDevice`** (D3D11⇄D2D1.1 interop, [§6](#6-windows-renderer-direct2d-on-d3d11-interop--hlsl))
   is a new Win32-internal renderer object for *animated* surfaces. It is **not** a `common/` contract —
   like the meter renderer, each platform owns its draw layer entirely; only the model is shared.

---

## 4. The shared skin-model boundary — **macOS team: read this**

> **This contract now lives canonically in [`docs/SKIN_MODEL.md`](../../docs/SKIN_MODEL.md)** — the shared
> root-`docs/` location the mac team already uses (next to `MACOS_PORT.md`), so it is reviewable without
> reaching into `Win32/`. That doc is the source of truth for the `common/` skin model both teams agree
> on; this section is now a pointer + the ratified summary. **When the contract changes, update
> `docs/SKIN_MODEL.md`, not this section.** The Windows renderer
> ([§6](#6-windows-renderer-direct2d-on-d3d11-interop--hlsl)) and the eventual Metal renderer are each
> team's own; they are explicitly *not* part of the contract.

The essentials — full detail + as-built struct/codec definitions in
[`docs/SKIN_MODEL.md`](../../docs/SKIN_MODEL.md):

- **Platform-neutral values, per-platform drawing.** [`common/ui/Skin.{h,cpp}`](../../common/ui/Skin.h)
  holds `SkinColor` (RGBA + an `inherit` "resolve against the base surface colour" flag — the old meter
  `CLR_INVALID`), a 14-role `SkinPalette`, three `SkinFont` roles (body/title/glyph), and the
  `Skin`/registry/codecs. No `windows.h`/`COLORREF`/`NSColor`/`d2d1`/`Metal`. Win32 converts to
  `COLORREF`+`HFONT`; mac to `NSColor`+`NSFont`.
- **Skins define their own colours (ratified)** — no OS `GetSysColor` inherit; the pre-engine light
  theme's three system lookups became literals. `SkinColor.inherit` is surface-relative, not OS-derived.
- **Shared registry, renderer-side active state.** `builtinSkins()` / `skinById()` / `skinSettingKey()`
  (`"skin"`) are shared; the active-skin holder + OS dark/light detection are per-platform. "Follow
  system light/dark" is a renderer-side meta-option that picks the dark-vs-light *skin*, never a colour.
- **The serialized string form is the cross-platform interchange** — positional 14-role CSV,
  exact-arity-or-whole-fallback with per-field graceful fallback; hex `RRGGBB`/`RRGGBBAA` or `"inherit"`.
  Frozen until user-customizable skins persist a palette (then add a version/arity prefix — position is
  identity, never reorder).
- **`common/` skin code stays graphics-free** — it compiles into `RabbitEarsCore` + the mac self-test
  with no GPU/UI deps.

*As built: [`common/ui/Skin.{h,cpp}`](../../common/ui/Skin.h), 14 CLI selftests (all pass). Open model
questions for the mac team: [§9](#9-open-questions).*

---

## 5. Skin abstraction on Win32 (renderer-side, Phase 2)

Grow [`Win32/ui/Theme.h`](../ui/Theme.h) into a thin **resolver** over the shared model:

- `currentTheme()` → returns a `const Theme&` **resolved from `activeSkin()`** (COLORREF-typed, cached,
  rebuilt on switch). Call sites unchanged.
- `themeBrush()` → backed by a **per-skin `HBRUSH` set** owned next to the resolved `Theme`; cleared on
  switch. Drop the fixed `[12]` arrays.
- New `themeFont(role)` → resolved `HFONT` per `SkinFont`; migrate the ~14 hardwired `CreateFontW` sites
  onto it (Phase 3 — net-new, can lag).
- `applySkinChrome(hwnd)` (generalized `applyDarkChrome`, MainWindow.cpp:237) → pushes the active skin's
  caption/border/text into DWM; called on skin switch, forces a non-client redraw.
- `onSkinChanged()` broadcast → `InvalidateRect(nullptr)` all top-levels + `discardDevice()` on D2D
  controls so brushes/bitmaps rebuild against the new palette.

Settings → **Theme** submenu (mirrors Settings → Meters): radio per skin, live apply, persisted.

---

## 6. Windows renderer: Direct2D-on-D3D11 interop + HLSL

The animated skins need `ID2D1Effect` graphs and custom pixel shaders — impossible on the legacy
`ID2D1HwndRenderTarget`. The migration target is **Direct2D 1.1** (`ID2D1DeviceContext`) on a **D3D11**
device. One shared `SkinDevice` owns the stack; animated surfaces get a flip-model swapchain.

### 6.1 The device stack
```cpp
UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;                 // MANDATORY for D2D interop
D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, _11_0, _10_1, _10_0 };
D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, _countof(levels),
                  D3D11_SDK_VERSION, &d3d, &got, &d3dCtx);      // WARP fallback on hard failure
ComPtr<IDXGIDevice1> dxgi; d3d.As(&dxgi);

ComPtr<ID2D1Factory1> f1;
d2dFactory()->QueryInterface(IID_PPV_ARGS(&f1));               // reuse the existing factory, upgraded
ComPtr<ID2D1Device> d2dDevice;  f1->CreateDevice(dxgi.Get(), &d2dDevice);
ComPtr<ID2D1DeviceContext> dc;  d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
dc->SetDpi(96, 96);                                            // preserve the load-bearing 96-pin
```
The existing `d2dFactory()` (`D2DSupport.h:18`) is `QueryInterface`'d to `ID2D1Factory1` in place — no
second factory. DirectWrite is untouched.

### 6.2 Flip-model swapchain per animated HWND
```cpp
DXGI_SWAP_CHAIN_DESC1 sd{};
sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;          // BGRA, D2D-compatible
sd.SampleDesc = {1,0};                            // flip model forbids MSAA on the back-buffer
sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;   sd.Scaling = DXGI_SCALING_STRETCH;
sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
dxgiFactory2->CreateSwapChainForHwnd(d3d.Get(), hwnd, &sd, nullptr, nullptr, &swap);
// target = back-buffer wrapped as ID2D1Bitmap1 (TARGET|CANNOT_DRAW), dc->SetTarget(bmp)
```

> ⚠️ **Phase-1 finding — a swapchain does NOT work for a surface with overlapping child controls.**
> The transport strip sits *behind* the transport buttons/meters, which are sibling child windows that
> overlap it. A DXGI swapchain's `Present` — **flip OR blt model** — composites the whole window rect
> through DWM and does **not** honor GDI sibling clipping, so it painted straight over those controls and
> hid them (regardless of z-order / `HWND_BOTTOM`; blt was tried and failed the same way — its `Present`
> also bypasses the sibling clip). What the spike actually ships: render the underglow (D3D11 pixel
> shader + a D2D hairline) to an **offscreen GDI-compatible texture** (`D3D11_RESOURCE_MISC_GDI_COMPATIBLE`),
> then **`BitBlt` it in the child's `WM_PAINT`** via `IDXGISurface1::GetDC`. GDI painting in a
> `WS_CLIPSIBLINGS` child *is* clipped to exclude overlapping siblings — the same mechanism the app's
> normal strip fill already relies on — so the controls always show through. Cost: a per-frame
> GPU→GDI blit (fine for a ~50px strip). **The two production-grade options for surfaces under many
> controls:** **(a)** draw the controls *into* the D2D/GPU scene (Phase-3 direction — no overlapping
> child HWNDs over GPU surfaces), or **(b)** **DirectComposition** (bind the swapchain to a DComp visual
> that composites correctly beneath child HWNDs). Pick one before Phase 3.

### 6.3 Migration with minimal churn
`ID2D1DeviceContext` **is** an `ID2D1RenderTarget`, so `BeginDraw`/`Clear`/`FillRectangle`/`DrawText`/
`CreateSolidColorBrush` are source-compatible — the body of `ChannelGridControl::paint()` is unchanged.
Only `ensureDevice`/`discardDevice`/`WM_SIZE` change: acquire the `dc` + per-control swapchain instead of
`CreateHwndRenderTarget`; add `swap->Present(1,0)` after `EndDraw`; resize via `SetTarget(nullptr)` →
release bitmap → `ResizeBuffers` → re-wrap. **Phase 1 leaves the grid on its legacy path** and runs only
the transport bar on the new device — they coexist via the shared factory.

### 6.4 Built-in effects (no shader authoring)
`CLSID_D2D1GaussianBlur` (meter bloom), `CLSID_D2D1Shadow` (panel shadows), and Glow =
Shadow→Blur→`CLSID_D2D1Composite`(`PLUS`) over the source (Cyberpunk neon rim). Created via
`dc->CreateEffect(...)`, drawn with `dc->DrawImage(effect)`.

### 6.5 Custom HLSL pixel shaders
Preferred path = **Direct2D custom effect** (`ID2D1EffectImpl` + `D2D1_PIXEL_SHADER`, registered with
`f1->RegisterEffectFromString`); a per-frame constant buffer (`{ float time; float w,h; float intensity; }`)
is pushed in `PrepareForRender` via `SetPixelShaderConstantBuffer`. For a pure full-surface background
fill, a **raw D3D11 fullscreen-triangle pass** (no vertex buffer, SV_Position-generated) is simpler and
cheaper, then `SetTarget` back to D2D for chrome on top — background-only (can't join the D2D graph).

### 6.6 The libVLC video-surface constraint
libVLC 3.0.23 runs its **own D3D11 vout** in a child HWND we hand it, presented on its decode thread. Our
swapchain and VLC's are independent windows in the same client area. We skin **around** the video (chrome,
nav, grid, transport, meters) and never composite over the VLC child. Shader-over-video would require
sharing VLC's texture (keyed mutex / shared handle) or a custom `libvlc_video_set_output_callbacks` D3D11
pipeline — **deferred**; not needed for "living chrome."

### 6.7 Animation loop & hygiene
Drive frames off the swapchain's **waitable object** (`MsgWaitForMultipleObjects`) — ≤1 frame/vsync, never
spin. **Animate only when visible:** stop presenting on `DXGI_STATUS_OCCLUDED`, halt on `SIZE_MINIMIZED`;
a static skin (Dark) renders once then only on invalidation — idle GPU/CPU stays ~0, matching today's
event-driven grid. **DPI:** `WM_DPICHANGED` refresh, keep `SetDpi(96,96)`. **Device-lost:**
`DXGI_ERROR_DEVICE_REMOVED`/`_RESET` from `Present`/`ResizeBuffers` rebuilds the whole `SkinDevice`
(one level up from the grid's `D2DERR_RECREATE_TARGET` recovery).

> ⚠️ The `layout()` `kSwpMove` bit-copy optimization (MainWindow.cpp:380) assumes strip pixels are stable
> during a move. A live-animated strip background invalidates that — animated skins likely force
> `SWP_NOCOPYBITS`/repaint for the strip.

---

## 7. Build & packaging

- **New flag** `RABBITEARS_THEME_ENGINE` (OFF by default) in root `CMakeLists.txt` beside
  `RABBITEARS_BUILD_GUI` (`:33`); gate C++ via `target_compile_definitions(RabbitEars PRIVATE
  RABBITEARS_THEME_ENGINE)` (the `mac/` idiom). Likely nested within `RABBITEARS_BUILD_GUI` (the engine is
  meaningless without the GUI), but a separate flag lets us build the GUI without the engine mid-migration.
- **New links** in `Win32/CMakeLists.txt` (`:69-72`): `d3d11 dxgi dxguid` always when the engine is on;
  `d3dcompiler` **only** if runtime `D3DCompile` is used. Already present & reusable: `d2d1 dwrite
  windowscodecs gdiplus`.
- **Shaders → precompiled `.cso`, embedded as RCDATA.** The app is `/MT` static-CRT and ships **no** extra
  runtime DLLs; runtime `D3DCompile` would force shipping `d3dcompiler_47.dll` (a new redist to avoid). So:
  offline `fxc.exe` (Windows SDK, Shader Model 4/5 — dxc/SM6 unnecessary) via `add_custom_command` → `.cso`
  → embed in [`packaging/RabbitEars.rc`](../../packaging/RabbitEars.rc) as RCDATA (mirroring `IDR_*_PNG`) →
  load via `FindResource`/`LoadResource` → `LoadPixelShader`/`CreatePixelShader`. Keep runtime `D3DCompile`
  as a **dev-only** fast-iteration fallback behind `_DEBUG`.
- **Rebuild trap:** any RCDATA-embedded `.cso`/asset must be added to the `.rc`'s `OBJECT_DEPENDS` list
  (`Win32/CMakeLists.txt:62-63`) or edits won't trigger a `.res` rebuild.
- **mac** gates the Metal renderer the same way (a `RABBITEARS_THEME_ENGINE` compile-def in
  `mac/CMakeLists.txt`) so the shared model still compiles into `RabbitEarsCore`/self-test when the
  platform renderer is off.

---

## 8. Phased plan

Each phase ships behind the flag, owner-verified at runtime.

**Phase 1 — Foundation spike (de-risk everything on ONE surface).**
Stand up the full `SkinDevice` (D3D11 BGRA + feature-level/WARP fallback, D2D1.1 device, one waitable
flip-model swapchain) on **only the command/transport bar**, at **visual parity** with today's look
(same `panelElevBg`/`windowBg` bands, same DirectWrite title). Prove **one** custom HLSL effect end-to-end
(a subtle animated accent underglow) — exercising `RegisterEffectFromString`, the per-frame constant
buffer, and the waitable-object loop with occlusion/minimize idling. Grid stays on its legacy HwndRT.
*Verify: bar renders identically, one effect animates, GPU idle when covered/minimized, resize/DPI clean.*

**Phase 2 — Skin abstraction.**
Land the `common/` skin model + registry + codecs (§4), the Win32 resolver (§5), the per-skin brush set,
the skin-switch broadcast, and the Settings → Theme UI. Still only Dark (parity) but now *selectable*
infrastructure. *Verify: switching skins (even with one skin) repaints cleanly, persists, no leaks.*

**Phase 3 — Reskin surfaces incrementally.**
Transport + meters first (the mockups), then command-bar chrome, then nav + grid, then dialogs. Migrate
the ~14 fonts onto `themeFont`. Convert `BUTTON` transport children to owner-draw where glow is needed.
Each surface owner-verified.

**Phase 4 — Author the sets.**
Steampunk + Cyberpunk palettes, art, and shaders (heat-haze, neon). macOS renderer mirrors in Metal.

---

## 9. Open questions

Model/boundary (resolve **with the mac team**):
- Home for the shared model: `common/ui/Skin.h` vs `common/skin/`? Color as `SkinColor` struct vs packed
  `uint32` (cheaper serialize, matches hex codec)?
- Codecs: UTF-8 `std::string` in `common/` (recommended, mac-friendly) with Win32 transcode at the
  settings boundary — confirmed OK for the mac side?
- Do per-meter `MeterPalette`s become skin-scoped defaults, or stay user overrides layered on the skin?
  (They only touch the theme via the `windowBg` fallback today.) And should the meter model itself be
  lifted into `common/` in the same pass?
- Does "follow system light/dark" stay a renderer-side meta-option orthogonal to `SkinId`? (Recommended: yes.)

Renderer (Win32-internal):
- Where does the Win32 renderer live — inside the GUI target, or factored so the CLI/self-test never pull
  D3D11 deps? (The CLI must stay graphics-free.)
- Does the interop `dc` replace **all** `HwndRenderTarget` controls at once, or run parallel during Phase 1?
  (Backlog favors one surface first — this doc assumes coexistence.)
- One swapchain per animated control, or a single DirectComposition surface for the whole window? (Affects
  whether each control keeps its own swapchain.)
- Does the shared factory need `MULTI_THREADED` (today `SINGLE_THREADED`, `D2DSupport.h:21`) once one device
  is shared across controls?

---

## Appendix A. Surface inventory

Every skinnable surface and its current render tech (Phase 3 scope). GDI/GDI+ surfaces need an owner-draw
or interop conversion for shader skins; D2D is easiest to extend; OS common-controls need subclass/owner-draw.

| Surface | Where | Tech today | Notes |
|---|---|---|---|
| Command bar (title, +Add Playlist, Settings ▾, min/max/close) | `MainWindow.cpp` `drawCmdBar:298` | GDI → offscreen HBITMAP → BitBlt | **Phase 1 interop target.** Uses `panelElevBg`; coral RoundRect; **hardcoded** close-hover `RGB(196,43,28):344` |
| Transport strip band | `MainWindow.cpp` `WM_PAINT:1542` | raw GDI `FillRect(windowBg)` | **Phase 1 interop target.** Different color from cmd bar |
| Transport buttons (play/pause/stop/record/full) | `createChildren:1336-1355` | classic `BUTTON` + MDL2 glyph + `DarkMode_Explorer` | OS-drawn — needs `NM_CUSTOMDRAW`/owner-draw to glow |
| Volume / buffer trackbars, labels, search edit, nav TreeView | `createChildren` | OS common controls, `SetWindowTheme` only | Can't take HLSL without subclass/owner-draw |
| Caption glyphs (min/max/close) | `drawCaptionGlyph:277` | 1px GDI pen strokes | Trivially reshadeable (owner-draw) |
| Dock gutters | `paintGutters:507` | GDI `FillRect(border)` | Trivial |
| Dock grips | `DockGripProc:626` | GDI `DC_BRUSH` + dot grid | `panelElevBg`/`border`/`textMuted` |
| Drop overlay | `DropOverlayProc:549` | `WS_EX_LAYERED`, `FillRect(accent)`, **alpha 110** | Fixed alpha → move into palette |
| Channel grid | `ChannelGridControl.cpp` | **Direct2D 1.0** HwndRT | Already D2D — easiest to add shaders; migrate to `dc` |
| Buffer meter | `BufferMeter.cpp:466` | GDI DIB back-buffer | Phase 3 |
| Mini-meters ×4 | `MiniMeter.cpp:345` | GDI memory DC + `DC_BRUSH` (+ GDI+ scope/tube) | Phase 3; palette-driven already |
| About / dialogs | `Dialogs.cpp` | GDI + GDI+ (About art) | Phase 3 |
| Video letterbox | `VideoProc:2028` | `BLACK_BRUSH` | **Leave alone** — libVLC renders over it |
| Splash | `Splash.cpp` | layered window + GDI+ | Low priority |

## Appendix B. Grounding

This doc was written from a direct source survey (theme palette, the D2D layer, the meter seam, the chrome
surfaces, the build/boundary) plus a rendering-architecture design pass. File:line anchors reflect the tree
at branch point `c6a0cf2`; verify them before relying on an exact line (the code moves).
