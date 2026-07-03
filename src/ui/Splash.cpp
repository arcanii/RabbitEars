// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/Splash.h"

#include <algorithm>
#include <random>
#include <thread>

#include <objbase.h>  // CreateStreamOnHGlobal (ole32)
#include <objidl.h>   // IStream — required by gdiplus.h below
// gdiplus.h uses unqualified min/max; NOMINMAX removes those macros, so pull the
// std versions into the Gdiplus namespace before including it.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "resource.h"
#include "ui/Theme.h"
#include "version.h"  // RE_VERSION_DISPLAY_W

namespace rabbitears {
namespace {

constexpr wchar_t kSplashClass[] = L"RabbitEarsSplash";

int sdpx(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

// Rotating, tongue-in-cheek "what the antenna is doing" captions, cycled while the
// (slow) libVLC init blocks the main thread. The splash runs on its own thread, so
// these keep animating even though the UI thread is stuck in WM_CREATE.
constexpr const wchar_t* kMessages[] = {
    L"Finding the power plug…",
    L"Connecting wires to the antenna…",
    L"Bending the left ear to the right…",
    L"Triangulating the wave-motion receptors…",
    L"Warming up the vacuum tubes…",
    L"Untangling the coaxial cable…",
    L"Polishing the rabbit ears…",
    L"Shooing off the static gremlins…",
    L"Finding where Arch keeps his cookies…",
    L"Locking onto the nearest tower…",
    L"Tuning the horizontal hold…",
    L"Nudging the aerial a smidge…",
    L"Almost picking up a signal…",
};

// Load a PNG from an RCDATA resource into a GDI+ Image. The returned stream must
// outlive the image and be Released by the caller after drawing.
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

struct SplashState {
    HWND        hwnd = nullptr;
    HINSTANCE   hInst = nullptr;
    UINT        dpi = 96;
    int         x = 0, y = 0, W = 0, H = 0, margin = 0, radius = 0, textH = 0;
    HANDLE      stop = nullptr;
    std::thread th;
};
SplashState* g_splash = nullptr;  // single splash instance

// Draw one frame (branded card + logo + caption) into the layered window.
void renderFrame(const SplashState& s, const wchar_t* message) {
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s.W;
    bi.bmiHeader.biHeight = -s.H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        ReleaseDC(nullptr, screen);
        return;
    }
    {
        Gdiplus::Bitmap bmp(s.W, s.H, s.W * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        const Theme& th = currentTheme();

        // Rounded dark card with a hairline coral edge.
        Gdiplus::GraphicsPath path;
        const int d = s.radius * 2;
        path.AddArc(0, 0, d, d, 180, 90);
        path.AddArc(s.W - d - 1, 0, d, d, 270, 90);
        path.AddArc(s.W - d - 1, s.H - d - 1, d, d, 0, 90);
        path.AddArc(0, s.H - d - 1, d, d, 90, 90);
        path.CloseFigure();
        Gdiplus::SolidBrush card(gc(th.panelElevBg, 255));
        g.FillPath(&card, &path);
        Gdiplus::Pen edge(gc(th.accent, 255), 1.0f);
        g.DrawPath(&edge, &path);

        // Logo, aspect-fit into the area above the caption.
        IStream* stream = nullptr;
        Gdiplus::Image* logo = loadPng(s.hInst, IDR_SPLASH_PNG, &stream);
        if (logo && logo->GetWidth() > 0 && logo->GetHeight() > 0) {
            const int boxW = s.W - 2 * s.margin, boxH = s.H - 2 * s.margin - s.textH;
            const double sc = std::min(static_cast<double>(boxW) / logo->GetWidth(),
                                       static_cast<double>(boxH) / logo->GetHeight());
            const int iw = static_cast<int>(logo->GetWidth() * sc);
            const int ih = static_cast<int>(logo->GetHeight() * sc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(logo, (s.W - iw) / 2, s.margin + (boxH - ih) / 2, iw, ih);
        }
        delete logo;
        if (stream) stream->Release();

        // Rotating caption, centred near the bottom.
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font font(&ff, static_cast<Gdiplus::REAL>(sdpx(s.dpi, 11)),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush tb(gc(th.textSecondary, 255));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF tr(0.0f, static_cast<Gdiplus::REAL>(s.H - s.margin - s.textH),
                          static_cast<Gdiplus::REAL>(s.W), static_cast<Gdiplus::REAL>(s.textH));
        g.DrawString(message, -1, &font, tr, &sf, &tb);

        // App version, small + muted, tucked into the bottom-right corner of the card.
        Gdiplus::Font vfont(&ff, static_cast<Gdiplus::REAL>(sdpx(s.dpi, 9)),
                            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush vb(gc(th.textMuted, 255));
        Gdiplus::StringFormat vsf;
        vsf.SetAlignment(Gdiplus::StringAlignmentFar);      // right
        vsf.SetLineAlignment(Gdiplus::StringAlignmentFar);  // bottom
        const int vpad = sdpx(s.dpi, 8);
        Gdiplus::RectF vr(0.0f, 0.0f, static_cast<Gdiplus::REAL>(s.W - vpad),
                          static_cast<Gdiplus::REAL>(s.H - vpad));
        g.DrawString(L"v" RE_VERSION_DISPLAY_W, -1, &vfont, vr, &vsf, &vb);
    }

    HDC memDC = CreateCompatibleDC(screen);
    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    POINT ptSrc{0, 0}, ptDst{s.x, s.y};
    SIZE sz{s.W, s.H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(s.hwnd, screen, &ptDst, &sz, memDC, &ptSrc, 0, &bf, ULW_ALPHA);
    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    DeleteObject(dib);  // ULW keeps its own copy
    ReleaseDC(nullptr, screen);
}

// Owns the splash window for its whole life so UpdateLayeredWindow + DestroyWindow
// stay on the creating thread (the UI thread is busy blocking in WM_CREATE).
void splashThread(SplashState* s, HANDLE created) {
    s->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kSplashClass, L"RabbitEars", WS_POPUP, s->x, s->y, s->W, s->H, nullptr,
                              nullptr, s->hInst, nullptr);
    SetEvent(created);  // unblock showSplash (s->hwnd is now set)
    if (!s->hwnd) return;
    ShowWindow(s->hwnd, SW_SHOWNOACTIVATE);

    // Shuffle the caption order once per launch so the sequence differs each run and
    // every caption shows once before any repeat; re-shuffle each time it wraps.
    constexpr int kN = static_cast<int>(ARRAYSIZE(kMessages));
    int order[kN];
    for (int k = 0; k < kN; ++k) order[k] = k;
    std::mt19937 rng(std::random_device{}());
    std::shuffle(order, order + kN, rng);
    int i = 0;
    renderFrame(*s, kMessages[order[0]]);
    for (;;) {
        const DWORD w = MsgWaitForMultipleObjects(1, &s->stop, FALSE, 1200, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;  // closeSplash signalled
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);  // stay responsive
        if (w == WAIT_TIMEOUT) {
            i = (i + 1) % kN;
            if (i == 0) std::shuffle(order, order + kN, rng);  // fresh order on each wrap
            renderFrame(*s, kMessages[order[i]]);
        }
    }
    DestroyWindow(s->hwnd);
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

    auto* s = new SplashState();
    s->hInst = hInst;
    s->dpi = GetDpiForSystem();
    s->W = sdpx(s->dpi, 400);
    s->H = sdpx(s->dpi, 240);
    s->margin = sdpx(s->dpi, 26);
    s->radius = sdpx(s->dpi, 16);
    s->textH = sdpx(s->dpi, 30);
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    s->x = wa.left + ((wa.right - wa.left) - s->W) / 2;
    s->y = wa.top + ((wa.bottom - wa.top) - s->H) / 2;
    s->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HANDLE created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s->th = std::thread(splashThread, s, created);
    if (created) {
        WaitForSingleObject(created, 3000);  // return once the window handle exists
        CloseHandle(created);
    }
    if (!s->hwnd) {  // creation failed — tear the thread down and report nothing
        SetEvent(s->stop);
        if (s->th.joinable()) s->th.join();
        CloseHandle(s->stop);
        delete s;
        return nullptr;
    }
    g_splash = s;
    return s->hwnd;
}

void closeSplash(HWND splash) {
    if (g_splash && g_splash->hwnd == splash) {
        SetEvent(g_splash->stop);
        if (g_splash->th.joinable()) g_splash->th.join();  // thread DestroyWindows on exit
        CloseHandle(g_splash->stop);
        delete g_splash;
        g_splash = nullptr;
    }
    // No fallback DestroyWindow: the splash window is owned by its own thread, so it
    // is only ever torn down there (via the join above). A non-matching/stale token
    // means it's already gone — destroying a foreign thread's HWND here would just
    // fail with ERROR_ACCESS_DENIED anyway.
}

}  // namespace rabbitears
