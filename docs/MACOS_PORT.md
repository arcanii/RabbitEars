# RabbitEars — macOS Port Plan

**Status:** Phase‑0 spike **merged to `main`**; Phase‑2 core carve‑out **in PR** (branch `phase-2-core-carve-out`).
**Audience:** the Windows team and the product owner.
**TL;DR:** Add macOS **in this repo** (monorepo). Phase 0 was additive‑only; **Phase 2** (below) makes the
shared core genuinely platform‑neutral — `RabbitEarsCore` links only `sqlite3`, the Win32 bits move to
`src/platform/win/` with macOS peers in `src/platform/mac/`, and the shared headers carry their own
non‑Windows branch so the mac shims retire. 0.1.5 has shipped and the Windows team green‑lit the merge.

> **Phase‑2 verification.** The macOS half is built + self‑tested green locally (stock Xcode `clang`/libc++:
> core, `.app`, and root‑CMake‑on‑mac). The Windows half is validated on the PR by a new
> `.github/workflows/windows-core.yml` job (MSVC builds the carved‑out core + CLI and runs `--selftest`),
> the peer of the existing `mac-core.yml`. Merge on both‑green + Windows‑team sign‑off, per §5.

> **Windows‑team review** ([`docs/MACOS_PORT_REVIEW.md`](MACOS_PORT_REVIEW.md)) resolutions: the two flagged
> self‑test traps were avoided — `serialize()` uses `std::swprintf("%.3f")` (not `to_wstring`) and `parse()`
> uses `std::wcstod` (not `stod`); `Encoding.h` keeps `WideCharToMultiByte` on Windows (surrogate‑correct);
> `RECT` stays a real Win32 `RECT` on Windows (zero UI call‑site churn); RC/OBJCXX languages made
> conditional; and `APP_VERSION` unified into `cmake/AppVersion.cmake` (was stale at 0.1.4 on mac).

---

## 1. Decision: one repo, not a separate repo or submodule

For a shared core this small (~4 genuinely portable logic files, a few thousand LOC) that the Windows team
edits every release, a separate repo is the wrong trade:

- A **submodule** forces an edit‑core → PR‑core → bump‑SHA two‑step for what is a one‑commit change in a
  monorepo, and adds a third repo to tag/CI — disproportionate ceremony at this scale.
- A **separate repo** combined with the 0.1.5 gate would force *temporary duplication of exactly the files
  the Windows team is actively editing* (`M3uParser.cpp`, `Database.cpp`) → guaranteed drift and a painful
  reconciliation.

A monorepo keeps **one source of truth** (portable files compiled *in place*, never copied), lets a core fix
and both platforms' adoption land atomically, and shares the one ready‑made cross‑platform asset: the
**family Ed25519 signing key + appcast/EdDSA scheme**. That scheme is *already* bidirectional —
`scripts/sign-release.sh` signs the **Windows** installer **on macOS** with Sparkle's `sign_update` today
(see `docs/RELEASING.md`). WinSparkle (Windows) and Sparkle (macOS) are the same update mechanism on two
platforms.

> Reserve the separate‑repo/submodule option only if hard team or release‑cadence isolation later becomes a
> firm requirement (independent release trains, separate access control). At today's size that's
> over‑engineering. If it ever happens, extract with `git filter-repo` to preserve history.

## 2. The safety contract for the Windows team

The whole plan is designed so **nothing you do for 0.1.5 is affected**:

1. **The root `CMakeLists.txt` is not touched** until after 0.1.5. The mac build has its own entry point at
   `mac/CMakeLists.txt` (`cmake -S mac -B build-mac`). It reaches *up* into the shared `src/` and
   `third_party/` sources read‑only.
2. **Everything mac‑specific lives under one new top‑level `mac/` directory** you never open.
3. **Two shared files are edited** on the spike, both guarded and **Windows‑behavior‑preserving**:
   - `src/db/Database.cpp` — `defaultDbPath()` body + the two Windows‑only `#include`s / `#pragma`s, all
     behind `#if defined(__APPLE__)` / `#if !defined(__APPLE__)`. The non‑Apple branch is your current code
     verbatim (~21 lines, all inside `#else`/guards; zero behavior change).
   - `src/core/M3uParser.cpp` — one line: `std::ifstream f(path, …)` → `std::ifstream f(std::filesystem::path(path), …)`
     (+1 `#include <filesystem>`). On Windows this opens the same file via the native wide path; it just also
     compiles on clang. This is a small cross‑platform *improvement* to shared code, not a mac‑only fork.
4. The spike **rebases onto `main`** periodically. Because it consumes your `M3uParser.cpp` / `Database.cpp`
   in place, your edits flow into the mac build automatically, and any break shows up **only on the mac
   branch, never in your tree**.

There is no PR to `main` until you've shipped 0.1.5 and we coordinate (Section 5).

## 3. What's actually shareable (the honest reality)

Repo strategy only affects the ~30% that's shared logic. The other ~70% (all the Direct2D/GDI+ UI, WASAPI
capture, `WinMain`, and the HWND‑bound player) is a from‑scratch native reimplementation **regardless of repo
choice** — that's inherent to porting a native Win32 UI, not a consequence of this plan.

| Component | Verdict on macOS | Notes |
|---|---|---|
| `core/M3uParser.cpp` | **Portable** (1‑line edit) | Reaches Windows via `platform/Encoding.h` (satisfied by an include shim); the only non‑portable line was the MSVC `ifstream(wchar_t*)` in `parseM3uFile`, now `filesystem::path` |
| `db/Database.cpp` (SQL) | **Portable** except `defaultDbPath()` | The guarded edit; all SQL / `open()` / models are portable |
| `ui/DockLayout.cpp` | **Portable logic** | Needs `RECT` → POD and `swprintf_s`/`_wtof` → std CRT (2 calls). Confirmed by building it on mac behind thin shims |
| `models/*.h`, `core/Http.h`, `platform/Log.h`, `platform/Updater.h` | **Clean** | No `windows.h`; used as shared contracts |
| `core/Http.cpp` | **Rewrite** (not reuse) | 100% WinHTTP → `mac/src/platform/Http.mm` (NSURLSession) behind the same `httpGet()` signature |
| `ui/VlcPlayer.cpp` | **Rewrite** (not reuse) | HWND / `set_hwnd` / `PostMessageW`‑bound → `VlcPlayerMac.mm` (`libvlc_media_player_set_nsobject` + NSNotification marshaling). Reuse only the libVLC call sequence |
| `platform/Updater.cpp` | **Reimplement** on the shared key | WinSparkle → Sparkle (`mac/src/platform/Updater.mm`), same Ed25519 key |
| `platform/Log.cpp` | **Reimplement** | → `os_log` + `~/Library/Application Support` |
| All Direct2D/GDI+ UI, WASAPI capture, `WinMain` | **From scratch** (Cocoa/Metal) | Zero file reuse; salvage the FFT/simulation math only |

## 4. The three phases (keyed to the 0.1.5 gate)

**Phase 0 — now, no coordination needed** (this branch). Additive `mac/` tree + the one guarded
`Database.cpp` edit. Deliverable: a macOS binary that parses a playlist, persists to
`~/Library/Application Support/RabbitEars`, and passes the ported core self‑test on macOS — proving the
shared core is *genuinely* portable, not aspirationally so. See `mac/README.md` to build it.

**Phase 1 — while 0.1.5 is in flight.** Keep the branch additive, keep rebasing, open **no PR to `main`**.
Build out the native Cocoa/Metal UI and `VlcPlayerMac` (the ~70%, fully unblocked and repo‑strategy‑independent).

**Phase 2 — after 0.1.5 ships + Windows‑team coordination.** One small, pre‑reviewed PR to `main` (Section 5).
Because a working mac app already exists by then, this refactor is *validated by rebuilding both targets
green* — refactor‑last, de‑risked, merged only with your sign‑off and slotted **between** releases so it never
collides with the version bump.

## 5. Exactly what the Phase‑2 PR to `main` will contain

So you can pre‑agree the shape now. All of it keeps every Windows target's link list and behavior identical:

1. `CMakeLists.txt` line 5: `if(NOT WIN32)` → `if(NOT WIN32 AND NOT APPLE)`; add `option(RABBITEARS_BUILD_MAC OFF)`.
2. Hoist the `sqlite3` target above the platform guard so both platforms share it.
3. Split `RabbitEarsCore`'s link line: `PUBLIC sqlite3` only; move `shell32/ole32/winhttp` into a
   Windows‑only platform selection (this is what makes "core is platform‑clean" compiler‑enforced).
4. Move `core/Http.cpp` **out** of the shared core source list (it's pure WinHTTP) into the Windows platform
   selection, with `Http.mm` as its mac peer behind the shared `Http.h`.
5. Move the Win32 platform bodies into `src/platform/win/` and promote `mac/src/platform/` to a peer
   `src/platform/mac/`; replace the `Database.cpp` `#ifdef` with a small `Paths` seam.
6. De‑Win32 `DockLayout.h`: `RECT` → POD `Rect`; `swprintf_s`/`_wtof` → `std::to_wstring`/`std::stod`.
7. Retire the `Encoding.h` include‑shim by introducing a real cross‑platform `Encoding` seam.
8. Add the mac `<item>` channel to the appcast pipeline (keep the single family Ed25519 key).

**Merge criteria:** `/W4`‑clean on Windows, `clang` clean on macOS, `--selftest` green on **both**, zero
Windows behavior change, and Windows‑team sign‑off.

## 6. What we need from you (the Windows team)

- **Nothing before 0.1.5.** Ship it on your normal cadence.
- **After 0.1.5:** a review window for the Phase‑2 PR above, and a heads‑up on any in‑flight refactor to
  `M3uParser.cpp` / `Database.cpp` so we time the merge between releases.
- A **macOS CI runner** (GitHub Actions `macos-latest`) that builds `mac/` and runs the core self‑test on
  every push — this turns any Windows‑side change that breaks the shared core into an immediate signal
  instead of a surprise. (Config lands with the Phase‑2 PR; harmless to Windows.)

## 7. Known risks & mitigations

- **The `Encoding.h` include‑shim is load‑bearing.** If its `utf8FromWide`/`wideFromUtf8` contract drifts
  from the real header, the mac core silently miscompiles. → Keep the signatures byte‑identical; the macOS CI
  self‑test catches drift immediately; retiring the shim is item #7 of the Phase‑2 PR.
- **`wchar_t` is 32‑bit on macOS (UTF‑32), 16‑bit on Windows (UTF‑16).** The shim treats `std::wstring` as the
  platform‑native wide encoding, so all round‑trips (parse → store → read) stay self‑consistent. Any future
  code that assumes 2‑byte `wchar_t` (surrogate math, fixed‑width indexing) must be avoided in shared files.
- **Long‑lived rebase against a fast‑moving `main`.** Mitigated by consuming shared sources read‑only (except
  the one guarded edit) so breaks surface only on the mac branch; rebase in small increments.
- **Effort is dominated by the ~70% native UI**, not the shared core. Repo strategy doesn't change that; the
  UI work starts now in parallel.

---

*Verified on macOS with stock Xcode `clang`/libc++: the shared core self‑test builds and passes (`ctest`),
the ObjC++ platform seams compile, and the `RabbitEars.app` bundle links. After the Phase‑2 carve‑out the
core is platform‑neutral (builds from the root CMake on macOS too) and the mac shims are retired — the
shared headers carry their own non‑Windows branch. See `mac/README.md`.*
