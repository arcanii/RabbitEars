// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which of libVLC's log lines are routine enough to keep below the diag log's default level.
//
// libVLC's HTTP/2 code (compiled into both libadaptive_plugin — HLS/DASH — and libhttps_plugin) logs,
// at ERROR, every stream it resets with an error code, as "local stream %u error: %s (0x%X)". Two of
// those were seen on a channel that played normally: "Cancellation (0x8)" — it cancelled a request it
// no longer needed (every ~6 s on an HLS FAST channel in the owner's log, ~150 lines in 15 minutes) —
// and "Stream closed (0x5)", its answer to frames that arrive on a stream it has already closed (in
// the owner's 2026-09-27 log, five of them within 2 ms of a Cancellation, on the same stream). VlcEngine logs these at Debug; the
// session's first one keeps its level, with a note, so the log still shows that they happen, and the
// engine's shutdown logs how many more there were — a stream in trouble can send the same lines (a
// fetch cancelled again and again), and the count keeps that visible at the default level.
//
// Only "local" resets, and only those two codes: a "peer stream" error (the server reset one of
// ours) and every other code (a protocol, flow-control or internal error) keep their level.
//
// Header-inline so RabbitEarsCli's --selftest can check it.
#pragma once

#include <string_view>

namespace rabbitears {

// `line` is exactly "local stream <number> error: Cancellation (0x8)" or "… Stream closed (0x5)".
inline bool isRoutineH2Reset(std::string_view line) {
    constexpr std::string_view kPrefix = "local stream ";
    if (line.substr(0, kPrefix.size()) != kPrefix) return false;
    line.remove_prefix(kPrefix.size());
    size_t digits = 0;  // %u: 1–10 decimal digits
    while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') ++digits;
    if (digits == 0 || digits > 10) return false;
    line.remove_prefix(digits);
    return line == " error: Cancellation (0x8)" || line == " error: Stream closed (0x5)";
}

}  // namespace rabbitears
