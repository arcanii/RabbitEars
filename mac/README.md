# RabbitEars — macOS

This directory is the macOS **app + build entry** (`mac/CMakeLists.txt`, run via
`cmake -S mac -B build-mac`). It reaches up into the shared `../src` and
`../third_party` sources and compiles them **in place** — including the macOS
platform layer at `../src/platform/mac/`, the peer of `../src/platform/win/`.
Full plan and rationale: [`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).

After the Phase-2 core carve-out, the shared core (`RabbitEarsCore` /
`RabbitEarsCoreMac`) is genuinely platform-neutral — it builds from **either**
build system on both OSes and links only `sqlite3`. The old `mac/src/shim/`
headers are gone: the shared headers now carry their own non-Windows branch.

## Build & test

```sh
# Core library + shared-core self-test (no external deps):
scripts/build-mac.sh
#   → configures build-mac/, builds, runs the self-test (expects ALL PASS)

# Also build the RabbitEars.app (needs libVLC + Sparkle, see below):
scripts/build-mac.sh --app
```

Or directly: `cmake -S mac -B build-mac && cmake --build build-mac && ctest --test-dir build-mac --output-on-failure`.

Both platforms are covered in CI: `.github/workflows/mac-core.yml` (macOS) and
`windows-core.yml` (Windows) each build the carved-out core + run the self-test.

## What builds today

| Target | Deps | Status |
|---|---|---|
| `sqlite3` | none | ✅ shared vendored amalgamation |
| `RabbitEarsCoreMac` | sqlite3 | ✅ shared `M3uParser` + `Database` + `DockLayout` + `platform/mac/Paths.cpp`, compiled in place |
| `RabbitEarsPlatformMac` | Foundation | ✅ `Http.mm` (NSURLSession), `Log.mm` (os_log) |
| `RabbitEarsSelfTest` | CoreMac | ✅ **verified ALL PASS on Apple clang** |
| `RabbitEars.app` | libVLC, Sparkle, Cocoa | 🚧 window opens + opens the DB; playback/update/UI are Phase-1 |

## How the shared code stays cross-platform

The platform split lives **inside the shared headers**, so Windows keeps its
exact types/impls and macOS gets a portable branch — no shadow headers:

- `../src/platform/Encoding.h` — `#if defined(_WIN32)` uses `WideCharToMultiByte`
  (unchanged); `#else` a portable UTF-8↔UTF-32 impl for clang.
- `../src/ui/DockLayout.h` — `#if defined(_WIN32)` includes `<windows.h>` for
  `RECT`; `#else` defines a layout-compatible POD `RECT`. `DockLayout.cpp` uses
  portable `std::swprintf`/`std::wcstod`.
- `Database::defaultDbPath()` is implemented per-platform in
  `../src/platform/win/Paths.cpp` and `../src/platform/mac/Paths.cpp`, so the core
  links only `sqlite3`.

## Provisioning the app's external deps (Phase-1)

- **libVLC**: `-DLIBVLC_MAC_PREFIX=<dir>` where the dir has `include/vlc/vlc.h` +
  `lib/libvlc.dylib` (VLCKit or the libvlc SDK). Without it the app builds but
  `VlcPlayerMac` is a no-op.
- **Sparkle**: `-DSPARKLE_FRAMEWORK=<path/Sparkle.framework>`. Without it the app
  builds but auto-update is a no-op. Uses the **same family Ed25519 key** as the
  Windows WinSparkle build.

## Layout

```
mac/
  CMakeLists.txt            # mac build entry (cmake -S mac -B build-mac)
  cmake/Mac.cmake           # libVLC + Sparkle provisioning (best-effort, non-fatal)
  src/
    app/{main,AppDelegate,VlcPlayerMac}.{h,mm}   # Cocoa shell + libVLC wrapper
    tools/selftest.cpp        # shared-core self-test (portable harness)
  packaging/{Info.plist.in, appcast-mac.xml}

../src/platform/             # shared platform layer (peers)
  Encoding.h  Log.h  Updater.h   # shared seam headers (dual-platform)
  win/{Http,Log,Updater,Paths}.cpp
  mac/{Http,Log,Updater}.mm  mac/Paths.cpp
```
