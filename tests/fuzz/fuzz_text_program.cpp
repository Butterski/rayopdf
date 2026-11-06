// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the font program readers (src/text/program.h): the input as an sfnt (table
// directory, every cmap subtable looked up and inverted, 'post' names), as a CFF font (charset and
// encoding) and as a Type 1 font's clear text.
#include "text/program.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace RayoPdf::Text::Program;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    Sfnt sfnt;
    if (ReadSfnt(input, sfnt)) {
        const uint32_t glyphs = sfnt.glyphCount < 4096 ? sfnt.glyphCount : 4096;
        for (const CmapSubtable& table : sfnt.cmaps) {
            for (uint32_t code : {0u, 0x20u, 0x41u, 0xF041u, 0xFFFFu, 0x1F600u}) table.Glyph(code);
        }
        std::vector<uint32_t> unicode;
        InvertUnicodeCmap(sfnt, glyphs, unicode);
        Names post;
        ReadPostNames(sfnt.post, glyphs, post);
        CffInfo cff;
        ReadCff(sfnt.cff, cff);
    }
    CffInfo cff;
    if (ReadCff(input, cff) && !cff.cidKeyed) {
        for (int code = 0; code < 256; ++code) {
            if (cff.codeGlyphs[code] >= static_cast<int32_t>(cff.glyphCount)) __builtin_trap();
        }
    }
    Type1Encoding type1;
    ReadType1Encoding(input, type1);
    return 0;
}
