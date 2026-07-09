// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window — dock gutters + drag-to-redock (split from MainWindow.cpp).
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

// ---- dock gutters + re-dock helpers ----------------------------------------

const DockLayout::Gutter* gutterAt(AppState* st, POINT pt) {
    for (const auto& g : st->gutters)
        if (PtInRect(&g.rc, pt)) return &g;
    return nullptr;
}

void paintGutters(AppState* st, HDC hdc) {
    for (const auto& g : st->gutters) {
#ifdef RABBITEARS_THEME_ENGINE
        // Per-skin neon edge glow (Phase 4b-2). Skipped mid-drag: the gutter resizes every
        // paced flush, so the plain border avoids per-frame GPU texture churn; the glow
        // returns on release. hdc is child-clipped (WM_PAINT), so the panels show through.
        if (st->skinStripOn && !st->draggingGutter && skin::paintSkinEdge(hdc, g.rc, st->dpi))
            continue;
#endif
        FillRect(hdc, &g.rc, themeBrush(currentTheme().border));
    }
}

void persistDock(AppState* st) { st->db.setSetting(L"dock_layout", st->dock.serialize()); }

void applyDockChange(HWND hwnd, AppState* st) {
    layout(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
    persistDock(st);
}

// Dock panel `p` against the outer `side` of the whole layout (outside whichever
// other panel currently sits at that extreme).
void dockToEdge(AppState* st, Panel p, DockSide side) {
    RECT rects[kPanelCount];
    std::vector<DockLayout::Gutter> g;
    st->dock.computeRects(RECT{0, 0, 1000, 1000}, 0, 0, rects, g);
    Panel target = p;
    long best = 0;
    bool first = true;
    for (int k = 0; k < kPanelCount; ++k) {
        if (k == static_cast<int>(p)) continue;
        const RECT& r = rects[k];
        const long v = (side == DockSide::Left)   ? r.left
                       : (side == DockSide::Right) ? -r.right
                       : (side == DockSide::Top)   ? r.top
                                                   : -r.bottom;
        if (first || v < best) {
            best = v;
            target = static_cast<Panel>(k);
            first = false;
        }
    }
    if (target != p) st->dock.dock(p, side, target);
}

// ---- drag-to-redock: grips + translucent snap-zone overlay ------------------

LRESULT CALLBACK DropOverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(reinterpret_cast<HDC>(w), &rc, themeBrush(currentTheme().accent));
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND ensureDropOverlay(HWND parent, AppState* st) {
    if (st->dockOverlay) return st->dockOverlay;
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
    st->dockOverlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                          WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                      kOverlayClass, L"", WS_POPUP, 0, 0, 10, 10, parent, nullptr,
                                      hInst, nullptr);
    if (st->dockOverlay)
        SetLayeredWindowAttributes(st->dockOverlay, 0, 110, LWA_ALPHA);  // ~43% translucent coral
    return st->dockOverlay;
}

void beginPanelDrag(HWND parent, AppState* st, Panel p) {
    st->panelDragActive = true;
    st->panelDragFrom = p;
    st->panelDropValid = false;
    ensureDropOverlay(parent, st);
    SetCapture(parent);
}

// Find the drop target (panel + side) under `pt` (client coords) and position the
// overlay over the half the region would take. Hidden when there is no valid target.
void updateDockTarget(HWND hwnd, AppState* st, POINT pt) {
    st->panelDropValid = false;
    for (int k = 0; k < kPanelCount; ++k) {
        if (static_cast<Panel>(k) == st->panelDragFrom) continue;
        const RECT& r = st->panelRects[k];
        if (r.right <= r.left || r.bottom <= r.top || !PtInRect(&r, pt)) continue;
        const double wdt = r.right - r.left, hgt = r.bottom - r.top;
        const double fx = (pt.x - r.left) / wdt, fy = (pt.y - r.top) / hgt;
        const double dl = fx, dr = 1.0 - fx, dt = fy, db = 1.0 - fy;
        const double mn = std::min(std::min(dl, dr), std::min(dt, db));
        const DockSide side = (mn == dl)   ? DockSide::Left
                              : (mn == dr) ? DockSide::Right
                              : (mn == dt) ? DockSide::Top
                                           : DockSide::Bottom;
        RECT hl = r;
        if (side == DockSide::Left) hl.right = r.left + static_cast<LONG>(wdt / 2);
        else if (side == DockSide::Right) hl.left = r.left + static_cast<LONG>(wdt / 2);
        else if (side == DockSide::Top) hl.bottom = r.top + static_cast<LONG>(hgt / 2);
        else hl.top = r.top + static_cast<LONG>(hgt / 2);
        st->panelDropTo = static_cast<Panel>(k);
        st->panelDropSide = side;
        st->panelDropValid = true;
        POINT tl{hl.left, hl.top}, br{hl.right, hl.bottom};
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);
        if (st->dockOverlay)
            SetWindowPos(st->dockOverlay, HWND_TOPMOST, tl.x, tl.y, br.x - tl.x, br.y - tl.y,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    if (st->dockOverlay) ShowWindow(st->dockOverlay, SW_HIDE);
}

void endPanelDrag(HWND hwnd, AppState* st, bool commit) {
    if (!st->panelDragActive) return;
    st->panelDragActive = false;
    ReleaseCapture();
    if (st->dockOverlay) ShowWindow(st->dockOverlay, SW_HIDE);
    if (commit && st->panelDropValid && st->panelDropTo != st->panelDragFrom) {
        st->dock.dock(st->panelDragFrom, st->panelDropSide, st->panelDropTo);
        applyDockChange(hwnd, st);
    }
    st->panelDropValid = false;
}

LRESULT CALLBACK DockGripProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const Theme& th = currentTheme();
            AppState* st = stateOf(GetParent(hwnd));
            const UINT dpi = st ? st->dpi : 96u;
            HBRUSH dcb = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc, th.panelElevBg);
            FillRect(dc, &rc, dcb);
            SetDCBrushColor(dc, th.border);
            FrameRect(dc, &rc, dcb);
            const int ds = std::max(2, dp(2, dpi)), gap = std::max(2, dp(2, dpi));
            const int gwid = 2 * ds + gap, ghgt = 3 * ds + 2 * gap;
            const int ox = rc.left + (rc.right - rc.left - gwid) / 2;
            const int oy = rc.top + (rc.bottom - rc.top - ghgt) / 2;
            SetDCBrushColor(dc, th.textMuted);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 2; ++c) {
                    RECT dot{ox + c * (ds + gap), oy + r * (ds + gap), ox + c * (ds + gap) + ds,
                             oy + r * (ds + gap) + ds};
                    FillRect(dc, &dot, dcb);
                }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        case WM_LBUTTONDOWN: {
            HWND parent = GetParent(hwnd);
            if (AppState* st = stateOf(parent))
                beginPanelDrag(parent, st, static_cast<Panel>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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


}  // namespace mw
}  // namespace rabbitears
