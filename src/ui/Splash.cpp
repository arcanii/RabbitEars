// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Splash.h"

#include <algorithm>

#include <objbase.h>  // CreateStreamOnHGlobal (ole32)
#include <objidl.h>   // IStream — required by gdiplus.h below
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "resource.h"
#include "ui/Theme.h"

namespace rabbitears {
namespace {

constexpr wchar_t kSplashClass[] = L"RabbitEarsSplash";

int sdpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

// Load a PNG from an RCDATA resource into a GDI+ Image (same approach as the
// About box). The returned stream must outlive the image and be Released by the
// caller after drawing.
Gdiplus::Image* loadPng(HINSTANCE hInst, int id, IStream** outStream) {
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
    auto* img = Gdiplus::Image::FromStream(stream);
    if (!img || img->GetLastStatus() != Gdiplus::Ok) {
        delete img;
        stream->Release();
        return nullptr;
    }
    *outStream = stream;
    return img;
}

Gdiplus::Color gc(COLORREF c, BYTE a) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

}  // namespace

HWND showSplash(HINSTANCE hInst) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kSplashClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    const UINT dpi = GetDpiForSystem();
    const int W = sdpx(dpi, 400), H = sdpx(dpi, 240);
    const int margin = sdpx(dpi, 26), radius = sdpx(dpi, 16), textH = sdpx(dpi, 30);

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + ((wa.right - wa.left) - W) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - H) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kSplashClass, L"RabbitEars", WS_POPUP, x, y, W, H, nullptr, nullptr,
                                hInst, nullptr);
    if (!hwnd) return nullptr;

    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        ReleaseDC(nullptr, screen);
        return hwnd;
    }

    {
        Gdiplus::Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const Theme& th = currentTheme();

        // Rounded dark card with a hairline coral edge.
        Gdiplus::GraphicsPath path;
        const int d = radius * 2;
        path.AddArc(0, 0, d, d, 180, 90);
        path.AddArc(W - d - 1, 0, d, d, 270, 90);
        path.AddArc(W - d - 1, H - d - 1, d, d, 0, 90);
        path.AddArc(0, H - d - 1, d, d, 90, 90);
        path.CloseFigure();
        Gdiplus::SolidBrush card(gc(th.panelElevBg, 255));
        g.FillPath(&card, &path);
        Gdiplus::Pen edge(gc(th.accent, 255), 1.0f);
        g.DrawPath(&edge, &path);

        // Logo, aspect-fit into the area above the caption.
        IStream* stream = nullptr;
        Gdiplus::Image* logo = loadPng(hInst, IDR_SPLASH_PNG, &stream);
        if (logo && logo->GetWidth() > 0 && logo->GetHeight() > 0) {
            const int boxW = W - 2 * margin, boxH = H - 2 * margin - textH;
            const double s = std::min(static_cast<double>(boxW) / logo->GetWidth(),
                                      static_cast<double>(boxH) / logo->GetHeight());
            const int iw = static_cast<int>(logo->GetWidth() * s);
            const int ih = static_cast<int>(logo->GetHeight() * s);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(logo, (W - iw) / 2, margin + (boxH - ih) / 2, iw, ih);
        }
        delete logo;
        if (stream) stream->Release();

        // "Loading…" caption, centred near the bottom.
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font font(&ff, static_cast<Gdiplus::REAL>(sdpx(dpi, 11)),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush tb(gc(th.textSecondary, 255));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF tr(0.0f, static_cast<Gdiplus::REAL>(H - margin - textH),
                          static_cast<Gdiplus::REAL>(W), static_cast<Gdiplus::REAL>(textH));
        g.DrawString(L"Loading…", -1, &font, tr, &sf, &tb);
    }

    HDC memDC = CreateCompatibleDC(screen);
    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    POINT ptSrc{0, 0}, ptDst{x, y};
    SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateLayeredWindow(hwnd, screen, &ptDst, &sz, memDC, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    DeleteObject(dib);  // ULW keeps its own copy; safe to free now
    ReleaseDC(nullptr, screen);
    return hwnd;
}

void closeSplash(HWND splash) {
    if (splash) DestroyWindow(splash);
}

}  // namespace rabbitears
