<p align="center">
  <img src="art/RabbitEars_logo.png" alt="RabbitEars" width="360">
</p>

<h1 align="center">RabbitEars</h1>

<p align="center"><i>A simple, fast, native IPTV viewer for Windows and macOS.</i></p>

<p align="center">
  <a href="https://github.com/arcanii/RabbitEars/releases"><img src="art/clockwork_icon3.png" alt="RabbitEars" width="132"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Windows-11-0078D6?style=for-the-badge&logo=windows11&logoColor=white" alt="Windows 11">
  <img src="https://img.shields.io/badge/Arch-x64%20%2B%20ARM64-6E56CF?style=for-the-badge" alt="x64 + ARM64">
  <img src="https://img.shields.io/badge/macOS-26%2B-000000?style=for-the-badge&logo=apple&logoColor=white" alt="macOS 26+">
  <img src="https://img.shields.io/badge/libVLC-3.0-FF8800?style=for-the-badge&logo=vlcmediaplayer&logoColor=white" alt="libVLC">
  <img src="https://img.shields.io/badge/SQLite-3-003B57?style=for-the-badge&logo=sqlite&logoColor=white" alt="SQLite">
  <img src="https://img.shields.io/badge/CMake-Ninja-064F8C?style=for-the-badge&logo=cmake&logoColor=white" alt="CMake + Ninja">
  <img src="https://img.shields.io/badge/Direct2D-Win32-8A2BE2?style=for-the-badge" alt="Direct2D / Win32">
  <img src="https://img.shields.io/badge/Cocoa-AppKit-1575F9?style=for-the-badge&logo=apple&logoColor=white" alt="Cocoa / AppKit">
  <img src="https://img.shields.io/badge/i18n-EN%20%2F%20JA-D97757?style=for-the-badge" alt="English / Japanese">
  <img src="https://img.shields.io/badge/License-GPL--3.0-D97757?style=for-the-badge" alt="License: GPL-3.0">
</p>

---

RabbitEars plays IPTV playlists with a clean, dark, Fluent-styled interface. Paste
an M3U/M3U8 URL (e.g. `https://iptv-org.github.io/iptv/index.m3u`) or open a local
playlist; it parses the channels (name, logo, group, stream URL), lets you
search/filter, star favourites, assign custom channel numbers, browse a built-in
**TV guide (XMLTV EPG)**, watch **several channels at once**, and **record** —
by hand or on a schedule. Everything persists in a local SQLite database.

It's **native on both platforms** — a Win32 + Direct2D app on Windows (x64 **and**
native ARM64) and a Cocoa + AppKit app on macOS — sharing a portable **C++20** core
(the M3U/XMLTV parsers, SQLite storage, the recording scheduler, and the
playlist/channel model) rather than leaning on a cross-platform UI toolkit.
Playback is **libVLC**, storage is **SQLite**, and updates ship in-app over
**WinSparkle / Sparkle**.

## Download

Installers are on the **[Releases](https://github.com/arcanii/RabbitEars/releases)** page:

- **Windows** — `RabbitEars-<ver>-setup.exe` (x64), `-arm64-setup.exe` (native
  ARM64), or `-universal-setup.exe` (bundles both and installs the native arch).
- **macOS** — the notarized universal (`arm64` + `x86_64`) `.dmg`.

Both auto-update in place once installed.

## Features

**On both Windows and macOS**

- **Add playlists** — paste an M3U/M3U8 URL (downloaded in the background) or open
  a local `.m3u`/`.m3u8` file; the playlist's `x-tvg-url` guide URL is picked up too.
- **Auto-parse** — name, logo (`tvg-logo`), group (`group-title`), stream URL,
  `tvg-id`, `tvg-chno`, plus `#EXTVLCOPT` playback hints.
- **Search & filter** — instant search by name; filter by
  All / Favourites / groups / countries.
- **Favourites & numbering** — star channels, assign custom channel numbers (LCN),
  and import/export your favourites as an M3U.
- **TV guide (EPG)** — load an XMLTV guide and browse now/next in a channels×time
  grid, then jump straight to a channel or schedule it.
- **Multi-view** — watch a **2×2 split**, or pop a channel out into a floating,
  resizable **picture-in-picture** window.
- **Recording & DVR** — record a channel to a lossless file, schedule recordings
  from the guide, or set an **EPG series rule** to catch every airing of a show.
- **Playback** — full libVLC transport with volume and fullscreen.
- **Persistence & updates** — everything saved locally in SQLite; in-app
  auto-update (WinSparkle / Sparkle).

**Windows extras** (the Windows app leads the feature set)

- **Channel-logo thumbnails** — fetched off-thread, disk-cached, drawn in the grid.
- **Themes** — four runtime-selectable skins (Dark · Light · Cyberpunk · Steampunk)
  with Direct2D + HLSL GPU effects (neon glow, transport-strip underglow, heat-haze).
- **Wake-to-record** — a Windows scheduled task wakes the PC and records with the
  app closed, with a preflight that warns when the power plan won't allow it.
- **Native ARM64** — a first-class Windows-on-ARM build (owner-measured ~4× faster
  than emulated x64), with per-arch auto-update and a universal installer.
- **Localization** — ships **English + 日本語**, following your system language with
  a Settings toggle.
- **Audio & signal meters** — a WASAPI audio-spectrum analyser plus signal /
  bitrate / frame-rate and realtime buffering meters.

Planned: a native-Japanese translation pass and more languages, transcoding on
record, a background dead-link checker, and closing the macOS feature gap. See
[Win32/BACKLOG.md](Win32/BACKLOG.md).

## Build

One unified CMake build drives a portable core (`common/`) plus the per-platform
apps (`Win32/`, `mac/`).

**Windows** — requires **Visual Studio 2026 Community** (MSVC + bundled CMake/Ninja):

```
scripts\build.cmd                              # engine + CLI (no downloads)
scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON    # + the GUI (provisions libVLC)
scripts\build-arm64.cmd -DRABBITEARS_BUILD_GUI=ON   # native ARM64 GUI
```

**macOS** — requires **Xcode** and **VLC.app** (for libVLC; auto-detected):

```
scripts/build-mac.sh                           # shared core + self-test
scripts/build-mac.sh --app                     # + RabbitEars.app
```

Test / inspect the engine without the GUI (Windows CLI):

```
build\Win32\RabbitEarsCli.exe --selftest       # parser + DB + scheduler self-test
build\Win32\RabbitEarsCli.exe --fetch <url>    # test download + parse
build\Win32\RabbitEarsCli.exe --import <url|file>   # import into the app's DB
build\Win32\RabbitEarsCli.exe --epg <url|file> # test the XMLTV guide pipeline
```

See [Win32/HANDOVER.md](Win32/HANDOVER.md) / [mac/README.md](mac/README.md) and
[docs/architecture.md](docs/architecture.md) for the design and current status.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). Bundles the public-domain SQLite
amalgamation; the GUI dynamically links **libVLC** (LGPL-2.1) — see the About box
for attribution.
