# Xtream VOD + Series — design

**Status:** design, not started. **Gate 1 (reconnaissance) is DONE** — see below. This document is
gate 2, and per `Win32/BACKLOG.md` no epic code lands before it exists and the shared-core boundary
has been flagged to the macOS team.

Written the same way `docs/THEME_ENGINE.md` was for the one prior epic of this size, and for the same
reason: the sandbox cannot see the GUI, the feedback loop with the owner is expensive, and a 3–5 week
monolith is exactly how this project ships badly.

---

## 1. What reconnaissance established

Run 2026-07-27 against the owner's real provider via `RabbitEarsCli --xtream`. **These are measured
numbers, not estimates** — every figure below came off one provider, and the second provider we meet
may differ. Where a decision depends on a number, that dependency is called out.

| | |
|---|---|
| `player_api.php` | Responds. `user_info.auth = 1`, `status = "Active"` |
| **`max_connections`** | **1** |
| Movies (`get_vod_streams`) | **43,599** items · 13.46 MB · 5.2 s |
| Series (`get_series`) | **13,152** items · 13.33 MB · 4.7 s |
| VOD categories | 67 · 5.2 KB · `parent_id` = 0 everywhere (flat) |
| Series categories | 39 · 3.0 KB · flat |
| `container_extension` | present, non-empty, on **all 43,599** movies (`mp4` / `mkv` / `avi`) |
| Play URL probe | `/movie/<u>/<p>/1218804.mp4` → **HTTP 302** (reachable) |
| Parse failures | none; 8 of 8 bodies parsed cleanly end to end |

### The four findings that drive the design

**F1 — `max_connections: 1` is the governing constraint.** One connection is the whole budget. A full
catalogue sync is two requests and ~10 s, which is cheap; but it must never overlap playback, and the
0.3.0 poster fetcher can have **no concurrency at all**. This is the same lesson the dead-link checker
learned the expensive way (`BACKLOG.md`): a greedy sweep reads as a scraper and an Xtream connection
cap kicks the user's actual playback. Design accordingly from the start rather than discovering it.

**F2 — there is no per-movie metadata.** `get_vod_info&vod_id=…` returned **204 bytes**, with `info`
as an *empty array* (PHP's `json_encode` of an empty associative array) and `movie_data` carrying only
id, name, added, category, container_extension. **No duration, no plot, no cast, no year.** Series
episodes, by contrast, carry a full ffprobe block (`info.duration_secs: 1696`, codec, resolution,
bitrate).

Consequence: **`Channel::durationSec` cannot be populated for a movie at import.** It has to come from
libVLC's `get_length` once the film is opened — which is exactly what the 0.2.16 seek work publishes
via `VlcPlayer::lengthMs()`. Duration is therefore a *cache filled by playback*, not import data.
`get_vod_info` is not worth calling per item at all on this provider (43,599 requests for nothing,
against a 1-connection cap — it would be actively harmful).

**F3 — poster art is mostly absent for movies, and present for series.** `stream_icon` is empty on
**39,048 of 43,599** movies (~90%). Series `cover` is well populated (TMDB URLs). This *inverts* the
backlog's assumption that 0.3.0 is "posters, then series": a movie poster grid would be ~90% blank
tiles and look broken. **Movies stay in the existing text grid; posters are a SERIES feature.** The
`tmdb` id is present on some movies, so enriching from TMDB is a possible later path — explicitly out
of scope (it is a second network dependency, a second API key, and a licensing question).

**F4 — types are mixed WITHIN a single field, not merely quoted consistently.** Not "this panel quotes
numbers" but "this panel quotes *some rows* of a field and not others":

| field | observed | note |
|---|---|---|
| `rating_5based` | `string+number` | 39,049 quoted of 43,599 — the rest are real numbers |
| `tmdb` | `string+number` | 1,992 quoted of 41,039 non-empty |
| `category_id` (series) | `string+null` | |
| `backdrop_path` | `array+null` | sometimes an array of URLs, sometimes `null` |
| `rating`, `added`, `is_adult`, `episode_run_time`, `last_modified`, `episodes[].id`, … | quoted numbers | universally |

A strict reader dies on this provider. `common/core/Json.h`'s tolerant scalar accessors
(`asInt64()` reads a JSON number *or* a numeric string; `asDouble()`, `asBool()` likewise) exist for
precisely this, and are already unit-tested against it. **Never add a "strict mode".** There is no
configuration in which the caller wants to hard-fail on a quoted number.

### Smaller observations worth recording

- `episodes` is a **map keyed by season number**, not an array — `{"1": [...], "2": [...]}`. Confirmed.
- `direct_source` is empty and `custom_sid` is null on all 43,599 movies → the play URL must be
  **constructed**, never taken from the payload. (Good for us: no credentials embedded in item data.)
- Series carry **both** `releaseDate` and `release_date` with identical values. Read one, ignore the other.
- Categories are flat (`parent_id` = 0 everywhere) — no tree to model.
- A movie carries **both** `category_id` (single, quoted) and `category_ids` (array of real numbers).
  Many-to-many is real; a movie can sit in several categories.
- `seasons[].overview` contained a cover URL rather than prose — provider data quality is uneven.
  Never assume a text field holds text.
- The account under test expires ~33 h after the run, and is a 1-connection line. Test windows are
  short and serialized; plan owner verification around that.

---

## 2. Shared-core boundary — **for the macOS team**

Everything in this epic except the UI lands in `common/`, so mac gets the engine free and owns a second
UI. Flagging early, as the theme engine did.

**Already landed on `main`** (commit `ffb69dc`, 0.2.16 groundwork):

- **`common/core/Json.{h,cpp}`** — new. Hand-rolled per house style (M3U and XMLTV are too). Tolerant
  scalars, strict structure. 32 selftests in the shared suite.
- **`common/models/Channel.h`** — gains `Kind {Live, Movie, Episode}`, `durationSec`, `resumeSec`,
  `watched`, `addedAt`, `isVod()`. **All default to the live-TV answer.**
- **Schema v8** — `channels.kind/duration_sec/resume_sec/watched/added_at`, `idx_channels_kind`
  (partial, `WHERE kind<>0`). Migration is incremental and idempotent on `PRAGMA user_version` like
  every step before it.

⚠️ **Two things the mac team must check** (the traps that have cost this project real time before):

1. **A mac build older than v8 opening a v8 DB is safe** — it takes the `v >= 7` early return and
   ignores the extra columns. Verified by reasoning, not by running a mac build; please confirm.
2. **`Channel` gained fields.** Anything on the mac side that constructs a `Channel` positionally, or
   parses one with exact arity (the way `mac/src/app/MeterModel.cpp` parses `MeterTuning` with
   `!= 5`), needs a look. The shared DAO handles the DB path, so this is only about mac-side code that
   builds or serializes a `Channel` itself.

**Still to land in `common/`** (this document's subject): `core/XtreamClient.{h,cpp}` and the VOD DAO
additions. Neither has a mac UI implication beyond "you can call it".

**`RabbitEarsCli --xtream` (`Win32/cli/XtreamRecon.*`) is deliberately Win32-only and deliberately
disposable** — a permissive census tool, explicitly *not* the production JSON path. Delete it whole
when this epic ships. It is not worth porting.

---

## 3. Data model

**A movie is a `channels` row with `kind = Movie`.** Not a second table. This is the decision the rest
of the design hangs off, and the reason it works is that an Xtream movie *already* imports as a channel
today (in a group called "Movies") — the shape fits; only the discriminator and a few fields were
missing. It buys the existing grid, search, filters, favourites, dead-link handling, recording and
playback path for free, with no forked query layer.

The cost is honest and bounded: `channels` carries five columns that mean nothing for live TV. They are
`NOT NULL` with live-TV defaults, so an existing 442-channel library is untouched and the partial index
costs a live-only user nothing.

```
Channel::Kind  Live = 0   an ordinary channel; every pre-v8 row
               Movie = 1  an Xtream VOD stream
               Episode= 2 reserved for 0.3.0; not written yet
```

| field | source | note |
|---|---|---|
| `durationSec` | **libVLC at play time** | F2: the API does not have it for movies |
| `resumeSec` | our own playback | written on stop/pause; cleared when watched |
| `watched` | our own playback | set near the end (threshold TBD, ~95%) |
| `addedAt` | `added` (quoted epoch) | enables a "recently added" sort |

**Series are NOT modelled yet.** 0.3.0 adds `series` / `episodes` tables; `Kind::Episode` is reserved
so the enum does not have to change under a shipped DB. Deliberately not designed here — the shape is
known (§1) but designing storage for it before movies ship is exactly the monolith this plan avoids.

### URL construction

```
movie    {origin}/movie/{user}/{pass}/{stream_id}.{container_extension}
episode  {origin}/series/{user}/{pass}/{episode_id}.{container_extension}
```

Verified reachable (HTTP 302 — a redirect to an edge is healthy, the same judgement
`common/core/DeadLinkCheck` already encodes). `container_extension` is present on 100% of movies here,
but the client **must** handle absence rather than guessing a suffix: a guessed extension yields a 404
that reads as "VOD is broken" when the truth is "this panel does not tell us the container". Skip the
item and say so.

---

## 4. Sync strategy

Driven entirely by F1 (`max_connections: 1`).

- **Two requests for a full catalogue**, not per-item: `get_vod_categories` + `get_vod_streams`.
  ~13.5 MB / ~5 s for 43,599 movies. Paging by category would be 67 requests for the same data — worse
  under a connection cap. **Do not call `get_vod_info` per item** (F2: it returns nothing useful, and
  43,599 requests against a 1-connection line is abusive).
- **Never sync while playing.** One connection means a sync during playback fights the user's stream.
  Gate on `isPlaying()` across all panes, and make it a user-triggered action first (like the dead-link
  sweep), not a background timer.
- **Sync on a worker thread with its own sqlite connection**, joined in `WM_DESTROY` — the pattern
  `DeadLinkSweep` already establishes.
- **Bulk-insert in one transaction.** 43,599 rows through the existing
  `INSERT … ON CONFLICT(playlist_id, stream_url) DO UPDATE` path; the dedupe index already exists.
- **Deletions matter.** A provider drops films constantly. A sync must retire rows that vanished, or
  the library only ever grows. Scope retirement to `(playlist_id, kind=Movie)` so live channels are
  never touched by a VOD sync.

**Perf discipline, inherited:** the country filter benchmarked a C++-side materialize-all at ~30 ms per
keystroke on 14k channels and had to become a SQLite scalar (`BACKLOG.md`). This adds 43,599 rows to
the same table — **~4× the library**. Every existing cross-channel query needs re-measuring at that
size, and `kind` filtering must be server-side. This is the single biggest regression risk in the epic,
and it lands on features that are already shipped and working.

---

## 5. Release plan

⚠️ **The two planned releases MERGED into one — ✅ SHIPPED as v0.2.16 (2026-07-28).** 0.2.16 was
built and version-bumped but never tagged and never given an appcast, so no user ever had it; both
halves therefore went out together. What merged is this plan, **not the version numbering — 0.2.17 is
the next version, unused and available.**

- **✅ SHIPPED in v0.2.16 — groundwork** (`ffb69dc`). Player seek + scrub bar + time readout; JSON
  reader; schema v8; buffer-meter glass. Independently useful, all of it a prerequisite.
- **✅ SHIPPED in v0.2.16 — Xtream client, movies only, existing grid, no posters.**
  `common/core/XtreamClient` (`538f0b2`), the sync worker + Movies nav root + Settings action
  (`e4e01a7`).
  Success was written as "43,599 movies in the grid, one plays, **resume works**" — the first two are
  built; resume is NOT, because it depends on three of §6's open questions that remain owner calls.
  **As shipped this is browse-and-play**; resume is listed under 0.3.0 anyway ("resume everywhere").
  ⚠️ The sync itself shipped **never having run against a live provider** (the test line expired) —
  see HANDOVER "What still needs the owner".
- **NEXT (0.2.17 or 0.3.0) — series → seasons → episodes, posters *for series*, resume everywhere.**
  §1 already carries the measured shape for series. A minor bump would mark the capability's arrival;
  0.2.17 is equally available if it lands incrementally.

---

## 6. Open questions

1. ✅ **ANSWERED — and it did.** Measured before shipping, with `RabbitEarsCli --benchdb`. Every
   country/group path was made immune (see BACKLOG), but **the search box was not**: one keystroke
   is **0.63 → 80.00 ms**, because `EN_CHANGE` runs `searchChannels()` on the UI thread with no
   debounce, no minimum length and no `LIMIT`, and the query has no `kind` predicate. That is the
   one live-TV path this epic degrades, and how to fix it (LIMIT / debounce / minimum length) is an
   open owner call — see BACKLOG. `allChannels()` / `channelsByPlaylist()` also grow to ~80 ms, but
   that is deliberate: the All view legitimately grows with the row count.
2. ✅ **ANSWERED — a sibling root, "Movies", showing CATEGORIES ONLY.** `ViewKind` gained `Movies` +
   `MovieGroup`. The root is omitted entirely when the library has no movies, so the sidebar is
   byte-identical for live-TV-only users, and selecting it loads no grid rows (`allMovies()` is
   43,599 rows / 77 ms and is never called from the UI) — it expands its category list instead.
3. **`watched` threshold** — 95% of duration, or "reached the end"? Duration is only known once played
   (F2), so this interacts with when `durationSec` gets cached.
4. **Resume prompt or silent resume?** Silent is friendlier; a prompt is safer when `resumeSec` is
   stale. Owner call.
5. **Second provider.** Every number here is from one panel. The client must not encode this panel's
   quirks as assumptions — F4 is the proof that panels differ from the spec, and they differ from each
   other too. `--xtream` exists to re-run against any new provider cheaply.
