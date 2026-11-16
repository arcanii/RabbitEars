// SPDX-License-Identifier: GPL-3.0-or-later
//
// TankSim — the data-flow tank's physics and LED render, the mac port of Win32/ui/BufferMeter.cpp (its
// "stable fluids" solver, the tank's circulation, pour, drain, waves and splashes, and renderLedBits —
// VERBATIM: every tunable and every comment explaining it is that file's). A little tank of liquid whose
// level tracks the stream's buffer health (a healthy stream rests ~half-full); data pours in at the top
// centre and leaves through a drain in the floor; throughput drives the pour, circulation, shimmer and
// waves, packet trouble drives turbulence and splashes. Pure C++: TankView (Cocoa) owns one and blits
// what it renders. Keep in step with BufferMeter.cpp (a candidate for common/ once its window glue is
// split from its pixel half on Win32).
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

#include "platform/WinTypes.h"  // COLORREF / RGB / MulDiv, as the Win32 original is written
#include "ui/PhotoCells.h"      // photoDialPx / kPhotoRows — the scaled dots match the photoreal cells

namespace rabbitears::mac {

// The liquid's surface colour (Win32 kDefaultFluidColor); persisted as buffer_fluid_color (RRGGBB).
constexpr COLORREF kDefaultFluidColor = RGB(190, 158, 244);

// What the render takes from the theme (mac: the strip's dark surround), the fluid colour, and the
// opt-in "Data-flow dots grow with the meter" (buffer_scaled_dots).
struct TankColors {
    COLORREF windowBg = RGB(20, 20, 20);
    COLORREF border = RGB(56, 56, 56);
    COLORREF accent = RGB(217, 119, 87);
    COLORREF fluid = kDefaultFluidColor;
    bool     scaledDots = false;
};

// ---- LED grid geometry (Win32 BufferMeter.h, verbatim) ----------------------
struct BufferGrid {
    int cols = 1, rows = 1, ox = 0, oy = 0;
};
inline BufferGrid bufferGrid(int W, int H, int gap, int pitch, int inset) {
    BufferGrid g;
    const int gridW = (W - 2 * inset) > 1 ? (W - 2 * inset) : 1;
    const int gridH = (H - 2 * inset) > 1 ? (H - 2 * inset) : 1;
    g.cols = (gridW + gap) / pitch; if (g.cols < 1) g.cols = 1;
    g.rows = (gridH + gap) / pitch; if (g.rows < 1) g.rows = 1;
    g.ox = inset + (gridW - (g.cols * pitch - gap)) / 2;
    g.oy = inset + (gridH - (g.rows * pitch - gap)) / 2;
    return g;
}
constexpr int kBufferScaledRows = kPhotoRows;
struct BufferPitch {
    int gap = 1, pitch = 3;
};
inline BufferPitch bufferLedPitch(int H, UINT dpi, bool scaled, SkinMaterial material = SkinMaterial::Flat) {
    BufferPitch p;
    p.gap = std::max(1, MulDiv(1, static_cast<int>(dpi), 96));
    p.pitch = std::max(p.gap + 2, MulDiv(3, static_cast<int>(dpi), 96));
    if (!scaled) return p;
    p.pitch = std::max(p.pitch, photoDialPx(H, dpi, material) / kBufferScaledRows);
    p.gap = std::min(p.pitch - 2, std::max(p.gap, static_cast<int>(p.pitch / 5.0f + 0.5f)));
    return p;
}

class TankSim {
public:
    TankSim();
    ~TankSim();
    TankSim(const TankSim&) = delete;
    TankSim& operator=(const TankSim&) = delete;

    // Buffer health 0..100 -> the target level (a healthy stream rests ~half-full); <= 0 also clears
    // the flow and trouble (no stream). Win32 bufferMeterSetHealth.
    void setHealth(int percent);
    // flowRate 0..1 (throughput), trouble 0..1 (corruption / loss pressure; latched, decays). Win32
    // bufferMeterSetFlow.
    void setFlow(float flowRate, float trouble);
    void step();               // one ~33-ms frame of the simulation
    bool active() const;       // worth stepping: a target, or fluid still draining out
    // Render into `px` (W*H, 0x00RRGGBB, top-down). `dpi` = 96 * the backing scale.
    // `inset`: the glass bezel's band (0 when the glass is off — the grid is then edge-to-edge, exactly
    // Win32's pre-glass grid).
    void render(uint32_t* px, int W, int H, UINT dpi, const TankColors& colors, int inset = 0);
    // Start a twin where this tank IS (the bridge's mirror): its targets, so it does not sit empty.
    void seedFrom(const TankSim& other);
    float fillTarget() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace rabbitears::mac
