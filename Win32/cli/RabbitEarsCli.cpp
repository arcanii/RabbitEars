// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEarsCli — headless test/inspection tool for the RabbitEars core (M3U
// parser + SQLite store), the GUI-free way to prove the core end-to-end (mirrors
// the sibling apps' GvasCli). Usage:
//   RabbitEarsCli --selftest              run parser + DB round-trip assertions
//   RabbitEarsCli <file.m3u> [--limit N]  parse a playlist file, store it, dump it
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include <sqlite3.h>  // selftest only: hand-builds a pre-EPG (v2) DB to test the upgrade

#include "core/Gzip.h"
#include "core/Http.h"
#include "core/M3uParser.h"
#include "core/RecordingScheduler.h"
#include "core/XmltvParser.h"
#include "db/Database.h"
#include "platform/Encoding.h"
#include "ui/DockLayout.h"
#include "ui/Skin.h"
#include "ui/VideoGrid.h"

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
    out("== Gzip (gunzip) ==\n");
    {
        // A real gzip member (FLG=0) of the 43-byte XML string below, produced by
        // .NET GzipStream — exercises the fixed-header fast path end to end.
        const std::string plain = "<tv><programme>News at Ten</programme></tv>";
        const unsigned char gz[] = {
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0xb3, 0x29,
            0x29, 0xb3, 0xb3, 0x29, 0x28, 0xca, 0x4f, 0x2f, 0x4a, 0xcc, 0xcd, 0x4d,
            0xb5, 0xf3, 0x4b, 0x2d, 0x2f, 0x56, 0x48, 0x2c, 0x51, 0x08, 0x49, 0xcd,
            0xb3, 0xd1, 0x47, 0x08, 0xdb, 0xe8, 0x03, 0x95, 0x01, 0x00, 0xd0, 0x64,
            0x56, 0xce, 0x2b, 0x00, 0x00, 0x00};
        const std::string gzStr(reinterpret_cast<const char*>(gz), sizeof(gz));
        expect(gunzipIfNeeded(gzStr) == plain, "gunzip inflates a real gzip member");

        // Same stream with an FNAME field injected (set FLG bit 3, splice "epg.xml\0"
        // after the 10-byte fixed header). The CRC32 covers the *uncompressed* data,
        // so it stays valid — this exercises the optional-field skip.
        std::string named = gzStr;
        named[3] = static_cast<char>(static_cast<unsigned char>(named[3]) | 0x08);
        const char fname[] = "epg.xml";  // sizeof includes the trailing NUL
        named.insert(10, std::string(fname, sizeof(fname)));
        expect(gunzipIfNeeded(named) == plain, "gunzip skips the FNAME header field");

        // Non-gzip bytes pass through untouched; a truncated stream fails to empty.
        expect(gunzipIfNeeded(plain) == plain, "non-gzip bytes pass through unchanged");
        expect(gunzipIfNeeded(gzStr.substr(0, 20)).empty(), "truncated gzip -> empty (failure)");
    }

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

    out("== EPG parser (XMLTV) ==\n");
    {
        const std::string xml =
            "\xEF\xBB\xBF"  // BOM tolerated
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n"
            "<tv generator-info-name=\"test\">\n"
            "  <channel id=\"cnn.us\"><display-name>CNN</display-name></channel>\n"
            "  <programme start=\"20260705140000 +0000\" stop=\"20260705150000 +0000\" channel=\"cnn.us\">\n"
            "    <title lang=\"en\">News &amp; Weather</title>\n"
            "    <sub-title>Caf&#233;</sub-title>\n"
            "    <desc>Plain &amp; <![CDATA[<raw> tags]]> end</desc>\n"
            "    <category lang=\"en\">News</category>\n"
            "    <episode-num system=\"onscreen\">S05E03</episode-num>\n"
            "    <icon src=\"http://x/icon.png\"/>\n"
            "  </programme>\n"
            "  <programme start=\"20260705150000 +0100\" stop=\"20260705160000 +0100\" channel=\"bbc.uk\">\n"
            "    <title>Match of the Day</title>\n"
            "  </programme>\n"
            "</tv>\n";
        const XmltvDocument epgDoc = parseXmltv(xml);
        expect(epgDoc.programmes.size() == 2,
               "2 programmes parsed (got " + std::to_string(epgDoc.programmes.size()) + ")");
        if (epgDoc.programmes.size() == 2) {
            const auto& p = epgDoc.programmes[0];
            expect(p.channelId == L"cnn.us", "programme @channel captured");
            expect(p.title == L"News & Weather", "&amp; entity decoded in title");
            expect(p.subTitle == L"Café", "decimal numeric entity (&#233;) decoded");
            expect(p.descr == L"Plain & <raw> tags end", "entities + CDATA verbatim in desc");
            expect(p.category == L"News", "first <category> captured");
            expect(p.episodeNum == L"S05E03", "<episode-num> captured");
            expect(p.iconUrl == L"http://x/icon.png", "self-closing <icon src> captured");
            expect(p.startUtc == 1783260000LL, "start -> UTC epoch (2026-07-05 14:00Z)");
            expect(p.stopUtc - p.startUtc == 3600, "stop - start == 1h");

            const auto& q = epgDoc.programmes[1];
            expect(q.startUtc == p.startUtc, "tz offset applied (15:00 +0100 == 14:00 +0000)");
        }
        expect(parseXmltvTime("20260705140000") == 1783260000LL, "missing tz treated as UTC");
        expect(parseXmltvTime("202607051400") == 1783260000LL, "missing seconds tolerated");
        expect(parseXmltvTime("2026") == 0, "too-few digits -> 0");
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

    out("== EPG store ==\n");
    {
        const long long epgPid =
            db.addPlaylist(L"Guide", L"http://x/pl", true, 1000, L"http://x/epg.xml.gz");
        expect(epgPid > 0, "addPlaylist with epgUrl");
        std::wstring storedEpg;
        for (const auto& pl : db.listPlaylists())
            if (pl.id == epgPid) storedEpg = pl.epgUrl;
        expect(storedEpg == L"http://x/epg.xml.gz", "epg_url persisted + read back via listPlaylists");

        // Custom EPG-URL override: point Refresh Guide at a better guide than the M3U's x-tvg-url.
        db.setPlaylistEpgUrl(epgPid, L"http://x/override.xml.gz");
        std::wstring overEpg;
        for (const auto& pl : db.listPlaylists())
            if (pl.id == epgPid) overEpg = pl.epgUrl;
        expect(overEpg == L"http://x/override.xml.gz", "setPlaylistEpgUrl overrides the guide URL");
        db.setPlaylistEpgUrl(epgPid, L"");  // clearing resets it back to the original for the rows below
        std::wstring clearedEpg = L"unset";
        for (const auto& pl : db.listPlaylists())
            if (pl.id == epgPid) clearedEpg = pl.epgUrl;
        expect(clearedEpg.empty(), "setPlaylistEpgUrl(\"\") clears the override");
        db.setPlaylistEpgUrl(epgPid, L"http://x/epg.xml.gz");  // restore for downstream programme tests

        auto mk = [](const wchar_t* ch, long long s, long long e, const wchar_t* t) {
            Programme p;
            p.channelId = ch;
            p.startUtc = s;
            p.stopUtc = e;
            p.title = t;
            return p;
        };
        const std::vector<Programme> progs = {mk(L"cnn.us", 1000, 2000, L"Morning"),
                                              mk(L"cnn.us", 2000, 3000, L"Noon"),
                                              mk(L"cnn.us", 3000, 4000, L"Evening"),
                                              mk(L"bbc.uk", 1500, 2500, L"Football")};
        const int storedN = db.bulkInsertProgrammes(epgPid, progs, 9999);
        expect(storedN == 4, "bulkInsertProgrammes stored 4 (got " + std::to_string(storedN) + ")");

        // At t=2500, "Noon" (2000..3000) is airing and "Evening" is next.
        const auto nn = db.nowNext(epgPid, L"cnn.us", 2500);
        expect(nn.size() == 2 && nn[0].title == L"Noon" && nn[1].title == L"Evening",
               "nowNext returns the airing programme + the following one");

        // Window [1800,2200) overlaps Morning + Noon (cnn) + Football (bbc) = 3.
        expect(db.programmesInWindow(epgPid, 1800, 2200).size() == 3,
               "programmesInWindow returns overlapping rows across channels");

        // A refresh is authoritative: re-inserting a smaller batch replaces the old one.
        const std::vector<Programme> fewer = {mk(L"cnn.us", 1000, 2000, L"Only")};
        expect(db.bulkInsertProgrammes(epgPid, fewer, 9999) == 1, "refresh re-inserts (1 row)");
        expect(db.programmesInWindow(epgPid, 0, 100000).size() == 1,
               "old programmes cleared on refresh (authoritative)");

        // FK cascade: deleting the playlist drops its guide.
        db.deletePlaylist(epgPid);
        expect(db.programmesInWindow(epgPid, 0, 100000).empty(),
               "ON DELETE CASCADE removes the playlist's programmes");
    }

    out("== Scheduled recordings ==\n");
    {
        ScheduledRecording s;
        s.channelId = L"cnn.us";
        s.channelName = L"CNN";
        s.streamUrl = L"http://s/cnn";
        s.userAgent = L"UA/1";
        s.title = L"News";
        s.startUtc = 5000;
        s.stopUtc = 8000;
        s.mux = L"mkv";
        s.createdAt = 1000;
        const long long sid = db.addSchedule(s);
        expect(sid > 0, "addSchedule returns id");

        auto list = db.listSchedules();
        expect(list.size() == 1 && list[0].channelName == L"CNN" &&
                   list[0].streamUrl == L"http://s/cnn" && list[0].startUtc == 5000 &&
                   list[0].stopUtc == 8000 && list[0].mux == L"mkv" &&
                   list[0].status == ScheduleStatus::Pending,
               "listSchedules round-trips the row");

        db.updateScheduleStatus(sid, ScheduleStatus::Recording, L"C:\\rec\\news.mkv");
        auto rec = db.listSchedules();
        expect(rec.size() == 1 && rec[0].status == ScheduleStatus::Recording &&
                   rec[0].filePath == L"C:\\rec\\news.mkv",
               "updateScheduleStatus sets status + file path");

        db.updateScheduleStatus(sid, ScheduleStatus::Done);  // empty path must not clobber
        auto done = db.listSchedules();
        expect(done.size() == 1 && done[0].status == ScheduleStatus::Done &&
                   done[0].filePath == L"C:\\rec\\news.mkv",
               "status update with no path preserves the recorded file");

        db.deleteSchedule(sid);
        expect(db.listSchedules().empty(), "deleteSchedule removes the row");
    }

    out("== Scheduler planning ==\n");
    {
        auto mk = [](long long id, long long start, long long stop, ScheduleStatus st) {
            ScheduledRecording s;
            s.id = id;
            s.startUtc = start;
            s.stopUtc = stop;
            s.status = st;
            return s;
        };
        using S = ScheduleStatus;
        {  // airing + recorder free -> start
            auto p = planScheduler({mk(1, 100, 200, S::Pending)}, 150, false);
            expect(p.start.size() == 1 && p.start[0] == 1 && p.stop.empty() && p.miss.empty(),
                   "airing + free -> start");
        }
        {  // airing + a manual recording holds the recorder -> stay pending (retry)
            auto p = planScheduler({mk(1, 100, 200, S::Pending)}, 150, true);
            expect(p.start.empty() && p.miss.empty(), "airing + recorder busy -> no start (retry)");
        }
        {  // window fully passed while still pending -> miss
            auto p = planScheduler({mk(1, 100, 200, S::Pending)}, 250, false);
            expect(p.miss.size() == 1 && p.miss[0] == 1 && p.start.empty(), "window passed -> miss");
        }
        {  // recording and now >= stop -> stop
            auto p = planScheduler({mk(1, 100, 200, S::Recording)}, 200, false);
            expect(p.stop.size() == 1 && p.stop[0] == 1 && p.start.empty(), "recording, now>=stop -> stop");
        }
        {  // recording mid-window -> no action
            auto p = planScheduler({mk(1, 100, 200, S::Recording)}, 150, false);
            expect(p.stop.empty() && p.start.empty() && p.miss.empty(), "recording mid-window -> no-op");
        }
        {  // two airing + free -> only ONE starts (single recorder)
            auto p = planScheduler({mk(1, 100, 200, S::Pending), mk(2, 100, 200, S::Pending)}, 150, false);
            expect(p.start.size() == 1, "two airing + free -> only one starts");
        }
        {  // an active recording blocks a second pending from starting
            auto p =
                planScheduler({mk(1, 100, 300, S::Recording), mk(2, 100, 200, S::Pending)}, 150, false);
            expect(p.start.empty() && p.stop.empty(), "busy with a schedule -> no second start");
        }
        {  // future schedule -> nothing yet
            auto p = planScheduler({mk(1, 500, 600, S::Pending)}, 100, false);
            expect(p.start.empty() && p.miss.empty() && p.stop.empty(), "future schedule -> no action");
        }
        {  // Done/Cancelled are inert
            auto p = planScheduler({mk(1, 100, 200, S::Done), mk(2, 100, 200, S::Cancelled)}, 150, false);
            expect(p.start.empty() && p.stop.empty() && p.miss.empty(), "terminal statuses are inert");
        }
    }

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

    out("\n== Video grid (multi-pane) ==\n");
    {
        VideoGridOpts opts;  // gaps/insets 0 so the tiling arithmetic is exact to assert
        // Single: one pane fills the region.
        auto s1 = computeVideoPanes(ViewMode::Single, 1, 10, 20, 100, 80, opts);
        expect(s1.size() == 1 && s1[0].x == 10 && s1[0].y == 20 && s1[0].w == 100 && s1[0].h == 80,
               "Single: one pane fills the region");
        // Split 2 -> side-by-side halves tiling the width exactly.
        auto s2 = computeVideoPanes(ViewMode::Split, 2, 0, 0, 100, 80, opts);
        expect(s2.size() == 2 && s2[0].x == 0 && s2[0].w == 50 && s2[1].x == 50 && s2[1].w == 50 &&
                   s2[0].h == 80 && s2[1].h == 80,
               "Split 2 -> side-by-side halves");
        // Split 4 -> 2x2 quadrants; the far row/column lands exactly on the content edge.
        auto s4 = computeVideoPanes(ViewMode::Split, 4, 0, 0, 100, 100, opts);
        const bool quad = s4.size() == 4 && s4[0].x == 0 && s4[0].y == 0 && s4[0].w == 50 &&
                          s4[0].h == 50 && s4[3].x == 50 && s4[3].y == 50 &&
                          s4[3].x + s4[3].w == 100 && s4[3].y + s4[3].h == 100;
        expect(quad, "Split 4 -> 2x2 quadrants tiling exactly");
        // Split honours the inter-pane gap while still bounding the far edge.
        VideoGridOpts g{};
        g.gap = 10;
        auto sg = computeVideoPanes(ViewMode::Split, 2, 0, 0, 100, 50, g);
        expect(sg[0].w == 45 && sg[1].x == 55 && sg[1].x + sg[1].w == 100,
               "Split honours the inter-pane gap");
        // Pip: pane 0 fills; pane 1 is the bottom-right inset inside the region.
        VideoGridOpts p{};
        p.pipW = 30;
        p.pipH = 20;
        p.pipMargin = 5;
        auto sp = computeVideoPanes(ViewMode::Pip, 2, 0, 0, 200, 100, p);
        expect(sp.size() == 2 && sp[0].w == 200 && sp[0].h == 100, "Pip: pane 0 fills the region");
        expect(sp[1].w == 30 && sp[1].h == 20 && sp[1].x == 165 && sp[1].y == 75,
               "Pip: inset sits in the bottom-right corner");
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

    out("== schema migration (v2 -> v4) ==\n");
    {
        // Hand-build a pre-EPG (v2) database, then let Database::open() upgrade it —
        // proving an existing 0.1.9 DB gains EPG + scheduler tables without losing data.
        const std::wstring mpath = std::wstring(tmp) + L"rabbitears_migrate.db";
        DeleteFileW(mpath.c_str());
        DeleteFileW((mpath + L"-wal").c_str());
        DeleteFileW((mpath + L"-shm").c_str());
        {
            sqlite3* raw = nullptr;
            sqlite3_open(utf8FromWide(mpath).c_str(), &raw);
            sqlite3_exec(
                raw,
                "CREATE TABLE playlists(id INTEGER PRIMARY KEY, name TEXT NOT NULL, source_url TEXT,"
                " source_path TEXT, is_url INTEGER NOT NULL DEFAULT 1, added_at INTEGER NOT NULL,"
                " last_refreshed_at INTEGER, channel_count INTEGER NOT NULL DEFAULT 0,"
                " enabled INTEGER NOT NULL DEFAULT 1);"
                "CREATE TABLE channels(id INTEGER PRIMARY KEY, playlist_id INTEGER NOT NULL"
                " REFERENCES playlists(id) ON DELETE CASCADE, name TEXT NOT NULL,"
                " stream_url TEXT NOT NULL, logo_url TEXT, group_title TEXT, tvg_id TEXT,"
                " tvg_name TEXT, lcn INTEGER, is_favourite INTEGER NOT NULL DEFAULT 0,"
                " dead_status INTEGER NOT NULL DEFAULT 0, last_checked_at INTEGER NOT NULL DEFAULT 0,"
                " sort_order INTEGER NOT NULL DEFAULT 0, user_agent TEXT, referrer TEXT);"
                "CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT);"
                "INSERT INTO playlists(id,name,added_at,enabled) VALUES(7,'Legacy',1000,1);"
                "INSERT INTO channels(playlist_id,name,stream_url,tvg_id) VALUES(7,'Old','http://o','o.uk');"
                "PRAGMA user_version=2;",
                nullptr, nullptr, nullptr);
            sqlite3_close(raw);
        }
        Database mdb;
        std::wstring merr;
        expect(mdb.open(mpath, &merr),
               "v2 DB opens + migrates" + (merr.empty() ? "" : " (" + utf8FromWide(merr) + ")"));
        expect(mdb.channelsByPlaylist(7).size() == 1, "existing channel survives the v2->v3 upgrade");
        const long long np = mdb.addPlaylist(L"New", L"http://n", true, 2000, L"http://n/epg");
        std::wstring got;
        for (const auto& pl : mdb.listPlaylists())
            if (pl.id == np) got = pl.epgUrl;
        expect(got == L"http://n/epg", "epg_url column added by migration");
        Programme pr;
        pr.channelId = L"o.uk";
        pr.startUtc = 1000;
        pr.stopUtc = 2000;
        pr.title = L"x";
        expect(mdb.bulkInsertProgrammes(7, {pr}, 3000) == 1, "epg_programmes table added by migration");
        ScheduledRecording ms;
        ms.channelName = L"Ch";
        ms.streamUrl = L"http://c";
        ms.startUtc = 1;
        ms.stopUtc = 2;
        expect(mdb.addSchedule(ms) > 0, "scheduled_recordings table added by migration (v4)");
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
    const long long pid = db.addPlaylist(name, source, isUrl, now, doc.epgUrl);
    const int n = db.bulkInsertChannels(pid, doc.channels, now);
    line(L"Imported " + std::to_wstring(n) + L" channels into " + Database::defaultDbPath());
    if (!doc.epgUrl.empty()) line(L"EPG URL captured: " + doc.epgUrl);
    return 0;
}

std::wstring epochToUtc(long long e) {
    if (e == 0) return L"(none)";
    const std::time_t t = static_cast<std::time_t>(e);
    std::tm tmv{};
    gmtime_s(&tmv, &t);
    wchar_t buf[32];
    wcsftime(buf, sizeof(buf) / sizeof(buf[0]), L"%Y-%m-%d %H:%M UTC", &tmv);
    return buf;
}

// Fetch (or read) an XMLTV feed, gunzip if needed, parse, and report a summary — the
// headless real-world check for the EPG pipeline (the --fetch analogue for guides).
int epgTool(const std::wstring& source) {
    std::string bytes;
    std::wstring err;
    const bool isUrl = source.rfind(L"http", 0) == 0;
    if (isUrl) {
        line(L"Fetching " + source);
        if (!httpGet(source, bytes, err)) { line(L"Download failed: " + err); return 1; }
    } else {
        std::ifstream f(source, std::ios::binary);
        if (!f) { line(L"Cannot open " + source); return 1; }
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    line(L"Read " + std::to_wstring(bytes.size()) + L" bytes");
    const std::string xml = gunzipIfNeeded(bytes);
    if (xml.empty()) { line(L"Empty or invalid after gunzip"); return 1; }
    if (xml.size() != bytes.size()) line(L"Gunzipped to " + std::to_wstring(xml.size()) + L" bytes");

    const XmltvDocument epg = parseXmltv(xml);
    std::set<std::wstring> chans;
    long long lo = 0, hi = 0;
    for (const auto& p : epg.programmes) {
        chans.insert(p.channelId);
        if (p.startUtc != 0 && (lo == 0 || p.startUtc < lo)) lo = p.startUtc;
        if (p.stopUtc > hi) hi = p.stopUtc;
    }
    line(L"Programmes: " + std::to_wstring(epg.programmes.size()));
    line(L"Channels:   " + std::to_wstring(chans.size()));
    line(L"From:       " + epochToUtc(lo));
    line(L"To:         " + epochToUtc(hi));
    int shown = 0;
    for (const auto& p : epg.programmes) {
        if (shown++ >= 8) break;
        line(L"  [" + p.channelId + L"] " + epochToUtc(p.startUtc) + L"  " + p.title);
    }
    return epg.programmes.empty() ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && std::wstring(argv[1]) == L"--selftest") return selftest();
    if (argc >= 3 && std::wstring(argv[1]) == L"--fetch") return fetch(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"--import") return importSource(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"--epg") return epgTool(argv[2]);
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
