// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS core self-test — the GUI-free proof that the SHARED core (M3U parser +
// SQLite store + dock-layout logic) is genuinely portable, built and run on
// clang. It reuses the exact core code the Windows app uses; only the harness
// (temp dir / env / file delete) is portable POSIX/std instead of the Win32
// calls in src/cli/RabbitEarsCli.cpp (which is Windows-only: wmain, GetTempPathW,
// DeleteFileW, SetConsoleOutputCP). Assertions mirror RabbitEarsCli --selftest.
#include <cstdio>
#include <cstdlib>  // setenv
#include <filesystem>
#include <string>
#include <vector>

#include "core/M3uParser.h"
#include "core/Strings.h"  // shared i18n catalog — completeness + placeholder-parity gate below
#include "db/Database.h"
#include "platform/Encoding.h"  // non-Windows branch of the shared header
#include "ui/DockLayout.h"

#include "MeterModel.h"  // mac-local meter model (rabbitears::mac) — codecs tested below

using namespace rabbitears;

namespace {

void out(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }

int g_fail = 0;
void expect(bool cond, const std::string& what) {
    out(cond ? "  [ok]  " + what + "\n" : "  [FAIL] " + what + "\n");
    if (!cond) ++g_fail;
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

}  // namespace

int main() {
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
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "rabbitears_selftest";
    setenv("RABBITEARS_DATA_DIR", dir.string().c_str(), 1);
    const std::wstring dbPath = Database::defaultDbPath();
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + L"-wal", ec);
    std::filesystem::remove(dbPath + L"-shm", ec);

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

        // --- Xtream fallback: country from the group-title prefix when the tvg-id has none ---
        const std::string xSample =
            "#EXTM3U\n"
            "#EXTINF:-1 group-title=\"US| NEWS\",XNews\nhttp://x/us1\n"
            "#EXTINF:-1 group-title=\"US | ENTERTAINMENT\",XEnt\nhttp://x/us2\n"
            "#EXTINF:-1 group-title=\"[UK] SPORTS\",XSport\nhttp://x/uk1\n"
            "#EXTINF:-1 group-title=\"FR - CINEMA\",XCine\nhttp://x/fr1\n"
            "#EXTINF:-1 group-title=\"es:CINE\",XEs\nhttp://x/es1\n"
            "#EXTINF:-1 group-title=\"|IT| CALCIO\",XIt\nhttp://x/it1\n"
            "#EXTINF:-1 group-title=\"HD| MOVIES\",XHd\nhttp://x/hd1\n"
            "#EXTINF:-1 group-title=\"TV | SHOWS\",XTv\nhttp://x/tv1\n"
            "#EXTINF:-1 group-title=\"4K| SPORT\",X4k\nhttp://x/4k1\n"
            "#EXTINF:-1 group-title=\"USA| NEWS\",XUsa\nhttp://x/usa1\n"
            "#EXTINF:-1 group-title=\"DOCUMENTARIES\",XDoc\nhttp://x/doc1\n"
            "#EXTINF:-1 group-title=\"(PL) NEWS\",XPl\nhttp://x/pl1\n"
            "#EXTINF:-1 group-title=\"SD| CINE\",XSd\nhttp://x/sd1\n"
            "#EXTINF:-1 group-title=\"EN| SERIES\",XEn\nhttp://x/en1\n"
            "#EXTINF:-1 group-title=\"XX| ADULT\",XXx\nhttp://x/xx1\n"
            "#EXTINF:-1 group-title=\"EX-YU| SPORT\",XExyu\nhttp://x/ex1\n"
            "#EXTINF:-1 group-title=\"ON - DEMAND\",XOn\nhttp://x/on1\n"
            "#EXTINF:-1 group-title=\"DE\",XBare\nhttp://x/de1\n"
            "#EXTINF:-1 tvg-id=\"12345\" group-title=\"PT| CANAIS\",XOpq\nhttp://x/pt1\n"
            "#EXTINF:-1 tvg-id=\"BBC.uk\" group-title=\"US| MIX\",XPrec\nhttp://x/prec1\n";
        const long long pid3 = db.addPlaylist(L"Xtream", L"http://xt", true, 4000);
        db.bulkInsertChannels(pid3, parseM3u(xSample).channels, 4000);
        countries = db.listCountries();
        expect(hasGroup(countries, L"fr") && hasGroup(countries, L"es") && hasGroup(countries, L"it"),
               "listCountries derives fr/es/it from Xtream group-title prefixes");
        expect(!hasGroup(countries, L"hd") && !hasGroup(countries, L"tv"),
               "deny-listed tech prefixes (HD/TV) are not countries");
        expect(!hasGroup(countries, L"do") && hasGroup(countries, L"us"),
               "no delimiter -> no country (DOCUMENTARIES); us still present");
        expect(db.channelsByCountry(L"us").size() == 4,
               "channelsByCountry('us') -> CNN+Fox + 2 Xtream ('USA|' rejected; tvg-id wins over 'US| MIX')");
        expect(db.channelsByCountry(L"uk").size() == 3,
               "channelsByCountry('uk') -> BBC + [UK] SPORTS + BBC.uk-in-US-group (tvg-id authoritative)");
        expect(db.channelsByCountry(L"fr").size() == 1 && db.channelsByCountry(L"es").size() == 1,
               "group-title-only channels are filterable by country");
        expect(db.channelsByCountry(L"US").size() == 4, "country query is case-insensitive");
        expect(hasGroup(countries, L"pl"), "parenthesised prefix '(PL)' derives pl");
        expect(!hasGroup(countries, L"sd") && !hasGroup(countries, L"en") &&
                   !hasGroup(countries, L"xx") && !hasGroup(countries, L"ex") &&
                   !hasGroup(countries, L"on"),
               "deny-listed SD/EN/XX/EX/ON prefixes are not countries");
        expect(!hasGroup(countries, L"de") && db.channelsByCountry(L"de").empty(),
               "a bare 2-letter group name ('DE') claims no country");
        expect(db.channelsByCountry(L"pt").size() == 1,
               "opaque numeric tvg-id + country group (the canonical Xtream shape) -> pt");
        db.deletePlaylist(pid3);  // restore the shared fixture state for the sections below
        expect(db.channelsByCountry(L"fr").empty(), "Xtream playlist removed (cascade) -> fr gone");
    }

    out("\n== Playlist enable/disable ==\n");
    {
        // State here: pid (Test, 4 channels incl. the lcn-5 favourite + lcn-12 Bee TV,
        // groups Movies/News;Local/Sports) and pid2 (CC, 4 channels, no groups) => 8 total.
        auto pls = db.listPlaylists();
        expect(pls.size() == 2, "listPlaylists -> 2");
        bool allEnabled = true;
        for (const auto& p : pls) allEnabled = allEnabled && p.enabled;
        expect(allEnabled, "new playlists default to enabled");
        expect(db.allChannels().size() == 8, "allChannels -> 8 before disable");

        db.setPlaylistEnabled(pid, false);  // hide the Test playlist
        bool disabled = false;
        for (const auto& p : db.listPlaylists()) if (p.id == pid) disabled = !p.enabled;
        expect(disabled, "setPlaylistEnabled(false) persists");
        expect(db.channelsByPlaylist(pid).size() == 4, "channelsByPlaylist ignores the disabled flag");
        expect(db.allChannels().size() == 4, "allChannels hides a disabled playlist");
        expect(db.favourites().empty(), "favourites hides a disabled playlist's favourite");
        expect(!db.channelByLcn(5) && !db.channelByLcn(12), "channelByLcn skips a disabled playlist");
        expect(!hasGroup(db.listGroups(), L"Movies"), "listGroups drops a disabled playlist's groups");

        db.setPlaylistEnabled(pid, true);  // restore for any later assertions
        expect(db.allChannels().size() == 8, "re-enable restores allChannels");
        expect(db.favourites().size() == 1, "re-enable restores favourites");
        expect(db.channelByLcn(5) && db.channelByLcn(5)->name == L"Channel, One",
               "re-enable restores LCN lookup");
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
        expect(r2[(int)Panel::Grid].left == 0 && r2[(int)Panel::Grid].right <= r2[(int)Panel::Nav].left,
               "after dock Grid->left-of-Nav, grid is leftmost");
        bool allPresent = true;
        for (int k = 0; k < kPanelCount; ++k)
            allPresent &= (r2[k].right > r2[k].left && r2[k].bottom > r2[k].top);
        expect(allPresent, "re-dock keeps all three panels laid out");
    }

    out("\n== Meter model codecs (rabbitears::mac) ==\n");
    {
        using namespace rabbitears::mac;

        // Style: round-trips for every look; unknown -> fallback.
        bool styleRt = true;
        for (MeterStyle s : {MeterStyle::Led, MeterStyle::Tube, MeterStyle::Lcd, MeterStyle::Scope})
            styleRt &= (meterStyleFromString(meterStyleToString(s), MeterStyle::Led) == s);
        expect(styleRt, "meter style round-trips for all looks");
        expect(meterStyleFromString("bogus", MeterStyle::Scope) == MeterStyle::Scope,
               "unknown meter style -> fallback");

        // Palette: the classic default pins both the look and the codec token order.
        const std::string kDefault = "inherit,26282C,60CD80,E8BC56,E86056,D97757,ECECF0";
        expect(meterPaletteToString(defaultMeterPalette(MeterKind::Spectrum)) == kDefault,
               "default meter palette serializes to the classic tokens");
        expect(meterPaletteToString(meterPaletteFromString(kDefault, MeterPalette{})) == kDefault,
               "meter palette string round-trips");
        expect(meterPaletteFromString("theme,26282C,60CD80,E8BC56,E86056,D97757,ECECF0",
                                      MeterPalette{})
                   .bg.inherit,
               "legacy 'theme' bg parses as inherit");

        // Wrong arity -> whole fallback; one bad token -> just that field falls back.
        MeterPalette fb;
        fb.off = SkinColor{1, 2, 3};
        expect(meterPaletteToString(meterPaletteFromString("26282C", fb)) == meterPaletteToString(fb),
               "meter palette wrong arity -> whole fallback");
        const MeterPalette pf =
            meterPaletteFromString("inherit,ZZZZZZ,60CD80,E8BC56,E86056,D97757,ECECF0", fb);
        expect(pf.off == fb.off && pf.low == SkinColor{96, 205, 128},
               "one garbled palette token -> that field falls back, others parse");

        // Tuning: neutral default; clamp 0..1 + per-field fallback; wrong arity -> fallback.
        expect(meterTuningToString(defaultMeterTuning()) == "0.500,0.500,0.500,0.500,0.500",
               "default meter tuning serializes neutral");
        const MeterTuning tfb;  // all 0.5
        const MeterTuning t = meterTuningFromString("2.0,-1.0,x,0.25,0.75", tfb);
        expect(t.glow == 1.0f && t.smoothing == 0.0f && t.sensitivity == 0.5f &&
                   t.peakHold == 0.25f && t.breathing == 0.75f,
               "meter tuning clamps 0..1 and falls back per garbled field");
        expect(meterTuningToString(meterTuningFromString("0.1,0.2", tfb)) == meterTuningToString(tfb),
               "meter tuning wrong arity -> whole fallback");
    }

    // ---- GridFilter pushdown (twin of the Win32 CLI "GridFilter" block) ----------------------
    //
    // The mac grid now passes a GridFilter, so these semantics are load-bearing for mac — but the
    // only gate on them lived in RabbitEarsCli, a WINDOWS-ONLY target. This is the mac twin,
    // running against real sqlite.
    {
        out("\n-- grid filter --\n");
        Database gdb;
        const std::wstring gpath = dbPath + L".grid";
        std::error_code gec;
        std::filesystem::remove(gpath, gec);
        std::filesystem::remove(gpath + L"-wal", gec);
        std::filesystem::remove(gpath + L"-shm", gec);
        std::wstring gerr;
        expect(gdb.open(gpath, &gerr), "grid: database opens");
        const long long gp = gdb.addPlaylist(L"Grid", L"http://g/x.m3u", true, 1000, L"");

        // 40 rows: the first 30 "News", the last 10 "Sports" — so a Sports filter matches ONLY
        // rows 30..39 and any cap-before-filter ordering shows an empty grid.
        std::vector<ParsedChannel> rows;
        for (int i = 0; i < 40; ++i) {
            ParsedChannel c;
            c.name = L"Ch " + std::to_wstring(i);
            c.streamUrl = L"http://g/" + std::to_wstring(i) + L".ts";
            c.groupTitle = (i < 30) ? L"News" : L"Sports";
            rows.push_back(std::move(c));
        }
        expect(gdb.bulkInsertChannels(gp, rows, 1000) == 40, "grid: 40 rows inserted");

        Database::GridFilter none;
        expect(gdb.allChannels(none).size() == 40, "grid: an empty filter returns everything");

        Database::GridFilter cap;
        cap.limit = 10;
        expect(gdb.allChannels(cap).size() == 10, "grid: limit caps the row count");

        Database::GridFilter zero;  // 0 must mean UNLIMITED, not SQLite's "LIMIT 0" = none
        zero.limit = 0;
        expect(gdb.allChannels(zero).size() == 40,
               "grid: limit 0 means unlimited (SQLite's LIMIT 0 would return NOTHING)");

        const auto gall = gdb.allChannels();
        gdb.setDeadStatus(gall[0].id, DeadStatus::Dead, 5000);
        Database::GridFilter hide;
        hide.hideDead = true;
        expect(gdb.allChannels(hide).size() == 39, "grid: hideDead drops the dead row");

        Database::GridFilter cats;
        cats.categories = {L"Sports"};
        expect(gdb.allChannels(cats).size() == 10, "grid: a category filter keeps only that group");

        // *** THE COMPOSITION TEST. Cap of 10 + a filter matching only rows 30..39. If the cap
        // were applied BEFORE the filter (the old mac C++ post-filter order) this returns ZERO. ***
        Database::GridFilter both;
        both.categories = {L"Sports"};
        both.limit = 10;
        expect(gdb.allChannels(both).size() == 10,
               "grid: cap + filter returns the first N MATCHES, not the matches among the first N "
               "(the empty-grid bug)");

        // The cap must reach the other grid entry points mac uses, not just allChannels.
        expect(gdb.searchChannels(L"Ch ", cap).size() == 10, "grid: searchChannels honours the cap");
        expect(gdb.channelsByGroup(L"News", cap).size() == 10, "grid: channelsByGroup honours the cap");
        gdb.close();
        std::filesystem::remove(gpath, gec);
        std::filesystem::remove(gpath + L"-wal", gec);
        std::filesystem::remove(gpath + L"-shm", gec);
    }

    // ---- i18n catalog gate (twin of the Win32 CLI block) -------------------------------------
    //
    // The mac app owns ~145 mac-only StringIds, and until now NOTHING on the mac side checked the
    // catalog: the gate lived only in RabbitEarsCli, a Windows-only target. So a mac-only id added
    // with an empty ja/zh-Hant row, or a translation that dropped a {0}, could only be caught by
    // CI running the *Windows* job. Both helpers are header-declared in the shared catalog and
    // already linked here, so this costs nothing to run on every mac build.
    {
        using namespace rabbitears::i18n;
        out("\n-- i18n catalog --\n");
        StringId missing = StringId::Count;
        expect(catalogIsComplete(&missing),
               "every StringId is non-empty in every shipped language");
        if (missing != StringId::Count)
            out("  first missing at StringId #" + std::to_string(static_cast<int>(missing)) + "\n");

        // Placeholder parity: every language must carry the same number of {n}/%d/%s tokens per
        // key as English, or TrF()/format breaks at RUNTIME, in that language only — the failure
        // mode least likely to be noticed by an English-speaking developer.
        struct LangCase { Lang lang; const char* name; };
        const LangCase others[] = {{Lang::Ja, "ja"}, {Lang::ZhHant, "zh-Hant"}, {Lang::ZhHK, "zh-HK"}};
        for (const LangCase& lc : others) {
            int mismatches = 0;
            StringId firstBad = StringId::Count;
            for (int k = 0; k < static_cast<int>(StringId::Count); ++k) {
                const StringId id = static_cast<StringId>(k);
                if (placeholderCount(id, Lang::En) != placeholderCount(id, lc.lang)) {
                    if (mismatches++ == 0) firstBad = id;
                }
            }
            expect(mismatches == 0,
                   std::string("placeholder tokens match between en and ") + lc.name +
                       " for every key");
            if (mismatches)
                out("  first parity mismatch at StringId #" +
                    std::to_string(static_cast<int>(firstBad)) + "\n");
        }
    }

    out(g_fail == 0 ? "\nALL PASS\n" : "\n" + std::to_string(g_fail) + " FAILURE(S)\n");
    return g_fail == 0 ? 0 : 1;
}
