# RabbitEars

A simple IPTV viewer for Windows — a native **Win32 / C++17** app built on
**libVLC**, with a dark "Claude-desktop-style" theme (coral accent) matching its
sibling apps `SQLTerminal-Win32` and `ManorLords-SGE`.

Paste an M3U/M3U8 URL (e.g. `https://iptv-org.github.io/iptv/index.m3u`) or open a
local playlist; RabbitEars parses the channels (name, logo, group, stream URL),
lets you search/filter, star favourites, assign custom channel numbers, and play
with full libVLC transport. Everything persists in a local SQLite database.

## Status

Early development. The engine (M3U parser + SQLite store) and build system are
complete and verified; the GUI is the next layer. See
[HANDOVER.md](HANDOVER.md) and [docs/architecture.md](docs/architecture.md).

## Build

Requires Visual Studio 2026 Community (MSVC + bundled CMake/Ninja).

```
scripts\build.cmd
build\RabbitEarsCli.exe --selftest            # core self-test (parser + DB)
build\RabbitEarsCli.exe path\to\playlist.m3u  # parse + store + dump a playlist
```

The GUI target (downloads + links libVLC) is built with
`scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON` once its sources land.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). Bundles the public-domain SQLite
amalgamation; the GUI dynamically links libVLC (LGPL-2.1).
