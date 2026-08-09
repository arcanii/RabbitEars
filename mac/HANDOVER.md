# RabbitEars — macOS Handover

The macOS team's living handover. (The Windows team's is [`Win32/HANDOVER.md`](../Win32/HANDOVER.md);
the port rationale + history is [`docs/MACOS_PORT.md`](../docs/MACOS_PORT.md).) Read this before
touching the mac app.

## What RabbitEars is

A cross-platform native IPTV player in **one repo**: **`common/`** (portable core — `M3uParser`,
`Database`, `DockLayout`, `FlowStats`, XMLTV/EPG + recording-scheduler cores, models, platform seam
*headers*), **`Win32/`** (the Windows app), **`mac/`** (this — the Cocoa app), under a unified root
`CMakeLists.txt` (`common` → `Win32`/`mac` per‑OS). Playback is **libVLC**; storage **SQLite**.

`main` carries **both platforms at decoupled versions**: **Windows 0.2.11** (theme engine + EPG/TV Guide,
scheduled recordings incl. wake-to-record + EPG series rules, multi-view Split/PIP, saved layouts, per-pane
recording, live language switch — the Windows team ships from `main`) and **mac 0.2.10** (the parity line). The version split lives in
`cmake/AppVersion.cmake` (`APP_VERSION` = Windows; an `if(APPLE)` override = mac). **That file is the one
recurring merge conflict** between the two teams — keep the Windows line and the `if(APPLE)` override intact.
Keep all mac work **Windows-safe** and let `windows-core` / `macOS core` CI confirm.

> **Scope:** the SHIPPED mac **0.2.9** (build 261, universal, notarized — released 2026-07-13) reaches parity
> with the Windows **0.2.9** set: a **recording-rule editor** (New…/Edit… series rules), **series-rule episode
> dedup** (schema v6, already shared), **Traditional Chinese** (zh-Hant + zh-HK) in the Language selector, and
> the **GPL-3.0 notices** bundled in `Resources/`. It builds on 0.2.8 (localization EN + 日本語 + gear regrouped),
> 0.2.7 (the #25→#29 stack: favourites I/O, PiP resize/persist, saved layouts, per-pane recording, the recording
> scheduler + series rules) and 0.2.0 (TV Guide, multi-view, PiP). Unported **by design**: the Windows **theme
> engine** (mac uses the native system appearance) and **wake-to-record** (a non-root mac app can't arm a wake).
> Windows **0.2.10** is a Win32-only Traditional-Chinese *language-selection* hotfix, N/A to mac.

## SHIPPED — the 0.2.6/0.2.7 parity stack (v0.2.7-mac, build 234, 2026-07-11)

The five-PR stack **merged to `main` in order #25→#29** (merge commits `f387ad0`→`de240fd`), the mac version was
bumped to **0.2.7** (`cmake/AppVersion.cmake` APPLE override → build 234, commit `f9f7404`), and **`v0.2.7-mac`
shipped** — universal, notarized, appcast live on `main` (`3c832cf`). All merges were `gh pr merge --merge`; the
version bump + appcast landed via `gh api` PUT (the git REST path — `git push` still hangs this session). Zero
`common/`/`Win32/` edits across the whole stack — every core (`VideoGrid`, `M3uWriter`, `RecordingScheduler`,
`RecordingRules`, the schema-v5 `Database` methods) was **already compiled into the mac binary**; wiring, not porting.

| PR | Branch | Phase | On-device |
|----|--------|-------|-----------|
| [#25](https://github.com/arcanii/RabbitEars/pull/25) | `mac-favourites-io-guide` | Favourites import/export + Show in TV Guide | ✅ verified |
| [#26](https://github.com/arcanii/RabbitEars/pull/26) | `mac-pip-resize-persist` | PiP inset resize + persist (size/pos) | ✅ verified |
| [#27](https://github.com/arcanii/RabbitEars/pull/27) | `mac-saved-layouts` | Named saved multi-view layouts | ✅ verified |
| [#28](https://github.com/arcanii/RabbitEars/pull/28) | `mac-recording` | Per-pane recording to file (ts/mp4) | ⚠️ **shipped unverified** |
| [#29](https://github.com/arcanii/RabbitEars/pull/29) | `mac-recording-scheduler` | Scheduled recordings + EPG series rules + honest wake | ⚠️ **shipped unverified** |

> **⚠ 0.2.7 shipped WITHOUT on-device verification of P4 recording + P5-7 scheduler.** The owner chose "ship it"
> on 2026-07-11; the recording/scheduler **file-muxing paths never ran on real hardware** (green mac-core CI +
> the adversarial reviews were deemed sufficient). "Does a real `.ts`/`.mp4` play?" and "does the ~30s tick fire
> a playable scheduled file?" remain **unproven on device** — if a recording/scheduler bug surfaces in the wild,
> start here (→ a 0.2.8-mac patch). P1-3 (favourites/PiP/layouts) WERE device-verified before merge.

Each phase got an **adversarial ObjC++ review** (multi-agent workflow) that found + fixed real bugs before
merge — the reviews repeatedly earned their cost:
- **P1** import used a last-wins map (a channel duplicated across playlists marked only one row) → set-based.
- **P2** resize-taller-near-top overflowed the container, persisting `_pipPosY>1` (jumps on relaunch) → clamp to free space at the pinned corner.
- **P3** empty-pane wasn't cleared on apply (a carried/pre-existing stream kept playing) + NaN survived the geometry clamp.
- **P4** the **MKV format would ship broken** (bundled VLC has `libmux_ts`+`libmux_mp4` but **no mkv muxer** — `libmkv_plugin` is the DEMUXER; dropped mkv); a main-thread record-stop hung the UI on a stalled feed → `stopRecordingAsync` (reaper-thread peer); non-POSIX-locale timestamp; MRC formatter leak; same-second filename collision.
- **P5-7** cancelling a rule-generated future airing **hard-deleted the dedup tombstone → next expansion resurrected it** (kept as Cancelled anchor, Win32-parity); series channels resolve by NORMALISED tvg-id (else `@feed` channels record nothing); Recordings table preserves selection by id across the ~30s reload.

**Recording design (P4/P5):** each `VlcPlayerMac` gets a recorder = a SECOND headless libVLC media player
muxing to `~/Movies/RabbitEars` via `:sout=#std{access=file,mux=ts|mp4,dst='…'}` + `:sout-keep` (stream-copy,
no set_nsobject) — mac is PER-PANE (Win32 uses one shared recorder), so scheduled recordings run on a
**dedicated headless recorder** driven by a ~30s `NSTimer` tick over the shared `planScheduler()`. A ~2nd
connection per recording (breaks 1-connection IPTV accounts) is surfaced; ts is the crash-safe default (mp4
loses its index on a hard crash). `stopRecordingAsync` is off-main; `-finalizeRecordingsForQuit` (from
`applicationWillTerminate:`) flushes open recordings because the MRC app-lifetime objects don't destruct on quit.

**Honest wake (P7):** an `IOPMAssertion(PreventUserIdleSystemSleep)` is held while any recording runs, but a
non-root, hardened, Developer-ID app **CANNOT wake a sleeping Mac** — `IOPMSchedulePowerEvent` requires root
(verified in the macOS 26 SDK `IOPMLib.h`). So scheduling shows the caveat up front and the Recordings window
states it: records only while running + Mac awake (lid open). That is the deliberate degraded design, NOT a TODO.

> **PiP-switch freeze fix — SHIPPED in 0.2.7** (`0ab8618`, already on `main` before the stack landed). "Play in
> PiP" while already in PiP (any re-play into a running pane) called `set_media` without stopping the current
> input — on some live IPTV feeds the old inset wedged (frozen) and the new never started. Fix:
> `libvlc_media_player_stop()` before `set_media` in `VlcPlayerMac::play()`. **Could NOT reproduce with VOD test
> streams** (identical code to 208 switched them cleanly); it targets the likely cause but is **still unconfirmed
> on the affected IPTV channels** — now released, so a persistent freeze there would be a 0.2.8-mac follow-up.

**NEXT (on-device, post-ship validation):** 0.2.7 is out, so this now validates *shipped* code — record a real
HLS stream (confirm the `.ts`/`.mp4` plays), schedule ~1 min out (watch the ~30s tick fire a playable file), and
confirm the PiP-switch fix on real IPTV. A failure here means a 0.2.8-mac patch, not a blocked merge. **On-device
traps that cost hours are listed under Working rules.**

## Current state — mac 0.2.16 on `main`, BUILT + NOTARIZED but **deliberately NOT PUBLISHED** (2026-08-09)

> **Read this box first.** `main` (`c5ac088`, build **403**) carries three merged mac releases' worth
> of work that **no user has**. The live appcast still serves **0.2.15**, so every installed mac app
> is on 0.2.15 and nothing below has reached anyone.
>
> **⏳ AND THERE IS UNMERGED WORK IN FLIGHT:** branch **`mac-seek-scrubbar`** (`bbc7db9`) has the
> whole seek layer — scrub bar, time readout, skip ±10 s, and **pause** (mac had none at all). It
> **builds clean and the selftest passes, but it has NOT been adversarially reviewed and has NOT
> been driven on device.** Both are required before merge here — that pair has caught a real bug in
> every phase of this project, including two regressions the author introduced. **Do not merge it on
> "it compiles".** See its own section below.

**Windows leapt 0.2.11 → 0.2.17 while mac was on the 0.2.12–0.2.15 line** (their v0.2.16 = the Xtream
VOD release, v0.2.17 = the "big library" release). `cmake/AppVersion.cmake` now reads Windows
**0.2.17** with the `if(APPLE)` override at **0.2.16** — keep both lines.

**Three things landed on `main` today, in order:**

1. **🔴 [PR #43](https://github.com/arcanii/RabbitEars/pull/43) — the mac build was BROKEN on `main`**
   (merged @ `d6a0434`). `macOS core` CI had been red since `04b4d22`: `Database::GridFilter` was a
   struct NESTED in `Database` with default member initializers, used as `= {}` default arguments on
   8 methods in the same class body. Those initializers belong to `Database`'s complete-class
   context, so **MSVC accepts it and Clang correctly rejects it** — Windows CI stayed green while
   every mac build died. Fixed by defining `GridFilter` at namespace scope + a
   `using GridFilter = ::rabbitears::GridFilter;` member alias, so all 15 Win32 call sites are
   unchanged. **⚠ Win32 `gui-build` CI dies at CONFIGURE time (`RABBITEARS_THEME_ENGINE needs fxc`)
   so it never compiles anything — the real MSVC gate for a `common/` change is Windows
   `core-selftest`, which compiles `RabbitEarsCli.cpp`.**

2. **[PR #44](https://github.com/arcanii/RabbitEars/pull/44) — 0.2.16 "safe to upgrade"** (merged @
   `5b76422`; version bumped 0.2.15→0.2.16 @ `59ec30a`). This is the first mac build carrying
   Windows' **schema v8 + v9**, and **v9 is a DATA migration** (it rewrites every stored
   `stream_url` to canonical form and merges the rows that then collide), so the release was scoped
   to correctness. Contents: favourites-import canonicalisation (both sides, or a pre-v9 export
   silently degrades to tvg-id-only matching); a **"Clear dead-link results"** gear item (mac marks
   channels Dead from playback failure alone and hides them — there was **no way back**); the
   **`GridFilter` pushdown + 5000-row cap + search debounce**; an **ATS exception**; and three live
   shipped bugs — the meter Look popup wired to **swapped** enum values (picking "LCD" gave you the
   vacuum-tube look), leaving PiP hijacking pane 0, and one series airing reporting `Cancelled`
   where the series keeps running (now `Skipped`). Two selftest blocks that were **Windows-only
   targets** were added to the mac selftest: the `GridFilter` twin (incl. the cap+filter composition
   case that returns ZERO under the old ordering) and the i18n catalog gate.

3. **[PR #45](https://github.com/arcanii/RabbitEars/pull/45) — Xtream VOD sync + 🎬 Movies nav root**
   (merged @ `e96a86b`) — see its own section below.

**The 0.2.16 artifact exists and is release-grade, but was held back on purpose.** Built **arm64-only**
(`LSMinimumSystemVersion` is macOS 26, which is Apple-Silicon-only, so the x86_64 slice can never
run — and every cached `vlc-3.0.23-universal.dmg` had been cleaned from old scratchpads).
`~/Downloads/RabbitEars-0.2.16.dmg`, build **398**, sha256 `36b2f578…`, **41,602,223** bytes,
Developer-ID signed + hardened + **notarized + stapled** (`spctl` → "Notarized Developer ID"), ATS
key confirmed in the shipped bundle. **No GitHub release, no appcast entry, no `sign_update`** —
the owner asked to hold the appcast, and the appcast entry is the step that actually pushes a build
to every existing user (the feed is served from `main`). `sign_update` is only needed for an appcast
enclosure, so skipping it also skipped its keychain prompt; the dmg is byte-stable after stapling,
so a signature computed later is still valid for this exact artifact. **⚠ Note the artifact is build
398 but `main` is now 402 — a publish should REBUILD rather than ship the stale dmg.**

**To publish, when the owner says so:** `sign_update --account SQLTerminal <dmg>` (one keychain
Allow) → `gh release create v0.2.16-mac <dmg> --target main --latest=false` → add the `<item>` to
`mac/packaging/appcast-mac.xml` (`sparkle:version` = CFBundleVersion; `xmllint` FIRST) and land it
on `main`.

**Migration safety — verified empirically, not reasoned.** The method is worth repeating: author the
OLD schema with the **actually-shipped** binary (`RABBITEARS_DATA_DIR=<scratch>` + the 0.2.15 app,
kill after ~5 s → a real v7 DB), seed the adversarial cases, then run the NEW binary against it.
Result on the SIGNED 0.2.16 artifact: v7 → **v9**, cols 15 → 20, `integrity ok`, `:80` rewritten,
**`:8080` left alone** (the correctness case), a collision merged with the loser's favourite + LCN
inherited, and the app's exact 20-column query still returning the full library (so v8's
"silently empty" failure mode is absent). **It also ran on the owner's REAL 13,798-channel library:
0 URLs left carrying `:80`/`:443`, 1,416 non-default-port URLs untouched, 0 duplicate rows, 22
favourites intact.** ⚠ **The Win32 handover's claim that v9 is "a no-op on a live-TV-only library"
is FALSE** — it rewrites rows on any provider that emits `:80`, which is the majority case.
⚠ **Downgrade is read-safe but NOT write-safe:** running 0.2.15 against a migrated v9 DB and hitting
Refresh re-inserts literal `:80` rows that no longer match — a duplicate-library hazard, and easy to
trigger given the dual-instance trap.

## Before it — v0.2.15-mac SHIPPED (2026-07-17), the last release users actually have

**`v0.2.15-mac`** (build 318, universal, notarized; release [`v0.2.15-mac`](https://github.com/arcanii/RabbitEars/releases/tag/v0.2.15-mac), appcast live @ `ea8e8b6`) **ships channel-logo thumbnails in the channel grid** — [PR #42](https://github.com/arcanii/RabbitEars/pull/42) (`mac-logo-thumbnails`), merged @ `7ce0947`. A logo `NSImageCell` column between `#` and `Channel`, fed by a new ARC **`LogoLoader`** (main-thread `-imageForURL:` → memory `NSCache` → disk cache `<dataDir>/logos` storing the ~96px PNG thumbnail → streamed `NSURLSessionDataDelegate` download capped at 3 MB → bomb-safe ImageIO decode: pixel dims from the header, reject >2048px, downsample; keyed by URL = cell-reuse-safe; faint placeholder for logo-less / cleartext-http / 404 rows). `tvg-logo` was already parsed+stored → **mac-only, zero `common/`/`Win32/`**. Two find→verify review passes + on-device both caught real bugs before merge (an MRC autoreleased-placeholder over-release CRASH; 2 medium hardening findings → the streamed-cap + ImageIO rewrite; 3 low re-review nits, 2 fixed in `35e5210`, the 2048px cap kept intentional). Version 0.2.14→0.2.15 (APPLE override). Standard universal recipe (cached `VLC-universal.app` reused → `package-mac.sh --sign` → `hdiutil` dmg → `codesign --timestamp` → `notarytool --wait` Accepted → `stapler staple` 84692037→**84708385** → `sign_update --account SQLTerminal` [keychain prompt — user clicked Allow] → `gh release create --latest=false` → appcast via `gh api PUT`). Verified end-to-end: downloaded asset **sha256 byte-identical** to the signed dmg (`f892bac5…`, `length=84708385`), spctl "Notarized Developer ID", staple validates, embedded SUPublicEDKey matches, sparkle:version 318 > 311, live raw feed serves 0.2.15 immediately; the Windows release keeps the "Latest" badge (v0.2.11). **`git push` HUNG for both main writes** (version bump + appcast) → each landed via `gh api PUT contents` + `git fetch`/`reset --hard`. **This cleared channel-logo-thumbnails from the Win32-gap backlog.** (Windows was 0.2.11 at the time; it has since gone to 0.2.17 — see the current-state section at the top, and the re-derived gap list, which supersede the backlog wording here.)

**Before it: `v0.2.14-mac`** (build 311, universal, notarized; release [`v0.2.14-mac`](https://github.com/arcanii/RabbitEars/releases/tag/v0.2.14-mac), appcast @ `87a8a8e`) **shipped the two shared-core (`common/`) P2 fixes** — the last of the Win32-gap shared-core work. **[PR #40](https://github.com/arcanii/RabbitEars/pull/40)**: series-rule **phantom-`Missed`** after a lead-time edit → fixed by persisting the programme's UNPADDED start (**schema v7**: `scheduled_recordings.prog_start_utc`) as the padding-proof airing identity. **[PR #41](https://github.com/arcanii/RabbitEars/pull/41)**: the **Xtream group-title→country fallback** (a registered SQLite scalar `effective_country()`; deny-list `HD SD TV EN XX EX ON`; `AR|`-Arabic→Argentina kept as documented known-wrong). Both were adversarially reviewed (the reviews refuted PR #40's first heuristic design and benchmarked PR #41's first C++-side filter at ~30 ms/keystroke → the SQL scalar) and are flagged to the Win32 team in `Win32/BACKLOG.md` (both Windows-affecting). Version 0.2.13→0.2.14 (APPLE override). Same universal recipe. **Extra release-verification for the schema migration:** the SHIPPED signed universal binary migrated a hand-built v6 DB → v7 with `prog_start_utc` added + rows intact (the one data-mutating change, proven on the artifact, not just a dev build). End-to-end verified: downloaded asset sha256-identical, `length=85505308`, spctl Notarized, embedded SUPublicEDKey matches, sparkle:version 311 > 297, feed serves 0.2.14; Windows keeps `--latest` (v0.2.11). `git push` worked for all four main writes; the `gh release view`/`releases/latest` endpoints threw transient GitHub 5xx HTML during verification — the release-list endpoint + a direct `curl` of the asset confirmed everything is live (a `gh` flake, not a release problem). Windows stays 0.2.11.

**Before it: `v0.2.13-mac`** (build 297, universal, notarized; release [`v0.2.13-mac`](https://github.com/arcanii/RabbitEars/releases/tag/v0.2.13-mac), appcast @ `6838da3`) — a **launch-hang hotfix**. The owner's installed app auto-updated to 0.2.12 and **beachballed on the Sparkle post-update relaunch**, recovering on a manual restart. **Root cause (confirmed via `showWindow`-phase timing instrumentation, not guessed):** the clean launch path is only ~290 ms, so heavy init was *not* it — the blocker was the **Terms-of-Use gate**, which re-fires on every version change and ran as an **app-modal (`[TermsDialog runModal]`) BEFORE the main window was built and before `[NSApp activateIgnoringOtherApps:]`**. On a Sparkle relaunch the app isn't auto-activated, so the modal came up **buried behind other windows with no visible main window** — reading as a beachball. A manual restart auto-activates the app, so the modal was visible and the user clicked Accept. **Fix (`2c1f9b8`):** build + show + activate the window FIRST, then present the ToU as a **document-modal SHEET** on it (a sheet cannot be buried independently of its window); the "usage" side effects (Spectrum tap, video attach, stats timer, resume auto-play, recording scheduler) moved into a new **`-finishStartup`** run only after Accept. Because a sheet blocks only its window (not the menu bar, unlike the old app-modal), **`AppDelegate -validateMenuItem:` now disables app commands until `-finishStartup` runs** (a `startupFinished` flag on the controller) so the gate still blocks the whole app. **Adversarially reviewed** (3 parallel lenses — MRC/ARC lifetime, threading/AppKit-async, logic/parity; the memory & threading mechanics were clean, and the review CAUGHT the one real regression: a document-modal sheet doesn't disable the menu bar → added the `validateMenuItem` gate). **On-device verified** (sole dev instance, isolated seeded DB): already-accepted → NO sheet, straight to play; version-change → window shown FIRST, ToU sheet descends from the title bar, **View menu commands greyed while pending**, Accept → channels load + resume auto-plays + meters, **menu re-enables**; the non-frontmost/Sparkle case is covered by construction (`makeKeyAndOrderFront` precedes the sheet unconditionally, so there is always a real window). Version 0.2.12→0.2.13 (APPLE override). Same universal recipe; verified end-to-end (downloaded asset sha256-identical, `length=85495168`, spctl Notarized, sparkle:version 297 > 293, feed serves 0.2.13). **`git push` WORKED this session** for all four writes (fix, bump, appcast, and this HANDOVER). Windows stays 0.2.11.

**Before it: `v0.2.12-mac`** (build 293, universal, notarized; release [`v0.2.12-mac`](https://github.com/arcanii/RabbitEars/releases/tag/v0.2.12-mac), appcast @ `816e9f0`) **shipped the four Win32-gap parity features below** — the batch that was merged + on-device GUI-verified is now released. Version bumped 0.2.11→0.2.12 (APPLE override in `cmake/AppVersion.cmake`; Windows stays 0.2.11). Standard universal recipe (cached `vlc-3.0.23-universal.dmg` → `package-mac.sh --sign --vlc <universal>` → `hdiutil` dmg → sign → `notarytool --keychain-profile SQLTerminal-notarize --wait` Accepted → `stapler staple` → `sign_update --account SQLTerminal` [keychain prompt — user clicked Allow] → `gh release create --latest=false` → appcast via `gh api PUT`). Verified end-to-end: downloaded asset **sha256 byte-identical** to the signed dmg (edSignature `ezJsOy61…` valid), `length=85492936` matches, spctl "Notarized Developer ID", staple validates, embedded SUPublicEDKey matches, sparkle:version 293 > 276, live raw feed serves 0.2.12. `git push` hung for all three main writes (version bump, HANDOVER, appcast) → each landed via `gh api PUT contents`.

### ⏳ IN FLIGHT — the seek layer (branch `mac-seek-scrubbar` @ `bbc7db9`, NOT merged, NOT verified)

**Status: builds clean, selftest `ALL PASS`, and that is ALL.** No adversarial review has run against
the code, and it has never been driven on device. Finish those two before merging — on this project
that pair has caught a real bug in *every* phase, including two the author introduced.

**What it adds.** `VlcPlayerMac` gains `timeMs` / `lengthMs` / `isSeekable` / `seekTo` / `setPaused` /
`isPaused` / `videoSize`; `MainWindowController` gains a bottom-bar transport cluster (⏸ ⏪ scrub ⏩
`12:34 / 1:45:00`). mac had **no seek API and no pause at all** (`libvlc_media_player_pause` appeared
nowhere in `mac/src`), so a film from the new VOD sync could not be scrubbed or even held.
**Zero new catalog strings** (reuses `TooltipBtnPause`, `LabelPlay`, `TooltipBtnSkipBack`/`Fwd`),
zero `common/`/`Win32/`.

**The sampling question was SETTLED BY MEASUREMENT — do not "fix" it back to Win32's shape.** The
getters are LIVE reads on the existing 250 ms main-thread tick. Measured against the vendored libVLC
3.0.23: `get_time` 0.05 µs, `get_length` 0.06 µs, `is_seekable` 0.16 µs — *less* than the audio-track
enumeration `setMuted()` already does there on every pane. They do **not** block on a wedged feed
(the input thread parks in `read()`/`connect()` without holding the player lock, tested on two wedge
shapes). A background sampler would block **anyway** — it takes the same lock `stop()` holds — and
would create a thread boundary mac does not have, across a raw `impl_->player` that `-teardownPane:`
deletes from a background queue. Win32's relaxed atomics exist because **its worker owns `mp_`**;
that is a Win32 problem. Live polling also means there is no cache to reset on stop, and Win32's
never-reset `videoWH_` staleness cannot be inherited.

**⚠ The one invariant it depends on** (written into `VlcPlayerMac.h` too): the getters *can* block for
the duration of a concurrent `libvlc_media_player_stop()`. That is safe today only because
`-teardownPane:` re-points the active-pane aliases and pops the pane **before** handing the player to
its background stop, and `play()`'s internal `stop()` runs on the tick's own thread. **If either ever
moves off-main, this tick becomes blockable — start there.**

**Four things the risk review caught before any code was written:**
- **The bottom bar had NO relayout pass.** `_status.frame` was assigned exactly once and at the
  560 pt `contentMinSize` it is already flush with the meter button — *zero slack*. A fixed reserve
  would have silently truncated the status line (which carries the sticky grid-truncation notice) for
  **every** user, including live-TV users who never see the bar. Now `-layoutBottomBar`, driven by
  observing the **content view's** frame — deliberately not the video pane's resize notification,
  which also fires when the channel grid is toggled.
- **End-of-film, not Stop, is the unrecoverable case.** mac attaches no libVLC event callbacks, so
  `-tickStats` is the ONLY thing that can retire the bar; the update sits **above** the
  `!fs.playing` early return. Stop has an explicit reset site; end-of-film has none.
- **The gate would flap at 4 Hz on live HLS** (a mid-stream reopen briefly makes `get_length` −1).
  Hysteresis on hide (~2 s), immediate on show — each flip re-frames the status line and mac has no
  `DeferWindowPos` batch.
- **`.hidden = v` in `-applyVideoOnly`** would resurrect the bar on a live channel when *leaving*
  Video Only → two-condition hide. (`_meterBtn` was missing from that list entirely and only vanished
  by z-order — also fixed.)

**Win32's three known defects** are prevented with mac mechanisms, not a literal port: the post-seek
latch is scoped to a `_panesGeneration` counter bumped on every pane-set change, so pane A's seek
target can never drive pane B's thumb.

**Decisions already taken (owner):** **pause is IN**; **Video Only stays chrome-free** (watching a
film with seek is ⌃⌘F fullscreen, which keeps chrome) — Win32 parity, and it avoids new overlay logic.

**⚠ Expect the bar on LIVE TV, not just movies.** Win32 commit `3660441` corrected exactly this
assumption after finding real HLS DVR windows on the owner's provider reporting ~0:36–1:54 that
genuinely rewind. Gate on `isSeekable() && lengthMs() > 0` only — never on `Channel::Kind::Movie`.

**Known gap to decide during review:** there is still no way to pause from the **video right-click
menu**, and Video Only has no transport at all. Also flagged for `Win32/BACKLOG.md`:
`MainWindowChrome.cpp:266-273` hides Win32's seek bar + time label in fullscreen but omits
`btnSeekBack`/`btnSeekFwd`, then returns before the only place their visibility is set — reads as a
defect, unconfirmed visually.

### 🎬 Xtream VOD sync + Movies nav root ([PR #45](https://github.com/arcanii/RabbitEars/pull/45), merged @ `e96a86b`, 2026-08-09)

**The engine was free.** `common/core/XtreamClient.{h,cpp}`, `Json`, `UrlCanon` and the VOD DAO
(`listVodGroups` / `moviesByGroup` / `allMovies` / `retireMissingChannels`) had been compiled into
`RabbitEars.app` since the 0.2.16 merge with **zero callers**. The mac work is a GCD worker, a gate
and a nav section — **mac-only, zero `common/`/`Win32/` code**. Nobody should budget time for a JSON
parser or an Xtream client.

- **`mac/src/app/VodSync.{h,mm}`** — a faithful port of `Win32/ui/VodSync.cpp`: account probe before
  the big pull, two catalogue requests, both category guards, the trust guard, retirement. **It holds
  no Objective-C objects** (pure C++ + GCD + `std::function`), which is why it is deliberately ABSENT
  from the `-fobjc-arc` list in `mac/CMakeLists.txt` — keep it that way.
- **It opens its OWN `Database`**, breaking the app's "one connection, main thread only" rule on
  purpose: `retireMissingChannels` stages its keep-set in a per-CONNECTION temp table and is
  non-reentrant.
- **Movies nav** = a section head + indented categories in the flat `NSPopUpButton` (mac has no
  tree). The head loads **NOTHING** and prompts for a category — `allMovies()` is 40k+ rows and a nav
  click must never be that. Kind-scoped in SQL, so a VOD category cannot collide with a live group of
  the same name.

**TWO MAC-SPECIFIC GATE HOLES a straight port leaves open — do not regress these:**
- **`isEngaged()` must be DERIVED, never latched.** `VlcPlayerMac` attaches no libVLC events, so a
  flag set in `play()` has no clear site and a geo-blocked channel or a finished film would refuse
  every future sync forever (Win32 tried exactly that — the pane's `nowPlayingId` — and rejected it
  by name). It reads `libvlc_media_player_get_state()` plus a **3 s grace window that EXPIRES**,
  covering the open+buffer gap where the socket is claimed but the state is still `NothingSpecial`.
- **mac has no `dyingPanes`.** `-teardownPane:` and `stopRecordingAsync` hand a player to a
  background queue and return, after which nothing can ask it anything while its socket is open.
  Hence `vlcDetachedPlayerCount()` (atomic), consulted by the gate.

**⚠ THE MOST EXPENSIVE LESSON OF THE DAY — a mitigation that was worse than the bug.** The first
revision stood `-schedulerTick` down while a sync ran, to avoid contending with the worker's write
transaction. That deferral had **no bound**: a slow or multi-target sync can push a due recording
past its whole window, and `plan.miss` then marks it **`Missed`** — silently losing the recording the
guard existed to protect. The right rule is Win32's, and it is the opposite: **a due recording
outranks a catalogue refresh** → `cancelVodSync()` and proceed (`Win32/HANDOVER.md:573`; their
`onToggleRecord` and `onSchedulerTick` both cancel). The adversarial review caught it AND separately
*measured* the contention being guarded against at **~0.2 s, not 5 s**. Generalise: when a mitigation
inverts a priority, check the unbounded-wait case.

**VERIFIED END-TO-END AGAINST A PROVIDER — a first for either platform.** Windows shipped this
unverified (the owner's Xtream line expired before a real sync could run). A ~40-line python
`player_api.php` stub on `127.0.0.1:8899` + an isolated `RABBITEARS_DATA_DIR` DB, driven through the
real gear menu:

| Case | Result |
|---|---|
| First sync | `50 added or updated, 0 removed`; correct `movie/u/p/1001.mp4` URLs; **exactly 3 HTTP requests** (probe + 2 catalogue calls — not 1/category, not 1/film) |
| Movies nav | 🎬 root + `Action`/`Comedy` indented; live group `US\| NEWS` stays separate |
| **Retirement** (20 dropped at the provider) | `30 added, **20 removed**`; highest surviving id exactly the cutoff; **live rows untouched** (kind-scoped) |
| **Trust guard** (25 of 30 unusable) | `5 added, **0 removed**` + "titles that looked missing were kept"; all 30 films survived |

**Flagged to Win32** in `Win32/BACKLOG.md`: the shared root cause is that
`Database::updateScheduleStatus` returns **`void`**, so a contended write is undetectable at every
call site, while `planScheduler` derives "recorder busy" from the DB ROW STATUS. Windows is *more*
exposed — it ships wake-to-record. The fix (return `bool`, gate `activeScheduleId` on it) is theirs
to sequence.

**NOT in this release, deliberately:** resume/watched (the columns exist but there is **no DAO
setter on either platform**, and Windows hasn't shipped it — mac is level, not behind), and
`allMovies()` from the UI.

### ✅ Shipped in v0.2.12-mac — the Win32-gap parity batch (merged @ `223b055`, 2026-07-15/16)

A gap-scan (mac vs Win32) drove four parity features, each its own branch off `main`, each with an
**adversarial ObjC++ review** (the reviews caught real bugs — see the notes). All mac-only (**zero
`common/`/`Win32/`**), no version bump — they ride the **next** release. **All four merged to `main`**
(`gh pr merge --merge`, order #36→#39). #36/#37 were fast-forward-clean; **#38 and #39 both collided** with
#36's/#37's gear-menu + settings-load edits in `MainWindowController.mm` (all four features add a Channels ▸
gear item and an `init` settings-load line at the same spots) — resolved by merging `main` into each branch
and keeping every block (the merged gear submenu now reads Import/Export ▸ Categories ▸ Hide-unavailable ▸
Resume-last; all three keys `resume_last`/`category_filter`/`hide_dead` load), **CI green on both platforms
before each merge**. **On-device GUI verification is now DONE (2026-07-15)** — all four were driven via
computer-use against an **isolated seeded DB** (a fresh arm64 build at `main`, run as the *sole* instance
after quitting the installed app + moving the stale universal bundle aside, so the dual-instance/bundle-id
trap was neutralised): #36 auto-played the seeded last channel on launch (gear ▸ Channels ▸ ✓ Resume last
channel checked); #37's right-click video menu showed Video only / Fullscreen / Single / Split(2×2) / PiP,
and entering Video-only **held** the `PreventUserIdleDisplaySleep` assertion "RabbitEars full-screen
playback" (verified via `pmset -g assertions`) which **released** on exit (correctly scoped, no leak);
#38's Categories dialog listed all 3 fixture groups and filtering to one narrowed the grid 6→2 then reset
to 6; #39's dead row rendered grey (`tertiaryLabelColor`) and the toggle hid it (6→5) / restored it greyed
(5→6) with the ✓ tracking state. The merged gear submenu reads Import/Export ▸ Categories ▸ Hide-unavailable
▸ ✓ Resume-last — the exact conflict-resolution result, no missing/duplicated items. **This closes the open
follow-up; the batch is now GUI-verified and ready to ride the next release.**

| PR | Feature | Review caught |
|----|---------|---------------|
| [#36](https://github.com/arcanii/RabbitEars/pull/36) `mac-resume-last-channel` | Resume last channel on launch (auto-play, default on) + gear toggle | (clean; a headless "point the stream at a logging 127.0.0.1 server, watch the access log" trick verified auto-play fired iff `resume_last`) |
| [#37](https://github.com/arcanii/RabbitEars/pull/37) `mac-video-menu-screensaver` | Right-click video context menu (Video Only/Fullscreen/Single/Split/PiP) + **suspend screen saver** in fullscreen/video-only (a `PreventUserIdleDisplaySleep` IOPMAssertion) | clean |
| [#38](https://github.com/arcanii/RabbitEars/pull/38) `mac-categories-filter` | **Categories** multi-select include filter (new `CategoriesDialog` ARC sheet; `category_filter` key) | a stale-category ghost could silently discard a real filter → intersect the saved set with live groups in `init` |
| [#39](https://github.com/arcanii/RabbitEars/pull/39) `mac-hide-unavailable` | **Hide unavailable channels** (dead-status): `VlcPlayerMac::playState()` polled in `tickStats`, grey/hide dead rows, `hide_dead` toggle | a healthy stream hitting terminal `libvlc_Error` mid-playback could latch Dead+hidden → demote to Dead ONLY on a true OPEN failure (per-pane `everPlayed` gate) |

**✅ DONE (2026-07-16, [PR #40](https://github.com/arcanii/RabbitEars/pull/40), merged @ `8e18aad`): the
series-rule phantom-`Missed` shared-core fix — with a SCHEMA MIGRATION (v6→v7).** The slot key was the
PADDED start, so editing a rule's lead mid-recording orphaned the existing row and re-created the airing
(duplicate Pending → recorder busy → phantom Missed; also resurrected Cancelled tombstones and defeated the
two-rules→one-recording collapse). Fix: persist the programme's UNPADDED start as
`scheduled_recordings.prog_start_utc` (v7) and dedup rule rows on `(channel, progStartUtc)` — exact,
padding-proof. The adversarial review REFUTED the first (heuristic window-containment) design with a decisive
counterexample — a padded window also contains *adjacent* airings when trail ≥ the next airing's duration —
and the empirical migration test caught the `migrate()` early-exit gate still at `v>=6` (the v7 step never
ran on a real DB until bumped). Pre-v7 rows use a title-scoped containment fallback (age out in ≤14 days);
manual rows unchanged; the mac rule editor now clamps lead/trail 0..240 min (Win32 `readMinutes` parity).
Regression tests in `RabbitEarsCli --selftest` ("Padding-proof dedup (v7)"); core-selftest green on both
platforms; **Win32 team flagged in `Win32/BACKLOG.md` (schema-v7 note)**. Rides the next release.

**✅ DONE (2026-07-16, [PR #41](https://github.com/arcanii/RabbitEars/pull/41), merged @ `6d90de6`): the
Xtream group-title→country fallback — the second and last shared-core P2 fix.** Xtream-panel playlists
(opaque/empty tvg-id, country in the group-title prefix: `US| NEWS`, `[UK] SPORTS`, `FR - CINEMA`) never
appeared in the Countries filter on either platform. New conservative `countryFromGroupTitle` fallback in
`common/db/Database.cpp` — 2 ASCII letters + an EXPLICIT delimiter (a bare space never counts: `IT MOVIES`
≠ Italy), deny-list `HD SD TV EN XX EX ON`, tvg-id authoritative — running as a **registered deterministic
SQLite scalar** `effective_country(tvg_id, group_title)` so both `listCountries` and `channelsByCountry`
filter server-side with one shared rule. The adversarial review earned its keep twice more: the integration
lens **benchmarked my first C++-side filter at ~30 ms/call and traced it onto the mac per-keystroke search
path** (→ the scalar), and the parsing lens ran an executed input corpus that added `EX`/`ON` to the
deny-list (`EX-YU|`, `ON-DEMAND`) and documented the unfixable `AR|`-Arabic→Argentina collision as
deliberately-kept. No schema change. Twin selftests extended to 17 country assertions (mac twin verified
locally against real sqlite, suite exit 0; the Windows CLI twin ran green in CI). **Win32 flagged in
`Win32/BACKLOG.md`** (their Countries nav gains Xtream entries; alpha-3 alias table + ISO whitelist noted
as opt-in follow-ups). Rides the next release.

**✅ SHIPPED in v0.2.15-mac — [PR #42](https://github.com/arcanii/RabbitEars/pull/42) `mac-logo-thumbnails` (channel-logo thumbnails, mac-only, merged @ `7ce0947`):** a logo column (`NSImageCell`) between `#` and `Channel`, fed by a new **`LogoLoader`** (ARC, app-lifetime) — main-thread `-imageForURL:` returns a cached thumbnail or nil + kicks off an async load (memory `NSCache` → disk cache `<dataDir>/logos` honouring `RABBITEARS_DATA_DIR`, storing the ~96px PNG THUMBNAIL → a streamed `NSURLSessionDataDelegate` download capped at 3 MB), then hops to main to coalesce a reload of the VISIBLE logo cells; keyed by URL (cell-reuse-safe). Logo-less / failed / cleartext-http (ATS-blocked) rows show a faint placeholder. `tvg-logo` was already parsed+stored → **mac-only, zero `common/`/`Win32/`**. Bomb-safe decode (ImageIO `CGImageSource` reads pixel dims from the header, rejects >2048px WITHOUT rasterizing, then downsamples). **Two review passes + on-device BOTH caught real issues:** on-device caught an MRC crash (`_logoPlaceholder` stored an autoreleased NSImage without `retain` → over-release → `objc_retain` EXC_BAD_ACCESS; fixed + the tint moved off the undeclared `-imageWithTintColor:` to a source-atop fill); the first find→verify workflow found 2 medium hardening findings (points-vs-pixels bomb bypass + unbounded chunked buffering → the streamed-cap + ImageIO rewrite); the focused re-review (`wve3zwr2h`) found 3 more (all LOW) — 2 fixed (`35e5210`: cache the thumbnail not the raw ≤3 MB source; decode/write OFF the serial delegate queue), the 2048px cap kept as intentional bomb-safety. **CI FULLY GREEN; on-device VERIFIED (5 https logos render as thumbnails, placeholders elsewhere, no crash, cache = PNG thumbnails; bomb rejection empirically proven). Merged @ `7ce0947` + shipped in v0.2.15-mac (build 318, 2026-07-17).** Follow-up flagged in the PR: the app has NO ATS exceptions, so cleartext-http logos (and http m3u/epg) don't load — a pre-existing app-wide policy, a separate decision.

## The gap to Windows 0.2.17, after today (re-derived by a 13-agent gap-scan, 2026-08-09)

A gap-scan verified **123 candidate items against real mac source**: 44 were "already compiled into
the mac binary, no caller" (the cheap wins), 31 missing, 27 already present, 12 partial, 9 N/A. Most
of the actionable set is now closed. **What remains, deduplicated:**

**🥇 THE ONE REAL GAP — the seek layer. ⏳ NOW BUILT ON `mac-seek-scrubbar`, awaiting review +
on-device verification (see its section above). The description below is what it was.** `VlcPlayerMac` has **no `timeMs` / `lengthMs` /
`isSeekable` / `seekTo` / `videoSize` at all**, and nothing shared exists (Win32's seek layer is UI
code, not `common/`). It is a pure native port, and it is what makes the VOD films that now sync
actually *watchable*. The scrub bar + elapsed/total readout, skip ±10 s, and PiP aspect-ratio snap
all hang off it. **M–L.** Traps to take from Win32 rather than rediscover: publish via relaxed
atomics sampled OFF the 250 ms main-thread poll (blocking libVLC getters there stall the UI on a
wedged feed); read seekability from libVLC **every tick**, never inferred from the URL (catch-up
feeds are live URLs that *are* seekable); carry a seek generation counter so a stale sample cannot
clobber a user seek; commit on drag **release** only.

**Then the tail (all small):** the background **dead-link sweep** (the dangerous half — `classifyProbe`,
`sweepIsTrustworthy` — is already compiled in and selftested; mac needs only `httpProbe` on
`NSURLSession` + a worker, and it is now unblocked because 0.2.16 landed the ATS exception — without
it every cleartext probe fails at the transport layer and the sweep silently discards itself);
**Settings ▸ System** (log level — `diag::setLevel` is header-only and already linked; **skip the beta
switchboard**, `FeatureFlags.h` declares an enum with no enumerators so it renders an empty box);
**PiP right-click menu + PiP⇄main swap + always-on-top**; and the **appcast host move** off
`raw.githubusercontent.com` — the last survivor of the original Win32-gap backlog.

**Cosmetics, LOW value:** meter glass (`common/ui/GlassMask`) + the VU-needle look — both blocked on
the *same* prerequisite, since `MeterView.mm` draws immediate-mode into `-drawRect:` with no pixel
buffer for a per-pixel LUT, so do them as ONE `CGBitmapContext` restructure or neither; look-aware
knobs + per-look labelling; tip buttons (needs a CUSTOM About window — mac uses the system panel,
which cannot host buttons, and `NSHumanReadableCopyright` is where the GPL-3.0 notice lives, so that
text is a licence obligation to carry over).

**mac is NOT behind here:** resume/watched (columns exist, **no DAO setter on either platform**),
series → seasons → episodes (unbuilt on both — and mac is *ahead* on the groundwork, since
`LogoLoader` is already the async, disk-cached, bomb-safe poster loader that work needs).

**Declined outright:** the data-flow tank buffer meter (a ~1000-line Navier-Stokes port for a
decorative widget whose input probably reads zero on HLS), transcoding on record (unshipped on
Windows too), `--benchdb` as a separate mac binary. Explicitly N/A: the theme engine, wake-to-record
(a non-root app cannot arm a wake), transcoding + JSON profiles, `E3` MeterModel promotion.

**Older P3/parked tail:** now/next readout, EPG genre tags, locale schedule dates, layout "reset to
default", in-app Licenses viewer, the 250 ms bg-pane audio bleed, 3 MRC dialog leaks (incl. the
pre-existing `-showSettings:` gear-menu tree leak — needs on-device verification before touching, a
wrong call there is an over-release crash, not a leak), meter fine-tuning, Intel-slice QA (largely
moot — macOS 26 is Apple-Silicon-only).

**Latest: `v0.2.11-mac`** (build 276, universal, notarized, appcast live @ `437ed49`) — an **i18n-polish release**:
PR #34 (dead-catalog-id prune: `LangRestart*` + 2 unused mac ids → 531→525 keys) + PR #35 (an AI-assisted,
adversarially-verified **CJK translation-quality pass**: 36 verified consistency fixes across 日本語 / 繁體中文 /
香港 — no mistranslations, all terminology / native-punctuation / regional word-choice polish; e.g. JA 復帰→
スリープ解除, zh-Hant 儀表→量表 + Taiwan-form 訊號/影格, zh-HK 影集→劇集 overrides). **14 changed ids are
Windows-facing** → flagged to the Win32 team in `Win32/BACKLOG.md`. Version bumped 0.2.10→0.2.11 (APPLE
override). Shipped via the standard universal recipe (universal VLC → `package-mac.sh --sign --vlc <universal>`
→ hdiutil dmg → notarize → staple → `sign_update` → `gh release create` → appcast via `gh api PUT`); verified
end-to-end (downloaded asset sha256 byte-identical to the signed dmg, edSignature valid, spctl "Notarized
Developer ID", sparkle:version 276 > 269). Both PRs merged with plain `gh pr merge --merge` (works on
agent-authored PRs — only `--admin` is auto-mode-blocked). The human native CJK review (testers) is still the
gate before *advertising* CJK; this pass shipped the machine-error fixes it surfaced.

**Before it: `v0.2.10-mac`** (build 269, universal, notarized, appcast live @ `0e961bd`) — **LIVE LANGUAGE SWITCH**
(parity with Windows 0.2.11). Settings ▸ Language (the App-menu submenu **or** the gear ▸ 言語) now applies
**live — no restart** (was restart-to-apply). `AppDelegate -selectLanguage:` flips `i18n::setActiveLang(...)`
then rebuilds every built-once surface: a new **`MainWindowController -applyLanguageLive`** relabels the window
title, top-bar buttons (Add Playlist / gear / Stop / record / meter — 3 new UNRETAINED ivars `_addBtn`/`_setBtn`/
`_stopBtn` since they were setup-locals), search placeholder, grid column headers, the row context menu (rebuilt
wholesale), the empty-pane hint, the filter popup (**selection preserved**), and the status line — then fans out
to the 4 `MeterView` + the two reused modeless windows via a new **`-relabelForLanguageChange`** on each (RELABEL
IN PLACE — nil-and-rebuild would dangle their self-referencing dataSource/delegate/target back-refs). The menu bar
is rebuilt via `-buildMenu` (also moves the ✓); the gear pull-down + all NSAlerts are built on-open so they
auto-localize; no font work (the mac system font cascades to CJK). Removed the restart TaskDialog + `-relaunch`.
**ZERO `common/`/`Win32/` changes.** Built by an **inventory workflow** (mapped every relabel surface) + an
**adversarial review workflow** (MRC-memory/completeness/behavior lenses, each finding independently verified),
which **caught a real MRC use-after-free**: the filter-selection preserve read a BARE pointer to a menu item's
`representedObject`, freed by `rebuildFilterMenu`'s `removeAllItems` before the `isEqualToString:` compare
(reproduced via NSZombie) — fixed with `[[rep retain] autorelease]` in `applyLanguageLive` **and** the identical
**pre-existing shipped bug in `reloadAfterPlaylistChange`** (same bare-pointer-into-freed-collection class as the
0.2.9 `recordingPathFor:` crash — a recurring MRC trap here); also fixed a now-recurring `buildMenu` leak
(autoreleased the ~11 menu allocs) + a TvGuide "(no title)" staleness. **On-device verified:** en → 日本語 → 繁體中文
switched **live** via both entry points, every surface re-rendered, the active "Parity" group filter was preserved
across switches, no restart, no crash. **Before it: `v0.2.9-mac`** (build 261, appcast @ `cb14d56`) — **Windows-0.2.9 parity**:
a **recording-rule editor** (New…/Edit… + double-click in the Recordings window's *Series Rules* tab — channel-or-
`(any channel)`, title, Exact/Contains, lead/trail minutes; OK gated on a non-empty title; New→`addRule`,
Edit→`updateRule`+`clearPendingForRule`+re-expand — in `RecordingsWindowController`, **zero new catalog strings**,
reuses the shared Win32 rule ids); **series-rule episode dedup** (the shared schema-v6 `episode_key` +
`episodeKey`/`expandRules` were already compiled in — the mac fix is the 0.2.9 **pre-filter**: restrict the
channel-blind expander to *recordable* programmes BEFORE dedup, normalised @feed-safe, in
`-expandRecordingRules:`); **Traditional Chinese** zh-Hant + zh-HK in the Language selector + `Tr.h` routing
(Simplified/bare-`zh` fall through to English); and the **GPL-3.0** LICENSE + THIRD-PARTY-NOTICES.txt + `licenses/`
bundled into `Contents/Resources` (via `mac/CMakeLists.txt`; deep-codesign covers them) + an About-box copyright
line (`NSHumanReadableCopyright`). **ZERO `common/`/`Win32/` source changes** (PR #31). **Two fixes rode along:**
(1) **a shipped-since-0.2.7 CRASH** — `recordingPathFor:` cached its filename-scrub `NSCharacterSet` in a `static`
from the AUTORELEASED `characterSetWithCharactersInString:` without `retain` (MRC), so the **2nd** recording of a
session (a 2nd manual record, or the scheduler tick) messaged a freed object → `EXC_BAD_ACCESS`; caught by the
0.2.9 on-device pass, fixed with a retain (`474004d`); (2) **a shared-catalog `\r\n` bug** (PR #32, `common/i18n`)
— 27 strings were double-escaped (`\\r\\n`) so both platforms rendered a literal `\r\n`; collapsed to real
newlines + regenerated `Strings.cpp`. **On-device VERIFIED** (isolated `RABBITEARS_DATA_DIR` + a `127.0.0.1`
m3u/XMLTV fixture of public HLS streams): playback, manual recording→valid h264+aac `.ts`, rule editor CRUD +
validation + field round-trip, episode dedup (3 airings→2 schedules), the scheduler (tick→Recording→auto-Done,
playable `.ts`, no crash), and 繁體中文 rendering + selector. Adversarial ObjC++/ARC + logic reviews: 0 code
defects (1 low zh-Hans-region routing edge fixed). ⚠ **Trap:** the installed app + a dev build share the bundle id,
so a dual-instance screen composites both windows — clicks are unsafe for real data; verify the running PID's DB
via `lsof`/`ps eww` and prefer a HEADLESS scheduler re-test (sqlite-arm a schedule + monitor the log/DB).
**Before it: `v0.2.8-mac`** (build 248, universal, notarized, appcast @ `03048ec`) — **localization
(English + 日本語)** over the shared `common/i18n` catalog (a `Tr`/`TrF` AppKit layer = peer of `Win32/ui/Tr.h`;
Language selector System/English/日本語 + restart-to-apply; ~290 UI strings wrapped; **+145 mac-only ids** incl.
machine-draft JA + zh-Hant), and the **gear menu regrouped to match Win32** (Channels/Recording/View/Layout/
Language submenus). PR #30, on-device verified (switch→restart→JA across menus/dialogs/About/Terms; one meter-
label overflow found + fixed). **The mac branch now merges the shared catalog to `main`** — additive +
generator-validated (531 ids × 4 langs: en/ja/zh-Hant/zh-HK; zh-HK inherits zh-Hant via `base`; the mac-only
Chinese is never displayed — the mac selector offers only System/English/日本語 — it exists for catalog
completeness). See `mac/src/app/Tr.h`. **Before it: `v0.2.7-mac`** (build 234) — the 0.2.6/0.2.7 parity stack
(#25→#29) + the PiP-switch fix. Note recording/scheduler shipped **without**
on-device verification. The prior 0.2.0 milestone (still accurate for the multi-view/EPG internals) follows.

The mac app is **shipped and auto-updating**: **`v0.2.0-mac`** on GitHub — universal (arm64 + x86_64),
notarized, self-contained, `0.2.0` build `208`. It lands the three Windows-parity features — **TV Guide (EPG)**,
**multi-view Split/2×2** and **Picture-in-Picture** — plus the unified app icon.
**Key discovery: this was wiring, not porting** — every shared core it needed (`VideoGrid`, `XmltvParser`,
`Gzip`, `Programme`, the `Database` EPG methods) **already compiled into the mac binary**; PR #24 touched
**no `common/`, `Win32/` or `third_party/` file at all**.
The 0.2.0 release was verified end-to-end before the appcast went live: `spctl` → "Notarized Developer ID",
staple validates, the `edSignature` verifies against the **downloaded** GitHub asset under the key that matches
the app's embedded `SUPublicEDKey`, `sparkle:version` 208 > 172, `length` byte-exact, and the launched bundle's
banner read `0.2.0 (208)`.

Older: **`v0.1.10-mac`** — universal (arm64 + x86_64),
notarized, self-contained. **App minimum is macOS 26** ("latest is best"; `LSMinimumSystemVersion` only,
deployment target unpinned so CI's older SDK still builds — note macOS 26 is Apple-Silicon-only, so the
x86_64 slice is effectively dead weight but shipped for parity). The Sparkle path is **proven end-to-end**
(0.1.7→0.1.8→0.1.9→0.1.10 auto-updates confirmed in the wild; the one historical snag was an XML `--` in
an appcast comment — **always `xmllint` the appcast before publishing**).

The app **plays IPTV** via libVLC in a native window:
- **rich channel grid** — ★ / # / name / group columns, live **search**, filter popup
  (All / ★ Favourites / groups / **countries**), a **Categories** multi-select include filter
  (Settings ⚙ ▸ Channels ▸ Categories…, persisted `category_filter`), **favourite** toggle + **LCN edit** (row menu),
  **resume-last-channel** (auto-plays the last-watched channel on launch — Settings ⚙ ▸ Channels ▸
  "Resume last channel", default on; toggle off for highlight-only); single click selects,
  **double-click / Return plays**;
- **Terms-of-Use gate** on first launch + after any version change (see below);
- **playlist management** — Settings ▸ Manage Playlists… (enable/disable/rename/refresh/delete);
- **audio/stream meters** — 4 kinds × 4 styles + a config dialog (see below);
- **top bar** — accent **`+ Add Playlist`** + a **⚙ gear** (Open File / Manage Playlists / Meters /
  Updates / About) + search + filter; plus a full **menu bar** (App / File / Edit / View);
- **split view** (grid | video) that fills correctly + **remembers window size/position**;
- **volume + mute**, native **fullscreen** (⌃⌘F) + **Video Only** (⌥⌘F), a **custom About**;
- **right-click the video** for a context menu (Video Only / Fullscreen / Single / Split / PiP — Win32 parity);
  the **screen saver / display sleep is suspended** while in full screen or Video Only (IOPMAssertion);
- **Sparkle auto-update**; **self-contained** (`scripts/package-mac.sh` bundles libvlc + ~343 plugins,
  so it runs with **no VLC.app installed**); an app icon; CI on both platforms.

The mac `.mm` are ObjC++ written **MRC-style** (ARC off target-wide; app-lifetime objects leak benignly).
`-fobjc-arc` is enabled **per-file** only where needed: `MeterView.mm`, `MetersDialog.mm`, `SpectrumTap.mm`,
`PlaylistsDialog.mm`, `TermsDialog.mm` (weak-self timers, blocks, an RT latch, block-captured self). The
shared core is portable C++ whose headers carry `#if defined(_WIN32)` branches.

## Build & run

```sh
scripts/build-mac.sh                 # shared core + self-test (no external deps)
scripts/build-mac.sh --app           # + RabbitEars.app  (needs VLC.app for libVLC)
open build-mac/mac/RabbitEars.app
build-mac/mac/RabbitEarsPlayProbe    # headless libVLC smoke test (exit 0 = Playing)
```
`Mac.cmake` auto‑detects VLC.app (or `-DLIBVLC_MAC_PREFIX=<dir>`) and downloads Sparkle. Unsigned dev
builds trip Gatekeeper — right‑click → Open, or `xattr -dr com.apple.quarantine build-mac/mac/RabbitEars.app`.

## Terms-of-Use gate (SHIPPED in v0.1.10)

`TermsDialog.{h,mm}` (ARC) — a modal shown once on **first launch and after any version change**, mirroring
the Win32 `showTerms` gate with the **verbatim terms text**. In `MainWindowController -showWindow`, right
after the DB opens: if `tos_accepted` ≠ the current full version (`RE_VERSION_FULL_W`, marketing.build) →
run the modal (via `-initWithVersion:`, which shows the version in the header). **Accept** persists the
version + continues; **Decline** quits. Every other launch is silent. Same `tos_accepted` key + full-version
scheme as Win32. On-device-validated. **To re-trigger for testing:**
`sqlite3 "$HOME/Library/Application Support/RabbitEars/rabbitears.db" "DELETE FROM settings WHERE key='tos_accepted'"` then relaunch.

## TV Guide / EPG (branch `mac-multiview-tvguide`, 0.2.0)

The whole data pipeline was **already compiled in** — this is wiring, not porting. **No `common/` edits.**
- **Refresh** — `MainWindowController -refreshGuide:` (View ▸ Refresh Guide, or the ⚙ menu): for every
  *enabled* playlist with a non-empty `epgUrl`, off the main queue → `httpGet(url,…,60000)` →
  `gunzipIfNeeded` (XMLTV is usually served as `.xml.gz`) → `parseXmltv(...).programmes` → back on the main
  queue → `Database::bulkInsertProgrammes`. Newest-refresh-wins via `_epgToken`. **DB is main-thread only.**
- **Guide window** — `TvGuideWindowController` (own modeless `NSWindow`) assembles rows from
  `programmesInWindow(pid, now-6h, now+72h)`, grouping consecutive same-`channelId` programmes and joining to
  channels by the **lowercased base tvg-id** (`normId` strips iptv-org's `@feed` suffix: `CNN.us@SD` → `cnn.us`).
  Channels absent from the playlist are skipped, so **every row is playable**. `EpgGuideView` is a *flipped*
  custom `NSView` drawing 4 clipped regions (programme blocks + now-line, frozen channel column, frozen hour
  axis, corner) with its own scroll offsets — the AppKit peer of Win32's Direct2D three-clip control.
  Clicking a programme opens details + **Play Channel** (`channelByTvgId` → play → hide the guide).
- **Guide URL** — parsed from the M3U `x-tvg-url`, or set per playlist via **Manage Playlists ▸ 📅**
  (`setPlaylistEpgUrl`). *(A real bug fixed here: `importDoc` used to drop `doc.epgUrl`, so Refresh Guide
  had nothing to fetch.)*
- **Gotchas:** hour-axis ticks are aligned to **local** hour boundaries via `NSCalendar` (raw UTC stepping
  mislabels fractional-hour zones like UTC+5:30). All programme text goes through the `Encoding.h` UTF seam —
  **never** the naive `widen`/`narrow` byte-cast (titles are routinely non-ASCII).

## Multi-view — Split / 2×2 + Picture-in-Picture (branch `mac-multiview-tvguide`, 0.2.0)

- **`VlcEngineMac`** owns the ONE `libvlc_instance_t` (`libvlc_new` loads ~325 plugins — once per process).
  Every `VlcPlayerMac` borrows it via `init(engine)`; an Nth pane is now cheap. **Destroy all players before
  the engine.**
- **Pane model** — `MacVideoPane { NSView* view; unique_ptr<VlcPlayerMac> player; Channel channel; long long
  channelId; }` in `_panes`. `_player` / `_videoView` are **raw ALIASES to the active pane**, so all the
  pre-existing playback/meter/stats code kept working untouched. Single view = exactly one pane.
- **Layout** — `-applyVideoPaneLayout` feeds the shared `common/ui/VideoGrid::computeVideoPanes` and
  **y-flips** each box (`VideoGrid` is top-down; AppKit is bottom-up): `y = ich - b.y - b.h`, flipping against
  the *same integer* height passed in. Split(4) ⇒ 2×2; Pip(2) ⇒ full backdrop + draggable bottom-right inset.
- **Single-audio active pane** — only the active pane is audible; background panes are muted by
  **audio-track deselect** (`libvlc_audio_set_track(mp,-1)`), NOT volume=0 (libVLC resets volume to 100% when
  it recreates the aout on an HLS quality switch). `tickStats` **re-asserts** the mute every 250ms because the
  track may not exist yet at `play()` time. Clicking a pane activates it (accent `CALayer` border).
- **Async teardown (do NOT regress this)** — `stop()` is synchronous and blocks on a stuck stream. On collapse,
  panes are torn down on a GCD background queue. The pane's **`NSView` is retained across the async stop and
  released back on the main thread**: libVLC holds it via `set_nsobject` and its vout renders into it until the
  player is released. `applyViewMode` also re-points `_player`/`_videoView` at a **surviving** pane *before*
  any teardown. Collapsing carries the active stream into pane 0 (`carryStreamFromPane`, skipped when it's
  already the same channel); `Stop` clears `channelId` so a stopped stream is not resurrected by that carry.
- **Triggers** — View ▸ Single (⌃⌘1) / Split 2×2 (⌃⌘2) / Picture-in-Picture (⌃⌘3), with checkmarks; row
  context menu ▸ **Play in PiP** (plays the inset, backdrop stays active + audible — Win32 parity).
- **mac is SIMPLER than Win32 here:** no vout-host pool. That whole Win32 apparatus exists only to dodge a
  Direct3D11 "VLC (Direct3D11 output)" popout; AVFoundation composites sibling `NSView`s fine (validated with
  4 live streams). Persisting view mode is deliberately NOT done (Win32 doesn't either).

## Meters — full Win32-parity meter system (SHIPPED in v0.1.9)

**KEY: only Spectrum needs audio capture; Signal/Bitrate/Frames run off `FlowStats` — no consent, no desync.**
(History: an earlier libVLC-3.x `libvlc_audio_set_callbacks` tap took over output + desynced — **do NOT
revisit it**; a Core Audio process tap that shipped in 0.1.8 was removed because denied consent delivered
undetectable silence, then rebuilt properly with a `hasAudioTrack`-gated placeholder.)
- **`common/models/FlowStats.h`** — shared stream-health snapshot (both `VlcPlayerMac::sampleStats()` + Win32).
- **`MeterModel.{h,cpp}`** (`rabbitears::mac`, **MAC-LOCAL**) — `MeterKind`/`MeterStyle`/`MeterPalette`(7
  `SkinColor` roles)/`MeterTuning`(5 knobs)/`MeterConfig` + UTF-8 codecs. Kept out of `common/` + the Windows
  binary until the Win32 team reviews it — **that promotion (→ `common/ui` under a neutral `rabbitears::meter`
  ns) is the deferred E3**; the owner chose to leave it mac-local and let the Win32 team drive E3.
- **`MeterView.{h,mm}`** (ARC) — ONE view renders any kind × style from a `MeterConfig`. All four styles are
  real: **LED** (dot-matrix), **LCD** (ghosted), **Tube** (translucent-halo glow, sized by the glow knob),
  **Scope** (a phosphor `NSBezierPath` trace + `NSShadow` bloom). The 5 tuning knobs are wired (sensitivity→
  gain, smoothing→easing, peakHold→spectrum peak decay, breathing→bitrate ceiling ebb, glow→Tube/Scope bloom;
  centred so 0.5 == the pre-tuning behaviour). Spectrum folds in the RT-thread `os_unfair_lock` latch + energy
  probe + "grant permission" placeholder.
- **`SpectrumTap.{h,mm}`** (ARC) — the process tap + **vDSP FFT** → 24 log bands (preallocated, no RT alloc).
  Opt-in: creating it triggers the one-time consent prompt.
- **`MetersDialog.{h,mm}`** (ARC) — Settings ▸ Meters…: per-kind **Show + Style + 7 colour wells + 5 tuning
  sliders + a live preview** (a real `MeterView` fed synthetic data by a weak-self timer, updated as any
  control changes). Persists `meter_<kind>` / `_style` / `_colors` / `_tuning`; `loadMeterConfig` reads them back.
- **MainWindowController** glue: a **`DraggableMeterBar`** floats the meters over full-bleed video
  (`meter_pos_x/y`); a bottom-bar show/hide button (`meters_hidden`); **Video Only** (⌥⌘F/Esc/dbl-click).

**⚠ The Spectrum meter needs the `com.apple.security.device.audio-input` entitlement.** It taps the app's own
audio output with `AudioHardwareCreateProcessTap`, and the **hardened runtime silently blocks audio capture
without it**: `AudioHardwareCreateProcessTap`, the aggregate device and the IOProc all return success, the tap
just delivers **zeros**, so the meter sits flat and *nothing* appears in the log. That entitlement is now in
`mac/packaging/RabbitEars.entitlements` (which `scripts/package-mac.sh` passes to `codesign`), alongside the
`NSAudioCaptureUsageDescription` Info.plist key that supplies the consent prompt's text.
**Consequence for testing:** a plain `scripts/build-mac.sh --app` dev build is *ad-hoc, linker-signed* with no
entitlements, so macOS never even prompts and the Spectrum meter is dead. To test it you must sign the dev
bundle first:
```sh
codesign --force --deep --options runtime --timestamp=none \
    --entitlements mac/packaging/RabbitEars.entitlements \
    -s "Developer ID Application: Matthew Mark (386M76FV3K)" build-mac/mac/RabbitEars.app
open build-mac/mac/RabbitEars.app     # now macOS prompts for audio recording; grant it
```
Signal/Bitrate/Frames never need any of this — they run off `FlowStats`.

**The "grant permission" placeholder** (`updateSpectrumAvailability:` → `MeterView -drawUnavailable:`) used to
be unreachable: it required **32 _consecutive_** audible-but-tap-silent 250 ms polls, and the counter was zeroed
both by `tickStats` whenever `libvlc_media_player_is_playing()` momentarily dipped false and by the
`demuxBytesPerSec == 0` branch — both happen routinely at HLS segment boundaries, so on a real stream it never
reached the threshold and a denied tap just looked like a dead meter. It now **accumulates** audible, tap-silent
polls (never reset by a transient dip) and trips after ~10 s of genuinely audible playback with zero tap energy;
`_spectrumEverHadEnergy` still latches "granted" so a quiet passage can't false-flag. The message was also wider
(43 chars @ 9 pt) than the 180 pt strip — it is now a short auto-shrinking *"Spectrum needs audio permission"*
with the full instruction in the view's tooltip. If the tap is hard-denied, `AudioDeviceStart` fails outright
(`SpectrumTap` init returns nil) and `startSpectrumTap` shows the placeholder immediately.

**Backlog (not blocking):** on-device fine-tuning of the Tube glow radius / Scope trace weight / knob curves
(built blind, ship-quality per owner) — tweak the constants in `fillCell`/`strokeScope`. And **E3** (the
`MeterModel` promotion, owned by the Win32 team). Also: `startSpectrumTap` runs inside `-showWindow` and
`AudioHardwareCreateProcessTap` **blocks on the consent prompt**, so on first launch the main window doesn't
appear until the user answers — worth deferring the tap off the critical path.

**Open low-severity findings (0.2.0 adversarial review, deliberately NOT fixed before the release cut** —
each needs on-device verification that wasn't available at the time, and none is user-visible enough to
justify an untested change to shipped audio/memory paths):
- **MRC dialog leaks.** `addPlaylist:` (~L714), `showSettings:` (~L1223) and `editChannelNumber:` (~L1457) in
  `MainWindowController.mm` `alloc` an `NSAlert`/`NSTextField`/`NSMenu` with `+1` and never release it. This
  is an MRC file, so each invocation leaks. The fix is an `autorelease` on each — but it depends on AppKit
  retaining the alert for the sheet's lifetime and the block retaining the accessory view, so **verify on
  device before shipping it**; a wrong call here is an over-release crash, not a leak.
- **Transient background-pane audio bleed (~250 ms).** `playChannel:intoPane:` calls `setMuted(true)`
  immediately, but at that moment no audio track exists yet, so `setMuted` sees `cur == -1` ("already
  silent") and no-ops. libVLC then auto-selects the track when it appears and the background pane is briefly
  audible until the next `tickStats` re-assert. A real fix needs a libVLC event callback
  (`libvlc_MediaPlayerESSelected`) rather than the 250 ms poll.

## Playlists — enable / disable / rename / refresh / delete (SHIPPED in v0.1.9)

A **Settings ▸ Manage Playlists…** sheet (`PlaylistsDialog`, ARC): per-playlist **Enabled** checkbox +
**⟳ Refresh** / **✎ Rename** icon buttons + a **Delete** button (confirmed). Live-applies + refreshes the grid.
- **Data layer (shared `common/db/Database`)** — the repo's **first schema migration** pattern:
  `Database::migrate()` is `user_version`-gated (`if (v < N)` steps). It added `playlists.enabled` (v1→2,
  idempotent via `hasColumn`, backfills existing rows to 1). *The Windows team has since extended `migrate()`
  through v3/v4 for EPG/recordings — the mac app runs the full chain and it's verified clean on a real DB.*
  `setPlaylistEnabled()`/`renamePlaylist()`; disabled playlists are hidden from every cross-playlist query via
  a shared `kEnabledOnly` predicate (`allChannels`/`favourites`/groups/countries/search/`channelByLcn`);
  `channelsByPlaylist()` stays literal. **Windows-safe** (default-enabled, no Win32 disable UI).
- **Refresh / friendly names** — Refresh re-downloads (URL) / re-reads (file) off the main queue + upserts via
  `bulkInsertChannels` (favourites + LCNs kept; does **not** prune removed channels), weak-self. Imports derive
  a **friendly name** (`friendlyName()` — stem/host, not the raw URL); the full URL/path stays as `source`.
- **Controller glue** — `reloadAfterPlaylistChange` re-points the active playlist if you disable/delete the
  current one + preserves the grid filter. The list uses a flipped `RETopClipView` so a short list top-anchors.

## Menu bar + gear (SHIPPED in v0.1.9)

Import/management/meter commands live in the macOS **menu bar** (a **File** menu: Add Playlist ⌘N / Open
Playlist File ⌘O / Manage Playlists…; **View ▸ Meters…**) as well as the in-window **⚙ gear**. `AppDelegate`
builds the menu bar; items target the app delegate and forward to `MainWindowController` (whose action
selectors are in the header), matching the View-toggle pattern (the controller isn't in the responder chain).

## Releasing (v0.1.10-mac shipped this way — full recipe in the `mac-release-deployment` memory)

**Version:** decoupled per-platform in `cmake/AppVersion.cmake` — Windows `APP_VERSION 0.2.4`, mac overridden
to **`0.1.10`** under `if(APPLE)`. Feeds both `CFBundleShortVersionString` + the generated `version.h`.
**Bump the mac line there** for a mac release; Windows is unaffected.

Deployed like the sibling **SQLTerminal**, **reusing the family credentials**: Developer ID **`386M76FV3K`**
signs, notarize via the **`SQLTerminal-notarize`** keychain profile, Sparkle EdDSA key in the login keychain
under account **`SQLTerminal`**.

```sh
# 1. universal build — a stock VLC.app is single-arch; get vlc-3.0.23-universal.dmg from videolan
#    (https://get.videolan.org/vlc/3.0.23/macosx/vlc-3.0.23-universal.dmg), mount, copy VLC.app out
scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DLIBVLC_MAC_PREFIX="<universal VLC.app>/Contents/MacOS"
# 2. bundle libVLC + sign inside-out (app, plugins, AND Sparkle's Updater.app/Autoupdate/XPC)
#    --entitlements now DEFAULTS to mac/packaging/RabbitEars.entitlements whenever --sign is given, and the
#    script refuses to sign a hardened binary whose entitlements lack audio-input. (It used to sign a
#    hardened build with an EMPTY entitlement set if you forgot the flag — which silently kills Spectrum.)
scripts/package-mac.sh <app> --vlc "<VLC.app>" \
    --sign "Developer ID Application: Matthew Mark (386M76FV3K)"
# 3. dmg → notarize → staple → Sparkle-sign
create-dmg … <dmg> <app>
xcrun notarytool submit <dmg> --keychain-profile SQLTerminal-notarize --wait
xcrun stapler staple <dmg>
build-mac*/sparkle/bin/sign_update --account SQLTerminal <dmg>   # prints edSignature + length
# 4. gh release create v<ver>-mac <dmg> --target main --latest=false   (keep Windows as "Latest")
#    then add the <item> to mac/packaging/appcast-mac.xml (sparkle:version = CFBundleVersion) ON MAIN.
#    xmllint FIRST. The feed serves from main; git push HANGS intermittently, so land the appcast via:
#    gh api --method PUT repos/OWNER/REPO/contents/mac/packaging/appcast-mac.xml \
#        -f message=… -f content=<base64> -f sha=<current-file-sha> -f branch=main
```

## Key files

```
mac/CMakeLists.txt                     # mac targets; rpath; Sparkle embed; icon; per-file -fobjc-arc list
mac/cmake/Mac.cmake                    # libVLC + Sparkle provisioning (-DLIBVLC_MAC_PREFIX overrides VLC.app)
mac/src/app/AppDelegate.mm             # lifecycle + menu bar (App/File/Edit/View) + custom About + Updates
mac/src/app/MainWindowController.mm    # the UI: top bar, grid, split, playback, meters glue, ToU gate,
                                       #   video PANE model (Single/Split-2×2/PiP) + EPG orchestration
mac/src/app/VlcEngineMac.{h,mm}        # the ONE shared libvlc_instance_t; players borrow handle()
mac/src/app/VlcPlayerMac.{h,mm}        # libVLC wrapper: init(engine), setMuted (track-deselect), sampleStats()
mac/src/app/EpgGuideView.{h,mm}        # TV Guide renderer: flipped NSView, channels×time grid (ARC)
mac/src/app/TvGuideWindowController.{h,mm}  # guide window; DB->rows (normId @feed join), play-from-guide (ARC)
mac/src/app/VodSync.{h,mm}             # Xtream VOD catalogue sync worker (pure C++ + GCD; NOT in the ARC list,
                                       #   it holds no Obj-C objects). Owns its OWN Database connection.
mac/src/app/TermsDialog.{h,mm}         # first-launch / version-change Terms-of-Use gate (ARC)
mac/src/app/PlaylistsDialog.{h,mm}     # Settings > Manage Playlists (enable/disable/rename/refresh/delete)
mac/src/app/MetersDialog.{h,mm}        # Settings > Meters (Show/Style/Colours/Tuning + live preview)
mac/src/app/MeterView.{h,mm}           # unified meter renderer (4 kinds × 4 styles)
mac/src/app/MeterModel.{h,cpp}         # mac-local meter model (rabbitears::mac) + UTF-8 codecs
mac/src/app/SpectrumTap.{h,mm}         # Core Audio process tap + vDSP FFT -> bands (opt-in)
mac/platform/{Http,Log,Updater}.mm  mac/platform/Paths.cpp   # macOS platform layer
mac/packaging/{Info.plist.in, appcast-mac.xml, RabbitEars.icns, RabbitEars.entitlements}
scripts/{build-mac.sh, package-mac.sh, make-icns.py}         # build / bundle+sign+notarize / icon
cmake/AppVersion.cmake                 # per-platform version (Windows APP_VERSION + APPLE override)
common/models/FlowStats.h              # shared stream-health snapshot (Win32 + mac)
common/ui/VideoGrid.{h,cpp}            # SHARED pane geometry (Single/Split/Pip) — mac y-flips the boxes
common/core/{XmltvParser,Gzip}.{h,cpp} # SHARED EPG parse + gunzip (already compiled into mac; called as-is)
../common/ …                           # the shared engine (edit carefully — feeds Windows too)
```

## Working rules

- **Can't test GUI/audio headlessly** — real Mac testing is required for anything visual or audible (drive it
  with the computer-use MCP: `open` the app + screenshot; that's how the "can't paste" / meter / list-anchor
  bugs surfaced).
- **On-device testing recipe.** Launch the dev binary with an isolated DB so you never touch the user's data:
  `RABBITEARS_DATA_DIR=/tmp/redb build-mac/mac/RabbitEars.app/Contents/MacOS/RabbitEars &` (`defaultDbPath()`
  honors it). A local `python3 -m http.server` serving a hand-made `.m3u` + XMLTV fixture makes the whole
  import→refresh→guide→playback path deterministic and offline; use **`http://127.0.0.1:…`** (the loopback IP
  literal is ATS-exempt, so `NSURLSession` won't block cleartext HTTP). Public HLS streams that work for
  multi-view testing: `test-streams.mux.dev/x36xhzz/x36xhzz.m3u8`, Apple's `bipbop_4x3_variant.m3u8`.
- **Dev builds must be native arm64.** `build-mac/CMakeCache.txt` can hold a stale
  `CMAKE_OSX_ARCHITECTURES=arm64;x86_64` from a release build; a stock VLC.app is arm64-only, so the x86_64
  slice fails to link libvlc. Pass `-DCMAKE_OSX_ARCHITECTURES=arm64` to `scripts/build-mac.sh --app`.
- The **ToU gate is keyed on the FULL version incl. build number**, so every rebuild after a commit re-prompts
  in dev. That's expected, not a bug.
- **`open` can launch the WRONG bundle.** Several `RabbitEars.app`s share the bundle id
  (`/Applications`, `build-mac/`, `build-mac-universal/`), and LaunchServices may resolve to any of them —
  a stale `build-mac-universal` copy silently hijacked `open build-mac/mac/RabbitEars.app` for a whole
  debugging session. **Always confirm the version banner in `rabbitears.log`** (`==== RabbitEars (macOS) X.Y.Z (build) ====`)
  before trusting an on-device result, and `lsregister -f` the bundle you mean (or move the others aside).
- Launching the raw binary (`.../Contents/MacOS/RabbitEars`) instead of the bundle makes the *shell* the TCC
  "responsible process", so audio-capture permission is attributed to the terminal — another way to get a
  silently dead Spectrum meter. Use `open <bundle>` for anything permission-related.
- **Branch off `main`, PR back**; CI validates both platforms. Keep any shared‑file (`common/`) edit
  behavior‑preserving on Windows.
- Run an **adversarial review on new ObjC++** (ARC/threading/Cocoa) before merging — it has repeatedly caught
  real bugs here.
- **`git push` hangs intermittently** this machine/session — clear stuck `git-remote-https` procs + retry, or
  use `gh pr merge` / `gh api` (REST works fine) for anything targeting `main`.
- **The repo lives under `~/Desktop`, which macOS TCC protects.** Access can be revoked *mid-session* — every
  read, including `git`'s own cwd probe, starts returning `EPERM: operation not permitted` while `~/Documents`
  and `~/Downloads` still work. An agent's `request_directory` grant does **not** lift it (that's Claude's
  permission layer, not the kernel's). Fix: `tccutil reset SystemPolicyDesktopFolder com.anthropic.claude-code`
  restores it immediately, no relaunch. **Any review or build that ran during an EPERM window is void, not
  green** — the tools were reading nothing.
- Windows `gui-build` CI is **pre-existing red** — and it dies at **CONFIGURE** time (`RABBITEARS_THEME_ENGINE
  needs fxc`), so it never compiles anything. **The real MSVC gate for a `common/` change is Windows
  `core-selftest`** (it compiles `RabbitEarsCli.cpp`). A `common/` header change can therefore break every
  mac build while Windows CI stays green — that is exactly how `main` sat unbuildable on mac for days.
- **Verify migrations by RUNNING them, not by reasoning.** Author the OLD schema with the
  actually-shipped binary (`RABBITEARS_DATA_DIR=<scratch>`, kill after ~5 s), seed the adversarial cases,
  then run the NEW binary. Reasoning got v9 wrong on both sides: the Win32 handover says it is a no-op on a
  live-TV-only library and it is not. ⚠ When seeding a `dead_status` case, set `last_checked_at` too — the
  merge couples them deliberately, and a zero timestamp looks like a bug that isn't one.
- **A mitigation can be worse than the bug.** Today's scheduler stand-down (deferring a recording while a
  VOD sync ran) was unbounded and could lose the very recording it protected. When a fix inverts a
  priority, check the unbounded-wait case — and prefer an adversarial review that MEASURES (the same
  review measured the contention at ~0.2 s, not the 5 s the mitigation assumed).## Seed prompt for a fresh session

```
Read mac/HANDOVER.md and the recalled memory. RabbitEars is a cross-platform native IPTV player
(Windows + macOS) in ONE repo (common/ + Win32/ + mac/, unified root CMake; playback libVLC, storage
SQLite). main carries BOTH platforms at decoupled versions in cmake/AppVersion.cmake (APP_VERSION =
Windows 0.2.17; an if(APPLE) override = mac 0.2.16) — that file is the recurring cross-team merge
conflict, keep both lines. App min macOS 26 (Apple-Silicon-only, so the x86_64 slice can never run;
arm64-only builds are fine). Build: scripts/build-mac.sh --app -DCMAKE_OSX_ARCHITECTURES=arm64.
Release recipe + gotchas: the mac-release-deployment memory.

TWO THINGS TO KNOW BEFORE ANYTHING ELSE:

1. NOTHING RECENT HAS REACHED USERS. main (c5ac088, build 403) carries three releases' worth of
   merged work, but the live appcast still serves 0.2.15, so every installed mac app is on 0.2.15.
   mac 0.2.16 is BUILT + NOTARIZED + STAPLED yet DELIBERATELY UNPUBLISHED at
   ~/Downloads/RabbitEars-0.2.16.dmg (arm64, build 398, sha256 36b2f578..., 41,602,223 bytes) —
   the owner asked to hold the appcast, which is the step that actually pushes a build to everyone.
   The artifact is build 398 and main is 403, so a publish should REBUILD. To publish:
   sign_update --account SQLTerminal (one keychain Allow) -> gh release create v0.2.16-mac --target
   main --latest=false -> appcast <item> on main (xmllint FIRST).

2. THERE IS UNMERGED WORK IN FLIGHT: branch mac-seek-scrubbar (bbc7db9) = the whole seek layer
   (scrub bar, time readout, skip +/-10s, and PAUSE — mac had none at all). It BUILDS CLEAN and the
   selftest passes, and that is ALL: it has NOT been adversarially reviewed and has NOT been driven
   on device. BOTH are required before merge. DO NOT MERGE IT ON "it compiles" — on this project
   that pair has caught a real bug in every phase, including two the previous session introduced
   itself. Details + the four already-fixed design traps are in the HANDOVER's own section.

Already merged on main and NOT yet released: PR #43 (a Clang-vs-MSVC break that had mac unbuildable
on main — GridFilter nested-struct default member initializers), PR #44 = 0.2.16 "safe to upgrade"
(first mac build carrying schema v8+v9, where v9 is a DATA migration that rewrites every stream_url
and merges colliding rows; plus favourites canonicalisation, "Clear dead-link results", the
GridFilter/SQL grid pushdown + 5000-row cap + search debounce, an ATS exception, and three live
shipped bugs), and PR #45 = the Xtream VOD movie sync + a Movies nav root, VERIFIED end-to-end
against a fake Xtream panel (a first for either platform — Windows shipped it unverified).

NEXT: finish the seek layer — (a) adversarial ObjC++ find->verify Workflow (MRC/threading/logic
lenses), (b) apply what it CONFIRMS (not raw findings), (c) GUI-verify against a real seekable file
AND a live channel, (d) merge + delete branch. Then the small tail: dead-link sweep (now unblocked
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
```
