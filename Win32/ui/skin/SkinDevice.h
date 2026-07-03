// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkinDevice — the shared Direct2D-on-D3D11 interop device for the theme engine
// (0.2.x, Phase-1 spike). ONE process-lifetime device stack (D3D11 + DXGI + D2D
// 1.1) that every skinned surface draws through; per-surface swapchains/targets
// live in the surfaces themselves (see SkinStrip). Gated behind the CMake option
// RABBITEARS_THEME_ENGINE — this translation unit only compiles when it is ON.
//
// Design rationale + the whole engine plan: Win32/docs/THEME_ENGINE.md §6.
#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace rabbitears::skin {

using Microsoft::WRL::ComPtr;

// Lazily-created, rebuilt-on-device-loss. Not thread-safe: all access is on the UI
// thread (same constraint as the existing D2D grid).
class SkinDevice {
public:
    static SkinDevice& instance();

    // (Re)create the device stack if needed. Returns false if no device (not even
    // WARP) could be created — callers then fall back to the GDI path.
    bool ensure();

    // Drop the whole stack (on DXGI_ERROR_DEVICE_REMOVED/RESET). Surfaces must drop
    // their swapchain-derived resources too, then call ensure() again next frame.
    void discard();

    bool valid() const { return d2dDevice_ != nullptr; }

    ID3D11Device*        d3d() const { return d3d_.Get(); }
    ID3D11DeviceContext* d3dCtx() const { return d3dCtx_.Get(); }
    IDXGIFactory2*       dxgiFactory() const { return dxgiFactory_.Get(); }
    ID2D1Device*         d2d() const { return d2dDevice_.Get(); }

private:
    SkinDevice() = default;
    SkinDevice(const SkinDevice&) = delete;
    SkinDevice& operator=(const SkinDevice&) = delete;

    ComPtr<ID3D11Device>        d3d_;
    ComPtr<ID3D11DeviceContext> d3dCtx_;
    ComPtr<IDXGIFactory2>       dxgiFactory_;
    ComPtr<ID2D1Device>         d2dDevice_;
};

}  // namespace rabbitears::skin
