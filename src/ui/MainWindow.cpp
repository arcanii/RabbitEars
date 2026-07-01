// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window (Layer B1b): custom title-bar chrome (reclaimed
// non-client area + owner-draw command bar), a nav sidebar, a video surface with
// transport controls, and the Direct2D channel grid fed from the SQLite store.
#include "ui/MainWindow.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>  // IStream — required by gdiplus.h below
#include <windowsx.h>
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it. (Used for the
// process-wide GDI+ startup the About box's rendering relies on.)
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "core/Http.h"
#include "core/M3uParser.h"
#include "db/Database.h"
#include "platform/Updater.h"
#include "resource.h"
#include "ui/BufferMeter.h"
#include "ui/ChannelGridControl.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/VlcPlayer.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

namespace rabbitears {
namespace {

constexpr wchar_t kMainClass[] = L"RabbitEarsMain";
constexpr wchar_t kVideoClass[] = L"ReVideoSurface";
constexpr UINT WM_APP_VLC = WM_APP + 1;
constexpr UINT WM_APP_PLAYLIST_DONE = WM_APP + 2;

constexpr int ID_ADD_URL = 2001;
constexpr int ID_OPEN_FILE = 2002;
constexpr int ID_ABOUT = 2003;
constexpr int ID_BTN_PLAY = 2010;
constexpr int ID_BTN_STOP = 2011;
constexpr int ID_VOL = 2012;
constexpr int ID_BTN_FULL = 2013;
constexpr int ID_BUFFER = 2014;
constexpr int ID_SEARCH = 2020;
constexpr int ID_GRID = 2021;
constexpr int ID_NAV = 2022;

int dp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

enum class ViewKind { All, Favourites, Group, Playlist };
struct ViewFilter {
    ViewKind     kind = ViewKind::All;
    std::wstring group;
    long long    playlistId = 0;
};

struct CmdBtn {
    int          id;
    const wchar_t* label;
    bool         accent;
};
const CmdBtn kCmdBtns[] = {
    {ID_ADD_URL, L"+  Add Playlist", true},
    {ID_OPEN_FILE, L"Open File", false},
    {ID_ABOUT, L"About", false},
};

struct PlaylistResult {
    bool         ok = false;
    std::wstring error;
    std::wstring name;
    std::wstring source;
    bool         isUrl = true;
    M3uDocument  doc;
};

struct AppState {
    Database   db;
    VlcPlayer  player;
    HWND       hwnd = nullptr;
    HWND       video = nullptr;
    HWND       nav = nullptr;
    HWND       splitter = nullptr;
    HWND       grid = nullptr;
    HWND       search = nullptr;
    HWND       btnPlay = nullptr;
    HWND       btnStop = nullptr;
    HWND       btnFull = nullptr;
    HWND       volBar = nullptr;
    HWND       status = nullptr;
    HWND       bufferMeter = nullptr;
    HFONT      uiFont = nullptr;
    HFONT      titleFont = nullptr;
    UINT       dpi = 96;
    int        sidebarW = 240;  // nav width in px (draggable via the splitter)
    int        cmdHover = -1;  // hovered toolbar button index
    int        capHover = -1;  // hovered caption button (0 min,1 max,2 close)
    bool       fullscreen = false;
    bool       busy = false;
    long long  nowPlayingId = 0;
    std::wstring nowPlayingName;
    ViewFilter filter;
    std::vector<ViewFilter> navFilters;  // indexed by tree item lParam
};

AppState* stateOf(HWND h) { return reinterpret_cast<AppState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }
void setStatus(AppState* st, const std::wstring& s) {
    if (st->status) SetWindowTextW(st->status, s.c_str());
}

int cmdBarH(UINT dpi) { return dp(46, dpi); }
int navWidth(UINT dpi) { return dp(240, dpi); }
int stripH(UINT dpi) { return dp(50, dpi); }
int capW(UINT dpi) { return dp(46, dpi); }

int measureText(HWND hwnd, HFONT font, const std::wstring& s) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return sz.cx;
}

void applyDarkChrome(HWND hwnd) {
    const Theme& th = currentTheme();
    BOOL dark = th.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    COLORREF cap = th.panelElevBg, txt = th.textSecondary, bdr = th.border;
    DwmSetWindowAttribute(hwnd, 35, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, 36, &txt, sizeof(txt));
    DwmSetWindowAttribute(hwnd, 34, &bdr, sizeof(bdr));
    DWORD corner = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
}

// ---- command bar geometry --------------------------------------------------

RECT captionRect(HWND hwnd, AppState* st, int i) {  // i = 0 min, 1 max, 2 close
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = capW(st->dpi), h = cmdBarH(st->dpi);
    const int right = rc.right - (2 - i) * w;
    return RECT{right - w, 0, right, h};
}

struct BtnRect {
    RECT rc;
    int  id;
};
std::vector<BtnRect> cmdButtonRects(HWND hwnd, AppState* st) {
    std::vector<BtnRect> out;
    const int h = cmdBarH(st->dpi);
    const int btnH = dp(30, st->dpi);
    const int y = (h - btnH) / 2;
    int x = dp(14, st->dpi) + measureText(hwnd, st->titleFont, L"RabbitEars") + dp(20, st->dpi);
    for (const CmdBtn& b : kCmdBtns) {
        const int w = measureText(hwnd, st->uiFont, b.label) + dp(24, st->dpi);
        out.push_back({RECT{x, y, x + w, y + btnH}, b.id});
        x += w + dp(8, st->dpi);
    }
    return out;
}

void drawCaptionGlyph(HDC dc, const RECT& r, int which, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = 5;
    if (which == 0) {  // minimize
        MoveToEx(dc, cx - s, cy, nullptr);
        LineTo(dc, cx + s + 1, cy);
    } else if (which == 1) {  // maximize
        Rectangle(dc, cx - s, cy - s, cx + s + 1, cy + s + 1);
    } else {  // close
        MoveToEx(dc, cx - s, cy - s, nullptr);
        LineTo(dc, cx + s + 1, cy + s + 1);
        MoveToEx(dc, cx - s, cy + s, nullptr);
        LineTo(dc, cx + s + 1, cy - s - 1);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void drawCmdBar(HWND hwnd, AppState* st, HDC target) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const Theme& th = currentTheme();
    const int W = rc.right, H = cmdBarH(st->dpi);

    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT strip{0, 0, W, H};
    FillRect(dc, &strip, themeBrush(th.panelElevBg));
    // bottom hairline
    RECT hair{0, H - 1, W, H};
    FillRect(dc, &hair, themeBrush(th.border));

    SetBkMode(dc, TRANSPARENT);
    // title
    HGDIOBJ oldFont = SelectObject(dc, st->titleFont);
    SetTextColor(dc, th.accent);
    RECT tr{dp(14, st->dpi), 0, W, H};
    DrawTextW(dc, L"RabbitEars", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // toolbar buttons
    SelectObject(dc, st->uiFont);
    auto btns = cmdButtonRects(hwnd, st);
    for (size_t i = 0; i < btns.size(); ++i) {
        const RECT& b = btns[i].rc;
        const bool accent = kCmdBtns[i].accent;
        const bool hover = (static_cast<int>(i) == st->cmdHover);
        if (accent || hover) {
            HBRUSH br = themeBrush(accent ? th.accent : th.hoverBg);
            HGDIOBJ ob = SelectObject(dc, br);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, b.left, b.top, b.right, b.bottom, dp(8, st->dpi), dp(8, st->dpi));
            SelectObject(dc, ob);
            SelectObject(dc, op);
        }
        SetTextColor(dc, accent ? th.accentText : th.textSecondary);
        RECT lr = b;
        DrawTextW(dc, kCmdBtns[i].label, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // caption buttons
    for (int i = 0; i < 3; ++i) {
        RECT cb = captionRect(hwnd, st, i);
        if (st->capHover == i) {
            COLORREF hb = (i == 2) ? RGB(196, 43, 28) : th.hoverBg;
            FillRect(dc, &cb, themeBrush(hb));
        }
        COLORREF glyph = (st->capHover == 2 && i == 2) ? RGB(255, 255, 255) : th.textSecondary;
        const bool isMax = IsZoomed(hwnd);
        drawCaptionGlyph(dc, cb, (i == 1 && isMax) ? 1 : i, glyph);
    }

    SelectObject(dc, oldFont);
    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---- layout ----------------------------------------------------------------

void layout(HWND hwnd, AppState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int cmdH = cmdBarH(st->dpi);

    // search box in the command bar (before caption buttons)
    const int sw = dp(220, st->dpi), sh = dp(28, st->dpi);
    const int sx = W - 3 * capW(st->dpi) - sw - dp(12, st->dpi);
    MoveWindow(st->search, sx, (cmdH - sh) / 2, sw, sh, TRUE);

    if (st->fullscreen) {
        for (HWND h : {st->nav, st->splitter, st->grid, st->search, st->btnPlay, st->btnStop,
                       st->volBar, st->btnFull, st->status, st->bufferMeter})
            ShowWindow(h, SW_HIDE);
        MoveWindow(st->video, 0, 0, W, H, TRUE);
        return;
    }
    for (HWND h : {st->nav, st->splitter, st->grid, st->search, st->btnPlay, st->btnStop, st->volBar,
                   st->btnFull, st->status, st->bufferMeter})
        ShowWindow(h, SW_SHOW);

    const int contentTop = cmdH;
    const int contentH = H - cmdH;
    const int splitW = dp(5, st->dpi);
    const int navW = std::clamp(st->sidebarW, dp(160, st->dpi), std::max(dp(160, st->dpi), W - dp(360, st->dpi)));
    MoveWindow(st->nav, 0, contentTop, navW, contentH, TRUE);
    MoveWindow(st->splitter, navW, contentTop, splitW, contentH, TRUE);

    const int cx = navW + splitW, cw = W - navW - splitW;
    const int sHt = stripH(st->dpi);
    int videoH = (contentH - sHt) * 58 / 100;
    if (videoH < dp(120, st->dpi)) videoH = dp(120, st->dpi);
    MoveWindow(st->video, cx, contentTop, cw, videoH, TRUE);

    const int stripY = contentTop + videoH;
    const int pad = dp(10, st->dpi), btnH = dp(30, st->dpi), by = stripY + (sHt - btnH) / 2;
    const int bw = dp(84, st->dpi);
    int x = cx + pad;
    MoveWindow(st->btnPlay, x, by, bw, btnH, TRUE); x += bw + pad;
    MoveWindow(st->btnStop, x, by, bw, btnH, TRUE); x += bw + pad;
    const int volW = dp(140, st->dpi);
    MoveWindow(st->volBar, x, by, volW, btnH, TRUE); x += volW + pad;
    const int fullW = dp(110, st->dpi);
    MoveWindow(st->btnFull, x, by, fullW, btnH, TRUE); x += fullW + pad * 2;
    // Buffer meter pinned right (taller than the buttons so the fluid waves show);
    // status fills the gap.
    const int meterW = dp(230, st->dpi), meterH = dp(40, st->dpi);
    const int meterX = W - pad - meterW, meterY = stripY + (sHt - meterH) / 2;
    MoveWindow(st->bufferMeter, meterX, meterY, meterW, meterH, TRUE);
    MoveWindow(st->status, x, by, std::max(0, meterX - pad - x), btnH, TRUE);

    const int gridY = stripY + sHt;
    MoveWindow(st->grid, cx, gridY, cw, std::max(0, H - gridY), TRUE);
}

// Draggable vertical splitter between the nav sidebar and the content pane.
LRESULT CALLBACK VSplitterProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool dragging = false;
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().windowBg));
            return 1;
        }
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            dragging = true;
            return 0;
        case WM_MOUSEMOVE:
            if (dragging) {
                HWND parent = GetParent(hwnd);
                AppState* st = stateOf(parent);
                if (!st) return 0;
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(parent, &pt);
                RECT prc;
                GetClientRect(parent, &prc);
                const int maxW = std::max(dp(160, st->dpi), static_cast<int>(prc.right) - dp(360, st->dpi));
                st->sidebarW = std::clamp(static_cast<int>(pt.x), dp(160, st->dpi), maxW);
                layout(parent, st);
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                ReleaseCapture();
                if (AppState* st = stateOf(GetParent(hwnd)))
                    st->db.setSetting(L"sidebar_w", std::to_wstring(st->sidebarW));
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- data ------------------------------------------------------------------

void updateCounts(AppState* st) {
    int shown = 0, total = 0;
    channelGridGetCounts(st->grid, &shown, &total);
    setStatus(st, std::to_wstring(shown) + L" channels");
}

void loadForFilter(AppState* st) {
    std::vector<Channel> ch;
    switch (st->filter.kind) {
        case ViewKind::All: ch = st->db.allChannels(); break;
        case ViewKind::Favourites: ch = st->db.favourites(); break;
        case ViewKind::Group: ch = st->db.channelsByGroup(st->filter.group); break;
        case ViewKind::Playlist: ch = st->db.channelsByPlaylist(st->filter.playlistId); break;
    }
    channelGridSetChannels(st->grid, std::move(ch));
    channelGridSetNowPlaying(st->grid, st->nowPlayingId);
    updateCounts(st);
}

HTREEITEM navInsert(HWND nav, HTREEITEM parent, const std::wstring& text, LPARAM param, bool bold) {
    TVINSERTSTRUCTW is{};
    is.hParent = parent;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM;
    is.item.pszText = const_cast<LPWSTR>(text.c_str());
    is.item.lParam = param;
    if (bold) {
        is.item.mask |= TVIF_STATE;
        is.item.state = TVIS_BOLD;
        is.item.stateMask = TVIS_BOLD;
    }
    return TreeView_InsertItem(nav, &is);
}

void refreshNav(AppState* st) {
    st->navFilters.clear();
    TreeView_DeleteAllItems(st->nav);

    st->navFilters.push_back({ViewKind::All});
    navInsert(st->nav, TVI_ROOT, L"All Channels", 0, false);
    st->navFilters.push_back({ViewKind::Favourites});
    navInsert(st->nav, TVI_ROOT, L"★ Favourites", 1, false);

    HTREEITEM groups = navInsert(st->nav, TVI_ROOT, L"Groups", -1, true);
    for (const std::wstring& g : st->db.listGroups()) {
        st->navFilters.push_back({ViewKind::Group, g, 0});
        navInsert(st->nav, groups, g, static_cast<LPARAM>(st->navFilters.size() - 1), false);
    }
    HTREEITEM playlists = navInsert(st->nav, TVI_ROOT, L"Playlists", -1, true);
    for (const Playlist& p : st->db.listPlaylists()) {
        st->navFilters.push_back({ViewKind::Playlist, L"", p.id});
        navInsert(st->nav, playlists, p.name + L" (" + std::to_wstring(p.channelCount) + L")",
                  static_cast<LPARAM>(st->navFilters.size() - 1), false);
    }
    TreeView_Expand(st->nav, playlists, TVE_EXPAND);
}

void playChannel(AppState* st, const Channel& c) {
    if (st->player.isReady()) st->player.play(c.streamUrl, c.userAgent, c.referrer);
    st->nowPlayingId = c.id;
    st->nowPlayingName = c.name;
    channelGridSetNowPlaying(st->grid, c.id);
    st->db.setSetting(L"last_channel_id", std::to_wstring(c.id));
    bufferMeterSetHealth(st->bufferMeter, 15);
    setStatus(st, L"Opening: " + c.name);
}

std::wstring nameFromSource(const std::wstring& src, bool isUrl) {
    size_t slash = src.find_last_of(isUrl ? L"/" : L"\\/");
    std::wstring n = (slash == std::wstring::npos) ? src : src.substr(slash + 1);
    if (n.empty()) n = src;
    return n;
}

void startPlaylistWorker(AppState* st, const std::wstring& source, bool isUrl,
                         const std::wstring& name) {
    st->busy = true;
    setStatus(st, isUrl ? L"Downloading playlist…" : L"Loading playlist…");
    HWND hwnd = st->hwnd;
    std::thread([hwnd, source, isUrl, name]() {
        auto* res = new PlaylistResult();
        res->isUrl = isUrl;
        res->source = source;
        res->name = name;
        if (isUrl) {
            std::string bytes;
            if (httpGet(source, bytes, res->error)) {
                res->doc = parseM3u(bytes);
                res->ok = true;
            }
        } else {
            std::wstring err;
            res->doc = parseM3uFile(source, &err);
            res->error = err;
            res->ok = err.empty();
        }
        PostMessageW(hwnd, WM_APP_PLAYLIST_DONE, 0, reinterpret_cast<LPARAM>(res));
    }).detach();
}

// ---- command handlers ------------------------------------------------------

void onAddUrl(AppState* st) {
    if (st->busy) return;
    std::wstring url = L"https://iptv-org.github.io/iptv/index.m3u";
    if (!promptText(st->hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                    st->dpi, L"Add Playlist", L"Playlist URL (.m3u / .m3u8):", url))
        return;
    if (url.empty()) return;
    startPlaylistWorker(st, url, true, nameFromSource(url, true));
}

void onOpenFile(AppState* st) {
    if (st->busy) return;
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

void onPlaylistDone(AppState* st, PlaylistResult* res) {
    st->busy = false;
    if (res->ok && !res->doc.channels.empty()) {
        const long long now = static_cast<long long>(time(nullptr));
        const long long pid = st->db.addPlaylist(res->name, res->source, res->isUrl, now);
        const int n = st->db.bulkInsertChannels(pid, res->doc.channels, now);
        refreshNav(st);
        st->filter = {ViewKind::Playlist, L"", pid};
        loadForFilter(st);
        setStatus(st, L"Added " + std::to_wstring(n) + L" channels from " + res->name);
    } else {
        std::wstring msg = res->error.empty() ? L"No channels found." : res->error;
        setStatus(st, L"Add playlist failed: " + msg);
    }
    delete res;
}

void toggleFullscreen(AppState* st) {
    st->fullscreen = !st->fullscreen;
    layout(st->hwnd, st);
    if (st->fullscreen) SetFocus(st->video);
    InvalidateRect(st->hwnd, nullptr, TRUE);
}

// ---- window procs ----------------------------------------------------------

void createChildren(HWND hwnd, AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    st->uiFont = CreateFontW(-dp(14, st->dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    st->titleFont = CreateFontW(-dp(16, st->dpi), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    st->video = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10,
                                10, hwnd, nullptr, hInst, nullptr);
    st->nav = CreateWindowExW(0, WC_TREEVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TVS_HASBUTTONS |
                                  TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_TRACKSELECT,
                              0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NAV)),
                              hInst, nullptr);
    st->splitter = CreateWindowExW(0, L"ReVSplitter", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0,
                                   0, 10, 10, hwnd, nullptr, hInst, nullptr);
    registerChannelGridClass(hInst);
    st->grid = createChannelGrid(hwnd, hInst, ID_GRID, st->dpi);
    st->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH)), hInst,
                                 nullptr);
    SendMessageW(st->search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search channels…"));

    st->btnPlay = CreateWindowExW(0, L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_PLAY)), hInst, nullptr);
    st->btnStop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_STOP)), hInst, nullptr);
    st->volBar = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                 0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VOL)),
                                 hInst, nullptr);
    st->btnFull = CreateWindowExW(0, L"BUTTON", L"Fullscreen", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
                                  hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_FULL)), hInst, nullptr);
    st->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10,
                                 10, hwnd, nullptr, hInst, nullptr);
    registerBufferMeterClass(hInst);
    st->bufferMeter = createBufferMeter(hwnd, hInst, ID_BUFFER, st->dpi);
    bufferMeterSetOnHiddenChanged(st->bufferMeter,
                                  [st](bool hidden) { st->db.setSetting(L"buffer_hidden", hidden ? L"1" : L"0"); });

    SendMessageW(st->volBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(st->volBar, TBM_SETPOS, TRUE, st->player.volume());

    for (HWND h : {st->search, st->btnPlay, st->btnStop, st->btnFull, st->status, st->volBar, st->nav}) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->uiFont), TRUE);
        SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    }
    TreeView_SetBkColor(st->nav, currentTheme().panelBg);
    TreeView_SetTextColor(st->nav, currentTheme().textPrimary);

    ChannelGridCallbacks cb;
    cb.onActivate = [st](const Channel& c) { playChannel(st, c); };
    cb.onToggleFavourite = [st](const Channel& c) {
        st->db.setFavourite(c.id, c.favourite);
        if (st->filter.kind == ViewKind::Favourites) loadForFilter(st);
    };
    cb.onSetNumber = [st](const Channel& c) { st->db.setChannelNumber(c.id, c.lcn); };
    channelGridSetCallbacks(st->grid, cb);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    AppState* st = stateOf(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            st->hwnd = hwnd;
            st->dpi = GetDpiForWindow(hwnd);
            st->sidebarW = navWidth(st->dpi);
            applyDarkChrome(hwnd);
            st->player.init();
            createChildren(hwnd, st);
            st->player.attach(st->video);
            st->player.setEventTarget(hwnd, WM_APP_VLC);

            std::wstring err;
            if (st->db.open(Database::defaultDbPath(), &err)) {
                if (auto sw = st->db.getSetting(L"sidebar_w")) st->sidebarW = _wtoi(sw->c_str());
                if (auto bh = st->db.getSetting(L"buffer_hidden"); bh && *bh == L"1")
                    bufferMeterSetHidden(st->bufferMeter, true);
                refreshNav(st);
                st->filter = {ViewKind::All};
                loadForFilter(st);
                int total = 0;
                channelGridGetCounts(st->grid, nullptr, &total);
                if (total == 0) setStatus(st, L"No channels yet — click “+ Add Playlist”.");
            } else {
                setStatus(st, L"Database error: " + err);
            }
            layout(hwnd, st);
            return 0;
        }
        case WM_NCCALCSIZE:
            if (wParam) {
                const UINT dpi = st->dpi ? st->dpi : 96;
                const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                p->rgrc[0].left += fx;
                p->rgrc[0].right -= fx;
                p->rgrc[0].bottom -= fy;
                if (IsZoomed(hwnd)) p->rgrc[0].top += fy;  // keep top (reclaim caption) unless maximized
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (hit == HTCLIENT && !IsZoomed(hwnd)) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                if (pt.y < dp(6, st->dpi)) return HTTOP;
            }
            return hit;
        }
        case WM_SIZE:
            layout(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);  // repaint command bar (max/restore glyph)
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().windowBg));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (!st->fullscreen) drawCmdBar(hwnd, st, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int cmdHover = -1, capHover = -1;
            if (pt.y < cmdBarH(st->dpi)) {
                auto btns = cmdButtonRects(hwnd, st);
                for (size_t i = 0; i < btns.size(); ++i)
                    if (PtInRect(&btns[i].rc, pt)) cmdHover = static_cast<int>(i);
                for (int i = 0; i < 3; ++i) {
                    RECT cb = captionRect(hwnd, st, i);
                    if (PtInRect(&cb, pt)) capHover = i;
                }
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            if (cmdHover != st->cmdHover || capHover != st->capHover) {
                st->cmdHover = cmdHover;
                st->capHover = capHover;
                RECT strip{0, 0, 100000, cmdBarH(st->dpi)};
                InvalidateRect(hwnd, &strip, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (st->cmdHover != -1 || st->capHover != -1) {
                st->cmdHover = st->capHover = -1;
                RECT strip{0, 0, 100000, cmdBarH(st->dpi)};
                InvalidateRect(hwnd, &strip, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.y >= cmdBarH(st->dpi)) break;
            for (int i = 0; i < 3; ++i) {
                RECT cb = captionRect(hwnd, st, i);
                if (PtInRect(&cb, pt)) {
                    if (i == 0) ShowWindow(hwnd, SW_MINIMIZE);
                    else if (i == 1) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                    else PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
            }
            for (const BtnRect& b : cmdButtonRects(hwnd, st))
                if (PtInRect(&b.rc, pt)) {
                    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(b.id, 0), 0);
                    return 0;
                }
            // empty area: drag the window
            if (DragDetect(hwnd, POINT{pt.x, pt.y + 0})) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pt.y < cmdBarH(st->dpi)) {
                bool onButton = false;
                for (const BtnRect& b : cmdButtonRects(hwnd, st)) onButton |= PtInRect(&b.rc, pt) != 0;
                for (int i = 0; i < 3; ++i) { RECT cb = captionRect(hwnd, st, i); onButton |= PtInRect(&cb, pt) != 0; }
                if (!onButton) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTTOP) { SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == ID_SEARCH && HIWORD(wParam) == EN_CHANGE) {
                wchar_t buf[256] = L"";
                GetWindowTextW(st->search, buf, ARRAYSIZE(buf));
                const std::wstring q = buf;
                if (q.empty()) {
                    loadForFilter(st);  // back to the current nav view
                } else {
                    // Global search across the whole library (any name/group/tvg match),
                    // not just the current nav view.
                    channelGridSetChannels(st->grid, st->db.searchChannels(q));
                    channelGridSetNowPlaying(st->grid, st->nowPlayingId);
                    updateCounts(st);
                }
                return 0;
            }
            switch (id) {
                case ID_ADD_URL: onAddUrl(st); return 0;
                case ID_OPEN_FILE: onOpenFile(st); return 0;
                case ID_ABOUT:
                    showAbout(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), st->dpi);
                    return 0;
                case ID_BTN_PLAY: st->player.togglePause(); return 0;
                case ID_BTN_STOP:
                    st->player.stop();
                    st->nowPlayingId = 0;
                    channelGridSetNowPlaying(st->grid, 0);
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    setStatus(st, L"Stopped");
                    return 0;
                case ID_BTN_FULL: toggleFullscreen(st); return 0;
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = reinterpret_cast<NMHDR*>(lParam);
            if (nh->idFrom == ID_NAV && nh->code == TVN_SELCHANGEDW) {
                auto* tv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                const LPARAM idx = tv->itemNew.lParam;
                if (idx >= 0 && idx < static_cast<LPARAM>(st->navFilters.size())) {
                    st->filter = st->navFilters[idx];
                    loadForFilter(st);
                }
            }
            return 0;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == st->volBar)
                st->player.setVolume(static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0)));
            return 0;
        case WM_APP_VLC:
            switch (static_cast<PlayerEvent>(wParam)) {
                case PlayerEvent::Opening:
                    bufferMeterSetHealth(st->bufferMeter, 15);
                    setStatus(st, L"Opening: " + st->nowPlayingName);
                    break;
                case PlayerEvent::Buffering: {
                    const int pct = static_cast<int>(lParam);
                    bufferMeterSetHealth(st->bufferMeter, pct);
                    setStatus(st, L"Buffering " + std::to_wstring(pct) + L"%  —  " + st->nowPlayingName);
                    break;
                }
                case PlayerEvent::Playing:
                    bufferMeterSetHealth(st->bufferMeter, 100);
                    setStatus(st, L"Playing: " + st->nowPlayingName);
                    if (st->nowPlayingId) {
                        st->db.setDeadStatus(st->nowPlayingId, DeadStatus::Alive,
                                             static_cast<long long>(time(nullptr)));
                        channelGridSetDeadStatus(st->grid, st->nowPlayingId, DeadStatus::Alive);
                    }
                    break;
                case PlayerEvent::Paused:
                    setStatus(st, L"Paused: " + st->nowPlayingName);
                    break;
                case PlayerEvent::Stopped: bufferMeterSetHealth(st->bufferMeter, 0); break;
                case PlayerEvent::EndReached:
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    setStatus(st, L"Stream ended");
                    break;
                case PlayerEvent::Error:
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    setStatus(st, L"Unavailable (offline or geo-locked): " + st->nowPlayingName);
                    if (st->nowPlayingId) {
                        st->db.setDeadStatus(st->nowPlayingId, DeadStatus::Dead,
                                             static_cast<long long>(time(nullptr)));
                        channelGridSetDeadStatus(st->grid, st->nowPlayingId, DeadStatus::Dead);
                    }
                    break;
            }
            return 0;
        case WM_APP_PLAYLIST_DONE:
            onPlaylistDone(st, reinterpret_cast<PlaylistResult*>(lParam));
            return 0;
        case WM_SETTINGCHANGE:
            applyDarkChrome(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = dp(760, st->dpi);
            mmi->ptMinTrackSize.y = dp(480, st->dpi);
            return 0;
        }
        case WM_DESTROY:
            st->player.stop();
            if (st->uiFont) DeleteObject(st->uiFont);
            if (st->titleFont) DeleteObject(st->titleFont);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        case WM_LBUTTONDBLCLK:
            SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ID_BTN_FULL, 0), 0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == 'F') {
                AppState* st = stateOf(GetParent(hwnd));
                if (st && st->fullscreen) toggleFullscreen(st);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerClasses(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{};
    vc.cbSize = sizeof(vc);
    vc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    vc.lpfnWndProc = VideoProc;
    vc.hInstance = hInst;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vc.lpszClassName = kVideoClass;
    RegisterClassExW(&vc);

    WNDCLASSEXW sc{};
    sc.cbSize = sizeof(sc);
    sc.style = CS_HREDRAW | CS_VREDRAW;
    sc.lpfnWndProc = VSplitterProc;
    sc.hInstance = hInst;
    sc.hCursor = LoadCursorW(nullptr, IDC_SIZEWE);
    sc.hbrBackground = nullptr;
    sc.lpszClassName = L"ReVSplitter";
    RegisterClassExW(&sc);
}

}  // namespace

int runApp(HINSTANCE hInst, int nCmdShow) {
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    registerClasses(hInst);

    auto* st = new AppState();
    HWND hwnd = CreateWindowExW(0, kMainClass, L"RabbitEars", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, dp(1180, 96), dp(760, 96), nullptr,
                                nullptr, hInst, st);
    if (!hwnd) {
        delete st;
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    initUpdater();  // WinSparkle: start background update checks

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    shutdownUpdater();
    delete st;
    Gdiplus::GdiplusShutdown(gdipToken);
    return static_cast<int>(m.wParam);
}

}  // namespace rabbitears
