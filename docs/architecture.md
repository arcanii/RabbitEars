# RabbitEars — Architecture & Blueprints

Synthesized from a deep analysis of the two sibling apps (`SQLTerminal-Win32`,
`ManorLords-SGE`) plus libVLC / M3U research. This is the implementation reference
for the layers not yet built; the exact source lines to port are cited so a future
session need not re-derive them.

## 1. Module map

```
third_party/sqlite/      vendored SQLite amalgamation (static lib)
src/platform/Encoding.h  UTF-8 <-> UTF-16 (verbatim from SQLTerminal)
src/models/              Playlist, Channel, ParsedChannel (plain structs, wstring)
src/core/M3uParser       playlist bytes -> M3uDocument{epgUrl, [ParsedChannel]}
src/db/Database          SQLite DAO (Stmt/Tx RAII, schema, queries)
src/core/Http            [Layer C] WinHTTP GET of a playlist URL (worker thread)
src/ui/Theme.h           palette + WM_CTLCOLOR + dialog dark-mode (verbatim)
src/ui/D2DSupport.h      shared D2D/DWrite factories (verbatim)
src/ui/MainWindow        [Layer B] chrome + layout + wiring
src/ui/ChannelGridControl[Layer B] D2D owner-draw channel grid (port SqlGridControl)
src/ui/VlcPlayer         [Layer B] libVLC wrapper + video child HWND
src/ui/ThemedDialog      [Layer C] dark MessageBox + Add-Playlist dialog
src/platform/Updater     [Layer D] WinSparkle auto-update
```

## 2. UI look & chrome (port targets, with exact references)

**Palette** (`src/ui/Theme.h`, already placed). Dark: windowBg `RGB(22,22,24)`,
panelBg `RGB(26,26,28)`, panelElevBg `RGB(32,32,35)` (title/command/status bars,
grid header), altRowBg `RGB(28,28,31)`, hoverBg `RGB(42,42,45)`, border
`RGB(48,48,52)`, textPrimary `RGB(230,230,232)`, accent (coral) `RGB(217,119,87)`,
selectionBg `RGB(92,52,38)`. Fonts: Segoe UI `-dp(14)`, icons Segoe MDL2 Assets
`-dp(17)`. Everything DPI-scaled via `dpiScale()`, rebuilt on `WM_DPICHANGED`.

**Custom title bar** — reclaim the non-client area and paint our own bar:
- Window `WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN`. On `WM_NCCALCSIZE(wParam!=0)`:
  `frameX = SM_CXFRAME + SM_CXPADDEDBORDER`, `frameY = SM_CYFRAME +
  SM_CXPADDEDBORDER` (via `GetSystemMetricsForDpi`); shrink `rgrc[0]`
  left/right by frameX, bottom by frameY, **keep top** (reclaims the caption); if
  `IsZoomed` add frameY to top; `return 0`. Ref: `SQLTerminal MainWindow.cpp:1662`,
  identical `ManorLords MainWindow.cpp:1946`.
- Fill the reclaimed strip with a full-width owner-draw **command-bar child**
  (height 46dp): draws toolbar glyph buttons, a title/status chip, and the 3
  caption buttons (min/max/close, 46dp flush right), all double-buffered. Manual
  hit-test in `WM_LBUTTONDOWN` → `SC_MINIMIZE`/`SC_MAXIMIZE`/`WM_CLOSE`. Empty-area
  drag via `DragDetect` → `WM_NCLBUTTONDOWN(HTCAPTION)`; dbl-click toggles
  maximize. Ref: `SQLTerminal MainWindow.cpp:797-1077`.
- Dark frame: `DwmSetWindowAttribute` 20 (immersive dark), 34/35/36
  (border/caption/text colors), 33 (round corners). Dark menus via uxtheme
  ordinals 133/135/136 (guarded `GetProcAddress`). Child controls
  `SetWindowTheme(h, L"DarkMode_Explorer")`.

**Layout** — follow the simpler **ManorLords 2-pane** model (`MainWindow.cpp:83-353`):
command bar (46dp top) · left **nav sidebar** (~260dp) · vertical splitter (4dp) ·
right **content pane** · custom status bar (bottom). RabbitEars' content pane =
video surface (top) + transport strip (reserved bottom row, like ManorLords'
edit-value+Apply row) + channel grid (below). One `layout()` using
`BeginDeferWindowPos`. Rounded inset pane frames painted by the parent (RoundRect
r8, 1px border) in the inset margins — **not** `SetWindowRgn`. Double-buffer every
custom surface (`WM_ERASEBKGND` → 1, paint to memory DC, BitBlt).

- Nav sidebar: a `DarkMode_Explorer`-themed TreeView (or an owner-draw list): All,
  Favourites, then dynamic groups (`Database::listGroups()`) and playlists. Selecting
  a node sets the grid's view filter.

## 3. Channel grid (port `SqlGridControl.{h,cpp}`, ~865 lines)

**Decision: port the D2D grid, do not use a virtual ListView.** RabbitEars needs a
logo thumbnail cell, a clickable star toggle, and inline LCN editing — trivial
extensions of the D2D FillRectangle/DrawText loop and `columnAtX/rowAtY`
hit-testing, and the D2D control sidesteps the owner-data repaint gotcha.

- Custom `WNDCLASS "ReChannelGrid"`, `CS_DBLCLKS`, `hbrBackground=nullptr`,
  `WM_ERASEBKGND`→1; renders via `ID2D1HwndRenderTarget` (handle
  `D2DERR_RECREATE_TARGET`). State in `GWLP_USERDATA`.
- Data: `std::vector<Channel>` + a `rowOrder` permutation (display→data index) for
  sort/filter; only paints visible rows (`first = scrollY/rowH`, break when
  `y0>=viewH`) → smooth at 12k+ channels. Ref: `SqlGridControl.cpp:346-466`.
- Columns enum `{COL_NUM, COL_STAR, COL_LOGO, COL_NAME, COL_GROUP}`; fixed widths
  (#≈48, star≈32, logo≈56, Name flexible, Group≈180, all `dpx()`).
- Per-column custom paint: NUM = LCN text; STAR = U+2605 (accent) when favourite
  else U+2606 (textSecondary); LOGO = `DrawBitmap` (see logo pipeline); NAME/GROUP
  = DrawText + ellipsis.
- **Logo pipeline**: lazy `ID2D1Bitmap*` per channel via WIC; **decode remote
  tvg-logo bytes on a worker thread**, cache to disk/DB keyed by URL hash,
  `InvalidateRect` the row on completion. Release in `discardDeviceResources`.
- Interaction: star cell click → `toggleFavourite` + persist + invalidate row;
  double-click NUM → overlay a child EDIT for inline LCN edit (Enter commits, Esc
  cancels; reposition/hide on scroll); row click (non-star/#) → notify parent to
  play `channels[rowOrder[r]].streamUrl`. Type-ahead digit buffer (reset ~1s) →
  `channelByLcn` jump.
- Search: reuse `gridSetFilter` (`SqlGridControl.cpp:814`); an EDIT with
  `EM_SETCUEBANNER L"Search channels…"`, `EN_CHANGE` → filter; match
  name/group/tvg. A separate view-mode pre-filter (All/Favourites/group/playlist)
  runs before the text filter in `applyFilterSort`.

## 4. libVLC integration

**Provisioning** (`cmake/LibVlc.cmake`, already written): downloads
`VideoLAN.LibVLC.Windows` `3.0.23.1` `.nupkg` at configure time (or uses the NuGet
cache), points include/lib at `build/x64/`, exposes DLLs + `plugins/`. Link only
`libvlc.lib`. POST_BUILD copies `libvlc.dll` + `libvlccore.dll` + the whole
`plugins/` tree next to the exe (libVLC auto-discovers plugins relative to the DLL).

**VlcPlayer wrapper** (Layer B):
- One `libvlc_instance_t` at startup:
  `{"--intf=dummy","--no-video-title-show","--no-osd","--no-stats",
  "--network-caching=1000","--http-reconnect","--quiet"}`.
- Video surface: a `WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS` child inside the content
  pane; `libvlc_media_player_set_hwnd(mp, childHwnd)` **before** `play()`.
- Play a channel: `libvlc_media_new_location(inst, url)`; add options for
  buffering + per-channel `:http-user-agent=` / `:http-referrer=` (from the parsed
  channel); `libvlc_media_player_new_from_media`; `set_hwnd`; `play`.
- Events: `libvlc_event_attach` for `Buffering`
  (`event.u.media_player_buffering.new_cache`), `Playing`, `EndReached`,
  `EncounteredError` → callback runs on a libVLC thread → `PostMessage(WM_APP+n)`.
- Transport: `libvlc_audio_set_volume(0..100)`;
  `libvlc_video_set_aspect_ratio(mp,"16:9"/"4:3"/NULL)`; audio tracks via
  `libvlc_audio_get_track_description` (walk list) + `libvlc_audio_set_track`.
  Fullscreen = reparent/maximize our own video panel (more reliable than
  `libvlc_set_fullscreen` for an embedded HWND).
- HLS/IPTV: the adaptive/ts/mp4 demux + http/https access + gnutls plugins ship in
  the package — keep those categories if pruning to shrink the installer.
- **License**: LGPLv2.1 — dynamic-link + ship unmodified DLLs/plugins + include
  the LGPL text and attribution.

## 5. Data model & schema (implemented — `src/db/Database`)

Tables: `playlists`, `channels`, `settings` (see `Database::createSchema`).
Channels carry `lcn` (nullable, custom number), `is_favourite`, `dead_status` +
`last_checked_at` (roadmap dead-link checker), `tvg_id` (roadmap EPG join),
`user_agent`/`referrer` (playback hints), `sort_order` (as-parsed). Indexes on
playlist, group, partial fav, partial lcn, tvg_id, name COLLATE NOCASE, and a
unique `(playlist_id, stream_url)` for idempotent refresh. DAO: addPlaylist,
bulkInsertChannels (upsert in one Tx, preserves fav/lcn), listPlaylists,
allChannels/byPlaylist/byGroup/favourites/searchChannels/channelByLcn, listGroups,
setFavourite/toggleFavourite/setChannelNumber/setDeadStatus, get/setSetting.

Threading (Layer C): follow SQLTerminal's `DatabaseSession` single-worker-thread +
job-queue pattern so playlist download/parse + bulk insert never block the
UI/video thread; marshal completion back with `PostMessage`. A background EPG /
dead-link refresh may open a second read connection (WAL allows concurrent reads).

## 6. M3U parser spec (implemented — `src/core/M3uParser`)

Validated against `https://iptv-org.github.io/iptv/index.m3u` (12,905 channels).
Grammar handled:
```
#EXTM3U [x-tvg-url="…"] [url-tvg="…"]
#EXTINF:<duration> <key="value" …>,<display name>
#EXTGRP:<group>            (applies to the current/next entry)
#EXTVLCOPT:<key>=<value>   (http-user-agent, http-referrer, …)
<stream url>
```
Rules that matter: tokenize on the UTF-8 bytes (delimiters are ASCII);
display-name = text after the **first *unquoted* comma** (quoted attribute values
and titles both contain commas/semicolons, e.g.
`group-title="Entertainment;Family;General"`); attributes are `key="value"` or
bare `key=value`; strip a UTF-8 BOM; tolerate CR/LF/CRLF; bare-URL lines with no
`#EXTINF` become channels named from the URL's last path segment.

## 7. Build layers

- **A (done):** engine (parser + store) + CLI + build system. Verified.
- **B (next):** GUI shell — LibVlc provisioning, chrome, layout, VlcPlayer,
  ChannelGridControl. Turn on with `-DRABBITEARS_BUILD_GUI=ON`.
- **C:** features — Add-Playlist (WinHTTP/comdlg32 on a worker thread), search,
  favourites, LCN, transport, nav filtering, settings resume.
- **D:** packaging — .rc + icon, Inno Setup installer, WinSparkle auto-update,
  version.h.in, LGPL bundle.
