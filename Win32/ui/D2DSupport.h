// SPDX-License-Identifier: GPL-3.0-or-later
// Shared Direct2D/DirectWrite helpers: process-lifetime factories (created once,
// like the themeBrush cache) plus a COLORREF->D2D color and a SafeRelease. The
// factories are device-independent and immortal, so all D2D controls share them.
// (Ported verbatim from SQLTerminal-Win32/src/ui/D2DSupport.h; namespace only.)
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <string>

#include "ui/Theme.h"  // FontRole / themeFontFamily / dpiScale — themeTextFormat resolves a skin role

namespace rabbitears {

// Single shared ID2D1Factory (single-threaded; all controls live on the UI
// thread). Returns nullptr only if creation fails.
inline ID2D1Factory* d2dFactory() {
    static ID2D1Factory* f = []() -> ID2D1Factory* {
        ID2D1Factory* p = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &p);
        return p;
    }();
    return f;
}

// Single shared IDWriteFactory.
inline IDWriteFactory* dwriteFactory() {
    static IDWriteFactory* f = []() -> IDWriteFactory* {
        IDWriteFactory* p = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&p));
        return p;
    }();
    return f;
}

inline D2D1_COLOR_F colorToD2D(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, 1.0f);
}

// A DirectWrite text format in a skin role's typeface (family via themeFontFamily),
// at an explicit 96-dpi pixel size + weight + horizontal alignment. Single-line,
// vertically centred, no wrap — the channel grid's universal setup. Pass
// `familyOverride` for a face that is NOT one of the skin's typography roles (e.g. the
// ★/☆ favourite dingbat's "Segoe UI Symbol"). Returns nullptr on failure — the caller
// null-checks, exactly like the ad-hoc CreateTextFormat it replaces. The DirectWrite
// analogue of themeFont(): only the *family* is skin-swappable; size/weight are layout.
inline IDWriteTextFormat* themeTextFormat(FontRole role, UINT dpi, int px96,
                                          DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                                          const wchar_t* familyOverride = nullptr) {
    IDWriteFactory* f = dwriteFactory();
    if (!f) return nullptr;
    const std::wstring fam = familyOverride ? std::wstring(familyOverride) : themeFontFamily(role);
    IDWriteTextFormat* out = nullptr;
    if (SUCCEEDED(f->CreateTextFormat(fam.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      static_cast<float>(dpiScale(px96, dpi)), L"", &out)) &&
        out) {
        out->SetTextAlignment(align);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return out;
}

template <class T>
inline void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace rabbitears
