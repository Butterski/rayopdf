// SPDX-License-Identifier: Apache-2.0
// Stream filters (ISO 32000-1 section 7.4): the general-purpose ones decoded here, image-only ones
// recognised and left to the image writer. Every decoder is bounded by an output limit, never
// reads past its input, and salvages: data decoded before a corrupt or truncated point is kept.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf {

enum class StreamStatus : uint8_t {
    Ok,
    Salvaged,     // decoding stopped at corrupt or truncated data; the output before it is kept
    Unsupported,  // a filter not decoded here (an image filter, Brotli, an unknown name); the
                  // output holds the data as it stands before that filter
    TooLarge,     // the output would pass the limit; the output is cut at the limit
    Corrupt,      // nothing usable
    NotStream,    // the value is no stream
};

namespace Filters {

enum class Kind : uint8_t {
    Flate,      // FlateDecode, Fl
    Lzw,        // LZWDecode, LZW
    Ascii85,    // ASCII85Decode, A85
    AsciiHex,   // ASCIIHexDecode, AHx
    RunLength,  // RunLengthDecode, RL
    Crypt,      // Crypt (handled by the document's security handler)
    Dct,        // DCTDecode, DCT (image)
    Jpx,        // JPXDecode (image)
    Jbig2,      // JBIG2Decode (image)
    Ccitt,      // CCITTFaxDecode, CCF (image)
    Brotli,     // BrotliDecode (PDF 2.0 errata; not decoded yet)
    Unknown,
};

// The filter a /Filter name (full or abbreviated) names.
Kind KindOf(std::string_view name);
// True for the image-only filters (Dct, Jpx, Jbig2, Ccitt).
bool IsImageFilter(Kind kind);

// /DecodeParms of Flate and LZW: the predictor (1 none, 2 TIFF, 10-15 PNG) and its image shape.
struct Params {
    int predictor = 1;
    int colors = 1;            // 1 to 32
    int bitsPerComponent = 8;  // 1, 2, 4, 8 or 16
    int columns = 1;
    int earlyChange = 1;       // LZW only
};

// Decodes `in` with one filter into `out` (replaced), at most `limit` bytes. Image filters,
// Crypt, Brotli and Unknown return Unsupported with `out` = `in`.
StreamStatus Decode(Kind kind, const Params& params, std::string_view in, std::string& out, size_t limit);

} // namespace Filters
} // namespace RayoPdf
