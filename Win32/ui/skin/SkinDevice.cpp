// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/skin/SkinDevice.h"

#include "ui/D2DSupport.h"  // the shared ID2D1Factory (QI'd up to ID2D1Factory1)

namespace rabbitears::skin {

SkinDevice& SkinDevice::instance() {
    static SkinDevice s;
    return s;
}

void SkinDevice::discard() {
    d2dDevice_.Reset();
    dxgiFactory_.Reset();
    if (d3dCtx_) d3dCtx_->ClearState();
    d3dCtx_.Reset();
    d3d_.Reset();
}

bool SkinDevice::ensure() {
    if (valid()) return true;
    discard();  // clean slate (device-lost rebuild)

    // D3D11 device. BGRA support is MANDATORY for Direct2D interop. Try hardware
    // across feature levels 11.1 -> 10.0 (custom pixel-shader effects still work at
    // 10.x), then fall back to WARP so the skin renders even without a usable GPU.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, &got, &d3dCtx_);
    if (FAILED(hr)) {
        // Some machines reject 11_1 in the array; retry without it, then WARP.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &levels[1],
                               ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &d3d_, &got, &d3dCtx_);
    }
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, &levels[1],
                               ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &d3d_, &got, &d3dCtx_);
    }
    if (FAILED(hr) || !d3d_) {
        discard();
        return false;
    }

    // DXGI factory (for CreateSwapChainForHwnd later) via the device's own adapter.
    ComPtr<IDXGIDevice1> dxgiDev;
    if (SUCCEEDED(d3d_.As(&dxgiDev))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)))
            adapter->GetParent(IID_PPV_ARGS(&dxgiFactory_));
    }
    if (!dxgiDev || !dxgiFactory_) {
        discard();
        return false;
    }

    // Reuse the app's existing D2D factory, upgraded to ID2D1Factory1 (available on
    // the DirectX 11.1 runtime), and build the D2D device on the same GPU.
    ID2D1Factory* f0 = d2dFactory();
    ComPtr<ID2D1Factory1> f1;
    if (!f0 || FAILED(f0->QueryInterface(IID_PPV_ARGS(&f1))) ||
        FAILED(f1->CreateDevice(dxgiDev.Get(), &d2dDevice_))) {
        discard();
        return false;
    }
    return valid();
}

}  // namespace rabbitears::skin
