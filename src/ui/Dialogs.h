// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone themed modal dialogs (no AppState dependency): the About box and a
// single-line text prompt. Split out of MainWindow.cpp to keep it focused.
#pragma once

#include <string>

#include <windows.h>

namespace rabbitears {

// Modal About box: artwork + name/version + libVLC attribution + Check-for-Updates.
void showAbout(HWND parent, HINSTANCE hInst, UINT dpi);

// Modal single-line prompt (used for the Add-Playlist URL). Returns true if the
// user pressed OK, with `value` holding the edited text.
bool promptText(HWND parent, HINSTANCE hInst, UINT dpi, const std::wstring& title,
                const std::wstring& label, std::wstring& value);

}  // namespace rabbitears
