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
// ZlibBound(input.size()) - 8). A ZlibStream fed the whole input.
size_t ZlibCompress(std::string_view input, uint8_t* out, Scratch& scratch);

// A zlib stream made as its input arrives, with ZlibCompress's bytes exactly: the input's whole
// size is given first (it sets the hash table and where the input ends), and only the last 32 KB
// window, the block being parsed and its lookahead are held, about 160 KB whatever the size. The
// output goes to `sink` a block at a time.
class ZlibStream {
public:
    // Receives the stream's bytes in order; false stops the stream.
    using Sink = bool (*)(void* context, const uint8_t* data, size_t size);

    ZlibStream(size_t total, Scratch& scratch, Sink sink, void* context);
    ZlibStream(const ZlibStream&) = delete;
    ZlibStream& operator=(const ZlibStream&) = delete;

    // Adds input; false when the sink failed or the input would pass `total`.
    bool Write(const uint8_t* data, size_t size);
    // Ends the stream; false unless exactly `total` bytes came, or when the sink failed.
    bool Finish();

private:
    bool Advance();
    void CompressBlock();
    bool Emit(const uint8_t* data, size_t size);

    const size_t total_;
    Scratch& scratch_;
    Sink sink_;
    void* context_;
    uint32_t base_ = 0;
    unsigned shift_ = 0;
    std::vector<uint8_t> window_;  // input from position origin_ to filled_
    std::vector<uint8_t> stage_;   // one block's output
    size_t origin_ = 0;
    size_t filled_ = 0;
    size_t at_ = 0;                // where the next block starts
    uint64_t bits_ = 0;            // bits not yet a whole byte, and how many
    unsigned count_ = 0;
    uint32_t adler_ = 1;
    bool anyBlock_ = false;
    bool failed_ = false;
};

// The Adler-32 checksum (RFC 1950) of `size` bytes, as zlib's adler32(adler, data, size). Start
// with 1. SSE2 where the target has it.
uint32_t Adler32(const uint8_t* data, size_t size, uint32_t adler = 1);

} // namespace RayoPdf::Image::Deflate
