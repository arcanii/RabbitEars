# RabbitEars — macOS (Phase-0 spike)

This directory is the **isolated, additive** macOS build tree. It has its own
CMake entry point and reaches up into the shared `../src` and `../third_party`
sources, compiling them **in place**. The repo-root `CMakeLists.txt` (the
Windows build, owned by the Windows team) is **not** touched. Full plan:
[`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).

## Build & test

```sh
# Core library + shared-core self-test (no external deps):
scripts/build-mac.sh
#   → configures build-mac/, builds, runs the self-test (expects ALL PASS)

# Also build the RabbitEars.app (needs libVLC + Sparkle, see below):
scripts/build-mac.sh --app
```

Or directly:

```sh
cmake -S mac -B build-mac
cmake --build build-mac
ctest --test-dir build-mac --output-on-failure
```

## What builds today

| Target | Deps | Status |
|---|---|---|
| `sqlite3` | none | ✅ shared vendored amalgamation |
| `RabbitEarsCoreMac` | sqlite3 | ✅ shared `M3uParser` + `Database` + `DockLayout`, compiled in place |
| `RabbitEarsPlatformMac` | Foundation | ✅ `Http.mm` (NSURLSession), `Log.mm` (os_log) |
| `RabbitEarsSelfTest` | CoreMac | ✅ **verified ALL PASS on Apple clang** |
| `RabbitEars.app` | libVLC, Sparkle, Cocoa | 🚧 window opens + opens the DB; playback/update/UI are Phase-1 |

## How the shared code compiles unchanged

- `mac/src/shim/platform/Encoding.h` — same `utf8FromWide`/`wideFromUtf8`
  contract as `src/platform/Encoding.h`, portable impl, **no `<windows.h>`**. On
  the include path *ahead* of `../src`, so shared sources resolve to it here and
  to the real header on Windows.
- `mac/src/shim/windows.h` — a minimal shim supplying only `RECT` +
  `swprintf_s`/`_wtof` for `DockLayout`. Retired by the Phase-2 de-Win32 work.
- Two guarded, Windows-behavior-preserving edits to shared files:
  `../src/db/Database.cpp` (`defaultDbPath()` + the Win32 `#include`s, behind
  `#if defined(__APPLE__)`; Windows branch byte-identical) and
  `../src/core/M3uParser.cpp` (one line: `ifstream(wstring)` →
  `ifstream(filesystem::path(...))`, an MSVC-extension fix that also compiles on
  clang).

## Provisioning the app's external deps (Phase-1)

- **libVLC**: `-DLIBVLC_MAC_PREFIX=<dir>` where the dir has
  `include/vlc/vlc.h` + `lib/libvlc.dylib` (VLCKit or the libvlc SDK). Without
  it the app builds but `VlcPlayerMac` is a no-op.
- **Sparkle**: `-DSPARKLE_FRAMEWORK=<path/Sparkle.framework>`. Without it the app
  builds but auto-update is a no-op. Uses the **same family Ed25519 key** as the
  Windows WinSparkle build.

## Layout

```
mac/
  CMakeLists.txt            # branch-local mac build entry (cmake -S mac -B build-mac)
  cmake/Mac.cmake           # libVLC + Sparkle provisioning (best-effort, non-fatal)
  src/
    shim/platform/Encoding.h  # UTF-8<->wide shim (no windows.h)
    shim/windows.h            # minimal RECT + swprintf_s/_wtof shim
    platform/{Http,Log,Updater}.mm   # mac impls of the shared seam headers
    app/{main,AppDelegate,VlcPlayerMac}.{h,mm}   # Cocoa shell + libVLC wrapper
    tools/selftest.cpp        # shared-core self-test (portable harness)
  packaging/{Info.plist.in, appcast-mac.xml}
```
