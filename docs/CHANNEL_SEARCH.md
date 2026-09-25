# Channel search on FTS5 — design and as-built record

> **Status (2026-09-25):** built for 0.2.20 (uncommitted at the time of writing); both decisions below
> taken by the owner. Shared core: the index, the query and the schema step live in `common/db`, so the
> mac app's channel search changes with it (see "For the mac team").

## Why

The main window's search box (`Database::searchChannels`, run on its 200 ms debounce tick on the UI
thread) matched `name`, `group_title` and `tvg_name` with `LIKE '%text%'`: **120–180 ms per search** on
the owner's 410,596 channels, ASCII-only case folding ("québec" found 2 of the 20 Québec channels), and
typing a group's name ("netflix", "quebec") flooded the 5,000-row grid with series episodes whose
GROUP matched (46,882 and 7,236 matches). The TV Guide's programme search (0.2.19,
`docs/EPG_SEARCH.md`) had just brought FTS5 into the build.

## Decisions (owner, 2026-09-25)

1. **Channel names only.** Groups stay browsable in the sidebar. *(Rejected: today's three fields —
   +74 MB instead of +47, and the group-title floods above.)*
2. **Kept current by triggers**, not rebuilt after writes. *(Rejected: the guide index's approach — a
   full ~4 s rebuild after every playlist add and VOD sync, a slow LIKE window in between, and the mac
   app getting the speed-up only once its code calls the rebuild.)* The price, accepted: see below.

## As built

- **Schema v11** (`Database::createChannelSearchIndex`, from `migrate()` after v10 lands in the same
  open, or directly from a v10 database): `channels_fts`, an external-content FTS5 table
  (`content='channels', content_rowid='id'`, `tokenize='trigram remove_diacritics 1'`) over
  `channels.name`; three triggers — after INSERT, after DELETE, and after `UPDATE OF name` **when the
  name actually changed** (`bulkInsertChannels`' upsert rewrites `name=excluded.name` on every row of a
  refresh) — and the `'rebuild'` of the rows already there. Table, triggers, build and
  `user_version=11` in ONE transaction, built from clean (any leftover triggers and FTS5
  `channels_fts` dropped first; an ordinary table squatting on the name is left alone and fails the
  step): a failure (a squatting table, a busy writer) leaves v10 with **no triggers**, so channel
  writes keep working, and the next open retries. On Windows only the APP's connection upgrades: the
  VOD sync's and the dead-link sweep's own connections open with `upgradeSchema=false`, so a step the
  app could not complete is never retried from a worker holding the write lock for seconds. (The mac
  workers, `mac/src/app/VodSync.mm` and `DeadLinkSweep.mm`, still open with the default — see below.)
- **Every writer is covered with no caller involved:** a playlist add (UI thread), the VOD sync (its
  own connection, both platforms), `retireMissingChannels`, and a playlist delete's `ON DELETE CASCADE`
  (foreign-key actions fire the triggers). The upsert never uses `REPLACE` (whose implicit delete
  would skip the delete trigger).
- **`searchChannels(term, filter)`:** channels (any kind) in enabled playlists whose name contains
  `term` — 3+ characters through the index (the whole text as ONE quoted FTS5 phrase: a substring,
  typed `*`, `"`, `AND` … literal), case- and accent-blind for Latin; under 3 characters, or without
  v11, `name LIKE ?1 ESCAPE '\'` (typed `%` and `_` literal — they were wildcards before). Same order
  and grid filter/cap as before: `WHERE id IN (SELECT rowid FROM channels_fts WHERE channels_fts MATCH
  ?1) AND …`, so `runChannelQuery`/`gridWhere` are unchanged.
- **`countUncoveredChannelNames(text, guideIds, maxRows)`:** distinct names of LIVE channels in
  enabled playlists matching the same way, NONE of whose channels carries one of the guide rows' ids
  (so an FHD beside the HD the guide shows is not counted); -1 under 3 characters, without the index,
  or past `maxRows` matching channels. Driven FROM the index (a CROSS JOIN), so its LIMIT stops a broad
  word early (~3.5 ms for "the"). The TV Guide search's "Also in your channel list, not in the guide: N"
  note. Also added: `liveGuideIds()` / `distinctLiveGuideIds()` for the guide's coverage line.
- **`channelSearchIndexed()`** — true once v11 landed on this connection.

## Measured (the real code, a copy of the owner's library — `RabbitEarsCli --epgsearch <copy>`)

| | before (LIKE, 3 fields) | v11 |
|---|---|---|
| "toronto", "quebec", "news", "netflix", "bbc one" | 120–180 ms | **0.1–1 ms** |
| "the" (5,001-row cap) | 177 ms | 44–48 ms |
| 2 characters ("to") — also most CJK searches (trigram needs 3) | ~130 ms | 107–153 ms (LIKE, names only: no gain) |
| the guide note's count | — | well under 1 ms for a specific name; ~3.5 ms for a broad word, which gives up past 5,000 rows |
| **v10 → v11 upgrade (once, at the first open)** | — | **3.9–4.5 s** on 410k channels (typical libraries: well under 0.1 s) |
| index size | — | +47 MB |
| trigger cost on writes | — | ~5 ms per 1,000 rows (+2 s on a 410k-row playlist add, measured in Python) |

## The price of triggers (accepted)

A build **without FTS5** cannot insert, update or delete `channels` once the triggers exist ("no such
module: fts5"): every Windows release through **0.2.18** and the mac app through **0.2.17**. Installing
one of those over a v11 database breaks adding, refreshing and deleting playlists (the delete silently —
its statement fails to prepare) and the VOD sync; reads, favourites and dead-link marks still work.
0.2.19+ has FTS5; auto-update only ever moves forward.

## Tests (`RabbitEarsCli --selftest`, "Channel search (schema v11, FTS5 + triggers)")

Accent/case-blind name match; a substring across a space; a group title no longer matching; the
2-character LIKE path; `%`/`_` literal on both paths; a rename by refresh (update trigger); a retired
movie and a deleted playlist's channels gone from the INDEX itself (queried directly — a search joins back
to `channels` and would hide a stale entry); a disabled playlist; `countUncoveredChannelNames` (movies
excluded, both feeds of a guide row's id skipped, -1 under 3 characters and past `maxRows`); **FTS5's own
`integrity-check` against `channels` after all of it**; a v10 database upgraded with its existing
channels indexed; a failed v11 (a squatting table) leaving v10, NO triggers and working writes, then
landing on the next open; a real v9 database going v9 → v10 → v11 in one open; the v2 → v11 chain; the
grid cap on the index path. **Shown to fail** with the delete trigger removed (integrity-check) and with the update
trigger removed (the rename check and integrity-check).

## For the mac team

`searchChannels` is shared, so a mac build of this commit searches **names only**, case- and
accent-blind, and fast — no mac code change needed, since the triggers keep the index current for
the mac's own writers too (its playlist import and VOD sync call the same `bulkInsertChannels` /
`retireMissingChannels`). Its first launch runs the v11 upgrade (~4 s at 410k channels). The downgrade
note above applies to mac 0.2.17. `countUncoveredChannelNames`, `liveGuideIds`, `distinctLiveGuideIds`,
`channelSearchIndexed` and `open()`'s defaulted `upgradeSchema` are additive — the mac VOD sync and
dead-link workers could pass `upgradeSchema=false` too, so a failed v11 is never retried from a worker
holding the write lock (Windows does). `channelByTvgId` now prefers a live channel over a movie carrying
the same tvg-id. A comment in
`mac/src/app/MainWindowController.mm` (~line 1290, "GLOBAL triple-LIKE … ~1.6 s") now describes the old
search — theirs to update.
