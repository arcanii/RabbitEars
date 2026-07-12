// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window — command handlers + multi-player panes (split from MainWindow.cpp).
#include "ui/MainWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>  // SHGetKnownFolderPath (Videos folder for recordings)
#include <shellapi.h>  // ShellExecuteW (self-restart on a language change)
#include <objidl.h>  // IStream — required by gdiplus.h below
#include <windowsx.h>
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it. (Used for the
// process-wide GDI+ startup the About box's rendering relies on.)
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "core/Gzip.h"
#include "core/Http.h"
#include "core/M3uParser.h"
#include "core/M3uWriter.h"
#include "core/RecordingRules.h"
#include "core/RecordingScheduler.h"
#include "core/XmltvParser.h"
#include "db/Database.h"
#include "platform/Log.h"
#include "platform/PowerPolicy.h"
#include "platform/Updater.h"
#include "platform/WakeScheduler.h"
#include "resource.h"
#include "version.h"
#include "ui/BufferMeter.h"
#include "ui/ChannelGridControl.h"
#include "ui/Dialogs.h"
#include "ui/DockLayout.h"
#include "ui/EpgGuideControl.h"
#include "ui/MiniMeter.h"
#include "ui/Splash.h"
#include "ui/Theme.h"
#include "ui/Tr.h"  // tr()/trf() localization + systemLang()/resolveLang()
#include "ui/VideoGrid.h"
#include "ui/VlcEngine.h"
#include "ui/VlcPlayer.h"

#include "audio/SpectrumTap.h"

#include "ui/MainWindowInternal.h"  // AppState + shared types/ids (rabbitears::mw)

#ifdef RABBITEARS_THEME_ENGINE
#include "platform/Encoding.h"   // wideFromUtf8 / utf8FromWide for the skin settings key + value
#include "ui/skin/SkinStrip.h"  // Phase-1 GPU skin spike: the transport-strip underglow surface
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

namespace rabbitears {
namespace mw {

// ---- command handlers ------------------------------------------------------

void onAddUrl(AppState* st) {
    if (st->busy) {
        setStatus(st, tr(i18n::StringId::StatusPlaylistLoadingWait));
        diag::warn(L"Add Playlist ignored: a playlist load is already in progress");
        return;
    }
    std::wstring url;  // no bundled/default playlist — users supply their own source
    if (!promptText(st->hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                    st->dpi, tr(i18n::StringId::AddPlaylistDialogTitle),
                    tr(i18n::StringId::AddPlaylistUrlPrompt), url))
        return;
    if (url.empty()) return;
    startPlaylistWorker(st, url, true, nameFromSource(url, true));
}

void onOpenFile(AppState* st) {
    if (st->busy) {
        setStatus(st, tr(i18n::StringId::StatusPlaylistLoadingWait));
        return;
    }
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = L"Playlists (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) startPlaylistWorker(st, path, false, nameFromSource(path, false));
}

// ---- Favourites import/export (Settings menu) --------------------------------
// Export writes the starred channels as a portable Extended-M3U (round-trips through our own
// parser and imports into any IPTV player); import stars library channels matching an M3U's
// entries — by exact stream URL first, then by tvg-id.

void onExportFavourites(AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    const std::vector<Channel> favs = st->db.favourites();
    if (favs.empty()) {
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ExportFavouritesTitle),
                       tr(i18n::StringId::ExportFavouritesNoneHeading),
                       tr(i18n::StringId::ExportFavouritesNoneBody));
        return;
    }
    wchar_t path[MAX_PATH] = L"favourites.m3u";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = L"M3U playlist (*.m3u)\0*.m3u\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.lpstrDefExt = L"m3u";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;

    M3uDocument doc;
    doc.channels.reserve(favs.size());
    for (const Channel& c : favs) {
        ParsedChannel p;
        p.name = c.name;
        p.streamUrl = c.streamUrl;
        p.logoUrl = c.logoUrl;
        p.groupTitle = c.groupTitle;
        p.tvgId = c.tvgId;
        p.tvgName = c.tvgName;
        p.chno = c.lcn.value_or(-1);
        p.userAgent = c.userAgent;
        p.referrer = c.referrer;
        doc.channels.push_back(std::move(p));
    }
    const std::string bytes = writeM3u(doc);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    // Flush explicitly and check BEFORE reporting success: a favourites file is smaller than
    // the filebuf, so the real disk write otherwise happens inside close(), which swallows
    // failures — a full disk / dying USB stick would get a success dialog over a truncated file.
    f.flush();
    if (!f) {
        diag::error(L"favourites export failed to write: " + std::wstring(path));
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ExportFavouritesTitle),
                       tr(i18n::StringId::ExportFavouritesWriteFailHeading),
                       trf(i18n::StringId::ExportFavouritesPathLine, { std::wstring(path) }));
        return;
    }
    f.close();
    diag::info(L"favourites exported: " + std::to_wstring(favs.size()) + L" -> " +
               std::wstring(path));
    setStatus(st, trf(i18n::StringId::StatusExportedFavourites, { std::to_wstring(favs.size()) }));
    showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ExportFavouritesTitle),
                   trf(i18n::StringId::ExportFavouritesDoneHeading, { std::to_wstring(favs.size()) }),
                   trf(i18n::StringId::FileLineDetail, { std::wstring(path) }));
}

void onImportFavourites(AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = st->hwnd;
    ofn.lpstrFilter = L"Playlists (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAYSIZE(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    std::wstring err;
    const M3uDocument doc = parseM3uFile(path, &err);
    if (doc.channels.empty()) {
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ImportFavouritesTitle),
                       tr(i18n::StringId::ImportFavouritesNoChannelsHeading),
                       trf(i18n::StringId::FileLineDetail, { std::wstring(path) }) +
                           (err.empty() ? L"" : trf(i18n::StringId::ProblemLineDetail, { err })));
        return;
    }
    // One pass over the library builds both match keys; the file may star a channel that
    // appears in several playlists — match ALL library rows carrying that URL / tvg-id.
    const std::vector<Channel> all = st->db.allChannels();
    std::unordered_map<std::wstring, std::vector<long long>> byUrl, byTvg;
    for (const Channel& c : all) {
        byUrl[c.streamUrl].push_back(c.id);
        if (!c.tvgId.empty()) byTvg[c.tvgId].push_back(c.id);
    }
    std::set<long long> ids;  // de-duped targets
    int missed = 0;
    for (const ParsedChannel& p : doc.channels) {
        const std::vector<long long>* hits = nullptr;
        if (auto it = byUrl.find(p.streamUrl); it != byUrl.end()) hits = &it->second;
        else if (!p.tvgId.empty())
            if (auto jt = byTvg.find(p.tvgId); jt != byTvg.end()) hits = &jt->second;
        if (hits)
            ids.insert(hits->begin(), hits->end());
        else
            ++missed;
    }
    for (long long id : ids) st->db.setFavourite(id, true);
    loadForFilter(st);  // refresh the grid (★ column / the Favourites view)
    diag::info(L"favourites imported: " + std::to_wstring(ids.size()) + L" starred, " +
               std::to_wstring(missed) + L" not in the library");
    std::wstring details = trf(i18n::StringId::ImportFavouritesDetails,
                               { std::wstring(path), std::to_wstring(doc.channels.size()),
                                 std::to_wstring(ids.size()) });
    if (missed > 0)
        details += trf(i18n::StringId::ImportFavouritesSkippedLine, { std::to_wstring(missed) });
    showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ImportFavouritesTitle),
                   trf(i18n::StringId::ImportFavouritesStarredHeading, { std::to_wstring(ids.size()) }),
                   details);
}

// ---- Named saved layouts (Settings → Layout) ----------------------------------
// Stored in the settings K/V: "layout_names" = newline-joined index (menu order), each
// layout at "layout_saved_<name>" as DockLayout::serialize(). Capped at kMaxSavedLayouts
// (the two menu-id ranges hold exactly that many).

std::vector<std::wstring> savedLayoutNames(AppState* st) {
    std::vector<std::wstring> names;
    if (auto v = st->db.getSetting(L"layout_names"); v && !v->empty()) {
        size_t pos = 0;
        while (pos <= v->size()) {
            const size_t nl = v->find(L'\n', pos);
            const std::wstring n =
                v->substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
            if (!n.empty()) names.push_back(n);
            if (nl == std::wstring::npos) break;
            pos = nl + 1;
        }
    }
    if (names.size() > static_cast<size_t>(kMaxSavedLayouts))
        names.resize(static_cast<size_t>(kMaxSavedLayouts));
    return names;
}

void storeLayoutNames(AppState* st, const std::vector<std::wstring>& names) {
    std::wstring joined;
    for (const auto& n : names) {
        if (!joined.empty()) joined += L'\n';
        joined += n;
    }
    st->db.setSetting(L"layout_names", joined);
}

void onLayoutSave(HWND hwnd, AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    std::wstring name = tr(i18n::StringId::LayoutDefaultName);
    if (!promptText(hwnd, hInst, st->dpi, tr(i18n::StringId::SaveLayoutDialogTitle),
                    tr(i18n::StringId::SaveLayoutNamePrompt), name) ||
        name.empty())
        return;
    for (wchar_t& c : name)
        if (c == L'\n' || c == L'\r') c = L' ';  // the index is newline-joined
    auto names = savedLayoutNames(st);
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        if (names.size() >= static_cast<size_t>(kMaxSavedLayouts)) {
            setStatus(st, trf(i18n::StringId::StatusLayoutLimitReached,
                              { std::to_wstring(kMaxSavedLayouts) }));
            return;
        }
        names.push_back(name);
        storeLayoutNames(st, names);
    }  // an existing name is simply overwritten below
    st->db.setSetting(L"layout_saved_" + name, st->dock.serialize());
    setStatus(st, trf(i18n::StringId::StatusLayoutSaved, { name }));
}

void onPlaylistDone(AppState* st, PlaylistResult* res) {
    st->busy = false;
    std::wstring summary, details;
    const std::wstring src = trf(i18n::StringId::PlaylistSourceLine, { res->source });
    if (res->ok && !res->doc.channels.empty()) {
        const long long now = static_cast<long long>(time(nullptr));
        const long long pid = st->db.addPlaylist(res->name, res->source, res->isUrl, now, res->doc.epgUrl);
        if (pid == 0) {
            setStatus(st, tr(i18n::StringId::StatusAddPlaylistFailedDb));
            diag::error(L"playlist add failed: addPlaylist returned 0 for " + res->source);
            summary = tr(i18n::StringId::PlaylistImportFailedHeading);
            details = src + tr(i18n::StringId::PlaylistDbSaveProblem);
        } else {
            const int n = st->db.bulkInsertChannels(pid, res->doc.channels, now);
            res->imported = n;
            refreshNav(st);
            st->filter = {ViewKind::Playlist, L"", pid};
            loadForFilter(st);
            setStatus(st, trf(i18n::StringId::StatusAddedChannels, { std::to_wstring(n), res->name }));
            diag::info(L"playlist added: \"" + res->name + L"\" (" + std::to_wstring(n) +
                       L" channels) from " + res->source);
            const int skipped = res->parsed - res->imported;
            summary = trf(i18n::StringId::PlaylistImportedHeading,
                          { std::to_wstring(res->imported), res->name });
            details = src +
                      trf(i18n::StringId::PlaylistChannelsParsedLine, { std::to_wstring(res->parsed) }) +
                      trf(i18n::StringId::PlaylistChannelsImportedLine,
                          { std::to_wstring(res->imported) });
            if (skipped > 0)
                details += trf(i18n::StringId::PlaylistSkippedLine, { std::to_wstring(skipped) });
            details += trf(i18n::StringId::PlaylistGroupsLine, { std::to_wstring(res->groups) });
        }
    } else {
        std::wstring msg = res->error;
        if (msg.empty())
            msg = res->ok ? tr(i18n::StringId::PlaylistNoChannelsMsg)
                          : tr(i18n::StringId::PlaylistNoChannelsFoundMsg);
        setStatus(st, trf(i18n::StringId::StatusAddPlaylistFailed, { msg }));
        diag::error(L"playlist add failed from " + res->source + L": " + msg);
        summary = tr(i18n::StringId::PlaylistImportFailedHeading);
        details = src + trf(i18n::StringId::ProblemLineDetail, { msg });
    }
    showInfoDialog(st->hwnd,
                   reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                   st->dpi, tr(i18n::StringId::ImportResultsTitle), summary, details);
    delete res;
}

// Fetch + store the XMLTV guide for every enabled playlist that carries an EPG URL.
// Mirrors startPlaylistWorker: the download + gunzip + parse run on a detached worker
// (busy-guarded), then WM_APP_EPG_DONE stores the parsed programmes on the UI thread.
void onEpgRefresh(AppState* st) {
    if (st->busy) {
        setStatus(st, tr(i18n::StringId::StatusBusyWait));
        return;
    }
    std::vector<EpgTarget> targets;
    for (const auto& pl : st->db.listPlaylists())
        if (pl.enabled && !pl.epgUrl.empty()) targets.push_back({pl.id, pl.name, pl.epgUrl});

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (targets.empty()) {
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::RefreshGuideTitle),
                       tr(i18n::StringId::RefreshGuideNoSourceHeading),
                       tr(i18n::StringId::RefreshGuideNoSourceBody));
        return;
    }
    st->busy = true;
    setStatus(st, tr(i18n::StringId::StatusDownloadingGuide));
    st->loadingDlg =
        showLoadingDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::TvGuideTitle),
                          tr(i18n::StringId::LoadingContactingGuide));
    diag::info(L"EPG refresh start: " + std::to_wstring(targets.size()) + L" playlist(s)");
    HWND hwnd = st->hwnd;
    std::thread([hwnd, targets]() {
        // Progress lines carry a heap wstring* the UI thread shows in the loading box, then frees.
        auto post = [hwnd](const std::wstring& s) {
            PostMessageW(hwnd, WM_APP_EPG_PROGRESS, 0, reinterpret_cast<LPARAM>(new std::wstring(s)));
        };
        const size_t n = targets.size();
        auto* res = new EpgResult();
        for (size_t i = 0; i < n; ++i) {
            const EpgTarget& t = targets[i];
            EpgFetch f;
            f.playlistId = t.id;
            f.name = t.name;
            const std::wstring tag =
                n > 1 ? trf(i18n::StringId::LoadingProgressTag,
                            { std::to_wstring(i + 1), std::to_wstring(n) })
                      : L"";
            std::string bytes;
            std::wstring err;
            post(trf(i18n::StringId::LoadingDownloadingName, { t.name, tag }));
            if (!httpGet(t.url, bytes, err, 60000)) {  // guides are large; allow 60 s
                f.error = err.empty() ? tr(i18n::StringId::EpgErrorDownloadFailed) : err;
            } else {
                post(trf(i18n::StringId::LoadingParsingName,
                         { t.name, std::to_wstring(bytes.size() / 1024), tag }));
                const std::string xml = gunzipIfNeeded(bytes);
                if (xml.empty())
                    f.error = tr(i18n::StringId::EpgErrorEmptyAfterDecompress);
                else
                    f.programmes = parseXmltv(xml).programmes;
            }
            res->fetches.push_back(std::move(f));
        }
        PostMessageW(hwnd, WM_APP_EPG_DONE, 0, reinterpret_cast<LPARAM>(res));
    }).detach();
}

void onEpgDone(AppState* st, EpgResult* res) {
    st->busy = false;
    closeLoadingDialog(st->loadingDlg);  // dismiss the "please wait" box before the results dialog
    st->loadingDlg = nullptr;
    const long long now = static_cast<long long>(time(nullptr));
    int okCount = 0, totalProg = 0;
    std::set<std::wstring> chans;
    std::wstring detail;
    for (auto& f : res->fetches) {
        if (!f.error.empty()) {
            detail += f.name + L":  " + f.error + L"\r\n";
            diag::error(L"EPG refresh failed for \"" + f.name + L"\": " + f.error);
            continue;
        }
        const int stored = st->db.bulkInsertProgrammes(f.playlistId, f.programmes, now);
        ++okCount;
        totalProg += stored;
        for (const auto& p : f.programmes) chans.insert(p.channelId);
        detail += trf(i18n::StringId::EpgDetailProgrammesLine, { f.name, std::to_wstring(stored) });
        diag::info(L"EPG stored " + std::to_wstring(stored) + L" programmes for \"" + f.name + L"\"");
    }
    std::wstring summary =
        okCount > 0 ? trf(i18n::StringId::EpgStoredSummary,
                          { std::to_wstring(totalProg), std::to_wstring(chans.size()) })
                    : tr(i18n::StringId::EpgRefreshFailedSummary);
    // A fresh guide is exactly when a series rule learns about next week's airings — force it.
    if (okCount > 0) {
        const int queued = expandRecordingRules(st, /*force=*/true);
        syncWakeFromSchedules(st);
        if (queued > 0) {
            detail += trf(i18n::StringId::EpgRulesQueuedDetail, { std::to_wstring(queued) });
            summary += trf(i18n::StringId::EpgQueuedByRulesSummary, { std::to_wstring(queued) });
        }
    }
    setStatus(st, summary);
    showInfoDialog(st->hwnd,
                   reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                   st->dpi, tr(i18n::StringId::RefreshGuideTitle), summary, detail);
    delete res;
}

// Assemble the timeline guide from stored programmes (all enabled playlists) and open
// the guide window. Programmes are joined to channels by tvg-id, and ONLY channels that
// exist in a playlist are shown (so every entry is playable). The whole loaded window
// scrolls client-side (no re-query).
// Defined below (after the scheduler helpers) — declared here for onEpgGuide's callback.
void scheduleFromGuide(AppState* st, const std::wstring& channelId, const std::wstring& channelName,
                       const std::wstring& title, long long startUtc, long long stopUtc);

void onEpgGuide(AppState* st) {
    // Opening the guide is the SLOW path (reopen — revealEpgGuide — is instant): it runs a
    // per-playlist programmesInWindow query + grouping synchronously on the UI thread, and the guide
    // window only appears at the very end. Put a "Loading TV guide…" box up first so the click has an
    // immediate, visible response (with the busy-spinner cursor) instead of a frozen window the user
    // assumes has hung. The box is painted synchronously (showLoadingDialog ends with UpdateWindow)
    // and torn down on EVERY exit below. It is a LOCAL HWND — it lives only for this synchronous call,
    // so it must NOT reuse st->loadingDlg (that belongs to the async EPG fetch; borrowing it would
    // orphan the fetch's box and leave onEpgDone closing the wrong window). No busy guard for the same
    // reason: the build only READS the DB on the UI thread, so it's safe to run during a fetch/playlist
    // load, and the fetch's own box is topmost so it still floats over the opened guide.
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    HWND loadDlg = showLoadingDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::TvGuideTitle),
                                     tr(i18n::StringId::LoadingBuildingGuide));
    // No diag timer exists on this path, so the absolute first-open cost is unmeasured; bracket it
    // (owner can't profile the GUI from the build sandbox). If builds run past ~2-3 s on real guides,
    // the fix is a worker with its OWN sqlite connection — see Win32/BACKLOG.md.
    LARGE_INTEGER freq, tBuild0, tBuild1, tShow0, tShow1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tBuild0);

    const long long now = static_cast<long long>(time(nullptr));
    const long long winStart = now - 6 * 3600;    // a little history
    const long long winEnd = now + 72 * 3600;     // three days ahead
    std::vector<GuideRow> rows;
    for (const auto& pl : st->db.listPlaylists()) {
        if (!pl.enabled) continue;
        auto progs = st->db.programmesInWindow(pl.id, winStart, winEnd);  // ordered channel_id, start
        if (progs.empty()) continue;
        // Index channels by their EPG id: iptv-org tvg-ids carry an "@feed" quality suffix
        // (e.g. "CNN.us@SD") while XMLTV feeds key on the base id ("CNN.us"), so match on the base,
        // case-insensitively (`normId`). Keep the FIRST channel per base — its FULL tvg-id becomes the
        // row's channelId, which Play/Schedule resolve via channelByTvgId, so every row stays playable.
        auto normId = [](const std::wstring& s) {
            std::wstring b = s.substr(0, s.find(L'@'));
            for (auto& ch : b)
                if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            return b;
        };
        std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> byBase;  // base -> (name, full tvg-id)
        for (const auto& c : st->db.channelsByPlaylist(pl.id))
            if (!c.tvgId.empty()) byBase.emplace(normId(c.tvgId), std::make_pair(c.name, c.tvgId));
        GuideRow cur;
        std::wstring curId;
        bool have = false;     // building a row for a channel that IS in this playlist?
        bool started = false;  // entered any channel group yet? (have can no longer double as this)
        auto flush = [&] {
            if (have && !cur.programmes.empty()) rows.push_back(std::move(cur));
            cur = GuideRow{};
            have = false;
        };
        for (auto& p : progs) {
            if (!started || p.channelId != curId) {
                flush();
                curId = p.channelId;
                started = true;
                auto it = byBase.find(normId(curId));  // programme.channelId is the EPG base id
                if (it != byBase.end()) {
                    cur.channelId = it->second.second;  // the channel's FULL tvg-id (Play/Schedule use it)
                    cur.channelName = it->second.first.empty() ? curId : it->second.first;
                    have = true;
                }
            }
            if (have) cur.programmes.push_back({p.title, p.descr, p.startUtc, p.stopUtc});
        }
        flush();
    }
    QueryPerformanceCounter(&tBuild1);
    if (rows.empty()) {
        closeLoadingDialog(loadDlg);  // dismiss before the modal info box, or it stacks on top
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::TvGuideTitle),
                       tr(i18n::StringId::GuideNoGuideHeading), tr(i18n::StringId::GuideNoGuideBody));
        return;
    }
    std::sort(rows.begin(), rows.end(),
              [](const GuideRow& a, const GuideRow& b) { return a.channelName < b.channelName; });
    GuideCallbacks cb;
    cb.onSchedule = [st](const std::wstring& channelId, const std::wstring& channelName,
                         const std::wstring& title, long long startUtc, long long stopUtc) {
        scheduleFromGuide(st, channelId, channelName, title, startUtc, stopUtc);
    };
    cb.onRecordSeries = [st](const std::wstring& channelId, const std::wstring& channelName,
                             const std::wstring& title) {
        recordSeriesFromGuide(st, channelId, channelName, title);
    };
    cb.onPlay = [st](const std::wstring& channelId, const std::wstring& channelName) {
        std::optional<Channel> ch;
        if (!channelId.empty()) ch = st->db.channelByTvgId(channelId);
        if (ch) {
            playChannel(st, *ch);
            // Starting a show hides the guide (a big window over the viewer) and brings the main
            // window forward, so the picked channel is actually visible instead of playing behind
            // the guide. Reopen 📺 TV Guide to bring it back.
            hideEpgGuide();
            SetForegroundWindow(st->hwnd);
        } else {
            // setStatus lands in the status bar, which is behind the guide — the user never saw
            // it. Say why loudly instead (almost always a tvg-id mismatch, per the guide caveat).
            HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
            showInfoDialog(st->hwnd, hi, st->dpi, tr(i18n::StringId::TvGuideTitle),
                           tr(i18n::StringId::GuideNoMatchHeading),
                           trf(i18n::StringId::GuideNoMatchBody, { channelName }));
        }
    };
    cb.isFavourite = [st](const std::wstring& channelId) {
        const auto ch = st->db.channelByTvgId(channelId);
        return ch && ch->favourite;
    };
    cb.onToggleFavourite = [st](const std::wstring& channelId, const std::wstring& channelName) {
        const auto ch = st->db.channelByTvgId(channelId);
        if (!ch) return;
        st->db.toggleFavourite(ch->id);        // ch->favourite is the pre-toggle state
        loadForFilter(st);                     // refresh the grid's favourite column / Favourites view
        setStatus(st, ch->favourite ? trf(i18n::StringId::StatusRemovedFavourite, { channelName })
                                    : trf(i18n::StringId::StatusAddedFavourite, { channelName }));
    };
    closeLoadingDialog(loadDlg);  // dismiss the box before the guide window paints
    const size_t nRows = rows.size();  // capture before the move below empties `rows`
    QueryPerformanceCounter(&tShow0);
    showEpgGuide(st->hwnd, hInst, st->dpi, std::move(rows), now, cb);
    QueryPerformanceCounter(&tShow1);
    auto ms = [&](LONGLONG a, LONGLONG b) { return std::to_wstring((b - a) * 1000 / freq.QuadPart); };
    diag::info(L"TV guide first-open: DB+build " + ms(tBuild0.QuadPart, tBuild1.QuadPart) +
               L" ms, window " + ms(tShow0.QuadPart, tShow1.QuadPart) + L" ms (" +
               std::to_wstring(nRows) + L" channels)");
}

// Prompt for a playlist's XMLTV guide URL (seeded with its current one), save the override,
// and offer to fetch it now. Shared by the playlist context menu and the TV Guide node menu.
void promptSetGuideUrl(HWND hwnd, AppState* st, long long pid) {
    std::wstring url;  // seed with the current URL (M3U x-tvg-url or a prior override)
    for (const auto& pl : st->db.listPlaylists())
        if (pl.id == pid) { url = pl.epgUrl; break; }
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    if (!promptText(hwnd, hInst, st->dpi, tr(i18n::StringId::SetGuideUrlTitle),
                    tr(i18n::StringId::SetGuideUrlPrompt), url))
        return;
    st->db.setPlaylistEpgUrl(pid, url);
    diag::info(L"set epg_url for playlist id=" + std::to_wstring(pid) +
               (url.empty() ? L" (cleared)" : L" to \"" + url + L"\""));
    if (url.empty()) {
        setStatus(st, tr(i18n::StringId::StatusGuideUrlCleared));
    } else if (MessageBoxW(hwnd, tr(i18n::StringId::SetGuideUrlSavedPrompt).c_str(),
                           tr(i18n::StringId::SetGuideUrlTitle).c_str(),
                           MB_YESNO | MB_ICONQUESTION) == IDYES) {
        onEpgRefresh(st);  // fetches every enabled playlist that has a URL
    } else {
        setStatus(st, tr(i18n::StringId::StatusGuideUrlSaved));
    }
}

void toggleFullscreen(AppState* st) {
    HWND hwnd = st->hwnd;
    st->fullscreen = !st->fullscreen;
    if (st->fullscreen) {
        // Real fullscreen: remember the window, drop the frame to a borderless
        // popup, and cover the whole monitor (over the taskbar). Windows treats a
        // borderless window covering the monitor as fullscreen and hides the shell.
        st->prevPlacement.length = sizeof(st->prevPlacement);
        GetWindowPlacement(hwnd, &st->prevPlacement);
        st->prevStyle = GetWindowLongW(hwnd, GWL_STYLE);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (st->prevStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetFocus(st->ap().hwnd);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, st->prevStyle);
        SetWindowPlacement(hwnd, &st->prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout(hwnd, st);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// "Video only": collapse the window to just the video — hide the nav list, channel grid,
// title/command bar, and transport strip — WITHOUT leaving the window (unlike fullscreen,
// which covers the whole monitor). Entered from Settings → Video only; a double-click on
// the video or Esc restores the chrome. Reuses the fullscreen layout/paint path (which
// already hides every child + fills the client with the video), minus the window-style
// change. A no-op while actually fullscreen — that mode already shows only the video.
void toggleVideoOnly(AppState* st) {
    // Entering video-only from fullscreen: drop fullscreen first (the two are mutually exclusive),
    // so "Video only" from the fullscreen right-click menu lands in windowed video-only instead of
    // doing nothing. toggleFullscreen clears st->fullscreen, so the toggle below then proceeds.
    if (st->fullscreen) toggleFullscreen(st);
    st->videoOnly = !st->videoOnly;
    layout(st->hwnd, st);
    InvalidateRect(st->hwnd, nullptr, TRUE);
    if (st->videoOnly) SetFocus(st->ap().hwnd);  // so Esc reaches VideoProc while the chrome is hidden
}

// ---- multi-player panes (split view / PIP) ---------------------------------

// Create video pane #index: a kVideoClass child window (its index stashed in GWLP_USERDATA
// so VideoProc knows which pane it is) bound to a fresh VlcPlayer that borrows the shared
// engine. Each pane posts events tagged with its index; only the active pane drives the
// transport strip. Created hidden — layout() shows the panes the current mode uses.
VideoPane* addPane(HWND hwnd, AppState* st, int index, bool floating) {  // default in the header proto
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    auto pane = std::make_unique<VideoPane>();
    pane->floating = floating;
    if (floating) {
        // PIP: a top-level TOPMOST popup owned by the main window, so DWM composites it above the
        // main window's libVLC D3D surface (a child sibling — even an owned, non-topmost popup —
        // gets drawn under that surface and stays invisible). NOACTIVATE so clicking doesn't steal
        // focus; TOOLWINDOW keeps it off the taskbar. Created hidden; positionFloatingPip() sizes +
        // shows it in SCREEN coords.
        pane->hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kVideoClass,
                                     L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 100, 100, hwnd, nullptr,
                                     hInst, nullptr);
    } else {
        // A tile in the video area. Pane 0 is visible from creation (parity with the old single
        // video window); extra tiles start hidden and layout() shows them.
        const DWORD style = WS_CHILD | WS_CLIPSIBLINGS | (index == 0 ? WS_VISIBLE : 0u);
        pane->hwnd = CreateWindowExW(0, kVideoClass, L"", style, 0, 0, 10, 10, hwnd, nullptr, hInst,
                                     nullptr);
    }
    SetWindowLongPtrW(pane->hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(index));
    VideoPane* raw = pane.get();
    st->panes.push_back(std::move(pane));
    raw->player.init(st->engine);
    raw->player.attach(raw->hwnd);  // the pane HWND — the pool's fallback, never a set_hwnd target
    raw->player.setEventTarget(hwnd, WM_APP_VLC);
    raw->player.setTag(index);
    raw->player.setVoutHostMsg(WM_APP_MAKE_VOUT_HOST);  // lets the worker grow the host pool on demand
    // Pre-create a small pool of vout hosts for this pane (hidden). libVLC renders into a host, never
    // the pane HWND directly: a new stream attaches to a proven-free host so the previous stream's
    // vout can drain without spawning the "VLC (Direct3D11 output)" popout on rapid channel-surf. Two
    // covers steady-state ping-pong; the worker grows the pool (WM_APP_MAKE_VOUT_HOST) under churn.
    for (int i = 0; i < 2; ++i)
        if (HWND h = makeVoutHost(st, index)) raw->player.registerVoutHost(h);
    if (index != st->active) raw->player.setMuted(true);  // only the active pane is audible
    return raw;
}

// Make pane `idx` the active one: route audio (others muted), the grid now-playing
// highlight, the play/pause glyph, the meters, and the active-pane border to it. Safe if
// out of range (no-op).
void setActivePane(AppState* st, int idx) {
    if (idx < 0 || idx >= static_cast<int>(st->panes.size())) return;
    diag::info(L"active pane -> " + std::to_wstring(idx) + L" (of " +
               std::to_wstring(st->panes.size()) + L")");
    st->active = idx;
    const int vol = static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0));
    for (int i = 0; i < static_cast<int>(st->panes.size()); ++i) {
        st->panes[i]->player.setMuted(i != idx);         // only the active pane keeps its audio track
        if (i == idx) st->panes[i]->player.setVolume(vol);
    }
    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
    SetWindowTextW(st->btnPlay, st->ap().player.isPlaying() ? kGlyphPause : kGlyphPlay);
    // Recording is PER-PANE (each pane's player has its own recorder) — the Record button
    // reflects and controls the ACTIVE pane, so its glyph must follow the pane switch.
    SetWindowTextW(st->btnRec, st->ap().player.isRecording() ? kGlyphStop : kGlyphRecord);
    resetStatMeters(st);  // the previous pane's readings don't apply to the newly-active stream
    setStatus(st, st->ap().nowPlayingName.empty()
                      ? trf(i18n::StringId::StatusActivePane, { std::to_wstring(idx + 1) })
                      : trf(i18n::StringId::StatusActiveChannel, { st->ap().nowPlayingName }));
    // erase=TRUE: the active-pane accent border is drawn in the inter-pane gap, so the PREVIOUS
    // active pane's border must be cleared (windowBg fill via WM_ERASEBKGND) — otherwise borders
    // accumulate and every tile ends up looking "active". WS_CLIPCHILDREN keeps the fill off the
    // video surfaces, so only the gaps repaint (no video flicker).
    InvalidateRect(st->hwnd, nullptr, TRUE);  // repaint the active-pane border (clearing the old one)
}

// Switch the view mode. Every pane except pane 0 is torn down and recreated for the target mode,
// because their window TYPE differs — Split tiles are child windows, the PIP pane is a floating
// top-level popup. Pane 0 (the primary child surface) always persists and keeps playing.
// Reap panes torn down by a mode switch once their async stop has finished (or force at exit).
// Runtime-safe: a pane whose reaper is still stopping a stuck stream is left for the next pass, so
// the UI thread never blocks. force=true (WM_DESTROY) drains everything synchronously — the exit
// watchdog covers a truly wedged feed.
void reapDyingPanes(AppState* st, bool force) {
    for (auto it = st->dyingPanes.begin(); it != st->dyingPanes.end();) {
        VideoPane* p = it->get();
        if (force || p->player.teardownComplete()) {
            p->player.shutdown();                 // joins the now-finished reaper instantly
            if (p->hwnd) DestroyWindow(p->hwnd);  // UI thread — safe now (the pane's vout is gone)
            it = st->dyingPanes.erase(it);
        } else {
            ++it;
        }
    }
}

void applyViewMode(AppState* st, ViewMode mode) {
    reapDyingPanes(st, /*force=*/false);  // clear any finished leftovers from a previous switch
    // A mode switch tears down every pane except pane 0 — including their RECORDERS (the async
    // teardown enqueues a recorder stop). Never kill a background recording silently: confirm
    // first, and if a scheduled recording lives in a dying pane, close out its DB row.
    {
        int recCount = 0;
        for (size_t i = 1; i < st->panes.size(); ++i)
            if (st->panes[i]->player.isRecording()) ++recCount;
        if (recCount > 0) {
            wchar_t msg[160];
            // The count is baked as "(s)" in the template — Japanese has no plural and the old
            // runtime %s "s" suffix leaked a stray Latin s into the Japanese string.
            swprintf_s(msg, tr(i18n::StringId::ViewSwitchStopRecordingConfirm).c_str(), recCount);
            if (MessageBoxW(st->hwnd, msg, tr(i18n::StringId::AppName).c_str(),
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return;
            if (st->activeScheduleId != 0 && st->schedulePane >= 1) {
                st->db.updateScheduleStatus(st->activeScheduleId, ScheduleStatus::Done);
                diag::info(L"scheduled recording closed by view switch (id " +
                           std::to_wstring(st->activeScheduleId) + L")");
                st->activeScheduleId = 0;
                st->schedulePane = -1;
            }
        }
    }
    // Carry the SELECTED pane's stream across the collapse. Pane 0 (the primary child surface)
    // always persists, but only pane 0's HWND survives — not necessarily its channel. If a
    // NON-pane-0 tile is active (e.g. you picked the bottom-right of a 2x2), remember what it was
    // playing and replay it into pane 0 below, so leaving Split/PIP keeps what you were watching
    // instead of snapping back to the top-left tile.
    const bool carryStream = st->active != 0 && st->ap().nowPlayingId != 0;
    const Channel carry = carryStream ? st->ap().nowPlaying : Channel{};
    // Tear the extra panes down ASYNCHRONOUSLY. libVLC 3.x stop()/release() block for seconds on a
    // stuck IPTV feed; doing that synchronously here (player.shutdown()) froze the UI thread — an
    // AppHang — when entering/leaving 2x2 or PIP with flaky streams. beginTeardown() hands the
    // blocking stop to a reaper thread and the pane parks (hidden) in dyingPanes until reaped.
    while (static_cast<int>(st->panes.size()) > 1) {
        auto p = std::move(st->panes.back());
        st->panes.pop_back();
        p->player.beginTeardown();                  // non-blocking; the blocking stop runs off-thread
        if (p->hwnd) ShowWindow(p->hwnd, SW_HIDE);   // hide until its vout is gone + we DestroyWindow it
        st->dyingPanes.push_back(std::move(p));
    }
    st->active = 0;
    st->viewMode = mode;
    // Persist here — the single choke point every entry path funnels through (Settings menu,
    // the video right-click menu, AND the grid's "Play in PIP"), so the mode survives a restart.
    st->db.setSetting(L"view_mode", std::to_wstring(static_cast<int>(mode)));
    st->pipMoved = false;  // a fresh PIP starts in the default corner (until the user drags it)
    if (mode == ViewMode::Split)
        for (int i = 1; i < 4; ++i) addPane(st->hwnd, st, i);        // three more tiles -> 2x2
    else if (mode == ViewMode::Pip)
        addPane(st->hwnd, st, 1, /*floating=*/true);                 // the floating PIP popup
    // Replay the carried selection into pane 0 (st->active is 0 now, so it plays audible) unless
    // pane 0 already has it. This is what makes "the selected stream survives" true.
    if (carryStream && st->panes[0]->nowPlayingId != carry.id)
        playChannelInPane(st, carry, 0);
    setActivePane(st, 0);
    layout(st->hwnd, st);
    InvalidateRect(st->hwnd, nullptr, TRUE);
}

std::wstring recordingsDir() {
    PWSTR vids = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vids)))
        dir = std::filesystem::path(vids) / L"RabbitEars";
    if (vids) CoTaskMemFree(vids);
    if (dir.empty()) dir = std::filesystem::temp_directory_path() / L"RabbitEars";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.wstring();
}

// Map a recording-format setting to its file extension + libVLC mux name. "mp4" is a
// DIRECT mp4 mux (stream copy): the mux writes its moov index when the recording stops,
// so a normal stop (manual, scheduled, quit — all finalize through doRecordStop) yields a
// playable file; only a hard crash mid-record loses one (unlike .ts, readable up to the
// cut). Anything unrecognized falls back to ts, the crash-safest container.
void formatToExtMux(const std::wstring& fmt, std::wstring& ext, std::string& mux) {
    if (fmt == L"mkv") {
        ext = L".mkv";
        mux = "mkv";
    } else if (fmt == L"mp4") {
        ext = L".mp4";
        mux = "mp4";
    } else {
        ext = L".ts";
        mux = "ts";
    }
}

std::wstring recordingPath(const std::wstring& channelName, const std::wstring& ext) {
    std::wstring name = channelName;
    for (wchar_t& ch : name)
        if (ch < 0x20 || wcschr(L"\\/:*?\"<>|'{},", ch)) ch = L'_';  // filename- + sout-safe
    if (name.empty()) name = L"channel";
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[32];
    swprintf_s(ts, L"%04d-%02d-%02d %02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    return recordingsDir() + L"\\" + name + L" - " + ts + ext;
}

void onToggleRecord(AppState* st) {
    // Recording is PER-PANE: the button toggles the ACTIVE pane's recorder, and other panes'
    // recordings run concurrently (each pane's player owns an independent recorder). A scheduled
    // recording only blocks the pane it is recording on — that pane's recorder belongs to the
    // scheduler, but any other pane records freely alongside it.
    if (st->activeScheduleId != 0 && st->schedulePane == st->active) {
        setStatus(st, tr(i18n::StringId::StatusScheduleOwnsPane));
        return;
    }
    if (st->ap().player.isRecording()) {
        const std::wstring file = st->ap().player.recordingFile();
        st->ap().player.stopRecording();
        SetWindowTextW(st->btnRec, kGlyphRecord);
        setStatus(st, trf(i18n::StringId::StatusRecordingSaved, { file }));
        syncKeepAwake(st);  // another pane may still be recording — re-derive, don't assume
        return;
    }
    if (st->ap().nowPlaying.id == 0) {
        setStatus(st, tr(i18n::StringId::StatusPlayChannelFirst));
        return;
    }
    std::wstring ext;
    std::string mux;
    formatToExtMux(st->recFormat, ext, mux);
    const std::wstring path = recordingPath(st->ap().nowPlaying.name, ext);
    if (st->ap().player.startRecording(st->ap().nowPlaying.streamUrl, st->ap().nowPlaying.userAgent,
                                  st->ap().nowPlaying.referrer, path, mux)) {
        SetWindowTextW(st->btnRec, kGlyphStop);
        setStatus(st, trf(i18n::StringId::StatusRecordingNow, { st->ap().nowPlaying.name, path }));
        syncKeepAwake(st);  // don't let the machine sleep out from under a manual recording
    }
}

// Stop the recorder owned by the ACTIVE schedule and release ownership. Per-pane recording:
// the recorder lives on the pane the schedule STARTED on (st->schedulePane), not necessarily
// the active pane — stopping st->ap().player here once silently killed an unrelated manual
// recording (review-caught). When the pinned pane is GONE (a view switch already closed the
// recording out) no recorder is stopped — NEVER fall back to the active pane, whose recording
// (if any) belongs to the user. Shared by the scheduler tick and the manager's Cancel/Delete.
void stopScheduledRecorder(AppState* st) {
    VlcPlayer* rec = nullptr;
    if (st->schedulePane >= 0 && st->schedulePane < static_cast<int>(st->panes.size()))
        rec = &st->panes[st->schedulePane]->player;
    else if (st->schedulePane < 0)
        rec = &st->ap().player;  // no pin recorded (pre-0.2.6 row) — old single-recorder path
    if (rec) rec->stopRecording();
    st->activeScheduleId = 0;
    st->schedulePane = -1;
    // stopRecording() only ENQUEUES — the stopped player's isRecording() still reads true here
    // (the worker clears it after the blocking stop). If we just stopped the ACTIVE pane's
    // recorder the glyph must show Record unconditionally; otherwise the active pane was
    // untouched and its live state is what the button should reflect.
    const bool stoppedActive = rec == &st->ap().player;
    SetWindowTextW(st->btnRec, (!stoppedActive && st->ap().player.isRecording()) ? kGlyphStop
                                                                                 : kGlyphRecord);
    // The stop is async, so isRecording() may still read true for this player — a stale "keep
    // awake" for one 30 s tick is harmless (the tick re-syncs), while releasing too eagerly
    // could let the machine sleep on a recorder that hasn't finished flushing.
    syncKeepAwake(st);
}

// The recording scheduler tick (~30s): decide via the pure planScheduler() core, then
// apply — driving the single shared recorder and writing status back to the DB. Runs
// only while the app is open (a schedule whose window passed while closed is marked
// Missed on the next tick). Guards against the manual Record button via activeScheduleId.
void onSchedulerTick(AppState* st) {
    reapDyingPanes(st, /*force=*/false);  // backstop: reap finished mode-switch panes (~30s tick)
    auto schedules = st->db.listSchedules();
    // NB: there is deliberately NO early return on an empty queue. The tail of this function
    // (rule expansion, keep-awake, wake-task upkeep) must run even with nothing queued —
    // otherwise a still-enabled series rule whose rows were all deleted would never re-queue,
    // and a stale wake task would never be cleared (both review-caught). planScheduler() over an
    // empty vector simply yields empty plans, so the loops below are no-ops.
    // One-time startup reconcile: a schedule still marked Recording is stale (a prior
    // session was closed mid-record — nothing is actually recording now), so reset it to
    // Pending and let planScheduler resume it (if still in window) or miss it.
    if (!st->schedulerReconciled) {
        st->schedulerReconciled = true;
        bool changed = false;
        for (const auto& s : schedules)
            if (s.status == ScheduleStatus::Recording) {
                st->db.updateScheduleStatus(s.id, ScheduleStatus::Pending);
                changed = true;
            }
        if (changed) schedules = st->db.listSchedules();
    }
    const long long now = static_cast<long long>(time(nullptr));
    // "Manual" recording = the ACTIVE pane's recorder is busy but no schedule owns it. The
    // scheduler starts on the active pane, so only that pane's manual recording blocks it —
    // recordings in other panes run concurrently (per-pane recorders, 0.2.6).
    const bool manualRecording = st->ap().player.isRecording() && st->activeScheduleId == 0;
    const SchedulerPlan plan = planScheduler(schedules, now, manualRecording);

    for (long long id : plan.stop) {
        // Only the schedule that actually OWNS a recorder gets one stopped; a foreign
        // Recording row (shouldn't happen) is just closed out in the DB.
        if (id == st->activeScheduleId) stopScheduledRecorder(st);
        st->db.updateScheduleStatus(id, ScheduleStatus::Done);
        diag::info(L"scheduled recording finished (id " + std::to_wstring(id) + L")");
        setStatus(st, tr(i18n::StringId::StatusScheduledRecordingSaved));
    }
    for (long long id : plan.miss) {
        st->db.updateScheduleStatus(id, ScheduleStatus::Missed);
        diag::warn(L"scheduled recording missed (id " + std::to_wstring(id) + L")");
    }
    for (long long id : plan.start) {  // planScheduler yields at most one
        const ScheduledRecording* s = nullptr;
        for (const auto& x : schedules)
            if (x.id == id) { s = &x; break; }
        if (!s) continue;
        std::wstring ext;
        std::string mux;
        formatToExtMux(s->mux, ext, mux);
        const std::wstring path = recordingPath(s->channelName, ext);
        if (st->ap().player.startRecording(s->streamUrl, s->userAgent, s->referrer, path, mux)) {
            st->activeScheduleId = id;
            st->schedulePane = st->active;  // pin the schedule to the pane it records on
            st->db.updateScheduleStatus(id, ScheduleStatus::Recording, path);
            SetWindowTextW(st->btnRec, kGlyphStop);  // the active pane's recorder just engaged
            diag::info(L"scheduled recording started: " + s->channelName + L" -> " + path);
            setStatus(st, trf(i18n::StringId::StatusRecordingScheduledNow, { s->channelName }));
        } else {
            st->db.updateScheduleStatus(id, ScheduleStatus::Failed);
            diag::error(L"scheduled recording failed to start (id " + std::to_wstring(id) + L")");
        }
    }

    // Rules first (they may queue an airing that starts within this tick), then the two
    // side-effects that must track the queue: sleep suppression and the wake task.
    expandRecordingRules(st);
    syncKeepAwake(st);
    syncWakeFromSchedules(st);
}

// ---- Recording Phase 3: keep-awake, wake task, and EPG-driven rules ----------

void syncKeepAwake(AppState* st) {
    bool any = false;
    for (const auto& p : st->panes)
        if (p->player.isRecording()) { any = true; break; }
    setRecordingKeepAwake(any);  // per-thread state — always called on the UI thread
}

void syncWakeFromSchedules(AppState* st) {
    // The schedule start the task SHOULD target: the earliest still-pending one (0 = none).
    long long earliest = 0;
    if (st->wakeToRecord)
        for (const ScheduledRecording& s : st->db.listSchedules())
            if (s.status == ScheduleStatus::Pending && (earliest == 0 || s.startUtc < earliest))
                earliest = s.startUtc;

    // This runs on every ~30 s tick, and touching Task Scheduler means a COM round-trip. Key on
    // the UNCLAMPED start so an unchanged queue costs nothing; only a changed target re-registers.
    if (earliest == st->wakeTaskFor) return;

    if (earliest == 0) {
        clearWakeTask();  // nothing queued (or the feature is off) — never wake a PC for nothing
        st->wakeTaskFor = 0;
        return;
    }
    // Clamp the FIRE time into the future: a past boundary would make Task Scheduler run the task
    // immediately, launching a second instance that just bounces off the single-instance mutex. If
    // we are exiting with an imminent recording, +30 s still relaunches us in time to catch it
    // (planScheduler happily starts a schedule mid-window).
    const long long now = static_cast<long long>(time(nullptr));
    if (syncWakeTask(std::max(earliest - kWakeLeadSeconds, now + 30)))
        st->wakeTaskFor = earliest;
    // On failure leave wakeTaskFor unchanged so the next tick retries rather than going quiet.
}

// Materialize enabled rules against the stored EPG. Pure matching lives in
// common/core/RecordingRules; here we only resolve each match to a playable stream (the core
// has no DB) and insert. Returns the number of schedules added.
int expandRecordingRules(AppState* st, bool force) {
    if (!st->db.isOpen()) return 0;
    const auto rules = st->db.listRules();
    if (rules.empty()) return 0;  // the common case: no rules, no cost
    const long long now = static_cast<long long>(time(nullptr));
    // The EPG query below spans 14 days across every enabled playlist — far too heavy to run on
    // every ~30 s tick. New airings only arrive with a guide refresh, which forces a pass anyway.
    if (!force && st->rulesExpandedAt != 0 && now - st->rulesExpandedAt < kRuleExpandIntervalSeconds)
        return 0;
    const long long horizon = now + kRuleHorizonSeconds;
    const auto programmes = st->db.programmesInWindowAll(now, horizon);
    if (programmes.empty()) {
        st->rulesExpandedAt = now;  // don't re-query every tick just because the guide is empty
        return 0;
    }

    const auto planned = expandRules(rules, programmes, st->db.listSchedules(), now, horizon);
    int added = 0;
    for (ScheduledRecording s : planned) {
        // The core can't look a channel up; a rule for a channel that has since left the
        // library simply produces nothing (rather than an unrecordable row).
        const auto ch = st->db.channelByTvgId(s.channelId);
        if (!ch) continue;
        s.channelName = ch->name.empty() ? s.channelName : ch->name;
        s.streamUrl = ch->streamUrl;
        s.userAgent = ch->userAgent;
        s.referrer = ch->referrer;
        s.createdAt = now;
        if (st->db.addSchedule(s) > 0) ++added;
    }
    st->rulesExpandedAt = now;
    if (added > 0)
        diag::info(L"recording rules queued " + std::to_wstring(added) + L" upcoming airing(s)");
    return added;
}

// Schedule a recording for a guide programme: resolve its tvg-id to a recordable stream,
// store the (self-contained) schedule, nudge the scheduler in case it is already airing,
// and confirm. Called from the TV Guide's right-click "Schedule recording".
void scheduleFromGuide(AppState* st, const std::wstring& channelId, const std::wstring& channelName,
                       const std::wstring& title, long long startUtc, long long stopUtc) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    std::optional<Channel> ch;
    if (!channelId.empty()) ch = st->db.channelByTvgId(channelId);
    if (!ch) {
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ScheduleRecordingTitle),
                       tr(i18n::StringId::ChannelNotFoundHeading),
                       trf(i18n::StringId::ScheduleChannelNotFoundBody, { channelName }));
        return;
    }
    ScheduledRecording s;
    s.channelId = channelId;
    s.channelName = ch->name.empty() ? channelName : ch->name;
    s.streamUrl = ch->streamUrl;
    s.userAgent = ch->userAgent;
    s.referrer = ch->referrer;
    s.title = title;
    s.startUtc = startUtc;
    s.stopUtc = stopUtc;
    s.mux = st->recFormat;  // the app's current TS/MKV setting
    s.createdAt = static_cast<long long>(time(nullptr));
    if (st->db.addSchedule(s) > 0) {
        onSchedulerTick(st);  // start immediately if the programme is already on air
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ScheduleRecordingTitle),
                       tr(i18n::StringId::ScheduleRecordingScheduledHeading),
                       trf(i18n::StringId::ScheduleRecordingScheduledBody, { title, s.channelName }));
    } else {
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::ScheduleRecordingTitle),
                       tr(i18n::StringId::ScheduleCouldNotHeading),
                       tr(i18n::StringId::ScheduleCouldNotBody));
    }
}

// "Record series" from the guide: store a rule that matches this programme's title on this
// channel, then expand it immediately so the user sees the upcoming airings queue up.
void recordSeriesFromGuide(AppState* st, const std::wstring& channelId,
                           const std::wstring& channelName, const std::wstring& title) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (title.empty()) return;
    std::optional<Channel> ch;
    if (!channelId.empty()) ch = st->db.channelByTvgId(channelId);
    if (!ch) {
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::RecordSeriesTitle),
                       tr(i18n::StringId::ChannelNotFoundHeading),
                       trf(i18n::StringId::RecordSeriesChannelNotFoundBody, { channelName }));
        return;
    }
    // Reject an exact duplicate so mashing the button doesn't pile up identical rules.
    for (const RecordingRule& r : st->db.listRules())
        if (normaliseTvgId(r.channelId) == normaliseTvgId(channelId) && r.titleMatch == title &&
            r.match == RuleMatch::Exact) {
            showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::RecordSeriesTitle),
                           tr(i18n::StringId::RecordSeriesAlreadyHeading),
                           trf(i18n::StringId::RecordSeriesAlreadyBody, { title, ch->name }));
            return;
        }

    RecordingRule r;
    r.channelId = channelId;
    r.channelName = ch->name.empty() ? channelName : ch->name;
    r.titleMatch = title;
    r.match = RuleMatch::Exact;  // the guide gives us the exact title; Contains would over-match
    r.mux = st->recFormat;
    r.createdAt = static_cast<long long>(time(nullptr));
    if (st->db.addRule(r) == 0) {
        showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::RecordSeriesTitle),
                       tr(i18n::StringId::RecordSeriesCouldNotSaveHeading),
                       tr(i18n::StringId::RecordSeriesCouldNotSaveBody));
        return;
    }
    const int added = expandRecordingRules(st, /*force=*/true);  // a brand-new rule must queue now
    syncWakeFromSchedules(st);
    onSchedulerTick(st);  // catch an airing that is already on
    diag::info(L"series rule added: \"" + title + L"\" on " + r.channelName);
    showInfoDialog(st->hwnd, hInst, st->dpi, tr(i18n::StringId::RecordSeriesTitle),
                   tr(i18n::StringId::RecordSeriesEveryAiringHeading),
                   trf(i18n::StringId::RecordSeriesEveryAiringBody,
                       { title, r.channelName, std::to_wstring(added) }));
}

// Settings → Recording Rules…: list the standing series rules, with enable/disable + delete.
// Deleting a rule drops its still-pending schedules but keeps recordings that already ran.
void onManageRules(AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    RuleManagerCallbacks cb;
    cb.list = [st] { return st->db.listRules(); };
    cb.setEnabled = [st](long long id, bool on) {
        st->db.setRuleEnabled(id, on);
        if (on) expandRecordingRules(st, /*force=*/true);  // a re-enabled rule re-queues at once
        syncWakeFromSchedules(st);
    };
    cb.remove = [st](long long id) {
        st->db.deleteRule(id);
        syncWakeFromSchedules(st);  // its pending rows are gone; the wake time may have moved
    };
    manageRules(st->hwnd, hInst, st->dpi, cb);
}

// Settings ▸ Scheduled Recordings… — the manager (list + New/Cancel/Delete). The host
// callbacks own the recorder + DB so cancel/delete stop an active recording, and New
// opens scheduleDialog over the manager and stores + nudges the scheduler.
void onManageSchedules(AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    ScheduleManagerCallbacks cb;
    // Registering the wake task says nothing about whether Windows will ARM it. Ask before the
    // user queues something the machine will sleep straight through. Only when they expect a wake
    // at all — with the feature off, "the app must be running" is already the documented deal.
    if (st->wakeToRecord) cb.wakeWarning = wakeVerdictText(queryWakePolicy());
    cb.list = [st] { return st->db.listSchedules(); };
    cb.cancel = [st](long long id) {
        // Per-pane recording: the schedule's recorder lives on its PINNED pane — the helper
        // stops the right one (stopping the active pane here once cut a manual recording).
        if (st->activeScheduleId == id) stopScheduledRecorder(st);
        st->db.updateScheduleStatus(id, ScheduleStatus::Cancelled);
        syncWakeFromSchedules(st);  // the earliest pending start may have just moved
    };
    cb.remove = [st](long long id) {
        if (st->activeScheduleId == id) stopScheduledRecorder(st);
        // A still-PENDING row that a series rule generated is a materialised prediction: hard
        // deleting it leaves no dedup anchor, so the very next expansion would recreate it and
        // the user's delete would silently undo itself. Cancel it instead — the Cancelled
        // tombstone is precisely what tells the rule "skip this airing". One-off rows, and rows
        // that already ran, delete for real.
        bool ruleTombstone = false;
        for (const ScheduledRecording& s : st->db.listSchedules())
            if (s.id == id) {
                ruleTombstone = (s.ruleId != 0 && s.status == ScheduleStatus::Pending);
                break;
            }
        if (ruleTombstone) {
            st->db.updateScheduleStatus(id, ScheduleStatus::Cancelled);
            setStatus(st, tr(i18n::StringId::StatusAiringCancelledRule));
        } else {
            st->db.deleteSchedule(id);
        }
        syncWakeFromSchedules(st);
    };
    cb.addNew = [st](HWND owner) {
        HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
        ScheduledRecording d;
        if (scheduleDialog(owner, hi, st->dpi, st->db.allChannels(), d)) {
            d.mux = st->recFormat;
            d.createdAt = static_cast<long long>(time(nullptr));
            if (st->db.addSchedule(d) > 0) onSchedulerTick(st);  // start now if already airing
        }
    };
    manageSchedules(st->hwnd, hInst, st->dpi, cb);
}

// Serialize the category include-set as a newline-joined list. Group titles come
// from a single M3U line so they never contain a newline; an empty stored value
// means the filter is off (show everything).
std::wstring joinCategories(const std::set<std::wstring>& s) {
    std::wstring out;
    for (const std::wstring& g : s) {
        if (!out.empty()) out += L'\n';
        out += g;
    }
    return out;
}

std::set<std::wstring> splitCategories(const std::wstring& s) {
    std::set<std::wstring> out;
    std::wstring cur;
    for (wchar_t ch : s) {
        if (ch == L'\n') {
            if (!cur.empty()) out.insert(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) out.insert(cur);
    return out;
}

// Settings → Categories…: a checklist over the distinct group titles. The include
// set is normalized so "all checked" and "none checked" both mean no restriction.
void onCategories(AppState* st) {
    std::vector<std::wstring> groups = st->db.listGroups();
    if (groups.empty()) {
        setStatus(st, tr(i18n::StringId::StatusNoCategories));
        showInfoDialog(st->hwnd,
                       reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                       st->dpi, tr(i18n::StringId::CategoriesTitle),
                       tr(i18n::StringId::CategoriesNoneHeading),
                       tr(i18n::StringId::CategoriesNoneBody));
        return;
    }
    std::set<std::wstring> checked;
    if (st->categoryActive)
        checked = st->categories;
    else
        checked.insert(groups.begin(), groups.end());  // all checked == no restriction

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (!chooseCategories(st->hwnd, hInst, st->dpi, groups, checked))
        return;  // cancelled — leave the current filter untouched

    const std::set<std::wstring> allSet(groups.begin(), groups.end());
    if (checked.empty() || checked == allSet) {
        st->categoryActive = false;
        st->categories.clear();
        st->db.setSetting(L"category_filter", L"");
    } else {
        st->categoryActive = true;
        st->categories = std::move(checked);
        st->db.setSetting(L"category_filter", joinCategories(st->categories));
    }
    loadForFilter(st);  // re-apply to the current nav view (mirrors Hide unavailable)
}

// Start/stop the audio spectrum tap to match the spectrum meter's visibility (no
// point capturing audio nobody's watching). The tap is read-only WASAPI loopback,
// so this never affects playback; on failure it just delivers nothing.
void syncSpectrumTap(AppState* st) {
    if (st->showSpectrum && st->meterSpectrum) {
        if (!st->spectrumTap.running()) {
            HWND m = st->meterSpectrum;
            st->spectrumTap.start(
                [m](const float* bands) { miniMeterPushSpectrum(m, bands, SpectrumTap::kBands); });
        }
    } else {
        st->spectrumTap.stop();
    }
}

// Clear the stat-driven mini meters back to idle (the spectrum decays on its own
// once the audio goes silent).
void resetStatMeters(AppState* st) {
    miniMeterReset(st->meterSignal);
    miniMeterReset(st->meterBitrate);
    miniMeterReset(st->meterFrames);
}

// Settings → Meters → Setup…: the per-meter look + palette dialog. Seeds it from the
// live meters, then on OK applies + persists (style, colours, enable) for each.
void onMeters(AppState* st) {
    HWND m[4] = {st->meterSpectrum, st->meterSignal, st->meterBitrate, st->meterFrames};
    const bool en[4] = {st->showSpectrum, st->showSignal, st->showBitrate, st->showFrames};
    MeterConfig cfg[4];
    for (int r = 0; r < 4; ++r) {
        cfg[r].enabled = en[r];
        cfg[r].style = miniMeterStyle(m[r]);
        cfg[r].palette = miniMeterPalette(m[r]);
        cfg[r].tuning = miniMeterTuning(m[r]);
    }
    // The data-flow (buffer) meter's current visible state (persisted as buffer_hidden).
    auto bh = st->db.getSetting(L"buffer_hidden");
    bool dataFlowOn = !(bh && *bh == L"1");
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (!chooseMeters(st->hwnd, hInst, st->dpi, cfg, dataFlowOn)) return;  // Cancel — no change
    bufferMeterSetHidden(st->bufferMeter, !dataFlowOn);
    st->db.setSetting(L"buffer_hidden", dataFlowOn ? L"0" : L"1");

    static const wchar_t* key[4] = {L"spectrum", L"signal", L"bitrate", L"frames"};
    st->showSpectrum = cfg[0].enabled;
    st->showSignal = cfg[1].enabled;
    st->showBitrate = cfg[2].enabled;
    st->showFrames = cfg[3].enabled;
    for (int r = 0; r < 4; ++r) {
        miniMeterSetStyle(m[r], cfg[r].style);
        miniMeterSetPalette(m[r], cfg[r].palette);
        miniMeterSetTuning(m[r], cfg[r].tuning);
        st->db.setSetting(std::wstring(L"meter_") + key[r], cfg[r].enabled ? L"1" : L"0");
        st->db.setSetting(std::wstring(L"meter_") + key[r] + L"_style",
                          meterStyleToString(cfg[r].style));
        st->db.setSetting(std::wstring(L"meter_") + key[r] + L"_colors",
                          meterPaletteToString(cfg[r].palette));
        st->db.setSetting(std::wstring(L"meter_") + key[r] + L"_knobs",
                          meterTuningToString(cfg[r].tuning));
    }
    syncSpectrumTap(st);   // enabling/disabling spectrum starts/stops the capture tap
    layout(st->hwnd, st);  // show/hide meters per the new enables
}

// Relaunch the app to apply a setting that only takes effect at startup (the display language). A
// fresh instance is launched with --restart (it WAITS on the single-instance mutex for us to exit,
// rather than bouncing), then we tear down — which releases the mutex and lets the new one take over.
void restartApp(AppState* st) {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", exe, L"--restart", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) {  // > 32 == launched OK (ShellExecute's success contract)
        DestroyWindow(st->hwnd);  // WM_DESTROY tears down cleanly (reaper threads + libVLC + the mutex)
    } else {
        // Couldn't spawn the new instance — stay open rather than close with no relaunch. The choice
        // is already persisted, so it still applies the next time the user launches RabbitEars.
        diag::warn(L"language restart: relaunch failed (ShellExecute), staying open");
    }
}

// Settings ▸ Language: persist the choice, then prompt (Restart now / Later). We deliberately do NOT
// re-render live — the built-once chrome + the cached Direct2D text formats would need a full rebuild
// + font-remake; restart-to-apply is the pragmatic first pass (live upgrade is backlogged). The
// active language is left UNCHANGED until the restart, so the current session stays fully consistent
// in the old language rather than showing a half-translated mix.
void setLanguageSelection(AppState* st, const wchar_t* pref) {
    if (st->uiLanguage == pref) return;  // already selected — nothing to change or prompt
    st->uiLanguage = pref;
    st->db.setSetting(L"ui_language", pref);

    // A themed TaskDialog with custom buttons — its text is localized to the JUST-CHOSEN language
    // (a preview of what the restart brings), via a temporary active-language switch for the lookups.
    const i18n::Lang shown = resolveLang(st->uiLanguage);
    const std::wstring title = wideFromUtf8(i18n::trU8(i18n::StringId::AppName, shown));
    const std::wstring instr = wideFromUtf8(i18n::trU8(i18n::StringId::LangRestartInstruction, shown));
    const std::wstring body = wideFromUtf8(i18n::trU8(i18n::StringId::LangRestartBody, shown));
    const std::wstring now = wideFromUtf8(i18n::trU8(i18n::StringId::LangRestartNow, shown));
    const std::wstring later = wideFromUtf8(i18n::trU8(i18n::StringId::LangRestartLater, shown));
    const TASKDIALOG_BUTTON btns[] = {{IDYES, now.c_str()}, {IDNO, later.c_str()}};
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = st->hwnd;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = title.c_str();
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = instr.c_str();
    cfg.pszContent = body.c_str();
    cfg.pButtons = btns;
    cfg.cButtons = ARRAYSIZE(btns);
    cfg.nDefaultButton = IDYES;
    int pressed = 0;
    if (SUCCEEDED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)) && pressed == IDYES)
        restartApp(st);  // else "Later" / dismissed — the choice is persisted, applies next launch
}

// Command-bar Settings (gear) menu — grouped submenus mirroring the mac gear menu's curation.
void showSettingsMenu(HWND hwnd, AppState* st, const RECT& anchor) {
    using namespace i18n;
    const UINT chk = MF_CHECKED;

    // Recording-format submenu (nested under Recording).
    HMENU fmt = CreatePopupMenu();
    AppendMenuW(fmt, MF_STRING | (st->recFormat != L"mkv" && st->recFormat != L"mp4" ? chk : 0u),
                ID_FMT_TS, tr(StringId::MenuFormatMpegTs).c_str());
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mkv" ? chk : 0u), ID_FMT_MKV,
                tr(StringId::MenuFormatMatroska).c_str());
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mp4" ? chk : 0u), ID_FMT_MP4,
                tr(StringId::MenuFormatMp4).c_str());

    // Channels submenu: hide-unavailable + categories | import/export favourites.
    HMENU chan = CreatePopupMenu();
    AppendMenuW(chan, MF_STRING | (st->hideDead ? chk : 0u), ID_HIDE_DEAD,
                tr(StringId::MenuHideUnavailable).c_str());
    std::wstring catLabel = tr(StringId::MenuCategories);
    if (st->categoryActive) catLabel += L"  (" + std::to_wstring(st->categories.size()) + L")";
    AppendMenuW(chan, MF_STRING | (st->categoryActive ? chk : 0u), ID_CATEGORIES, catLabel.c_str());
    AppendMenuW(chan, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(chan, MF_STRING, ID_FAV_IMPORT, tr(StringId::MenuImportFavourites).c_str());
    AppendMenuW(chan, MF_STRING, ID_FAV_EXPORT, tr(StringId::MenuExportFavourites).c_str());

    // Recording submenu: scheduled + rules | format | wake-to-record + run-now.
    HMENU rec = CreatePopupMenu();
    AppendMenuW(rec, MF_STRING, ID_SCHEDULES, tr(StringId::MenuScheduledRecordings).c_str());
    AppendMenuW(rec, MF_STRING, ID_RULES, tr(StringId::MenuRecordingRules).c_str());
    AppendMenuW(rec, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(rec, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt), tr(StringId::MenuRecordingFormat).c_str());
    AppendMenuW(rec, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(rec, MF_STRING | (st->wakeToRecord ? chk : 0u), ID_WAKE_RECORD,
                tr(StringId::MenuWakeToRecord).c_str());
    // Greyed with the feature off, or nothing queued: no task is registered, nothing to demand-start.
    AppendMenuW(rec, MF_STRING | ((st->wakeToRecord && st->wakeTaskFor > 0) ? 0u : MF_GRAYED),
                ID_WAKE_RUN_NOW, tr(StringId::MenuRunWakeTaskNow).c_str());

    // View submenu: the three view modes | video-only.
    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Single ? chk : 0u), ID_VIEW_SINGLE,
                tr(StringId::MenuViewSingle).c_str());
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Split ? chk : 0u), ID_VIEW_SPLIT,
                tr(StringId::MenuViewSplit).c_str());
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Pip ? chk : 0u), ID_VIEW_PIP,
                tr(StringId::MenuPictureInPicture).c_str());
    AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(viewMenu, MF_STRING | (st->videoOnly ? chk : 0u), ID_VIDEO_ONLY,
                tr(StringId::MenuVideoOnly).c_str());  // carries the \tCtrl+Shift+V accelerator hint

    // Layout submenu: reset | dock moves | save/apply/delete named layouts.
    HMENU layoutMenu = CreatePopupMenu();
    AppendMenuW(layoutMenu, MF_STRING, ID_LAYOUT_RESET, tr(StringId::MenuLayoutResetDefault).c_str());
    AppendMenuW(layoutMenu, MF_SEPARATOR, 0, nullptr);
    const struct { StringId name; Panel p; } dockPanels[] = {{StringId::MenuMoveSidebar, Panel::Nav},
                                                             {StringId::MenuMoveVideo, Panel::Video},
                                                             {StringId::MenuMoveChannels, Panel::Grid}};
    const StringId dockSides[] = {StringId::MenuDockToLeft, StringId::MenuDockToRight,
                                  StringId::MenuDockToTop, StringId::MenuDockToBottom};
    for (const auto& pn : dockPanels) {
        HMENU sub = CreatePopupMenu();
        for (int s = 0; s < 4; ++s)
            AppendMenuW(sub, MF_STRING, ID_DOCK_BASE + static_cast<int>(pn.p) * 4 + s,
                        tr(dockSides[s]).c_str());
        AppendMenuW(layoutMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), tr(pn.name).c_str());
    }
    AppendMenuW(layoutMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(layoutMenu, MF_STRING, ID_LAYOUT_SAVE, tr(StringId::MenuSaveLayoutAs).c_str());
    const std::vector<std::wstring> savedNames = savedLayoutNames(st);
    if (!savedNames.empty()) {
        HMENU applyMenu = CreatePopupMenu(), delMenu = CreatePopupMenu();
        for (size_t i = 0; i < savedNames.size(); ++i) {  // capped at kMaxSavedLayouts by the getter
            AppendMenuW(applyMenu, MF_STRING, ID_LAYOUT_APPLY_BASE + static_cast<int>(i),
                        savedNames[i].c_str());
            AppendMenuW(delMenu, MF_STRING, ID_LAYOUT_DELETE_BASE + static_cast<int>(i),
                        savedNames[i].c_str());
        }
        AppendMenuW(layoutMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(applyMenu),
                    tr(StringId::MenuApplySavedLayout).c_str());
        AppendMenuW(layoutMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(delMenu),
                    tr(StringId::MenuDeleteSavedLayout).c_str());
    }

    // Language submenu: System default / English / 日本語 / 繁體中文 (restart-to-apply).
    HMENU langMenu = CreatePopupMenu();
    AppendMenuW(langMenu, MF_STRING | (st->uiLanguage == L"system" ? chk : 0u), ID_LANG_SYSTEM,
                tr(StringId::LangSystemDefault).c_str());
    AppendMenuW(langMenu, MF_STRING | (st->uiLanguage == L"en" ? chk : 0u), ID_LANG_EN,
                tr(StringId::LangEnglish).c_str());
    AppendMenuW(langMenu, MF_STRING | (st->uiLanguage == L"ja" ? chk : 0u), ID_LANG_JA,
                tr(StringId::LangJapanese).c_str());
    AppendMenuW(langMenu, MF_STRING | (st->uiLanguage == L"zh-Hant" ? chk : 0u), ID_LANG_ZH_HANT,
                tr(StringId::LangTraditionalChinese).c_str());
    AppendMenuW(langMenu, MF_STRING | (st->uiLanguage == L"zh-HK" ? chk : 0u), ID_LANG_ZH_HK,
                tr(StringId::LangTraditionalChineseHK).c_str());

    // ---- assemble the top-level menu (grouped, mac-style) ----
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN_FILE, tr(StringId::MenuOpenFile).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(chan), tr(StringId::MenuChannels).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EPG_GUIDE, tr(StringId::TvGuideTitle).c_str());
    AppendMenuW(m, MF_STRING, ID_EPG_REFRESH, tr(StringId::MenuRefreshGuide).c_str());
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(rec), tr(StringId::MenuRecording).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), tr(StringId::MenuView).c_str());
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), tr(StringId::MenuLayout).c_str());
    AppendMenuW(m, MF_STRING, ID_METERS_SETUP, tr(StringId::MenuMeters).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(langMenu), tr(StringId::MenuLanguage).c_str());
#ifdef RABBITEARS_THEME_ENGINE
    HMENU themeMenu = CreatePopupMenu();
    const std::string& skinSel = activeSkinSelection();
    AppendMenuW(themeMenu, MF_STRING | (skinSel == "system" ? chk : 0u), ID_THEME_SYSTEM,
                tr(StringId::MenuThemeFollowSystem).c_str());
    AppendMenuW(themeMenu, MF_SEPARATOR, 0, nullptr);
    const auto& skins = builtinSkins();  // one item per registered skin (auto-grows in Phase 4)
    for (size_t i = 0; i < skins.size(); ++i)
        AppendMenuW(themeMenu, MF_STRING | (skinSel == skins[i].id ? chk : 0u),
                    ID_THEME_SKIN_BASE + static_cast<int>(i), wideFromUtf8(skins[i].name).c_str());
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), tr(StringId::MenuTheme).c_str());
#endif
    AppendMenuW(m, MF_STRING | (st->resumeLast ? chk : 0u), ID_RESUME_LAST,
                tr(StringId::MenuResumeLastChannel).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_ABOUT, tr(StringId::MenuAbout).c_str());

    POINT pt{anchor.left, anchor.bottom};
    ClientToScreen(hwnd, &pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);  // frees the submenus too
    if (cmd == ID_LAYOUT_RESET) {
        st->dock = DockLayout::makeDefault();
        applyDockChange(hwnd, st);
        return;
    }
    if (cmd >= ID_DOCK_BASE && cmd < ID_DOCK_BASE + kPanelCount * 4) {
        const int off = cmd - ID_DOCK_BASE;
        dockToEdge(st, static_cast<Panel>(off / 4), static_cast<DockSide>(off % 4));
        applyDockChange(hwnd, st);
        return;
    }
    if (cmd == ID_LAYOUT_SAVE) {
        onLayoutSave(hwnd, st);
        return;
    }
    if (cmd >= ID_LAYOUT_APPLY_BASE && cmd < ID_LAYOUT_APPLY_BASE + kMaxSavedLayouts) {
        const auto names = savedLayoutNames(st);
        const size_t i = static_cast<size_t>(cmd - ID_LAYOUT_APPLY_BASE);
        if (i < names.size()) {
            if (auto s = st->db.getSetting(L"layout_saved_" + names[i]); s && !s->empty()) {
                st->dock = DockLayout::parse(*s);  // malformed input degrades to the default tree
                applyDockChange(hwnd, st);         // applies + re-persists the live dock_layout
                setStatus(st, trf(i18n::StringId::StatusLayoutApplied, { names[i] }));
            }
        }
        return;
    }
    if (cmd >= ID_LAYOUT_DELETE_BASE && cmd < ID_LAYOUT_DELETE_BASE + kMaxSavedLayouts) {
        auto names = savedLayoutNames(st);
        const size_t i = static_cast<size_t>(cmd - ID_LAYOUT_DELETE_BASE);
        if (i < names.size()) {
            st->db.setSetting(L"layout_saved_" + names[i], L"");  // K/V store has no delete; empty = gone
            const std::wstring gone = names[i];
            names.erase(names.begin() + static_cast<ptrdiff_t>(i));
            storeLayoutNames(st, names);
            setStatus(st, trf(i18n::StringId::StatusLayoutDeleted, { gone }));
        }
        return;
    }
#ifdef RABBITEARS_THEME_ENGINE
    if (cmd == ID_THEME_SYSTEM) {
        setSkinSelection(hwnd, st, "system");
        return;
    }
    if (cmd >= ID_THEME_SKIN_BASE &&
        cmd < ID_THEME_SKIN_BASE + static_cast<int>(builtinSkins().size())) {
        setSkinSelection(hwnd, st, builtinSkins()[cmd - ID_THEME_SKIN_BASE].id.c_str());
        return;
    }
#endif
    switch (cmd) {
        case ID_OPEN_FILE:
            onOpenFile(st);
            break;
        case ID_ABOUT:
            showAbout(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                      st->dpi);
            break;
        case ID_FMT_TS:
            st->recFormat = L"ts";
            st->db.setSetting(L"rec_format", L"ts");
            break;
        case ID_FMT_MKV:
            st->recFormat = L"mkv";
            st->db.setSetting(L"rec_format", L"mkv");
            break;
        case ID_FMT_MP4:
            st->recFormat = L"mp4";
            st->db.setSetting(L"rec_format", L"mp4");
            break;
        case ID_RESUME_LAST:
            st->resumeLast = !st->resumeLast;
            st->db.setSetting(L"resume_last", st->resumeLast ? L"1" : L"0");
            break;
        case ID_RULES:
            onManageRules(st);
            break;
        case ID_WAKE_RECORD: {
            st->wakeToRecord = !st->wakeToRecord;
            st->db.setSetting(L"wake_to_record", st->wakeToRecord ? L"1" : L"0");
            syncWakeFromSchedules(st);  // register or tear down the Windows task right away
            if (!st->wakeToRecord) {
                setStatus(st, tr(i18n::StringId::StatusWakeToRecordOff));
                break;
            }
            // Switching it on is exactly when a promise is made. Keep it honest: if the power plan
            // won't arm an RTC wake, say so here instead of at 3am when the recording didn't run.
            const std::wstring warn = wakeVerdictText(queryWakePolicy());
            setStatus(st, warn.empty() ? tr(i18n::StringId::StatusWakeWillWake) : warn);
            break;
        }
        case ID_WAKE_RUN_NOW:
            // The honest end-to-end test on a machine you can't put to sleep (a VM, a remote box):
            // this runs the registered task for real, --scheduled-wake and all.
            setStatus(st, runWakeTaskNow()
                              ? tr(i18n::StringId::StatusWakeTaskStarted)
                              : tr(i18n::StringId::StatusWakeTaskFailed));
            break;
        case ID_FAV_EXPORT:
            onExportFavourites(st);
            break;
        case ID_FAV_IMPORT:
            onImportFavourites(st);
            break;
        case ID_HIDE_DEAD:
            st->hideDead = !st->hideDead;
            st->db.setSetting(L"hide_dead", st->hideDead ? L"1" : L"0");
            loadForFilter(st);
            break;
        case ID_CATEGORIES:
            onCategories(st);
            break;
        case ID_EPG_GUIDE:
            onEpgGuide(st);
            break;
        case ID_SCHEDULES:
            onManageSchedules(st);
            break;
        case ID_EPG_REFRESH:
            onEpgRefresh(st);
            break;
        case ID_METERS_SETUP:
            onMeters(st);
            break;
        case ID_VIDEO_ONLY:
            toggleVideoOnly(st);
            break;
        case ID_VIEW_SINGLE:
            applyViewMode(st, ViewMode::Single);
            break;
        case ID_VIEW_SPLIT:
            applyViewMode(st, ViewMode::Split);
            break;
        case ID_VIEW_PIP:
            applyViewMode(st, ViewMode::Pip);
            break;
        case ID_LANG_SYSTEM:
            setLanguageSelection(st, L"system");
            break;
        case ID_LANG_EN:
            setLanguageSelection(st, L"en");
            break;
        case ID_LANG_JA:
            setLanguageSelection(st, L"ja");
            break;
        case ID_LANG_ZH_HANT:
            setLanguageSelection(st, L"zh-Hant");
            break;
        case ID_LANG_ZH_HK:
            setLanguageSelection(st, L"zh-HK");
            break;
    }
}


}  // namespace mw
}  // namespace rabbitears
