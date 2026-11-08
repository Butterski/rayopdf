// SPDX-License-Identifier: Apache-2.0
// What the text layer reads of embedded font programs (algorithms §3.3, §3.4): glyph names and
// built-in encodings of Type 1 (FontFile) and CFF (FontFile3 /Type1C, /CIDFontType0C) fonts, and
// the 'cmap', 'post' and 'maxp' tables of TrueType / OpenType (FontFile2, FontFile3 /OpenType).
// Outlines are read only for glyph-shape recovery (text/outline.h). Every read is bounds-checked:
// font programs are untrusted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Text::Program {

// Glyph names by glyph id (or by code), held in one buffer.
struct Names {
    std::string bytes;
    std::vector<uint32_t> ends;  // name i spans [ends[i - 1], ends[i]) (from 0 for i = 0)

    size_t Count() const { return ends.size(); }
    std::string_view Name(size_t i) const {
        if (i >= ends.size()) return {};
        const uint32_t start = i == 0 ? 0 : ends[i - 1];
        return std::string_view(bytes).substr(start, ends[i] - start);
    }
    void Push(std::string_view name) {
        bytes.append(name.data(), name.size());
        ends.push_back(static_cast<uint32_t>(bytes.size()));
    }
};

// The built-in encoding of a Type 1 font program: the `dup <code> /<name> put` entries of its
// clear-text /Encoding array, or StandardEncoding. `data` may be PFB-wrapped.
struct Type1Encoding {
    bool standard = false;  // "/Encoding StandardEncoding def"
    Names codes;            // 256 names, empty for undefined codes
};
bool ReadType1Encoding(std::string_view data, Type1Encoding& out);

// A CFF font (CFF 1): glyph names (name-keyed) or CIDs (CID-keyed) from its charset, and the
// built-in encoding of a name-keyed font.
struct CffInfo {
    bool cidKeyed = false;
    uint32_t glyphCount = 0;
    Names glyphNames;                  // name-keyed: by glyph id
    std::vector<uint16_t> glyphCids;   // CID-keyed: by glyph id
    // Name-keyed built-in encoding: 0 Standard, 1 Expert (predefined, see Tables), 2 custom.
    int encoding = 0;
    int32_t codeGlyphs[256] = {};      // custom: the glyph of each code, -1 for none
};
bool ReadCff(std::string_view data, CffInfo& out);

// A TrueType 'cmap' subtable: code -> glyph id.
struct CmapSubtable {
    uint16_t platform = 0;
    uint16_t encoding = 0;
    uint16_t format = 0;
    std::string_view data;  // the subtable, bounds-checked against its own length

    // The glyph of `code`, 0 for none.
    uint32_t Glyph(uint32_t code) const;
    // Calls visit(code, glyph) for each mapped code, at most `limit` codes in all.
    template <class Visit>
    void ForEach(Visit& visit, uint32_t limit) const;
};

struct Sfnt {
    uint32_t glyphCount = 0;   // 'maxp' numGlyphs, 0 when absent
    std::vector<CmapSubtable> cmaps;
    std::string_view post;
    std::string_view cff;      // an OpenType font's 'CFF ' table
    std::string_view head, loca, glyf;  // TrueType outlines (text/outline.h)

    // The (platform, encoding) subtable, nullptr when absent.
    const CmapSubtable* Cmap(uint16_t platform, uint16_t encoding) const;
};
// Reads the table directory of an sfnt (TrueType 0x00010000 / 'true', OpenType 'OTTO', the first
// font of a 'ttcf' collection). False when it is none.
bool ReadSfnt(std::string_view data, Sfnt& out);
// The 'post' glyph names (formats 1 and 2), empty when it has none.
bool ReadPostNames(std::string_view post, uint32_t glyphCount, Names& out);
// Unicode of each glyph id below `glyphCount` from the font's Unicode subtables ((3,10), (3,1),
// (0,*)): the lowest code point that maps to it, 0 for none.
void InvertUnicodeCmap(const Sfnt& sfnt, uint32_t glyphCount, std::vector<uint32_t>& unicodeOfGlyph);

// Implementation of CmapSubtable::ForEach.
void ForEachMapping(const CmapSubtable& table, uint32_t limit, void (*visit)(void*, uint32_t, uint32_t), void* context);
template <class Visit>
void CmapSubtable::ForEach(Visit& visit, uint32_t limit) const {
    ForEachMapping(*this, limit, [](void* c, uint32_t code, uint32_t glyph) { (*static_cast<Visit*>(c))(code, glyph); },
                   &visit);
}

} // namespace RayoPdf::Text::Program
