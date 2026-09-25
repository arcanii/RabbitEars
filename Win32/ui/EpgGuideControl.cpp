// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/EpgGuideControl.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <vector>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>  // SetWindowSubclass, EM_SETCUEBANNER

#include "resource.h"     // IDI_APPICON
#include "core/SearchFold.h"  // channel-name matching folds like the programme search's marking
#include "ui/D2DSupport.h"
#include "ui/Dialogs.h"   // showInfoDialog, programmeDialog
#include "ui/Theme.h"     // currentTheme + applyDialogDarkMode + dialogCtlColor + themeFont
#include "ui/Tr.h"        // tr / trf

// windows.h maps DrawText -> DrawTextW and d2d1.h declares ID2D1RenderTarget::DrawText
// through that same macro, so rt->DrawText(...) resolves to DrawTextW consistently
// (matching ChannelGridControl).

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReEpgGuide";

// The single live guide window (one at a time; re-opening repopulates it).
HWND g_guide = nullptr;

// The child control + timers. The window has no other children or timers.
constexpr int  kIdSearch = 102;  // the toolbar's search box (EDIT) — channels and programmes
constexpr UINT_PTR kSearchTimer = 1;  // 200 ms debounce of the search, like the main search
constexpr UINT_PTR kFlashTimer = 2;   // ends the highlight on a block a search result jumped to
constexpr UINT kSearchDebounceMs = 200;
constexpr UINT kFlashMs = 2500;

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

// A line in the search-results list. Top to bottom: the matching channels (a Section heading and the
// one Channels item that narrows the grid to them), then the programmes (a Section heading with the
// count, a Heading per day, a Hit per programme), then the description-only programmes under a
// Section of their own, their days starting again beneath it.
enum class ItemKind {
    Heading,   // a day ("Today", "Tomorrow", a date)
    Section,   // a block's heading, in the accent: channels, programmes found (or none, or "Preparing
               // search…" alone), "Found in the description"
    Channels,  // "Show only channels matching …" — choosing it applies the channel filter
    Hit,       // one programme
};
struct ResultItem {
    ItemKind     kind = ItemKind::Hit;
    std::wstring label;  // Heading / Section text
    int          hit = -1;  // Hit: index into GuideState::hits
    int          y = 0, h = 0;  // content-space position within the list
};
bool selectable(const ResultItem& it) { return it.kind == ItemKind::Channels || it.kind == ItemKind::Hit; }

struct GuideState {
    std::vector<GuideRow> rows;     // the filtered view everything paints + hit-tests
    std::vector<GuideRow> allRows;  // the full set; `rows` is this filtered by `filter`
    std::wstring filter;            // the channel filter a search's Channels item applied ("" = none)
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
    int toolbarH = 40;  // the strip above the grid: the search box + Now
    int channelColW = 200;
    int  pxPerHour = 150;
    int  hoverRow = -1;
    bool tracking = false;  // WM_MOUSELEAVE armed? (one-shot TrackMouseEvent)

    // ---- the child control
    HWND  hSearch = nullptr;
    HWND  lastFocus = nullptr;        // the child that had focus when the guide was deactivated
    bool  inRebuild = false;          // jumpToHit's onRebuild is re-entering showEpgGuide
    bool  swallowedDown = false;      // a results click was ignored (pending search); drop its dblclk
    HFONT editFont = nullptr;
    RECT  searchField{}, nowButton{};  // painted frames (client coords)
    RECT  chip{};                     // the corner cell's channel-filter chip ("toronto ✕"), while filtering
    bool  nowHover = false, chipHover = false;
    int   themedDark = -1;            // the skin darkness themeGuideChrome last applied (-1 = none yet)

    // ---- search
    bool searchPending = false;   // debounce armed (KillTimer can't unpost a queued WM_TIMER)
    bool sessionReady = false;    // onSearchBegin ran for this burst of typing
    bool preparing = false;       // painting "Preparing search…" while onSearchBegin runs
    bool showResults = false;     // the results list replaces the grid
    std::wstring searchedText;    // what `hits` and the channel matches answer
    std::vector<GuideSearchHit> hits;
    bool hitsTruncated = false;
    int  channelCount = 0;        // guide rows whose channel name matches searchedText
    std::wstring channelNames;    // the first of them, " · "-joined, for the Channels item
    std::vector<ResultItem> items;
    int  resultsH = 0;            // content height of `items`
    int  resultsScrollY = 0;
    int  resultSel = -1;          // index into items (a selectable one), -1 = none
    int  resultHover = -1;
    int  resultHdrH = 28;
    int  resultRowH = 46;         // two lines: a title match, the Channels item
    int  resultRow3H = 62;        // three: a description match with a snippet (threeLines)
    // Rows vs results: a jump may rebuild the rows (onRebuild) only if they have not been rebuilt
    // since the results were fetched — once per set of results, however often the user clicks.
    int rowsGeneration = 0;   // bumped by applyData
    int hitsGeneration = -1;  // rowsGeneration when `hits` were fetched
    // "Click jumps; double-click opens the dialog" (owner, EPG_SEARCH.md §7.3): a LEFT click on a
    // result jumps at once, so a double-click's second click lands on the grid — remember what that
    // click jumped to, so a WM_LBUTTONDBLCLK within the system's double-click time can open it.
    GuideSearchHit jumped;
    bool           jumpedValid = false;
    DWORD          jumpedTick = 0;
    // A left click on the Channels item likewise leaves the list at once; the second half of a
    // double-click on it lands on the grid and must not open whatever programme is under the cursor.
    bool           filteredByClick = false;
    DWORD          filteredTick = 0;
    int            flashRow = -1;       // block highlighted after a jump
    long long      flashStart = 0;

    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush*  brush = nullptr;
    ID2D1SolidColorBrush*  markBrush = nullptr;   // matched text in the results
    IDWriteTextFormat*     fmtChannel = nullptr;  // channel names (frozen column)
    IDWriteTextFormat*     fmtProg = nullptr;     // programme title
    IDWriteTextFormat*     fmtSub = nullptr;      // programme time range
    IDWriteTextFormat*     fmtTime = nullptr;     // hour-axis labels
};

GuideState* stateOf(HWND h) { return reinterpret_cast<GuideState*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void computeMetrics(GuideState* st) {
    st->rowH = dpx(st->dpi, 44);
    st->headerH = dpx(st->dpi, 34);
    st->toolbarH = dpx(st->dpi, 40);
    st->channelColW = dpx(st->dpi, 200);
    st->pxPerHour = dpx(st->dpi, 150);
    st->resultHdrH = dpx(st->dpi, 28);
    st->resultRowH = dpx(st->dpi, 46);
    st->resultRow3H = dpx(st->dpi, 62);
}

int gridTop(const GuideState* st) { return st->toolbarH + st->headerH; }

// `s` through searchFold: case and Latin accents folded, one character for one.
std::wstring foldCopy(const std::wstring& s) {
    std::wstring o(s);
    for (wchar_t& c : o) c = searchFold(c);
    return o;
}

// The one channel-name match, for the results' "Matching channels" count AND the filter the
// Channels item applies, so the two always agree: a substring, case- and accent-insensitive
// ("quebec" finds "TVA QUÉBEC"). `foldedNeedle` = foldCopy(what was typed).
bool channelMatches(const GuideRow& row, const std::wstring& foldedNeedle) {
    return foldCopy(row.channelName).find(foldedNeedle) != std::wstring::npos;
}

// `s` with every channelMatches-style occurrence of `foldedNeedle` wrapped in U+0002 … U+0003, for
// drawMarked (searchFold is one character for one, so folded positions are positions in `s`).
std::wstring markFolded(const std::wstring& s, const std::wstring& foldedNeedle) {
    if (foldedNeedle.empty()) return s;
    const std::wstring f = foldCopy(s);
    std::wstring o;
    size_t i = 0;
    for (size_t at; (at = f.find(foldedNeedle, i)) != std::wstring::npos; i = at + foldedNeedle.size()) {
        o.append(s, i, at - i);
        o += L'\x02';
        o.append(s, at, foldedNeedle.size());
        o += L'\x03';
    }
    return o.append(s, i, std::wstring::npos);
}

// The same normalisation onEpgGuide builds rows with: strip an '@feed' suffix, ASCII-lowercase.
std::wstring normId(std::wstring s) {
    if (const size_t at = s.find(L'@'); at != std::wstring::npos) s.resize(at);
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return s;
}

std::wstring editText(HWND edit) {
    if (!edit) return L"";
    const int n = GetWindowTextLengthW(edit);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

// Spaces off both ends — U+3000 too, the ideographic space a Japanese IME's Space types.
std::wstring trimmed(const std::wstring& s) {
    const size_t b = s.find_first_not_of(L" \t\r\n\u3000");
    if (b == std::wstring::npos) return L"";
    return s.substr(b, s.find_last_not_of(L" \t\r\n\u3000") - b + 1);
}

// Set the channel filter and rebuild the visible `rows` from `allRows` by it. Everything downstream
// (paint, hit-testing, scrollbars) iterates `rows`, so this is all filtering needs.
void setFilter(GuideState* st, const std::wstring& text) {
    st->filter = text;
    if (st->filter.empty()) {
        st->rows = st->allRows;
    } else {
        const std::wstring needle = foldCopy(st->filter);
        st->rows.clear();
        for (const GuideRow& row : st->allRows)
            if (channelMatches(row, needle)) st->rows.push_back(row);
    }
    st->scrollY = 0;  // jump to the top of the filtered list
    st->flashRow = -1;  // row indices just changed
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

// The search box's font — rebuilt with the formats (DPI change, live UI-language change).
void recreateEditFont(GuideState* st) {
    HFONT old = st->editFont;
    st->editFont = themeFont(FontRole::Body, st->dpi, 13, FW_NORMAL);
    if (st->hSearch) SendMessageW(st->hSearch, WM_SETFONT, reinterpret_cast<WPARAM>(st->editFont), TRUE);
    if (old) DeleteObject(old);
}

void discardDevice(GuideState* st) {
    SafeRelease(st->markBrush);
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
    st->rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &st->markBrush);
    return st->brush != nullptr && st->markBrush != nullptr;
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
    return std::max(0, static_cast<int>(rc.bottom) - gridTop(st));
}
int resultsViewH(HWND hwnd, GuideState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    return std::max(0, static_cast<int>(rc.bottom) - st->toolbarH);
}

void clampScroll(HWND hwnd, GuideState* st) {
    st->scrollX = std::clamp(st->scrollX, 0, std::max(0, contentWidth(st) - viewW(hwnd, st)));
    st->scrollY = std::clamp(st->scrollY, 0, std::max(0, contentHeight(st) - viewH(hwnd, st)));
    st->resultsScrollY =
        std::clamp(st->resultsScrollY, 0, std::max(0, st->resultsH - resultsViewH(hwnd, st)));
}

void updateScrollbars(HWND hwnd, GuideState* st) {
    clampScroll(hwnd, st);
    SCROLLINFO sh{sizeof(sh)};
    sh.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    sh.nMin = 0;
    // The results list does not scroll sideways: an empty range hides the bar while it shows.
    sh.nMax = st->showResults ? 0 : std::max(0, contentWidth(st) - 1);
    sh.nPage = st->showResults ? 1u : static_cast<UINT>(viewW(hwnd, st));
    sh.nPos = st->showResults ? 0 : st->scrollX;
    SetScrollInfo(hwnd, SB_HORZ, &sh, TRUE);
    SCROLLINFO sv{sizeof(sv)};
    sv.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    sv.nMin = 0;
    if (st->showResults) {
        sv.nMax = std::max(0, st->resultsH - 1);
        sv.nPage = static_cast<UINT>(resultsViewH(hwnd, st));
        sv.nPos = st->resultsScrollY;
    } else {
        sv.nMax = std::max(0, contentHeight(st) - 1);
        sv.nPage = static_cast<UINT>(viewH(hwnd, st));
        sv.nPos = st->scrollY;
    }
    SetScrollInfo(hwnd, SB_VERT, &sv, TRUE);
}

// Place the search EDIT inside its painted frame, the (painted) Now button, and the corner cell's
// (painted) channel-filter chip.
void layoutChildren(HWND hwnd, GuideState* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int m = dpx(st->dpi, 6), fieldH = st->toolbarH - 2 * m;
    const int nowW = dpx(st->dpi, 64);
    const int searchW = std::clamp(static_cast<int>(rc.right) - nowW - 4 * m, dpx(st->dpi, 120),
                                   dpx(st->dpi, 440));
    st->searchField = {m, m, m + searchW, m + fieldH};
    st->nowButton = {st->searchField.right + m, m, st->searchField.right + m + nowW, m + fieldH};
    const int fm = dpx(st->dpi, 5);
    st->chip = {fm, st->toolbarH + fm, st->channelColW - fm, gridTop(st) - fm};
    // The text sits right of the painted magnifier; the EDIT is borderless, vertically centred, as
    // tall as its font needs (a CJK UI face is taller than Segoe UI at the same pixel size).
    int editH = dpx(st->dpi, 18);
    if (st->editFont) {
        if (HDC dc = GetDC(hwnd)) {
            HGDIOBJ old = SelectObject(dc, st->editFont);
            TEXTMETRICW tm{};
            if (GetTextMetricsW(dc, &tm)) editH = tm.tmHeight + dpx(st->dpi, 2);
            SelectObject(dc, old);
            ReleaseDC(hwnd, dc);
        }
    }
    const int textInset = dpx(st->dpi, 24);
    auto place = [&](HWND e, const RECT& f) {
        if (!e) return;
        const int h = std::min(editH, static_cast<int>(f.bottom - f.top) - 2);  // inside THIS frame
        const int y = f.top + ((f.bottom - f.top) - h) / 2;
        SetWindowPos(e, nullptr, f.left + textInset, y,
                     std::max(0, static_cast<int>(f.right - f.left) - textInset - dpx(st->dpi, 6)), h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(st->hSearch, st->searchField);
}

// ---- search results -----------------------------------------------------------

// Local-calendar-day index of an epoch (for "Today" / "Tomorrow" grouping).
long long localDay(long long epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    return static_cast<long long>(tmv.tm_year) * 400 + tmv.tm_yday;
}

std::wstring dayLabel(long long epoch, long long nowUtc) {
    const long long d = localDay(epoch), today = localDay(nowUtc);
    if (d == today) return tr(i18n::StringId::GuideSearchToday);
    if (d == localDay(nowUtc + 24 * 3600)) return tr(i18n::StringId::GuideSearchTomorrow);
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    SYSTEMTIME sys{};
    sys.wYear = static_cast<WORD>(tmv.tm_year + 1900);
    sys.wMonth = static_cast<WORD>(tmv.tm_mon + 1);
    sys.wDay = static_cast<WORD>(tmv.tm_mday);
    sys.wDayOfWeek = static_cast<WORD>(tmv.tm_wday);
    wchar_t b[64] = L"";
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &sys, nullptr, b, 64, nullptr))
        return hm(epoch);
    return b;
}

// A description match shows its snippet as a third line; everything else selectable has two.
bool threeLines(const GuideSearchHit& g) { return !g.inTitle && !g.snippet.empty(); }

// Lay the results out as a flat list (see ItemKind): the matching channels first, then the
// programmes under their count, a heading per local day. The hits arrive title matches first, then
// description-only matches (each soonest first), so the second block gets a heading of its own and its
// days start again under it. A programme already airing is filed under TODAY even if it began before
// midnight. While "Preparing search…" shows, that is the whole list.
void buildItems(GuideState* st) {
    // The selection names an ITEM; a rebuild of the same results (a DPI or language change) can add
    // or drop headings, so carry it over by what it pointed at. (runSearch, with new results, then
    // sets its own selection.)
    ItemKind selKind = ItemKind::Heading;  // = none
    int selHit = -1;
    if (st->resultSel >= 0 && st->resultSel < static_cast<int>(st->items.size())) {
        selKind = st->items[static_cast<size_t>(st->resultSel)].kind;
        selHit = st->items[static_cast<size_t>(st->resultSel)].hit;
    }
    st->items.clear();
    int y = 0;
    auto add = [&](ItemKind kind, std::wstring label, int hit, int h) {
        ResultItem it;
        it.kind = kind;
        it.label = std::move(label);
        it.hit = hit;
        it.y = y;
        it.h = h;
        y += h;
        st->items.push_back(std::move(it));
    };
    if (st->preparing) {
        add(ItemKind::Section, tr(i18n::StringId::GuideSearchPreparing), -1, st->resultHdrH);
    } else {
        if (st->channelCount > 0) {
            add(ItemKind::Section, trf(i18n::StringId::GuideSearchChannelsHeading, {std::to_wstring(st->channelCount)}),
                -1, st->resultHdrH);
            add(ItemKind::Channels, L"", -1, st->resultRowH);
        }
        std::wstring status;
        if (st->hits.empty()) status = trf(i18n::StringId::GuideSearchNone, {st->searchedText});
        else if (st->hitsTruncated)
            status = trf(i18n::StringId::GuideSearchShowingFirst, {std::to_wstring(st->hits.size())});
        else status = trf(i18n::StringId::GuideSearchCount, {std::to_wstring(st->hits.size())});
        add(ItemKind::Section, std::move(status), -1, st->resultHdrH);
    }
    long long lastDay = -1;
    bool descrBlock = false;
    for (size_t i = 0; !st->preparing && i < st->hits.size(); ++i) {
        const GuideSearchHit& g = st->hits[i];
        if (!g.inTitle && !descrBlock) {
            add(ItemKind::Section, tr(i18n::StringId::GuideSearchInDescriptions), -1, st->resultHdrH);
            descrBlock = true;
            lastDay = -1;
        }
        const long long when = std::max(g.startUtc, st->nowUtc);
        const long long d = localDay(when);
        if (d != lastDay) {
            add(ItemKind::Heading, dayLabel(when, st->nowUtc), -1, st->resultHdrH);
            lastDay = d;
        }
        add(ItemKind::Hit, L"", static_cast<int>(i), threeLines(g) ? st->resultRow3H : st->resultRowH);
    }
    st->resultsH = y;
    st->resultHover = -1;  // the items moved under the cursor; the next WM_MOUSEMOVE finds it again
    st->resultSel = -1;
    for (size_t i = 0; i < st->items.size(); ++i) {
        const ResultItem& it = st->items[i];
        if ((selKind == ItemKind::Channels && it.kind == ItemKind::Channels) ||
            (selKind == ItemKind::Hit && it.kind == ItemKind::Hit && it.hit == selHit))
            st->resultSel = static_cast<int>(i);
    }
}

int firstSelectable(const GuideState* st) {
    for (size_t i = 0; i < st->items.size(); ++i)
        if (selectable(st->items[i])) return static_cast<int>(i);
    return -1;
}

// The selected item, if it is a programme; nullptr otherwise.
const GuideSearchHit* selectedHit(const GuideState* st) {
    if (st->resultSel < 0 || st->resultSel >= static_cast<int>(st->items.size())) return nullptr;
    const ResultItem& it = st->items[static_cast<size_t>(st->resultSel)];
    return it.kind == ItemKind::Hit ? &st->hits[static_cast<size_t>(it.hit)] : nullptr;
}

int itemAtY(HWND hwnd, GuideState* st, int y) {
    if (y < st->toolbarH || y >= st->toolbarH + resultsViewH(hwnd, st)) return -1;
    const int cy = y - st->toolbarH + st->resultsScrollY;
    for (size_t i = 0; i < st->items.size(); ++i)
        if (selectable(st->items[i]) && cy >= st->items[i].y && cy < st->items[i].y + st->items[i].h)
            return static_cast<int>(i);
    return -1;
}

// Keep the selected result visible after a keyboard move.
// `movingUp`: the selection just moved up — then the headings directly above it (a day, a block's own
// heading — for the list's first item, everything above it) come into view with it, even when the
// item itself was already visible.
void revealSelection(HWND hwnd, GuideState* st, bool movingUp = false) {
    if (st->resultSel < 0 || st->resultSel >= static_cast<int>(st->items.size())) return;
    const ResultItem& it = st->items[static_cast<size_t>(st->resultSel)];
    const int vh = resultsViewH(hwnd, st);
    size_t k = static_cast<size_t>(st->resultSel);
    while (k > 0 && !selectable(st->items[k - 1])) --k;
    const int blockTop = (k == 0) ? 0 : st->items[k].y;
    if (it.y < st->resultsScrollY || (movingUp && blockTop < st->resultsScrollY)) {
        st->resultsScrollY = blockTop;
    } else if (it.y + it.h > st->resultsScrollY + vh) {
        st->resultsScrollY = it.y + it.h - vh;
    }
}

void moveSelection(HWND hwnd, GuideState* st, int delta) {
    if (st->items.empty()) return;
    int i = st->resultSel < 0 ? firstSelectable(st) : st->resultSel;
    if (i < 0) return;
    const int step = delta > 0 ? 1 : -1;
    for (int left = std::abs(delta); left > 0;) {
        int j = i + step;
        while (j >= 0 && j < static_cast<int>(st->items.size()) && !selectable(st->items[static_cast<size_t>(j)]))
            j += step;
        if (j < 0 || j >= static_cast<int>(st->items.size())) break;
        i = j;
        --left;
    }
    st->resultSel = i;
    revealSelection(hwnd, st, /*movingUp=*/delta < 0);
    updateScrollbars(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Leaving the results for the grid must also drop a search still waiting in the debounce, or its
// tick would bring the list straight back over the grid.
void cancelPendingSearch(HWND hwnd, GuideState* st) {
    KillTimer(hwnd, kSearchTimer);
    st->searchPending = false;
}

void setShowResults(HWND hwnd, GuideState* st, bool show) {
    if (st->showResults == show) return;
    st->showResults = show;
    st->resultHover = -1;
    layoutChildren(hwnd, st);
    updateScrollbars(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// The channels for `text`: the guide's rows whose name matches — what the Channels item would narrow
// the grid to (setFilter applies the same channelMatches). Counted and listed by NAME: a channel in
// two playlists has two rows (and both show when filtered) but is one channel to the user. The first
// few names are listed on the item, the typed text marked in them.
void findChannels(GuideState* st, const std::wstring& text) {
    constexpr int kNamesListed = 12;
    const std::wstring needle = foldCopy(text);
    std::unordered_set<std::wstring> seen;
    st->channelCount = 0;
    st->channelNames.clear();
    if (needle.empty()) return;  // "" is in every name
    for (const GuideRow& row : st->allRows) {
        if (!channelMatches(row, needle) || !seen.insert(row.channelName).second) continue;
        if (st->channelCount < kNamesListed) {
            if (!st->channelNames.empty()) st->channelNames += L"  ·  ";
            st->channelNames += markFolded(row.channelName, needle);
        } else if (st->channelCount == kNamesListed) {
            st->channelNames += L"  ·  …";
        }
        ++st->channelCount;
    }
}

// Run the search for what the box says now: the guide's own channel names (in memory), and the
// programmes (the host's database search).
void runSearch(HWND hwnd, GuideState* st) {
    const std::wstring text = trimmed(editText(st->hSearch));
    if (text.empty()) {
        st->hits.clear();
        st->channelCount = 0;
        st->channelNames.clear();
        st->items.clear();
        st->resultsH = 0;
        st->searchedText.clear();
        st->sessionReady = false;  // the next burst re-reads the channel set (playlists may change)
        setShowResults(hwnd, st, false);
        return;
    }
    if (!st->sessionReady && st->cb.onSearchBegin) {
        // Say so before the host's (possibly seconds-long) index rebuild blocks this thread.
        st->preparing = true;
        st->hits.clear();
        st->channelCount = 0;
        st->channelNames.clear();
        buildItems(st);  // "Preparing search…" alone
        st->resultsScrollY = 0;
        st->showResults = false;
        setShowResults(hwnd, st, true);
        UpdateWindow(hwnd);
        auto begin = st->cb.onSearchBegin;  // a copy, defensively: a host could re-enter showEpgGuide
        begin();
        st->preparing = false;
    }
    st->sessionReady = true;
    findChannels(st, text);
    st->hitsTruncated = false;
    // The host searches from the current time; the badge and the day headings must agree with it
    // (st->nowUtc otherwise dates from whenever the guide was built or revealed).
    st->nowUtc = static_cast<long long>(time(nullptr));
    st->hits = st->cb.onSearch ? st->cb.onSearch(text, &st->hitsTruncated) : std::vector<GuideSearchHit>{};
    st->hitsGeneration = st->rowsGeneration;
    st->searchedText = text;
    buildItems(st);
    st->resultsScrollY = 0;
    st->resultSel = firstSelectable(st);  // the Channels item when channels match: type + Enter filters
    st->showResults = false;
    setShowResults(hwnd, st, true);
}

// ---- marked text -------------------------------------------------------------------

// Draw `marked` (matches wrapped in U+0002 … U+0003) in one line, the matches in the accent colour
// and bold, the rest in `base`; trimmed with an ellipsis at the right edge.
void drawMarked(GuideState* st, const std::wstring& marked, IDWriteTextFormat* fmt, float x, float y,
                float w, float h, COLORREF base, COLORREF mark) {
    if (marked.empty() || !fmt || w <= 0) return;
    std::wstring plain;
    std::vector<DWRITE_TEXT_RANGE> ranges;
    UINT32 open = 0;
    bool inMark = false;
    for (wchar_t c : marked) {
        if (c == L'\x02') {
            open = static_cast<UINT32>(plain.size());
            inMark = true;
        } else if (c == L'\x03') {
            if (inMark && plain.size() > open)
                ranges.push_back({open, static_cast<UINT32>(plain.size()) - open});
            inMark = false;
        } else {
            plain += c;
        }
    }
    IDWriteFactory* dw = dwriteFactory();
    IDWriteTextLayout* layout = nullptr;
    if (!dw || FAILED(dw->CreateTextLayout(plain.c_str(), static_cast<UINT32>(plain.size()), fmt, w, h,
                                           &layout)) || !layout)
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* ellipsis = nullptr;
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt, &ellipsis)) && ellipsis) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trim, ellipsis);
    }
    st->markBrush->SetColor(colorToD2D(mark));
    for (const auto& r : ranges) {
        layout->SetDrawingEffect(st->markBrush, r);
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
    }
    st->brush->SetColor(colorToD2D(base));
    st->rt->DrawTextLayout(D2D1::Point2F(x, y), layout, st->brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    SafeRelease(ellipsis);
    SafeRelease(layout);
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
    const float tbH = static_cast<float>(st->toolbarH);
    const float hdrTop = tbH;                                       // the hour axis starts here
    const float hdrH = static_cast<float>(st->headerH);
    const float gridY = tbH + hdrH;                                 // the first channel row

    auto fill = [&](float x, float y, float w, float h, COLORREF c) {
        st->brush->SetColor(colorToD2D(c));
        rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), st->brush);
    };
    // Text in a box, vertically centred in it (every format here has paragraph alignment CENTER —
    // themeTextFormat), clipped to it; `centred` also centres it horizontally.
    auto text = [&](const std::wstring& s, IDWriteTextFormat* fmt, float x, float y, float w, float h,
                    COLORREF c, bool centred = false) {
        if (s.empty() || !fmt) return;
        st->brush->SetColor(colorToD2D(c));
        if (centred) fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt, D2D1::RectF(x, y, x + w, y + h),
                     st->brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (centred) fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    auto rectF = [](const RECT& r) {
        return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                           static_cast<float>(r.right), static_cast<float>(r.bottom));
    };
    // A search field: recessed fill in the EDIT's own colour (dialogCtlColor paints EDITs windowBg),
    // a border (accent while it holds text), and a magnifier left of the text.
    auto field = [&](const RECT& r, bool active) {
        const auto rr = D2D1::RoundedRect(rectF(r), 4.0f, 4.0f);
        st->brush->SetColor(colorToD2D(th.windowBg));
        rt->FillRoundedRectangle(rr, st->brush);
        st->brush->SetColor(colorToD2D(active ? th.accent : th.border));
        rt->DrawRoundedRectangle(rr, st->brush, active ? 1.6f : 1.0f);
        const float mr = static_cast<float>(dpx(st->dpi, 5));
        const float mcx = static_cast<float>(r.left + dpx(st->dpi, 11));
        const float mcy = static_cast<float>(r.top + r.bottom) * 0.5f - 1.0f;
        st->brush->SetColor(colorToD2D(active ? th.accent : th.textSecondary));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mcx, mcy), mr, mr), st->brush, 1.3f);
        rt->DrawLine(D2D1::Point2F(mcx + mr * 0.75f, mcy + mr * 0.75f),
                     D2D1::Point2F(mcx + mr * 1.7f, mcy + mr * 1.7f), st->brush, 1.5f);
    };

    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(colorToD2D(th.panelBg));

    if (st->showResults) {
        // ---- the results list (replaces the hour axis, the channel column and the grid)
        const float top = tbH;
        rt->PushAxisAlignedClip(D2D1::RectF(0, top, cw, ch), D2D1_ANTIALIAS_MODE_ALIASED);
        const float sy = static_cast<float>(st->resultsScrollY);
        const float lh1 = static_cast<float>(dpx(st->dpi, 19)), lh2 = static_cast<float>(dpx(st->dpi, 17));
        for (size_t i = 0; i < st->items.size(); ++i) {
            const ResultItem& it = st->items[i];
            const float y = top + static_cast<float>(it.y) - sy;
            const float h = static_cast<float>(it.h);
            if (y + h < top || y >= ch) continue;
            if (!selectable(it)) {
                // A block's heading stands apart from the day headings under it: accent, bolder.
                const bool section = it.kind == ItemKind::Section;
                fill(0, y, cw, h, th.panelElevBg);
                text(it.label, section ? st->fmtProg : st->fmtSub, pad * 2, y, cw - pad * 4, h,
                     section ? th.accent : th.textSecondary);
                continue;
            }
            const bool sel = static_cast<int>(i) == st->resultSel;
            const bool hov = static_cast<int>(i) == st->resultHover;
            fill(0, y, cw, h, sel ? th.selectionBg : (hov ? th.hoverBg : th.panelBg));
            fill(0, y + h - 1, cw, 1, th.border);
            const COLORREF tc = sel ? th.selectionText : th.textPrimary;
            const COLORREF sc = sel ? th.selectionText : th.textSecondary;
            const COLORREF mc = sel ? th.selectionText : th.accent;
            const float x = pad * 2;
            if (it.kind == ItemKind::Channels) {
                text(trf(i18n::StringId::GuideSearchFilterChannels, {st->searchedText}), st->fmtProg, x, y + pad,
                     cw - pad * 4, lh1, tc);
                drawMarked(st, st->channelNames, st->fmtSub, x, y + pad + lh1, cw - pad * 4, lh2, sc, mc);
                continue;
            }
            const GuideSearchHit& g = st->hits[static_cast<size_t>(it.hit)];
            const bool airing = g.startUtc <= st->nowUtc && st->nowUtc < g.stopUtc;
            float w = cw - pad * 4;
            if (airing) {  // "On now" badge at the right of the title line
                const float bw = static_cast<float>(dpx(st->dpi, 70));
                const float bx = cw - pad * 2 - bw;
                st->brush->SetColor(colorToD2D(th.accent));
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, y + pad, bx + bw, y + pad + lh1), 3.0f, 3.0f),
                                         st->brush);
                text(tr(i18n::StringId::GuideSearchOnNow), st->fmtSub, bx, y + pad, bw, lh1, th.accentText,
                     /*centred=*/true);
                w -= bw + pad;
            }
            drawMarked(st, g.markedTitle.empty() ? g.title : g.markedTitle, st->fmtProg, x, y + pad, w, lh1,
                       tc, mc);
            text(trf(i18n::StringId::GuideTimeRange, {hm(g.startUtc), hm(g.stopUtc)}) + L"  ·  " +
                     g.channelName,
                 st->fmtSub, x, y + pad + lh1, cw - pad * 4, lh2, sc);
            if (threeLines(g))
                drawMarked(st, g.snippet, st->fmtSub, x, y + pad + lh1 + lh2, cw - pad * 4, lh2, sc, mc);
        }
        rt->PopAxisAlignedClip();
    } else {
        const int nr = static_cast<int>(st->rows.size());
        const int firstRow = st->rowH > 0 ? st->scrollY / st->rowH : 0;

        // ---- programme area (clipped so scrolled blocks can't bleed into the frozen panes)
        rt->PushAxisAlignedClip(D2D1::RectF(colW, gridY, cw, ch), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int r = std::max(0, firstRow); r < nr; ++r) {
            const float rowY = gridY + static_cast<float>(r * st->rowH - st->scrollY);
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
                const bool flash = r == st->flashRow && p.startUtc == st->flashStart;
                const float by0 = rowY + 2.0f, by1 = rowY + static_cast<float>(st->rowH) - 2.0f;
                const auto rr = D2D1::RoundedRect(D2D1::RectF(bx0, by0, bx1, by1), 3.0f, 3.0f);
                st->brush->SetColor(colorToD2D(airing ? th.selectionBg : th.panelElevBg));
                rt->FillRoundedRectangle(rr, st->brush);
                st->brush->SetColor(colorToD2D((airing || flash) ? th.accent : th.border));
                rt->DrawRoundedRectangle(rr, st->brush, flash ? 3.0f : 1.0f);
                // Title + time, clipped to the block.
                const float tx = bx0 + pad, tw = bx1 - bx0 - pad * 2;
                if (tw > pad) {
                    const COLORREF tc = airing ? th.selectionText : th.textPrimary;
                    const COLORREF sc = airing ? th.selectionText : th.textSecondary;
                    text(p.title, st->fmtProg, tx, rowY + dpx(st->dpi, 5), tw,
                         static_cast<float>(dpx(st->dpi, 18)), tc);
                    text(trf(i18n::StringId::GuideTimeRange, {hm(p.startUtc), hm(p.stopUtc)}), st->fmtSub, tx,
                         rowY + static_cast<float>(dpx(st->dpi, 23)), tw, static_cast<float>(dpx(st->dpi, 16)),
                         sc);
                }
            }
        }
        // "Now" line.
        const float nowX = colW + static_cast<float>(timeToContentX(st, st->nowUtc) - st->scrollX);
        if (nowX >= colW && nowX < cw) fill(nowX, gridY, static_cast<float>(dpx(st->dpi, 2)), ch - gridY, th.accent);
        rt->PopAxisAlignedClip();

        // ---- frozen hour axis (top of the grid)
        rt->PushAxisAlignedClip(D2D1::RectF(colW, hdrTop, cw, gridY), D2D1_ANTIALIAS_MODE_ALIASED);
        fill(colW, hdrTop, cw - colW, hdrH, th.panelElevBg);
        for (long long h = st->originUtc; h <= st->endUtc; h += 3600) {
            const float hx = colW + static_cast<float>(timeToContentX(st, h) - st->scrollX);
            if (hx < colW - 1 || hx >= cw) continue;
            fill(hx, hdrTop, 1, hdrH, th.border);
            text(hm(h), st->fmtTime, hx + pad, hdrTop, static_cast<float>(st->pxPerHour) - pad, hdrH,
                 th.textSecondary);
        }
        rt->PopAxisAlignedClip();

        // ---- frozen channel column (left)
        rt->PushAxisAlignedClip(D2D1::RectF(0, gridY, colW, ch), D2D1_ANTIALIAS_MODE_ALIASED);
        fill(0, gridY, colW, ch - gridY, th.panelElevBg);
        for (int r = std::max(0, firstRow); r < nr; ++r) {
            const float rowY = gridY + static_cast<float>(r * st->rowH - st->scrollY);
            if (rowY >= ch) break;
            if (r == st->hoverRow) fill(0, rowY, colW, static_cast<float>(st->rowH), th.hoverBg);
            fill(0, rowY + static_cast<float>(st->rowH) - 1, colW, 1, th.border);
            text(st->rows[r].channelName, st->fmtChannel, pad, rowY, colW - pad * 2,
                 static_cast<float>(st->rowH), th.textPrimary);
        }
        rt->PopAxisAlignedClip();

        // ---- corner: while a channel filter is on, a chip naming it with an ✕ (a click clears it)
        fill(0, hdrTop, colW, hdrH, th.panelElevBg);
        if (!st->filter.empty()) {
            const D2D1_RECT_F cr = rectF(st->chip);
            const float chH = cr.bottom - cr.top;
            const auto rr = D2D1::RoundedRect(cr, chH * 0.5f, chH * 0.5f);
            st->brush->SetColor(colorToD2D(st->chipHover ? th.hoverBg : th.windowBg));
            rt->FillRoundedRectangle(rr, st->brush);
            st->brush->SetColor(colorToD2D(th.accent));
            rt->DrawRoundedRectangle(rr, st->brush, 1.4f);
            const float xw = chH;  // the ✕ sits in a square at the right end
            drawMarked(st, st->filter, st->fmtSub, cr.left + chH * 0.5f, cr.top, cr.right - cr.left - chH * 0.5f - xw,
                       chH, th.textPrimary, th.textPrimary);  // (nothing marked: for its ellipsis)
            const float xc = cr.right - xw * 0.5f, yc = (cr.top + cr.bottom) * 0.5f;
            const float xs = static_cast<float>(dpx(st->dpi, 4));
            st->brush->SetColor(colorToD2D(st->chipHover ? th.accent : th.textSecondary));
            rt->DrawLine(D2D1::Point2F(xc - xs, yc - xs), D2D1::Point2F(xc + xs, yc + xs), st->brush, 1.5f);
            rt->DrawLine(D2D1::Point2F(xc - xs, yc + xs), D2D1::Point2F(xc + xs, yc - xs), st->brush, 1.5f);
        }
        fill(colW - 1, hdrTop, 1, ch - hdrTop, th.border);  // column divider
        fill(0, gridY - 1, cw, 1, th.border);               // header divider
    }

    // ---- toolbar: the search box + Now (drawn last: always on top)
    fill(0, 0, cw, tbH, th.panelElevBg);
    field(st->searchField, !editText(st->hSearch).empty());
    {
        const D2D1_RECT_F nb = rectF(st->nowButton);
        const auto nr = D2D1::RoundedRect(nb, 4.0f, 4.0f);
        st->brush->SetColor(colorToD2D(st->nowHover ? th.hoverBg : th.panelBg));
        rt->FillRoundedRectangle(nr, st->brush);
        st->brush->SetColor(colorToD2D(th.border));
        rt->DrawRoundedRectangle(nr, st->brush, 1.0f);
        text(tr(i18n::StringId::GuideNowButton), st->fmtSub, nb.left, nb.top, nb.right - nb.left, nb.bottom - nb.top,
             th.textPrimary, /*centred=*/true);
    }
    fill(0, tbH - 1, cw, 1, th.border);  // toolbar divider

    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) discardDevice(st);
}

// ---- interaction -----------------------------------------------------------

int rowAtY(HWND hwnd, GuideState* st, int y) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (y < gridTop(st) || y >= rc.bottom) return -1;
    const int idx = (y - gridTop(st) + st->scrollY) / st->rowH;
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

// The programme popup (Play / Schedule / Record series) — shared by a grid click and a search result.
void openProgramme(HWND hwnd, GuideState* st, const std::wstring& channelId, const std::wstring& channelName,
                   const std::wstring& title, const std::wstring& descr, long long startUtc, long long stopUtc) {
    std::wstring info = channelName + L"\r\n" +
                        trf(i18n::StringId::GuideTimeRange, {hm(startUtc), hm(stopUtc)}) + L"\r\n";
    if (!descr.empty()) info += L"\r\n" + descr;
    const GuideCallbacks cb = st->cb;  // a copy: an action can re-enter showEpgGuide
    const ProgrammeAction act = programmeDialog(hwnd, st->hInst, st->dpi, title, info);
    if (act == ProgrammeAction::Play && cb.onPlay)
        cb.onPlay(channelId, channelName);
    else if (act == ProgrammeAction::Schedule && cb.onSchedule)
        cb.onSchedule(channelId, channelName, title, startUtc, stopUtc);
    else if (act == ProgrammeAction::RecordSeries && cb.onRecordSeries)
        cb.onRecordSeries(channelId, channelName, title);
}

void onClick(HWND hwnd, GuideState* st, int x, int y) {
    int r = -1;
    const GuideProgramme* p = programmeAt(hwnd, st, x, y, &r);
    if (!p) return;
    // Snapshot the fields before the modal below pumps messages (p may be invalidated).
    const std::wstring channelId = st->rows[r].channelId, channelName = st->rows[r].channelName,
                       title = p->title, descr = p->descr;
    const long long startUtc = p->startUtc, stopUtc = p->stopUtc;
    openProgramme(hwnd, st, channelId, channelName, title, descr, startUtc, stopUtc);
}

// Find a search hit in the grid's rows: its row index in `rows`, or -1 when the rows this window was
// built with do not contain it. A channel filter that shows the row stays on; one that hides it is
// cleared.
int rowOfHit(GuideState* st, const GuideSearchHit& h) {
    const std::wstring want = normId(h.channelId);
    auto holds = [&](const GuideRow& row) {
        if (row.channelId != h.channelId && normId(row.channelId) != want) return false;
        for (const GuideProgramme& p : row.programmes)
            if (p.startUtc == h.startUtc) return true;
        return false;
    };
    for (size_t i = 0; i < st->rows.size(); ++i)  // on show (the first such row: a channel in two
        if (holds(st->rows[i])) return static_cast<int>(i);  // playlists has two)
    for (size_t i = 0; i < st->allRows.size(); ++i)
        if (holds(st->allRows[i])) {
            // Filtered out: with the filter cleared, `rows` is `allRows` in the same order.
            setFilter(st, L"");
            return static_cast<int>(i);
        }
    return -1;
}

// "Click jumps": leave the results, scroll the grid to the programme and highlight it briefly.
// `byClick` = a left click on the result, the one a double-click's second click must be able to open.
void jumpToHit(HWND hwnd, GuideState* st, GuideSearchHit hit, bool byClick) {
    int row = rowOfHit(st, hit);
    // Not in the rows: rebuild them — re-entering showEpgGuide, which keeps the search box and
    // replaces the rows — but only when that could help: the programme lies inside the window a
    // rebuild covers, and the rows have not already been rebuilt since these results were fetched
    // (else every click on a result past +72 h would rebuild the whole guide and still miss).
    const long long now = static_cast<long long>(time(nullptr));
    const bool rebuildCouldHelp = hit.startUtc < now + kGuideWindowAheadSec &&
                                  hit.stopUtc > now - kGuideWindowPastSec &&
                                  st->rowsGeneration == st->hitsGeneration;
    if (row < 0 && rebuildCouldHelp && st->cb.onRebuild) {
        auto rebuild = st->cb.onRebuild;  // a copy: showEpgGuide reassigns st->cb
        st->inRebuild = true;             // showEpgGuide: a rebuild, not a reopen — keep the list
        rebuild();
        if (!IsWindow(hwnd) || stateOf(hwnd) != st) return;
        st->inRebuild = false;
        // The list stays up if the jump still misses: its channels must describe the NEW rows, the
        // ones its Channels item would filter.
        findChannels(st, st->searchedText);
        buildItems(st);
        row = rowOfHit(st, hit);
    }
    if (row < 0) {
        MessageBeep(MB_ICONASTERISK);  // outside the guide's time window (or no longer in the guide)
        updateScrollbars(hwnd, st);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    KillTimer(hwnd, kSearchTimer);  // a search still pending must not bring the list back over it
    st->searchPending = false;
    setShowResults(hwnd, st, false);
    st->scrollX = std::max(0, timeToContentX(st, hit.startUtc) - dpx(st->dpi, 80));
    st->scrollY = std::max(0, row * st->rowH - viewH(hwnd, st) / 3);
    st->flashRow = row;
    st->flashStart = hit.startUtc;
    SetTimer(hwnd, kFlashTimer, kFlashMs, nullptr);
    st->jumped = std::move(hit);
    st->jumpedValid = byClick;
    st->jumpedTick = GetTickCount();
    SetFocus(hwnd);  // keys + wheel to the grid; the search box keeps its text (Enter reopens it)
    updateScrollbars(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void openHit(HWND hwnd, GuideState* st, const GuideSearchHit& h) {
    const GuideSearchHit copy = h;  // the modal pumps messages; `h` may not survive
    openProgramme(hwnd, st, copy.channelId, copy.channelName, copy.title, copy.descr, copy.startUtc,
                  copy.stopUtc);
}

void onResultsContextMenu(HWND hwnd, GuideState* st, int x, int y) {
    const int i = itemAtY(hwnd, st, y);
    if (i < 0) return;
    st->resultSel = i;
    InvalidateRect(hwnd, nullptr, FALSE);
    if (st->items[static_cast<size_t>(i)].kind != ItemKind::Hit) return;  // the Channels item: no menu
    const GuideSearchHit h = st->hits[static_cast<size_t>(st->items[static_cast<size_t>(i)].hit)];
    enum { kPlay = 1, kSchedule, kSeries, kShow };
    HMENU m = CreatePopupMenu();
    if (st->cb.onPlay) AppendMenuW(m, MF_STRING, kPlay, tr(i18n::StringId::ProgrammePlayButton).c_str());
    if (st->cb.onSchedule)
        AppendMenuW(m, MF_STRING, kSchedule, tr(i18n::StringId::ProgrammeScheduleButton).c_str());
    if (st->cb.onRecordSeries) AppendMenuW(m, MF_STRING, kSeries, tr(i18n::StringId::RecordSeriesTitle).c_str());
    AppendMenuW(m, MF_STRING, kShow, tr(i18n::StringId::GuideSearchShowInGuide).c_str());
    POINT pt{x, y};
    ClientToScreen(hwnd, &pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    const GuideCallbacks cb = st->cb;  // a copy: an action can re-enter showEpgGuide
    if (cmd == kPlay && cb.onPlay) cb.onPlay(h.channelId, h.channelName);
    else if (cmd == kSchedule && cb.onSchedule)
        cb.onSchedule(h.channelId, h.channelName, h.title, h.startUtc, h.stopUtc);
    else if (cmd == kSeries && cb.onRecordSeries) cb.onRecordSeries(h.channelId, h.channelName, h.title);
    else if (cmd == kShow) jumpToHit(hwnd, st, h, /*byClick=*/false);
}

// Right-click a channel row -> a "favourite" toggle for that channel. x,y are client coords.
void onGuideContextMenu(HWND hwnd, GuideState* st, int x, int y) {
    if (!st->cb.onToggleFavourite) return;
    const int r = rowAtY(hwnd, st, y);
    if (r < 0 || r >= static_cast<int>(st->rows.size())) return;
    const std::wstring channelId = st->rows[r].channelId, channelName = st->rows[r].channelName;
    if (channelId.empty()) return;  // no tvg-id -> not a resolvable channel to favourite
    const bool fav = st->cb.isFavourite && st->cb.isFavourite(channelId);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1,
                fav ? tr(i18n::StringId::GuideRemoveFromFavourites).c_str()
                    : tr(i18n::StringId::GuideAddToFavourites).c_str());
    POINT pt{x, y};
    ClientToScreen(hwnd, &pt);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) st->cb.onToggleFavourite(channelId, channelName);
}

// Scroll the time axis back to now (the toolbar's Now button, Home in the grid).
void goToNow(HWND hwnd, GuideState* st) {
    st->nowUtc = static_cast<long long>(time(nullptr));
    KillTimer(hwnd, kSearchTimer);  // a search still pending must not bring the list back over it
    st->searchPending = false;
    setShowResults(hwnd, st, false);
    st->scrollX = std::max(0, timeToContentX(st, st->nowUtc) - dpx(st->dpi, 80));
    SetFocus(hwnd);  // keys to the grid it shows (typing then starts a new search, as after a jump)
    updateScrollbars(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool inRect(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// Inside the chip as painted: a pill (its ends are half-circles), not its bounding rectangle.
bool inChip(const GuideState* st, int x, int y) {
    const RECT& r = st->chip;
    if (!inRect(r, x, y)) return false;
    const float rad = static_cast<float>(r.bottom - r.top) * 0.5f;
    const float left = static_cast<float>(r.left) + rad, right = static_cast<float>(r.right) - rad;
    const float cy = static_cast<float>(r.top) + rad;
    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
    const float cx = std::clamp(px, left, std::max(left, right));  // the nearest point on the spine
    return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= rad * rad;
}

// Empty the search box and close its results at once (not after the debounce).
void clearSearch(HWND hwnd, GuideState* st) {
    SetWindowTextW(st->hSearch, L"");  // its EN_CHANGE only arms the debounce — cancelled here
    KillTimer(hwnd, kSearchTimer);
    st->searchPending = false;
    runSearch(hwnd, st);  // empty text: closes the results and ends the search session
}

// Show every channel again (the chip's ✕, Esc).
void clearChannelFilter(HWND hwnd, GuideState* st) {
    setFilter(st, L"");
    st->chipHover = false;
    updateScrollbars(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// The Channels item: narrow the grid to the channels the list matched, and leave the list for it —
// the box is emptied (the chip in the corner now says what the grid is filtered by; the box is free
// for the next search). `byClick`: a left click, whose double-click's second half must not open the
// programme it lands on in the grid.
void applyChannelFilter(HWND hwnd, GuideState* st, bool byClick) {
    const std::wstring text = st->searchedText;  // what the list, and its channel count, answer
    if (text.empty()) return;
    setFilter(st, text);
    clearSearch(hwnd, st);  // empties the box and closes the list at once
    st->filteredByClick = byClick;
    st->filteredTick = GetTickCount();
    SetFocus(hwnd);  // keys + wheel to the grid
    updateScrollbars(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Esc, from wherever it was pressed (`focused` = the box when it has focus, or null for the guide
// itself), one layer at a time: empty the focused box (which also closes the results); else leave
// the results list; else clear the channel filter; else empty the box; else close the guide.
void onEscape(HWND hwnd, GuideState* st, HWND focused) {
    const bool boxText = !editText(st->hSearch).empty();
    if (focused == st->hSearch && boxText) return clearSearch(hwnd, st);
    if (st->showResults) return setShowResults(hwnd, st, false);
    if (!st->filter.empty()) return clearChannelFilter(hwnd, st);
    if (boxText) return clearSearch(hwnd, st);
    DestroyWindow(hwnd);
}

// Bring the results back after a jump (Enter, or a click in the search box or on its frame) — by
// searching AGAIN,
// never by re-showing the old list: the session may have ended (a reopen, a rebuild) and "now" has
// moved, so old headings and "On now" badges would be wrong. Usually well under a frame; a new
// session first reloads the channel set (~90 ms) and, if the index is stale, rebuilds it. Keeps the
// user's place: the result that was selected, and the scroll, if they are still in the new list.
void reshowResults(HWND hwnd, GuideState* st) {
    KillTimer(hwnd, kSearchTimer);
    st->searchPending = false;
    std::wstring selChannel;
    long long selStart = 0;
    const GuideSearchHit* sel = selectedHit(st);  // a programme (the Channels item comes back first anyway)
    const bool hadSel = sel != nullptr;
    if (hadSel) {
        selChannel = sel->channelId;
        selStart = sel->startUtc;
    }
    const int oldScroll = st->resultsScrollY;
    runSearch(hwnd, st);  // selects the first item, scrolls to the top
    if (!hadSel || !st->showResults) return;
    for (size_t i = 0; i < st->items.size(); ++i) {
        const ResultItem& it = st->items[i];
        if (it.kind != ItemKind::Hit) continue;
        const GuideSearchHit& h = st->hits[static_cast<size_t>(it.hit)];
        if (h.channelId == selChannel && h.startUtc == selStart) {
            st->resultSel = static_cast<int>(i);
            st->resultsScrollY = oldScroll;
            revealSelection(hwnd, st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }
}

// Arrows / page keys / Enter for the results list — from the search box, or from the guide itself
// (where focus sits after a jump put the grid in front). Returns true if handled.
bool onResultsKey(HWND hwnd, GuideState* st, WPARAM vk) {
    const bool nav = vk == VK_DOWN || vk == VK_UP || vk == VK_NEXT || vk == VK_PRIOR || vk == VK_RETURN;
    if (!nav) return false;
    if (st->searchPending) {  // act on what the box says NOW, not on the results before the last key
        cancelPendingSearch(hwnd, st);
        runSearch(hwnd, st);
    }
    if (!st->showResults) {
        // After a jump: Enter (or ↓ from the box) brings the results back.
        // Gated on the TEXT, not the old results: this searches again, and may find what the last
        // search did not (a refresh, a reopen since).
        if (trimmed(editText(st->hSearch)).empty() || (vk != VK_RETURN && vk != VK_DOWN)) return false;
        reshowResults(hwnd, st);
        if (st->showResults && GetFocus() != st->hSearch) SetFocus(st->hSearch);
        return true;
    }
    switch (vk) {
        case VK_DOWN: moveSelection(hwnd, st, 1); return true;
        case VK_UP: moveSelection(hwnd, st, -1); return true;
        case VK_NEXT: moveSelection(hwnd, st, 5); return true;
        case VK_PRIOR: moveSelection(hwnd, st, -5); return true;
        default:  // VK_RETURN: open the programme, or apply the channel filter
            if (const GuideSearchHit* h = selectedHit(st)) openHit(hwnd, st, *h);
            else if (st->resultSel >= 0 && st->resultSel < static_cast<int>(st->items.size()) &&
                     st->items[static_cast<size_t>(st->resultSel)].kind == ItemKind::Channels)
                applyChannelFilter(hwnd, st, /*byClick=*/false);
            return true;
    }
}

// The search EDIT keeps its own typing; the keys that mean something to the guide are routed here.
LRESULT CALLBACK EditSubProc(HWND edit, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR ref) {
    HWND guide = reinterpret_cast<HWND>(ref);
    GuideState* st = stateOf(guide);
    if (st && msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            onEscape(guide, st, edit);
            return 0;
        }
        if (wParam == VK_TAB) {  // no dialog manager: Tab hands the keys to the grid, if it shows
            if (!st->showResults) SetFocus(guide);
            return 0;
        }
        if (onResultsKey(guide, st, wParam)) return 0;
    }
    // A deliberate click into the search box after a jump brings its results back (focus arriving
    // any other way — activation, Tab — does not; see WM_COMMAND).
    if (st && msg == WM_LBUTTONDOWN && !st->showResults && !trimmed(editText(edit)).empty())
        reshowResults(guide, st);
    if (msg == WM_CHAR && (wParam == L'\r' || wParam == 0x1B || wParam == L'\t'))
        return 0;  // no ding for Enter / Esc / Tab
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(edit, EditSubProc, 1);
    return DefSubclassProc(edit, msg, wParam, lParam);
}

void createChildren(HWND hwnd, GuideState* st) {
    st->hSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                                  10, 10, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)),
                                  st->hInst, nullptr);
    if (st->hSearch) {
        SetWindowSubclass(st->hSearch, EditSubProc, 1, reinterpret_cast<DWORD_PTR>(hwnd));
        SendMessageW(st->hSearch, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(tr(i18n::StringId::GuideSearchCue).c_str()));
    }
    recreateEditFont(st);
    layoutChildren(hwnd, st);
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
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            return dialogCtlColor(msg, wParam);
        case WM_COMMAND: {
            const HWND from = reinterpret_cast<HWND>(lParam);
            if (HIWORD(wParam) == EN_CHANGE && from == st->hSearch && st->hSearch) {
                KillTimer(hwnd, kSearchTimer);
                st->searchPending = true;
                SetTimer(hwnd, kSearchTimer, kSearchDebounceMs, nullptr);
                InvalidateRect(hwnd, &st->searchField, FALSE);  // the field's border tracks "has text"
                return 0;
            }
            // NOT on EN_SETFOCUS: focus also arrives by restoring it on activation, and that must not
            // bring the results back over whatever the guide was just asked to show. A deliberate
            // click in the box, or Enter, does (EditSubProc / onResultsKey).
            break;
        }
        case WM_TIMER:
            if (wParam == kSearchTimer) {
                KillTimer(hwnd, kSearchTimer);
                if (!st->searchPending) return 0;  // cancelled after this tick was queued
                st->searchPending = false;
                runSearch(hwnd, st);
                return 0;
            }
            if (wParam == kFlashTimer) {
                KillTimer(hwnd, kFlashTimer);
                st->flashRow = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_SIZE:
            if (st->rt) st->rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            layoutChildren(hwnd, st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (st->showResults)
                st->resultsScrollY -= (delta / WHEEL_DELTA) * st->resultRowH * 2;
            else if (LOWORD(wParam) & MK_SHIFT)
                st->scrollX -= (delta / WHEEL_DELTA) * st->pxPerHour / 2;  // Shift+wheel = time
            else
                st->scrollY -= (delta / WHEEL_DELTA) * st->rowH * 3;
            clampScroll(hwnd, st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si)};
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int& pos = st->showResults ? st->resultsScrollY : st->scrollY;
            const int line = st->showResults ? st->resultRowH : st->rowH;
            const int page = st->showResults ? resultsViewH(hwnd, st) : viewH(hwnd, st);
            switch (LOWORD(wParam)) {
                case SB_LINEUP: pos -= line; break;
                case SB_LINEDOWN: pos += line; break;
                case SB_PAGEUP: pos -= page; break;
                case SB_PAGEDOWN: pos += page; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            clampScroll(hwnd, st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_HSCROLL: {
            if (st->showResults) return 0;
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
            const int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            const bool nh = inRect(st->nowButton, mx, my);
            if (nh != st->nowHover) {
                st->nowHover = nh;
                InvalidateRect(hwnd, &st->nowButton, FALSE);
            }
            const bool chh = !st->showResults && !st->filter.empty() && inChip(st, mx, my);
            if (chh != st->chipHover) {
                st->chipHover = chh;
                InvalidateRect(hwnd, &st->chip, FALSE);
            }
            if (st->showResults) {
                const int i = itemAtY(hwnd, st, my);
                if (i != st->resultHover) {
                    st->resultHover = i;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            const int r = rowAtY(hwnd, st, my);
            if (r != st->hoverRow) {
                st->hoverRow = r;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            st->tracking = false;
            if (st->hoverRow != -1 || st->resultHover != -1 || st->nowHover || st->chipHover) {
                st->hoverRow = -1;
                st->resultHover = -1;
                st->nowHover = false;
                st->chipHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
            if (msg == WM_LBUTTONDOWN) st->swallowedDown = false;  // only ever the down just before
            if (inRect(st->nowButton, mx, my)) {
                goToNow(hwnd, st);
                return 0;
            }
            // A click on the box's painted frame (the magnifier, the padding) focuses the box — and,
            // after a jump, is the same deliberate click that brings its results back.
            if (inRect(st->searchField, mx, my)) {
                if (!st->hSearch) return 0;
                SetFocus(st->hSearch);
                if (!st->showResults && !trimmed(editText(st->hSearch)).empty()) reshowResults(hwnd, st);
                return 0;
            }
            if (my < st->toolbarH) return 0;
            // A double-click whose first click jumped from a result: open that programme. (Before the
            // chip below: the second click can land where the chip now is — the top-left of what was
            // the list.)
            if (msg == WM_LBUTTONDBLCLK && st->jumpedValid && !st->showResults &&
                GetTickCount() - st->jumpedTick <= GetDoubleClickTime()) {
                st->jumpedValid = false;
                openHit(hwnd, st, st->jumped);
                return 0;
            }
            // ...and one whose first click applied the channel filter: nothing more to do.
            if (msg == WM_LBUTTONDBLCLK && st->filteredByClick && !st->showResults &&
                GetTickCount() - st->filteredTick <= GetDoubleClickTime()) {
                st->filteredByClick = false;
                return 0;
            }
            // The channel-filter chip: a click on it clears the filter (a single click — the second
            // half of a double-click is never taken for one).
            if (msg == WM_LBUTTONDOWN && !st->showResults && !st->filter.empty() && inChip(st, mx, my)) {
                clearChannelFilter(hwnd, st);
                return 0;
            }
            if (st->showResults) {
                // Typed but not yet searched (inside the 200 ms debounce): the list on screen is
                // about to change. Search now (as the keys do: the new list, its first hit selected)
                // and ignore the click — and the second half of a double-click, which would otherwise
                // open whatever the NEW list put under the cursor.
                if (msg == WM_LBUTTONDBLCLK && st->swallowedDown) {
                    st->swallowedDown = false;
                    return 0;
                }
                if (st->searchPending) {
                    cancelPendingSearch(hwnd, st);
                    runSearch(hwnd, st);
                    st->swallowedDown = true;
                    return 0;
                }
                const int i = itemAtY(hwnd, st, my);
                if (i >= 0) {
                    st->resultSel = i;
                    const ResultItem& it = st->items[static_cast<size_t>(i)];
                    if (it.kind == ItemKind::Channels) {
                        applyChannelFilter(hwnd, st, /*byClick=*/msg == WM_LBUTTONDOWN);
                        return 0;
                    }
                    const GuideSearchHit& h = st->hits[static_cast<size_t>(it.hit)];
                    // Still on the list at the second click (the jump did not happen — the programme is
                    // outside the guide's rows): the double-click opens it.
                    if (msg == WM_LBUTTONDBLCLK) openHit(hwnd, st, h);
                    else jumpToHit(hwnd, st, h, /*byClick=*/true);
                }
                return 0;
            }
            SetFocus(hwnd);  // out of the box: typing anywhere starts a search again
            onClick(hwnd, st, mx, my);
            return 0;
        }
        case WM_RBUTTONUP:
            if (st->showResults && st->searchPending) {  // see WM_LBUTTONDOWN
                cancelPendingSearch(hwnd, st);
                runSearch(hwnd, st);
            }
            else if (st->showResults) onResultsContextMenu(hwnd, st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            else onGuideContextMenu(hwnd, st, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_ACTIVATE:
            // Keep keyboard focus in the search box if it had it across a dialog or Alt-Tab (default
            // activation would put it on the guide itself, where the results' keys used to be dead).
            if (LOWORD(wParam) == WA_INACTIVE) {
                HWND f = GetFocus();
                st->lastFocus = (f && IsChild(hwnd, f)) ? f : nullptr;
            } else if (st->lastFocus && IsWindow(st->lastFocus) && IsWindowVisible(st->lastFocus)) {
                SetFocus(st->lastFocus);
                return 0;
            }
            break;
        case WM_SETFOCUS:
            if (st->showResults && st->hSearch) SetFocus(st->hSearch);  // the list is driven from there
            return 0;
        case WM_CHAR: {
            // Type anywhere in the guide: into the search box. Over the grid the first character starts
            // a NEW search — it replaces what the box still holds from the last one (kept after a jump,
            // Now or Esc) — while Backspace or a space edits that text from its end. Only the FIRST: an
            // IME commit ("東京") or a dead key's fallback ("^x") arrives as several WM_CHARs all posted
            // to the guide, and those after the first must add to it, not replace it again.
            const wchar_t c = static_cast<wchar_t>(wParam);
            HWND box = st->hSearch;
            if (!box) return 0;
            if (c == VK_BACK || (c >= L' ' && c != 0x7F)) {
                const bool fresh = GetFocus() != box;
                SetFocus(box);
                const int n = GetWindowTextLengthW(box);
                const bool replace = fresh && c != VK_BACK && c != L' ' && c != L'\u3000' && !st->showResults;
                SendMessageW(box, EM_SETSEL, replace ? 0 : n, n);
                SendMessageW(box, WM_CHAR, wParam, lParam);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                onEscape(hwnd, st, nullptr);
                return 0;
            }
            if (wParam == VK_HOME && !st->showResults) {
                goToNow(hwnd, st);
                return 0;
            }
            // The results' keys work from here too; over the grid only Enter (back to the results).
            if ((st->showResults || wParam == VK_RETURN) && onResultsKey(hwnd, st, wParam)) return 0;
            break;
        case WM_DPICHANGED: {
            st->dpi = HIWORD(wParam);
            computeMetrics(st);
            recreateFormats(st);
            recreateEditFont(st);
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            buildItems(st);  // result row heights are DPI-scaled
            layoutChildren(hwnd, st);
            updateScrollbars(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_NCDESTROY:
            if (g_guide == hwnd) g_guide = nullptr;
            discardDevice(st);
            releaseFormats(st);
            if (st->editFont) DeleteObject(st->editFont);
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
    // CS_DBLCLKS: "double-click opens the programme" after a result's first click jumped to it.
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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
    st->nowUtc = nowUtc;
    st->sessionReady = false;  // fresh data -> the next search re-reads the channel set
    st->flashRow = -1;
    ++st->rowsGeneration;      // results fetched before now predate these rows (see jumpToHit)
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
    setFilter(st, L"");  // fresh data -> fresh channel filter (populates st->rows from st->allRows)
}

// The guide's chrome in the current skin: the caption, border and child controls
// (applyDialogDarkMode), and the window's OWN scroll bars — WS_HSCROLL/WS_VSCROLL on the guide itself,
// which applyDialogDarkMode's child pass never reaches, so they stayed light beside the dark grid.
// SetWindowTheme on the top-level window is enough to darken them (checked by capturing a test window:
// no AllowDarkModeForWindow or SetPreferredAppMode needed). A light skin REMOVES the theme instead —
// the window's bars, and the search box's (applyDialogDarkMode only ever sets the dark one) — which is
// exactly the look before any of this. Only when the skin's darkness changed: every SetWindowTheme
// sends WM_THEMECHANGED and repaints the frame.
void themeGuideChrome(HWND hwnd) {
    applyDialogDarkMode(hwnd);
    const int dark = currentTheme().dark ? 1 : 0;
    GuideState* st = stateOf(hwnd);
    if (st && st->themedDark == dark) return;
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    if (!dark && st && st->hSearch) SetWindowTheme(st->hSearch, nullptr, nullptr);
    if (st) st->themedDark = dark;
}

}  // namespace

void hideEpgGuide() {
    if (g_guide && IsWindow(g_guide)) ShowWindow(g_guide, SW_HIDE);
}

bool epgGuideOpen() { return g_guide && IsWindow(g_guide); }

void epgGuideRefreshTheme() {
    if (!g_guide || !IsWindow(g_guide)) return;
    // The main window's own repaint never reaches this separate top-level window, so a live skin
    // switch left the guide in the old skin until something repainted it. The grid, the results and
    // the painted frames read currentTheme() at paint time; the caption and scroll bars are pushed.
    themeGuideChrome(g_guide);
    RedrawWindow(g_guide, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void epgGuideRefreshLanguage() {
    if (!g_guide || !IsWindow(g_guide)) return;
    // Live UI-language change. The caption is a translated string set once at CreateWindowExW, and
    // the four cached IDWriteTextFormats (fmtChannel/fmtProg/fmtSub/fmtTime) bake in the font family
    // at creation — for a CJK language that family is Yu Gothic UI / Microsoft JhengHei UI, not Segoe
    // UI. Re-set the caption + rebuild the formats (recreateFormats reads themeFontFamily() live), the
    // search box's font and cue banner, then repaint. The stored programme rows don't change, so no
    // applyData()/DB rebuild is needed. What paint() draws from tr()/trf() — the time ranges, Now,
    // the Channels item and "On now" — self-heals on the invalidate (the hour axis is wcsftime,
    // language-neutral). The translated text that is CACHED is the results' headings in
    // ResultItem::label (the block headings with their counts, "Today", "Tomorrow"), which
    // buildItems() re-reads below; a heading for a later day is a date in the Windows locale, which
    // the UI language does not change.
    SetWindowTextW(g_guide, tr(i18n::StringId::GuideWindowTitle).c_str());
    if (GuideState* st = stateOf(g_guide)) {
        recreateFormats(st);
        recreateEditFont(st);
        layoutChildren(g_guide, st);  // the new face may be taller: re-size the box to it
        if (st->hSearch)
            SendMessageW(st->hSearch, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(tr(i18n::StringId::GuideSearchCue).c_str()));
        buildItems(st);
    }
    InvalidateRect(g_guide, nullptr, FALSE);
}

void revealEpgGuide(long long nowUtc) {
    if (!g_guide || !IsWindow(g_guide)) return;
    if (GuideState* st = stateOf(g_guide)) {
        // Move the "now" marker + airing highlight to the current time and re-centre on it. No
        // applyData() — the stored programmes are unchanged, so this skips the (costly) DB rebuild
        // that made reopen laggy. The rows span what the data held in −6 h..+72 h of when they were
        // built, so "now" stays on-screen for as long as the reopen falls inside that span.
        // Reopening shows the GRID (the search box keeps its text; clicking into it brings the
        // results back) and starts a fresh search session, so the next search re-reads the channel
        // set — playlists may have changed while the guide was hidden.
        st->nowUtc = nowUtc;
        cancelPendingSearch(g_guide, st);
        st->showResults = false;
        st->sessionReady = false;
        st->lastFocus = nullptr;  // activation must not restore a box — focus on the grid
        layoutChildren(g_guide, st);
        st->scrollX = std::max(0, timeToContentX(st, nowUtc) - dpx(st->dpi, 80));
        updateScrollbars(g_guide, st);
        InvalidateRect(g_guide, nullptr, FALSE);
    }
    themeGuideChrome(g_guide);  // re-theme the caption + scroll bars in case the skin changed while hidden
    ShowWindow(g_guide, IsIconic(g_guide) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_guide);
}

bool epgGuideShowChannel(const std::wstring& tvgId, long long nowUtc) {
    if (!g_guide || !IsWindow(g_guide) || tvgId.empty()) return false;
    GuideState* st = stateOf(g_guide);
    if (!st) return false;
    // Match on the NORMALISED base id — the same normalisation onEpgGuide builds rows with:
    // iptv-org tvg-ids carry an '@feed' quality suffix (CNN.us@SD) while a row may hold a
    // different feed variant of the same channel, so strip at '@' + ASCII-lowercase both sides.
    // Look in the FULL row set first, and change nothing when the channel is not there. Only then
    // clear any channel filter (with it cleared, `rows` is `allRows` in the same order, so the index
    // carries over) and let a showing results list give way to the grid (the search box keeps its
    // text).
    const std::wstring want = normId(tvgId);
    int row = -1;
    for (size_t i = 0; i < st->allRows.size(); ++i)
        if (normId(st->allRows[i].channelId) == want) {
            row = static_cast<int>(i);
            break;
        }
    if (row < 0) return false;  // channel has no guide row (no EPG coverage / no tvg-id match)
    if (!st->filter.empty()) setFilter(st, L"");
    cancelPendingSearch(g_guide, st);
    st->showResults = false;
    st->lastFocus = nullptr;  // activation must not restore a box — focus on the grid
    layoutChildren(g_guide, st);
    st->nowUtc = nowUtc;
    st->scrollX = std::max(0, timeToContentX(st, nowUtc) - dpx(st->dpi, 80));  // centre on "now"
    st->scrollY = row * st->rowH;  // top-align the row (clamped near the bottom by updateScrollbars)
    st->hoverRow = row;            // transient highlight so the target row is identifiable
    updateScrollbars(g_guide, st);
    InvalidateRect(g_guide, nullptr, FALSE);
    themeGuideChrome(g_guide);
    ShowWindow(g_guide, IsIconic(g_guide) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_guide);
    return true;
}

void showEpgGuide(HWND owner, HINSTANCE hInst, UINT dpi, std::vector<GuideRow> rows, long long nowUtc,
                  GuideCallbacks cb) {
    registerGuideClass(hInst);
    if (g_guide && IsWindow(g_guide)) {  // already open — repopulate + focus
        GuideState* st = stateOf(g_guide);
        if (st) {
            // Re-opened (the TV Guide command — hidden, minimized or behind the main window): show the
            // grid, like revealEpgGuide, with focus on it, and every channel. A search result's jump
            // (onRebuild, flagged by jumpToHit) is the one re-entry that must leave the results list
            // as it is — and the channel filter: the jump keeps it when it shows the programme's row.
            if (!st->inRebuild) {
                cancelPendingSearch(g_guide, st);
                st->showResults = false;
                st->lastFocus = nullptr;
            }
            const std::wstring keepFilter = st->inRebuild ? st->filter : std::wstring();
            st->cb = std::move(cb);
            applyData(st, std::move(rows), nowUtc);  // clears the filter...
            if (!keepFilter.empty()) setFilter(st, keepFilter);  // ...which a jump's rebuild restores
            // Start scrolled so "now" sits a little in from the left edge.
            st->scrollX = std::max(0, timeToContentX(st, nowUtc) - dpx(st->dpi, 80));
            layoutChildren(g_guide, st);
            updateScrollbars(g_guide, st);
            InvalidateRect(g_guide, nullptr, FALSE);
        }
        themeGuideChrome(g_guide);  // re-theme the caption + scroll bars in case the skin changed
        ShowWindow(g_guide, IsIconic(g_guide) ? SW_RESTORE : SW_SHOW);  // re-reveal if a play-from-guide had hidden it
        SetForegroundWindow(g_guide);
        return;
    }
    const int w = dpx(dpi, 1100), h = dpx(dpi, 680);
    // WS_CLIPCHILDREN: the Direct2D paint must not draw over the search box.
    HWND hwnd = CreateWindowExW(0, kClass, tr(i18n::StringId::GuideWindowTitle).c_str(),
                                WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h, owner, nullptr, hInst, nullptr);
    if (!hwnd) return;
    g_guide = hwnd;
    if (GuideState* st = stateOf(hwnd)) {
        st->hInst = hInst;
        st->cb = std::move(cb);
        st->dpi = GetDpiForWindow(hwnd);
        computeMetrics(st);
        recreateFormats(st);
        createChildren(hwnd, st);
        applyData(st, std::move(rows), nowUtc);
        st->scrollX = std::max(0, timeToContentX(st, nowUtc) - dpx(st->dpi, 80));
        updateScrollbars(hwnd, st);
    }
    themeGuideChrome(hwnd);  // dark/immersive caption + themed border + scroll bars (matches the app chrome)
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

}  // namespace rabbitears
