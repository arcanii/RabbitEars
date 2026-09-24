// SPDX-License-Identifier: GPL-3.0-or-later
// httpGet with a running byte count — Windows-only (implemented in platform/Http.cpp).
//
// Declared here rather than in the shared core/Http.h because the mac httpGet has no peer and
// nothing in common/ needs one: only the Win32 guide refresh reports download progress.
#pragma once

#include <functional>
#include <string>

namespace rabbitears {

// Called on the downloading thread after each read with the bytes received so far — the bytes
// WinHttpReadData returned, already decoded when the server used a Content-Encoding (httpGet asks
// WinHTTP to decode, best-effort), so the count can exceed a Content-Length. No total is offered.
using HttpProgressFn = std::function<void(unsigned long long bytesReceived)>;

// Exactly httpGet (core/Http.h), plus `onProgress` (may be empty).
bool httpGetWithProgress(const std::wstring& url, std::string& out, std::wstring& error,
                         int timeoutMs, const HttpProgressFn& onProgress);

}  // namespace rabbitears
