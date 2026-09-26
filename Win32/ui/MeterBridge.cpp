// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/MeterBridge.h"

#include <algorithm>
#include <string>

#include "resource.h"
#include "ui/BufferMeter.h"
#include "ui/MainWindowInternal.h"
#include "ui/MeterTray.h"
#include "ui/MiniMeter.h"
#include "ui/Theme.h"
#include "ui/Tr.h"

namespace rabbitears {
namespace mw {
namespace {

constexpr wchar_t kClass[] = L"RabbitEarsMeterBridge";
constexpr int kIdSpectrum = 1, kIdSignal = 2, kIdBitrate = 3, kIdFrames = 4, kIdTank = 5;

HWND      g_bridge = nullptr;
AppState* g_st = nullptr;

struct Bridge {
    HWND spectrum = nullptr, signal = nullptr, bitrate = nullptr, frames = nullptr, tank = nullptr;
    UINT dpi = 96;
    bool placing = false;  // SetWindowPlacement in progress: a DPI change then keeps the placed size
};

Bridge* stateOf(HWND h) { return reinterpret_cast<Bridge*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

// The tray's meter for each of ours, and whether the tray shows it — the bridge shows the same set
// (the tank while the tray's is not hidden).
struct Pair {
    HWND tray, mine;
    bool on;
    int  w96;  // width at the standard height (MeterTray.h — the tray's own)
};

void pairs(const AppState* st, const Bridge* b, Pair out[5]) {
    out[0] = {st->meterSpectrum, b->spectrum, st->showSpectrum, kTrayMeterW96[0]};
    out[1] = {st->meterSignal, b->signal, st->showSignal, kTrayMeterW96[1]};
    out[2] = {st->meterBitrate, b->bitrate, st->showBitrate, kTrayMeterW96[2]};
    out[3] = {st->meterFrames, b->frames, st->showFrames, kTrayMeterW96[3]};
    out[4] = {st->bufferMeter, b->tank, !bufferMeterHidden(st->bufferMeter), kTrayTankW96};
}

// One row, left to right as in the tray, as tall as the window allows — the widths keep the tray's
// proportions, scaled together, and shrink with the height when the row would not fit the width.
void layoutBridge(HWND hwnd) {
    Bridge* b = stateOf(hwnd);
    if (!b || !g_st) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int pad = dp(12, b->dpi), gap = dp(8, b->dpi), base = dp(kMeterHeightStd, b->dpi);
    Pair ps[5];
    pairs(g_st, b, ps);
    int sumW = 0, n = 0;
    for (const Pair& p : ps)
        if (p.on && p.mine) {
            sumW += dp(p.w96, b->dpi);
            ++n;
        }
    if (n == 0) {  // nothing on: nothing shown (not the last layout's meters, nor a paused tank at 0,0)
        for (const Pair& p : ps)
            if (p.mine) ShowWindow(p.mine, SW_HIDE);
        if (b->tank) bufferMeterSetHidden(b->tank, true);
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    const int availW = std::max(1, static_cast<int>(rc.right) - 2 * pad - gap * (n - 1));
    const int availH = std::max(1, static_cast<int>(rc.bottom) - 2 * pad);
    double scale = static_cast<double>(availH) / base;
    scale = std::min(scale, static_cast<double>(availW) / sumW);
    // No taller than the tray's own maximum (stage A): the fixed-pitch looks' cost grows with the area
    // (Tube: ~5 ms a frame per meter at 120 dp, measured — RabbitEarsRender --bench-paint). In pixels,
    // so the rounding of dp() at this DPI cannot push it past trayDp(120).
    scale = std::min(scale, static_cast<double>(trayDp(kMeterHeightMax, b->dpi)) / base);
    const int h = std::max(1, static_cast<int>(base * scale));
    int total = gap * (n - 1);
    for (const Pair& p : ps)
        if (p.on && p.mine) total += static_cast<int>(dp(p.w96, b->dpi) * scale);
    int x = (static_cast<int>(rc.right) - total) / 2;
    const int y = (static_cast<int>(rc.bottom) - h) / 2;
    HDWP dwp = BeginDeferWindowPos(n + 1);
    // The tank's sim runs only while it is shown here (a hidden tank pauses; it also un-hides one that its
    // own menu hid, once the tray shows the tank again).
    if (b->tank) bufferMeterSetHidden(b->tank, !ps[4].on);
    for (const Pair& p : ps) {
        if (!p.mine) continue;
        if (!p.on) {
            if (dwp) dwp = DeferWindowPos(dwp, p.mine, nullptr, 0, 0, 0, 0,
                                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
            continue;
        }
        const int w = static_cast<int>(dp(p.w96, b->dpi) * scale);
        if (dwp) dwp = DeferWindowPos(dwp, p.mine, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        x += w + gap;
    }
    if (dwp) EndDeferWindowPos(dwp);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// The window's NORMAL rect (GetWindowPlacement — not the maximized/minimized one), in workspace
// coordinates, which is what SetWindowPlacement restores from.
void saveRect(HWND hwnd) {
    if (!g_st || !g_st->db.isOpen()) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp)) return;
    const RECT r = wp.rcNormalPosition;
    g_st->db.setSetting(L"meter_bridge_rect", std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," +
                                                  std::to_wstring(r.right - r.left) + L"," +
                                                  std::to_wstring(r.bottom - r.top));
}

// Make our meters twins of the tray's: the same look, palette and tuning now, and every later change
// (and all data) through the mirror link.
void linkMirrors(Bridge* b) {
    if (!g_st) return;
    Pair ps[5];
    pairs(g_st, b, ps);
    for (int i = 0; i < 4; ++i) {
        if (!ps[i].tray || !ps[i].mine) continue;
        miniMeterSetStyle(ps[i].mine, miniMeterStyle(ps[i].tray));
        miniMeterSetPalette(ps[i].mine, miniMeterPalette(ps[i].tray));
        miniMeterSetTuning(ps[i].mine, miniMeterTuning(ps[i].tray));
        miniMeterSetMirror(ps[i].tray, ps[i].mine);
    }
    if (ps[4].tray && ps[4].mine) bufferMeterSetMirror(ps[4].tray, ps[4].mine);
}

// BEFORE our meters die: Windows destroys an owned window (this) before its owner and a window's
// children after its own WM_DESTROY, so this runs while every meter still exists — and the spectrum
// unlink waits out any forward the audio thread has in flight (miniMeterSetMirror).
void unlinkMirrors() {
    if (!g_st) return;
    for (HWND t : {g_st->meterSpectrum, g_st->meterSignal, g_st->meterBitrate, g_st->meterFrames})
        if (t) miniMeterSetMirror(t, nullptr);
    if (g_st->bufferMeter) bufferMeterSetMirror(g_st->bufferMeter, nullptr);
}

LRESULT CALLBACK BridgeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Bridge* b = stateOf(hwnd);
    switch (msg) {
        case WM_CREATE: {
            auto* nb = new Bridge();
            nb->dpi = GetDpiForWindow(hwnd);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(nb));
            HINSTANCE hInst = reinterpret_cast<const CREATESTRUCTW*>(lParam)->hInstance;
            nb->spectrum = createMiniMeter(hwnd, hInst, kIdSpectrum, nb->dpi, MeterKind::Spectrum);
            nb->signal = createMiniMeter(hwnd, hInst, kIdSignal, nb->dpi, MeterKind::Signal);
            nb->bitrate = createMiniMeter(hwnd, hInst, kIdBitrate, nb->dpi, MeterKind::Bitrate);
            nb->frames = createMiniMeter(hwnd, hInst, kIdFrames, nb->dpi, MeterKind::Frames);
            nb->tank = createBufferMeter(hwnd, hInst, kIdTank, nb->dpi);
            // Its own right-click Hide is the tray tank's setting (buffer_hidden), so the two agree —
            // the bridge then drops its tank, as it does when the tray's is hidden.
            bufferMeterSetOnHiddenChanged(nb->tank, [](bool hidden) {
                if (!g_st) return;
                bufferMeterSetHidden(g_st->bufferMeter, hidden);
                if (g_st->db.isOpen()) g_st->db.setSetting(L"buffer_hidden", hidden ? L"1" : L"0");
                if (g_bridge) layoutBridge(g_bridge);
            });
            linkMirrors(nb);
            return 0;
        }
        case WM_SIZE:
            layoutBridge(hwnd);
            return 0;
        case WM_EXITSIZEMOVE:
            saveRect(hwnd);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().windowBg));
            return 1;
        }
        case WM_GETMINMAXINFO: {
            const UINT d = b ? b->dpi : GetDpiForWindow(hwnd);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = dp(320, d);
            mmi->ptMinTrackSize.y = dp(90, d);
            return 0;
        }
        case WM_DPICHANGED: {
            if (b) {
                b->dpi = HIWORD(wParam);
                for (HWND m : {b->spectrum, b->signal, b->bitrate, b->frames})
                    if (m) miniMeterSetDpi(m, b->dpi);
                if (b->tank) bufferMeterSetDpi(b->tank, b->dpi);
            }
            // Moved to another monitor by the user: the suggested rect keeps the window's size in dp.
            // While openBridge places it at its saved rect, that rect IS the size: keep it (or every
            // launch onto a monitor of another scaling would grow or shrink it by the ratio).
            if (!b || !b->placing) {
                const RECT* r = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_CLOSE:  // the user closed it: it stays closed at the next launch
            if (g_st && g_st->db.isOpen()) g_st->db.setSetting(L"meter_bridge", L"0");
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            saveRect(hwnd);
            unlinkMirrors();
            if (g_bridge == hwnd) g_bridge = nullptr;
            return 0;
        case WM_NCDESTROY:
            delete b;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void openBridge(AppState* st) {
    if (g_bridge) return;
    g_st = st;
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(st->hwnd, GWLP_HINSTANCE));
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = BridgeProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    // Where it was last (its normal rect, workspace coordinates), if a good part of that is still on a
    // screen's work area; else a wide strip over the main window's lower half — its NORMAL rect too, so
    // a main window launched minimized does not put the bridge at the icon's -32000.
    // Workspace coordinates are screen coordinates less the work area's offset on its monitor (a taskbar
    // on the top or left): `onScreen` is the saved rect in screen coordinates, for the visibility check
    // and so the window is created ON its monitor (its DPI) before SetWindowPlacement puts it exactly.
    RECT r{}, onScreen{};
    bool haveRect = false;
    if (auto s = st->db.getSetting(L"meter_bridge_rect")) {
        int v[4] = {};
        if (swscanf_s(s->c_str(), L"%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4 && v[2] > 0 && v[3] > 0) {
            r = {v[0], v[1], v[0] + v[2], v[1] + v[3]};
            if (HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                MONITORINFO mi{sizeof(mi)};
                if (GetMonitorInfoW(mon, &mi)) {
                    onScreen = r;
                    OffsetRect(&onScreen, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
                    RECT on{};
                    haveRect = IntersectRect(&on, &onScreen, &mi.rcWork) && on.right - on.left >= dp(120, st->dpi) &&
                               on.bottom - on.top >= dp(60, st->dpi);
                }
            }
        }
    }
    if (!haveRect) {
        WINDOWPLACEMENT mwp{sizeof(mwp)};
        GetWindowPlacement(st->hwnd, &mwp);
        const RECT mr = mwp.rcNormalPosition;
        const int w = std::min(static_cast<int>(mr.right - mr.left), dp(960, st->dpi)), h = dp(230, st->dpi);
        const int x = mr.left + ((mr.right - mr.left) - w) / 2, y = mr.bottom - h - dp(80, st->dpi);
        r = {x, y, x + w, y + h};
        onScreen = r;  // the main window's own monitor: the offset is at most a taskbar, the DPI the same
    }
    g_bridge = CreateWindowExW(0, kClass, tr(i18n::StringId::MeterBridgeTitle).c_str(),
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, onScreen.left, onScreen.top,
                               r.right - r.left, r.bottom - r.top, st->hwnd, nullptr, hInst, nullptr);
    if (!g_bridge) return;
    applyDialogDarkMode(g_bridge);
    WINDOWPLACEMENT wp{sizeof(wp)};
    wp.showCmd = SW_SHOWNOACTIVATE;
    wp.rcNormalPosition = r;
    Bridge* b = stateOf(g_bridge);
    if (b) b->placing = true;
    SetWindowPlacement(g_bridge, &wp);  // shows it, at the normal rect
    if (b) b->placing = false;
    layoutBridge(g_bridge);
}

}  // namespace

void toggleMeterBridge(AppState* st) {
    if (!st) return;
    if (g_bridge) {
        SendMessageW(g_bridge, WM_CLOSE, 0, 0);  // remembers "closed"
        return;
    }
    if (st->db.isOpen()) st->db.setSetting(L"meter_bridge", L"1");
    openBridge(st);
}

bool meterBridgeOpen() { return g_bridge && IsWindow(g_bridge); }

void restoreMeterBridge(AppState* st) {
    if (st && st->db.isOpen() && st->db.getSetting(L"meter_bridge").value_or(L"0") == L"1") openBridge(st);
}

void meterBridgeRelayout(AppState* st) {
    (void)st;
    if (!g_bridge) return;
    layoutBridge(g_bridge);
    // The meters too: an idle one (nothing playing) would keep the old glass / fluid colour.
    RedrawWindow(g_bridge, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void meterBridgeRefreshTheme() {
    if (!g_bridge) return;
    applyDialogDarkMode(g_bridge);
    RedrawWindow(g_bridge, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void meterBridgeRefreshLanguage() {
    if (g_bridge) SetWindowTextW(g_bridge, tr(i18n::StringId::MeterBridgeTitle).c_str());
}

}  // namespace mw
}  // namespace rabbitears
