// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEarsCli — headless test/inspection tool for the RabbitEars core (M3U
// parser + SQLite store), the GUI-free way to prove the core end-to-end (mirrors
// the sibling apps' GvasCli). Usage:
//   RabbitEarsCli --selftest              run parser + DB round-trip assertions
//   RabbitEarsCli <file.m3u> [--limit N]  parse a playlist file, store it, dump it
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <windows.h>

#include "core/M3uParser.h"
#include "db/Database.h"
#include "platform/Encoding.h"

using namespace rabbitears;

namespace {

void out(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }
void outw(const std::wstring& w) { out(utf8FromWide(w)); }
void line(const std::wstring& w) { outw(w); out("\n"); }

int g_fail = 0;
void expect(bool cond, const std::string& what) {
    if (cond) {
        out("  [ok]  " + what + "\n");
    } else {
        out("  [FAIL] " + what + "\n");
        ++g_fail;
    }
}

long long findId(const std::vector<Channel>& v, const std::wstring& name) {
    for (const auto& c : v)
        if (c.name == name) return c.id;
    return 0;
}
bool hasGroup(const std::vector<std::wstring>& v, const std::wstring& g) {
    for (const auto& x : v)
        if (x == g) return true;
    return false;
}
bool hasChannelNamed(const std::vector<Channel>& v, const std::wstring& name) {
    for (const auto& c : v)
        if (c.name == name) return true;
    return false;
}

int selftest() {
    out("== M3U parser ==\n");
    const std::string sample =
        "\xEF\xBB\xBF"  // UTF-8 BOM (must be stripped)
        "#EXTM3U x-tvg-url=\"http://epg.example/guide.xml\"\r\n"
        "#EXTINF:-1 tvg-id=\"a.b\" tvg-logo=\"http://l/a.png\" group-title=\"News;Local\",Channel, One\r\n"
        "http://s/a.m3u8\r\n"
        "#EXTINF:-1 tvg-chno=\"12\" tvg-name=\"Bee\" group-title=\"Movies\",Bee TV\n"
        "#EXTVLCOPT:http-user-agent=UA/1.0\n"
        "#EXTVLCOPT:http-referrer=http://ref/\n"
        "http://s/b.m3u8\n"
        "\n"
        "#EXTINF:0,Gamma\n"
        "#EXTGRP:Sports\n"
        "http://s/c\n"
        "http://bare/d.m3u8\n";

    const M3uDocument doc = parseM3u(sample);
    expect(doc.epgUrl == L"http://epg.example/guide.xml", "EXTM3U x-tvg-url captured");
    expect(doc.channels.size() == 4, "4 channels parsed (got " + std::to_string(doc.channels.size()) + ")");
    if (doc.channels.size() == 4) {
        const auto& a = doc.channels[0];
        expect(a.name == L"Channel, One", "title with comma preserved (first unquoted comma splits)");
        expect(a.groupTitle == L"News;Local", "quoted group with semicolons preserved");
        expect(a.tvgId == L"a.b", "tvg-id parsed");
        expect(a.logoUrl == L"http://l/a.png", "tvg-logo parsed");
        expect(a.streamUrl == L"http://s/a.m3u8", "stream url captured");

        const auto& b = doc.channels[1];
        expect(b.name == L"Bee TV", "name parsed");
        expect(b.chno == 12, "tvg-chno parsed as int");
        expect(b.tvgName == L"Bee", "tvg-name parsed");
        expect(b.userAgent == L"UA/1.0", "#EXTVLCOPT http-user-agent captured");
        expect(b.referrer == L"http://ref/", "#EXTVLCOPT http-referrer captured");

        const auto& c = doc.channels[2];
        expect(c.name == L"Gamma", "name parsed (no attrs)");
        expect(c.groupTitle == L"Sports", "#EXTGRP applied to following entry");

        const auto& d = doc.channels[3];
        expect(d.name == L"d.m3u8", "bare-URL entry names itself from the URL");
        expect(d.streamUrl == L"http://bare/d.m3u8", "bare-URL stream captured");
    }

    out("== SQLite store ==\n");
    // Isolate to a temp dir; wipe any prior run.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"rabbitears_selftest";
    _wputenv_s(L"RABBITEARS_DATA_DIR", dir.c_str());
    const std::wstring dbPath = Database::defaultDbPath();
    DeleteFileW(dbPath.c_str());
    DeleteFileW((dbPath + L"-wal").c_str());
    DeleteFileW((dbPath + L"-shm").c_str());

    Database db;
    std::wstring err;
    expect(db.open(dbPath, &err), "database opens" + (err.empty() ? "" : " (" + utf8FromWide(err) + ")"));

    const long long pid = db.addPlaylist(L"Test", L"http://x", true, 1000);
    expect(pid > 0, "addPlaylist returns id");
    const int n = db.bulkInsertChannels(pid, doc.channels, 1000);
    expect(n == 4, "bulkInsertChannels inserted 4 (got " + std::to_string(n) + ")");

    auto chans = db.channelsByPlaylist(pid);
    expect(chans.size() == 4, "channelsByPlaylist returns 4");

    auto groups = db.listGroups();
    expect(hasGroup(groups, L"Movies") && hasGroup(groups, L"News;Local") && hasGroup(groups, L"Sports"),
           "listGroups has Movies/News;Local/Sports");

    auto byLcn = db.channelByLcn(12);
    expect(byLcn && byLcn->name == L"Bee TV", "channelByLcn(12) -> Bee TV");

    expect(hasChannelNamed(db.searchChannels(L"bee"), L"Bee TV"), "searchChannels('bee') finds Bee TV");

    const long long chanOneId = findId(chans, L"Channel, One");
    db.toggleFavourite(chanOneId);
    expect(db.favourites().size() == 1, "toggleFavourite -> 1 favourite");

    db.setChannelNumber(chanOneId, 5);
    auto byLcn5 = db.channelByLcn(5);
    expect(byLcn5 && byLcn5->name == L"Channel, One", "setChannelNumber(5) -> channelByLcn(5)");

    // Idempotent refresh: re-insert the same parse; count stays 4 and user data (fav/lcn) survives.
    const int n2 = db.bulkInsertChannels(pid, doc.channels, 2000);
    expect(n2 == 4, "re-insert reports 4 (upsert)");
    expect(db.channelsByPlaylist(pid).size() == 4, "still 4 channels after refresh (deduped)");
    expect(db.favourites().size() == 1, "favourite preserved across refresh");
    expect(db.channelByLcn(5) && db.channelByLcn(5)->name == L"Channel, One",
           "custom LCN preserved across refresh");

    db.setSetting(L"volume", L"80");
    auto vol = db.getSetting(L"volume");
    expect(vol && *vol == L"80", "settings get/set round-trip");

    out(g_fail == 0 ? "\nALL PASS\n" : "\n" + std::to_string(g_fail) + " FAILURE(S)\n");
    return g_fail == 0 ? 0 : 1;
}

int dumpFile(const std::wstring& path, int limit) {
    std::wstring err;
    const M3uDocument doc = parseM3uFile(path, &err);
    if (!err.empty()) {
        line(L"Error: " + err);
        return 1;
    }
    line(L"Parsed " + std::to_wstring(doc.channels.size()) + L" channels from " + path);
    if (!doc.epgUrl.empty()) line(L"EPG (x-tvg-url): " + doc.epgUrl);

    // Store into an isolated temp DB and read back, exercising the full path.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _wputenv_s(L"RABBITEARS_DATA_DIR", (std::wstring(tmp) + L"rabbitears_cli").c_str());
    const std::wstring dbPath = Database::defaultDbPath();
    DeleteFileW(dbPath.c_str());
    DeleteFileW((dbPath + L"-wal").c_str());
    DeleteFileW((dbPath + L"-shm").c_str());

    Database db;
    if (!db.open(dbPath, &err)) {
        line(L"DB open failed: " + err);
        return 1;
    }
    const long long pid = db.addPlaylist(path, path, false, static_cast<long long>(time(nullptr)));
    const int n = db.bulkInsertChannels(pid, doc.channels, static_cast<long long>(time(nullptr)));
    line(L"Stored " + std::to_wstring(n) + L" channels; groups: " +
         std::to_wstring(db.listGroups().size()));

    line(L"");
    line(L"  #     FAV  NAME                                     GROUP");
    line(L"  ----  ---  ---------------------------------------  --------------------");
    auto chans = db.channelsByPlaylist(pid);
    int shown = 0;
    for (const auto& c : chans) {
        if (shown++ >= limit) break;
        std::wstring num = c.lcn ? std::to_wstring(*c.lcn) : L"-";
        num.resize(4, L' ');
        std::wstring name = c.name.substr(0, 39);
        name.resize(39, L' ');
        line(L"  " + num + L"  " + (c.favourite ? L" * " : L"   ") + L"  " + name + L"  " +
             c.groupTitle.substr(0, 20));
    }
    if (static_cast<int>(chans.size()) > limit)
        line(L"  ... and " + std::to_wstring(chans.size() - limit) + L" more");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && std::wstring(argv[1]) == L"--selftest") return selftest();
    if (argc >= 2) {
        int limit = 20;
        for (int i = 2; i < argc - 1; ++i)
            if (std::wstring(argv[i]) == L"--limit") limit = _wtoi(argv[i + 1]);
        return dumpFile(argv[1], limit);
    }
    out("RabbitEarsCli — RabbitEars core test tool\n"
        "  RabbitEarsCli --selftest\n"
        "  RabbitEarsCli <file.m3u> [--limit N]\n");
    return 0;
}
