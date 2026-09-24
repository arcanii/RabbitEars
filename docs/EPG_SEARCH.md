# EPG programme search — design

> **Status (2026-09-24):** design **approved by the owner** (all six decisions in §7 taken as
> recommended); implementation starting. Step 2 of the owner's two EPG
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
| schema **v10**: create the two FTS5 tables (empty) | `common/db/Database.cpp` `migrate()` | instant, at the mac app's first launch after the merge. **No index is built then** — see §3. **Please confirm there is no competing v10 in flight.** |
| new `Database::rebuildProgrammeIndex()`, `searchProgrammes()` | `common/db/Database.{h,cpp}` | available; **nothing runs them unless called**. `bulkInsertProgrammes` is NOT changed, so a mac guide refresh costs exactly what it does today. |
| i18n | `common/i18n/*.json` | new keys appended; mac need not use them. |

No enum a mac `switch` depends on changes (no mac `switch` uses `StringId`). A mac build **older** than
this change, opening a v10 database, behaves like the FTS5-less probe above: works, and changes
`epg_programmes` without re-indexing — which the stamp (§3) detects the next time anything searches.

**If mac adds search later:** call `rebuildProgrammeIndex()` after its refresh stores (or let the first
search do it — §3), and reuse `searchProgrammes()`.

---

## 3. Data model

### Schema v10 — two empty tables

```sql
CREATE VIRTUAL TABLE epg_fts_title USING fts5(
    title, content='epg_programmes', content_rowid='id',
    tokenize='trigram remove_diacritics 1');
CREATE VIRTUAL TABLE epg_fts_descr USING fts5(
    descr, content='epg_programmes', content_rowid='id',
    tokenize='unicode61 remove_diacritics 2');
```

- **External content** (`content='epg_programmes'`): the text is stored once, in `epg_programmes`; the
  index holds only tokens. Trigram's `remove_diacritics` is parsed in its create (`sqlite3.c:266895`;
  1 and 2 both select the same fold mode there, `:266899`), and the vendored probe built and queried
  both tables with exactly these options. Side effect: with `remove_diacritics` on, FTS5 no longer
  accelerates `LIKE`/`GLOB` on the trigram table (`:267014`) — irrelevant here, since the short-query
  fallback (§4) scans `epg_programmes` directly.
- **No triggers.** Triggers would make every write to `epg_programmes` depend on FTS5, and a build
  without it would then fail every guide refresh with "no such module". The index is maintained
  explicitly instead. The probe above is what makes this a verified property, not a hope.
- **The migration must not re-run v9.** `migrate()` has no per-step gate: its only guard is the early
  `if (v >= 9) return;` (`Database.cpp:511`); after it, `user_version` is rewritten from the column
  checks (8 at most, `:666`) and v9's channel-URL rewrite runs whenever v8's columns exist (`:685`).
  Simply raising the early return to `v >= 10` would send every v9 database back through v9 (and set
  `user_version` to 8 on the way). So: wrap the v2–v9 steps in `if (v < 9) { … }`, and give v10 its own
  step — `CREATE VIRTUAL TABLE IF NOT EXISTS` ×2, then `user_version=10` only when both tables exist.
  The realistic failures (`SQLITE_BUSY`, a full disk) leave v9, retry on the next open, and search falls
  back to `LIKE` meanwhile (§4). It cannot fail for want of FTS5: one `sqlite3` target feeds every
  binary that contains this step.

### Keeping the index true — a stamp, and one rebuild after a refresh

- **`rebuildProgrammeIndex()`** — its own `BEGIN IMMEDIATE` transaction: `'rebuild'` both tables, then
  write `epg_fts_stamp` to `settings` — the row count, the max `id`, and the concatenated
  `epg_refreshed_<playlist>` values. `bulkInsertProgrammes` has rewritten those on every refresh since
  the commit that created schema v3 (`910e19e`), so every older build changes them too. Count and max
  id alone would NOT do: an old build re-importing a same-sized guide reuses the same ids.
- **Win32 calls it ONCE per refresh**, after `onEpgDone` has stored every playlist — not inside
  `bulkInsertProgrammes`, where N guide playlists would mean N full rebuilds (`'rebuild'` re-indexes the
  whole table). The loading box names the step ("Indexing the guide for search…"). Measured on the
  owner's guide: **+1.27 s** (330 + 938 ms). Nothing can search in between: the store and the rebuild
  run back-to-back on the UI thread, which is also the thread that searches.
- **Search checks the stamp first** (`COUNT(*)` + `MAX(id)` + the settings: **7 ms** on the real
  guide, measured — so once per guide open, not per keystroke) and rebuilds when it does not match,
  behind "Preparing search…". That one rule covers everything else: the first search after upgrading
  (v10 created the tables empty), a playlist deleted (count changed), a refresh by an older build or by
  the mac app, and a failed rebuild. **Until the rebuild has run, the index must not be read** — see
  the next point.
- **Stale entries are dangerous, not just untidy.** FTS5 with external content reads the content row
  for columns, `snippet()` and `highlight()`; a stale rowid there fails with `SQLITE_CORRUPT_VTAB`
  ("fts5: missing row … from content table", `sqlite3.c:261872–261883`), and a REUSED rowid silently
  returns the wrong programme (a single-guide refresh reuses ids from 1). The stamp check before every
  search session is what makes reading the index safe; the match queries additionally read **rowids
  only** (§4).

### The write lock

`BEGIN IMMEDIATE` holds SQLite's writer lock for its whole transaction; the Win32 VOD sync and dead-link
workers (`Win32/ui/VodSync.cpp:97`, `Win32/ui/DeadLinkSweep.cpp:39`; mac has the same two) open their
own connections and wait up to `busy_timeout` (5 s, `Database.cpp:421`), then fail with `SQLITE_BUSY`.
With the rebuild in its own transaction, no single hold grows: the store stays ~1 s, the rebuild adds a
separate ~1.3 s on this guide. A guide ~4× the owner's would push the rebuild alone toward 5 s.
**Measure on the real library before choosing anything cleverer.** The cleverer option on record —
incremental per playlist, FTS5 `'delete'` rows for the old programmes and indexing only the new — is
fragile with external content: `'delete'` must be given the exact values that were indexed, so it has
to run BEFORE the old rows are deleted and only when the stamp says the index is in sync, or it
corrupts the index.

---

## 4. The query

`Database::searchProgrammes(text, fromUtc, limit, …)` — two queries.

**1. Match, filter, rank — rowids only.** Both FTS arms read `rowid` and nothing else; the join back to
`epg_programmes` drops any id that no longer exists; the channel filter runs in SQL so the `LIMIT` counts
only rows the user can play:

```sql
SELECT p.id, MAX(m.inTitle) AS inTitle
FROM (SELECT rowid AS id, 1 AS inTitle FROM epg_fts_title WHERE epg_fts_title MATCH :titleQ
      UNION ALL
      SELECT rowid, 0 FROM epg_fts_descr WHERE epg_fts_descr MATCH :descrQ) m
JOIN epg_programmes p ON p.id = m.id
JOIN temp.guide_channels g                      -- the user's channels, see below
  ON g.playlist_id = p.playlist_id
 AND g.cid = lower(substr(p.channel_id, 1, instr(p.channel_id || '@', '@') - 1))
WHERE p.stop_utc > :fromUtc
GROUP BY p.id
ORDER BY inTitle DESC, MIN(p.start_utc)
LIMIT :limit;
```

- `temp.guide_channels(playlist_id, cid)` holds the (playlist, normalised tvg-id) pairs of every
  channel in an **enabled** playlist, built with the same `normId` rule, per playlist — exactly the rows
  the guide can show. SQLite's `lower()` is ASCII-only, like `normId`. Built when a search session
  starts (it is small: 2,495 pairs here); **not measured yet**.
- **Title matches first, then description-only matches, each soonest first.** Past programmes are
  excluded (`fromUtc` = now) — they cannot be played, except on the 298 catch-up channels, a later
  feature. *(Decided, §7.4.)*

**2. Details for the ≤ 200 winners** — `SELECT … FROM epg_programmes WHERE id IN (…)`, and for the
description matches a `snippet(epg_fts_descr, …)` query restricted to those ids (`… MATCH :descrQ AND
rowid IN (…)`), which only ever reads rows that exist. Not measured yet; bounded by the 200 ids.

**The user's text is never FTS5 syntax.** Typed `"`, `*`, `(`, `-`, `AND`/`OR`/`NOT`/`NEAR` must search
for themselves. Title query: the trimmed text as ONE quoted phrase (inner `"` doubled) — trigram
matches it as a substring, and spaces count as characters, hence the trim. Description query: each word
quoted and simply juxtaposed (FTS5's implicit AND, which — unlike an explicit `AND` — drops a word that
yields no tokens, such as a lone `-`), the last one a prefix: `"doctor" "who"*`.

**1–2 characters:** trigram yields no tokens for a term under 3 characters, so the phrase matches
nothing. Those fall back to `LIKE` on titles (~30 ms), rather than waiting for the third character.
*(Decided, §7.5.)*

**No index (v10 not landed):** the same function answers with `LIKE` over titles + descriptions
(46–70 ms, ASCII-only case folding) — slower and less complete, never broken.

**Limit 200**, with "showing the first 200 — type more to narrow it" when hit (now true, since the
limit counts playable rows).

---

## 5. The UI (Win32)

**The layout** *(decided, §7.1)*: keep the corner cell as the **channel filter** the owner
already uses — but make it a real EDIT (the missing caret, paste, selection, IME) — and add a
**"Search programmes…" box** in a new thin toolbar strip above the hour axis. The strip also holds a
**Now** button (scrolls the time axis back to now — decided, §7.6), and is where step 3's day buttons
would go.

- Both boxes are standard `EDIT` controls themed like the main search box (`dialogCtlColor` for
  `WM_CTLCOLOREDIT`, a cue banner). The guide window is a Direct2D surface without `WS_CLIPCHILDREN`
  today (`EpgGuideControl.cpp:675`); it gains it, so the grid never paints over the boxes.
- **Keys:** a focused EDIT receives Esc/Enter/↓ itself, not the guide's window procedure, so both boxes
  are subclassed (precedent: `ChannelGridControl.cpp:462`) to keep Esc = clear, then close, and to add
  ↓/Enter. Today's "type anywhere in the guide to filter channels" survives by forwarding a printable
  `WM_CHAR` from the grid to the channel-filter box (focus it, then pass the character).
- **Programme search** debounces 200 ms exactly as the main search does (one-shot timer + a
  `searchPending` flag re-checked on the tick, because `KillTimer` does not remove an already-posted
  `WM_TIMER`).
- **Results replace the grid** while the box has text: a Direct2D list in the guide's own style — per
  row: day + time, channel name, **title** (matched text emphasised), and for description matches a
  one-line snippet. An "on now" badge for what is airing. Grouped under "Today" / "Tomorrow" / a date.
- **Keyboard:** ↓ from the box into the list, ↑/↓ to move, **Enter** = the programme dialog (Play /
  Schedule / Record series — the existing `programmeDialog`), **Esc** = back to the grid.
- **Mouse:** click = **jump to it in the grid** (scroll the time axis and the row into view, briefly
  highlight the block, keep the query) *(decided, §7.3)*; double-click = the programme dialog;
  right-click = Record / Record series / Show in guide.
- **The grid may be older than the search.** Search reads the database; the grid's rows can predate the
  last refresh (they survive a hide/reveal). If a result is not in the grid's rows, the jump rebuilds
  them first (the existing `onEpgGuide` path) rather than failing.
- **Nothing found / no guide / "Preparing search…"** each get a plain line, not an empty list.

New strings (appended to `keys.json`, en/ja/zh-Hant): the placeholder, the result count, "none found",
"showing the first 200", "on now", Today/Tomorrow, "Preparing search…", and the loading-box line
"Indexing the guide for search…". Dates in results need locale-aware formatting, which Win32 does not
have yet (the guide prints `HH:MM` only) — step 3 needs it too.

---

## 6. Testing

**Selftest (CLI, automated):**
- v9 → v10 on a database that already holds programmes: two empty tables, `user_version` 10, and **v9's
  URL rewrite NOT re-run** (a canary row whose stream URL v9 would rewrite stays untouched).
- A v10 step that fails leaves v9 and search still answers via `LIKE` — injected by pre-creating an
  ordinary table named `epg_fts_title`, so `CREATE VIRTUAL TABLE` fails.
- Search: title substring ("ronto" finds "Toronto"), accents ("quebec" finds "Québec"), non-Latin case
  (Greek); description words and a prefix; title matches ranked before description-only ones; past
  programmes excluded; a programme on a channel the user does not have is dropped **before** the limit;
  **typed FTS5 syntax searched literally** (quotes, `*`, parentheses, `AND`/`OR`/`NEAR`, a lone `-`).
- The stamp: a refresh then a search finds the new titles and not the old; deleting a playlist, a
  refresh without a rebuild (simulating an older build), and a same-sized re-import each force a
  rebuild; a stale index is never read (no `SQLITE_CORRUPT_VTAB`).
- Two guide playlists: one refresh → one rebuild.
- A performance guard on a large synthetic guide.

**Measured before committing, on a COPY of the real library** (`run-profile.ps1 -Refresh` snapshots it
into the dev profile, so v10 runs on real data without touching the real one): the migration, the
refresh's store + index time, the first-search rebuild, `temp.guide_channels` build time, and search
latency (with snippets and the channel filter) for common and rare words.

**Owner, live:** search for a show known to be on today; a word only in a description; an accented
title; "jump to it" and the programme dialog from a result; a refresh (the new "Indexing…" line); the
channel filter now shows a caret, and typing anywhere in the guide still filters.

---

## 7. Decisions — **DECIDED by the owner, 2026-09-24: "go with the recommendations"**

1. **Two boxes.** The corner cell stays the channel filter (now a real text box), and a separate
   "Search programmes…" box sits in a new toolbar. *(Rejected: one box listing channels AND
   programmes — typing would no longer narrow the grid the way the owner's "toronto" filter does.)*
2. **Titles AND descriptions**, accepting ~1.3 s more per guide refresh (in the loading box) and
   ~27 MB more database.
3. **A click jumps to the programme in the grid**; Enter or double-click opens the programme dialog.
4. **Past programmes are hidden.**
5. **One or two typed characters search titles with the slower scan** (~30 ms) rather than waiting.
6. **A Now button** goes into the new toolbar in step 2.

**Explicitly not in step 2:** the calendar (step 3, parked); a second guide source; catch-up playback
of past results; moving the main window's CHANNEL search to FTS5 (its `LIKE` over 411k rows is still
~134 ms a query — the same `SQLITE_ENABLE_FTS5` makes it a natural follow-up); any mac UI.
