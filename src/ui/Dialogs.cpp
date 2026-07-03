// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Dialogs.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

#include <commctrl.h>  // ListView (the categories checklist)
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
#pragma comment(lib, "comctl32.lib")

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
            RECT ar{tx, br.bottom + dp(10, st->dpi), rc.right - m, br.bottom + dp(58, st->dpi)};
            DrawTextW(mem, L"Plays media with libVLC (LGPL-2.1)\r\n© VideoLAN and the VLC contributors.",
                      -1, &ar, DT_LEFT | DT_TOP | DT_WORDBREAK);
            // Full-width legal disclaimer below the artwork, above the buttons.
            const int discTop = m + dp(250, st->dpi);
            RECT sep{m, discTop - dp(12, st->dpi), rc.right - m, discTop - dp(11, st->dpi)};
            FillRect(mem, &sep, themeBrush(th.border));
            RECT dr{m, discTop, rc.right - m, discTop + dp(96, st->dpi)};
            SetTextColor(mem, th.textSecondary);
            DrawTextW(mem,
                      L"Rabbit Ears is provided only for educational purposes, and does not "
                      L"represent supporting any illegal activity that you do with it. "
                      L"We don't know, we don't care.",
                      -1, &dr, DT_LEFT | DT_TOP | DT_WORDBREAK);
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

// ---- categories checklist ---------------------------------------------------

constexpr int ID_CAT_LV = 1301;
constexpr int ID_CAT_FILTER = 1302;
constexpr int ID_CAT_ALL = 1303;
constexpr int ID_CAT_NONE = 1304;

struct CategoriesState {
    HWND  lv = nullptr;      // checkbox list of group titles
    HWND  filter = nullptr;  // "Filter categories…" search box
    HWND  count = nullptr;   // "N of M categories selected"
    HFONT font = nullptr;
    UINT  dpi = 96;
    // The model — every group + its checked state. The ListView is a filtered
    // view of this; each visible row's lParam is an index back into it.
    std::vector<std::pair<std::wstring, bool>> items;
    bool  syncing = false;  // ignore LVN_ITEMCHANGED while we repopulate the list
    bool  ok = false;
    bool  done = false;
};

std::wstring lowerCopy(std::wstring s) {
    for (wchar_t& c : s) c = towlower(c);
    return s;
}

void catUpdateCount(CategoriesState* st) {
    int on = 0;
    for (const auto& it : st->items)
        if (it.second) ++on;
    wchar_t b[64];
    swprintf_s(b, L"%d of %d categories selected", on, static_cast<int>(st->items.size()));
    SetWindowTextW(st->count, b);
}

// Rebuild the ListView from the model, filtered by the current search text.
void catRepopulate(CategoriesState* st) {
    wchar_t term[128] = L"";
    GetWindowTextW(st->filter, term, ARRAYSIZE(term));
    const std::wstring needle = lowerCopy(term);

    st->syncing = true;
    SendMessageW(st->lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st->lv);
    int row = 0;
    for (int i = 0; i < static_cast<int>(st->items.size()); ++i) {
        if (!needle.empty() && lowerCopy(st->items[i].first).find(needle) == std::wstring::npos)
            continue;
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.pszText = const_cast<LPWSTR>(st->items[i].first.c_str());
        it.lParam = i;  // model index
        const int idx = ListView_InsertItem(st->lv, &it);
        ListView_SetCheckState(st->lv, idx, st->items[i].second ? TRUE : FALSE);
        ++row;
    }
    SendMessageW(st->lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->lv, nullptr, TRUE);  // redraw was off during the rebuild; force a repaint
    st->syncing = false;
}

void catSetAll(CategoriesState* st, bool on) {
    for (auto& it : st->items) it.second = on;
    catRepopulate(st);
    catUpdateCount(st);
}

LRESULT CALLBACK CategoriesProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<CategoriesState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            RECT lr{dp(18, st->dpi), dp(14, st->dpi), 100000, dp(38, st->dpi)};
            DrawTextW(dc, L"Show channels only from the checked categories:", -1, &lr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE);
            SelectObject(dc, of);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = reinterpret_cast<LPNMHDR>(lParam);
            if (nh->idFrom == ID_CAT_LV && nh->code == LVN_ITEMCHANGED && !st->syncing) {
                auto* nv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if (nv->uChanged & LVIF_STATE) {
                    // The 1-based state-image index encodes the checkbox: 1 = unchecked,
                    // 2 = checked. A genuine user toggle moves between the two.
                    const UINT wasImg = (nv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                    const UINT nowImg = (nv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                    if (nowImg != wasImg && (nowImg == 1 || nowImg == 2)) {
                        const int mi = static_cast<int>(nv->lParam);
                        if (mi >= 0 && mi < static_cast<int>(st->items.size())) {
                            st->items[mi].second = (nowImg == 2);
                            catUpdateCount(st);
                        }
                    }
                }
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_CAT_FILTER:
                    if (HIWORD(wParam) == EN_CHANGE) catRepopulate(st);
                    return 0;
                case ID_CAT_ALL:
                    catSetAll(st, true);
                    return 0;
                case ID_CAT_NONE:
                    catSetAll(st, false);
                    return 0;
                case IDOK:
                    st->ok = true;
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                case IDCANCEL:
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
            }
            return 0;
        case WM_CLOSE:
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- first-run Terms of Use -------------------------------------------------

constexpr int ID_TERMS_ACCEPT = 1401;

constexpr wchar_t kTermsText[] =
    L"Please read these terms before using RabbitEars. By choosing “I Accept” you "
    L"agree to them. If you do not agree, choose “Decline” and the application will "
    L"close.\r\n\r\n"
    L"1.  Educational purpose.  RabbitEars is a media player provided for educational and "
    L"personal use only. It is offered “as is”, without warranty of any kind, and you "
    L"use it entirely at your own risk.\r\n\r\n"
    L"2.  No content is included.  RabbitEars ships with no channels, playlists, or media of "
    L"any kind. It plays only the playlists that you choose to add. You are solely responsible "
    L"for obtaining your playlists from lawful sources and for ensuring that your use complies "
    L"with all applicable laws and the rights of content owners in your jurisdiction.\r\n\r\n"
    L"3.  No endorsement.  The authors of RabbitEars do not provide, host, recommend, or "
    L"endorse any stream or content, and have no knowledge of or control over what you choose "
    L"to play. As the project puts it: we don’t know, and we don’t care.\r\n\r\n"
    L"4.  Your responsibility.  Any illegal activity carried out with this software is yours "
    L"alone and is not supported, encouraged, or condoned by the authors.\r\n\r\n"
    L"5.  Open source.  RabbitEars plays media using libVLC, © VideoLAN and the VLC "
    L"contributors, under the GNU LGPL v2.1.\r\n\r\n"
    L"By clicking “I Accept”, you confirm that you have read, understood, and agree to "
    L"these terms.";

struct TermsState {
    HWND  edit = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    bool  accepted = false;
    bool  done = false;
};

LRESULT CALLBACK TermsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<TermsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TERMS_ACCEPT) {
                st->accepted = true;
                st->done = true;
                DestroyWindow(hwnd);
            } else if (LOWORD(wParam) == IDCANCEL) {
                st->accepted = false;
                st->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CLOSE:
            st->accepted = false;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

constexpr int ID_IMPORT_OK = 1501;

struct ImportResultsState {
    bool done = false;
};

LRESULT CALLBACK ImportResultsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<ImportResultsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_IMPORT_OK || LOWORD(wParam) == IDOK ||
                LOWORD(wParam) == IDCANCEL) {
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

    const int W = dp(470, dpi), H = dp(470, dpi);
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

bool chooseCategories(HWND parent, HINSTANCE hInst, UINT dpi,
                      const std::vector<std::wstring>& allGroups,
                      std::set<std::wstring>& checked) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CategoriesProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsCategories";
        RegisterClassExW(&wc);
        registered = true;
    }
    CategoriesState st;
    st.dpi = dpi;
    st.items.reserve(allGroups.size());
    for (const std::wstring& g : allGroups) st.items.emplace_back(g, checked.count(g) != 0);
    st.font = CreateFontW(-dp(14, dpi), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

    const int W = dp(460, dpi), H = dp(560, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsCategories", L"Categories",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (!dlg) {
        DeleteObject(st.font);
        return false;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(18, dpi);
    const int bw = dp(90, dpi), bh = dp(30, dpi);
    const int btnY = cr.bottom - bh - dp(14, dpi);
    const int countY = btnY - dp(26, dpi);
    const int listTop = dp(80, dpi);
    const int listBottom = countY - dp(8, dpi);

    st.filter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, m, dp(44, dpi),
                                cr.right - 2 * m, dp(26, dpi), dlg,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CAT_FILTER)), hInst,
                                nullptr);
    SendMessageW(st.filter, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Filter categories…"));

    st.lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                            m, listTop, cr.right - 2 * m, listBottom - listTop, dlg,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CAT_LV)), hInst, nullptr);
    ListView_SetExtendedListViewStyle(
        st.lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    RECT lvc;
    GetClientRect(st.lv, &lvc);
    col.cx = lvc.right - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - dp(6, dpi);
    ListView_InsertColumn(st.lv, 0, &col);

    st.count = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, m,
                               countY, cr.right - 2 * m, dp(20, dpi), dlg, nullptr, hInst, nullptr);

    HWND all = CreateWindowExW(0, L"BUTTON", L"Select All", WS_CHILD | WS_VISIBLE | WS_TABSTOP, m,
                               btnY, dp(96, dpi), bh, dlg,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CAT_ALL)), hInst,
                               nullptr);
    HWND none = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                m + dp(96, dpi) + dp(8, dpi), btnY, dp(74, dpi), bh, dlg,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CAT_NONE)), hInst,
                                nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cr.right - 2 * bw - dp(28, dpi), btnY, bw, bh, dlg,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), hInst, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  cr.right - bw - dp(18, dpi), btnY, bw, bh, dlg,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst,
                                  nullptr);

    for (HWND h : {st.filter, st.lv, st.count, all, none, ok, cancel})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);

    applyDialogDarkMode(dlg);
    // Dark-mode the ListView surface itself (theme handles scrollbars + checkboxes).
    const Theme& th = currentTheme();
    ListView_SetBkColor(st.lv, th.windowBg);
    ListView_SetTextBkColor(st.lv, th.windowBg);
    ListView_SetTextColor(st.lv, th.textPrimary);

    catRepopulate(&st);
    catUpdateCount(&st);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.filter);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (st.ok) {  // the model is the source of truth — no read-before-destroy needed
        checked.clear();
        for (const auto& it : st.items)
            if (it.second) checked.insert(it.first);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(st.font);
    return st.ok;
}

bool showTerms(HWND parent, HINSTANCE hInst, UINT dpi) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TermsProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsTerms";
        RegisterClassExW(&wc);
        registered = true;
    }
    TermsState st;
    auto mkFont = [&](int px, int weight) {
        return CreateFontW(-dp(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    };
    st.font = mkFont(14, FW_NORMAL);
    st.titleFont = mkFont(20, FW_SEMIBOLD);

    const int W = dp(560, dpi), H = dp(500, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsTerms", L"RabbitEars — Terms of Use",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (!dlg) {
        DeleteObject(st.font);
        DeleteObject(st.titleFont);
        return false;  // fail open would be worse; a null dialog is treated as declined
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(20, dpi), bw = dp(110, dpi), bh = dp(30, dpi);
    const int btnY = cr.bottom - bh - dp(16, dpi);

    HWND head = CreateWindowExW(0, L"STATIC", L"Terms of Use", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                m, dp(16, dpi), cr.right - 2 * m, dp(28, dpi), dlg, nullptr, hInst,
                                nullptr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", kTermsText,
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE |
                                  ES_READONLY | ES_AUTOVSCROLL,
                              m, dp(52, dpi), cr.right - 2 * m, btnY - dp(66, dpi), dlg, nullptr,
                              hInst, nullptr);
    HWND accept = CreateWindowExW(0, L"BUTTON", L"I Accept",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  cr.right - bw - m, btnY, bw, bh, dlg,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TERMS_ACCEPT)),
                                  hInst, nullptr);
    HWND decline = CreateWindowExW(0, L"BUTTON", L"Decline", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   cr.right - 2 * bw - m - dp(8, dpi), btnY, bw, bh, dlg,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst,
                                   nullptr);
    SendMessageW(head, WM_SETFONT, reinterpret_cast<WPARAM>(st.titleFont), TRUE);
    for (HWND h : {st.edit, accept, decline})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    applyDialogDarkMode(dlg);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(accept);
    MSG m2;
    while (!st.done && GetMessageW(&m2, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &m2)) {
            TranslateMessage(&m2);
            DispatchMessageW(&m2);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(st.font);
    DeleteObject(st.titleFont);
    return st.accepted;
}

// Generic themed modal (title + bold summary + scrollable details). Backed by the
// ImportResults* class/proc above, kept as-is since the widget itself is generic.
void showInfoDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                    const std::wstring& summary, const std::wstring& details) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ImportResultsProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsImportResults";
        RegisterClassExW(&wc);
        registered = true;
    }
    ImportResultsState st;
    auto mkFont = [&](int px, int weight) {
        return CreateFontW(-dp(px, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    };
    HFONT bodyFont = mkFont(11, FW_NORMAL);
    HFONT headFont = mkFont(15, FW_SEMIBOLD);

    const int W = dp(520, dpi), H = dp(340, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsImportResults", title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (!dlg) {
        DeleteObject(bodyFont);
        DeleteObject(headFont);
        return;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(20, dpi), bw = dp(110, dpi), bh = dp(30, dpi);
    const int btnY = cr.bottom - bh - dp(16, dpi);

    HWND head = CreateWindowExW(0, L"STATIC", summary.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, m, dp(16, dpi),
                                cr.right - 2 * m, dp(24, dpi), dlg, nullptr, hInst, nullptr);
    HWND body = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", details.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE |
                                    ES_READONLY | ES_AUTOVSCROLL,
                                m, dp(48, dpi), cr.right - 2 * m, btnY - dp(60, dpi), dlg, nullptr,
                                hInst, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cr.right - bw - m, btnY, bw, bh, dlg,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_IMPORT_OK)), hInst,
                              nullptr);
    SendMessageW(head, WM_SETFONT, reinterpret_cast<WPARAM>(headFont), TRUE);
    for (HWND h : {body, ok})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
    applyDialogDarkMode(dlg);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(ok);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(bodyFont);
    DeleteObject(headFont);
}

}  // namespace rabbitears
