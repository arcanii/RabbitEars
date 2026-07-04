# RabbitEars — Shared Skin Model (cross-platform contract)

> **This is the one part of the theme engine both the Windows and macOS teams must agree on.** It defines
> the platform-neutral **skin model** that lives in shared **`common/`** — colour/font *values* and their
> serialized form — with **no** `windows.h` / `COLORREF` / `d2d1` / `Metal` / `NSColor` anywhere. Each
> platform's **renderer** (Win32 Direct2D-on-D3D11 + HLSL; macOS Metal / Core Graphics later) converts and
> draws; the renderers are each team's own and are explicitly *not* part of this contract.
>
> - **Home of this doc:** root `docs/` (shared — next to [`MACOS_PORT.md`](MACOS_PORT.md)), so the contract
>   is reviewable without reaching into `Win32/`.
> - **Home of the code:** [`common/ui/Skin.{h,cpp}`](../common/ui/Skin.h) — the FIRST model physically in
>   `common/`. **Status: implemented, covered by 14 CLI `--selftest` assertions (all pass).**
> - **Windows renderer design** (not shared): [`Win32/docs/THEME_ENGINE.md`](../Win32/docs/THEME_ENGINE.md)
>   — §5 (the Win32 resolver over this model) + §6 (the D3D11/HLSL renderer). This doc was extracted from
>   that doc's §4 so the shared boundary stands on its own; `THEME_ENGINE.md` §4 is now a pointer here.

## 1. Platform-neutral colour

Replaces `COLORREF` (which is `0x00BBGGRR` byte-swapped — do not reuse its layout). The `CLR_INVALID`
"follow the theme" sentinel becomes an explicit flag:

```cpp
struct SkinColor {                 // common/ui/Skin.h — no windows.h
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool    inherit = false;       // true == "resolve against the base surface color" (was CLR_INVALID)
};
// Win32 renderer: COLORREF toColorRef(SkinColor);   mac renderer: NSColor* toNSColor(SkinColor);
```

## 2. The `Skin` struct (as built, `common/ui/Skin.h`)

A string `id` (not an enum, so Phase-4 skins add without touching a shared enum), a `SkinPalette` of the
14 *used* roles (the pre-engine 17 `Theme` fields minus the 4 dead `syn*`, plus `dangerHover` — was the
hardcoded close-hover red), and typography:

```cpp
struct SkinFont {                       // resolved to HFONT / NSFont by the renderer
    std::string family = "Segoe UI";
    float       sizePt = 10.5f;
    int         weight = 400;           // CSS-ish 100..900 (400 normal, 600 semibold)
    bool        symbol = false;         // platform icon font (glyph role == "Segoe MDL2 Assets"): a
};                                      //   renderer that can't load `family` substitutes its own
                                        //   (mac: SF Symbols) rather than failing.

struct SkinPalette {
    SkinColor windowBg, panelBg, panelElevBg, altRowBg, hoverBg, border,
              textPrimary, textSecondary, textMuted,
              accent, accentText, selectionBg, selectionText, dangerHover;
};

struct Skin {
    std::string id;        // stable token ("dark") — the persisted selection
    std::string name;      // display name ("Dark")
    bool        dark;      // hint for OS dark-mode chrome (DWM immersive / NSAppearance)
    SkinPalette palette;
    SkinFont    body, title, glyph;
    // Phase 4: an optional GPU-skin manifest (shader ids + params), resolved per-platform.
};
```

**Skins define their own colours (ratified).** Skins do **not** inherit OS window/text colours — the
pre-engine light theme's three `GetSysColor` lookups are dropped for literals. `SkinColor.inherit` means
"resolve against the base *surface* colour" (the old meter `CLR_INVALID`), **not** an OS system colour —
there is intentionally no OS-derived vocabulary. The mac renderer mirrors these literals.

## 3. The registry + active selection (as built)

The registry is shared; the *active-state holder* is renderer-side (platform event models + OS dark/light
detection differ), but both platforms persist the selection under the same shared key:

```cpp
const std::vector<Skin>& builtinSkins();     // immortal; index 0 == dark
const Skin&              skinById(const std::string& id);   // unknown -> dark
std::vector<std::string> builtinSkinIds();
const char*              defaultSkinId();     // "dark"
const char*              skinSettingKey();    // "skin" — shared settings K/V key for the active id
```

The built-in set is currently `dark`, `light`, `cyberpunk`, `steampunk` (the individual
`make<Name>Skin()` factories are renderer-authored; the mac renderer supplies its own colour-equivalent
built-ins under the same ids). "Follow system light/dark" stays a renderer-side meta-option: it picks the
dark-vs-light *skin* by the OS mode; it never sources colours from the OS.

## 4. String codecs (as built, UTF-8)

Same discipline as the meter model's `meter*To/FromString`: `skinColorTo/FromString` (`"inherit"` /
`RRGGBB` / `RRGGBBAA`) and `skinPaletteTo/FromString` (the 14 roles, positional CSV,
exact-arity-or-whole-fallback with per-field graceful fallback). Only the skin **id** persists today, so
the positional palette form is safe to freeze — but **before user-customizable skins persist a palette
(Phase 4), add a version/arity prefix** so a future 15th role doesn't discard every saved skin. Do not
reorder roles: position is identity.

## 5. Boundary rules (the actual agreement)

- `common/` skin code stays **graphics-free** — it compiles into `RabbitEarsCore` and the mac self-test
  with **no** GPU/UI deps.
- Color/font **values** live in the model; **conversion + drawing** (COLORREF/D2D on Win32,
  NSColor/Metal on mac) live in each renderer.
- The **serialized string form is the cross-platform interchange** — both renderers read the identical
  persisted skin. This is the meter seam's one genuinely portable contract, done right.
- Skins define their OWN colours — no OS `GetSysColor` / system-colour inheritance (see §2).
- **Open items for the mac team to weigh in on** are collected in the Windows renderer design doc's open
  questions — [`Win32/docs/THEME_ENGINE.md` §9](../Win32/docs/THEME_ENGINE.md#9-open-questions), the
  "Model/boundary — resolve with the mac team" subsection.

---

*Extracted from [`Win32/docs/THEME_ENGINE.md`](../Win32/docs/THEME_ENGINE.md) §4 (the Windows renderer
design doc) so the shared contract is reviewable on its own. When the contract changes, update **this**
doc; `THEME_ENGINE.md` §4 now points here.*
