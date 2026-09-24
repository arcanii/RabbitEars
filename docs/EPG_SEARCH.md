# EPG programme search — design

> **Status (2026-09-24):** design **approved by the owner** (all six decisions in §7 taken as
> recommended — then decision 1 REVISED by the owner after the first live test: ONE search box, §7.1);
> IMPLEMENTED and committed (`0892cf4`; the recording-rules fold fix found on the way, `35b8826`) —
> §§2–6 describe it as built; owner-tested live in the dev profile (build 431). Step 2 of the owner's two EPG
> requests (the full context, step 1 and every measurement it rests on: `Win32/HANDOVER.md` →
> "0.2.19-dev — the owner's two EPG requests"). Step 3, the calendar, is out of scope and parked: the
> owner's provider publishes ~6 hours of future guide and its Xtream API none.
>
> **The owner's request:** *"FTS in the EPG to find what you want fast."* Agreed: search titles
> **and descriptions** (owner, 2026-09-24).
>
> **Shared core:** the index and the query live in `common/` and the SQLite build flag is shared, so
> **§2 is for the macOS team** — it changes their binary and their database schema, though by design
> it costs them nothing at run time until they add a search UI.

Every number below is **measured**, and says where; anything not measured says so. The draft was
adversarially reviewed against the code before the owner saw it; its corrections are folded in.

---

## 1. What is already established

### The data (the owner's real library, refreshed 2026-09-24)

- **192,595 programmes** across **2,881** guide channels (2,495 of them match a channel tvg-id in the
  owner's playlists); one playlist carries a guide.
- Fields actually present: **title** (every row; 37,807 distinct; 17 % contain non-ASCII — Latin
  accents, Greek, Cyrillic, Arabic; no CJK) and **description** (170,553 rows; 28.1 M characters,
  8× the titles). **Sub-title, category and episode number are empty in this feed.**
- Horizon: ~1.8 days of past, **~6 h of future** (median channel 5.8 h). Search will mostly find what
  is on *today* — which is also why the calendar is parked.

### Today's code

- `epg_programmes` (`common/db/Database.cpp:521`) — one index, `(playlist_id, channel_id, start_utc)`.
  **No text search exists**: the guide window's only filter is a channel-NAME match typed into its
  corner cell, in memory with ASCII-only case folding (`Win32/ui/EpgGuideControl.cpp`, `WM_CHAR`;
  typing works anywhere in the guide window). That corner cell is **painted, not an EDIT**, which is
  why the owner saw no caret.
- `bulkInsertProgrammes` (`Database.cpp:1336`) replaces ONE playlist's guide wholesale inside one
  `BEGIN IMMEDIATE` transaction; both platforms call it once per guide playlist, on the **UI thread**
  (Win32 `onEpgDone`; mac on the main queue, `MainWindowController.mm:3397–3408`). It and the playlist
  delete's `ON DELETE CASCADE` are the only writers of `epg_programmes` (grep, both platforms).
- **FTS5 is not compiled into our SQLite** on either platform. The amalgamation (3.53.2) defines
  `SQLITE_CORE` (`sqlite3.c:26`), which compiles FTS5 out unless `SQLITE_ENABLE_FTS5` is set
  (`sqlite3.c:241080`); the root `CMakeLists.txt:81` `sqlite3` target sets only
  `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_DQS=0`, `SQLITE_DEFAULT_FOREIGN_KEYS=1`.
  `SQLITE_OMIT_LOAD_EXTENSION` also rules out loading FTS5 at run time.
- The guide maps programmes to channels **per playlist**: a playlist's programmes are matched only
  against that same playlist's channels, on a **normalised tvg-id** — the part before `@`,
  ASCII-lower-cased (`normId` in `onEpgGuide`, `Win32/ui/MainWindowCommands.cpp:548–564`; mac's own in
  `TvGuideWindowController.mm`). Programmes on channels the user does not have are dropped (~13 % of the
  owner's guide channels). The lookup map is a local, rebuilt on each guide build.
- The built guide rows **outlive a refresh**: reopening the guide after hiding it re-reveals the old
  rows without a rebuild (`revealEpgGuide`).

### Measured: why FTS5, and which tokenizers

On the real 192,595 programmes. "Python" = Python's SQLite 3.50.4; "vendored" = a throwaway probe
compiled against **our** `third_party/sqlite/sqlite3.c` (3.53.2) with the app's defines plus
`SQLITE_ENABLE_FTS5`.

| approach | cost per search | index size | build | finds |
|---|---|---|---|---|
| `LIKE '%q%'` (today's channel-search technique), titles | 29–35 ms (Python) | — | — | misses accents and non-ASCII case: "quebec" 10 titles vs 59; a Greek word 0 vs 20 |
| `LIKE`, titles + descriptions | 46–70 ms (Python) | — | — | same misses |
| FTS5 **trigram** on titles | ~0.3 ms typical | **9.9 MB** (vendored) | **330 ms** (vendored) | any substring ("news" finds "CityNews"), accent- and case-insensitive |
| FTS5 **unicode61** on descriptions | ~0.3 ms typical | **16.9 MB** (vendored) | **938 ms** (vendored) | whole words + prefixes, accent- and case-insensitive |
| (rejected) trigram on descriptions | — | 145 MB (Python) | 4.55 s (Python) | too big for what it adds |

The two recommended tables together, one combined match (title substring ∪ description words, joined
back, upcoming only, top 200, rowids only — **no snippet**, vendored build): **0.1–0.3 ms** for
"toronto", "survivor", "kimmel", "quebec", "hockey", a Greek word; **17.7 ms** for "news"; **32.6 ms**
for "the" (a word in most descriptions — the worst case found). All inside the 200 ms debounce the
main search already uses, on the UI thread. The snippet step (§4) and the channel filter are NOT in
these numbers.

Why two tables: a tokenizer is chosen per FTS5 **table**, not per column. Titles want trigram (people
type fragments of a show name, and compound titles like "CityNews" are common); descriptions want words
(trigram there is 145 MB).

### Measured: a library with the index stays usable by a build WITHOUT FTS5

The same probe, built WITHOUT FTS5 (as every release through 0.2.18 is), opened a database carrying the
two FTS5 tables at `user_version` 10: it opened; read `epg_programmes`; ran a whole guide refresh
(`DELETE` + `INSERT` in one transaction — no triggers, so nothing references the missing module);
`quick_check` and `integrity_check` answered **"ok"**; a schema change and `VACUUM` succeeded. Only
touching an FTS table failed (`no such module: fts5`). **So a downgrade, or an older build, is safe —
but it leaves the index stale.** §3's stamp handles that.

Binary cost: the vendored `sqlite3.obj` grows 3.15 → 3.99 MB; a linked test executable grew
**~270 KB** (x64, MSVC /O2). Not measured on ARM64 or Apple clang.

---

## 2. Shared-core boundary — **for the macOS team**

The mac app links the same vendored `sqlite3` static target (root `CMakeLists.txt` → `RabbitEarsCore`,
`common/CMakeLists.txt:31` → the mac app, `mac/CMakeLists.txt:108`); nothing in `mac/` links the system
SQLite. So:

| change | file | effect on mac |
|---|---|---|
| `SQLITE_ENABLE_FTS5` on the `sqlite3` target | root `CMakeLists.txt` | FTS5 compiled into the mac binary: ~270 KB bigger per architecture (x64 MSVC figure; not measured on clang). The amalgamation's FTS5 is plain C, but **it has not been compiled with Apple clang here** — the first mac build is the check. |
| schema **v10**: create the two FTS5 tables (empty) | `common/db/Database.cpp` `migrate()` | instant (41 ms on a copy of the owner's library), at the mac app's first launch after the merge. **No index is built then.** **Please confirm there is no competing v10 in flight.** |
| new `programmeSearchState()`, `rebuildProgrammeIndex()`, `refreshProgrammeSearchChannels()`, `searchProgrammes()` | `common/db/Database.{h,cpp}` | available; **nothing runs them unless called**. `bulkInsertProgrammes` does not maintain the index, so a mac guide refresh costs exactly what it does today. |
| `channelsByPlaylist` ORDER BY gains a final `, id` | `common/db/Database.cpp` | changes the order only among rows that tie on every other key (previously unspecified) — so the guide and the search pick the same channel for a tvg-id. |
| i18n | `common/i18n/*.json` | new keys appended; mac need not use them. |

No enum a mac `switch` depends on changes (no mac `switch` uses `StringId`). A mac build **older** than
this change, opening a v10 database, behaves like the FTS5-less probe above: works, and changes
`epg_programmes` without re-indexing — which the stamp (§3) reports as `NeedsRebuild` to whoever next
asks.

**If mac adds search later:** after its refresh stores, call `rebuildProgrammeIndex()`; before a search
session, `if (programmeSearchState() == NeedsRebuild) rebuildProgrammeIndex();` then
`refreshProgrammeSearchChannels()`; then `searchProgrammes()` per query. `searchProgrammes` never builds
the index itself — without a Ready index it answers with a LIKE scan.

---

## 3. Data model

### Schema v10 — two empty tables

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS epg_fts_title USING fts5(
    title, content='epg_programmes', content_rowid='id',
    tokenize='trigram remove_diacritics 1');
CREATE VIRTUAL TABLE IF NOT EXISTS epg_fts_descr USING fts5(
    descr, content='epg_programmes', content_rowid='id',
    tokenize='unicode61 remove_diacritics 2');
```

- **External content** (`content='epg_programmes'`): the text is stored once, in `epg_programmes`; the
  index holds only tokens. Trigram's `remove_diacritics` is parsed in its create (`sqlite3.c:266895`;
  1 and 2 both select the same fold mode there, `:266899`). Both tokenizers fold case and accents for
  **Latin** script; they do **not** strip e.g. Greek tonos (`σημερα` does not find `Σήμερα`). Side
  effect: with `remove_diacritics` on, FTS5 no longer accelerates `LIKE`/`GLOB` on the trigram table
  (`:267014`) — irrelevant, since the LIKE fallback scans `epg_programmes` directly.
- **unicode61 cannot segment CJK**: a run of Chinese or Japanese without spaces is ONE token, so a word
  inside it is not findable through `epg_fts_descr`. A search term containing CJK therefore searches
  descriptions with `LIKE` (a table scan, ~50 ms) — titles stay on the trigram index, which needs no
  word boundaries.
- **No triggers.** Triggers would make every write to `epg_programmes` depend on FTS5, and a build
  without it would then fail every guide refresh with "no such module". The index is maintained
  explicitly instead. The probe in §1 is what makes this a verified property, not a hope.
- **The migration must not re-run v9.** `migrate()` has no per-step gate: its only guard was the early
  `if (v >= 9) return;`; after it, `user_version` is rewritten from the column checks (8 at most) and
  v9's channel-URL rewrite runs whenever v8's columns exist. Simply raising the early return to
  `v >= 10` would have sent every v9 database back through v9. **As built:** `if (v >= 10) return;`,
  then `if (v == 9) { createProgrammeSearchTables(); return; }` — a v9 database skips the old block
  entirely — and a database older than v9 runs the old block and then v10 only if v9 landed in the same
  open. v10 is ONE transaction: both `CREATE VIRTUAL TABLE IF NOT EXISTS`, a check that both names are
  genuinely `USING fts5` tables (`IF NOT EXISTS` succeeds over an ordinary table of the same name), the
  removal of any old `epg_fts_stamp`, and `user_version=10`. A failure (`SQLITE_BUSY`, a full disk)
  leaves v9 and nothing half-made; the next open retries, and search answers with LIKE meanwhile. It
  cannot fail for want of FTS5: one `sqlite3` target feeds every binary that contains this step.
  **Selftest-pinned**, and the pin was shown to fail with the v9 gate removed.

### Keeping the index true — a stamp, and explicit rebuilds

- **`rebuildProgrammeIndex()`** — its own `BEGIN IMMEDIATE` transaction: `'rebuild'` both tables, then
  write `epg_fts_stamp` to `settings` — the row count, the max `id`, and the concatenated
  `epg_refreshed_<playlist>` values. `bulkInsertProgrammes` has rewritten those on every refresh since
  the commit that created schema v3 (`910e19e`), so every older build changes them too. Count and max
  id alone would NOT do: an old build re-importing a same-sized guide reuses the same ids. **Measured
  through this code on a copy of the owner's library: ~1.5 s** for 193k programmes.
- **Nothing in `Database` builds the index on its own** — callers do. Win32 does it (a) **once per guide
  refresh**, after `onEpgDone` has stored every playlist ("Indexing the guide for search…"), not inside
  `bulkInsertProgrammes`, where N guide playlists would mean N full rebuilds; and (b) **before the first
  search of a burst of typing** when `programmeSearchState()` says `NeedsRebuild` ("Preparing search…").
- **`programmeSearchState()`** compares the stamp (`COUNT` + `MAX(id)` + the settings: **7 ms** on the
  real guide) once, then remembers the answer until this object changes `epg_programmes`
  (`bulkInsertProgrammes`, `deletePlaylist`) or is reopened. That one check covers the first search after
  upgrading (v10 created the tables empty and cleared any stamp), a playlist deleted (count changed), a
  refresh by an older build or by the mac app, and a failed rebuild. **Selftest-pinned**, including that
  a fresh connection reads a saved stamp as current (no rebuild per launch).
- **A stale index is never READ.** `searchProgrammes` uses the index only when the state is `Ready`;
  otherwise it answers with LIKE. Reading it stale would be wrong, not just untidy: a single-guide refresh
  restarts ids at 1, so a stale entry points at a different programme (selftest-pinned with a reused id,
  and shown to fail when the guard is removed); and reading a column, `highlight()` or `snippet()`
  through FTS5 on a rowid whose row is gone fails with `SQLITE_CORRUPT_VTAB` (`sqlite3.c:261872–261883`).

### The write lock

`BEGIN IMMEDIATE` holds SQLite's writer lock for its whole transaction; the Win32 VOD sync and dead-link
workers (`Win32/ui/VodSync.cpp:97`, `Win32/ui/DeadLinkSweep.cpp:39`; mac has the same two) open their
own connections and wait up to `busy_timeout` (5 s, `Database.cpp:421`), then fail with `SQLITE_BUSY`.
With the rebuild in its own transaction, no single hold grows: the store stays ~1 s, the rebuild adds a
separate ~1.5 s on this guide. A guide ~3× the owner's would push the rebuild alone toward 5 s. The
cleverer option on record — incremental per playlist, FTS5 `'delete'` rows for the old programmes and
indexing only the new — is fragile with external content: `'delete'` must be given the exact values that
were indexed, so it has to run BEFORE the old rows are deleted and only when the stamp says the index is
in sync, or it corrupts the index.

---

## 4. The query

`Database::searchProgrammes(text, fromUtc, limit, &truncated)` — as built.

**The channel set.** `refreshProgrammeSearchChannels()` fills a TEMP table
`guide_channels(playlist_id, cid, name, tvg_id)` with ONE `INSERT … SELECT`: every channel with a tvg-id
in an enabled playlist, keyed by (playlist, normalised tvg-id), keeping per key the FIRST channel in
`channelsByPlaylist`'s own order (`ROW_NUMBER() OVER (… ORDER BY kind, (lcn IS NULL), lcn, sort_order,
name COLLATE NOCASE, id)`) — the channel the guide's row join keeps, so a result names and plays the same
channel its guide row does. ~90 ms at the owner's 410k channels (listing them through
`channelsByPlaylist` took 1.1 s). The guide itself only builds rows for programmes in its −6 h..+72 h
window; search has no upper bound.

**1. Match, filter, rank — rowids only:**

```sql
SELECT p.id, MAX(m.t) AS inTitle, p.start_utc, g.name, g.tvg_id
FROM (SELECT rowid AS id, 1 AS t FROM epg_fts_title WHERE epg_fts_title MATCH :titleQ
      UNION ALL
      SELECT rowid, 0 FROM epg_fts_descr WHERE epg_fts_descr MATCH :descrQ   -- or, for a CJK term:
      -- SELECT id, 0 FROM epg_programmes WHERE descr LIKE :term ESCAPE '\'
     ) m
CROSS JOIN epg_programmes p ON p.id = m.id
CROSS JOIN temp.guide_channels g
  ON g.playlist_id = p.playlist_id
 AND g.cid = lower(substr(p.channel_id, 1, instr(p.channel_id || '@', '@') - 1))
WHERE p.stop_utc > :fromUtc
  AND p.playlist_id IN (SELECT id FROM playlists WHERE enabled = 1)
GROUP BY p.id
ORDER BY inTitle DESC, p.start_utc
LIMIT :limit + 1;           -- the extra row sets `truncated`
```

- **`CROSS JOIN` pins the join order.** With plain `JOIN`, the vendored 3.53.2 planner drove from
  `epg_programmes` through `idx_epg_lookup` (`playlist_id IN …`) and probed the matches per row: **47 ms
  for a rare word**, against **0.2 ms** matches-first (measured on the owner's guide).
- The description arm is left out when the typed text has no word a tokenizer could keep.
- **Title matches first, then description-only matches, each soonest first.** Past programmes are
  excluded (`fromUtc` = now) — decided, §7.4.

**2. The winners' rows** — `SELECT … FROM epg_programmes WHERE id IN (…)` for the ≤ `limit` ids.

**3. Marking, in C++, not FTS5.** `highlight()` and `snippet()` read and re-tokenise the content row of
every match before the LIMIT applies: `snippet()` took **700 ms for "the"** and 180 ms for "news" on the
owner's guide. So the ≤ 200 results are marked in C++ (U+0002 … U+0003 around each match): the title
with the typed text (title matches), and for description-only matches an excerpt around the earliest
typed word. The marking is simpler than the index's matching and can differ: it compares characters
through `searchFold` (`common/core/SearchFold.h` — case for Latin, Greek and Cyrillic as FTS5 folds it,
Latin accents dropped, one character for one), so "quebec" marks "Québec"; a match the index found
through some other folding (Vietnamese and pinyin accents) stays unmarked (the snippet is then the
description's start), and the stroke letters (Ø Ł …) fold to their base letter though FTS5 keeps them; it marks every typed word as a prefix (the index takes only the
last); it compares typed punctuation literally; and it treats the common punctuation blocks (U+00A0–BF,
U+2000–206F, U+3000–303F) as word boundaries, as the tokenizer does, so `“Doctor` still marks `Doctor`.
For a CJK word it drops the word-start rule (CJK has no word boundaries to start at).

**The user's text is never FTS5 syntax.** Typed `"`, `*`, `(`, `-`, `AND`/`OR`/`NOT`/`NEAR` search for
themselves. Title query: the trimmed text as ONE quoted phrase (inner `"` doubled) — trigram matches it
as a substring. Description query: each word quoted and simply juxtaposed (FTS5's implicit AND, which —
unlike an explicit `AND` — drops a word that yields no tokens, such as a lone `-`), the last one a
prefix: `"doctor" "who"*`.

**The LIKE fallback** — the whole text as ONE substring (not word by word), ASCII-only case folding,
~50 ms: used when the term has **1–2 characters** (trigram yields no tokens under 3; decided, §7.5) or the
index is **not `Ready`** (never built, stale, or v10 not landed). It searches titles, and also
descriptions unless the index is `Ready` — so a 1–2 character Latin term with a Ready index searches
titles only (a one-letter description scan matches nearly everything). A CJK term always searches
descriptions too, since two CJK characters are already a whole word.

**Measured through this code on a copy of the owner's library** (`RabbitEarsCli --epgsearch <copy>`):
v10 upgrade 41 ms; index rebuild ~1.5 s; channel set ~90 ms; searches **0.2–0.4 ms** for "toronto",
"survivor", "quebec", "hockey", "kimmel", "doctor who", "montreal"; **22.5 ms** "news", **40.8 ms** "the",
13.7 ms "the news"; 1–2 characters (LIKE) ~48 ms.

---

## 5. The UI (Win32)

**The layout** *(decided, §7.1 — revised by the owner after the first live test)*: **one search box**,
"Search channels and programmes…", in a new 40-dp toolbar strip above the hour axis. It replaces the
corner cell's old type-to-filter: typing anywhere in the guide goes to it. The strip also holds a
painted **Now** button (scrolls the time axis back to now — decided, §7.6; Home does the same in the
grid), and is where step 3's day buttons would go. The corner cell is empty until a channel filter is
on; then it shows a **chip** — the filter's text and an ✕ — and a click anywhere on it clears the filter.

- The box is a standard `EDIT` control, themed by `dialogCtlColor` (which paints EDITs in the theme's
  `windowBg`, so its painted frame uses that colour too), with a cue banner. The guide window gains
  `WS_CLIPCHILDREN` so its Direct2D paint never draws over it.
- **Keys:** a focused EDIT receives Esc/Enter/arrows itself, so the box is subclassed; there is no
  dialog manager, so Tab is handled there too (it hands the keys to the grid, when the grid shows).
  **Esc**, one layer at a time: empties the focused box (closing the results), else leaves the
  results, else clears the channel filter, else empties the box, else closes the guide. "Type anywhere
  in the guide": a printable `WM_CHAR` on the guide goes to the box — over the grid it REPLACES what
  the box still holds from the last search (a new search; Backspace edits that text instead), over the
  results it adds to it. Focus is remembered across deactivation (a dialog, Alt-Tab) and restored to
  the box if it had it.
- **Channels** are matched in the guide itself, in memory — the rows it was built with, by channel
  NAME, a substring, case- and accent-insensitive (`searchFold`: "quebec" finds "TVA QUÉBEC"). One
  function (`channelMatches`) gives both the count the results show and the rows the filter keeps.
- **Programme search** debounces 200 ms exactly as the main search does (one-shot timer + a
  `searchPending` flag re-checked on the tick, because `KillTimer` does not remove an already-posted
  `WM_TIMER`); a navigation key pressed inside those 200 ms runs the pending search first, so it acts
  on what the box says. The first search of a SESSION — after the box was empty, after the guide's rows
  were (re)built, or after the guide was reopened — paints "Preparing search…" and then calls the host,
  which loads the channel set and rebuilds a stale index.
- **Results replace the grid** while shown: a Direct2D list in the guide's own style. First, when any
  channel name matches, "Matching channels: N" and ONE item — "Show only channels matching “x”", the
  first dozen names beneath it with the typed text marked; choosing it narrows the grid to those
  channels, empties the box and shows the chip (the grid's old type-to-filter, now "type, Enter": the
  item is selected by default). Then the programmes under their count ("Upcoming programmes found: N",
  "showing the first 200", or "no upcoming programmes match"), day headings ("Today" / "Tomorrow" / a
  date in the user's locale) — the description-only matches under a heading of their own ("Found in the
  description"), their days starting again beneath it — and per result the title with the matched
  text in bold accent, time range and channel, an "On now" badge for what is airing, and for description
  matches a one-line snippet (a third line: those rows are 62 dp, the rest 46).
- **Keyboard:** ↑/↓/PgUp/PgDn move the selection and **Enter** opens the programme dialog (Play /
  Schedule / Record series — the existing `programmeDialog`), or applies the channel filter — from the
  search box or from the guide itself. After a jump, Enter (or ↓ in the search box), or a click in the search box, brings the results
  back — by searching again, so "Today" and "On now" are current. Focus arriving in the box any other way
  (restored on activation, Tab) does NOT bring them back: that would cover what "Show in TV Guide" or a
  reopen was asked to show. Home scrolls the grid to now.
- **Mouse** *(decided, §7.3)*: a click **jumps to the programme in the grid** — scrolls the time axis and
  the row into view, outlines the block in the accent colour for 2.5 s, keeps the query; a **double-click**
  opens the programme dialog (the first click has already jumped, so the second — arriving as
  `WM_LBUTTONDBLCLK` on the grid, the class now has `CS_DBLCLKS` — opens the programme that click jumped
  to; if the jump could not happen, the list is still there and the double-click opens the result
  directly); right-click = Play / Schedule / Record series / Show in guide. A click on the channels item
  applies the filter (its double-click's second click, landing on the grid, is ignored). A click on the
  box's painted frame focuses the box. A jump keeps a channel filter that shows the programme's row, and
  clears one that hides it.
- **The grid may be older than the search.** Search reads the database; the grid's rows can predate the
  last refresh (they survive a hide/reveal). If a result is not in the grid's rows, the jump rebuilds
  them (the host's `onEpgGuide`, re-entering `showEpgGuide` on the same window) and retries — but only
  when that could help: the programme lies inside the window a rebuild covers (`kGuideWindowPastSec` /
  `kGuideWindowAheadSec` around now, shared with the host), and the rows have not been rebuilt since
  those results were fetched. Otherwise, or if it is still missing, the jump just beeps. Reopening the
  guide shows the grid with every channel (the query stays in the box; the rows are rebuilt, which
  clears a channel filter).

New strings (appended to `keys.json`, en/ja/zh-Hant; CJK a machine draft): the placeholder, Now,
"Preparing search…", none found, the count, "showing the first {0}", "On now", Today, Tomorrow,
"Show in guide", "Found in the description", the loading-box line "Indexing the guide for search…",
"Matching channels: {0}" and "Show only channels matching “{0}”".

---

## 6. Testing

**Selftest (CLI, automated — `RabbitEarsCli --selftest`, block "Programme search"):**
- A fresh database reaches v10 with the index not yet built; v2 → v10 walks the whole chain.
- **v9 → v10 does NOT re-run v9's URL rewrite** — a canary stream URL in a spelling v9 would rewrite
  stays untouched (and the check asserts the canary IS such a spelling). Shown to fail with the gate
  removed.
- **A failed v10** — injected by an ordinary table squatting on `epg_fts_title` (`IF NOT EXISTS` then
  succeeds, the fts5-declaration check fails) — leaves v9, reports `Unavailable`, and LIKE still searches
  descriptions; with the squatter gone, the next open lands v10.
- Search: title substring ("ronto" finds "Toronto"); accents ("quebec" finds "Québec"); Greek case;
  description words and a prefix; title matches ranked before description-only ones; past programmes,
  channels the user does not have, and disabled playlists dropped — **before** the limit; typed FTS5
  syntax searched literally (quotes, parentheses, `AND`, `OR*`); odd input (a lone `-`, `"`, `NEAR(`,
  `*`, `(((`) neither errors nor poisons the connection; a 2-character term falls back to titles.
- Marking: a marked title, a marked snippet, the channel name + full tvg-id on a hit; "quebec" marks
  "Québec" (title and snippet) and lower-case Greek marks upper-case; `searchFold`'s table edges (accents
  dropped; Æ Œ Ĳ ß × ı ĸ kept); a CJK word found in a description and marked mid-run (2 characters →
  LIKE, 3 → the index path with a LIKE description arm); a word right after a curly quote found AND
  marked.
- The stamp: a refresh leaves `NeedsRebuild` and the stale index is bypassed; a rebuild finds the new
  title; deleting a playlist leaves `NeedsRebuild`; a fresh connection reads a saved stamp as `Ready`;
  a simulated older-build same-sized refresh (only `epg_refreshed_<id>` changed) is detected; **a refresh
  that reuses ids** (one playlist) is never answered from the stale index — asserted to really reuse the
  id, and shown to fail with the Ready guard removed.
- A performance guard: 40,000 synthetic programmes, a word in every row, 200 results through the index
  in < 500 ms (49 ms measured).

**Measured on a COPY of the real library** with `RabbitEarsCli --epgsearch <copy> [terms…]` (§4's
numbers). `run-profile.ps1 -Refresh` snapshots the real library into the dev profile for the owner's
live test, where v10 runs on real data without touching the real one.

**Owner, live:** search for a show known to be on today; a word only in a description; an accented
title (marked); "jump to it", the double-click and Enter dialogs from a result; the right-click menu;
Now; a refresh (the new "Indexing…" line); a channel name + Enter narrows the grid, the chip's ✕ (and
Esc) clears it; typing over the grid starts a new search.

---

## 7. Decisions — **DECIDED by the owner, 2026-09-24: "go with the recommendations"**

1. **One box** — *REVISED by the owner, 2026-09-24, after the first live test* ("two search bars?"). As
   first decided, the corner cell kept the channel filter (as a real text box) beside a separate
   "Search programmes…" box; built, they read as two copies of one search. Now one box lists matching
   channels AND programmes, and the channels' item narrows the grid (§5) — the "toronto" filter the
   owner used is "toronto, Enter", no longer live with every keystroke.
2. **Titles AND descriptions**, accepting ~1.3 s (measured ~1.5 s as built) more per guide refresh (in the loading box) and
   ~27 MB more database.
3. **A click jumps to the programme in the grid**; Enter or double-click opens the programme dialog.
4. **Past programmes are hidden.**
5. **One or two typed characters search titles with the slower scan** (~30 ms) rather than waiting.
6. **A Now button** goes into the new toolbar in step 2.

**Explicitly not in step 2:** the calendar (step 3, parked); a second guide source; catch-up playback
of past results; moving the main window's CHANNEL search to FTS5 (its `LIKE` over 411k rows is still
~134 ms a query — the same `SQLITE_ENABLE_FTS5` makes it a natural follow-up); any mac UI.
