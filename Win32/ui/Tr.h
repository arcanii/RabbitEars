// SPDX-License-Identifier: GPL-3.0-or-later
// Tr - Win32 wide-string convenience over the pure i18n catalog (common/core/Strings).
//
// tr(id) converts the active-language UTF-8 string to std::wstring for Win32 APIs; trf(id, {...})
// fills its {0},{1},... placeholders. Win32 UI call sites include THIS, not core/Strings.h directly,
// so they get wide strings. The catalog + language state live in common/ (mac-shareable); the wide
// conversion is Windows-only, so it lives here.
#pragma once

#include <initializer_list>
#include <string>

#include "core/Strings.h"
#include "platform/Encoding.h"  // wideFromUtf8

namespace rabbitears {

// The active-language string for `id`, as wide text.
inline std::wstring tr(i18n::StringId id) { return wideFromUtf8(i18n::trU8(id)); }

// Ordered placeholder fill: every {0} is replaced by args[0], {1} by args[1], and so on (each
// index replaced everywhere it appears). Values arrive already wide (channel names, counts via
// std::to_wstring). Missing indices are left as-is; extra args are ignored. The Japanese template
// may reorder {0}/{1} for grammar - this fills by index, so order-independent.
inline std::wstring trf(i18n::StringId id, std::initializer_list<std::wstring> args) {
    std::wstring s = wideFromUtf8(i18n::trU8(id));
    int i = 0;
    for (const std::wstring& a : args) {
        const std::wstring token = L"{" + std::to_wstring(i++) + L"}";
        // Resume PAST each insertion so an arg that itself contains "{n}" can't re-match (or loop).
        for (std::size_t pos = 0; (pos = s.find(token, pos)) != std::wstring::npos; pos += a.size())
            s.replace(pos, token.size(), a);
    }
    return s;
}

// The system UI language, mapped onto our two-language set (extend when languages are added).
inline i18n::Lang systemLang() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE ? i18n::Lang::Ja
                                                                      : i18n::Lang::En;
}

// Resolve a persisted "ui_language" preference ("system" | "en" | "ja") to the effective language.
// Anything unrecognized (or "system") follows the OS.
inline i18n::Lang resolveLang(const std::wstring& pref) {
    if (pref == L"ja") return i18n::Lang::Ja;
    if (pref == L"en") return i18n::Lang::En;
    return systemLang();
}

}  // namespace rabbitears
