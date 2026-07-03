// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone themed modal dialogs (no AppState dependency): the About box and a
// single-line text prompt. Split out of MainWindow.cpp to keep it focused.
#pragma once

#include <set>
#include <string>
#include <vector>

#include <windows.h>

namespace rabbitears {

// Modal About box: artwork + name/version + libVLC attribution + Check-for-Updates.
void showAbout(HWND parent, HINSTANCE hInst, UINT dpi);

// Modal single-line prompt (used for the Add-Playlist URL). Returns true if the
// user pressed OK, with `value` holding the edited text.
bool promptText(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                const std::wstring& label, std::wstring& value);

// First-run Terms-of-Use gate. Modal; returns true if the user accepted, false if
// they declined (the caller should then exit the app). Blocks until answered.
bool showTerms(HWND parent, HINSTANCE hInst, UINT dpi);

// Modal themed info popup: a bold one-line `summary` headline over a scrollable,
// read-only `details` body. Used for post-import results and other notices.
void showInfoDialog(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                    const std::wstring& summary, const std::wstring& details);

// Modal categories checklist (Settings → Categories…). `allGroups` is every
// distinct group title; `checked` is in/out — the initially-checked groups on
// entry, the user's final selection on exit. A live filter box, Select All /
// Clear, and a running count help wrangle large libraries. Returns true if OK was
// pressed (on Cancel, returns false and leaves `checked` untouched).
bool chooseCategories(HWND parent, HINSTANCE hInst, UINT dpi,
                      const std::vector<std::wstring>& allGroups,
                      std::set<std::wstring>& checked);

}  // namespace rabbitears
