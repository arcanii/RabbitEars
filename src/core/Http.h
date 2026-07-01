// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal synchronous HTTP(S) GET via WinHTTP, for downloading M3U playlists.
// Call from a worker thread (it blocks). Follows redirects, auto-decompresses.
#pragma once

#include <string>

namespace rabbitears {

// Download `url` (http/https) into `out` (raw bytes). Returns false and sets
// `error` on failure. `timeoutMs` (per phase) is applied when > 0.
bool httpGet(const std::wstring& url, std::string& out, std::wstring& error, int timeoutMs = 0);

}  // namespace rabbitears
