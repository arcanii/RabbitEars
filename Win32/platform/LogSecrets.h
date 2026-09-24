// SPDX-License-Identifier: GPL-3.0-or-later
// Tell the Win32 log sink which provider logins to mask (implemented in platform/Log.cpp, so
// GUI-only — the CLI does not link the sink).
//
// Every line diag::write() records is passed through redactUrls() (platform/UrlRedact.h). On its
// own that masks the credentials it can recognise by shape INSIDE a scheme:// URL (user-info,
// credential-named query values, Xtream stream paths). What it cannot catch without help is the
// same login anywhere else — a message quoting a stream path without its scheme, say. So the app
// also registers the credentials the URLs it handles carry (urlCredentials()): each playlist's
// URLs at startup, a URL playlist when its download starts, a new playlist's guide URL when it is
// added, a guide URL on Set Guide URL, and each stream URL when it is played or recorded (its
// path login only when part of it is already registered — streamLoginToRegister) — and the sink
// masks those wherever they appear in a line. A login none of those has registered is not masked
// outside a scheme:// URL.
#pragma once

#include <string>

namespace rabbitears::diag {

// Register the credentials `url` carries (see urlCredentials()). Thread-safe; duplicates and
// values under 4 characters are ignored. Takes effect for every line written afterwards.
void addSecretsFromUrl(const std::wstring& url);

// Mask the previous session's log (rabbitears.log.1) in place with the same rules, rewriting it
// (via a temporary sibling, swapped in) only when something changed. For the one written before
// masking existed — upgrading keeps it beside the new log, where "send us the logs" would include
// it. Call after the startup registrations. One pass over the file per registered secret; a file
// over 64 MB, or one that cannot be read or rewritten, is left as it is, with a warning in the
// new log.
void scrubPreviousLog();

}  // namespace rabbitears::diag
