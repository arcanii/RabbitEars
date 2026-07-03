// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS implementation of platform/Log.h (the Windows peer is Log.cpp).
// Writes to ~/Library/Application Support/RabbitEars/rabbitears.log
// (RABBITEARS_DATA_DIR override; same folder as the database), mirrors to
// os_log, rotates the previous run to rabbitears.log.1, and flushes every line.
#import <Foundation/Foundation.h>
#include <os/log.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "platform/Encoding.h"  // resolves to the mac shim on this build
#include "platform/Log.h"

namespace rabbitears::diag {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::filesystem::path g_path;

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
    const std::string line = utf8FromWide(level ? level : L"INFO") + ": " + utf8FromWide(msg);
    os_log(OS_LOG_DEFAULT, "%{public}s", line.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) g_file << line << std::endl;  // std::endl flushes: a crash still leaves a usable log
}

std::wstring filePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return wideFromUtf8(g_path.string());  // path::string() is UTF-8 on macOS
}

}  // namespace rabbitears::diag
