// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkinStrip — the Phase-1 theme-engine spike surface: a GPU-rendered "underglow"
// for the transport strip. It renders a D3D11 pixel shader + a D2D hairline to an
// offscreen GDI-compatible texture and hands it back for the caller to BitBlt.
//
// It is deliberately WINDOWLESS. The transport controls (buttons / volume / buffer /
// meters) are child windows that overlap the strip, and a DXGI swapchain's Present
// ignores GDI clipping and paints over them. So the caller blits the underglow into
// its OWN paint DC — the main window's WM_PAINT (BeginPaint, child-clipped by
// WS_CLIPCHILDREN) and a GetDCEx(DCX_CLIPCHILDREN) animation tick — exactly where the
// app already fills the strip today, so the controls always show through.
//
// Gated behind RABBITEARS_THEME_ENGINE; the caller only invokes these when the flag
// is defined. See Win32/docs/THEME_ENGINE.md §6.
#pragma once

#include <windows.h>

namespace rabbitears::skin {

// Bring up the shared interop device. Returns true if the GPU strip is available;
// false → the caller keeps the plain GDI strip fill. Safe to call more than once.
bool initSkinStrip();

// Render the underglow at size (r width × r height) and BitBlt it into `dst` at
// (r.left, r.top). `dst` MUST be child-clipped — a BeginPaint HDC, or GetDCEx with
// DCX_CLIPCHILDREN — so the transport controls are excluded. Returns false on
// failure (caller should GDI-fill the strip instead).
// `overlay` (optional) draws on the rendered frame BEFORE it is blitted, in `dst`'s
// coordinates, so what it draws (the meter labels, ui/MeterLabels.h) is part of every
// animated frame rather than painted over one — no flicker.
bool paintSkinStrip(HDC dst, const RECT& r, UINT dpi, void (*overlay)(HDC dc, void* ctx) = nullptr,
                    void* ctx = nullptr);

// Phase 4b-2: render a per-skin neon "edge glow" for a dock gutter rect `r` (the thin
// divider between panels) and BitBlt it into `dst` at (r.left, r.top). The bar's
// orientation is inferred from the rect aspect; the glow is a bloom down the centreline
// in the skin accent, fading to the window background at the edges. Static (rendered on
// WM_PAINT, not the animation tick). Returns false on failure (caller GDI-fills instead).
bool paintSkinEdge(HDC dst, const RECT& r, UINT dpi);

// Release all GPU resources. Call at shutdown.
void shutdownSkinStrip();

// Tooling only (RabbitEarsRender): pin the animation clock so underglow renders are reproducible.
// From the first call on, the strip reads `ms` instead of GetTickCount64() — both when
// initSkinStrip() latches its start time and on every paint — until the process exits. Call it with
// 0 before initSkinStrip(), then with the desired time: uTime is then exactly that time in seconds.
// The app never calls it, so its clock is unchanged.
void setStripClockForTest(ULONGLONG ms);

}  // namespace rabbitears::skin
