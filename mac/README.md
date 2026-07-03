# RabbitEars — macOS

This directory is the macOS **app** (`mac/src/app`), its **self-test**
(`mac/src/tools`), and the macOS **platform layer** (`mac/platform/*` — the peer
of `Win32/platform/*`). It is a **subdirectory of the unified root build**: run
`cmake -S . -B build-mac` (or `scripts/build-mac.sh`) and the root dispatches to
`common/` + `mac/` on macOS. Full plan and rationale:
[`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).

The shared core (`RabbitEarsCore`, in `common/`) is platform-neutral — one
library built for both OSes, linking only `sqlite3`. The old `mac/src/shim/`
headers are gone: the shared headers (`common/platform/Encoding.h`,
`common/ui/DockLayout.h`) carry their own non-Windows branch.

## Build & test

```sh
# Core library + shared-core self-test (no external deps):
scripts/build-mac.sh
#   → configures build-mac/, builds, runs the self-test (expects ALL PASS)

# Also build the RabbitEars.app (needs libVLC + Sparkle, see below):
scripts/build-mac.sh --app
```

Or directly: `cmake -S . -B build-mac && cmake --build build-mac && ctest --test-dir build-mac --output-on-failure`.

Both platforms are covered in CI: `.github/workflows/mac-core.yml` (macOS) and
`windows-core.yml` (Windows — core self-test + full GUI build) each exercise the
shared core off the same unified root build.

## What builds today

| Target | Deps | Status |
|---|---|---|
| `sqlite3` | none | ✅ shared vendored amalgamation |
| `RabbitEarsCore` (`common/`) | sqlite3 | ✅ shared `M3uParser` + `Database` + `DockLayout` |
| `RabbitEarsPlatformMac` | Foundation | ✅ `Http.mm` (NSURLSession) + `Paths.cpp` (Application Support db path) |
| `RabbitEarsSelfTest` | PlatformMac | ✅ **verified ALL PASS on Apple clang** |
| `RabbitEars.app` | libVLC, Sparkle, Cocoa | ✅ **plays** — rich channel grid (★/#/name/group, search, filter, favourites) + libVLC video + load-by-URL/file; menu bar with Check-for-Updates; **Sparkle auto-update wired** (auto-provisioned + embedded) |
| `RabbitEarsPlayProbe` | libVLC | ✅ headless play smoke test — reached `Playing` on a live HLS stream |

## Running the mac app (playback)

The app is a working MVP: load an M3U by URL or file, pick a channel, it plays.

```sh
scripts/build-mac.sh --app                 # builds RabbitEars.app + the probe
open build-mac/mac/RabbitEars.app          # or run the binary directly
build-mac/mac/RabbitEarsPlayProbe          # headless: verify libVLC plays (exit 0 = Playing)
```

**libVLC runtime** is wired automatically: `Mac.cmake` finds VLC.app (or `-DLIBVLC_MAC_PREFIX`),
the app links `@rpath/libvlc.dylib` with an `rpath` to that lib dir, and `VlcPlayerMac` points
`VLC_PLUGIN_PATH` at the provisioned plugins tree so codecs/demuxers load. Verified locally: the
probe plays Apple's HLS reference stream.

## How the shared code stays cross-platform

The platform split lives **inside the shared headers**, so Windows keeps its
exact types/impls and macOS gets a portable branch — no shadow headers:

- `common/platform/Encoding.h` — `#if defined(_WIN32)` uses `WideCharToMultiByte`
  (unchanged); `#else` a portable UTF-8↔UTF-32 impl for clang.
- `common/ui/DockLayout.h` — `#if defined(_WIN32)` includes `<windows.h>` for
  `RECT`; `#else` defines a layout-compatible POD `RECT`. `DockLayout.cpp` uses
  portable `std::swprintf`/`std::wcstod`.
- `Database::defaultDbPath()` is implemented per-platform in
  `Win32/platform/Paths.cpp` and `mac/platform/Paths.cpp`, so the core links only
  `sqlite3`.

## Provisioning the app's external deps (Phase-1)

- **libVLC**: `-DLIBVLC_MAC_PREFIX=<dir>` where the dir has `include/vlc/vlc.h` +
  `lib/libvlc.dylib` (VLCKit or the libvlc SDK). Without it the app builds but
  `VlcPlayerMac` is a no-op.
- **Sparkle**: `-DSPARKLE_FRAMEWORK=<path/Sparkle.framework>`. Without it the app
  builds but auto-update is a no-op. Uses the **same family Ed25519 key** as the
  Windows WinSparkle build.

## Layout

```
mac/                        # the macOS app (subdir of the unified root build)
  CMakeLists.txt            # cmake -S . -B build-mac dispatches here on APPLE
  cmake/Mac.cmake           # libVLC + Sparkle provisioning (best-effort, non-fatal)
  platform/{Http,Log,Updater}.mm  platform/Paths.cpp   # macOS platform layer
  src/app/{main,AppDelegate,VlcPlayerMac}.{h,mm}        # Cocoa shell + libVLC wrapper
  src/tools/selftest.cpp    # shared-core self-test (portable harness)
  packaging/{Info.plist.in, appcast-mac.xml}

../common/                  # shared engine, both OSes (RabbitEarsCore)
  core/  db/  models/  ui/DockLayout  platform/{Encoding,Log,Updater}.h  version.h.in
../Win32/                   # the Windows app (peer)
  WinMain  ui/  audio/  cli/  platform/{Http,Log,Updater,Paths}.cpp  resource.h
```
