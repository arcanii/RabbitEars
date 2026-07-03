// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows implementation of Database::defaultDbPath() — %LOCALAPPDATA%\RabbitEars
// (RABBITEARS_DATA_DIR override). Extracted from Database.cpp so RabbitEarsCore
// links only sqlite3 (the shell32/ole32 dependency lives here now). The macOS
// peer is src/platform/mac/Paths.cpp.
#include "db/Database.h"

#include <cstdlib>
#include <filesystem>

#include <shlobj.h>
#include <windows.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace rabbitears {

std::wstring Database::defaultDbPath() {
    // Test/CI override, mirroring SQLTerminal's SQLT_DATA_DIR.
    if (const wchar_t* env = _wgetenv(L"RABBITEARS_DATA_DIR")) {
        std::filesystem::path dir(env);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return (dir / L"rabbitears.db").wstring();
    }
    PWSTR local = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        dir = std::filesystem::path(local) / L"RabbitEars";
    }
    if (local) CoTaskMemFree(local);
    if (dir.empty()) dir = std::filesystem::temp_directory_path() / L"RabbitEars";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return (dir / L"rabbitears.db").wstring();
}

}  // namespace rabbitears
