Read mac/HANDOVER.md and the recalled memory. RabbitEars is a cross-platform native IPTV player
(Windows + macOS) in ONE repo (common/ + Win32/ + mac/, unified root CMake; playback libVLC, storage
SQLite). main carries BOTH platforms at decoupled versions in cmake/AppVersion.cmake (APP_VERSION =
Windows 0.2.17; an if(APPLE) override = mac 0.2.16) — that file is the recurring cross-team merge
conflict, keep both lines. App min macOS 26 (Apple-Silicon-only, so the x86_64 slice can never run;
arm64-only builds are fine). Build: scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES=arm64.
Release recipe + gotchas: the mac-release-deployment memory.

TWO THINGS TO KNOW BEFORE ANYTHING ELSE:

1. NOTHING RECENT HAS REACHED USERS. main (478ac16, build 409) carries three releases' worth of
   merged work, but the live appcast still serves 0.2.15, so every installed mac app is on 0.2.15.
   mac 0.2.16 is BUILT + NOTARIZED + STAPLED yet DELIBERATELY UNPUBLISHED at
   ~/Downloads/RabbitEars-0.2.16.dmg (arm64, build 398, sha256 36b2f578..., 41,602,223 bytes) —
   the owner asked to hold the appcast, which is the step that actually pushes a build to everyone.
   The artifact is build 398 and main is 409, so a publish should REBUILD. To publish:
   sign_update --account SQLTerminal (one keychain Allow) -> gh release create v0.2.16-mac --target
   main --latest=false -> appcast <item> on main (xmllint FIRST).

2. THE SEEK LAYER IS ON main BUT UNVERIFIED. PR #46 (478ac16) landed the scrub bar, time readout,
   skip +/-10s and PAUSE (mac had none at all). It was merged on a green CI by owner decision, but it
   has NOT been adversarially reviewed and has NOT been driven on device. That was acceptable only
   because nothing is published. BOTH GATES ARE A HARD PRECONDITION OF ANY RELEASE CARRYING IT:
   (a) an adversarial ObjC++ find->verify Workflow (MRC/threading/logic lenses), applying only what
   it CONFIRMS, and (b) a GUI pass against a real seekable file AND a live channel — the live case
   is the one Win32 got wrong. The four design traps already fixed, and the measurements that settled
   its sampling design, are in the HANDOVER's own section; do not "fix" it back to Win32's atomics.

Already merged on main and NOT yet released: PR #43 (a Clang-vs-MSVC break that had mac unbuildable
on main — GridFilter nested-struct default member initializers), PR #44 = 0.2.16 "safe to upgrade"
(first mac build carrying schema v8+v9, where v9 is a DATA migration that rewrites every stream_url
and merges colliding rows; plus favourites canonicalisation, "Clear dead-link results", the
GridFilter/SQL grid pushdown + 5000-row cap + search debounce, an ATS exception, and three live
shipped bugs), and PR #45 = the Xtream VOD movie sync + a Movies nav root, VERIFIED end-to-end
against a fake Xtream panel (a first for either platform — Windows shipped it unverified).

NEXT: VERIFY the seek layer (see 2 above) — that is the top priority, because it is already on main
and a release would carry it. Then the small tail: dead-link sweep (now unblocked
by 0.2.16's ATS exception; the dangerous half is already compiled in), Settings > System log level
(SKIP the beta switchboard — its enum has no enumerators), PiP menu/swap/always-on-top, the appcast
host move off raw.githubusercontent.com, and PiP aspect-snap (videoSize() now exists for it).
mac is NOT behind on resume/watched or series/seasons — neither exists on either platform.

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
