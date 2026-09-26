# Photorealistic skins & meters — design notes (DRAFT)

> **Status (2026-09-25):** Phase 0 (the render tool), Phase 1 (the six defects) and the VU instruments
> SHIPPED in 0.2.18 (`3c14828` and before; the "uncommitted" notes in the sections below are from
> 2026-09-23 and out of date). **The owner answered the six decisions** (bottom of this file): meters AND
> skins, all three size routes, new selectable looks, skins drive meters. **Stage A — meter SIZE — is
> COMMITTED (2026-09-26)** (see "Stage A" at the bottom and Win32/HANDOVER.md's 0.2.21-dev item (3)).
> Then stage B (Phase 2: photoreal looks) and stage C (Phase 3: skins as materials).
>
> The owner's ask: *"for our next version, can we improve the UI? Make the skins and meters more
> photorealistic?"*

This file is the record of the research that shaped the plan (five investigations + a completeness
critic, run against HEAD `7b5060c`), the plan itself, and how to use the Phase 0 tool. Read it before
starting any visual work — the constraints section is the part that changes what is possible.

---

## Phase 0 — `RabbitEarsRender` (DONE)

**The problem it solves.** Every visual change in this project has gone to the owner blind: the dev
sandbox cannot launch the GUI, and a single "needs work" has cost a whole release cycle. The tool
renders the real meter and strip code to PNG headlessly, so a change can be looked at — by the
assistant (its file reader displays PNGs) and by the owner as one contact sheet — before it ships.

**What it renders** (per built-in skin; theme-engine builds have 4, classic builds 2):
- `tray_<skin>_<96|150>dpi_glass<00|60>.png` — every look (LED, Tube, LCD, Scope, VU) × every kind
  (Spectrum, Signal, Bitrate, Frames) at the **real tray size**, plus the buffer tank healthy and
  troubled; nearest-neighbour zoomed ×4 / ×3 so each device pixel is visible.
- `preview_<skin>_glass<00|60>.png` — the Settings ▸ Meters preview size (150×86, tank 170×76), ×2.
- `strip_<skin>_<96|150>dpi_{default|vu_glass60}[_warp][_trayzoom3].png` (theme-engine builds only)
  — the real GPU `paintSkinStrip()` at 1:1 with the tray composited at the `layout()` positions, plus a
  ×3 crop of the tray. `default` means the LED look with glass off — **not** the default meter set: every
  strip composite shows all four meters, while a default install shows only Spectrum + Signal (at
  different x positions). `_warp` is added when no hardware device was available.

**How it works.** `Win32/render/RabbitEarsRender.cpp`. Each meter is created on a hidden (never
shown) window, fed a synthetic signal (the mini-meters get the Settings dialog's own preview feed; the
tank gets a steady healthy or a troubled health/flow sequence), ticked through its **real**
WndProc (`WM_TIMER`) and painted through it (`WM_PAINT`); the frame is read back with two READ-ONLY
accessors, `miniMeterSnapshot()` / `bufferMeterSnapshot()`, which copy the back-buffer the real paint
BitBlts 1:1 to the screen. The strip's animation clock is pinned with `skin::setStripClockForTest()`
so every run is byte-identical. Those three functions are the only production-code seams; the app
never calls them.

**Run it** (after a normal `scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON` build — it is built
alongside the app):
```
build\Win32\RabbitEarsRender.exe <outdir> [--skin ID]... [--strip-only | --no-strip] [--time MS]
```
~10 s for the full set of 56 PNGs; exit 0 = all written, 1 = a render or setup step failed, 2 = an
unrecognised or malformed argument (`--help` exits 0). Renders are byte-reproducible on one machine;
across machines the classic Light theme (OS system colours) and the ClearType readout may differ.

**Verified** (2026-09-23): the 56 PNGs are **byte-identical** to the scratch proof of concept's, whose
24 meter sheets had in turn been shown pixel-identical to the compiled objects `RabbitEars.exe` was
linked from (the dev build directory, not a release) — so the tool draws what the app draws, and adding
the accessors changed nothing. Two runs are byte-identical
to each other. A classic (theme engine OFF) build renders its 12 PNGs; its dark-theme sheets are
byte-identical to the engine's `dark` skin (the documented "pixel-identical to the pre-engine look").
The proof of concept also rendered the strip on the WARP software device (by forcing it in a copy of
`SkinDevice.cpp`) within 1 LSB of the RTX 4090; the landed tool has no switch for that and uses WARP
only when no hardware device can be created (SkinDevice's own fallback).

**What it cannot show — keep this in mind when judging a sheet:**
- **The owner's own settings.** Looks, palettes, glass strength, VU lamp and fluid colour are
  defaults; the user's database is never read. What the owner actually sees is unknown until they send
  a screenshot (none has been compared with a render yet).
- **Live data.** The feed is synthetic (the Settings preview's).
- **The strip's child controls** — buttons, sliders, status text are separate windows the strip is
  drawn behind; the composite shows only the strip and the meters.
- **Taste.** It shows pixels; only the owner's verdict decides whether they read as "real".
- **CI.** Not run anywhere automatically: the `gui-build` job dies at configure (no fxc outside a
  Developer prompt while the theme engine defaults ON).

**How to use it for a visual change:** render before, make the change, render after, look at both
(zoomed sheets for the tray, preview for detail), and hand the owner the before/after pair — not the
live app.

---

## The constraints that shape everything (verified in code during research)

- **Size is the dominant limit.** Tray meters are `dp(30)` tall (MainWindowChrome.cpp `layout()`),
  minus a `dpx(2)` chrome inset → **a 26 px dial at 100 %** (52 px at 200 %). Widths: Spectrum 112,
  Signal 58, Bitrate 96, Frames 72, tank 115 (96-dpi design values). LED cells are only a few pixels
  across (the tank's grid is a 3-px pitch at 115×30). The tray **cannot be resized** — the dock layout
  places only nav/video/grid. At 26 px only 1-px steps, colour and motion survive; most recorded visual
  failures ("too blurry", "two bands", the double needle) trace back to size. Grain, screws, numerals
  and domed lenses need ≥ ~40 px.
- **The Settings preview is not the tray.** Preview dials are 3.2× taller, fed 24 spectrum bands vs
  the tray's 16, and the VU arc (sized from height alone) spans nearly the whole width of a preview dial
  but under half of the wider tray dials. Judging a look in Settings judges a different composition.
- **Skins are palettes only**: 14 colours, 3 fonts, 3 GPU strengths (`common/ui/Skin.h`) — no
  materials, textures or light. The mini-meters take only their panel background and 1-px border from
  the skin (the tank also takes the accent, text colour and a font), so apart from those every skin's
  meters look the same.
- **The strip is mostly covered.** The status control fills the strip's free middle with an opaque
  panel colour, so a material on the strip shows only in its top and bottom ~10 dp bands unless the
  sliders and status text become owner-drawn.
- **Defaults hide the work.** A default install shows flat LED Spectrum + Signal meters, glass off.
  VU, Tube and glass are all opt-in — most users have never seen them.
- **Real frame rates:** a visible-window probe measured `SetTimer(33)` at ~46.7 ms (~21 Hz, the
  meters) and `SetTimer(16)` at ~29.9 ms (~33 Hz, the strip) on the dev machine — not the 30/60 the
  code comments assume. Design any animation for the real rate.
- **All blending is gamma-space sRGB** (no linear light anywhere; GDI+ never sets gamma-correct
  compositing) — a likely contributor to the muddy, brownish edges on the scope bloom (not proven).
- **Lighting today:** the glass bezel implies a key light from **above-left** (so does the tank text
  shadow); the VU is lit from **below-left** by its internal bulb and its needle shadow follows the
  bulb. LED/LCD/Tube/Scope have no lighting at all. Never written down as a rule.
- **mac.** The mac team declares the theme engine *N/A by design* (`cmake/AppVersion.cmake`), so skin
  work is Windows-only. Meter work can live entirely in the Win32 renderer: the "mac parses
  MeterTuning with exact arity" worry is **not live** — Win32 stores tuning under `meter_<kind>_knobs`,
  mac under `_tuning`, in a different database. The live risk is any `common/` change, which must still
  compile on Apple clang and cannot be compiled here. A Win32-only meter overhaul does reopen a visible
  gap right after 0.2.17 closed it — an owner call.

## Defects the renders exposed (Phase 1 candidates — bugs, not taste)

1. **VU "red zone" is drawn near-white** — it uses `palette.peak`, default `RGB(236,236,240)`.
2. **VU needle shadow reads as a second needle** at preview size — its widths scale with dial height
   and were tuned for a 26 px dial.
3. **The tank's Mb/s readout covers about half the tank** at tray size (11 px semibold).
4. **Light skin: unlit LED cells are near-black** (`off` default `RGB(38,40,44)` on a white panel) —
   louder than the lit cells.
5. **Light skin: the strip underglow is invisible** — added onto white, then clamped (`underglow.hlsl`).
6. **Tube and Scope glows are not clipped** — they bleed over the themed 1-px border.

## Phase 1 — the six defects FIXED (2026-09-23, uncommitted, owner's eye pending)

All Win32-only (`Win32/ui/MiniMeter.{h,cpp}`, `BufferMeter.cpp`, `Dialogs.cpp`, the strip shader
`Win32/ui/skin/shaders/underglow.hlsl`); **nothing under `common/`, so mac is untouched.** Rendered
before and after with `RabbitEarsRender` and compared pixel by pixel, per meter — the "unchanged"
claims below are measured, not assumed. (The tool's "150dpi" sheets are 156 % scaling, not 150 %:
a 41-px dial where a real 150 % tray has 39.)

| # | fix | what changes for an existing user |
|---|---|---|
| 1 | VU red zone drawn with the palette's **`high`** (default red) instead of `peak` (near-white) | every VU user; 21 px per meter at 100 % scaling. Someone who had recoloured the red zone through Peak (it was documented that way from 0.2.14) now gets High — no migration |
| 2 | VU needle shadow: above a 26-px dial its angle and widths grow by **√(h/26)** and its alphas are divided by it, instead of everything scaling linearly | **byte-identical at 100 % scaling**; fainter and closer at 125 %+ (32 px and up) and in the Settings preview |
| 3 | Tank readout font sized from the tank's height — `clamp(round(0.3·h96), 9, 11)`: **9 px in the tray at every scaling, 11 in Settings** (Settings byte-identical); drop shadow black under light text, **white under dark text** | every user with the tank on (default on). It still overlaps the water's surface row slightly (43 % of the width, was 51 %); on Light the smear is gone |
| 4 | On a **light panel** the STOCK `off` and the MARKER use of the stock `peak` are re-derived (`meterDrawnPalette`): `off` = the tank's own unlit dot (231,231,233 on Light — identical to the tank's), peak caps + the Scope trace = (39,39,39). The Tube core and the Bitrate ramp keep the raw near-white peak — "the hottest light" brightens on any panel. User-picked colours are never touched; the Meters dialog shows the resolved Dim/Peak and never saves them back | the Light skin, or any skin where a meter was given a light Bg. **On a dark panel LED/LCD/Tube are byte-identical** |
| 5 | Underglow: when a channel exceeds 1.0 the pixel is scaled by its brightest channel (exposure) instead of clipped | Light skin: a faint warm wash (bottom row ≈ RGB 255,243,239) where there was pure white. **Dark/Cyberpunk/Steampunk strips byte-identical outside the meters** (their worst-case channel is 0.42, so the new branch never runs) |
| 6 | Tube halo and Scope bloom clipped (GDI+ `SetClip`) to the dial, as the VU already was; a Scope trace at zero is lifted to the lowest row where it fits whole | Tube: pixels in the 2-px chrome band only. Scope: the chrome band, plus the trace's floor segments (it used to sit half below the dial) |

**Why #4 had to touch `peak` at all:** fixing `off` alone made the Spectrum's near-white peak caps
vanish on Light — they had only ever been visible because near-black unlit cells framed them. **Why
only the marker use:** resolving every use made the Light Tube's cores dark and ran the Bitrate ramp
backwards (toward brown); adversarial review caught it and it was split.

**Reviewed adversarially** — two lenses (code; every behavioural claim against the pixels). Code
found 1 medium + 3 low, all addressed: the colour picker seeded with the RESOLVED colour saved it on
an unchanged OK, pinning a Light-skin Dim/Peak that then breaks on a dark skin (fixed: an unchanged
pick is a no-op — which also stops Bg from being pinned to the theme's window colour); an idle Scope
trace half-clipped by the new clip (fixed: floor lift); an overstated "every dark skin" (reworded:
dark PANEL); the red-zone re-role (documented above). Claims found the peak "mirror" arithmetic wrong,
three loose comment numbers, and the dark Tube cores / brown Bitrate ramp — all fixed. Final state:
both theme flags build clean, `--selftest` ALL PASS, the classic build's dark sheets byte-identical to
the engine's, two consecutive renders byte-identical.

**Taste calls this leaves for the owner (the renders cannot decide them):**
- **Tube on Light** keeps its pale glow but its peak caps are grey smudges (a dark marker under a
  pale core). A glow cannot really read on white.
- **The Light underglow is faint by design** — the Light skin's `stripGlow` is 0.35 ("a neon underglow
  reads wrong on a light surface"). Stronger means changing that value in `common/ui/Skin.cpp`, which
  mac compiles.
- **The readout at 9 px** is still semibold and still grazes the water's surface row; normal weight
  would recede further, at some legibility.
- The stock green and amber are ~2:1 contrast on white — legible, but a Phase 2 question.

**Found while checking, NOT fixed (pre-existing, tiny):** at 144+ dpi the Signal meter's tallest bar
has a top cell that pokes 2 px into the chrome gutter (`paintSignal` walks down from `B` in whole cell
pitches). Invisible with glass on (the bezel covers the gutter).

**Owner check:** the before/after sheet handed over with this change (six labelled pairs; not in the
repo — rerun `RabbitEarsRender` on both trees to reproduce it); then, in the app, the tray on your own
skin, scaling and settings — especially **VU** (red zone, and the shadow if you run above 100 %) and
the **tank readout**, which change for every user — and switch to **Light** once. In Settings ▸
Meters on Light, open the Dim swatch and press OK without changing it, then switch to Dark: the unlit
cells must still be dark.

## The VU instruments — built to the owner's two reference photos (2026-09-23, uncommitted, owner: "amazing")

The owner sent two photos and asked to *"match this realistic VU meter"* and *"add a Marantz VU meter
like the second"*: a modern backlit desktop VU meter, and a 1970s silver cassette-deck meter. Both
now exist, in a new renderer **`Win32/ui/VuDial.{h,cpp}`**:

- **Vu ("VU needle") is REPLACED** by the backlit instrument: black brushed faceplate, recessed cream
  card, an amber bulb glowing up round a brass shroud at the bottom centre, a -20..+5 scale whose red
  zone thickens toward +5, "VU", a PEAK lamp. The pre-0.2.18 full-width Phase-Linear-style dial (and
  its Phase 1 shadow fix) is gone; `common/ui/VuLamp` is now used only for `vuLampIsUnset`.
- **VuSilver ("Silver VU") is NEW** — the sixth look, token `vu_silver`: brushed-silver faceplate, a
  deep window with a chrome inner wall, an evenly lit ivory card, blue scale to -1, red 0..+6 with
  its ticks hanging below the arc, a 0-100 % scale. **No maker's name or logo on the dial** — a
  trademark in a GPL app is not ours to draw; the look is named generically.

**How they are built** (read VuDial.h first):
- **Measured, not guessed.** Colours are medians sampled off the photos; the arcs are fitted to three
  points each (the Backlit pivot turned out to sit ~0.6 card-heights BELOW the window — the brass boss
  is the lamp's shroud); the dB marks sit where the photos print them (a textbook voltage-linear scale
  crowded -20..-3 into the left third and looked wrong beside them). To compare
  them again: crop the photo to the card, render the preview, put them side by side.
- **Real proportions.** The instrument keeps its own aspect (1.62:1 / 2.45:1), centred, with
  faceplate either side on the wide tray meters — a card stretched to 4:1 stops reading as a VU.
- **Detail by size.** Numerals and legends draw only where they would be >= 5 px (VU >= 7 px, the
  % row >= 7 px): the Settings preview shows everything; the tray (26-39 px dials) shows the light,
  arc, red zone, ticks, needle and PEAK lamp, and no numerals.
- **Cached.** Everything but the needle is rendered once per (face, size, DPI, lamp, red) and
  blitted each frame — cheaper per frame than the old look.
- **What the needle reads, per kind.** Only SOUND reads in dB: the Spectrum kind's level is already
  logarithmic (SpectrumTap sends (dBFS+72)/60), so it maps linearly onto the printed scale (full input
  = the top mark, 30 dB of input below it). Signal / Bitrate / Frames are not dB quantities: they
  keep the old look's linear law, red zone = the top fifth. **The PEAK lamp lights whenever the
  needle is in the red zone, on every kind** (owner's call, 2026-09-23), fading over ~1/4 s after.
- **The palette** still drives what it did: Bg = the lamp's hue (black = the stock bulb), High = the
  red zone, Accent = the needle — the STOCK accent now means the face's own black needle, and the
  Meters dialog's swatch shows that.

**Verified:** both theme flags build clean; `--selftest` ALL PASS; `gen_i18n --check` (589 keys — new
`MeterLookVuSilver`, appended); renders byte-reproducible; **every other look (LED/Tube/LCD/Scope)
and the tank are pixel-identical** to before across all 4 skins x 2 DPIs x glass 0/60 + previews; the
classic build's dark sheets still equal the engine's. **Adversarially reviewed** (1 pass): 2 medium —
the non-audio kinds read through the dB law, so PEAK and the red zone sat lit for most streams (a
60 fps channel, any healthy HD stream); and a saved "nan" tuning could crash any VU meter every paint
(`meterTuningFromString` now rejects non-finite values, and the needle is NaN-proof) — plus the Accent
swatch not matching the needle, the Silver shadow reading as a second bar at preview size, legend
collisions and comment slips. All fixed and re-verified.

✅ **OWNER-SEEN LIVE (2026-09-23, dev profile, 150 % scaling): *"the VU meters look amazing"*.**

**Not verified / open:** no selftest covers VuDial's scale math (it is a GUI translation unit the CLI
does not link — split the pure tables out if it grows); `VuLamp`'s mask/colour code is now dead in
the app but still selftested and still in `common/` (mac compiles it; flagged in BACKLOG rather than
deleted). **The owner's live look was the real check** (above) — their Frame-rate VU has a cyan lamp,
which the Backlit face honours as a cyan-lit card.

**The size question is now concrete:** the tray dial is 26 px tall at 100 % and 39 px at 150 %. The
numerals that make the reference photos read as instruments need ~50 px. Making the meter tray taller
(decision 2 below) is what would put them in the tray.

## History — what must not be repeated

- **Broad gradients across a meter face read as blur.** The one recorded owner verdict of "too blurry"
  (`d3ce334`) came from that; the framed-pane glass model that replaced it keeps `add` exactly 0 over
  the dial. Do not reintroduce face gradients, refraction (sub-cell at this size) or an animated
  highlight (fights the tank's own drifting specular; there is no shared frame clock).
- **"Needs work" does not localise the fault.** The glass bezel's reservation cleared when the *tank*
  was framed too — the eye was reading the odd one out, not a defect in the bezel.
- **A test can pin the flattering pixel** — the VU lamp's first assertions covered only the hotspot
  while the scale band the user reads had gone 16 % darker.
- The owner's reference material (the **Phase Linear 400** photo, a second reference instrument, the
  Steampunk/Cyberpunk mockups) is only *described* — in commit messages, code comments and
  `THEME_ENGINE.md` — **none of the images is in the repo**.

---

## The proposal

**Headline:** at 26 px, realism comes from light, colour and crisp 1-px detail — not texture. The
biggest single lever is making the meters bigger, which only the owner can decide.

- **Phase 0 — render tool.** ✅ DONE (above).
- **Phase 1 — fix what is already wrong** (the six defects above). ✅ DONE in the working tree (see
  "Phase 1" above) — items 1, 3 and 4 change the look for existing users, so the owner verifies.
- **Phase 2 — physical light and colour** (the core of realism at 26 px):
  gamma-correct glows (GDI+ `CompositingQualityGammaCorrected`, alphas retuned); LED lens cues (dark
  tinted "off" lenses, 1-px highlight + bright core on lit cells); **one written light rig** (key light
  above-left, VU bulb as the second light); VU composition (arc sized from face *width*, pre-rendered
  static face, clean anti-aliased needle, numerals only at ≥ 40 px); Scope as a CRT (phosphor green,
  afterglow, brighter where the trace moves slowly, clipped).
- **Phase 3 — skins as materials:** skin-aware meter styling (today a Steampunk meter differs from a
  Dark one only in panel background and border), per-skin bezel tint (touches `common/` — mac compiles it), procedural HLSL strip materials
  (brushed aluminium / brass + rivets / black glass) — only worth it with owner-drawn controls.
- **Phase 4 — bigger meters** (owner decision): a taller strip, a resizable meter bridge, or a pop-out.
  This is what unlocks photographic detail.

**Procedural throughout, no textures:** exact at every DPI, no licensing questions (the app is
GPL-3.0; the `art/` PNGs' licences are unknown), no installer growth (the universal installer would
carry every texture twice).

**Recommended for the next version:** Phase 1 (+ gamma-correct glows if there is room). Phases 2–4
are a 0.3.x epic.

## Decisions only the owner can make

1. **Scope:** meters only, or skins/chrome too?
2. **Size:** may the meters get bigger? (Matters more than anything else here.)
3. **Default or opt-in:** replace the default look for every existing user, or ship new selectable looks?
4. **Lighting:** one above-left key light for every skin, or a rig per skin?
5. **Skins driving meters:** should Steampunk meters look brass, Cyberpunk neon, and so on?
6. **mac:** is a Win32-only meter overhaul acceptable?

### ✅ The owner's answers (2026-09-25)

1. **Scope: meters AND skins** — the skinned chrome and strip as materials too (Phase 3 is in).
2. **Size: all three** — a taller meter tray, a pop-out meter bridge, AND a user-resizable tray (Phase 4
   is in, in all three forms).
3. **New selectable looks** — the existing looks stay byte-identical; photoreal ones are added beside
   them (render before/after must show the old looks unchanged).
4. **Lighting — not asked; assumed:** one above-left key light for every skin (the VU bulb as the second
   light). Flag it if a skin looks wrong under it.
5. **Skins drive meters: yes** — a meter's materials and bezel follow the active skin.
6. **mac — not asked; assumed acceptable:** meter looks and skins are Win32-only already.

## Stage A — meter SIZE (committed 2026-09-26)

The three size routes as ONE mechanism plus a window:
- **One meter height** (`meter_height`, dp; 30 = today's tray, which must stay byte-identical): at 30 the
  meters sit inline in the 50-dp strip; taller, they get a row of their own above the transport row (strip
  = 10 dp + the meters + 50 dp, at most half the video panel, the tank fitting the row — `Win32/ui/MeterTray.h`);
  every meter's width and the tank's scale by height/30 — the Standard / Large / Extra-large presets are
  30 / 50 / 72 dp (Settings ▸ Meters ▸ …), and
  **dragging the strip's top edge** sets any height between (the strip's top ~4 dp are main-window client
  area — the skin strip paints into the main window's DC, so no child window is in the way).
- **The meter bridge** (`Win32/ui/MeterBridge`): a separate resizable window with the tray's meters in
  one row, as large as the window; its meters are MIRRORS of the tray's (`miniMeterSetMirror` /
  `bufferMeterSetMirror` — data, resets, look, palette, tuning; linking seeds the twin with the tank's fill
  and the Bitrate history so far), so nothing that feeds meters changes.
- **What the size buys, today, with no paint change** (`RabbitEarsRender --meter-height 72`): at 72 dp
  (113 px at 150 %) the VU dials show their numerals, PEAK and legends; at 50 dp and 100 % they are clean
  but under the numeral threshold. The LED/LCD looks keep a fixed small cell pitch and read as a fine
  mesh when tall — **stage B's first job**: cells (and lens detail) that scale with the meter, as a NEW
  selectable look (the owner's rule: existing looks stay byte-identical).
- **Built 2026-09-26** (Win32/ui/MeterTray.h, the strip-edge drag, Settings ▸ Meters, the bridge compiled
  and wired) — the standard height renders byte-identical. **The width limit** (found building it): inline,
  a taller tray runs out of width beside the transport controls, so at Large / Extra large most meters
  hide on a 1920-px screen at 150 % — **the owner chose (2026-09-26) a row of their own above the transport
  row at Large+** (built: `MeterTray.h`; the standard height stays inline, byte-identical), and to keep the
  bridge an owned window. Paint cost per frame, measured (`RabbitEarsRender
  --bench-paint`): the fixed-pitch Tube look is the costly one — a Tube-look Bitrate meter at 120 dp,
  its history full, ~10 ms a frame at 100 % and 150 % scaling, ~15 ms at 115 % (Frames Tube ~5–5.5 ms,
  Spectrum / Signal Tube ≤ ~2 ms) — stage B's scaled cells address it; until then the bridge's meters stop
  at 120 dp.
- **The Bitrate history** (fixed right after stage A's commit): its cell looks draw one column per sample,
  and the ring held 64 — tall (so wide) dials kept an empty band on the left (20 px at 72 dp, 250 px at
  120 dp, at 150 %). Now `kBitrateHistory` = 256, beside `kMeterHeightMax` in MeterTray.h, and a selftest
  checks every scaling (147 columns at worst, at 120 dp); Standard / Large and the Scope trace are
  byte-identical. The render tool fills a Bitrate history first at non-standard heights, and the bench
  always — BOTH changed what their numbers mean for Bitrate (a full dial): compare like with like.
- Status and the exact TODO: Win32/HANDOVER.md, 0.2.21-dev item (3).

Plus two things that would help: the **reference photos/mockups** (not in the repo), and more **real
screenshots** of the running app, to check the renders against the owner's actual screen and settings —
the first one came 2026-09-26 (the owner's Large tray, three Backlit VUs + a Silver VU + the tank; not in
the repo).
