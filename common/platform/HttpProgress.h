// SPDX-License-Identifier: GPL-3.0-or-later
// httpGet with a running byte count — a seam each platform's Http implements (Win32/platform/
// Http.cpp, mac/platform/Http.mm), used by both guide refreshes to report download progress.
// Kept out of core/Http.h because nothing in common/ itself calls it.
#pragma once

#include <functional>
#include <string>

namespace rabbitears {

// Called after each read with the bytes received so far — the bytes WinHttpReadData returned
// (Win32, on the downloading thread) or NSURLSession delivered (mac, on the session's delegate
// queue while the caller blocks — so calls never overlap), already decoded when the server used a
// Content-Encoding, so the count can exceed a Content-Length. No total is offered.
using HttpProgressFn = std::function<void(unsigned long long bytesReceived)>;

// Exactly httpGet (core/Http.h), plus `onProgress` (may be empty).
bool httpGetWithProgress(const std::wstring& url, std::string& out, std::wstring& error,
                         int timeoutMs, const HttpProgressFn& onProgress);

}  // namespace rabbitears
