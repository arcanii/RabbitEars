// SPDX-License-Identifier: GPL-3.0-or-later
//
// UrlCanon — one canonical spelling for a stream URL.
//
// WHY THIS EXISTS. `channels` dedupes on `(playlist_id, stream_url)` as a LITERAL string, and two
// spellings of the same stream therefore store as two channels. That is not hypothetical: on the
// owner's real provider (2026-07-28) the m3u_plus playlist emits
//
//     http://line.example.ru:80/movie/USER/PASS/1218804.mp4
//
// while the VOD sync constructs the origin from the playlist URL, which was entered without a port:
//
//     http://line.example.ru/movie/USER/PASS/1218804.mp4
//
// Same stream id, same container, different string — so the first sync INSERTED 43,606 movies
// beside the 43,599 the m3u had already imported instead of upgrading them in place, and every film
// now appears twice. `bulkInsertChannels` documents that collision as happening "BY DESIGN"; a real
// provider falsified it with one `:80`.
//
// ⚠️ SCOPE IS DELIBERATELY MINIMAL, AND MUST STAY THAT WAY. This strips the scheme's DEFAULT port
// and does nothing else. It does not lowercase the host, touch the path, sort the query, decode
// percent-escapes, or strip a trailing slash — every one of which is a legitimate URL
// canonicalisation and every one of which can change WHICH RESOURCE the URL names on a provider
// that does not follow the spec. The failure mode here is not a wrong dedupe: a canonicaliser that
// is even slightly too eager REPOINTS EVERY STREAM IN THE LIBRARY, silently, and the user finds out
// when nothing plays. `http://h:80/x` and `http://h/x` are the same resource by RFC 3986 §6.2.3,
// which is the one rewrite that carries no such risk.
//
// ⚠️ AND IT IS NOT SAFE TO APPLY ON ITS OWN. Canonicalising on the WRITE path without first
// migrating the stored rows is actively destructive: 410,147 of the owner's 454,195 rows carry an
// explicit `:80`, so the next playlist refresh would write the canonical spelling, miss every
// existing row on the dedupe index, and insert 410k duplicate live channels. The migration must
// land in the same change, and it must run first. See Win32/BACKLOG.md.
#pragma once

#include <string>
#include <string_view>

namespace rabbitears {

// The canonical spelling of `url`: identical to the input except that an explicitly written
// DEFAULT port for the scheme is removed (`:80` on http, `:443` on https).
//
// Everything else is returned byte-for-byte unchanged, including: a non-default port, a port that
// is default for a DIFFERENT scheme (`:80` on https stays), any non-http(s) scheme (rtsp, rtmp,
// udp, file), userinfo, IPv6 literals, the path, the query and the fragment. Input that does not
// parse as scheme://authority is returned untouched rather than guessed at.
//
// Idempotent: canonicalStreamUrl(canonicalStreamUrl(u)) == canonicalStreamUrl(u).
std::wstring canonicalStreamUrl(const std::wstring& url);

// The same rule over UTF-8 bytes, for the SQLite scalar that rewrites the column in bulk (410,147
// rows on the owner's library — converting each one to UTF-16 and back would dominate the cost).
//
// Byte-identical results are guaranteed rather than hoped for: every delimiter the rule looks at
// (`:` `/` `?` `#` `@` `[` `]` and the ASCII digits) is below 0x80, and UTF-8 never encodes those
// bytes inside a multi-byte sequence — so a host or path containing non-ASCII cannot be misparsed,
// and the two overloads are pinned against each other in --selftest.
std::string canonicalStreamUrlU8(std::string_view url);

}  // namespace rabbitears
