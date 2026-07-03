// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Http.h"

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace rabbitears {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

}  // namespace

bool httpGet(const std::wstring& url, std::string& out, std::wstring& error, int timeoutMs) {
    out.clear();

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"", path[4096] = L"", extra[4096] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    // WinHttpCrackUrl splits the "?query#frag" into ExtraInfo — give it a buffer or it
    // is silently dropped. Xtream-style playlists carry the credentials there
    // (?username=…&password=…&type=m3u_plus), so losing it means an un-authenticated
    // request and an empty/denied response.
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    uc.dwSchemeLength = 1;  // request scheme parsing
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = L"Invalid URL.";
        return false;
    }
    // WinHttpOpenRequest wants the path + query together as the object name.
    const std::wstring object = std::wstring(path) + extra;
    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    Handle session;
    // Present a VLC-style User-Agent: many IPTV / Xtream-Codes panels gate playlist
    // delivery on a recognized player UA and reject unknown ones. (We play via libVLC
    // anyway.)
    session.h = WinHttpOpen(L"VLC/3.0.23 LibVLC/3.0.23", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = L"WinHttpOpen failed.";
        return false;
    }
    if (timeoutMs > 0)
        WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    // Best-effort gzip/deflate; ignore failure on older systems.
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

    Handle connect;
    connect.h = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) {
        error = L"WinHttpConnect failed.";
        return false;
    }

    Handle request;
    request.h = WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        error = L"WinHttpOpenRequest failed.";
        return false;
    }
    // Follow redirects (default policy already does; set explicitly for clarity).
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = L"Network request failed.";
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status >= 400) {
        error = L"Server returned HTTP " + std::to_wstring(status) + L".";
        return false;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            error = L"Read error.";
            return false;
        }
        if (avail == 0) break;
        const size_t base = out.size();
        out.resize(base + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, out.data() + base, avail, &read)) {
            error = L"Read error.";
            return false;
        }
        out.resize(base + read);
        if (read == 0) break;
    }
    return true;
}

}  // namespace rabbitears
