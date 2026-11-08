// SPDX-License-Identifier: Apache-2.0
// The JBIG2 decoder of JBIG2Decode streams (internal to the image module), written from ITU-T T.88
// (02/2000): the embedded organisation ISO 32000-1 section 7.4.7 describes (segment headers and
// data, no file header), with the segments of /JBIG2Globals read first. Segment types: symbol
// dictionaries (arithmetic and Huffman, refinement/aggregate coding, retained bitmap coding
// contexts), text regions (arithmetic and Huffman, refinement, standard and custom Huffman tables,
// the symbol ID Huffman table), pattern dictionaries and halftone regions (MMR and arithmetic,
// skip bitmaps, any grid), generic regions (MMR through image/ccitt.h, arithmetic with templates
// 0-3, TPGDON and any AT pixels, unknown data length), generic refinement regions (templates 0
// and 1, TPGRON, of the page or of an intermediate region), page information, end of stripe
// (pages of unknown height), end of page, end of file, code tables; profiles, extensions and
// unknown types are skipped. The 12-AT-pixel template of T.88 amendment 2 (EXTTEMPLATE) fails
// with Unsupported.
//
// Output, as the filter defines it: the first page's bitmap as packed 1-bit rows, most
// significant bit first, (width + 7) / 8 bytes per row, rows top to bottom, 0 = black and
// 1 = white. JBIG2 codes 1 = black; the filter inverts every byte (as poppler and pdf.js do), so
// the padding bits of a row are 1. Region segments are combined into the page with their own
// external combination operator, as poppler does. Segment page associations are not checked;
// a second page information segment ends the page.
//
// Every size is treated as hostile: the page and every region, symbol, pattern collective
// bitmap, gray-scale plane and grid hold at most `maxPixels` pixels; retained data (symbols,
// patterns, intermediate regions, the page, retained statistics, gray-scale values) together at
// most 2^22 + maxPixels / 2 bytes; decoding work (pixels decoded or drawn, symbols, instances,
// segments, statistics reset, tables parsed) draws on a budget of 2^22 + 8 * maxPixels units, so
// data that never ends a loop (an arithmetic decoder reads 0xFF past the end) stops with TooLarge;
// at most kMaxSegments segments, kMaxReferences segment references, kMaxSymbols symbols per
// dictionary or text region, no recursion (an aggregate symbol is a text region of the
// dictionary's symbols, decoded in place). Speed: about 5 ms per megapixel (release, one core).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace RayoPdf::Image::Jbig2 {

constexpr uint32_t kMaxSegments = 1u << 14;
constexpr uint32_t kMaxReferences = 1u << 16;
constexpr uint32_t kMaxSymbols = 1u << 20;

struct Info {
    uint32_t width = 0;   // the page bitmap's width
    uint32_t height = 0;  // and height (for a page of unknown height, as its regions and stripes make it)
    // A segment after the page information failed to decode: the page holds the segments decoded
    // before it (what poppler shows of such a stream).
    bool damaged = false;
};

enum class Result : uint8_t {
    Ok,
    Unsupported,  // a valid stream with a feature this decoder does not read (EXTTEMPLATE)
    TooLarge,     // more than `maxPixels` pixels in one bitmap, or a budget exceeded
    Corrupt,      // no page information segment, or damaged before the page was created
};

// Decodes JBIG2Decode data `data` with the optional /JBIG2Globals stream `globals` (null or empty
// for none) into `rows` (replaced; empty unless the result is Ok).
Result Decode(const uint8_t* data, size_t size, const uint8_t* globals, size_t globalsSize, uint64_t maxPixels,
              Info& info, std::string& rows);

} // namespace RayoPdf::Image::Jbig2
