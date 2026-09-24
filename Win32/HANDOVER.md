# RabbitEars — Handover

A native **Windows (Win32 / C++20)** IPTV player built on **libVLC**, themed to
match its two sibling apps, **`G:\SQLTerminal-Win32`** and **`G:\ManorLords-SGE`**
(dark "Claude-desktop-style" look, coral accent `#D97757`, custom title bar,
CMake + Ninja + MSVC, dependencies vendored / NuGet-provisioned with **no Visual
Studio project**). This is the single starting point for anyone (human or agent)
continuing the work — read it before touching code.

> **Location:** this is the **Windows team's** handover, kept under **`Win32/`** (with its
> companion `Win32/BACKLOG.md` and design docs in `Win32/docs/`) so it doesn't collide with the
> macOS team's edits on shared root-level files — they own **`mac/`** (`mac/README.md`) and share
> `common/` + root `docs/`. Moved here from the repo root in the 0.2.x theme-engine stream.

## Stack decision (important)

The design doc (`IPTV Player Application Design.docx`) lists a "WinUI 3 / EF Core"
table. That is a boilerplate artifact and is **overridden** by the explicit
direction to leverage the two C++ reference apps' look. RabbitEars is therefore a
**custom-drawn native Win32 / C++20 app** (GDI + Direct2D), exactly like the
siblings — *not* WinUI 3, *not* .NET/EF Core. Storage is SQLite via the C API.

| Component     | Choice                                                        |
|---------------|---------------------------------------------------------------|
| Language      | C++20, Windows SDK                                             |
| UI            | Custom Win32 chrome + Direct2D/GDI owner-draw (shared Theme.h) |
| Media engine  | libVLC 3.0.23 (VideoLAN.LibVLC.Windows NuGet, provisioned)     |
| Storage       | SQLite (vendored amalgamation, C API)                         |
| M3U parsing   | Custom parser (`src/core/M3uParser`)                          |
| Build         | CMake + Ninja + MSVC (VS 2026 Community), deps vendored        |
| Installer     | Inno Setup 6 (`packaging/installer.iss`)                       |
| Auto-update   | WinSparkle, EdDSA-signed appcast on GitHub (LIVE as of 0.1.1) |

## Current state — **v0.2.19 SHIPPED, auto-update LIVE** (`APP_VERSION` 0.2.19 — bump before the next cut) · macOS **0.2.17**

### ✅ 0.2.19 — SHIPPED (2026-09-24), both appcasts LIVE @ `d2401b5`

**Released:** tag **`v0.2.19`** @ `5a2378d` (verified: `git ls-remote origin refs/heads/main` == HEAD
before building AND before tagging), full version **`0.2.19.434`**, GitHub release "RabbitEars 0.2.19"
with three installers — the uploaded sizes match, and the two per-arch assets downloaded back from the
release are byte-identical to the local builds:

| installer | bytes | SHA-256 |
|---|---|---|
| `RabbitEars-0.2.19-setup.exe` (x64) | 35,539,018 | `B74C01ADCBC32F1757C7AFEC7272452941A653EEC1F467D3CA644C83D8EE05ED` |
| `RabbitEars-0.2.19-arm64-setup.exe` | 30,374,457 | `A16F1D6A198CDD7428B9032AE8561151524F36ECCBBA96133BE4FD3C364FA6E0` |
| `RabbitEars-0.2.19-universal-setup.exe` | 63,614,899 | `A2EF1BB6A8A434C4D95E68E55D15AB9B259AE96B9623A2FBA99682AA562F9F26` |

Both theme flags built and `--selftest` ALL PASS (709) on the release commit (x64); both build dirs
cached at THEME_ENGINE=ON / BUILD_GUI=ON; the ARM64 exe's PE machine is `0xAA64` (cross-compiled — this
x64 box cannot run the ARM64 selftest). Signed by the owner on the Mac with the 0.2.18 recipe below
(`SIGN_UPDATE=… SIGN_UPDATE_ARGS="--account SQLTerminal" scripts/sign-release.sh`, first try); both
signatures verified on Windows against the app's EdDSA key over the DOWNLOADED bytes (valid on their
own installer, INVALID swapped); `make-appcast.ps1 -Tag v0.2.19` for both arches.

**What 0.2.19 contains:** `609b31b` guide-refresh progress + Set Guide URL address extraction + log
credential masking (step 1) · `0892cf4` the TV Guide's one search box — channels + FTS5 programme
search, schema v10 (step 2) · `35b8826` Greek/Romanian case folding in recording rules · docs
(`50af36c`, `7325974`, `5a2378d`). The release notes are user-facing, in the 0.2.18 style.

**Shipped with less than a full owner run:** the search's filter chip, the jump from a result and
typing over the grid (in the owner's test list, nothing reported against them — the screenshots
covered the results list); the recording-rules fold (selftest only — no Greek series rule has run).
**To watch after release:** schema v10 on other people's libraries (a table-creation migration, far
milder than v9's rewrite), and the mac team's first compile of the new `common/db` code.

### 🛠️ 0.2.19 — the owner's two EPG requests (2026-09-24)

The owner asked for **(1) full-text search in the EPG** and **(2) a calendar in the EPG** to see when a
show airs in the future. Agreed order (owner, 2026-09-24): step 1 refresh feedback + log hygiene →
step 2 a real guide search box + FTS5 programme search (descriptions INCLUDED, owner's call) → step 3
the calendar, only once there is guide data that reaches far enough ahead to fill one.

**Step 1 — DONE, owner-verified live (dev profile, 2026-09-24): "can see progress - good", Set Guide
URL "works", VOD played with its line masked.** Win32-only except one additive `common/` overload:
- **Guide refresh progress.** A running MB count while the XMLTV downloads (Win32-only
  `httpGetWithProgress`, `Win32/platform/HttpProgress.h`), a running programme count while it parses
  (new overload `parseXmltv(bytes, onProgress)` in `common/core/XmltvParser` — the one-argument form
  keeps its signature and results; mac calls only that), and the loading box now STAYS UP through the
  UI-thread store and the series-rule pass ("Saving N programmes…", "Checking your recording rules…")
  — before, it closed first and a 190k-programme store left a window that did not respond with
  nothing on screen. `updateLoadingDialog` now paints its line before returning.
- **Per-step timings in the log** (`EPG timings for "<playlist>": download … gunzip … parse … store`).
- **Set Guide URL keeps just the address** out of pasted text (`extractHttpUrl`,
  `Win32/platform/UrlRedact`): the owner had pasted `EPG Link : http://…` from the provider's email,
  which was saved as-is and failed every refresh with "Invalid URL.". A lone address is kept EXACTLY as
  typed (a password may end in `.`/`!`/`)`); text with no http(s) address gets a notice and the prompt
  reopens with the text kept. Two new i18n pairs (595 keys now).
- **🔒 The diagnostic log no longer records provider logins.** It did, in clear: an Xtream login is in
  every playlist/guide URL (query) and every stream URL (path, `/USER/PASS/123`) — all 410,154 of the
  owner's stream URLs carry it, and `play:` lines, playlist/guide lines and libVLC messages logged them.
  Now `Win32/platform/Log.cpp` masks EVERY line (`redactUrls`): user-info, credential-named query
  values and Xtream stream-path logins by shape, plus registered secrets anywhere as whole tokens
  (registered from each playlist's URLs at startup, on download/add/Set Guide URL, and a known
  account's path spelling on play/record — `Win32/platform/LogSecrets.h`). **The previous session's
  log is scrubbed at startup** (`scrubPreviousLog`, temp-file + swap, 64 MB cap, WARN on any failure)
  so the pre-fix log does not survive the upgrade. libVLC lines are no longer cut at 1,023 bytes (a cut
  could leave half a password). **Verified:** the REAL `UrlRedact.cpp`, run over all 410,154 real stream
  URLs by a throwaway harness: 0 still carry the login — by shape alone, with startup registration, and
  quoted without a scheme; the owner's own logs after the test: 0 occurrences in either file, and the
  10:57 log's three clear lines now read `username=***&password=***`. ⚠️ **The mac log sink
  (`mac/platform/Log.mm`) masks nothing** — flagged for the mac team in BACKLOG, their tree untouched.

**Verified:** both theme flags build clean at /W4, `--selftest` ALL PASS (632 checks; new blocks for
the parser progress, the masking and the URL extraction — including a pin of the path-encoding mirror
against XtreamClient's real encoder), `gen_i18n --check` OK. **Adversarially reviewed in three
rounds:** R1 (code+security, claims) 11 claims findings + 3 medium code findings (a new playlist's
guide URL not registered until the next launch; a clean guide URL losing a trailing `!`; masking
garbling unrelated words) → fixed; R2: 2 medium (lookalike URLs registering ordinary words as secrets
for the whole session; the old-log scrub was quadratic — 79 s on 16 MB) → fixed (gated registration,
single-pass replace); R3: no high/medium, 4 low + 4 nits → all fixed. Reviewed state snapshot:
tree `c0b12fe` (pre-R3 fixes). **Deferred to BACKLOG:** a warning when the pasted guide link looks
like a playlist link; the topmost loading box staying above other apps during the ~1 s store.

**What the investigation found — read before steps 2 and 3** (all measured on the owner's real data):
- **The owner's provider publishes only ~6 hours of future guide.** 192,595 programmes over 2,881
  channels; median channel horizon **5.8 h** after a refresh (p90 7.2 h; 13 channels ≥ 24 h), plus
  ~1.8 days of past. A calendar cannot show airings the provider never sends.
- **Its Xtream API publishes NO guide at all:** `get_simple_data_table` and `get_short_epg` returned
  empty lists for 18 channels across 6 regions (USA/FR/DE/CA/UK/PL) with the account authenticated
  (Active) and `get_live_streams` returning 15,345 streams. `xmltv.php` is the only source. So the
  calendar needs a SECOND guide source (today: one URL per playlist, and a refresh wipes the whole
  playlist's programmes) — or stays parked.
- **298 of 15,345 live channels have catch-up** (`tv_archive`, 1–3 days): past search results could
  be playable on those — a possible later feature, not scoped.
- **Refresh timings (two runs):** download of 61 MB **7.4–9.9 s** (75–85 % of the refresh), gunzip
  5 ms, parse 0.72 s, store **0.78–1.28 s on the UI thread**, rule pass 0 ms (no rules). The guide's
  first open: **1,221 ms** on the UI thread for 2,410 channels (46,776 programmes in its window) — the
  first measurement ever of BACKLOG's deferred "TV Guide off-thread" item.
- **Search, benchmarked on the real 192,595 programmes** (Python's SQLite 3.50, same engine): today's
  kind of scan (`LIKE`, NOCASE is ASCII-only) 29–35 ms titles / 46–70 ms with descriptions, and it
  MISSES accents and non-Latin case ("quebec" 10 titles vs 59; a Greek query 0 vs 20). FTS5: <1 ms for
  typical queries, 8–22 ms for "news". Index: titles trigram 12.6 MB (rebuild 0.34 s), titles
  unicode61 4.5 MB (0.17 s), + descriptions unicode61 +30 MB (1.21 s), + descriptions trigram 145 MB
  (4.55 s — too big). The feed has NO sub-titles, categories or episode numbers; titles 17 % non-ASCII
  (Latin accented, Greek, Cyrillic, Arabic; no CJK). **FTS5 is NOT compiled into our SQLite** on
  either platform (`CMakeLists.txt:81` — the amalgamation's `SQLITE_CORE` compiles FTS5 out unless
  `SQLITE_ENABLE_FTS5`); enabling it is one define that reaches the mac build too.
- **The guide's corner search box has no caret** (owner): it is painted, not an EDIT — step 2 replaced it.

**Step 2 — DONE, committed `0892cf4`, owner-tested live** (dev profile, build 431, 2026-09-24), plus
**`35b8826`**, a recording-rules fix found on the way. The design AND the as-built record:
**[`docs/EPG_SEARCH.md`](../docs/EPG_SEARCH.md)** (§7 = the owner's decisions).
- **One search box** — "Search channels and programmes…" — in a new guide toolbar with a **Now** button;
  the corner cell's painted type-to-filter is gone. *The owner's first live look at a TWO-box build
  ("two search bars?") changed decision §7.1 to one box.* Results replace the grid: **matching channels
  first** (in memory, by name, case- and accent-blind; its one item — selected by default, so
  "toronto, Enter" — narrows the grid and shows a **"toronto ✕" chip** in the corner; click or Esc
  clears it), then **upcoming programmes** (FTS5), title matches before description-only ones, by day,
  the match marked, "On now" badges. Click jumps to the programme in the grid; double-click / Enter
  opens the programme dialog; right-click = Play / Schedule / Record series / Show in guide.
- **Shared core (reaches the mac build):** `SQLITE_ENABLE_FTS5` on the root `sqlite3` target (mac builds
  that target too); **schema v10** — external-content FTS5 tables (titles trigram, descriptions
  unicode61), rebuilt after each guide store, stamp-checked so a stale index is never read, a v9
  database never re-runs v9's rewrite, an FTS5-less build stays at v9 and searches by LIKE;
  `Database::searchProgrammes` / `refreshProgrammeSearchChannels` / `programmeSearchState` /
  `rebuildProgrammeIndex`; `common/core/SearchFold.h` (the accent-blind fold used to mark matches —
  "quebec" marks "Québec" — and for the guide's channel matching). 14 new i18n keys (609; CJK drafts).
- **Measured live** (owner's dev profile, 201,962 programmes, 2,474 guide channels): a refresh =
  download 8.0 s, parse 0.71 s, store 0.83 s, **index rebuild 1.53 s** (its own loading-box line,
  "Indexing the guide for search…"); a search session's first query +0.1 s (the channel set);
  searches **0.3–2 ms** typical, ≤ 61 ms worst seen; the guide's first open 1.4 s. The v10 upgrade on
  a copy of the real library: 26–41 ms.
- **Owner-verified:** the results for "toronto" and "quebec" (screenshots: the channels block, marked
  titles — "Québec" included — centred labels, the shorter rows), then asked to commit. The chip, the
  jump and type-over-the-grid were in the test list; nothing was reported against them.
- **Verified:** both theme flags build, `--selftest` ALL PASS (709), `gen_i18n --check` OK; four
  adversarial review rounds on the first (two-box) build, three on the one-box rework and the folds —
  every finding fixed.
- **`35b8826` — recording rules:** `foldChar` (case-only) now folds the Greek capitals with a tonos,
  Ϊ Ϋ and final ς, and Romanian Ș Ț, so a rule for "άρης" matches "ΆΡΗΣ"; the PERSISTED episode keys
  are re-folded before comparing (so rows stored by the older fold still dedup), which needed the fold
  to be idempotent — kra (ĸ) no longer pairs with Ĺ.
- 🍎 **Not compiled with Apple clang here** — the mac team's first build of `main` with `0892cf4` is the
  first compile of the new `common/db` code on their side; flagged in BACKLOG. A mac build on it
  migrates its database to v10 (empty FTS tables; nothing on mac searches yet).

**Shipped in 0.2.19** (step 1 + step 2 + the rules fix — see the block above). **Step 3 (the
calendar) stays parked** — see "What the investigation found".

### ✅ 0.2.18 — SHIPPED (2026-09-24), both appcasts LIVE @ `2df99ea`

**Auto-update LIVE (2026-09-24):** both appcasts `0.2.18.426` committed + pushed @ `2df99ea`. Owner-signed
on the Mac (`sign_update --account SQLTerminal` — see the signing note under "Release / auto-update");
**both signatures verified on Windows against the app's EdDSA public key before publishing** (each valid
on its own installer, INVALID when swapped), both feeds HTTP 200 serving `0.2.18.426` byte-identical to
the committed blobs, and both enclosure URLs downloaded end-to-end: HTTP 200, the signed length, the
recorded SHA-256, and the signature valid on the downloaded bytes.

**Released:** tag **`v0.2.18`** @ `3c14828` (verified: `git ls-remote origin refs/heads/main` == HEAD
before building AND before tagging), full version **`0.2.18.426`**, GitHub release "RabbitEars
0.2.18" with three installers — sizes and SHA-256 (the uploaded sizes were checked against these):

| installer | bytes | SHA-256 |
|---|---|---|
| `RabbitEars-0.2.18-setup.exe` (x64) | 35,418,641 | `151087AA85325B2E7A32ABA85E3F05EF69355C6306C5FD3CEEE61CDC52AE3C30` |
| `RabbitEars-0.2.18-arm64-setup.exe` | 30,261,202 | `29949469F6E743E461981FFB19BC33D3EED4F4E95C5ADCE9D587BE6113D103B8` |
| `RabbitEars-0.2.18-universal-setup.exe` | 63,373,548 | `A32D396096994B72CB8D011AB8E2C0982329A6B6117654230D464AC9B49310A9` |

Both theme flags built and `--selftest` ALL PASS on the release commit; the ARM64 exe's PE machine is
`0xAA64`; both build dirs cached at THEME_ENGINE=ON / BUILD_GUI=ON.

✅ **The appcast step, as it actually ran** (the recipe for the next release): the owner signed on the
Mac — the first attempt failed with `sign_update not found`, because `scripts/sign-release.sh` looks
only in `./bin` and on PATH, while `sign_update` lives in the mac build dir, and because the family key
needs `--account SQLTerminal`, which the script does not pass by default. What worked, from the repo
root on the Mac:
`SIGN_UPDATE="$(ls build-mac*/sparkle/bin/sign_update | head -1)" SIGN_UPDATE_ARGS="--account SQLTerminal" scripts/sign-release.sh <installer>`.
Then `make-appcast.ps1` twice with `-Tag v0.2.18`, each signature checked on Windows against the public
key in `Win32/platform/Updater.cpp` (Python `cryptography`, Ed25519 over the file's raw bytes), commit +
push, and both feeds + both enclosures verified.

**What 0.2.18 contains** (four commits on top of `b4b3e4c`, plus the two already pushed before):
`b5c016f` search debounce · `b4b3e4c` lost schedule-status writes · `e8886cb` RabbitEarsRender
(photoreal Phase 0) · `ad5f315` Phase 1 — the six meter defects · `aefadf2` dev side profile +
`APP_VERSION` 0.2.18 · `3c14828` the VU instruments (backlit "VU needle" + new "Silver VU", PEAK lamp
lights red in the red zone). The rows below are the pre-release record of each.

⚠️ **Shipped without an owner run:** the search debounce and the lost-status-write fix (its six owner
checks are in the "Lost schedule-status writes" block — the ordinary scheduled-recording path first);
the Light-skin meter fixes; and the final PEAK-follows-the-needle rule (no separate review pass).
**Owner-seen live before the cut:** the tank readout ("good — looks nice"), the VU dials ("look
amazing").

### ▶️ The 0.2.18 changes, as recorded before the cut (2026-07-29 → 2026-09-24)

| change | status | owner check |
|---|---|---|
| 🔎 **Search debounce** (`b5c016f`) | pushed | typing in search feels smooth; a nav click right after typing shows the node |
| 🔴→✅ **Lost schedule-status writes** (`b4b3e4c`) — flagged by the mac team; can truncate a recording or silently mark it Missed | pushed | the six checks in the "Lost schedule-status writes" block below — the ordinary scheduled-recording path FIRST, because the start order changed |
| 🖼️ **Photoreal Phase 0 — `RabbitEarsRender`**, a headless render tool | see git | none needed (dev tool; the app is unchanged) — but the **six photoreal decisions** in `docs/PHOTOREAL.md` are yours |
| 🖼️ **Photoreal Phase 1 — the six meter defects fixed** (red zone, VU shadow, tank readout, Light-skin LEDs + underglow, glow clipping) | working tree, **uncommitted** | ✅ owner, live: *"tank readout is good — looks nice"*, *"the VU meter is nicer"* (its VU parts were then superseded, below). Still unseen: the Light skin |
| 🎛️ **The VU instruments** — "VU needle" rebuilt as the owner's backlit reference meter; NEW "Silver VU" after their cassette-deck photo (`Win32/ui/VuDial`) | working tree, **uncommitted** | ✅ owner, live: *"the VU meters look amazing"*. Then, on request: the PEAK lamp lights red whenever the needle is in the red zone (every kind) — not yet seen live. See PHOTOREAL.md "The VU instruments" |
| 🧪 **Dev side profile** — `scripts\run-profile.ps1` runs a build BESIDE the installed app on a snapshot of the library (`Win32/platform/Profile.h`) | working tree, **uncommitted** | ✅ used for the checks above. Never touches the wake task or WinSparkle |
| 🔢 **`APP_VERSION` 0.2.17 → 0.2.18** (`cmake/AppVersion.cmake:11`; the mac override untouched) | working tree, **uncommitted** | bumping is not releasing — the tag + two appcasts still gate it |

**The biggest change to how this project works:** visual work no longer has to go to the owner blind.
`build\Win32\RabbitEarsRender.exe <outdir>` renders every meter look, the buffer tank and every skinned
transport strip to PNG from the REAL paint code, byte-reproducibly, and the assistant can read those
PNGs. Details, limits and the photoreal proposal: **[`docs/PHOTOREAL.md`](docs/PHOTOREAL.md)**.

### ✅ 0.2.17 — SHIPPED (2026-07-28) — the big-library release

**Released:** tag **`v0.2.17`** @ `3660441`, full version **`0.2.17.388`**; three installers on GitHub
release `v0.2.17` — x64 `35,376,445` / arm64 `30,241,924` / universal `63,319,122` bytes — **two
appcasts** (`0.2.17.388`) committed @ `fe3d872` and LIVE (both feeds HTTP 200, and both enclosure URLs
downloaded end-to-end with the byte count matching the signed length). Owner-signed on the Mac; the
two feeds cross-checked so the x64 and arm64 signatures cannot be swapped.

**The theme: the library is TEN TIMES bigger than the design assumed.** Every item traces back to the
0.2.16 on-device pass discovering a real **411,149-row** library where `docs/XTREAM_VOD.md` had
planned for 44k. `--benchdb`'s defaults still model the small shape — see BACKLOG.

| what | why | verified |
|---|---|---|
| **v8 migration fix** (`aad57f5`) | five `ALTER`s shared ONE guard, so a partial failure latched `user_version=8` over a half-migrated table and every channel query then failed to prepare — **library silently empty, permanently**. Present in the v0.2.16 binaries. | selftest |
| **`canonicalStreamUrl()` + schema v9** (`22253b6`, `9eed4ff`, `986df48`) | the m3u writes `host:80`, the VOD sync builds `host` → the dedupe index saw two strings and **every film was stored twice**. v9 rewrites stored URLs and merges the rows that become equal. | dry run on a COPY of the real DB (454,195 → 410,596 rows, 0 duplicates, 14/14 favourites kept, integrity ok), then ✅ **ran on the owner's live DB** |
| **Grid row cap** (`04b4d22`) | All Channels **1485 → 108 ms**; search **1626 → ~134 ms PER KEYSTROKE**. The two view filters moved into SQL so the cap composes with them. | ✅ *"all channels appears instantly, search much more responsive"* |
| **Skip back / forward** (`3660441`) | ±10 s buttons flanking the scrub bar, sharing its visibility exactly. | ✅ *"used forward back — works good, looks good"* |

⚠️ **Two things shipped that no one has exercised**, recorded here rather than discovered later:
1. **Schema v9 runs on every user's first launch.** It is a no-op on a live-TV-only library (nothing
   to canonicalise, nothing to merge) and the transaction retries rather than half-applying — but the
   owner's is the **only real library it has ever touched**, and it is the first migration here that
   rewrites existing rows rather than adding columns. *First place to look if anyone reports a wrong
   channel list after upgrading.*
2. **The VOD sync's DELETION path still has no live run.** A first sync only inserts; the **second**
   is the one that calls `retireMissingChannels` in anger. `0 removed` is the healthy answer.

**The three lessons this line actually taught** (all three cost real time, none was a coding error):

- **A confident claim in a comment is not a verified fact.** `bulkInsertChannels` documented that the
  m3u and sync URLs "collide on idx_channels_dedupe BY DESIGN" — a real provider falsified it with
  one `:80` and doubled a 43,599-film library. Two more of the same shape shipped in this line:
  "macOS needs no source change" (it did not compile — `_wtoll`), and "pinned by a selftest" twice
  over, where the tests did not execute the code they claimed to cover.
- **One sample is not an answer about a class.** The live-scrub-bar question was recorded as
  ANSWERED off a single channel; a second look found `is_seekable` varies **per channel on one
  provider**, and the bar is a *working rewind* rather than the bug it was nearly suppressed as.
- **A fix can be the regression.** Arming the exit watchdog before the worker joins (to stop them
  skipping the wake-task registration) put two unbounded joins inside a 4 s budget, which would have
  force-exited past the only code that finalises an in-progress recording.

## Previous release — **v0.2.16 SHIPPED (2026-07-28)** · macOS 0.2.15

### ✅ 0.2.16 — SHIPPED (2026-07-28) — the Xtream VOD release

**Released:** tag **`v0.2.16`** @ `d83b002`, full version **`0.2.16.377`**; three installers on GitHub
release `v0.2.16` — x64 `35,370,751` / arm64 `30,218,567` / universal `63,289,025` bytes — **two
appcasts** (`0.2.16.377`) committed @ `0344b54` and LIVE (both feeds HTTP 200, and **both enclosure
URLs downloaded end-to-end with the byte count matching the signed length**, which is the check that
catches a bad `-Tag`). Owner-signed on the Mac; the two feeds cross-checked so the x64 and arm64
signatures cannot be swapped.

⚠️ **The pre-release verification returned NO-GO once, on the same class of blocker as 0.2.15** — the
docs commit `d83b002` had not reached the remote when the release was called for. `gh release create`
tags the REMOTE head, so building then would have stamped `0.2.16.377` while the tag pointed at
count 376. Caught by `git ls-remote origin refs/heads/main` before anything was built. **That check
has now saved two consecutive releases; run it before you build, not after.**

**✅ OWNER-VERIFIED ON-DEVICE before the cut** — the two things the sandbox cannot check:
- **The scrub bar is INVISIBLE on live TV and works on seekable content.** Confirmed on the real
  provider: absent on `NL - SPONGEBOB` (a 24/7 feed), present and tracking on a series episode
  (`0:49 / 43:30`). This was the release's top risk item and it closed as GO.
- **Glass bezel and the empty-tank fix: "look fine"** — clearing the one reservation carried over
  from 0.2.15 ("needs work — but ok for this release").

⛔ **What shipped UNVERIFIED, and it is the riskiest thing in the release: the VOD sync itself.** The
owner's Xtream line expired before a real sync could run, so insert + `retireMissingChannels` has
never executed against a live provider — only against selftests. Mitigated by design rather than by
luck: it is user-triggered, invisible without an Xtream playlist, gated on playback/recording, and
refuses to delete anything when too much of the response was unusable. **First real sync is still
outstanding — see "What still needs the owner".**

> ### ⚠️ Version note — the two PLANNED releases merged into one shipped 0.2.16 (owner's call, 2026-07-28)
>
> The plan was 0.2.16 = groundwork, 0.2.17 = the Xtream client. **0.2.16 was version-bumped but never
> tagged and never given an appcast, so no user ever had it** — so both halves shipped together as a
> single **0.2.16**. What merged is the *release plan*, not the version numbering: **0.2.17 is simply
> the next version, unused and available.** Practical consequences: no bump was needed for this cut
> (`APP_VERSION` already read `0.2.16`); and text elsewhere that says "0.2.17" where it means "the
> Xtream VOD work" predates this call — commit messages (`538f0b2`, `e4e01a7`) say it too and cannot
> be rewritten. Read those as *the VOD half of 0.2.16*.

> **`main` is clean and fully released.** `APP_VERSION` is `0.2.16` (`cmake/AppVersion.cmake:11`; the
> `if(APPLE)` override is untouched at `0.2.15`) — **the next release bumps it to `0.2.17`**, or to
> `0.3.0` if series lands first and you want the minor bump to mark the capability.
>
> The Xtream VOD epic's two gates are both CLOSED and **movies have SHIPPED**. The design doc —
> written off a REAL provider's measured answers, not estimates — is
> **[`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md)**; read it before touching the epic. **Series
> (0.3.0) is the remaining half**, and §1 already has the measured shape for it.

### ✅ 0.2.15 — SHIPPED (2026-07-26)

**Released:** tag **`v0.2.15`** @ `1324f5f`, full version **`0.2.15.365`**; three installers on GitHub
release `v0.2.15` — x64 `35,350,517` / arm64 `30,194,643` / universal `63,242,002` bytes — **two
appcasts** (`0.2.15.365`) committed @ `77035ed` and LIVE (both feeds HTTP 200 **and both enclosure URLs
verified HTTP 200**, which is the check that actually catches a bad `-Tag`). Owner-signed on the Mac;
every signature byte-length-verified against its file, the two appcasts cross-checked against each
other, and all three installer hashes confirmed unchanged across the round trip to the Mac.

⚠️ **The pre-release verification returned NO-GO first, on a real blocker** — worth reading before the
next release, because the failure was invisible and permanent. The last commit (`1324f5f`, docs only)
was **unpushed**, so `origin/main` was at commit count 364 while the binaries were already stamped
`0.2.15.365`. `gh release create` tags the REMOTE head, so the tag would have pointed at source that
did not contain what shipped, and anyone rebuilding from the tag would get `0.2.15.364` — a *lower*
WinSparkle version than the release. **Push before tagging, and verify `git ls-remote origin
refs/heads/main` matches HEAD.** (Also: push promptly — the mac team pushes to `main` too, and a
commit landing between the build and the tag moves the count and permanently mismatches the
already-built installers.)

`APP_VERSION` is **`0.2.15`** (`cmake/AppVersion.cmake:11`; the `if(APPLE)` override is separate and
untouched). The `0.2.15-dev` branch was merged and pruned; everything is on `main`.

**✅ OWNER-VERIFIED ON-DEVICE (2026-07-26, x64 build `0.2.15 (362)`)** — the whole line was checked in
one pass: *"tank looks better … VU meters better … VU color change good … About box looks good"*.
The one reservation: **glass bezel "needs work — but ok for this release"** (see BACKLOG). This
matters because 0.2.15 is almost entirely VISUAL, and visual work is the one thing the dev sandbox
cannot check at all.

- **About-box tip section** (`e02e140`) ✅ — the Buy-Me-a-Coffee / Ko-fi buttons finally say what they
  are. Deliberately silent on how the two backends differ (owner's 0.2.13 call). The box grew
  dp(470)→dp(530) and is now **capped to the work area**: dp(530) is 795px at 150%, and a 1366×768
  laptop has ~708px of work area, so uncapped the entire button row sat under the taskbar.
- **PIP right-click menu + PIP⇄main swap** (`e02e140`) — the floating PIP gets its own menu instead of
  the main window's view menu. The swap is **allowed while recording**: recording runs on a separate
  headless player (`VlcPlayer::rec_`) and `doStop()` never touches it. *Not exercised in the pass.*
- **Dead-link checker graduated** (`e02e140`) — flag *and* branch deleted. Safe because a sweep is
  user-triggered and now **undoable**: Settings ▸ Channels ▸ "Clear dead-link results".
- **Splash**: the "Arch cookies" line became `SplashWrappingTinFoil`.
- **Glass "framed pane"** (`5f447b7`) ⚠️ — three bands: the theme's own 1px border left bit-identical,
  an opaque hard-stepped bezel in the chrome gutter, and the dial carrying only the bezel's cast
  shadow (`add` is exactly 0 over content — the invariant that forecloses the old "reads as blur"
  failure). Costs **zero dial pixels**; footprint went *down* 42.4%→38.2%. **Owner: needs work.**
- **Look-aware meter knobs** (`80e3a38`, `9bb9f7a`) — the slider band is now a function of
  (kind, LOOK), not kind alone. Two of VU's three sliders were DEAD, and `Sens` was a real bug
  (`scalarLevel` never applied it). Completing it also removed four dead `Glow` sliders that shipped
  in the DEFAULT configuration, since `glow` is only read by Tube and Scope.
- **VU bottom-lamp lighting** (`b07ef8c`) ✅ — `common/ui/VuLamp.{h,cpp}`, pigment × light. The face
  was a gradient brighter at the TOP (i.e. a lamp *above* the dial) and the needle's shadow offset
  down-right. Now a bulb below the bottom edge and left of centre, a needle shadow that is the needle
  ROTATED 2.6° about the pivot (a fixed offset gives constant separation, which reads as two
  needles), and **blue via `palette.bg`** — hue only, black restores the stock bulb.
- **Data-flow tank: top-centre pour, floor drain** (`e4d24e5`, `ad2d738`) ✅ — the drain is a dish in
  the level controller's OWN target rather than a velocity sink, so it shares the controller's fixed
  point instead of fighting the buffer-health readout. Unclaimed win found by review: per-column
  waterline spread collapsed **7.66 → 1.72 rows** (the old right→left drift piled water 9 rows deep
  on the left wall and 1.3 on the right). Resting level moved 5→6 of 10 LED rows at full health;
  `kVisibleFill` 0.68 kept, owner-accepted.
- **User-selectable fluid colour** (`2071aa4`) ✅ — one global swatch in the Data-flow row (that row
  has no `MeterConfig`). Depth shading is Beer-Lambert and reproduces the old default **byte-identically
  at every depth**.

**Two review lessons worth carrying forward.** (1) A design panel's *reasoning* can be wrong even when
its *conclusion* is right: the buffer drain's "a velocity sink is unsolvable here" was disproven by
experiment (`ad2d738`) — a sink survives projection at 68% because `ITER = 8` is nowhere near
converged. (2) A test can pin the flattering pixel: the VU lamp's original assertions covered only the
hotspot, which sits in the bottom rows furthest from every marking, while the scale band the user
actually reads had gone 16% darker. Both were caught by adversarial review, not by the build.

### v0.2.14 — previous release

**Released:** **`v0.2.14`** (2026-07-26), tag `v0.2.14` @ `aa8580f`, full version **`0.2.14.349`**; three
installers on GitHub release `v0.2.14` — x64 `35,334,214` / arm64 `30,189,007` / universal `63,226,356`
bytes — **two appcasts** (`0.2.14.349`) committed @ `d594fd0` and LIVE (both enclosures HTTP 200).
Owner-signed on the Mac; each signature byte-length-verified against the built file, and the two
appcasts cross-checked so the x64/arm64 signatures cannot be swapped.

⚠️ **The biggest single release of the 0.2.x line, and the least runtime-verified** — six features plus a
background network worker. Mitigated by design, not by luck: the dead-link checker is **inert unless its
beta flag is ticked**, and the glass overlay is **off at strength 0**, so a user who touches neither gets
0.2.13 plus bug fixes. Owner did verify the System dialog, the PIP toggle, VU, glass and **a real
dead-link sweep** (31 alive / 22 dead / 53 written on a 442-channel library) before the cut.

**Features**
- **⚙️ Settings ▸ System…** (`0ff649e`) — the first "organise the settings" dialog, built to grow:
  titled sections, themed hairlines. Carries the **log level** and the **beta-feature switchboard**.
- **📊 Log levels** (`0ff649e`, instrumented `61773c4`) — standard Trace/Debug/Info/Warn/Error with a
  runtime threshold. **Header-only by necessity**: mac implements the same `common/platform/Log.h`
  (`mac/platform/Log.mm`), so adding a function to it would break their link — the levels, threshold and
  codec are inline over a function-local atomic, and **neither platform sink changed**. Deliberately **no
  "Off"**: the log exists so a tester can send it after a failure. Instrumented in the same release
  because levels with no call sites are a setting that lies — DEBUG carries every libVLC state
  transition + the scheduler verdict, TRACE the 250ms flow snapshot.
- **🧪 Beta feature flags** (`common/core/FeatureFlags.h`) — opt-in switches persisted as `beta_<id>`,
  default OFF, with the rules written into the header (inert when off; the id is stable because it is
  the settings key; delete the flag *with* its branch when a feature graduates). This is what let the
  dead-link checker ship at all.
- **🔗 Dead-link checker, BETA** (`081faf9` core + `57d4fbb` sweep) — `common/core/DeadLinkCheck` holds
  the verdict logic (17 selftests) because that is where the danger is: a bad sweep marks thousands of
  channels dead and, with "Hide unavailable" on, **the library disappears**. Hence: a failure to CONNECT
  is *Inconclusive*, never Dead; **an entire sweep is discarded unless enough of it reached a server**;
  5xx is inconclusive (an overloaded provider recovers); only 401/403/404/410 mark Dead. New
  **`httpProbe()`** seam — `httpGet` is unusable here (a live stream's body never ends), so it reads
  headers and closes; GET not HEAD (edges mishandle HEAD), redirects NOT followed (a 3xx is healthy).
  Bounded at 250/run, sequential, paced — a greedy 12k sweep reads as a scraper and an Xtream
  connection cap can kick the user's actual playback. Own sqlite connection; joined in `WM_DESTROY`.
- **📻 VU needle meter** (`10dae97`, thinned `c87a6e9`) — fifth `MeterStyle`. **Real ballistics**: ~300ms
  and *symmetric*, because a VU lags going down as much as up and that weight is the instrument; reusing
  the LED attack/decay envelope would make it a peak meter. Integrates every tick regardless of the
  active look, so switching to it shows a needle already tracking.
- **🔍 Glass overlay** (`6c7ec62`, reworked `d3ce334`) — `common/ui/GlassMask` precomputes two LUTs
  applied over the finished frame (frame-invariant ⇒ zero per-frame maths). Now a **beveled plate**:
  clear centre, all the optics in a rim band. A first attempt spread gradients over the whole face and
  read as *blur* — the owner's Phase Linear 400 photo is why. Free win alongside: `MiniMeter` now caches
  a DIB instead of creating+destroying one **every paint** (~240 GDI ops/sec across the tray).
  **Global** strength, not per-meter: the buffer meter has no `MeterConfig`, the knob band is full, and a
  6th `MeterTuning` field would break mac's exact-arity parser.
- **🖼️ PIP "Keep above other apps"** (`9774418`) — off ⇒ topmost only while RabbitEars is foreground,
  re-raised on `WM_ACTIVATEAPP`. It can never be left plain non-topmost: an owned popup then composites
  UNDER the main window's libVLC D3D surface.
- **⏭️ `ScheduleStatus::Skipped`** (`af6592d`) — cancelling one airing of a series rule already behaved
  correctly (any-status row is a tombstone); it just *said* "Cancelled", which reads as if the series was
  stopped. The mac `switch` case shipped in the SAME commit — it is `-Wswitch -Werror` there.

**Fixes**
- **PIP no longer hijacks the main view on exit** (`a4b93aa`) — `applyViewMode` carried the active pane's
  stream into pane 0, right for Split (equal tiles) but wrong for PIP (a secondary overlay).
- **PIP resize snaps to the video aspect** (same commit) — needed a new worker-sampled
  `VlcPlayer::videoSize()` (libVLC calls stay off the UI thread), published as one atomic.
- **Unicode case-fold for rule matching** (`c7ec7b0`) — `foldTitle` used `towlower` under a comment
  claiming it handled "any language". The app never calls `setlocale()`, so it folded **only A-Z**: a
  Contains rule for "café" never matched "CAFÉ", "тв" never matched "ТВ" — silently. Explicit simple-fold
  table (Latin-1, Latin Ext-A, Greek, both Cyrillic blocks); `common/` cannot use `CharLowerW`/ICU.
- **`PRAGMA busy_timeout`** (`bb110c3`) — five writers discard `stepDone()`, so a contended write was a
  *silently lost setting*. Also the prerequisite for any second connection.

**Deferred with evidence — TV Guide off-thread.** Both premises fail: `programmesInWindow` **already**
bounds by the window (the backlog's "cheap win" is a no-op, and index variants all regressed in
benchmarking), and the owner's library has **zero `epg_programmes`**, so the diag timer has never fired.
See `BACKLOG.md` for how to get a real number before anyone tries again.

---

📚 **Release history for v0.2.13 and older lives in [`HANDOVER-ARCHIVE.md`](HANDOVER-ARCHIVE.md).**
Split out 2026-07-26 (this file was 1,503 lines). The archive holds the contemporaneous per-release
accounts back to v0.1.1 — read it before re-litigating an old decision or re-trying an idea that may
already have been reverted. Everything needed to work on the app *today* is in this file.

## Engine (Layer A — complete, /W4 clean, proven)

- **M3U/M3U8 parser** (`src/core/M3uParser`): full EXTINF dialect — `#EXTM3U`
  (+ `x-tvg-url`/`url-tvg`), `#EXTINF` attrs (`tvg-id`/`-logo`/`-name`/`group-title`/
  `tvg-chno` + inline `http-user-agent`/`http-referrer`), `#EXTGRP`, `#EXTVLCOPT`,
  bare-URL playlists. Splits the display name on the **first *unquoted* comma**;
  strips BOM; tolerates CR/LF/CRLF.
- **SQLite store** (`src/db/Database`): typed DAO, RAII `Stmt` (bound params) + `Tx`
  (one `BEGIN IMMEDIATE` bulk insert), WAL + FK, schema on open,
  `%LOCALAPPDATA%\RabbitEars\rabbitears.db` (env `RABBITEARS_DATA_DIR`). Idempotent
  refresh via `ON CONFLICT(playlist_id,stream_url)` preserving favourite + LCN.
  `deletePlaylist`, `channelsByGroup/Playlist`, `favourites`, `searchChannels`,
  `setDeadStatus`, settings K/V.
- **RabbitEarsCli** (`src/cli/RabbitEarsCli.cpp`): `--selftest` (30 assertions),
  `--fetch <url>` (WinHTTP + parse), `--import <url|file>` (into the DB; respects
  `RABBITEARS_DATA_DIR`), `<file.m3u>` dump. Runs headlessly in the sandbox — use it
  to repro core/parse/store issues.

## Release / auto-update (LIVE — see `docs/RELEASING.md`)

- Shares the **family Ed25519 key** with the siblings: the WinSparkle public key in
  `Win32/platform/Updater.cpp` (`sKPprIa95Hw+…`) equals the macOS `SUPublicEDKey`, so
  installers are **signed on macOS** with the same private key.
- **Per release:** bump version in 4 places (`APP_VERSION` in `cmake/AppVersion.cmake`
  — now the single source shared with the macOS build, `MyVer` in
  `packaging/installer.iss`, VERSIONINFO in `packaging/RabbitEars.rc`,
  `assemblyIdentity` in `packaging/app.manifest`) → commit → `scripts\build.cmd
  -DRABBITEARS_BUILD_GUI=ON` → `scripts\build-installer.cmd` (Inno at
  `%LOCALAPPDATA%\Programs\Inno Setup 6`) → **sign on the Mac**, from the repo root there:
  `SIGN_UPDATE="$(ls build-mac*/sparkle/bin/sign_update | head -1)" SIGN_UPDATE_ARGS="--account
  SQLTerminal" scripts/sign-release.sh RabbitEars-<ver>-setup.exe` (the script finds neither the tool
  nor the account on its own — 0.2.18) → verify each signature on Windows against `Updater.cpp`'s
  public key → `scripts\make-appcast.ps1 -Version A.B.C.<build>
  -SetupExe … -Signature <sig> -Tag v<ver>` → `gh release create` with the installer
  → commit/push `appcast.xml` (repo root). Build number = git commit count (baked
  after the commit).
- **Caveat:** 0.1.0 shipped before signing, so **0.1.0 users can't auto-update** —
  0.1.1 is the baseline; 0.1.1 users get 0.1.2+ automatically. **Authenticode**
  signing (to silence SmartScreen) is still not set up.

## Architecture (bottom-up)

```
sqlite3               third_party/sqlite/  vendored public-domain amalgamation. Static lib.
RabbitEarsCore        common/core, db,     platform-neutral engine: M3uParser, Database,
                      models, ui/DockLayout DockLayout. Links only sqlite3 (no UI/HTTP/OS
                                           paths). Built on BOTH Windows and macOS.
RabbitEarsPlatformWin Win32/platform/      Windows platform layer: Http (WinHTTP) + Paths
                                           (%LOCALAPPDATA% db path). Linked by CLI + GUI.
RabbitEarsCli         Win32/cli/           headless core tool (--selftest/--fetch/--import).
RabbitEarsRender      Win32/render/        headless render tool (GUI-gated): meters + skinned
                                           strip -> PNG from the real paint code. docs/PHOTOREAL.md.
RabbitEars            Win32/ (ui, WinMain, Win32 GUI (gated: RABBITEARS_BUILD_GUI).
 (GUI)                audio, platform/)    MainWindow (chrome+layout+wiring), ChannelGrid-
                                           Control (D2D grid), BufferMeter (LED), VlcPlayer
                                           (worker libVLC + recorder), Dialogs (About/prompt),
                                           Splash (layered), Log (diagnostics), Updater
                                           (WinSparkle).
```

> Layout note: the tree is split into `common/` (shared engine, both OSes), `Win32/` (the
> Windows app), and `mac/` (the macOS app), built by one unified root `CMakeLists.txt`. Some
> inline `src/...` paths elsewhere in this doc predate that split.

## Toolchain (non-obvious)

- **VS 2026 Community** at `C:\Program Files\Microsoft Visual Studio\18\Community`
  (MSVC + bundled CMake/Ninja). `cmake`/`cl` are **not** on PATH.
- Build: **`scripts\build.cmd`** (vcvars64 + PATH; `-G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo`). Pass extra args, e.g.
  `scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON`. From PowerShell, invoke it as
  `& "G:\RabbitEars\scripts\build.cmd" …` (a bare `scripts\build.cmd` after `;` can
  be mis-parsed as a module).
- **RelWithDebInfo, not Debug** (Debug CRT heap lock stalls the UI thread).
- **`LINK1168: cannot open RabbitEars.exe`** = an instance is running (usually the owner's) → close it
  with `WM_CLOSE` (`(Get-Process RabbitEars).CloseMainWindow()`), then rebuild. **Never force-kill it**:
  a normal close is the only path that finalises an in-progress recording (an mp4 without its moov atom
  is unplayable).
- Static CRT (`/MT`) — the exe needs no VC++ redist.

## Build, test, verify

```
scripts\build.cmd -DRABBITEARS_BUILD_GUI=ON -DRABBITEARS_THEME_ENGINE=ON   :: GUI + tools (libVLC once)
build\Win32\RabbitEarsCli.exe --selftest          :: the full core selftest (~6 s; must print ALL PASS)
build\Win32\RabbitEarsCli.exe --import <url|file> :: exercise fetch+parse+store headlessly
build\Win32\RabbitEarsRender.exe <outdir>         :: meters + skinned strip -> 56 PNGs (~10 s)
build\Win32\RabbitEars.exe                        :: the app (owner runs; the sandbox can't)
scripts\build-installer.cmd                       :: -> build\installer\RabbitEars-<ver>-setup.exe
```
Always build BOTH theme flags before committing (`-DRABBITEARS_THEME_ENGINE=OFF`, then `=ON`, leaving
the cache at ON). The render tool is how a visual change gets looked at before the owner sees it:
render before, change, render after, compare — see `docs/PHOTOREAL.md` for what it can and cannot show.

## Gotchas to carry forward

- **libVLC 3.x `stop()`/`release()` are SYNCHRONOUS/blocking** — keep ALL media-player
  lifecycle on the `VlcPlayer` worker thread (both `mp_` playback and `rec_` recorder).
- **libVLC event callbacks run on a libVLC thread** — only atomics + `PostMessage`.
- **`set_hwnd` before `play()`** or libVLC opens its own top-level output window.
- **libVLC `i_read_bytes` is 0 for HLS/adaptive** — don't trust the input-byte
  counter for those; consumption (`i_demux_read_bytes`) is the reliable rate.
- **VLC sout single-quoted values**: a literal `'` must be **doubled** (`''`), else
  the chain parser truncates the path. Sanitize filenames; double quotes in the dir.
- **Playback uses the GPU by default** (DXVA2/D3D11VA decode + Direct3D11 vout — we
  don't override `--avcodec-hw`/`--vout`). Recording is a **stream copy** → no
  decode/encode → no GPU.
- **`WM_CTLCOLORSTATIC` must return an opaque themed brush + `SetBkColor`** (else
  ghosting / broken ClearType).
- **`EnableWindow(mainHwnd, FALSE)` doesn't cascade** to the custom command bar —
  track `busy` explicitly during playlist fetch.
- **Modal dialogs must read their controls BEFORE `DestroyWindow`** — the Add-Playlist
  prompt bug was reading the edit box after destroy → empty URL → silent no-op.
  (Fixed; watch for the pattern in `Dialogs.cpp`.)
- **libVLC is LGPLv2.1** — dynamic-link + ship unmodified DLLs/plugins; include the
  attribution; no GPL-only plugins.
- **WASAPI process-loopback needs a Win11-era NTDDI** — `AUDIOCLIENT_ACTIVATION_PARAMS`
  et al. are `#if`'d out at the project-wide `NTDDI_VERSION=0x0A000006`, so
  `SpectrumTap.cpp` `#undef`+`#define`s `NTDDI_VERSION 0x0A00000C` **before** the first
  Windows header. Runtime still degrades gracefully on older Windows (meter idle).
- **`themeBrush()` caches only 12 colors and LEAKS beyond that** — the LED meters draw
  every cell with the **GDI DC brush** (`SetDCBrushColor` + `GetStockObject(DC_BRUSH)`),
  never `themeBrush`, so many per-cell colors cost no allocations.
- **Stop the `SpectrumTap` before the meter HWNDs die** — its capture thread pushes to
  `meterSpectrum`, so `WM_DESTROY` calls `spectrumTap.stop()` (joins the thread) first;
  child windows are destroyed only after the parent's `WM_DESTROY` returns.
- **`make-appcast.ps1 -Tag` defaults to `v<Version>` — i.e. `v0.2.12.325`, NOT `v0.2.12`.** Omit it and the
  enclosure URL points at a release tag that doesn't exist, so every auto-update 404s. **Always pass
  `-Tag v<marketing.version>` explicitly** (bitten twice; `docs/RELEASING.md` step 5 now shows it).
- **`build-arm64.cmd` needs the right vcvars for the HOST** — `vcvarsarm64.bat` is the ARM64-**native**
  toolchain and exists only on a Windows-on-ARM box; on an x64 dev machine the same VS component installs
  `vcvarsamd64_arm64.bat` (x64→ARM64 **cross**). The script prefers native and falls back to cross (0.2.12).
  Both target ARM64 — `LibVlc.cmake` keys on `CMAKE_CXX_COMPILER_ARCHITECTURE_ID` (the **target**), not the
  host, so the arm64 libVLC/WinSparkle slices are picked either way. **Verify the output is really ARM64**:
  PE machine at `e_lfanew+4` should be `0xAA64`.
- **"Every modal disables the main window" is ALMOST true — two surfaces are MODELESS**: the TV Guide
  (`epgGuideOpen()`) and the topmost "please wait" box (`st->loadingDlg`, with `st->busy` for its worker).
  Anything gating on `IsWindowEnabled(hwnd)` must name those explicitly. Worse, a dialog opened *from* the
  live guide calls `EnableWindow(mainWnd, TRUE)` on exit, which can **re-enable the main window underneath
  an unrelated nested modal loop** and silently break its modality.
- **`KillTimer` does not purge a `WM_TIMER` already posted** — if the handler then runs a nested modal
  loop, that stale message can re-enter it. Use a re-entry latch (see `maybeShowSupportPrompt`).

## Backlog

Moved to **[`BACKLOG.md`](BACKLOG.md)** — the parked work, headlined by the **theme engine** (0.2.x
epic: full reskin + selectable D3D11/shader skins). Also there: JSON profiles, scheduled recording,
recording formats, EPG + dead-link checker, resume-last-channel, named saved layouts, group-title
country fallback, the dialog work-area clamp + shared-`runModalLoop` cleanup, DPI-change relayout,
Authenticode + portable-zip. `HANDOVER.md` stays focused on **current state**.

## Git state

Owner-owned repo `github.com/arcanii/RabbitEars`. **Development is on `main`** — `0.2.15-dev` was
merged and deleted, and the four stale mac-side PR branches were pruned with it, so `main` is now the
only branch local and remote. (All five were verified fully merged with zero unmerged commits and no
open PRs before deletion; four of them belonged to already-merged macOS PRs #33/#34/#35/#42.)
Tags `v0.1.0`…**`v0.2.18`** (0.2.18 @ `3c14828`, `0.2.18.426`, published 2026-09-24 — appcasts
pending, see "Current state"); **the next tag is `v0.2.19`** (or `v0.3.0` if series lands first).
**v0.2.17 released @ `3660441`** (full `0.2.17.388`; both appcasts @ `fe3d872`) — the big-library
release: canonical `stream_url` + **schema v9** (the duplicate-films merge), the grid row cap, skip
back/forward, and the v8-migration fix. Prior: **v0.2.16 released @ `d83b002`** (full `0.2.16.377`;
both appcasts @ `0344b54`) — the Xtream VOD release: movies sync into a 🎬 Movies root, player seek +
scrub bar, buffer-meter glass. Prior: **v0.2.15 released @ `1324f5f`** (full `0.2.15.365`; both appcasts @
`77035ed`) — the instruments release: VU relit from a bottom bulb + optional blue lamp, framed glass,
the data-flow tank reoriented to pour in at the top and drain at the floor, user-selectable fluid
colour, look-aware meter knobs, About tip section, PIP menu + swap, dead-link checker out of beta.
Prior: **v0.2.14 @ `aa8580f`** (`0.2.14.349`, appcasts @ `d594fd0`) — System settings, beta flags, the
beta dead-link checker, VU + glass meters, two PIP fixes. Prior: **v0.2.13 @ `93dea6f`** (`0.2.13.329`, appcasts @ `d57997f`) — Ko-fi; **v0.2.12 @
`76c6a46`** (`0.2.12.325`) — Buy Me a Coffee + CJK QA + Xtream countries + schema v7. The **mac line is
decoupled** (`if(APPLE)` in `cmake/AppVersion.cmake`, currently **0.2.17** — the mac line caught up in
Aug 2026, and declares the theme engine *N/A by design*) and the mac team pushes to
`main` too, so **`git fetch` + rebase before every release** — the 0.2.0 cut had a push rejected mid-flight
by a concurrent mac commit. **Release-tooling note (0.2.2):** this machine now
has **`gh` CLI (2.96) AND Inno Setup**, so the whole release ran locally: commit → push → build →
`build-installer.cmd` (Inno) → `gh release create v0.2.2` + upload → `make-appcast.ps1` → commit/push
`appcast.xml`. **Only EdDSA signing stays on the Mac** (`scripts/sign-release.sh` → `sign_update` + the key).
The `raw.githubusercontent.com` feed caches ~5 min (`max-age=300`) — an installed app won't see the new appcast
until that expires (looked like "0.2.1 doesn't detect the update" for a few min; not a bug). Prior: **v0.2.1 @
`79ab12c`** (`0.2.1.148`). Earlier: **v0.2.0
@ `343aa0e`** (`0.2.0.107`; appcast @ `7b3946a`), the theme engine, theme-ON by default. The `theme-engine`
branch was **merged to `main` + deleted** (only
`main` remains; PR #16 superseded + closed). **The macOS team pushes to `main` too** (mac Phase-1), so
**`git fetch` + rebase before a release** — the 0.2.0 push integrated a concurrent mac commit mid-flight
(the first push was rejected until re-fetched).
**As of 2026-09-23** the search debounce (`b5c016f`) and the lost-status-write fix (`b4b3e4c`, on top
of the mac team's 30 commits — their VOD, seek layer and **v0.2.17-mac**) are both **pushed**; the
photoreal Phase 0 render tool came after and may not be committed yet — **run `git status` and
`git log origin/main..` to see.** Whatever
this paragraph says, it goes stale the moment anyone pushes — the repo has two writers — so **re-verify
`git ls-remote origin refs/heads/main` == HEAD immediately before building anything for a release**,
because the build number is the commit count. That check has caught a real blocker on two consecutive
cuts.
Build number = git commit count, baked at CMake configure time
**after** the commit — so a build must follow the release commit to stamp the matching `0.2.0.<count>`. Commit/push only when the
owner asks; stage **specific paths** (the owner keeps adding `art/*.png` — never `git add -A`); end
commit messages with the Co-Authored-By trailer.

## Immediate next steps (pick up here)

### 📦 What went into 0.2.16 (2026-07-27 → 28) — six commits, all now released

`APP_VERSION` was already `0.2.16` when these landed, so the cut needed no bump. Both theme flags
build clean at /W4 and `--selftest` is ALL PASS after every one of them.

| commit | what |
|---|---|
| `ffb69dc` | groundwork: player seek + scrub bar, `common/core/Json`, schema v8, buffer-meter glass, the `--xtream` recon probe |
| `1a486be` | gate 2 — [`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md), written off the real provider run |
| `538f0b2` | the Xtream client core: `common/core/XtreamClient`, DAO plumbing, **APP_VERSION → 0.2.16** (its message says "0.2.17 core" — that was the pre-collapse plan) |
| `1d59783` + `7751cdc` | the perf work: country views made immune to VOD size, then the **Movies nav root** data model |
| `e4e01a7` | **the VOD UI**: Movies nav root wired, `Win32/ui/VodSync.{h,cpp}`, Settings action, 15 i18n keys (message says "0.2.17" — same reason) |

#### The Xtream client core (`538f0b2`)

`common/core/XtreamClient.{h,cpp}` — **pure, no network by design.** The caller fetches and hands
bytes in, because with `max_connections: 1` whether a sync may run at all depends on whether a pane
is playing, and only the caller knows that. Burying the fetch would hide that decision somewhere it
cannot be made correctly.

`Database` gained `retireMissingChannels()` (the other half of a sync — a catalogue churns, so
without it the library only ever grows) and `bulkInsertChannels` now carries `kind`/`added_at`.

#### The Movies nav root (`7751cdc`) — owner's call, data model only

Movies live under **one "Movies" root**, not as ~67 sibling categories in the live tree.
`listGroups()` is now live-only; `listVodGroups()` / `moviesByGroup()` / `allMovies()` serve the new
root. The two trees are separate namespaces — a category name present on both sides returns only its
own rows to each accessor, which is pinned by selftests.

**⚠️ The nav is NOT wired yet.** `ViewKind` has no `Movies`/`MovieGroup`, `refreshNav` does not build
the root, and `loadForFilter` cannot select it. The DB is ready; the UI is not.

#### Perf — measured, not guessed (`1d59783`, `7751cdc`)

New **`RabbitEarsCli --benchdb [movies] [live]`** (defaults to the owner's real 43,599 / 442). The
design doc called this the epic's biggest risk and said measure BEFORE the sync ships. It was real:

| query | live-only | +43,599 movies | after |
|---|---|---|---|
| `listCountries()` | 0.12 ms | **13.01 ms** | **0.09 ms** |
| `channelsByCountry()` | 0.13 ms | **6.38 ms** | **0.12 ms** |
| `listGroups()` | 0.11 ms | **8.21 ms** | **0.07 ms** |

**Every live-TV path is now immune to VOD library size.** The bigger half was CORRECTNESS, not speed:
a movie has no country, but its category name goes through the country-prefix fallback — a provider
with `NL - FILMS` categories would have filed all 43,599 films under the Netherlands. The owner's real
categories dodge it only because `VOD` is three letters, which is luck, not design.

Left as measured, all on non-interactive paths: `moviesByGroup()` 2.38 ms (the real browse path),
`listVodGroups()` 9.7 ms (nav refresh), `allMovies()` **77 ms** (materializes 43,599 rows),
`searchChannels()` 7.9 ms (movies stay searchable on purpose), bulk insert ~260 ms.

**A dead end, recorded so nobody retries it:** widening the VOD partial index to
`(kind, group_title)` and inlining `kind` as a literal did NOT fix `listVodGroups` (8.9 → 9.7 ms,
noise). The composite index is kept because it does serve `moviesByGroup`. Not worth chasing further
on a once-per-nav-refresh path.

- **Player seek + scrub bar + time readout** — `VlcPlayer` gains `timeMs/lengthMs/isSeekable/seekTo`,
  sampled on the worker beside `videoSize()` and published as atomics. The bar appears only when
  libVLC reports the media seekable with a real length; seeks commit on `TB_ENDTRACK` only (one seek
  per gesture, not per drag tick — a network stream would re-buffer continuously).
  ⚠️ **"Invisible on live IPTV" is libVLC's judgement, not an invariant we enforce.** An HLS feed with
  a DVR window can legitimately report seekable, and a fair number of IPTV channels are exactly that.
  Showing a scrub bar there is arguably CORRECT — but it means the "strip is unchanged for existing
  users" claim needs an eye on a real channel list. **This is the top thing to check on device.**
- **`common/core/Json.{h,cpp}`** — hand-rolled JSON reader (house style). Tolerant exactly where
  panels are non-standard, strict on structure. 32 selftests.
- **Schema v8** — `channels.kind/duration_sec/resume_sec/watched/added_at`. A movie is a channel row,
  not a second table. All columns default to the live-TV answer; verified by migrating a hand-built
  v2 DB and asserting a pre-v8 row comes back `kind=Live`.
- **Buffer-meter glass** — the backlog's likeliest answer to the 0.2.15 "bezel needs work". Grid
  geometry lifted into `BufferMeter.h` so `--selftest` can pin the one property that cannot be
  eyeballed: glass OFF is bit-identical to the pre-glass renderer at every size/DPI.
- **`RabbitEarsCli --xtream`** — the reconnaissance probe (below).

**Three defects an adversarial review caught in the seek work, worth remembering:** pressing **Stop**
stranded the scrub bar on screen forever, because `doStop()` DETACHES its libVLC callbacks before
teardown (deliberately, so a dying stream cannot post stale events) — so **no event follows a stop**
and `sampleStats()` has already quit. Nothing existed to retire the bar. Also: a pane switch between
two *seekable* panes never flipped visibility, so pane A's seek target drove pane B's thumb; and a
Seek queued behind a Play landed the old film's position on the new stream (now stamped with a stream
generation). The pattern in all three: **state cleared only on a visibility TRANSITION is not cleared
at all when the transition doesn't happen.**

### ✅ The VOD UI is DONE and COMMITTED (`e4e01a7`, pushed) — ▶️ PICK UP AT "What still needs the owner"

All three remaining items landed. Both theme flags build clean at /W4 and `--selftest` is ALL PASS.
**None of it has been seen running** — see "What still needs the owner". This completes 0.2.16;
nothing is left to build before the release except the owner's on-device pass.

1. **Movies nav root — settled as CATEGORIES-ONLY.** `ViewKind` gained `Movies` + `MovieGroup`.
   `refreshNav` builds the root from `listVodGroups()` and **omits it entirely when there are no
   movies**, so the sidebar is byte-identical for every existing live-TV-only user. Unlike
   Groups/Countries the root IS selectable (`ViewKind::Guide`'s precedent): selecting it clears the
   grid, expands its category list and says so, rather than leaving the previous view's rows sitting
   under a heading that says Movies. `MovieGroup` → `moviesByGroup()` (2.4 ms). **`allMovies()` is
   never called from the UI** — 43,599 rows is not a browsable view.
2. **`Win32/ui/VodSync.{h,cpp}`** — own sqlite connection, joined in `WM_DESTROY`; three requests per
   playlist (auth probe + categories + streams, ~13.5 MB); `bulkInsertChannels` then
   `retireMissingChannels` behind a trust guard that INSERTS but refuses to RETIRE below 50% usable
   items. Syncs every enabled Xtream playlist in one run.
3. **Settings ▸ Channels ▸ "Sync movies from provider"** — `ID_VOD_SYNC = 2027`, `WM_APP+8/+9`,
   15 i18n keys × en/ja/zh-Hant (zh-HK inherits).

**The three constraints, and how they are actually met** (all three needed more than the obvious):

- 🔴 **`max_connections: 1`.** The gate refuses on `isPlaying()` **OR `isRecording()` OR
  `nowPlayingId != 0`**, across `panes` *and* `dyingPanes`. All three terms are load-bearing:
  `rec_` is a **second libVLC player with its own socket** that `doStop()` never touches, so a
  scheduled recording runs with nothing "playing"; and `playing_` is set from the
  `libvlc_MediaPlayerPlaying` **event**, so it is false for the whole open/buffer window while the
  socket is already claimed. The gate is also **not start-only** — `playChannelInPane`,
  `onToggleRecord` and `onSchedulerTick` all call `cancelVodSync()`, because the user's stream
  always wins. Finally the sync and the **dead-link sweep now exclude each other** (both refuse, and
  the menu greys both while either runs): they are the two provider-facing workers, and a sweep
  whose probes get refused for capacity can persist `dead_status=Dead` on live TV.
- 🔴 **`retireMissingChannels()` reentrancy** — worker has its own connection; joined at
  `WM_DESTROY` **after** the players, see the shutdown-order note below.
- 🔴 **Trust** — the guard reads `XtreamVodResult`'s counts in 64-bit arithmetic (`total` is
  provider-controlled, and the multiplication is the one place a broken panel could overflow into
  *deleting* the library). A run that refuses retirement says so in its own status message.

**Three defects three review rounds caught that are worth carrying forward as patterns:**

1. **A fix can be a regression.** Moving `armExitWatchdog` ahead of the worker joins (to stop them
   skipping the wake-task registration) put two *unbounded* joins inside a 4 s budget sized for
   libVLC teardown — so `ExitProcess` would fire mid-join and skip `player.shutdown()`, **the only
   thing that finalizes an in-progress recording** (an mp4 with no moov atom is unplayable). The
   order is now: wake-task registration → cancel both workers → arm watchdog → libVLC teardown →
   *then* join the network workers. **Recordings finalize before anything unbounded is waited on.**
2. **"Survive the error" can be worse than failing.** An unparseable `get_vod_categories` was
   originally survived by falling back to one group — but `bulkInsertChannels` writes
   `group_title=excluded.group_title` **unconditionally**, so that would have rewritten all 43,599
   stored movies to "Movies" in one committed transaction and flattened the whole tree, on a
   transient hiccup, with no undo. It now aborts the sync.
3. **A modal loop pumps posted messages.** The playlist context menu bounds-checked its `navFilters`
   index before `TrackPopupMenu` and re-read it after. `WM_APP_VOD_DONE` is the app's first posted
   message that calls `refreshNav()` — so the menu's Rename/**Delete** could name a different
   playlist than the one right-clicked. The id is now read before the menu.

Also still open from the design doc, and **deliberately not decided here** — they are owner calls,
and the design doc's "success = … resume works" line depends on all three: where duration comes from
(the API has none for movies — cache `VlcPlayer::lengthMs()` at play time), the `watched` threshold,
and resume-prompt vs silent-resume. **As it stands the VOD feature is browse-and-play, not resume**,
and resume is listed under 0.3.0 anyway ("resume everywhere"). Decide whether 0.2.16 ships without
it — the recommendation is yes, since resume cannot be designed against a library nobody has used.

#### ⚠️ One measured regression this release ships with — an owner decision

**The search box is now 0.63 → 80.00 ms on the FIRST KEYSTROKE** (126×, `--benchdb`). `EN_CHANGE`
runs `searchChannels()` synchronously on the UI thread with no debounce, no minimum length and no
`LIMIT`, and the query carries no `kind` predicate — so one letter matches and materializes most of
a 43,599-film library. The 7.9 ms figure previously signed off here was measured with the term
`"Channel 1"`, which matches **zero** movies, so it timed the scan and none of the materialization;
`--benchdb` now reports both. Movies staying searchable is a deliberate design-doc position, and how
to degrade it (LIMIT / debounce / minimum length) changes what the user gets — so it is left for the
owner rather than settled here. See BACKLOG.

`allChannels()` (0.73 → **80.4 ms**) and `channelsByPlaylist()` (0.59 → **80.0 ms**) also grow with
VOD size. That one is a deliberate position — "the All view legitimately grows with the row count" —
and the post-sync UI is careful never to land on it. What was NOT deliberate and is now fixed: those
two views ORDER BY `kind` first, because an Xtream playlist and its VOD sync write to the **same
playlist row** and `bulkInsertChannels` restarts `sort_order` at 0 per batch, so 43,599 movies
numbered 0..43598 interleaved straight through 442 live channels numbered 0..441. For a live-only
library every row is `kind=0`, so the ordering is byte-identical — macOS included.

### 🎬 Xtream VOD — both gates CLOSED, and what reconnaissance found

`RabbitEarsCli --xtream` ran against the owner's real provider (2026-07-27): `player_api.php` **works**,
**43,599 movies** / **13,152 series**, `container_extension` on 100% of movies, play URL **HTTP 302
reachable**, all 8 bodies parsed cleanly. The design doc is
**[`docs/XTREAM_VOD.md`](docs/XTREAM_VOD.md)** — written off measured numbers, with the shared-core
boundary flagged to the macOS team in its §2.

**Three findings changed the plan** (full detail in BACKLOG + the doc): **`max_connections: 1`** is the
governing constraint; **movies carry NO metadata** (`get_vod_info` returns 204 bytes with an empty
`info`, so duration must be cached from `lengthMs()` at play time); and **poster art inverts** —
`stream_icon` is empty on ~90% of movies but series `cover` is populated, so **movies stay in the text
grid and posters become a SERIES feature**.

⚠️ **The biggest risk is not VOD** — 43,599 rows is ~4× the current library, in the same `channels`
table that already needed a SQLite scalar to keep the country filter under ~30 ms/keystroke at 14k
rows. ✅ **Re-measured with `--benchdb` before shipping — but AGAINST THE WRONG SHAPE.** At the design
doc's 43,599 movies + 442 live, the country/group paths came out immune and only the search box
degraded (0.63 → 80.00 ms per keystroke). Then the on-device pass showed the owner's real library is
**410,147 rows, ALL `kind=Live`** — the provider lists movies and series flat in the m3u — where the
same queries cost **0.6–1.4 SECONDS**. ✅ **FIXED in 0.2.17** by the grid row cap (All Channels 1485 →
108 ms; search 1626 → ~134 ms). **Read the numbers and the analysis in BACKLOG before doing any more
perf work on this table — `--benchdb`'s DEFAULTS still model the 44k shape, not the real one.**

### 🔎 Search debounce — DONE, committed + pushed as `b5c016f`, needs the owner's keyboard (2026-07-28)

The first work after v0.2.17, and the "cheap next step" BACKLOG named after the grid cap. The search
box's `EN_CHANGE` used to run `searchChannels()` synchronously on the UI thread **on every
keystroke** — ~134 ms each on the real 411,149-row library. It now arms a 200 ms one-shot timer
(`kSearchDebounceTimer` = `0xA4`, a genuine gap; the main window's only other timers are `0xA1`-`0xA3`)
and the tick re-reads the box, so a typing burst is one query instead of one per letter.

Three files, +~80 lines, all Win32-only — **nothing under `common/`, so mac is untouched.**
`Win32/ui/MainWindow.cpp` (arm / tick / `WM_DESTROY`), `Win32/ui/MainWindowData.cpp` (`applySearch`,
`cancelSearchDebounce`, the cancel inside `loadForFilter`), `Win32/ui/MainWindowInternal.h`.

⚠️ **It reduces the NUMBER of stalls, not the length of one** — one search is still ~134 ms of frozen
UI thread. The floor is the `LIKE` scan; **FTS5 is still the real fix.** Don't read "search is fixed"
into this.

**Verified:** both theme flags build clean at /W4, `--selftest` ALL PASS, and an adversarial review
(4 lenses × independent refutation, 19 agents) raised 15 findings of which **0 survived**. Two
refutations conceded their mechanism while disputing the harm; both are now written into the code as
comments rather than left implicit. One of my own comment claims was false on inspection and was
corrected before review — it called `loadForFilter`'s load "uncapped" when `gridFilter(st, capped=true)`
plainly caps it. *Same trap as ever: a comment asserting behaviour is a claim, not a fact.*

🔴 **NONE of that is runtime verification.** `applySearch` sits in a GUI TU the CLI does not link, so
the change has **zero automated coverage** and the sandbox cannot launch the GUI. The owner's checks
are listed in BACKLOG — in short: typing feels smooth, results land after you stop, clearing returns
to the nav view, and a nav click right after typing shows the NODE rather than the search results.

### 🔴→✅ Lost schedule-status writes — FIXED and committed (unreleased), needs an on-device pass (2026-09-23)

The macOS team's 2026-08-09 finding (top of BACKLOG): `updateScheduleStatus` returned `void`, and
`planScheduler` learns that a *schedule* holds the recorder only from the row status. A `Recording`
write lost to contention (a VOD sync holding the writer lock past `busy_timeout`) left the row Pending
while the recorder ran, so the next tick re-started it over the file in progress, **leaving a
truncated fragment** — and again on every further lost write. If the retries kept losing, the row
ended `Missed` while the recorder ran on past the window. **The lost write itself was never logged.**
Windows was the more exposed platform because wake-to-record fires unattended.

**The fix, in one line each** (full rationale in BACKLOG):
- `updateScheduleStatus` → **`bool`** (shared; source-compatible for mac, which ignores it everywhere).
- **Persist `Recording` first, start the recorder only if it landed** — the new shared
  `beginScheduledStart`. This **departs from the proposed fix** (start, then stop on failure) because
  `startRecording` only enqueues; stopping afterwards would open a provider connection and leave a
  near-empty file for every lost write.
- The **startup reconcile retries** instead of latching after one attempt (a lost reset used to block
  every schedule until that row's stop time).
- **Lost terminal statuses are write-behind**: remembered, replayed each tick, overlaid on the rows the
  planner sees, and flushed once more at exit (best-effort). ⚠️ Each decision is pinned to its row's
  **identity**, not its id — SQLite reuses a deleted top rowid, and the first draft, keyed by id, would
  have stamped a stale "Cancelled" onto the next new schedule. **The adversarial review caught that,
  independently from all four of its lenses.**
- `deleteSchedule`, `deleteRule` and `clearPendingForRule` also return `bool`. A lost delete of a
  one-off Pending/Recording row falls back to an inert Cancelled. `deleteRule` is now **one
  transaction** (false = nothing changed; it used to be two DELETEs that could orphan rows). A rule edit
  **spares**, and a rule delete **defers**, any queued airing whose Skip/Cancel/Missed has not landed,
  so no re-expansion can re-create it. `addRule`'s failure is now checked.
- **Pre-existing fix, found by the review — a deleted series-rule airing could come back.** Deleting one
  *while it was recording* let the rule re-create it and restart the programme you had just deleted;
  and deleting its Cancelled/Skipped entry a *second* time did the same for any airing not yet over. A
  row whose series rule **still exists** is now never hard-deleted until its own window has ended: a live
  one becomes Cancelled; an already-decided one is kept and not relabelled, with a new status line (new
  i18n key `StatusAiringKeptRule`) saying it can't be removed until its scheduled recording time is over
  (nothing removes it automatically — Delete works after that). Rows of a *deleted* rule, and one-offs,
  delete as before; a row the Delete's listing misses is skipped, never deleted blind. The mac app
  already had this guard. Residual, pre-existing and shared with mac: another rule matching the same
  airing, or a guide refresh that extends the programme, can still re-queue it after the row is deleted.
- The wake task reads through the overlay, so **within the session** an un-landed cancel does not arm
  a wake. That lasts only as long as the in-memory overlay: after a relaunch that followed a pre-empted
  exit flush, the DB alone decides again.

**Verified:** both theme flags clean; `--selftest` ALL PASS with two new blocks (28 assertions). The
core of the first runs the **real** `beginScheduledStart` while a second connection holds
`BEGIN IMMEDIATE`, and checks the write genuinely waited ~5.4 s on the lock (so it cannot pass on an
unrelated failure), returned `NotPersisted`, never started the recorder, and left the row Pending for
a clean retry; the rest of that block covers the uncontended DAO results, `Started`, `RecorderFailed`
and a pure ordering pin. The second drives **real SQLite rowid reuse** through the real overlay/flush
functions and proves a new schedule that inherits a dead row's id is still **started by the planner**,
and that a flush stops writing after its first loss.
**Adversarially reviewed in rounds, each on the code as it then stood.** Round 1 (18 agents): 12
confirmed — 1 code defect found by all four lenses (rowid reuse), 1 further lost-write site, 1
wake-task gap, 9 over-claiming comments. Round 2, on the fixed version (13 agents): the core confirmed
sound, plus 9 low-severity findings (rule-edit gap, `deleteRule` orphaning, read-back fragility, a
selftest false-green, UI-thread stall amplification, 4 over-claims) and 4 doc residues. Round 3, on
the round-2 fixes (19 agents): 15 findings — the same gap on rule *delete*, `deleteRule`'s split case
(→ made transactional), a real **pre-existing** ordinary-path bug (the recording-rule-row restart
above), misleading log text, and doc/count fixes. Round 4, on the round-3 fixes (17 agents): 15
findings, 2 medium — both the SAME hole: round 3's restart fix covered only the first Delete, and a
second Delete on the resulting tombstone re-opened it. Also a finished recording being mislabelled
Cancelled, a rule edit that could silently skip its clear (a regression round 3 introduced), an owner
check that could not tell old from new, and doc gaps. All addressed. Round 5, on the rewritten delete
decision (14 agents): 12 findings, 2 medium — both the same misleading status line on a kept row (→ a
new i18n string); also relabelling of already-decided rows, a silenced "stale row" log line, the
active row's saved copy not used as a fallback, and residual gaps now documented. All addressed.
Round 6, on the round-5 fixes (14 agents): 12 findings, 2 medium — both the new string's wording (it
implied the row disappears on its own, and blamed "the rule" even when the rule was deleted); also a
regression (rows of a deleted rule could not be removed for up to 14 days → keep only while the rule
exists), a row the listing missed still being deleted blind (→ skipped), a stale-reset id dropped on
one missed read (→ kept), and comment/doc wording. **All applied; by the owner's call the change was
committed after round 6's fixes WITHOUT a seventh round** — so the round-6 fixes themselves are
reviewed only by construction (each is the reviewers' own suggested correction), not re-reviewed.
**The pattern worth knowing:** the change's own lost-write core has held since round 2; what kept
producing findings was the adjacent manager glue, where each fix exposed a neighbouring pre-existing
assumption. If a problem turns up in the Scheduled Recordings **Delete** path, start there.
**Not verified, and not verifiable here:** only `updateScheduleStatus` via `beginScheduledStart` is
tested under real contention. Not tested: the lost paths of `deleteRule` / `deleteSchedule` /
`clearPendingForRule`; the Win32 glue in `onSchedulerTick`, `applyViewMode`, `syncWakeFromSchedules`,
the `WM_DESTROY` flush and the two managers (GUI code the CLI does not link); and any real recording.

**What the owner must check on device**, in value order (Settings ▸ System… → log level **Debug**
first, so the scheduler's lines reach the log):
1. **The ordinary path is unchanged — this matters most, because the start order changed.** Schedule a
   short recording (Settings ▸ Scheduled Recordings… ▸ New…) starting a minute or two out. It should
   start on time, show `Recording` in the manager, finish as `Done`, and the file should play to the end.
   The log should read `scheduled recording started` then `scheduled recording finished`.
2. **Cancel one mid-recording** from the manager: the recorder stops, the row reads `Cancelled` (or
   `Skipped` for a series-rule airing), and the partial file plays.
3. **The contention path, if a live Xtream line is available:** start a movie sync (Settings ▸ Channels
   ▸ Sync movies) so that a scheduled recording falls due mid-sync. The scheduler cancels the sync; the
   write should simply land (mac measured the contention at ~0.2 s). If the log instead shows
   `Recording status write was lost … will retry next tick`, the next tick (~30 s later) must start it,
   and there must be **exactly ONE** file for that airing — two short files is the old bug.
4. **Close mid-recording, relaunch inside the window:** a normal close finalises the file; on relaunch
   the stale `Recording` row is reset and the airing resumes into a new file (or is marked Missed if the
   window has passed).
5. **Edit a series rule** (Settings ▸ Recording Rules… ▸ Edit, change the padding): its upcoming airings
   are re-queued with the new padding, an airing you had **Skipped** stays skipped, and deleting a rule
   still removes its queued airings but keeps recordings that already ran.
6. **Delete a series-rule airing while it is recording** (Scheduled Recordings… ▸ Delete) — pick a
   programme with well over a minute left. The recorder stops and the row stays listed as `Cancelled`.
   **Press Delete on it again:** it must stay listed, and the status line must say it can't be removed
   until its scheduled recording time is over. Then FORCE a rule
   expansion — deleting does not trigger one, and on its own it runs only every 15 minutes: close the
   manager, open Settings ▸ Recording Rules…, and turn any enabled rule off and back on. Within one tick
   (~30 s) the airing must **not** start recording again. (Before this change, that expansion re-created
   the row and restarted the programme.) Without forcing an expansion you would have to watch for 16+
   minutes, on a programme with more than that left.

Any log line containing **`database busy`** or **`still unlanded`** is worth sending (search
case-insensitively — the start-write line says `was lost` in lowercase). Before this fix a lost
scheduler write was completely silent, so nobody knows yet how often it happens. Watch in particular
for **`stale Recording row could not be reset`**: while it keeps repeating, the startup reset has not
landed and no schedule can start. `recording rule delete DEFERRED` is a refusal this change introduced
(not a formerly silent failure): the rule is held back until an airing's status lands.

### 🖼️ Photoreal skins & meters — Phase 0 DONE, Phase 1 fixed (uncommitted), the rest waits on six owner decisions (2026-09-23)

The owner asked to make the skins and meters "more photorealistic". Research (five investigations and
a completeness critic) and the proposal are in **[`docs/PHOTOREAL.md`](docs/PHOTOREAL.md)** — read its
constraints section first: a tray meter's dial is **26 px tall at 100 %**, so realism has to come from
light, colour and crisp 1-px detail, and the biggest lever is making the meters bigger (an owner call).

**Phase 0 — `RabbitEarsRender` — is built and verified:** `Win32/render/RabbitEarsRender.cpp`, a
GUI-gated console tool. It renders every meter look x kind, the tank and every skinned strip to PNG
by driving the meters' real WndProc on hidden windows. Its three production seams are read-only or
test-only (`miniMeterSnapshot`, `bufferMeterSnapshot`, `skin::setStripClockForTest`) and the app never
calls them. Output: 56 PNGs, byte-identical to the scratch proof of concept (itself pixel-identical to
the app's compiled objects) and across runs. Both theme flags build clean; the classic build renders 12.
**Adversarially reviewed** (2 lenses, 25 findings, 24 upheld on verification, 1 refuted): none touched
the production seams; all were doc overstatements in PHOTOREAL.md and tool hardening (`--strip-only`
dropped the VU strip, `--time` unvalidated, unchecked GDI+ startup, a crop guard, `.gitignore` for
`render-out/`), all applied — the render output stayed byte-identical. Two were answered with
documentation instead of changed output, to keep that byte-identity: the `default` strip shows all four
meters (not the default install's two), and the tray-sheet title prints `windowBg` as a raw COLORREF
(`0x00BBGGRR`). Either is a one-line change if a future sheet needs it — then re-baseline the PNGs.
**Not committed at the time of writing** — the owner commits.

**It found six existing visual defects** — Phase 1 candidates, listed in PHOTOREAL.md: the VU "red
zone" is near-white, the VU needle's shadow reads as a second needle, the tank's readout covers half
the tank, the Light skin's unlit LEDs are near-black, the Light skin's underglow is invisible, and
Tube/Scope glows bleed over the border.

**Phase 1 — the six defects — is FIXED in the working tree (2026-09-23), not committed.** Win32-only
(`MiniMeter`, `BufferMeter`, `Dialogs`, `underglow.hlsl`; nothing under `common/`). Each was rendered
before/after and compared pixel by pixel, per meter: on a dark panel LED/LCD/Tube are byte-identical
inside every dial, the dark skins' strips are byte-identical outside the meters, and the VU at 100 %
scaling differs only at the red zone. **Adversarially reviewed** (code + every claim against the
pixels): 1 medium — the Meters colour picker, seeded with a RESOLVED Light-skin colour, would have saved
it on an unchanged OK and broken the meters on the next dark skin — plus an idle Scope trace
half-clipped, a peak role that ran the Light Bitrate ramp backwards, and several over-claiming comments;
all fixed and re-verified. Both theme flags build clean, `--selftest` ALL PASS, the classic build's dark
sheets are still byte-identical to the engine's. Full table, the taste calls it leaves and one tiny
pre-existing gutter overlap it found: **PHOTOREAL.md "Phase 1"**.
⚠️ Phase 0 and Phase 1 touch the same files (`MiniMeter.*`, `BufferMeter.cpp`). To commit Phase 0 on
its own first, its exact state was saved as git tree **`228ddb5`** (all twelve Phase 0 paths, including
the untracked `render/` and `PHOTOREAL.md`) — `git restore --source=228ddb5 --staged -- <paths>` stages
Phase 0 without touching the working tree. (An unreferenced tree object: `git gc` may prune it after
~2 weeks.)

**Next:** the owner looks at the Phase 1 before/after sheet and the live tray, and answers the six
decisions at the bottom of PHOTOREAL.md (the size question matters most) — ideally with the Phase
Linear 400 photo, the mockups, and one real screenshot. Keep rendering before/after sheets for every
visual change and hand the owner the pair, not the live app.

### 🧪 Dev side profile — run a build beside the installed app (2026-09-23, uncommitted)

```
powershell -File scripts\run-profile.ps1            # build\Win32\RabbitEars.exe as profile "dev"
powershell -File scripts\run-profile.ps1 -Refresh   # re-take the library snapshot first (dev copy closed)
```
`RABBITEARS_DATA_DIR` pointed at `%LOCALAPPDATA%\RabbitEarsProfiles\<name>` makes an instance a
**side profile** (`Win32/platform/Profile.h`): its own single-instance mutex (hash of the data dir),
"RabbitEars · <name>" in the title bar, and it **never** touches the per-user wake-to-record task (the
toggle is greyed) or WinSparkle (per-user registry state; an update from a profile would run the
installer and close the installed app). A data dir that IS the default one is the normal install.
The script snapshots the real DB read-only (SQLite online backup, 0.5 s for 411k rows), strips
`scheduled_recordings` + `recording_rules` from the COPY, refuses an exe that predates profiles (it
would take the normal mutex and clear the real wake task), and validates `-Name`.
⚠️ **Gotcha that cost 25 minutes:** the dev tree is on `G:`, an SMB share, and launching an exe from
there through the SHELL (`Start-Process`, Explorer) raises an "Open File – Security Warning" that
blocks until someone answers it — it looked like the script hanging. The script now uses
CreateProcess (`UseShellExecute = false`). Still shared by both instances: the logo cache, the
recordings folder, and the provider's connection limit.
**Adversarially reviewed** (1 pass): 1 medium (an old exe would clear the real wake task — now
refused) + 3 low (unvalidated `-Name`; WinSparkle shared; the wake toggle promising a wake that could
not happen) + 2 nits — all fixed. **Snapshots of the reviewed trees** for splitting commits: Phase 0 =
`228ddb5`; Phase 0 + Phase 1 + this profile + the 0.2.18 bump = `de8eece` (unreferenced tree objects —
`git gc` may prune them after ~2 weeks; `git restore --source=<tree> --staged -- <paths>`).

### What still needs the owner

**0.2.17 shipped better verified than anything before it** — all four changes were confirmed on the
owner's real 411,149-row library BEFORE the cut, rather than after. What is left:

0. ✅ **THE FIRST REAL VOD SYNC RAN (2026-07-28) — and it works.** Owner's live provider:
   **43,606 movies, 67 categories**, all with group titles and `added_at` populated, the 🎬 Movies
   root appeared in the sidebar when it finished, films play, and seek/pause/scrub work on them.
   The headline feature of 0.2.16 is no longer unexercised.

   ⚠️ It also **doubled** the movie library — the m3u/sync `:80` mismatch — ✅ **FIXED by schema v9
   in 0.2.17**, which merged the 43,599 duplicates away on the owner's live database.

   🔴 **STILL OUTSTANDING, and it is now the single least-exercised path in the app: run the sync a
   SECOND time.** The first can only INSERT; the second is the one that calls
   `retireMissingChannels` in anger and can **DELETE**. *"Movie sync done — N added or updated,
   **0 removed**"* is the healthy answer, and with canonical URLs it should now UPDATE the existing
   movies rather than adding any. A removal count out of proportion to what the provider actually
   dropped is the failure to watch for. If it ever goes wrong the library is rebuildable: delete the
   playlist and re-import.

0b. 🔴 **Schema v9 on somebody else's library.** It ran cleanly on the owner's (454,195 → 410,596,
   integrity ok, 14/14 favourites kept) and is a no-op on a live-TV-only one, but that is the only
   real library it has ever touched, and it is the first migration here that REWRITES existing rows.
   *First place to look if any user reports a wrong or empty channel list after upgrading to 0.2.17.*

1. ✅ **ANSWERED ON DEVICE (2026-07-28) — and the FIRST answer here was WRONG. Read the correction.**
   The initial pass sampled ONE live channel (`NL - SPONGEBOB`), saw no scrub bar, and this entry
   recorded "absent on live channels — 0.2.16 is invisible to existing users". **A wider look
   disproved it the same day:** BBC News shows `0:36 / 0:36`, another channel `1:00`, another
   `1:54` — and **all of them actually rewind.** These are real HLS DVR windows, so libVLC's
   `is_seekable` is telling the truth and the bar is doing something useful.
   **Verdict: leave it exactly as it is.** A short rewind buffer on live TV is a capability, not a
   glitch, and suppressing it would throw away working functionality. What is wrong is only the
   CLAIM: 0.2.16 is *not* invisible on live channels — on any provider offering a DVR window it
   adds a working rewind, which is a nicer surprise than the one we were braced for.
   ⚠️ **The methodological lesson is the point of keeping this entry.** One channel was treated as
   the answer to a question about a whole class of streams, and the wrong conclusion was written
   into three documents before a second sample was taken. `is_seekable` varies PER CHANNEL on one
   provider — sample several before concluding anything about it.
2. **The rest of the 0.2.16 pass** — the glass bezel (does a framed tank beside four framed
   mini-meters resolve the 0.2.15 "needs work"?), the scrub bar on something actually seekable, and
   the two 0.2.15 leftovers below (the empty-tank fix, the bezel).
3. **A real VOD sync**, once the worker exists — against a 1-connection line, not during playback.

⏳ **The recon account expired ~2026-07-28** (`exp_date` 1785276000 against a server clock of
1785155974 — ~33 h after the run). Anything needing a live VOD stream needs a renewed line. The
recon report itself is saved and re-runnable: `RabbitEarsCli --xtream` against any provider.

---

✅ **0.2.15 SHIPPED** (2026-07-26) — tag `v0.2.15` @ `1324f5f`, `0.2.15.365`, three signed installers on
GitHub release `v0.2.15`, both appcasts LIVE @ `77035ed` (feeds AND enclosure URLs verified HTTP 200).
See the "Current state" block for the full feature list, the NO-GO blocker the pre-release check caught,
and the two review lessons worth carrying forward.

**Owner-verified on-device before the cut** — unusually thorough for this line, and it mattered because
0.2.15 is almost entirely visual: tank, VU meters, VU colour change, About box, PIP swap all confirmed
on the real x64 build. That leaves **only two things unseen** in the shipped release:
- **the empty-tank fix** (`200e5bc`) — landed after the pass. Play a channel, stop it, and the tank
  should drain COMPLETELY rather than leaving a dim bottom stripe; the sim should also go idle a
  couple of seconds later, where before it ran forever.
- **the glass bezel**, which the owner explicitly accepted as-is: *"needs work — but ok for this
  release"*. Deferred to **pass 4** in `BACKLOG.md`, written as an investigation with a shortlist of
  suspects rather than a fix, because "needs work" does not say which way it is wrong.

**🎬 The next EPIC, if it is wanted: Xtream VOD + Series (`BACKLOG.md`, scoped 2026-07-26).**
RabbitEars has **no VOD support at all** today — no seek API in the player, no scrub bar, no
duration/resume/watched in the `Channel` model, and no Xtream `player_api.php` client (the Xtream
handling that exists treats those playlists as flat lists of live channels). Also: **there is no JSON
parser in the repo**, and `player_api.php` is JSON-only. Estimated ~15–22 focused days; BACKLOG holds
the full breakdown, the risks, and a three-release plan that lands at **0.3.0** rather than starting
there. **Two things must happen first, in order:** a 30-minute reconnaissance of the owner's real
provider (panels are wildly non-standard and some disable the API), then a design doc
`Win32/docs/XTREAM_VOD.md` written BEFORE any code — exactly as `THEME_ENGINE.md` was for the one
prior epic of this size, and with the same obligation to flag the shared-core boundary to the macOS
team, since the client, parser, schema and models all land in `common/`.

**Candidates for 0.2.16** (as judged before the work started; ✅ = now landed in `ffb69dc`):
✅ **wire the glass into the buffer meter** — done, and it was indeed the item with the clearest
rationale; whether it resolves the bezel reservation is still an owner call. Glass bezel **pass 4**
remains open and is now better informed (the tank is no longer the unframed odd one out, so if the
bezel still reads wrong, the suspects narrow to the three luminance steps / the light skin / the
corner pip). Still open and untouched: the **buffer-depth tester task** (does `bufferedBytes` report
at all on IPTV?); the About box's **"Bg" swatch not saying it is the VU lamp**; the **x86-64 arch
label**; and Authenticode signing (still owner-gated, also unblocks the Microsoft Store track).

**Still open from 0.2.14:** the 22 dead-link verdicts from that release's one real sweep have never
been confirmed as true positives — much less pressing now that "Clear dead-link results" makes a wrong
verdict one click to undo, which is precisely what made graduating the checker safe.

✅ **0.2.14 SHIPPED** (2026-07-26) — tag `v0.2.14` @ `aa8580f`, `0.2.14.349`, three signed installers on
GitHub release `v0.2.14`, both appcasts LIVE @ `d594fd0`. Six features + a background worker — see the
"Current state" block above for the full list and the reasoning behind each.

**Pending the owner's on-device pass** (0.2.14 shipped with less runtime verification than usual):
- **"Skip this airing"** has never been exercised on a real series rule.
- **The Unicode case-fold fix** has never been tested against a non-English guide (the owner's library
  has no EPG at all — see the deferred TV-Guide item).
- **The dead-link checker** got one real sweep (31 alive / 22 dead / 53 written). Worth confirming those
  **22 are genuinely dead** rather than false positives — with "Hide unavailable" on they vanish from the
  grid, and a wrong verdict is invisible until a user goes looking for a channel.
- Everything else (System dialog, log levels, PIP toggle + both PIP fixes, VU, glass) was owner-seen.

**Candidates for 0.2.15:** the About-box tip section (owner-requested — the tip buttons have no
explanation); further **glass** work (a bezel/frame is the top idea — see BACKLOG for the explicit "do
NOT reintroduce" list); the **PIP right-click menu + PIP⇄main swap**; graduating the dead-link checker
out of beta once it has mileage; Authenticode signing (also unblocks the Microsoft Store track).

📚 **The shipped-release blocks for v0.2.13 and older moved to
[`HANDOVER-ARCHIVE.md`](HANDOVER-ARCHIVE.md)** — including the long write-ups for localization,
wake-to-record, the recording scheduler, saved layouts, favourites I/O and the ARM64 port.

## Seed prompt for a new session

Paste this verbatim to start a fresh session with working context restored:

> You are continuing **RabbitEars**, a native **Windows Win32 / C++20** IPTV player on **libVLC
> 3.0.23** with a shared **`common/`** core (also feeds the macOS app), dark "Claude-desktop" chrome
> (coral `#D97757`, custom `WM_NCCALCSIZE` title bar), CMake + Ninja + MSVC (VS 2026), deps
> vendored/NuGet. Repo `G:\RabbitEars` (a TrueNAS SMB share).
>
> **Read `Win32/HANDOVER.md` first** — the top "Current state" block (0.2.19 and the EPG work) — plus
> `Win32/BACKLOG.md` and `Win32/docs/PHOTOREAL.md` (the photoreal epic, parked on owner decisions).
> Older release history is in `Win32/HANDOVER-ARCHIVE.md`; check it before re-trying an idea.
>
> **State:** **0.2.19 is SHIPPED and auto-update is LIVE** (full `0.2.19.434`, tag `v0.2.19` @ `5a2378d`,
> appcasts @ `d2401b5`). `APP_VERSION` is still **0.2.19** — bump it (`cmake/AppVersion.cmake:11`, ask
> the owner) before the next cut. macOS is at 0.2.17.
> **The owner's two EPG requests** — full-text search in the guide and a calendar of future airings:
> **steps 1 and 2 SHIPPED in 0.2.19** — step 1 = guide-refresh progress + timings, Set Guide URL
> extracting the address from pasted text, provider logins masked in the diagnostic log; step 2
> (`0892cf4`) = ONE search box in the TV Guide for channels and programmes, FTS5 + schema v10 in
> `common/` (`docs/EPG_SEARCH.md` is the design and as-built record), plus `35b8826` (Greek/Romanian
> case folding in recording rules). **Step 3, the calendar, is parked**: the owner's provider
> publishes only ~6 h of future guide and its Xtream API none — HANDOVER's "What the investigation
> found" has every measurement. BACKLOG's EPG section lists the follow-ups. The repo has TWO writers
> (the mac team pushes to `main`), so run `git fetch`, `git status` and `git log origin/main..` first,
> and verify `git ls-remote origin refs/heads/main` == HEAD immediately before building anything for
> a release.
>
> **Shipped without a full owner run:** in 0.2.19 the search's filter chip, the jump from a result and
> typing over the grid (nothing reported against them), and the recording-rules fold (selftest only);
> in 0.2.18 the search debounce, the lost schedule-status-write fix (HANDOVER's "Lost schedule-status
> writes" block has six owner checks — the ordinary scheduled-recording path first), the Light-skin
> meter fixes, and the "PEAK lamp lights whenever the needle is in the red" rule. Owner-seen and
> liked: the tank readout and the VU dials ("look amazing"), the guide search results.
>
> **Tools that change how visual work is done:**
> * `build\Win32\RabbitEarsRender.exe <outdir> [--skin ID] [--no-strip]` renders every meter look, the
>   tank and every skinned strip to PNG from the REAL paint code, byte-reproducibly. Render before
>   and after every visual change, compare with a pixel diff, read the PNGs yourself, and hand the
>   owner a labelled before/after sheet. The "150dpi" sheets are 156 % scaling.
> * `powershell -File scripts\run-profile.ps1 [-Refresh]` runs the dev build BESIDE the installed app
>   as profile "dev" (`Win32/platform/Profile.h`) on a snapshot of the real library — it never touches
>   the wake task or WinSparkle. The owner checks things live this way. **This sandbox cannot drive
>   the GUI (computer-use for RabbitEars was declined): the owner drives the screen — tell them where
>   to look.** The owner runs at 150 % scaling; their tray is LED Spectrum, Tube Signal, LCD Bitrate
>   and a VU Frame-rate meter with a cyan lamp, glass 69 %.
>
> **The photoreal epic, next:** Phase 0 (the render tool), Phase 1 (six defects) and the VU
> instruments (`Win32/ui/VuDial` — backlit "VU needle" + "Silver VU", built to the owner's reference
> photos) have shipped. The rest waits on the owner's decisions at the bottom of PHOTOREAL.md — **the
> size question matters most**: the tray dials are 26–39 px tall, and their numerals need ~50 px, so
> a taller meter tray is what would put the scale's numbers in the tray.
>
> **The one number that matters for perf:** the owner's real library is **411,149 rows**, not the
> ~44k the design assumed; `--benchdb`'s defaults still model the small shape. Measure against the
> real one. **Still unexercised:** schema v9's row merge on anyone else's library, and the VOD sync's
> DELETE path (the SECOND sync; "0 removed" is healthy).
>
> **Traps that have cost real time:**
> * **A confident comment is not a verified fact.** Verify or weaken it — every adversarial review
>   this session still found over-claiming comments.
> * **`common/` is shared with mac** (Apple clang, which you cannot compile here): keep shared changes
>   additive; a new enum value mac switches on needs its mac `switch` case in the same commit; grep
>   `mac/` first; prefer flagging over editing their tree. Meter looks and skins are Win32-only.
> * **Launching an exe from `G:` through the SHELL** (`Start-Process`, Explorer) raises a blocking
>   "Open File – Security Warning" — it looks like a hang. Use CreateProcess (`run-profile.ps1` does).
> * **`LNK1168`** = RabbitEars is running. Close it with `WM_CLOSE` (`CloseMainWindow`), never a
>   force-kill (an in-progress recording loses its moov atom). If it will not close, a dialog is
>   probably open in it — ask the owner to close it.
> * **Splitting uncommitted work into several commits:** snapshot the working state as a git tree with
>   a temporary index (`GIT_INDEX_FILE=… git read-tree HEAD; git add <paths>; git write-tree`) before
>   the next change touches the same files; later `git read-tree <tree>` + `git commit`.
> * **Editing files via inline Python in bash heredocs** mangled `\r` once (a `\\r` became a CR) and
>   stray brackets twice — write the script to a file, and scan edited files for control characters.
> * **Command ids:** pick from a genuine gap (the computed ranges `ID_DOCK_BASE` 2051–2062,
>   `ID_LAYOUT_*_BASE` 2079–2098, `ID_THEME_SKIN_BASE` 2100+ have no literal to grep).
> * **Release:** bump ONLY `APP_VERSION` in `cmake/AppVersion.cmake` line 11 (leave the `if(APPLE)`
>   override). Three installers (`build-installer.cmd`, `… arm64`, `… universal`), two appcasts, always
>   `-Tag v<ver>`. Push before tagging; verify `ls-remote` == HEAD before building. **Signing on the
>   Mac** needs `SIGN_UPDATE` pointed at `build-mac*/sparkle/bin/sign_update` and
>   `SIGN_UPDATE_ARGS="--account SQLTerminal"` (HANDOVER's release recipe); verify each signature on
>   Windows against `Updater.cpp`'s public key before publishing.
> * **Provider logins are secrets.** Every Xtream URL carries one (query or path). The Win32 log now
>   masks them (`Win32/platform/UrlRedact`, `LogSecrets.h`); never print a URL from the DB or a log
>   in a tool result without masking it first, and never put one in a doc or commit.
> * **Build with `-DRABBITEARS_THEME_ENGINE=ON` explicitly** and verify BOTH flags before committing
>   (build dirs cache the flag; leave the cache at ON).
> * **i18n:** `common/i18n/*.json` → `python tools/i18n/gen_i18n.py` (never hand-edit
>   `common/core/Strings.*`; `--check` must pass). 595 keys × 4 languages; `zh-HK` is an override
>   layer; append new keys at the END of `keys.json`. CJK is a machine draft.
>
> **Working rules:** every change adversarially reviewed (background agents) + build-verified with
> BOTH theme flags + `--selftest` ALL PASS before committing; render before/after for anything
> visual. Commit only when asked; stage specific paths (never `git add -A`); end commit messages with
> the Co-Authored-By trailer. Hand every runtime check to the owner, and never conclude anything about
> a class of streams from one channel.
