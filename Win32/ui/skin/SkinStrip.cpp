// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/skin/SkinStrip.h"

#include <algorithm>

#include <d2d1_1helper.h>

#include "ui/D2DSupport.h"      // colorToD2D
#include "ui/Theme.h"           // currentTheme
#include "ui/skin/SkinDevice.h"

// Compiled-shader bytecode embedded as C arrays by the build (fxc -> .cso ->
// bin2h.cmake). Provide: underglow_vs[/ _len], underglow_ps[/ _len].
#include "underglow_vs.h"
#include "underglow_ps.h"
#include "edgeglow_ps.h"  // Phase 4b-2: dock-gutter edge glow (shares the underglow VS)

// The underglow is rendered to an offscreen GDI-compatible texture (D3D11 shader
// pass + a D2D hairline) and BitBlt'd into the caller's child-clipped DC. See the
// header + THEME_ENGINE.md §6 for why this is windowless (swapchain Present ignores
// GDI child/sibling clipping and paints over the transport controls).

namespace rabbitears::skin {
namespace {

// Must match `cbuffer Constants` in underglow.hlsl (HLSL 16-byte register packing).
struct StripConstants {
    float resolution[2];  // uResolution
    float time;           // uTime
    float intensity;      // uIntensity   (fills register b0.0)
    float bg[4];          // uBgColor
    float accent[4];      // uAccent
};

struct StripState {
    ComPtr<ID3D11Texture2D>        tex;        // offscreen target, GDI-compatible (BGRA)
    ComPtr<ID3D11RenderTargetView> rtv;        // tex as a D3D render target (shader pass)
    ComPtr<ID2D1DeviceContext>     d2dCtx;     // for the D2D hairline pass
    ComPtr<ID2D1Bitmap1>           d2dTarget;  // tex wrapped for D2D
    ComPtr<ID3D11VertexShader>     vs;
    ComPtr<ID3D11PixelShader>      ps;
    ComPtr<ID3D11Buffer>           cbuf;
    UINT       width = 0, height = 0;          // current tex size
    ULONGLONG  t0 = 0;                          // GetTickCount64 at init (animation clock)
};

StripState* g_strip = nullptr;

// Phase 4b-2: the dock-gutter edge glow. Its own gutter-sized offscreen texture + the
// edge pixel shader, sharing the one SkinDevice and the fullscreen-triangle VS. No D2D
// pass and no animation clock — a static bloom rendered on WM_PAINT.
struct EdgeState {
    ComPtr<ID3D11Texture2D>        tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader>     vs;
    ComPtr<ID3D11PixelShader>      ps;
    ComPtr<ID3D11Buffer>           cbuf;
    UINT width = 0, height = 0;
};

EdgeState* g_edge = nullptr;

void fillColor(float out[4], COLORREF c) {
    out[0] = GetRValue(c) / 255.0f;
    out[1] = GetGValue(c) / 255.0f;
    out[2] = GetBValue(c) / 255.0f;
    out[3] = 1.0f;
}

void discardResources(StripState* st) {
    st->d2dTarget.Reset();
    st->rtv.Reset();
    st->tex.Reset();
    st->cbuf.Reset();
    st->ps.Reset();
    st->vs.Reset();
    if (st->d2dCtx) st->d2dCtx->SetTarget(nullptr);
    st->d2dCtx.Reset();
    st->width = st->height = 0;
}

// Drop the edge surface's device-derived resources (no D2D target to unbind). Called on
// device loss so the next paintSkinEdge() rebuilds them against the fresh device — the
// edge has no D2D pass of its own to observe D2DERR_RECREATE_TARGET, so it relies on the
// strip's loss path (renderOffscreen) invoking this. Without it, the guards in
// ensureEdgeResources see stale non-null ComPtrs and keep using dead-device resources.
void discardEdgeResources(EdgeState* st) {
    st->rtv.Reset();
    st->tex.Reset();
    st->cbuf.Reset();
    st->ps.Reset();
    st->vs.Reset();
    st->width = st->height = 0;
}

// Ensure device + one-time objects (context/shaders/cbuf) + a size-matched offscreen
// texture (recreated when w/h change). Any failure discards everything; caller retries.
bool ensureResources(StripState* st, UINT w, UINT h) {
    SkinDevice& dev = SkinDevice::instance();
    if (!dev.ensure()) return false;

    if (!st->d2dCtx) {
        if (FAILED(dev.d2d()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &st->d2dCtx))) {
            discardResources(st);
            return false;
        }
        st->d2dCtx->SetDpi(96.0f, 96.0f);  // 1 D2D unit == 1 physical pixel
    }
    if (!st->vs || !st->ps) {
        if (FAILED(dev.d3d()->CreateVertexShader(underglow_vs, underglow_vs_len, nullptr, &st->vs)) ||
            FAILED(dev.d3d()->CreatePixelShader(underglow_ps, underglow_ps_len, nullptr, &st->ps))) {
            discardResources(st);
            return false;
        }
    }
    if (!st->cbuf) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(StripConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev.d3d()->CreateBuffer(&bd, nullptr, &st->cbuf))) {
            discardResources(st);
            return false;
        }
    }
    if (!st->tex || st->width != w || st->height != h) {
        st->d2dTarget.Reset();
        st->rtv.Reset();
        if (st->d2dCtx) st->d2dCtx->SetTarget(nullptr);
        st->tex.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;                 // BGRA — required for GDI + D2D
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;      // lets IDXGISurface1::GetDC() work
        if (FAILED(dev.d3d()->CreateTexture2D(&td, nullptr, &st->tex)) ||
            FAILED(dev.d3d()->CreateRenderTargetView(st->tex.Get(), nullptr, &st->rtv))) {
            discardResources(st);
            return false;
        }
        ComPtr<IDXGISurface> surf;
        if (FAILED(st->tex.As(&surf))) {
            discardResources(st);
            return false;
        }
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(st->d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &st->d2dTarget))) {
            discardResources(st);
            return false;
        }
        st->width = w;
        st->height = h;
    }
    return true;
}

// Render one animated frame into the offscreen texture: the D3D underglow shader pass,
// then a D2D top hairline on the same texture. Returns false on failure.
bool renderOffscreen(StripState* st, UINT w, UINT h) {
    if (!ensureResources(st, w, h)) return false;

    SkinDevice& dev = SkinDevice::instance();
    ID3D11DeviceContext* c = dev.d3dCtx();

    ID3D11RenderTargetView* rtv = st->rtv.Get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);

    const Theme& th = currentTheme();
    StripConstants cb{};
    cb.resolution[0] = static_cast<float>(w);
    cb.resolution[1] = static_cast<float>(h);
    cb.time = static_cast<float>((GetTickCount64() - st->t0) / 1000.0);
    cb.intensity = currentSkin().gpu.stripGlow;  // per-skin underglow strength (SkinGpu manifest)
    fillColor(cb.bg, th.windowBg);
    fillColor(cb.accent, th.accent);
    c->UpdateSubresource(st->cbuf.Get(), 0, nullptr, &cb, 0, 0);

    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(st->vs.Get(), nullptr, 0);
    c->PSSetShader(st->ps.Get(), nullptr, 0);
    ID3D11Buffer* cbuf = st->cbuf.Get();
    c->PSSetConstantBuffers(0, 1, &cbuf);
    c->Draw(3, 0);

    ID3D11RenderTargetView* noRtv = nullptr;
    c->OMSetRenderTargets(1, &noRtv, nullptr);   // unbind so D2D can target the texture

    st->d2dCtx->SetTarget(st->d2dTarget.Get());
    st->d2dCtx->BeginDraw();
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(st->d2dCtx->CreateSolidColorBrush(colorToD2D(th.border), &brush))) {
        st->d2dCtx->DrawLine(D2D1::Point2F(0.0f, 0.5f),
                             D2D1::Point2F(static_cast<float>(w), 0.5f), brush.Get(), 1.0f);
    }
    const HRESULT hr = st->d2dCtx->EndDraw();
    st->d2dCtx->SetTarget(nullptr);
    if (hr == D2DERR_RECREATE_TARGET) {   // device lost — rebuild next frame
        discardResources(st);
        if (g_edge) discardEdgeResources(g_edge);  // the shared device is gone; drop the edge's
                                                   // dead-device resources too, or its guards keep them
        SkinDevice::instance().discard();
        return false;
    }
    c->Flush();  // submit GPU work before GDI reads the texture via GetDC
    return true;
}

// Ensure the edge shader/cbuf (one-time) + a gutter-sized offscreen texture (recreated
// when the gutter's w/h change). Mirrors ensureResources() but with no D2D target — the
// edge glow is a pure D3D shader fill. Any failure resets what it touched; caller retries.
bool ensureEdgeResources(EdgeState* st, UINT w, UINT h) {
    SkinDevice& dev = SkinDevice::instance();
    if (!dev.ensure()) return false;

    if (!st->vs || !st->ps) {
        if (FAILED(dev.d3d()->CreateVertexShader(underglow_vs, underglow_vs_len, nullptr, &st->vs)) ||
            FAILED(dev.d3d()->CreatePixelShader(edgeglow_ps, edgeglow_ps_len, nullptr, &st->ps))) {
            st->vs.Reset();
            st->ps.Reset();
            return false;
        }
    }
    if (!st->cbuf) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(StripConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev.d3d()->CreateBuffer(&bd, nullptr, &st->cbuf))) return false;
    }
    if (!st->tex || st->width != w || st->height != h) {
        st->rtv.Reset();
        st->tex.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;                 // BGRA — required for GDI
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;      // IDXGISurface1::GetDC() for the BitBlt
        if (FAILED(dev.d3d()->CreateTexture2D(&td, nullptr, &st->tex)) ||
            FAILED(dev.d3d()->CreateRenderTargetView(st->tex.Get(), nullptr, &st->rtv))) {
            st->tex.Reset();
            st->rtv.Reset();
            return false;
        }
        st->width = w;
        st->height = h;
    }
    return true;
}

// Render one static edge-glow frame into the gutter-sized texture (D3D pixel shader only;
// the shader infers the bar orientation from w vs h). Returns false on failure.
bool renderEdgeOffscreen(EdgeState* st, UINT w, UINT h) {
    if (!ensureEdgeResources(st, w, h)) return false;

    SkinDevice& dev = SkinDevice::instance();
    ID3D11DeviceContext* c = dev.d3dCtx();

    ID3D11RenderTargetView* rtv = st->rtv.Get();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);

    const Theme& th = currentTheme();
    StripConstants cb{};
    cb.resolution[0] = static_cast<float>(w);
    cb.resolution[1] = static_cast<float>(h);
    cb.time = 0.0f;        // static — gutters render on WM_PAINT, not the animation tick
    cb.intensity = currentSkin().gpu.edgeGlow;  // per-skin gutter-neon strength (SkinGpu manifest)
    fillColor(cb.bg, th.windowBg);
    fillColor(cb.accent, th.accent);
    c->UpdateSubresource(st->cbuf.Get(), 0, nullptr, &cb, 0, 0);

    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(st->vs.Get(), nullptr, 0);
    c->PSSetShader(st->ps.Get(), nullptr, 0);
    ID3D11Buffer* cbuf = st->cbuf.Get();
    c->PSSetConstantBuffers(0, 1, &cbuf);
    c->Draw(3, 0);

    ID3D11RenderTargetView* noRtv = nullptr;
    c->OMSetRenderTargets(1, &noRtv, nullptr);   // unbind
    c->Flush();  // submit before GDI reads the texture via GetDC
    return true;
}

}  // namespace

bool initSkinStrip() {
    if (!SkinDevice::instance().ensure()) return false;
    if (!g_strip) {
        g_strip = new StripState();
        g_strip->t0 = GetTickCount64();
    }
    if (!g_edge) g_edge = new EdgeState();  // Phase 4b-2 gutter glow (shares the device)
    return true;
}

bool paintSkinStrip(HDC dst, const RECT& r, UINT dpi) {
    (void)dpi;  // strip renders at device pixels; kept for API symmetry
    if (!g_strip) return false;
    const UINT w = static_cast<UINT>(std::max<LONG>(r.right - r.left, 1));
    const UINT h = static_cast<UINT>(std::max<LONG>(r.bottom - r.top, 1));
    if (!renderOffscreen(g_strip, w, h)) return false;

    ComPtr<IDXGISurface1> surf;
    if (FAILED(g_strip->tex.As(&surf))) return false;
    HDC src = nullptr;
    if (FAILED(surf->GetDC(FALSE, &src)) || !src) return false;
    BitBlt(dst, r.left, r.top, static_cast<int>(w), static_cast<int>(h), src, 0, 0, SRCCOPY);
    surf->ReleaseDC(nullptr);
    return true;
}

bool paintSkinEdge(HDC dst, const RECT& r, UINT dpi) {
    (void)dpi;  // gutters render at device pixels; kept for API symmetry with paintSkinStrip
    if (!g_edge) return false;
    const UINT w = static_cast<UINT>(std::max<LONG>(r.right - r.left, 1));
    const UINT h = static_cast<UINT>(std::max<LONG>(r.bottom - r.top, 1));
    if (!renderEdgeOffscreen(g_edge, w, h)) return false;

    ComPtr<IDXGISurface1> surf;
    if (FAILED(g_edge->tex.As(&surf))) return false;
    HDC src = nullptr;
    if (FAILED(surf->GetDC(FALSE, &src)) || !src) return false;
    BitBlt(dst, r.left, r.top, static_cast<int>(w), static_cast<int>(h), src, 0, 0, SRCCOPY);
    surf->ReleaseDC(nullptr);
    return true;
}

void shutdownSkinStrip() {
    if (g_strip) {
        discardResources(g_strip);
        delete g_strip;
        g_strip = nullptr;
    }
    delete g_edge;  // ComPtr members release themselves; no D2D target to unbind
    g_edge = nullptr;
}

}  // namespace rabbitears::skin
