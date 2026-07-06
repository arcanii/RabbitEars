// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/EpgGuideControl.h"

#include <algorithm>
#include <ctime>
#include <cwchar>
#include <string>
#include <vector>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "resource.h"     // IDI_APPICON
#include "ui/D2DSupport.h"
#include "ui/Dialogs.h"   // showInfoDialog
#include "ui/Theme.h"     // currentTheme + applyDialogDarkMode

// windows.h maps DrawText -> DrawTextW and d2d1.h declares ID2D1RenderTarget::DrawText
// through that same macro, so rt->DrawText(...) resolves to DrawTextW consistently
// (matching ChannelGridControl).

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReEpgGuide";

// The single live guide window (one at a time; re-opening repopulates it).
HWND g_guide = nullptr;

int dpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

// epoch (UTC seconds) -> local "HH:MM".
std::wstring hm(long long epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    wchar_t b[8];
    wcsftime(b, sizeof(b) / sizeof(b[0]), L"%H:%M", &tmv);
    return b;
}

struct GuideState {
    std::vector<GuideRow> rows;     // the filtered view everything paints + hit-tests
    std::vector<GuideRow> allRows;  // the full set; `rows` is this filtered by `filter`
    std::wstring filter;            // channel-name search (type-to-filter; shown in the corner cell)
    long long originUtc = 0;  // left edge (earliest start floored to the hour)
    long long endUtc = 0;     // right edge (latest stop ceiled to the hour)
    long long nowUtc = 0;
    UINT      dpi = 96;
    HINSTANCE hInst = nullptr;
    GuideCallbacks cb;

    int scrollX = 0;  // px along the time axis
    int scrollY = 0;  // px along the channel axis
    int rowH = 44;
    int headerH = 34;
    int channelColW = 200;
    int  pxPerHour = 150;
    int  hoverRow = -1;
    bool tracking = false;  // WM_MOUSELEAVE armed? (one-shot TrackMouseEvent)

    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush*  brush = nullptr;
    IDWriteTextFormat*     fmtChannel = nullptr;  // channel names (frozen column)
    IDWriteTextFormat*     fmtProg = nullptr;     // programme title
    IDWriteTextFormat*     fmtSub = nullptr;      // programme time range
    IDWriteTextFormat*     fmtTime = nullptr;     // hour-axis labels
};

GuideState* stateOf(HWND h) { return reinterpret_cast<GuideState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void computeMetrics(GuideState* st) {
    st->rowH = dpx(st->dpi, 44);
    st->headerH = dpx(st->dpi, 34);
    st->channelColW = dpx(st->dpi, 200);
    st->pxPerHour = dpx(st->dpi, 150);
}

// ASCII-lowercase (matches the toUpper style used elsewhere; no locale/include needed).
std::wstring lowerCopy(const std::wstring& s) {
    std::wstring o(s);
    for (wchar_t& c : o)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return o;
}

// Rebuild the visible `rows` from `allRows` by the current channel-name search. Everything
// downstream (paint, hit-testing, scrollbars) iterates `rows`, so this is all search needs.
void applyFilter(GuideState* st) {
    if (st->filter.empty()) {
        st->rows = st->allRows;
    } else {
        const std::wstring needle = lowerCopy(st->filter);
        st->rows.clear();
        for (const GuideRow& row : st->allRows)
            if (lowerCopy(row.channelName).find(needle) != std::wstring::npos)
                st->rows.push_back(row);
    }
    st->scrollY = 0;  // jump to the top of the filtered list
}

void releaseFormats(GuideState* st) {
    SafeRelease(st->fmtChannel);
    SafeRelease(st->fmtProg);
    SafeRelease(st->fmtSub);
    SafeRelease(st->fmtTime);
}

void recreateFormats(GuideState* st) {
    releaseFormats(st);
    st->fmtChannel = themeTextFormat(FontRole::Body, st->dpi, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                     DWRITE_TEXT_ALIGNMENT_LEADING);
    st->fmtProg = themeTextFormat(FontRole::Body, st->dpi, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                  DWRITE_TEXT_ALIGNMENT_LEADING);
    st->fmtSub = themeTextFormat(FontRole::Body, st->dpi, 12, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_LEADING);
    st->fmtTime = themeTextFormat(FontRole::Body, st->dpi, 12, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_LEADING);
}

void discardDevice(GuideState* st) {
    SafeRelease(st->brush);
    SafeRelease(st->rt);
}

bool ensureDevice(GuideState* st, HWND hwnd) {
    if (st->rt) return true;
    ID2D1Factory* f = d2dFactory();
    if (!f) return false;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = 96.0f;  // pin to 96: 1 D2D unit == 1 px (we DPI-scale via dpx())
    props.dpiY = 96.0f;
    if (FAILED(f->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, size),
                                         &st->rt)) ||
        !st->rt)
        return false;
    st->rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &st->brush);
    return st->brush != nullptr;
}

// content-space X (before the frozen column and horizontal scroll) for a time.
int timeToContentX(GuideState* st, long long t) {
    return static_cast<int>((t - st->originUtc) * st->pxPerHour / 3600);
}
int contentWidth(GuideState* st) { return timeToContentX(st, st->endUtc); }
int contentHeight(GuideState* st) { return static_cast<int>(st->rows.size()) * st->rowH; }

int viewW(HWND hwnd, GuideState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    return std::max(0, static_cast<int>(rc.right) - st->channelColW);
}
int viewH(HWND hwnd, GuideState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    return std::max(0, static_cast<int>(rc.bottom) - st->headerH);
}

void clampScroll(HWND hwnd, GuideState* st) {
    st->scrollX = std::clamp(st->scrollX, 0, std::max(0, contentWidth(st) - viewW(hwnd, st)));
    st->scrollY = std::clamp(st->scrollY, 0, std::max(0, contentHeight(st) - viewH(hwnd, st)));
}

void updateScrollbars(HWND hwnd, GuideState* st) {
    clampScroll(hwnd, st);
    SCROLLINFO sh{sizeof(sh)};
    sh.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    sh.nMin = 0;
    sh.nMax = std::max(0, contentWidth(st) - 1);
    sh.nPage = static_cast<UINT>(viewW(hwnd, st));
    sh.nPos = st->scrollX;
    SetScrollInfo(hwnd, SB_HORZ, &sh, TRUE);
    SCROLLINFO sv{sizeof(sv)};
    sv.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    sv.nMin = 0;
    sv.nMax = std::max(0, contentHeight(st) - 1);
    sv.nPage = static_cast<UINT>(viewH(hwnd, st));
    sv.nPos = st->scrollY;
    SetScrollInfo(hwnd, SB_VERT, &sv, TRUE);
}

// ---- paint -----------------------------------------------------------------

void paint(HWND hwnd, GuideState* st) {
    if (!ensureDevice(st, hwnd)) return;
    if (!st->fmtChannel) recreateFormats(st);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const float cw = static_cast<float>(rc.right), ch = static_cast<float>(rc.bottom);
    const Theme& th = currentTheme();
    ID2D1HwndRenderTarget* rt = st->rt;
    const float pad = static_cast<float>(dpx(st->dpi, 6));
    const float colW = static_cast<float>(st->channelColW);
    const float hdrH = static_cast<float>(st->headerH);

    auto fill = [&](float x, float y, float w, float h, COLORREF c) {
        st->brush->SetColor(colorToD2D(c));
        rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), st->brush);
    };
    auto text = [&](const std::wstring& s, IDWriteTextFormat* fmt, float x, float y, float w, float h,
                    COLORREF c) {
        if (s.empty() || !fmt) return;
        st->brush->SetColor(colorToD2D(c));
        rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt, D2D1::RectF(x, y, x + w, y + h),
                     st->brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(colorToD2D(th.panelBg));

    const int nr = static_cast<int>(st->rows.size());
    const int firstRow = st->rowH > 0 ? st->scrollY / st->rowH : 0;

    // ---- programme area (clipped so scrolled blocks can't bleed into the frozen panes)
    rt->PushAxisAlignedClip(D2D1::RectF(colW, hdrH, cw, ch), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int r = std::max(0, firstRow); r < nr; ++r) {
        const float rowY = hdrH + static_cast<float>(r * st->rowH - st->scrollY);
        if (rowY >= ch) break;
        fill(colW, rowY, cw - colW, static_cast<float>(st->rowH),
             r == st->hoverRow ? th.hoverBg : ((r & 1) ? th.altRowBg : th.panelBg));
        for (const GuideProgramme& p : st->rows[r].programmes) {
            if (p.stopUtc <= p.startUtc) continue;
            const float x0 = colW + static_cast<float>(timeToContentX(st, p.startUtc) - st->scrollX);
            const float x1 = colW + static_cast<float>(timeToContentX(st, p.stopUtc) - st->scrollX);
            if (x1 <= colW || x0 >= cw) continue;  // fully off-screen (or under the column)
            const float bx0 = std::max(x0, colW) + 1.0f, bx1 = x1 - 1.0f;
            if (bx1 - bx0 < 2.0f) continue;
            const bool airing = (p.startUtc <= st->nowUtc && st->nowUtc < p.stopUtc);
            const float by0 = rowY + 2.0f, by1 = rowY + static_cast<float>(st->rowH) - 2.0f;
            const auto rr = D2D1::RoundedRect(D2D1::RectF(bx0, by0, bx1, by1), 3.0f, 3.0f);
            st->brush->SetColor(colorToD2D(airing ? th.selectionBg : th.panelElevBg));
            rt->FillRoundedRectangle(rr, st->brush);
            st->brush->SetColor(colorToD2D(airing ? th.accent : th.border));
            rt->DrawRoundedRectangle(rr, st->brush, 1.0f);
            // Title + time, clipped to the block.
            const float tx = bx0 + pad, tw = bx1 - bx0 - pad * 2;
            if (tw > pad) {
                const COLORREF tc = airing ? th.selectionText : th.textPrimary;
                const COLORREF sc = airing ? th.selectionText : th.textSecondary;
                text(p.title, st->fmtProg, tx, rowY + dpx(st->dpi, 5), tw,
                     static_cast<float>(dpx(st->dpi, 18)), tc);
                text(hm(p.startUtc) + L" – " + hm(p.stopUtc), st->fmtSub, tx,
                     rowY + static_cast<float>(dpx(st->dpi, 23)), tw, static_cast<float>(dpx(st->dpi, 16)),
                     sc);
            }
        }
    }
    // "Now" line.
    const float nowX = colW + static_cast<float>(timeToContentX(st, st->nowUtc) - st->scrollX);
    if (nowX >= colW && nowX < cw) fill(nowX, hdrH, static_cast<float>(dpx(st->dpi, 2)), ch - hdrH, th.accent);
    rt->PopAxisAlignedClip();

    // ---- frozen hour axis (top)
    rt->PushAxisAlignedClip(D2D1::RectF(colW, 0, cw, hdrH), D2D1_ANTIALIAS_MODE_ALIASED);
    fill(colW, 0, cw - colW, hdrH, th.panelElevBg);
    for (long long h = st->originUtc; h <= st->endUtc; h += 3600) {
        const float hx = colW + static_cast<float>(timeToContentX(st, h) - st->scrollX);
        if (hx < colW - 1 || hx >= cw) continue;
        fill(hx, 0, 1, hdrH, th.border);
        text(hm(h), st->fmtTime, hx + pad, 0, static_cast<float>(st->pxPerHour) - pad, hdrH,
             th.textSecondary);
    }
    rt->PopAxisAlignedClip();

    // ---- frozen channel column (left)
    rt->PushAxisAlignedClip(D2D1::RectF(0, hdrH, colW, ch), D2D1_ANTIALIAS_MODE_ALIASED);
    fill(0, hdrH, colW, ch - hdrH, th.panelElevBg);
    for (int r = std::max(0, firstRow); r < nr; ++r) {
        const float rowY = hdrH + static_cast<float>(r * st->rowH - st->scrollY);
        if (rowY >= ch) break;
        if (r == st->hoverRow) fill(0, rowY, colW, static_cast<float>(st->rowH), th.hoverBg);
        fill(0, rowY + static_cast<float>(st->rowH) - 1, colW, 1, th.border);
        text(st->rows[r].channelName, st->fmtChannel, pad, rowY, colW - pad * 2,
             static_cast<float>(st->rowH), th.textPrimary);
    }
    rt->PopAxisAlignedClip();

    // ---- corner: a channel search field (type to filter), drawn as a real input box so it's
    // obviously interactive — recessed fill + border + magnifier, accent-highlighted when active.
    fill(0, 0, colW, hdrH, th.panelElevBg);
    {
        const bool active = !st->filter.empty();
        const float fm = static_cast<float>(dpx(st->dpi, 4));  // field inset within the corner cell
        const auto rr = D2D1::RoundedRect(D2D1::RectF(fm, fm, colW - fm, hdrH - fm), 4.0f, 4.0f);
        st->brush->SetColor(colorToD2D(th.panelBg));           // recessed field background
        rt->FillRoundedRectangle(rr, st->brush);
        st->brush->SetColor(colorToD2D(active ? th.accent : th.border));  // accent border while searching
        rt->DrawRoundedRectangle(rr, st->brush, active ? 1.6f : 1.0f);
        // magnifier: a small ring + handle on the left of the field.
        const float mr = static_cast<float>(dpx(st->dpi, 5));
        const float mcx = fm + static_cast<float>(dpx(st->dpi, 9)), mcy = hdrH * 0.5f;
        st->brush->SetColor(colorToD2D(active ? th.accent : th.textSecondary));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mcx, mcy), mr, mr), st->brush, 1.3f);
        rt->DrawLine(D2D1::Point2F(mcx + mr * 0.75f, mcy + mr * 0.75f),
                     D2D1::Point2F(mcx + mr * 1.7f, mcy + mr * 1.7f), st->brush, 1.5f);
        // query text, or a dim placeholder when empty.
        const float tx = mcx + mr + static_cast<float>(dpx(st->dpi, 6));
        text(active ? st->filter : std::wstring(L"Search channels…"), st->fmtSub, tx,
             static_cast<float>(dpx(st->dpi, 8)), (colW - fm) - tx, static_cast<float>(dpx(st->dpi, 18)),
             active ? th.textPrimary : th.textSecondary);
    }
    fill(colW - 1, 0, 1, ch, th.border);   // column divider
    fill(0, hdrH - 1, cw, 1, th.border);   // header divider

    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) discardDevice(st);
}

// ---- interaction -----------------------------------------------------------

int rowAtY(HWND hwnd, GuideState* st, int y) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (y < st->headerH || y >= rc.bottom) return -1;
    const int idx = (y - st->headerH + st->scrollY) / st->rowH;
    return (idx >= 0 && idx < static_cast<int>(st->rows.size())) ? idx : -1;
}

// The programme block at a client point (or nullptr); *rowOut gets its channel row.
const GuideProgramme* programmeAt(HWND hwnd, GuideState* st, int x, int y, int* rowOut) {
    if (x < st->channelColW) return nullptr;  // channel column / header
    const int r = rowAtY(hwnd, st, y);
    if (r < 0) return nullptr;
    const long long t =
        st->originUtc + static_cast<long long>(x - st->channelColW + st->scrollX) * 3600 / st->pxPerHour;
    for (const GuideProgramme& p : st->rows[r].programmes)
        if (p.startUtc <= t && t < p.stopUtc) {
            if (rowOut) *rowOut = r;
            return &p;
        }
    return nullptr;
}

void onClick(HWND hwnd, GuideState* st, int x, int y) {
    int r = -1;
    const GuideProgramme* p = programmeAt(hwnd, st, x, y, &r);
    if (!p) return;
    // Snapshot the fields before the modal below pumps messages (p may be invalidated).
    const std::wstring channelId = st->rows[r].channelId, channelName = st->rows[r].channelName,
                       title = p->title;
    const long long startUtc = p->startUtc, stopUtc = p->stopUtc;
    std::wstring info = channelName + L"\r\n" + hm(startUtc) + L" – " + hm(stopUtc) + L"\r\n";
    if (!p->descr.empty()) info += L"\r\n" + p->descr;

    const ProgrammeAction act = programmeDialog(hwnd, st->hInst, st->dpi, title, info);
    if (act == ProgrammeAction::Play && st->cb.onPlay)
        st->cb.onPlay(channelId, channelName);
    else if (act == ProgrammeAction::Schedule && st->cb.onSchedule)
        st->cb.onSchedule(channelId, channelName, title, startUtc, stopUtc);
}

LRESULT CALLBACK GuideProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* st = new GuideState();
        st->dpi = GetDpiForWindow(hwnd);
        computeMetrics(st);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    GuideState* st = stateOf(hwnd);
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
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (LOWORD(wParam) & MK_SHIFT)
                st->scrollX -= (delta / WHEEL_DELTA) * st->pxPerHour / 2;  // Shift+wheel = time
            else
                st->scrollY -= (delta / WHEEL_DELTA) * st->rowH * 3;
            clampScroll(hwnd, st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            const int page = viewH(hwnd, st);
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
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_HSCROLL: {
            const int page = viewW(hwnd, st);
            SCROLLINFO si{sizeof(si)};
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_HORZ, &si);
            switch (LOWORD(wParam)) {
                case SB_LINELEFT: st->scrollX -= st->pxPerHour / 4; break;
                case SB_LINERIGHT: st->scrollX += st->pxPerHour / 4; break;
                case SB_PAGELEFT: st->scrollX -= page; break;
                case SB_PAGERIGHT: st->scrollX += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: st->scrollX = si.nTrackPos; break;
            }
            clampScroll(hwnd, st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!st->tracking) {  // arm one-shot leave tracking so the hover can clear
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                st->tracking = true;
            }
            const int r = rowAtY(hwnd, st, GET_Y_LPARAM(lParam));
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
            onClick(hwnd, st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_CHAR: {
            // Type-to-search: the corner cell shows the query; filtering rebuilds st->rows,
            // which paint + hit-testing already iterate. Esc (in WM_KEYDOWN) clears, then closes.
            const wchar_t c = static_cast<wchar_t>(wParam);
            if (c == VK_BACK) {
                if (st->filter.empty()) return 0;
                st->filter.pop_back();
            } else if (c >= L' ' && c != 0x7F) {
                st->filter.push_back(c);
            } else {
                return 0;  // Enter/Tab/Esc and other control chars aren't search input
            }
            applyFilter(st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (!st->filter.empty()) {  // first Esc clears the search, second closes the guide
                    st->filter.clear();
                    applyFilter(st);
                    updateScrollbars(hwnd, st);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    DestroyWindow(hwnd);
                }
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            st->dpi = HIWORD(wParam);
            computeMetrics(st);
            recreateFormats(st);
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_NCDESTROY:
            if (g_guide == hwnd) g_guide = nullptr;
            discardDevice(st);
            releaseFormats(st);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerGuideClass(HINSTANCE hInst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = GuideProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));  // taskbar / Alt-Tab icon
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

// Fold `rows` + `nowUtc` into `st`, deriving the visible time span. Empty rows / no
// programmes degrade to a one-day window around "now" so the axis still renders.
void applyData(GuideState* st, std::vector<GuideRow> rows, long long nowUtc) {
    st->allRows = std::move(rows);
    st->filter.clear();  // fresh data -> fresh search
    st->nowUtc = nowUtc;
    long long lo = 0, hi = 0;
    bool any = false;
    for (const auto& row : st->allRows)  // span from ALL rows so filtering never moves the axis
        for (const auto& p : row.programmes) {
            if (p.startUtc == 0) continue;
            // A missing/degenerate stop (stopUtc <= start) still contributes its start, so a
            // feed of unknown-length programmes can't push `hi` below `lo` (L2).
            const long long end = std::max(p.stopUtc, p.startUtc);
            if (!any || p.startUtc < lo) lo = p.startUtc;
            if (end > hi) hi = end;
            any = true;
        }
    if (!any) {
        lo = nowUtc;
        hi = nowUtc + 24 * 3600;
    }
    st->originUtc = lo - (lo % 3600);                       // floor to the hour
    st->endUtc = hi + ((3600 - (hi % 3600)) % 3600);        // ceil to the hour
    if (st->endUtc <= st->originUtc) st->endUtc = st->originUtc + 24 * 3600;  // guarantee a span
    st->scrollX = 0;
    st->scrollY = 0;
    applyFilter(st);  // populate st->rows from st->allRows (filter was just cleared)
}

}  // namespace

void hideEpgGuide() {
    if (g_guide && IsWindow(g_guide)) ShowWindow(g_guide, SW_HIDE);
}

void showEpgGuide(HWND owner, HINSTANCE hInst, UINT dpi, std::vector<GuideRow> rows, long long nowUtc,
                  GuideCallbacks cb) {
    registerGuideClass(hInst);
    if (g_guide && IsWindow(g_guide)) {  // already open — repopulate + focus
        GuideState* st = stateOf(g_guide);
        if (st) {
            st->cb = std::move(cb);
            applyData(st, std::move(rows), nowUtc);
            // Start scrolled so "now" sits a little in from the left edge.
            st->scrollX = std::max(0, timeToContentX(st, nowUtc) - dpx(st->dpi, 80));
            updateScrollbars(g_guide, st);
            InvalidateRect(g_guide, nullptr, FALSE);
        }
        applyDialogDarkMode(g_guide);  // re-theme the caption in case the skin changed
        ShowWindow(g_guide, SW_SHOW);  // re-reveal if a play-from-guide had hidden it
        SetForegroundWindow(g_guide);
        return;
    }
    const int w = dpx(dpi, 1100), h = dpx(dpi, 680);
    HWND hwnd = CreateWindowExW(0, kClass, L"TV Guide — RabbitEars",
                                WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL, CW_USEDEFAULT,
                                CW_USEDEFAULT, w, h, owner, nullptr, hInst, nullptr);
    if (!hwnd) return;
    g_guide = hwnd;
    if (GuideState* st = stateOf(hwnd)) {
        st->hInst = hInst;
        st->cb = std::move(cb);
        st->dpi = GetDpiForWindow(hwnd);
        computeMetrics(st);
        recreateFormats(st);
        applyData(st, std::move(rows), nowUtc);
        st->scrollX = std::max(0, timeToContentX(st, nowUtc) - dpx(st->dpi, 80));
        updateScrollbars(hwnd, st);
    }
    applyDialogDarkMode(hwnd);  // dark/immersive caption + themed border (matches the app chrome)
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

}  // namespace rabbitears
