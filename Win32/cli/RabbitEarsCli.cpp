// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEarsCli — headless test/inspection tool for the RabbitEars core (M3U
// parser + SQLite store), the GUI-free way to prove the core end-to-end (mirrors
// the sibling apps' GvasCli). Usage:
//   RabbitEarsCli --selftest              run parser + DB round-trip assertions
//   RabbitEarsCli <file.m3u> [--limit N]  parse a playlist file, store it, dump it
//   RabbitEarsCli --xtream [url] [--raw]  probe a real Xtream provider's player_api.php
#include <algorithm>
#include <chrono>
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
#include "core/Json.h"
#include "core/XtreamClient.h"
#include "core/M3uParser.h"
#include "core/M3uWriter.h"
#include "core/PowerPolicy.h"
#include "core/RecordingRules.h"
#include "core/UrlCanon.h"
#include "core/Strings.h"
#include "core/RecordingScheduler.h"
#include "core/XmltvParser.h"
#include "db/Database.h"
#include "platform/Encoding.h"
#include "ui/DockLayout.h"
#include "core/DeadLinkCheck.h"
#include "ui/GlassMask.h"
#include "ui/VuLamp.h"
#include "ui/Skin.h"
#include "ui/VideoGrid.h"

#include "XtreamRecon.h"
// Relative, deliberately: this is a Win32/ GUI header and Win32/ is NOT on the CLI's include
// path. Only the header-inline bufferGrid() is used — the rest of the declarations here are
// never called, so nothing from the GUI TU has to link.
#include "../ui/BufferMeter.h"

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

    out("== M3U writer (round-trip) ==\n");
    {
        // Everything the parser produced must survive write -> re-parse unchanged —
        // this is the favourites-export contract.
        const M3uDocument rt = parseM3u(writeM3u(doc));
        expect(rt.epgUrl == doc.epgUrl, "x-tvg-url round-trips");
        expect(rt.channels.size() == doc.channels.size(),
               "channel count round-trips (got " + std::to_string(rt.channels.size()) + ")");
        if (rt.channels.size() == doc.channels.size()) {
            bool all = true;
            for (size_t i = 0; i < rt.channels.size(); ++i) {
                const auto& x = doc.channels[i];
                const auto& y = rt.channels[i];
                all = all && x.name == y.name && x.streamUrl == y.streamUrl &&
                      x.logoUrl == y.logoUrl && x.groupTitle == y.groupTitle &&
                      x.tvgId == y.tvgId && x.tvgName == y.tvgName && x.chno == y.chno &&
                      x.userAgent == y.userAgent && x.referrer == y.referrer;
            }
            expect(all, "every field of every channel round-trips");
            expect(rt.channels[0].name == L"Channel, One",
                   "comma-in-name survives (quoted attrs shield the split)");
        }
        // A quote inside an attribute value can't be represented — it degrades to an
        // apostrophe but must still re-parse as ONE channel with the other fields intact.
        M3uDocument tricky;
        ParsedChannel t;
        t.name = L"Quote \"Show\"";
        t.groupTitle = L"Say \"Hi\"";
        t.streamUrl = L"http://s/q.m3u8";
        tricky.channels.push_back(t);
        const M3uDocument rq = parseM3u(writeM3u(tricky));
        expect(rq.channels.size() == 1 && rq.channels[0].groupTitle == L"Say 'Hi'" &&
                   rq.channels[0].streamUrl == L"http://s/q.m3u8",
               "embedded quotes degrade to apostrophes, entry still parses");
        expect(writeM3u(M3uDocument{}) == "#EXTM3U\r\n", "empty document -> bare header");
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

    if (byLcn) {
        auto byId = db.channelById(byLcn->id);
        expect(byId && byId->name == L"Bee TV", "channelById round-trips the primary key");
    }
    expect(!db.channelById(999999), "channelById(unknown) -> nullopt");

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

    out("== Recording rules (DAO) ==\n");
    {
        RecordingRule r;
        r.channelId = L"cnn.us";
        r.channelName = L"CNN";
        r.titleMatch = L"News";
        r.match = RuleMatch::Contains;
        r.leadSec = 60;
        r.trailSec = 120;
        r.mux = L"mkv";
        r.createdAt = 5000;
        const long long rid = db.addRule(r);
        expect(rid > 0, "addRule returns id");

        auto rules = db.listRules();
        expect(rules.size() == 1 && rules[0].channelId == L"cnn.us" && rules[0].titleMatch == L"News" &&
                   rules[0].match == RuleMatch::Contains && rules[0].enabled &&
                   rules[0].leadSec == 60 && rules[0].trailSec == 120 && rules[0].mux == L"mkv",
               "listRules round-trips every field");

        db.setRuleEnabled(rid, false);
        expect(!db.listRules()[0].enabled, "setRuleEnabled(false) persists");
        db.setRuleEnabled(rid, true);

        // deleteRule drops the rule's still-Pending rows but KEEPS its history (Done etc.).
        auto mkRuleSched = [&](long long ruleId, long long start, ScheduleStatus st) {
            ScheduledRecording s;
            s.channelName = L"CNN";
            s.streamUrl = L"http://c";
            s.startUtc = start;
            s.stopUtc = start + 100;
            s.status = st;
            s.ruleId = ruleId;
            s.createdAt = 5000;
            return db.addSchedule(s);
        };
        const long long pendId = mkRuleSched(rid, 10000, ScheduleStatus::Pending);
        const long long doneId = mkRuleSched(rid, 20000, ScheduleStatus::Done);
        const long long otherId = mkRuleSched(0, 30000, ScheduleStatus::Pending);  // one-off
        expect(pendId > 0 && doneId > 0 && otherId > 0, "rule-tagged schedules insert");
        expect(db.listSchedules().size() == 3, "3 schedules queued");
        {
            auto all = db.listSchedules();
            const auto it = std::find_if(all.begin(), all.end(),
                                         [&](const ScheduledRecording& s) { return s.id == pendId; });
            expect(it != all.end() && it->ruleId == rid, "rule_id round-trips on a schedule");
        }

        db.deleteRule(rid);
        auto left = db.listSchedules();
        expect(db.listRules().empty(), "deleteRule removes the rule");
        expect(left.size() == 2, "deleteRule drops only the rule's PENDING rows (got " +
                                     std::to_string(left.size()) + ")");
        const bool keptDone = std::any_of(left.begin(), left.end(), [&](const ScheduledRecording& s) {
            return s.id == doneId && s.status == ScheduleStatus::Done;
        });
        const bool keptOneOff =
            std::any_of(left.begin(), left.end(), [&](const ScheduledRecording& s) { return s.id == otherId; });
        expect(keptDone, "a Done recording survives its rule's deletion (history)");
        expect(keptOneOff, "an unrelated one-off schedule is untouched");
        for (const auto& s : left) db.deleteSchedule(s.id);  // leave the DB clean for later blocks
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

    out("== Wake-timer policy (preflight) ==\n");
    {
        using W = WakeTimerSetting;
        // (hasBattery, onBattery, ac, dc) — rtcWakeCapable defaults true; overridden where tested.
        auto decide = [](bool hasBat, bool onBat, W ac, W dc) {
            WakePolicyInputs in;
            in.hasBattery = hasBat;
            in.onBattery = onBat;
            in.ac = ac;
            in.dc = dc;
            return decideWakePolicy(in);
        };
        {  // desktop, wake timers on -> nothing to say
            auto v = decide(false, false, W::Enable, W::Enable);
            expect(v.willWake && v.reason == WakeBlock::None && !v.otherSourceBlocked,
                   "desktop + wake timers enabled -> wakes, silent");
        }
        {  // the two blocking settings, on the source in use
            auto v = decide(false, false, W::Disable, W::Enable);
            expect(!v.willWake && v.reason == WakeBlock::TimersDisabled,
                   "AC wake timers disabled -> blocked (TimersDisabled)");
            auto i = decide(false, false, W::ImportantOnly, W::Enable);
            expect(!i.willWake && i.reason == WakeBlock::ImportantOnly,
                   "AC important-timers-only -> blocked (an app task is not 'important')");
        }
        {  // the source in use selects the index — DC only counts while discharging
            expect(decide(true, true, W::Disable, W::Enable).willWake,
                   "on battery reads DC (enabled), ignoring a disabled AC");
            expect(!decide(true, true, W::Enable, W::Disable).willWake,
                   "on battery + DC disabled -> blocked even though AC is enabled");
            expect(decide(true, false, W::Enable, W::Disable).willWake,
                   "plugged in reads AC (enabled), ignoring a disabled DC");
        }
        {  // the real-world laptop default: AC=Enable, DC=Disable. Records tonight, misses
           // tomorrow's airing if unplugged first — warn softly rather than stay silent.
            auto v = decide(true, false, W::Enable, W::Disable);
            expect(v.willWake && v.reason == WakeBlock::None && v.otherSourceBlocked,
                   "plugged-in laptop with DC disabled -> wakes now, flags the other source");
            auto b = decide(true, true, W::Disable, W::Enable);
            expect(b.willWake && b.otherSourceBlocked,
                   "on battery with AC disabled -> wakes now, flags the other source");
        }
        {  // a desktop has no DC state to switch to, so its DC index must never raise the flag
            auto v = decide(false, false, W::Enable, W::Disable);
            expect(v.willWake && !v.otherSourceBlocked,
                   "desktop (no battery) ignores the DC index entirely");
        }
        {  // An unreadable battery gauge (BatteryFlag 0xFF) reports hasBattery=false while the
           // machine really is discharging. Trusting it would read AC and promise a wake that the
           // DC policy refuses — the one false POSITIVE this feature must never emit.
            auto v = decide(false, true, W::Enable, W::Disable);
            expect(!v.willWake && v.reason == WakeBlock::TimersDisabled,
                   "onBattery implies a battery: reads DC even when hasBattery is misreported");
            auto o = decide(false, true, W::Disable, W::Enable);
            expect(o.willWake && o.otherSourceBlocked,
                   "onBattery without hasBattery still flags the blocked other source");
        }
        {  // hardware trumps policy, and beats a perfectly enabled plan
            WakePolicyInputs in;
            in.rtcWakeCapable = false;
            auto v = decideWakePolicy(in);
            expect(!v.willWake && v.reason == WakeBlock::NoRtcCapability,
                   "no RTC wake capability -> blocked, regardless of the power plan");
        }
        {  // an unreadable/newer index must fail OPEN — never nag on a policy we can't read
            auto v = decide(false, false, W::Unknown, W::Unknown);
            expect(v.willWake && v.reason == WakeBlock::None, "unknown AC index -> fail open");
            auto o = decide(true, false, W::Enable, W::Unknown);
            expect(o.willWake && !o.otherSourceBlocked, "unknown DC index -> no mismatch warning");
        }
    }

    out("== Recording rules (EPG expansion) ==\n");
    {
        expect(normaliseTvgId(L"CNN.us@SD") == L"cnn.us", "normaliseTvgId strips @feed + lowercases");


        expect(normaliseTvgId(L"") == L"", "normaliseTvgId tolerates empty");

        auto prog = [](const wchar_t* chan, const wchar_t* title, long long a, long long b) {
            Programme p;
            p.channelId = chan;
            p.title = title;
            p.startUtc = a;
            p.stopUtc = b;
            return p;
        };
        auto rule = [](long long id, const wchar_t* chan, const wchar_t* title, RuleMatch m) {
            RecordingRule r;
            r.id = id;
            r.channelId = chan;
            r.channelName = L"CNN";
            r.titleMatch = title;
            r.match = m;
            return r;
        };
        // now=1000, horizon=100000. Two airings of "News" + one "Movie" on the same channel.
        const std::vector<Programme> progs = {
            prog(L"CNN.us", L"News", 2000, 3000),
            prog(L"CNN.us", L"Movie", 3000, 4000),
            prog(L"CNN.us", L"News", 90000, 91000),
            prog(L"BBC.uk", L"News", 2000, 3000),  // different channel
        };
        {  // exact title + channel -> both News airings on CNN only
        // Locale-independent case folding. The app never calls setlocale(), so the old bare
        // towlower() folded only A-Z: a Contains rule for "cafe"-with-an-accent silently missed the
        // uppercase airing. Asserted through the public expander, since both sides of a match fold.
        {
            auto foldHit = [&](const wchar_t* progTitle, const wchar_t* ruleTitle) {
                std::vector<Programme> ps{prog(L"cnn.us", progTitle, 2000, 3000)};
                return expandRules({rule(9, L"cnn.us", ruleTitle, RuleMatch::Contains)}, ps, {},
                                   1000, 100000)
                    .size();
            };
            expect(foldHit(L"CAFÉ MUSIC", L"café") == 1, "fold: Latin-1 accents");
            expect(foldHit(L"ТВ Новости", L"тв") == 1,
                   "fold: Cyrillic");
            expect(foldHit(L"ΕΡΤ ΝΕΑ", L"ερτ") == 1,
                   "fold: Greek");
            expect(foldHit(L"ŽIVOT", L"život") == 1, "fold: Latin Extended-A");
            expect(foldHit(L"NEWS AT TEN", L"news") == 1, "fold: plain ASCII still works");
            expect(foldHit(L"ニュース", L"ニュース") == 1,
                   "fold: caseless scripts pass through");
            expect(foldHit(L"Sport", L"news") == 0, "fold: a non-match is still a non-match");
        }
            auto v = expandRules({rule(5, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {}, 1000, 100000);
            expect(v.size() == 2, "exact rule matches both airings (got " + std::to_string(v.size()) + ")");
            expect(v.size() == 2 && v[0].ruleId == 5 && v[0].title == L"News" &&
                       v[0].status == ScheduleStatus::Pending,
                   "generated rows carry ruleId + title + Pending");
        }
        {  // the rule's channel id is matched on the normalised base id (@feed / case)
            auto v = expandRules({rule(5, L"CNN.us@HD", L"news", RuleMatch::Exact)}, progs, {}, 1000, 100000);
            expect(v.size() == 2, "channel + title match are case/@feed insensitive");
        }
        {  // empty channelId == any channel
            RecordingRule any = rule(6, L"", L"News", RuleMatch::Exact);
            auto v = expandRules({any}, progs, {}, 1000, 100000);
            expect(v.size() == 3, "empty channelId matches any channel (got " + std::to_string(v.size()) + ")");
        }
        {  // Contains
            auto v = expandRules({rule(7, L"cnn.us", L"ov", RuleMatch::Contains)}, progs, {}, 1000, 100000);
            expect(v.size() == 1 && v[0].title == L"Movie", "contains match");
        }
        {  // disabled rule yields nothing; empty title pattern is refused (would match all)
            RecordingRule off = rule(8, L"cnn.us", L"News", RuleMatch::Exact);
            off.enabled = false;
            expect(expandRules({off}, progs, {}, 1000, 100000).empty(), "disabled rule expands to nothing");
            expect(expandRules({rule(9, L"cnn.us", L"", RuleMatch::Contains)}, progs, {}, 1000, 100000).empty(),
                   "empty title pattern never matches");
        }
        {  // horizon + already-finished programmes are skipped
            auto v = expandRules({rule(5, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {}, 1000, 50000);
            expect(v.size() == 1 && v[0].startUtc == 2000, "horizon excludes the far airing");
            auto w = expandRules({rule(5, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {}, 3500, 100000);
            expect(w.size() == 1 && w[0].startUtc == 90000, "a finished programme is not scheduled");
        }
        {  // lead/trail padding
            RecordingRule pad = rule(5, L"cnn.us", L"Movie", RuleMatch::Exact);
            pad.leadSec = 60;
            pad.trailSec = 120;
            auto v = expandRules({pad}, progs, {}, 1000, 100000);
            expect(v.size() == 1 && v[0].startUtc == 2940 && v[0].stopUtc == 4120,
                   "lead/trail padding applied");
        }
        {  // dedup vs existing rows — ANY status, so a cancelled slot never comes back
            ScheduledRecording done;
            done.channelId = L"CNN.us@SD";  // normalised on both sides
            done.startUtc = 2000;
            done.status = ScheduleStatus::Cancelled;
            auto v = expandRules({rule(5, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {done}, 1000, 100000);
            expect(v.size() == 1 && v[0].startUtc == 90000,
                   "a cancelled airing is not recreated (dedup on channel+start)");
        }
        {  // two rules matching the same airing collapse to ONE recording
            auto v = expandRules({rule(1, L"cnn.us", L"News", RuleMatch::Exact),
                                  rule(2, L"cnn.us", L"New", RuleMatch::Contains)},
                                 progs, {}, 1000, 100000);
            expect(v.size() == 2, "overlapping rules dedup onto one row per airing");
        }
        {  // Tombstones are LOAD-BEARING: every terminal status must block re-creation, which is
           // why the schedules manager cancels (rather than deletes) a rule's pending airing.
            auto blocks = [&](ScheduleStatus stt) {
                ScheduledRecording t;
                t.channelId = L"CNN.us";
                t.startUtc = 2000;
                t.status = stt;
                auto v = expandRules({rule(5, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {t}, 1000,
                                     100000);
                return v.size() == 1 && v[0].startUtc == 90000;  // only the far airing remains
            };
            expect(blocks(ScheduleStatus::Cancelled) && blocks(ScheduleStatus::Done) &&
                       blocks(ScheduleStatus::Missed) && blocks(ScheduleStatus::Failed) &&
                       blocks(ScheduleStatus::Pending) && blocks(ScheduleStatus::Recording),
                   "an existing row of ANY status blocks re-creating that airing");
            // Skipped is the whole point of the status: the rule must NOT re-queue this airing,
            // while still queuing every other episode (the far airing at 90000 survives).
            expect(blocks(ScheduleStatus::Skipped),
                   "a Skipped airing is a tombstone — the rule does not re-queue it");
        }
        {  // --- Padding-proof dedup (v7): a rule row owns its airing across lead/trail edits ---
           // The slot key is the PADDED start, so editing a rule's lead used to orphan the
           // existing row: a mid-recording lead edit spawned a duplicate Pending row that could
           // never start (recorder busy) and rotted into a phantom Missed, and a Cancelled
           // future airing's tombstone was resurrected. The stable identity is the programme's
           // unpadded start, persisted as progStartUtc (schema v7); pre-v7 rows fall back to
           // title-scoped window containment. Manual rows (ruleId 0) keep slot-only dedup.
            ScheduledRecording rec;  // created when the rule had leadSec=60 / trailSec=120
            rec.channelId = L"CNN.us";
            rec.title = L"News";
            rec.startUtc = 1940;  // 2000 - 60 (the OLD lead)
            rec.stopUtc = 3120;   // 3000 + 120
            rec.status = ScheduleStatus::Recording;
            rec.ruleId = 5;
            rec.progStartUtc = 2000;  // the airing's unpadded start (v7)
            RecordingRule edited = rule(5, L"cnn.us", L"News", RuleMatch::Exact);  // lead now 0
            auto v = expandRules({edited}, progs, {rec}, 2500 /*mid-airing*/, 100000);
            expect(v.size() == 1 && v[0].startUtc == 90000,
                   "a lead edit mid-recording does not duplicate the in-progress airing");
            expect(v.size() == 1 && v[0].progStartUtc == 90000,
                   "new rows persist the unpadded programme start");

            rec.status = ScheduleStatus::Cancelled;  // future tombstone, padding since edited
            auto w = expandRules({edited}, progs, {rec}, 1000, 100000);
            expect(w.size() == 1 && w[0].startUtc == 90000,
                   "a cancelled airing stays cancelled after a lead edit");

            // Pre-v7 row (progStartUtc 0): the title-scoped containment fallback still owns it.
            rec.status = ScheduleStatus::Recording;
            rec.progStartUtc = 0;
            auto lg = expandRules({edited}, progs, {rec}, 2500, 100000);
            expect(lg.size() == 1 && lg[0].startUtc == 90000,
                   "legacy (pre-v7) row: containment fallback prevents the duplicate");
            // ...but the fallback is title-scoped: a DIFFERENT programme nested inside a rule
            // row's padding window is NOT swallowed (its recording would be silently lost when
            // the containing row is Cancelled/Failed).
            ScheduledRecording game;  // legacy row [1000, 20000] spans the Movie airing
            game.channelId = L"CNN.us";
            game.title = L"Football";
            game.startUtc = 1000;
            game.stopUtc = 20000;
            game.status = ScheduleStatus::Failed;
            game.ruleId = 9;
            auto nest = expandRules({rule(5, L"cnn.us", L"Movie", RuleMatch::Exact)}, progs,
                                    {game}, 1000, 100000);
            expect(nest.size() == 1 && nest[0].startUtc == 3000,
                   "a different programme nested in a legacy row's window is still scheduled");

            ScheduledRecording manual;  // ruleId 0: spans both near airings, suppresses neither
            manual.channelId = L"CNN.us";
            manual.startUtc = 1000;
            manual.stopUtc = 20000;
            manual.status = ScheduleStatus::Pending;
            auto m = expandRules({rule(5, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {manual},
                                 1000, 100000);
            expect(m.size() == 2, "a spanning manual row does not suppress rule airings");

            // Two rules with DIFFERENT padding matching the same airing still collapse to one
            // row (the "two rules -> one recording" promise, previously defeated by padding).
            RecordingRule a = rule(1, L"cnn.us", L"News", RuleMatch::Exact);
            a.leadSec = 60;
            auto t = expandRules({a, rule(2, L"cnn.us", L"New", RuleMatch::Contains)}, progs, {},
                                 1000, 100000);
            expect(t.size() == 2, "different-padding rules still collapse onto one row per airing");

            // The other direction: lead INCREASED, so the new padded start lands EARLIER than
            // the stored one — the slot key differs either way; the airing identity still owns it.
            ScheduledRecording bare;  // created with lead 0
            bare.channelId = L"CNN.us";
            bare.title = L"News";
            bare.startUtc = 2000;
            bare.stopUtc = 3000;
            bare.status = ScheduleStatus::Recording;
            bare.ruleId = 5;
            bare.progStartUtc = 2000;
            RecordingRule wider = rule(5, L"cnn.us", L"News", RuleMatch::Exact);
            wider.leadSec = 300;
            auto u = expandRules({wider}, progs, {bare}, 2500, 100000);
            expect(u.size() == 1 && u[0].startUtc == 89700,  // 90000 - lead 300
                   "a lead increase mid-recording does not duplicate the in-progress airing");

            // Back-to-back SAME-TITLE bulletins with trail >= the next airing's duration: the
            // unpadded identity keeps them distinct (window containment alone would swallow
            // every second bulletin — no row, no Missed, silent loss).
            std::vector<Programme> bulletins = {prog(L"CNN.us", L"News", 2000, 3000),
                                                prog(L"CNN.us", L"News", 3000, 4000)};
            RecordingRule longTrail = rule(4, L"cnn.us", L"News", RuleMatch::Exact);
            longTrail.trailSec = 3000;  // >= the 1000-second bulletin length
            auto bb = expandRules({longTrail}, bulletins, {}, 1000, 100000);
            expect(bb.size() == 2, "back-to-back same-title airings each keep their own row");

            // Edit flow with a big trail: the surviving in-progress row must not suppress the
            // rule's NEXT airing (whose Pending row the edit just cleared for re-creation).
            ScheduledRecording live;  // recording News [2000,3000], trail spans far beyond
            live.channelId = L"CNN.us";
            live.title = L"News";
            live.startUtc = 2000;
            live.stopUtc = 95000;  // huge trail: window covers the 90000 airing entirely
            live.status = ScheduleStatus::Recording;
            live.ruleId = 5;
            live.progStartUtc = 2000;
            auto fut = expandRules({rule(5, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {live},
                                   2500, 100000);
            expect(fut.size() == 1 && fut[0].startUtc == 90000 && fut[0].progStartUtc == 90000,
                   "a big-trail in-progress row does not suppress the rule's next airing");
        }
        {  // --- Episode dedup: skip a REPEAT airing of an already-scheduled episode ---
            auto ep = [](const wchar_t* chan, const wchar_t* title, long long a, long long b,
                         const wchar_t* epnum, const wchar_t* sub) {
                Programme p;
                p.channelId = chan;
                p.title = title;
                p.startUtc = a;
                p.stopUtc = b;
                p.episodeNum = epnum;
                p.subTitle = sub;
                return p;
            };
            {  // same show + same episode-num on two airings -> record ONCE
                std::vector<Programme> ps = {ep(L"CNN.us", L"Doctor Who", 2000, 3000, L"1.1.0/1", L""),
                                             ep(L"CNN.us", L"Doctor Who", 90000, 91000, L"1.1.0/1", L"")};
                auto v = expandRules({rule(5, L"cnn.us", L"Doctor Who", RuleMatch::Exact)}, ps, {}, 1000, 100000);
                expect(v.size() == 1, "repeat of the same episode-num is skipped (got " + std::to_string(v.size()) + ")");
                expect(v.size() == 1 && !v[0].episodeKey.empty(), "scheduled row carries an episode key");
            }
            {  // distinct episode-nums -> both recorded
                std::vector<Programme> ps = {ep(L"CNN.us", L"Doctor Who", 2000, 3000, L"1.1.0/1", L""),
                                             ep(L"CNN.us", L"Doctor Who", 90000, 91000, L"1.2.0/1", L"")};
                auto v = expandRules({rule(5, L"cnn.us", L"Doctor Who", RuleMatch::Exact)}, ps, {}, 1000, 100000);
                expect(v.size() == 2, "distinct episodes are both scheduled");
            }
            {  // no episode-num -> sub-title is the identity
                std::vector<Programme> ps = {ep(L"CNN.us", L"Nova", 2000, 3000, L"", L"The Deep"),
                                             ep(L"CNN.us", L"Nova", 90000, 91000, L"", L"The Deep")};
                auto v = expandRules({rule(5, L"cnn.us", L"Nova", RuleMatch::Exact)}, ps, {}, 1000, 100000);
                expect(v.size() == 1, "sub-title dedups a repeat when episode-num is absent");
            }
            {  // neither field -> no episode identity -> slot dedup only (both distinct airings kept)
                std::vector<Programme> ps = {ep(L"CNN.us", L"Live", 2000, 3000, L"", L""),
                                             ep(L"CNN.us", L"Live", 90000, 91000, L"", L"")};
                auto v = expandRules({rule(5, L"cnn.us", L"Live", RuleMatch::Exact)}, ps, {}, 1000, 100000);
                expect(v.size() == 2, "no episode identity -> distinct airings both scheduled");
            }
            {  // persisted across runs: feed a produced row back as `existing` -> a later airing skips
                std::vector<Programme> first = {ep(L"CNN.us", L"Doctor Who", 2000, 3000, L"1.1.0/1", L"")};
                auto made = expandRules({rule(5, L"cnn.us", L"Doctor Who", RuleMatch::Exact)}, first, {}, 1000, 100000);
                std::vector<Programme> later = {ep(L"CNN.us", L"Doctor Who", 90000, 91000, L"1.1.0/1", L"")};
                auto v = expandRules({rule(5, L"cnn.us", L"Doctor Who", RuleMatch::Exact)}, later, made, 1000, 100000);
                expect(made.size() == 1 && v.empty(),
                       "a later airing of an already-scheduled episode is not re-created");
            }
            {  // title-scoped: a Contains rule over two series does NOT cross-dedup on a shared num
                std::vector<Programme> ps = {ep(L"CNN.us", L"Star Trek: TNG", 2000, 3000, L"1.1.0/1", L""),
                                             ep(L"CNN.us", L"Star Trek: Voyager", 90000, 91000, L"1.1.0/1", L"")};
                auto v = expandRules({rule(7, L"cnn.us", L"Star Trek", RuleMatch::Contains)}, ps, {}, 1000, 100000);
                expect(v.size() == 2, "same episode-num on two different shows is not cross-deduped");
            }
            {  // a partial/blank xmltv_ns episode-num ("0 . . ") must NOT collapse distinct episodes:
               // the sub-title still separates them (regression guard for the combined num|sub-title key)
                std::vector<Programme> ps = {ep(L"CNN.us", L"Nova", 2000, 3000, L"0 . . ", L"The Deep"),
                                             ep(L"CNN.us", L"Nova", 90000, 91000, L"0 . . ", L"The Sky")};
                auto v = expandRules({rule(5, L"cnn.us", L"Nova", RuleMatch::Exact)}, ps, {}, 1000, 100000);
                expect(v.size() == 2, "blank-component episode-num defers to sub-title (distinct episodes kept)");
            }
            {  // whitespace-only episode-num likewise must not collapse -> the sub-title separates them
                std::vector<Programme> ps = {ep(L"CNN.us", L"Frontline", 2000, 3000, L"   ", L"Part 1"),
                                             ep(L"CNN.us", L"Frontline", 90000, 91000, L"   ", L"Part 2")};
                auto v = expandRules({rule(5, L"cnn.us", L"Frontline", RuleMatch::Exact)}, ps, {}, 1000, 100000);
                expect(v.size() == 2, "whitespace-only episode-num defers to sub-title");
            }
            {  // same partial num AND same sub-title == a real repeat -> still deduped to one
                std::vector<Programme> ps = {ep(L"CNN.us", L"Nova", 2000, 3000, L"0 . . ", L"The Deep"),
                                             ep(L"CNN.us", L"Nova", 90000, 91000, L"0 . . ", L"The Deep")};
                auto v = expandRules({rule(5, L"cnn.us", L"Nova", RuleMatch::Exact)}, ps, {}, 1000, 100000);
                expect(v.size() == 1, "same partial num + same sub-title still dedups a repeat");
            }
        }
        {  // degenerate inputs
            expect(expandRules({}, progs, {}, 1000, 100000).empty(), "no rules -> nothing");
            expect(expandRules({rule(1, L"cnn.us", L"News", RuleMatch::Exact)}, {}, {}, 1000, 100000).empty(),
                   "no programmes -> nothing");
            expect(expandRules({rule(1, L"cnn.us", L"News", RuleMatch::Exact)}, progs, {}, 100000, 1000).empty(),
                   "horizon before now -> nothing");
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

    out("\n== Dead-link check (beta) ==\n");
    {
        // The ONLY thing that matters here: a network failure must never look like a dead channel.
        expect(classifyProbe(ProbeTransport::NoConnect, 0) == ProbeOutcome::Inconclusive,
               "no connection is INCONCLUSIVE, never dead (offline laptop != dead library)");
        expect(classifyProbe(ProbeTransport::Timeout, 0) == ProbeOutcome::Inconclusive,
               "a timeout is inconclusive");
        expect(classifyProbe(ProbeTransport::Ok, 200) == ProbeOutcome::Alive, "200 -> alive");
        expect(classifyProbe(ProbeTransport::Ok, 206) == ProbeOutcome::Alive, "206 partial -> alive");
        expect(classifyProbe(ProbeTransport::Ok, 302) == ProbeOutcome::Alive,
               "a redirect is alive (CDN edge hand-off)");
        expect(classifyProbe(ProbeTransport::Ok, 404) == ProbeOutcome::Dead, "404 -> dead");
        expect(classifyProbe(ProbeTransport::Ok, 410) == ProbeOutcome::Dead, "410 gone -> dead");
        expect(classifyProbe(ProbeTransport::Ok, 403) == ProbeOutcome::Dead,
               "403 forbidden -> dead (geo-blocked is unusable from here)");
        expect(classifyProbe(ProbeTransport::Ok, 500) == ProbeOutcome::Inconclusive,
               "5xx is INCONCLUSIVE — an overloaded provider recovers");
        expect(classifyProbe(ProbeTransport::Ok, 599) == ProbeOutcome::Inconclusive,
               "an unrecognised status is inconclusive");

        // The whole-sweep guard: the thing standing between a dropped wifi link and a wiped library.
        expect(!sweepIsTrustworthy(0, 0, 50),
               "a sweep where NOTHING responded is discarded (the network was down)");
        expect(!sweepIsTrustworthy(1, 1, 50), "a sweep that barely responded is discarded");
        expect(sweepIsTrustworthy(40, 10, 50), "a sweep with a healthy response share is trusted");
        expect(!sweepIsTrustworthy(2, 0, 0), "too few probes to conclude anything");
        expect(sweepIsTrustworthy(5, 0, 0), "a small but fully-responding sweep is trusted");

        // TTL / resume.
        expect(needsProbe(0, 1000), "never-checked always needs a probe");
        expect(!needsProbe(1000, 1000 + kDeadLinkTtlSeconds - 1), "inside the TTL, skip");
        expect(needsProbe(1000, 1000 + kDeadLinkTtlSeconds), "past the TTL, re-probe");
        expect(needsProbe(5000, 1000), "a timestamp from the future is re-probed, not trusted");
    }
    out("\n== Glass mask (meter overlay) ==\n");
    {
        std::vector<uint8_t> add, mul;
        // Off must be a TRUE no-op — the renderer skips the pass entirely on these values.
        buildGlassMask(40, 20, GlassParams{0.0f}, add, mul);
        expect(add.size() == 800 && mul.size() == 800, "glass mask sized w*h");
        bool neutral = true;
        for (size_t i = 0; i < add.size(); ++i) neutral = neutral && add[i] == 0 && mul[i] == 255;
        expect(neutral, "strength 0 -> add=0 / mul=255 everywhere (no-op)");
        expect(glassIsNoop(GlassParams{0.0f}) && !glassIsNoop(GlassParams{0.5f}),
               "glassIsNoop tracks strength");

        buildGlassMask(40, 20, GlassParams{1.0f}, add, mul);
        // The framed-pane model: band 0 is the theme's border (untouched), band 1 is an opaque
        // bezel (lit top/left, dark bottom/right, one corner pip), band 2 is the dial with only
        // the bezel's cast shadow on it — and NO additive term anywhere.
        //
        // This REPLACES `expect(add[0] > 0, "top-left lip is lit")`: the outermost pixel is the
        // theme's FrameRect and the glass no longer touches it. That is the point — `add` is
        // achromatic, so brightening it would wash a brass or blue themed border toward neutral
        // and the meter would read as "the border got fatter and hotter".
        expect(add[0] == 0 && mul[0] == 255,
               "band 0 — the theme's own border pixel — is left bit-identical");
        bool band0Clean = true;
        for (int yy = 0; yy < 20; ++yy)
            for (int xx = 0; xx < 40; ++xx) {
                const int ring = std::min(std::min(yy, 19 - yy), std::min(xx, 39 - xx));
                if (ring != 0) continue;
                const size_t i = static_cast<size_t>(yy) * 40 + xx;
                band0Clean = band0Clean && add[i] == 0 && mul[i] == 255;
            }
        expect(band0Clean, "the whole outer ring is the theme's, all the way round");
        // ...and this REPLACES the old bottom-right lip check — the shading moved one ring inward.
        expect(mul[static_cast<size_t>(18) * 40 + 38] <= 30 &&
                   add[static_cast<size_t>(18) * 40 + 38] < 10,
               "bottom-right of the bezel is opaque and dark");
        const size_t mid = static_cast<size_t>(10) * 40 + 20;
        expect(add[mid] == 0 && mul[mid] == 255,
               "the centre of the pane is untouched (you read the dial through it)");
        // Crispness, stated as the property that actually matters: a clear central band survives.
        // (Counting "touched pixels" is the wrong test — on a 30px-tall meter a rim taken from both
        // edges legitimately covers much of the height; what must NOT happen is the two sides
        // meeting in the middle.)
        bool centreClear = true;
        for (int yy = 8; yy <= 11; ++yy)
            for (int xx = 8; xx <= 31; ++xx) {
                const size_t i = static_cast<size_t>(yy) * 40 + xx;
                centreClear = centreClear && add[i] == 0 && mul[i] == 255;
            }
        expect(centreClear, "a clear central band survives — the rim never meets in the middle");

        // The invariant that forecloses the "bright smear" failure mode for good.
        bool noAddOnDial = true;
        for (int yy = 0; yy < 20; ++yy)
            for (int xx = 0; xx < 40; ++xx) {
                const int ring = std::min(std::min(yy, 19 - yy), std::min(xx, 39 - xx));
                if (ring < 2) continue;                       // band 0 + band 1
                noAddOnDial = noAddOnDial && add[static_cast<size_t>(yy) * 40 + xx] == 0;
            }
        expect(noAddOnDial, "nothing brightens the dial — every additive term lives in the bezel");

        // The bezel is an OPAQUE, CLOSED ring exactly one band wide.
        bool ringClosed = true;
        for (int yy = 0; yy < 20; ++yy)
            for (int xx = 0; xx < 40; ++xx) {
                const int ring = std::min(std::min(yy, 19 - yy), std::min(xx, 39 - xx));
                if (ring != 1) continue;
                ringClosed = ringClosed && mul[static_cast<size_t>(yy) * 40 + xx] <= 30;
            }
        expect(ringClosed, "the bezel occludes — a frame in front, not a wash — on all four sides");

        // A thin BRIGHT top edge over a DARK bottom edge, one pixel each.
        expect(add[1 * 40 + 20] > 90 && add[18 * 40 + 20] < 10, "bright top lip, dark bottom lip");
        expect(add[10 * 40 + 1] > 40 && add[10 * 40 + 1] < add[1 * 40 + 20],
               "the left lip is lit but subordinate to the top — one light, from above-left");
        // One tight corner pip, and exactly ONE lit corner.
        expect(add[1 * 40 + 1] > add[1 * 40 + 20] + 10, "a pip where the two chamfers meet");
        expect(add[1 * 40 + 38] < 10 && add[18 * 40 + 1] < 10,
               "the other three corners are 1px mitres — the light has one direction");
        // The cast shadow, and only on the dial.
        expect(mul[2 * 40 + 20] < 200 && mul[2 * 40 + 20] > mul[1 * 40 + 20],
               "the bezel casts a shadow onto the dial, lighter than the bezel itself");
        expect(mul[2 * 40 + 20] < mul[17 * 40 + 20], "the shadow is deepest under the top rail");
        // No pixel is blown to white (the previous model's top-left computed 1.06 and clipped).
        uint8_t hiAdd = 0;
        for (uint8_t v : add) hiAdd = v > hiAdd ? v : hiAdd;
        expect(hiAdd < 200, "nothing clips to pure white — the corner blow-out is gone");

        // The frame is a machined part of ONE thickness, not a fraction of each axis. This is the
        // assertion that would have caught the aspect bug in the previous mask.
        buildGlassMask(200, 20, GlassParams{1.0f, 2}, add, mul);
        expect(mul[1 * 200 + 100] <= 30 && mul[2 * 200 + 100] > 150,
               "wide panel: a 1px bezel band on the SHORT axis");
        expect(mul[10 * 200 + 1] <= 30 && mul[10 * 200 + 2] > 150,
               "wide panel: the SAME 1px band on the LONG axis");
        buildGlassMask(20, 200, GlassParams{1.0f, 2}, add, mul);
        expect(mul[1 * 20 + 10] <= 30 && mul[10 * 20 + 1] <= 30,
               "tall panel: identical — no geometry is derived from an axis");

        // chromePx is honoured, so the frame lands on reserved chrome at every DPI.
        expect(glassChromePx(112, 30, 2) == 2 && glassChromePx(224, 60, 4) == 4 &&
                   glassChromePx(150, 86, 2) == 2,
               "the frame is exactly the chrome the renderer reserved — tray AND dialog preview");
        expect(glassChromePx(40, 20, 0) == 2 && glassChromePx(8, 8, 0) == 2,
               "with nothing reserved, the fallback still leaves room for a border + a bezel");
        expect(glassChromePx(3, 3, 2) == 0 && glassChromePx(4, 4, 2) == 1,
               "a pane too small for a frame drops the frame rather than eating itself");
        buildGlassMask(3, 3, GlassParams{1.0f, 2}, add, mul);
        bool tinyNeutral = true;
        for (size_t i = 0; i < add.size(); ++i)
            tinyNeutral = tinyNeutral && add[i] == 0 && mul[i] == 255;
        expect(add.size() == 9 && tinyNeutral, "3x3 -> no frame, no crash, no dial eaten");

        // The settings preview must show what the tray will ship.
        std::vector<uint8_t> addP, mulP;
        buildGlassMask(112, 30, GlassParams{1.0f, 2}, add, mul);
        buildGlassMask(150, 86, GlassParams{1.0f, 2}, addP, mulP);
        bool previewMatches = true;
        for (int yy = 0; yy < 6; ++yy)
            previewMatches =
                previewMatches &&
                add[static_cast<size_t>(yy) * 112 + 56] == addP[static_cast<size_t>(yy) * 150 + 75] &&
                mul[static_cast<size_t>(yy) * 112 + 56] == mulP[static_cast<size_t>(yy) * 150 + 75];
        expect(previewMatches,
               "the dp(86) Settings preview gets the same edge as the dp(30) tray meter");

        // The ring rides the one global knob, and 0 is still bit-exact off.
        buildGlassMask(112, 30, GlassParams{0.5f, 2}, add, mul);
        expect(add[1 * 112 + 56] > 0 && add[1 * 112 + 56] < 90 && mul[0] == 255,
               "half strength = a half-drawn frame, and band 0 is STILL the theme's");

        buildGlassMask(0, 0, GlassParams{1.0f}, add, mul);
        expect(add.empty() && mul.empty(), "degenerate size -> empty tables");
        buildGlassMask(8, 8, GlassParams{5.0f}, add, mul);
        expect(add.size() == 64 && mul.size() == 64, "out-of-range strength clamped, not fatal");
    }

    out("\n== VU lamp (dial illumination) ==\n");
    {
        auto luma = [](const VuLampRgb& c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; };
        // Exactly what drawVu does: clamp the ENDPOINT, then lerp toward the clamped value.
        auto faceAt = [&](const VuDial& d, int m) {
            const float t = m / 255.0f;
            auto ch = [&](float lo, float hi) { return lo + (std::min(255.0f, hi) - lo) * t; };
            return VuLampRgb{ch(d.faceDim.r, d.faceHot.r), ch(d.faceDim.g, d.faceHot.g),
                             ch(d.faceDim.b, d.faceHot.b)};
        };
        std::vector<uint8_t> m;

        // ---- the field is geometry: where the bulb is, and that it is a bulb ----
        buildVuLampMask(108, 26, m);
        expect(m.size() == 108u * 26u, "lamp mask sized w*h");
        buildVuLampMask(0, 26, m);
        expect(m.empty(), "a degenerate size yields an empty mask, not a crash");

        buildVuLampMask(108, 26, m);
        auto at = [&](int x, int y) { return static_cast<int>(m[static_cast<size_t>(y) * 108 + x]); };
        expect(at(45, 25) > at(45, 0),
               "the dial is lit from BELOW — the old face was brighter on top, which is a lamp "
               "ABOVE the dial and the single biggest reason it read as flat paper");
        int hottest = 0, hx = 0, hy = 0;
        for (int y = 0; y < 26; ++y)
            for (int x = 0; x < 108; ++x)
                if (at(x, y) > hottest) { hottest = at(x, y); hx = x; hy = y; }
        expect(hy == 25, "the brightest row is the bottom one — the bulb is behind the bottom rim");
        expect(hx > 35 && hx < 55, "...and left of centre, so the pool has a findable source");
        expect(at(2, 25) - at(105, 25) > 20,
               "the bottom edge varies ACROSS the dial — the term a vertical gradient cannot have, "
               "and the one that makes this read as a lamp rather than as a ramp");
        bool mono = true;
        for (int y = 0; y < 25; ++y) mono = mono && at(hx, y) <= at(hx, y + 1);
        expect(mono, "no ring or band: the lamp column rises monotonically to the bulb");
        // Same shape at every tray width AND in the settings preview — which is what lets the
        // preview be trusted for the LIGHTING (it still lies about pen widths, which have floors).
        std::vector<uint8_t> mn, mp;
        buildVuLampMask(54, 26, mn);
        buildVuLampMask(146, 82, mp);
        expect(std::abs(at(107, 0) - static_cast<int>(mn[53])) < 24 &&
                   std::abs(at(107, 0) - static_cast<int>(mp[145])) < 24,
               "the falloff is the same shape on 54px, 108px and the 146px settings preview");

        // ---- the guard on the lamp colour ----
        expect(vuLampIsUnset(22, 22, 24) && vuLampIsUnset(9, 11, 18) && vuLampIsUnset(26, 21, 16),
               "every dark skin's window background is rejected as a lamp");
        expect(vuLampIsUnset(0, 0, 0),
               "black means 'stock bulb' — the only way back once a colour picker has written a "
               "real RGB over the CLR_INVALID sentinel");
        expect(!vuLampIsUnset(60, 150, 255) && !vuLampIsUnset(255, 255, 255),
               "a real lamp colour is accepted");

        // ---- pigment x light ----
        const VuDial warm = vuDialColours(vuStockLamp());
        const VuDial blue = vuDialColours(vuLampFrom(60, 150, 255));
        const VuDial fallback = vuDialColours(vuLampFrom(22, 22, 24));
        expect(luma(faceAt(warm, 252)) > 228.0f && luma(faceAt(warm, 252)) < 234.0f,
               "the stock dial's hotspot still lands on the old flat face's brightest (229.8) — "
               "this reads as 'lit', not as 'the meters got turned down'");
        expect(luma(warm.faceHot) - luma(warm.faceDim) > 45.0f,
               "...over about twice the old face's 25 luma of modelling");
        expect(warm.faceDim.r > warm.faceDim.g && warm.faceDim.g > warm.faceDim.b,
               "the stock dial is still cream: R > G > B all the way down to the far rim");
        expect(luma(fallback.faceHot) == luma(warm.faceHot),
               "a background colour picked by mistake falls back to the stock bulb");
        expect(blue.faceHot.b > blue.faceHot.g && blue.faceHot.g > blue.faceHot.r,
               "a blue lamp makes a BLUE dial, not a blue tint smeared over cream");
        expect(blue.ink.b < 64.0f && luma(blue.ink) < luma(faceAt(blue, 77)) * 0.35f,
               "...with dark markings on it, as on the reference meter");
        // The property that makes the blue option legible with no second knob: ink and ground are
        // the same pigment under the same lamp, so their ratio is fixed by kInkFrac alone. It also
        // survives the glass overlay, which is a pure multiply on the dial.
        auto ratio = [&](const VuDial& d) { return luma(faceAt(d, 77)) / luma(d.ink); };
        expect(std::abs(ratio(warm) - ratio(blue)) < 0.1f &&
                   std::abs(ratio(warm) - ratio(vuDialColours(vuLampFrom(255, 255, 255)))) < 0.1f,
               "tick contrast is a ratio invariant — identical under warm, blue and white lamps");
        expect(ratio(warm) > 3.8f, "...and at least as punchy as the old hard-coded brown's 3.83:1");
        expect(std::abs(luma(vuDialColours(vuLampFrom(20, 50, 85)).faceHot) -
                        luma(vuDialColours(vuLampFrom(60, 150, 255)).faceHot)) < 3.0f,
               "a lamp's brightness is the model's business — only its hue comes from the swatch");

        // ---- the band the user actually READS ----
        // The hotspot assertion above is necessary but SELF-FLATTERING: mask 252 occurs only in the
        // bottom rows near x=0.42w — simultaneously the row furthest from every marking and the one
        // the glass bezel's shadow attacks. Pinning only that would let the dial go dull everywhere
        // that matters while the test stayed green. The arc and all seven ticks live between 0.14h
        // and 0.333h from the top, so this pins THAT band: the old flat gradient carried 224 there,
        // this model 188. That 16% is the price of the modelling, paid deliberately and in the open
        // — and it is the number to re-measure if kFaceLo is ever turned.
        buildVuLampMask(108, 26, m);
        double bandSum = 0.0;
        int bandN = 0;
        for (int y = static_cast<int>(0.14f * 26); y <= static_cast<int>(0.333f * 26); ++y)
            for (int x = 0; x < 108; ++x) {
                bandSum += luma(faceAt(warm, at(x, y)));
                ++bandN;
            }
        const float bandLuma = static_cast<float>(bandSum / bandN);
        expect(bandLuma > 180.0f && bandLuma < 196.0f,
               "the scale band — where the ticks actually are — sits near luma 188; the modelling "
               "is paid for HERE, not at the hotspot the assertions above pin");
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
        {   // v8 (VOD): the columns must land, and — the part that matters for 442 existing
            // users — a row written long before v8 must come back as an ordinary LIVE channel.
            // If kind ever defaulted to anything else, every channel in the library would
            // become a movie the moment the app was updated.
            const auto legacy = mdb.channelsByPlaylist(7);
            expect(!legacy.empty() && legacy[0].kind == Channel::Kind::Live,
                   "v8: a pre-v8 channel row migrates to kind=Live, not VOD");
            expect(!legacy.empty() && legacy[0].durationSec == 0 && legacy[0].resumeSec == 0 &&
                       !legacy[0].watched && legacy[0].addedAt == 0,
                   "v8: pre-v8 rows carry neutral VOD defaults");
            expect(!legacy.empty() && !legacy[0].isVod(), "v8: isVod() is false for a live row");
            sqlite3* rawv = nullptr;
            long long ver = 0;
            if (sqlite3_open16(mpath.c_str(), &rawv) == SQLITE_OK) {
                sqlite3_stmt* q = nullptr;
                if (sqlite3_prepare_v2(rawv, "PRAGMA user_version", -1, &q, nullptr) == SQLITE_OK &&
                    sqlite3_step(q) == SQLITE_ROW)
                    ver = sqlite3_column_int64(q, 0);
                if (q) sqlite3_finalize(q);
                sqlite3_close(rawv);
            }
            // 9, not 8: a v2 DB opened by a current build walks the whole chain, and v9 (the
            // stream_url canonicalisation) is the last step. That it reaches 9 is also the only
            // proof that canonicalizeStreamUrls() actually COMMITTED — it is the one migration
            // step that reports failure rather than being probed for structurally.
            expect(ver == 9, "v9: user_version advances to 9 after the v2 DB is migrated");
        }
        {   // --- VOD sync retirement. This DELETES rows, so it is pinned hard: the failure
            // mode is wiping somebody's library, and the live-TV list must be untouchable
            // by a VOD sync no matter what the provider returned.
            ParsedChannel live;  // a live channel in the same playlist, which must survive
            live.name = L"BBC One";
            live.streamUrl = L"http://o/live/1.ts";
            ParsedChannel m1, m2;
            m1.name = L"Film A"; m1.streamUrl = L"http://o/movie/u/p/1.mp4";
            m1.kind = Channel::Kind::Movie; m1.addedAt = 1742736240LL;
            m2.name = L"Film B"; m2.streamUrl = L"http://o/movie/u/p/2.mkv";
            m2.kind = Channel::Kind::Movie;
            expect(mdb.bulkInsertChannels(7, {live, m1, m2}, 4000) == 3,
                   "vod sync: live + 2 movies inserted");
            auto after = mdb.channelsByPlaylist(7);
            int movies = 0, lives = 0;
            long long addedSeen = 0;
            for (const auto& ch : after) {
                if (ch.kind == Channel::Kind::Movie) { ++movies; if (ch.name == L"Film A") addedSeen = ch.addedAt; }
                if (ch.kind == Channel::Kind::Live) ++lives;
            }
            expect(movies == 2, "vod sync: kind=Movie round-trips through the DAO");
            expect(addedSeen == 1742736240LL, "vod sync: added_at round-trips");

            // The provider dropped Film B. Only it should go.
            const int removed = mdb.retireMissingChannels(7, static_cast<int>(Channel::Kind::Movie),
                                                          {m1.streamUrl});
            expect(removed == 1, "vod sync: exactly the vanished movie is retired");
            after = mdb.channelsByPlaylist(7);
            bool haveA = false, haveB = false, haveLive = false, haveLegacy = false;
            for (const auto& ch : after) {
                if (ch.name == L"Film A") haveA = true;
                if (ch.name == L"Film B") haveB = true;
                if (ch.name == L"BBC One") haveLive = true;
                if (ch.name == L"Old") haveLegacy = true;  // the pre-v8 row from above
            }
            expect(haveA && !haveB, "vod sync: the surviving movie stays, the dropped one goes");
            expect(haveLive && haveLegacy,
                   "vod sync: LIVE channels are untouched — a VOD sync can never wipe the TV list");

            // *** The disaster guard. A failed or empty fetch must delete NOTHING; the
            // dead-link checker learned the same lesson (discard a sweep that reached no
            // server) after a bad verdict could make a library vanish. ***
            expect(mdb.retireMissingChannels(7, static_cast<int>(Channel::Kind::Movie), {}) == 0,
                   "vod sync: an EMPTY keep-set retires nothing (a failed fetch cannot wipe a library)");
            // 0 and -1 mean DIFFERENT things, and the difference is the whole safety story: 0 is
            // "the provider dropped nothing", -1 is "the retirement failed and rolled back". A
            // caller that cannot tell them apart reports a clean sync over a library that will
            // never converge — the "looks like it worked" failure Database.h warns about.
            {
                Database closed;  // never opened
                expect(closed.retireMissingChannels(7, static_cast<int>(Channel::Kind::Movie),
                                                    {L"http://o/movie/1.mp4"}) == -1,
                       "vod sync: a FAILED retirement returns -1, not a plausible 0");
            }
            after = mdb.channelsByPlaylist(7);
            int stillMovies = 0;
            for (const auto& ch : after)
                if (ch.kind == Channel::Kind::Movie) ++stillMovies;
            expect(stillMovies == 1, "vod sync: ...and the movie really is still there");
            // Retiring the OTHER kind must not touch movies. Assert the EXACT count, not
            // ">= 0" — sqlite3_changes is never negative, so that form could not fail. This
            // retires the pre-v8 "Old" row (kind=0, not in the keep set), which is correct
            // and is re-checked below.
            expect(mdb.retireMissingChannels(7, static_cast<int>(Channel::Kind::Live),
                                             {L"http://o/live/1.ts"}) == 1,
                   "vod sync: a live retirement removes exactly the one stale live row");
            after = mdb.channelsByPlaylist(7);
            bool movieSurvived = false, keptLive = false, oldGone = true;
            for (const auto& ch : after) {
                if (ch.name == L"Film A") movieSurvived = true;
                if (ch.name == L"BBC One") keptLive = true;
                if (ch.name == L"Old") oldGone = false;
            }
            expect(movieSurvived, "vod sync: a LIVE retirement leaves movies alone (kind scoping works)");
            expect(keptLive && oldGone, "vod sync: ...and retires exactly the live row not kept");

            // *** A partially-staged keep set must delete NOTHING. The keep-URL inserts are
            // now checked per row: a failure aborts and rolls back, because a URL that failed
            // to stage is indistinguishable from one the provider dropped — and silently
            // deleting the difference is how a library gets wiped. ***
            std::vector<std::wstring> dupes{m1.streamUrl, m1.streamUrl, m1.streamUrl};
            expect(mdb.retireMissingChannels(7, static_cast<int>(Channel::Kind::Movie), dupes) == 0,
                   "vod sync: duplicate keep-URLs stage cleanly (INSERT OR IGNORE) and retire nothing");
            // channel_count must not go stale after a retire — a sync runs insert-then-retire.
            long long counted = 0;
            for (const auto& pl : mdb.listPlaylists())
                if (pl.id == 7) counted = pl.channelCount;
            expect(counted == static_cast<long long>(mdb.channelsByPlaylist(7).size()),
                   "vod sync: playlists.channel_count is recomputed after a retirement");

            // *** An M3U refresh must NOT revert a movie to a live channel. ParsedChannel from
            // the M3U parser cannot express `kind`, so treating "the M3U didn't say" as "it is
            // Live" would zero added_at, hide every film, and leave retireMissingChannels with
            // no kind=1 rows to act on — a permanent no-op, library grows forever. ***
            // *** VOD must be invisible to the COUNTRY views. A category named like a country
            // prefix ("NL - FILMS") would otherwise file every film under the Netherlands and
            // swamp the country tree — a correctness failure, not just a slow query. Pinned
            // here because the deny-list approach can never keep up with category naming. ***
            ParsedChannel trap;
            trap.name = L"Trap Film";
            trap.streamUrl = L"http://o/movie/u/p/999.mp4";
            trap.groupTitle = L"NL - FILMS";  // parses as Netherlands under the group-title rule
            trap.kind = Channel::Kind::Movie;
            ParsedChannel trapLive;  // the same prefix on a LIVE row must still work
            trapLive.name = L"Trap Live";
            trapLive.streamUrl = L"http://o/live/nl.ts";
            trapLive.groupTitle = L"NL - NIEUWS";
            expect(mdb.bulkInsertChannels(7, {trap, trapLive}, 6000) == 2, "vod sync: trap rows inserted");
            bool trapFilmInCountry = false, trapLiveInCountry = false;
            for (const auto& ch : mdb.channelsByCountry(L"nl")) {
                if (ch.name == L"Trap Film") trapFilmInCountry = true;
                if (ch.name == L"Trap Live") trapLiveInCountry = true;
            }
            expect(!trapFilmInCountry,
                   "vod sync: a movie in an 'NL - …' category does NOT appear under Netherlands");
            expect(trapLiveInCountry,
                   "vod sync: ...while a LIVE channel with the same prefix still does");

            // *** The "Movies root" contract: the live tree must not gain VOD categories,
            // and the Movies tree must not gain live groups. Two separate namespaces. ***
            {
                const auto liveGroups = mdb.listGroups();
                const auto vodGroups = mdb.listVodGroups();
                auto has = [](const std::vector<std::wstring>& v, const std::wstring& s) {
                    return std::find(v.begin(), v.end(), s) != v.end();
                };
                expect(!has(liveGroups, L"NL - FILMS"),
                       "movies root: a VOD category does NOT appear in the LIVE group tree");
                expect(has(vodGroups, L"NL - FILMS"),
                       "movies root: ...it appears under Movies instead");
                expect(has(liveGroups, L"NL - NIEUWS") && !has(vodGroups, L"NL - NIEUWS"),
                       "movies root: a LIVE group stays live-only");
                // Same NAME on both sides must not cross-contaminate the row lists.
                expect(mdb.channelsByGroup(L"NL - FILMS").empty(),
                       "movies root: channelsByGroup finds no live rows in a VOD-only category");
                expect(!mdb.moviesByGroup(L"NL - FILMS").empty(),
                       "movies root: moviesByGroup does find them");
                expect(mdb.moviesByGroup(L"NL - NIEUWS").empty(),
                       "movies root: moviesByGroup finds no movies in a live-only group");
                bool allAreMovies = !mdb.allMovies().empty();
                for (const auto& ch : mdb.allMovies())
                    if (ch.kind != Channel::Kind::Movie) allAreMovies = false;
                expect(allAreMovies, "movies root: allMovies() returns only kind=Movie rows");
            }

            // *** v9: the canonicalising WRITE path + the dedupe MERGE. The owner's real library
            // had zero collisions carrying user state, so the merge ships having never met a real
            // conflict — which is exactly why it is pinned here with a fixture where the row that
            // LOSES carries every piece of user data there is. ***
            {
                const std::wstring vpath = std::wstring(tmp) + L"rabbitears_v9merge.db";
                DeleteFileW(vpath.c_str());
                Database vdb;
                expect(vdb.open(vpath), "v9: fixture DB opens");
                const long long vp = vdb.addPlaylist(L"P", L"http://h:80/get.php?username=u&password=p",
                                                     true, 1000);
                // The m3u spelling (with :80) and the sync spelling (without) of ONE film. Insert
                // them as two rows the way the real bug did — by writing the literal URLs before
                // canonicalisation is in force is impossible here (the DB is already v9), so drive
                // the merge through migrate() instead: write both, then re-open.
                ParsedChannel a;  // "m3u" copy — carries ALL the user state
                a.name = L"Film"; a.streamUrl = L"http://h:80/movie/u/p/7.mp4";
                a.groupTitle = L"VOD";
                ParsedChannel b;  // "sync" copy — canonical, kind=Movie, has added_at
                b.name = L"Film"; b.streamUrl = L"http://h/movie/u/p/7.mp4";
                b.groupTitle = L"VOD"; b.kind = Channel::Kind::Movie; b.addedAt = 4242;
                // With v9 live, BOTH canonicalise on insert and collide immediately — which is
                // itself the property that stops the bug recurring.
                vdb.bulkInsertChannels(vp, {a, b}, 1000);
                const auto merged = vdb.channelsByPlaylist(vp);
                expect(merged.size() == 1,
                       "v9: the two spellings of one film insert as ONE row, not two");
                expect(!merged.empty() && merged[0].streamUrl == L"http://h/movie/u/p/7.mp4",
                       "v9: ...stored under the canonical spelling");
                expect(!merged.empty() && merged[0].kind == Channel::Kind::Movie,
                       "v9: ...and the upsert keeps kind=Movie rather than reverting to Live");

                // The retire path must speak the SAME spelling. This is the case that would
                // otherwise delete an entire movie library: the keep-set is built from the raw
                // constructed URL, which for a :80 origin is NOT what got stored.
                const int gone = vdb.retireMissingChannels(
                    vp, static_cast<int>(Channel::Kind::Movie), {L"http://h:80/movie/u/p/7.mp4"});
                expect(gone == 0,
                       "v9: a keep-set in the :80 spelling still matches the stored canonical row "
                       "(else a sync retires everything it just inserted)");
                expect(vdb.channelsByPlaylist(vp).size() == 1, "v9: ...the film is still there");
            }

            // *** URL canonicalisation — step 1 of the duplicate-movies fix (BACKLOG). The tests
            // that matter here are the NEGATIVE ones. Stripping a default port is worth one
            // duplicate row if it fails to fire; stripping anything else REPOINTS EVERY STREAM IN
            // THE LIBRARY at a different endpoint, silently, and the user finds out when nothing
            // plays. So every "leave it alone" case below is load-bearing. ***
            {
                auto same = [](const wchar_t* in, const wchar_t* want) {
                    return canonicalStreamUrl(in) == std::wstring(want);
                };
                // The measured real-world case: the owner's m3u writes :80, the VOD sync does not.
                expect(same(L"http://line.example.ru:80/movie/u/p/1218804.mp4",
                            L"http://line.example.ru/movie/u/p/1218804.mp4"),
                       "urlcanon: an explicit :80 on http is stripped");
                expect(canonicalStreamUrl(L"http://line.example.ru:80/movie/u/p/1218804.mp4") ==
                           canonicalStreamUrl(L"http://line.example.ru/movie/u/p/1218804.mp4"),
                       "urlcanon: ...so the m3u and the sync spelling converge on ONE dedupe key");
                expect(same(L"https://h/p", L"https://h/p"), "urlcanon: no port is already canonical");
                expect(same(L"https://h:443/p", L"https://h/p"), "urlcanon: :443 on https is stripped");

                // --- the dangerous half: everything that must survive untouched ---
                expect(same(L"http://h:8080/p", L"http://h:8080/p"),
                       "urlcanon: a NON-default port is preserved (stripping it repoints the stream)");
                expect(same(L"https://h:80/p", L"https://h:80/p"),
                       "urlcanon: :80 is NOT default for https, so it stays");
                expect(same(L"http://h:443/p", L"http://h:443/p"),
                       "urlcanon: :443 is NOT default for http, so it stays");
                expect(same(L"rtsp://h:554/p", L"rtsp://h:554/p"),
                       "urlcanon: a scheme we do not know the default for is never touched");
                expect(same(L"udp://@239.0.0.1:1234", L"udp://@239.0.0.1:1234"),
                       "urlcanon: multicast udp is left exactly as-is");
                expect(same(L"http://h:8/p", L"http://h:8/p") &&
                           same(L"http://h:800/p", L"http://h:800/p"),
                       "urlcanon: a port that merely resembles the default is preserved");
                expect(same(L"http://h:/p", L"http://h:/p"),
                       "urlcanon: an EMPTY port is left alone rather than guessed at");

                // --- authority parsing: the two places a naive 'find the colon' goes wrong ---
                expect(same(L"http://u:pw@h/p", L"http://u:pw@h/p"),
                       "urlcanon: a colon inside USERINFO is not a port");
                expect(same(L"http://u:pw@h:80/p", L"http://u:pw@h/p"),
                       "urlcanon: ...and the real port after it still strips, credentials intact");
                expect(same(L"http://[::1]/p", L"http://[::1]/p"),
                       "urlcanon: the colons in an IPv6 literal are not a port");
                expect(same(L"http://[::1]:80/p", L"http://[::1]/p"),
                       "urlcanon: ...but a port after the closing bracket is");

                // --- shape preservation ---
                expect(same(L"HTTP://h:80/p", L"HTTP://h/p"),
                       "urlcanon: the scheme matches case-insensitively but is not rewritten");
                expect(same(L"http://h:80", L"http://h"), "urlcanon: authority-only URL, no path");
                expect(same(L"http://h:80/p?a=b#f", L"http://h/p?a=b#f"),
                       "urlcanon: query and fragment survive verbatim");
                expect(same(L"http://h:80/PaTh/Mixed.MP4", L"http://h/PaTh/Mixed.MP4"),
                       "urlcanon: path case is NOT normalised (paths are case-sensitive)");
                expect(same(L"", L"") && same(L"not a url", L"not a url") &&
                           same(L"http:///p", L"http:///p"),
                       "urlcanon: empty, scheme-less and authority-less input come back untouched");

                // Idempotent, because the migration will run it over rows that may already be
                // canonical and a second pass must be a no-op.
                for (const wchar_t* u : {L"http://h:80/p", L"http://h:8080/p", L"http://[::1]:80/p",
                                         L"https://h:443/p", L"rtsp://h:554/p"})
                    expect(canonicalStreamUrl(canonicalStreamUrl(u)) == canonicalStreamUrl(u),
                           "urlcanon: canonicalising twice changes nothing");
            }

            ParsedChannel refetch;  // exactly what parseM3u would produce for the same URL
            refetch.name = L"Film A";
            refetch.streamUrl = m1.streamUrl;
            expect(mdb.bulkInsertChannels(7, {refetch}, 5000) == 1, "vod sync: m3u refresh applies");
            after = mdb.channelsByPlaylist(7);
            for (const auto& ch : after)
                if (ch.name == L"Film A") {
                    expect(ch.kind == Channel::Kind::Movie,
                           "vod sync: an M3U refresh does NOT revert a movie to kind=Live");
                    expect(ch.addedAt == 1742736240LL,
                           "vod sync: ...nor zero the provider's added_at");
                }
        }
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
        RecordingRule mr;
        mr.titleMatch = L"News";
        mr.createdAt = 1000;
        expect(mdb.addRule(mr) > 0, "recording_rules table added by migration (v5)");
        expect(mdb.listSchedules().size() == 1 && mdb.listSchedules()[0].ruleId == 0,
               "rule_id column added by migration; pre-v5 rows read back as 0");
        ScheduledRecording ek;
        ek.channelName = L"Ch";
        ek.streamUrl = L"http://c";
        ek.startUtc = 5;
        ek.stopUtc = 6;
        ek.episodeKey = L"n:1.1.0/1";
        expect(mdb.addSchedule(ek) > 0, "scheduled_recordings.episode_key added by migration (v6)");
        bool ekRoundTrip = false;
        for (const auto& s : mdb.listSchedules())
            if (s.episodeKey == L"n:1.1.0/1") ekRoundTrip = true;
        expect(ekRoundTrip, "episode_key round-trips through the schedule DAO");

        // Rule editor DAO: updateRule overwrites the editable fields (the "Edit…" path).
        auto rules0 = mdb.listRules();
        expect(rules0.size() == 1, "one rule present before edit");
        RecordingRule ur = rules0[0];
        ur.titleMatch = L"Edited";
        ur.match = RuleMatch::Contains;
        ur.channelId = L"bbc.uk";
        ur.channelName = L"BBC";
        ur.leadSec = 120;
        ur.trailSec = 300;
        ur.enabled = false;
        mdb.updateRule(ur);
        auto rules1 = mdb.listRules();
        expect(rules1.size() == 1 && rules1[0].titleMatch == L"Edited" &&
                   rules1[0].match == RuleMatch::Contains && rules1[0].channelId == L"bbc.uk" &&
                   rules1[0].leadSec == 120 && rules1[0].trailSec == 300 && !rules1[0].enabled,
               "updateRule overwrites titleMatch/match/channel/padding/enabled");
        // clearPendingForRule drops a rule's Pending schedule but keeps its history.
        const long long rid = rules1[0].id;
        ScheduledRecording sp;
        sp.channelName = L"c";
        sp.streamUrl = L"u";
        sp.startUtc = 10;
        sp.stopUtc = 20;
        sp.ruleId = rid;
        sp.status = ScheduleStatus::Pending;
        mdb.addSchedule(sp);
        ScheduledRecording sd = sp;
        sd.startUtc = 30;
        sd.stopUtc = 40;
        sd.status = ScheduleStatus::Done;
        mdb.addSchedule(sd);
        mdb.clearPendingForRule(rid);
        int pend = 0, done = 0;
        for (const auto& s : mdb.listSchedules()) {
            if (s.ruleId != rid) continue;
            if (s.status == ScheduleStatus::Pending) ++pend;
            if (s.status == ScheduleStatus::Done) ++done;
        }
        expect(pend == 0 && done == 1, "clearPendingForRule drops Pending rows, keeps history");
    }
    {  // v5 -> v6 specifically: a DB from 0.2.7/0.2.8 (user_version=5, scheduled_recordings WITHOUT
       // episode_key) must still gain the column. Guards the migration early-return bound — a stale
       // `if (v >= 5) return` skipped v6, making every schedule query fail on the missing column.
       // The v2 test above can't catch this (2 < 5 so it never early-returns).
        const std::wstring p5 = std::wstring(tmp) + L"rabbitears_migrate_v5.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) DeleteFileW((p5 + sfx).c_str());
        {
            sqlite3* raw = nullptr;
            sqlite3_open(utf8FromWide(p5).c_str(), &raw);
            sqlite3_exec(
                raw,
                "CREATE TABLE playlists(id INTEGER PRIMARY KEY, name TEXT NOT NULL, source_url TEXT,"
                " source_path TEXT, is_url INTEGER NOT NULL DEFAULT 1, added_at INTEGER NOT NULL,"
                " last_refreshed_at INTEGER, channel_count INTEGER NOT NULL DEFAULT 0,"
                " enabled INTEGER NOT NULL DEFAULT 1, epg_url TEXT);"
                "CREATE TABLE channels(id INTEGER PRIMARY KEY, playlist_id INTEGER NOT NULL"
                " REFERENCES playlists(id) ON DELETE CASCADE, name TEXT NOT NULL,"
                " stream_url TEXT NOT NULL, logo_url TEXT, group_title TEXT, tvg_id TEXT,"
                " tvg_name TEXT, lcn INTEGER, is_favourite INTEGER NOT NULL DEFAULT 0,"
                " dead_status INTEGER NOT NULL DEFAULT 0, last_checked_at INTEGER NOT NULL DEFAULT 0,"
                " sort_order INTEGER NOT NULL DEFAULT 0, user_agent TEXT, referrer TEXT);"
                "CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT);"
                "CREATE TABLE scheduled_recordings(id INTEGER PRIMARY KEY, channel_id TEXT,"
                " channel_name TEXT NOT NULL, stream_url TEXT NOT NULL, user_agent TEXT, referrer TEXT,"
                " title TEXT, start_utc INTEGER NOT NULL, stop_utc INTEGER NOT NULL,"
                " mux TEXT NOT NULL DEFAULT 'ts', status INTEGER NOT NULL DEFAULT 0, file_path TEXT,"
                " created_at INTEGER NOT NULL, rule_id INTEGER);"  // v5 shape — NO episode_key (that is v6)
                "CREATE TABLE recording_rules(id INTEGER PRIMARY KEY, channel_id TEXT, channel_name TEXT,"
                " title_match TEXT NOT NULL, match_kind INTEGER NOT NULL DEFAULT 0,"
                " enabled INTEGER NOT NULL DEFAULT 1, lead_sec INTEGER NOT NULL DEFAULT 0,"
                " trail_sec INTEGER NOT NULL DEFAULT 0, mux TEXT NOT NULL DEFAULT 'ts',"
                " created_at INTEGER NOT NULL);"
                "PRAGMA user_version=5;",
                nullptr, nullptr, nullptr);
            sqlite3_close(raw);
        }
        Database v5db;
        std::wstring e5;
        expect(v5db.open(p5, &e5),
               "v5 DB opens + migrates" + (e5.empty() ? "" : " (" + utf8FromWide(e5) + ")"));
        ScheduledRecording s6;
        s6.channelName = L"C";
        s6.streamUrl = L"u";
        s6.startUtc = 1;
        s6.stopUtc = 2;
        s6.episodeKey = L"n:1.1|s:x";
        expect(v5db.addSchedule(s6) > 0,
               "v5 DB gains episode_key on upgrade (a stale v>=5 guard would skip the v6 ALTER)");
        bool got6 = false;
        for (const auto& s : v5db.listSchedules())
            if (s.episodeKey == L"n:1.1|s:x") got6 = true;
        expect(got6, "episode_key present + round-trips after a v5->v6 upgrade");
    }

    out("== i18n string catalog (all shipped languages) ==\n");
    {
        using namespace i18n;
        StringId missing = StringId::Count;
        expect(catalogIsComplete(&missing),
               "every StringId is non-empty in every shipped language");
        // Placeholder parity: every language must carry the same number of {n}/%d/%s tokens per key
        // as English, or trf()/swprintf break at runtime (a translator dropping a {0} or %d). The
        // completeness check above already spans ALL languages; list each non-reference language here
        // so its placeholder parity + a distinct-translation spot-check run too (add one when wiring
        // a new language).
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
                   std::string("placeholder tokens match between en and ") + lc.name + " for every key");
            if (mismatches) out("  first parity mismatch at StringId #" +
                                std::to_string(static_cast<int>(firstBad)) + "\n");
            // The catalog is a fixed-size array indexed by the enum, so a real lookup must be non-null
            // and this language's text must actually differ from English for a translated key.
            expect(std::string(trU8(StringId::ButtonCancel, Lang::En)) !=
                       std::string(trU8(StringId::ButtonCancel, lc.lang)),
                   std::string("en and ") + lc.name + " differ for a translated key (Cancel)");
        }
        // Active-language selection must actually switch the returned bytes.
        setActiveLang(Lang::Ja);
        expect(trU8(StringId::ButtonCancel) != nullptr && trU8(StringId::ButtonCancel)[0] != '\0',
               "active-language lookup returns a real string");
        setActiveLang(Lang::En);  // leave the process back on English for any later test
    }

    out("\n== JSON reader (Xtream player_api) ==\n");
    {
        // The tolerance below is not "being nice to bad input" — it is the documented shape
        // of real Xtream panels, which quote numbers inconsistently, omit fields, and answer
        // with a season-keyed MAP where an array would be natural. A strict reader would
        // hard-fail on providers we cannot test against.
        JsonValue v;
        std::string jerr;

        {   // numbers-as-strings, the flagged provider risk, read transparently either way
            const std::string j =
                "{\"a\":1234,\"b\":\"1234\",\"c\":\"7.8\",\"d\":7.8,\"e\":\"\",\"f\":null,"
                "\"g\":\"abc\",\"h\":\"8.8 / 10\"}";
            expect(parseJson(j, v, &jerr), "json: mixed-typing fixture parses (" + jerr + ")");
            expect(v["a"].asInt64() == 1234 && v["b"].asInt64() == 1234,
                   "json: an int reads the same quoted or bare");
            expect(v["c"].asDouble() > 7.79 && v["c"].asDouble() < 7.81,
                   "json: a quoted float reads as a number");
            expect(v["c"].asInt64() == 7, "json: a quoted float truncates to int like a bare one");
            expect(v["b"].isNumericString() && !v["a"].isNumericString(),
                   "json: isNumericString distinguishes the two spellings");
            expect(v["g"].asInt64(-1) == -1 && v["h"].asInt64(-1) == -1,
                   "json: a non-numeric string does NOT masquerade as a number");
            expect(v["e"].asString().empty() && v["f"].isNull(),
                   "json: empty string and null are distinct and both readable");
            expect(v["nope"].isNull() && v["nope"]["deeper"].isNull(),
                   "json: a missing key chains safely instead of faulting");
            expect(v["a"].asString() == "1234",
                   "json: a number renders from its ORIGINAL token, not a reformatted double");
        }
        {   // Numeric overflow must FAIL, not saturate. A saturated stream_id passes the
            // caller's `id > 0` test, escapes the skip counter, and becomes an ordinary
            // movie row whose URL 404s — "obviously wrong wherever it surfaces" was not true.
            JsonValue o;
            expect(parseJson("{\"a\":\"99999999999999999999\",\"b\":\"99999999999999999999abc\"}",
                             o, &jerr) && o["a"].asInt64(-1) == -1 && o["b"].asInt64(-1) == -1,
                   "json: an integer too large for 64 bits FAILS rather than saturating");
            // double -> long long out of range is UNDEFINED, and the two architectures
            // RabbitEars ships DISAGREE: x64 yields LLONG_MIN (rejected by an `id > 0` test),
            // ARM64 saturates to LLONG_MAX (accepted -> a bogus row). Range-check, don't cast.
            expect(parseJson("{\"a\":1e300,\"b\":\"1e30\"}", o, &jerr) &&
                       o["a"].asInt64(-1) == -1 && o["b"].asInt64(-1) == -1,
                   "json: an out-of-range double yields the default on BOTH architectures");
        }
        {   // 64-bit ids must survive exactly — a double would round them
            const std::string j = "{\"id\":9007199254740993,\"s\":\"9007199254740993\"}";
            expect(parseJson(j, v, &jerr), "json: 64-bit id fixture parses");
            expect(v["id"].asInt64() == 9007199254740993LL,
                   "json: a >2^53 integer survives exactly (a double would round it)");
            expect(v["s"].asInt64() == 9007199254740993LL, "json: ...quoted too");
        }
        {   // the season-keyed episodes map: the keys carry data, so order/keys must survive
            const std::string j =
                "{\"episodes\":{\"1\":[{\"id\":\"11\"}],\"2\":[{\"id\":\"21\"},{\"id\":\"22\"}]}}";
            expect(parseJson(j, v, &jerr), "json: season-keyed episodes map parses");
            const JsonValue& eps = v["episodes"];
            expect(eps.isObject() && eps.size() == 2, "json: episodes is a 2-member object");
            expect(eps.members()[0].first == "1" && eps.members()[1].first == "2",
                   "json: season keys are preserved in document order (they ARE the season number)");
            expect(eps["2"].size() == 2 && eps["2"][1]["id"].asInt64() == 22,
                   "json: episodes nest and index correctly");
        }
        {   // structure is strict — a truncated download must NOT look like a short library
            expect(!parseJson("[{\"a\":1},", v, &jerr), "json: a truncated body fails");
            expect(jerr.find("byte") != std::string::npos,
                   "json: the error names a byte offset (" + jerr + ")");
            expect(!parseJson("<html>error</html>", v, &jerr), "json: an HTML error page fails");
            expect(!parseJson("[1,2]<br />Fatal error", v, &jerr),
                   "json: trailing junk after the root FAILS (a PHP notice is a server fault)");
            expect(!parseJson("", v, &jerr), "json: an empty body fails");
            expect(!parseJson("[-]", v, &jerr), "json: '-' with no digits is not a number");
            expect(parseJson("[]", v, &jerr) && v.isArray() && v.size() == 0,
                   "json: an empty array is valid and distinguishable from a failure");
            std::string deep(200, '[');
            expect(!parseJson(deep, v, &jerr), "json: a nesting bomb is refused, not a stack overflow");
        }
        {   // encodings a panel really emits
            expect(parseJson("\xEF\xBB\xBF{\"a\":1}", v, &jerr) && v["a"].asInt64() == 1,
                   "json: a UTF-8 BOM is accepted (a BOM'd PHP file is misconfigured, not broken)");
            expect(parseJson("[\"Ar\\u00e8s\"]", v, &jerr) &&
                       v[0].asString().find("\xC3\xA8") != std::string::npos,
                   "json: \\u escapes decode to UTF-8");
            expect(parseJson("[\"\\ud83c\\udfac\"]", v, &jerr) && v[0].asString().size() == 4,
                   "json: a surrogate PAIR becomes one 4-byte UTF-8 sequence");
            expect(parseJson("[\"\\ud800x\"]", v, &jerr) &&
                       v[0].asString().find("\xEF\xBF\xBD") != std::string::npos,
                   "json: a LONE surrogate becomes U+FFFD, not invalid UTF-8 (SQLite would reject it)");
            expect(parseJson("{\"a\":1,\"a\":2}", v, &jerr) && v["a"].asInt64() == 2,
                   "json: a duplicate key resolves last-wins, as mainstream parsers do");
        }
        {   // flags: panels spell booleans four ways
            const std::string j = "{\"p\":1,\"q\":\"1\",\"r\":0,\"s\":\"0\",\"t\":true,\"u\":\"\"}";
            expect(parseJson(j, v, &jerr), "json: flag fixture parses");
            expect(v["p"].asBool() && v["q"].asBool() && v["t"].asBool(),
                   "json: 1, \"1\" and true all read true");
            expect(!v["r"].asBool() && !v["s"].asBool() && !v["u"].asBool(),
                   "json: 0, \"0\" and \"\" all read false (this is user_info.auth)");
        }
    }

    out("\n== Xtream client (player_api -> models) ==\n");
    {
        // Fixtures are cut from what the owner's REAL provider returned on 2026-07-27
        // (Win32/docs/XTREAM_VOD.md §1) — including the shapes that would break a strict
        // client: a field quoted on some rows and bare on others, a null, and an absent
        // container_extension.
        XtreamCreds cr;
        expect(parseXtreamPlaylistUrl(
                   L"http://line.example.com/get.php?username=abc&password=def&type=m3u_plus", cr),
               "xtream: credentials parsed from a get.php playlist URL");
        expect(cr.origin == L"http://line.example.com" && cr.username == L"abc" &&
                   cr.password == L"def",
               "xtream: origin/username/password split correctly");
        XtreamCreds bad;
        expect(!parseXtreamPlaylistUrl(L"http://host/playlist.m3u", bad),
               "xtream: a plain .m3u URL is NOT an Xtream playlist (a legitimate answer)");
        // Scheme-less forms. The earlier version of this test passed only because its fixture
        // had no "//" at all — these are the shapes that actually slipped through.
        expect(!parseXtreamPlaylistUrl(L"line.example.com/get.php?username=a&password=b", bad),
               "xtream: a URL with no scheme is rejected");
        expect(!parseXtreamPlaylistUrl(L"host//x/get.php?username=a&password=b", bad),
               "xtream: a bare '//' is not a scheme");
        expect(!parseXtreamPlaylistUrl(L"//host/get.php?username=a&password=b", bad),
               "xtream: a protocol-relative URL is rejected");
        // *** No path at all: the authority must end at '?', or the whole query is swallowed
        // into the origin and every constructed URL becomes nonsense. ***
        XtreamCreds noPath;
        expect(parseXtreamPlaylistUrl(L"http://host?username=a&password=bb", noPath) &&
                   noPath.origin == L"http://host",
               "xtream: the origin terminates at '?' when the URL has no path");
        expect(xtreamMovieUrl(noPath, 12, L"mp4") == L"http://host/movie/a/bb/12.mp4",
               "xtream: ...so the play URL is still well-formed");

        // *** Credentials cross from a QUERY position to a PATH position. '+' means space in
        // a query and a literal '+' in a path, so carrying the raw spelling made the API URL
        // right and the play URL silently wrong — and the auth probe would still succeed. ***
        XtreamCreds enc;
        expect(parseXtreamPlaylistUrl(L"http://h/get.php?username=a+b&password=p%2Fq", enc),
               "xtream: a %-encoded credential parses");
        expect(enc.username == L"a b" && enc.password == L"p/q",
               "xtream: credentials are stored DECODED ('+' is a space, %2F is '/')");
        expect(xtreamApiUrl(enc).find(L"username=a%20b&password=p%2Fq") != std::wstring::npos,
               "xtream: re-encoded for a QUERY position");
        expect(xtreamMovieUrl(enc, 7, L"mp4") == L"http://h/movie/a%20b/p%2Fq/7.mp4",
               "xtream: and re-encoded for a PATH position, so both URLs are right");

        expect(xtreamApiUrl(cr) == L"http://line.example.com/player_api.php?username=abc&password=def",
               "xtream: the auth URL takes no action verb");
        expect(xtreamApiUrl(cr, L"get_vod_streams").find(L"&action=get_vod_streams") !=
                   std::wstring::npos,
               "xtream: an action verb is appended");
        expect(xtreamMovieUrl(cr, 1218804, L"mp4") ==
                   L"http://line.example.com/movie/abc/def/1218804.mp4",
               "xtream: the movie play URL matches the form verified reachable (HTTP 302)");
        expect(xtreamEpisodeUrl(cr, 1306481, L"mkv") ==
                   L"http://line.example.com/series/abc/def/1306481.mkv",
               "xtream: the episode play URL uses /series/");
        // *** A guessed suffix would 404 and read as "VOD is broken" when the truth is
        // "this panel did not tell us the container". Those must stay distinguishable. ***
        expect(xtreamMovieUrl(cr, 1218804, L"").empty(),
               "xtream: NO url is built when container_extension is missing (never guess)");
        expect(xtreamMovieUrl(cr, 0, L"mp4").empty(), "xtream: no url without a stream id");
        // .empty() alone let junk into the play URL, producing exactly the 404-that-reads-as-
        // broken this design promises to avoid — and without counting it.
        expect(xtreamMovieUrl(cr, 1, L"  ").empty() && xtreamMovieUrl(cr, 1, L"mp4?a=b").empty() &&
                   xtreamMovieUrl(cr, 1, L"toolongext").empty(),
               "xtream: a non-extension-shaped container_extension builds no URL either");

        {   // auth — the real shape, with every numeric field quoted as the panel sends them
            const std::string body =
                "{\"user_info\":{\"username\":\"abc\",\"password\":\"def\",\"message\":\"\","
                "\"auth\":1,\"status\":\"Active\",\"exp_date\":\"1785276000\",\"is_trial\":\"0\","
                "\"active_cons\":\"1\",\"max_connections\":\"1\"},"
                "\"server_info\":{\"port\":\"80\",\"timestamp_now\":1785155974}}";
            XtreamAccount a;
            std::wstring aerr;
            expect(parseXtreamAccount(body, a, &aerr), "xtream: auth response parses");
            expect(a.authOk, "xtream: auth=1 reads as accepted");
            expect(a.status == L"Active", "xtream: account status read");
            expect(a.expiresAt == 1785276000LL, "xtream: quoted exp_date reads as an epoch");
            expect(a.maxConnections == 1,
                   "xtream: quoted max_connections reads as 1 (the constraint on every sync)");
            expect(a.serverTime == 1785155974LL, "xtream: server timestamp read");
            // A panel answers BAD CREDENTIALS with HTTP 200 — "responded" != "let us in".
            XtreamAccount r;
            expect(parseXtreamAccount("{\"user_info\":{\"auth\":0}}", r, nullptr) && !r.authOk,
                   "xtream: auth=0 (bad credentials, HTTP 200) reads as REJECTED");
            expect(parseXtreamAccount("{\"user_info\":{\"auth\":\"0\"}}", r, nullptr) && !r.authOk,
                   "xtream: ...and so does a quoted \"0\"");
            XtreamAccount noUi;
            expect(!parseXtreamAccount("[]", noUi, nullptr),
                   "xtream: a response with no user_info is not an auth response");
            // ABSENT != 0. A fork reporting status without an `auth` member must not be told
            // its credentials were rejected.
            XtreamAccount noAuth;
            expect(parseXtreamAccount("{\"user_info\":{\"status\":\"Active\"}}", noAuth, nullptr) &&
                       noAuth.authOk,
                   "xtream: an ABSENT auth member falls back to status, not to 'rejected'");
            XtreamAccount expired;
            expect(parseXtreamAccount("{\"user_info\":{\"status\":\"Expired\"}}", expired, nullptr) &&
                       !expired.authOk,
                   "xtream: ...and a non-Active status without auth is not accepted");
        }

        {   // categories
            const std::string body =
                "[{\"category_id\":\"1407\",\"category_name\":\"VOD - ACTIE [NL]\",\"parent_id\":0},"
                "{\"category_id\":\"1479\",\"category_name\":\"VOD - VIDEOLAND [NL]\",\"parent_id\":0}]";
            std::vector<XtreamCategory> cats;
            expect(parseXtreamCategories(body, cats, nullptr) && cats.size() == 2,
                   "xtream: categories parse");
            expect(cats[0].id == L"1407" && cats[0].name == L"VOD - ACTIE [NL]",
                   "xtream: quoted category_id is kept as a key, name read");
        }

        {   // *** the VOD list, with every real-world hazard in five items ***
            const std::string body =
                "["
                // 1: the ordinary case
                "{\"num\":1,\"name\":\"Young Hearts (2024)\",\"stream_id\":1218804,"
                "\"stream_icon\":\"http://i/1.jpg\",\"rating\":\"7.3\",\"rating_5based\":3.7,"
                "\"added\":\"1742736240\",\"is_adult\":\"0\",\"category_id\":\"1407\","
                "\"container_extension\":\"mp4\",\"custom_sid\":null,\"direct_source\":\"\"},"
                // 2: mixed typing on the SAME fields, and an empty icon (~90% of the real library)
                "{\"name\":\"Werewolves (2024)\",\"stream_id\":\"1218803\","
                "\"stream_icon\":\"\",\"rating_5based\":\"0\",\"added\":1742736000,"
                "\"is_adult\":1,\"category_id\":1479,\"container_extension\":\"mkv\"},"
                // 3: no container_extension -> no constructible URL -> skipped, counted
                "{\"name\":\"No Ext\",\"stream_id\":99,\"category_id\":\"1407\"},"
                // 4: container_extension present but NULL -> same
                "{\"name\":\"Null Ext\",\"stream_id\":98,\"container_extension\":null},"
                // 5: no id at all -> skipped, counted separately
                "{\"name\":\"No Id\",\"container_extension\":\"mp4\"}"
                "]";
            XtreamVodResult r;
            std::wstring verr;
            // Two statements, deliberately: building the message in the same call as the
            // parse leaves the two arguments indeterminately sequenced, and MSVC evaluates
            // right-to-left — so the diagnostic would be built from `verr` BEFORE the parse
            // wrote it, showing an empty error on the one occasion it matters.
            const bool vodOk = parseXtreamVodStreams(body, r, &verr);
            expect(vodOk, "xtream: VOD list parses (" + utf8FromWide(verr) + ")");
            expect(r.total == 5, "xtream: every element is counted, including the skipped ones");
            expect(r.movies.size() == 2, "xtream: only the constructible items are kept");
            expect(r.skippedNoExt == 2,
                   "xtream: a missing AND a null container_extension both count as no-extension");
            expect(r.skippedNoId == 1, "xtream: an item with no stream_id is counted separately");
            expect(r.movies[0].streamId == 1218804 && r.movies[1].streamId == 1218803,
                   "xtream: a bare id and a QUOTED id both read (the panel mixes them)");
            expect(r.movies[0].added == 1742736240LL && r.movies[1].added == 1742736000LL,
                   "xtream: quoted and bare `added` epochs both read");
            expect(!r.movies[0].adult && r.movies[1].adult,
                   "xtream: is_adult reads from \"0\" and from a bare 1");
            expect(r.movies[1].categoryId == L"1479",
                   "xtream: a BARE numeric category_id still yields the same key as a quoted one");
            expect(r.movies[1].icon.empty(),
                   "xtream: an empty stream_icon is preserved as empty (~90% of the real library)");

            // mapping to DB rows
            std::vector<XtreamCategory> cats{{L"1407", L"VOD - ACTIE [NL]"}, {L"1479", L"VOD - VIDEOLAND [NL]"}};
            const auto rows = xtreamMoviesToChannels(cr, r.movies, cats);
            expect(rows.size() == 2, "xtream: every kept movie becomes a row");
            expect(rows[0].streamUrl == L"http://line.example.com/movie/abc/def/1218804.mp4",
                   "xtream: the row's stream URL is the constructed play URL");
            expect(rows[0].kind == Channel::Kind::Movie && rows[1].kind == Channel::Kind::Movie,
                   "xtream: rows are marked kind=Movie, so schema v8 discriminates them");
            expect(rows[0].groupTitle == L"VOD - ACTIE [NL]" &&
                       rows[1].groupTitle == L"VOD - VIDEOLAND [NL]",
                   "xtream: group_title is the category NAME, so VOD lands in the existing nav tree");
            expect(rows[0].addedAt == 1742736240LL, "xtream: added_at carried onto the row");
            expect(rows[0].isValid(), "xtream: the produced row passes the DAO's validity gate");
            // An unknown category must not produce an empty group (which would read as a
            // blank nav entry) — it falls back.
            std::vector<XtreamMovie> orphan{r.movies[0]};
            orphan[0].categoryId = L"9999";
            const auto oRows = xtreamMoviesToChannels(cr, orphan, cats, L"Movies");
            expect(oRows.size() == 1 && oRows[0].groupTitle == L"Movies",
                   "xtream: an unknown category falls back rather than producing a blank group");
        }

        {   // structural failures must be distinguishable from an empty library
            XtreamVodResult r;
            expect(!parseXtreamVodStreams("{\"user_info\":{}}", r, nullptr),
                   "xtream: an OBJECT where a list belongs fails (not an empty library)");
            expect(!parseXtreamVodStreams("<html>blocked</html>", r, nullptr),
                   "xtream: an HTML error page fails");
            expect(parseXtreamVodStreams("[]", r, nullptr) && r.movies.empty() && r.total == 0,
                   "xtream: an EMPTY list parses and reports zero, which is a real answer");
        }
    }

    out("\n== Buffer-meter LED grid (glass inset) ==\n");
    {
        // The tank's grid runs edge-to-edge, so wiring the glass bezel in costs real dial.
        // That is only acceptable because it is GATED on the glass strength, and THAT is
        // what these assertions pin: with glass off the grid must be bit-identical to the
        // pre-glass renderer. The renderer itself is unreachable from here (anonymous
        // namespace in a GUI TU), which is exactly why the geometry was lifted into the
        // header — see BufferMeter.h.

        // The real tray size at 96 dpi, with the layout recorded in BACKLOG.md.
        const BufferGrid off = bufferGrid(115, 30, 1, 3, 0);
        expect(off.cols == 38 && off.rows == 10 && off.ox == 1 && off.oy == 0,
               "buffer grid: glass OFF reproduces the pre-glass 38x10 @ (1,0) exactly");

        // Glass on: one row and one column of dial is spent on the frame, as designed.
        const int chrome = glassChromePx(115, 30, 2);
        expect(chrome == 2, "buffer grid: glassChromePx(115,30,2) == 2 (the bezel fits)");
        const BufferGrid on = bufferGrid(115, 30, 1, 3, chrome);
        expect(on.cols == 37 && on.rows == 9 && on.ox == 2 && on.oy == 2,
               "buffer grid: glass ON costs exactly one row + one column (37x9 @ (2,2))");

        // The grid must never intrude on the band the bezel is painted into — if it did,
        // the frame would be drawn OVER live LEDs and read as a rendering fault.
        bool contained = true, sane = true;
        for (int w = 8; w <= 400; w += 7) {
            for (int h = 8; h <= 120; h += 5) {
                for (int dpi96 = 96; dpi96 <= 192; dpi96 += 48) {
                    const int gap = std::max(1, MulDiv(1, dpi96, 96));
                    const int pitch = std::max(gap + 2, MulDiv(3, dpi96, 96));
                    const int ins = glassChromePx(w, h, MulDiv(2, dpi96, 96));
                    const BufferGrid g = bufferGrid(w, h, gap, pitch, ins);
                    if (g.cols < 1 || g.rows < 1 || g.ox < 0 || g.oy < 0) sane = false;
                    // A panel too small to carry BOTH a frame and a cell legitimately
                    // overflows; the renderer clips per-pixel. Only check where it fits.
                    if (w - 2 * ins >= pitch && h - 2 * ins >= pitch) {
                        if (g.ox < ins || g.oy < ins) contained = false;
                        if (g.ox + g.cols * pitch - gap > w - ins) contained = false;
                        if (g.oy + g.rows * pitch - gap > h - ins) contained = false;
                    }
                }
            }
        }
        expect(sane, "buffer grid: never degenerate (cols/rows >= 1, offsets >= 0) at any size/DPI");
        expect(contained, "buffer grid: LEDs stay inside the bezel band at every size/DPI");

        // And the gate itself: inset 0 must equal the old layout at EVERY size, not just 115x30.
        bool gated = true;
        for (int w = 8; w <= 400; w += 7)
            for (int h = 8; h <= 120; h += 5) {
                const BufferGrid g = bufferGrid(w, h, 1, 3, 0);
                const int cols = std::max(1, (w + 1) / 3), rows = std::max(1, (h + 1) / 3);
                if (g.cols != cols || g.rows != rows) gated = false;
                if (g.ox != (w - (cols * 3 - 1)) / 2 || g.oy != (h - (rows * 3 - 1)) / 2) gated = false;
            }
        expect(gated, "buffer grid: inset 0 == the original expressions at every size (no default regression)");
    }

    // The --xtream recon probe's census scanner + redactor. Covered here because a
    // scanner bug does not crash — it silently mis-reports the provider's shape, and the
    // VOD design doc gets written off that report.
    out("\n== Xtream recon (census scanner + redaction) ==\n");
    xtreamReconSelftest(&expect);

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

// Diagnose EPG matching: list each playlist's tvg-ids, and (given an EPG url/file) report how many
// match the EPG's channel ids exactly vs only case-insensitively. Markers: '=' exact, '~' case-only,
// 'x' no match. Reads the REAL app DB (%LOCALAPPDATA%\RabbitEars).
int tvgIds(const std::wstring& epgArg) {
    auto lower = [](const std::wstring& s) {
        std::wstring o = s;
        for (auto& c : o)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        return o;
    };
    std::wstring err;
    Database db;
    if (!db.open(Database::defaultDbPath(), &err)) { line(L"DB open failed: " + err); return 1; }

    std::set<std::wstring> epgIds, epgLc;
    if (!epgArg.empty()) {
        std::string bytes;
        if (epgArg.rfind(L"http", 0) == 0) {
            line(L"Fetching EPG " + epgArg);
            if (!httpGet(epgArg, bytes, err, 90000)) { line(L"EPG download failed: " + err); return 1; }
        } else {
            std::ifstream f(epgArg, std::ios::binary);
            if (!f) { line(L"Cannot open " + epgArg); return 1; }
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        const XmltvDocument epg = parseXmltv(gunzipIfNeeded(bytes));
        for (const auto& p : epg.programmes) { epgIds.insert(p.channelId); epgLc.insert(lower(p.channelId)); }
        line(L"EPG: " + std::to_wstring(epg.programmes.size()) + L" programmes, " +
             std::to_wstring(epgIds.size()) + L" distinct channel ids");
    }

    for (const auto& pl : db.listPlaylists()) {
        const auto chans = db.channelsByPlaylist(pl.id);
        int withId = 0, exact = 0, ci = 0, base = 0;
        for (const auto& c : chans) {
            if (c.tvgId.empty()) continue;
            ++withId;
            if (epgIds.empty()) continue;
            if (epgIds.count(c.tvgId)) ++exact;
            else if (epgLc.count(lower(c.tvgId))) ++ci;
            else {
                const std::wstring b = c.tvgId.substr(0, c.tvgId.find(L'@'));  // strip iptv-org @feed
                if (b != c.tvgId && (epgIds.count(b) || epgLc.count(lower(b)))) ++base;
            }
        }
        line(L"");
        std::wstring hdr = L"Playlist #" + std::to_wstring(pl.id) + L"  \"" + pl.name + L"\"  " +
                           std::to_wstring(chans.size()) + L" ch, " + std::to_wstring(withId) + L" w/ tvg-id";
        if (!epgIds.empty())
            hdr += L"  -> EPG match: " + std::to_wstring(exact) + L" exact + " + std::to_wstring(ci) +
                   L" case-insensitive + " + std::to_wstring(base) + L" after @-strip";
        line(hdr);
        int shown = 0;
        for (const auto& c : chans) {
            if (c.tvgId.empty()) continue;
            if (shown++ >= 20) break;
            std::wstring mark = L"   ";
            if (!epgIds.empty()) {
                if (epgIds.count(c.tvgId)) mark = L" = ";
                else if (epgLc.count(lower(c.tvgId))) mark = L" ~ ";
                else {
                    const std::wstring b = c.tvgId.substr(0, c.tvgId.find(L'@'));
                    mark = (b != c.tvgId && (epgIds.count(b) || epgLc.count(lower(b)))) ? L" @ " : L" x ";
                }
            }
            line(mark + c.tvgId + L"   <=   " + c.name.substr(0, 40));
        }
    }
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

// Does a real-sized VOD import degrade the EXISTING live-TV UI? Win32/docs/XTREAM_VOD.md
// calls this the epic's biggest risk and says to measure it BEFORE the sync ships: 43,599
// movies is ~4x the owner's library, landing in the same `channels` table that already
// needed a registered SQLite scalar to keep the country filter under ~30 ms/keystroke at
// 14k rows. Builds an isolated DB, times the queries loadForFilter() actually runs at
// live-only size, then again after the movies land, and prints the ratio.
int benchDb(int movies, int live) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _wputenv_s(L"RABBITEARS_DATA_DIR", (std::wstring(tmp) + L"rabbitears_bench").c_str());
    const std::wstring dbPath = Database::defaultDbPath();
    DeleteFileW(dbPath.c_str());
    DeleteFileW((dbPath + L"-wal").c_str());
    DeleteFileW((dbPath + L"-shm").c_str());

    std::wstring err;
    Database db;
    if (!db.open(dbPath, &err)) { line(L"DB open failed: " + err); return 1; }
    const long long now = static_cast<long long>(time(nullptr));
    const long long pid = db.addPlaylist(L"Bench", L"http://bench", true, now);

    // Live channels shaped like a real IPTV list: a country-suffixed tvg-id on most, an
    // Xtream-style "|NL| SPORT" group prefix on the rest — both paths the country filter has
    // to walk (tvg-id first, then the group-title fallback registered in Database::open).
    static const wchar_t* kCc[] = {L"nl", L"uk", L"us", L"de", L"fr", L"be", L"es", L"it"};
    std::vector<ParsedChannel> liveRows;
    liveRows.reserve(live);
    for (int i = 0; i < live; ++i) {
        ParsedChannel c;
        c.name = L"Channel " + std::to_wstring(i);
        c.streamUrl = L"http://bench/live/" + std::to_wstring(i) + L".ts";
        const std::wstring cc = kCc[i % 8];
        if (i % 3)  // two thirds carry a tvg-id; the rest exercise the group-title fallback
            c.tvgId = L"Ch" + std::to_wstring(i) + L"." + cc;
        std::wstring up = cc;
        for (auto& ch : up) ch = static_cast<wchar_t>(towupper(ch));
        c.groupTitle = L"|" + up + L"| " + (i % 2 ? L"ALGEMEEN" : L"SPORT");
        liveRows.push_back(std::move(c));
    }
    db.bulkInsertChannels(pid, liveRows, now);

    struct Timing { const char* what; double before = 0, after = 0; };
    // Median of N runs: a single sample on a network share (this repo lives on one) is noise.
    auto timeIt = [&](auto&& fn) {
        constexpr int kRuns = 5;
        std::vector<double> ms;
        for (int r = 0; r < kRuns; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            ms.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        return ms[kRuns / 2];
    };
    const std::wstring aGroup = L"|NL| SPORT";
    auto runAll = [&](std::vector<Timing>& t, bool after) {
        auto put = [&](const char* what, double v) {
            for (auto& x : t)
                if (x.what == what) { (after ? x.after : x.before) = v; return; }
            Timing n{what};
            (after ? n.after : n.before) = v;
            t.push_back(n);
        };
        put("allChannels()",        timeIt([&] { (void)db.allChannels(); }));
        put("listGroups()",         timeIt([&] { (void)db.listGroups(); }));
        put("listVodGroups()",      timeIt([&] { (void)db.listVodGroups(); }));
        put("allMovies()",          timeIt([&] { (void)db.allMovies(); }));
        put("moviesByGroup()",      timeIt([&] { (void)db.moviesByGroup(L"VOD - CATEGORY 3 [NL]"); }));
        put("listCountries()",      timeIt([&] { (void)db.listCountries(); }));
        put("channelsByCountry()",  timeIt([&] { (void)db.channelsByCountry(L"nl"); }));
        put("channelsByGroup()",    timeIt([&] { (void)db.channelsByGroup(aGroup); }));
        put("favourites()",         timeIt([&] { (void)db.favourites(); }));
        put("searchChannels()",     timeIt([&] { (void)db.searchChannels(L"Channel 1"); }));
        // The FIRST KEYSTROKE, which is the honest worst case and the one the line above misses:
        // "Channel 1" matches no movie by name or category, so it measures the scan and none of
        // the materialization. A single letter matches most of the library, and the search box
        // runs this synchronously on the UI thread on every EN_CHANGE with no debounce and no
        // LIMIT — so this figure, not the one above, is what the user feels while typing.
        put("searchChannels() 1ch",  timeIt([&] { (void)db.searchChannels(L"e"); }));
        put("channelsByPlaylist()", timeIt([&] { (void)db.channelsByPlaylist(pid); }));
    };

    std::vector<Timing> t;
    line(L"Building baseline: " + std::to_wstring(live) + L" live channels...");
    runAll(t, /*after=*/false);

    // Movies exactly as the Xtream client would produce them: kind=Movie, group_title = the
    // category NAME, no tvg-id (so they must NOT appear in any country bucket).
    line(L"Adding " + std::to_wstring(movies) + L" movies (kind=Movie)...");
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::vector<ParsedChannel> vod;
        vod.reserve(movies);
        for (int i = 0; i < movies; ++i) {
            ParsedChannel c;
            c.name = L"Film Title Number " + std::to_wstring(i) + L" (2024)";
            c.streamUrl = L"http://bench/movie/u/p/" + std::to_wstring(i) + L".mp4";
            c.groupTitle = L"VOD - CATEGORY " + std::to_wstring(i % 67) + L" [NL]";
            c.kind = Channel::Kind::Movie;
            c.addedAt = now - i;
            vod.push_back(std::move(c));
        }
        db.bulkInsertChannels(pid, vod, now);
    }
    const double insertMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    runAll(t, /*after=*/true);

    line(L"");
    line(L"  query                   live-only     +VOD        ratio");
    line(L"  ------------------------------------------------------------");
    for (const auto& x : t) {
        wchar_t buf[160];
        const double ratio = x.before > 0.01 ? x.after / x.before : 0.0;
        std::wstring what = wideFromUtf8(x.what);
        what.resize(22, L' ');
        swprintf_s(buf, L"  %s  %8.2f ms  %8.2f ms   %s%.1fx", what.c_str(), x.before, x.after,
                   ratio >= 10.0 ? L"** " : L"", ratio);
        line(buf);
    }
    line(L"");
    {
        wchar_t buf[128];
        swprintf_s(buf, L"  bulk insert of %d movies: %.0f ms", movies, insertMs);
        line(buf);
    }
    // The correctness half: VOD rows must stay OUT of the live-TV views, or the country tree
    // and every group the user already had are swamped by 43,599 films.
    const size_t nlAfter = db.channelsByCountry(L"nl").size();
    const size_t groupsAfter = db.listGroups().size();
    const size_t vodGroupsAfter = db.listVodGroups().size();
    line(L"  countries: channelsByCountry(nl) returned " + std::to_wstring(nlAfter) +
         L" rows (movies carry no tvg-id and no country prefix, so they must NOT be here)");
    line(L"  groups:    listGroups() returns " + std::to_wstring(groupsAfter) +
         L" LIVE groups (unchanged by the import — VOD has its own root), and " +
         std::to_wstring(vodGroupsAfter) + L" VOD categories under Movies");
    line(L"");
    line(L"Interpretation: anything over ~2x on a per-keystroke path (channelsByCountry,");
    line(L"channelsByGroup, searchChannels) is a regression the user will feel. allChannels()");
    line(L"legitimately grows with the row count — it is the 'All' view.");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && std::wstring(argv[1]) == L"--selftest") return selftest();
    if (argc >= 3 && std::wstring(argv[1]) == L"--fetch") return fetch(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"--import") return importSource(argv[2]);
    if (argc >= 3 && std::wstring(argv[1]) == L"--epg") return epgTool(argv[2]);
    if (argc >= 2 && std::wstring(argv[1]) == L"--tvgids")
        return tvgIds(argc >= 3 ? std::wstring(argv[2]) : std::wstring());
    if (argc >= 2 && std::wstring(argv[1]) == L"--benchdb") {
        // Defaults are the owner's real numbers: 43,599 movies beside a 442-channel list.
        const int mv = argc >= 3 ? _wtoi(argv[2]) : 43599;
        const int lv = argc >= 4 ? _wtoi(argv[3]) : 442;
        return benchDb(mv > 0 ? mv : 43599, lv > 0 ? lv : 442);
    }
    if (argc >= 2 && std::wstring(argv[1]) == L"--xtream") {
        // Both extra args are optional and order-independent: the URL (omit it to auto-pick
        // an Xtream playlist out of the app's own DB) and --raw (disable redaction).
        std::wstring url;
        bool raw = false;
        for (int i = 2; i < argc; ++i) {
            const std::wstring a = argv[i];
            if (a == L"--raw") raw = true;
            else if (url.empty()) url = a;
        }
        return xtreamRecon(url, raw);
    }
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
        "  RabbitEarsCli --epg <url|file>\n"
        "  RabbitEarsCli --tvgids [epg url|file]\n"
        "  RabbitEarsCli --benchdb [movies] [live]  (does a VOD import slow the live-TV UI?\n"
        "                                            defaults 43599 movies / 442 live)\n"
        "  RabbitEarsCli --xtream [url] [--raw]  (probe an Xtream provider; url defaults\n"
        "                                         to an Xtream playlist in the app's DB.\n"
        "                                         Output is credential-redacted unless --raw.)\n"
        "  RabbitEarsCli <file.m3u> [--limit N]\n");
    return 0;
}
