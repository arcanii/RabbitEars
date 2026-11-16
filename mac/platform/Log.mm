// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS implementation of platform/Log.h + platform/LogSecrets.h (the Windows peer is Log.cpp).
// Writes to ~/Library/Application Support/RabbitEars/rabbitears.log
// (RABBITEARS_DATA_DIR override; same folder as the database), mirrors to
// os_log, rotates the previous run to rabbitears.log.1, and flushes every line.
//
// Every line is passed through redactUrls() (common/platform/UrlRedact.h) BEFORE it reaches
// either the file or os_log: an Xtream provider puts the account's login in its playlist, guide
// and stream URLs, the log records those URLs, and the log exists to be sent to someone. os_log
// is a second copy (`log show` reads it, and %{public} keeps it unredacted there), so it is
// masked too — the same line goes to both.
#import <Foundation/Foundation.h>
#include <os/log.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "core/GuideSources.h"
#include "platform/Encoding.h"  // non-Windows branch of the shared header
#include "platform/Log.h"
#include "platform/LogSecrets.h"
#include "platform/UrlRedact.h"

namespace rabbitears::diag {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::filesystem::path g_path;
std::vector<std::wstring> g_secrets;  // provider logins, masked anywhere in a line (guarded by g_mutex)

std::filesystem::path dataDir() {
    if (const char* env = std::getenv("RABBITEARS_DATA_DIR")) return std::filesystem::path(env);
    if (const char* home = std::getenv("HOME"))
        return std::filesystem::path(home) / "Library" / "Application Support" / "RabbitEars";
    return std::filesystem::temp_directory_path() / "RabbitEars";
}

}  // namespace

void init(const wchar_t* appVersion) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    const std::filesystem::path dir = dataDir();
    std::filesystem::create_directories(dir, ec);
    g_path = dir / "rabbitears.log";
    std::filesystem::rename(g_path, dir / "rabbitears.log.1", ec);  // rotate (ignore if absent)
    g_file.open(g_path, std::ios::out | std::ios::trunc);
    if (g_file) {
        g_file << "==== RabbitEars (macOS) " << utf8FromWide(appVersion ? appVersion : L"?")
               << " ====" << std::endl;
    }
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) g_file.close();
}

void write(const wchar_t* level, const std::wstring& msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Every line, whoever wrote it: a URL reaches the log from playback, playlist and guide code,
    // and each can carry the provider login (see platform/LogSecrets.h). Masked under the lock,
    // which also guards g_secrets.
    const std::string line =
        utf8FromWide(level ? level : L"INFO") + ": " + utf8FromWide(redactUrls(msg, g_secrets));
    os_log(OS_LOG_DEFAULT, "%{public}s", line.c_str());
    if (g_file) g_file << line << std::endl;  // std::endl flushes: a crash still leaves a usable log
}

std::wstring filePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return wideFromUtf8(g_path.string());  // path::string() is UTF-8 on macOS
}

void addSecretsFromUrl(const std::wstring& url) {
    // A guide link naming several guides (core/GuideSources.h): each guide's own login. (A piece of
    // a split link never splits again, so this recurses once.)
    if (const std::vector<std::wstring> urls = splitGuideUrls(url); urls.size() > 1) {
        for (const auto& u : urls) addSecretsFromUrl(u);
        return;
    }
    std::vector<std::wstring> found = urlCredentials(url);
    std::lock_guard<std::mutex> lock(g_mutex);
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
        std::lock_guard<std::mutex> lock(g_mutex);
        secrets = g_secrets;
    }
    const std::filesystem::path prev = dataDir() / "rabbitears.log.1";
    std::error_code ec;
    const auto size = std::filesystem::file_size(prev, ec);
    if (ec) return;  // no previous session
    // Runs on the main thread during startup; the masking is linear, but a log this big is not
    // worth the pause. Say so rather than skip it silently — the file still holds what it held.
    constexpr std::uintmax_t kMaxScrubBytes = 64ull * 1024 * 1024;
    auto notMasked = [](const std::wstring& why) {
        write(L"WARN", L"previous session's log (rabbitears.log.1) not masked: " + why);
    };
    if (size > kMaxScrubBytes)
        return notMasked(std::to_wstring(size) + L" bytes is too large to mask at startup");
    std::string bytes;
    {
        std::ifstream in(prev, std::ios::binary);
        if (!in) return notMasked(L"could not open it");
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (bytes.size() != size) return notMasked(L"short read");  // never write a short read back
    const std::wstring text = wideFromUtf8(bytes);
    const std::wstring safe = redactUrls(text, secrets);
    if (safe == text) return;
    const std::string outBytes = utf8FromWide(safe);
    // Write a sibling, then swap it in (rename(2) replaces atomically): a failed or short write
    // (a full disk) leaves the original in place rather than a truncated log.
    std::filesystem::path tmp = prev;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(outBytes.data(), static_cast<std::streamsize>(outBytes.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return notMasked(L"could not write the masked copy");
        }
    }
    std::filesystem::rename(tmp, prev, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return notMasked(L"could not replace it");
    }
    write(L"INFO", L"masked provider logins in the previous session's log (rabbitears.log.1)");
}

}  // namespace rabbitears::diag
