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
#include <unordered_map>
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

#include "core/Gzip.h"
#include "core/Http.h"
#include "core/M3uParser.h"
#include "core/RecordingScheduler.h"
#include "core/XmltvParser.h"
#include "db/Database.h"
#include "platform/Log.h"
#include "platform/Updater.h"
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


// Shutdown safety-net: once teardown begins, guarantee the process exits within a
// bounded time. libVLC's stop()/release() can block on a stuck stream, leaving
// RabbitEars.exe running headless after the window is gone — and a lingering process
// locks the exe/DLLs so the WinSparkle auto-update installer can't overwrite them (the
// update fails). If the clean teardown finishes first, runApp returns and the process
// exits normally long before this fires; it only wins if something hangs.
void armExitWatchdog(DWORD ms) {
    std::thread([ms] {
        Sleep(ms);
        diag::info(L"Shutdown watchdog fired — forcing process exit so the updater can proceed");
        ExitProcess(0);
    }).detach();
}



int dp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

const CmdBtn kCmdBtns[] = {
    {ID_ADD_URL, L"+  Add Playlist", true},
    {ID_SETTINGS, L"Settings  ▾", false},
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

#ifdef RABBITEARS_THEME_ENGINE
// Recreate the three chrome fonts for the active skin and re-apply them to the controls
// that carry them. A skin may change the font family/size (Phase 4); dark<->light share
// Segoe UI so this is a no-op look today, but the plumbing is skin-correct. (titleFont is
// used at paint in drawCmdBar; no control carries it, so it just needs recreating.)
void remakeUiFonts(AppState* st) {
    // Keep the old handles until every control has been switched to the new font, so no
    // control ever references a freed HFONT.
    HFONT oldUi = st->uiFont, oldTitle = st->titleFont, oldGlyph = st->glyphFont;
    st->uiFont = themeFont(FontRole::Body, st->dpi);
    st->titleFont = themeFont(FontRole::Title, st->dpi);
    st->glyphFont = themeFont(FontRole::Glyph, st->dpi);
    for (HWND h : {st->search, st->status, st->volBar, st->bufBar, st->bufLabel, st->nav})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->uiFont), TRUE);
    SendMessageW(st->volIcon, WM_SETFONT, reinterpret_cast<WPARAM>(st->glyphFont), TRUE);
    for (HWND h : {st->btnPlay, st->btnStop, st->btnRec, st->btnFull})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->glyphFont), TRUE);
    if (oldUi) DeleteObject(oldUi);
    if (oldTitle) DeleteObject(oldTitle);
    if (oldGlyph) DeleteObject(oldGlyph);
}

// Re-apply the active skin to the window chrome + OS-drawn common controls. Owner-drawn
// surfaces (command bar, strip, grid, meters) read currentTheme() each paint, so they
// only need the repaint; the fonts, DWM caption/border, and common-controls dark/light
// theme have to be pushed explicitly. Called at startup and on every Settings→Theme switch.
void applyActiveSkin(HWND hwnd, AppState* st, bool repaint) {
    remakeUiFonts(st);            // skin-driven chrome fonts (parity today; matters for Phase 4)
    clearThemeBrushes();          // free the previous skin's cached HBRUSHes
    applyDarkChrome(hwnd);        // DWM caption/border/text from the (new) currentTheme()
    const Theme& th = currentTheme();
    const wchar_t* sub = th.dark ? L"DarkMode_Explorer" : L"Explorer";
    for (HWND h : {st->search, st->status, st->volBar, st->bufBar, st->bufLabel, st->nav,
                   st->btnPlay, st->btnStop, st->btnRec, st->btnFull})
        SetWindowTheme(h, sub, nullptr);
    TreeView_SetBkColor(st->nav, th.panelBg);
    TreeView_SetTextColor(st->nav, th.textPrimary);
    if (repaint)
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// Settings→Theme: switch skins live. Sets + persists the selection, then rebrushes,
// re-chromes, and repaints the whole UI. `sel` is a skin id or "system" (follow OS).
void setSkinSelection(HWND hwnd, AppState* st, const char* sel) {
    activeSkinSelection() = sel;
    st->db.setSetting(wideFromUtf8(skinSettingKey()), wideFromUtf8(sel));
    applyActiveSkin(hwnd, st, /*repaint=*/true);
}
#endif

// ---- command bar geometry --------------------------------------------------

RECT captionRect(HWND hwnd, AppState* st, int i) {  // i = 0 min, 1 max, 2 close
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = capW(st->dpi), h = cmdBarH(st->dpi);
    const int right = rc.right - (2 - i) * w;
    return RECT{right - w, 0, right, h};
}

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
#ifdef RABBITEARS_THEME_ENGINE
            const COLORREF hb = (i == 2) ? th.dangerHover : th.hoverBg;
#else
            const COLORREF hb = (i == 2) ? RGB(196, 43, 28) : th.hoverBg;
#endif
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

// Position any floating (PIP) panes in SCREEN coords from their last laid-out client rect
// (paneBounds), so the popup tracks the video area as the main window moves/resizes. The PIP is a
// top-level owned window, so it can't ride in layout()'s DeferWindowPos child batch. Called at the
// end of layout() and on WM_MOVE.
void positionFloatingPip(AppState* st) {
    for (int i = 0; i < static_cast<int>(st->panes.size()) && i < 4; ++i) {
        VideoPane* p = st->panes[i].get();
        if (!p->floating || !p->hwnd) continue;
        RECT r = st->paneBounds[i];  // default corner, main-client coords from the last layout()
        const int w = static_cast<int>(r.right - r.left), h = static_cast<int>(r.bottom - r.top);
        // Honour a user-dragged position (pipPos, client coords); else the default corner. The size
        // always comes from the layout — dragging only moves the PIP, it doesn't resize it.
        POINT tl = st->pipMoved ? st->pipPos : POINT{r.left, r.top};
        ClientToScreen(st->hwnd, &tl);
        const bool show = (w > 0 && h > 0) && !st->fullscreen && !st->videoOnly;
        // HWND_TOPMOST so it composites over the main window's libVLC D3D surface (an owned but
        // non-topmost popup did not beat that surface).
        SetWindowPos(p->hwnd, HWND_TOPMOST, tl.x, tl.y, w, h,
                     SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
}

void layout(HWND hwnd, AppState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int cmdH = cmdBarH(st->dpi);

    // Batch every child move into one atomic BeginDeferWindowPos pass so a resize /
    // splitter drag repaints the panes together instead of child-by-child. The
    // child-by-child churn is what left splitter drag artifacts + stale transport-
    // button pixels when a panel moved (this is how ManorLords-SGE avoids the same).
    // Two flag sets:
    //  - kSwp (the three panels): SWP_NOCOPYBITS — nav/grid/video RESIZE during a
    //    drag, so repaint fresh rather than bit-copying a stale interior.
    //  - kSwpMove (strip controls / meters / search): fixed-size children that only
    //    MOVE — let the window manager bit-copy their pixels to the new spot. No
    //    repaint means no erase, which is what made the classic-BUTTON transport
    //    controls flicker during gutter drags. (The parent's WM_PAINT strip fill
    //    covers the pixels they vacate.)
    constexpr UINT kSwp = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
    constexpr UINT kSwpMove = SWP_NOZORDER | SWP_NOACTIVATE;
    HDWP dwp = BeginDeferWindowPos(24);
    auto place = [&](HWND h, int px, int py, int pw, int ph) {
        if (h && dwp)
            dwp = DeferWindowPos(dwp, h, nullptr, px, py, std::max(0, pw), std::max(0, ph), kSwp);
    };
    auto placeMove = [&](HWND h, int px, int py, int pw, int ph) {
        if (h && dwp)
            dwp = DeferWindowPos(dwp, h, nullptr, px, py, std::max(0, pw), std::max(0, ph),
                                 kSwpMove);
    };

    // search box in the command bar (before caption buttons)
    const int sw = dp(220, st->dpi), sh = dp(28, st->dpi);
    const int sx = W - 3 * capW(st->dpi) - sw - dp(12, st->dpi);
    placeMove(st->search, sx, (cmdH - sh) / 2, sw, sh);

    if (st->fullscreen || st->videoOnly) {  // both collapse to just the video filling the client
        for (HWND h : {st->nav, st->grid, st->search, st->btnPlay, st->btnStop, st->btnRec,
                       st->volIcon, st->volBar, st->bufLabel, st->bufBar, st->btnFull, st->status,
                       st->bufferMeter, st->meterSpectrum, st->meterSignal, st->meterBitrate,
                       st->meterFrames, st->gripNav, st->gripVideo, st->gripGrid})
            ShowWindow(h, SW_HIDE);
        // Show the active tile full (or pane 0 if the active pane is the floating PIP); hide the
        // rest, including the floating PIP (a separate top-level window, not in this child batch).
        const int showIdx = st->panes[st->active]->floating ? 0 : st->active;
        for (int i = 0; i < static_cast<int>(st->panes.size()); ++i) {
            HWND h = st->panes[i]->hwnd;
            if (!h) continue;
            if (st->panes[i]->floating) { ShowWindow(h, SW_HIDE); continue; }
            if (!dwp) continue;
            if (i == showIdx)
                dwp = DeferWindowPos(dwp, h, nullptr, 0, 0, W, H, kSwp | SWP_SHOWWINDOW);
            else
                dwp = DeferWindowPos(dwp, h, nullptr, 0, 0, 0, 0,
                                     kSwpMove | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
        if (dwp) EndDeferWindowPos(dwp);
        return;
    }
    for (HWND h : {st->nav, st->grid, st->search, st->btnPlay, st->btnStop, st->btnRec,
                   st->volIcon, st->volBar, st->bufLabel, st->bufBar, st->btnFull, st->status,
                   st->bufferMeter})
        ShowWindow(h, SW_SHOW);

    // The three regions (Nav · Video+transport · Grid) are placed by the user's dock
    // tree; the transport strip rides at the bottom of the Video panel. Divider gutters
    // between regions are painted + dragged by the parent (WM_PAINT / WM_LBUTTONDOWN).
    const int contentTop = cmdH;
    const int sHt = stripH(st->dpi);
    const RECT content{0, contentTop, W, H};
    const int gutterW = dp(5, st->dpi);
    const int minPanel = dp(140, st->dpi);
    RECT rects[kPanelCount];
    st->dock.computeRects(content, gutterW, minPanel, rects, st->gutters);
    const RECT navR = rects[static_cast<int>(Panel::Nav)];
    const RECT vidR = rects[static_cast<int>(Panel::Video)];
    const RECT gridR = rects[static_cast<int>(Panel::Grid)];

    place(st->nav, navR.left, navR.top, static_cast<int>(navR.right - navR.left),
          static_cast<int>(navR.bottom - navR.top));
    place(st->grid, gridR.left, gridR.top, static_cast<int>(gridR.right - gridR.left),
          static_cast<int>(gridR.bottom - gridR.top));

    const int vidW = static_cast<int>(vidR.right - vidR.left);
    const int videoAreaH = std::max(0, static_cast<int>(vidR.bottom - vidR.top) - sHt);
    // Lay the video panes across the video area per the current view mode (Single fills it, Split
    // tiles a grid; the PIP pane is a floating top-level window placed by positionFloatingPip()).
    // Record each pane's rect for the active-border paint + the PIP screen placement; SWP_SHOWWINDOW
    // reveals the child tiles this mode uses (extra tiles are created hidden).
    VideoGridOpts vgo;
    vgo.gap = dp(3, st->dpi);
    vgo.pipW = dp(220, st->dpi);
    vgo.pipH = dp(124, st->dpi);
    vgo.pipMargin = dp(12, st->dpi);
    const auto boxes = computeVideoPanes(st->viewMode, static_cast<int>(st->panes.size()),
                                         static_cast<int>(vidR.left), static_cast<int>(vidR.top),
                                         vidW, videoAreaH, vgo);
    for (int i = 0; i < static_cast<int>(st->panes.size()); ++i) {
        const PaneBox& b = boxes[i];
        if (i < 4) st->paneBounds[i] = RECT{b.x, b.y, b.x + b.w, b.y + b.h};
        HWND h = st->panes[i]->hwnd;
        // Floating (PIP) panes aren't children of this window — positionFloatingPip() places them
        // in screen coords after the deferred child batch.
        if (!h || st->panes[i]->floating || !dwp) continue;
        const UINT f = SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_SHOWWINDOW | SWP_NOZORDER;
        dwp = DeferWindowPos(dwp, h, nullptr, b.x, b.y, std::max(0, b.w), std::max(0, b.h), f);
    }

    // Remember panel rects (for drop hit-testing) + place the drag-to-redock grips in
    // each region's top-right corner, kept above the region's content.
    st->panelRects[static_cast<int>(Panel::Nav)] = navR;
    st->panelRects[static_cast<int>(Panel::Video)] = vidR;
    st->panelRects[static_cast<int>(Panel::Grid)] = gridR;
    const int gw = dp(22, st->dpi), gh = dp(14, st->dpi), gm = dp(3, st->dpi);
    auto placeGrip = [&](HWND g, const RECT& r) {
        if (g && dwp)
            dwp = DeferWindowPos(dwp, g, HWND_TOP, static_cast<int>(r.right) - gw - gm,
                                 static_cast<int>(r.top) + gm, gw, gh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    };
    placeGrip(st->gripNav, navR);
    placeGrip(st->gripVideo, vidR);
    placeGrip(st->gripGrid, gridR);

    const int stripY = vidR.top + videoAreaH;
    const int pad = dp(10, st->dpi), btnH = dp(30, st->dpi), by = stripY + (sHt - btnH) / 2;
    const int bw = dp(34, st->dpi), ig = dp(4, st->dpi);  // square icon buttons, tight cluster
    const int cx = vidR.left;
    int x = cx + pad;
    placeMove(st->btnPlay, x, by, bw, btnH); x += bw + ig;
    placeMove(st->btnStop, x, by, bw, btnH); x += bw + ig;
    placeMove(st->btnRec, x, by, bw, btnH); x += bw + pad;
    const int iconW = dp(20, st->dpi);
    placeMove(st->volIcon, x, by, iconW, btnH); x += iconW + dp(2, st->dpi);
    const int volW = dp(126, st->dpi);
    placeMove(st->volBar, x, by, volW, btnH); x += volW + pad;
    const int fullW = dp(34, st->dpi);  // fullscreen icon
    placeMove(st->btnFull, x, by, fullW, btnH); x += fullW + pad * 2;
    // Buffer-size control: "Buffer 1.5 s" label + slider (the receive->show delay).
    const int bufLabelW = dp(84, st->dpi), bufBarW = dp(110, st->dpi);
    placeMove(st->bufLabel, x, by, bufLabelW, btnH); x += bufLabelW + dp(2, st->dpi);
    placeMove(st->bufBar, x, by, bufBarW, btnH); x += bufBarW + pad * 2;
    // Meter tray, laid out right-to-left within the Video panel; disabled/too-narrow
    // meters are hidden (also via the deferred pass, so show/move stay atomic).
    const int meterH = dp(30, st->dpi), meterY = stripY + (sHt - meterH) / 2;
    const int bufMeterW = dp(115, st->dpi);  // the fluid tank: half its old width, to match the tray
    int rightX = vidR.right - pad;
    placeMove(st->bufferMeter, rightX - bufMeterW, meterY, bufMeterW, meterH);
    rightX -= bufMeterW + pad;
    struct MtrSlot { HWND h; bool on; int w; };
    const MtrSlot slots[] = {  // rightmost first
        {st->meterFrames, st->showFrames, dp(72, st->dpi)},
        {st->meterBitrate, st->showBitrate, dp(96, st->dpi)},
        {st->meterSignal, st->showSignal, dp(58, st->dpi)},
        {st->meterSpectrum, st->showSpectrum, dp(112, st->dpi)},
    };
    for (const MtrSlot& s : slots) {
        if (s.on && rightX - s.w >= x + pad) {
            if (s.h && dwp)
                dwp = DeferWindowPos(dwp, s.h, nullptr, rightX - s.w, meterY, s.w, meterH,
                                     kSwpMove | SWP_SHOWWINDOW);
            rightX -= s.w + dp(6, st->dpi);
        } else if (s.h && dwp) {
            dwp = DeferWindowPos(dwp, s.h, nullptr, 0, 0, 0, 0,
                                 kSwpMove | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
    }
    placeMove(st->status, x, by, std::max(0, rightX - pad - x), btnH);

    if (dwp) EndDeferWindowPos(dwp);
    positionFloatingPip(st);  // place the PIP popup (a top-level window) in screen coords
    InvalidateRect(hwnd, nullptr, FALSE);  // repaint the parent-drawn divider gutters
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
        case ViewKind::Country: ch = st->db.channelsByCountry(st->filter.country); break;
        case ViewKind::Playlist: ch = st->db.channelsByPlaylist(st->filter.playlistId); break;
        case ViewKind::Guide: break;  // action node (opens the TV Guide window); loads no grid channels
    }
    applyChannelFilters(st, ch);
    channelGridSetChannels(st->grid, std::move(ch));
    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
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

// Friendly name for an ISO-3166 alpha-2 country code (from the tvg-id suffix); the
// uppercased code itself for anything not in the (common-IPTV-countries) table.
std::wstring countryLabel(const std::wstring& code) {
    struct CC { const wchar_t* code; const wchar_t* name; };
    static const CC kNames[] = {
        {L"us", L"United States"}, {L"uk", L"United Kingdom"}, {L"gb", L"United Kingdom"},
        {L"ca", L"Canada"}, {L"au", L"Australia"}, {L"nz", L"New Zealand"}, {L"ie", L"Ireland"},
        {L"de", L"Germany"}, {L"fr", L"France"}, {L"es", L"Spain"}, {L"it", L"Italy"},
        {L"pt", L"Portugal"}, {L"nl", L"Netherlands"}, {L"be", L"Belgium"}, {L"ch", L"Switzerland"},
        {L"at", L"Austria"}, {L"se", L"Sweden"}, {L"no", L"Norway"}, {L"dk", L"Denmark"},
        {L"fi", L"Finland"}, {L"pl", L"Poland"}, {L"cz", L"Czechia"}, {L"sk", L"Slovakia"},
        {L"hu", L"Hungary"}, {L"ro", L"Romania"}, {L"bg", L"Bulgaria"}, {L"gr", L"Greece"},
        {L"tr", L"Turkey"}, {L"ru", L"Russia"}, {L"ua", L"Ukraine"}, {L"rs", L"Serbia"},
        {L"hr", L"Croatia"}, {L"si", L"Slovenia"}, {L"al", L"Albania"}, {L"br", L"Brazil"},
        {L"mx", L"Mexico"}, {L"ar", L"Argentina"}, {L"cl", L"Chile"}, {L"co", L"Colombia"},
        {L"pe", L"Peru"}, {L"ve", L"Venezuela"}, {L"in", L"India"}, {L"pk", L"Pakistan"},
        {L"bd", L"Bangladesh"}, {L"cn", L"China"}, {L"jp", L"Japan"}, {L"kr", L"South Korea"},
        {L"id", L"Indonesia"}, {L"my", L"Malaysia"}, {L"sg", L"Singapore"}, {L"th", L"Thailand"},
        {L"vn", L"Vietnam"}, {L"ph", L"Philippines"}, {L"sa", L"Saudi Arabia"}, {L"ae", L"UAE"},
        {L"qa", L"Qatar"}, {L"il", L"Israel"}, {L"eg", L"Egypt"}, {L"ma", L"Morocco"},
        {L"dz", L"Algeria"}, {L"za", L"South Africa"}, {L"ng", L"Nigeria"}, {L"ke", L"Kenya"},
    };
    for (const CC& e : kNames)
        if (code == e.code) return e.name;
    std::wstring up = code;
    for (wchar_t& c : up)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
    return up;
}

void refreshNav(AppState* st) {
    st->navFilters.clear();
    TreeView_DeleteAllItems(st->nav);

    st->navFilters.push_back({ViewKind::All});
    navInsert(st->nav, TVI_ROOT, L"All Channels", 0, false);
    st->navFilters.push_back({ViewKind::Favourites});
    navInsert(st->nav, TVI_ROOT, L"★ Favourites", 1, false);
    st->navFilters.push_back({ViewKind::Guide});
    navInsert(st->nav, TVI_ROOT, L"📺 TV Guide", 2, false);  // selecting it opens the guide window

    HTREEITEM groups = navInsert(st->nav, TVI_ROOT, L"Groups", -1, true);
    for (const std::wstring& g : st->db.listGroups()) {
        st->navFilters.push_back({ViewKind::Group, g, 0});
        navInsert(st->nav, groups, g, static_cast<LPARAM>(st->navFilters.size() - 1), false);
    }
    HTREEITEM countries = navInsert(st->nav, TVI_ROOT, L"Countries", -1, true);
    {
        std::vector<std::pair<std::wstring, std::wstring>> cs;  // (display name, code)
        for (const std::wstring& cc : st->db.listCountries()) cs.emplace_back(countryLabel(cc), cc);
        std::sort(cs.begin(), cs.end());  // alphabetical by name
        for (const auto& [label, cc] : cs) {
            st->navFilters.push_back({ViewKind::Country, L"", 0, cc});
            navInsert(st->nav, countries, label, static_cast<LPARAM>(st->navFilters.size() - 1), false);
        }
    }
    HTREEITEM playlists = navInsert(st->nav, TVI_ROOT, L"Playlists", -1, true);
    for (const Playlist& p : st->db.listPlaylists()) {
        st->navFilters.push_back({ViewKind::Playlist, L"", p.id});
        navInsert(st->nav, playlists, p.name + L" (" + std::to_wstring(p.channelCount) + L")",
                  static_cast<LPARAM>(st->navFilters.size() - 1), false);
    }
    TreeView_Expand(st->nav, playlists, TVE_EXPAND);
}

void resetStatMeters(AppState* st);  // defined below — clear the stat meters on switch

// Play `c` into pane `idx`. When idx is the active pane it also drives the shared chrome (grid
// now-playing highlight, meters, status, last-channel); a background pane (e.g. the PIP) just loads
// and plays — muted, since only the active pane is audible (click it to hear it).
void playChannelInPane(AppState* st, const Channel& c, int idx) {
    if (idx < 0 || idx >= static_cast<int>(st->panes.size())) return;
    VideoPane& p = *st->panes[idx];
    diag::info(L"play pane " + std::to_wstring(idx) + L" #" + std::to_wstring(c.id) + L" \"" + c.name +
               L"\" ua=[" + c.userAgent + L"] ref=[" + c.referrer + L"]");
    if (p.player.isReady()) p.player.play(c.streamUrl, c.userAgent, c.referrer);
    p.nowPlayingId = c.id;
    p.nowPlayingName = c.name;
    p.nowPlaying = c;
    if (idx == st->active) {
        channelGridSetNowPlaying(st->grid, c.id);
        st->db.setSetting(L"last_channel_id", std::to_wstring(c.id));
        bufferMeterSetHealth(st->bufferMeter, 15);
        resetStatMeters(st);  // clear signal/bitrate/frames so switching to a dead/stalled stream
                              // can't leave the previous channel's readings frozen on the meters
        setStatus(st, L"Opening: " + c.name);
    } else {
        setStatus(st, L"PIP: " + c.name);
    }
}

void playChannel(AppState* st, const Channel& c) { playChannelInPane(st, c, st->active); }

std::wstring bufLabelText(int ms) {
    wchar_t b[24];
    swprintf_s(b, L"Buffer %.1f s", ms / 1000.0);
    return b;
}

// Snap + apply the network buffer size, persist it, sync the slider/label, and
// (optionally) re-buffer the current stream so the change takes effect immediately.
void setBufferMs(AppState* st, int ms, bool replay) {
    ms = std::clamp((ms + kBufStepMs / 2) / kBufStepMs * kBufStepMs, kBufMinMs, kBufMaxMs);
    st->ap().player.setNetworkCaching(ms);
    st->db.setSetting(L"buffer_ms", std::to_wstring(ms));
    if (st->bufBar) SendMessageW(st->bufBar, TBM_SETPOS, TRUE, ms / kBufStepMs);
    if (st->bufLabel) SetWindowTextW(st->bufLabel, bufLabelText(ms).c_str());
    if (replay && st->ap().player.isPlaying() && st->ap().nowPlaying.id != 0) playChannel(st, st->ap().nowPlaying);
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
        res->parsed = static_cast<int>(res->doc.channels.size());
        std::set<std::wstring> grp;
        for (const auto& c : res->doc.channels)
            if (!c.groupTitle.empty()) grp.insert(c.groupTitle);
        res->groups = static_cast<int>(grp.size());
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
    std::wstring summary, details;
    const std::wstring src = L"Source:  " + res->source + L"\r\n\r\n";
    if (res->ok && !res->doc.channels.empty()) {
        const long long now = static_cast<long long>(time(nullptr));
        const long long pid = st->db.addPlaylist(res->name, res->source, res->isUrl, now, res->doc.epgUrl);
        if (pid == 0) {
            setStatus(st, L"Add playlist failed: could not save to the database");
            diag::error(L"playlist add failed: addPlaylist returned 0 for " + res->source);
            summary = L"Could not import the playlist";
            details = src + L"Problem:  the playlist could not be saved to the database.\r\n";
        } else {
            const int n = st->db.bulkInsertChannels(pid, res->doc.channels, now);
            res->imported = n;
            refreshNav(st);
            st->filter = {ViewKind::Playlist, L"", pid};
            loadForFilter(st);
            setStatus(st, L"Added " + std::to_wstring(n) + L" channels from " + res->name);
            diag::info(L"playlist added: \"" + res->name + L"\" (" + std::to_wstring(n) +
                       L" channels) from " + res->source);
            const int skipped = res->parsed - res->imported;
            summary = L"Imported " + std::to_wstring(res->imported) + L" channels from " + res->name;
            details = src + L"Channels parsed:  " + std::to_wstring(res->parsed) + L"\r\n" +
                      L"Channels imported:  " + std::to_wstring(res->imported) + L"\r\n";
            if (skipped > 0)
                details += L"Skipped (blank or duplicate URLs):  " + std::to_wstring(skipped) + L"\r\n";
            details += L"Groups:  " + std::to_wstring(res->groups) + L"\r\n";
        }
    } else {
        std::wstring msg = res->error;
        if (msg.empty())
            msg = res->ok ? L"The playlist contained no channels." : L"No channels found.";
        setStatus(st, L"Add playlist failed: " + msg);
        diag::error(L"playlist add failed from " + res->source + L": " + msg);
        summary = L"Could not import the playlist";
        details = src + L"Problem:  " + msg + L"\r\n";
    }
    showInfoDialog(st->hwnd,
                   reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                   st->dpi, L"Import results", summary, details);
    delete res;
}

// Fetch + store the XMLTV guide for every enabled playlist that carries an EPG URL.
// Mirrors startPlaylistWorker: the download + gunzip + parse run on a detached worker
// (busy-guarded), then WM_APP_EPG_DONE stores the parsed programmes on the UI thread.
void onEpgRefresh(AppState* st) {
    if (st->busy) {
        setStatus(st, L"Busy — please wait…");
        return;
    }
    std::vector<EpgTarget> targets;
    for (const auto& pl : st->db.listPlaylists())
        if (pl.enabled && !pl.epgUrl.empty()) targets.push_back({pl.id, pl.name, pl.epgUrl});

    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (targets.empty()) {
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Refresh Guide", L"No guide source found",
                       L"None of your enabled playlists carry an XMLTV guide URL (x-tvg-url in the "
                       L"#EXTM3U header).\r\n\r\nAdd a playlist that includes one, then try again.");
        return;
    }
    st->busy = true;
    setStatus(st, L"Downloading guide…");
    st->loadingDlg =
        showLoadingDialog(st->hwnd, hInst, st->dpi, L"TV Guide", L"Contacting guide source…");
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
                n > 1 ? L" (" + std::to_wstring(i + 1) + L" of " + std::to_wstring(n) + L")" : L"";
            std::string bytes;
            std::wstring err;
            post(L"Downloading " + t.name + L"…" + tag);
            if (!httpGet(t.url, bytes, err, 60000)) {  // guides are large; allow 60 s
                f.error = err.empty() ? L"download failed" : err;
            } else {
                post(L"Parsing " + t.name + L" (" + std::to_wstring(bytes.size() / 1024) + L" KB)…" +
                     tag);
                const std::string xml = gunzipIfNeeded(bytes);
                if (xml.empty())
                    f.error = L"empty or invalid after decompression";
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
        detail += f.name + L":  " + std::to_wstring(stored) + L" programmes\r\n";
        diag::info(L"EPG stored " + std::to_wstring(stored) + L" programmes for \"" + f.name + L"\"");
    }
    const std::wstring summary =
        okCount > 0 ? L"Stored " + std::to_wstring(totalProg) + L" programmes across " +
                          std::to_wstring(chans.size()) + L" channels"
                    : L"Could not refresh the guide";
    setStatus(st, summary);
    showInfoDialog(st->hwnd,
                   reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                   st->dpi, L"Refresh Guide", summary, detail);
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
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    if (rows.empty()) {
        showInfoDialog(st->hwnd, hInst, st->dpi, L"TV Guide", L"No guide to show",
                       L"No stored programmes match a channel in your playlists.\r\n\r\n"
                       L"Either run Settings ▸ Refresh Guide… first, or the guide's channel IDs don't "
                       L"match your playlist — point it at a guide whose tvg-ids line up (right-click a "
                       L"playlist ▸ Set Guide URL…).");
        return;
    }
    std::sort(rows.begin(), rows.end(),
              [](const GuideRow& a, const GuideRow& b) { return a.channelName < b.channelName; });
    GuideCallbacks cb;
    cb.onSchedule = [st](const std::wstring& channelId, const std::wstring& channelName,
                         const std::wstring& title, long long startUtc, long long stopUtc) {
        scheduleFromGuide(st, channelId, channelName, title, startUtc, stopUtc);
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
            showInfoDialog(st->hwnd, hi, st->dpi, L"TV Guide", L"No matching channel",
                           L"Couldn't find a channel for \"" + channelName +
                               L"\" in your playlist.\r\n\r\nThe guide matches programmes to channels "
                               L"by tvg-id — this programme's channel ID has no match. Point the "
                               L"playlist at a guide whose IDs match it (right-click the playlist "
                               L"▸ Set Guide URL…).");
        }
    };
    showEpgGuide(st->hwnd, hInst, st->dpi, std::move(rows), now, cb);
}

// Prompt for a playlist's XMLTV guide URL (seeded with its current one), save the override,
// and offer to fetch it now. Shared by the playlist context menu and the TV Guide node menu.
void promptSetGuideUrl(HWND hwnd, AppState* st, long long pid) {
    std::wstring url;  // seed with the current URL (M3U x-tvg-url or a prior override)
    for (const auto& pl : st->db.listPlaylists())
        if (pl.id == pid) { url = pl.epgUrl; break; }
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    if (!promptText(hwnd, hInst, st->dpi, L"Set Guide URL",
                    L"XMLTV guide URL (.xml or .xml.gz; blank to clear):", url))
        return;
    st->db.setPlaylistEpgUrl(pid, url);
    diag::info(L"set epg_url for playlist id=" + std::to_wstring(pid) +
               (url.empty() ? L" (cleared)" : L" to \"" + url + L"\""));
    if (url.empty()) {
        setStatus(st, L"Guide URL cleared");
    } else if (MessageBoxW(hwnd, L"Guide URL saved.\n\nDownload the guide now?", L"Set Guide URL",
                           MB_YESNO | MB_ICONQUESTION) == IDYES) {
        onEpgRefresh(st);  // fetches every enabled playlist that has a URL
    } else {
        setStatus(st, L"Guide URL saved — run Settings ▸ Refresh Guide to fetch it");
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
    if (st->fullscreen) return;
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
VideoPane* addPane(HWND hwnd, AppState* st, int index, bool floating = false) {
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
    raw->player.attach(raw->hwnd);
    raw->player.setEventTarget(hwnd, WM_APP_VLC);
    raw->player.setTag(index);
    if (index != st->active) raw->player.setVolume(0);  // only the active pane is audible
    return raw;
}

// Make pane `idx` the active one: route audio (others muted), the grid now-playing
// highlight, the play/pause glyph, the meters, and the active-pane border to it. Safe if
// out of range (no-op).
void setActivePane(AppState* st, int idx) {
    if (idx < 0 || idx >= static_cast<int>(st->panes.size())) return;
    st->active = idx;
    const int vol = static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0));
    for (int i = 0; i < static_cast<int>(st->panes.size()); ++i)
        st->panes[i]->player.setVolume(i == idx ? vol : 0);  // active audible, others muted
    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
    SetWindowTextW(st->btnPlay, st->ap().player.isPlaying() ? kGlyphPause : kGlyphPlay);
    resetStatMeters(st);  // the previous pane's readings don't apply to the newly-active stream
    setStatus(st, st->ap().nowPlayingName.empty()
                      ? L"Active pane " + std::to_wstring(idx + 1)
                      : L"Active: " + st->ap().nowPlayingName);
    InvalidateRect(st->hwnd, nullptr, FALSE);  // repaint the active-pane border + meters
}

// Switch the view mode. Every pane except pane 0 is torn down and recreated for the target mode,
// because their window TYPE differs — Split tiles are child windows, the PIP pane is a floating
// top-level popup. Pane 0 (the primary child surface) always persists and keeps playing.
void applyViewMode(AppState* st, ViewMode mode) {
    while (static_cast<int>(st->panes.size()) > 1) {
        auto& p = st->panes.back();
        p->player.shutdown();  // join its worker + reaper threads before the window dies
        if (p->hwnd) DestroyWindow(p->hwnd);
        st->panes.pop_back();
    }
    st->active = 0;
    st->viewMode = mode;
    st->pipMoved = false;  // a fresh PIP starts in the default corner (until the user drags it)
    if (mode == ViewMode::Split)
        for (int i = 1; i < 4; ++i) addPane(st->hwnd, st, i);        // three more tiles -> 2x2
    else if (mode == ViewMode::Pip)
        addPane(st->hwnd, st, 1, /*floating=*/true);                 // the floating PIP popup
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
    if (st->activeScheduleId != 0) {  // the recorder is owned by a scheduled recording
        setStatus(st, L"A scheduled recording is in progress — manage it in Scheduled Recordings.");
        return;
    }
    if (st->ap().player.isRecording()) {
        const std::wstring file = st->ap().player.recordingFile();
        st->ap().player.stopRecording();
        SetWindowTextW(st->btnRec, kGlyphRecord);
        setStatus(st, L"Recording saved: " + file);
        return;
    }
    if (st->ap().nowPlaying.id == 0) {
        setStatus(st, L"Play a channel first, then Record.");
        return;
    }
    const std::wstring ext = (st->recFormat == L"mkv") ? L".mkv" : L".ts";
    const std::string mux = (st->recFormat == L"mkv") ? "mkv" : "ts";
    const std::wstring path = recordingPath(st->ap().nowPlaying.name, ext);
    if (st->ap().player.startRecording(st->ap().nowPlaying.streamUrl, st->ap().nowPlaying.userAgent,
                                  st->ap().nowPlaying.referrer, path, mux)) {
        SetWindowTextW(st->btnRec, kGlyphStop);
        setStatus(st, L"● Recording " + st->ap().nowPlaying.name + L"  →  " + path);
    }
}

// The recording scheduler tick (~30s): decide via the pure planScheduler() core, then
// apply — driving the single shared recorder and writing status back to the DB. Runs
// only while the app is open (a schedule whose window passed while closed is marked
// Missed on the next tick). Guards against the manual Record button via activeScheduleId.
void onSchedulerTick(AppState* st) {
    auto schedules = st->db.listSchedules();
    if (schedules.empty()) return;
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
    // "Manual" recording = the recorder is busy but no schedule owns it.
    const bool manualRecording = st->ap().player.isRecording() && st->activeScheduleId == 0;
    const SchedulerPlan plan = planScheduler(schedules, now, manualRecording);

    for (long long id : plan.stop) {
        st->ap().player.stopRecording();
        st->db.updateScheduleStatus(id, ScheduleStatus::Done);
        if (st->activeScheduleId == id) st->activeScheduleId = 0;
        diag::info(L"scheduled recording finished (id " + std::to_wstring(id) + L")");
        setStatus(st, L"Scheduled recording saved.");
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
        const std::wstring ext = (s->mux == L"mkv") ? L".mkv" : L".ts";
        const std::string mux = (s->mux == L"mkv") ? "mkv" : "ts";
        const std::wstring path = recordingPath(s->channelName, ext);
        if (st->ap().player.startRecording(s->streamUrl, s->userAgent, s->referrer, path, mux)) {
            st->activeScheduleId = id;
            st->db.updateScheduleStatus(id, ScheduleStatus::Recording, path);
            diag::info(L"scheduled recording started: " + s->channelName + L" -> " + path);
            setStatus(st, L"● Recording (scheduled) " + s->channelName);
        } else {
            st->db.updateScheduleStatus(id, ScheduleStatus::Failed);
            diag::error(L"scheduled recording failed to start (id " + std::to_wstring(id) + L")");
        }
    }
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
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Schedule recording", L"Channel not found",
                       L"Couldn't match \"" + channelName + L"\" to a playable channel in your "
                       L"library (its tvg-id isn't in an enabled playlist).");
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
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Schedule recording", L"Recording scheduled",
                       title + L"\r\n" + s.channelName +
                           L"\r\n\r\nThe app must be running at the scheduled time.");
    } else {
        showInfoDialog(st->hwnd, hInst, st->dpi, L"Schedule recording", L"Could not schedule",
                       L"The recording could not be saved.");
    }
}

// Settings ▸ Scheduled Recordings… — the manager (list + New/Cancel/Delete). The host
// callbacks own the recorder + DB so cancel/delete stop an active recording, and New
// opens scheduleDialog over the manager and stores + nudges the scheduler.
void onManageSchedules(AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    ScheduleManagerCallbacks cb;
    cb.list = [st] { return st->db.listSchedules(); };
    cb.cancel = [st](long long id) {
        if (st->activeScheduleId == id) {
            st->ap().player.stopRecording();
            st->activeScheduleId = 0;
        }
        st->db.updateScheduleStatus(id, ScheduleStatus::Cancelled);
    };
    cb.remove = [st](long long id) {
        if (st->activeScheduleId == id) {
            st->ap().player.stopRecording();
            st->activeScheduleId = 0;
        }
        st->db.deleteSchedule(id);
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
        setStatus(st, L"No categories to filter — this library has no group titles.");
        showInfoDialog(st->hwnd,
                       reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE)),
                       st->dpi, L"Categories", L"No categories to filter",
                       L"The channels in your library have no group titles, so there are no "
                       L"categories to include or exclude.\r\n\r\n"
                       L"Add a playlist whose #EXTINF lines carry group-title tags to use this "
                       L"filter.\r\n");
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

// Command-bar Settings menu: Open File, About, recording format, and view toggles.
void showSettingsMenu(HWND hwnd, AppState* st, const RECT& anchor) {
    HMENU fmt = CreatePopupMenu();
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mkv" ? 0u : MF_CHECKED), ID_FMT_TS,
                L"MPEG-TS  (.ts)");
    AppendMenuW(fmt, MF_STRING | (st->recFormat == L"mkv" ? MF_CHECKED : 0u), ID_FMT_MKV,
                L"Matroska  (.mkv)");

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN_FILE, L"Open File…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt), L"Recording format");
    AppendMenuW(m, MF_STRING | (st->hideDead ? MF_CHECKED : 0u), ID_HIDE_DEAD,
                L"Hide unavailable channels");
    std::wstring catLabel = L"Categories…";
    if (st->categoryActive) catLabel += L"  (" + std::to_wstring(st->categories.size()) + L")";
    AppendMenuW(m, MF_STRING | (st->categoryActive ? MF_CHECKED : 0u), ID_CATEGORIES,
                catLabel.c_str());
    AppendMenuW(m, MF_STRING, ID_EPG_GUIDE, L"TV Guide");
    AppendMenuW(m, MF_STRING, ID_EPG_REFRESH, L"Refresh Guide…");
    AppendMenuW(m, MF_STRING, ID_SCHEDULES, L"Scheduled Recordings…");

    // Meters… opens the full setup dialog (per-meter enable + look + colours + the data-flow
    // row live there now — the old inline quick-toggle checkboxes were redundant).
    AppendMenuW(m, MF_STRING, ID_METERS_SETUP, L"Meters…");
    AppendMenuW(m, MF_STRING | (st->videoOnly ? MF_CHECKED : 0u), ID_VIDEO_ONLY,
                L"Video only\tCtrl+Shift+V");

    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Single ? MF_CHECKED : 0u),
                ID_VIEW_SINGLE, L"Single");
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Split ? MF_CHECKED : 0u),
                ID_VIEW_SPLIT, L"Split (2×2)");
    AppendMenuW(viewMenu, MF_STRING | (st->viewMode == ViewMode::Pip ? MF_CHECKED : 0u),
                ID_VIEW_PIP, L"Picture-in-picture");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"View");

    HMENU layoutMenu = CreatePopupMenu();
    AppendMenuW(layoutMenu, MF_STRING, ID_LAYOUT_RESET, L"Reset to default");
    AppendMenuW(layoutMenu, MF_SEPARATOR, 0, nullptr);
    const struct { const wchar_t* name; Panel p; } dockPanels[] = {
        {L"Move sidebar", Panel::Nav}, {L"Move video", Panel::Video}, {L"Move channels", Panel::Grid}};
    const wchar_t* dockSides[] = {L"To left", L"To right", L"To top", L"To bottom"};
    for (const auto& pn : dockPanels) {
        HMENU sub = CreatePopupMenu();
        for (int s = 0; s < 4; ++s)
            AppendMenuW(sub, MF_STRING, ID_DOCK_BASE + static_cast<int>(pn.p) * 4 + s, dockSides[s]);
        AppendMenuW(layoutMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), pn.name);
    }
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), L"Layout");

#ifdef RABBITEARS_THEME_ENGINE
    HMENU themeMenu = CreatePopupMenu();
    const std::string& skinSel = activeSkinSelection();
    AppendMenuW(themeMenu, MF_STRING | (skinSel == "system" ? MF_CHECKED : 0u), ID_THEME_SYSTEM,
                L"Follow System");
    AppendMenuW(themeMenu, MF_SEPARATOR, 0, nullptr);
    const auto& skins = builtinSkins();  // one item per registered skin (auto-grows in Phase 4)
    for (size_t i = 0; i < skins.size(); ++i)
        AppendMenuW(themeMenu, MF_STRING | (skinSel == skins[i].id ? MF_CHECKED : 0u),
                    ID_THEME_SKIN_BASE + static_cast<int>(i), wideFromUtf8(skins[i].name).c_str());
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), L"Theme");
#endif

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"About…");  // last item, ellipsis to match siblings

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
    }
}

// ---- window procs ----------------------------------------------------------

// Transport-button style: skin-native owner-draw flag-on (painted by
// drawTransportButton), classic OS push-button flag-off (byte-identical shipping look).
#ifdef RABBITEARS_THEME_ENGINE
constexpr DWORD kTransportBtnStyle = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW;

// The play/stop/record/fullscreen buttons paint from the active skin instead of the OS
// DarkMode_Explorer push-button look: flat (windowBg, so they melt into the transport
// strip band), a palette hover / pressed wash, and an accent glyph while playing /
// recording. Flag-off this whole path compiles out and they stay classic BUTTONs.
bool isTransportBtn(AppState* st, HWND h) {
    return h == st->btnPlay || h == st->btnStop || h == st->btnRec || h == st->btnFull;
}

// Owner-draw buttons don't report hover in DRAWITEMSTRUCT::itemState, so each is
// subclassed to track it in its (otherwise unused) GWLP_USERDATA; drawTransportButton
// reads it back. TrackMouseEvent delivers the matching WM_MOUSELEAVE.
LRESULT CALLBACK TransportBtnSubProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!GetWindowLongPtrW(hwnd, GWLP_USERDATA)) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 1);
                TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, TransportBtnSubProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// A soft accent bloom for a "lit" transport button — the skin's neon/glow layer on the
// owner-draw buttons (Phase 4b). Reuses the meters' GDI+ phosphor technique (MiniMeter
// drawTubeGlow): concentric low-alpha accent ellipses, wide dim halo -> bright inner
// core, sized to the button. GDI+ is the right tool here — a GPU surface behind these
// child-window buttons would hit the sibling-clipping wall the strip underglow already
// documents; GDI+ is started globally by runApp. `strength` 0..1 scales the whole bloom.
void drawTransportGlow(HDC hdc, const RECT& rc, COLORREF accent, float strength) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float cx = (rc.left + rc.right) / 2.0f;
    const float cy = (rc.top + rc.bottom) / 2.0f;
    const float minDim = static_cast<float>(std::min(rc.right - rc.left, rc.bottom - rc.top));
    const BYTE cr = GetRValue(accent), cg = GetGValue(accent), cb = GetBValue(accent);
    const struct { float frac; BYTE alpha; } layers[] = {{0.60f, 44}, {0.44f, 66}, {0.28f, 96}};
    for (const auto& L : layers) {
        const float rad = minDim * L.frac;
        const auto a = static_cast<BYTE>(L.alpha * strength);
        Gdiplus::SolidBrush br(Gdiplus::Color(a, cr, cg, cb));
        g.FillEllipse(&br, cx - rad, cy - rad, rad * 2.0f, rad * 2.0f);
    }
}

void drawTransportButton(AppState* st, const DRAWITEMSTRUCT* dis) {
    const Theme& th = currentTheme();
    const RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool hover = GetWindowLongPtrW(dis->hwndItem, GWLP_USERDATA) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    // The record button stays lit while a recording is live; any button lights on hover.
    // Play/pause isn't treated as "active" while playing — you almost always are, so the
    // live cue would never rest — it is reserved for recording.
    const bool active = (dis->hwndItem == st->btnRec && st->ap().player.isRecording());
    const bool lit = (hover || active) && !disabled;

    // Flat into the strip band at rest; a muted-accent wash on press. Hover feedback is
    // the accent bloom (below), not a bg lift, so the glow reads on the flat band.
    const COLORREF bg = pressed ? th.selectionBg : th.windowBg;
    FillRect(dis->hDC, &rc, themeBrush(bg));
    if (lit) drawTransportGlow(dis->hDC, rc, th.accent, active ? 1.0f : 0.55f);

    const COLORREF fg = disabled           ? th.textMuted
                        : (lit || pressed) ? th.textPrimary  // bright core inside the bloom
                                           : th.textSecondary;
    wchar_t glyph[8] = L"";
    GetWindowTextW(dis->hwndItem, glyph, ARRAYSIZE(glyph));  // current MDL2 glyph (swaps with state)
    HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, st->glyphFont));
    const int oldBk = SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    RECT tr = rc;
    DrawTextW(dis->hDC, glyph, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SetBkMode(dis->hDC, oldBk);
    SelectObject(dis->hDC, oldFont);

    // Keyboard focus needs an affordance the flat fill doesn't give: a thin accent frame.
    // ODS_NOFOCUSRECT is set once the user is on the mouse (focus cues hidden), so honour
    // it — the frame shows only for keyboard navigation, matching classic button behaviour.
    if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
        RECT fr = rc;
        InflateRect(&fr, -dpiScale(2, st->dpi), -dpiScale(2, st->dpi));
        FrameRect(dis->hDC, &fr, themeBrush(th.accent));
    }
}
#else
constexpr DWORD kTransportBtnStyle = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
#endif

void createChildren(HWND hwnd, AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    st->uiFont = themeFont(FontRole::Body, st->dpi);      // skin-driven flag-on; Segoe UI 14 flag-off
    st->titleFont = themeFont(FontRole::Title, st->dpi);  // Segoe UI 16 semibold
    st->glyphFont = themeFont(FontRole::Glyph, st->dpi);  // Segoe MDL2 Assets 13

    addPane(hwnd, st, 0);  // pane 0: the primary video surface + its player (always present)
#ifdef RABBITEARS_THEME_ENGINE
    // GPU transport-strip underglow: windowless. The parent's WM_PAINT (+ a timer tick)
    // BitBlt it into the strip band behind the transport controls — WS_CLIPCHILDREN keeps
    // it behind them, exactly like the plain strip fill it replaces. Off → GDI fill.
    st->skinStripOn = skin::initSkinStrip();
    if (st->skinStripOn) SetTimer(hwnd, kSkinAnimTimer, 16, nullptr);  // ~60 fps
#endif
    SetTimer(hwnd, kSchedulerTimer, 30000, nullptr);  // recording scheduler: check every ~30 s
    st->nav = CreateWindowExW(0, WC_TREEVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TVS_HASBUTTONS |
                                  TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_TRACKSELECT,
                              0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NAV)),
                              hInst, nullptr);
    // Double-buffer the nav TreeView so it doesn't flicker when the parent force-repaints
    // every child during a splitter/gutter drag (RDW_ALLCHILDREN). The D2D channel grid is
    // already flicker-free; this brings the standard-control sidebar to parity.
    TreeView_SetExtendedStyle(st->nav, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    // (The old single nav splitter is replaced by the dock tree's divider gutters,
    // painted + dragged by the parent — see layout()/WM_PAINT/WM_LBUTTONDOWN.)
    registerChannelGridClass(hInst);
    st->grid = createChannelGrid(hwnd, hInst, ID_GRID, st->dpi);
    st->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH)), hInst,
                                 nullptr);
    SendMessageW(st->search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search channels…"));

    st->btnPlay = CreateWindowExW(0, L"BUTTON", kGlyphPlay, kTransportBtnStyle, 0, 0, 10, 10, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_PLAY)), hInst, nullptr);
    st->btnStop = CreateWindowExW(0, L"BUTTON", kGlyphStop, kTransportBtnStyle, 0, 0, 10, 10, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_STOP)), hInst, nullptr);
    st->btnRec = CreateWindowExW(0, L"BUTTON", kGlyphRecord, kTransportBtnStyle, 0, 0, 10, 10, hwnd,
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
    st->btnFull = CreateWindowExW(0, L"BUTTON", kGlyphFull, kTransportBtnStyle, 0, 0, 10, 10,
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
    // Drag-to-redock grips (one per region), positioned + shown by layout().
    for (HWND* g : {&st->gripNav, &st->gripVideo, &st->gripGrid})
        *g = CreateWindowExW(0, kGripClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, hwnd,
                             nullptr, hInst, nullptr);
    SetWindowLongPtrW(st->gripNav, GWLP_USERDATA, static_cast<LONG_PTR>(Panel::Nav));
    SetWindowLongPtrW(st->gripVideo, GWLP_USERDATA, static_cast<LONG_PTR>(Panel::Video));
    SetWindowLongPtrW(st->gripGrid, GWLP_USERDATA, static_cast<LONG_PTR>(Panel::Grid));

    SendMessageW(st->volBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(st->volBar, TBM_SETPOS, TRUE, st->ap().player.volume());
    SendMessageW(st->bufBar, TBM_SETRANGE, TRUE,
                 MAKELPARAM(kBufMinMs / kBufStepMs, kBufMaxMs / kBufStepMs));
    SendMessageW(st->bufBar, TBM_SETPOS, TRUE, st->ap().player.networkCaching() / kBufStepMs);
    SetWindowTextW(st->bufLabel, bufLabelText(st->ap().player.networkCaching()).c_str());

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
                          st->meterSignal, st->meterBitrate, st->meterFrames, st->btnPlay,
                          st->btnStop, st->btnRec, st->btnFull}) {
            ti.uId = reinterpret_cast<UINT_PTR>(tool);
            SendMessageW(st->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        SendMessageW(st->tip, TTM_SETMAXTIPWIDTH, 0, dp(280, st->dpi));  // enable multiline (meter stats)
    }

    for (HWND h : {st->search, st->status, st->volBar, st->bufBar, st->bufLabel, st->nav}) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->uiFont), TRUE);
        SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    }
    // Transport buttons render Segoe MDL2 icon glyphs, so they take the glyph font.
    for (HWND h : {st->btnPlay, st->btnStop, st->btnRec, st->btnFull}) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->glyphFont), TRUE);
        SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    }
#ifdef RABBITEARS_THEME_ENGINE
    // BS_OWNERDRAW transport buttons are painted by drawTransportButton (WM_DRAWITEM in
    // MainProc); subclass each to track hover, which owner-draw doesn't report on its own.
    for (HWND h : {st->btnPlay, st->btnStop, st->btnRec, st->btnFull})
        SetWindowSubclass(h, TransportBtnSubProc, 1, 0);
#endif
    TreeView_SetBkColor(st->nav, currentTheme().panelBg);
    TreeView_SetTextColor(st->nav, currentTheme().textPrimary);

    ChannelGridCallbacks cb;
    cb.onActivate = [st](const Channel& c) { playChannel(st, c); };
    cb.onToggleFavourite = [st](const Channel& c) {
        st->db.setFavourite(c.id, c.favourite);
        if (st->filter.kind == ViewKind::Favourites) loadForFilter(st);
    };
    cb.onSetNumber = [st](const Channel& c) { st->db.setChannelNumber(c.id, c.lcn); };
    cb.onContextMenu = [st](const Channel& c, POINT pt) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Play");
        AppendMenuW(m, MF_STRING, 2, L"Play in PIP");
        const int cmd =
            TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, st->hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 1) {
            playChannel(st, c);
        } else if (cmd == 2) {
            if (st->viewMode != ViewMode::Pip) applyViewMode(st, ViewMode::Pip);  // ensure a PIP exists
            playChannelInPane(st, c, 1);  // pane 1 is the PIP — plays muted; click the PIP to hear it
        }
    };
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
            st->engine.init();            // one shared libVLC instance backs every player
            createChildren(hwnd, st);     // creates pane 0 (its window + player, bound to the engine)
            st->dock = DockLayout::makeDefault();  // valid tree even if the DB fails below

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
                {  // per-meter look + palette (Settings → Meters…)
                    HWND mtr[4] = {st->meterSpectrum, st->meterSignal, st->meterBitrate,
                                   st->meterFrames};
                    static const wchar_t* mk[4] = {L"spectrum", L"signal", L"bitrate", L"frames"};
                    for (int r = 0; r < 4; ++r) {
                        const MeterKind k = static_cast<MeterKind>(r);
                        if (auto s = st->db.getSetting(std::wstring(L"meter_") + mk[r] + L"_style"))
                            miniMeterSetStyle(mtr[r], meterStyleFromString(*s, defaultMeterStyle(k)));
                        if (auto c = st->db.getSetting(std::wstring(L"meter_") + mk[r] + L"_colors"))
                            miniMeterSetPalette(mtr[r],
                                                meterPaletteFromString(*c, defaultMeterPalette(k)));
                        if (auto kn = st->db.getSetting(std::wstring(L"meter_") + mk[r] + L"_knobs"))
                            miniMeterSetTuning(mtr[r], meterTuningFromString(*kn, defaultMeterTuning()));
                    }
                }
                if (auto dl = st->db.getSetting(L"dock_layout"); dl && !dl->empty())
                    st->dock = DockLayout::parse(*dl);
#ifdef RABBITEARS_THEME_ENGINE
                if (auto sk = st->db.getSetting(wideFromUtf8(skinSettingKey())); sk && !sk->empty())
                    activeSkinSelection() = utf8FromWide(*sk);
                applyActiveSkin(hwnd, st, /*repaint=*/false);  // window not shown yet; first paint uses it
#endif
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
        case WM_MOVE:
            // The floating PIP is positioned in SCREEN coords, so it must follow the main window.
            if (st->viewMode == ViewMode::Pip) positionFloatingPip(st);
            break;  // fall through to DefWindowProc
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
        case WM_ACTIVATE:
            // Keep the video focused while the chrome is hidden, so Esc reaches VideoProc even
            // after the user Alt-Tabs away and back (video-only + fullscreen both rely on it).
            if (LOWORD(wParam) != WA_INACTIVE && (st->videoOnly || st->fullscreen))
                SetFocus(st->ap().hwnd);
            break;
        case WM_KEYDOWN:
            // Esc is also handled here, not only in VideoProc: after a resize the main window
            // holds focus, so VideoProc never sees the key. Exit the active view mode.
            if (wParam == VK_ESCAPE) {
                if (st->fullscreen) toggleFullscreen(st);
                else if (st->videoOnly) toggleVideoOnly(st);
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (!st->fullscreen && !st->videoOnly) {
                drawCmdBar(hwnd, st, hdc);
                // Paint the transport-strip background here (like the command bar) rather
                // than relying on WM_ERASEBKGND: relayouts invalidate NOERASE, so if the
                // strip isn't filled on every paint, meters that move/hide leave stale
                // "blank grid" footprints and the strip's top edge shows vertical seams.
                // WS_CLIPCHILDREN keeps this fill behind the meter/button children on top.
                const RECT vidR = st->panelRects[static_cast<int>(Panel::Video)];
                RECT strip{vidR.left, vidR.bottom - stripH(st->dpi), vidR.right, vidR.bottom};
                bool gdiStrip = true;
#ifdef RABBITEARS_THEME_ENGINE
                // hdc is BeginPaint's DC — child-clipped by WS_CLIPCHILDREN, so the underglow
                // lands behind the transport controls, exactly like the plain fill would.
                if (st->skinStripOn && skin::paintSkinStrip(hdc, strip, st->dpi)) gdiStrip = false;
#endif
                if (gdiStrip) FillRect(hdc, &strip, themeBrush(currentTheme().windowBg));
                paintGutters(st, hdc);  // dock dividers live in the gaps between panels
                // Active-pane indicator (Split/PIP only): an accent frame in the gap around the
                // active pane so it's clear which pane the transport + audio drive. Drawn in the
                // parent's child-clipped DC, so it lands in the inter-pane gap, not over the
                // libVLC surface. Single view has one pane, so no indicator is needed.
                if (st->viewMode != ViewMode::Single && !st->panes.empty() &&
                    !st->panes[st->active]->floating) {
                    RECT r = st->paneBounds[st->active];
                    const int t = dp(2, st->dpi);
                    InflateRect(&r, t, t);
                    HBRUSH br = themeBrush(currentTheme().accent);
                    for (int k = 0; k < t; ++k) {
                        FrameRect(hdc, &r, br);
                        InflateRect(&r, -1, -1);
                    }
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (st && wParam == kSchedulerTimer) {
                onSchedulerTick(st);
                return 0;
            }
#ifdef RABBITEARS_THEME_ENGINE
            if (st && wParam == kSkinAnimTimer) {
                // Animate the GPU strip via a child-clipped DC (DCX_CLIPCHILDREN) so the
                // transport controls are excluded and the command bar isn't redrawn each
                // frame. Idle while fullscreen (strip hidden), minimized, or not visible.
                if (st->skinStripOn && !st->fullscreen && !st->videoOnly && !IsIconic(hwnd) &&
                    IsWindowVisible(hwnd)) {
                    const RECT vidR = st->panelRects[static_cast<int>(Panel::Video)];
                    RECT strip{vidR.left, vidR.bottom - stripH(st->dpi), vidR.right, vidR.bottom};
                    if (HDC dc = GetDCEx(hwnd, nullptr, DCX_CACHE | DCX_CLIPCHILDREN)) {
                        skin::paintSkinStrip(dc, strip, st->dpi);
                        ReleaseDC(hwnd, dc);
                    }
                }
                return 0;
            }
#endif
            break;
        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (st->panelDragActive && (wParam & MK_LBUTTON)) {
                updateDockTarget(hwnd, st, pt);  // move the drop-zone highlight
                return 0;
            }
            if (st->draggingGutter && (wParam & MK_LBUTTON)) {
                const RECT& nr = st->dragGutter.nodeRect;
                const int gutterW = dp(5, st->dpi);
                double ratio;
                if (st->dragGutter.vertical) {
                    const int span = std::max(1, static_cast<int>(nr.bottom - nr.top) - gutterW);
                    ratio = static_cast<double>(pt.y - nr.top) / span;
                } else {
                    const int span = std::max(1, static_cast<int>(nr.right - nr.left) - gutterW);
                    ratio = static_cast<double>(pt.x - nr.left) / span;
                }
                st->dock.setRatio(st->dragGutter.node, ratio);
                layout(hwnd, st);  // relayout + gutter repaint (layout() invalidates)
                // Pace the synchronous repaint at ~60Hz rather than per mouse-move.
                // WM_PAINT is lower priority than input, so with NO sync flush the
                // queued paints starve during a fast drag (the old vertical-streak
                // bug) — but flushing on EVERY move floods sync paints (drag lag) and
                // erasing the video child to black mid-frame made it flash. Every
                // ~15ms: repaint the parent (gutters/strip/cmd bar, all drawn in
                // WM_PAINT) and the two flicker-safe resizing panels (double-buffered
                // TreeView + D2D grid). The video child is never forced — libVLC
                // repaints it at frame rate on its own. The strip controls move by
                // bit-copy now (kSwpMove in layout()), so they need no repaint at all.
                const ULONGLONG nowTick = GetTickCount64();
                if (nowTick - st->gutterFlushTick >= 15) {
                    st->gutterFlushTick = nowTick;
                    RedrawWindow(hwnd, nullptr, nullptr,
                                 RDW_UPDATENOW | RDW_NOCHILDREN | RDW_NOERASE);
                    if (st->nav) UpdateWindow(st->nav);
                    if (st->grid) UpdateWindow(st->grid);
                }
                return 0;
            }
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
            if (!st->fullscreen) {
                if (const DockLayout::Gutter* g = gutterAt(st, pt)) {
                    st->draggingGutter = true;
                    st->dragGutter = *g;  // copy: node ptr + nodeRect stay valid through the drag
                    SetCapture(hwnd);
                    return 0;
                }
            }
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
        case WM_LBUTTONUP:
            if (st->panelDragActive) {
                endPanelDrag(hwnd, st, /*commit=*/true);
                return 0;
            }
            if (st->draggingGutter) {
                st->draggingGutter = false;
                ReleaseCapture();
                persistDock(st);  // save the new split ratios
                // One settling flush so every child (incl. the bit-copied strip
                // controls and the video panel) lands crisp at the final geometry.
                RedrawWindow(hwnd, nullptr, nullptr,
                             RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            // Capture stolen mid-drag (Alt-Tab, UAC, Win+L, another app foregrounding)
            // — end any drag cleanly so a divider / panel can't stick to the cursor.
            if (st->panelDragActive) endPanelDrag(hwnd, st, /*commit=*/false);
            if (st->draggingGutter) {
                st->draggingGutter = false;
                persistDock(st);
                RedrawWindow(hwnd, nullptr, nullptr,
                             RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);  // settle
            }
            return 0;
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTTOP) { SetCursor(LoadCursorW(nullptr, IDC_SIZENS)); return TRUE; }
            if (LOWORD(lParam) == HTCLIENT && !st->fullscreen) {
                POINT cp;
                GetCursorPos(&cp);
                ScreenToClient(hwnd, &cp);
                if (const DockLayout::Gutter* g = gutterAt(st, cp)) {
                    SetCursor(LoadCursorW(nullptr, g->vertical ? IDC_SIZENS : IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
#ifdef RABBITEARS_THEME_ENGINE
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (dis && dis->CtlType == ODT_BUTTON && isTransportBtn(st, dis->hwndItem)) {
                drawTransportButton(st, dis);
                return TRUE;
            }
            break;  // not one of ours -> fall through to DefWindowProc
        }
#endif
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
                    channelGridSetNowPlaying(st->grid, st->ap().nowPlayingId);
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
                case ID_BTN_PLAY: st->ap().player.togglePause(); return 0;
                case ID_BTN_STOP:
                    st->ap().player.stop();
                    st->ap().nowPlayingId = 0;
                    channelGridSetNowPlaying(st->grid, 0);
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    resetStatMeters(st);
                    setStatus(st, L"Stopped");
                    return 0;
                case ID_BTN_REC: onToggleRecord(st); return 0;
                case ID_BTN_FULL: toggleFullscreen(st); return 0;
                case ID_VIDEO_ONLY: toggleVideoOnly(st); return 0;  // Ctrl+Shift+V accelerator + menu
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = reinterpret_cast<NMHDR*>(lParam);
            if (nh->code == TTN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                di->hinst = nullptr;
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->bufferMeter)) {
                    const FlowStats fs = st->ap().player.flowStats();
                    const double bufS = st->ap().player.networkCaching() / 1000.0;
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
                               st->ap().player.networkCaching() / 1000.0);
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
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->btnPlay)) {
                    di->lpszText = const_cast<LPWSTR>(st->ap().player.isPlaying() ? L"Pause" : L"Play");
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->btnStop)) {
                    di->lpszText = const_cast<LPWSTR>(L"Stop");
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->btnRec)) {
                    di->lpszText = const_cast<LPWSTR>(st->ap().player.isRecording() ? L"Stop recording"
                                                                               : L"Record");
                    return 0;
                }
                if (nh->idFrom == reinterpret_cast<UINT_PTR>(st->btnFull)) {
                    di->lpszText = const_cast<LPWSTR>(L"Fullscreen");
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
                        AppendMenuW(menu, MF_STRING, 2, L"Rename…");
                        AppendMenuW(menu, MF_STRING, 3, L"Set Guide URL…");
                        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                        AppendMenuW(menu, MF_STRING, 1, L"Delete Playlist");
                        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x,
                                                       scr.y, 0, hwnd, nullptr);
                        DestroyMenu(menu);
                        const long long pid = st->navFilters[fi].playlistId;
                        if (cmd == 2) {  // Rename… — changes the friendly display name only
                            std::wstring name = label;  // seed the box with the current name
                            if (promptText(hwnd,
                                           reinterpret_cast<HINSTANCE>(
                                               GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                                           st->dpi, L"Rename Playlist", L"Playlist name:", name) &&
                                !name.empty()) {
                                st->db.renamePlaylist(pid, name);
                                diag::info(L"renamed playlist id=" + std::to_wstring(pid) + L" to \"" +
                                           name + L"\"");
                                refreshNav(st);
                                setStatus(st, L"Playlist renamed to \"" + name + L"\"");
                            }
                        } else if (cmd == 3) {  // Set Guide URL… — override this playlist's XMLTV URL
                            promptSetGuideUrl(hwnd, st, pid);
                        } else if (cmd == 1) {  // Delete Playlist
                            const std::wstring m = L"Delete playlist \"" + std::wstring(label) +
                                                   L"\"?\n\nThis removes its channels from RabbitEars.";
                            if (MessageBoxW(hwnd, m.c_str(), L"Delete Playlist",
                                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                                st->db.deletePlaylist(pid);
                                diag::info(L"deleted playlist id=" + std::to_wstring(pid) + L" (" +
                                           label + L")");
                                st->filter = {ViewKind::All};
                                refreshNav(st);
                                loadForFilter(st);
                                setStatus(st, L"Playlist deleted");
                            }
                        }
                    } else if (fi >= 0 && fi < static_cast<LPARAM>(st->navFilters.size()) &&
                               st->navFilters[fi].kind == ViewKind::Guide) {
                        // TV Guide node: EPG management — Refresh + a custom guide URL per playlist.
                        // (Don't TreeView_SelectItem it: selecting the Guide node opens the window.)
                        HMENU menu = CreatePopupMenu();
                        AppendMenuW(menu, MF_STRING, 100, L"Refresh Guide");
                        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                        const auto pls = st->db.listPlaylists();
                        if (pls.empty()) {
                            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Set Guide URL… (no playlists)");
                        } else if (pls.size() == 1) {
                            AppendMenuW(menu, MF_STRING, 200, L"Set Guide URL…");
                        } else {
                            HMENU sub = CreatePopupMenu();
                            for (size_t i = 0; i < pls.size() && i < 64; ++i)
                                AppendMenuW(sub, MF_STRING, 200 + static_cast<int>(i),
                                            (pls[i].name + L"…").c_str());
                            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Set Guide URL");
                        }
                        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, scr.x,
                                                       scr.y, 0, hwnd, nullptr);
                        DestroyMenu(menu);
                        if (cmd == 100)
                            onEpgRefresh(st);
                        else if (cmd >= 200 && cmd < 200 + static_cast<int>(pls.size()))
                            promptSetGuideUrl(hwnd, st, pls[cmd - 200].id);
                    }
                }
                return TRUE;
            }
            if (nh->idFrom == ID_NAV && nh->code == TVN_SELCHANGEDW) {
                auto* tv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                const LPARAM idx = tv->itemNew.lParam;
                if (idx >= 0 && idx < static_cast<LPARAM>(st->navFilters.size())) {
                    if (st->navFilters[idx].kind == ViewKind::Guide) {
                        onEpgGuide(st);  // action node: open the TV Guide window (not a grid filter)
                    } else {
                        st->filter = st->navFilters[idx];
                        loadForFilter(st);
                    }
                }
            }
            return 0;
        }
        case WM_HSCROLL: {
            HWND ctl = reinterpret_cast<HWND>(lParam);
            if (ctl == st->volBar) {
                st->ap().player.setVolume(static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0)));
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
            // Events carry the posting pane's index in the HIWORD (LOWORD = the PlayerEvent).
            // Only the active pane drives the shared transport strip + meters; inactive panes
            // still play but must not flip the play/pause glyph or hijack the meters.
            if (HIWORD(wParam) != st->active) break;
            switch (static_cast<PlayerEvent>(LOWORD(wParam))) {
                case PlayerEvent::Opening:
                    bufferMeterSetHealth(st->bufferMeter, 15);
                    setStatus(st, L"Opening: " + st->ap().nowPlayingName);
                    diag::info(L"event: Opening — " + st->ap().nowPlayingName);
                    break;
                case PlayerEvent::Buffering: {
                    const int pct = static_cast<int>(lParam);
                    bufferMeterSetHealth(st->bufferMeter, pct);
                    setStatus(st, L"Buffering " + std::to_wstring(pct) + L"%  —  " + st->ap().nowPlayingName);
                    break;
                }
                case PlayerEvent::Playing:
                    bufferMeterSetHealth(st->bufferMeter, 100);
                    SetWindowTextW(st->btnPlay, kGlyphPause);
                    setStatus(st, L"Playing: " + st->ap().nowPlayingName);
                    diag::info(L"event: Playing — " + st->ap().nowPlayingName);
                    if (st->ap().nowPlayingId) {
                        st->db.setDeadStatus(st->ap().nowPlayingId, DeadStatus::Alive,
                                             static_cast<long long>(time(nullptr)));
                        channelGridSetDeadStatus(st->grid, st->ap().nowPlayingId, DeadStatus::Alive);
                    }
                    break;
                case PlayerEvent::Paused:
                    SetWindowTextW(st->btnPlay, kGlyphPlay);
                    setStatus(st, L"Paused: " + st->ap().nowPlayingName);
                    diag::info(L"event: Paused — " + st->ap().nowPlayingName);
                    break;
                case PlayerEvent::Stats: {
                    // Turn real stream stats into honest meter signals.
                    const FlowStats fs = st->ap().player.flowStats();
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
                    SetWindowTextW(st->btnPlay, kGlyphPlay);
                    diag::info(L"event: Stopped");
                    break;
                case PlayerEvent::EndReached:
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    resetStatMeters(st);
                    SetWindowTextW(st->btnPlay, kGlyphPlay);
                    setStatus(st, L"Stream ended");
                    diag::info(L"event: EndReached — " + st->ap().nowPlayingName);
                    break;
                case PlayerEvent::Error:
                    bufferMeterSetHealth(st->bufferMeter, 0);
                    resetStatMeters(st);
                    SetWindowTextW(st->btnPlay, kGlyphPlay);
                    setStatus(st, L"Unavailable (offline or geo-locked): " + st->ap().nowPlayingName);
                    diag::error(L"event: PLAYBACK ERROR (offline / geo-locked / codec) — " +
                                st->ap().nowPlayingName);
                    if (st->ap().nowPlayingId) {
                        st->db.setDeadStatus(st->ap().nowPlayingId, DeadStatus::Dead,
                                             static_cast<long long>(time(nullptr)));
                        channelGridSetDeadStatus(st->grid, st->ap().nowPlayingId, DeadStatus::Dead);
                    }
                    break;
            }
            return 0;
        case WM_APP_PLAYLIST_DONE:
            onPlaylistDone(st, reinterpret_cast<PlaylistResult*>(lParam));
            return 0;
        case WM_APP_EPG_PROGRESS: {
            std::wstring* progress = reinterpret_cast<std::wstring*>(lParam);
            if (progress) {
                updateLoadingDialog(st->loadingDlg, *progress);
                delete progress;
            }
            return 0;
        }
        case WM_APP_EPG_DONE:
            onEpgDone(st, reinterpret_cast<EpgResult*>(lParam));
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
            armExitWatchdog(4000);   // bound teardown so a stuck libVLC release can't wedge exit
            KillTimer(hwnd, kSchedulerTimer);
#ifdef RABBITEARS_THEME_ENGINE
            KillTimer(hwnd, kSkinAnimTimer);
            skin::shutdownSkinStrip();
            st->skinStripOn = false;
#endif
            st->spectrumTap.stop();  // join the capture thread before the meter HWNDs die
            for (auto& p : st->panes)
                p->player.shutdown();  // join every pane's worker + reaper threads
            st->engine.shutdown();     // then release the shared libVLC instance (all players are down)
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
            HDC dc = reinterpret_cast<HDC>(wParam);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            // An empty floating PIP is a small black popup that's easy to miss. Until a channel
            // is played into it (nowPlayingId != 0, after which libVLC's surface covers this),
            // highlight it: an accent frame + a centred hint so it's obvious where the PIP is and
            // how to fill it. Only the floating PIP pane gets this — tiles are laid out in the grid.
            AppState* st = stateOf(GetParent(hwnd));
            const int idx = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (st && idx >= 0 && idx < static_cast<int>(st->panes.size()) &&
                st->panes[idx]->floating && st->panes[idx]->nowPlayingId == 0) {
                const Theme& th = currentTheme();
                HBRUSH ab = themeBrush(th.accent);
                RECT fr = rc;
                const int t = dp(3, st->dpi);
                for (int k = 0; k < t; ++k) {  // a bold accent frame just inside the edge
                    FrameRect(dc, &fr, ab);
                    InflateRect(&fr, -1, -1);
                }
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, th.textSecondary);
                HGDIOBJ of = SelectObject(dc, st->uiFont);
                const wchar_t* hint = L"Picture-in-Picture\r\nRight-click a channel ▸ Play in PIP";
                RECT tr = rc;
                InflateRect(&tr, -dp(10, st->dpi), 0);
                RECT calc = tr;
                DrawTextW(dc, hint, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
                tr.top = rc.top + ((rc.bottom - rc.top) - (calc.bottom - calc.top)) / 2;
                DrawTextW(dc, hint, -1, &tr, DT_CENTER | DT_WORDBREAK);
                SelectObject(dc, of);
            }
            return 1;
        }
        case WM_LBUTTONDOWN: {
            // A manual click-drag (not WM_NCLBUTTONDOWN, so it coexists with the double-click exit):
            // on the floating PIP it moves the PIP; in video-only it moves the title-bar-less main
            // window; otherwise a plain click just makes this pane active.
            AppState* st = stateOf(GetParent(hwnd));
            if (!st) break;
            const int idx = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            const bool isPip =
                idx >= 0 && idx < static_cast<int>(st->panes.size()) && st->panes[idx]->floating;
            if (isPip) {
                if (idx != st->active) setActivePane(st, idx);  // clicking the PIP activates it too
                RECT wr;
                GetWindowRect(hwnd, &wr);  // drag THIS window (the PIP), not the parent
                GetCursorPos(&st->videoDragStart);
                st->videoDragOrigin = {wr.left, wr.top};
                st->videoDragging = true;
                st->videoDragMoved = false;
                st->draggingPip = true;
                SetCapture(hwnd);
                return 0;
            }
            if (st->videoOnly) {
                RECT wr;
                GetWindowRect(GetParent(hwnd), &wr);
                GetCursorPos(&st->videoDragStart);
                st->videoDragOrigin = {wr.left, wr.top};
                st->videoDragging = true;
                st->videoDragMoved = false;
                st->draggingPip = false;
                SetCapture(hwnd);
                return 0;
            }
            if (idx != st->active) setActivePane(st, idx);  // plain click: activate this pane
            break;
        }
        case WM_MOUSEMOVE: {
            AppState* st = stateOf(GetParent(hwnd));
            if (st && st->videoDragging && (wParam & MK_LBUTTON)) {
                POINT c;
                GetCursorPos(&c);
                const int dx = c.x - st->videoDragStart.x, dy = c.y - st->videoDragStart.y;
                if (!st->videoDragMoved && (std::abs(dx) >= GetSystemMetrics(SM_CXDRAG) ||
                                            std::abs(dy) >= GetSystemMetrics(SM_CYDRAG)))
                    st->videoDragMoved = true;  // past the dead zone -> a real drag, not a shaky click
                if (st->videoDragMoved) {
                    const int nx = st->videoDragOrigin.x + dx, ny = st->videoDragOrigin.y + dy;
                    if (st->draggingPip) {
                        // Move the PIP popup; remember its offset from the main client so it keeps
                        // tracking the main window afterwards (positionFloatingPip honours pipMoved).
                        SetWindowPos(hwnd, nullptr, nx, ny, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        POINT tl{nx, ny};
                        ScreenToClient(st->hwnd, &tl);
                        st->pipPos = tl;
                        st->pipMoved = true;
                    } else {
                        SetWindowPos(GetParent(hwnd), nullptr, nx, ny, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            AppState* st = stateOf(GetParent(hwnd));
            if (st && st->videoDragging) {
                st->videoDragging = false;
                st->draggingPip = false;
                ReleaseCapture();
                return 0;
            }
            break;
        }
        case WM_CAPTURECHANGED: {
            AppState* st = stateOf(GetParent(hwnd));
            if (st) { st->videoDragging = false; st->draggingPip = false; }  // capture lost -> end drag
            return 0;
        }
        case WM_RBUTTONUP: {
            // A focus-independent view menu: Esc can be lost to the libVLC surface after a
            // resize or Alt-Tab, but a right-click menu always works.
            AppState* st = stateOf(GetParent(hwnd));
            if (!st) break;
            HMENU pm = CreatePopupMenu();
            AppendMenuW(pm, MF_STRING | (st->videoOnly ? MF_CHECKED : 0u), 1, L"Video only");
            AppendMenuW(pm, MF_STRING | (st->fullscreen ? MF_CHECKED : 0u), 2, L"Fullscreen");
            AppendMenuW(pm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(pm, MF_STRING | (st->viewMode == ViewMode::Single ? MF_CHECKED : 0u), 3,
                        L"Single view");
            AppendMenuW(pm, MF_STRING | (st->viewMode == ViewMode::Split ? MF_CHECKED : 0u), 4,
                        L"Split view (2×2)");
            AppendMenuW(pm, MF_STRING | (st->viewMode == ViewMode::Pip ? MF_CHECKED : 0u), 5,
                        L"Picture-in-picture");
            POINT pt;
            GetCursorPos(&pt);
            const int cmd = TrackPopupMenu(pm, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                           GetParent(hwnd), nullptr);
            DestroyMenu(pm);
            if (cmd == 1) {
                toggleVideoOnly(st);
            } else if (cmd == 2) {
                if (st->videoOnly) toggleVideoOnly(st);  // the two modes are mutually exclusive
                toggleFullscreen(st);
            } else if (cmd == 3) {
                applyViewMode(st, ViewMode::Single);
            } else if (cmd == 4) {
                applyViewMode(st, ViewMode::Split);
            } else if (cmd == 5) {
                applyViewMode(st, ViewMode::Pip);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            AppState* st = stateOf(GetParent(hwnd));
            if (st && st->videoOnly)
                toggleVideoOnly(st);  // in video-only, a double-click restores the chrome
            else
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ID_BTN_FULL, 0), 0);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == 'F') {
                AppState* st = stateOf(GetParent(hwnd));
                if (st && st->fullscreen) toggleFullscreen(st);
                else if (st && st->videoOnly && wParam == VK_ESCAPE) toggleVideoOnly(st);
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

    WNDCLASSEXW gc{};  // drag-to-redock grip
    gc.cbSize = sizeof(gc);
    gc.lpfnWndProc = DockGripProc;
    gc.hInstance = hInst;
    gc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    gc.lpszClassName = kGripClass;
    RegisterClassExW(&gc);

    WNDCLASSEXW oc{};  // translucent drop-zone overlay
    oc.cbSize = sizeof(oc);
    oc.lpfnWndProc = DropOverlayProc;
    oc.hInstance = hInst;
    oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    oc.lpszClassName = kOverlayClass;
    RegisterClassExW(&oc);
}

}  // namespace mw

int runApp(HINSTANCE hInst, int nCmdShow) {
    using namespace mw;  // the window internals now live in rabbitears::mw
    // Single-instance guard (before diag::init, so a second launch doesn't rotate the
    // running instance's log). The mutex name matches the installer's AppMutex, so the
    // auto-update installer can also detect/close a stray instance. Held for the process
    // lifetime (released on exit); a second launch just focuses the existing window.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"RabbitEars.SingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kMainClass, nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(instanceMutex);
        return 0;
    }

    diag::init(RE_VERSION_DISPLAY_W);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    // Branded splash up front: the main window's WM_CREATE blocks for a few seconds
    // (libVLC init + DB load), so show something immediately. It's a layered window,
    // so DWM keeps compositing it while this thread is busy building the main window.
    HWND splash = showSplash(hInst);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES |
                                              ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES |
                                              ICC_DATE_CLASSES};
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
    // The window was created at an unscaled 1180x760 — the monitor DPI isn't known until the
    // HWND exists. WM_CREATE has now set st->dpi, so rescale to the real DPI while the window
    // is still hidden (before the ToU gate / ShowWindow). Otherwise, on a >100% monitor it is
    // born narrower than the DPI-scaled min-track-size (WM_GETMINMAXINFO uses dp(1060,dpi)), and
    // the width snaps up to that minimum on the first user resize.
    if (st->dpi != 96)
        SetWindowPos(hwnd, nullptr, 0, 0, dp(1180, st->dpi), dp(760, st->dpi),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Terms of Use gate: the user must accept before the app is usable, and must
    // RE-ACCEPT on every version change (new install OR update). `tos_accepted` stores
    // the full version (marketing.build) it was last accepted for, so any bump
    // re-prompts. The main window exists (DB opened in WM_CREATE) but is not shown yet,
    // so the gate appears on its own; declining tears everything down and exits.
    if (st->db.isOpen()) {
        const std::wstring tosVer = RE_VERSION_FULL_W;
        const auto accepted = st->db.getSetting(L"tos_accepted");
        if (!accepted || *accepted != tosVer) {
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
            st->db.setSetting(L"tos_accepted", tosVer);
            diag::info(L"Terms accepted for " + tosVer);
        }
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    if (splash) closeSplash(splash);  // main window is up (already closed on the first-run path)

    initUpdater(hwnd);  // WinSparkle: start background update checks (+ shutdown coordination)

    // Ctrl+Shift+V toggles "Video only" from anywhere (an accelerator, so it fires whatever
    // child control has focus); it routes to WM_COMMAND(ID_VIDEO_ONLY) on the main window.
    ACCEL accels[] = {{FCONTROL | FSHIFT | FVIRTKEY, 'V', ID_VIDEO_ONLY}};
    HACCEL hAccel = CreateAcceleratorTableW(accels, ARRAYSIZE(accels));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    shutdownUpdater();
    delete st;
    Gdiplus::GdiplusShutdown(gdipToken);
    diag::shutdown();
    return static_cast<int>(m.wParam);
}

}  // namespace rabbitears
