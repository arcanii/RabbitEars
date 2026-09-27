// SPDX-License-Identifier: GPL-3.0-or-later
// PhotoCells — the cells of the three photoreal cell looks (Win32/docs/PHOTOREAL.md, stage B): Studio LED,
// Backlit LCD and VFD, NEW looks beside the classic LED / LCD / Vacuum tube, which stay exactly as they were
// (the owner's rule).
//
// What they fix: the classic cells keep a fixed 3-dp pitch whatever the meter's height, so a Large or Extra
// large meter only gets MORE, finer rows — a mesh, beside VU instruments that read as hardware. These cells
// scale WITH the meter: about kPhotoRows rows in any dial tall enough for them, never finer than the classic
// pitch — so at the standard height they keep the classic pitch and gap (not its exact grid: the block is
// centred, and fills the dial with whole cells), and at Extra large a cell is big enough to show what it is
// made of:
//   * Studio LED — a rectangular LED in a housing: a lens tinted with its own colour when off, a hot core
//     and a rim when lit, a glint from the key light (above-left, as everywhere) one pixel inside the rim, a
//     soft bloom on a dark panel.
//   * Backlit LCD — crisp, flat segments with an edge seal, the unlit ones a ghost of their colour, the field
//     a shade deeper than the panel (on a light panel, a reflective grey-green LCD instead), a little light
//     spill around a lit segment on a dark one.
//   * VFD (vacuum fluorescent display) — phosphor segments under dark teal glass, a glow round each lit
//     one, and where the cells are big enough the filament wires across the whole window and the control
//     grid's fine mesh over it.
//
// Split like VuDial: the geometry is header-inline pure arithmetic (the painters in MiniMeter.cpp and
// --selftest use it); the rasteriser (PhotoCells.cpp) writes pixels, never calls GDI, and keeps each cell it
// shades in a per-meter cache, so a frame is a fill, row copies, the lit cells' glow blends and (VFD) one pass
// over the window — against the classic Tube's GDI+ ellipses per lit cell, ~10 ms a frame for a Tube Bitrate
// meter at 120 dp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include <windows.h>

#include "ui/Skin.h"  // SkinMaterial — the bezel's material (common/, pure C++)

namespace rabbitears {

enum class CellFinish { Led, Lcd, Vfd };

// ---- Geometry -----------------------------------------------------------------------------------------
// The frame: a photoreal meter's window sits inside a bezel in the skin's material (stage C) — the meter's
// 2-dp chrome band (meterChromePx), and on a taller meter a sixteenth of its height, so the frame grows with it.
// A skin without a material (Flat) keeps just the chrome band, drawn as every meter draws it.
inline int photoChromePx(UINT dpi) { return MulDiv(2, static_cast<int>(dpi), 96); }  // == meterChromePx
// Whether a material is one this renderer draws a frame for: every one but Flat — and not a value from a newer skin
// model this build does not know, which is drawn as Flat (Skin.h's promise for an unknown material).
inline bool photoMaterialFramed(SkinMaterial m) {
    return m != SkinMaterial::Flat && static_cast<int>(m) <= static_cast<int>(SkinMaterial::NeonGlass);
}
inline int photoBezelPx(int meterPx, UINT dpi, SkinMaterial m) {
    return photoMaterialFramed(m) ? std::max(photoChromePx(dpi), meterPx / 16) : photoChromePx(dpi);
}
// The dial — the window inside the frame — of a meter `meterPx` tall: what the rows below are laid out in, and
// what the buffer tank's opt-in scaled dots are sized from (BufferMeter.h), so its dots match the cells beside it.
inline int photoDialPx(int meterPx, UINT dpi, SkinMaterial m) { return meterPx - 2 * photoBezelPx(meterPx, dpi, m); }

// Everything else is in device pixels, relative to the dial.

// Rows a cell look aims for in a dial tall enough (a hardware LED ladder has 8-12): the pitch is the dial's
// height over this, floored, and as many whole cells fit as they will — 10 to 13 rows.
constexpr int kPhotoRows = 10;

// The classic looks' pitch (MiniMeter.cpp paintSpectrum & co: max(dpx(3), 2)) — the finest these cells go.
inline int photoClassicPitch(UINT dpi) { return std::max(MulDiv(3, static_cast<int>(dpi), 96), 2); }

// The pitch (a cell and the gap above it) for a dial `dialH` px tall.
inline int photoPitch(int dialH, UINT dpi) { return std::max(photoClassicPitch(dpi), dialH / kPhotoRows); }

// The housing between two cells: the classic 1 dp, or a fifth of the pitch once that is more.
inline int photoGap(int pitch, UINT dpi) {
    return std::max(MulDiv(1, static_cast<int>(dpi), 96), static_cast<int>(pitch / 5.0f + 0.5f));
}

// The rows of a dial: as many whole cells as fit (the top one needs no gap above it), the block centred.
struct PhotoRows {
    int pitch = 0, gap = 0, rows = 0;
    int bottom = 0;  // the lowest cell's bottom edge (exclusive), from the dial's top
    // Row r (0 = the bottom one): its top and bottom edges.
    int cellBottom(int r) const { return bottom - r * pitch; }
    int cellTop(int r) const { return cellBottom(r) - (pitch - gap); }
};
inline PhotoRows photoRows(int dialH, UINT dpi) {
    PhotoRows g;
    if (dialH <= 0) return g;
    g.pitch = photoPitch(dialH, dpi);
    g.gap = std::min(photoGap(g.pitch, dpi), g.pitch - 1);
    g.rows = std::max(1, (dialH + g.gap) / g.pitch);
    const int used = g.rows * g.pitch - g.gap;
    g.bottom = dialH - std::max(0, dialH - used) / 2;  // an odd pixel left over goes above the block
    return g;
}

// Columns `colW` px apart (a cell and the gap right of it), at most `maxCols`, the block centred in `dialW`.
struct PhotoCols {
    int colW = 0, cols = 0;
    int left = 0;  // the first column's left edge, from the dial's left
    int cellLeft(int c) const { return left + c * colW; }
};
inline PhotoCols photoCols(int dialW, int colW, int gap, int maxCols) {
    PhotoCols g;
    if (dialW <= 0 || colW <= 0 || maxCols <= 0) return g;
    g.colW = colW;
    g.cols = std::clamp((dialW + gap) / colW, 0, maxCols);
    const int used = g.cols * colW - gap;
    g.left = g.cols > 0 ? std::max(0, dialW - used) / 2 : 0;
    return g;
}

// Each kind's column width. Spectrum and Signal divide the dial among their bands / five bars, as the
// classic looks do; Bitrate's history columns and Frames' bar are cells as wide as they are tall (Frames
// about a third wider — pitch/3 floored, so 20-33 % — as its classic 4-dp columns are beside the 3-dp
// pitch), so they scale with the meter too.
inline int photoBitrateColW(const PhotoRows& g) { return g.pitch; }
inline int photoFramesColW(const PhotoRows& g) { return g.pitch + std::max(1, g.pitch / 3); }

// The Signal meter's bar j (0..4 of 5): how many cells tall — the tallest the whole column, never none.
inline int photoSignalCells(int j, int rows) {
    return std::clamp(static_cast<int>((rows * (j + 1)) / 5.0f + 0.5f), 1, std::max(1, rows));
}

// ---- The rasteriser (PhotoCells.cpp) -----------------------------------------------------------------

// Whether a meter's panel (meterPanelColor) is dark enough for light to show on it: the Studio LED's bloom and
// the Backlit LCD's spill are drawn only then (the VFD's glass is dark on any panel). The Meters dialog asks
// the same question to hide the Glow knob where it would do nothing.
inline bool photoPanelIsDark(COLORREF panel) {
    return (299 * GetRValue(panel) + 587 * GetGValue(panel) + 114 * GetBValue(panel)) / 1000 < 128;
}

// One cell to draw: where (back-buffer px), its colour when lit, and whether it is. An unlit cell still
// carries the colour it WOULD have — an LED's lens is tinted with it.
struct PhotoCell {
    RECT     r;
    COLORREF lit;
    bool     on;
};

// Everything the cells' pixels depend on besides the cell itself.
struct PhotoScene {
    CellFinish finish = CellFinish::Led;
    COLORREF   panel = RGB(24, 22, 22);    // the meter's panel (meterPanelColor)
    COLORREF   off = RGB(38, 40, 44);      // the drawn palette's Dim
    COLORREF   hot = RGB(236, 236, 240);   // the RAW Peak — "the hottest light" an LED's core leans to
    float      glow = 0.5f;                // the Glow knob (0.5 = stock) — LED / VFD bloom
    int        pitch = 3;                  // the rows' pitch (PhotoRows) — gates the VFD's filaments and mesh,
                                           // and sets the mesh's period
    bool operator==(const PhotoScene&) const = default;
};

// What a meter keeps between frames: the cells it shaded (each once per size, colour, lit and scene), at most
// `capacity` of them — the least recently drawn goes first. One per meter, UI thread only (the sRGB tables are
// one process-wide static).
constexpr size_t kPhotoSpriteCap = 48;  // a frame needs ~26 at most (a Bitrate meter: 13 rows' colours, lit and not)
class PhotoCellCache {
public:
    explicit PhotoCellCache(size_t capacity = kPhotoSpriteCap) : cap_(capacity > 0 ? capacity : 1) {}
    struct Sprite {
        int w = 0, h = 0;          // the cell
        COLORREF lit = 0;
        bool on = false;
        uint64_t used = 0;         // last frame it was drawn in (least recently used goes first)
        std::vector<uint32_t> px;  // w*h, 0x00RRGGBB, opaque (the lens over the finish's field)
        int m = 0;                 // the glow's reach beyond the cell (0 = none)
        std::vector<uint16_t> glow;  // (w+2m)*(h+2m)*3, linear light 0..65535, ADDED over what is there
    };
    const Sprite& get(const PhotoScene& scene, int w, int h, COLORREF lit, bool on);
    void newFrame(const PhotoScene& scene);

private:
    size_t     cap_;
    PhotoScene scene_{};
    bool       valid_ = false;
    uint64_t   frame_ = 0;
    std::vector<Sprite> sprites_;
};

// Draw `cells` into a 0x00RRGGBB top-down buffer `px` (`stride` px a row, `bufH` rows), clipped to `dial`:
// the finish's own field over the dial first, every cell, the lit cells' glow, then the VFD's filaments and
// mesh. The chrome outside `dial` is never touched.
void paintPhotoCells(uint32_t* px, int stride, int bufH, const RECT& dial, const PhotoScene& scene,
                     const std::vector<PhotoCell>& cells, PhotoCellCache& cache);

// The bezel: the ring between `meter` (the whole meter) and `meter` inset by `px` on every side, in `material`,
// lit from above-left (a raised frame: its outer bevel catches the light on the top and left, the lip at the
// window's edge on the bottom and right). Anodised / Satin: brushed metal; Brass: polished brass, with a rivet
// at each corner once the frame is 6 px or more; NeonGlass: black glass with a neon tube in `neon` (the skin's
// accent) along its middle once the frame is 3 px or more, and on a narrower one (the standard tray below 125 %)
// a neon edge. Flat (or a material this build does not know) draws nothing. Only the ring is written. `cache`
// keeps the ring per size.
struct PhotoBezel {
    SkinMaterial material = SkinMaterial::Flat;
    int          px = 0;
    COLORREF     neon = RGB(244, 55, 148);
    bool operator==(const PhotoBezel&) const = default;
};
class PhotoBezelCache {
public:
    // Only the ring: its top and bottom runs (b rows of w each), then its left and right sides (h - 2b rows of 2b)
    // — at 120 dp and 500 % a whole-meter copy would be MBs per meter, the ring a fraction of it.
    std::vector<uint32_t> ring;
    PhotoBezel            key{};
    int                   w = 0, h = 0;
};
void paintPhotoBezel(uint32_t* px, int stride, int bufH, const RECT& meter, const PhotoBezel& bezel,
                     PhotoBezelCache& cache);

}  // namespace rabbitears
