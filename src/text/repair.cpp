// SPDX-License-Identifier: Apache-2.0
// Tables of private-use forms whose meaning is known:
//   - Adobe's corporate-use area of the Adobe Glyph List (the Symbol font's bracket, brace and
//     integral pieces, its sans-serif and serif registered/copyright/trademark signs, the arrow
//     extenders): the Unicode Miscellaneous Technical pieces U+239B-U+23AE and the plain signs.
//   - Symbol fonts mapped into U+F020-U+F0FF (Microsoft's symbol cmap convention, which Word's
//     ToUnicode maps repeat): the byte read through the Symbol encoding and the Adobe Glyph List.
//   - Wingdings in U+F020-U+F0FF: the bullets, boxes and check marks Word lists use.
//   - Thai presentation forms U+F700-U+F71A (the shifted marks and descender-less consonants of
//     Thai fonts): the Thai characters they draw.

#include "text/repair.h"

#include "common/utf8.h"
#include "text/tables.h"

namespace RayoPdf::Text::Repair {
namespace {

struct Pair {
    uint16_t from;
    uint16_t to;
};

// Adobe corporate-use code points of the AGL (sorted) and their Unicode equivalents.
constexpr Pair kAdobePua[] = {
    {0xF6D9, 0x00AE}, {0xF6DA, 0x00A9}, {0xF6DB, 0x2122},  // register/copyright/trademark serif
    {0xF8E6, 0x23D0}, {0xF8E7, 0x23AF},                    // arrowvertex, arrowhorizex
    {0xF8E8, 0x00AE}, {0xF8E9, 0x00A9}, {0xF8EA, 0x2122},  // ... sans
    {0xF8EB, 0x239B}, {0xF8EC, 0x239C}, {0xF8ED, 0x239D},  // parenleft tp ex bt
    {0xF8EE, 0x23A1}, {0xF8EF, 0x23A2}, {0xF8F0, 0x23A3},  // bracketleft tp ex bt
    {0xF8F1, 0x23A7}, {0xF8F2, 0x23A8}, {0xF8F3, 0x23A9},  // braceleft tp mid bt
    {0xF8F4, 0x23AA}, {0xF8F5, 0x23AE},                    // braceex, integralex
    {0xF8F6, 0x239E}, {0xF8F7, 0x239F}, {0xF8F8, 0x23A0},  // parenright tp ex bt
    {0xF8F9, 0x23A4}, {0xF8FA, 0x23A5}, {0xF8FB, 0x23A6},  // bracketright tp ex bt
    {0xF8FC, 0x23AB}, {0xF8FD, 0x23AC}, {0xF8FE, 0x23AD},  // braceright tp mid bt
};

// Wingdings glyphs of list bullets and boxes (sorted by code).
constexpr Pair kWingdings[] = {
    {0x6C, 0x25CF}, {0x6E, 0x25A0}, {0x6F, 0x25A1}, {0x71, 0x2751}, {0x76, 0x2756}, {0xA7, 0x25AA},
    {0xD8, 0x27A2}, {0xFB, 0x2717}, {0xFC, 0x2713}, {0xFD, 0x2612}, {0xFE, 0x2611},
};

// Thai presentation forms U+F700 + i.
constexpr uint16_t kThai[] = {
    0x0E10, 0x0E34, 0x0E35, 0x0E36, 0x0E37, 0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C,  // F700-F709
    0x0E48, 0x0E49, 0x0E4A, 0x0E4B, 0x0E4C, 0x0E0D, 0x0E31, 0x0E4D, 0x0E47, 0x0E48,  // F70A-F713
    0x0E49, 0x0E4A, 0x0E4B, 0x0E4C, 0x0E38, 0x0E39, 0x0E3A,                          // F714-F71A
};

template <size_t N>
uint32_t Lookup(const Pair (&table)[N], uint32_t cp) {
    for (const Pair& p : table) {
        if (p.from == cp) return p.to;
    }
    return 0;
}

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size()) {
            char a = haystack[i + j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != needle[j]) break;
            ++j;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

} // namespace

Context ContextForName(std::string_view baseName) {
    Context c;
    c.wingdings = ContainsNoCase(baseName, "wingdings");
    // Adobe's and Microsoft's Symbol ("Symbol", "SymbolMT", "Symbol,Bold"); not other fonts named
    // after it (OpenSymbol puts Wingdings-like bullets at the same positions).
    c.symbol = !c.wingdings && baseName.substr(0, 6) == "Symbol";
    return c;
}

bool IsJunk(uint32_t cp) {
    if (cp == 0xFFFD) return true;
    if (cp < 0x20) return cp != '\t' && cp != '\n' && cp != '\f';
    if (cp >= 0x80 && cp <= 0x9F) return true;
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD);
}

uint32_t SymbolCodePoint(uint32_t cp, const Context& context) {
    if (cp >= 0xF020 && cp <= 0xF0FF) {
        const uint32_t code = cp - 0xF000;
        if (context.wingdings) return Lookup(kWingdings, code);
        if (context.symbol) {
            const uint16_t name = Tables::kEncodings[static_cast<int>(Tables::Encoding::Symbol)][code];
            uint32_t points[4];
            if (name != Tables::kNoName && Tables::AglCodePoints(name, points) == 1) {
                const uint32_t piece = Lookup(kAdobePua, points[0]);
                return piece ? piece : points[0];
            }
        }
        return 0;
    }
    if (cp >= 0xF700 && cp <= 0xF71A && context.thai) return kThai[cp - 0xF700];
    return Lookup(kAdobePua, cp);
}

bool RepairText(std::string_view text, const Context& context, std::string& out) {
    // A first pass finds whether anything changes (the common case: nothing does).
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    bool change = false;
    while (!change && Utf8::Decode(text, at, cp, length)) {
        change = (cp >= 0x09 && cp <= 0x0D) || (cp >= 0xE000 && cp <= 0xF8FF && SymbolCodePoint(cp, context) != 0);
        at += length;
    }
    if (!change) return false;
    out.clear();
    at = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        if (cp >= 0x09 && cp <= 0x0D) {
            out += ' ';
        } else if (const uint32_t mapped = cp >= 0xE000 && cp <= 0xF8FF ? SymbolCodePoint(cp, context) : 0) {
            Utf8::Append(out, mapped);
        } else {
            out.append(text.data() + at, length);
        }
        at += length;
    }
    // Bytes past a malformed sequence stay as they are.
    if (at < text.size()) out.append(text.data() + at, text.size() - at);
    // Whitespace controls alone (a tab and a line feed for one glyph) are one space.
    if (out.find_first_not_of(' ') == std::string::npos) out = " ";
    return true;
}

void Count(std::string_view text, MapStats& stats) {
    ++stats.entries;
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    bool junk = text.empty();
    while (Utf8::Decode(text, at, cp, length)) {
        if (IsJunk(cp)) junk = true;
        if ((cp < 0x80 && ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z')) || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 &&
                                                                             cp != 0xF7)) {
            ++stats.latinLetters;
            if (cp >= 0xC0) ++stats.accented;
        }
        if (cp >= 0x0E01 && cp <= 0x0E5B) stats.thai = true;
        at += length;
    }
    if (junk) ++stats.junk;
}

bool Untrusted(const MapStats& stats) {
    if (stats.entries < 8) return false;
    if (stats.junk * 2 > stats.entries) return true;
    if (stats.entries >= 16 && stats.largestShare * 2 > stats.entries) return true;
    // Mostly accented letters: 70 % (Latin-1 itself has 57 %, so a map of a whole simple font's code
    // range stays trusted; Latin Extended has more, so only maps of a simple font's size are judged).
    return stats.entries <= 256 && stats.latinLetters >= 30 && stats.accented * 10 >= stats.latinLetters * 7;
}

} // namespace RayoPdf::Text::Repair
