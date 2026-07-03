// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/Log.h"

#include <cstdio>
#include <filesystem>
#include <mutex>

#include <windows.h>

#include <shlobj.h>

namespace rabbitears::diag {
namespace {

std::mutex g_mtx;
FILE*      g_fp = nullptr;

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
    fwprintf(g_fp, L"%s [%-8s] (t%lu) %s\n", ts, level, tid, msg.c_str());
    fflush(g_fp);  // per-line flush so a crash still leaves the tail on disk
}

}  // namespace rabbitears::diag
