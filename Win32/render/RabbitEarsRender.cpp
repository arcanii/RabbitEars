// SPDX-License-Identifier: GPL-3.0-or-later
//
// RabbitEarsRender — headless render tool for visual work (photoreal Phase 0).
//
// Renders every MiniMeter look x kind, the BufferMeter (data-flow tank) and — with the theme engine —
// the GPU-skinned transport strip to PNG, for every built-in skin, at 96 and 150 DPI, with the glass
// overlay off and on. It runs the meters' REAL code: each meter is created on a hidden window, fed a
// synthetic signal, ticked through its real WndProc (WM_TIMER -> onTick/step) and painted through it
// (WM_PAINT -> onPaint/render); the frame is then read back with the read-only snapshot accessors
// (miniMeterSnapshot / bufferMeterSnapshot), which copy the back-buffer the real paint blits 1:1 to
// the screen. The strip is the real paintSkinStrip() drawn into a memory DC, with its animation clock
// pinned (skin::setStripClockForTest) so every run is byte-identical.
//
// Nothing is ever shown, RabbitEars.exe is never run, and the user's library/settings are never read:
// every look, palette, glass strength and skin comes from defaults set here. Two machine-dependent
// inputs remain: the classic (theme engine OFF) LIGHT theme uses OS system colours, and the tank's
// readout is ClearType text — so a render is reproducible on one machine, not necessarily across them.
// What it CANNOT show: the owner's own saved meter settings, live stream data (the feed is synthetic),
// and the strip's child controls (buttons, sliders, status text), drawn as separate windows.
// Quirk kept for byte-compatibility with the proof of concept: the tray sheet title prints windowBg as
// the raw COLORREF, i.e. 0x00BBGGRR, not #RRGGBB.
//
// Provenance: a scratch proof of concept of this tool rendered 24 meter sheets pixel-identical to the
// compiled objects RabbitEars.exe was linked from; this is that harness made permanent, and its output
// was byte-identical to the proof of concept's when it landed. See Win32/docs/PHOTOREAL.md.
//
// usage: RabbitEarsRender [outdir] [--skin ID]... [--strip-only | --no-strip] [--time MS]
//   outdir      where the PNGs go (created if missing; default .\render-out)
//   --skin ID   render only this skin (repeatable; default: every built-in skin)
//   --strip-only / --no-strip   only / skip the transport-strip renders (theme-engine builds)
//   --time MS   the strip animation time in milliseconds (default 1500)
//   --bench-paint  no PNGs: time the REAL per-frame work (one tick + one paint) of every meter kind and
//               look, and the tank, at the tray's widths for 30 / 50 / 72 / 120 dp, at 144 dpi (150 %) —
//               each kind's width for every look (the own row gives a needle look its own, which differs)
//   --meter-height DP   the tray sheets AND the strips at this meter height, 30..120 (default 30 = the
//               standard tray; the strip grows and the widths scale with it, as in the app — ui/MeterTray.h);
//               other heights add _h<DP> to the names, and fill a Bitrate meter's whole history first (a
//               tall dial holds more columns than the 90 samples fed) — as --bench-paint does at every height;
//               there (the app's own row) a needle look takes its instrument's own width, as in layout()
//   --meter-labels  the meters' labels under an own row of meters, printed in the strip's frame as the app
//               prints them (ui/MeterLabels.h); adds _labels to the names of the strips that show them
// Exit code: 0 = every PNG written, 1 = something failed (details on stdout), 2 = bad arguments.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "core/Strings.h"
#include "ui/BufferMeter.h"
#include "ui/MiniMeter.h"
#include "ui/MeterLabels.h"  // the labels under an own row of meters (--meter-labels)
#include "ui/MeterTray.h"  // the tray geometry layout() uses
#include "ui/Skin.h"
#include "ui/Theme.h"
#ifdef RABBITEARS_THEME_ENGINE
#include "ui/skin/SkinDevice.h"
#include "ui/skin/SkinStrip.h"
#endif

#include "miniz.h"

using namespace rabbitears;

namespace {

// Both meters drive their animation from a SetTimer with id 1 (kTimerId in MiniMeter.cpp and
// BufferMeter.cpp); their WM_TIMER handlers ignore any other id. If that id ever changes, the renders
// silently stop animating (they would show an idle meter), so keep this in step.
constexpr WPARAM kMeterTimerId = 1;

HINSTANCE    g_inst = nullptr;
HWND         g_parent = nullptr;
std::wstring g_out;
int          g_files = 0;
int          g_failures = 0;

int dp(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }  // == MainWindow.cpp dp()

void fail(const wchar_t* what) {
    ++g_failures;
    wprintf(L"  !! %ls\n", what);
}

// 0x00RRGGBB per pixel, row-major, top-down (what the snapshot accessors return).
struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};

bool writePng(const Img& im, const std::wstring& name) {
    if (im.w <= 0 || im.h <= 0) {
        fail((L"empty image for " + name).c_str());
        return false;
    }
    std::vector<uint8_t> rgb(static_cast<size_t>(im.w) * im.h * 3);
    for (size_t i = 0; i < im.px.size(); ++i) {
        rgb[i * 3 + 0] = static_cast<uint8_t>(im.px[i] >> 16);
        rgb[i * 3 + 1] = static_cast<uint8_t>(im.px[i] >> 8);
        rgb[i * 3 + 2] = static_cast<uint8_t>(im.px[i]);
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), im.w, im.h, 3, &len,
                                                           MZ_DEFAULT_LEVEL, MZ_FALSE);
    if (!png) {
        fail((L"PNG encode failed for " + name).c_str());
        return false;
    }
    const std::wstring path = g_out + L"\\" + name;
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    bool ok = false;
    if (f) {
        ok = fwrite(png, 1, len, f) == len;
        fclose(f);
    }
    mz_free(png);
    if (!ok) {
        fail((L"could not write " + path).c_str());
        return false;
    }
    ++g_files;
    wprintf(L"  wrote %ls  (%dx%d)\n", path.c_str(), im.w, im.h);
    return true;
}

// A GDI DIB canvas for composing contact sheets with text labels.
struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    uint32_t* bits = nullptr;
    int w = 0, h = 0;
    Canvas(int W, int H, uint32_t fill) : w(W), h(H) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* p = nullptr;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        bits = static_cast<uint32_t*>(p);
        old = static_cast<HBITMAP>(SelectObject(dc, bmp));
        if (bits)
            for (int i = 0; i < W * H; ++i) bits[i] = fill;
    }
    ~Canvas() {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    void text(int x, int y, const std::wstring& s, COLORREF c, int px, bool bold = false) {
        HFONT f = CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ o = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c);
        TextOutW(dc, x, y, s.c_str(), static_cast<int>(s.size()));
        SelectObject(dc, o);
        DeleteObject(f);
        GdiFlush();
    }
    // Nearest-neighbour integer upscale — every source pixel becomes an s x s block, so a zoomed
    // sheet shows the exact device pixels the meter produced (no resampling blur).
    void put(const Img& im, int x, int y, int s) {
        if (!bits) return;
        for (int yy = 0; yy < im.h * s; ++yy) {
            const int dy = y + yy;
            if (dy < 0 || dy >= h) continue;
            const uint32_t* src = im.px.data() + static_cast<size_t>(yy / s) * im.w;
            uint32_t* dst = bits + static_cast<size_t>(dy) * w;
            for (int xx = 0; xx < im.w * s; ++xx) {
                const int dx = x + xx;
                if (dx >= 0 && dx < w) dst[dx] = src[xx / s];
            }
        }
    }
    Img snapshot() const {
        GdiFlush();
        Img im;
        if (!bits) return im;
        im.w = w;
        im.h = h;
        im.px.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = bits[i] & 0x00FFFFFFu;
        return im;
    }
};

// ---- synthetic signal: the Settings > Meters dialog's own preview feed -------------------------
// Transcribed from meterFeedPreviews() (Win32/ui/Dialogs.cpp). The tray's spectrum is fed 16 bands
// by SpectrumTap (kBands = 16); the dialog feeds 24 — `nBands` picks which.
void feedMini(HWND m, MeterKind k, int tick, int nBands) {
    const float t = static_cast<float>(tick) * 0.06f;
    switch (k) {
        case MeterKind::Spectrum: {
            float bands[32];
            for (int i = 0; i < nBands; ++i) {
                const float v = 0.5f + 0.42f * std::sin(t + i * 0.5f) + 0.14f * std::sin(t * 2.3f + i);
                bands[i] = std::clamp(v, 0.0f, 1.0f);
            }
            miniMeterPushSpectrum(m, bands, nBands);
            break;
        }
        case MeterKind::Signal:
            miniMeterSetSignal(m, std::clamp(0.6f + 0.4f * std::sin(t * 0.7f), 0.0f, 1.0f),
                               (std::sin(t * 0.31f) > 0.8f) ? 0.7f : 0.0f);
            break;
        case MeterKind::Bitrate:
            miniMeterPushBitrate(m, 3.0e6 * (0.5 + 0.5 * std::sin(t * 1.3) + 0.15 * std::sin(t * 11.0)));
            break;
        case MeterKind::Frames: {
            const int fps = 28 + static_cast<int>(std::lround(6.0 * std::sin(t * 0.9f)));
            // One dropped-frame burst early on (tick 39) so the red flare has fully decayed by the
            // captured frame (90 ticks): the sheet shows the steady colour, not a transient.
            miniMeterSetFrames(m, fps, (tick == 39) ? 3 : 0);
            break;
        }
    }
}

// `fullHistory`: first fill a Bitrate meter's whole history with earlier samples of the same feed, as
// steady playback has — a tall dial holds more columns than `ticks` samples (126 at 120 dp, 96 dpi).
// Off for the standard-height sheets and the preview, which keep their bytes.
Img renderMini(MeterKind kind, MeterStyle style, int w, int h, UINT dpi, int ticks, int nBands,
               bool fullHistory = false) {
    Img im;
    HWND m = createMiniMeter(g_parent, g_inst, 100, dpi, kind);
    if (!m) {
        fail(L"createMiniMeter failed");
        return im;
    }
    SetWindowPos(m, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    miniMeterSetStyle(m, style);
    if (fullHistory && kind == MeterKind::Bitrate)
        for (int i = -kBitrateHistory; i < 0; ++i) feedMini(m, kind, i, nBands);
    for (int i = 0; i < ticks; ++i) {
        feedMini(m, kind, i, nBands);
        SendMessageW(m, WM_TIMER, kMeterTimerId, 0);  // the real onTick
    }
    SendMessageW(m, WM_PAINT, 0, 0);  // the real onPaint, into the meter's cached back-buffer
    if (!miniMeterSnapshot(m, im.px, im.w, im.h)) fail(L"a mini meter produced no frame");
    DestroyWindow(m);
    return im;
}

// A steady healthy stream (health 100, flow ~0.65) or a troubled one, run long enough for the tank
// to fill and slosh; deterministic because the sim's RNG is seeded (MeterState::rng).
Img renderBuffer(int w, int h, UINT dpi, int ticks, bool troubled, const wchar_t* metrics) {
    Img im;
    HWND b = createBufferMeter(g_parent, g_inst, 200, dpi);
    if (!b) {
        fail(L"createBufferMeter failed");
        return im;
    }
    SetWindowPos(b, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    for (int i = 0; i < ticks; ++i) {
        const float t = i * 0.06f;
        if (troubled) {
            bufferMeterSetHealth(b, 38);
            bufferMeterSetFlow(b, 0.3f, (std::sin(t * 1.7f) > 0.6f) ? 0.8f : 0.0f);
        } else {
            bufferMeterSetHealth(b, 100);
            bufferMeterSetFlow(b, std::clamp(0.65f + 0.2f * std::sin(t * 0.8f), 0.0f, 1.0f), 0.0f);
        }
        SendMessageW(b, WM_TIMER, kMeterTimerId, 0);  // the real sim step
    }
    bufferMeterSetMetrics(b, metrics);
    SendMessageW(b, WM_PAINT, 0, 0);  // the real render(), into the meter's cached DIB
    if (!bufferMeterSnapshot(b, im.px, im.w, im.h, metrics && metrics[0]))
        fail(L"the buffer meter produced no frame");
    DestroyWindow(b);
    return im;
}

const MeterStyle kStyles[] = {MeterStyle::Led, MeterStyle::Tube, MeterStyle::Lcd, MeterStyle::Scope,
                              MeterStyle::Vu, MeterStyle::VuSilver};
const wchar_t* kStyleNames[] = {L"LED", L"Tube", L"LCD", L"Scope", L"VU", L"Silver"};
constexpr int kStyleCount = static_cast<int>(sizeof(kStyles) / sizeof(kStyles[0]));
const MeterKind kKinds[] = {MeterKind::Spectrum, MeterKind::Signal, MeterKind::Bitrate,
                            MeterKind::Frames};
const wchar_t* kKindNames[] = {L"Spectrum", L"Signal", L"Bitrate", L"Frames"};
// Tray widths: ui/MeterTray.h (kTrayMeterW96 in kKinds order, kTrayTankW96), as layout() uses them.
// --meter-height: the tray meters' height in dp (the strip and the widths follow it, as in the app). 30
// = the standard tray — whose sheets keep their names and bytes; any other height adds "_h<dp>".
int g_meterH96 = kMeterHeightStd;
// --meter-labels: the meters' labels under an own row (MeterLabels.h) — "_labels" in the names of the
// strips that print them (an own row); the tray sheets never do.
bool g_meterLabels = false;
std::wstring heightSuffix() {
    wchar_t s[16] = L"";
    if (g_meterH96 != kMeterHeightStd) swprintf_s(s, L"_h%d", g_meterH96);
    return s;
}

std::wstring wid(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// One contact sheet: rows = looks, columns = kinds, all at the REAL tray size for `dpi`, plus the
// buffer tank (healthy + troubled). Zoomed by integer nearest-neighbour so each device pixel is
// visible.
void traySheet(const std::string& skin, UINT dpi, float glass) {
    miniMeterSetGlass(glass);
    const int mh = dp(g_meterH96, dpi);
    const int zoom = std::max(1, ((dpi <= 96) ? 4 : 3) * 30 / g_meterH96);
    const int gap = 8, labelW = 70, top = 34, rowLabelH = 16;
    // Each look at the width the app gives it: its kind's — or, taller than the standard height (the
    // app's own row), a needle look's own (miniMeterNaturalWidth). A column is as wide as its widest.
    auto cellW = [&](int k, int s) {
        if (g_meterH96 != kMeterHeightStd)
            if (const int nw = miniMeterNaturalWidth(kStyles[s], mh, dpi)) return nw;
        return trayWidth(kTrayMeterW96[k], mh, dpi);
    };
    int colW[4], totalW = labelW;
    for (int k = 0; k < 4; ++k) {
        colW[k] = 0;
        for (int s = 0; s < kStyleCount; ++s) colW[k] = std::max(colW[k], cellW(k, s));
        totalW += colW[k] * zoom + gap;
    }
    const int bufW = trayWidth(kTrayTankW96, mh, dpi);
    totalW = std::max(totalW, labelW + 2 * (bufW * zoom + gap));
    const int rowH = mh * zoom + rowLabelH + gap;
    const int H = top + kStyleCount * rowH + rowH + 10;
    const Theme& th = currentTheme();
    Canvas cv(totalW + 10, H, 0x00101012u);
    wchar_t title[200];
    swprintf_s(title, L"skin=%ls  dpi=%u  glass=%.2f  tray meter %dpx tall  zoom x%d (nearest)  windowBg=%06X",
               wid(skin).c_str(), dpi, glass, mh, zoom, th.windowBg);
    cv.text(6, 6, title, RGB(230, 230, 230), 15, true);
    for (int s = 0; s < kStyleCount; ++s) {
        const int y = top + s * rowH;
        cv.text(6, y + rowLabelH + mh * zoom / 2 - 8, kStyleNames[s], RGB(230, 230, 230), 15, true);
        int x = labelW;
        for (int k = 0; k < 4; ++k) {
            if (s == 0) {
                wchar_t lab[64];
                swprintf_s(lab, L"%ls %dx%d", kKindNames[k], trayWidth(kTrayMeterW96[k], mh, dpi), mh);
                cv.text(x, y, lab, RGB(170, 170, 176), 13);
            }
            const int nb = 16;  // SpectrumTap::kBands — what the tray spectrum is really fed
            Img im = renderMini(kKinds[k], kStyles[s], cellW(k, s), mh, dpi, 90, nb, g_meterH96 != kMeterHeightStd);
            cv.put(im, x, y + rowLabelH, zoom);
            x += colW[k] * zoom + gap;
        }
    }
    {
        const int y = top + kStyleCount * rowH;
        cv.text(6, y + rowLabelH + mh * zoom / 2 - 8, L"Buffer", RGB(230, 230, 230), 15, true);
        Img a = renderBuffer(bufW, mh, dpi, 360, false, L"12.4 Mb/s");
        Img b = renderBuffer(bufW, mh, dpi, 360, true, L"1.8 Mb/s");
        wchar_t lab[96];
        swprintf_s(lab, L"healthy %dx%d (health 100, flow ~0.65)", bufW, mh);
        cv.text(labelW, y, lab, RGB(170, 170, 176), 13);
        cv.text(labelW + bufW * zoom + gap, y, L"troubled (health 38, flow 0.3, bursts)",
                RGB(170, 170, 176), 13);
        cv.put(a, labelW, y + rowLabelH, zoom);
        cv.put(b, labelW + bufW * zoom + gap, y + rowLabelH, zoom);
    }
    wchar_t name[128];
    swprintf_s(name, L"tray_%ls_%udpi_glass%02d%ls.png", wid(skin).c_str(), dpi,
               static_cast<int>(glass * 100 + 0.5f), heightSuffix().c_str());
    writePng(cv.snapshot(), name);
}

// The Settings > Meters dialog preview size (150x86, buffer 170x76 — Dialogs.cpp), 96 dpi, x2.
void previewSheet(const std::string& skin, float glass) {
    miniMeterSetGlass(glass);
    const UINT dpi = 96;
    const int pw = dp(150, dpi), ph = dp(86, dpi), zoom = 2, gap = 10, labelW = 70, top = 30;
    const int rowH = ph * zoom + gap;
    Canvas cv(labelW + 4 * (pw * zoom + gap) + 10, top + (kStyleCount + 1) * rowH + 10, 0x00101012u);
    wchar_t title[160];
    swprintf_s(title, L"Settings preview size %dx%d  skin=%ls  glass=%.2f  zoom x%d", pw, ph,
               wid(skin).c_str(), glass, zoom);
    cv.text(6, 6, title, RGB(230, 230, 230), 15, true);
    for (int s = 0; s < kStyleCount; ++s) {
        const int y = top + s * rowH;
        cv.text(6, y + ph * zoom / 2 - 8, kStyleNames[s], RGB(230, 230, 230), 15, true);
        for (int k = 0; k < 4; ++k) {
            Img im = renderMini(kKinds[k], kStyles[s], pw, ph, dpi, 90, 24);
            cv.put(im, labelW + k * (pw * zoom + gap), y, zoom);
        }
    }
    const int y = top + kStyleCount * rowH;
    cv.text(6, y + dp(76, dpi) * zoom / 2 - 8, L"Buffer", RGB(230, 230, 230), 15, true);
    cv.put(renderBuffer(dp(170, dpi), dp(76, dpi), dpi, 360, false, L"12.4 Mb/s"), labelW, y, zoom);
    wchar_t name[128];
    swprintf_s(name, L"preview_%ls_glass%02d.png", wid(skin).c_str(), static_cast<int>(glass * 100 + 0.5f));
    writePng(cv.snapshot(), name);
}

#ifdef RABBITEARS_THEME_ENGINE
std::wstring adapterName() {
    auto& dev = skin::SkinDevice::instance();
    if (!dev.valid()) return L"(no device)";
    skin::ComPtr<IDXGIDevice> dx;
    if (FAILED(dev.d3d()->QueryInterface(IID_PPV_ARGS(&dx)))) return L"?";
    skin::ComPtr<IDXGIAdapter> ad;
    if (FAILED(dx->GetAdapter(&ad))) return L"?";
    DXGI_ADAPTER_DESC d{};
    ad->GetDesc(&d);
    return d.Description;
}

// The strip's labels for paintSkinStrip's overlay, as the app's paintStripLabels prints them.
struct StripLabels {
    UINT dpi = 96;
    RECT rc[kMeterLabelSlots]{};
};
void paintRenderLabels(HDC dc, void* ctx) {
    const auto* l = static_cast<const StripLabels*>(ctx);
    for (int i = 0; i < kMeterLabelSlots; ++i)
        if (!IsRectEmpty(&l->rc[i])) paintMeterLabel(dc, l->rc[i], meterLabelText(i), l->dpi);
}

// The transport strip: the REAL paintSkinStrip() (D3D11 shader + D2D hairline into an offscreen
// GDI-compatible texture, BitBlt'd into the DC we pass — here a memory DC on a DIB, no window), with
// the meter tray composited at the exact MainWindowChrome.cpp layout() positions. The buttons,
// volume slider, buffer label/slider and status text are separate child controls and are NOT drawn.
void stripShot(const std::string& skin, UINT dpi, float glass, MeterStyle style, const wchar_t* tag,
               const std::wstring& adapterTag) {
    miniMeterSetGlass(glass);
    // The strip at this meter height, as layout() makes it for a video panel 1100 dp wide (and tall
    // enough not to cap it): inline at the standard height, an own row of meters above the transport
    // row when taller (MeterTray.h).
    const int W = dp(1100, dpi), pad = dp(10, dpi);
    const int labelPx = meterLabelPx(g_meterLabels, dpi);  // an own row's labels' row, with --meter-labels
    const StripMetrics sm = stripMetrics(g_meterH96, dpi, 0, W - 2 * pad, labelPx);
    const int H = sm.stripPx;       // == MainWindow stripHeight()
    Canvas cv(W, H, 0x00FF00FFu);  // magenta = "strip failed to paint"
    const int meterH = sm.meterPx;
    const int meterY = sm.ownRow ? dp(kMeterRowTopDp, dpi) : (H - meterH) / 2;
    // Where layout()'s transport controls end, left to right from the strip's edge (play, stop, record,
    // the volume icon + slider, fullscreen, the buffer label + slider — no VOD seek cluster here): inline,
    // a meter that would reach left of it (+ pad) is HIDDEN in the app, so it is left out here too. An
    // own row has only the panel's left edge to its left.
    const int bw = dp(34, dpi), ig = dp(4, dpi);
    int controlsEnd = pad;
    controlsEnd += bw + ig;
    controlsEnd += bw + ig;
    controlsEnd += bw + pad;
    controlsEnd += dp(20, dpi) + dp(2, dpi);
    controlsEnd += dp(126, dpi) + pad;
    controlsEnd += dp(34, dpi) + pad * 2;
    controlsEnd += dp(84, dpi) + dp(2, dpi);
    controlsEnd += dp(110, dpi) + pad * 2;
    const int meterLeft = sm.ownRow ? 0 : controlsEnd;
    // Where layout() puts the tank and the meters, right to left — in an own row a needle look at its
    // instrument's own width (miniMeterNaturalWidth) — and, with the labels on, each one's label cell.
    struct Placed {
        int slot, x, w;  // slot: the MeterKind (0..3), or kMeterLabelTank
    };
    Placed placed[kMeterLabelSlots];
    int nPlaced = 0;
    int rightX = W - pad;
    const int bufW = trayWidth(kTrayTankW96, meterH, dpi);
    placed[nPlaced++] = {kMeterLabelTank, rightX - bufW, bufW};
    rightX -= bufW + pad;
    const int order[] = {3, 2, 1, 0};  // rightmost first: Frames, Bitrate, Signal, Spectrum
    for (int k : order) {
        int w = trayWidth(kTrayMeterW96[k], meterH, dpi);
        if (sm.ownRow)
            if (const int nw = miniMeterNaturalWidth(style, meterH, dpi)) w = nw;
        if (rightX - w < meterLeft + pad) continue;  // does not fit: hidden, as in layout()
        placed[nPlaced++] = {k, rightX - w, w};
        rightX -= w + trayMeterGapPx(sm.ownRow, dpi);  // an own row's meters touch, as in layout()
    }
    StripLabels labels;
    labels.dpi = dpi;
    const bool withLabels = sm.ownRow && labelPx > 0;
    if (withLabels)
        for (int i = 0; i < nPlaced; ++i)
            labels.rc[placed[i].slot] =
                RECT{placed[i].x, meterY + meterH, placed[i].x + placed[i].w, meterY + meterH + labelPx};
    // The strip first (its labels in the frame, as the app prints them), then the meters over it.
    const bool ok = skin::paintSkinStrip(cv.dc, RECT{0, 0, W, H}, dpi, withLabels ? paintRenderLabels : nullptr,
                                         &labels);
    GdiFlush();
    for (int i = 0; i < nPlaced; ++i) {
        const Placed& p = placed[i];
        cv.put(p.slot == kMeterLabelTank
                   ? renderBuffer(p.w, meterH, dpi, 360, false, L"12.4 Mb/s")
                   : renderMini(kKinds[p.slot], style, p.w, meterH, dpi, 90, 16, g_meterH96 != kMeterHeightStd),
               p.x, meterY, 1);
    }
    wchar_t name[160];
    swprintf_s(name, L"strip_%ls_%udpi_%ls%ls%ls%ls.png", wid(skin).c_str(), dpi, tag, adapterTag.c_str(),
               heightSuffix().c_str(), withLabels ? L"_labels" : L"");
    writePng(cv.snapshot(), name);
    if (!ok) fail((std::wstring(L"paintSkinStrip returned false for ") + name).c_str());
    // A 3x zoom of just the right-hand tray, where the meters sit on the strip.
    const int trayX = rightX - dp(10, dpi);
    Img full = cv.snapshot();
    if (full.px.empty() || trayX < 0 || trayX >= W) {
        fail(L"strip canvas unavailable - tray crop skipped");
        return;
    }
    Img tray;
    tray.w = W - trayX;
    tray.h = H;
    tray.px.resize(static_cast<size_t>(tray.w) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < tray.w; ++x)
            tray.px[static_cast<size_t>(y) * tray.w + x] = full.px[static_cast<size_t>(y) * W + trayX + x];
    Canvas z(tray.w * 3, H * 3, 0);
    z.put(tray, 0, 0, 3);
    swprintf_s(name, L"strip_%ls_%udpi_%ls%ls%ls%ls_trayzoom3.png", wid(skin).c_str(), dpi, tag,
               adapterTag.c_str(), heightSuffix().c_str(), withLabels ? L"_labels" : L"");
    writePng(z.snapshot(), name);
}
#endif  // RABBITEARS_THEME_ENGINE

// Make `id` the active theme, deterministically — never "system", which follows the OS setting.
bool selectSkin(const std::string& id) {
#ifdef RABBITEARS_THEME_ENGINE
    activeSkinSelection() = id;
    return true;
#else
    // Without the theme engine there are only the two classic themes.
    if (id == "dark") themeOverride() = 1;
    else if (id == "light") themeOverride() = 0;
    else return false;
    return true;
#endif
}

std::vector<std::string> allSkins() {
#ifdef RABBITEARS_THEME_ENGINE
    return builtinSkinIds();
#else
    return {"dark", "light"};
#endif
}

LRESULT CALLBACK ParentProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// --bench-paint: what a meter costs the UI thread per animation frame (its onTick + onPaint, 30 per
// second in the app) at each size — the median of `frames` frames after a warm-up, in ms.
void benchPaint(UINT dpi) {
    const int heights[] = {30, 50, 72, 120};
    constexpr int kWarm = 40, kFrames = 60;
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    auto nowMs = [&]() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    };
    wprintf(L"per-frame tick+paint, ms (median of %d), dpi %u, glass %.2f; columns: meter height 30 / 50 / 72 / 120 dp\n",
            kFrames, dpi, miniMeterGlass());
    for (int k = 0; k < 4; ++k)
        for (int s = 0; s < kStyleCount; ++s) {
            wprintf(L"  %-8ls %-7ls", kKindNames[k], kStyleNames[s]);
            for (int h : heights) {
                const int mh = dp(h, dpi), w = trayWidth(kTrayMeterW96[k], mh, dpi);
                HWND m = createMiniMeter(g_parent, g_inst, 100, dpi, kKinds[k]);
                if (!m) {
                    fail(L"createMiniMeter failed");
                    continue;
                }
                SetWindowPos(m, nullptr, 0, 0, w, mh, SWP_NOZORDER | SWP_NOACTIVATE);
                miniMeterSetStyle(m, kStyles[s]);
                // Bitrate is timed with its whole history filled first (off the clock), as in steady
                // playback: a tall dial holds more columns than the warm-up's samples.
                if (kKinds[k] == MeterKind::Bitrate)
                    for (int i = -kBitrateHistory; i < 0; ++i) feedMini(m, kKinds[k], i, 16);
                std::vector<double> t;
                for (int i = 0; i < kWarm + kFrames; ++i) {
                    feedMini(m, kKinds[k], i, 16);
                    const double t0 = nowMs();
                    SendMessageW(m, WM_TIMER, kMeterTimerId, 0);
                    SendMessageW(m, WM_PAINT, 0, 0);
                    if (i >= kWarm) t.push_back(nowMs() - t0);
                }
                DestroyWindow(m);
                wprintf(L"  %7.3f", median(t));
            }
            wprintf(L"\n");
        }
    wprintf(L"  %-16ls", L"Tank");
    for (int h : heights) {
        const int mh = dp(h, dpi), w = trayWidth(kTrayTankW96, mh, dpi);
        HWND b = createBufferMeter(g_parent, g_inst, 200, dpi);
        if (!b) {
            fail(L"createBufferMeter failed");
            continue;
        }
        SetWindowPos(b, nullptr, 0, 0, w, mh, SWP_NOZORDER | SWP_NOACTIVATE);
        bufferMeterSetMetrics(b, L"12.4 Mb/s");
        std::vector<double> t;
        for (int i = 0; i < kWarm + kFrames; ++i) {
            bufferMeterSetHealth(b, 100);
            bufferMeterSetFlow(b, 0.65f, 0.0f);
            const double t0 = nowMs();
            SendMessageW(b, WM_TIMER, kMeterTimerId, 0);
            SendMessageW(b, WM_PAINT, 0, 0);
            if (i >= kWarm) t.push_back(nowMs() - t0);
        }
        DestroyWindow(b);
        wprintf(L"  %7.3f", median(t));
    }
    wprintf(L"\n");
}

int usage() {
    wprintf(L"usage: RabbitEarsRender [outdir] [--skin ID]... [--strip-only | --no-strip] [--time MS]"
            L" [--meter-height DP] [--meter-labels] [--bench-paint]\n");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring outDir = L"render-out";
    std::vector<std::string> skins;
    bool stripOnly = false, noStrip = false, benchOnly = false;
    ULONGLONG timeMs = 1500;
    bool haveOut = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--skin" && i + 1 < argc) {
            // Skin ids are ASCII; anything else becomes '?' and is then rejected as unknown.
            std::string id;
            for (const wchar_t c : std::wstring(argv[++i]))
                id.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            skins.push_back(id);
        } else if (a == L"--bench-paint") {
            benchOnly = true;
        } else if (a == L"--meter-labels") {
            g_meterLabels = true;
        } else if (a == L"--strip-only") {
            stripOnly = true;
        } else if (a == L"--no-strip") {
            noStrip = true;
        } else if (a == L"--time" && i + 1 < argc) {
            const std::wstring v = argv[++i];
            if (v.empty() || v.find_first_not_of(L"0123456789") != std::wstring::npos) return usage();
            timeMs = static_cast<ULONGLONG>(_wtoi64(v.c_str()));
        } else if (a == L"--meter-height" && i + 1 < argc) {
            const std::wstring v = argv[++i];
            if (v.empty() || v.find_first_not_of(L"0123456789") != std::wstring::npos) return usage();
            g_meterH96 = _wtoi(v.c_str());
            if (g_meterH96 < kMeterHeightMin || g_meterH96 > kMeterHeightMax) return usage();
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] != L'-' && !haveOut) {
            outDir = a;
            haveOut = true;
        } else {
            return usage();
        }
    }
    if (stripOnly && noStrip) return usage();
    const std::vector<std::string> known = allSkins();
    if (skins.empty()) skins = known;
    for (const std::string& s : skins)
        if (std::find(known.begin(), known.end(), s) == known.end()) {
            // ASCII only in console output: the CRT stops printing a line at the first character the
            // console code page cannot represent (an em dash here once swallowed the message).
            wprintf(L"unknown skin \"%ls\" - built-in skins are:", wid(s).c_str());
            for (const std::string& k : known) wprintf(L" %ls", wid(k).c_str());
            wprintf(L"\n");
            return 2;
        }
#ifndef RABBITEARS_THEME_ENGINE
    if (stripOnly) {
        wprintf(L"--strip-only needs a theme-engine build (RABBITEARS_THEME_ENGINE=ON)\n");
        return 2;
    }
#endif

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    g_out = std::filesystem::absolute(outDir, ec).wstring();
    if (g_out.empty()) g_out = outDir;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = GetModuleHandleW(nullptr);
    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gtok = 0;
    if (Gdiplus::GdiplusStartup(&gtok, &gin, nullptr) != Gdiplus::Ok)
        fail(L"GdiplusStartup failed - the Tube/Scope/VU looks will not draw correctly");
    i18n::setActiveLang(i18n::Lang::En);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ParentProc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"RabbitEarsRenderParent";
    RegisterClassExW(&wc);
    // Never shown: WS_POPUP without WS_VISIBLE. The meters are its (hidden) children.
    g_parent = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10,
                               nullptr, nullptr, g_inst, nullptr);
    registerMiniMeterClass(g_inst);
    registerBufferMeterClass(g_inst);
    if (benchOnly) {
        selectSkin(skins.front());
        for (float glass : {0.0f, 0.69f}) {  // glass off, and at the owner's 69 %
            miniMeterSetGlass(glass);
            benchPaint(144);
        }
        Gdiplus::GdiplusShutdown(gtok);
        return g_failures ? 1 : 0;
    }

#ifdef RABBITEARS_THEME_ENGINE
    // uTime for every strip is exactly timeMs: t0 is latched at 0, then the clock reads timeMs.
    skin::setStripClockForTest(0);
    const bool gpu = !noStrip && skin::initSkinStrip();
    skin::setStripClockForTest(timeMs);
    std::wstring atag;
    if (!noStrip) {
        const std::wstring adapter = adapterName();
        wprintf(L"strip device: %ls  adapter=\"%ls\"\n", gpu ? L"ok" : L"UNAVAILABLE", adapter.c_str());
        // SkinDevice falls back to WARP when no hardware device is available; tag those files.
        if (adapter.find(L"Basic Render") != std::wstring::npos) atag = L"_warp";
        if (!gpu) fail(L"initSkinStrip failed - strip renders will be magenta");
    }
#endif

    for (const std::string& id : skins) {
        selectSkin(id);
        wprintf(L"skin %ls\n", wid(id).c_str());
        for (UINT dpi : {96u, 150u}) {
#ifdef RABBITEARS_THEME_ENGINE
            // "default" = the LED look with glass off — NOT the default meter SET (a default install
            // shows only Spectrum + Signal); every strip composite shows all four meters.
            if (!noStrip) {
                stripShot(id, dpi, 0.0f, MeterStyle::Led, L"default", atag);
                stripShot(id, dpi, 0.6f, MeterStyle::Vu, L"vu_glass60", atag);
                // Taller (the app's own row, where a needle look takes its own width): the Silver face too.
                // Not at the standard height, whose file set stays as it always was.
                if (g_meterH96 != kMeterHeightStd)
                    stripShot(id, dpi, 0.6f, MeterStyle::VuSilver, L"silver_glass60", atag);
            }
#endif
            if (!stripOnly) {
                traySheet(id, dpi, 0.0f);
                traySheet(id, dpi, 0.6f);
            }
        }
        if (!stripOnly) {
            previewSheet(id, 0.0f);
            previewSheet(id, 0.6f);
        }
    }
#ifdef RABBITEARS_THEME_ENGINE
    skin::shutdownSkinStrip();
#endif
    DestroyWindow(g_parent);
    Gdiplus::GdiplusShutdown(gtok);
    wprintf(L"done: %d PNGs in %ls, %d failure(s)\n", g_files, g_out.c_str(), g_failures);
    return g_failures ? 1 : 0;
}
