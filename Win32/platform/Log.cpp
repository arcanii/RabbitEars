// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

#include <windows.h>

#include <shlobj.h>

#include "platform/Encoding.h"
#include "platform/LogSecrets.h"
#include "platform/UrlRedact.h"

namespace rabbitears::diag {
namespace {

std::mutex g_mtx;
FILE*      g_fp = nullptr;
std::vector<std::wstring> g_secrets;  // provider logins, masked anywhere in a line (guarded by g_mtx)

// Same resolution as Database::defaultDbPath's directory (kept independent so the
// logger has no dependency on the DB layer).
std::filesystem::path dataDir() {
    if (const wchar_t* env = _wgetenv(L"RABBITEARS_DATA_DIR")) return std::filesystem::path(env);
    PWSTR local = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        dir = std::filesystem::path(local) / L"RabbitEars";
    if (local) CoTaskMemFree(local);
    if (dir.empty()) dir = std::filesystem::temp_directory_path() / L"RabbitEars";
    return dir;
}

std::wstring osVersionLine() {
    OSVERSIONINFOW osv{};
    osv.dwOSVersionInfoSize = sizeof(osv);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")))
            fn(&osv);  // RTL_OSVERSIONINFOW is layout-compatible with OSVERSIONINFOW
    wchar_t buf[64];
    swprintf_s(buf, L"Windows %lu.%lu build %lu", osv.dwMajorVersion, osv.dwMinorVersion,
               osv.dwBuildNumber);
    return buf;
}

}  // namespace

std::wstring filePath() { return (dataDir() / L"rabbitears.log").wstring(); }

void init(const wchar_t* appVersion) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_fp) return;
    std::error_code ec;
    const auto dir = dataDir();
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / L"rabbitears.log";
    const auto prev = dir / L"rabbitears.log.1";
    std::filesystem::remove(prev, ec);
    std::filesystem::rename(path, prev, ec);  // keep the previous run for comparison
    g_fp = _wfopen(path.c_str(), L"w, ccs=UTF-8");
    if (!g_fp) return;

    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(g_fp, L"================ RabbitEars session start ================\n");
    fwprintf(g_fp, L"when : %04d-%02d-%02d %02d:%02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond);
    fwprintf(g_fp, L"app  : %s\n", appVersion ? appVersion : L"?");
    fwprintf(g_fp, L"os   : %s\n", osVersionLine().c_str());
    fwprintf(g_fp, L"exe  : %s\n", exe);
    fwprintf(g_fp, L"log  : %s\n", path.c_str());
    fwprintf(g_fp, L"----------------------------------------------------------\n");
    fflush(g_fp);
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_fp) {
        fwprintf(g_fp, L"================ RabbitEars session end ==================\n");
        fflush(g_fp);
        fclose(g_fp);
        g_fp = nullptr;
    }
}

void write(const wchar_t* level, const std::wstring& msg) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[16];
    swprintf_s(ts, L"%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const unsigned long tid = GetCurrentThreadId();
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_fp) return;
    // Every line, whoever wrote it: a URL can reach the log from playback, playlist, guide and
    // libVLC's own messages, and each can carry the provider login (see platform/LogSecrets.h).
    const std::wstring safe = redactUrls(msg, g_secrets);
    fwprintf(g_fp, L"%s [%-8s] (t%lu) %s\n", ts, level, tid, safe.c_str());
    fflush(g_fp);  // per-line flush so a crash still leaves the tail on disk
}

void addSecretsFromUrl(const std::wstring& url) {
    std::vector<std::wstring> found = urlCredentials(url);
    std::lock_guard<std::mutex> lk(g_mtx);
    // A stream path's login only when part of it is already known — the same account (see
    // streamLoginToRegister); `found` counts as known, for a URL that carries both.
    std::vector<std::wstring> known = g_secrets;
    known.insert(known.end(), found.begin(), found.end());
    const std::vector<std::wstring> path = streamLoginToRegister(url, known);
    found.insert(found.end(), path.begin(), path.end());
    if (found.empty()) return;
    for (const auto& s : found)
        if (std::find(g_secrets.begin(), g_secrets.end(), s) == g_secrets.end())
            g_secrets.push_back(s);
    // Longest first, so a secret that contains another is masked whole rather than piecewise.
    std::sort(g_secrets.begin(), g_secrets.end(),
              [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
}

void scrubPreviousLog() {
    std::vector<std::wstring> secrets;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        secrets = g_secrets;
    }
    const auto prev = dataDir() / L"rabbitears.log.1";
    std::error_code ec;
    const auto size = std::filesystem::file_size(prev, ec);
    if (ec) return;  // no previous session
    // Runs on the UI thread during startup; the masking is linear, but a log this big is not
    // worth the pause (at Trace, the flow lines alone are ~2 MB an hour per playing pane). Say
    // so rather than skip it silently — the file still holds whatever it held.
    constexpr std::uintmax_t kMaxScrubBytes = 64ull * 1024 * 1024;
    if (size > kMaxScrubBytes) {
        write(L"WARN", L"previous session's log (rabbitears.log.1) not masked: " +
                           std::to_wstring(size) + L" bytes is too large to mask at startup");
        return;
    }
    // Every way this can give up says so in the new log: an old log left unmasked must not look
    // like one that had nothing to mask.
    auto notMasked = [](const std::wstring& why) {
        write(L"WARN", L"previous session's log (rabbitears.log.1) not masked: " + why);
    };
    std::string bytes;
    {
        std::ifstream in(prev, std::ios::binary);
        if (!in) return notMasked(L"could not open it");
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // A short read must not be written back as the whole log.
    if (bytes.size() != size) return notMasked(L"short read");
    // init() opens the log with ccs=UTF-8, which writes a BOM; keep it if it is there.
    const bool bom = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                     static_cast<unsigned char>(bytes[1]) == 0xBB &&
                     static_cast<unsigned char>(bytes[2]) == 0xBF;
    const std::wstring text = wideFromUtf8(bom ? bytes.substr(3) : bytes);
    const std::wstring safe = redactUrls(text, secrets);
    if (safe == text) return;
    const std::string outBytes = utf8FromWide(safe);
    // Write a sibling, then swap it in: a failed or short write (a full disk) leaves the original
    // in place rather than a truncated log.
    auto tmp = prev;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (bom) out.write("\xEF\xBB\xBF", 3);
        out.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return notMasked(L"could not write the masked copy");
        }
    }
    if (!MoveFileExW(tmp.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = GetLastError();
        std::filesystem::remove(tmp, ec);
        return notMasked(L"could not replace it (error " + std::to_wstring(err) + L")");
    }
    write(L"INFO", L"masked provider logins in the previous session's log (rabbitears.log.1)");
}

}  // namespace rabbitears::diag
