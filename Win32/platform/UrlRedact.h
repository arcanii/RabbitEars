// SPDX-License-Identifier: GPL-3.0-or-later
// UrlRedact — keep provider logins out of the diagnostic log, and pull a URL out of pasted text.
//
// Why this exists: an Xtream provider puts the account's username and password in its playlist,
// guide and stream URLs — the playlist and guide links carry them as ?username=…&password=…, and
// each stream URL as path segments (http://host/USER/PASS/123, /live/USER/PASS/123.ts,
// /movie/USER/PASS/123.mkv). The log records playback, guide and playlist URLs, and it exists to
// be sent to someone after something went wrong — so without masking, sending it hands over the
// account.
//
// Pure functions (no Win32 state); the log sink (platform/Log.cpp) applies redactUrls() to every
// line. Windows-only: the mac log sink is mac/platform/Log.mm and is not covered by this.
#pragma once

#include <string>
#include <vector>

namespace rabbitears {

// The http(s) address in what a user pasted into a URL box, or empty when there is none.
//  - When `text`, less surrounding whitespace, is a single address (starts with http:// or
//    https://, no whitespace, quote or angle bracket inside), it is returned EXACTLY as typed —
//    a password may legitimately end in '.', '!' or ')'. A bare "http://" gives empty.
//  - Otherwise the first http:// or https:// occurrence is pulled out of the surrounding text (a
//    provider's email line "EPG Link : http://host/xmltv.php?…"): it runs to the first
//    whitespace, quote or angle bracket, trailing . , ; : ! ? are dropped, and so is a trailing
//    ')' when the address contains no '('. Empty when that first occurrence is a bare "http://".
std::wstring extractHttpUrl(const std::wstring& text);

// Does `url` look like a PLAYLIST link rather than an XMLTV guide link? For Set Guide URL, whose
// single-line prompt keeps only the first line of a pasted email block — often the M3U link, which
// then stores 0 programmes on every refresh. True for an Xtream playlist endpoint (…/get.php, or the
// path form …/m3u_plus and …/m3u), a type=m3u / type=m3u_plus query, or a path ending in .m3u /
// .m3u8; false for anything ending in
// .xml / .xml.gz / .gz or an xmltv.php endpoint, and for everything else (a guide link can look like
// anything, so only the clear playlist shapes are flagged). Case-insensitive.
bool looksLikePlaylistUrl(const std::wstring& url);
// Does `url` have one of a guide link's clear shapes — a path ending in .xml, .gz or …/xmltv.php
// (query and fragment ignored, case-insensitive)? Set Guide URL takes such an address from a pasted
// line that names the playlist link first.
bool looksLikeGuideUrl(const std::wstring& url);

// The credential values `url` carries in a form that can be recognised generically: the
// user-info part (user:pass@host) and the values of credential-named query parameters (username,
// password, and the like — see UrlRedact.cpp). Each is returned as written in the URL and, when
// different, percent-decoded; a query value also with '+' read as a space and then decoded (as
// XtreamClient reads it). Values shorter than 4 characters are skipped. A stream path's login is
// NOT included — see streamLoginToRegister.
std::vector<std::wstring> urlCredentials(const std::wstring& url);

// The two login segments of `url`'s Xtream stream path (see redactUrls), as written and
// percent-decoded, each at least 4 characters — but ONLY when at least one of them is already in
// `known`, i.e. the stream belongs to an account whose playlist or guide URL registered its login.
// That adds the spelling XtreamClient writes into a path (it re-encodes the credential, e.g. a
// space as %20) and registers nothing for a URL that merely has the shape
// (http://host/stream/channel/5.ts), whose "logins" would be ordinary words.
std::vector<std::wstring> streamLoginToRegister(const std::wstring& url,
                                                const std::vector<std::wstring>& known);

// `line` with credentials masked as "***":
//  - inside every URL it contains — scheme://… up to the next whitespace, quote or angle bracket,
//    or up to where a following scheme:// begins (two URLs run together are two URLs): the
//    non-empty user-info, the values of credential-named query parameters, and the two login
//    segments of an Xtream stream path — /USER/PASS/123[.ext] (exactly three segments, the first
//    not one of the kind words below), or /live|movie|series|timeshift/USER/PASS/…/123[.ext] (the
//    last segment digits, optionally with an extension; trailing sentence punctuation after it is
//    ignored). A non-Xtream URL of that shape loses two path segments in that line; that is the
//    accepted cost of never needing to know the login in advance;
//  - ANYWHERE in the line: every occurrence of each string in `secrets` that stands as a whole
//    token (see replaceWhole in UrlRedact.cpp: a neighbouring letter, digit or '_' blocks a match
//    at an edge where the secret itself is one), so a short login such as "live" is not masked
//    inside other words. Entries shorter than 4 characters are ignored. Not just inside URLs,
//    because a message may quote a stream's path without its scheme (/USER/PASS/1). Pass the
//    secrets longest first (addSecretsFromUrl keeps them so), so one that contains another is
//    masked whole.
std::wstring redactUrls(const std::wstring& line, const std::vector<std::wstring>& secrets);

}  // namespace rabbitears
