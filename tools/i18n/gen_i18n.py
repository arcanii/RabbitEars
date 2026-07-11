#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# i18n code generator. The SOURCE OF TRUTH is the JSON under common/i18n/:
#   languages.json  ordered list of languages; order == the Lang enum order; first = reference (en)
#   keys.json       ordered list of {id, cluster, placeholders, comment}; order == StringId order
#   <code>.json     { StringId: "translated text" } — one per language (en.json, ja.json, ...)
#
# It emits common/core/Strings.{h,cpp} (an enum-indexed per-language table). The generated files
# ARE committed so the build needs no Python; re-run this after editing any JSON and commit both.
#
# Adding a language: add its <code>.json, add an entry to languages.json (code/enum/endonym), wire
# Lang -> menu/resolve in Win32/ui/Tr.h + MainWindowCommands.cpp, then run this. The completeness
# check below (and the CLI selftest) fail loudly until every key is translated.
#
# Usage:
#   python tools/i18n/gen_i18n.py            validate + (re)generate common/core/Strings.{h,cpp}
#   python tools/i18n/gen_i18n.py --check    validate + verify the C++ is up to date (CI; no write)
#   python tools/i18n/gen_i18n.py --review <code>
#                                            write common/i18n/review-<code>.md — an English/target
#                                            side-by-side for a native reviewer to confirm wording
#                                            (read-only; corrections go back into <code>.json).
import json, os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
I18N = os.path.join(ROOT, 'common', 'i18n')
OUT_H = os.path.join(ROOT, 'common', 'core', 'Strings.h')
OUT_CPP = os.path.join(ROOT, 'common', 'core', 'Strings.cpp')
QUOTE = chr(34); BS = chr(92)
TOKEN = re.compile(r'\{\d+\}|%[ds%]')   # {0}.. , %d, %s, %% — the placeholder multiset we parity-check


def die(msg):
    sys.stderr.write('gen_i18n: ' + msg + '\n'); sys.exit(1)


def load(name):
    with open(os.path.join(I18N, name), encoding='utf-8') as f:
        return json.load(f)


def cpp_escape(s):
    out = []
    for c in s:
        if c == '\n': out.append(BS + 'n')
        elif c == '\r': out.append(BS + 'r')
        elif c == '\t': out.append(BS + 't')
        elif c == QUOTE: out.append(BS + QUOTE)
        elif c == BS: out.append(BS + BS)
        else: out.append(c)
    return ''.join(out)


def tokens(s):
    return sorted(TOKEN.findall(s))


def write_review(keys, ref_code, tgt_code, ref_strings, tgt_strings):
    lines = ['# i18n review: %s (against %s)' % (tgt_code, ref_code), '',
             'Read the reference and target side by side; put corrections into `%s.json` and re-run'
             ' the generator. Rows are grouped by UI context.' % tgt_code, '']
    cur = None
    for k in keys:
        if k['cluster'] != cur:
            cur = k['cluster']
            lines += ['', '## %s' % cur, '', '| Key | %s | %s |' % (ref_code, tgt_code),
                      '|-----|------|------|']
        def cell(s):
            return (s or '').replace('|', chr(92) + '|').replace('\n', '<br>').replace('\r', '')
        lines.append('| `%s` | %s | %s |' %
                     (k['id'], cell(ref_strings.get(k['id'])), cell(tgt_strings.get(k['id']))))
    return '\n'.join(lines) + '\n'


def main():
    check = '--check' in sys.argv
    review = sys.argv[sys.argv.index('--review') + 1] if '--review' in sys.argv else None
    langs = load('languages.json')          # [{code, enum, endonym, reference?}]
    keys = load('keys.json')                # [{id, cluster, placeholders, comment}]
    if not langs:
        die('languages.json is empty')
    ref = next((l for l in langs if l.get('reference')), langs[0])
    strings = {l['code']: load(l['code'] + '.json') for l in langs}

    if review:
        if review not in strings:
            die('no such language "%s" (have: %s)' % (review, ', '.join(strings)))
        out = os.path.join(I18N, 'review-%s.md' % review)
        open(out, 'w', encoding='utf-8', newline='\n').write(
            write_review(keys, ref['code'], review, strings[ref['code']], strings[review]))
        print('gen_i18n: wrote ' + out)
        return

    # ---- validate ----
    ids = [k['id'] for k in keys]
    if len(ids) != len(set(ids)):
        die('duplicate ids in keys.json')
    for i in ids:
        if not re.match(r'^[A-Za-z_]\w*$', i):
            die('id is not a valid C identifier: ' + i)
    problems = []
    for k in keys:
        i = k['id']
        reftext = strings[ref['code']].get(i)
        for l in langs:
            t = strings[l['code']].get(i)
            if t is None or t == '':
                problems.append('%s: missing %s translation' % (l['code'], i))
            elif reftext is not None and tokens(t) != tokens(reftext):
                problems.append('%s: placeholder mismatch for %s (%s vs %s=%s)'
                                % (l['code'], i, tokens(t), ref['code'], tokens(reftext)))
    # every language file must cover exactly the key set (no orphan translations)
    keyset = set(ids)
    for l in langs:
        extra = set(strings[l['code']]) - keyset
        if extra:
            problems.append('%s.json has %d keys not in keys.json: %s'
                            % (l['code'], len(extra), sorted(extra)[:5]))
    if problems:
        die('validation failed:\n  ' + '\n  '.join(problems))

    # ---- emit ----
    lang_enum = ', '.join(l['enum'] for l in langs)
    enum_lines, arr = [], {l['code']: [] for l in langs}
    cur = None
    for k in keys:
        cl = k['cluster']
        if cl != cur:
            enum_lines.append('    // --- %s ---' % cl)
            for l in langs: arr[l['code']].append('    // --- %s ---' % cl)
            cur = cl
        enum_lines.append('    %s,' % k['id'])
        for l in langs:
            arr[l['code']].append('    "%s",  // %s' % (cpp_escape(strings[l['code']][k['id']]), k['id']))

    h = HEADER_H.replace('@@LANG_ENUM@@', lang_enum).replace('@@ENUM@@', '\n'.join(enum_lines))
    tables = '\n\n'.join(
        '// %s\nconstexpr std::array<const char*, N> k%s = {{\n%s\n}};' %
        (l['endonym'], l['enum'], '\n'.join(arr[l['code']])) for l in langs)
    table_ptrs = ', '.join('&k%s' % l['enum'] for l in langs)
    cpp = HEADER_CPP.replace('@@TABLES@@', tables).replace('@@TABLE_PTRS@@', table_ptrs)

    if check:
        # Compare EOL-agnostically: the committed C++ is LF, but a Windows checkout (core.autocrlf)
        # may present it as CRLF — that must NOT read as drift.
        def norm(s):
            return s.replace('\r\n', '\n').replace('\r', '\n')
        cur_h = open(OUT_H, encoding='utf-8', newline='').read() if os.path.exists(OUT_H) else ''
        cur_cpp = open(OUT_CPP, encoding='utf-8', newline='').read() if os.path.exists(OUT_CPP) else ''
        if norm(cur_h) != norm(h) or norm(cur_cpp) != norm(cpp):
            die('common/core/Strings.{h,cpp} are stale — run  python tools/i18n/gen_i18n.py')
        print('gen_i18n: OK (%d keys x %d languages, C++ up to date)' % (len(keys), len(langs)))
        return
    open(OUT_H, 'w', encoding='utf-8', newline='\n').write(h)
    open(OUT_CPP, 'w', encoding='utf-8', newline='\n').write(cpp)
    print('gen_i18n: wrote Strings.{h,cpp} — %d keys x %d languages (%s)'
          % (len(keys), len(langs), ', '.join(l['code'] for l in langs)))


HEADER_H = '''// SPDX-License-Identifier: GPL-3.0-or-later
// GENERATED by tools/i18n/gen_i18n.py from common/i18n/*.json — DO NOT EDIT BY HAND.
// Edit the JSON (keys.json / <lang>.json / languages.json) and re-run the generator.
//
// The pure i18n string catalog. enum StringId indexes a per-language table; array-indexed-by-enum
// makes "every key exists in every language" a generator- AND compile-time guarantee. Canonical
// storage is UTF-8 (const char*), converted to std::wstring at the Win32 boundary (Win32/ui/Tr.h).
// No Win32, no DB — unit-tested headlessly in the CLI selftest. Lives in common/ so both platforms
// share it and the completeness selftest runs on both CIs.
#pragma once

namespace rabbitears {
namespace i18n {

enum class Lang { @@LANG_ENUM@@ };  // order matches common/i18n/languages.json (first = reference)

enum class StringId {
@@ENUM@@
    Count  // sentinel — MUST stay last
};

// Process-global effective language. English until set. Backed by a relaxed std::atomic so the
// splash worker thread can read it (to localize captions) while the UI thread sets it at startup.
Lang activeLang();
void setActiveLang(Lang);

// UTF-8 lookup. Never returns null. The 1-arg form uses the active language.
const char* trU8(StringId id);
const char* trU8(StringId id, Lang lang);

// Selftest hooks: catalogIsComplete is true iff every id is non-empty in every language (sets
// *firstMissing on failure). placeholderCount counts {n}/%d/%s tokens (for cross-language parity).
bool catalogIsComplete(StringId* firstMissing);
int  placeholderCount(StringId id, Lang lang);

}  // namespace i18n
}  // namespace rabbitears
'''

HEADER_CPP = '''// SPDX-License-Identifier: GPL-3.0-or-later
// GENERATED by tools/i18n/gen_i18n.py from common/i18n/*.json — DO NOT EDIT BY HAND.
#include "core/Strings.h"

#include <array>
#include <atomic>
#include <cstddef>

namespace rabbitears {
namespace i18n {
namespace {

constexpr std::size_t N = static_cast<std::size_t>(StringId::Count);

@@TABLES@@

// One row per Lang value, in enum order — trU8 indexes this by static_cast<size_t>(lang).
constexpr const std::array<const char*, N>* kTables[] = { @@TABLE_PTRS@@ };
constexpr std::size_t kLangCount = sizeof(kTables) / sizeof(kTables[0]);

// Atomic: the splash worker thread reads the active language (via trU8) to localize its captions
// while the UI thread sets it during startup. Relaxed is enough — no ordering dependency, we only
// need race-free reads/writes of this one enum.
std::atomic<Lang> g_lang{Lang::En};

}  // namespace

Lang activeLang() { return g_lang.load(std::memory_order_relaxed); }
void setActiveLang(Lang l) { g_lang.store(l, std::memory_order_relaxed); }

const char* trU8(StringId id, Lang lang) {
    const std::size_t i = static_cast<std::size_t>(id);
    const std::size_t l = static_cast<std::size_t>(lang);
    if (i >= N || l >= kLangCount) return "";
    const char* s = (*kTables[l])[i];
    return s ? s : "";
}
const char* trU8(StringId id) { return trU8(id, g_lang.load(std::memory_order_relaxed)); }

bool catalogIsComplete(StringId* firstMissing) {
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t l = 0; l < kLangCount; ++l)
            if (!(*kTables[l])[i] || !(*kTables[l])[i][0]) {
                if (firstMissing) *firstMissing = static_cast<StringId>(i);
                return false;
            }
    return true;
}

int placeholderCount(StringId id, Lang lang) {
    const char* s = trU8(id, lang);
    int n = 0;
    for (const char* p = s; *p; ++p) {
        if (*p == '{' && p[1] >= '0' && p[1] <= '9') ++n;            // {0}, {1}, ...
        else if (*p == '%' && (p[1] == 'd' || p[1] == 's')) ++n;    // %d / %s (%% is skipped)
    }
    return n;
}

}  // namespace i18n
}  // namespace rabbitears
'''

if __name__ == '__main__':
    main()
