// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/jpeg.h at 9fbbfaf (Apache-2.0, same author).
// The JPEG decoder of DCTDecode images (internal to the image module): baseline and extended
// sequential (SOF0, SOF1) and progressive (SOF2) Huffman-coded JPEG with 8-bit samples and 1, 3
// or 4 components, sampling factors 1 or 2 per axis, restart intervals. Arithmetic coding
// (SOF9-SOF15, DAC), 12-bit samples, lossless and hierarchical frames, and DNL fail with a status.
//
// The samples match libjpeg's defaults (`djpeg -dct int`, what poppler and libjpeg-based viewers
// show): the islow integer IDCT, its "fancy" (triangle) chroma upsampling, its YCbCr constants.
// Dequantized coefficients are saturated to int16 before the IDCT.
//
// Output colour models, as libjpeg chooses them from the Adobe APP14 marker's transform byte:
// - 1 component: gray.
// - 3 components: RGB, converted from YCbCr unless APP14 says transform 0 (stored as RGB).
// - 4 components: CMYK as stored; YCCK (APP14 transform 2) converted as libjpeg does,
//   C = 255 - R'(Y, Cr), M = 255 - G'(Y, Cb, Cr), Y = 255 - B'(Y, Cb), K as stored.
// The four values are what a PDF reads as its colour space's components: PDF (ISO 32000-1
// 8.6.4.4, pdf.js, poppler) takes them as ink amounts and inverts them only through /Decode,
// unlike stand-alone viewers, which take an APP14 marker to mean Adobe's inverted CMYK. The
// caller applies /Decode and the colour space.
//
// Every read is bounds-checked; buffers are sized from validated headers (no dimension 0 or above
// 65535, at most `maxPixels` pixels, no more blocks than the data could code). Work is bounded
// apart from the data size, which progressive scans do not bound (an end-of-band run codes 32767
// blocks in a few bits): at most 1000 scans, which together visit each coefficient of the frame
// at most 32 times.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace RayoPdf::Image::Jpeg {

struct Info {
    uint32_t width = 0;
    uint32_t height = 0;
    int components = 0;       // 1, 3 or 4
    bool progressive = false;
    int adobeTransform = -1;  // the APP14 transform byte, -1 without an APP14 marker
};

enum class Result : uint8_t {
    Ok,
    Unsupported,  // a valid JPEG this decoder does not read (arithmetic, 12-bit, lossless, ...)
    TooLarge,     // more than `maxPixels` pixels
    Corrupt,      // not a JPEG, or damaged before the image is complete
};

// Reads the markers up to the frame header: the checks the decoder makes before it decodes.
Result ReadInfo(const uint8_t* data, size_t size, Info& info);

// Decodes to interleaved 8-bit samples, info.components per pixel, rows top to bottom, no row
// padding, into `samples` (replaced). `samples` is left empty on failure.
Result Decode(const uint8_t* data, size_t size, uint64_t maxPixels, Info& info, std::string& samples);

} // namespace RayoPdf::Image::Jpeg
