// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal synchronous HTTP(S) GET via WinHTTP, for downloading M3U playlists.
// Call from a worker thread (it blocks). Follows redirects, auto-decompresses.
#pragma once

#include <string>

#include "core/DeadLinkCheck.h"  // ProbeTransport

namespace rabbitears {

// Download `url` (http/https) into `out` (raw bytes). Returns false and sets
// `error` on failure. `timeoutMs` (per phase) is applied when > 0.
bool httpGet(const std::wstring& url, std::string& out, std::wstring& error, int timeoutMs = 0);

// Ask whether `url` is still there, WITHOUT downloading it. Returns the transport outcome and,
// when that is Ok, the HTTP status in `httpStatus`.
//
// This exists because httpGet() is unusable for the job: a live stream's body never ends, so
// "just GET it" would download forever. httpProbe reads the response HEADERS and closes.
//
// Redirects are NOT followed — a 3xx is itself a healthy answer (it is how CDNs hand off to an
// edge), and following one would turn a cheap probe into a chain of requests.
// Call from a worker thread; it blocks.
ProbeTransport httpProbe(const std::wstring& url, int& httpStatus, int timeoutMs = 8000);

}  // namespace rabbitears
