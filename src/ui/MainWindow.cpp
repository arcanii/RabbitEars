// SPDX-License-Identifier: GPL-3.0-or-later
// RabbitEars main window.
//
// Layer B1 (current): a themed window that proves the libVLC pipeline —
// embedded video surface + transport (play/pause, stop, volume) + status. The
// full custom title-bar chrome, nav sidebar and channel grid land next (B1b).
#include "ui/MainWindow.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>  // CreateStreamOnHGlobal (ole32)
#include <objidl.h>   // IStream — required by gdiplus.h below
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "resource.h"
#include "ui/Theme.h"
#include "ui/VlcPlayer.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

namespace rabbitears {
namespace {

// Startup diagnostics: when RABBITEARS_DEBUG is set, append a line to
// rabbitears_debug.log next to the exe. No-op by default.
void dbg(const char* msg) {
    if (!_wgetenv(L"RABBITEARS_DEBUG")) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    const size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) +
           L"rabbitears_debug.log";
    if (FILE* f = _wfopen(path.c_str(), L"a")) {
        fprintf(f, "%lu: %s\n", GetTickCount(), msg);
        fclose(f);
    }
}

constexpr wchar_t kMainClass[] = L"RabbitEarsMain";
constexpr wchar_t kVideoClass[] = L"ReVideoSurface";
constexpr UINT WM_APP_VLC = WM_APP + 1;

constexpr int ID_BTN_PLAY = 1001;
constexpr int ID_BTN_STOP = 1002;
constexpr int ID_VOL = 1003;
constexpr int ID_BTN_ABOUT = 1004;

// A reliable public HLS test stream, so B1 can validate playback without a
// populated playlist. (B1b plays real channels from the DB.)
constexpr wchar_t kTestStream[] = L"https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8";

struct AppState {
    VlcPlayer player;
    HWND      video = nullptr;
    HWND      btnPlay = nullptr;
    HWND      btnStop = nullptr;
    HWND      volBar = nullptr;
    HWND      status = nullptr;
    HWND      btnAbout = nullptr;
    HFONT     uiFont = nullptr;
    UINT      dpi = 96;
};

int dp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

AppState* stateOf(HWND h) { return reinterpret_cast<AppState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void setStatus(AppState* st, const std::wstring& s) {
    if (st->status) SetWindowTextW(st->status, s.c_str());
}

void applyDarkChrome(HWND hwnd) {
    const Theme& th = currentTheme();
    BOOL dark = th.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF cap = th.panelElevBg, txt = th.textSecondary, bdr = th.border;
    DwmSetWindowAttribute(hwnd, 35 /*CAPTION_COLOR*/, &cap, sizeof(cap));
    DwmSetWindowAttribute(hwnd, 36 /*TEXT_COLOR*/, &txt, sizeof(txt));
    DwmSetWindowAttribute(hwnd, 34 /*BORDER_COLOR*/, &bdr, sizeof(bdr));
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
}

void layout(HWND hwnd, AppState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int strip = dp(56, st->dpi);
    const int pad = dp(10, st->dpi);
    const int btnW = dp(90, st->dpi), btnH = dp(32, st->dpi);
    const int by = H - strip + (strip - btnH) / 2;

    // Video fills everything above the transport strip.
    MoveWindow(st->video, 0, 0, W, H - strip, TRUE);

    int x = pad;
    MoveWindow(st->btnPlay, x, by, btnW, btnH, TRUE); x += btnW + pad;
    MoveWindow(st->btnStop, x, by, btnW, btnH, TRUE); x += btnW + pad;
    const int volW = dp(160, st->dpi);
    MoveWindow(st->volBar, x, by, volW, btnH, TRUE); x += volW + pad * 2;
    // About pinned to the far right; status fills the gap between.
    const int aboutW = dp(80, st->dpi);
    MoveWindow(st->btnAbout, W - aboutW - pad, by, aboutW, btnH, TRUE);
    const int statusW = (W - aboutW - pad * 2) - x;
    MoveWindow(st->status, x, by, statusW > 0 ? statusW : 0, btnH, TRUE);
}

void createChildren(HWND hwnd, AppState* st) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    st->uiFont = CreateFontW(-dp(14, st->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    st->video = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, hwnd, nullptr, hInst, nullptr);
    st->btnPlay = CreateWindowExW(0, L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_PLAY)), hInst, nullptr);
    st->btnStop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_STOP)), hInst, nullptr);
    st->volBar = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                 0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VOL)), hInst, nullptr);
    st->status = CreateWindowExW(0, L"STATIC", L"Starting…", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                 0, 0, 10, 10, hwnd, nullptr, hInst, nullptr);
    st->btnAbout = CreateWindowExW(0, L"BUTTON", L"About", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   0, 0, 10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_ABOUT)), hInst, nullptr);

    SendMessageW(st->volBar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(st->volBar, TBM_SETPOS, TRUE, st->player.volume());

    for (HWND h : {st->btnPlay, st->btnStop, st->status, st->volBar, st->btnAbout}) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st->uiFont), TRUE);
        SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    }
}

// ---- About dialog (renders the embedded RabbitEars.png via GDI+) -----------

struct AboutState {
    Gdiplus::Image* img = nullptr;
    IStream*        stream = nullptr;  // must outlive img (GDI+ reads it lazily)
    HFONT           titleFont = nullptr;
    HFONT           bodyFont = nullptr;
    UINT            dpi = 96;
    bool            done = false;
};

// Load a PNG stored as an RCDATA resource into a GDI+ Image. The backing stream
// is returned via `outStream` and must be kept alive for the image's lifetime.
Gdiplus::Image* loadPngResource(HINSTANCE hInst, int id, IStream** outStream) {
    *outStream = nullptr;
    HRSRC res = FindResourceW(hInst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return nullptr;
    const DWORD size = SizeofResource(hInst, res);
    HGLOBAL data = LoadResource(hInst, res);
    if (!data || size == 0) return nullptr;
    const void* src = LockResource(data);
    HGLOBAL buf = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!buf) return nullptr;
    if (void* dst = GlobalLock(buf)) {
        memcpy(dst, src, size);
        GlobalUnlock(buf);
    }
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(buf, TRUE /*free HGLOBAL on release*/, &stream) != S_OK) {
        GlobalFree(buf);
        return nullptr;
    }
    Gdiplus::Image* img = Gdiplus::Image::FromStream(stream);
    if (!img || img->GetLastStatus() != Gdiplus::Ok) {
        delete img;
        stream->Release();
        return nullptr;
    }
    *outStream = stream;
    return img;
}

LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<AboutState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // fully painted in WM_PAINT (double-buffered)
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            const Theme& th = currentTheme();
            FillRect(mem, &rc, themeBrush(th.panelBg));

            const int m = dp(22, st->dpi);
            int imgW = 0, imgH = 0;
            if (st->img && st->img->GetWidth() && st->img->GetHeight()) {
                const int boxW = dp(150, st->dpi), boxH = dp(244, st->dpi);
                const UINT iw = st->img->GetWidth(), ih = st->img->GetHeight();
                const double s = std::min(double(boxW) / iw, double(boxH) / ih);
                imgW = static_cast<int>(iw * s);
                imgH = static_cast<int>(ih * s);
                Gdiplus::Graphics g(mem);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(st->img, m, m, imgW, imgH);
            }

            const int tx = m + (imgW > 0 ? imgW + dp(20, st->dpi) : 0);
            SetBkMode(mem, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(mem, st->titleFont);
            SetTextColor(mem, th.textPrimary);
            RECT tr{tx, m, rc.right - m, m + dp(36, st->dpi)};
            DrawTextW(mem, L"RabbitEars", -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);

            SelectObject(mem, st->bodyFont);
            SetTextColor(mem, th.textSecondary);
            RECT br{tx, tr.bottom + dp(6, st->dpi), rc.right - m, tr.bottom + dp(80, st->dpi)};
            DrawTextW(mem, L"A simple IPTV viewer for Windows.\r\nVersion 0.1.0", -1, &br,
                      DT_LEFT | DT_TOP | DT_WORDBREAK);

            SetTextColor(mem, th.textMuted);
            RECT ar{tx, rc.bottom - dp(64, st->dpi), rc.right - m, rc.bottom - dp(16, st->dpi)};
            DrawTextW(mem, L"Plays media with libVLC (LGPL-2.1)\r\n© VideoLAN and the VLC contributors.",
                      -1, &ar, DT_LEFT | DT_TOP | DT_WORDBREAK);

            SelectObject(mem, oldFont);
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                if (st) st->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            if (st) st->done = true;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Modal About box centered on `parent`. Runs its own message pump (no
// PostQuitMessage — that would kill the app's main loop, same thread).
void showAbout(HWND parent, HINSTANCE hInst, UINT dpi) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsAbout";
        RegisterClassExW(&wc);
        registered = true;
    }

    AboutState st;
    st.dpi = dpi;
    st.img = loadPngResource(hInst, IDR_ABOUT_PNG, &st.stream);
    auto mkFont = [&](int px, int weight) {
        return CreateFontW(-dp(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    };
    st.titleFont = mkFont(22, FW_SEMIBOLD);
    st.bodyFont = mkFont(14, FW_NORMAL);

    const int W = dp(470, dpi), H = dp(324, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsAbout", L"About RabbitEars",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (dlg) {
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
        const int bw = dp(90, dpi), bh = dp(30, dpi);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0,
                                  bw, bh, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                  hInst, nullptr);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(st.bodyFont), TRUE);
        RECT cr;
        GetClientRect(dlg, &cr);
        MoveWindow(ok, cr.right - bw - dp(20, dpi), cr.bottom - bh - dp(14, dpi), bw, bh, TRUE);
        applyDialogDarkMode(dlg);

        EnableWindow(parent, FALSE);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        MSG m;
        while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }

    delete st.img;
    if (st.stream) st.stream->Release();
    if (st.titleFont) DeleteObject(st.titleFont);
    if (st.bodyFont) DeleteObject(st.bodyFont);
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
            dbg("WM_CREATE begin");
            st->dpi = GetDpiForWindow(hwnd);
            applyDarkChrome(hwnd);
            const bool ready = st->player.init();
            dbg(ready ? "player.init OK" : "player.init FAILED");
            createChildren(hwnd, st);
            st->player.attach(st->video);
            st->player.setEventTarget(hwnd, WM_APP_VLC);
            layout(hwnd, st);
            if (!st->player.isReady()) {
                setStatus(st, L"libVLC runtime not available.");
            } else {
                setStatus(st, L"Loading test stream…");
                const bool ok = st->player.play(kTestStream);
                dbg(ok ? "play() started" : "play() returned false");
            }
            dbg("WM_CREATE end");
            return 0;
        }
        case WM_SIZE:
            layout(hwnd, st);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().panelElevBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BTN_PLAY: st->player.togglePause(); return 0;
                case ID_BTN_STOP:
                    st->player.stop();
                    setStatus(st, L"Stopped.");
                    return 0;
                case ID_BTN_ABOUT:
                    showAbout(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                              st->dpi);
                    return 0;
            }
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == st->volBar) {
                const int pos = static_cast<int>(SendMessageW(st->volBar, TBM_GETPOS, 0, 0));
                st->player.setVolume(pos);
            }
            return 0;
        case WM_APP_VLC: {
            switch (static_cast<PlayerEvent>(wParam)) {
                case PlayerEvent::Opening: setStatus(st, L"Opening…"); break;
                case PlayerEvent::Buffering:
                    setStatus(st, L"Buffering " + std::to_wstring(static_cast<int>(lParam)) + L"%");
                    break;
                case PlayerEvent::Playing: setStatus(st, L"Playing"); break;
                case PlayerEvent::Paused: setStatus(st, L"Paused"); break;
                case PlayerEvent::Stopped: setStatus(st, L"Stopped"); break;
                case PlayerEvent::EndReached: setStatus(st, L"Ended"); break;
                case PlayerEvent::Error: setStatus(st, L"Playback error"); break;
            }
            return 0;
        }
        case WM_SETTINGCHANGE:
            applyDarkChrome(hwnd);
            return 0;
        case WM_DESTROY:
            dbg("WM_DESTROY");
            st->player.stop();
            if (st->uiFont) DeleteObject(st->uiFont);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerClasses(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
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
    vc.style = CS_HREDRAW | CS_VREDRAW;
    vc.lpfnWndProc = VideoProc;
    vc.hInstance = hInst;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vc.lpszClassName = kVideoClass;
    RegisterClassExW(&vc);
}

}  // namespace

int runApp(HINSTANCE hInst, int nCmdShow) {
    dbg("runApp begin");
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    registerClasses(hInst);

    auto* st = new AppState();
    HWND hwnd = CreateWindowExW(0, kMainClass, L"RabbitEars", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720, nullptr, nullptr, hInst, st);
    if (!hwnd) {
        char buf[64];
        sprintf(buf, "CreateWindow FAILED err=%lu", GetLastError());
        dbg(buf);
        delete st;
        return 1;
    }
    dbg("window created; entering loop");
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    dbg("loop exited");
    delete st;
    Gdiplus::GdiplusShutdown(gdipToken);
    return static_cast<int>(m.wParam);
}

}  // namespace rabbitears
