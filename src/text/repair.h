// SPDX-License-Identifier: Apache-2.0
// Repairing and judging font text (algorithms §3.6): text a font's ToUnicode, encoding or program
// gives is rewritten where its meaning is known (whitespace control characters, private-use forms
// of symbol, Thai and Adobe Symbol pieces), and a ToUnicode map is judged untrustworthy from its
// own text. font.cpp applies both while loading a font.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::Text::Repair {

// What a font tells about reading private-use text.
struct Context {
    bool symbol = false;     // Symbol, SymbolMT: U+F020-U+F0FF through the Symbol encoding
    bool wingdings = false;  // a Wingdings font: U+F020-U+F0FF bullets and boxes through its table
    bool thai = false;       // the font has Thai text: U+F700-U+F71A are Thai presentation forms
};

// The context the base font name gives (`thai` is set by the caller from the font's text).
Context ContextForName(std::string_view baseName);

// A character census.py counts as junk: U+FFFD, private use, C0/C1 controls except tab, line feed
// and form feed.
bool IsJunk(uint32_t cp);

// `text` (UTF-8) with known forms rewritten, into `out` (replaced): U+0009-U+000D as U+0020 (a
// ToUnicode that maps a space glyph to a tab), Adobe's corporate private-use Symbol pieces
// (parenlefttp U+F8EB, ...) as their Unicode equivalents (U+239B, ...), and with `context` the
// private-use symbol, Wingdings and Thai forms. False (and `out` untouched) when nothing changes.
bool RepairText(std::string_view text, const Context& context, std::string& out);

// The text of private-use code point `cp` under `context`, 0 when it has none (exposed for the
// fonts whose codes are F0xx symbol positions without any text).
uint32_t SymbolCodePoint(uint32_t cp, const Context& context);

// Statistics of the texts one font maps its codes to, for judging a ToUnicode map.
struct MapStats {
    uint32_t entries = 0;       // mapped codes (including those dropped as unusable)
    uint32_t junk = 0;          // entries whose text is or holds junk after repair
    uint32_t latinLetters = 0;  // over all entries' text
    uint32_t accented = 0;      // Latin letters from U+00C0 up
    uint32_t largestShare = 0;  // entries sharing the most common non-space text
    bool thai = false;          // some entry is Thai
};

// Adds one entry's text to `stats` (texts sharing are counted by the caller into largestShare).
void Count(std::string_view text, MapStats& stats);

// Whether a ToUnicode map with these statistics is untrustworthy: most of its entries junk, a
// majority of codes onto one character, or Latin text that is 70 % accented letters (legacy font
// re-encodings such as Chanakya or Kruti Dev; maps of at most 256 codes). Maps of fewer than 8
// codes are never judged.
bool Untrusted(const MapStats& stats);

} // namespace RayoPdf::Text::Repair
