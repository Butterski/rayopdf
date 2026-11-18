// SPDX-License-Identifier: Apache-2.0
// Glyphs to words and lines (algorithms §4, roadmap D4 step 4): hidden and clipped glyphs dropped,
// overprinted duplicates folded into one bold glyph (PDFBox / MuPDF style, §4.2), spacing accents
// drawn apart over or under a letter of their font (TeX's OT1 \accent, some word processors)
// composed with it into one glyph (§4.4: NFC, else the letter and its combining mark; math's
// accents stay apart), fragments built in content order and merged per baseline and direction,
// superscripts and subscripts kept in their line, spacing accents that content order set apart
// from the glyph they sit over (drawn after their line, or before the line their glyph starts:
// TeX's \hat at a line start) seated just before it, or composed with it as beside it, and word
// breaks from space glyphs and from font-size-relative gaps (MuPDF-style thresholds, §4.1).
#pragma once

#include "layout/page.h"

#include <string>
#include <string_view>

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
// unordered; OrderLines orders them. `mathScripts` (--math=latex) also joins a script's own
// script to its line: a lone fragment of math-font glyphs at 0.87 of the size of a script glyph
// of the line or smaller, its baseline from 0.6 em below to 0.9 em above the line's; and composes
// a spacing accent with a bold letter only when another letter of its font is beside it (a word: a
// formula's \tilde{\mathbf{u}} keeps its accent for the formula's rebuild).
void BuildLines(PageData& page, bool mathScripts = false);

// Run-in headings (LaTeX \paragraph, the fourth and fifth levels of many templates): in an ordered
// line, a bold run that starts it, ends without punctuation and is set off from the text after it
// by a wide space (a quad, not a word space) becomes a line of its own on the same baseline, with
// the rest of the line right after it in page.order. Semantics reads it as a heading. A bold
// lead-in that ends a sentence ("Results. We find ...") or introduces it ("Note: ...") stays.
// An equation's number that ends its formula's line ("(12)" set a quad or more after the formula)
// becomes a line of its own the same way, where semantics reads it as a paragraph of its own.
// Runs after OrderLines, so that the order of the page's lines is decided on whole lines.
void SplitRunInHeadings(PageData& page);

// The text of a line with word breaks as single spaces, from its glyph `skip` on.
void LineText(const PageData& page, const Line& line, std::string& out, uint32_t skip = 0);
// LineText appended to `out`.
void AppendLineText(const PageData& page, const Line& line, std::string& out, uint32_t skip = 0);

// A caption: a label word and its number ("Figure 6.1.1: …", "Table 13.2. …", "Fig. A1",
// "TABLE IV"), in any letter case.
bool IsCaption(std::string_view t);

// Calls use(text) with the texts of `line`'s glyphs [k0, k1) in order, a run of glyphs whose texts
// follow each other in the page text as one piece: the same bytes as glyph by glyph.
template <class Use>
void ForGlyphTextRuns(const PageData& page, const Line& line, uint32_t k0, uint32_t k1, Use use) {
    const size_t textSize = page.text.size();
    uint32_t k = k0;
    while (k < k1) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        const std::string_view first = page.GlyphText(g);
        size_t end = size_t(g.text) + first.size();
        for (++k; k < k1; ++k) {
            const Glyph& n = page.glyphs[page.lineGlyphs[line.first + k]];
            if (n.text != end || textSize - end < n.textLength) break;
            end += n.textLength;
        }
        use(std::string_view(first.data(), end - g.text));
    }
}

// Appends the texts of `line`'s glyphs [k0, k1), with no spaces between (ForGlyphTextRuns).
void AppendGlyphTexts(const PageData& page, const Line& line, uint32_t k0, uint32_t k1, std::string& out);

// A superscript of `line`: a visible glyph smaller than the line's size and raised above its
// baseline (the writer's <sup>, and footnote references). OCR layers fit a size to every word
// box, so their sizes say nothing about scripts.
inline bool IsSuperscript(const Glyph& g, const Line& line) {
    return g.size < 0.85f * line.size && !(g.flags & kGlyphInvisible) && g.v - line.baseline > 0.2f * line.size;
}
inline bool IsSubscript(const Glyph& g, const Line& line) {
    return g.size < 0.85f * line.size && !(g.flags & kGlyphInvisible) && g.v - line.baseline < -0.1f * line.size;
}

// A list marker (algorithms §6.3): a bullet glyph, an enumerator (1. 1) (1) a) (a) i. iv)), or a
// task box (☐ ☑ ☒).
enum class Marker : uint8_t { None, Bullet, Number, Letter, Roman, TaskOpen, TaskDone };

// The marker `token` (one word, without spaces) is; `value` is an enumerator's ordinal.
Marker ParseMarker(std::string_view token, uint32_t& value);

} // namespace RayoPdf::Layout
