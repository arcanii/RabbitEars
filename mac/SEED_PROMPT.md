Read mac/HANDOVER.md and the recalled memory. RabbitEars is a cross-platform native IPTV player
(Windows + macOS) in ONE repo (common/ + Win32/ + mac/, unified root CMake; playback libVLC, storage
SQLite). main carries BOTH platforms at decoupled versions in cmake/AppVersion.cmake (APP_VERSION =
Windows 0.2.17; an if(APPLE) override = mac 0.2.17) — that file is the recurring cross-team merge
conflict, keep both lines. App min macOS 26 (Apple-Silicon-only, so the x86_64 slice can never run;
arm64-only builds are fine). Build: scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES=arm64.
Release recipe + gotchas: the mac-release-deployment memory.

TWO THINGS TO KNOW BEFORE ANYTHING ELSE:

1. v0.2.17-mac IS SHIPPED AND LIVE (2026-08-10, build 418, arm64, notarized; appcast @ 46e3072,
   feed serving 418 against the 318 users had). Everything merged is now in users' hands — the
   backlog of three unpublished releases is cleared. The old held ~/Downloads/RabbitEars-0.2.16.dmg
   is SUPERSEDED; delete it, never ship it. **THE FUNCTIONAL GAP TO WINDOWS IS CLOSED** — both
   platforms are 0.2.17 and the numbers match on purpose. What is unported is N/A BY DESIGN, not
   outstanding: theme engine, wake-to-record, PIP always-on-top (mac's PiP is a subview of the one
   main window, not a separate top-level window) and the beta switchboard (enum has no enumerators).
   What is genuinely left is COSMETICS ONLY (meter glass + VU needle — one CGBitmapContext
   restructure or neither; look-aware knobs; tip buttons, which need a custom About window) plus the
   appcast host move off raw.githubusercontent.com — and that one is NOT a gap, since
   Win32/platform/Updater.cpp serves from the same host.

   ⚠ WHAT 0.2.17 SHIPPED WITHOUT: an ON-DEVICE pass on the four gap-tail features (dead-link sweep,
   Settings > Logging, PiP menu/swap, PiP aspect snap). They were merged on green CI AND a full
   adversarial review that found and fixed four defects, and their non-GUI halves were verified by
   running — but nobody drove them in the GUI. Owner's call, same as 0.2.7's recorder. IF A BUG
   SURFACES IN THE WILD, START THERE. The seek layer, by contrast, IS fully device-verified.

2. THE SEEK LAYER IS VERIFIED — both gates ran on 2026-08-10, so it is NO LONGER a release blocker.
   PR #46 (478ac16) landed the scrub bar, time readout, skip +/-10s and PAUSE. The adversarial
   review (12 findings -> 9 survived -> 3 refuted, incl. the only HIGH) plus a full on-device pass
   found FIVE defects, all fixed in PR #47 (mac-seek-review-fixes): the post-seek latch was scoped
   to the pane SET not the player; it latched the UNCLAMPED target; the bottom bar reserved 150pt
   for a cluster that starts at 162 (the readout sat ON the meter button and ate its clicks);
   -applyLanguageLive never relabelled the skip buttons; and the bar could never retire at
   end-of-film (at libvlc_Ended, length and seekable BOTH persist — only get_time freezes).
   Do not "fix" the sampling back to Win32's atomics; that was settled by measurement.
   TWO TRAPS WORTH INHERITING, each of which produced a confidently WRONG answer:
   (a) a libVLC probe MUST use the app's instance args — `--no-video --no-audio` reports length=0
   on a live HLS while the app's instance reports the real DVR window (180s on real live TV),
   which is why the scrub bar CORRECTLY appears on live channels; and (b) `python3 -m http.server`
   implements no HTTP Range, which makes a local .mp4 seek look broken in a way indistinguishable
   from an app bug (keep a Range-capable stub in the fixture kit).

Already merged on main and NOT yet released: PR #43 (a Clang-vs-MSVC break that had mac unbuildable
on main — GridFilter nested-struct default member initializers), PR #44 = 0.2.16 "safe to upgrade"
(first mac build carrying schema v8+v9, where v9 is a DATA migration that rewrites every stream_url
and merges colliding rows; plus favourites canonicalisation, "Clear dead-link results", the
GridFilter/SQL grid pushdown + 5000-row cap + search debounce, an ATS exception, and three live
shipped bugs), and PR #45 = the Xtream VOD movie sync + a Movies nav root, VERIFIED end-to-end
against a fake Xtream panel (a first for either platform — Windows shipped it unverified).

NEXT: there is no parity work left to do, so pick by value, not by gap. The highest-value item is
the on-device pass 0.2.17 shipped without (see 1 above) — the checklist is in the HANDOVER's
gap-tail section. After that: the appcast host move (BOTH platforms, not a mac gap), then cosmetics
(meter glass + VU needle as ONE CGBitmapContext restructure or neither; look-aware knobs; tip
buttons, which need a custom About window since the system panel cannot host them and
NSHumanReadableCopyright is where the GPL-3.0 notice lives). New features are now genuinely new
work rather than catching up — resume/watched and series/seasons/episodes exist on NEITHER platform,
and mac is AHEAD on the groundwork for the latter (LogoLoader is already the async, disk-cached,
bomb-safe poster loader it needs).

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
