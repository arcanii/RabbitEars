// SPDX-License-Identifier: GPL-3.0-or-later
// Gzip — minimal gunzip for EPG (XMLTV) feeds.
//
// WinHTTP and NSURLSession transparently decompress *transfer-encoded* gzip
// (an HTTP `Content-Encoding: gzip` response), but XMLTV guides are very commonly
// served as `.xml.gz` *file bodies* (`Content-Type: application/gzip`), which
// therefore arrive still-compressed. gunzipIfNeeded() inflates those and passes
// non-gzip bytes straight through, so a caller can pipe any fetched EPG payload
// into parseXmltv() without caring how it was compressed.
#pragma once

#include <string>

namespace rabbitears {

// If `bytes` starts with the gzip magic (1F 8B), decode the single gzip member and
// return the original data; otherwise return `bytes` unchanged. Returns an empty
// string on a malformed/truncated/unsupported gzip stream (callers treat empty as a
// failure). Only the first member of a multi-member stream is decoded.
std::string gunzipIfNeeded(const std::string& bytes);

}  // namespace rabbitears
