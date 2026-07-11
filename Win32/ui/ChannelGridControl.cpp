// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ChannelGridControl.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <commctrl.h>   // SetWindowSubclass
#include <objbase.h>    // CoInitializeEx / CreateStreamOnHGlobal
#include <shlobj.h>     // SHGetKnownFolderPath
#include <wincodec.h>   // WIC image decode
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

#include "core/Http.h"
#include "platform/Encoding.h"
#include "ui/D2DSupport.h"
#include "ui/Theme.h"
#include "ui/Tr.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

// NOTE: windows.h defines DrawText -> DrawTextW, and d2d1.h declares
// ID2D1RenderTarget::DrawText THROUGH that same macro. We keep the macro and call
// rt->DrawText(...) so declaration and call both resolve to DrawTextW.

namespace rabbitears {
namespace {

constexpr wchar_t kClass[] = L"ReChannelGrid";
constexpr UINT_PTR kTypeTimer = 2;      // resets the type-a-number accumulator
constexpr UINT     kLogoReady = WM_APP + 10;  // a background logo decode finished
constexpr int      kLogoWorkers = 3;
enum Col { COL_NUM = 0, COL_FAV, COL_LOGO, COL_NAME, COL_GROUP, COL_COUNT };

int dpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// ---- logo cache (shared_ptr so background workers can outlive the grid) -----

struct LogoEntry {
    enum State { Idle, Pending, Ready, Failed } state = Idle;
    std::vector<uint8_t> bgra;  // decoded premultiplied BGRA
    int                  w = 0, h = 0;
    ID2D1Bitmap*         bmp = nullptr;  // created lazily on the UI thread
};

struct LogoCache {
    std::mutex                                    mtx;
    std::condition_variable                       cv;
    std::deque<std::wstring>                       queue;
    std::unordered_map<std::wstring, LogoEntry>    map;
    bool                                          stop = false;
    HWND                                          grid = nullptr;
};

std::filesystem::path logoDir() {
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        dir = std::filesystem::path(p) / L"RabbitEars" / L"logos";
    if (p) CoTaskMemFree(p);
    if (dir.empty()) dir = std::filesystem::temp_directory_path() / L"RabbitEars" / L"logos";
    return dir;
}

std::wstring hashName(const std::wstring& url) {
    uint64_t h = 1469598103934665603ull;
    const std::string u = utf8FromWide(url);
    for (unsigned char c : u) { h ^= c; h *= 1099511628211ull; }
    wchar_t buf[20];
    swprintf(buf, 20, L"%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::vector<uint8_t> loadLogoBytes(const std::wstring& url) {
    std::error_code ec;
    const std::filesystem::path dir = logoDir();
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (hashName(url) + L".img");
    if (std::filesystem::exists(file, ec)) {
        std::ifstream f(file, std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!b.empty()) return b;
    }
    std::string bytes;
    std::wstring err;
    if (!httpGet(url, bytes, err, 10000) || bytes.empty()) return {};
    std::vector<uint8_t> b(bytes.begin(), bytes.end());
    std::ofstream o(file, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return b;
}

IStream* streamFromBytes(const std::vector<uint8_t>& b) {
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, b.size());
    if (!hg) return nullptr;
    if (void* dst = GlobalLock(hg)) {
        memcpy(dst, b.data(), b.size());
        GlobalUnlock(hg);
    }
    IStream* s = nullptr;
    if (CreateStreamOnHGlobal(hg, TRUE, &s) != S_OK) {
        GlobalFree(hg);
        return nullptr;
    }
    return s;
}

bool decodeLogo(IWICImagingFactory* wic, const std::wstring& url, std::vector<uint8_t>& outBgra,
                int& outW, int& outH) {
    const std::vector<uint8_t> bytes = loadLogoBytes(url);
    if (bytes.empty()) return false;
    IStream* s = streamFromBytes(bytes);
    if (!s) return false;
    IWICBitmapDecoder* dec = nullptr;
    HRESULT hr = wic->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &dec);
    s->Release();
    if (FAILED(hr) || !dec) return false;
    IWICBitmapFrameDecode* frame = nullptr;
    hr = dec->GetFrame(0, &frame);
    dec->Release();
    if (FAILED(hr) || !frame) return false;
    UINT sw = 0, sh = 0;
    frame->GetSize(&sw, &sh);
    bool ok = false;
    if (sw && sh) {
        const int kMax = 96;
        const double scale = std::min(1.0, std::min(double(kMax) / sw, double(kMax) / sh));
        const UINT tw = std::max<UINT>(1, static_cast<UINT>(sw * scale));
        const UINT th = std::max<UINT>(1, static_cast<UINT>(sh * scale));
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            outBgra.resize(static_cast<size_t>(tw) * th * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, tw * 4, static_cast<UINT>(outBgra.size()),
                                           outBgra.data()))) {
                outW = static_cast<int>(tw);
                outH = static_cast<int>(th);
                ok = true;
            }
        }
        if (conv) conv->Release();
        if (scaler) scaler->Release();
    }
    frame->Release();
    return ok;
}

void logoWorker(std::shared_ptr<LogoCache> cache) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (;;) {
        std::wstring url;
        {
            std::unique_lock<std::mutex> lk(cache->mtx);
            cache->cv.wait(lk, [&] { return cache->stop || !cache->queue.empty(); });
            if (cache->stop) break;
            url = cache->queue.front();
            cache->queue.pop_front();
        }
        std::vector<uint8_t> bgra;
        int w = 0, h = 0;
        const bool ok = wic && decodeLogo(wic, url, bgra, w, h);
        {
            std::lock_guard<std::mutex> lk(cache->mtx);
            LogoEntry& e = cache->map[url];
            if (ok) { e.bgra = std::move(bgra); e.w = w; e.h = h; e.state = LogoEntry::Ready; }
            else { e.state = LogoEntry::Failed; }
        }
        PostMessageW(cache->grid, kLogoReady, 0, 0);
    }
    if (wic) wic->Release();
    CoUninitialize();
}

// ---- grid state ------------------------------------------------------------

struct GridState {
    std::vector<Channel> channels;
    std::vector<size_t>  rowOrder;
    std::wstring         filterLower;
    int                  selectedRow = -1;
    int                  hoverRow = -1;
    long long            nowPlayingId = 0;
    int                  scrollY = 0;
    UINT                 dpi = 96;
    int                  rowH = 30;
    int                  headerH = 30;
    bool                 tracking = false;
    int                  typeNum = 0;
    ChannelGridCallbacks cb;

    HWND                 editHwnd = nullptr;  // inline # editor
    int                  editRow = -1;
    HFONT                editFont = nullptr;

    std::shared_ptr<LogoCache> logo;

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
    // Body-role formats: skin-driven family, the grid's fixed 14px size + normal/semibold
    // weights. The ★/☆ favourite marker is a Unicode dingbat from "Segoe UI Symbol" — not
    // one of the skin's typography roles (an MDL2/display face lacks it), so its family is
    // pinned via the override. themeTextFormat() null-checks the factory and returns nullptr
    // on failure, matching the old early-out (the paint path already guards null formats).
    st->fmtLeft = themeTextFormat(FontRole::Body, st->dpi, 14, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_LEADING);
    st->fmtRight = themeTextFormat(FontRole::Body, st->dpi, 14, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_TEXT_ALIGNMENT_TRAILING);
    st->fmtHeader = themeTextFormat(FontRole::Body, st->dpi, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_TEXT_ALIGNMENT_LEADING);
    st->fmtStar = themeTextFormat(FontRole::Body, st->dpi, 15, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_CENTER, L"Segoe UI Symbol");
}

void releaseLogoBitmaps(GridState* st) {
    if (!st->logo) return;
    std::lock_guard<std::mutex> lk(st->logo->mtx);
    for (auto& kv : st->logo->map) SafeRelease(kv.second.bmp);
}

void discardDevice(GridState* st) {
    releaseLogoBitmaps(st);
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
    // Pin to 96 DPI so 1 D2D unit == 1 physical pixel (matches hit-testing; we do
    // our own DPI scaling via dpx()).
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    if (FAILED(f->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, size),
                                         &st->rt)) ||
        !st->rt)
        return false;
    st->rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &st->brush);
    return st->brush != nullptr;
}

int colWidth(GridState* st, HWND hwnd, int col) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int numW = dpx(st->dpi, 54), favW = dpx(st->dpi, 40), logoW = dpx(st->dpi, 48),
              groupW = dpx(st->dpi, 180);
    int nameW = rc.right - numW - favW - logoW - groupW;
    if (nameW < dpx(st->dpi, 120)) nameW = dpx(st->dpi, 120);
    switch (col) {
        case COL_NUM: return numW;
        case COL_FAV: return favW;
        case COL_LOGO: return logoW;
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

// Return a ready D2D bitmap for `url`, creating it lazily; otherwise schedule a
// background fetch/decode and return nullptr.
ID2D1Bitmap* logoBitmap(GridState* st, const std::wstring& url) {
    if (url.empty() || !st->logo || !st->rt) return nullptr;
    std::lock_guard<std::mutex> lk(st->logo->mtx);
    LogoEntry& e = st->logo->map[url];
    if (e.bmp) return e.bmp;
    if (e.state == LogoEntry::Ready && !e.bgra.empty()) {
        ID2D1Bitmap* bmp = nullptr;
        auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(st->rt->CreateBitmap(D2D1::SizeU(e.w, e.h), e.bgra.data(),
                                           static_cast<UINT32>(e.w) * 4, props, &bmp))) {
            e.bmp = bmp;
            return bmp;
        }
        e.state = LogoEntry::Failed;
        return nullptr;
    }
    if (e.state == LogoEntry::Idle) {
        e.state = LogoEntry::Pending;
        st->logo->queue.push_back(url);
        st->logo->cv.notify_one();
    }
    return nullptr;
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

// ---- inline # (LCN) editing ------------------------------------------------

void commitEdit(HWND grid, bool save) {
    GridState* st = stateOf(grid);
    if (!st || !st->editHwnd) return;
    HWND e = st->editHwnd;
    const int row = st->editRow;
    st->editHwnd = nullptr;  // clear first so the WM_KILLFOCUS during destroy no-ops
    st->editRow = -1;
    if (save && row >= 0 && row < static_cast<int>(st->rowOrder.size())) {
        wchar_t buf[32] = L"";
        GetWindowTextW(e, buf, 32);
        Channel& c = st->channels[st->rowOrder[row]];
        if (buf[0] == L'\0') c.lcn = std::nullopt;
        else c.lcn = _wtoi(buf);
        if (st->cb.onSetNumber) st->cb.onSetNumber(c);
    }
    DestroyWindow(e);
    InvalidateRect(grid, nullptr, FALSE);
}

LRESULT CALLBACK EditSubProc(HWND edit, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                             DWORD_PTR ref) {
    HWND grid = reinterpret_cast<HWND>(ref);
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) { commitEdit(grid, true); return 0; }
            if (wParam == VK_ESCAPE) { commitEdit(grid, false); return 0; }
            break;
        case WM_KILLFOCUS:
            commitEdit(grid, true);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(edit, EditSubProc, id);
            break;
    }
    return DefSubclassProc(edit, msg, wParam, lParam);
}

void beginEdit(HWND hwnd, GridState* st, int row) {
    commitEdit(hwnd, true);
    const Channel* c = channelAtRow(st, row);
    if (!c) return;
    const int x = colLeft(st, hwnd, COL_NUM);
    const int w = colWidth(st, hwnd, COL_NUM);
    const int y = st->headerH + row * st->rowH - st->scrollY;
    if (!st->editFont)
        st->editFont = themeFont(FontRole::Body, st->dpi, 14, FW_NORMAL);
    const std::wstring cur = c->lcn ? std::to_wstring(*c->lcn) : std::wstring();
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
    st->editHwnd = CreateWindowExW(0, L"EDIT", cur.c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER, x, y, w, st->rowH,
                                   hwnd, nullptr, hInst, nullptr);
    st->editRow = row;
    if (st->editHwnd) {
        SendMessageW(st->editHwnd, WM_SETFONT, reinterpret_cast<WPARAM>(st->editFont), TRUE);
        SetWindowSubclass(st->editHwnd, EditSubProc, 1, reinterpret_cast<DWORD_PTR>(hwnd));
        SetFocus(st->editHwnd);
        SendMessageW(st->editHwnd, EM_SETSEL, 0, -1);
    }
}

// ---- paint -----------------------------------------------------------------

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
        const bool dead = (c.deadStatus == DeadStatus::Dead);
        const COLORREF rowbg = sel ? th.selectionBg
                                   : (r == st->hoverRow ? th.hoverBg
                                                        : ((r & 1) ? th.altRowBg : th.panelBg));
        fill(0, y, static_cast<float>(rc.right), static_cast<float>(st->rowH), rowbg);
        if (playing) fill(0, y, static_cast<float>(dpx(st->dpi, 3)), static_cast<float>(st->rowH), th.accent);

        const COLORREF txtc = dead ? th.textMuted : (sel ? th.selectionText : th.textPrimary);
        const COLORREF subc = dead ? th.textMuted : (sel ? th.selectionText : th.textSecondary);

        // # (channel number)
        const std::wstring num = c.lcn ? std::to_wstring(*c.lcn) : std::wstring();
        text(num, st->fmtRight, static_cast<float>(colLeft(st, hwnd, COL_NUM)),
             static_cast<float>(colWidth(st, hwnd, COL_NUM)), y, static_cast<float>(st->rowH), subc, pad);
        // ★ favourite
        const COLORREF star = dead ? th.textMuted
                                   : (c.favourite ? th.accent : (sel ? th.selectionText : th.textMuted));
        text(c.favourite ? L"★" : L"☆", st->fmtStar, static_cast<float>(colLeft(st, hwnd, COL_FAV)),
             static_cast<float>(colWidth(st, hwnd, COL_FAV)), y, static_cast<float>(st->rowH), star, 0);
        // logo
        {
            const float lx = static_cast<float>(colLeft(st, hwnd, COL_LOGO));
            const float lw = static_cast<float>(colWidth(st, hwnd, COL_LOGO));
            const float bx = lx + dpx(st->dpi, 4), bw = lw - dpx(st->dpi, 8);
            const float bh = static_cast<float>(st->rowH - dpx(st->dpi, 8));
            const float by = y + (st->rowH - bh) / 2;
            ID2D1Bitmap* bmp = logoBitmap(st, c.logoUrl);
            if (bmp) {
                const D2D1_SIZE_F sz = bmp->GetSize();
                const float sc = std::min(bw / sz.width, bh / sz.height);
                const float dw = sz.width * sc, dh = sz.height * sc;
                const float dx = bx + (bw - dw) / 2, dy = by + (bh - dh) / 2;
                rt->DrawBitmap(bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh), dead ? 0.35f : 1.0f);
            } else {
                // placeholder: rounded chip + initial
                st->brush->SetColor(colorToD2D(th.panelElevBg));
                const float rad = static_cast<float>(dpx(st->dpi, 4));
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bh, by + bh), rad, rad), st->brush);
                const std::wstring init = c.name.empty() ? L"?" : c.name.substr(0, 1);
                text(init, st->fmtStar, bx, bh, by, bh, th.textMuted, 0);
            }
        }
        // name + group
        text(c.name, st->fmtLeft, static_cast<float>(colLeft(st, hwnd, COL_NAME)),
             static_cast<float>(colWidth(st, hwnd, COL_NAME)), y, static_cast<float>(st->rowH), txtc, pad);
        text(c.groupTitle, st->fmtLeft, static_cast<float>(colLeft(st, hwnd, COL_GROUP)),
             static_cast<float>(colWidth(st, hwnd, COL_GROUP)), y, static_cast<float>(st->rowH), subc, pad);
    }

    // Header band on top.
    fill(0, 0, static_cast<float>(rc.right), static_cast<float>(st->headerH), th.panelElevBg);
    fill(0, static_cast<float>(st->headerH - 1), static_cast<float>(rc.right), 1, th.border);
    // groupHead / chanHead outlive the header loop below so their .c_str() stays valid.
    const std::wstring groupHead = tr(i18n::StringId::GridHeaderGroup);
    const std::wstring chanHead = tr(i18n::StringId::LabelChannel);
    const wchar_t* heads[COL_COUNT] = {L"#", L"", L"", chanHead.c_str(), groupHead.c_str()};
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
        case kLogoReady:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SIZE:
            if (st->rt) st->rt->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            commitEdit(hwnd, true);
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
        case WM_LBUTTONDOWN: {
            commitEdit(hwnd, true);
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
            } else if (col != COL_NUM && c && st->cb.onActivate) {
                st->cb.onActivate(*c);  // # column only selects (double-click to edit)
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const int r = rowAtY(st, GET_Y_LPARAM(lParam));
            if (r < 0) return 0;
            const int col = colAtX(st, hwnd, GET_X_LPARAM(lParam));
            st->selectedRow = r;
            if (col == COL_NUM) {
                beginEdit(hwnd, st, r);
            } else if (const Channel* c = channelAtRow(st, r); c && col != COL_FAV && st->cb.onActivate) {
                st->cb.onActivate(*c);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const int r = rowAtY(st, GET_Y_LPARAM(lParam));
            if (r < 0) return 0;
            st->selectedRow = r;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (const Channel* c = channelAtRow(st, r); c && st->cb.onContextMenu) {
                POINT pt;
                GetCursorPos(&pt);
                st->cb.onContextMenu(*c, pt);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            commitEdit(hwnd, true);
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            st->scrollY -= (delta / WHEEL_DELTA) * st->rowH * 3;
            clampScroll(hwnd, st);
            updateScrollbar(hwnd, st);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            commitEdit(hwnd, true);
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
            commitEdit(hwnd, false);
            if (st->logo) {
                {
                    std::lock_guard<std::mutex> lk(st->logo->mtx);
                    st->logo->stop = true;
                }
                st->logo->cv.notify_all();  // detached workers drain and exit; shared_ptr frees the cache
            }
            discardDevice(st);
            releaseFormats(st);
            if (st->editFont) DeleteObject(st->editFont);
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
            st->logo = std::make_shared<LogoCache>();
            st->logo->grid = h;
            for (int i = 0; i < kLogoWorkers; ++i) std::thread(logoWorker, st->logo).detach();
        }
    }
    return h;
}

void channelGridSetChannels(HWND grid, std::vector<Channel> channels) {
    GridState* st = stateOf(grid);
    if (!st) return;
    commitEdit(grid, false);
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
    if (st->editFont) { DeleteObject(st->editFont); st->editFont = nullptr; }
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

void channelGridSetDeadStatus(HWND grid, long long channelId, DeadStatus status) {
    GridState* st = stateOf(grid);
    if (!st) return;
    for (Channel& c : st->channels)
        if (c.id == channelId) c.deadStatus = status;
    InvalidateRect(grid, nullptr, FALSE);
}

}  // namespace rabbitears
