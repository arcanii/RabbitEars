# Windows-team review — macOS Phase-2 plan

**Reviews:** [`docs/MACOS_PORT.md`](MACOS_PORT.md) §5 (the 8-item Phase-2 PR to `main`) against
the actual Windows tree, after **v0.1.5 shipped**.
**Verdict:** the plan is **sound and correctly sequenced** (additive spike → refactor-last,
validated both-green). **Green-lit** to proceed as the Mac team's pre-reviewed PR. Six concrete
refinements below — none blocking; two would fail the shared self-test if taken literally.
Reviewed at commit `141c962`.

## Answers to §6 (the Mac team's asks of the Windows team)

- **Timing** — 0.1.5 is out and we're **between releases**; proceed now, aiming to land before
  the next Windows release so the refactor never collides with a version bump.
- **In-flight edits to `M3uParser.cpp` / `Database.cpp`** — **none.** The shared core is frozen
  from the Windows side; merge whenever ready.
- **macOS CI runner** — **done:** `.github/workflows/mac-core.yml` builds the shared core and
  runs the self-test on `macos-latest` via `scripts/build-mac.sh` on every shared-core change
  (first run green in ~29 s). The PR can drop its "CI config" line item.

## Item-by-item (line references are to the current `main`)

**1 — `if(NOT WIN32)` → `if(NOT WIN32 AND NOT APPLE)` + `option(RABBITEARS_BUILD_MAC)`.**
Accurate (`CMakeLists.txt:5`). **Also required, and not in the summary:** `project(RabbitEars
LANGUAGES C CXX RC)` at `CMakeLists.txt:3` declares the **RC** language, which is Windows-only
(no RC compiler on macOS → configure fails), while the mac build needs **OBJCXX**. Make the
languages conditional: base `project(… LANGUAGES C CXX)`, then `enable_language(RC)` under
`WIN32` / `enable_language(OBJCXX)` under `APPLE`. Update the line-6 "Win32-only" `FATAL_ERROR`.

**2 — Hoist `sqlite3` above the platform guard.** Clean; matches `mac/CMakeLists.txt:55`. The
`if(MSVC) … /w` at `CMakeLists.txt:75-77` is already compiler-gated, so it stays clang-safe.

**3 — Split the core link line (`PUBLIC sqlite3` only; Win libs elsewhere).** Accurate
(`CMakeLists.txt:89`, `sqlite3 shell32 ole32 winhttp`). Nuance: `winhttp` leaves with `Http.cpp`
(item 4), but `shell32`/`ole32` are still needed by `Database.cpp`'s Windows `defaultDbPath()`
(`SHGetKnownFolderPath`) until the Paths seam (item 5) lands — so they become an `if(WIN32)`
augmentation of core, not a full removal.

**4 — Move `core/Http.cpp` out of the shared source list.** Correct, and already mirrored on mac
(`RabbitEarsCoreMac` omits it). **Watch:** `RabbitEarsCli` (`--fetch` / `--import`) links core
and calls `httpGet` — keep `Http.cpp` in the Windows link path or the CLI loses networking.

**5 — `src/platform/win|mac` + a `Paths` seam replacing the `Database.cpp` `#ifdef`.** Sound. Two
watch-items: moving `src/platform/{Log,Updater}.cpp` edits the GUI target's source list
(`CMakeLists.txt:129-130`); and the Paths-seam extraction **re-touches shared `Database.cpp`** —
hold it to the same "Windows branch byte-identical, re-verify `/W4` + `--selftest`" bar as the
original guarded edit (`Database.cpp:156` `defaultDbPath()`).

**6 — De-Win32 `DockLayout.h`. Highest ripple; two verified traps.**
- `RECT`→POD is a **public-API** change, not internal: `RECT` appears in `Gutter{rc,nodeRect}`
  and `computeRects(const RECT&, …, RECT rects[])` (`DockLayout.h:46-57`). Call sites
  `src/ui/MainWindow.cpp` (real Win32 `RECT`s + gutter hit-test/paint) and mac
  `mac/src/tools/selftest.cpp` must update. Keep the POD `Rect{long left,top,right,bottom;}` in
  RECT field order so the Win32 boundary is a trivial cast.
- **The number swap breaks the self-test as worded.** `serialize()` uses
  `swprintf_s(r, L"%.3f", ratio)` (`DockLayout.cpp:46`) → `0.220`; `std::to_wstring(double)`
  yields `0.220000`, failing the exact assertion `s == L"|0.220(N,-0.600(V,G))"` on **both**
  platforms. `parse()` uses `_wtof` (`DockLayout.cpp:76`, returns `0.0` on junk); use
  **`std::wcstod`, not `std::stod`** — `std::stod` throws `std::invalid_argument` on
  `garbage(((` and breaks the "malformed → default" contract the self-test asserts. (The mac
  shim already uses `wcstod`.)

**7 — Retire the `Encoding.h` include-shim → a real seam.** Make §7's warning concrete: the
shim's manual `utf8FromWide` (`mac/src/shim/platform/Encoding.h:28`) treats each `wchar_t` as a
full code point — correct on macOS (32-bit, UTF-32) but **wrong on Windows (16-bit, UTF-16)**,
where astral characters (emoji channel names, common in IPTV lists) are surrogate pairs and the
manual loop would emit broken/CESU-8-style bytes. The unified seam must keep
`WideCharToMultiByte`/`MultiByteToWideChar` (`src/platform/Encoding.h`) on Windows and use the
manual UTF-8↔UTF-32 path only on macOS — one header, two platform impls. Do **not** promote the
shim's implementation onto the Windows path.

**8 — Add the mac appcast item.** A separate `mac/packaging/appcast-mac.xml` already exists, so
prefer **two peer feeds** (`appcast.xml` for WinSparkle + `appcast-mac.xml` for Sparkle, one
family Ed25519 key) over a single per-OS feed. Note `scripts/make-appcast.ps1` is PowerShell —
the mac feed wants a peer generator (or a cross-platform rewrite of the generator).

**Bonus (outside the 8):** `mac/CMakeLists.txt:38` hardcodes `APP_VERSION "0.1.4"` (stale — we're
at 0.1.5). The plan's version unification (a single root `APP_VERSION` consumed by both targets)
removes the duplication; fold it into the same PR.

## Merge criteria we'll hold the PR to

Per §5: `/W4`-clean on Windows, `clang`-clean on macOS, `--selftest` green on **both**, zero
Windows behavior change, and Windows-team sign-off. The macOS half is now automated by
`mac-core.yml`; we'll run the Windows `/W4` + `--selftest` pass on the PR branch before sign-off.
