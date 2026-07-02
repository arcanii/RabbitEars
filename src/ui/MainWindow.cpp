// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window (Layer B1b): custom title-bar chrome (reclaimed
// non-client area + owner-draw command bar), a nav sidebar, a video surface with
// transport controls, and the Direct2D channel grid fed from the SQLite store.
#include "ui/MainWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>  // SHGetKnownFolderPath (Videos folder for recordings)
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
#include "platform/Log.h"
#include "platform/Updater.h"
#include "resource.h"
#include "version.h"
#include "ui/BufferMeter.h"
#include "ui/ChannelGridControl.h"
#include "ui/Dialogs.h"
#include "ui/MiniMeter.h"
#include "ui/Splash.h"
#include "ui/Theme.h"
#include "ui/VlcPlayer.h"

#include "audio/SpectrumTap.h"

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
constexpr int ID_SETTINGS = 2004;   // command-bar Settings menu
constexpr int ID_FMT_TS = 2005;     // recording format: MPEG-TS
constexpr int ID_FMT_MKV = 2006;    // recording format: Matroska
constexpr int ID_HIDE_DEAD = 2007;  // hide unavailable (dead/geo-blocked) channels
constexpr int ID_CATEGORIES = 2008;  // Categories… include-filter dialog
constexpr int ID_BTN_PLAY = 2010;
constexpr int ID_BTN_STOP = 2011;
constexpr int ID_VOL = 2012;
constexpr int ID_BTN_FULL = 2013;
constexpr int ID_BUFFER = 2014;
constexpr int ID_BUF = 2015;  // buffer-size slider
constexpr int ID_BTN_REC = 2016;
constexpr int ID_SEARCH = 2020;
constexpr int ID_GRID = 2021;
constexpr int ID_NAV = 2022;
constexpr int ID_METER_SPECTRUM = 2030;  // mini-meter control ids
constexpr int ID_METER_SIGNAL = 2031;
constexpr int ID_METER_BITRATE = 2032;
constexpr int ID_METER_FRAMES = 2033;
constexpr int ID_MTR_SPECTRUM = 2040;  // Settings → Meters toggle commands
constexpr int ID_MTR_SIGNAL = 2041;
constexpr int ID_MTR_BITRATE = 2042;
constexpr int ID_MTR_FRAMES = 2043;

int dp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

// Buffer (network-caching) slider bounds in ms, snapped to kBufStepMs. This is the
// receive->show latency the user trades for smoothness on flaky streams.
constexpr int kBufMinMs = 500, kBufMaxMs = 8000, kBufStepMs = 250;

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
    {ID_SETTINGS, L"Settings  ▾", false},
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
    HWND       btnRec = nullptr;
    HWND       btnFull = nullptr;
    HWND       volIcon = nullptr;   // speaker glyph left of the volume slider
    HWND       volBar = nullptr;
    HWND       bufLabel = nullptr;  // "Buffer 1.5 s"
    HWND       bufBar = nullptr;    // network-caching slider (receive->show delay)
    HWND       tip = nullptr;       // shared tooltip (volume slider, buffer slider, meter)
    HWND       status = nullptr;
    HWND       bufferMeter = nullptr;
    HWND       meterSpectrum = nullptr;  // modular LED mini-meters (Settings → Meters)
    HWND       meterSignal = nullptr;
    HWND       meterBitrate = nullptr;
    HWND       meterFrames = nullptr;
    HFONT      uiFont = nullptr;
    HFONT      titleFont = nullptr;
    HFONT      glyphFont = nullptr;  // Segoe MDL2 Assets, for the speaker glyph
    UINT       dpi = 96;
    int        sidebarW = 240;  // nav width in px (draggable via the splitter)
    int        cmdHover = -1;  // hovered toolbar button index
    int        capHover = -1;  // hovered caption button (0 min,1 max,2 close)
    bool       fullscreen = false;
    WINDOWPLACEMENT prevPlacement{};  // saved to restore from fullscreen
    LONG       prevStyle = 0;         // window style saved on entering fullscreen
    bool       busy = false;
    std::wstring recFormat = L"ts";  // recording container: "ts" | "mkv"
    bool       hideDead = false;     // hide unavailable (dead/geo-blocked) channels
    bool       categoryActive = false;    // is the Categories include-filter on?
    std::set<std::wstring> categories;    // included group titles when active
    bool       showSpectrum = true;       // Settings → Meters visibility (persisted)
    bool       showSignal = true;
    bool       showBitrate = false;
    bool       showFrames = false;
    long long  nowPlayingId = 0;
    std::wstring nowPlayingName;
    Channel    nowPlaying;   // last channel passed to playChannel (for re-buffering)
    ViewFilter filter;
    std::vector<ViewFilter> navFilters;  // indexed by tree item lParam
    SpectrumTap spectrumTap;             // read-only WASAPI process-loopback → spectrum meter
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
                       st->btnRec, st->volIcon, st->volBar, st->bufLabel, st->bufBar, st->btnFull,
                       st->status, st->bufferMeter, st->meterSpectrum, st->meterSignal,
                       st->meterBitrate, st->meterFrames})
            ShowWindow(h, SW_HIDE);
        MoveWindow(st->video, 0, 0, W, H, TRUE);
        return;
    }
    for (HWND h : {st->nav, st->splitter, st->grid, st->search, st->btnPlay, st->btnStop, st->btnRec,
                   st->volIcon, st->volBar, st->bufLabel, st->bufBar, st->btnFull, st->status,
                   st->bufferMeter})
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
    MoveWindow(st->btnRec, x, by, bw, btnH, TRUE); x += bw + pad;
    const int iconW = dp(20, st->dpi);
    MoveWindow(st->volIcon, x, by, iconW, btnH, TRUE); x += iconW + dp(2, st->dpi);
    const int volW = dp(126, st->dpi);
    MoveWindow(st->volBar, x, by, volW, btnH, TRUE); x += volW + pad;
    const int fullW = dp(110, st->dpi);
    MoveWindow(st->btnFull, x, by, fullW, btnH, TRUE); x += fullW + pad * 2;
    // Buffer-size control: "Buffer 1.5 s" label + slider (the receive->show delay).
    const int bufLabelW = dp(84, st->dpi), bufBarW = dp(110, st->dpi);
    MoveWindow(st->bufLabel, x, by, bufLabelW, btnH, TRUE); x += bufLabelW + dp(2, st->dpi);
    MoveWindow(st->bufBar, x, by, bufBarW, btnH, TRUE); x += bufBarW + pad * 2;
    // Meter tray, laid out right-to-left: the big fluid buffer meter is pinned to
    // the right edge, then the enabled mini meters stack to its left; the status
    // text fills whatever space is left. Disabled meters are hidden.
    const int meterH = dp(30, st->dpi), meterY = stripY + (sHt - meterH) / 2;
    const int bufMeterW = dp(230, st->dpi);
    int rightX = W - pad;
    MoveWindow(st->bufferMeter, rightX - bufMeterW, meterY, bufMeterW, meterH, TRUE);
    rightX -= bufMeterW + pad;
    struct MtrSlot { HWND h; bool on; int w; };
    const MtrSlot slots[] = {  // rightmost first
        {st->meterFrames, st->showFrames, dp(72, st->dpi)},
        {st->meterBitrate, st->showBitrate, dp(96, st->dpi)},
        {st->meterSignal, st->showSignal, dp(58, st->dpi)},
        {st->meterSpectrum, st->showSpectrum, dp(112, st->dpi)},
    };
    for (const MtrSlot& s : slots) {
        // Only place a meter if it genuinely fits to the left of the buffer meter and
        // right of the transport controls; otherwise hide it (it reappears when the
        // window is widened) so the tray never overlaps the buttons on a narrow strip.
        if (s.on && rightX - s.w >= x + pad) {
            MoveWindow(s.h, rightX - s.w, meterY, s.w, meterH, TRUE);
            ShowWindow(s.h, SW_SHOW);
            rightX -= s.w + dp(6, st->dpi);
        } else {
            ShowWindow(s.h, SW_HIDE);
        }
    }
    MoveWindow(st->status, x, by, std::max(0, rightX - pad - x), btnH, TRUE);

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

// Apply the global view settings (currently: hide unavailable/geo-blocked). Reused
// by the nav views and global search so the toggle is consistent everywhere.
void applyChannelFilters(AppState* st, std::vector<Channel>& ch) {
    if (st->hideDead)
        ch.erase(std::remove_if(ch.begin(), ch.end(),
                                [](const Channel& c) { return c.deadStatus == DeadStatus::Dead; }),
                 ch.end());
    if (st->categoryActive && !st->categories.empty())
        ch.erase(std::remove_if(ch.begin(), ch.end(),
                                [st](const Channel& c) {
                                    // Uncategorized channels (blank group) can't be picked in
                                    // the Categories dialog, so never hide them behind it.
                                    return !c.groupTitle.empty() &&
                                           st->categories.find(c.groupTitle) == st->categories.end();
                                }),
                 ch.end());
}

void loadForFilter(AppState* st) {
    std::vector<Channel> ch;
    switch (st->filter.kind) {
        case ViewKind::All: ch = st->db.allChannels(); break;
        case ViewKind::Favourites: ch = st->db.favourites(); break;
        case ViewKind::Group: ch = st->db.channelsByGroup(st->filter.group); break;
        case ViewKind::Playlist: ch = st->db.channelsByPlaylist(st->filter.playlistId); break;
    }
    applyChannelFilters(st, ch);
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
    diag::info(L"select channel #" + std::to_wstring(c.id) + L" \"" + c.name + L"\" ua=[" +
               c.userAgent + L"] ref=[" + c.referrer + L"]");
    if (st->player.isReady()) st->player.play(c.streamUrl, c.userAgent, c.referrer);
    st->nowPlayingId = c.id;
    st->nowPlayingName = c.name;
    st->nowPlaying = c;
    channelGridSetNowPlaying(st->grid, c.id);
    st->db.setSetting(L"last_channel_id", std::to_wstring(c.id));
    bufferMeterSetHealth(st->bufferMeter, 15);
    setStatus(st, L"Opening: " + c.name);
}

std::wstring bufLabelText(int ms) {
    wchar_t b[24];
    swprintf_s(b, L"Buffer %.1f s", ms / 1000.0);
    return b;
}

// Snap + apply the network buffer size, persist it, sync the slider/label, and
// (optionally) re-buffer the current stream so the change takes effect immediately.
void setBufferMs(AppState* st, int ms, bool replay) {
    ms = std::clamp((ms + kBufStepMs / 2) / kBufStepMs * kBufStepMs, kBufMinMs, kBufMaxMs);
    st->player.setNetworkCaching(ms);
    st->db.setSetting(L"buffer_ms", std::to_wstring(ms));
    if (st->bufBar) SendMessageW(st->bufBar, TBM_SETPOS, TRUE, ms / kBufStepMs);
    if (st->bufLabel) SetWindowTextW(st->bufLabel, bufLabelText(ms).c_str());
    if (replay && st->player.isPlaying() && st->nowPlaying.id != 0) playChannel(st, st->nowPlaying);
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
    diag::info((isUrl ? L"playlist download start: " : L"playlist load start: ") + source);
    HWND hwnd = st->hwnd;
    std::thread([hwnd, source, isUrl, name]() {
        auto* res = new PlaylistResult();
        res->isUrl = isUrl;
        res->source = source;
        res->name = name;
        if (isUrl) {
            std::string bytes;
            // 30 s per-phase timeout so a stalled connection can't hang the worker
            // forever (which would latch `busy` and leave no feedback).
            if (httpGet(source, bytes, res->error, 30000)) {
                diag::info(L"downloaded " + std::to_wstring(bytes.size()) + L" bytes");
                res->doc = parseM3u(bytes);
                res->ok = true;
                diag::info(L"parsed " + std::to_wstring(res->doc.channels.size()) + L" channels");
            } else {
                diag::error(L"download failed from " + source + L": " + res->error);
            }
        } else {
            std::wstring err;
            res->doc = parseM3uFile(source, &err);
            res->error = err;
            res->ok = err.empty();
            if (res->ok)
                diag::info(L"parsed " + std::to_wstring(res->doc.channels.size()) + L" channels");
            else
                diag::error(L"file load failed: " + err);
        }
        PostMessageW(hwnd, WM_APP_PLAYLIST_DONE, 0, reinterpret_cast<LPARAM>(res));
    }).detach();
}

// ---- command handlers ------------------------------------------------------

void onAddUrl(AppState* st) {
    if (st->busy) {
        setStatus(st, L"A playlist is still loading — please wait…");
        diag::warn(L"Add Playlist ignored: a playlist load is already in progress");
        return;
    }
    std::wstring url;  // no bundled/default playlist — users supply their own source
    if (!promptText(st->hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                    st->dpi, L"Add Playlist", L"Playlist URL (.m3u / .m3u8):", url))
        return;
    if (url.empty()) return;
    startPlaylistWorker(st, url, true, nameFromSource(url, true));
}

void onOpenFile(AppState* st) {
    if (st->busy) {
        setStatus(st, L"A playlist is still loading — please wait…");
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
        diag::info(L"playlist added: \"" + res->name + L"\" (" + std::to_wstring(n) +
                   L" channels) from " + res->source);
    } else {
        std::wstring msg = res->error.empty() ? L"No channels found." : res->error;
        setStatus(st, L"Add playlist failed: " + msg);
        diag::error(L"playlist add failed from " + res->source + L": " + msg);
    }
    delete res;
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
        SetFocus(st->video);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, st->prevStyle);
        SetWindowPlacement(hwnd, &st->prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout(hwnd, st);
    InvalidateRect(hwnd, nullptr, TRUE);
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
    if (st->player.isRecording()) {
        const std::wstring file = st->player.recordingFile();
        st->player.stopRecording();
        SetWindowTextW(st->btnRec, L"Record");
        setStatus(st, L"Recording saved: " + file);
        return;
    }
    if (st->nowPlaying.id == 0) {
        setStatus(st, L"Play a channel first, then Record.");
        return;
    }
    const std::wstring ext = (st->recFormat == L"mkv") ? L".mkv" : L".ts";
    const std::string mux = (st->recFormat == L"mkv") ? "mkv" : "ts";
    const std::wstring path = recordingPath(st->nowPlaying.name, ext);
    if (st->player.startRecording(st->nowPlaying.streamUrl, st->nowPlaying.userAgent,
                                  st->nowPlaying.referrer, path, mux)) {
        SetWindowTextW(st->btnRec, L"Stop Rec");
        setStatus(st, L"● Recording " + st->nowPlaying.name + L"  →  " + path);
    }
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
        setStatus(st, L"No categories yet — add a playlist first.");
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

// Command-bar Settings menu: Open File, About, recording format, and view toggles.
void showSettingsMenu(HWND hwnd, AppState* st, const RECT& anchor) {
    HMENU fmt = CreatePopupMenu();
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mkv" ? 0u : MF_CHECKED), ID_FMT_TS,
                L"MPEG-TS  (.ts)");
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mkv" ? MF_CHECKED : 0u), ID_FMT_MKV,
                L"Matroska  (.mkv)");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN_FILE, L"Open File…");
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"About RabbitEars");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt), L"Recording format");
    AppendMenuW(m, MF_STRING | (st->hideDead ? MF_CHECKED : 0u), ID_HIDE_DEAD,
                L"Hide unavailable channels");
    std::wstring catLabel = L"Categories…";
    if (st->categoryActive) catLabel += L"  (" + std::to_wstring(st->categories.size()) + L")";
    AppendMenuW(m, MF_STRING | (st->categoryActive ? MF_CHECKED : 0u), ID_CATEGORIES,
                catLabel.c_str());

    HMENU meters = CreatePopupMenu();
    AppendMenuW(meters, MF_STRING | (st->showSpectrum ? MF_CHECKED : 0u), ID_MTR_SPECTRUM,
                L"Audio spectrum");
    AppendMenuW(meters, MF_STRING | (st->showSignal ? MF_CHECKED : 0u), ID_MTR_SIGNAL,
                L"Signal strength");
    AppendMenuW(meters, MF_STRING | (st->showBitrate ? MF_CHECKED : 0u), ID_MTR_BITRATE, L"Bitrate");
    AppendMenuW(meters, MF_STRING | (st->showFrames ? MF_CHECKED : 0u), ID_MTR_FRAMES, L"Frame rate");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(meters), L"Meters");

    POINT pt{anchor.left, anchor.bottom};
    ClientToScreen(hwnd, &pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);  // frees the submenu too
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
        case ID_HIDE_DEAD:
            st->hideDead = !st->hideDead;
            st->db.setSetting(L"hide_dead", st->hideDead ? L"1" : L"0");
            loadForFilter(st);
            break;
        case ID_CATEGORIES:
            onCategories(st);
            break;
        case ID_MTR_SPECTRUM:
            st->showSpectrum = !st->showSpectrum;
            st->db.setSetting(L"meter_spectrum", st->showSpectrum ? L"1" : L"0");
            syncSpectrumTap(st);
            layout(hwnd, st);
            break;
        case ID_MTR_SIGNAL:
            st->showSignal = !st->showSignal;
            st->db.setSetting(L"meter_signal", st->showSignal ? L"1" : L"0");
            layout(hwnd, st);
            break;
        case ID_MTR_BITRATE:
            st->showBitrate = !st->showBitrate;
            st->db.setSetting(L"meter_bitrate", st->showBitrate ? L"1" : L"0");
            layout(hwnd, st);
            break;
        case ID_MTR_FRAMES:
            st->showFrames = !st->showFrames;
            st->db.setSetting(L"meter_frames", st->showFrames ? L"1" : L"0");
            layout(hwnd, st);
            break;
    }
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
    st->glyphFont = CreateFontW(-dp(13, st->dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

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
    st->btnRec = CreateWindowExW(0, L"BUTTON", L"Record", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_REC)), hInst, nullptr);
    // Speaker glyph (Segoe MDL2 "Volume") so the slider is obviously the volume.
    st->volIcon = CreateWindowExW(0, L"STATIC", L"",
                                  WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY, 0, 0,
                                  10, 10, hwnd, nullptr, hInst, nullptr);
    st->volBar = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                 0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VOL)),
                                 hInst, nullptr);
    st->bufLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10,
                                   10, hwnd, nullptr, hInst, nullptr);
    st->bufBar = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 0,
                                 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BUF)),
                                 hInst, nullptr);
    st->btnFull = CreateWindowExW(0, L"BUTTON", L"Fullscreen", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
                                  hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_FULL)), hInst, nullptr);
    st->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 10,
                                 10, hwnd, nullptr, hInst, nullptr);
    registerBufferMeterClass(hInst);
    st->bufferMeter = createBufferMeter(hwnd, hInst, ID_BUFFER, st->dpi);
    bufferMeterSetOnHiddenChanged(st->bufferMeter,
                                  [st](bool hidden) { st->db.setSetting(L"buffer_hidden", hidden ? L"1" : L"0"); });
    registerMiniMeterClass(hInst);
    st->meterSpectrum = createMiniMeter(hwnd, hInst, ID_METER_SPECTRUM, st->dpi, MeterKind::Spectrum);
    st->meterSignal = createMiniMeter(hwnd, hInst, ID_METER_SIGNAL, st->dpi, MeterKind::Signal);
    st->meterBitrate = createMiniMeter(hwnd, hInst, ID_METER_BITRATE, st->dpi, MeterKind::Bitrate);
    st->meterFrames = createMiniMeter(hwnd, hInst, ID_METER_FRAMES, st->dpi, MeterKind::Frames);

    SendMessageW(st->volBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(st->volBar, TBM_SETPOS, TRUE, st->player.volume());
    SendMessageW(st->bufBar, TBM_SETRANGE, TRUE,
                 MAKELPARAM(kBufMinMs / kBufStepMs, kBufMaxMs / kBufStepMs));
    SendMessageW(st->bufBar, TBM_SETPOS, TRUE, st->player.networkCaching() / kBufStepMs);
    SetWindowTextW(st->bufLabel, bufLabelText(st->player.networkCaching()).c_str());

    // Speaker glyph + a hover tooltip ("Volume: N%") so the slider self-explains.
    SetWindowTextW(st->volIcon, L"");
    SendMessageW(st->volIcon, WM_SETFONT, reinterpret_cast<WPARAM>(st->glyphFont), TRUE);
    st->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr,
                              hInst, nullptr);
    if (st->tip) {
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd;  // owner receives TTN_GETDISPINFO
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        for (HWND tool : {st->volBar, st->volIcon, st->bufBar, st->bufferMeter, st->meterSpectrum,
                          st->meterSignal, st->meterBitrate, st->meterFrames}) {
            ti.uId = reinterpret_cast<UINT_PTR>(tool);
            SendMessageW(st->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        SendMessageW(st->tip, TTM_SETMAXTIPWIDTH, 0, dp(280, st->dpi));  // enable multiline (meter stats)
    }

    for (HWND h : {st->search, st->btnPlay, st->btnStop, st->btnRec, st->btnFull, st->status, st->volBar,
                   st->bufBar, st->bufLabel, st->nav}) {
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

            const std::wstring dbPath = Database::defaultDbPath();
            std::wstring err;
            if (st->db.open(dbPath, &err)) {
                if (auto sw = st->db.getSetting(L"sidebar_w")) st->sidebarW = _wtoi(sw->c_str());
                if (auto bh = st->db.getSetting(L"buffer_hidden"); bh && *bh == L"1")
                    bufferMeterSetHidden(st->bufferMeter, true);
                if (auto bm = st->db.getSetting(L"buffer_ms"); bm && !bm->empty())
                    setBufferMs(st, _wtoi(bm->c_str()), /*replay=*/false);
                if (auto rf = st->db.getSetting(L"rec_format"); rf && (*rf == L"ts" || *rf == L"mkv"))
                    st->recFormat = *rf;
                if (auto hd = st->db.getSetting(L"hide_dead"); hd && *hd == L"1") st->hideDead = true;
                if (auto cf = st->db.getSetting(L"category_filter"); cf && !cf->empty()) {
                    st->categories = splitCategories(*cf);
                    st->categoryActive = !st->categories.empty();
                }
                if (auto v = st->db.getSetting(L"meter_spectrum")) st->showSpectrum = (*v == L"1");
                if (auto v = st->db.getSetting(L"meter_signal")) st->showSignal = (*v == L"1");
                if (auto v = st->db.getSetting(L"meter_bitrate")) st->showBitrate = (*v == L"1");
                if (auto v = st->db.getSetting(L"meter_frames")) st->showFrames = (*v == L"1");
                syncSpectrumTap(st);
                refreshNav(st);
                st->filter = {ViewKind::All};
                loadForFilter(st);
                int total = 0;
                channelGridGetCounts(st->grid, nullptr, &total);
                if (total == 0) setStatus(st, L"No channels yet — click “+ Add Playlist”.");
                diag::info(L"db opened: " + dbPath + L" (" + std::to_wstring(total) + L" channels)");
            } else {
                setStatus(st, L"Database error: " + err);
                diag::error(L"db open FAILED: " + dbPath + L" — " + err);
            }
            layout(hwnd, st);
            return 0;
        }
        case WM_NCCALCSIZE:
            if (wParam) {
                if (st->fullscreen) return 0;  // client fills the whole (borderless) window
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
            if (hit == HTCLIENT && !st->fullscreen && !IsZoomed(hwnd)) {
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
                    if (b.id == ID_SETTINGS)
                        showSettingsMenu(hwnd, st, b.rc);
                    else
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
                    std::vector<Channel> hits = st->db.searchChannels(q);
                    applyChannelFilters(st, hits);
                    channelGridSetChannels(st->grid, std::move(hits));
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
                    resetStatMeters(st);
                    setStatus(st, L"Stopped");
                    return 0;
                case ID_BTN_REC: onToggleRecord(st); return 0;
                case ID_BTN_FULL: toggleFullscreen(st); return 0;
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = reinterpret_cast<NMHDR*>(lParam);
            if (nh->code == TTN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                di->hinst = nullptr;
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->bufferMeter)) {
                    const FlowStats fs = st->player.flowStats();
                    const double bufS = st->player.networkCaching() / 1000.0;
                    static wchar_t buf[256];
                    if (fs.playing) {
                        const double cons = fs.demuxBytesPerSec * 8.0 / 1.0e6;
                        const int loss =
                            fs.corruptedDelta + fs.discontinuityDelta + fs.lostPicturesDelta;
                        // Received / measured buffered-delay come from libVLC's input-byte
                        // counter, which reads 0 for HLS/adaptive streams — only show them
                        // when they're actually reported.
                        wchar_t extra[96] = L"";
                        if (fs.readBytesPerSec > 0.0) {
                            const double rcv = fs.readBytesPerSec * 8.0 / 1.0e6;
                            const double delay = fs.demuxBytesPerSec > 1000.0
                                                     ? fs.bufferedBytes / fs.demuxBytesPerSec
                                                     : 0.0;
                            swprintf_s(extra, L"\r\nReceived: %.2f Mb/s\r\nBuffered: %.1f s", rcv, delay);
                        }
                        swprintf_s(
                            buf, L"Consumption: %.2f Mb/s\r\nBuffer (latency): %.1f s\r\nRecent loss: %d%s",
                            cons, bufS, loss, extra);
                    } else {
                        swprintf_s(buf, L"Not playing\r\nBuffer (latency): %.1f s", bufS);
                    }
                    di->lpszText = buf;
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->bufBar)) {
                    static wchar_t buf[96];
                    swprintf_s(buf, L"Network buffer: %.1f s (receive->show delay)",
                               st->player.networkCaching() / 1000.0);
                    di->lpszText = buf;
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->meterSpectrum)) {
                    di->lpszText = const_cast<LPWSTR>(L"Audio spectrum (this app's sound)");
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->meterSignal)) {
                    di->lpszText = const_cast<LPWSTR>(L"Signal strength (stream health)");
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->meterBitrate)) {
                    di->lpszText = const_cast<LPWSTR>(L"Stream bitrate history");
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->meterFrames)) {
                    di->lpszText = const_cast<LPWSTR>(L"Frame rate (flares red on dropped frames)");
                    return 0;
                }
                const int vol = static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0));
                swprintf_s(di->szText, L"Volume: %d%%", vol);
                di->lpszText = di->szText;
                return 0;
            }
            if (nh->idFrom == ID_NAV && nh->code == NM_RCLICK) {
                // Right-click a playlist node -> "Delete Playlist".
                const DWORD mp = GetMessagePos();
                POINT scr{GET_X_LPARAM(mp), GET_Y_LPARAM(mp)}, cli = scr;
                ScreenToClient(st->nav, &cli);
                TVHITTESTINFO ht{};
                ht.pt = cli;
                HTREEITEM item = TreeView_HitTest(st->nav, &ht);
                if (item) {
                    wchar_t label[256] = L"";
                    TVITEMW ti{};
                    ti.mask = TVIF_PARAM | TVIF_TEXT;
                    ti.hItem = item;
                    ti.pszText = label;
                    ti.cchTextMax = ARRAYSIZE(label);
                    TreeView_GetItem(st->nav, &ti);
                    const LPARAM fi = ti.lParam;
                    if (fi >= 0 && fi < static_cast<LPARAM>(st->navFilters.size()) &&
                        st->navFilters[fi].kind == ViewKind::Playlist) {
                        TreeView_SelectItem(st->nav, item);
                        HMENU menu = CreatePopupMenu();
                        AppendMenuW(menu, MF_STRING, 1, L"Delete Playlist");
                        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x,
                                                       scr.y, 0, hwnd, nullptr);
                        DestroyMenu(menu);
                        if (cmd == 1) {
                            const std::wstring m = L"Delete playlist \"" + std::wstring(label) +
                                                   L"\"?\n\nThis removes its channels from RabbitEars.";
                            if (MessageBoxW(hwnd, m.c_str(), L"Delete Playlist",
                                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                                const long long pid = st->navFilters[fi].playlistId;
                                st->db.deletePlaylist(pid);
                                diag::info(L"deleted playlist id=" + std::to_wstring(pid) + L" (" +
                                           label + L")");
                                st->filter = {ViewKind::All};
                                refreshNav(st);
                                loadForFilter(st);
                                setStatus(st, L"Playlist deleted");
                            }
                        }
                    }
                }
                return TRUE;
            }
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
        case WM_HSCROLL: {
            HWND ctl = reinterpret_cast<HWND>(lParam);
            if (ctl == st->volBar) {
                st->player.setVolume(static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0)));
            } else if (ctl == st->bufBar) {
                const int ms = static_cast<int>(SendMessageW(st->bufBar, TBM_GETPOS, 0, 0)) * kBufStepMs;
                if (LOWORD(wParam) == TB_THUMBTRACK)
                    SetWindowTextW(st->bufLabel, bufLabelText(ms).c_str());  // live while dragging
                else
                    setBufferMs(st, ms, /*replay=*/true);  // apply + re-buffer on release/click/key
            }
            return 0;
        }
        case WM_APP_VLC:
            switch (static_cast<PlayerEvent>(wParam)) {
                case PlayerEvent::Opening:
                    bufferMeterSetHealth(st->bufferMeter, 15);
                    setStatus(st, L"Opening: " + st->nowPlayingName);
                    diag::info(L"event: Opening — " + st->nowPlayingName);
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
                    diag::info(L"event: Playing — " + st->nowPlayingName);
                    if (st->nowPlayingId) {
                        st->db.setDeadStatus(st->nowPlayingId, DeadStatus::Alive,
                                             static_cast<long long>(time(nullptr)));
                        channelGridSetDeadStatus(st->grid, st->nowPlayingId, DeadStatus::Alive);
                    }
                    break;
                case PlayerEvent::Paused:
                    setStatus(st, L"Paused: " + st->nowPlayingName);
                    diag::info(L"event: Paused — " + st->nowPlayingName);
                    break;
                case PlayerEvent::Stats: {
                    // Turn real stream stats into honest meter signals.
                    const FlowStats fs = st->player.flowStats();
                    // Throughput -> 0..1 current speed. Exponential soft-knee so
                    // healthy bitrates saturate near 1 while a stall reads ~0;
                    // K ~= 768 kbps, tuned for typical IPTV (SD flows slower than HD).
                    constexpr double kFlowRef = 96000.0;  // bytes/s
                    const float flow =
                        fs.playing ? static_cast<float>(1.0 - std::exp(-fs.demuxBytesPerSec / kFlowRef))
                                   : 0.0f;
                    // Corruption + discontinuities + dropped frames -> 0..1 turbulence.
                    constexpr float kTroubleRef = 6.0f;
                    const float trouble = std::clamp(
                        (fs.corruptedDelta + fs.discontinuityDelta + 0.5f * fs.lostPicturesDelta) /
                            kTroubleRef,
                        0.0f, 1.0f);
                    bufferMeterSetFlow(st->bufferMeter, flow, trouble);
                    // Meter overlay headline: the consumption rate (real throughput).
                    // (The receive rate / measured delay come from libVLC's input-byte
                    // counter, which stays 0 for HLS/adaptive — so they're not shown
                    // here; the configured buffer latency is on the slider + tooltip.)
                    wchar_t m[24] = L"";
                    if (fs.playing && fs.demuxBytesPerSec > 0.0) {
                        const double bps = fs.demuxBytesPerSec * 8.0;
                        if (bps >= 1.0e6)
                            swprintf_s(m, L"%.1f Mb/s", bps / 1.0e6);
                        else
                            swprintf_s(m, L"%.0f kb/s", bps / 1.0e3);
                    }
                    bufferMeterSetMetrics(st->bufferMeter, m);
                    // Feed the modular stat meters off the same real snapshot.
                    const float strength =
                        fs.playing ? std::clamp((0.35f + 0.65f * flow) * (1.0f - trouble), 0.0f, 1.0f)
                                   : 0.0f;
                    miniMeterSetSignal(st->meterSignal, strength, trouble);
                    miniMeterPushBitrate(st->meterBitrate, fs.demuxBytesPerSec);
                    miniMeterSetFrames(st->meterFrames,
                                       static_cast<int>(std::lround(fs.displayedPerSec)),
                                       fs.lostPicturesDelta);
                    break;
                }
                case PlayerEvent::Stopped:
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    resetStatMeters(st);
                    diag::info(L"event: Stopped");
                    break;
                case PlayerEvent::EndReached:
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    resetStatMeters(st);
                    setStatus(st, L"Stream ended");
                    diag::info(L"event: EndReached — " + st->nowPlayingName);
                    break;
                case PlayerEvent::Error:
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    resetStatMeters(st);
                    setStatus(st, L"Unavailable (offline or geo-locked): " + st->nowPlayingName);
                    diag::error(L"event: PLAYBACK ERROR (offline / geo-locked / codec) — " +
                                st->nowPlayingName);
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
            mmi->ptMinTrackSize.x = dp(1060, st->dpi);
            mmi->ptMinTrackSize.y = dp(480, st->dpi);
            return 0;
        }
        case WM_DESTROY:
            st->spectrumTap.stop();  // join the capture thread before the meter HWNDs die
            st->player.stop();
            if (st->uiFont) DeleteObject(st->uiFont);
            if (st->titleFont) DeleteObject(st->titleFont);
            if (st->glyphFont) DeleteObject(st->glyphFont);
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
    diag::init(RE_VERSION_DISPLAY_W);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    // Branded splash up front: the main window's WM_CREATE blocks for a few seconds
    // (libVLC init + DB load), so show something immediately. It's a layered window,
    // so DWM keeps compositing it while this thread is busy building the main window.
    HWND splash = showSplash(hInst);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES |
                                              ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    registerClasses(hInst);

    auto* st = new AppState();
    HWND hwnd = CreateWindowExW(0, kMainClass, L"RabbitEars", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, dp(1180, 96), dp(760, 96), nullptr,
                                nullptr, hInst, st);
    if (!hwnd) {
        closeSplash(splash);
        delete st;
        Gdiplus::GdiplusShutdown(gdipToken);
        return 1;
    }
    // First run: require accepting the Terms of Use before the app is usable. The
    // main window exists (DB opened in WM_CREATE) but is not shown yet, so the gate
    // appears on its own; declining tears everything down and exits.
    if (st->db.isOpen() && !st->db.getSetting(L"tos_accepted")) {
        closeSplash(splash);  // don't leave the splash behind the modal gate
        splash = nullptr;
        if (!showTerms(hwnd, hInst, st->dpi)) {
            diag::info(L"Terms declined — exiting");
            DestroyWindow(hwnd);
            delete st;
            Gdiplus::GdiplusShutdown(gdipToken);
            diag::shutdown();
            return 0;
        }
        st->db.setSetting(L"tos_accepted", L"1");
        diag::info(L"Terms accepted (first run)");
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    if (splash) closeSplash(splash);  // main window is up (already closed on the first-run path)

    initUpdater();  // WinSparkle: start background update checks

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    shutdownUpdater();
    delete st;
    Gdiplus::GdiplusShutdown(gdipToken);
    diag::shutdown();
    return static_cast<int>(m.wParam);
}

}  // namespace rabbitears
