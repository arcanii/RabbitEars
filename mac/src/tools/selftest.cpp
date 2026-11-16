// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS core self-test — the GUI-free proof that the SHARED core (M3U parser +
// SQLite store + dock-layout logic) is genuinely portable, built and run on
// clang. It reuses the exact core code the Windows app uses; only the harness
// (temp dir / env / file delete) is portable POSIX/std instead of the Win32
// calls in src/cli/RabbitEarsCli.cpp (which is Windows-only: wmain, GetTempPathW,
// DeleteFileW, SetConsoleOutputCP). Assertions mirror RabbitEarsCli --selftest.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>  // setenv
#include <filesystem>
#include <string>
#include <vector>

#include "core/GuideSources.h"
#include "core/M3uParser.h"
#include "core/XtreamClient.h"  // XtreamCreds — the UrlRedact pin against its path encoder
#include "core/Strings.h"  // shared i18n catalog — completeness + placeholder-parity gate below
#include "db/Database.h"
#include "platform/Encoding.h"  // non-Windows branch of the shared header
#include "platform/UrlRedact.h"
#include "ui/DockLayout.h"

#include "MeterModel.h"
#include "ScheduleStatusWriter.h"  // mac scheduler write-behind glue — tested below
#include "core/RecordingScheduler.h"
#include "sqlite3.h"  // a second connection holding the writer lock (genuine contention)  // mac-local meter model (rabbitears::mac) — codecs tested below

using namespace rabbitears;

// The selftest links no log sink (mac/platform/Log.mm is app-only), but core code under test logs
// through diag::write — so define a quiet one here.
namespace rabbitears::diag {
void write(const wchar_t*, const std::wstring&) {}
}  // namespace rabbitears::diag

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

    // The two blocks below are PORTED VERBATIM from Win32/cli/RabbitEarsCli.cpp --selftest (same
    // expect() contract), so the shared GuideSources + UrlRedact code is proven under Apple clang
    // too — keep them in step with the Win32 originals.
    out("== Guide links naming several guides (core/GuideSources) ==\n");
    {
        using V = std::vector<std::wstring>;
        // The parser keeps the header's list as written; Refresh Guide splits it.
        const M3uDocument two = parseM3u("#EXTM3U url-tvg=\"https://a.example/epg.xml.gz,https://b.example/x.xml\"\n"
                                         "#EXTINF:-1,One\nhttp://s/1\n");
        expect(two.epgUrl == L"https://a.example/epg.xml.gz,https://b.example/x.xml",
               "M3U: a url-tvg listing two guides is kept as written");
        expect(splitGuideUrls(two.epgUrl) == V{L"https://a.example/epg.xml.gz", L"https://b.example/x.xml"},
               "splitGuideUrls: two comma-separated guides -> two links, in order");
        expect(splitGuideUrls(L"http://h.example/xmltv.php?username=u&password=p") ==
                       V{L"http://h.example/xmltv.php?username=u&password=p"} &&
                   splitGuideUrls(L"  http://h.example/g.xml \t") == V{L"http://h.example/g.xml"},
               "splitGuideUrls: one link stays one (trimmed)");
        expect(splitGuideUrls(L"http://a/x.php?ids=1,2,3&k=http") == V{L"http://a/x.php?ids=1,2,3&k=http"} &&
                   splitGuideUrls(L"http://a/x.xml,httpx://b/y.xml") == V{L"http://a/x.xml,httpx://b/y.xml"} &&
                   splitGuideUrls(L"http://a/x.xml,ftp://b/y.xml") == V{L"http://a/x.xml,ftp://b/y.xml"} &&
                   splitGuideUrls(L"http://a/x.xml,http:/b") == V{L"http://a/x.xml,http:/b"},
               "splitGuideUrls: a comma not followed by http(s):// stays inside the link");
        expect(splitGuideUrls(L"http://a/x.xml , HTTPS://b/y.xml,,\thttp://c/z.xml") ==
                       V{L"http://a/x.xml", L"HTTPS://b/y.xml", L"http://c/z.xml"} &&
                   splitGuideUrls(L"http://a/x.xml http://b/y.xml") == V{L"http://a/x.xml", L"http://b/y.xml"},
               "splitGuideUrls: a run of commas and spaces separates (scheme in any case)");
        expect(splitGuideUrls(L"http://a/x.xml,http://b/y.xml,http://a/x.xml") ==
                       V{L"http://a/x.xml", L"http://b/y.xml"} &&
                   splitGuideUrls(L"").empty() && splitGuideUrls(L" \t ").empty() &&
                   splitGuideUrls(L"http://a/x.xml,") == V{L"http://a/x.xml,"},
               "splitGuideUrls: a repeat dropped; blank = none; ONE link's trailing comma is left as written");
        expect(splitGuideUrls(L"https://a/x.xml.gz,https://b/y.xml.gz,") ==
                       V{L"https://a/x.xml.gz", L"https://b/y.xml.gz"} &&
                   splitGuideUrls(L"http://a/x.xml,,http://a/x.xml ,") == V{L"http://a/x.xml"} &&
                   splitGuideUrls(L"EPG Link : http://h/xmltv.php?username=u&password=p") ==
                       V{L"http://h/xmltv.php?username=u&password=p"} &&
                   splitGuideUrls(L"EPG: http://a/x.xml, http://b/y.xml") == V{L"http://a/x.xml", L"http://b/y.xml"},
               "splitGuideUrls: a list drops a label before its first link and separators after its last");

        // Merging: a channel's programmes come from the FIRST guide that lists it.
        auto prog = [](const wchar_t* ch, const wchar_t* title) {
            Programme p;
            p.channelId = ch;
            p.title = title;
            p.startUtc = 1000;
            return p;
        };
        std::vector<std::vector<Programme>> guides(3);
        // The first guide spells a channel "One.UK@HD", a later one "one.uk": the SAME channel only once
        // both are normalised, as the guide joins them.
        guides[0] = {prog(L"One.UK@HD", L"A1"), prog(L"One.UK@HD", L"A2"), prog(L"two.uk", L"A3")};
        // Out of channel order on purpose: the merge must not assume a guide groups its channels.
        guides[1] = {prog(L"three.uk", L"B1"), prog(L"one.uk", L"B2"), prog(L"three.uk", L"B3")};
        // Starts with guide 1's last channel: a per-channel cache carried across guides would keep it.
        guides[2] = {prog(L"three.uk", L"C1"), prog(L"Two.UK", L"C2"), prog(L"four.uk", L"C3")};
        const GuideMerge m = mergeGuideSources(std::move(guides));
        std::wstring titles;
        for (const auto& p : m.programmes) titles += p.title + L" ";
        expect(titles == L"A1 A2 A3 B1 B3 C3 ",
               "mergeGuideSources: each channel from the first guide listing it, in order (got " +
                   utf8FromWide(titles) + ")");
        expect(m.kept == std::vector<size_t>{3, 2, 1} && m.dropped == std::vector<size_t>{0, 1, 2},
               "mergeGuideSources: per-guide kept 3/2/1, left out 0/1/2 (ids as the guide joins them: "
               "case and @suffix ignored)");
        std::vector<std::vector<Programme>> one(1);
        one[0] = {prog(L"x", L"X1"), prog(L"x", L"X2")};
        const GuideMerge m1 = mergeGuideSources(std::move(one));
        expect(m1.programmes.size() == 2 && m1.programmes[1].title == L"X2" && m1.kept == std::vector<size_t>{2} &&
                   m1.dropped == std::vector<size_t>{0} && mergeGuideSources({}).programmes.empty(),
               "mergeGuideSources: one guide passes through whole; none gives nothing");
        // A programme the store would not keep (no readable start) claims no channel.
        std::vector<std::vector<Programme>> unreadable(2);
        Programme noStart = prog(L"five.uk", L"F0");
        noStart.startUtc = 0;
        Programme orphan = prog(L"six.uk", L"S1");  // listed by no guide with a readable start
        orphan.startUtc = 0;
        Programme alsoNoStart = prog(L"five.uk", L"F2");  // on a channel its own guide owns
        alsoNoStart.startUtc = 0;
        unreadable[0] = {noStart};
        unreadable[1] = {prog(L"five.uk", L"F1"), alsoNoStart, orphan};
        const GuideMerge mu = mergeGuideSources(std::move(unreadable));
        std::wstring ut;
        for (const auto& p : mu.programmes) ut += p.title + L" ";
        expect(ut == L"F0 F1 " && mu.kept == std::vector<size_t>{1, 1} && mu.dropped == std::vector<size_t>{0, 2},
               "mergeGuideSources: a programme with no readable start claims no channel (the next guide's is "
               "taken; the first guide still passes whole), and a later guide's is left out (got " +
                   utf8FromWide(ut) + ")");

        // Set Guide URL: a typed LIST is kept whole; anything else keeps one address (the old rules).
        expect(guideUrlList(L"http://a.example/x.xml, https://b.example/y.xml.gz", L"") ==
                       L"http://a.example/x.xml,https://b.example/y.xml.gz" &&
                   guideUrlList(L"EPG: http://a.example/x.xml, http://b.example/y.xml", L"") ==
                       L"http://a.example/x.xml,http://b.example/y.xml",
               "guideUrlList: a list of guide links (a label before it dropped) -> joined with commas");
        expect(guideUrlList(L"http://a.example/x.xml", L"").empty() &&
                   guideUrlList(L"http://h/get.php?username=u&password=p http://h/xmltv.php?username=u&password=p",
                                L"")
                       .empty() &&
                   guideUrlList(L"http://host:8080/c/ http://host:8080/xmltv.php?username=U&password=P", L"").empty() &&
                   guideUrlList(L"https://prov.example/epg.xml.gz https://prov.example/", L"").empty(),
               "guideUrlList: one link, or a playlist, portal or website link beside a guide link -> not a list");
        expect(guideUrlList(L"http://h/epg1, http://h/epg2", L"http://h/epg1,http://h/epg2") ==
                       L"http://h/epg1,http://h/epg2" &&
                   guideUrlList(L"http://h/epg1,http://h/epg2, https://x.example/y.xml", L"http://h/epg1,http://h/epg2") ==
                       L"http://h/epg1,http://h/epg2,https://x.example/y.xml" &&
                   guideUrlList(L"http://h/epg1,http://h/epg3", L"http://h/epg1,http://h/epg2").empty(),
               "guideUrlList: the playlist's stored links are kept whatever their shape; a new one must look like "
               "a guide link");
        const std::wstring storedWithPlaylist = L"http://h/get.php?username=u&password=p,http://h/xmltv.php";
        expect(guideUrlList(storedWithPlaylist, storedWithPlaylist).empty(),
               "guideUrlList: a stored list with a playlist link in it is not kept as a list (the prompt's one-address "
               "rules then take the guide link)");
    }

    out("== Log masking + pasted guide URL (common/platform/UrlRedact) ==\n");
    {
        // A guide/playlist URL: credential-named query values are masked with no registration.
        const std::wstring epgLine =
            L"set epg_url for playlist id=3 to \"http://h.example:8080/xmltv.php?username=alice123&password=s3cretPW\"";
        const std::wstring m1 = redactUrls(epgLine, {});
        expect(m1 == L"set epg_url for playlist id=3 to \"http://h.example:8080/xmltv.php?username=***&password=***\"",
               "query username/password masked; host, path and quotes kept");
        expect(redactUrls(L"http://h/x?PassWord=xyz1234&type=m3u_plus&action=get_live_streams", {}) ==
                   L"http://h/x?PassWord=***&type=m3u_plus&action=get_live_streams",
               "param names case-insensitive; non-credential params kept");
        expect(redactUrls(L"http://h/x?user=ab", {}) == L"http://h/x?user=***",
               "a credential-named value is masked whatever its length");

        // A login in a path the shape rules do not recognise needs registration.
        const std::vector<std::wstring> creds =
            urlCredentials(L"http://h/get.php?username=alice123&password=s3cretPW&type=m3u_plus");
        expect(creds.size() == 2 && creds[0] == L"alice123" && creds[1] == L"s3cretPW",
               "urlCredentials: username + password from the query (got " + std::to_string(creds.size()) + ")");
        expect(redactUrls(L"play: http://h:80/alice123/s3cretPW/index.m3u8", creds) ==
                   L"play: http://h:80/***/***/index.m3u8",
               "registered login masked in a path that is not a stream shape");

        // The spelling XtreamClient writes into a stream path is registered from the playlist URL
        // alone (pins encodeLikeXtream to XtreamClient's encoder), so last session's log is masked
        // at startup even where it quotes the path without its scheme.
        {
            const std::wstring pl =
                L"http://h:8080/get.php?username=us%21er9&password=p%40ss+w0rd%23&type=m3u_plus";
            XtreamCreds xc;
            expect(parseXtreamPlaylistUrl(pl, xc), "pin: the tricky playlist URL parses");
            const std::wstring movie = xtreamMovieUrl(xc, 77, L"mkv");
            const size_t pathAt = movie.find(L'/', movie.find(L"://") + 3);
            const std::wstring bare = L"open failed for " + movie.substr(pathAt);  // no scheme
            const std::wstring masked = redactUrls(bare, urlCredentials(pl));
            expect(!movie.empty() && masked == L"open failed for /movie/***/***/77.mkv",
                   "pin: XtreamClient's path spelling of a special-character login is registered");
        }
        expect(redactUrls(L"http://h/x?username=abcd&password=ab#cdef", {}) ==
                   L"http://h/x?username=***&password=***",
               "a '#' inside a query value does not end it (XtreamClient reads past it)");
        expect(redactUrls(L"alice123 opened http://h/alice123/x", creds) == L"*** opened http://h/***/x",
               "a registered login is masked anywhere in the line, not only in a URL");
        expect(redactUrls(L"http stream error for /alice123/s3cretPW/12345.ts", creds) ==
                   L"http stream error for /***/***/12345.ts",
               "...including a stream path quoted without its scheme");
        expect(redactUrls(L"a plain line, no secrets", creds) == L"a plain line, no secrets",
               "a line with no URL and no registered login is returned unchanged");
        expect(redactUrls(L"a http://h/?password=abcd1 b http://g/x?user=efgh2&q=1 c", {}) ==
                   L"a http://h/?password=*** b http://g/x?user=***&q=1 c",
               "every URL on a line is masked (no registered secrets: the URL scan alone)");
        expect(redactUrls(L"x https://bob:hunter22@host/p y", {}) == L"x https://***@host/p y",
               "user-info masked");

        // Xtream stream paths are masked by shape, with nothing registered.
        expect(redactUrls(L"play: http://h:8080/abcuser/abcpass/123456", {}) ==
                   L"play: http://h:8080/***/***/123456",
               "stream path /USER/PASS/123 masked unregistered");
        expect(redactUrls(L"http://h/live/abcuser/abcpass/123456.ts http://h/movie/abcuser/abcpass/77.mkv", {}) ==
                   L"http://h/live/***/***/123456.ts http://h/movie/***/***/77.mkv",
               "/live/ and /movie/ stream paths masked unregistered");
        expect(redactUrls(L"http://h/timeshift/u1234/p1234/60/2026-09-24:10-00/123.ts", {}) ==
                   L"http://h/timeshift/***/***/60/2026-09-24:10-00/123.ts",
               "/timeshift/ path: only the two login segments masked");
        expect(redactUrls(L"https://cdn.x/a/b/playlist.m3u8 http://h/x/12.ts http://h/a/b/c/12.ts", {}) ==
                   L"https://cdn.x/a/b/playlist.m3u8 http://h/x/12.ts http://h/a/b/c/12.ts",
               "non-stream shapes untouched (non-numeric last segment; 2 segments; 4 without a kind)");
        expect(redactUrls(L"see http://h/abcuser/abcpass/123.ts, ok (http://h/abcuser/abcpass/9)", {}) ==
                   L"see http://h/***/***/123.ts, ok (http://h/***/***/9)",
               "stream shape still recognised with the sentence's punctuation after it");
        expect(redactUrls(L"http://a/e.xml,http://u1:pw12@b/abcuser/abcpass/1.ts", {}) ==
                   L"http://a/e.xml,http://***@b/***/***/1.ts",
               "two URLs run together are analysed separately");
        expect(redactUrls(L"http://h/x?password=abcd;ef&type=m3u_plus", {}) ==
                   L"http://h/x?password=***&type=m3u_plus",
               "';' is part of a query value, not a separator");
        expect(redactUrls(L"http://h/x?u=1&amp;password=abcd12", {}) == L"http://h/x?u=1&amp;password=***",
               "an HTML-escaped &amp;password= still counts");
        expect(redactUrls(L"alive=1 live /live/ x", { L"live" }) == L"alive=1 *** /***/ x",
               "a registered login is masked only as a whole token (not inside 'alive')");
        expect(redactUrls(L"u=http%3A%2F%2Fh%2Falice123%2Fx", creds) == L"u=http%3A%2F%2Fh%2F***%2Fx",
               "...where a %XX escape before it counts as a boundary");
        // A stream path's login is registered only for an account already known.
        const std::vector<std::wstring> sc =
            streamLoginToRegister(L"http://h/movie/abcuser/ab%20cd12/9.mkv", { L"abcuser" });
        auto scHas = [&sc](const wchar_t* v) { return std::find(sc.begin(), sc.end(), v) != sc.end(); };
        expect(sc.size() == 3 && scHas(L"abcuser") && scHas(L"ab%20cd12") && scHas(L"ab cd12"),
               "streamLoginToRegister: a known account's path login, as written and decoded");
        expect(streamLoginToRegister(L"http://h/movie/abcuser/abcpass/9.mkv", {}).empty() &&
                   streamLoginToRegister(L"http://h/stream/channel/5.ts", { L"alice123" }).empty(),
               "streamLoginToRegister: nothing for an unknown account or a lookalike (no ordinary words)");
        expect(urlCredentials(L"http://h/live/abcuser/abcpass/1.ts").empty(),
               "urlCredentials never returns a stream path's segments");
        const std::vector<std::wstring> plus = urlCredentials(L"http://h/?password=ab+cd12");
        expect(std::find(plus.begin(), plus.end(), L"ab+cd12") != plus.end() &&
                   std::find(plus.begin(), plus.end(), L"ab cd12") != plus.end(),
               "urlCredentials: a query value also with '+' read as a space");
        expect(redactUrls(L"http://h/live/chan1/123.m3u8 and http://h/hls/live/123.m3u8", {}) ==
                   L"http://h/live/chan1/123.m3u8 and http://h/***/***/123.m3u8",
               "3 segments starting with a kind word are not /USER/PASS/123; any other 3 are "
               "(masked in that line only - the documented cost)");
        expect(redactUrls(L"udp://@239.1.1.1:1234 rtp://@232.0.0.1:5000", {}) ==
                   L"udp://@239.1.1.1:1234 rtp://@232.0.0.1:5000",
               "an empty user-info (multicast) is not masked");
        expect(redactUrls(L"x!abc123 y and pass!x", { L"!abc123", L"pass!" }) == L"x*** y and ***x",
               "a secret's punctuation edge matches whatever is beside it");
        {
            std::wstring big;
            for (int k = 0; k < 200000; ++k) big += L" alice123";
            const auto t0 = std::chrono::steady_clock::now();
            const std::wstring masked = redactUrls(big, { L"alice123" });
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            expect(masked.find(L"alice123") == std::wstring::npos && masked.size() == 200000u * 4u &&
                       ms < 2000,
                   "200,000 hits masked in one pass (" + std::to_string(ms) + " ms; an in-place loop took seconds)");
        }
        expect(redactUrls(L"http://h/ab/cd/x.ts ab", { L"ab" }) == L"http://h/ab/cd/x.ts ab",
               "a registered secret under 4 chars is ignored");
        expect(urlCredentials(L"http://h/?username=ab&password=abc").empty(),
               "urlCredentials skips values under 4 chars");
        const std::vector<std::wstring> enc = urlCredentials(L"http://h/?username=u%40mail&password=p%2Bw0rd");
        auto has = [&enc](const wchar_t* v) { return std::find(enc.begin(), enc.end(), v) != enc.end(); };
        expect(has(L"u%40mail") && has(L"u@mail") && has(L"p%2Bw0rd") && has(L"p+w0rd"),
               "urlCredentials returns the raw AND the percent-decoded form");
        const std::vector<std::wstring> ui = urlCredentials(L"ftp://bob:hunter22@host/");
        expect(ui.size() == 1 && ui[0] == L"hunter22",
               "urlCredentials: user-info password kept, the 3-char user skipped");

        // Set Guide URL: keep just the address out of whatever was pasted.
        expect(extractHttpUrl(L"EPG Link : http://h.example/xmltv.php?username=a&password=b") ==
                   L"http://h.example/xmltv.php?username=a&password=b",
               "extractHttpUrl: a provider email line yields just the address");
        expect(extractHttpUrl(L"  https://h/x.php?password=Secret!.  ") == L"https://h/x.php?password=Secret!.",
               "extractHttpUrl: a lone address is kept exactly as typed (a password may end in ! or .)");
        expect(extractHttpUrl(L"Guide: https://h/x.xml.gz.") == L"https://h/x.xml.gz",
               "extractHttpUrl: an address in prose loses the sentence's full stop");
        expect(extractHttpUrl(L"(see http://h/guide)") == L"http://h/guide",
               "extractHttpUrl: an unmatched closing parenthesis dropped");
        expect(extractHttpUrl(L"http://h/a_(b)") == L"http://h/a_(b)",
               "extractHttpUrl: a matched parenthesis kept");
        expect(extractHttpUrl(L"<http://h/g>") == L"http://h/g", "extractHttpUrl: angle brackets end it");
        expect(extractHttpUrl(L"HTTP://H/G") == L"HTTP://H/G", "extractHttpUrl: scheme case-insensitive, kept as typed");
        expect(extractHttpUrl(L"x https://a/1 then http://b/2") == L"https://a/1",
               "extractHttpUrl: the FIRST address wins, https or http");
        expect(extractHttpUrl(L"no address here").empty() && extractHttpUrl(L"http://").empty() &&
                   extractHttpUrl(L"ftp://h/x").empty(),
               "extractHttpUrl: none, a bare scheme, or a non-http scheme -> empty");
        // ...and warn when what was pasted is the PLAYLIST link.
        expect(looksLikePlaylistUrl(L"http://h:80/get.php?username=u&password=p&type=m3u_plus&output=ts") &&
                   looksLikePlaylistUrl(L"http://h/GET.PHP?username=u") &&
                   looksLikePlaylistUrl(L"https://h/list/playlist.m3u") &&
                   looksLikePlaylistUrl(L"https://h/live/index.M3U8?token=1") &&
                   looksLikePlaylistUrl(L"http://h/panel_api.php?type=m3u&u=1") &&
                   looksLikePlaylistUrl(L"http://h/x.php?a=1&type=m3u_plus#frag") &&
                   looksLikePlaylistUrl(L"http://h:8080/playlist/user/pass/m3u_plus") &&
                   looksLikePlaylistUrl(L"http://h/playlist/user/pass/M3U?output=ts"),
               "looksLikePlaylistUrl: get.php, the path form, .m3u/.m3u8 and type=m3u[_plus] flagged");
        expect(!looksLikePlaylistUrl(L"http://h/xmltv.php?username=u&password=p") &&
                   !looksLikePlaylistUrl(L"https://h/guide.xml") && !looksLikePlaylistUrl(L"https://h/epg.xml.gz") &&
                   !looksLikePlaylistUrl(L"https://h/epg.gz?type=m3u") && !looksLikePlaylistUrl(L"https://h/epg") &&
                   !looksLikePlaylistUrl(L"http://h/x.php?type=m3u8") &&
                   !looksLikePlaylistUrl(L"http://h/x.php?subtype=m3u") &&
                   !looksLikePlaylistUrl(L"http://h/get.php.xml"),
               "looksLikePlaylistUrl: guide shapes (xmltv.php, .xml, .gz), unknowns and near-misses not flagged");
        expect(looksLikeGuideUrl(L"http://h/xmltv.php?username=u&password=p") &&
                   looksLikeGuideUrl(L"https://h/EPG.XML.GZ?x=1#f") && looksLikeGuideUrl(L"https://h/guide.xml") &&
                   !looksLikeGuideUrl(L"http://h/c/") && !looksLikeGuideUrl(L"http://h/get.php?type=m3u") &&
                   !looksLikeGuideUrl(L"http://h/x.php?file=a.xml"),
               "looksLikeGuideUrl: the clear guide shapes only (the path, not the query)");
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

    // The two scheduler blocks below are PORTED from Win32/cli/RabbitEarsCli.cpp (Win32 timer and
    // sqlite3_open16 swapped for portable ones); the third is mac's own glue.

    // The macOS team's 2026-08-09 finding (Win32/BACKLOG.md): updateScheduleStatus was void, so a
    // status write LOST under contention was invisible — and planScheduler learns that a SCHEDULE
    // holds the recorder only from the status column. A lost Recording write left the row Pending
    // while the recorder ran; the next tick re-started it over the file in progress. The contended
    // assertions below drive the REAL failure (a second connection holding the writer lock) through
    // the REAL beginScheduledStart the scheduler calls — not a copy of its logic.
    out("== Schedule status writes that can be LOST ==\n");
    {
        using S = ScheduleStatus;
        ScheduledRecording s;
        s.channelId = L"cnn.us";
        s.channelName = L"CNN";
        s.streamUrl = L"http://s/cnn";
        s.title = L"News";
        s.startUtc = 5000;
        s.stopUtc = 8000;
        s.mux = L"ts";
        s.createdAt = 1000;
        const long long sid = db.addSchedule(s);
        expect(sid > 0, "lost-write fixture: schedule added");
        auto rowOf = [&](long long id) {
            for (const ScheduledRecording& r : db.listSchedules())
                if (r.id == id) return r;
            return ScheduledRecording{};
        };

        expect(db.updateScheduleStatus(sid, S::Pending), "a status write that lands reports true");
        expect(!db.updateScheduleStatus(sid + 100000, S::Done),
               "a status write that matches no row reports false (nothing landed)");

        {  // Pure ordering pin — cheap, no contention: a lost persist must never reach the recorder.
            bool recorderCalled = false;
            const ScheduledStart r = beginScheduledStart([] { return false; },
                                                         [&] { recorderCalled = true; return true; });
            expect(r == ScheduledStart::NotPersisted && !recorderCalled,
                   "beginScheduledStart: persist fails -> the recorder is never started");
        }

        // ---- The genuine failure: another connection holds BEGIN IMMEDIATE (the WAL writer lock),
        // exactly what a VOD sync's bulk transaction does. busy_timeout=5000, so this waits ~5 s.
        sqlite3* locker = nullptr;
        const bool lockOpened = sqlite3_open(utf8FromWide(dbPath).c_str(), &locker) == SQLITE_OK;
        const bool locked =
            lockOpened && sqlite3_exec(locker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
        expect(locked, "lost-write fixture: a second connection holds the writer lock");
        if (locked) {
            bool recorderStarted = false;
            const auto t0 = std::chrono::steady_clock::now();
            const ScheduledStart r = beginScheduledStart(
                [&] { return db.updateScheduleStatus(sid, S::Recording, L"C:\\rec\\lost.ts"); },
                [&] { recorderStarted = true; return true; });
            const long long waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - t0).count();
            // Proves the false came from WAITING ON THE LOCK and giving up — not from some other,
            // instant failure that would also return false and make this test pass for the wrong
            // reason. (busy_timeout is 5000 ms; allow scheduler slack below it.)
            expect(waited >= 4000,
                   "the contended write genuinely waited out busy_timeout (" + std::to_string(waited) +
                       " ms) before reporting the loss");
            expect(r == ScheduledStart::NotPersisted,
                   "beginScheduledStart reports NotPersisted when the Recording write is lost");
            expect(!recorderStarted,
                   "...and did NOT start the recorder (no un-owned recording to be truncated later)");
            sqlite3_exec(locker, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        if (locker) sqlite3_close(locker);

        const ScheduledRecording after = rowOf(sid);
        expect(after.status == S::Pending && after.filePath.empty(),
               "the lost write left the row Pending with no file path — nothing half-written");
        {
            const SchedulerPlan p = planScheduler({after}, 6000, false);
            expect(p.start.size() == 1 && p.start[0] == sid,
                   "...so the next tick re-emits the start: the retry is clean");
        }

        {  // Uncontended, the same call starts normally and the row records ownership + path.
            bool recorderStarted = false;
            const ScheduledStart r = beginScheduledStart(
                [&] { return db.updateScheduleStatus(sid, S::Recording, L"C:\\rec\\ok.ts"); },
                [&] { recorderStarted = true; return true; });
            const ScheduledRecording row = rowOf(sid);
            expect(r == ScheduledStart::Started && recorderStarted && row.status == S::Recording &&
                       row.filePath == L"C:\\rec\\ok.ts",
                   "uncontended: Started, recorder running, row Recording with its path");
        }
        {  // Recorder refuses AFTER the row says Recording: the caller is told so it can write Failed.
            const ScheduledStart r = beginScheduledStart(
                [&] { return db.updateScheduleStatus(sid, S::Recording, L"C:\\rec\\ok.ts"); },
                [] { return false; });
            expect(r == ScheduledStart::RecorderFailed && rowOf(sid).status == S::Recording,
                   "recorder refuses -> RecorderFailed, and the row still reads Recording (caller "
                   "must write Failed)");
            expect(db.updateScheduleStatus(sid, S::Failed) && rowOf(sid).status == S::Failed,
                   "...which it can: Failed lands and the row is inert");
        }
        expect(db.deleteSchedule(sid), "deleteSchedule reports a DELETE that landed");
    }

    // The write-behind for lost TERMINAL statuses — and the defect adversarial review found in its
    // first draft: scheduled_recordings.id is a plain INTEGER PRIMARY KEY, so SQLite hands a deleted
    // top id to the next insert, and a decision keyed by id alone stamped itself onto that unrelated
    // new schedule. These run the real overlay/flush functions the Win32 tick calls, against REAL
    // rowid reuse in this schema, with the real updateScheduleStatus as the writer.
    out("== Lost-status write-behind vs rowid reuse ==\n");
    {
        using S = ScheduleStatus;
        auto mkRow = [](long long start, const wchar_t* url) {
            ScheduledRecording s;
            s.channelName = L"CNN";
            s.streamUrl = url;
            s.title = L"News";
            s.startUtc = start;
            s.stopUtc = start + 1000;
            s.mux = L"ts";
            s.createdAt = 4242;
            return s;
        };
        const long long idA = db.addSchedule(mkRow(10000, L"http://s/a"));
        const long long idB = db.addSchedule(mkRow(20000, L"http://s/b"));  // the TOP id
        expect(idA > 0 && idB > idA, "write-behind fixture: two schedules, B holds the top id");
        // A MISSING row must not look like a real one: a default ScheduledRecording reads Pending,
        // which is exactly what several assertions below expect — they would pass on a vanished row.
        auto findRow = [](const std::vector<ScheduledRecording>& rows, long long id) {
            for (const ScheduledRecording& r : rows)
                if (r.id == id) return r;
            ScheduledRecording none;
            none.id = -1;
            none.status = ScheduleStatus::Failed;  // no assertion in this block expects Failed
            return none;
        };
        int writes = 0;
        auto writer = [&](long long id, S s) { ++writes; return db.updateScheduleStatus(id, s); };

        // As if the user's Cancel of B had been LOST: the decision is pinned to B's identity.
        PendingStatusWrites pending;
        pending[idB] = pendingWriteFor(findRow(db.listSchedules(), idB), S::Cancelled);
        expect(pendingWriteTarget(db.listSchedules(), idB, pending[idB]) == PendingWriteTarget::SameRow,
               "a remembered decision targets its own row while that row exists");
        {
            auto view = db.listSchedules();
            overlayPendingWrites(view, pending);
            expect(findRow(view, idB).status == S::Cancelled && findRow(view, idA).status == S::Pending,
                   "the overlay shows the planner the decision, and ONLY on its own row");
        }

        // The user deletes B (the manager's Remove). The decision's row is gone.
        expect(db.deleteSchedule(idB), "write-behind fixture: B deleted");
        expect(pendingWriteTarget(db.listSchedules(), idB, pending[idB]) == PendingWriteTarget::Absent,
               "...its decision now has no target (Absent)");
        writes = 0;
        expect(flushPendingWrites(db.listSchedules(), pending, writer) == 0 && writes == 0 &&
                   pending.count(idB) == 1,
               "an Absent decision is never written (no UPDATE to wait on the lock) and is kept");

        // A NEW, unrelated schedule is inserted — and SQLite gives it B's old id. This is the premise
        // of the whole defect; if it ever stops holding, the assertions after it test nothing.
        const long long idC = db.addSchedule(mkRow(30000, L"http://s/c"));
        expect(idC == idB, "premise: SQLite reuses the deleted top rowid for the next insert (got " +
                               std::to_string(idC) + ", B was " + std::to_string(idB) + ")");
        expect(pendingWriteTarget(db.listSchedules(), idC, pending[idB]) == PendingWriteTarget::Reused,
               "the reused id is recognised as a DIFFERENT row (Reused)");
        {
            auto view = db.listSchedules();
            overlayPendingWrites(view, pending);
            // Backed by the planner, not just the status field: inside C's window (30000..31000) it
            // must actually be STARTED. This is the regression the block exists to catch — an overlay
            // that dropped or relabelled the reused row would leave C silently never recording.
            const SchedulerPlan p = planScheduler(view, 30500, false);
            expect(findRow(view, idC).status == S::Pending &&
                       std::find(p.start.begin(), p.start.end(), idC) != p.start.end(),
                   "the new schedule does NOT inherit the dead row's Cancelled — the planner starts it");
        }
        writes = 0;
        expect(flushPendingWrites(db.listSchedules(), pending, writer) == 0 && writes == 0 &&
                   pending.empty(),
               "the stale decision is discarded unwritten");
        expect(findRow(db.listSchedules(), idC).status == S::Pending,
               "...and the new schedule is still Pending in the DB — nothing was stamped onto it");

        {  // One writer lock: after the first lost write, a flush must stop writing (each further
           // attempt would be another busy_timeout stall on the UI thread) — but keep every entry.
            PendingStatusWrites two;
            two[idA] = pendingWriteFor(findRow(db.listSchedules(), idA), S::Missed);
            two[idC] = pendingWriteFor(findRow(db.listSchedules(), idC), S::Missed);
            int attempts = 0;
            const int landed = flushPendingWrites(db.listSchedules(), two,
                                                  [&](long long, S) { ++attempts; return false; });
            expect(landed == 0 && attempts == 1 && two.size() == 2,
                   "a flush stops writing after the first loss, and keeps every entry for next time");
        }

        // The ordinary retry: a decision for a row that still exists lands and is removed.
        pending[idA] = pendingWriteFor(findRow(db.listSchedules(), idA), S::Missed);
        writes = 0;
        expect(flushPendingWrites(db.listSchedules(), pending, writer) == 1 && writes == 1 &&
                   pending.empty() && findRow(db.listSchedules(), idA).status == S::Missed,
               "a SameRow decision is written, lands, and leaves the queue");

        expect(db.deleteSchedule(idA) && db.deleteSchedule(idC),
               "write-behind fixture: cleaned up for later blocks");
    }

    // The mac glue around the shared write-behind (mac/src/app/ScheduleStatusWriter.h) — the
    // object the scheduler tick AND the Recordings window write every terminal status through —
    // driven against the same genuine writer lock (busy_timeout ~5 s per lost write).
    out("== mac ScheduleStatusWriter: lost writes remembered, overlaid, replayed ==\n");
    {
        using S = ScheduleStatus;
        rabbitears::mac::ScheduleStatusWriter w(&db);
        RecordingRule rule;
        rule.titleMatch = L"Bulletin";
        rule.channelId = L"cnn.us";
        rule.channelName = L"CNN";
        rule.mux = L"ts";
        rule.enabled = true;
        rule.createdAt = 7000;
        const long long ruleId = db.addRule(rule);
        auto mk = [&](long long start, const wchar_t* url, long long rid) {
            ScheduledRecording s;
            s.channelId = L"cnn.us";
            s.channelName = L"CNN";
            s.streamUrl = url;
            s.title = L"Bulletin";
            s.startUtc = start;
            s.stopUtc = start + 1000;
            s.mux = L"ts";
            s.createdAt = 7100;
            s.ruleId = rid;
            return s;
        };
        const long long a = db.addSchedule(mk(50000, L"http://s/ra", ruleId));  // will be Skipped
        const long long b = db.addSchedule(mk(52000, L"http://s/rb", ruleId));  // a plain prediction
        expect(ruleId > 0 && a > 0 && b > a, "writer fixture: a rule with two queued airings");
        auto rowIn = [](const std::vector<ScheduledRecording>& rows, long long id) {
            for (const auto& r : rows)
                if (r.id == id) return r;
            ScheduledRecording none;
            none.id = -1;
            none.status = S::Failed;
            return none;
        };
        const ScheduledRecording rowA = rowIn(db.listSchedules(), a);

        sqlite3* locker = nullptr;
        const bool locked = sqlite3_open(utf8FromWide(dbPath).c_str(), &locker) == SQLITE_OK &&
                            sqlite3_exec(locker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK;
        expect(locked, "writer fixture: a second connection holds the writer lock");
        if (locked) {
            // The user Skips airing A while a VOD sync holds the lock: the write is LOST...
            expect(!w.setTerminal(a, &rowA, S::Skipped) && w.pendingCount() == 1,
                   "a lost Skip is reported (false) AND remembered");
            // ...but the planner never sees it as Pending: inside A's window it must NOT start.
            const auto view = w.listAsDecided();
            const SchedulerPlan p = planScheduler(view, 50500, false);
            expect(rowIn(view, a).status == S::Skipped && p.start.empty(),
                   "the overlay shows the Skip to the planner, so the skipped airing does not record");
            expect(w.hasLiveDecision(view, rowIn(view, a)) && !w.hasLiveDecision(view, rowIn(view, b)),
                   "hasLiveDecision: true for the un-landed row only");
            // A rule delete now would drop A (raw Pending) and lose the Skip: refused.
            expect(!w.deleteRule(ruleId), "rule delete DEFERRED while an airing's Skip has not landed");
            sqlite3_exec(locker, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        if (locker) sqlite3_close(locker);
        bool ruleStill = false;
        for (const auto& r : db.listRules()) ruleStill = ruleStill || r.id == ruleId;
        expect(ruleStill && rowIn(db.listSchedules(), a).status == S::Pending,
               "...the rule is still there, and A is still raw Pending (the Skip has not landed)");

        // The lock is gone: a rule EDIT clears the predictions — sparing nothing now, because the
        // flush inside it lands the Skip first, turning A into a real tombstone that stays.
        w.clearRulePredictions(ruleId);
        const auto after = db.listSchedules();
        expect(w.pendingCount() == 0 && rowIn(after, a).status == S::Skipped && rowIn(after, b).id == -1,
               "after the lock: the Skip lands (A kept as a Skipped tombstone), B's prediction cleared");
        expect(w.deleteRule(ruleId), "with nothing un-landed, the rule deletes");
        expect(db.deleteSchedule(a), "writer fixture: cleaned up");
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
