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

#include "core/Http.h"
#include "core/M3uParser.h"
#include "db/Database.h"
#include "platform/Encoding.h"
#include "ui/DockLayout.h"
#include "ui/Skin.h"

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

    out("\n== By country (tvg-id suffix) ==\n");
    {
        const std::string ccSample = "#EXTM3U\n"
                                     "#EXTINF:-1 tvg-id=\"CNN.us\",CNN\nhttp://s/cnn\n"
                                     "#EXTINF:-1 tvg-id=\"Fox.us\",Fox\nhttp://s/fox\n"
                                     "#EXTINF:-1 tvg-id=\"BBC.uk\",BBC\nhttp://s/bbc\n"
                                     "#EXTINF:-1 tvg-id=\"NoCountry.longid\",NoCC\nhttp://s/nocc\n";
        const M3uDocument ccDoc = parseM3u(ccSample);
        const long long pid2 = db.addPlaylist(L"CC", L"http://cc", true, 3000);
        db.bulkInsertChannels(pid2, ccDoc.channels, 3000);
        auto countries = db.listCountries();
        expect(hasGroup(countries, L"us") && hasGroup(countries, L"uk"),
               "listCountries derives us + uk from tvg-id");
        expect(!hasGroup(countries, L"longid"), "listCountries ignores non-2-letter suffixes");
        expect(db.channelsByCountry(L"us").size() == 2, "channelsByCountry('us') -> 2 (CNN, Fox)");
        expect(db.channelsByCountry(L"uk").size() == 1, "channelsByCountry('uk') -> 1 (BBC)");
        expect(db.channelsByCountry(L"de").empty(), "channelsByCountry('de') -> 0");
    }

    out("\n== Dock layout ==\n");
    {
        DockLayout def = DockLayout::makeDefault();
        const std::wstring s = def.serialize();
        expect(s == L"|0.220(N,-0.600(V,G))", "default layout serializes canonically");
        expect(DockLayout::parse(s).serialize() == s, "serialize -> parse round-trips");
        expect(DockLayout::parse(L"garbage(((").serialize() == s, "malformed layout -> default");
        expect(DockLayout::parse(L"|0.5(N,G)").serialize() == s, "layout missing a panel -> default");

        const RECT content{0, 0, 1000, 800};
        RECT r[kPanelCount];
        std::vector<DockLayout::Gutter> g;
        def.computeRects(content, 5, 60, r, g);
        expect(r[(int)Panel::Nav].left == 0 && r[(int)Panel::Nav].right < r[(int)Panel::Video].left,
               "default: nav is the left column");
        expect(r[(int)Panel::Video].left == r[(int)Panel::Grid].left &&
                   r[(int)Panel::Video].bottom <= r[(int)Panel::Grid].top,
               "default: video sits above grid in the right column");
        expect(g.size() == 2, "default has two draggable gutters");

        DockLayout d2 = DockLayout::makeDefault();
        d2.dock(Panel::Grid, DockSide::Left, Panel::Nav);
        RECT r2[kPanelCount];
        std::vector<DockLayout::Gutter> g2;
        d2.computeRects(content, 5, 60, r2, g2);
        expect(r2[(int)Panel::Grid].left == 0 &&
                   r2[(int)Panel::Grid].right <= r2[(int)Panel::Nav].left,
               "after dock Grid->left-of-Nav, grid is leftmost");
        bool allPresent = true;
        for (int k = 0; k < kPanelCount; ++k)
            allPresent &= (r2[k].right > r2[k].left && r2[k].bottom > r2[k].top);
        expect(allPresent, "re-dock keeps all three panels laid out");
    }

    out("\n== Skin model ==\n");
    {
        // Color codec: RRGGBB round-trip, inherit sentinel, alpha, bad-input fallback.
        expect(skinColorToString(SkinColor{200, 30, 20}) == "C81E14", "color -> RRGGBB hex");
        expect(skinColorFromString("C81E14", {}) == SkinColor{200, 30, 20}, "RRGGBB -> color round-trip");
        SkinColor inh{};
        inh.inherit = true;
        expect(skinColorToString(inh) == "inherit", "inherit color -> 'inherit'");
        expect(skinColorFromString("inherit", {}).inherit, "'inherit' -> inherit color");
        expect(skinColorFromString("nothex", SkinColor{1, 2, 3}) == SkinColor{1, 2, 3},
               "bad hex -> fallback");
        const SkinColor alpha{10, 20, 30, 128};
        expect(skinColorFromString(skinColorToString(alpha), {}) == alpha, "RRGGBBAA alpha round-trips");

        // Palette codec: full round-trip (first/mid/last roles), exact-arity + per-field fallback.
        const Skin& dark = skinById("dark");
        const SkinPalette& lp = skinById("light").palette;
        const std::string ps = skinPaletteToString(dark.palette);
        const SkinPalette rt = skinPaletteFromString(ps, lp);
        expect(rt.windowBg == dark.palette.windowBg && rt.accent == dark.palette.accent &&
                   rt.dangerHover == dark.palette.dangerHover,
               "palette round-trip preserves windowBg/accent/dangerHover (order intact)");
        expect(skinPaletteFromString("a,b,c", lp).accent == lp.accent,
               "wrong token count -> whole fallback");
        const SkinPalette pf = skinPaletteFromString("ZZZZZZ" + ps.substr(6), lp);
        expect(pf.windowBg == lp.windowBg && pf.accent == dark.palette.accent,
               "per-field fallback: bad field 0 falls back, good fields still parse");

        // Registry: lookup, unknown-id fallback, count.
        expect(skinById("dark").id == "dark" && skinById("light").id == "light",
               "skinById resolves dark + light");
        expect(skinById("cyberpunk").id == "cyberpunk" &&
                   skinById("cyberpunk").palette.accent == SkinColor{244, 55, 148},
               "cyberpunk skin registered with a neon-magenta accent");
        expect(skinById("steampunk").id == "steampunk" &&
                   skinById("steampunk").palette.accent == SkinColor{201, 148, 66} &&
                   skinById("steampunk").title.family == "Georgia",
               "steampunk skin registered (brass accent + serif title)");
        expect(skinById("bogus").id == "dark", "skinById unknown -> dark fallback");
        expect(builtinSkins().size() >= 4 && std::string(defaultSkinId()) == "dark",
               "four built-in skins; default is dark");
        expect(skinById("dark").glyph.symbol && !skinById("dark").body.symbol,
               "glyph is a symbol font; body is not");
        expect(std::string(skinSettingKey()) == "skin", "shared skin settings key");

        // GPU-effect manifest: per-skin glow + heat-haze strengths -> the HLSL shader.
        expect(SkinGpu{}.stripGlow == 1.0f && SkinGpu{}.edgeGlow == 0.9f && SkinGpu{}.heatHaze == 0.0f,
               "default SkinGpu reproduces the pre-manifest strengths (heat-haze off)");
        expect(skinById("dark").gpu.stripGlow == 1.0f && skinById("dark").gpu.edgeGlow == 0.9f &&
                   skinById("dark").gpu.heatHaze == 0.0f,
               "dark keeps the approved glow, heat-haze off");
        expect(skinById("cyberpunk").gpu.edgeGlow == 1.0f, "cyberpunk pushes the gutter neon to full");
        expect(skinById("steampunk").gpu.stripGlow < skinById("dark").gpu.stripGlow &&
                   skinById("steampunk").gpu.edgeGlow < skinById("dark").gpu.edgeGlow,
               "steampunk softens the glow vs dark (brass embers, not neon)");
        expect(skinById("steampunk").gpu.heatHaze > 0.0f, "steampunk enables the heat-haze shimmer");
        expect(skinById("dark").gpu.heatHaze == 0.0f && skinById("light").gpu.heatHaze == 0.0f &&
                   skinById("cyberpunk").gpu.heatHaze == 0.0f,
               "heat-haze is Steampunk-only (dark/light/cyberpunk = 0)");
        expect(skinById("light").gpu.stripGlow < 0.5f,
               "light dials the glow down (neon reads wrong on a light theme)");
        expect(skinGpuFromString(skinGpuToString(SkinGpu{0.5f, 0.25f, 0.75f}), {}).heatHaze == 0.75f,
               "gpu codec round-trips heatHaze (exact for 0.75)");
        expect(skinGpuFromString("1.0,0.9", SkinGpu{0.7f, 0.7f, 0.1f}).stripGlow == 0.7f,
               "gpu codec wrong arity (2 tokens) -> whole fallback");
        expect(skinGpuFromString("2.0,-1.0,3.0", {}).stripGlow == 1.0f &&
                   skinGpuFromString("2.0,-1.0,3.0", {}).edgeGlow == 0.0f &&
                   skinGpuFromString("2.0,-1.0,3.0", {}).heatHaze == 1.0f,
               "gpu codec clamps all three to 0..1");
        expect(skinGpuFromString("0.5,nan,0.5", SkinGpu{0.3f, 0.4f, 0.2f}).edgeGlow == 0.4f,
               "gpu codec rejects non-finite -> whole fallback");
    }

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

int fetch(const std::wstring& url) {
    line(L"Fetching " + url);
    std::string bytes;
    std::wstring err;
    if (!httpGet(url, bytes, err)) {
        line(L"Download failed: " + err);
        return 1;
    }
    line(L"Downloaded " + std::to_wstring(bytes.size()) + L" bytes");
    const M3uDocument doc = parseM3u(bytes);
    line(L"Parsed " + std::to_wstring(doc.channels.size()) + L" channels" +
         (doc.epgUrl.empty() ? L"" : L"; EPG: " + doc.epgUrl));
    return doc.channels.empty() ? 1 : 0;
}

// Import a URL or local file into the app's real DB (%LOCALAPPDATA%\RabbitEars).
int importSource(const std::wstring& source) {
    Database db;
    std::wstring err;
    if (!db.open(Database::defaultDbPath(), &err)) {
        line(L"DB open failed: " + err);
        return 1;
    }
    const bool isUrl = source.rfind(L"http", 0) == 0;
    M3uDocument doc;
    if (isUrl) {
        std::string bytes;
        if (!httpGet(source, bytes, err)) {
            line(L"Download failed: " + err);
            return 1;
        }
        doc = parseM3u(bytes);
    } else {
        doc = parseM3uFile(source, &err);
        if (!err.empty()) {
            line(L"Read failed: " + err);
            return 1;
        }
    }
    size_t slash = source.find_last_of(isUrl ? L"/" : L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? source : source.substr(slash + 1);
    const long long now = static_cast<long long>(time(nullptr));
    const long long pid = db.addPlaylist(name, source, isUrl, now);
    const int n = db.bulkInsertChannels(pid, doc.channels, now);
    line(L"Imported " + std::to_wstring(n) + L" channels into " + Database::defaultDbPath());
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && std::wstring(argv[1]) == L"--selftest") return selftest();
    if (argc >= 3 && std::wstring(argv[1]) == L"--fetch") return fetch(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"--import") return importSource(argv[2]);
    if (argc >= 2) {
        int limit = 20;
        for (int i = 2; i < argc - 1; ++i)
            if (std::wstring(argv[i]) == L"--limit") limit = _wtoi(argv[i + 1]);
        return dumpFile(argv[1], limit);
    }
    out("RabbitEarsCli — RabbitEars core test tool\n"
        "  RabbitEarsCli --selftest\n"
        "  RabbitEarsCli --fetch <url>\n"
        "  RabbitEarsCli --import <url|file>   (into the app's real DB)\n"
        "  RabbitEarsCli <file.m3u> [--limit N]\n");
    return 0;
}
