// SPDX-License-Identifier: Apache-2.0
// The generated glyph-name, encoding and standard 14 tables (tables_data.cpp, written by
// tools/gen_text_tables.py from the Adobe Glyph List and pdf.js; algorithms §3.3, §3.5) and the
// lookups over them. Glyph names are numbered by their place in one sorted list; every table
// refers to names by that number.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::Text::Tables {

constexpr uint16_t kNoName = 0xFFFF;
// kAglValues entries from here up index kAglMultiStarts.
constexpr uint16_t kAglMultiBase = 0xD800;

enum class Encoding : uint8_t { Standard, MacRoman, WinAnsi, MacExpert, Symbol, ZapfDingbats, CffExpert };
constexpr int kEncodingCount = 7;

struct StandardFont {
    const char* name;
    uint16_t fixedWidth;   // nonzero: every glyph has this width (Courier)
    uint16_t widthStart;   // the font's run of kStandardWidthNames / kStandardWidthValues
    uint16_t widthCount;
    int16_t ascent;        // 0 when unknown (Symbol, ZapfDingbats)
    int16_t descent;
};

struct StandardAlias {
    const char* name;
    uint8_t font;          // index into kStandardFonts
};

// The text GlyphNameToUnicode gives the name of each code of a base encoding, and whether
// UsableText accepts it: the same for every font of that encoding, so generated once. The texts
// live in the record itself (no pointer to relocate in the static binary).
constexpr size_t kEncodingPoolBytes = 608;
struct EncodingTexts {
    char pool[kEncodingPoolBytes];  // the named codes' texts, in code order
    uint32_t poolSize;
    uint32_t offset[256];           // code c's text: length[c] bytes, offset[c] bytes into the pool
    uint16_t length[256];
    bool named[256];                // the code has a name and GlyphNameToUnicode gave it text
    bool usable[256];               // and UsableText accepts that text
    std::string_view Pool() const { return std::string_view(pool, poolSize); }
    std::string_view Text(int code) const { return std::string_view(pool + offset[code], length[code]); }
};

extern const uint16_t kGlyphNameCount;
extern const char kGlyphNameBytes[];
extern const uint16_t kGlyphNameOffsets[];
// Open addressing over the names by GlyphNameHash (tables.cpp), 0xFFFF free; at most half full.
constexpr int kGlyphNameHashBits = 14;
extern const uint16_t kGlyphNameSlots[1 << kGlyphNameHashBits];
extern const uint16_t kAglValues[];
extern const uint16_t kAglMultiCount;
extern const uint16_t kAglMultiStarts[];
extern const uint16_t kAglMultiCodePoints[];
extern const uint16_t kZapfCount;
extern const uint16_t kZapfNames[];
extern const uint16_t kZapfValues[];
extern const uint16_t kEncodings[kEncodingCount][256];
extern const uint16_t kPdfDocEncoding[256];
extern const uint16_t kMacGlyphNames[258];
extern const uint16_t kCffStandardStrings[391];
extern const uint16_t kCffExpertCharsetCount;
extern const uint16_t kCffExpertCharset[];
extern const uint16_t kCffExpertSubsetCharsetCount;
extern const uint16_t kCffExpertSubsetCharset[];
extern const uint16_t kStandardWidthNames[];
extern const uint16_t kStandardWidthValues[];
extern const StandardFont kStandardFonts[14];
extern const uint16_t kStandardAliasCount;
extern const StandardAlias kStandardAliases[];
// [zapf][encoding]: with zapf, GlyphNameToUnicode's ZapfDingbats list comes first.
extern const EncodingTexts kEncodingTexts[2][kEncodingCount];

// Standard 14 indices (kStandardFonts order).
constexpr int kCourier = 0;
constexpr int kHelvetica = 4;
constexpr int kTimesRoman = 8;
constexpr int kSymbol = 12;
constexpr int kZapfDingbats = 13;

// The text of name `index`.
inline std::string_view GlyphName(uint16_t index) {
    if (index >= kGlyphNameCount) return {};
    return std::string_view(kGlyphNameBytes + kGlyphNameOffsets[index],
                            static_cast<size_t>(kGlyphNameOffsets[index + 1] - kGlyphNameOffsets[index]));
}
// The index of `name`, kNoName when the tables do not hold it. A hash lookup.
uint16_t FindGlyphName(std::string_view name);
// The AGL code points of name `index` (zero, one, or a few); `out` has room for 4.
size_t AglCodePoints(uint16_t index, uint32_t out[4]);
// The ZapfDingbats list's code point of name `index`, 0 when none.
uint32_t ZapfCodePoint(uint16_t index);
// The standard 14 width of name `index` in font `font`; false when the font has none for it.
bool StandardWidth(int font, uint16_t index, uint16_t& width);
// The standard 14 font `name` names, exactly or through pdf.js's aliases after normalising
// (spaces removed, ',' and '_' as '-'); -1 when none.
int StandardFontByName(std::string_view name);

} // namespace RayoPdf::Text::Tables

namespace RayoPdf::Text {
// GlyphNameToUnicode of font.h; with `zapfDingbats` the ZapfDingbats list (a1..a191) comes first.
bool GlyphNameToUnicode(std::string_view name, bool zapfDingbats, std::string& out);
} // namespace RayoPdf::Text
