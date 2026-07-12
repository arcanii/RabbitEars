// SPDX-License-Identifier: GPL-3.0-or-later
// Tr - AppKit (NSString) convenience over the pure i18n catalog (common/core/Strings).
//
// The macOS peer of Win32/ui/Tr.h. Tr(id) converts the active-language UTF-8 string to an
// (autoreleased) NSString for Cocoa APIs; TrF(id, {...}) fills its {0},{1},... placeholders.
// Mac UI call sites include THIS, not core/Strings.h directly, so they get NSStrings.
//
// The string catalog + the active-language global live in common/ (shared with Win32); only
// the NSString conversion and the mac language *preference* are here. The preference is kept
// in NSUserDefaults (key "ui_language"), NOT the settings DB, because the menu bar is built at
// startup BEFORE the DB opens - so the language must be resolvable without it.
#pragma once

#import <Foundation/Foundation.h>
#include <initializer_list>

#include "core/Strings.h"

namespace rabbitears {

// The active-language string for `id`, as an autoreleased NSString. Never nil.
inline NSString* Tr(i18n::StringId id) {
    return [NSString stringWithUTF8String:i18n::trU8(id)];
}

// Ordered placeholder fill: {0} -> args[0], {1} -> args[1], ... Each index is replaced
// everywhere it appears; a nil arg becomes empty. The Japanese template may reorder
// {0}/{1} for grammar - this fills by index, so it is order-independent. Insertions are
// skipped past so an arg that itself contains a "{n}" token can't be re-matched (mirrors trf).
inline NSString* TrF(i18n::StringId id, std::initializer_list<NSString*> args) {
    NSMutableString* s = [NSMutableString stringWithUTF8String:i18n::trU8(id)];
    int i = 0;
    for (NSString* a : args) {
        NSString* token = [NSString stringWithFormat:@"{%d}", i++];
        NSString* val = a ?: @"";
        NSRange search = NSMakeRange(0, s.length);
        for (;;) {
            NSRange hit = [s rangeOfString:token options:0 range:search];
            if (hit.location == NSNotFound) break;
            [s replaceCharactersInRange:hit withString:val];
            NSUInteger next = hit.location + val.length;
            if (next >= s.length) break;
            search = NSMakeRange(next, s.length - next);
        }
    }
    return s;
}

// The mac system UI language, mapped onto our language set (extend as languages are added).
inline i18n::Lang macSystemLang() {
    NSString* code = [[NSLocale preferredLanguages].firstObject lowercaseString];
    if ([code hasPrefix:@"ja"]) return i18n::Lang::Ja;
    return i18n::Lang::En;
}

// Resolve a persisted pref ("system" | "en" | "ja") to the effective language.
inline i18n::Lang macResolveLang(NSString* pref) {
    if ([pref isEqualToString:@"ja"]) return i18n::Lang::Ja;
    if ([pref isEqualToString:@"en"]) return i18n::Lang::En;
    return macSystemLang();  // "system" or anything unrecognized follows the OS
}

// The NSUserDefaults key holding the language preference.
inline NSString* languagePrefKey() { return @"ui_language"; }

// Current persisted pref ("system" if unset).
inline NSString* currentLanguagePref() {
    NSString* p = [[NSUserDefaults standardUserDefaults] stringForKey:languagePrefKey()];
    return p.length ? p : @"system";
}

// Read the persisted pref and set the process-global active language. Call ONCE at startup,
// BEFORE building any UI (menus/windows) so every Tr() reflects the chosen language.
inline void applyStartupLanguage() {
    i18n::setActiveLang(macResolveLang(currentLanguagePref()));
}

// Persist a new language pref. Does NOT change the RUNNING language (setActiveLang is applied
// only at startup, matching Win32) - the app restarts to apply.
inline void setLanguagePref(NSString* code) {
    [[NSUserDefaults standardUserDefaults] setObject:code forKey:languagePrefKey()];
}

}  // namespace rabbitears
