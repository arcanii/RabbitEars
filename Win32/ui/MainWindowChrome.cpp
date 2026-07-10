// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window — command bar + caption + layout (split from MainWindow.cpp).
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
        // comes from the layout (which folds in a user resize via st->pipW/pipH — see the vgo
        // sites). Clamp a remembered position so the PIP never strands outside the client area —
        // a persisted pipPos can be stale after a window resize, monitor change, or DPI change.
        POINT tl = st->pipMoved ? st->pipPos : POINT{r.left, r.top};
        if (st->pipMoved) {
            RECT cli;
            GetClientRect(st->hwnd, &cli);
            tl.x = std::max(0L, std::min(tl.x, cli.right - w));
            tl.y = std::max(0L, std::min(tl.y, cli.bottom - h));
        }
        ClientToScreen(st->hwnd, &tl);
        const bool show = (w > 0 && h > 0) && !st->fullscreen && !st->videoOnly;
        // HWND_TOPMOST so it composites over the main window's libVLC D3D surface (an owned but
        // non-topmost popup did not beat that surface).
        SetWindowPos(p->hwnd, HWND_TOPMOST, tl.x, tl.y, w, h,
                     SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        // The PIP's vout hosts are children of this popup — keep them filling its client rect.
        for (HWND vh : p->voutHosts)
            if (vh) SetWindowPos(vh, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Fold a user-chosen PIP size (the resize grip; persisted "pip_size") into the layout options,
// clamped so a stale setting can never produce an unusably tiny or region-swallowing PIP.
static void applyUserPipSize(const AppState* st, VideoGridOpts& vgo, int regionW, int regionH) {
    if (st->pipW <= 0 || st->pipH <= 0) return;  // 0 = no user resize; keep the defaults
    const int minW = dp(120, st->dpi), minH = dp(68, st->dpi);
    const int maxW = std::max(minW, regionW * 3 / 5), maxH = std::max(minH, regionH * 3 / 5);
    vgo.pipW = std::max(minW, std::min(st->pipW, maxW));
    vgo.pipH = std::max(minH, std::min(st->pipH, maxH));
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
        // Tile the panes per the CURRENT view mode across the whole client (no chrome/strip), so
        // Split stays a 2x2 grid and Single/PIP fill it with the main pane — rather than collapsing
        // to just the active tile. The floating PIP is a separate top-level window, hidden in these
        // modes by positionFloatingPip().
        VideoGridOpts vgoF;
        vgoF.gap = dp(3, st->dpi);
        vgoF.pipW = dp(220, st->dpi);
        vgoF.pipH = dp(124, st->dpi);
        vgoF.pipMargin = dp(12, st->dpi);
        applyUserPipSize(st, vgoF, W, H);
        const auto boxesF = computeVideoPanes(st->viewMode, static_cast<int>(st->panes.size()),
                                              0, 0, W, H, vgoF);
        for (int i = 0; i < static_cast<int>(st->panes.size()); ++i) {
            const PaneBox& b = boxesF[i];
            if (i < 4) st->paneBounds[i] = RECT{b.x, b.y, b.x + b.w, b.y + b.h};
            HWND h = st->panes[i]->hwnd;
            if (!h) continue;
            if (st->panes[i]->floating) { ShowWindow(h, SW_HIDE); continue; }
            if (!dwp) continue;
            dwp = DeferWindowPos(dwp, h, nullptr, b.x, b.y, std::max(0, b.w), std::max(0, b.h),
                                 kSwp | SWP_SHOWWINDOW);
            // Keep every vout host filling its pane (children of the pane -> pane-client coords).
            for (HWND vh : st->panes[i]->voutHosts)
                if (vh) SetWindowPos(vh, nullptr, 0, 0, std::max(0, b.w), std::max(0, b.h),
                                     SWP_NOZORDER | SWP_NOACTIVATE);
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
    applyUserPipSize(st, vgo, vidW, videoAreaH);
    const auto boxes = computeVideoPanes(st->viewMode, static_cast<int>(st->panes.size()),
                                         static_cast<int>(vidR.left), static_cast<int>(vidR.top),
                                         vidW, videoAreaH, vgo);
    for (int i = 0; i < static_cast<int>(st->panes.size()); ++i) {
        const PaneBox& b = boxes[i];
        if (i < 4) st->paneBounds[i] = RECT{b.x, b.y, b.x + b.w, b.y + b.h};
        HWND h = st->panes[i]->hwnd;
        // Floating (PIP) panes aren't children of this window — positionFloatingPip() places them
        // (and their vout hosts) in screen coords after the deferred child batch.
        if (!h || st->panes[i]->floating || !dwp) continue;
        const UINT f = SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_SHOWWINDOW | SWP_NOZORDER;
        dwp = DeferWindowPos(dwp, h, nullptr, b.x, b.y, std::max(0, b.w), std::max(0, b.h), f);
        // Keep every vout host filling its pane (children of the pane -> pane-client coords). Hidden
        // hosts are sized too, so they're correct the instant PlayerEvent::Playing reveals one.
        for (HWND vh : st->panes[i]->voutHosts)
            if (vh) SetWindowPos(vh, nullptr, 0, 0, std::max(0, b.w), std::max(0, b.h),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
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


}  // namespace mw
}  // namespace rabbitears
