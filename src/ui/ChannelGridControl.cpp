// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ChannelGridControl.h"

#include <algorithm>
#include <cwctype>
#include <string>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "ui/D2DSupport.h"
#include "ui/Theme.h"

// NOTE: windows.h defines DrawText -> DrawTextW, and d2d1.h declares
// ID2D1RenderTarget::DrawText THROUGH that same macro (so the real member is
// DrawTextW). We therefore keep the macro and call rt->DrawText(...) — both the
// declaration and our call expand to DrawTextW consistently.

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReChannelGrid";
constexpr UINT_PTR kTypeTimer = 2;  // resets the type-a-number accumulator
enum Col { COL_NUM = 0, COL_FAV, COL_NAME, COL_GROUP, COL_COUNT };

int dpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

struct GridState {
    std::vector<Channel> channels;
    std::vector<size_t>  rowOrder;   // display index -> channels index
    std::wstring         filterLower;
    int                  selectedRow = -1;  // display index
    int                  hoverRow = -1;
    long long            nowPlayingId = 0;
    int                  scrollY = 0;
    UINT                 dpi = 96;
    int                  rowH = 30;
    int                  headerH = 30;
    bool                 tracking = false;
    int                  typeNum = 0;   // type-a-number-to-jump accumulator
    ChannelGridCallbacks cb;

    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush*  brush = nullptr;
    IDWriteTextFormat*     fmtLeft = nullptr;
    IDWriteTextFormat*     fmtRight = nullptr;
    IDWriteTextFormat*     fmtStar = nullptr;
    IDWriteTextFormat*     fmtHeader = nullptr;
};

GridState* stateOf(HWND h) { return reinterpret_cast<GridState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void computeMetrics(GridState* st) {
    st->rowH = dpx(st->dpi, 30);
    st->headerH = dpx(st->dpi, 30);
}

void releaseFormats(GridState* st) {
    SafeRelease(st->fmtLeft);
    SafeRelease(st->fmtRight);
    SafeRelease(st->fmtStar);
    SafeRelease(st->fmtHeader);
}

void recreateFormats(GridState* st) {
    releaseFormats(st);
    IDWriteFactory* f = dwriteFactory();
    if (!f) return;
    const float sz = static_cast<float>(dpx(st->dpi, 14));
    auto mk = [&](const wchar_t* family, DWRITE_TEXT_ALIGNMENT a, DWRITE_FONT_WEIGHT w, float size,
                  IDWriteTextFormat** out) {
        if (SUCCEEDED(f->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, size, L"", out)) &&
            *out) {
            (*out)->SetTextAlignment(a);
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    };
    mk(L"Segoe UI", DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_NORMAL, sz, &st->fmtLeft);
    mk(L"Segoe UI", DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_NORMAL, sz, &st->fmtRight);
    mk(L"Segoe UI Symbol", DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_FONT_WEIGHT_NORMAL,
       static_cast<float>(dpx(st->dpi, 15)), &st->fmtStar);
    mk(L"Segoe UI", DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_FONT_WEIGHT_SEMI_BOLD, sz, &st->fmtHeader);
}

void discardDevice(GridState* st) {
    SafeRelease(st->brush);
    SafeRelease(st->rt);
}

bool ensureDevice(GridState* st, HWND hwnd) {
    if (st->rt) return true;
    ID2D1Factory* f = d2dFactory();
    if (!f) return false;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));
    if (FAILED(f->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                         D2D1::HwndRenderTargetProperties(hwnd, size), &st->rt)) ||
        !st->rt)
        return false;
    st->rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &st->brush);
    return st->brush != nullptr;
}

int colWidth(GridState* st, HWND hwnd, int col) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int numW = dpx(st->dpi, 54), favW = dpx(st->dpi, 40), groupW = dpx(st->dpi, 190);
    int nameW = rc.right - numW - favW - groupW;
    if (nameW < dpx(st->dpi, 120)) nameW = dpx(st->dpi, 120);
    switch (col) {
        case COL_NUM: return numW;
        case COL_FAV: return favW;
        case COL_NAME: return nameW;
        case COL_GROUP: return groupW;
    }
    return 0;
}
int colLeft(GridState* st, HWND hwnd, int col) {
    int x = 0;
    for (int c = 0; c < col; ++c) x += colWidth(st, hwnd, c);
    return x;
}

void applyFilter(GridState* st) {
    st->rowOrder.clear();
    if (st->filterLower.empty()) {
        st->rowOrder.reserve(st->channels.size());
        for (size_t i = 0; i < st->channels.size(); ++i) st->rowOrder.push_back(i);
    } else {
        for (size_t i = 0; i < st->channels.size(); ++i) {
            const Channel& c = st->channels[i];
            if (lower(c.name).find(st->filterLower) != std::wstring::npos ||
                lower(c.groupTitle).find(st->filterLower) != std::wstring::npos ||
                lower(c.tvgName).find(st->filterLower) != std::wstring::npos)
                st->rowOrder.push_back(i);
        }
    }
    st->selectedRow = -1;
}

int contentHeight(GridState* st) { return static_cast<int>(st->rowOrder.size()) * st->rowH; }
int viewHeight(HWND hwnd, GridState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    return std::max(0, static_cast<int>(rc.bottom) - st->headerH);
}
void clampScroll(HWND hwnd, GridState* st) {
    const int maxS = std::max(0, contentHeight(st) - viewHeight(hwnd, st));
    st->scrollY = std::clamp(st->scrollY, 0, maxS);
}
void updateScrollbar(HWND hwnd, GridState* st) {
    clampScroll(hwnd, st);
    SCROLLINFO si{sizeof(si)};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, contentHeight(st) - 1);
    si.nPage = static_cast<UINT>(viewHeight(hwnd, st));
    si.nPos = st->scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

int rowAtY(GridState* st, int y) {
    if (y < st->headerH) return -1;
    const int idx = (y - st->headerH + st->scrollY) / st->rowH;
    return (idx >= 0 && idx < static_cast<int>(st->rowOrder.size())) ? idx : -1;
}
int colAtX(GridState* st, HWND hwnd, int x) {
    int left = 0;
    for (int c = 0; c < COL_COUNT; ++c) {
        const int w = colWidth(st, hwnd, c);
        if (x >= left && x < left + w) return c;
        left += w;
    }
    return -1;
}

void ensureVisible(HWND hwnd, GridState* st, int row) {
    if (row < 0) return;
    const int top = row * st->rowH;
    const int vh = viewHeight(hwnd, st);
    if (top < st->scrollY) st->scrollY = top;
    else if (top + st->rowH > st->scrollY + vh) st->scrollY = top + st->rowH - vh;
    clampScroll(hwnd, st);
}

const Channel* channelAtRow(GridState* st, int row) {
    if (row < 0 || row >= static_cast<int>(st->rowOrder.size())) return nullptr;
    return &st->channels[st->rowOrder[row]];
}

void paint(HWND hwnd, GridState* st) {
    if (!ensureDevice(st, hwnd)) return;
    if (!st->fmtLeft) recreateFormats(st);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const Theme& th = currentTheme();
    ID2D1HwndRenderTarget* rt = st->rt;

    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(colorToD2D(th.panelBg));

    auto fill = [&](float x, float y, float w, float h, COLORREF c) {
        st->brush->SetColor(colorToD2D(c));
        rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), st->brush);
    };
    auto text = [&](const std::wstring& s, IDWriteTextFormat* fmt, float x, float w, float y, float h,
                    COLORREF c, float padL) {
        if (s.empty() || !fmt) return;
        st->brush->SetColor(colorToD2D(c));
        rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt,
                     D2D1::RectF(x + padL, y, x + w - padL, y + h), st->brush,
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const int nr = static_cast<int>(st->rowOrder.size());
    const int first = st->rowH > 0 ? st->scrollY / st->rowH : 0;
    const float pad = static_cast<float>(dpx(st->dpi, 8));
    for (int r = std::max(0, first); r < nr; ++r) {
        const float y = static_cast<float>(st->headerH + r * st->rowH - st->scrollY);
        if (y >= rc.bottom) break;
        const Channel& c = st->channels[st->rowOrder[r]];
        const bool sel = (r == st->selectedRow);
        const bool playing = (st->nowPlayingId != 0 && c.id == st->nowPlayingId);
        const COLORREF rowbg = sel ? th.selectionBg
                                   : (r == st->hoverRow ? th.hoverBg
                                                        : ((r & 1) ? th.altRowBg : th.panelBg));
        fill(0, y, static_cast<float>(rc.right), static_cast<float>(st->rowH), rowbg);
        if (playing) fill(0, y, static_cast<float>(dpx(st->dpi, 3)), static_cast<float>(st->rowH), th.accent);

        const COLORREF txtc = sel ? th.selectionText : th.textPrimary;
        const COLORREF subc = sel ? th.selectionText : th.textSecondary;
        const std::wstring num = c.lcn ? std::to_wstring(*c.lcn) : std::wstring();
        text(num, st->fmtRight, static_cast<float>(colLeft(st, hwnd, COL_NUM)),
             static_cast<float>(colWidth(st, hwnd, COL_NUM)), y, static_cast<float>(st->rowH), subc, pad);
        text(c.favourite ? L"★" : L"☆", st->fmtStar,
             static_cast<float>(colLeft(st, hwnd, COL_FAV)), static_cast<float>(colWidth(st, hwnd, COL_FAV)),
             y, static_cast<float>(st->rowH), c.favourite ? th.accent : (sel ? th.selectionText : th.textMuted), 0);
        text(c.name, st->fmtLeft, static_cast<float>(colLeft(st, hwnd, COL_NAME)),
             static_cast<float>(colWidth(st, hwnd, COL_NAME)), y, static_cast<float>(st->rowH), txtc, pad);
        text(c.groupTitle, st->fmtLeft, static_cast<float>(colLeft(st, hwnd, COL_GROUP)),
             static_cast<float>(colWidth(st, hwnd, COL_GROUP)), y, static_cast<float>(st->rowH), subc, pad);
    }

    // Header band on top.
    fill(0, 0, static_cast<float>(rc.right), static_cast<float>(st->headerH), th.panelElevBg);
    fill(0, static_cast<float>(st->headerH - 1), static_cast<float>(rc.right), 1, th.border);
    const wchar_t* heads[COL_COUNT] = {L"#", L"", L"Channel", L"Group"};
    for (int c = 0; c < COL_COUNT; ++c) {
        if (!*heads[c]) continue;
        IDWriteTextFormat* hf = (c == COL_NUM) ? st->fmtRight : st->fmtHeader;
        text(heads[c], hf, static_cast<float>(colLeft(st, hwnd, c)),
             static_cast<float>(colWidth(st, hwnd, c)), 0, static_cast<float>(st->headerH),
             th.textSecondary, pad);
    }

    if (st->rt->EndDraw() == D2DERR_RECREATE_TARGET) discardDevice(st);
}

LRESULT CALLBACK GridProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* st = new GridState();
        st->dpi = GetDpiForWindow(hwnd);
        computeMetrics(st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    GridState* st = stateOf(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            paint(hwnd, st);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (st->rt) st->rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            updateScrollbar(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            if (!st->tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                st->tracking = true;
            }
            const int r = rowAtY(st, GET_Y_LPARAM(lParam));
            if (r != st->hoverRow) {
                st->hoverRow = r;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            st->tracking = false;
            if (st->hoverRow != -1) {
                st->hoverRow = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetFocus(hwnd);
            const int r = rowAtY(st, GET_Y_LPARAM(lParam));
            if (r < 0) return 0;
            const int col = colAtX(st, hwnd, GET_X_LPARAM(lParam));
            st->selectedRow = r;
            const Channel* c = channelAtRow(st, r);
            if (col == COL_FAV) {
                if (c && st->cb.onToggleFavourite) {
                    st->channels[st->rowOrder[r]].favourite = !st->channels[st->rowOrder[r]].favourite;
                    st->cb.onToggleFavourite(st->channels[st->rowOrder[r]]);
                }
            } else if (c && st->cb.onActivate) {
                st->cb.onActivate(*c);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            st->scrollY -= (delta / WHEEL_DELTA) * st->rowH * 3;
            clampScroll(hwnd, st);
            updateScrollbar(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            const int page = viewHeight(hwnd, st);
            SCROLLINFO si{sizeof(si)};
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            switch (LOWORD(wParam)) {
                case SB_LINEUP: st->scrollY -= st->rowH; break;
                case SB_LINEDOWN: st->scrollY += st->rowH; break;
                case SB_PAGEUP: st->scrollY -= page; break;
                case SB_PAGEDOWN: st->scrollY += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: st->scrollY = si.nTrackPos; break;
            }
            clampScroll(hwnd, st);
            updateScrollbar(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN: {
            const int n = static_cast<int>(st->rowOrder.size());
            const int pageRows = std::max(1, viewHeight(hwnd, st) / st->rowH);
            int sel = st->selectedRow;
            switch (wParam) {
                case VK_UP: sel = std::max(0, sel - 1); break;
                case VK_DOWN: sel = (sel < 0) ? 0 : std::min(n - 1, sel + 1); break;
                case VK_PRIOR: sel = std::max(0, (sel < 0 ? 0 : sel) - pageRows); break;
                case VK_NEXT: sel = std::min(n - 1, (sel < 0 ? 0 : sel) + pageRows); break;
                case VK_HOME: sel = 0; break;
                case VK_END: sel = n - 1; break;
                case VK_RETURN: {
                    const Channel* c = channelAtRow(st, st->selectedRow);
                    if (c && st->cb.onActivate) st->cb.onActivate(*c);
                    return 0;
                }
                default: return 0;
            }
            if (n > 0 && sel != st->selectedRow) {
                st->selectedRow = sel;
                ensureVisible(hwnd, st, sel);
                updateScrollbar(hwnd, st);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_CHAR:
            // Type a channel number to jump to it (LCN); resets after ~0.9s idle.
            if (wParam >= L'0' && wParam <= L'9') {
                st->typeNum = st->typeNum * 10 + static_cast<int>(wParam - L'0');
                if (st->typeNum > 999999) st->typeNum = static_cast<int>(wParam - L'0');
                KillTimer(hwnd, kTypeTimer);
                SetTimer(hwnd, kTypeTimer, 900, nullptr);
                for (int r = 0; r < static_cast<int>(st->rowOrder.size()); ++r) {
                    const Channel& c = st->channels[st->rowOrder[r]];
                    if (c.lcn && *c.lcn == st->typeNum) {
                        st->selectedRow = r;
                        ensureVisible(hwnd, st, r);
                        updateScrollbar(hwnd, st);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    }
                }
            }
            return 0;
        case WM_TIMER:
            if (wParam == kTypeTimer) {
                st->typeNum = 0;
                KillTimer(hwnd, kTypeTimer);
            }
            return 0;
        case WM_NCDESTROY:
            discardDevice(st);
            releaseFormats(st);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

void registerChannelGridClass(HINSTANCE hInst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = GridProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

HWND createChannelGrid(HWND parent, HINSTANCE hInst, int id, UINT dpi) {
    HWND h = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS, 0, 0,
                             10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst,
                             nullptr);
    if (h) {
        if (GridState* st = stateOf(h)) {
            st->dpi = dpi;
            computeMetrics(st);
            recreateFormats(st);
        }
    }
    return h;
}

void channelGridSetChannels(HWND grid, std::vector<Channel> channels) {
    GridState* st = stateOf(grid);
    if (!st) return;
    st->channels = std::move(channels);
    applyFilter(st);
    st->scrollY = 0;
    updateScrollbar(grid, st);
    InvalidateRect(grid, nullptr, FALSE);
}

void channelGridSetFilter(HWND grid, const std::wstring& textFilter) {
    GridState* st = stateOf(grid);
    if (!st) return;
    const std::wstring lo = lower(textFilter);
    if (lo == st->filterLower) return;
    st->filterLower = lo;
    applyFilter(st);
    st->scrollY = 0;
    updateScrollbar(grid, st);
    InvalidateRect(grid, nullptr, FALSE);
}

void channelGridSetCallbacks(HWND grid, ChannelGridCallbacks cb) {
    if (GridState* st = stateOf(grid)) st->cb = std::move(cb);
}

void channelGridUpdateDpi(HWND grid, UINT dpi) {
    GridState* st = stateOf(grid);
    if (!st) return;
    st->dpi = dpi;
    computeMetrics(st);
    recreateFormats(st);
    updateScrollbar(grid, st);
    InvalidateRect(grid, nullptr, FALSE);
}

void channelGridApplyTheme(HWND grid) { InvalidateRect(grid, nullptr, FALSE); }

void channelGridGetCounts(HWND grid, int* shown, int* total) {
    GridState* st = stateOf(grid);
    if (shown) *shown = st ? static_cast<int>(st->rowOrder.size()) : 0;
    if (total) *total = st ? static_cast<int>(st->channels.size()) : 0;
}

void channelGridSetNowPlaying(HWND grid, long long channelId) {
    GridState* st = stateOf(grid);
    if (!st) return;
    st->nowPlayingId = channelId;
    InvalidateRect(grid, nullptr, FALSE);
}

}  // namespace rabbitears
