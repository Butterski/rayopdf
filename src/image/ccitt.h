// SPDX-License-Identifier: Apache-2.0
// The CCITTFaxDecode filter (ISO 32000-1 7.4.6), written from ITU-T T.4 (Group 3, one- and
// two-dimensional) and T.6 (Group 4): every /DecodeParms entry of Table 11, the inline-image
// abbreviation /CCF included (the caller resolves names; this decoder takes plain values).
// - /K < 0: Group 4, every row two-dimensional; /K = 0: Group 3 one-dimensional (modified
//   Huffman); /K > 0: Group 3 mixed, each row starts with a tag bit (1: one-dimensional, 0: two-
//   dimensional). The first reference row is white.
// - End-of-line patterns (eleven or more 0 bits and a 1, so any fill bits before one included)
//   are read before any row, whatever /EndOfLine says. They are required for every row after
//   the first with /EndOfLine, and (as poppler reads such data) when the data starts with one:
//   then the bits between a row's end and the next pattern are skipped, unread. Otherwise a row
//   without one is read as well.
// - /EncodedByteAlign: before each row the decoder skips to a byte boundary, except after a row
//   that started after an end-of-line pattern; such a pattern positions the next row itself. That
//   reads both layouts PDF writers use with patterns (fill bits that end each pattern on a byte
//   boundary, or a pattern at a byte boundary followed directly by its row) and aligned rows
//   without patterns (Group 4 in particular).
// - Two end-of-line patterns in a row (EOFB of T.6, the start of RTC of T.4; for /K > 0 with the
//   tag bit between them) end the data; the rest of RTC is read too. With /EndOfBlock true /Rows
//   is no stop condition ("overriding the Rows parameter"); with /EndOfBlock false decoding
//   stops after /Rows rows (when not 0). Data that ends at a row boundary (nothing but 0 bits
//   left) ends the output either way. When `maxRows` stops decoding with /EndOfBlock true, an
//   EOFB or RTC right after the last row (no fill bits) is read as well, so that bytesUsed is
//   where data that follows starts (JBIG2's MMR halftone planes).
// - /DamagedRowsBeforeError (only with /EndOfLine and /K >= 0): a row whose codes are wrong is
//   skipped to the next end-of-line pattern and replaced by the previous row, or by a white row
//   when the previous one was replaced too, up to that many rows; the next one is an error.
// - The T.4/T.6 uncompressed mode (extension code 0000001 111) is not decoded: Unsupported.
// - Tolerated with poppler's output (poppler reports an error): runs and vertical-mode changes
//   past /Columns end the row there.
//
// Output, as the filter defines it: one row per decoded line, (columns + 7) / 8 bytes, pixels
// from the most significant bit; 0 is black unless /BlackIs1 (then 1 is black). The bits after
// the last column of a row are a black pixel's (0, or 1 with /BlackIs1), as poppler writes them,
// so the bytes equal poppler's.
//
// Bounds: a row's runs end at /Columns (longer runs are cut there); at most `maxPixels` pixels
// (rows x columns) are written, and at most `maxRows` rows (the image's /Height: rows after it
// are never read). Buffers grow with the decoded data, never with /Columns or /Rows alone; every
// code consumes input bits, so the work is linear in the input size plus the output size.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace RayoPdf::Image::Ccitt {

// The /DecodeParms entries, with the defaults of ISO 32000-1 Table 11.
struct Params {
    int32_t k = 0;
    bool endOfLine = false;
    bool encodedByteAlign = false;
    uint32_t columns = 1728;
    uint32_t rows = 0;  // 0: not given
    bool endOfBlock = true;
    bool blackIs1 = false;
    uint32_t damagedRowsBeforeError = 0;
};

enum class Result : uint8_t {
    Ok,             // ended at EOFB/RTC, /Rows, `maxRows` or the end of the data
    Unsupported,    // uncompressed mode
    InvalidParams,  // /Columns 0
    TooLarge,       // /Columns above 2^30 or `maxPixels`, or more than `maxPixels` pixels
    Corrupt,        // a code that is not valid where it stands, or data ending inside a row
};

struct Info {
    uint32_t rows = 0;         // rows in the output
    uint32_t damagedRows = 0;  // rows replaced under /DamagedRowsBeforeError
    bool endOfBlock = false;   // EOFB or RTC was read
    size_t bytesUsed = 0;      // input bytes up to the last bit read (EOFB included)
};

// Decodes `data` into `rows` (replaced). On TooLarge, Corrupt and Unsupported `rows` keeps the
// complete rows decoded before the problem (info.rows of them): what the filter output before
// failing. InvalidParams leaves it empty.
Result Decode(const uint8_t* data, size_t size, const Params& params, uint32_t maxRows, uint64_t maxPixels,
              std::string& rows, Info& info);

} // namespace RayoPdf::Image::Ccitt
