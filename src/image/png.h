// SPDX-License-Identifier: Apache-2.0
// Internal to the image module: the pieces of the PNG writer (PNG specification, 3rd edition,
// sections 5, 9 and 11) that the tests check on their own. EncodePng (image.h) puts them
// together.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define RAYOPDF_PNG_SSE2 1
#endif

namespace RayoPdf::Image::Png {

// The CRC-32 of PNG chunks (ISO 3309, reflected 0xEDB88320): start with 0, pass the result on to
// continue over more bytes. Eight bytes a step through eight tables (slicing-by-8).
uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

// PNG row filters. One row per call; `up` is the row above, or zeros for the first row, and `bpp`
// the bytes per pixel (1 to 4). The scalar function is the reference that the SSE2 one matches
// byte for byte (the tests compare them).
//
// Returns the filter, 0 to 4, that the PNG encoder heuristic picks for `row` (the smallest sum of
// the residuals' magnitudes read as signed bytes, the lowest filter on ties) and writes the
// residuals under it: `out` holds four slots of rowBytes + 1 bytes, one for each of filters 1 to
// 4, the residuals from a slot's second byte on. Only the returned filter's slot is defined
// (filter 0 writes none: its residuals are the row); first bytes are never written.
int FilterBest(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out);
int FilterBestScalar(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out);

// Row y (rowBytes bytes) of an image EncodeRows writes, asked for once each, top to bottom; the
// pointer stays valid until the call after next (the filters read the row above). Null ends the
// image early: the writer then fails.
using RowSource = const uint8_t* (*)(void* context, uint32_t y);
// Where a PNG file goes while it is made: bytes appended in order, and bytes already appended set
// again in place (the IDAT chunk's length, known when the compressed data ends).
class Output {
public:
    virtual bool Append(const uint8_t* data, size_t size) = 0;
    virtual bool Patch(uint64_t offset, const uint8_t* data, size_t size) = 0;

protected:
    ~Output() = default;
};

// Each writer makes the file into an Output (false when it cannot be made, or the Output failed:
// what was written is then no PNG), or into a string (empty on failure).
//
// A PNG of the rows `source` gives (8 bits, 1 to 4 channels, image.h's channel order).
bool EncodeRows(uint32_t width, uint32_t height, int channels, RowSource source, void* context, Output& out);
bool EncodeRows(uint32_t width, uint32_t height, int channels, RowSource source, void* context, std::string& out);
// A gray image of `depth` bits (1, 2 or 4) from the packed rows `source` gives (samples most
// significant first, as PDF and PNG both store them, at least the PNG row's bytes): sample s stands
// for s * 255 / (2^depth - 1), or for its complement with `invert`. Padding bits past the last
// sample are written as 0.
bool EncodePackedGray(uint32_t width, uint32_t height, int depth, bool invert, RowSource source, void* context,
                      Output& out);
// The gray depths that hold every value of `pixels` (8-bit gray) exactly, as a set: bit 0 for 1
// bit (0 and 255), bit 1 for 2 bits (multiples of 85), bit 2 for 4 bits (multiples of 17).
// `allowed` is the set of the pixels before (7 at the start); 0 once only 8 bits will do.
uint8_t GrayDepths(const uint8_t* pixels, size_t count, uint8_t allowed);
// The fewest bits a set of GrayDepths allows: 1, 2, 4 or 8.
int GrayDepthOf(uint8_t allowed);
// A gray image of `depth` bits (1, 2, 4 or 8) from the 8-bit gray rows `source` gives, whose
// values that depth holds exactly (GrayDepths).
bool EncodeGray(uint32_t width, uint32_t height, int depth, RowSource source, void* context, Output& out);
// EncodePng (image.h) into an Output: gray at the fewest exact bits.
bool EncodePixels(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, Output& out);

} // namespace RayoPdf::Image::Png
