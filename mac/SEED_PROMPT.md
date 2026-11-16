Read mac/HANDOVER.md and the recalled memory. RabbitEars is a cross-platform native IPTV player
(Windows + macOS) in ONE repo (common/ + Win32/ + mac/, unified root CMake; playback libVLC, storage
SQLite). main carries BOTH platforms at decoupled versions in cmake/AppVersion.cmake (APP_VERSION =
Windows 0.2.22-dev, 0.2.21 shipped; an if(APPLE) override = mac 0.2.22) — that file is the recurring
cross-team merge conflict, keep both lines. App min macOS 26 (Apple-Silicon-only; arm64-only builds are
fine). Build: scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES=arm64. Release recipe + gotchas: the
mac-release-deployment memory.

THREE THINGS TO KNOW BEFORE ANYTHING ELSE:

1. v0.2.22-mac IS SHIPPED AND LIVE (2026-11-16, build 479, arm64, notarized; appcast @ 5db07ff; v0.2.17-mac retitled
   "superseded"). It migrated a real 0.2.17-authored library on the ARTIFACT before publishing. PRs #49-#55 (2026-11-16)
   ported everything Windows shipped in 0.2.18-0.2.22-dev: log credential masking; the scheduler's lost-
   status-write fix; the TV Guide on the shared row build (search, coverage line, multi-guide refresh on a
   worker); catch-up; the photoreal meter line (Backlit + Silver VU, Studio LED / Backlit LCD / VFD, meter
   size + bridge + labels + glass + look-aware knobs); and the data-flow tank, which mac NEVER had. The
   HANDOVER's top box has the table. Several shared pieces MOVED Win32 -> common (UrlRedact, LogSecrets.h,
   GuideModel, HttpProgress.h, TimeZone.h as a seam, CatchupSync, PhotoCells behind the new
   common/platform/WinTypes.h) with their include names unchanged.

2. IT SHIPPED WITHOUT AN ON-DEVICE PASS (owner's call) — nothing in this round was driven in the GUI; if a bug surfaces,
   start with the HANDOVER checklist. The selftest went
   154 -> 443 (mostly Win32 CLI blocks ported verbatim, so the shared code is proven under Apple clang), the
   looks/tank/bridge were rendered headlessly and eyeballed, but: the checklist is in the HANDOVER box, and
   two ASSUMPTIONS ride on it — the audio needle undoes libVLC auhal's volume as v^3 (from VLC source, not
   measured), and a real Xtream archive playing (catch-up proven only through shared code).

3. 0.2.22 WAS A ONE-WAY DOOR: it migrates libraries to schema v11 (FTS5), and 0.2.17 (no FTS5 compiled in)
   CANNOT WRITE a v11 library (`no such module: fts5` from the channels_fts_* triggers). EVERY older mac release
   (v0.1.7-v0.2.17) is retitled "superseded by 0.2.22" with a do-not-reinstall warning — v0.1.7's says "on macOS 26+",
   since the feed still offers it (minimumSystemVersion 11.0) to macOS 11-15, where 0.2.22 cannot run.
   NB the system /usr/bin/sqlite3 has no FTS5 either — query a v11 library with a tiny program against the
   vendored third_party/sqlite (build-*/libsqlite3.a), not the CLI.

What stays N/A BY DESIGN: the theme engine (so skin MATERIALS draw Flat — Windows does the same for a skin
without one), wake-to-record, PIP always-on-top, the beta switchboard, Windows' side profile, its dark scroll
bars, its libVLC HTTP/2 log filter (mac routes no libVLC log).

NEXT: the on-device pass 0.2.22 shipped without (checklist in the HANDOVER box). Release recipe: the
mac-release-deployment memory; sign_update must run in the OWNER's terminal (the keychain ACL refuses this
tooling). After that: the appcast host move (BOTH
platforms), and genuinely new work — resume/watched and series/seasons/episodes exist on NEITHER platform.

HARD-WON RULES (each cost real time):
- Win32 gui-build CI is pre-existing red and dies at CONFIGURE (needs fxc), so it compiles NOTHING.
  The real MSVC gate for a common/ change is Windows core-selftest. A common/ header change can
  break every mac build while Windows CI stays green — that is how main sat unbuildable for days.
- VERIFY BY RUNNING, NOT BY REASONING. Migrations: author the OLD schema with the actually-shipped
  binary (RABBITEARS_DATA_DIR=<scratch>, kill after ~5s), seed adversarial cases, run the NEW binary.
  Reasoning got v9 wrong on both sides — the Win32 handover calls it a no-op on a live-TV-only
  library and it is not. Seeding a dead_status case? Set last_checked_at too (the merge couples them).
  Perf/threading: MEASURE. The seek design was settled by benchmarking libVLC getters (0.05-0.16us,
  and they do not block on a wedged feed), which reversed the inherited "sample off the UI thread"
  advice.
- A MITIGATION CAN BE WORSE THAN THE BUG. Deferring a due recording while a VOD sync ran was
  unbounded and could lose the recording it protected; the right rule is Win32's — a due recording
  outranks a catalogue refresh, so cancelVodSync() and proceed. When a fix inverts a priority, check
  the unbounded-wait case.
- mac .mm are MRC by default; -fobjc-arc is PER FILE (list in mac/CMakeLists.txt).
  MainWindowController.mm / AppDelegate.mm / VlcPlayerMac.mm are MRC; VodSync.mm holds no Obj-C
  objects on purpose. The recurring trap, shipped 3x: an autoreleased object stored into an ivar
  WITHOUT retain, or a bare pointer into a collection that is then freed. ALWAYS run an adversarial
  find->verify Workflow AND on-device-verify a native change.
- GUI/audio cannot be verified headlessly. The INSTALLED /Applications app shares the bundle id with
  a dev build (dual-instance composite trap) so QUIT it and run the dev build as the SOLE instance
  against an isolated RABBITEARS_DATA_DIR DB. A ~40-line python player_api.php / m3u / XMLTV stub on
  127.0.0.1 makes provider paths deterministic and offline (loopback is ATS-exempt).
- git push HANGS intermittently AND gh api throws transient 5xx — sometimes both. Land on main via
  gh api PUT contents; push a branch via plain git push (retry) OR the Git Data API
  (blobs->tree->commit->ref, VERIFY the returned tree sha == git rev-parse HEAD^{tree}).
  NEVER git reset --hard origin/<branch> while a push is failing — origin is stale and it DISCARDS
  local commits. Commit doc edits before any git op.
- Sole dev: review the plan, then gh pr merge <n> --merge --delete-branch. Don't wait for an approval
  that never comes. PRs exist as the CI/MSVC gate. Never --admin (auto-mode blocks the bypass flag).
- i18n: edit common/i18n/*.json + run tools/i18n/gen_i18n.py — Strings.{h,cpp} are GENERATED, never
  hand-edited. keys.json order == the StringId enum order, so APPEND, never insert. zh-HK carries
  only overrides and inherits zh-Hant via a "base" declared in languages.json, NOT in zh-HK.json.
