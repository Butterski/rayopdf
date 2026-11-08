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
// the bytes per pixel (1 to 4). The scalar functions are the reference that the SSE2 ones match
// byte for byte (the tests compare them).
//
// The filter, 0 to 4, that the PNG encoder heuristic picks for `row`: the smallest sum of the
// residuals' magnitudes read as signed bytes, the lowest filter on ties.
int ChooseFilter(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp);
int ChooseFilterScalar(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp);
// Writes the residuals of `row` under `filter` (0 to 4) to `out`, rowBytes bytes.
void FilterRow(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out);
void FilterRowScalar(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out);

// Row y (rowBytes bytes) of an image EncodeRows writes, asked for once each, top to bottom; the
// pointer stays valid until the call after next (the filters read the row above).
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
// A gray image of `depth` bits (1, 2 or 4) from its packed rows (`rowBytes` apart, samples most
// significant first, as PDF and PNG both store them): sample s stands for s * 255 / (2^depth - 1),
// or for its complement with `invert`. Padding bits past the last sample are written as 0.
bool EncodeGray(const uint8_t* rows, size_t rowBytes, uint32_t width, uint32_t height, int depth, bool invert,
                Output& out);
bool EncodeGray(const uint8_t* rows, size_t rowBytes, uint32_t width, uint32_t height, int depth, bool invert,
                std::string& out);
// EncodePng (image.h) into an Output.
bool EncodePixels(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, Output& out);

} // namespace RayoPdf::Image::Png
