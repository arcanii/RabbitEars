// SPDX-License-Identifier: GPL-3.0-or-later
// See GuideRefresh.h. MRC file with no retained Objective-C objects: Tr/TrF's autoreleased
// NSStrings live inside the @autoreleasepool around each use.
#import <Foundation/Foundation.h>

#include "GuideRefresh.h"

#include <chrono>
#include <set>
#include <thread>
#include <utility>

#include "core/GuideSources.h"
#include "core/Gzip.h"
#include "core/XmltvParser.h"
#include "db/Database.h"
#include "platform/Encoding.h"
#include "platform/HttpProgress.h"
#include "platform/Log.h"

#import "Tr.h"

namespace rabbitears::mac {
namespace {

using Clock = std::chrono::steady_clock;
long long msSince(Clock::time_point t0) {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}
std::wstring wsOf(NSString* s) { return wideFromUtf8(s.UTF8String ?: ""); }
NSString* nsOf(const std::wstring& w) { return [NSString stringWithUTF8String:utf8FromWide(w).c_str()] ?: @""; }
NSString* n(size_t v) { return [NSString stringWithFormat:@"%zu", v]; }

// A translated line, built inside its own pool (this runs on a GCD worker).
std::wstring tr(i18n::StringId id) {
    @autoreleasepool { return wsOf(Tr(id)); }
}
std::wstring megabytes(unsigned long long b) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f", (double)b / (1024.0 * 1024.0));
    return wideFromUtf8(buf);
}

// 1. One playlist: split, fetch, parse, merge.
GuideFetch fetchPlaylist(const GuideTarget& t, size_t index, size_t count, const GuideProgressFn& progress,
                         const std::atomic<bool>& cancel) {
    using i18n::StringId;
    GuideFetch f;
    f.playlistId = t.playlistId;
    f.name = t.name;
    std::wstring tag;
    if (count > 1) { @autoreleasepool { tag = wsOf(TrF(StringId::LoadingProgressTag, {n(index + 1), n(count)})); } }
    std::vector<std::wstring> urls = splitGuideUrls(t.url);
    if (urls.empty()) urls.push_back(t.url);  // a blank link: the download says why
    const size_t nu = urls.size();
    std::vector<std::vector<Programme>> parsed;  // the guides that parsed, in the link's order
    std::vector<size_t> parsedFrom;              // which of the link's guides each one is
    std::vector<std::wstring> failed;            // why each of the others did not, in order
    for (size_t k = 0; k < nu; ++k) {
        if (cancel) break;
        std::wstring label = t.name;
        if (nu > 1) { @autoreleasepool { label += wsOf(TrF(StringId::LoadingGuideSourceTag, {n(k + 1), n(nu)})); } }
        @autoreleasepool { progress(wsOf(TrF(StringId::LoadingDownloadingName, {nsOf(label), nsOf(tag)})), true); }
        std::string bytes;
        std::wstring err, why;
        const Clock::time_point tDownload = Clock::now();
        const bool got = httpGetWithProgress(urls[k], bytes, err, 60000, [&](unsigned long long received) {
            @autoreleasepool {
                progress(wsOf(TrF(StringId::LoadingDownloadingProgress,
                                  {nsOf(label), nsOf(megabytes(received)), nsOf(tag)})), false);
            }
        });
        f.downloadMs += msSince(tDownload);
        if (!got) {
            why = err.empty() ? tr(StringId::EpgErrorDownloadFailed) : err;
        } else {
            @autoreleasepool {
                progress(wsOf(TrF(StringId::LoadingParsingName, {nsOf(label), n(bytes.size() / 1024), nsOf(tag)})), true);
            }
            const std::string xml = gunzipIfNeeded(bytes);  // .xml.gz bodies arrive still-compressed
            std::string().swap(bytes);
            if (xml.empty()) {
                why = tr(StringId::EpgErrorEmptyAfterDecompress);
            } else {
                const Clock::time_point tParse = Clock::now();
                parsed.push_back(parseXmltv(xml, [&](size_t soFar) {
                    @autoreleasepool {
                        progress(wsOf(TrF(StringId::LoadingReadingProgress, {nsOf(label), n(soFar), nsOf(tag)})), false);
                    }
                }).programmes);
                parsedFrom.push_back(k);
                f.parseMs += msSince(tParse);
            }
        }
        if (!why.empty()) {
            if (nu > 1) { @autoreleasepool { failed.push_back(wsOf(TrF(StringId::EpgGuideSourceError, {n(k + 1), n(nu), nsOf(why)}))); } }
            else failed.push_back(why);
        }
    }
    if (parsed.empty()) {  // nothing to store: the playlist fails, with every guide's reason
        for (const auto& w : failed) f.error += (f.error.empty() ? L"" : L"; ") + w;
        if (f.error.empty()) f.error = tr(StringId::EpgErrorDownloadFailed);  // cancelled before any
    } else if (nu == 1) {
        f.programmes = std::move(parsed[0]);
    } else {
        GuideMerge m = mergeGuideSources(std::move(parsed));
        f.programmes = std::move(m.programmes);
        for (size_t q = 0; q < parsedFrom.size(); ++q)
            f.sourceLog.push_back(L"guide " + std::to_wstring(parsedFrom[q] + 1) + L" of " + std::to_wstring(nu) +
                                  L": " + std::to_wstring(m.kept[q]) + L" programmes taken, " +
                                  std::to_wstring(m.dropped[q]) +
                                  L" left out (an earlier guide's channel, or no readable start)");
        f.sourceErrors = std::move(failed);  // reported beside the programmes the others gave
    }
    return f;
}

// 2 + 3. Store each fetched guide, then re-index once.
void storeAll(GuideRefreshResult& res, const std::wstring& dbPath, const GuideProgressFn& progress,
              const std::atomic<bool>& cancel) {
    using i18n::StringId;
    Database db;
    std::wstring err;
    if (!db.open(dbPath, &err, /*upgradeSchema=*/false)) {
        res.storeError = err.empty() ? std::wstring(L"cannot open the database") : err;
        diag::error(L"EPG store: cannot open its own DB connection: " + res.storeError);
        return;
    }
    constexpr auto kYield = std::chrono::milliseconds(250);  // let a waiting writer in between
    bool first = true;
    const long long now = (long long)time(nullptr);  // stamp at store time (Win32 parity)
    for (GuideFetch& f : res.fetches) {
        if (!f.error.empty()) continue;
        if (!first) std::this_thread::sleep_for(kYield);
        if (cancel) return;
        first = false;
        @autoreleasepool { progress(wsOf(TrF(StringId::LoadingSavingProgrammes, {n(f.programmes.size()), nsOf(f.name)})), true); }
        const Clock::time_point t0 = Clock::now();
        f.stored = db.bulkInsertProgrammes(f.playlistId, f.programmes, now);
        f.storeMs = msSince(t0);
        f.storeDone = true;
        // 0 with an error: nothing landed, the old guide kept — a failure (0 with none: a guide with no
        // valid programmes, stored). More than 0 with one: some rows failed.
        if (f.stored == 0 && !db.lastError().empty()) {
            f.error = db.lastError();
            diag::error(L"EPG store: nothing stored for \"" + f.name + L"\": " + f.error);
        } else if (!db.lastError().empty()) {
            diag::warn(L"EPG store: " + std::to_wstring(f.stored) + L" of " + std::to_wstring(f.programmes.size()) +
                       L" programmes stored for \"" + f.name + L"\"; the first failure: " + db.lastError());
        }
    }
    std::set<std::wstring> chans;
    for (const GuideFetch& f : res.fetches)
        if (f.storeDone && f.error.empty())
            for (const auto& p : f.programmes) chans.insert(p.channelId);
    res.channels = chans.size();
    if (first || cancel || db.programmeSearchState() != Database::ProgrammeSearchState::NeedsRebuild) return;
    std::this_thread::sleep_for(kYield);
    if (cancel) return;
    progress(tr(StringId::LoadingIndexingGuide), true);
    const Clock::time_point t0 = Clock::now();
    const bool indexed = db.rebuildProgrammeIndex();
    res.indexMs = msSince(t0);
    res.index = indexed ? GuideRefreshResult::Index::Rebuilt : GuideRefreshResult::Index::Failed;
    if (!indexed) res.indexError = db.lastError();
}

}  // namespace

GuideRefreshResult refreshGuides(const std::vector<GuideTarget>& targets, const std::wstring& dbPath,
                                 const GuideProgressFn& progress, const std::atomic<bool>& cancel) {
    GuideRefreshResult res;
    try {
        for (size_t i = 0; i < targets.size(); ++i) res.fetches.push_back(fetchPlaylist(targets[i], i, targets.size(), progress, cancel));
        bool anyParsed = false;
        for (const auto& f : res.fetches) anyParsed = anyParsed || f.error.empty();
        if (anyParsed && !cancel) storeAll(res, dbPath, progress, cancel);
    } catch (...) {
        // Holds every parsed programme (tens of MB), so bad_alloc is a real outcome.
        res.storeError = L"internal error";
        try { diag::error(L"EPG refresh: stopped by an exception"); } catch (...) {}
    }
    for (GuideFetch& f : res.fetches) {  // the caller needs only the counts
        if (f.error.empty() && !f.storeDone && !res.storeError.empty()) f.error = res.storeError;
        f.parsed = f.programmes.size();
        std::vector<Programme>().swap(f.programmes);
    }
    return res;
}

}  // namespace rabbitears::mac
