// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Central color palette for the UI — the shared "Claude-desktop-style" dark look
// with a warm coral accent, kept a matched pair with the sibling apps
// SQLTerminal-Win32 and ManorLords-SGE (ported near-verbatim from
// SQLTerminal-Win32/src/ui/Theme.h; only the namespace differs). Pure GDI
// COLORREFs — no platform deps beyond <windows.h>.
#include <windows.h>

#include <dwmapi.h>
#include <uxtheme.h>

#include <string>  // std::wstring — role font-family names (themeFontFamily / themeTextFormat)

#include "core/Strings.h"  // i18n::activeLang() — switch to a Japanese face when the UI is Japanese

#ifdef RABBITEARS_THEME_ENGINE
#include <unordered_map>

#include "platform/Encoding.h"  // wideFromUtf8 — resolve a skin's UTF-8 font family to a wide name
#include "ui/Skin.h"  // the shared skin model this file resolves into the Win32 COLORREF Theme
#endif

namespace rabbitears {

struct Theme {
    bool dark;
    COLORREF windowBg;       // deepest surface (video letterbox, window erase)
    COLORREF panelBg;        // panels (nav sidebar, grid rows)
    COLORREF panelElevBg;    // headers / elevated bands (title/command bar, grid header, status bar)
    COLORREF altRowBg;       // zebra-stripe row
    COLORREF hoverBg;        // hover highlight
    COLORREF border;         // hairline separators
    COLORREF textPrimary;
    COLORREF textSecondary;
    COLORREF textMuted;
    COLORREF accent;         // primary action / active / now-playing (coral)
    COLORREF accentText;     // text drawn on top of `accent`
    COLORREF selectionBg;    // selected grid row background
    COLORREF selectionText;
#ifdef RABBITEARS_THEME_ENGINE
    COLORREF dangerHover;    // destructive-action hover (close button); skin-driven (Phase 3)
#endif
    COLORREF synKeyword;     // (carried from the shared theme; unused by RabbitEars)
    COLORREF synNumber;
    COLORREF synString;
    COLORREF synComment;
};

inline Theme makeDarkTheme() {
    Theme t{};
    t.dark = true;
    t.windowBg = RGB(22, 22, 24);
    t.panelBg = RGB(26, 26, 28);
    t.panelElevBg = RGB(32, 32, 35);
    t.altRowBg = RGB(28, 28, 31);
    t.hoverBg = RGB(42, 42, 45);
    t.border = RGB(48, 48, 52);
    t.textPrimary = RGB(230, 230, 232);
    t.textSecondary = RGB(154, 154, 160);
    t.textMuted = RGB(106, 106, 112);
    t.accent = RGB(217, 119, 87);       // Claude coral (#D97757)
    t.accentText = RGB(40, 18, 10);
    t.selectionBg = RGB(92, 52, 38);     // muted coral
    t.selectionText = RGB(247, 238, 233);
#ifdef RABBITEARS_THEME_ENGINE
    t.dangerHover = RGB(196, 43, 28);
#endif
    t.synKeyword = RGB(201, 143, 214);
    t.synNumber = RGB(159, 209, 154);
    t.synString = RGB(224, 150, 107);
    t.synComment = RGB(118, 150, 118);
    return t;
}

inline Theme makeLightTheme() {
    Theme t{};
    t.dark = false;
    t.windowBg = GetSysColor(COLOR_WINDOW);
    t.panelBg = GetSysColor(COLOR_WINDOW);
    t.panelElevBg = RGB(244, 244, 246);
    t.altRowBg = RGB(245, 245, 245);
    t.hoverBg = RGB(232, 232, 234);
    t.border = RGB(214, 214, 218);
    t.textPrimary = GetSysColor(COLOR_WINDOWTEXT);
    t.textSecondary = RGB(96, 96, 102);
    t.textMuted = RGB(140, 140, 146);
    t.accent = RGB(193, 95, 60);          // coral, darkened for light bg
    t.accentText = RGB(255, 255, 255);
    t.selectionBg = RGB(250, 232, 224);
    t.selectionText = RGB(74, 27, 12);
#ifdef RABBITEARS_THEME_ENGINE
    t.dangerHover = RGB(196, 43, 28);
#endif
    t.synKeyword = RGB(199, 37, 108);
    t.synNumber = RGB(128, 0, 128);
    t.synString = RGB(196, 26, 22);
    t.synComment = RGB(34, 139, 34);
    return t;
}

// Follow the system light/dark setting (Win10 2004+). Returns false (light) when
// the registry value is missing.
inline bool systemUsesDarkMode() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

// Theme override: -1 = follow system, 0 = force light, 1 = force dark.
inline int& themeOverride() {
    static int mode = -1;
    return mode;
}

#ifdef RABBITEARS_THEME_ENGINE
// ---- theme engine: currentTheme() is resolved from the active Skin ----------
// The persisted selection: a skin id ("dark"/"light") or "system" (follow the OS
// dark/light setting). Default "system" preserves the pre-engine follow-the-OS
// behaviour. The Settings->Theme UI (Phase 2c) sets this + persists it under
// skinSettingKey(); themeOverride() above is unused on this path.
inline std::string& activeSkinSelection() {
    static std::string sel = "system";
    return sel;
}

inline COLORREF toColorRef(const SkinColor& c) { return RGB(c.r, c.g, c.b); }

// Resolve a shared Skin into the Win32 COLORREF Theme. The 4 unused syntax-highlight
// fields keep their classic values (RabbitEars never draws them); `dark` drives the
// DWM immersive-dark chrome. The "dark" skin reproduces makeDarkTheme() exactly, so
// the default (system -> dark on a dark OS) is pixel-identical to the pre-engine look.
inline Theme resolveSkinTheme(const Skin& s) {
    Theme t = makeDarkTheme();  // seed: fills the unused syn* fields with classic values
    t.dark          = s.dark;
    t.windowBg      = toColorRef(s.palette.windowBg);
    t.panelBg       = toColorRef(s.palette.panelBg);
    t.panelElevBg   = toColorRef(s.palette.panelElevBg);
    t.altRowBg      = toColorRef(s.palette.altRowBg);
    t.hoverBg       = toColorRef(s.palette.hoverBg);
    t.border        = toColorRef(s.palette.border);
    t.textPrimary   = toColorRef(s.palette.textPrimary);
    t.textSecondary = toColorRef(s.palette.textSecondary);
    t.textMuted     = toColorRef(s.palette.textMuted);
    t.accent        = toColorRef(s.palette.accent);
    t.accentText    = toColorRef(s.palette.accentText);
    t.selectionBg   = toColorRef(s.palette.selectionBg);
    t.selectionText = toColorRef(s.palette.selectionText);
    t.dangerHover   = toColorRef(s.palette.dangerHover);
    return t;
}
#endif

inline const Theme& currentTheme() {
#ifdef RABBITEARS_THEME_ENGINE
    // Resolve the active selection -> a concrete skin -> a cached COLORREF Theme,
    // rebuilt only when the selection (or, for "system", the OS mode) changes. Called
    // only on the UI thread, so the function-local statics need no synchronisation.
    static Theme cached;
    static std::string cachedKey;
    const std::string& sel = activeSkinSelection();
    std::string skinId = (sel == "system") ? (systemUsesDarkMode() ? "dark" : "light") : sel;
    const std::string key = (sel == "system") ? ("system:" + skinId) : skinId;
    if (key != cachedKey) {
        cached = resolveSkinTheme(skinById(skinId));
        cachedKey = key;
    }
    return cached;
#else
    static const Theme dark = makeDarkTheme();
    static const Theme light = makeLightTheme();
    const int o = themeOverride();
    const bool useDark = (o < 0) ? systemUsesDarkMode() : (o == 1);
    return useDark ? dark : light;
#endif
}

#ifdef RABBITEARS_THEME_ENGINE
// The active Skin (same resolution as currentTheme(): the selection, with "system"
// mapped to dark/light by the OS mode). Used by themeFont() for typography.
inline const Skin& currentSkin() {
    const std::string& sel = activeSkinSelection();
    const std::string skinId = (sel == "system") ? (systemUsesDarkMode() ? "dark" : "light") : sel;
    return skinById(skinId);
}
#endif

// ---- shared dialog dark-mode + DPI helpers ----------------------------------

// Scale a 96-dpi design value to a window's DPI.
inline int dpiScale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

// Typography roles. A role fixes the *typeface* (family + symbol flag); the caller
// chooses the pixel size + weight (that is layout, not theme). themeFont() builds an
// HFONT the caller owns (create-on-demand + DeleteObject, like the ad-hoc CreateFontW
// it replaces). Flag-on the family comes from the active skin; flag-off it is the
// hardwired Segoe UI / MDL2 default — so call sites stay unconditional and the
// shipping look is byte-for-byte unchanged.
enum class FontRole { Body, Title, Glyph };

#ifdef RABBITEARS_THEME_ENGINE
inline const SkinFont& skinFontForRole(FontRole role) {
    const Skin& s = currentSkin();
    return (role == FontRole::Title) ? s.title : (role == FontRole::Glyph) ? s.glyph : s.body;
}
#endif

// Whether the role is a symbol/icon face (drives the GDI pitch hint below + the JP-font opt-out).
inline bool themeFontIsSymbol(FontRole role) {
#ifdef RABBITEARS_THEME_ENGINE
    return skinFontForRole(role).symbol;
#else
    return role == FontRole::Glyph;
#endif
}

// The role's typeface family, wide (skin-driven flag-on; hardwired flag-off). Shared by the GDI
// themeFont() below and the DirectWrite themeTextFormat() (D2DSupport.h). When the UI language is
// Japanese, every NON-symbol role switches to a Japanese UI face: "Segoe UI" carries no CJK glyphs,
// so GDI menus/dialogs would render tofu (DirectWrite falls back on its own, but pinning the face
// keeps both stacks consistent). The MDL2 glyph (symbol) role is never touched, and this override
// deliberately wins over a skin's display face so Japanese stays legible under every skin.
inline std::wstring themeFontFamily(FontRole role) {
    std::wstring fam =
#ifdef RABBITEARS_THEME_ENGINE
        wideFromUtf8(skinFontForRole(role).family);
#else
        role == FontRole::Glyph ? std::wstring(L"Segoe MDL2 Assets") : std::wstring(L"Segoe UI");
#endif
    if (i18n::activeLang() == i18n::Lang::Ja && !themeFontIsSymbol(role))
        fam = L"Yu Gothic UI";  // Win10/11 Japanese UI face (Meiryo UI is the documented fallback)
    return fam;
}

// Build an HFONT in the role's typeface at an explicit 96-dpi pixel size + weight
// (FW_*). This is the seam the migrated call sites use — each keeps the size/weight
// it always drew, so only the *family* is now skin-swappable.
inline HFONT themeFont(FontRole role, UINT dpi, int px96, int weight) {
    const std::wstring fam = themeFontFamily(role);
    const DWORD pitch =
        themeFontIsSymbol(role) ? (DEFAULT_PITCH | FF_DONTCARE) : (VARIABLE_PITCH | FF_SWISS);
    return CreateFontW(-dpiScale(px96, dpi), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, pitch, fam.c_str());
}

// Convenience: the role's *default* size + weight — skin-carried flag-on (SkinFont
// sizePt/weight), the classic chrome sizes flag-off (Body 14, Title 16 semibold,
// Glyph 13). Used for the shell chrome (MainWindow); other sites pass a size above.
inline HFONT themeFont(FontRole role, UINT dpi) {
#ifdef RABBITEARS_THEME_ENGINE
    const SkinFont& f = skinFontForRole(role);
    const int px96 = static_cast<int>(f.sizePt * 4.0 / 3.0 + 0.5);  // pt -> px @96dpi
    return themeFont(role, dpi, px96, f.weight);
#else
    switch (role) {
        case FontRole::Title: return themeFont(role, dpi, 16, FW_SEMIBOLD);
        case FontRole::Glyph: return themeFont(role, dpi, 13, FW_NORMAL);
        case FontRole::Body:
        default:              return themeFont(role, dpi, 14, FW_NORMAL);
    }
#endif
}

#ifdef RABBITEARS_THEME_ENGINE
// Per-skin brush set: an unbounded colour->HBRUSH cache, replacing the fragile 12-slot
// array (which silently overflowed + leaked once a second skin's palette was in play).
// clearThemeBrushes() frees + drops them on a skin switch (Phase 2c calls it before the
// repaint). Correctness never depends on the clear — a colour always maps to its brush.
inline std::unordered_map<COLORREF, HBRUSH>& themeBrushCache() {
    static std::unordered_map<COLORREF, HBRUSH> cache;
    return cache;
}
inline void clearThemeBrushes() {
    for (auto& kv : themeBrushCache()) DeleteObject(kv.second);
    themeBrushCache().clear();
}
inline HBRUSH themeBrush(COLORREF c) {
    auto& cache = themeBrushCache();
    const auto it = cache.find(c);
    if (it != cache.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    cache[c] = b;
    return b;
}
#else
// Small process-lifetime brush cache so WM_CTLCOLOR* can return a stable HBRUSH.
inline HBRUSH themeBrush(COLORREF c) {
    static COLORREF colors[12];
    static HBRUSH brushes[12];
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (colors[i] == c) return brushes[i];
    HBRUSH b = CreateSolidBrush(c);
    if (count < 12) {
        colors[count] = c;
        brushes[count] = b;
        ++count;
    }
    return b;
}
#endif

// Handle WM_CTLCOLOR{STATIC,EDIT,LISTBOX,BTN}: dark field/background, light text.
inline LRESULT dialogCtlColor(UINT msg, WPARAM wParam) {
    const Theme& th = currentTheme();
    HDC hdc = reinterpret_cast<HDC>(wParam);
    const COLORREF bg =
        (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) ? th.windowBg : th.panelBg;
    SetTextColor(hdc, th.textPrimary);
    SetBkColor(hdc, bg);
    return reinterpret_cast<LRESULT>(themeBrush(bg));
}

// Theme a popup dialog + its child controls dark (Win10 1809+; no-op otherwise).
inline void applyDialogDarkMode(HWND dlg) {
    const Theme& th = currentTheme();
    BOOL dark = th.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(dlg, 20 /*USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF cap = th.panelElevBg, txt = th.textSecondary, bdr = th.border;
    DwmSetWindowAttribute(dlg, 35 /*CAPTION_COLOR*/, &cap, sizeof(cap));
    DwmSetWindowAttribute(dlg, 36 /*TEXT_COLOR*/, &txt, sizeof(txt));
    DwmSetWindowAttribute(dlg, 34 /*BORDER_COLOR*/, &bdr, sizeof(bdr));
    if (!th.dark) return;
    if (HMODULE ux = GetModuleHandleW(L"uxtheme.dll")) {
        using AllowFn = BOOL(WINAPI*)(HWND, BOOL);
        if (auto allow = reinterpret_cast<AllowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133))))
            allow(dlg, TRUE);
    }
    EnumChildWindows(
        dlg,
        [](HWND child, LPARAM) -> BOOL {
            wchar_t cls[64] = L"";
            GetClassNameW(child, cls, 64);
            SetWindowTheme(child,
                           lstrcmpiW(cls, L"COMBOBOX") == 0 ? L"DarkMode_CFD" : L"DarkMode_Explorer",
                           nullptr);
            return TRUE;
        },
        0);
}

}  // namespace rabbitears
