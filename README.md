<p align="center">
  <img src="art/RabbitEars_logo.png" alt="RabbitEars" width="360">
</p>

<h1 align="center">RabbitEars</h1>

<p align="center"><i>A simple, fast, native IPTV viewer for Windows.</i></p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Windows-11-0078D6?style=for-the-badge&logo=windows11&logoColor=white" alt="Windows 11">
  <img src="https://img.shields.io/badge/libVLC-3.0-FF8800?style=for-the-badge&logo=vlcmediaplayer&logoColor=white" alt="libVLC">
  <img src="https://img.shields.io/badge/SQLite-3-003B57?style=for-the-badge&logo=sqlite&logoColor=white" alt="SQLite">
  <img src="https://img.shields.io/badge/CMake-Ninja-064F8C?style=for-the-badge&logo=cmake&logoColor=white" alt="CMake + Ninja">
  <img src="https://img.shields.io/badge/Direct2D-Win32-8A2BE2?style=for-the-badge" alt="Direct2D / Win32">
  <img src="https://img.shields.io/badge/License-GPL--3.0-D97757?style=for-the-badge" alt="License: GPL-3.0">
</p>

---

RabbitEars plays IPTV playlists with a clean, dark, Fluent-styled interface. Paste
an M3U/M3U8 URL (e.g. `https://iptv-org.github.io/iptv/index.m3u`) or open a local
playlist; it parses the channels (name, logo, group, stream URL), lets you
search/filter, star favourites, assign custom channel numbers, and play with full
libVLC transport. Everything persists in a local SQLite database.

It is a native **Win32 / C++20** app — custom-drawn chrome and a Direct2D channel
grid — built to match its sibling apps rather than a cross-platform UI toolkit.

## Features

- **Add playlists** — paste an M3U/M3U8 URL (downloaded in the background) or open
  a local `.m3u`/`.m3u8` file.
- **Auto-parse** — name, logo (`tvg-logo`), group (`group-title`), stream URL,
  `tvg-id`, `tvg-chno`, plus `#EXTVLCOPT` playback hints.
- **Search & filter** — instant search by name; a nav sidebar for
  All / Favourites / groups / playlists.
- **Favourites & numbering** — star channels; assign custom channel numbers (LCN).
- **Playback** — full libVLC playback with volume, fullscreen, and a **realtime
  buffering meter**.
- **Persistence** — playlists, favourites, and settings saved locally in SQLite.

Roadmap: channel-logo thumbnails, XMLTV EPG (now/next), recording, picture-in-picture,
import/export favourites, scheduled auto-refresh, and a dead-link checker.

## Build

Requires **Visual Studio 2026 Community** (MSVC + bundled CMake/Ninja).

```
scripts\build.cmd                              # engine + CLI (no downloads)
scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON    # + the GUI (provisions libVLC)
```

Test / inspect the engine without the GUI:

```
build\RabbitEarsCli.exe --selftest             # parser + DB self-test
build\RabbitEarsCli.exe --fetch <url>          # test download + parse
build\RabbitEarsCli.exe --import <url|file>    # import into the app's DB
```

See [Win32/HANDOVER.md](Win32/HANDOVER.md) and [docs/architecture.md](docs/architecture.md)
for the design and current status. (The macOS port lives under [mac/](mac/).)

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). Bundles the public-domain SQLite
amalgamation; the GUI dynamically links **libVLC** (LGPL-2.1) — see the About box
for attribution.
