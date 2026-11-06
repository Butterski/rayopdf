// SPDX-License-Identifier: Apache-2.0
// Internal to the image module: the pieces of the PNG writer (PNG specification, 3rd edition,
// sections 5, 9 and 11) that the tests check on their own. EncodePng (image.h) puts them
// together.
#pragma once

#include <cstddef>
#include <cstdint>

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

} // namespace RayoPdf::Image::Png
