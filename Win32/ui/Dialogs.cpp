// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Dialogs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

#include <commctrl.h>  // ListView (the categories checklist)
#include <commdlg.h>   // ChooseColor (the swatch colour picker)
#include <shellapi.h>  // ShellExecuteW (open the GitHub link in the About box)
#include <objbase.h>  // CreateStreamOnHGlobal (ole32)
#include <objidl.h>   // IStream — required by gdiplus.h below
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "platform/Updater.h"
#include "resource.h"
#include "ui/BufferMeter.h"  // the Data-flow row's preview (createBufferMeter / bufferMeterSet*)
#include "ui/MiniMeter.h"
#include "ui/Theme.h"
#include "version.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace rabbitears {
namespace {

int dp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

// Keep a centred W×H dialog fully on-screen: clamp its top-left to the work area of the
// monitor the parent is on. Dialogs centre on the parent window, which pushes a tall
// dialog off the top edge when the main window sits near the top of the screen.
void clampToWorkArea(HWND parent, int W, int H, int& x, int& y) {
    RECT wa;
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi))
        wa = mi.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (x + W > wa.right) x = static_cast<int>(wa.right) - W;
    if (y + H > wa.bottom) y = static_cast<int>(wa.bottom) - H;
    if (x < wa.left) x = static_cast<int>(wa.left);  // left/top win if the dialog is
    if (y < wa.top) y = static_cast<int>(wa.top);    // larger than the work area
}

constexpr int ID_CHECK_UPDATES = 1201;

// The project's GitHub — shown as a clickable link in the About box.
constexpr wchar_t kGithubUrl[] = L"https://github.com/arcanii/RabbitEars";
constexpr wchar_t kGithubLabel[] = L"github.com/arcanii/RabbitEars";

// ---- About box (renders the embedded RabbitEars.png via GDI+) --------------

struct AboutState {
    Gdiplus::Image* img = nullptr;
    IStream*        stream = nullptr;  // must outlive img (GDI+ reads it lazily)
    Gdiplus::Image* altImg = nullptr;    // easter-egg artwork, lazy-loaded on the first bunny click
    IStream*        altStream = nullptr;
    bool            showAlt = false;     // click the artwork to toggle to BadAss_RabbitEars
    RECT            imgRect = {};         // where the artwork was last drawn (click hit-test)
    RECT            linkRect = {};        // the GitHub link's drawn rect (click hit-test + hand cursor)
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
            // Click the bunny to toggle the easter-egg artwork (BadAss_RabbitEars).
            Gdiplus::Image* cur = (st->showAlt && st->altImg) ? st->altImg : st->img;
            if (cur && cur->GetWidth() && cur->GetHeight()) {
                const int boxW = dp(188, st->dpi), boxH = dp(305, st->dpi);  // +25% over the original 150x244
                const UINT iw = cur->GetWidth(), ih = cur->GetHeight();
                const double s = std::min(double(boxW) / iw, double(boxH) / ih);
                imgW = static_cast<int>(iw * s);
                const int imgH = static_cast<int>(ih * s);
                Gdiplus::Graphics g(mem);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(cur, m, m, imgW, imgH);
                st->imgRect = {m, m, m + imgW, m + imgH};  // remember for click hit-testing
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
            // Clickable link to the project on GitHub: accent-coloured + underlined, opened in
            // WM_LBUTTONDOWN; a hand cursor shows over it (WM_SETCURSOR). linkRect is the hit-test.
            SetTextColor(mem, th.accent);
            SIZE lsz{};
            GetTextExtentPoint32W(mem, kGithubLabel, lstrlenW(kGithubLabel), &lsz);
            const int ly = ar.bottom + dp(8, st->dpi);
            st->linkRect = {tx, ly, tx + lsz.cx, ly + lsz.cy};
            DrawTextW(mem, kGithubLabel, -1, &st->linkRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
            RECT ul{tx, ly + lsz.cy, tx + lsz.cx, ly + lsz.cy + dp(1, st->dpi)};
            FillRect(mem, &ul, themeBrush(th.accent));
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
        case WM_SETCURSOR:
            if (st && LOWORD(lParam) == HTCLIENT) {
                POINT cp;
                GetCursorPos(&cp);
                ScreenToClient(hwnd, &cp);
                if (PtInRect(&st->linkRect, cp)) {  // hand cursor over the GitHub link
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        case WM_LBUTTONDOWN: {  // click the artwork to toggle the easter-egg image
            if (!st) break;
            const POINT pt{static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                           static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
            if (PtInRect(&st->linkRect, pt)) {  // open the project on GitHub in the browser
                ShellExecuteW(hwnd, L"open", kGithubUrl, nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (PtInRect(&st->imgRect, pt)) {
                if (!st->altImg) {  // lazy-load the easter egg on first click
                    HINSTANCE hi = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
                    st->altImg = loadPngResource(hi, IDR_ABOUT_ALT_PNG, &st->altStream);
                }
                if (st->altImg) {  // only toggle if the alt art actually loaded
                    st->showAlt = !st->showAlt;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
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
    st.titleFont = themeFont(FontRole::Title, dpi, 22, FW_SEMIBOLD);
    st.bodyFont = themeFont(FontRole::Body, dpi, 14, FW_NORMAL);

    // Wide enough that the right-column text clears the +25% artwork without wrapping: the
    // longest line ("© VideoLAN and the VLC contributors.") is ~239px, and tx≈230 + m=22, so
    // the client must reach ~500px — dp(530) leaves a comfortable margin.
    const int W = dp(530, dpi), H = dp(470, dpi);
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
        while (!st.done) {
            const BOOL r = GetMessageW(&m, nullptr, 0, 0);
            if (r == 0) {  // WM_QUIT — WinSparkle asked us to close to install an update.
                // Re-post it so runApp's OUTER loop also sees it and the process actually exits;
                // otherwise this nested loop swallows the quit and the app lingers, locking the
                // exe so the installer fails. (This About box is the only path to "Check for
                // Updates", so it is always open during a user-triggered update.)
                PostQuitMessage(static_cast<int>(m.wParam));
                break;
            }
            if (r == -1) break;  // GetMessage error
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
    delete st.altImg;
    if (st.altStream) st.altStream->Release();
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
    st.font = themeFont(FontRole::Body, dpi, 14, FW_NORMAL);

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
    st.font = themeFont(FontRole::Body, dpi, 14, FW_NORMAL);

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
    st.font = themeFont(FontRole::Body, dpi, 14, FW_NORMAL);
    st.titleFont = themeFont(FontRole::Title, dpi, 20, FW_SEMIBOLD);

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
    HFONT bodyFont = themeFont(FontRole::Body, dpi, 11, FW_NORMAL);
    HFONT headFont = themeFont(FontRole::Body, dpi, 15, FW_SEMIBOLD);

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
    while (!st.done) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) {  // WM_QUIT (the app was closed under this modal — e.g. from the TV
            // guide, whose owned window is destroyed with the main window). Re-post so
            // runApp's OUTER loop also exits, instead of the 4s watchdog force-killing it.
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (r == -1) break;  // GetMessage error
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

// ---- Loading box (modeless "please wait" for async guide fetch) ------------
namespace {
constexpr int ID_LOADING_MSG = 1901;
struct LoadingState {  // owned by the window; freed on WM_NCDESTROY
    HFONT headFont = nullptr;
    HFONT bodyFont = nullptr;
};

LRESULT CALLBACK LoadingProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            return dialogCtlColor(msg, wParam);
        case WM_CLOSE:
            return 0;  // modeless progress box: closed programmatically when the work finishes
        case WM_NCDESTROY: {
            auto* ls = reinterpret_cast<LoadingState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (ls) {
                if (ls->headFont) DeleteObject(ls->headFont);
                if (ls->bodyFont) DeleteObject(ls->bodyFont);
                delete ls;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
}  // namespace

HWND showLoadingDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                       const std::wstring& message) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = LoadingProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);  // arrow + busy spinner
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsLoading";
        RegisterClassExW(&wc);
        registered = true;
    }
    const int W = dp(430, dpi), H = dp(150, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    // Topmost so it stays visible over the (large, non-topmost) TV Guide window during a fetch.
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"RabbitEarsLoading",
                               title.empty() ? L"Loading" : title.c_str(), WS_POPUP | WS_CAPTION, x, y,
                               W, H, parent, nullptr, hInst, nullptr);
    if (!dlg) return nullptr;
    auto* ls = new LoadingState();
    ls->headFont = themeFont(FontRole::Body, dpi, 15, FW_SEMIBOLD);
    ls->bodyFont = themeFont(FontRole::Body, dpi, 11, FW_NORMAL);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ls));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(24, dpi);
    HWND head = CreateWindowExW(0, L"STATIC", L"Loading TV guide…",
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, m, dp(22, dpi),
                                cr.right - 2 * m, dp(24, dpi), dlg, nullptr, hInst, nullptr);
    HWND body = CreateWindowExW(0, L"STATIC", message.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, m, dp(58, dpi),
                                cr.right - 2 * m, dp(48, dpi), dlg,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LOADING_MSG)), hInst,
                                nullptr);
    SendMessageW(head, WM_SETFONT, reinterpret_cast<WPARAM>(ls->headFont), TRUE);
    SendMessageW(body, WM_SETFONT, reinterpret_cast<WPARAM>(ls->bodyFont), TRUE);
    applyDialogDarkMode(dlg);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    return dlg;
}

void updateLoadingDialog(HWND dlg, const std::wstring& message) {
    if (dlg && IsWindow(dlg)) SetWindowTextW(GetDlgItem(dlg, ID_LOADING_MSG), message.c_str());
}

void closeLoadingDialog(HWND dlg) {
    if (dlg && IsWindow(dlg)) DestroyWindow(dlg);
}

// ---- Programme popup (click a TV Guide entry) ------------------------------

namespace {
constexpr int ID_PROG_PLAY = 1801, ID_PROG_SCHED = 1802;

struct ProgrammeDlgState {
    ProgrammeAction action = ProgrammeAction::None;  // set in the Proc before destroy
    bool            done = false;
};

LRESULT CALLBACK ProgrammeDlgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<ProgrammeDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, w);
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_PROG_PLAY: st->action = ProgrammeAction::Play; st->done = true; DestroyWindow(hwnd); return 0;
                case ID_PROG_SCHED: st->action = ProgrammeAction::Schedule; st->done = true; DestroyWindow(hwnd); return 0;
                case IDCANCEL: st->done = true; DestroyWindow(hwnd); return 0;
            }
            return 0;
        case WM_CLOSE: st->done = true; DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
}  // namespace

ProgrammeAction programmeDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                                const std::wstring& info) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ProgrammeDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsProgramme";
        RegisterClassExW(&wc);
        registered = true;
    }
    ProgrammeDlgState st;
    HFONT bodyFont = themeFont(FontRole::Body, dpi, 11, FW_NORMAL);
    HFONT headFont = themeFont(FontRole::Body, dpi, 15, FW_SEMIBOLD);
    const int W = dp(460, dpi), H = dp(300, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2, y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsProgramme",
                               title.empty() ? L"Programme" : title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (!dlg) {
        DeleteObject(bodyFont);
        DeleteObject(headFont);
        return ProgrammeAction::None;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(20, dpi), bh = dp(30, dpi), btnY = cr.bottom - bh - dp(16, dpi);

    HWND head = CreateWindowExW(0, L"STATIC", title.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                m, dp(16, dpi), cr.right - 2 * m, dp(24, dpi), dlg, nullptr, hInst,
                                nullptr);
    HWND body = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", info.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE |
                                    ES_READONLY | ES_AUTOVSCROLL,
                                m, dp(48, dpi), cr.right - 2 * m, btnY - dp(60, dpi), dlg, nullptr, hInst,
                                nullptr);
    const int pw = dp(116, dpi), sw = dp(104, dpi), cw = dp(84, dpi), gap = dp(8, dpi);
    HWND play = CreateWindowExW(0, L"BUTTON", L"Play channel",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, m, btnY, pw, bh,
                                dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROG_PLAY)), hInst,
                                nullptr);
    HWND sched = CreateWindowExW(0, L"BUTTON", L"Schedule…", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 m + pw + gap, btnY, sw, bh, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROG_SCHED)), hInst,
                                 nullptr);
    HWND close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 cr.right - cw - m, btnY, cw, bh, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst, nullptr);
    SendMessageW(head, WM_SETFONT, reinterpret_cast<WPARAM>(headFont), TRUE);
    for (HWND h : {body, play, sched, close})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
    applyDialogDarkMode(dlg);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(play);
    MSG msg;
    while (!st.done) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); DestroyWindow(dlg); break; }
        if (r == -1) { DestroyWindow(dlg); break; }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(bodyFont);
    DeleteObject(headFont);
    return st.action;
}

// ---- Meters… setup dialog (Settings → Meters…) -----------------------------
namespace {

constexpr int  ID_MTR_ROW = 1600;   // per row r: enable=+r*16, combo +1, preview +2, swatch j +3+j, slider j +10+j
constexpr int  ID_MTR_RESET = 1596;
constexpr UINT kMtrPreviewTimer = 7;
constexpr UINT kMtrPreviewMs = 60;
constexpr int  kMtrRoles = 7;
constexpr int  kMtrKnobs = 4;   // max "feel" sliders per row

// Which tuning knobs each meter row exposes (label + field index into MeterTuning:
// 0=glow,1=smoothing,2=sensitivity,3=peakHold,4=breathing; field -1 = unused slot).
struct KnobDesc { const wchar_t* label; int field; };
const KnobDesc kMtrKnobDesc[4][kMtrKnobs] = {
    {{L"Glow", 0}, {L"Smooth", 1}, {L"Sens", 2}, {L"Peak", 3}},   // Spectrum
    {{L"Glow", 0}, {L"Smooth", 1}, {L"Sens", 2}, {nullptr, -1}},  // Signal
    {{L"Glow", 0}, {L"Sens", 2}, {L"Breathe", 4}, {nullptr, -1}}, // Bitrate
    {{L"Glow", 0}, {L"Smooth", 1}, {L"Sens", 2}, {nullptr, -1}},  // Frames
};
float knobGet(const MeterTuning& t, int f) {
    switch (f) {
        case 0: return t.glow;
        case 1: return t.smoothing;
        case 2: return t.sensitivity;
        case 3: return t.peakHold;
        case 4: return t.breathing;
    }
    return 0.5f;
}
void knobSet(MeterTuning& t, int f, float v) {
    switch (f) {
        case 0: t.glow = v; break;
        case 1: t.smoothing = v; break;
        case 2: t.sensitivity = v; break;
        case 3: t.peakHold = v; break;
        case 4: t.breathing = v; break;
    }
}

struct MetersDlgState {
    MeterConfig cfg[4];
    HWND  preview[4] = {};
    HWND  enable[4] = {};
    HWND  combo[4] = {};
    HWND  swatch[4][kMtrRoles] = {};
    HWND  slider[4][kMtrKnobs] = {};
    HWND  bufPreview = nullptr;  // the "Data flow" (buffer/fluid) meter preview — no Look/palette
    HWND  bufEnable = nullptr;
    bool  bufOn = true;          // working copy of the data-flow meter's visible state
    HFONT font = nullptr, bold = nullptr;
    UINT  dpi = 96;
    UINT  feedTick = 0;
    bool  ok = false, done = false;
};

// Resolve a palette role index (0..6) to a displayable colour (bg follows the theme).
COLORREF meterRoleColor(const MeterPalette& p, int j) {
    switch (j) {
        case 0: return (p.bg == CLR_INVALID) ? currentTheme().windowBg : p.bg;
        case 1: return p.off;
        case 2: return p.low;
        case 3: return p.mid;
        case 4: return p.high;
        case 5: return p.accent;
        case 6: return p.peak;
    }
    return p.off;
}
void meterSetRole(MeterPalette& p, int j, COLORREF c) {
    switch (j) {
        case 0: p.bg = c; break;
        case 1: p.off = c; break;
        case 2: p.low = c; break;
        case 3: p.mid = c; break;
        case 4: p.high = c; break;
        case 5: p.accent = c; break;
        case 6: p.peak = c; break;
    }
}

// Drive the four preview meters with synthetic data (never touches the SpectrumTap).
void meterFeedPreviews(MetersDlgState* st) {
    const float t = static_cast<float>(st->feedTick++) * 0.06f;
    float bands[24];
    for (int i = 0; i < 24; ++i) {
        const float v = 0.5f + 0.42f * std::sin(t + i * 0.5f) + 0.14f * std::sin(t * 2.3f + i);
        bands[i] = std::clamp(v, 0.0f, 1.0f);
    }
    miniMeterPushSpectrum(st->preview[0], bands, 24);
    miniMeterSetSignal(st->preview[1], std::clamp(0.6f + 0.4f * std::sin(t * 0.7f), 0.0f, 1.0f),
                       (std::sin(t * 0.31f) > 0.8f) ? 0.7f : 0.0f);
    miniMeterPushBitrate(st->preview[2],
                         3.0e6 * (0.5 + 0.5 * std::sin(t * 1.3) + 0.15 * std::sin(t * 11.0)));
    const int fps = 28 + static_cast<int>(std::lround(6.0 * std::sin(t * 0.9f)));
    miniMeterSetFrames(st->preview[3], fps, (st->feedTick % 40 == 0) ? 3 : 0);
    if (st->bufPreview) {  // a healthy stream: ~half-full, flowing, an occasional ripple
        bufferMeterSetHealth(st->bufPreview,
                             50 + static_cast<int>(std::lround(34.0 * std::sin(t * 0.5f))));
        bufferMeterSetFlow(st->bufPreview, std::clamp(0.6f + 0.35f * std::sin(t * 0.8f), 0.0f, 1.0f),
                           (std::sin(t * 0.5f) > 0.85f) ? 0.5f : 0.0f);
        bufferMeterSetMetrics(st->bufPreview, L"12.4 Mb/s");
    }
}

void meterEditSwatch(HWND dlg, MetersDlgState* st, int r, int j) {
    static COLORREF custom[16] = {};  // persists custom colours across opens
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = dlg;  // disables the meters dialog (not the main window) while open
    cc.rgbResult = meterRoleColor(st->cfg[r].palette, j);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        meterSetRole(st->cfg[r].palette, j, cc.rgbResult);
        miniMeterSetPalette(st->preview[r], st->cfg[r].palette);
        InvalidateRect(st->swatch[r][j], nullptr, FALSE);
    }
}

// Reset the WORKING copy (looks + palettes) to defaults; enables are left alone and
// nothing is committed until OK, so Cancel still fully reverts.
void meterReset(MetersDlgState* st) {
    for (int r = 0; r < 4; ++r) {
        st->cfg[r].style = defaultMeterStyle(static_cast<MeterKind>(r));
        st->cfg[r].palette = defaultMeterPalette(static_cast<MeterKind>(r));
        st->cfg[r].tuning = defaultMeterTuning();
        SendMessageW(st->combo[r], CB_SETCURSEL, static_cast<int>(st->cfg[r].style), 0);
        miniMeterSetStyle(st->preview[r], st->cfg[r].style);
        miniMeterSetPalette(st->preview[r], st->cfg[r].palette);
        miniMeterSetTuning(st->preview[r], st->cfg[r].tuning);
        for (int j = 0; j < kMtrRoles; ++j) InvalidateRect(st->swatch[r][j], nullptr, FALSE);
        for (int j = 0; j < kMtrKnobs; ++j) {
            const int f = kMtrKnobDesc[r][j].field;
            if (f >= 0 && st->slider[r][j])
                SendMessageW(st->slider[r][j], TBM_SETPOS, TRUE,
                             static_cast<LPARAM>(std::lround(knobGet(st->cfg[r].tuning, f) * 100.0f)));
        }
    }
}

// Stop feeding + destroy the preview meters before the dialog dies, so no stray
// WM_TIMER lands in a freed MiniMeterState.
void meterTeardown(HWND dlg, MetersDlgState* st) {
    KillTimer(dlg, kMtrPreviewTimer);
    for (int r = 0; r < 4; ++r)
        if (st->preview[r]) {
            DestroyWindow(st->preview[r]);
            st->preview[r] = nullptr;
        }
    if (st->bufPreview) {
        DestroyWindow(st->bufPreview);
        st->bufPreview = nullptr;
    }
}

LRESULT CALLBACK MetersProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<MetersDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, wParam);
        case WM_DRAWITEM: {
            if (!st) break;
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < kMtrRoles; ++j)
                    if (di->hwndItem == st->swatch[r][j]) {
                        SetDCBrushColor(di->hDC, meterRoleColor(st->cfg[r].palette, j));
                        FillRect(di->hDC, &di->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                        const bool hot = (di->itemState & (ODS_FOCUS | ODS_SELECTED)) != 0;
                        SetDCBrushColor(di->hDC,
                                        hot ? currentTheme().accent : currentTheme().border);
                        FrameRect(di->hDC, &di->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                        return TRUE;
                    }
            return TRUE;
        }
        case WM_TIMER:
            if (st && wParam == kMtrPreviewTimer) meterFeedPreviews(st);
            return 0;
        case WM_HSCROLL: {  // a knob trackbar moved — update the working cfg + live preview
            if (!st) break;
            const HWND bar = reinterpret_cast<HWND>(lParam);
            for (int r = 0; r < 4; ++r)
                for (int j = 0; j < kMtrKnobs; ++j)
                    if (bar == st->slider[r][j]) {
                        const int f = kMtrKnobDesc[r][j].field;
                        if (f >= 0) {
                            const int pos = static_cast<int>(SendMessageW(bar, TBM_GETPOS, 0, 0));
                            knobSet(st->cfg[r].tuning, f, static_cast<float>(pos) / 100.0f);
                            miniMeterSetTuning(st->preview[r], st->cfg[r].tuning);
                        }
                        return 0;
                    }
            return 0;
        }
        case WM_COMMAND: {
            if (!st) return 0;
            const int code = HIWORD(wParam);
            const HWND ctl = reinterpret_cast<HWND>(lParam);
            for (int r = 0; r < 4; ++r) {
                if (ctl == st->combo[r] && code == CBN_SELCHANGE) {
                    const int sel = static_cast<int>(SendMessageW(st->combo[r], CB_GETCURSEL, 0, 0));
                    if (sel >= 0) {
                        st->cfg[r].style = static_cast<MeterStyle>(sel);
                        miniMeterSetStyle(st->preview[r], st->cfg[r].style);
                    }
                    return 0;
                }
                if (ctl == st->enable[r] && code == BN_CLICKED) {
                    st->cfg[r].enabled =
                        SendMessageW(st->enable[r], BM_GETCHECK, 0, 0) == BST_CHECKED;
                    return 0;
                }
                for (int j = 0; j < kMtrRoles; ++j)
                    if (ctl == st->swatch[r][j] && code == BN_CLICKED) {
                        meterEditSwatch(hwnd, st, r, j);
                        return 0;
                    }
            }
            if (ctl == st->bufEnable && code == BN_CLICKED) {  // the Data flow row's enable
                st->bufOn = SendMessageW(st->bufEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;
                return 0;
            }
            const int id = LOWORD(wParam);
            if (id == IDOK) {
                st->ok = true;
                st->done = true;
            } else if (id == IDCANCEL) {
                st->done = true;
            } else if (id == ID_MTR_RESET) {
                meterReset(st);
            }
            if (st->done) {
                meterTeardown(hwnd, st);
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            if (st) {
                st->done = true;
                meterTeardown(hwnd, st);
            }
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool chooseMeters(HWND parent, HINSTANCE hInst, UINT dpi, MeterConfig cfg[4], bool& dataFlowOn) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MetersProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsMeters";
        RegisterClassExW(&wc);
        registered = true;
    }
    MetersDlgState st;
    st.dpi = dpi;
    for (int r = 0; r < 4; ++r) st.cfg[r] = cfg[r];
    st.bufOn = dataFlowOn;
    st.font = themeFont(FontRole::Body, dpi, 11, FW_NORMAL);
    st.bold = themeFont(FontRole::Body, dpi, 12, FW_SEMIBOLD);

    const int rowH = dp(148, dpi);  // taller rows: each now carries a "feel" slider band
    const int dfRowH = dp(116, dpi);  // the Data flow row: enable + preview only (no swatches/sliders)
    const int W = dp(720, dpi), H = dp(50, dpi) + 4 * rowH + dfRowH + dp(96, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    clampToWorkArea(parent, W, H, x, y);  // keep the (tall) dialog fully on-screen
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsMeters", L"Meters",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (!dlg) {
        DeleteObject(st.font);
        DeleteObject(st.bold);
        return false;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(18, dpi);

    HWND head = CreateWindowExW(0, L"STATIC", L"Enable each meter, pick its look, and set its colours.",
                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, m, dp(14, dpi),
                                cr.right - 2 * m, dp(22, dpi), dlg, nullptr, hInst, nullptr);
    SendMessageW(head, WM_SETFONT, reinterpret_cast<WPARAM>(st.bold), TRUE);

    const wchar_t* kNames[4] = {L"Audio spectrum", L"Signal strength", L"Bitrate", L"Frame rate"};
    const wchar_t* kLooks[4] = {L"LED", L"Vacuum tube", L"LCD", L"Oscilloscope"};
    const wchar_t* kRoles[kMtrRoles] = {L"Bg", L"Dim", L"Low", L"Mid", L"High", L"Accent", L"Peak"};
    const int top0 = dp(44, dpi);
    const int sw = dp(30, dpi), sgap = dp(6, dpi);

    for (int r = 0; r < 4; ++r) {
        const int y0 = top0 + r * rowH;
        st.enable[r] = CreateWindowExW(
            0, L"BUTTON", kNames[r], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, m, y0,
            dp(150, dpi), dp(22, dpi), dlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MTR_ROW + r * 16)), hInst, nullptr);
        SendMessageW(st.enable[r], BM_SETCHECK, st.cfg[r].enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(st.enable[r], WM_SETFONT, reinterpret_cast<WPARAM>(st.bold), TRUE);

        st.preview[r] =
            createMiniMeter(dlg, hInst, ID_MTR_ROW + r * 16 + 2, dpi, static_cast<MeterKind>(r));
        MoveWindow(st.preview[r], m, y0 + dp(28, dpi), dp(150, dpi), dp(86, dpi), FALSE);
        miniMeterSetStyle(st.preview[r], st.cfg[r].style);
        miniMeterSetPalette(st.preview[r], st.cfg[r].palette);
        miniMeterSetTuning(st.preview[r], st.cfg[r].tuning);
        ShowWindow(st.preview[r], SW_SHOW);

        st.combo[r] = CreateWindowExW(
            0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            m + dp(162, dpi), y0 + dp(28, dpi), dp(140, dpi), dp(180, dpi), dlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MTR_ROW + r * 16 + 1)), hInst, nullptr);
        for (int s = 0; s < 4; ++s)
            SendMessageW(st.combo[r], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kLooks[s]));
        SendMessageW(st.combo[r], CB_SETCURSEL, static_cast<int>(st.cfg[r].style), 0);
        SendMessageW(st.combo[r], WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);

        const int sx0 = m + dp(316, dpi), sy = y0 + dp(28, dpi);
        for (int j = 0; j < kMtrRoles; ++j) {
            const int sx = sx0 + j * (sw + sgap);
            st.swatch[r][j] = CreateWindowExW(
                0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, sx, sy, sw, sw,
                dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MTR_ROW + r * 16 + 3 + j)),
                hInst, nullptr);
            HWND lbl = CreateWindowExW(0, L"STATIC", kRoles[j], WS_CHILD | WS_VISIBLE | SS_CENTER,
                                       sx - dp(4, dpi), sy + sw + dp(2, dpi), sw + dp(8, dpi),
                                       dp(14, dpi), dlg, nullptr, hInst, nullptr);
            SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
        }

        // Inline "feel" sliders for this meter's relevant knobs (below the swatches).
        const int kx0 = m + dp(162, dpi), ky = y0 + dp(86, dpi), kw = dp(120, dpi);
        for (int j = 0; j < kMtrKnobs; ++j) {
            const KnobDesc& kd = kMtrKnobDesc[r][j];
            if (kd.field < 0) continue;
            const int kx = kx0 + j * (kw + dp(8, dpi));
            HWND klbl = CreateWindowExW(0, L"STATIC", kd.label, WS_CHILD | WS_VISIBLE | SS_CENTER, kx,
                                        ky, kw, dp(14, dpi), dlg, nullptr, hInst, nullptr);
            SendMessageW(klbl, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
            st.slider[r][j] = CreateWindowExW(
                0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                kx, ky + dp(15, dpi), kw, dp(26, dpi), dlg,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MTR_ROW + r * 16 + 10 + j)), hInst,
                nullptr);
            SendMessageW(st.slider[r][j], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(
                st.slider[r][j], TBM_SETPOS, TRUE,
                static_cast<LPARAM>(std::lround(knobGet(st.cfg[r].tuning, kd.field) * 100.0f)));
        }
    }

    // Fifth row — the "Data flow" (buffer/fluid) meter: enable + live preview only. It is the
    // buffering tank at the far right of the transport bar and has no Look or palette (its own
    // internal fluid style), so this row omits the combo, swatches, and sliders.
    {
        const int y0 = top0 + 4 * rowH;
        st.bufEnable = CreateWindowExW(
            0, L"BUTTON", L"Data flow", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, m, y0,
            dp(150, dpi), dp(22, dpi), dlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MTR_ROW + 4 * 16)), hInst, nullptr);
        SendMessageW(st.bufEnable, BM_SETCHECK, st.bufOn ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(st.bufEnable, WM_SETFONT, reinterpret_cast<WPARAM>(st.bold), TRUE);

        registerBufferMeterClass(hInst);  // idempotent; the main window registered it already
        st.bufPreview = createBufferMeter(dlg, hInst, ID_MTR_ROW + 4 * 16 + 2, dpi);
        MoveWindow(st.bufPreview, m, y0 + dp(28, dpi), dp(170, dpi), dp(76, dpi), FALSE);
        ShowWindow(st.bufPreview, SW_SHOW);
        // The note advertises right-click-to-hide, and the preview honours it — so keep the
        // "Data flow" checkbox + working state in sync when the user right-clicks the preview,
        // else the box would disagree with the preview (and OK would silently win).
        bufferMeterSetOnHiddenChanged(st.bufPreview, [&st](bool hidden) {
            st.bufOn = !hidden;
            SendMessageW(st.bufEnable, BM_SETCHECK, hidden ? BST_UNCHECKED : BST_CHECKED, 0);
        });

        HWND note = CreateWindowExW(
            0, L"STATIC",
            L"The buffering tank shown at the far right of the transport bar. You can also hide it "
            L"by right-clicking the meter itself.",
            WS_CHILD | WS_VISIBLE, m + dp(190, dpi), y0 + dp(30, dpi),
            cr.right - dp(190, dpi) - 2 * m, dp(72, dpi), dlg, nullptr, hInst, nullptr);
        SendMessageW(note, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    }

    const int bw = dp(90, dpi), bh = dp(30, dpi), btnY = cr.bottom - bh - dp(16, dpi);
    HWND reset = CreateWindowExW(0, L"BUTTON", L"Reset to defaults",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, m, btnY, dp(150, dpi), bh, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MTR_RESET)), hInst,
                                 nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cr.right - 2 * bw - dp(26, dpi), btnY, bw, bh, dlg,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), hInst, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  cr.right - bw - dp(16, dpi), btnY, bw, bh, dlg,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst,
                                  nullptr);
    for (HWND h : {reset, ok, cancel})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);

    applyDialogDarkMode(dlg);
    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetTimer(dlg, kMtrPreviewTimer, kMtrPreviewMs, nullptr);
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
    if (st.ok) {
        for (int r = 0; r < 4; ++r) cfg[r] = st.cfg[r];
        dataFlowOn = st.bufOn;
    }
    DeleteObject(st.font);
    DeleteObject(st.bold);
    return st.ok;
}

// ---- Scheduled recordings: New/Edit dialog + Manager -----------------------

namespace {

// epoch (UTC seconds) <-> local SYSTEMTIME, for the DateTimePickers.
SYSTEMTIME epochToLocalSt(long long epoch) {
    const unsigned long long ft100 = static_cast<unsigned long long>(epoch + 11644473600LL) * 10000000ULL;
    FILETIME ft{static_cast<DWORD>(ft100 & 0xFFFFFFFFULL), static_cast<DWORD>(ft100 >> 32)};
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return local;
}
long long localStToEpoch(const SYSTEMTIME& local) {
    SYSTEMTIME utc{};
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME ft{};
    SystemTimeToFileTime(&utc, &ft);
    const unsigned long long ft100 =
        (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<long long>(ft100 / 10000000ULL) - 11644473600LL;
}

constexpr int ID_SCH_CHAN = 1601, ID_SCH_TITLE = 1602, ID_SCH_START = 1603, ID_SCH_STOP = 1604;

struct ScheduleDlgState {
    HWND  combo = nullptr, title = nullptr, start = nullptr, stop = nullptr;
    HFONT font = nullptr;
    UINT  dpi = 96;
    bool  ok = false, done = false;
    // Captured in the IDOK handler *before* the window (and its controls) are destroyed,
    // so the show-function reads these, not the dead HWNDs.
    const Channel* picked = nullptr;
    SYSTEMTIME     pickedStart{}, pickedStop{};
    std::wstring   pickedTitle;
};

LRESULT CALLBACK ScheduleDlgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<ScheduleDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, w);
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ of = SelectObject(dc, st->font);
            SetTextColor(dc, currentTheme().textPrimary);
            const wchar_t* labels[4] = {L"Channel:", L"Title:", L"Start:", L"Stop:"};
            const int ys[4] = {18, 76, 134, 192};
            for (int i = 0; i < 4; ++i) {
                RECT r{dp(18, st->dpi), dp(ys[i], st->dpi), dp(86, st->dpi), dp(ys[i] + 20, st->dpi)};
                DrawTextW(dc, labels[i], -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE);
            }
            SelectObject(dc, of);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case IDOK: {  // validate + capture BEFORE destroy (the controls die with the window)
                    const int sel = static_cast<int>(SendMessageW(st->combo, CB_GETCURSEL, 0, 0));
                    SYSTEMTIME a{}, b{};
                    if (sel < 0 || DateTime_GetSystemtime(st->start, &a) != GDT_VALID ||
                        DateTime_GetSystemtime(st->stop, &b) != GDT_VALID ||
                        localStToEpoch(b) <= localStToEpoch(a)) {
                        MessageBeep(MB_ICONWARNING);
                        return 0;  // keep the dialog open
                    }
                    st->picked =
                        reinterpret_cast<const Channel*>(SendMessageW(st->combo, CB_GETITEMDATA, sel, 0));
                    st->pickedStart = a;
                    st->pickedStop = b;
                    wchar_t t[256] = L"";
                    GetWindowTextW(st->title, t, ARRAYSIZE(t));
                    st->pickedTitle = t;
                    st->ok = true;
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
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
    return DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace

bool scheduleDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::vector<Channel>& channels,
                    ScheduledRecording& out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ScheduleDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsSchedule";
        RegisterClassExW(&wc);
        registered = true;
    }
    // Sort a pointer view of the channels by name for the type-ahead combo. `channels`
    // outlives this modal call, so the stored Channel* item-data stays valid.
    std::vector<const Channel*> sorted;
    sorted.reserve(channels.size());
    for (const auto& c : channels) sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(),
              [](const Channel* a, const Channel* b) { return a->name < b->name; });

    ScheduleDlgState st;
    st.dpi = dpi;
    st.font = themeFont(FontRole::Body, dpi, 14, FW_NORMAL);
    const int W = dp(460, dpi), H = dp(300, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2, y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsSchedule",
                               out.channelName.empty() ? L"New Recording Schedule" : L"Schedule Recording",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst,
                               nullptr);
    if (!dlg) {
        DeleteObject(st.font);
        return false;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(18, dpi), fx = dp(90, dpi), fw = cr.right - fx - m;

    st.combo = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, fx,
                               dp(14, dpi), fw, dp(320, dpi), dlg,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SCH_CHAN)), hInst, nullptr);
    int preSel = -1;
    for (int i = 0; i < static_cast<int>(sorted.size()); ++i) {
        const int idx = static_cast<int>(
            SendMessageW(st.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sorted[i]->name.c_str())));
        SendMessageW(st.combo, CB_SETITEMDATA, idx, reinterpret_cast<LPARAM>(sorted[i]));
        if (preSel < 0 && !out.streamUrl.empty() && sorted[i]->streamUrl == out.streamUrl) preSel = idx;
        else if (preSel < 0 && !out.channelId.empty() && sorted[i]->tvgId == out.channelId) preSel = idx;
    }
    if (preSel >= 0) SendMessageW(st.combo, CB_SETCURSEL, preSel, 0);

    st.title = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", out.title.c_str(),
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, fx, dp(72, dpi), fw,
                               dp(26, dpi), dlg,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SCH_TITLE)), hInst, nullptr);

    st.start = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, fx,
                               dp(130, dpi), fw, dp(26, dpi), dlg,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SCH_START)), hInst, nullptr);
    st.stop = CreateWindowExW(0, DATETIMEPICK_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, fx,
                              dp(188, dpi), fw, dp(26, dpi), dlg,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SCH_STOP)), hInst, nullptr);
    DateTime_SetFormat(st.start, L"yyyy-MM-dd  HH:mm");
    DateTime_SetFormat(st.stop, L"yyyy-MM-dd  HH:mm");
    SYSTEMTIME nowSt;
    GetLocalTime(&nowSt);
    const long long nowE = localStToEpoch(nowSt);
    SYSTEMTIME s0 = epochToLocalSt(out.startUtc > 0 ? out.startUtc : nowE + 300);
    SYSTEMTIME s1 = epochToLocalSt(out.stopUtc > 0 ? out.stopUtc : nowE + 3900);
    DateTime_SetSystemtime(st.start, GDT_VALID, &s0);
    DateTime_SetSystemtime(st.stop, GDT_VALID, &s1);

    const int bw = dp(90, dpi), bh = dp(30, dpi), btnY = cr.bottom - bh - dp(16, dpi);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cr.right - 2 * bw - dp(28, dpi), btnY, bw, bh, dlg,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), hInst, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  cr.right - bw - dp(18, dpi), btnY, bw, bh, dlg,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst, nullptr);
    for (HWND h : {st.combo, st.title, st.start, st.stop, ok, cancel})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    applyDialogDarkMode(dlg);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.combo);
    MSG msg;
    while (!st.done) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); DestroyWindow(dlg); break; }
        if (r == -1) { DestroyWindow(dlg); break; }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    bool result = false;
    if (st.ok && st.picked) {  // captured in the IDOK handler (the controls are gone now)
        out.channelId = st.picked->tvgId;
        out.channelName = st.picked->name;
        out.streamUrl = st.picked->streamUrl;
        out.userAgent = st.picked->userAgent;
        out.referrer = st.picked->referrer;
        out.title = st.pickedTitle;
        out.startUtc = localStToEpoch(st.pickedStart);
        out.stopUtc = localStToEpoch(st.pickedStop);
        result = true;
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(st.font);
    return result;
}

namespace {

constexpr int ID_MG_LV = 1701, ID_MG_NEW = 1702, ID_MG_CANCEL = 1703, ID_MG_DELETE = 1704;

const wchar_t* scheduleStatusText(ScheduleStatus s) {
    switch (s) {
        case ScheduleStatus::Pending: return L"Pending";
        case ScheduleStatus::Recording: return L"● Recording";
        case ScheduleStatus::Done: return L"Done";
        case ScheduleStatus::Missed: return L"Missed";
        case ScheduleStatus::Failed: return L"Failed";
        case ScheduleStatus::Cancelled: return L"Cancelled";
    }
    return L"";
}

std::wstring scheduleWhen(long long start, long long stop) {
    SYSTEMTIME a = epochToLocalSt(start), b = epochToLocalSt(stop);
    wchar_t buf[80];
    if (a.wYear == b.wYear && a.wMonth == b.wMonth && a.wDay == b.wDay)
        swprintf_s(buf, L"%04d-%02d-%02d  %02d:%02d–%02d:%02d", a.wYear, a.wMonth, a.wDay, a.wHour,
                   a.wMinute, b.wHour, b.wMinute);
    else  // crosses midnight — show the stop date too
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d – %02d-%02d %02d:%02d", a.wYear, a.wMonth, a.wDay,
                   a.wHour, a.wMinute, b.wMonth, b.wDay, b.wHour, b.wMinute);
    return buf;
}

struct ManageDlgState {
    HWND  lv = nullptr;
    HFONT font = nullptr;
    UINT  dpi = 96;
    ScheduleManagerCallbacks cb;
    std::vector<ScheduledRecording> rows;
    bool  done = false;
};

void mgRepopulate(ManageDlgState* st) {
    st->rows = st->cb.list ? st->cb.list() : std::vector<ScheduledRecording>{};
    SendMessageW(st->lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st->lv);
    for (int i = 0; i < static_cast<int>(st->rows.size()); ++i) {
        const ScheduledRecording& s = st->rows[i];
        std::wstring title = s.title.empty() ? L"(untitled)" : s.title;
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.lParam = i;
        it.pszText = const_cast<LPWSTR>(title.c_str());
        const int row = ListView_InsertItem(st->lv, &it);
        ListView_SetItemText(st->lv, row, 1, const_cast<LPWSTR>(s.channelName.c_str()));
        std::wstring when = scheduleWhen(s.startUtc, s.stopUtc);
        ListView_SetItemText(st->lv, row, 2, const_cast<LPWSTR>(when.c_str()));
        ListView_SetItemText(st->lv, row, 3, const_cast<LPWSTR>(scheduleStatusText(s.status)));
    }
    SendMessageW(st->lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->lv, nullptr, TRUE);
}

long long mgSelectedId(ManageDlgState* st) {
    const int sel = ListView_GetNextItem(st->lv, -1, LVNI_SELECTED);
    if (sel < 0) return 0;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    if (!ListView_GetItem(st->lv, &it)) return 0;
    const int idx = static_cast<int>(it.lParam);
    return (idx >= 0 && idx < static_cast<int>(st->rows.size())) ? st->rows[idx].id : 0;
}

LRESULT CALLBACK ManageDlgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<ManageDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, themeBrush(currentTheme().panelBg));
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return dialogCtlColor(msg, w);
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_MG_NEW:
                    if (st->cb.addNew) {
                        st->cb.addNew(hwnd);
                        mgRepopulate(st);
                    }
                    return 0;
                case ID_MG_CANCEL:
                    if (const long long id = mgSelectedId(st); id && st->cb.cancel) {
                        st->cb.cancel(id);
                        mgRepopulate(st);
                    }
                    return 0;
                case ID_MG_DELETE:
                    if (const long long id = mgSelectedId(st); id && st->cb.remove) {
                        st->cb.remove(id);
                        mgRepopulate(st);
                    }
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
    return DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace

void manageSchedules(HWND parent, HINSTANCE hInst, UINT dpi, ScheduleManagerCallbacks cb) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ManageDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"RabbitEarsScheduleMgr";
        RegisterClassExW(&wc);
        registered = true;
    }
    ManageDlgState st;
    st.dpi = dpi;
    st.cb = std::move(cb);
    st.font = themeFont(FontRole::Body, dpi, 14, FW_NORMAL);
    const int W = dp(640, dpi), H = dp(460, dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    const int x = pr.left + ((pr.right - pr.left) - W) / 2, y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND dlg =
        CreateWindowExW(WS_EX_DLGMODALFRAME, L"RabbitEarsScheduleMgr", L"Scheduled Recordings",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst, nullptr);
    if (!dlg) {
        DeleteObject(st.font);
        return;
    }
    SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    RECT cr;
    GetClientRect(dlg, &cr);
    const int m = dp(16, dpi), bh = dp(30, dpi), btnY = cr.bottom - bh - dp(14, dpi);
    const int listBottom = btnY - dp(10, dpi);

    st.lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS,
                            m, m, cr.right - 2 * m, listBottom - m, dlg,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MG_LV)), hInst, nullptr);
    ListView_SetExtendedListViewStyle(st.lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const int lvW = cr.right - 2 * m - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    const struct { const wchar_t* h; int pct; } cols[4] = {
        {L"Title", 34}, {L"Channel", 26}, {L"When", 28}, {L"Status", 12}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(cols[i].h);
        col.cx = lvW * cols[i].pct / 100;
        ListView_InsertColumn(st.lv, i, &col);
    }

    const int bw = dp(96, dpi), gap = dp(8, dpi);
    HWND newB = CreateWindowExW(0, L"BUTTON", L"New…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, m, btnY, bw,
                                bh, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MG_NEW)), hInst,
                                nullptr);
    HWND cancelB = CreateWindowExW(0, L"BUTTON", L"Cancel recording",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP, m + bw + gap, btnY, dp(140, dpi),
                                   bh, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MG_CANCEL)),
                                   hInst, nullptr);
    HWND delB = CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                m + bw + gap + dp(140, dpi) + gap, btnY, bw, bh, dlg,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MG_DELETE)), hInst,
                                nullptr);
    HWND closeB = CreateWindowExW(0, L"BUTTON", L"Close",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  cr.right - bw - dp(16, dpi), btnY, bw, bh, dlg,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), hInst, nullptr);
    for (HWND h : {st.lv, newB, cancelB, delB, closeB})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);

    applyDialogDarkMode(dlg);
    const Theme& th = currentTheme();
    ListView_SetBkColor(st.lv, th.windowBg);
    ListView_SetTextBkColor(st.lv, th.windowBg);
    ListView_SetTextColor(st.lv, th.textPrimary);
    mgRepopulate(&st);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.lv);
    MSG msg;
    while (!st.done) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { PostQuitMessage(static_cast<int>(msg.wParam)); DestroyWindow(dlg); break; }
        if (r == -1) { DestroyWindow(dlg); break; }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DeleteObject(st.font);
}

}  // namespace rabbitears
