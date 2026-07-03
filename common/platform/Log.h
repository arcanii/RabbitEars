// SPDX-License-Identifier: GPL-3.0-or-later
// Lightweight, thread-safe diagnostic log written to
//   %LOCALAPPDATA%\RabbitEars\rabbitears.log   (RABBITEARS_DATA_DIR override; same
// folder as the database). Always-on; the previous run is kept as
// rabbitears.log.1. Exists so we can see what happened on a tester's machine —
// startup environment, playlist/DB outcomes, playback events, and libVLC's own
// network/codec messages (routed in from VlcPlayer). Every line is flushed, so a
// crash still leaves a usable log.
#pragma once

#include <string>

namespace rabbitears::diag {

// Open (rotating the previous run to .log.1) and write the session banner.
// `appVersion` is the human-readable version line (e.g. L"0.1.0 (13)").
void init(const wchar_t* appVersion);
void shutdown();

// Core sink (thread-safe). `level` is a short tag, e.g. L"INFO"/L"WARN"/L"ERROR".
void write(const wchar_t* level, const std::wstring& msg);

inline void info(const std::wstring& m) { write(L"INFO", m); }
inline void warn(const std::wstring& m) { write(L"WARN", m); }
inline void error(const std::wstring& m) { write(L"ERROR", m); }

std::wstring filePath();  // full path to the current log (for surfacing in the UI)

}  // namespace rabbitears::diag
