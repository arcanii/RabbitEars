# vlc-tools — vendored VLC helper binaries

## `x64/vlc-cache-gen.exe` (VLC 3.0.23, win64)

Generates libVLC's **`plugins.dat`** plugin cache, eliminating the ~10 s cold-start plugin
scan (libVLC otherwise re-scans all 323 plugin DLLs every launch, and an installed copy under
Program Files can never self-cache — the directory is read-only at runtime).

- **Provenance:** extracted from the official
  [`vlc-3.0.23-win64.zip`](https://download.videolan.org/pub/videolan/vlc/3.0.23/win64/vlc-3.0.23-win64.zip)
  (2026-07-09). Its companion `libvlccore.dll` is **SHA256-identical** to the one in our
  `VideoLAN.LibVLC.Windows 3.0.23.1` NuGet (`D3475B83…BC8E35`), so the tool is ABI-exact for the
  core we ship. Re-verify that hash match whenever the libVLC NuGet version is bumped.
- **License:** GPL-2.0-or-later (VLC), compatible with this repo's GPL-3.0-or-later.
- **How it is used:** `packaging/installer.iss` ships it to `{app}` and runs it against
  `{app}\plugins` **post-install, on native-x64 machines only** — the same approach VLC's own
  installer uses. Install-time generation makes the cache's per-plugin path/mtime/size records
  exactly match the installed files (a pre-generated cache would go stale via zip-extraction
  mtime/timezone skew and just be ignored).
- ⚠️ **Never run it under x64-emulation on ARM** — it silently produces an EMPTY 24-byte cache,
  and libVLC would then load **0 plugins → no playback**. The installer gates on
  `ProcessorArchitecture = paX64`; the ARM64 build ships no cache tool (its native scan is ~3 s).
- CI: `.github/workflows/plugins-cache-verify.yml` proves this exact exe + the NuGet plugin set
  produce a valid, non-empty cache on a native-x64 runner.
