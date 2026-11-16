// SPDX-License-Identifier: GPL-3.0-or-later
//
// macOS core self-test — the GUI-free proof that the SHARED core (M3U parser +
// SQLite store + dock-layout logic) is genuinely portable, built and run on
// clang. It reuses the exact core code the Windows app uses; only the harness
// (temp dir / env / file delete) is portable POSIX/std instead of the Win32
// calls in src/cli/RabbitEarsCli.cpp (which is Windows-only: wmain, GetTempPathW,
// DeleteFileW, SetConsoleOutputCP). Assertions mirror RabbitEarsCli --selftest.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>
#include <cstdio>

#include <arpa/inet.h>   // the local HTTP server
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>        // a real .xml.gz body
#include <cstdlib>  // setenv
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>
#include <string>
#include <vector>

#include "core/GuideSources.h"
#include "core/M3uParser.h"
#include "core/RecordingRules.h"
#include "core/SearchFold.h"
#include "core/UrlCanon.h"
#include "core/XmltvParser.h"
#include "core/XtreamClient.h"  // XtreamCreds — the UrlRedact pin against its path encoder
#include "core/Strings.h"  // shared i18n catalog — completeness + placeholder-parity gate below
#include "db/Database.h"
#include "platform/Encoding.h"  // non-Windows branch of the shared header
#include "platform/TimeZone.h"
#include "platform/UrlRedact.h"
#include "ui/DockLayout.h"
#include "ui/CatchupSync.h"
#include "ui/GuideModel.h"
#include "ui/PhotoCells.h"

#include "GuideRefresh.h"  // Refresh Guide's worker — run against a local HTTP server below
#include "MeterModel.h"  // mac-local meter model (rabbitears::mac) — codecs tested below
#include "ScheduleStatusWriter.h"  // mac scheduler write-behind glue — tested below
#include "core/RecordingScheduler.h"
#include "sqlite3.h"  // second connections (genuine lock contention) + hand-built tables

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


// ---- a tiny local HTTP server, for the Refresh Guide worker test ------------------------------
// Serves fixed bodies by path on 127.0.0.1 (an ephemeral port), "HTTP/1.1 200" or 404, one request
// per connection; runs until stop(). Enough for NSURLSession, nothing more.
class LocalHttp {
public:
    explicit LocalHttp(std::map<std::string, std::string> bodies) : bodies_(std::move(bodies)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 16) != 0) return;
        socklen_t len = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { loop(); });
    }
    ~LocalHttp() { stop(); }
    int port() const { return port_; }
    int requests() const { return requests_; }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    void loop() {
        while (!stop_) {
            pollfd pf{fd_, POLLIN, 0};
            if (::poll(&pf, 1, 100) <= 0) continue;
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
            }
            ++requests_;
            const size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
            const std::string path = sp1 == std::string::npos ? "" : req.substr(sp1 + 1, sp2 - sp1 - 1);
            const auto it = bodies_.find(path);
            std::string resp;
            if (it == bodies_.end()) {
                resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            } else {
                resp = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                       std::to_string(it->second.size()) + "\r\nConnection: close\r\n\r\n" + it->second;
            }
            for (size_t off = 0; off < resp.size();) {
                const ssize_t n = ::send(c, resp.data() + off, resp.size() - off, 0);
                if (n <= 0) break;
                off += static_cast<size_t>(n);
            }
            ::close(c);
        }
    }
    std::map<std::string, std::string> bodies_;
    int fd_ = -1, port_ = 0;
    std::atomic<int> requests_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// gzip `raw` (a real .xml.gz body, as a provider serves it — the worker must gunzip it).
std::string gzipped(const std::string& raw) {
    z_stream z{};
    deflateInit2(&z, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);  // +16 = gzip wrapper
    std::string out(deflateBound(&z, raw.size()) + 32, '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data()));
    z.avail_in = static_cast<uInt>(raw.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

// An XMLTV timestamp for `utc`.
std::string xmltvTime(long long utc) {
    const std::time_t t = static_cast<std::time_t>(utc);
    std::tm g{};
    gmtime_r(&t, &g);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d%H%M%S +0000", &g);
    return b;
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

    // The five blocks below are PORTED from Win32/cli/RabbitEarsCli.cpp --selftest (temp-file
    // deletes and sqlite3_open16 swapped for portable ones): the shared programme search, search
    // marking, guide-row build, other-connection store and channel search the mac guide now calls,
    // proven under Apple clang. Keep them in step with the Win32 originals.
    const std::wstring wdir = wideFromUtf8(dir.string());
    out("== Programme search (schema v10, FTS5) ==\n");
    {
        using State = Database::ProgrammeSearchState;
        const std::wstring spath = wdir + L"/search_selftest.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(spath + sfx)), ec_); }
        const long long now = 1'800'000'000;  // a fixed "now": everything below is relative to it
        auto mk = [](const wchar_t* ch, long long s, long long e, const wchar_t* t, const wchar_t* d) {
            Programme p;
            p.channelId = ch;
            p.startUtc = s;
            p.stopUtc = e;
            p.title = t;
            p.descr = d;
            return p;
        };
        auto titles = [](const std::vector<Database::ProgrammeHit>& hs) {
            std::wstring s;
            for (const auto& h : hs) s += (s.empty() ? L"" : L"|") + h.programme.title;
            return s;
        };
        auto chan = [](const wchar_t* name, const wchar_t* tvg, const wchar_t* url) {
            ParsedChannel c;
            c.name = name;
            c.tvgId = tvg;
            c.streamUrl = url;
            return c;
        };
        long long p1 = 0, p2 = 0;
        {
            Database sdb;
            expect(sdb.open(spath), "search: a fresh DB opens");
            expect(sdb.programmeSearchState() == State::NeedsRebuild,
                   "search: a fresh DB reaches v10 (tables exist, index not built yet)");
            p1 = sdb.addPlaylist(L"P1", L"http://p1", true, 1000, L"http://p1/epg");
            p2 = sdb.addPlaylist(L"P2", L"http://p2", true, 1000, L"http://p2/epg");
            sdb.bulkInsertChannels(p1, {chan(L"CBC Toronto", L"CBCToronto.ca@HD", L"http://s/1"),
                                        chan(L"Global", L"Global.ca", L"http://s/2"),
                                        chan(L"ERT", L"Ert.gr", L"http://s/3"),
                                        chan(L"BBC", L"bbc.uk", L"http://s/4")},
                                   1000);
            sdb.bulkInsertChannels(p2, {chan(L"Disabled Chan", L"dis.xx", L"http://s/5")}, 1000);
            std::vector<Programme> g1 = {
                mk(L"cbctoronto.ca", now + 100, now + 200, L"Toronto Tonight", L"local news"),
                mk(L"global.ca", now + 300, now + 400, L"The Québec Hour", L"a tour of Montréal"),
                mk(L"ert.gr", now + 500, now + 600, L"ΕΛΛΑΔΑ Σήμερα", L""),
                mk(L"ert.gr", now + 610, now + 620, L"ΣΉΜΕΡΑ", L""),    // a capital with a tonos
                mk(L"ert.gr", now + 630, now + 640, L"Ο κόσμος", L""),  // a final sigma
                mk(L"cbctoronto.ca", now - 900, now - 100, L"Past Toronto Show", L"already over"),
                mk(L"unknown.xx", now + 100, now + 200, L"Hidden Toronto", L"no such channel"),
                mk(L"bbc.uk", now + 900, now + 999, L"Doctor Who", L"the doctor travels in time"),
                mk(L"bbc.uk", now + 700, now + 800, L"Who Knows", L"a quiz about the doctor"),
                mk(L"bbc.uk", now + 50, now + 60, L"Crime (Beat) \"AND\" OR*", L"literal syntax"),
            };
            for (int k = 0; k < 30; ++k)  // 30 upcoming "Loop" shows on an allowed channel...
                g1.push_back(mk(L"global.ca", now + 1000 + k, now + 2000 + k, L"Loop Show", L""));
            for (int k = 0; k < 5; ++k)   // ...and 5 EARLIER ones the user cannot play
                g1.push_back(mk(L"unknown.xx", now + 10 + k, now + 20 + k, L"Loop Hidden", L""));
            expect(sdb.bulkInsertProgrammes(p1, g1, 5000) == static_cast<int>(g1.size()),
                   "search: fixture guide stored");
            expect(sdb.bulkInsertProgrammes(p2, {mk(L"dis.xx", now + 100, now + 200,
                                                    L"Toronto Disabled", L"")}, 5000) == 1,
                   "search: second playlist's guide stored");
            expect(sdb.rebuildProgrammeIndex() && sdb.programmeSearchState() == State::Ready,
                   "search: rebuildProgrammeIndex -> Ready");
            // Load the channel set while P2 is still enabled, THEN disable it: its channel stays in
            // the set, so only the query's own enabled-playlist guard can drop its programme.
            expect(sdb.refreshProgrammeSearchChannels(), "search: channel set loaded");
            sdb.setPlaylistEnabled(p2, false);

            auto r = sdb.searchProgrammes(L"ronto", now, 200);
            expect(titles(r) == L"Toronto Tonight",
                   "search: title substring; past, unknown-channel and disabled-playlist rows dropped (got " +
                       utf8FromWide(titles(r)) + ")");
            expect(r.size() == 1 && r[0].inTitle &&
                       r[0].markedTitle.find(L'\x02') != std::wstring::npos &&
                       r[0].markedTitle.find(L'\x03') != std::wstring::npos,
                   "search: a title match is marked (U+0002 ... U+0003)");
            r = sdb.searchProgrammes(L"quebec", now, 200);
            expect(titles(r) == L"The Québec Hour",
                   "search: accent-insensitive title ('quebec' finds 'Québec')");
            expect(r.size() == 1 && r[0].markedTitle == L"The \x02Québec\x03 Hour",
                   "search: ... and marks the accented 'Québec' it found");
            r = sdb.searchProgrammes(L"ελλαδα", now, 200);
            expect(!r.empty(), "search: non-Latin case folding (Greek lower finds upper)");
            expect(r.size() == 1 && r[0].markedTitle == L"\x02ΕΛΛΑΔΑ\x03 Σήμερα",
                   "search: ... and marks the upper-case Greek it found");
            r = sdb.searchProgrammes(L"σήμερα", now, 200);
            expect(titles(r) == L"ΕΛΛΑΔΑ Σήμερα|ΣΉΜΕΡΑ" && r[0].markedTitle == L"ΕΛΛΑΔΑ \x02Σήμερα\x03" &&
                       r[1].markedTitle == L"\x02ΣΉΜΕΡΑ\x03",
                   "search: a Greek word with a tonos finds AND marks its capitals (Ή = ή) (got " +
                       utf8FromWide(titles(r)) + ")");
            r = sdb.searchProgrammes(L"ΚΌΣΜΟΣ", now, 200);
            expect(titles(r) == L"Ο κόσμος" && r[0].markedTitle == L"Ο \x02κόσμος\x03",
                   "search: Greek capitals find AND mark a final sigma (Σ = ς) (got " + utf8FromWide(titles(r)) +
                       ")");
            r = sdb.searchProgrammes(L"montreal", now, 200);
            expect(r.size() == 1 && !r[0].inTitle && !r[0].snippet.empty(),
                   "search: a description word matches accent-insensitively ('montreal' -> 'Montréal')");
            expect(r.size() == 1 && r[0].snippet == L"a tour of \x02Montréal\x03",
                   "search: ... and the snippet marks 'Montréal'");
            // searchFold's Latin edges: accents dropped, letters of their own kept (lower-cased).
            expect(searchFold(L'É') == L'e' && searchFold(L'ç') == L'c' && searchFold(L'Ł') == L'l' &&
                       searchFold(L'Ÿ') == L'y' && searchFold(L'ÿ') == L'y' && searchFold(L'Ĺ') == L'l' &&
                       searchFold(L'ž') == L'z' && searchFold(L'ſ') == L's',
                   "searchFold: Latin accents fold to the base letter");
            expect(searchFold(L'Æ') == L'æ' && searchFold(L'Œ') == L'œ' && searchFold(L'Ĳ') == L'ĳ' &&
                       searchFold(L'ß') == L'ß' && searchFold(L'×') == L'×' && searchFold(L'÷') == L'÷' &&
                       searchFold(L'ı') == L'ı' && searchFold(L'ĸ') == L'ĸ',
                   "searchFold: ligatures and letters of their own keep their identity");
            expect(searchFold(L'Σ') == L'σ' && searchFold(L'Ж') == L'ж' && searchFold(L'Ё') == L'ё' &&
                       searchFold(L'東') == L'東' && searchFold(L'Z') == L'z' && searchFold(L'-') == L'-',
                   "searchFold: Greek + Cyrillic case; CJK and punctuation unchanged");
            expect(searchFold(L'Ά') == L'ά' && searchFold(L'Έ') == L'έ' && searchFold(L'Ί') == L'ί' &&
                       searchFold(L'Ό') == L'ό' && searchFold(L'Ώ') == L'ώ' && searchFold(L'Ϋ') == L'ϋ' &&
                       searchFold(L'ς') == L'σ' && searchFold(L'ά') == L'ά',
                   "searchFold: Greek as FTS5 folds it (case only, the tonos kept; final sigma = sigma)");
            expect(searchFold(L'Ș') == L's' && searchFold(L'ș') == L's' && searchFold(L'Ț') == L't' &&
                       searchFold(L'ț') == L't',
                   "searchFold: Romanian comma-below letters");
            expect(searchFold(L'µ') == L'μ' && searchFold(L'Ґ') == L'ґ' && searchFold(L'ґ') == L'ґ',
                   "searchFold: micro sign and Ukrainian Ґ, as FTS5 folds them");
            r = sdb.searchProgrammes(L"local", now, 200);
            expect(r.size() == 1 && !r[0].inTitle &&
                       r[0].snippet == L"\x02local\x03 news",
                   "search: a description-only match carries a marked snippet");
            expect(r.size() == 1 && r[0].channelName == L"CBC Toronto" && r[0].channelTvgId == L"CBCToronto.ca@HD",
                   "search: a hit names the user's channel and its FULL tvg-id (for Play/Schedule)");
            {  // the TV Guide's coverage count: distinct guide ids, as the guide joins them, live only
                const long long p3 = sdb.addPlaylist(L"P3", L"http://p3", true, 1000);
                ParsedChannel mv = chan(L"A Movie", L"movie.id", L"http://s/m1");
                mv.kind = Channel::Kind::Movie;
                sdb.bulkInsertChannels(p3, {chan(L"CNN HD", L"CNN.us@HD", L"http://s/c1"),
                                            chan(L"CNN SD", L"cnn.us@SD", L"http://s/c2"),
                                            chan(L"Local", L"", L"http://s/c3"), mv},
                                       1000);
                std::map<long long, int> ids;
                for (const auto& [plId, count] : sdb.distinctLiveGuideIds()) ids[plId] = count;
                expect(ids.size() == 3 && ids[p1] == 4 && ids[p2] == 1 && ids[p3] == 1,
                       "distinctLiveGuideIds: per playlist (disabled ones too); CNN.us@HD = cnn.us@SD; no-id "
                       "rows and movies not counted");
            }
            r = sdb.searchProgrammes(L"travel", now, 200);
            expect(titles(r) == L"Doctor Who" && !r[0].inTitle,
                   "search: the last description word is a prefix ('travel' finds 'travels')");
            expect(titles(sdb.searchProgrammes(L"doctor", now, 200)) == L"Doctor Who|Who Knows",
                   "search: title matches rank before description-only ones, even when later");
            expect(titles(sdb.searchProgrammes(L"(Beat) \"AND\" OR*", now, 200)) ==
                       L"Crime (Beat) \"AND\" OR*",
                   "search: typed FTS5 syntax is searched literally");
            for (const wchar_t* odd : {L"-", L"\"", L"NEAR(", L"a\"b", L"*", L"AND", L"(((", L"  "}) {
                sdb.searchProgrammes(odd, now, 200);  // must not throw, crash or poison the connection
            }
            expect(titles(sdb.searchProgrammes(L"ronto", now, 200)) == L"Toronto Tonight",
                   "search: odd input left the connection usable");
            expect(titles(sdb.searchProgrammes(L"to", now, 200)).find(L"Toronto Tonight") != std::wstring::npos,
                   "search: a 2-character term falls back to a title scan");
            bool more = false;
            r = sdb.searchProgrammes(L"loop", now, 10, &more);
            expect(r.size() == 10 && more && titles(r).find(L"Hidden") == std::wstring::npos,
                   "search: the limit counts only playable rows, and reports there were more");

            // A refresh makes the index stale: until rebuilt it is never READ (LIKE answers).
            expect(sdb.bulkInsertProgrammes(p1, {mk(L"cbctoronto.ca", now + 100, now + 200,
                                                    L"Fresh Title", L"brand new description")},
                                            6000) == 1,
                   "search: refresh stored");
            expect(sdb.programmeSearchState() == State::NeedsRebuild,
                   "search: a refresh leaves NeedsRebuild");
            expect(titles(sdb.searchProgrammes(L"fresh", now, 200)) == L"Fresh Title" &&
                       sdb.searchProgrammes(L"ronto", now, 200).empty(),
                   "search: a stale index is bypassed (LIKE finds the new title, not the old)");
            expect(sdb.rebuildProgrammeIndex() && titles(sdb.searchProgrammes(L"fresh", now, 200)) ==
                                                       L"Fresh Title",
                   "search: after the rebuild the index finds the new title");
            sdb.deletePlaylist(p2);
            expect(sdb.programmeSearchState() == State::NeedsRebuild,
                   "search: deleting a playlist leaves NeedsRebuild");
            expect(sdb.rebuildProgrammeIndex(), "search: rebuilt after the delete");
        }
        {   // A FRESH connection must read the saved stamp as current — else every launch would pay a
            // full rebuild (~1.5 s on the owner's guide) before its first search.
            Database sdb;
            expect(sdb.open(spath), "search: reopens after the rebuild");
            expect(sdb.programmeSearchState() == State::Ready,
                   "search: a new connection finds the saved index current (no rebuild per launch)");
        }
        // An OLDER build's refresh (no re-index), simulated on a raw connection: a same-sized
        // re-import that reuses the same ids — only epg_refreshed_<id> tells it apart.
        auto rawExec = [&spath](const char* sql) {
            sqlite3* raw = nullptr;
            bool ok = false;
            if (sqlite3_open(utf8FromWide(spath).c_str(), &raw) == SQLITE_OK)
                ok = sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
            if (raw) sqlite3_close(raw);
            return ok;
        };
        const std::string oldRefresh =
            "BEGIN; UPDATE epg_programmes SET title='Old Build Title';"
            " INSERT INTO settings(key,value) VALUES('epg_refreshed_" + std::to_string(p1) +
            "','7777') ON CONFLICT(key) DO UPDATE SET value=excluded.value; COMMIT;";
        expect(rawExec(oldRefresh.c_str()), "search: simulated older-build refresh applied");
        {
            Database sdb;
            expect(sdb.open(spath), "search: reopens");
            expect(sdb.programmeSearchState() == State::NeedsRebuild,
                   "search: an older build's same-sized refresh is detected (epg_refreshed changed)");
        }
        // v9 -> v10 on an existing library must NOT re-run v9's URL rewrite (a canary row in a
        // spelling v9 would rewrite), and a failed v10 must leave v9 with search still answering.
        // A REAL v9 database has no channel index either (v11): drop it too, so the retry below builds
        // it from scratch — the v9 -> v10 -> v11 path in one open.
        const std::string downgrade =
            "BEGIN; DROP TABLE epg_fts_title; DROP TABLE epg_fts_descr;"
            " DROP TRIGGER channels_fts_ai; DROP TRIGGER channels_fts_ad; DROP TRIGGER channels_fts_au;"
            " DROP TABLE channels_fts;"
            " UPDATE channels SET stream_url='http://Canary.EXAMPLE:80/x.ts' WHERE name='Global';"
            " CREATE TABLE epg_fts_title(x); PRAGMA user_version=9; COMMIT;";
        expect(rawExec(downgrade.c_str()), "search: fixture turned into a v9 DB with a squatter table");
        expect(canonicalStreamUrl(L"http://Canary.EXAMPLE:80/x.ts") != L"http://Canary.EXAMPLE:80/x.ts",
               "search: the canary is a spelling v9 WOULD rewrite (else the next checks prove nothing)");
        auto canaryUrl = [](Database& d, long long pid) {
            for (const auto& c : d.channelsByPlaylist(pid))
                if (c.name == L"Global") return c.streamUrl;
            return std::wstring(L"<missing>");
        };
        {
            Database sdb;
            expect(sdb.open(spath), "search: the v9 DB opens");
            expect(sdb.programmeSearchState() == State::Unavailable,
                   "search: v10 failed (a non-FTS table squats the name) -> Unavailable, not a crash");
            expect(canaryUrl(sdb, p1) == L"http://Canary.EXAMPLE:80/x.ts",
                   "search: v9's URL rewrite was NOT re-run on a v9 DB");
            sdb.refreshProgrammeSearchChannels();
            expect(titles(sdb.searchProgrammes(L"brand new", now, 200)) == L"Old Build Title",
                   "search: with no index, LIKE still searches descriptions");
        }
        expect(rawExec("DROP TABLE epg_fts_title;"), "search: squatter removed");
        {
            Database sdb;
            expect(sdb.open(spath), "search: reopens after the squatter is gone");
            expect(sdb.programmeSearchState() == State::NeedsRebuild,
                   "search: the retried v10 lands on the next open");
            expect(canaryUrl(sdb, p1) == L"http://Canary.EXAMPLE:80/x.ts",
                   "search: ...still without re-running v9");
            expect(sdb.channelSearchIndexed() &&
                       rawExec("INSERT INTO channels_fts(channels_fts, rank) VALUES('integrity-check', 1)") &&
                       !sdb.searchChannels(L"toronto").empty(),
                   "search: ...and v11 follows in the same open, the channel index built from the v9 rows");
        }
        {   // Performance guard: a large synthetic guide stays fast to search — THROUGH THE INDEX.
            Database sdb;
            expect(sdb.open(spath), "search/perf: opens");
            std::vector<Programme> big;
            big.reserve(40000);
            for (int k = 0; k < 40000; ++k)
                big.push_back(mk(L"global.ca", now + k, now + k + 30,
                                 (L"Show " + std::to_wstring(k % 997) + L" news").c_str(),
                                 L"the evening news and weather"));
            expect(sdb.bulkInsertProgrammes(p1, big, 8000) == 40000, "search/perf: 40,000 stored");
            expect(sdb.rebuildProgrammeIndex() && sdb.programmeSearchState() == State::Ready,
                   "search/perf: indexed (so the timing below is the index path, not LIKE)");
            sdb.refreshProgrammeSearchChannels();
            const auto t0 = std::chrono::steady_clock::now();
            const auto hits = sdb.searchProgrammes(L"news", now, 200);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            expect(hits.size() == 200 && ms < 500,
                   "search: 40,000 programmes, a word in every row, 200 results in " + std::to_string(ms) +
                       " ms (< 500)");
        }
        {   // One playlist: a refresh empties the table, so its new rows REUSE the old ids — a stale
            // index read by mistake would answer "alpha" with whatever now sits at alpha's id. Plus
            // two description cases the marking must handle: CJK (no word boundaries) and a word
            // right after typographic punctuation.
            const std::wstring rpath = wdir + L"/search_reuse.db";
            for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(rpath + sfx)), ec_); }
            Database rdb;
            expect(rdb.open(rpath), "search/reuse: opens");
            const long long rp = rdb.addPlaylist(L"R", L"http://r", true, 1000, L"http://r/epg");
            rdb.bulkInsertChannels(rp, {chan(L"Chan", L"chan.xx", L"http://s/9")}, 1000);
            expect(rdb.bulkInsertProgrammes(rp, {mk(L"chan.xx", now + 10, now + 20, L"Alpha Show", L"")},
                                            5000) == 1,
                   "search/reuse: first guide stored");
            expect(rdb.rebuildProgrammeIndex() && rdb.refreshProgrammeSearchChannels(),
                   "search/reuse: indexed");
            const auto a = rdb.searchProgrammes(L"alpha", now, 200);
            expect(titles(a) == L"Alpha Show", "search/reuse: 'alpha' found through the index");
            expect(rdb.bulkInsertProgrammes(rp, {mk(L"chan.xx", now + 10, now + 20, L"Beta Show", L"")},
                                            6000) == 1,
                   "search/reuse: refreshed (index now stale)");
            const auto b = rdb.searchProgrammes(L"beta", now, 200);
            expect(!a.empty() && b.size() == 1 && b[0].id == a[0].id,
                   "search/reuse: the refresh really reused alpha's id (else the next check proves nothing)");
            expect(rdb.searchProgrammes(L"alpha", now, 200).empty(),
                   "search/reuse: the stale index is not read (it would answer 'alpha' with 'Beta Show')");

            const std::wstring tokyo = L"東京都の天気予報です";
            expect(rdb.bulkInsertProgrammes(
                       rp, {mk(L"chan.xx", now + 10, now + 20, L"Weather", tokyo.c_str()),
                            mk(L"chan.xx", now + 30, now + 40, L"Special",
                               L"It’s a “Doctor Who” special")},
                       7000) == 2 &&
                       rdb.rebuildProgrammeIndex(),
                   "search/reuse: CJK + punctuation fixture indexed");
            auto w = rdb.searchProgrammes(L"天気", now, 200);  // 2 CJK characters: a whole word
            expect(titles(w) == L"Weather" && !w[0].inTitle &&
                       w[0].snippet == L"東京都の\x02天気\x03予報です",
                   "search/reuse: a 2-character CJK word is found in a description, and marked mid-run");
            expect(titles(rdb.searchProgrammes(L"天気予", now, 200)) == L"Weather",
                   "search/reuse: a 3-character CJK term (index path) still searches descriptions");
            auto d = rdb.searchProgrammes(L"doctor", now, 200);
            expect(titles(d) == L"Special" && d[0].snippet.find(L"\x02" L"Doctor\x03") != std::wstring::npos,
                   "search/reuse: a word right after a curly quote is found AND marked");
        }
    }


    out("== Search marking follows the match (core/SearchFold ftsFold) ==\n");
    {
        // 1. ftsFold IS FTS5's fold: every BMP and plane-1 character (plane 1 holds every cased script
        //    beyond the BMP; FTS5 folds only Deseret there), through the vendored SQLite's own two tokenizers.
        sqlite3* fdb = nullptr;
        const bool opened =
            sqlite3_open(":memory:", &fdb) == SQLITE_OK &&
            sqlite3_exec(fdb,
                         "CREATE VIRTUAL TABLE t USING fts5(x, tokenize='trigram remove_diacritics 1');"
                         "CREATE VIRTUAL TABLE ti USING fts5vocab(t, 'instance');"
                         "CREATE VIRTUAL TABLE u USING fts5(x, tokenize='unicode61 remove_diacritics 2');"
                         "CREATE VIRTUAL TABLE ui USING fts5vocab(u, 'instance');",
                         nullptr, nullptr, nullptr) == SQLITE_OK;
        auto utf8Of = [](char32_t cp) {
            std::string s;
            if (cp < 0x80) {
                s += static_cast<char>(cp);
            } else if (cp < 0x800) {
                s += static_cast<char>(0xC0 | (cp >> 6));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                s += static_cast<char>(0xE0 | (cp >> 12));
                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                s += static_cast<char>(0xF0 | (cp >> 18));
                s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (cp & 0x3F));
            }
            return s;
        };
        auto codePoints32 = [](const std::string& s) {  // strict enough for SQLite's own output
            std::vector<char32_t> v;
            for (size_t i = 0; i < s.size();) {
                const unsigned char b = static_cast<unsigned char>(s[i]);
                const int n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
                char32_t cp = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
                for (int k = 1; k < n && i + k < s.size(); ++k)
                    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
                v.push_back(cp);
                i += n;
            }
            return v;
        };
        std::vector<char32_t> cps;
        for (char32_t cp = 1; cp <= 0xFFFF; ++cp)
            if (cp < 0xD800 || cp > 0xDFFF) cps.push_back(cp);
        for (char32_t cp = 0x10000; cp <= 0x1FFFF; ++cp) cps.push_back(cp);
        std::unordered_map<long long, std::string> tri, uni;
        bool likeAsciiOnly = false, accentInsideToken = false;
        if (opened) {
            sqlite3_exec(fdb, "BEGIN", nullptr, nullptr, nullptr);
            sqlite3_stmt* insT = nullptr;
            sqlite3_stmt* insU = nullptr;
            sqlite3_prepare_v2(fdb, "INSERT INTO t(rowid, x) VALUES (?, ?)", -1, &insT, nullptr);
            sqlite3_prepare_v2(fdb, "INSERT INTO u(rowid, x) VALUES (?, ?)", -1, &insU, nullptr);
            for (char32_t cp : cps) {
                const std::string one = utf8Of(cp), three = one + one + one;  // trigram: ONE token
                sqlite3_bind_int64(insT, 1, cp);
                sqlite3_bind_text(insT, 2, three.c_str(), static_cast<int>(three.size()), SQLITE_TRANSIENT);
                sqlite3_step(insT);
                sqlite3_reset(insT);
                sqlite3_bind_int64(insU, 1, cp);
                sqlite3_bind_text(insU, 2, one.c_str(), static_cast<int>(one.size()), SQLITE_TRANSIENT);
                sqlite3_step(insU);
                sqlite3_reset(insU);
            }
            sqlite3_finalize(insT);
            sqlite3_finalize(insU);
            sqlite3_exec(fdb, "COMMIT", nullptr, nullptr, nullptr);
            for (auto [sql, into] : {std::pair<const char*, std::unordered_map<long long, std::string>*>{
                                         "SELECT doc, term FROM ti", &tri},
                                     {"SELECT doc, term FROM ui", &uni}}) {
                sqlite3_stmt* q = nullptr;
                sqlite3_prepare_v2(fdb, sql, -1, &q, nullptr);
                while (sqlite3_step(q) == SQLITE_ROW)
                    (*into)[sqlite3_column_int64(q, 0)] = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
                sqlite3_finalize(q);
            }
            // LIKE's own rule, which the Like marking mirrors: ASCII letters case-blind, the rest exact.
            sqlite3_stmt* q = nullptr;
            sqlite3_prepare_v2(fdb, "SELECT 'Af' LIKE 'aF', 'Á' LIKE 'a', 'Á' LIKE 'á', 'Ｑ' LIKE 'ｑ'", -1, &q,
                               nullptr);
            likeAsciiOnly = sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0) == 1 &&
                            sqlite3_column_int(q, 1) == 0 && sqlite3_column_int(q, 2) == 0 &&
                            sqlite3_column_int(q, 3) == 0;
            sqlite3_finalize(q);
            // unicode61 keeps a dropped accent INSIDE its token ("e + U+0301 cole" is one word, "ecole"),
            // which is why marking skips it within a word and looking back for a word's start.
            accentInsideToken =
                sqlite3_exec(fdb,
                             "CREATE VIRTUAL TABLE w USING fts5(x, tokenize='unicode61 remove_diacritics 2');"
                             "CREATE VIRTUAL TABLE wi USING fts5vocab(w, 'instance');"
                             "INSERT INTO w(x) VALUES ('e\xCC\x81" "cole');",
                             nullptr, nullptr, nullptr) == SQLITE_OK;
            q = nullptr;
            sqlite3_prepare_v2(fdb, "SELECT group_concat(term, '|') FROM wi", -1, &q, nullptr);
            accentInsideToken = accentInsideToken && sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0) &&
                                std::string(reinterpret_cast<const char*>(sqlite3_column_text(q, 0))) == "ecole";
            sqlite3_finalize(q);
            sqlite3_close(fdb);
        }
        size_t bad = 0, badUni = 0, dropped = 0;
        std::string firstBad;
        for (char32_t cp : cps) {
            char32_t want = 0;  // no trigram at all: the tokenizer drops it
            if (const auto t = tri.find(cp); t != tri.end()) {
                const std::vector<char32_t> v = codePoints32(t->second);
                want = (v.size() == 3 && v[0] == v[1] && v[1] == v[2]) ? v[0] : 0xFFFFFFFF;
            } else {
                ++dropped;
            }
            if (ftsFoldCodePoint(cp) != want) {
                ++bad;
                if (firstBad.size() < 120) {
                    char b[40];
                    snprintf(b, sizeof b, " U+%04X->%X(fts %X)", static_cast<unsigned>(cp),
                             static_cast<unsigned>(ftsFoldCodePoint(cp)), static_cast<unsigned>(want));
                    firstBad += b;
                }
            }
            if (const auto u = uni.find(cp); u != uni.end()) {  // a character unicode61 keeps
                const std::vector<char32_t> v = codePoints32(u->second);
                if (v.size() != 1 || v[0] != want) ++badUni;
            }
        }
        expect(opened && !tri.empty() && bad == 0,
               "ftsFold == the vendored FTS5's fold for all " + std::to_string(cps.size()) +
                   " code points (BMP + plane 1; trigram; " + std::to_string(bad) + " differ" + firstBad + ")");
        expect(opened && uni.size() > 40000 && badUni == 0,
               "ftsFold: unicode61 (descriptions) folds every character it keeps as trigram does (" +
                   std::to_string(uni.size()) + " kept, " + std::to_string(badUni) + " differ)");
        expect(dropped > 0 && ftsFold(L'\u0301') == 0 && ftsFoldCodePoint(0x10400) == 0x10428,
               "ftsFold: combining accents the tokenizers drop fold to 0; Deseret capitals fold (+40)");
        expect(likeAsciiOnly, "LIKE (vendored SQLite): ASCII letters case-blind, accents and fullwidth exact");
        expect(accentInsideToken, "unicode61 (vendored SQLite) keeps a dropped accent inside its word (e+U+0301 "
                                  "cole -> 'ecole')");
        // 2. searchFold: ftsFold one character for one, plus the stroke letters FTS5 keeps.
        expect(searchFold(L'ộ') == L'o' && searchFold(L'Ǎ') == L'a' && searchFold(L'Ａ') == L'ａ' &&
                   searchFold(L'Ƀ') == L'ƀ' && searchFold(L'\u2126') == L'ω' && searchFold(L'\u0301') == L'\u0301',
               "searchFold: FTS5's fold beyond Latin-1/Extended-A (Vietnamese, pinyin, fullwidth, Latin "
               "Extended-B, the ohm sign); a dropped accent stays itself (one for one)");
        expect(searchFold(L'Ø') == L'o' && searchFold(L'đ') == L'd' && searchFold(L'Ħ') == L'h' &&
                   searchFold(L'ŀ') == L'l' && searchFold(L'Ł') == L'l' && searchFold(L'ŧ') == L't' &&
                   ftsFold(L'Ł') == L'ł' && ftsFold(L'Ø') == L'ø',
               "searchFold folds the stroke letters Ø Đ Ħ Ŀ Ł Ŧ; ftsFold keeps them, as FTS5 does");

        // 3. Marking, through the real search: marked the way each result was matched.
        const std::wstring mpath = wdir + L"/mark_selftest.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(mpath + sfx)), ec_); }
        const long long now = 1'800'000'000;
        auto mk = [](long long s, const wchar_t* t, const wchar_t* d) {
            Programme p;
            p.channelId = L"mark.tv";
            p.startUtc = s;
            p.stopUtc = s + 5;
            p.title = t;
            p.descr = d;
            return p;
        };
        auto marked = [](const std::vector<Database::ProgrammeHit>& hs) {
            std::wstring s;
            for (const auto& h : hs) s += (s.empty() ? L"" : L"|") + (h.inTitle ? h.markedTitle : h.snippet);
            return s;
        };
        Database mdb;
        const bool mopen = mdb.open(mpath);
        const long long mp = mopen ? mdb.addPlaylist(L"M", L"http://m", true, 1000, L"http://m/epg") : 0;
        ParsedChannel mc;
        mc.name = L"Mark TV";
        mc.tvgId = L"mark.tv";
        mc.streamUrl = L"http://s/m";
        std::wstring pasted;  // a pasted sentence longer than the excerpt's 90 characters after a match
        for (int k = 0; k < 20; ++k) pasted += (k ? L" " : L"") + std::wstring(L"qwert");
        const std::wstring pastedDescr = L"x " + pasted + L" y" + std::wstring(50, L'z');
        const std::vector<Programme> mg = {
            mk(now + 10, L"Hà Nội Tonight", L""),
            mk(now + 20, L"Lodz vs Łódź", L""),
            mk(now + 30, L"Que\u0301bec Morning", L""),  // decomposed: e + a combining acute
            mk(now + 40, L"Cafe\u0301 Hour", L""),
            mk(now + 50, L"ＮＨＫ World", L""),
            mk(now + 60, L"África or Africa", L""),
            mk(now + 70, L"Weather", L"明日の東京 天気 予報、天気"),
            mk(now + 80, L"Evening", L"the sunnews hour"),
            mk(now + 90, L"VTV Morning", L"tin tức Hà Nội hôm nay"),
            mk(now + 100, L"東京 天気 特集", L""),
            // A spaced term matched deep into a long description with no space after it: the excerpt
            // must not end inside the match.
            mk(now + 110, L"Long Weather", (std::wstring(L"明日の大阪 天気") + std::wstring(100, L'晴')).c_str()),
            mk(now + 120, L"Accent Start", L"x \u0301abcd"),  // a dropped accent before a word
            mk(now + 130, L"Pasted", pastedDescr.c_str()),
        };
        expect(mopen && mp > 0 && mdb.bulkInsertChannels(mp, {mc}, 1000) == 1 &&
                   mdb.bulkInsertProgrammes(mp, mg, 5000) == static_cast<int>(mg.size()) &&
                   mdb.rebuildProgrammeIndex() && mdb.refreshProgrammeSearchChannels(),
               "mark: fixture guide stored and indexed");
        std::wstring got = marked(mdb.searchProgrammes(L"noi", now, 200));
        expect(got == L"Hà \x02Nội\x03 Tonight|tin tức Hà \x02Nội\x03 hôm nay",
               "mark: Vietnamese 'noi' found by the index AND marked on 'Nội', in a title and a description (got " +
                   utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"lodz", now, 200));
        expect(got == L"\x02Lodz\x03 vs Łódź",
               "mark: 'lodz' marks 'Lodz' only - the index keeps Ł, so 'Łódź' is not what it matched (got " +
                   utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"quebec", now, 200));
        expect(got == L"\x02Que\u0301bec\x03 Morning",
               "mark: a decomposed 'Québec' is marked whole, its combining accent inside (got " + utf8FromWide(got) +
                   ")");
        got = marked(mdb.searchProgrammes(L"que\u0301bec", now, 200));
        expect(got == L"\x02Que\u0301bec\x03 Morning",
               "mark: a combining accent TYPED in the search is skipped too (got " + utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"bec", now, 200));
        expect(got == L"Que\u0301\x02" L"bec\x03 Morning",
               "mark: an occurrence never starts on a dropped accent (it belongs to the letter before) (got " +
                   utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"cafe", now, 200));
        expect(got == L"\x02" L"Cafe\u0301\x03 Hour",
               "mark: a combining accent on the last letter is inside the mark (got " + utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"ｎｈｋ", now, 200));
        expect(got == L"\x02ＮＨＫ\x03 World",
               "mark: fullwidth letters fold case as the index does (got " + utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"af", now, 200));
        expect(got == L"C\x02" L"af\x03" L"e\u0301 Hour|África or \x02" L"Af\x03rica",
               "mark: a 2-character term (LIKE) marks as LIKE matched - ASCII case only, 'Áf' left (got " +
                   utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"東京 天気", now, 200));
        expect(got == L"\x02東京 天気\x03 特集|明日の\x02東京 天気\x03 予報、天気",
               "mark: a CJK term - its title by the index, a description (LIKE) as the whole term once, not each "
               "word (got " + utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"大阪 天気", now, 200));
        expect(got == L"明日の\x02大阪 天気\x03" + std::wstring(85, L'晴') + L"…",
               "mark: an excerpt never ends inside a spaced term's match (got " + utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(L"abcd", now, 200));
        expect(got == L"x \u0301\x02" L"abcd\x03",
               "mark: a word start is found looking back past an accent the index drops (got " + utf8FromWide(got) +
                   ")");
        expect(mdb.bulkInsertProgrammes(mp, mg, 6000) == static_cast<int>(mg.size()) &&
                   mdb.programmeSearchState() != Database::ProgrammeSearchState::Ready,
               "mark: (fixture) the guide stored again - the index is stale, so searches use LIKE");
        got = marked(mdb.searchProgrammes(L"news", now, 200));
        expect(got == L"the sun\x02news\x03 hour",
               "mark: a description LIKE found mid-word is marked there (got " + utf8FromWide(got) + ")");
        got = marked(mdb.searchProgrammes(pasted, now, 200));
        expect(got == L"x \x02" + pasted + L"\x03…",
               "mark: an excerpt runs past its 90 characters to the end of a longer match (got " + utf8FromWide(got) +
                   ")");
    }


    out("== TV Guide rows + coverage (buildGuideModel) ==\n");
    {
        const std::wstring gpath = wdir + L"/guide_selftest.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(gpath + sfx)), ec_); }
        const long long now = 1'800'000'000;
        auto ch = [](const wchar_t* name, const wchar_t* tvg, const wchar_t* url, int chno = -1,
                     Channel::Kind kind = Channel::Kind::Live) {
            ParsedChannel c;
            c.name = name;
            c.tvgId = tvg;
            c.streamUrl = url;
            c.chno = chno;
            c.kind = kind;
            return c;
        };
        auto pr = [](const wchar_t* cid, long long s, long long e, const wchar_t* t) {
            Programme p;
            p.channelId = cid;
            p.startUtc = s;
            p.stopUtc = e;
            p.title = t;
            p.descr = std::wstring(L"about ") + t;
            return p;
        };
        Database gdb;
        expect(gdb.open(gpath), "guide: a fresh DB opens");
        // A: has a guide link. Two feeds of one channel, where channel number, insertion order and
        // name disagree about which comes first (channel number decides — channelsByPlaylist's
        // order); a film carrying a guide id; a channel the guide has nothing for; one with no id.
        const long long pa = gdb.addPlaylist(L"A", L"http://a", true, 1000, L"http://a/epg");
        gdb.bulkInsertChannels(pa, {ch(L"CNN HD", L"CNN.us@HD", L"http://s/a1", 9),
                                    ch(L"CNN SD", L"cnn.us@SD", L"http://s/a2", 5),
                                    ch(L"A Film", L"movie.xx", L"http://s/a3", -1, Channel::Kind::Movie),
                                    ch(L"BBC", L"BBC.uk", L"http://s/a4"),
                                    ch(L"Quiet", L"none.xx", L"http://s/a5"),
                                    ch(L"Plain", L"", L"http://s/a6")},
                               1000);
        // B: no guide link; one of its ids is on A's guide. C: disabled, with a guide of its own.
        const long long pb = gdb.addPlaylist(L"B", L"http://b", true, 1000);
        gdb.bulkInsertChannels(pb, {ch(L"Local", L"local.xx", L"http://s/b1"),
                                    ch(L"Zed", L"zzz.xx", L"http://s/b2")},
                               1000);
        const long long pc = gdb.addPlaylist(L"C", L"http://c", true, 1000, L"http://c/epg");
        gdb.bulkInsertChannels(pc, {ch(L"Hidden", L"dis.xx", L"http://s/c1")}, 1000);
        expect(gdb.bulkInsertProgrammes(pa, {pr(L"cnn.us", now + 3600, now + 7200, L"Late"),
                                             pr(L"cnn.us", now - 600, now + 3600, L"Early"),
                                             pr(L"cnn.us", now - 90000, now - 86400, L"Yesterday"),  // out of the window
                                             pr(L"bbc.uk", now, now + 1800, L"Football"),
                                             pr(L"movie.xx", now, now + 1800, L"Film Guide"),
                                             pr(L"zzz.xx", now, now + 1800, L"Zed Show"),
                                             pr(L"other.xx", now, now + 1800, L"Nobody's")},
                                        now) == 7,
               "guide: A's guide stored");
        expect(gdb.bulkInsertProgrammes(pc, {pr(L"dis.xx", now, now + 1800, L"Disabled Show")}, now) == 1,
               "guide: C's guide stored");
        gdb.setPlaylistEnabled(pc, false);

        // The join's channel list is channelsByPlaylist's live, id-carrying channels, in its order.
        auto viaList = [&](long long pid) {
            std::vector<std::pair<std::wstring, std::wstring>> v;
            for (const auto& c : gdb.channelsByPlaylist(pid))
                if (!c.tvgId.empty() && c.kind == Channel::Kind::Live) v.emplace_back(c.name, c.tvgId);
            return v;
        };
        std::map<long long, std::vector<std::pair<std::wstring, std::wstring>>> viaQuery;
        long long lastPid = -1;
        bool grouped = true;  // each playlist's channels arrive together, playlists in id order
        for (const auto& c : gdb.liveGuideChannels()) {
            if (c.playlistId < lastPid) grouped = false;
            lastPid = c.playlistId;
            viaQuery[c.playlistId].emplace_back(c.name, c.tvgId);
        }
        expect(grouped && viaQuery[pa] == viaList(pa) && viaQuery[pb] == viaList(pb) &&
                   viaQuery[pc] == viaList(pc) && viaQuery[pa].size() == 4,
               "guide: liveGuideChannels = channelsByPlaylist's live channels with an id, same order");

        const GuideModel m = buildGuideModel(gdb, now);
        std::wstring got;
        for (const auto& r : m.rows) {
            got += (got.empty() ? L"" : L" | ") + r.channelName + L" [" + r.channelId + L"]:";
            for (const auto& p : r.programmes) got += L" " + p.title;
        }
        expect(got == L"BBC [BBC.uk]: Football | CNN SD [cnn.us@SD]: Early Late",
               "guide: rows by name; the lower channel number names a shared id; films, the disabled "
               "playlist and the out-of-window show left out (got " + utf8FromWide(got) + ")");
        expect(!m.rows.empty() && m.rows[0].programmes[0].descr == L"about Football",
               "guide: a programme's description reaches its block");
        const GuideCoverage& cv = m.coverage;
        expect(cv.valid && cv.withId == 5 && cv.shown == 2 && cv.noProgrammes == 1 && cv.inNoLinkPlaylists == 2 &&
                   cv.guideUnmatched == 2,
               "guide: coverage — 5 ids, 2 shown, 1 without programmes, 2 in the unlinked playlist, "
               "2 guide channels matching none (the film's and other.xx; zzz.xx is B's) (got " +
                   std::to_string(cv.withId) + "/" + std::to_string(cv.shown) + "/" +
                   std::to_string(cv.noProgrammes) + "/" + std::to_string(cv.inNoLinkPlaylists) + "/" +
                   std::to_string(cv.guideUnmatched) + ")");
    }


    out("== Guide stored on another connection (Win32's store worker) ==\n");
    {
        using State = Database::ProgrammeSearchState;
        const std::wstring rpath = wdir + L"/store2_selftest.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(rpath + sfx)), ec_); }
        const long long now = 1'800'000'000;
        auto pr = [](const wchar_t* cid, long long s, const wchar_t* t) {
            Programme p;
            p.channelId = cid;
            p.startUtc = s;
            p.stopUtc = s + 600;
            p.title = t;
            return p;
        };
        auto titles = [](const std::vector<Database::ProgrammeHit>& hs) {
            std::wstring s;
            for (const auto& h : hs) s += (s.empty() ? L"" : L"|") + h.programme.title;
            return s;
        };
        auto chan = [](const wchar_t* name, const wchar_t* tvg, const wchar_t* url) {
            ParsedChannel c;
            c.name = name;
            c.tvgId = tvg;
            c.streamUrl = url;
            return c;
        };
        Database ui;  // the app's connection: it searches, and remembers what it learnt about the index
        expect(ui.open(rpath), "store2: the app's connection opens");
        const long long p1 = ui.addPlaylist(L"P1", L"http://r1", true, 1000, L"http://r1/epg");
        const long long p2 = ui.addPlaylist(L"P2", L"http://r2", true, 1000, L"http://r2/epg");
        ui.bulkInsertChannels(p1, {chan(L"One", L"one.xx", L"http://r/1")}, 1000);
        ui.bulkInsertChannels(p2, {chan(L"Two", L"two.xx", L"http://r/2")}, 1000);
        expect(ui.bulkInsertProgrammes(p1, {pr(L"one.xx", now + 100, L"Alpha Show")}, now) == 1 &&
                   ui.rebuildProgrammeIndex() && ui.refreshProgrammeSearchChannels() &&
                   titles(ui.searchProgrammes(L"alpha", now, 50)) == L"Alpha Show" &&
                   ui.programmeSearchState() == State::Ready,
               "store2: the app's connection has searched, and remembers Ready");

        Database worker;  // the store worker's connection
        expect(worker.open(rpath, nullptr, /*upgradeSchema=*/false), "store2: a worker connection opens");
        // The worker stores; its rebuild has not run yet. The new first row takes alpha's freed id, so
        // the old index would answer "alpha" with Beta Show — through the app's connection, which was
        // never told. It must notice the other connection's commit by itself.
        expect(worker.bulkInsertProgrammes(p1, {pr(L"one.xx", now + 100, L"Beta Show"),
                                                pr(L"one.xx", now + 900, L"Gamma Show")}, now) == 2,
               "store2: the worker stores a new guide");
        expect(ui.programmeSearchState() == State::NeedsRebuild,
               "store2: the app's connection sees the index went stale (another connection committed)");
        expect(ui.searchProgrammes(L"alpha", now, 50).empty() &&
                   titles(ui.searchProgrammes(L"beta", now, 50)) == L"Beta Show",
               "store2: meanwhile its search reads the new guide, not the stale index (LIKE)");
        expect(worker.rebuildProgrammeIndex() && ui.programmeSearchState() == State::Ready &&
                   titles(ui.searchProgrammes(L"show", now, 50)) == L"Beta Show|Gamma Show",
               "store2: after the worker's rebuild, Ready again, and the index answers");

        // An error that ends the whole transaction (here a trigger's RAISE(ROLLBACK); for real, a full
        // disk) must stop the store: nothing after it may commit on its own in autocommit.
        expect(worker.bulkInsertProgrammes(p2, {pr(L"two.xx", now + 200, L"Delta Show")}, now) == 1,
               "store2: P2's guide stored");
        {
            sqlite3* raw = nullptr;
            sqlite3_open(utf8FromWide(rpath).c_str(), &raw);
            const bool made = raw && sqlite3_exec(raw,
                                                  "CREATE TRIGGER boom BEFORE INSERT ON epg_programmes"
                                                  " WHEN NEW.title='BOOM' BEGIN SELECT RAISE(ROLLBACK,'boom'); END",
                                                  nullptr, nullptr, nullptr) == SQLITE_OK;
            sqlite3_close(raw);
            expect(made, "store2: (fixture) a trigger that rolls the transaction back");
        }
        const std::wstring refreshedBefore = worker.getSetting(L"epg_refreshed_" + std::to_wstring(p1)).value_or(L"");
        const int boom = worker.bulkInsertProgrammes(p1, {pr(L"one.xx", now + 100, L"Zeta Show"),
                                                          pr(L"one.xx", now + 200, L"BOOM"),
                                                          pr(L"one.xx", now + 300, L"Eta Show")}, now + 5);
        std::wstring p1Titles;
        for (const auto& p : worker.programmesInWindow(p1, 0, now + 100000))
            p1Titles += (p1Titles.empty() ? L"" : L"|") + p.title;
        expect(boom == 0 && p1Titles == L"Beta Show|Gamma Show" &&
                   worker.getSetting(L"epg_refreshed_" + std::to_wstring(p1)).value_or(L"") == refreshedBefore,
               "store2: a rolled-back store leaves the old guide whole — no rows after it committed alone (got " +
                   utf8FromWide(p1Titles) + ")");
        expect(worker.lastError().find(L"boom") != std::wstring::npos,
               "store2: and says why (lastError)");
        expect(worker.bulkInsertProgrammes(p2, {pr(L"two.xx", now + 400, L"Theta Show")}, now) == 1,
               "store2: the connection is not left inside a transaction — the next store works");

        // Failures that end only their STATEMENT must still roll the whole guide back: the old guide's
        // DELETE, and the refresh-time write (a stamp left unchanged could match the old index's).
        auto rawExec = [&](const char* sql) {
            sqlite3* raw = nullptr;
            sqlite3_open(utf8FromWide(rpath).c_str(), &raw);
            const bool ok = raw && sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
            sqlite3_close(raw);
            return ok;
        };
        auto p1Now = [&]() {
            std::wstring t;
            for (const auto& p : worker.programmesInWindow(p1, 0, now + 100000)) t += (t.empty() ? L"" : L"|") + p.title;
            return t;
        };
        expect(rawExec("CREATE TRIGGER nodel BEFORE DELETE ON epg_programmes WHEN OLD.title='Gamma Show'"
                       " BEGIN SELECT RAISE(ABORT,'nodel'); END"),
               "store2: (fixture) a DELETE that fails on its own");
        expect(worker.bulkInsertProgrammes(p1, {pr(L"one.xx", now + 100, L"Kappa Show")}, now + 6) == 0 &&
                   p1Now() == L"Beta Show|Gamma Show" && worker.lastError().find(L"nodel") != std::wstring::npos,
               "store2: a failed DELETE stores nothing — no new rows on top of the old guide (got " +
                   utf8FromWide(p1Now()) + ")");
        expect(rawExec("DROP TRIGGER nodel") &&
                   rawExec("CREATE TRIGGER noset BEFORE UPDATE ON settings WHEN NEW.key LIKE 'epg_refreshed_%'"
                           " BEGIN SELECT RAISE(ABORT,'noset'); END"),
               "store2: (fixture) a refresh-time write that fails on its own");
        expect(worker.bulkInsertProgrammes(p1, {pr(L"one.xx", now + 100, L"Kappa Show")}, now + 7) == 0 &&
                   p1Now() == L"Beta Show|Gamma Show" &&
                   worker.getSetting(L"epg_refreshed_" + std::to_wstring(p1)).value_or(L"") == refreshedBefore &&
                   worker.lastError().find(L"noset") != std::wstring::npos,
               "store2: a failed refresh-time write stores nothing, and says why (got " + utf8FromWide(p1Now()) + ")");
        expect(rawExec("DROP TRIGGER noset") &&
                   rawExec("CREATE TRIGGER nomu BEFORE INSERT ON epg_programmes WHEN NEW.title='Mu Show'"
                           " BEGIN SELECT RAISE(ABORT,'nomu'); END"),
               "store2: (fixture) rows that fail on their own");
        // Every row failing keeps what there was; SOME failing stores the rest and says why.
        expect(worker.bulkInsertProgrammes(p1, {pr(L"one.xx", now + 100, L"Mu Show"),
                                                pr(L"one.xx", now + 200, L"Mu Show")}, now + 8) == 0 &&
                   p1Now() == L"Beta Show|Gamma Show" && worker.lastError().find(L"nomu") != std::wstring::npos,
               "store2: a batch whose every row fails stores nothing — the old guide stays (got " +
                   utf8FromWide(p1Now()) + ")");
        const int partial = worker.bulkInsertProgrammes(p1, {pr(L"one.xx", now + 100, L"Mu Show"),
                                                             pr(L"one.xx", now + 200, L"Nu Show")}, now + 9);
        const std::wstring afterPartial = p1Now();  // (read first: expect's arguments have no set order)
        expect(partial == 1 && afterPartial == L"Nu Show" && worker.lastError().find(L"nomu") != std::wstring::npos,
               "store2: a row failing on its own is skipped, the rest stored, the failure named (got " +
                   utf8FromWide(afterPartial) + ")");
        expect(rawExec("DROP TRIGGER nomu"), "store2: (fixture) dropped");
        // A playlist deleted while its guide downloaded: nothing lands — not even a refresh time.
        const long long gone = 987654;
        expect(worker.bulkInsertProgrammes(gone, {pr(L"one.xx", now + 100, L"Lambda Show")}, now) == 0 &&
                   worker.bulkInsertProgrammes(gone, {}, now) == 0 &&
                   !worker.getSetting(L"epg_refreshed_" + std::to_wstring(gone)).has_value() &&
                   !worker.lastError().empty(),
               "store2: a store for a deleted playlist lands nothing — not even a refresh time, even when empty");
        // An empty feed for a real playlist IS stored — the old guide replaced by nothing — with no error.
        expect(worker.bulkInsertProgrammes(p2, {}, now + 10) == 0 && worker.lastError().empty() &&
                   worker.programmesInWindow(p2, 0, now + 100000).empty(),
               "store2: an empty guide is stored (0, no error), as before");
    }


    out("== Channel search (schema v11, FTS5 + triggers) ==\n");
    {
        const std::wstring cpath = wdir + L"/chsearch.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(cpath + sfx)), ec_); }
        auto ch = [](const wchar_t* name, const wchar_t* group, const wchar_t* url,
                     Channel::Kind kind = Channel::Kind::Live) {
            ParsedChannel c;
            c.name = name;
            c.groupTitle = group;
            c.streamUrl = url;
            c.kind = kind;
            return c;
        };
        auto names = [](const std::vector<Channel>& v) {
            std::wstring s;
            for (const auto& c : v) s += (s.empty() ? L"" : L"|") + c.name;
            return s;
        };
        // A raw connection for what Database does not expose (it runs beside an open Database: WAL).
        auto rawQuery = [&](const char* sql) {
            sqlite3* r = nullptr;
            long long v = -1;
            if (sqlite3_open(utf8FromWide(cpath).c_str(), &r) == SQLITE_OK) {
                sqlite3_stmt* q = nullptr;
                if (sqlite3_prepare_v2(r, sql, -1, &q, nullptr) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
                    v = sqlite3_column_int64(q, 0);
                if (q) sqlite3_finalize(q);
            }
            if (r) sqlite3_close(r);
            return v;
        };
        auto rawExec = [&](const char* sql) {
            sqlite3* r = nullptr;
            bool ok = false;
            if (sqlite3_open(utf8FromWide(cpath).c_str(), &r) == SQLITE_OK)
                ok = sqlite3_exec(r, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
            if (r) sqlite3_close(r);
            return ok;
        };
        // FTS5's own check of an external-content index against its content table: an index out of
        // step with `channels` (a missed insert, delete or rename) fails it.
        auto indexInStep = [&] {
            return rawExec("INSERT INTO channels_fts(channels_fts, rank) VALUES('integrity-check', 1)");
        };
        const std::vector<ParsedChannel> base = {ch(L"TVA QUÉBEC HD", L"CA French", L"http://c/1"),
                                                 ch(L"CBC Toronto", L"CA English", L"http://c/2"),
                                                 ch(L"Sky Sport 1", L"DE Sport", L"http://c/3"),
                                                 ch(L"100% Hits", L"Music", L"http://c/4")};
        long long p1 = 0;
        {
            Database cdb;
            expect(cdb.open(cpath), "chsearch: a fresh DB opens");
            expect(cdb.channelSearchIndexed() && rawQuery("PRAGMA user_version") == 11,
                   "chsearch: a fresh DB reaches v11 (the channel index exists)");
            p1 = cdb.addPlaylist(L"C1", L"http://c1", true, 1000);
            expect(cdb.bulkInsertChannels(p1, base, 1000) == 4, "chsearch: 4 channels stored");
            expect(names(cdb.searchChannels(L"quebec")) == L"TVA QUÉBEC HD",
                   "chsearch: case- and accent-blind through the index ('quebec' finds 'TVA QUÉBEC HD')");
            expect(names(cdb.searchChannels(L"ky spo")) == L"Sky Sport 1", "chsearch: a substring, across a space");
            expect(cdb.searchChannels(L"french").empty(),
                   "chsearch: NAMES only — a group title no longer matches (owner, 2026-09-25)");
            expect(names(cdb.searchChannels(L"To")) == L"CBC Toronto",
                   "chsearch: 2 characters take the LIKE path, names only");
            expect(names(cdb.searchChannels(L"%")) == L"100% Hits" && cdb.searchChannels(L"_").empty(),
                   "chsearch: the LIKE path takes a typed % and _ literally");
            expect(names(cdb.searchChannels(L"0% H")) == L"100% Hits", "chsearch: ...and so does the index path");
            expect(cdb.countUncoveredChannelNames(L"toronto", {}, 100) == 1 &&
                       cdb.countUncoveredChannelNames(L"to", {}, 100) == -1,
                   "chsearch: countUncoveredChannelNames by name; -1 under 3 characters");
            // A refresh renaming a channel keeps its stream URL: the upsert's UPDATE path.
            std::vector<ParsedChannel> renamed = base;
            renamed[0].name = L"TVA Montréal HD";
            expect(cdb.bulkInsertChannels(p1, renamed, 2000) == 4, "chsearch: refreshed with one channel renamed");
            expect(cdb.searchChannels(L"quebec").empty() && names(cdb.searchChannels(L"montreal")) == L"TVA Montréal HD",
                   "chsearch: a rename reaches the index (the update trigger)");
            // Movies: searched, but not counted as channels; retired by a VOD sync; gone with their playlist.
            const long long p2 = cdb.addPlaylist(L"C2", L"http://c2", true, 1000);
            expect(cdb.bulkInsertChannels(p2, {ch(L"Toronto Story", L"Drama", L"http://c/m1", Channel::Kind::Movie),
                                               ch(L"Toronto Nights", L"Drama", L"http://c/m2", Channel::Kind::Movie)},
                                          1000) == 2,
                   "chsearch: two movies stored");
            expect(cdb.searchChannels(L"toronto").size() == 3 && cdb.countUncoveredChannelNames(L"toronto", {}, 100) == 1,
                   "chsearch: movies are searched, but not counted as live channel names");
            // Asked of the INDEX itself: searchChannels joins back to `channels`, so a stale entry for a
            // deleted row would stay invisible there — and wrong the day its rowid is reused.
            expect(cdb.retireMissingChannels(p2, static_cast<int>(Channel::Kind::Movie), {L"http://c/m1"}) == 1 &&
                       rawQuery("SELECT COUNT(*) FROM channels_fts WHERE channels_fts MATCH '\"nights\"'") == 0,
                   "chsearch: a retired movie leaves the index (the delete trigger)");
            cdb.setPlaylistEnabled(p1, false);
            expect(cdb.searchChannels(L"sky sport").empty(), "chsearch: a disabled playlist's channels are not found");
            cdb.setPlaylistEnabled(p1, true);
            cdb.deletePlaylist(p2);
            expect(rawQuery("SELECT COUNT(*) FROM channels_fts WHERE channels_fts MATCH '\"story\"'") == 0,
                   "chsearch: a deleted playlist's channels leave the index (the cascade fires the trigger)");
            // The guide note's count: a name is "not in the guide" only when none of its channels carries
            // a guide row's id — another feed of a channel the guide shows (an FHD beside the HD) is in it.
            const long long p4 = cdb.addPlaylist(L"C4", L"http://c4", true, 1000);
            auto idd = [&](const wchar_t* name, const wchar_t* tvg, const wchar_t* url) {
                ParsedChannel c = ch(name, L"CA", url);
                c.tvgId = tvg;
                return c;
            };
            expect(cdb.bulkInsertChannels(p4, {idd(L"CBC Toronto HD", L"CBC.ca@HD", L"http://c/41"),
                                               idd(L"CBC Toronto FHD", L"cbc.ca@FHD", L"http://c/42"),
                                               idd(L"Toronto One", L"one.ca", L"http://c/43"),
                                               idd(L"Toronto Radio", L"", L"http://c/44")},
                                          1000) == 4,
                   "chsearch: an id-carrying fixture stored");
            // A same-named channel elsewhere with NO id does not uncover a name the guide has (the rule
            // is "none of its channels covered", not whichever channel is read last — this one is).
            const long long p5 = cdb.addPlaylist(L"C5", L"http://c5", true, 1000);
            expect(cdb.bulkInsertChannels(p5, {ch(L"CBC Toronto HD", L"CA", L"http://c/51")}, 1000) == 1,
                   "chsearch: a same-named, id-less channel in another playlist stored");
            const std::unordered_set<std::wstring> guideRows{L"cbc.ca"};
            expect(cdb.countUncoveredChannelNames(L"toronto", guideRows, 100) == 3,
                   "chsearch: countUncoveredChannelNames skips both feeds of the guide's cbc.ca (3 = CBC Toronto, "
                   "Toronto One, Toronto Radio)");
            expect(cdb.countUncoveredChannelNames(L"toronto", guideRows, 5) == -1,
                   "chsearch: ...and gives up (-1) past maxRows matching channels (6 here)");
            // A guide row plays by tvg-id: a movie carrying the same id, listed FIRST, must not win.
            const long long p6 = cdb.addPlaylist(L"C6", L"http://c6", true, 1000);
            ParsedChannel film = idd(L"Dup Film", L"dup.xx", L"http://c/61");
            film.kind = Channel::Kind::Movie;
            expect(cdb.bulkInsertChannels(p6, {film, idd(L"Dup Live", L"dup.xx", L"http://c/62")}, 1000) == 2,
                   "chsearch: a movie and a live channel sharing a tvg-id stored (the movie first)");
            const auto byId = cdb.channelByTvgId(L"dup.xx");
            expect(byId && byId->name == L"Dup Live", "channelByTvgId: the live channel wins over a movie sorted first");
            expect(indexInStep(), "chsearch: the index is in step with channels after inserts, a rename, a "
                                  "retirement and a playlist delete (FTS5 integrity-check)");
        }
        // v10 -> v11 on an existing library: the build fills the index from the rows already there.
        expect(rawExec("BEGIN; DROP TRIGGER channels_fts_ai; DROP TRIGGER channels_fts_ad;"
                       " DROP TRIGGER channels_fts_au; DROP TABLE channels_fts; PRAGMA user_version=10; COMMIT;"),
               "chsearch: fixture turned back into a v10 DB");
        {
            Database cdb;
            expect(cdb.open(cpath) && cdb.channelSearchIndexed(), "chsearch: a v10 DB lands v11 on open");
            expect(names(cdb.searchChannels(L"montreal")) == L"TVA Montréal HD" && indexInStep(),
                   "chsearch: ...with the existing channels indexed");
        }
        // A failed v11 (an ordinary table squats the name) leaves v10 with NO triggers — channel writes
        // keep working — and search on LIKE; the next open, squatter gone, lands v11.
        expect(rawExec("BEGIN; DROP TRIGGER channels_fts_ai; DROP TRIGGER channels_fts_ad;"
                       " DROP TRIGGER channels_fts_au; DROP TABLE channels_fts; CREATE TABLE channels_fts(x);"
                       " PRAGMA user_version=10; COMMIT;"),
               "chsearch: fixture turned into a v10 DB with a squatter table");
        {
            Database cdb;
            expect(cdb.open(cpath) && !cdb.channelSearchIndexed() && rawQuery("PRAGMA user_version") == 10,
                   "chsearch: v11 failed -> stays v10, no crash");
            expect(rawQuery("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name LIKE 'channels_fts%'") == 0,
                   "chsearch: ...and left no trigger behind");
            expect(cdb.bulkInsertChannels(p1, {ch(L"Late Arrival", L"X", L"http://c/9")}, 3000) == 1,
                   "chsearch: ...so channel writes still work");
            expect(names(cdb.searchChannels(L"sky sport")) == L"Sky Sport 1", "chsearch: ...and search answers by LIKE");
        }
        expect(rawExec("DROP TABLE channels_fts;"), "chsearch: squatter removed");
        {
            Database cdb;
            expect(cdb.open(cpath) && cdb.channelSearchIndexed(), "chsearch: the retried v11 lands on the next open");
            expect(names(cdb.searchChannels(L"arrival")) == L"Late Arrival" && indexInStep(),
                   "chsearch: ...indexing the channel written while it was at v10");
        }
    }



    // Refresh Guide's worker (mac/src/app/GuideRefresh) end to end: a local HTTP server serves the
    // guides, the worker fetches them (a link naming THREE guides — one gzipped, one missing — plus
    // a one-guide playlist and a playlist whose guide 404s), merges, stores on its OWN connection and
    // re-indexes; then the guide rows (buildGuideModel) and the search see the result.
    out("== Refresh Guide worker (GuideRefresh: split, fetch, merge, store, index) ==\n");
    {
        const std::wstring rpath = wdir + L"/refresh_selftest.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(rpath + sfx)), ec_); }
        const long long now = (long long)time(nullptr);
        auto prog = [&](const char* ch, long long start, const char* title) {
            return std::string("<programme start=\"") + xmltvTime(start) + "\" stop=\"" + xmltvTime(start + 1800) +
                   "\" channel=\"" + ch + "\"><title>" + title + "</title></programme>\n";
        };
        auto tv = [](const std::string& body) { return "<?xml version=\"1.0\"?>\n<tv>\n" + body + "</tv>\n"; };
        const long long t1 = now + 600, t2 = now + 2400;
        std::map<std::string, std::string> bodies;
        bodies["/a.xml"] = tv(prog("one.uk", t1, "Alpha One") + prog("one.uk", t2, "Alpha Two") + prog("two.uk", t1, "Alpha Three"));
        // b: two.uk is a's (dropped), three.uk and film.uk are b's own.
        bodies["/b.xml.gz"] = gzipped(tv(prog("two.uk", t1, "Beta Two") + prog("three.uk", t1, "Beta Three") +
                                         prog("film.uk", t1, "Beta Film")));
        bodies["/c.xml"] = tv(prog("four.uk", t1, "Gamma Four"));
        LocalHttp srv(bodies);
        expect(srv.port() > 0, "refresh fixture: local HTTP server listening");
        const std::wstring base = L"http://127.0.0.1:" + std::to_wstring(srv.port());

        Database rdb;
        std::wstring err;
        expect(rdb.open(rpath, &err), "refresh fixture: database opens (schema created)");
        auto live = [](const wchar_t* name, const wchar_t* tvg, const wchar_t* url) {
            ParsedChannel c;
            c.name = name;
            c.tvgId = tvg;
            c.streamUrl = url;
            return c;
        };
        const std::wstring multi = base + L"/a.xml, " + base + L"/b.xml.gz , " + base + L"/missing.xml";
        const long long p1 = rdb.addPlaylist(L"Multi", L"http://p/1.m3u", true, now, multi);
        const long long p2 = rdb.addPlaylist(L"Single", L"http://p/2.m3u", true, now, base + L"/c.xml");
        const long long p3 = rdb.addPlaylist(L"Broken", L"http://p/3.m3u", true, now, base + L"/404.xml");
        ParsedChannel film = live(L"A Film", L"film.uk", L"http://s/film.mkv");
        film.kind = Channel::Kind::Movie;  // a film carrying a tvg-id must never name a guide row
        rdb.bulkInsertChannels(p1, {live(L"One", L"One.UK@HD", L"http://s/1"), live(L"Two", L"two.uk", L"http://s/2"),
                                    live(L"Three", L"three.uk", L"http://s/3"), film}, now);
        rdb.bulkInsertChannels(p2, {live(L"Four", L"four.uk", L"http://s/4")}, now);
        rdb.bulkInsertChannels(p3, {live(L"Five", L"five.uk", L"http://s/5")}, now);

        std::vector<std::wstring> lines;
        int forced = 0;
        const std::atomic<bool> noCancel{false};
        const auto res = rabbitears::mac::refreshGuides(
            {{p1, L"Multi", multi}, {p2, L"Single", base + L"/c.xml"}, {p3, L"Broken", base + L"/404.xml"}}, rpath,
            [&](const std::wstring& l, bool force) { lines.push_back(l); forced += force ? 1 : 0; }, noCancel);
        expect(res.fetches.size() == 3, "refresh: one result per playlist");
        if (res.fetches.size() == 3) {
            const auto& m = res.fetches[0];
            expect(m.error.empty() && m.stored == 5,
                   "multi-guide link: a's 3 + b's own 2 stored, b's copy of a's channel left out (got " +
                       std::to_string(m.stored) + ")");
            expect(m.sourceErrors.size() == 1 && m.sourceErrors[0].find(L"3") != std::wstring::npos,
                   "...and the missing third guide is reported beside them (\"guide 3 of 3: …\")");
            expect(m.sourceLog.size() == 2, "...with a merge log line per guide that loaded");
            expect(res.fetches[1].error.empty() && res.fetches[1].stored == 1, "a one-guide playlist stores as before");
            expect(!res.fetches[2].error.empty() && res.fetches[2].stored == 0 &&
                       res.fetches[2].error.find(L"404") != std::wstring::npos,
                   "a guide that 404s fails its playlist, saying so (" + utf8FromWide(res.fetches[2].error) + ")");
        }
        expect(res.storeError.empty() && res.index == rabbitears::mac::GuideRefreshResult::Index::Rebuilt,
               "stored on the worker's own connection, then the search index rebuilt once");
        bool sawGuideTag = false;
        for (const auto& l : lines) sawGuideTag = sawGuideTag || l.find(L"3") != std::wstring::npos;
        expect(forced >= 8 && sawGuideTag, "progress: a step line per download, parse and store (" +
                                                std::to_string(forced) + " forced lines)");
        expect(srv.requests() >= 5, "every guide was requested (" + std::to_string(srv.requests()) + " requests)");

        // The app's connection sees the worker's commits: the guide rows, without the film.
        const rabbitears::GuideModel gm = rabbitears::buildGuideModel(rdb, now);
        std::set<std::wstring> rowNames;
        for (const auto& r : gm.rows) rowNames.insert(r.channelName);
        expect(rowNames == std::set<std::wstring>{L"One", L"Two", L"Three", L"Four"},
               "guide rows: the four live channels with programmes — never the film carrying a tvg-id");
        expect(rdb.programmeSearchState() == Database::ProgrammeSearchState::Ready,
               "the app's connection reads the worker's index as current");
        rdb.refreshProgrammeSearchChannels();
        const auto hits = rdb.searchProgrammes(L"Beta", now, 50);
        std::set<std::wstring> titles;
        for (const auto& h : hits) titles.insert(h.programme.title);
        expect(titles == std::set<std::wstring>{L"Beta Three"},
               "search: b's own live programme is found; its copy of a's channel was never stored, the film is no row");

        // A cancel before it starts stores nothing.
        const std::atomic<bool> cancelled{true};
        const auto none = rabbitears::mac::refreshGuides({{p2, L"Single", base + L"/c.xml"}}, rpath,
                                                         [](const std::wstring&, bool) {}, cancelled);
        expect(none.fetches.size() == 1 && !none.fetches[0].error.empty() && none.fetches[0].stored == 0,
               "cancelled (quit): nothing fetched or stored");
        srv.stop();
        rdb.close();
    }

    // PORTED from Win32/cli/RabbitEarsCli.cpp: catch-up's shared parts — Xtream stream ids and
    // timeshift URLs, the server-clock decisions (CatchupSync, through THIS platform's TimeZone),
    // channel_archive, the archive search — under Apple clang and NSTimeZone.
    out("== Catch-up (Xtream archives: stream ids, timeshift URLs, channel_archive, past results) ==\n");
    {
        // Stream ids: live forms only — a film's or an episode's id is another namespace.
        expect(xtreamLiveStreamId(L"http://h:80/u/p/12345") == 12345 &&
                   xtreamLiveStreamId(L"http://h/live/u/p/678.ts") == 678 &&
                   xtreamLiveStreamId(L"http://h/live/u/p/42.m3u8?token=x") == 42,
               "catchup: live stream ids — /u/p/ID, /live/u/p/ID.ext, a query ignored");
        expect(xtreamLiveStreamId(L"http://h/movie/u/p/99.mp4") == 0 &&
                   xtreamLiveStreamId(L"http://h/series/u/p/5.mkv") == 0 &&
                   xtreamLiveStreamId(L"http://h/timeshift/u/p/60/2026-09-24:10-00/123.ts") == 0 &&
                   xtreamLiveStreamId(L"http://h/u/p/abc") == 0 && xtreamLiveStreamId(L"http://h/a/b/c/d") == 0 &&
                   xtreamLiveStreamId(L"http://h/u/p/1234567890123456") == 0 &&  // 16 digits: no overflow
                   xtreamLiveStreamId(L"not a url") == 0,
               "catchup: not live stream ids — movie, series, timeshift, non-numeric, too long, other shapes");
        {
            XtreamCreds mine;
            mine.origin = L"http://h";
            mine.username = L"movie";  // a username that is also a path word
            mine.password = L"p+w";
            expect(xtreamLiveStreamId(L"http://h/movie/p+w/77") == 77 &&
                       xtreamLiveStreamId(L"http://h/movie/p%2Bw/77", &mine) == 77 &&
                       xtreamLiveStreamId(L"http://h/live/movie/p+w/78.ts", &mine) == 78 &&
                       xtreamLiveStreamId(L"http://h/other/p+w/77", &mine) == 0 &&
                       xtreamLiveStreamId(L"http://h/movie/nope/77", &mine) == 0,
                   "catchup: stream ids — a user named \"movie\"; with a login, only URLs carrying it (decoded)");
        }
        // Timeshift URLs: the start on the SERVER's clock, the login encoded for a path.
        XtreamCreds cr;
        cr.origin = L"http://h:8080";
        cr.username = L"us+er";
        cr.password = L"p@ss";
        expect(xtreamTimeshiftUrl(cr, 891260, 1790325000, 55, 7200) ==
                   L"http://h:8080/timeshift/us%2Ber/p%40ss/55/2026-09-25:10-30/891260.ts",
               "catchup: timeshift URL — server-local start (+2 h), login path-encoded");
        expect(xtreamTimeshiftUrl(cr, 7, 1798759800, 30, 3600).find(L"/2027-01-01:00-30/") != std::wstring::npos &&
                   xtreamTimeshiftUrl(cr, 7, 1798759800, 30, -18000).find(L"/2026-12-31:18-30/") != std::wstring::npos,
               "catchup: timeshift URL — across midnight and the year, both directions");
        expect(xtreamTimeshiftUrl(cr, 0, 1798759800, 30, 0).empty() && xtreamTimeshiftUrl(cr, 7, 1798759800, 0, 0).empty() &&
                   xtreamTimeshiftUrl(XtreamCreds{}, 7, 1798759800, 30, 0).empty(),
               "catchup: timeshift URL — empty without an id, a duration or a login");
        // The server's clock from the account probe.
        XtreamAccount acct;
        expect(parseXtreamAccount(R"({"user_info":{"auth":1,"status":"Active"},"server_info":{"timezone":)"
                                  R"("Europe/Amsterdam","time_now":"2026-07-27 14:39:34","timestamp_now":1785155974}})",
                                  acct) &&
                   acct.timezone == L"Europe/Amsterdam" && acct.serverLocalTime == 1785163174,
               "catchup: account — the server's time zone and wall clock");
        auto offsetOf = [](long long local, long long utc) {
            XtreamAccount a;
            a.serverLocalTime = local;
            a.serverTime = utc;
            int o = 99999;
            return xtreamServerUtcOffset(a, &o) ? o : 99999;
        };
        const long long T = 1785155974;
        expect(offsetOf(T + 7200, T) == 7200 && offsetOf(T + 7201, T) == 7200 && offsetOf(T + 7199, T) == 7200 &&
                   offsetOf(T - 12600, T) == -12600 && offsetOf(T + 20700, T) == 20700 && offsetOf(T, T) == 0,
               "catchup: offset — rounded to the quarter hour: CEST, a second either side, Newfoundland -3:30, "
               "Nepal +5:45, UTC");
        expect(offsetOf(T + 20 * 3600, T) == 99999 && offsetOf(T - 13 * 3600, T) == 99999 &&
                   offsetOf(0, T) == 99999 && offsetOf(T, 0) == 99999,
               "catchup: offset — none past -12 h / +14 h, or without both readings (the stored one is kept)");
        XtreamAccount noClock;
        expect(parseXtreamAccount(R"({"user_info":{"auth":1},"server_info":{"time_now":"yesterday",)"
                                  R"("timestamp_now":1785155974}})", noClock) &&
                   noClock.serverLocalTime == 0 && offsetOf(noClock.serverLocalTime, noClock.serverTime) == 99999,
               "catchup: account — an unreadable time_now gives no offset, not garbage");
        XtreamAccount feb31;
        expect(parseXtreamAccount(R"({"user_info":{"auth":1},"server_info":{"time_now":"2026-02-31 10:00:00"}})",
                                  feb31) &&
                   feb31.serverLocalTime == 0,
               "catchup: account — a date that does not exist (31 February) is refused");
        XtreamAccount feb29, leap29;
        expect(parseXtreamAccount(R"({"user_info":{"auth":1},"server_info":{"time_now":"2026-02-29 10:00:00"}})",
                                  feb29) &&
                   parseXtreamAccount(R"({"user_info":{"auth":1},"server_info":{"time_now":"2028-02-29 10:00:00"}})",
                                      leap29) &&
                   feb29.serverLocalTime == 0 && leap29.serverLocalTime == 1835431200,
               "catchup: account — 29 February only in a leap year (2026 refused, 2028 read)");
        // Which streams keep an archive — every spelling the panels use.
        std::vector<XtreamArchive> arch;
        size_t total = 0;
        expect(parseXtreamLiveArchive(R"([{"stream_id":1,"tv_archive":1,"tv_archive_duration":"3"},)"
                                      R"({"stream_id":"2","tv_archive":"1","tv_archive_duration":2},)"
                                      R"({"stream_id":3,"tv_archive":0,"tv_archive_duration":"3"},)"
                                      R"({"stream_id":4,"tv_archive":1,"tv_archive_duration":0},)"
                                      R"({"stream_id":0,"tv_archive":1,"tv_archive_duration":2},)"
                                      R"({"stream_id":5,"tv_archive":true,"tv_archive_duration":"1"}])",
                                      arch, &total) &&
                   total == 6 && arch.size() == 3 && arch[0].streamId == 1 && arch[0].days == 3 &&
                   arch[1].streamId == 2 && arch[1].days == 2 && arch[2].streamId == 5 && arch[2].days == 1,
               "catchup: get_live_streams — archived streams by any spelling; no archive, 0 days or no id skipped");
        expect(!parseXtreamLiveArchive("<html>502</html>", arch) && !parseXtreamLiveArchive(R"({"a":1})", arch),
               "catchup: get_live_streams — an error page or an object FAILS (not \"no archives\")");
        // The server's zone on the PROGRAMME's day (Win32/platform/TimeZone, the C++20 tz database).
        int summer = 0, winter = 0, ny = 0, sydney = 0, unknown = 12345;
        const bool tzOk = utcOffsetAt(L"Europe/Amsterdam", 1790325000, &summer) &&  // 2026-09-25
                          utcOffsetAt(L"Europe/Amsterdam", 1796083200, &winter) &&  // 2026-12-01
                          utcOffsetAt(L"America/New_York", 1796083200, &ny) &&
                          utcOffsetAt(L"Australia/Sydney", 1796083200, &sydney);
        expect(tzOk && summer == 7200 && winter == 3600 && ny == -18000 && sydney == 39600,
               "catchup: time zones — CEST/CET, New York winter, Sydney summer (got " + std::to_string(summer) + "/" +
                   std::to_string(winter) + "/" + std::to_string(ny) + "/" + std::to_string(sydney) + ")");
        expect(!utcOffsetAt(L"Mars/Olympus_Mons", 1796083200, &unknown) && !utcOffsetAt(L"", 0, &unknown) &&
                   !utcOffsetAt(L"Europe/Zürich", 0, &unknown) && unknown == 12345,
               "catchup: time zones — an unknown, empty or non-ASCII name is refused, the offset untouched");
        // The zone the sync keeps (M2): one that agrees with the clock the probe measured.
        const int cest = 7200, cet = 3600;
        const long long summerT = 1790325000, winterT = 1796083200, dstEnd = 1792890000;  // 2026-10-25 01:00Z
        expect(serverZoneAgrees(L"Europe/Amsterdam", summerT, cest) && serverZoneAgrees(L"Europe/Amsterdam", winterT, cet) &&
                   !serverZoneAgrees(L"Europe/Amsterdam", summerT, cet) &&
                   !serverZoneAgrees(L"America/New_York", summerT, cest) &&
                   !serverZoneAgrees(L"Mars/Olympus_Mons", summerT, cest) && !serverZoneAgrees(L"", summerT, 0),
               "catchup: server zone — agrees at CEST and CET, not with another offset, zone, an unknown or no name");
        expect(serverZoneAgrees(L"Europe/Amsterdam", dstEnd, cest) && serverZoneAgrees(L"Europe/Amsterdam", dstEnd, cet) &&
                   serverZoneAgrees(L"Europe/Amsterdam", dstEnd - 60, cet) &&
                   !serverZoneAgrees(L"Europe/Amsterdam", dstEnd + 3600, cest),
               "catchup: server zone — at the daylight-saving change either offset agrees (two minutes' grace), "
               "an hour on only the new one");
        using Zone = std::optional<std::wstring>;
        const Zone never;  // nothing stored yet
        expect(serverZoneToStore(L"Europe/Amsterdam", never, summerT, &cest) == Zone(L"Europe/Amsterdam") &&
                   serverZoneToStore(L"America/New_York", never, summerT, &cest) == Zone(L"") &&
                   serverZoneToStore(L"America/New_York", Zone(L"Asia/Tokyo"), summerT, &cest) == Zone(L"") &&
                   serverZoneToStore(L"Mars/Olympus_Mons", never, summerT, &cest) == Zone(L""),
               "catchup: zone to store, measured — the probe's zone if it agrees; \"\" (the offset decides) if it "
               "disagrees or is unknown here, and no stored one agrees");
        expect(serverZoneToStore(L"America/New_York", Zone(L"Europe/Amsterdam"), summerT, &cest) ==
                       Zone(L"Europe/Amsterdam") &&
                   serverZoneToStore(L"Mars/Olympus_Mons", Zone(L"Europe/Amsterdam"), summerT, &cest) ==
                       Zone(L"Europe/Amsterdam"),
               "catchup: zone to store, measured — a probe naming a wrong zone keeps the stored one the clock agrees with");
        expect(serverZoneToStore(L"Europe/Amsterdam", Zone(L"Europe/Paris"), summerT, &cest) == Zone(L"Europe/Amsterdam") &&
                   serverZoneToStore(L"", Zone(L"Mars/Olympus_Mons"), summerT, &cest) == Zone(L""),
               "catchup: zone to store, measured — an agreeing probe wins over another stored zone; a stored name "
               "unknown here is refused");
        expect(serverZoneToStore(L"Europe/Amsterdam", Zone(L"Mars/Olympus_Mons"), summerT, nullptr) ==
                   Zone(L"Europe/Amsterdam"),
               "catchup: zone to store, nothing measured — a stored name unknown here counts as none (replaced)");
        // (Without a measurement a name is judged "known here" at the current time, not at the probe's
        // unchecked timestamp — defensive only: MSVC's tz database answers even at a microsecond stamp,
        // so no test here can tell the two apart.)
        expect(serverZoneToStore(L"Mars/Olympus_Mons", Zone(L"Mars/Olympus_Mons"), summerT, nullptr) == Zone(L"") &&
                   serverZoneToStore(L"", Zone(L"Mars/Olympus_Mons"), summerT, nullptr) == never,
               "catchup: zone to store, nothing measured — an unknown probed name stores \"\", nothing probed "
               "leaves an unknown stored one");
        expect(serverZoneToStore(L"", Zone(L"Europe/Amsterdam"), summerT, &cest) == Zone(L"Europe/Amsterdam") &&
                   serverZoneToStore(L"", Zone(L"America/New_York"), summerT, &cest) == Zone(L"") &&
                   serverZoneToStore(L"", Zone(L""), summerT, &cest) == never && serverZoneToStore(L"", never, summerT, &cest) == never,
               "catchup: zone to store, measured, the probe naming none — the STORED zone is checked again");
        expect(serverZoneToStore(L"Europe/Amsterdam", never, summerT, nullptr) == Zone(L"Europe/Amsterdam") &&
                   serverZoneToStore(L"Mars/Olympus_Mons", never, summerT, nullptr) == Zone(L"") &&
                   serverZoneToStore(L"America/New_York", Zone(L""), summerT, nullptr) == never &&
                   serverZoneToStore(L"America/New_York", Zone(L"Europe/Amsterdam"), summerT, nullptr) == never &&
                   serverZoneToStore(L"", never, summerT, nullptr) == never,
               "catchup: zone to store, nothing measured — a name only where none was stored, never over a kept "
               "or refused one");

        // channel_archive, the guide rows and past search results.
        const std::wstring cpath = wdir + L"/catchup_selftest.db";
        for (const wchar_t* sfx : {L"", L"-wal", L"-shm"}) { std::error_code ec_; std::filesystem::remove(std::filesystem::path(utf8FromWide(cpath + sfx)), ec_); }
        const long long now = 1'800'000'000;
        auto ch = [](const wchar_t* name, const wchar_t* tvg, const wchar_t* url, int chno = -1,
                     Channel::Kind kind = Channel::Kind::Live) {
            ParsedChannel c;
            c.name = name;
            c.tvgId = tvg;
            c.streamUrl = url;
            c.chno = chno;
            c.kind = kind;
            return c;
        };
        auto pr = [](const wchar_t* cid, long long s, long long e, const wchar_t* t) {
            Programme p;
            p.channelId = cid;
            p.startUtc = s;
            p.stopUtc = e;
            p.title = t;
            return p;
        };
        Database cdb;
        expect(cdb.open(cpath) && cdb.channelArchiveReady(), "catchup: channel_archive exists on a fresh DB");
        const long long p1 = cdb.addPlaylist(L"P1", L"http://h/get.php?username=u&password=p", true, 1000,
                                             L"http://h/xmltv.php");
        const long long p2 = cdb.addPlaylist(L"P2", L"http://h2/pl", true, 1000);
        cdb.bulkInsertChannels(p1, {ch(L"CNN HD", L"CNN.us@HD", L"http://h/u/p/101", 1),
                                    ch(L"CNN", L"cnn.us", L"http://h/u/p/102", 2),
                                    ch(L"BBC", L"bbc.uk", L"http://h/live/u/p/103.ts", 3),
                                    ch(L"A Film", L"", L"http://h/movie/u/p/101.mp4", -1, Channel::Kind::Movie)},
                               1000);
        cdb.bulkInsertChannels(p2, {ch(L"Other", L"oth.xx", L"http://h2/u/p/201")}, 1000);
        long long cnnHd = 0, cnn = 0, bbc = 0, other = 0;
        const auto urls = cdb.liveChannelUrls(p1);
        for (const auto& [id, url] : urls) {
            const long long sid = xtreamLiveStreamId(url);
            if (sid == 101) cnnHd = id;
            if (sid == 102) cnn = id;
            if (sid == 103) bbc = id;
        }
        for (const auto& row : cdb.liveChannelUrls(p2)) other = row.first;
        expect(urls.size() == 3 && cnnHd && cnn && bbc && other,
               "catchup: liveChannelUrls — the playlist's live channels (not its film), found by stream id");
        {
            // Series episodes and films filed as live (an Xtream playlist's episodes are kind 0) are left
            // out by the first path segment, case-sensitively — a login that is itself "Movie" or
            // "series" keeps its live URLs, which have one segment fewer.
            const long long p3 = cdb.addPlaylist(L"P3", L"http://h3/get.php?username=series&password=p", true, 1000);
            cdb.bulkInsertChannels(p3, {ch(L"Short Movie", L"", L"http://h3/Movie/p/301"),
                                        ch(L"Short series", L"", L"http://h3/series/p/302"),
                                        ch(L"Long movie", L"", L"http://h3/live/movie/p/303.ts"),
                                        ch(L"Trailing slash", L"", L"http://h3/series/p/304/"),
                                        ch(L"Slash in the query", L"", L"http://h3/series/p/305?t=a/b"),
                                        ch(L"An episode", L"", L"http://h3/series/u/p/5.mkv"),
                                        ch(L"A film as live", L"", L"http://h3/movie/u/p/9.mp4")},
                                   1000);
            auto sidsOf = [&](const std::wstring& user) {
                std::set<long long> sids;
                for (const auto& [id, url] : cdb.liveChannelUrls(p3, user)) sids.insert(xtreamLiveStreamId(url));
                return sids;
            };
            const std::set<long long> anyone = sidsOf(L""), named = sidsOf(L"series");
            expect(anyone == std::set<long long>{301, 302, 303},
                   "catchup: liveChannelUrls — series/…/…/… and movie/…/…/… left out; a login \"Movie\" or "
                   "\"series\" keeps its short live URLs (the old NOT LIKE dropped them) (got " +
                       std::to_string(anyone.size()) + ")");
            expect(named == std::set<long long>{0, 301, 302, 303, 304, 305},
                   "catchup: liveChannelUrls — told the user is \"series\", its trailing-slash and query URLs "
                   "are kept (its word's filter off: the episodes come back for the parser to refuse, as 0; "
                   "the films stay out) (got " + std::to_string(named.size()) + ")");
            cdb.deletePlaylist(p3);
        }
        {
            // Matching the list to the playlist (matchArchiveFlags): its own login only. The list's answer
            // stands when it names no archive at all, or when the playlist's URLs read as that login's
            // (L4); only a list naming archives while NONE of the URLs reads keeps the stored flags.
            XtreamCreds mine;
            expect(parseXtreamPlaylistUrl(L"http://h/get.php?username=u&password=p", mine), "catchup: (fixture) login");
            auto sorted = [](std::vector<std::pair<long long, int>> v) {
                std::sort(v.begin(), v.end());
                return v;
            };
            const ArchiveMatch some = matchArchiveFlags(urls, {{101, 1}, {102, 3}, {999, 2}}, mine);
            std::vector<std::pair<long long, int>> want{{cnnHd, 1}, {cnn, 3}};
            expect(some.usable && sorted(some.flags) == sorted(want),
                   "catchup: archive match — the playlist's channels by stream id, an unknown stream ignored");
            const ArchiveMatch none = matchArchiveFlags(urls, {{999, 2}, {998, 1}}, mine);
            expect(none.usable && none.flags.empty(),
                   "catchup: archive match — archives only elsewhere on the line, URLs that read: the answer "
                   "\"none here\" stands (every flag cleared)");
            const ArchiveMatch empty = matchArchiveFlags(urls, {}, mine);
            expect(empty.usable && empty.flags.empty(),
                   "catchup: archive match — a list with no archive at all IS an answer (every flag cleared)");
            XtreamCreds theirs = mine;
            theirs.username = L"someone";
            const ArchiveMatch foreign = matchArchiveFlags(urls, {{101, 1}, {102, 3}}, theirs);
            expect(!foreign.usable && foreign.flags.empty(),
                   "catchup: archive match — no URL reads as this login's: NOT usable (the stored flags are kept)");
            const ArchiveMatch foreignNone = matchArchiveFlags(urls, {}, theirs);
            expect(foreignNone.usable && foreignNone.flags.empty(),
                   "catchup: archive match — a list naming no archive at all is an answer, whatever the URLs");
            // The login crosses from the playlist URL's QUERY ('+' = space) to the stream URLs' PATH
            // ('+' is a '+'): "%20" there is the same user; "+" is not (a panel that writes it so keeps
            // its old flags, by the rule above).
            XtreamCreds enc;
            expect(parseXtreamPlaylistUrl(L"http://h/get.php?username=a+b&password=p%2Fq", enc),
                   "catchup: (fixture) an encoded login");
            const ArchiveMatch spelt = matchArchiveFlags(
                {{1, L"http://h/a%20b/p%2Fq/77"}, {2, L"http://h/a+b/p%2Fq/78"}}, {{77, 1}, {78, 2}}, enc);
            expect(spelt.usable && spelt.flags == std::vector<std::pair<long long, int>>{{1, 1}},
                   "catchup: archive match — the login decoded as a path: %20 matches a query's '+', a path '+' does not");
            expect(!matchArchiveFlags({{2, L"http://h/a+b/p%2Fq/78"}}, {{78, 2}}, enc).usable,
                   "catchup: archive match — a playlist spelling the login only with '+': NOT usable (flags kept)");
        }
        expect(cdb.replaceChannelArchive(p1, {{cnn, 3}, {other, 2}, {bbc, 0}}) &&
                   cdb.channelArchiveDays() == std::unordered_map<long long, int>{{cnn, 3}},
               "catchup: replaceChannelArchive — this playlist's channels only, 0 days skipped");
        bool cnnHdNone = false, cnnThree = false;
        for (const auto& g : cdb.liveGuideChannels()) {
            if (g.id == cnnHd && g.archiveDays == 0) cnnHdNone = true;
            if (g.id == cnn && g.archiveDays == 3) cnnThree = true;
        }
        expect(cnnHdNone && cnnThree, "catchup: liveGuideChannels carries each channel's archive days");

        expect(cdb.bulkInsertProgrammes(p1, {pr(L"cnn.us", now - 3 * 3600, now - 2 * 3600, L"Morning Show"),
                                             pr(L"cnn.us", now - 26 * 3600, now - 25 * 3600, L"Yesterday Show"),
                                             pr(L"cnn.us", now - 5 * 3600, now - 4 * 3600, L"Noon Show"),
                                             pr(L"cnn.us", now - 5 * 86400, now - 5 * 86400 + 3600, L"Old Show"),
                                             pr(L"cnn.us", now - 600, now + 1200, L"Airing Show"),
                                             pr(L"cnn.us", now + 3600, now + 7200, L"Later Show"),
                                             pr(L"bbc.uk", now - 3 * 3600, now - 2 * 3600, L"Past Bbc Show")},
                                        now) == 7,
               "catchup: fixture guide stored");
        const GuideModel gm = buildGuideModel(cdb, now);
        bool rowOk = false;
        for (const auto& r : gm.rows)
            if (r.channelName == L"CNN HD" && r.archiveChannel == cnn && r.archiveDays == 3) rowOk = true;
        expect(rowOk, "catchup: the guide row is named after CNN HD and plays catch-up from its sibling CNN");

        auto titles = [](const std::vector<Database::ProgrammeHit>& hs) {
            std::wstring s;
            for (const auto& h : hs) s += (s.empty() ? L"" : L"|") + h.programme.title;
            return s;
        };
        expect(cdb.rebuildProgrammeIndex() && cdb.refreshProgrammeSearchChannels(), "catchup: search ready");
        const auto plain = cdb.searchProgrammes(L"show", now, 50);
        expect(titles(plain) == L"Airing Show|Later Show",
               "catchup: without archive, search keeps hiding what has ended (got " + utf8FromWide(titles(plain)) + ")");
        const auto withPast = cdb.searchProgrammes(L"show", now, 50, nullptr, true);
        expect(titles(withPast) == L"Airing Show|Later Show|Morning Show|Noon Show|Yesterday Show",
               "catchup: with archive — upcoming first, then ended ones inside the archive, most recent "
               "first; not past it, not on a channel without one (got " + utf8FromWide(titles(withPast)) + ")");
        expect(!withPast.empty() && withPast[0].archiveChannelId == cnn && withPast[0].archiveDays == 3,
               "catchup: a hit names the channel whose archive plays it");
        const auto likePath = cdb.searchProgrammes(L"Sh", now, 50, nullptr, true);  // 2 characters: LIKE
        expect(titles(likePath) == L"Airing Show|Later Show|Morning Show|Noon Show|Yesterday Show",
               "catchup: the LIKE path orders and filters the same way (got " + utf8FromWide(titles(likePath)) + ")");

        // Each block of results has its own limit (L6): a word common among upcoming programmes still
        // shows aired ones.
        {
            bool trunc = false, truncLike = false;
            const auto one = cdb.searchProgrammes(L"show", now, 1, &trunc, true);
            const auto oneLike = cdb.searchProgrammes(L"Sh", now, 1, &truncLike, true);
            expect(titles(one) == L"Airing Show|Morning Show" && trunc && titles(oneLike) == L"Airing Show|Morning Show" &&
                       truncLike,
                   "catchup: a limit per block — the first upcoming AND the latest aired, 'more' said (got " +
                       utf8FromWide(titles(one)) + " / " + utf8FromWide(titles(oneLike)) + ")");
            bool truncNo = true;
            const auto plainOne = cdb.searchProgrammes(L"show", now, 1, &truncNo);
            expect(titles(plainOne) == L"Airing Show" && truncNo,
                   "catchup: without archive, one block and one limit, as before");
            bool upMore = true, pastMore = false, upMoreLike = true, pastMoreLike = false;
            const auto two = cdb.searchProgrammes(L"show", now, 2, &upMore, true, &pastMore);
            const auto twoLike = cdb.searchProgrammes(L"Sh", now, 2, &upMoreLike, true, &pastMoreLike);
            expect(titles(two) == L"Airing Show|Later Show|Morning Show|Noon Show" && !upMore && pastMore &&
                       titles(twoLike) == titles(two) && !upMoreLike && pastMoreLike,
                   "catchup: each block says 'more' of itself — all upcoming shown, the aired cut (got " +
                       utf8FromWide(titles(two)) + ")");
            bool eitherMore = false, eitherMoreLike = false;
            cdb.searchProgrammes(L"show", now, 2, &eitherMore, true);
            cdb.searchProgrammes(L"Sh", now, 2, &eitherMoreLike, true);
            expect(eitherMore && eitherMoreLike,
                   "catchup: without truncatedPast, 'truncated' says so of the aired block too (both paths)");
        }

        // Two siblings keep an archive: the one keeping the LONGEST plays (CNN, 3 days, beside CNN HD's
        // 1 — L5), in the guide's row and in search alike; on a tie, the first in channel order (CNN HD).
        auto rowPick = [&]() {
            GuideArchivePick p;
            long long pid = 0;
            for (const auto& r : buildGuideModel(cdb, now).rows)
                if (r.channelName == L"CNN HD") {
                    p = {r.archiveChannel, r.archiveDays};
                    pid = r.playlistId;
                }
            return std::make_pair(p, pid);
        };
        expect(cdb.replaceChannelArchive(p1, {{cnn, 3}, {cnnHd, 1}}) && cdb.refreshProgrammeSearchChannels(),
               "catchup: both CNN feeds keep an archive");
        const auto [longest, rowPlaylist] = rowPick();
        const auto both = cdb.searchProgrammes(L"morning", now, 50, nullptr, true);
        expect(longest.channel == cnn && longest.days == 3 && rowPlaylist == p1 && both.size() == 1 &&
                   both[0].archiveChannelId == cnn && both[0].archiveDays == 3,
               "catchup: of two archived siblings, the longest archive plays — guide row and search agree");
        expect(cdb.replaceChannelArchive(p1, {{cnn, 2}, {cnnHd, 2}}) && cdb.refreshProgrammeSearchChannels(),
               "catchup: both CNN feeds keep two days");
        const GuideArchivePick tie = rowPick().first;
        const auto tieHits = cdb.searchProgrammes(L"morning", now, 50, nullptr, true);
        expect(tie.channel == cnnHd && tie.days == 2 && tieHits.size() == 1 && tieHits[0].archiveChannelId == cnnHd,
               "catchup: on a tie, the first in channel order plays — guide row and search agree");
        // An open guide takes new flags in place (M1): the picks re-read from the database are the ones a
        // rebuilt guide would make — by the row's or the result's FULL tvg-id, per playlist.
        expect(cdb.replaceChannelArchive(p1, {{cnn, 3}, {cnnHd, 1}}), "catchup: the flags change under an open guide");
        {
            const GuideArchivePicks picks(cdb);
            const GuideArchivePick byRow = picks.pick(p1, L"CNN.us@HD"), byBase = picks.pick(p1, L"cnn.us");
            const GuideArchivePick rebuilt = rowPick().first;
            expect(byRow.channel == rebuilt.channel && byRow.days == rebuilt.days && byRow.channel == cnn &&
                       byBase.channel == cnn && picks.pick(p2, L"CNN.us@HD").channel == 0 &&
                       picks.pick(p1, L"bbc.uk").channel == 0,
                   "catchup: GuideArchivePicks — what a rebuild would pick, by full or base id; none elsewhere");
            // …and what a new search session would: by the hit's own playlist and channel id.
            expect(cdb.refreshProgrammeSearchChannels(), "catchup: (fixture) a new search session");
            const auto hit = cdb.searchProgrammes(L"morning", now, 50, nullptr, true);
            const GuideArchivePick byHit =
                hit.size() == 1 ? picks.pick(hit[0].playlistId, hit[0].channelTvgId) : GuideArchivePick{};
            expect(hit.size() == 1 && byHit.channel == hit[0].archiveChannelId && byHit.days == hit[0].archiveDays &&
                       byHit.channel == cnn,
                   "catchup: GuideArchivePicks — a search result's pick, by its playlist and tvg-id, is the search's");
        }
        expect(cdb.replaceChannelArchive(p1, {{cnn, 2}, {cnnHd, 2}}) &&
                   GuideArchivePicks(cdb).pick(p1, L"cnn.us").channel == cnnHd,
               "catchup: GuideArchivePicks — on a tie, the first in channel order, as a rebuild picks");
        expect(cdb.replaceChannelArchive(p1, {}) && cdb.channelArchiveDays().empty(),
               "catchup: an empty replacement clears the playlist's flags");
        {
            bool more = true, morePast = true;
            const auto none = cdb.refreshProgrammeSearchChannels()
                                  ? cdb.searchProgrammes(L"show", now, 50, &more, true, &morePast)
                                  : std::vector<Database::ProgrammeHit>{};
            expect(titles(none) == L"Airing Show|Later Show" && !more && !morePast,
                   "catchup: no channel keeps an archive — with catch-up, the search as without (got " +
                       utf8FromWide(titles(none)) + ")");
        }
        expect(cdb.replaceChannelArchive(p1, {{cnn, 2}}) && cdb.channelArchiveDays().size() == 1, "catchup: set again");
        cdb.deletePlaylist(p1);
        expect(cdb.channelArchiveDays().empty(), "catchup: a deleted playlist's flags go with its channels (cascade)");
    }

    // PORTED from Win32/cli/RabbitEarsCli.cpp: the photoreal cells' geometry and rasteriser, now shared
    // (common/ui/PhotoCells, through common/platform/WinTypes.h), checked under Apple clang; the look
    // codec half is mac's own model against Win32's tokens.
    out("== Photoreal cell looks (common/ui/PhotoCells — the rasteriser mac draws through) ==\n");
    {
        // Every look survives the settings store with WIN32'S token (a meter_style written by either platform
        // reads on the other): its own token, parsed back to itself, no two looks sharing one.
        {
            using namespace rabbitears::mac;
            bool codec = true;
            std::set<std::string> tokens;
            for (int s = 0; s < kMeterStyleCount; ++s) {
                const MeterStyle look = static_cast<MeterStyle>(s);
                const std::string t = meterStyleToString(look);
                const MeterStyle other = look == MeterStyle::Led ? MeterStyle::Tube : MeterStyle::Led;
                codec = codec && meterStyleFromString(t, other) == look && tokens.insert(t).second;
            }
            expect(codec && tokens == std::set<std::string>{"led", "tube", "lcd", "scope", "vu", "vu_silver",
                                                            "led_studio", "lcd_backlit", "vfd"} &&
                       isPhotoCellLook(MeterStyle::StudioLed) && isPhotoCellLook(MeterStyle::BacklitLcd) &&
                       isPhotoCellLook(MeterStyle::Vfd) && !isPhotoCellLook(MeterStyle::Led) &&
                       isVuLook(MeterStyle::Vu) && isVuLook(MeterStyle::VuSilver) && !isVuLook(MeterStyle::Scope),
                   "mac meter looks: every token round-trips, is its own, and is Win32's (" +
                       std::to_string(tokens.size()) + " looks)");
        }

        // The rows: whole cells inside the dial, as many as fit, the block centred, never finer than the classic
        // pitch (nor than the classic Bitrate column — so its history ring covers these columns too); once the
        // dial has room, the pitch is its height over kPhotoRows (floored), so 10 to 13 rows — at every dial
        // height a meter can have (a whole cell up to a 120-dp bridge meter at 500 %), every 25 % step from 100 %
        // to 500 %.
        bool rowsOk = true;
        std::string rowsMiss;
        for (UINT dpi = 96; dpi <= 480; dpi += 24)
            for (int h = photoClassicPitch(dpi); h <= 600; ++h) {
                const PhotoRows g = photoRows(h, dpi);
                const int topMargin = g.cellTop(g.rows - 1), bottomMargin = h - g.bottom;
                const bool roomy = h >= kPhotoRows * photoClassicPitch(dpi);
                const bool ok = g.rows >= 1 && g.pitch >= photoClassicPitch(dpi) &&
                                g.pitch >= std::max(MulDiv(3, static_cast<int>(dpi), 96), 2) && g.gap >= 1 && g.gap < g.pitch &&
                                topMargin >= 0 && bottomMargin >= 0 && std::abs(topMargin - bottomMargin) <= 1 &&
                                g.rows * g.pitch - g.gap <= h && (g.rows + 1) * g.pitch - g.gap > h &&
                                (!roomy || (g.pitch == h / kPhotoRows && g.rows >= kPhotoRows && g.rows <= kPhotoRows + 3));
                if (!ok && rowsOk) rowsMiss = "dpi " + std::to_string(dpi) + " h " + std::to_string(h);
                rowsOk = rowsOk && ok;
            }
        // Pinned, at 150 % (144 dpi): the standard tray's 39-px dial keeps the classic 5-px pitch; a Flat skin's
        // Extra large dial (102 px — no frame beyond the chrome) has 10 rows at a 10-px pitch where the classic looks
        // draw 20 of 5. (A skin with a material frames it: 96 px, a 9-px pitch — the tank test below pins that.)
        const PhotoRows std150 = photoRows(39, 144), xl150 = photoRows(102, 144);
        expect(rowsOk && std150.pitch == 5 && xl150.pitch == 10 && xl150.rows == 10 && xl150.gap == 2,
               "photo rows: whole, centred, >= the classic pitch, 10-13 at size; 150 %: standard pitch " +
                   std::to_string(std150.pitch) + ", Extra large (Flat) " + std::to_string(xl150.rows) + " rows at a " +
                   std::to_string(xl150.pitch) + "-px pitch" +
                   (rowsOk ? std::string() : " (first miss: " + rowsMiss + ")"));

        // The columns: as many as fit (capped), inside the width, the block centred; the Signal bars climb to a
        // full column.
        bool colsOk = true;
        std::string colsMiss;
        for (int w = 1; w <= 700 && colsOk; ++w)
            for (int colW = 2; colW <= 40 && colsOk; ++colW)
                for (int gap = 1; gap < colW && colsOk; gap += 3)
                    for (int maxCols : {5, 16, 1 << 16}) {
                        const PhotoCols c = photoCols(w, colW, gap, maxCols);
                        const int used = c.cols * colW - gap, right = w - (c.left + std::max(0, used));
                        const bool ok = c.cols >= 0 && c.cols <= maxCols && c.left >= 0 && right >= 0 &&
                                        (c.cols == 0 || std::abs(c.left - right) <= 1) &&
                                        (c.cols == maxCols || (c.cols + 1) * colW - gap > w);
                        if (!ok) {
                            colsOk = false;
                            colsMiss = "w " + std::to_string(w) + " colW " + std::to_string(colW) + " gap " +
                                       std::to_string(gap) + " max " + std::to_string(maxCols);
                            break;
                        }
                    }
        bool signalOk = true;
        for (int rows = 1; rows <= 40; ++rows)
            for (int j = 0; j < 5; ++j) {
                const int cells = photoSignalCells(j, rows);
                signalOk = signalOk && cells >= 1 && cells <= rows &&
                           (j == 0 || cells >= photoSignalCells(j - 1, rows)) && (j != 4 || cells == rows);
            }
        expect(colsOk && signalOk, "photo columns: as many as fit, inside, centred; Signal bars climb to a full column" +
                                       (colsOk ? std::string() : " (first miss: " + colsMiss + ")"));

        // The rasteriser. A 200 x 60 buffer of a sentinel colour, a dial inside it, and cells that touch the
        // dial's edges (their glow would spill past them).
        constexpr uint32_t kSentinel = 0x00ABCDEFu;
        const int bw = 200, bh = 60;
        const RECT dial{20, 10, 180, 50};
        const COLORREF green = RGB(96, 205, 128), darkPanel = RGB(24, 22, 22), lightPanel = RGB(243, 243, 243);
        auto luma = [](uint32_t p) {
            return (299 * ((p >> 16) & 0xFF) + 587 * ((p >> 8) & 0xFF) + 114 * (p & 0xFF)) / 1000;
        };
        auto paint = [&](CellFinish f, COLORREF panel, float glow, const std::vector<PhotoCell>& cells,
                         PhotoCellCache* keep = nullptr) {
            std::vector<uint32_t> px(static_cast<size_t>(bw) * bh, kSentinel);
            PhotoScene sc;
            sc.finish = f;
            sc.panel = panel;
            sc.glow = glow;
            sc.pitch = 10;
            PhotoCellCache fresh;
            paintPhotoCells(px.data(), bw, bh, dial, sc, cells, keep ? *keep : fresh);
            return px;
        };
        auto at = [&](const std::vector<uint32_t>& px, int x, int y) { return px[static_cast<size_t>(y) * bw + x]; };
        // Cells 24 x 8 at the dial's corners and one in the middle (lit, then unlit).
        const std::vector<PhotoCell> edgeCells = {{RECT{20, 10, 44, 18}, green, true},
                                                  {RECT{156, 42, 180, 50}, green, true},
                                                  {RECT{80, 26, 104, 34}, green, true}};
        bool inside = true, filled = true;
        for (CellFinish f : {CellFinish::Led, CellFinish::Lcd, CellFinish::Vfd})
            for (COLORREF panel : {darkPanel, lightPanel}) {
                const std::vector<uint32_t> px = paint(f, panel, 1.0f, edgeCells);
                for (int y = 0; y < bh; ++y)
                    for (int x = 0; x < bw; ++x) {
                        const bool out = x < dial.left || x >= dial.right || y < dial.top || y >= dial.bottom;
                        if (out && at(px, x, y) != kSentinel) inside = false;
                        if (!out && at(px, x, y) == kSentinel) filled = false;
                    }
            }
        expect(inside && filled, "photo cells: every dial pixel drawn and nothing outside the dial (glow and all), "
                                 "every finish, dark and light");

        // An LED: lit, its core outshines its rim and any unlit lens; unlit, the lens still carries its hue.
        const std::vector<PhotoCell> litMid = {{RECT{80, 26, 104, 34}, green, true}};
        const std::vector<PhotoCell> offMid = {{RECT{80, 26, 104, 34}, green, false}};
        const std::vector<uint32_t> ledOn = paint(CellFinish::Led, darkPanel, 0.5f, litMid);
        const std::vector<uint32_t> ledOff = paint(CellFinish::Led, darkPanel, 0.5f, offMid);
        const uint32_t onCore = at(ledOn, 92, 30), onRim = at(ledOn, 81, 33), offCore = at(ledOff, 92, 30);
        const bool tinted = ((offCore >> 8) & 0xFF) > ((offCore >> 16) & 0xFF) && ((offCore >> 8) & 0xFF) > (offCore & 0xFF);
        const uint32_t flatGreen = ((green & 0xFFu) << 16) | (green & 0xFF00u) | ((green >> 16) & 0xFFu);  // as 0x00RRGGBB
        expect(luma(onCore) > luma(flatGreen) + 10 && luma(onCore) > luma(onRim) && luma(onRim) > luma(offCore) &&
                   tinted,
               "LED lens: lit core " + std::to_string(luma(onCore)) + " > its flat colour " +
                   std::to_string(luma(flatGreen)) + " (a hot core) and > rim " + std::to_string(luma(onRim)) +
                   " > unlit " + std::to_string(luma(offCore)) + "; the unlit lens green-tinted");
        // The key light's glint: one pixel inside the rim, top-left (the cell's second row, third column) is
        // lighter than the same spot top-right (its mirror image — the core and rim are symmetric), lit or not.
        const int glintOn = luma(at(ledOn, 82, 27)) - luma(at(ledOn, 101, 27));
        const int glintOff = luma(at(ledOff, 82, 27)) - luma(at(ledOff, 101, 27));
        expect(glintOn > 20 && glintOff > 5,
               "LED glint: top-left inside the rim " + std::to_string(glintOn) + " (lit) / " + std::to_string(glintOff) +
                   " (unlit) lighter than top-right");

        // Glow: on a dark panel a lit LED lights the housing beside it (more with the Glow knob up, none with it
        // down); on a light panel there is none (light added to white is lost).
        const int gx = 106, gy = 30;  // 2 px right of the cell, in the housing
        const uint32_t darkLit = at(ledOn, gx, gy), darkUnlit = at(ledOff, gx, gy);
        const uint32_t knobUp = at(paint(CellFinish::Led, darkPanel, 1.0f, litMid), gx, gy);
        const uint32_t knobOff = at(paint(CellFinish::Led, darkPanel, 0.0f, litMid), gx, gy);
        const uint32_t lightLit = at(paint(CellFinish::Led, lightPanel, 1.0f, litMid), gx, gy);
        const uint32_t lightUnlit = at(paint(CellFinish::Led, lightPanel, 1.0f, offMid), gx, gy);
        expect(luma(darkLit) > luma(darkUnlit) && luma(knobUp) > luma(darkLit) && knobOff == darkUnlit &&
                   lightLit == lightUnlit,
               "LED glow: the housing by a lit cell " + std::to_string(luma(darkLit)) + " > by an unlit one " +
                   std::to_string(luma(darkUnlit)) + ", Glow up " + std::to_string(luma(knobUp)) +
                   ", Glow off none; none on a light panel");
        // ...and it fades to nothing at its reach — no edge. This cell's glow reaches 6 px (0.7 of its 8-px
        // height): its last pixel (5 px out) and the first beyond it match, with Glow up; 1 px out it is bright.
        const std::vector<uint32_t> up = paint(CellFinish::Led, darkPanel, 1.0f, litMid);
        const int nearOut = luma(at(up, 104, 30)), lastIn = luma(at(up, 109, 30)), beyond = luma(at(up, 110, 30));
        expect(std::abs(lastIn - beyond) <= 1 && nearOut > beyond + 10,
               "LED glow fades out: 1 px out " + std::to_string(nearOut) + ", at its reach " + std::to_string(lastIn) +
                   ", beyond " + std::to_string(beyond));

        // The VFD's window is dark glass on a light panel too; the LCD's is a reflective grey-green there.
        const uint32_t vfdField = at(paint(CellFinish::Vfd, lightPanel, 0.5f, {}), 30, 40);
        const uint32_t lcdField = at(paint(CellFinish::Lcd, lightPanel, 0.5f, {}), 30, 40);
        const uint32_t lr = (lcdField >> 16) & 0xFF, lg = (lcdField >> 8) & 0xFF, lb = lcdField & 0xFF;
        expect(luma(vfdField) < 40 && luma(lcdField) > 150 && lg > lr && lg > lb,
               "VFD glass stays dark on a light panel (" + std::to_string(luma(vfdField)) +
                   "); the LCD's field is a light grey-green there (" + std::to_string(lr) + "," + std::to_string(lg) +
                   "," + std::to_string(lb) + ")");

        // The Backlit LCD's and the VFD's unlit cells follow the palette's Dim, as the LED's lens does (and the
        // classic LCD's ghost).
        auto unlitWith = [&](CellFinish f, COLORREF dim) {
            std::vector<uint32_t> px(static_cast<size_t>(bw) * bh, kSentinel);
            PhotoScene sc;
            sc.finish = f;
            sc.panel = darkPanel;
            sc.off = dim;
            sc.pitch = 10;
            PhotoCellCache c;
            paintPhotoCells(px.data(), bw, bh, dial, sc, offMid, c);
            return luma(at(px, 92, 30));
        };
        const int lcdDim = unlitWith(CellFinish::Lcd, RGB(38, 40, 44)), lcdBright = unlitWith(CellFinish::Lcd, RGB(200, 200, 200));
        const int vfdDim = unlitWith(CellFinish::Vfd, RGB(38, 40, 44)), vfdBright = unlitWith(CellFinish::Vfd, RGB(200, 200, 200));
        expect(lcdBright > lcdDim + 20 && vfdBright > vfdDim + 20,
               "unlit Backlit LCD / VFD cells follow Dim: " + std::to_string(lcdDim) + " -> " + std::to_string(lcdBright) +
                   " / " + std::to_string(vfdDim) + " -> " + std::to_string(vfdBright));

        // The cache changes nothing: a frame painted over a warm cache — even one that had to evict (more
        // colours than it keeps) — is the frame a fresh cache paints, and the frame a cache big enough never to
        // evict paints.
        std::vector<PhotoCell> many;
        for (int i = 0; i < 64; ++i) {
            const int x = 20 + (i % 16) * 10, y = 10 + (i / 16) * 10;
            many.push_back({RECT{x, y, x + 8, y + 8}, RGB(4 * i, 255 - 3 * i, 90 + i), (i % 3) != 0});
        }
        PhotoCellCache warm;
        paint(CellFinish::Vfd, darkPanel, 0.5f, many, &warm);
        std::vector<PhotoCell> shuffled(many.rbegin(), many.rend());
        paint(CellFinish::Vfd, darkPanel, 0.5f, shuffled, &warm);
        PhotoCellCache roomy(256);
        const std::vector<uint32_t> truth = paint(CellFinish::Vfd, darkPanel, 0.5f, many, &roomy);
        expect(paint(CellFinish::Vfd, darkPanel, 0.5f, many, &warm) == truth &&
                   paint(CellFinish::Vfd, darkPanel, 0.5f, many) == truth && many.size() > kPhotoSpriteCap,
               "photo cells: a warm evicting cache, a fresh one and one that never evicts paint the same frame");
        // ...and within one frame it keeps a lit cell and an unlit one of the same colour apart (a Spectrum
        // column is exactly that), in either order.
        bool apart = true;
        for (bool litFirst : {true, false})
            for (CellFinish f : {CellFinish::Led, CellFinish::Lcd, CellFinish::Vfd}) {
                const std::vector<PhotoCell> pair = {{RECT{30, 26, 54, 34}, green, litFirst},
                                                     {RECT{120, 26, 144, 34}, green, !litFirst}};
                const std::vector<uint32_t> px = paint(f, darkPanel, 0.5f, pair);
                const uint32_t litPx = at(px, litFirst ? 42 : 132, 30), offPx = at(px, litFirst ? 132 : 42, 30);
                apart = apart && luma(litPx) > luma(offPx) + 40;
            }
        expect(apart, "photo cells: a lit and an unlit cell of one colour in one frame each drawn as itself");
        // ...and a new scene (a skin switch, a new Bg) throws the old cells away: a cache warmed on a dark panel
        // paints a light one as a fresh cache does.
        PhotoCellCache switched;
        paint(CellFinish::Led, darkPanel, 0.5f, litMid, &switched);
        expect(paint(CellFinish::Led, lightPanel, 0.5f, litMid, &switched) == paint(CellFinish::Led, lightPanel, 0.5f, litMid),
               "photo cells: a cache warmed on one panel paints another as a fresh cache does");

        // Stage C: the frame. Its width is the chrome band (a Flat skin: nothing more), a sixteenth of the meter's
        // height once that is more; the dial is what is left inside it.
        bool frameOk = true;
        for (UINT dpi = 96; dpi <= 480; dpi += 24)
            for (int h = 1; h <= 600; ++h) {
                const int chrome = MulDiv(2, static_cast<int>(dpi), 96);
                frameOk = frameOk && photoBezelPx(h, dpi, SkinMaterial::Flat) == chrome &&
                          photoBezelPx(h, dpi, static_cast<SkinMaterial>(9)) == chrome &&  // unknown: as Flat
                          photoBezelPx(h, dpi, SkinMaterial::Brass) == std::max(chrome, h / 16) &&
                          photoDialPx(h, dpi, SkinMaterial::Anodised) == h - 2 * std::max(chrome, h / 16);
            }
        // The ring: every material but Flat fills exactly the ring — nothing inside the window, nothing outside the
        // meter — and Flat, or a material from a newer skin model this build does not know (9), draws nothing.
        const RECT meterRc{20, 10, 180, 50};
        auto bezelFrame = [&](SkinMaterial m, int bpx) {
            std::vector<uint32_t> px(static_cast<size_t>(bw) * bh, kSentinel);
            PhotoBezel bz;
            bz.material = m;
            bz.px = bpx;
            PhotoBezelCache c;
            paintPhotoBezel(px.data(), bw, bh, meterRc, bz, c);
            return px;
        };
        bool ringOk = true;
        for (SkinMaterial m : {SkinMaterial::Anodised, SkinMaterial::Satin, SkinMaterial::Brass, SkinMaterial::NeonGlass,
                               SkinMaterial::Flat, static_cast<SkinMaterial>(9)}) {
            const std::vector<uint32_t> px = bezelFrame(m, 7);
            for (int y = 0; y < bh; ++y)
                for (int x = 0; x < bw; ++x) {
                    const bool inMeter = x >= meterRc.left && x < meterRc.right && y >= meterRc.top && y < meterRc.bottom;
                    const bool inWindow = x >= meterRc.left + 7 && x < meterRc.right - 7 && y >= meterRc.top + 7 &&
                                          y < meterRc.bottom - 7;
                    const bool drawn = at(px, x, y) != kSentinel;
                    const bool known = m != SkinMaterial::Flat && static_cast<int>(m) != 9;
                    const bool want = known && inMeter && !inWindow;
                    if (drawn != want) ringOk = false;
                }
        }
        expect(frameOk && ringOk, "frame: the chrome band or a sixteenth of the height; each material fills exactly the "
                                  "ring round the window, Flat (or an unknown material) nothing");
        // Lit from above-left, a raised frame: the outer edge brighter on the left than on the right and on the top
        // than on the bottom; the lip at the window the other way round. NeonGlass carries its tube's colour down the
        // frame's middle — and, on a frame too narrow for a tube, along its outer edge.
        bool litOk = true;
        std::string litMiss;
        for (SkinMaterial m : {SkinMaterial::Anodised, SkinMaterial::Satin, SkinMaterial::Brass}) {
            const std::vector<uint32_t> px = bezelFrame(m, 7);
            const int my = 30, mx = 100;  // mid-height / mid-width, clear of the rivets
            const bool ok = luma(at(px, 20, my)) > luma(at(px, 179, my)) && luma(at(px, 26, my)) < luma(at(px, 173, my)) &&
                            luma(at(px, mx, 10)) > luma(at(px, mx, 49)) && luma(at(px, mx, 16)) < luma(at(px, mx, 43));
            if (!ok && litOk) litMiss = std::to_string(static_cast<int>(m));
            litOk = litOk && ok;
        }
        auto magenta = [](uint32_t p) {
            const int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;  // the stock neon is magenta
            return r > 150 && r > g + 60 && b > g + 30;
        };
        const uint32_t tube = at(bezelFrame(SkinMaterial::NeonGlass, 7), 23, 30);  // 3 px in: the frame's middle
        const uint32_t edge2 = at(bezelFrame(SkinMaterial::NeonGlass, 2), 20, 30);  // a 2-px frame's outer pixel
        const int tr = (tube >> 16) & 0xFF, tg = (tube >> 8) & 0xFF, tb = tube & 0xFF;
        expect(litOk && magenta(tube) && magenta(edge2),
               "frame: lit from above-left (outer edge left > right, top > bottom; the lip the reverse); the neon tube "
               "magenta (" + std::to_string(tr) + "," + std::to_string(tg) + "," + std::to_string(tb) +
                   "), a 2-px frame's neon edge too" + (litOk ? std::string() : " (first miss: material " + litMiss + ")"));
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
