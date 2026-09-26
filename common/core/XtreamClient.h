// SPDX-License-Identifier: GPL-3.0-or-later
//
// XtreamClient — turns Xtream-Codes `player_api.php` responses into RabbitEars models.
//
// DELIBERATELY PURE: nothing here touches the network. The caller fetches the bytes
// (WinHTTP on Windows, NSURLSession on mac) and hands them in. Three reasons, and the
// last one is the important one:
//   1. it keeps every shape decision unit-testable headlessly, against fixtures taken
//      from a real provider;
//   2. it keeps the shared core free of an HTTP dependency, as `common/` already is;
//   3. **the provider's connection cap belongs to the caller.** The owner's line reports
//      `max_connections: 1` — one connection is the ENTIRE budget — so a sync must never
//      overlap playback, and only the caller knows whether a pane is playing. Putting the
//      fetch in here would hide that decision somewhere it cannot be made correctly.
//
// Everything below is built on what `RabbitEarsCli --xtream` actually measured against a
// real panel (Win32/docs/XTREAM_VOD.md §1). Two of those findings shape this API directly:
//
//   • TYPES ARE MIXED WITHIN A SINGLE FIELD — not merely "this panel quotes numbers", but
//     the same field quoted on some rows and bare on others (`rating_5based` and `tmdb`
//     came back `string+number`; `category_id` `string+null`). Every read here goes through
//     JsonValue's tolerant accessors. A strict reader dies on this provider.
//   • THERE IS NO PER-MOVIE METADATA. `get_vod_info` returned 204 bytes with an empty
//     `info`: no duration, plot, cast or year. So this client never calls it (43,599
//     requests, for nothing, against a one-connection cap), and duration is left to be
//     cached from libVLC at play time.
#pragma once

#include <string>
#include <vector>

#include "core/Json.h"
#include "models/ParsedChannel.h"

namespace rabbitears {

// ---------------------------------------------------------------------------
// Credentials + URL construction
// ---------------------------------------------------------------------------

struct XtreamCreds {
    std::wstring origin;    // "http://host:port" — scheme + authority, no trailing slash
    // DECODED. They are read out of a query string but also have to be emitted into a PATH
    // (/movie/USER/PASS/), and the encodings differ — '+' is a space in a query and a literal
    // '+' in a path. Storing the raw query spelling made the API URL correct and the PLAY URL
    // silently wrong, which is the worst place for the divergence to land: the auth probe
    // round-trips through a query and succeeds, so login works and only playback fails.
    std::wstring username;
    std::wstring password;
    bool valid() const { return !origin.empty() && !username.empty() && !password.empty(); }
};

// Pull credentials out of the `get.php?username=…&password=…` playlist URL the app already
// stores. Returns false when the URL is not an Xtream playlist — which is a legitimate
// answer, not an error: plenty of providers hand out a plain .m3u with no API behind it.
bool parseXtreamPlaylistUrl(const std::wstring& playlistUrl, XtreamCreds& out);

// `…/player_api.php?username=…&password=…[&action=…]`. Empty action = the auth probe.
std::wstring xtreamApiUrl(const XtreamCreds& c, const std::wstring& action = {});

// `{origin}/movie/{user}/{pass}/{streamId}.{ext}` — verified reachable (HTTP 302, a
// redirect to an edge, which is healthy). Returns empty when `ext` is empty: a GUESSED
// suffix yields a 404 that reads as "VOD is broken" when the truth is "this panel did not
// tell us the container", and those need to stay distinguishable.
std::wstring xtreamMovieUrl(const XtreamCreds& c, long long streamId, const std::wstring& ext);

// `{origin}/series/{user}/{pass}/{episodeId}.{ext}` — the 0.3.0 shape, here because it is
// the same rule and belongs beside its sibling.
std::wstring xtreamEpisodeUrl(const XtreamCreds& c, long long episodeId, const std::wstring& ext);

// A live channel's ARCHIVE (catch-up): `{origin}/timeshift/{user}/{pass}/{minutes}/{YYYY-MM-DD:HH-MM}/
// {streamId}.ts`, from `startUtc` for `minutes`. The panel reads that start on ITS OWN wall clock, so
// `serverUtcOffsetSec` — the server's local time minus UTC at `startUtc` (see XtreamAccount) — is
// added first; seconds are dropped. Measured on the owner's panel (Europe/Amsterdam, 2026-09-25): this
// form returned MPEG-TS for archived programmes; `streaming/timeshift.php?…` and `.m3u8` did no better.
// A channel can advertise an archive and still answer an empty 200 for a time it did not record.
// Empty when the credentials, id or duration are unusable.
std::wstring xtreamTimeshiftUrl(const XtreamCreds& c, long long streamId, long long startUtc, int minutes,
                                int serverUtcOffsetSec);

// The stream id in an Xtream LIVE stream URL — `{origin}/{user}/{pass}/{id}[.ext]` or
// `{origin}/live/{user}/{pass}/{id}[.ext]` — or 0 for anything else. Movie and series URLs
// (`/movie/{user}/{pass}/…`, `/series/…`: one segment more) give 0 even though they end in a number:
// ids are numbered per kind, so a film's id can equal a live channel's, and get_live_streams' ids name
// live channels only. With `creds`, the path's user and password (percent-decoded) must also be that
// login's — so a non-Xtream `/a/b/123` in the same playlist is not taken for stream 123.
long long xtreamLiveStreamId(const std::wstring& streamUrl, const XtreamCreds* creds = nullptr);

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

struct XtreamAccount {
    // user_info.auth, with an absent member falling back to `status` — see the .cpp.
    // ⚠ authOk means "the credentials were accepted", NOT "the account is usable": it is
    // true for a rejected-for-other-reasons line, so a caller must check `status` too.
    bool         authOk = false;
    std::wstring status;                // "Active", "Expired", "Banned", …
    long long    expiresAt = 0;         // unix epoch; 0 == unknown/unlimited
    int          maxConnections = 0;    // 0 == not reported. THE constraint on any sync.
    long long    serverTime = 0;        // server_info.timestamp_now; 0 == absent
    // The server's wall clock, for catch-up URLs (xtreamTimeshiftUrl): server_info.timezone (an IANA
    // name, e.g. "Europe/Amsterdam"; empty == absent) and server_info.time_now ("YYYY-MM-DD HH:MM:SS",
    // local) read as if it were UTC — so serverLocalTime - serverTime is the server's UTC offset at
    // that moment. 0 == absent or unparseable.
    std::wstring timezone;
    long long    serverLocalTime = 0;
};

// The server's UTC offset from an account probe — serverLocalTime - serverTime, rounded to the quarter
// hour (every offset in use today is a multiple of 15 min; the two readings can straddle a second
// tick). False — `*offsetSec` untouched — when either reading is missing or the result lies outside
// −12 h … +14 h (a millisecond timestamp, a mis-set clock): no answer beats a wrong one, since a
// wrong offset silently plays the wrong part of the archive.
bool xtreamServerUtcOffset(const XtreamAccount& a, int* offsetSec);

// A panel answers BAD CREDENTIALS with HTTP 200 and `{"user_info":{"auth":0}}`, so
// "it responded" and "it let us in" are different questions. `authOk` is the second one.
bool parseXtreamAccount(const std::string& body, XtreamAccount& out, std::wstring* err = nullptr);

struct XtreamCategory {
    std::wstring id;    // quoted number on every panel seen; kept as text — it is a key
    std::wstring name;
};
// Shared by get_vod_categories / get_series_categories / get_live_categories: same shape.
// `parent_id` is ignored — it was 0 everywhere, so the category list is flat.
bool parseXtreamCategories(const std::string& body, std::vector<XtreamCategory>& out,
                           std::wstring* err = nullptr);

struct XtreamMovie {
    long long    streamId = 0;
    std::wstring name;
    std::wstring icon;          // stream_icon — EMPTY on ~90% of items on the panel measured
    std::wstring containerExt;  // mp4 / mkv / avi
    std::wstring categoryId;
    long long    added = 0;     // quoted epoch
    bool         adult = false; // is_adult, quoted "0"/"1"
};

struct XtreamVodResult {
    std::vector<XtreamMovie> movies;
    // Skips are COUNTED, not silent. A sync that quietly drops 4,000 films looks like a
    // provider with a small library; the caller surfaces these so it looks like what it is.
    int skippedNoId = 0;   // no usable stream_id/vod_id
    int skippedNoExt = 0;  // container_extension absent or empty -> no constructible URL
    int total = 0;         // array elements seen, including the skipped ones
};

bool parseXtreamVodStreams(const std::string& body, XtreamVodResult& out,
                           std::wstring* err = nullptr);

// One live stream that keeps an archive (catch-up), from get_live_streams.
struct XtreamArchive {
    long long streamId = 0;
    int       days = 0;  // tv_archive_duration
};
// get_live_streams → the streams with `tv_archive` set and a positive `tv_archive_duration` (the owner's
// panel: 299 of 15,345, 1–3 days). `total` (optional) = the entries in the list. Fails — rather than
// answering "no archives" — on anything but a JSON array (an HTML error page, an object), so a caller
// never replaces good archive data with nothing because a request went wrong.
bool parseXtreamLiveArchive(const std::string& body, std::vector<XtreamArchive>& out, size_t* total = nullptr,
                            std::wstring* err = nullptr);

// ---------------------------------------------------------------------------
// Model mapping
// ---------------------------------------------------------------------------

// Build the rows for Database::bulkInsertChannels(). `categories` supplies the display name
// for group_title (so VOD lands in the existing nav tree with no new concept); a movie whose
// category is unknown falls back to `fallbackGroup`.
//
// Movies with no constructible URL were already dropped by parseXtreamVodStreams, so every
// row this returns is playable-by-construction.
std::vector<ParsedChannel> xtreamMoviesToChannels(const XtreamCreds& c,
                                                  const std::vector<XtreamMovie>& movies,
                                                  const std::vector<XtreamCategory>& categories,
                                                  const std::wstring& fallbackGroup = L"Movies");

}  // namespace rabbitears
