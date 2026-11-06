// SPDX-License-Identifier: Apache-2.0
// Forked from RayoMD src/core/flate.h at ef106ca (Apache-2.0, same author).
// A DEFLATE (RFC 1951) encoder in the zlib format (RFC 1950) for the IDAT data of the PNG files
// the image module writes: filtered pixel rows. Greedy matching over one hash table keyed on 8
// bytes (two candidates a position), and per block of at most 64 KB the cheapest of a stored, a
// fixed-code and a dynamic-code block by exact bit count, so the output is never much larger
// than stored blocks. Output depends only on the input. RayoMD's segmenting of an image for
// threads and its second, 4-byte table for page content are left out: one image is one stream
// compressed on one thread (pages run in parallel instead), and on filtered pixels the 4-byte
// table finds short distant matches that cost as many bits as the literals they replace and up
// to six times the time (RayoMD, 2026-10-13).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace RayoPdf::Image::Deflate {

// Working memory kept from call to call: about 300 KB once used. One per thread.
struct Scratch {
    std::vector<uint32_t> table;   // two most recent positions per bucket, keyed on 8 bytes
    std::vector<uint32_t> tokens;  // literal byte, or length << 16 | (distance - 1)
    uint32_t base = 0;             // table position of the next input's first byte
};

// The largest input ZlibCompress takes.
constexpr size_t kMaxInput = (size_t(1) << 31) - (size_t(1) << 20);

// The most bytes ZlibCompress writes for `size` input bytes (stored blocks, at worst), plus the
// 8 bytes its whole-word stores may touch past the end of the output.
inline size_t ZlibBound(size_t size) {
    const size_t blocks = std::max<size_t>(1, (size + 65277) / 65278);
    return size + 5 * blocks + 6 + 8;
}

// Compresses `input`, at most kMaxInput bytes, to `out`, which has room for
// ZlibBound(input.size()) bytes; returns the number of bytes written (at most
// ZlibBound(input.size()) - 8).
size_t ZlibCompress(std::string_view input, uint8_t* out, Scratch& scratch);

// The Adler-32 checksum (RFC 1950) of `size` bytes, as zlib's adler32(adler, data, size). Start
// with 1. SSE2 where the target has it.
uint32_t Adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

} // namespace RayoPdf::Image::Deflate
