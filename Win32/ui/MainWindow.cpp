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
        AppendMenuW(m, MF_STRING, 3, L"Show in TV Guide");
        const int cmd =
            TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, st->hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 1) {
            playChannel(st, c);
        } else if (cmd == 2) {
            if (st->viewMode != ViewMode::Pip) applyViewMode(st, ViewMode::Pip);  // ensure a PIP exists
            playChannelInPane(st, c, 1);  // pane 1 is the PIP — plays muted; click the PIP to hear it
        } else if (cmd == 3) {
            // Jump to this channel's row in the guide. Build the guide first if needed —
            // onEpgGuide is synchronous but may early-return without creating the window
            // (no EPG data), in which case the scroll below reports false and we explain.
            if (!epgGuideOpen()) onEpgGuide(st);
            if (!epgGuideShowChannel(c.tvgId, static_cast<long long>(time(nullptr))))
                setStatus(st, c.tvgId.empty()
                                  ? L"This channel has no tvg-id — the guide can't match it."
                                  : L"No guide row for " + c.name + L" — try Refresh Guide…");
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
                if (auto rf = st->db.getSetting(L"rec_format");
                    rf && (*rf == L"ts" || *rf == L"mkv" || *rf == L"mp4"))
                    st->recFormat = *rf;
                if (auto rl = st->db.getSetting(L"resume_last")) st->resumeLast = (*rl == L"1");
                if (auto wr = st->db.getSetting(L"wake_to_record")) st->wakeToRecord = (*wr == L"1");
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
                // Restore the view mode (persisted by applyViewMode). Pane 0 already exists
                // (createChildren above), so the switch is safe pre-show; the PIP size loads
                // first (layout() inside applyViewMode reads it), the PIP position after —
                // applyViewMode resets pipMoved, so setting it earlier would be discarded.
                if (auto ps = st->db.getSetting(L"pip_size"); ps && !ps->empty()) {
                    if (const size_t x = ps->find(L'x'); x != std::wstring::npos) {
                        st->pipW = _wtoi(ps->substr(0, x).c_str());
                        st->pipH = _wtoi(ps->substr(x + 1).c_str());
                    }
                }
                if (auto vm = st->db.getSetting(L"view_mode"); vm && !vm->empty()) {
                    const int m = _wtoi(vm->c_str());
                    if (m == static_cast<int>(ViewMode::Split) ||
                        m == static_cast<int>(ViewMode::Pip))
                        applyViewMode(st, static_cast<ViewMode>(m));
                }
                if (auto pp = st->db.getSetting(L"pip_pos"); pp && !pp->empty()) {
                    if (const size_t c = pp->find(L','); c != std::wstring::npos) {
                        st->pipPos.x = _wtoi(pp->substr(0, c).c_str());
                        st->pipPos.y = _wtoi(pp->substr(c + 1).c_str());
                        st->pipMoved = true;  // positionFloatingPip clamps a stale/off-window pos
                    }
                }
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
                // Resume the last-watched channel (Settings → Resume last channel, default on).
                // channelById returns nullopt if the channel/playlist was deleted — just skip.
                if (st->resumeLast) {
                    if (auto lc = st->db.getSetting(L"last_channel_id"); lc && !lc->empty()) {
                        if (auto ch = st->db.channelById(_wtoi64(lc->c_str()))) {
                            diag::info(L"resuming last channel: " + ch->name);
                            playChannel(st, *ch);
                        }
                    }
                }
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
            }
            // Active-pane indicator (Split/PIP) — drawn in ALL modes, including fullscreen /
            // video-only where the panes tile the whole client, so it's clear which tile drives
            // the transport + audio. In the parent's child-clipped DC, so it lands in the inter-
            // pane gap, not over a libVLC surface. Single view has one pane, so no indicator.
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
            if (nh->idFrom == ID_NAV && nh->code == NM_CLICK) {
                // Reopen the 📺 TV Guide when its node is clicked while ALREADY selected. Playing a
                // channel from the guide hides the guide window (hideEpgGuide) but leaves this node
                // selected, so a plain re-click fires no TVN_SELCHANGEDW (below) — the guide could
                // never be brought back. Handle the already-selected case here (SELCHANGEDW still
                // covers a fresh select / keyboard nav). onEpgGuide re-reveals a hidden guide.
                const DWORD mp = GetMessagePos();
                POINT cli{GET_X_LPARAM(mp), GET_Y_LPARAM(mp)};
                ScreenToClient(st->nav, &cli);
                TVHITTESTINFO ht{};
                ht.pt = cli;
                HTREEITEM item = TreeView_HitTest(st->nav, &ht);
                if (item && item == TreeView_GetSelection(st->nav)) {
                    TVITEMW ti{};
                    ti.mask = TVIF_PARAM;
                    ti.hItem = item;
                    if (TreeView_GetItem(st->nav, &ti)) {
                        const LPARAM fi = ti.lParam;
                        if (fi >= 0 && fi < static_cast<LPARAM>(st->navFilters.size()) &&
                            st->navFilters[fi].kind == ViewKind::Guide) {
                            if (epgGuideOpen()) revealEpgGuide(time(nullptr));  // instant reopen, no DB rebuild
                            else onEpgGuide(st);
                        }
                    }
                }
                return 0;
            }
            if (nh->idFrom == ID_NAV && nh->code == TVN_SELCHANGEDW) {
                auto* tv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                const LPARAM idx = tv->itemNew.lParam;
                if (idx >= 0 && idx < static_cast<LPARAM>(st->navFilters.size())) {
                    if (st->navFilters[idx].kind == ViewKind::Guide) {
                        // Action node: open the TV Guide window (not a grid filter). Instant reopen
                        // if it's already built — only the first open pays the DB rebuild.
                        if (epgGuideOpen()) revealEpgGuide(time(nullptr));
                        else onEpgGuide(st);
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
        case WM_APP_VLC: {
            // Events carry the posting pane's index in the HIWORD (LOWORD = the PlayerEvent).
            const int paneIdx = HIWORD(wParam);
            const PlayerEvent pe = static_cast<PlayerEvent>(LOWORD(wParam));
            // Vout-host swap runs for the POSTING pane regardless of which pane is active: the moment
            // its new stream is Playing, show that pane's live vout host and hide the rest. Deferring
            // this until Playing (rather than at set_hwnd time) keeps the OLD host visible through
            // Opening/Buffering, so a channel switch has no black gap.
            if (pe == PlayerEvent::Playing && paneIdx >= 0 &&
                paneIdx < static_cast<int>(st->panes.size())) {
                const HWND cur = st->panes[paneIdx]->player.currentHost();
                for (HWND h : st->panes[paneIdx]->voutHosts)
                    if (IsWindow(h)) ShowWindow(h, h == cur ? SW_SHOW : SW_HIDE);
            }
            // Only the active pane drives the shared transport strip + meters; inactive panes
            // still play but must not flip the play/pause glyph or hijack the meters.
            if (paneIdx != st->active) break;
            switch (pe) {
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
        }
        case WM_APP_MAKE_VOUT_HOST: {
            // A VlcPlayer worker ran out of free vout hosts and asked us (the UI thread, which owns
            // window creation) to grow its pane's pool. wParam = the pane HWND (== the player's
            // video_), lParam = the pane index. Validate the pane still matches — a stale request
            // from a worker torn down by a mode switch must not create an orphan host — then create,
            // size + register the host and hand its HWND back as the SendMessageTimeout result.
            const int paneIdx = static_cast<int>(lParam);
            if (paneIdx < 0 || paneIdx >= static_cast<int>(st->panes.size())) return 0;
            if (st->panes[paneIdx]->hwnd != reinterpret_cast<HWND>(wParam)) return 0;
            return reinterpret_cast<LRESULT>(makeVoutHost(st, paneIdx));
        }
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
            // Hand the queue to Windows on the way out — this is the whole point of wake-to-record:
            // the wake task must be registered for whatever is still pending BEFORE we stop running.
            // Also drop the sleep block; the process is going away and the state is per-thread.
            if (st->db.isOpen()) syncWakeFromSchedules(st);
            setRecordingKeepAwake(false);
#ifdef RABBITEARS_THEME_ENGINE
            KillTimer(hwnd, kSkinAnimTimer);
            skin::shutdownSkinStrip();
            st->skinStripOn = false;
#endif
            st->spectrumTap.stop();  // join the capture thread before the meter HWNDs die
            reapDyingPanes(st, /*force=*/true);  // drain async mode-switch teardowns first (their
                                                 // players still borrow the shared instance)
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
                // Bottom-right corner = RESIZE drag (dp(18) square); anywhere else = MOVE drag.
                // Hit-test via GetCursorPos, not lParam: while a stream plays the click arrives
                // forwarded from the covering vout host, whose lParam is in ITS client space.
                {
                    RECT cr;
                    GetClientRect(hwnd, &cr);
                    POINT sp;
                    GetCursorPos(&sp);
                    POINT lp = sp;
                    ScreenToClient(hwnd, &lp);
                    const int grip = dp(18, st->dpi);
                    if (lp.x >= cr.right - grip && lp.y >= cr.bottom - grip) {
                        st->resizingPip = true;
                        st->pipResizeStart = sp;
                        st->pipResizeOrigin = {cr.right, cr.bottom};
                        SetCapture(hwnd);
                        return 0;
                    }
                }
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
                // Video-only now tiles the 2x2 grid too, so a click must be able to switch the
                // active tile — not only drag the borderless window. Activate the clicked pane
                // first (a no-op in single-pane video-only), then arm the window drag; a real
                // drag past the dead zone still moves the window.
                if (idx >= 0 && idx < static_cast<int>(st->panes.size()) && idx != st->active)
                    setActivePane(st, idx);
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
            if (st && st->resizingPip && (wParam & MK_LBUTTON)) {
                POINT c;
                GetCursorPos(&c);
                // Route the new size through layout(): applyUserPipSize clamps it, and the
                // normal path re-places the popup, its vout hosts, and paneBounds together.
                st->pipW = static_cast<int>(st->pipResizeOrigin.cx) + (c.x - st->pipResizeStart.x);
                st->pipH = static_cast<int>(st->pipResizeOrigin.cy) + (c.y - st->pipResizeStart.y);
                layout(st->hwnd, st);
                return 0;
            }
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
            if (st && st->resizingPip) {
                st->resizingPip = false;
                ReleaseCapture();
                // Persist the EFFECTIVE (clamped) size: paneBounds holds what layout() actually
                // granted, so a wild drag doesn't store an out-of-range value.
                const int idx = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (idx >= 0 && idx < 4 && idx < static_cast<int>(st->panes.size())) {
                    const RECT& b = st->paneBounds[idx];
                    st->pipW = static_cast<int>(b.right - b.left);
                    st->pipH = static_cast<int>(b.bottom - b.top);
                }
                st->db.setSetting(L"pip_size", std::to_wstring(st->pipW) + L"x" +
                                                   std::to_wstring(st->pipH));
                return 0;
            }
            if (st && st->videoDragging) {
                // Persist the PIP position once, on release (mirrors persistDock — never per
                // mouse-move); only after a real drag, so a shaky click doesn't write the DB.
                const bool droppedPip = st->draggingPip && st->videoDragMoved;
                st->videoDragging = false;
                st->draggingPip = false;
                ReleaseCapture();
                if (droppedPip)
                    st->db.setSetting(L"pip_pos", std::to_wstring(st->pipPos.x) + L"," +
                                                      std::to_wstring(st->pipPos.y));
                return 0;
            }
            break;
        }
        case WM_CAPTURECHANGED: {
            AppState* st = stateOf(GetParent(hwnd));
            if (st) {  // capture lost -> end any drag/resize cleanly
                st->videoDragging = false;
                st->draggingPip = false;
                st->resizingPip = false;
            }
            return 0;
        }
        case WM_SETCURSOR: {
            // Resize-cursor feedback over the PIP's bottom-right grip corner. Only works while
            // no vout host covers the pane (i.e. the empty-PIP hint is showing) — the host
            // doesn't forward WM_SETCURSOR — but the resize DRAG itself always works (the
            // forwarded WM_LBUTTONDOWN hit-tests the same corner via GetCursorPos).
            AppState* st = stateOf(GetParent(hwnd));
            if (st && LOWORD(lParam) == HTCLIENT) {
                const int idx = static_cast<int>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (idx >= 0 && idx < static_cast<int>(st->panes.size()) &&
                    st->panes[idx]->floating) {
                    RECT cr;
                    GetClientRect(hwnd, &cr);
                    POINT cp;
                    GetCursorPos(&cp);
                    ScreenToClient(hwnd, &cp);
                    const int grip = dp(18, st->dpi);
                    if (cp.x >= cr.right - grip && cp.y >= cr.bottom - grip) {
                        SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                        return TRUE;
                    }
                }
            }
            break;
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

// Per-pane vout-host child window. libVLC renders into one of these (never the pane HWND directly),
// so a superseded stream's Direct3D vout can drain on its own host while the next stream attaches to
// a different, free one — instead of libVLC spawning a top-level "VLC (Direct3D11 output)" window.
// The host fills the pane, and (because mouse/key input is disabled on the player, so libVLC routes
// input to GetParent(vout) == this host) it forwards the clicks/keys it receives up to the pane's
// VideoProc, which owns activate/drag/dblclick-fullscreen/right-menu. Client coords match 1:1
// because the host fills the pane; a forwarded WM_LBUTTONDOWN does SetCapture(pane), so the ensuing
// move/up route straight to the pane.
LRESULT CALLBACK VoutHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONUP:
        case WM_KEYDOWN:
            if (HWND pane = GetParent(hwnd)) return SendMessageW(pane, msg, wParam, lParam);
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// UI thread only: create one vout host inside pane `paneIdx`, hidden and sized to the pane's client
// rect, register it in the pane's voutHosts, and return its HWND. Shared by addPane (pre-creating a
// steady-state pool) and the WM_APP_MAKE_VOUT_HOST handler (on-demand growth from a player worker).
// Returns null on a bad index or a dead pane window.
HWND makeVoutHost(AppState* st, int paneIdx) {
    if (!st || paneIdx < 0 || paneIdx >= static_cast<int>(st->panes.size())) return nullptr;
    HWND pane = st->panes[paneIdx]->hwnd;
    if (!IsWindow(pane)) return nullptr;
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(pane, GWLP_HINSTANCE));
    RECT rc{};
    GetClientRect(pane, &rc);
    // WS_CLIPSIBLINGS so one host's black erase can't paint over another while they briefly overlap
    // on a switch. Created HIDDEN (no WS_VISIBLE): layout() keeps it sized to the pane and
    // PlayerEvent::Playing shows it once its stream is live.
    HWND host = CreateWindowExW(0, kVoutHostClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, rc.right,
                                rc.bottom, pane, nullptr, hInst, nullptr);
    if (!host) return nullptr;
    st->panes[paneIdx]->voutHosts.push_back(host);
    return host;
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

    WNDCLASSEXW hc{};  // per-pane vout host: libVLC's Direct3D render target (see VoutHostProc)
    hc.cbSize = sizeof(hc);
    hc.style = CS_DBLCLKS;  // forward double-clicks to the pane (fullscreen toggle)
    hc.lpfnWndProc = VoutHostProc;
    hc.hInstance = hInst;
    hc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    hc.lpszClassName = kVoutHostClass;
    RegisterClassExW(&hc);

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

int runApp(HINSTANCE hInst, int nCmdShow, bool scheduledWake) {
    using namespace mw;  // the window internals now live in rabbitears::mw
    // Single-instance guard (before diag::init, so a second launch doesn't rotate the
    // running instance's log). The mutex name matches the installer's AppMutex, so the
    // auto-update installer can also detect/close a stray instance. Held for the process
    // lifetime (released on exit); a second launch just focuses the existing window.
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"RabbitEars.SingleInstance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // A wake-launch that finds us already running has nothing to do: the live instance's
        // scheduler tick will start the recording. Yanking its window to the foreground (and
        // over whatever the user is doing) would be the opposite of unattended.
        if (!scheduledWake) {
            if (HWND existing = FindWindowW(kMainClass, nullptr)) {
                if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
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
            // An unattended wake-launch must never sit at a modal dialog while the recording
            // window passes — an auto-update would otherwise silently break every scheduled
            // recording until the user next opened the app. If the Terms were accepted for a
            // PRIOR version, honour that for this run only: we deliberately do NOT write
            // tos_accepted, so the next interactive launch still re-prompts. A first-ever run
            // (no acceptance on record) still gates — we won't infer consent that never existed.
            const bool priorAcceptance = accepted && !accepted->empty();
            if (scheduledWake && priorAcceptance) {
                diag::info(L"scheduled wake: deferring the Terms re-prompt (last accepted for " +
                           *accepted + L"); it will be shown on the next interactive launch");
            } else {
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
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    // A wake-launch comes up minimized and WITHOUT taking focus: the user may be asleep, or
    // working in another app on a machine that just woke. It still runs a full message loop, so
    // the scheduler tick fires and records exactly as if the app had been open all along.
    ShowWindow(hwnd, scheduledWake ? SW_SHOWMINNOACTIVE : nCmdShow);
    UpdateWindow(hwnd);
    if (splash) closeSplash(splash);  // main window is up (already closed on the first-run path)
    if (scheduledWake) diag::info(L"started by the recording wake task (minimized, unattended)");

    initUpdater(hwnd);  // WinSparkle: start background update checks (+ shutdown coordination)

    // Run the scheduler once immediately rather than waiting up to 30 s for the first WM_TIMER:
    // a machine woken by the recording task has a deadline. This also expands any EPG rules and
    // (re)registers the wake task for whatever is still queued.
    if (st->db.isOpen()) onSchedulerTick(st);

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
