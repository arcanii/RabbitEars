// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS implementation of Database::defaultDbPath() —
// ~/Library/Application Support/RabbitEars (RABBITEARS_DATA_DIR override). Pure
// C++ (no Foundation), so it lives in the shared core lib and the self-test
// links it directly. The Windows peer is Win32/platform/Paths.cpp.
#include "db/Database.h"

#include <cstdlib>
#include <filesystem>

namespace rabbitears {

std::wstring Database::defaultDbPath() {
    std::filesystem::path dir;
    if (const char* env = std::getenv("RABBITEARS_DATA_DIR")) {
        dir = std::filesystem::path(env);
    } else if (const char* home = std::getenv("HOME")) {
        dir = std::filesystem::path(home) / "Library" / "Application Support" / "RabbitEars";
    } else {
        dir = std::filesystem::temp_directory_path() / "RabbitEars";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / "rabbitears.db").wstring();  // path::string() is UTF-8 on macOS
}

}  // namespace rabbitears
