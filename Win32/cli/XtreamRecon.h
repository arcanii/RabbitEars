// SPDX-License-Identifier: GPL-3.0-or-later
// XtreamRecon — gate 1 of the Xtream VOD + Series epic (see Win32/BACKLOG.md).
//
// A one-shot, READ-ONLY reconnaissance probe against a real Xtream-Codes provider,
// answering the three questions the design doc cannot be written without:
//   1. Does `player_api.php` respond at all, or has the panel disabled it?
//   2. What shape do get_vod_streams / get_series / get_series_info actually return?
//   3. Is a playable VOD URL constructible (does `container_extension` exist)?
//
// It is deliberately a separate file from RabbitEarsCli.cpp and deliberately does NOT
// put a JSON parser in common/: the production parser is a later, STRICT thing, and
// this scanner is its opposite — maximally permissive, because the whole point is to
// survive the non-standard shapes we are hunting for and report them rather than
// choke. When the epic lands, delete this pair of files whole.
//
// Everything it prints and saves is credential-redacted by default (see the .cpp) so
// the report can be handed straight back to a developer.
#pragma once

#include <string>

namespace rabbitears {

// `source` is an Xtream playlist URL (the get.php?username=…&password=… form the app
// already stores) — or empty to auto-pick from the app's real DB. `raw` disables
// redaction for an owner who only wants to read it locally. Returns 0 on success.
int xtreamRecon(const std::wstring& source, bool raw);

// Selftest hook. The census scanner and the redactor are the two things that decide
// whether a recon report can be TRUSTED — a scanner bug means the report quietly lies
// and the design doc gets built on it — so they are covered by --selftest like any
// other core logic. They live in an anonymous namespace in the .cpp (nothing outside
// the probe should ever call them), so the assertions run in there and report back
// through the CLI's own expect(), which is passed in.
int xtreamReconSelftest(void (*expect)(bool, const std::string&));

}  // namespace rabbitears
