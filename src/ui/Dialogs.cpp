// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Dialogs.h"

#include <algorithm>
#include <cstring>

#include <objbase.h>  // CreateStreamOnHGlobal (ole32)
#include <objidl.h>   // IStream — required by gdiplus.h below
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "platform/Updater.h"
#include "resource.h"
#include "ui/Theme.h"
#include "version.h"

#pragma comment(lib, "gdiplus.lib")

namespace rabbitears {
namespace {

int dp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

constexpr int ID_CHECK_UPDATES = 1201;

// ---- About box (renders the embedded RabbitEars.png via GDI+) --------------

struct AboutState {
    Gdiplus::Image* img = nullptr;
    IStream*        stream = nullptr;  // must outlive img (GDI+ reads it lazily)
    HFONT           titleFont = nullptr;
    HFONT           bodyFont = nullptr;
    UINT            dpi = 96;
    bool            done = false;
};

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
    if (CreateStreamOnHGlobal(buf, TRUE, &stream) != S_OK) {
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
            return 1;
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
            int imgW = 0;
            if (st->img && st->img->GetWidth() && st->img->GetHeight()) {
                const int boxW = dp(150, st->dpi), boxH = dp(244, st->dpi);
                const UINT iw = st->img->GetWidth(), ih = st->img->GetHeight();
                const double s = std::min(double(boxW) / iw, double(boxH) / ih);
                imgW = static_cast<int>(iw * s);
                Gdiplus::Graphics g(mem);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(st->img, m, m, imgW, static_cast<int>(ih * s));
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
            DrawTextW(mem, L"A simple IPTV viewer for Windows.\r\nVersion " RE_VERSION_DISPLAY_W, -1,
                      &br, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SetTextColor(mem, th.textMuted);
            RECT ar{tx, rc.bottom - dp(100, st->dpi), rc.right - m, rc.bottom - dp(58, st->dpi)};
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
            if (LOWORD(wParam) == ID_CHECK_UPDATES) {
                checkForUpdates();
            } else if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
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

// ---- single-line text prompt ------------------------------------------------

struct PromptState {
    HWND         edit = nullptr;
    HFONT        font = nullptr;
    std::wstring label;
    std::wstring result;  // edit text, captured on OK before the window is destroyed
    UINT         dpi = 96;
    bool         ok = false;
    bool         done = false;
};

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ of = SelectObject(dc, st->font);
            SetTextColor(dc, currentTheme().textPrimary);
            RECT lr{dp(18, st->dpi), dp(14, st->dpi), 100000, dp(40, st->dpi)};
            DrawTextW(dc, st->label.c_str(), -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE);
            SelectObject(dc, of);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                // Read the edit text NOW — the window (and its edit) is about to die.
                wchar_t buf[4096] = L"";
                GetWindowTextW(st->edit, buf, ARRAYSIZE(buf));
                st->result = buf;
                st->ok = true;
                st->done = true;
                DestroyWindow(hwnd);
            } else if (LOWORD(wParam) == IDCANCEL) {
                st->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void showAbout(HWND parent, HINSTANCE hInst, UINT dpi) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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

    const int W = dp(470, dpi), H = dp(348, dpi);
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
        RECT cr;
        GetClientRect(dlg, &cr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                  cr.right - bw - dp(20, dpi), cr.bottom - bh - dp(14, dpi), bw, bh,
                                  dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), hInst, nullptr);
        const int uw = dp(140, dpi);
        HWND upd = CreateWindowExW(0, L"BUTTON", L"Check for Updates…", WS_CHILD | WS_VISIBLE,
                                   dp(20, dpi), cr.bottom - bh - dp(14, dpi), uw, bh, dlg,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHECK_UPDATES)),
                                   hInst, nullptr);
        SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(st.bodyFont), TRUE);
        SendMessageW(upd, WM_SETFONT, reinterpret_cast<WPARAM>(st.bodyFont), TRUE);
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

bool promptText(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                const std::wstring& label, std::wstring& value) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsPrompt";
        RegisterClassExW(&wc);
        registered = true;
    }
    PromptState st;
    st.dpi = dpi;
    st.label = label;
    st.font = CreateFontW(-dp(14, dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    const int W = dp(520, dpi), H = dp(180, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsPrompt", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (!dlg) {
        DeleteObject(st.font);
        return false;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, dp(18, dpi),
                              dp(48, dpi), cr.right - dp(36, dpi), dp(28, dpi), dlg, nullptr, hInst,
                              nullptr);
    const int bw = dp(90, dpi), bh = dp(30, dpi);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cr.right - 2 * bw - dp(28, dpi), cr.bottom - bh - dp(16, dpi), bw, bh,
                              dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), hInst, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  cr.right - bw - dp(18, dpi), cr.bottom - bh - dp(16, dpi), bw, bh,
                                  dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst,
                                  nullptr);
    for (HWND h : {st.edit, ok, cancel}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    applyDialogDarkMode(dlg);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (st.ok) value = st.result;  // captured in PromptProc before the window was destroyed
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(st.font);
    return st.ok;
}

}  // namespace rabbitears
