// SPDX-License-Identifier: Apache-2.0
// Glyph outlines of embedded font programs, for glyph-shape recovery (layout/ocr_glyphs.h): the
// glyphs of a font whose text is missing or wrong are drawn and recognised by the OCR add-on.
// Nothing on the text path reads outlines; they are loaded only for such fonts.
//
// - TrueType 'glyf' (OpenType specification 1.9): contours of quadratics with the implicit
//   on-curve midpoints; composites with offsets, scales and 2x2 transforms, nested at most
//   kMaxGlyfDepth deep (point-matched components are skipped). Forked from rayomd-gui
//   src/text/glyf.cpp at 9fbbfaf (Apache-2.0, same author).
// - CFF (Adobe TN #5176) with Type 2 charstrings (TN #5177): subroutines (local and global, with
//   their bias) called at most kMaxSubrDepth deep, hint masks, flex, and the seac form of endchar.
//   The interpreter is forked from rayomd-gui src/text/cff_outlines.cpp at 9fbbfaf.
// - Type 1 (Adobe Type 1 Font Format): the eexec section decrypted, Subrs and CharStrings
//   decrypted with lenIV, Type 1 charstrings with callothersubr flex and hint replacement, seac.
//
// Every read is bounds-checked and every loop bounded: font programs are untrusted. Outlines are
// in glyph space mapped through the caller's matrix; GlyphMatrix gives glyph space to text space
// at size 1 (the FontMatrix, or 1 / unitsPerEm).
#pragma once

#include "raster/path.h"
#include "text/program.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RayoPdf::Text::Program {

constexpr int kMaxGlyfDepth = 8;
constexpr int kMaxSubrDepth = 10;
constexpr uint32_t kMaxCharstringOps = 1u << 16;  // operators run for one glyph, subroutines and seac included

// TrueType: the 'head', 'loca' and 'glyf' tables of an sfnt (ReadSfnt).
struct GlyfOutlines {
    std::string_view loca, glyf;
    bool longLoca = false;
    uint32_t glyphCount = 0;
    uint16_t unitsPerEm = 1000;
};
// False when the font has no usable 'glyf' outlines.
bool ReadGlyfOutlines(const Sfnt& sfnt, GlyfOutlines& out);
// Glyph `glyph` in font units through `m`, appended to `out`. False when it is out of range or
// malformed (`out` may then hold part of it); an empty glyph (a space) is true with nothing added.
bool GlyfOutline(const GlyfOutlines& font, uint32_t glyph, const Raster::Matrix& m, Raster::Path& out);

// CFF: what the charstrings need, each INDEX item as a view of the program.
struct CffOutlines {
    struct FontDict {
        std::vector<std::string_view> localSubrs;
        double matrix[6] = {1, 0, 0, 1, 0, 0};
        bool hasMatrix = false;  // a CID-keyed font DICT's own FontMatrix (before the Top DICT's)
    };
    CffInfo info;                                // charset: glyph names or CIDs
    std::vector<std::string_view> charStrings;   // by glyph id
    std::vector<std::string_view> globalSubrs;
    std::vector<FontDict> fonts;                 // name-keyed: one
    std::vector<uint8_t> fdOfGlyph;              // CID-keyed: the font DICT of each glyph
    double matrix[6] = {0.001, 0, 0, 0.001, 0, 0};  // the Top DICT's FontMatrix
    bool hasMatrix = false;                          // ... given in the Top DICT
};
// Reads a bare CFF (FontFile3 /Type1C, /CIDFontType0C, or an OpenType 'CFF ' table). False when
// it is not CFF 1 with Type 2 charstrings or anything in it is out of range.
bool ReadCffOutlines(std::string_view data, CffOutlines& out);
// Glyph `glyph` in charstring units through `m`.
bool CffGlyphOutline(const CffOutlines& font, uint32_t glyph, const Raster::Matrix& m, Raster::Path& out);
// Charstring units of `glyph` to text space at size 1.
Raster::Matrix CffGlyphMatrix(const CffOutlines& font, uint32_t glyph);
// The glyph named `name` (name-keyed fonts), -1 when there is none.
int32_t CffGlyphByName(const CffOutlines& font, std::string_view name);
// The glyph of CID `cid` (CID-keyed: through the charset; name-keyed: the CID is the glyph).
int32_t CffGlyphByCid(const CffOutlines& font, uint32_t cid);

// Type 1: the decrypted charstrings.
struct Type1Outlines {
    std::vector<std::string> subrs;
    std::vector<std::pair<std::string, std::string>> charStrings;  // (name, charstring), sorted by name
    double matrix[6] = {0.001, 0, 0, 0.001, 0, 0};                 // /FontMatrix
};
// Reads a Type 1 program (FontFile; PFB segments allowed). False when no CharStrings are found.
bool ReadType1Outlines(std::string_view data, Type1Outlines& out);
// The glyph named `name` in charstring units through `m`.
bool Type1GlyphOutline(const Type1Outlines& font, std::string_view name, const Raster::Matrix& m, Raster::Path& out);

} // namespace RayoPdf::Text::Program
