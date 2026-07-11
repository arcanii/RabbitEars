<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# RabbitEars localization (i18n)

This directory is the **source of truth** for every user-facing string. The C++ the app compiles
(`common/core/Strings.{h,cpp}`) is **generated** from these JSON files — never edit the C++ by hand.

## Layout

| File | What it is |
|------|------------|
| `languages.json` | Ordered list of supported languages. Order defines the `Lang` enum; the first entry (`reference`) is the source language (English). Each entry: `code` (filename), `enum` (`Lang::` value), `endonym` (the language's own name, shown in the menu). |
| `keys.json` | Ordered list of `{id, cluster, placeholders, comment}`. Order == the `StringId` enum order. This is the **contract**: every translation file must cover exactly these ids. `cluster` groups strings by UI context; `placeholders` are the `{0}`/`%d`/`%s` tokens that must survive translation. |
| `<code>.json` | `{ StringId: "text" }` for one language — `en.json`, `ja.json`, … English is the reference. |
| `review-<code>.md` | *Generated* side-by-side review sheet (not committed). |

## Regenerating the C++

After editing any JSON, regenerate and commit both the JSON and the C++:

```
python tools/i18n/gen_i18n.py
```

The generator validates before writing and **fails loudly** on: a missing translation, a placeholder
mismatch (a language dropped/added a `{0}`/`%d`/`%s` vs. English), an orphan key, or a bad id. CI runs
`python tools/i18n/gen_i18n.py --check` to catch a committed C++ that drifted from the JSON. The CLI
`--selftest` independently re-checks completeness + placeholder parity at runtime.

## Translating / reviewing (translator confirmation)

1. Generate a side-by-side sheet for the reviewer:
   ```
   python tools/i18n/gen_i18n.py --review ja
   ```
   This writes `review-ja.md` — English and Japanese in one table, grouped by UI context.
2. The native reviewer reads it and edits `ja.json` (or hands back corrections to apply there).
3. Re-run the generator and commit.

**The current Japanese is a machine draft** (glossary-consistent) and needs a native pass before
release — the Terms-of-Use text especially. Keep translations terse for menu items/buttons; preserve
every `{0}`/`%d`/`%s` token exactly; do not translate the brand `RabbitEars`.

## Adding a language (e.g. French)

1. `cp en.json fr.json` and translate the values (keys stay identical).
2. Add to `languages.json`: `{ "code": "fr", "enum": "Fr", "endonym": "Français" }`.
3. Wire the Win32 side (small): add `Lang::Fr` handling in `Win32/ui/Tr.h` (`resolveLang`/`systemLang`)
   and an item + id + dispatch in the Settings ▸ Language submenu (`Win32/ui/MainWindowCommands.cpp`).
4. `python tools/i18n/gen_i18n.py` — it won't build until every key has a French value, so nothing
   ships half-translated.

## Notes

- The catalog is pure `common/` (no Win32), so macOS can adopt the same strings later.
- Two Win32 file-dialog filter strings (embedded `\0` separators) are kept as literals in code, not here.
