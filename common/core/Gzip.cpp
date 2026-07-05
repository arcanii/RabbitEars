// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Gzip.h"

#include <cstddef>

#include "miniz.h"

namespace rabbitears {
namespace {

// RFC 1952 §2.3.1 — FLG bits for the optional gzip header fields.
constexpr unsigned char kFhcrc = 1u << 1;
constexpr unsigned char kFextra = 1u << 2;
constexpr unsigned char kFname = 1u << 3;
constexpr unsigned char kFcomment = 1u << 4;

unsigned char at(const std::string& b, size_t i) { return static_cast<unsigned char>(b[i]); }

}  // namespace

std::string gunzipIfNeeded(const std::string& bytes) {
    const size_t n = bytes.size();
    // A gzip member is at minimum the 10-byte fixed header + 8-byte trailer. Anything
    // shorter, or lacking the 1F 8B magic, is treated as already-plain and passed through.
    if (n < 18 || at(bytes, 0) != 0x1f || at(bytes, 1) != 0x8b) return bytes;
    if (at(bytes, 2) != 0x08) return {};  // CM must be 8 (DEFLATE) — the only defined method

    const unsigned char flg = at(bytes, 3);
    size_t pos = 10;  // magic(2) CM(1) FLG(1) MTIME(4) XFL(1) OS(1)

    // Optional fields follow the fixed header in this exact order.
    if (flg & kFextra) {
        if (pos + 2 > n) return {};
        const size_t xlen = at(bytes, pos) | (static_cast<size_t>(at(bytes, pos + 1)) << 8);
        pos += 2 + xlen;
    }
    if (flg & kFname) {  // NUL-terminated original filename
        while (pos < n && bytes[pos] != '\0') ++pos;
        ++pos;  // step past the NUL
    }
    if (flg & kFcomment) {  // NUL-terminated comment
        while (pos < n && bytes[pos] != '\0') ++pos;
        ++pos;
    }
    if (flg & kFhcrc) pos += 2;  // CRC16 of the header

    // Need a non-empty raw-DEFLATE body plus the 8-byte trailer (CRC32 + ISIZE).
    if (pos + 8 > n) return {};
    const size_t deflateLen = n - 8 - pos;

    // flags=0 => raw DEFLATE, no zlib wrapper (gzip frames raw deflate), heap output
    // grown by miniz. We don't verify the trailing CRC32/ISIZE — a corrupt stream
    // simply fails to inflate.
    size_t outLen = 0;
    void* out = tinfl_decompress_mem_to_heap(bytes.data() + pos, deflateLen, &outLen, 0);
    if (!out) return {};
    std::string result(static_cast<const char*>(out), outLen);
    mz_free(out);
    return result;
}

}  // namespace rabbitears
