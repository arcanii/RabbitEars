// SPDX-License-Identifier: GPL-3.0-or-later
// Lightweight, thread-safe diagnostic log written to
//   %LOCALAPPDATA%\RabbitEars\rabbitears.log   (RABBITEARS_DATA_DIR override; same
// folder as the database). Always-on; the previous run is kept as
// rabbitears.log.1. Exists so we can see what happened on a tester's machine —
// startup environment, playlist/DB outcomes, playback events, and libVLC's own
// network/codec messages (routed in from VlcPlayer). Every line is flushed, so a
// crash still leaves a usable log.
#pragma once

#include <atomic>
#include <string>

namespace rabbitears::diag {

// Open (rotating the previous run to .log.1) and write the session banner.
// `appVersion` is the human-readable version line (e.g. L"0.1.0 (13)").
void init(const wchar_t* appVersion);
void shutdown();

// Core sink (thread-safe). `level` is a short tag, e.g. L"INFO"/L"WARN"/L"ERROR".
// NB this writes UNCONDITIONALLY — severity filtering lives in the helpers below, so that
// the sink (and therefore BOTH platform implementations, Win32/platform/Log.cpp and
// mac/platform/Log.mm) needs no changes to gain levels.
void write(const wchar_t* level, const std::wstring& msg);

// ---- severity levels -------------------------------------------------------------------
// Standard ordering, least→most severe. The active threshold drops anything BELOW it, so
// Info (the default, == the historical always-on behaviour) keeps Info/Warn/Error and hides
// the chatty Debug/Trace that exist for a tester chasing a specific bug.
//
// There is deliberately NO "Off": this log's whole reason to exist is that a tester can send
// it after something went wrong (see the file comment), and a silent build defeats that.
// Error is the quietest setting, and errors always survive.
//
// Everything here is header-only + inline on purpose. The threshold lives in a function-local
// static (one instance across all TUs, per the inline rule), so adding levels costs the two
// platform sinks exactly nothing — important, because mac implements this same header.
enum class Level { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

inline std::atomic<int>& levelRef() {
    static std::atomic<int> lvl{static_cast<int>(Level::Info)};
    return lvl;
}
inline void  setLevel(Level l) { levelRef().store(static_cast<int>(l), std::memory_order_relaxed); }
inline Level level() { return static_cast<Level>(levelRef().load(std::memory_order_relaxed)); }
// Cheap guard — use it to skip building an expensive message, e.g.
//   if (diag::enabled(diag::Level::Debug)) diag::debug(expensiveDump());
inline bool  enabled(Level l) {
    return static_cast<int>(l) >= levelRef().load(std::memory_order_relaxed);
}

inline void trace(const std::wstring& m) { if (enabled(Level::Trace)) write(L"TRACE", m); }
inline void debug(const std::wstring& m) { if (enabled(Level::Debug)) write(L"DEBUG", m); }
inline void info(const std::wstring& m)  { if (enabled(Level::Info))  write(L"INFO", m); }
inline void warn(const std::wstring& m)  { if (enabled(Level::Warn))  write(L"WARN", m); }
inline void error(const std::wstring& m) { if (enabled(Level::Error)) write(L"ERROR", m); }

// Stable tokens for the settings K/V (shared by both platforms — do not rename).
inline const char* levelToString(Level l) {
    switch (l) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
        case Level::Info:
        default:           return "info";
    }
}
inline Level levelFromString(const std::string& s, Level fallback = Level::Info) {
    if (s == "trace") return Level::Trace;
    if (s == "debug") return Level::Debug;
    if (s == "info")  return Level::Info;
    if (s == "warn")  return Level::Warn;
    if (s == "error") return Level::Error;
    return fallback;  // unknown token -> caller's default (meter/skin codec discipline)
}

std::wstring filePath();  // full path to the current log (for surfacing in the UI)

}  // namespace rabbitears::diag
