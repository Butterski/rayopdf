// SPDX-License-Identifier: Apache-2.0
// Glyphs to words and lines (algorithms §4, roadmap D4 step 4): hidden and clipped glyphs dropped,
// overprinted duplicates folded into one bold glyph (PDFBox / MuPDF style, §4.2), fragments built
// in content order and merged per baseline and direction, superscripts and subscripts kept in
// their line, and word breaks from space glyphs and from font-size-relative gaps (MuPDF-style
// thresholds, §4.1).
#pragma once

#include "layout/page.h"

#include <string>

namespace RayoPdf::Layout {

// Gaps along the baseline in units of the font size (algorithms §4.1).
constexpr float kWordGap = 0.13f;          // a word break when no space glyphs say otherwise
constexpr float kWordGapWithSpaces = 0.3f; // a word break on lines that carry their own spaces
constexpr float kFragmentGap = 3.0f;       // a larger gap ends a line fragment in content order
constexpr float kJoinGap = 0.8f;           // fragments not adjacent in content order join across less
                                           // (LaTeX's gutter is 1 em: 10 pt at 10 pt)
constexpr float kGutterCandidate = 0.6f;   // a wider gap may be a column gutter or cell boundary: it
                                           // splits the line when the lines around it are blank there
constexpr float kMarkerGap = 4.0f;         // a list marker joins the text this far to its right

// Fills page.lines and page.lineGlyphs from page.glyphs, and page.hiddenGlyphs. Lines come
// unordered; OrderLines orders them.
void BuildLines(PageData& page);

// The text of a line with word breaks as single spaces, from its glyph `skip` on.
void LineText(const PageData& page, const Line& line, std::string& out, uint32_t skip = 0);

// A list marker (algorithms §6.3): a bullet glyph, an enumerator (1. 1) (1) a) (a) i. iv)), or a
// task box (☐ ☑ ☒).
enum class Marker : uint8_t { None, Bullet, Number, Letter, Roman, TaskOpen, TaskDone };

// The marker `token` (one word, without spaces) is; `value` is an enumerator's ordinal.
Marker ParseMarker(std::string_view token, uint32_t& value);

} // namespace RayoPdf::Layout
