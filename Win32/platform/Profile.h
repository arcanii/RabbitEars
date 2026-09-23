// SPDX-License-Identifier: GPL-3.0-or-later
//
// Profile — a RabbitEars instance whose data lives somewhere other than %LOCALAPPDATA%\RabbitEars.
//
// RABBITEARS_DATA_DIR (read by Database::defaultDbPath and by the log) moves the database, the
// settings and the log. When it is set, the instance is a separate PROFILE, and this header is what
// lets one run BESIDE the normal install — a dev build checked against the installed release on
// the same machine, for instance:
//   * it takes its own single-instance mutex (profileMutexName), so the two do not bounce off each
//     other — while a second launch of the SAME profile still does;
//   * it never touches the Windows wake-to-record task ("RabbitEars Recording Wake"). That task is
//     one per user, not per profile: a profile syncing it would re-point the installed app's task
//     at itself, or delete it outright whenever its own queue happened to be empty;
//   * it never runs WinSparkle, whose state is per USER in the registry (Updater.cpp);
//   * it carries a title-bar tag (appTitle), so the two windows can be told apart.
// Still shared, whatever the profile: the channel-logo cache (%LOCALAPPDATA%\RabbitEars\logos), the
// recordings folder — and the provider's connection limit, which is why two instances cannot both
// play a max_connections:1 line.
//
// Header-only and Win32-only. The CLI sets the same variable for its selftests and benchmarks; it
// never includes this.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <string>

#include <windows.h>
#include <shlobj.h>  // SHGetKnownFolderPath — the default data dir (Paths.cpp links shell32)

namespace rabbitears {

// A path in the form two spellings of one directory share: canonical where possible, no trailing
// separator, case-folded (NTFS paths are case-insensitive).
inline std::wstring profileKeyOf(const std::wstring& dir) {
    std::error_code ec;
    std::wstring p = std::filesystem::weakly_canonical(dir, ec).wstring();
    if (ec || p.empty()) p = dir;
    while (p.size() > 1 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    for (wchar_t& c : p) c = static_cast<wchar_t>(std::towlower(c));
    return p;
}

// Whether this instance is a side profile, and its data dir. Read once: the environment cannot change
// under a running app. RABBITEARS_DATA_DIR set to anything moves the database (Database::
// defaultDbPath) — EXCEPT that a value naming the default data dir itself (%LOCALAPPDATA%\RabbitEars,
// however spelled) is the normal install, not a profile: it shares the real database, so it must
// also share the normal mutex and the wake task, or two schedulers would run on one queue.
struct ProfileEnv {
    bool         set = false;
    std::wstring dir;
};
inline const ProfileEnv& profileEnv() {
    static const ProfileEnv p = [] {
        ProfileEnv e;
        const wchar_t* env = _wgetenv(L"RABBITEARS_DATA_DIR");
        if (!env) return e;
        PWSTR local = nullptr;
        std::wstring def;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            def = (std::filesystem::path(local) / L"RabbitEars").wstring();
        if (local) CoTaskMemFree(local);
        if (!def.empty() && profileKeyOf(env) == profileKeyOf(def)) return e;
        e.set = true;
        e.dir = env;
        return e;
    }();
    return p;
}
inline bool isSideProfile() { return profileEnv().set; }

// The single-instance mutex. The normal install keeps the historical name, which the installer's
// AppMutex also names, so an auto-update can still find the running app. A profile appends a hash
// of its data dir: the same dir gives the same mutex (a profile is still single-instance), any other
// dir a different one. FNV-1a over the case-folded canonical path — NTFS paths are case-insensitive,
// and a hand-rolled hash cannot differ between two builds the way std::hash is allowed to.
inline std::wstring profileMutexName() {
    if (!isSideProfile()) return L"RabbitEars.SingleInstance";
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : profileKeyOf(profileEnv().dir)) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    wchar_t buf[64];
    swprintf_s(buf, L"RabbitEars.SingleInstance.%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// The name shown in the title bar and the taskbar: "RabbitEars" for the normal install, and
// "RabbitEars · dev" for a profile at ...\dev (the data dir's last component). Built once — the
// chrome paints it on every repaint.
inline const std::wstring& appTitle() {
    static const std::wstring title = [] {
        std::wstring t = L"RabbitEars";
        if (!isSideProfile()) return t;
        const std::filesystem::path dir(profileEnv().dir);
        std::wstring leaf = dir.filename().wstring();
        if (leaf.empty()) leaf = dir.parent_path().filename().wstring();  // "...\dev\" -> "dev"
        t += L" · ";
        t += leaf.empty() ? std::wstring(L"profile") : leaf;
        return t;
    }();
    return title;
}

}  // namespace rabbitears
