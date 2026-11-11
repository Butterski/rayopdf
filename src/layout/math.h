// SPDX-License-Identifier: Apache-2.0
// LaTeX math output (--math=latex; roadmap D6 keeps Unicode math as the default): formulas found
// in the text layer written as $…$ (inline) and $$…$$ (display) LaTeX rebuilt from the glyphs'
// geometry. The default output (MathMode::Unicode) never reaches this code.
//
// Finding formulas. Display formulas: MarkDisplayFormulas groups the lines of a page that hold a
// set-off formula (lines with math glyphs and next to no text words, near each other: a
// fraction's numerator and denominator, an operator's limits and a right-aligned equation number
// are lines of their own) into one line flagged kLineFormula, which semantics keeps as a block of
// its own. Inline formulas: FindInlineFormulas marks runs of a paragraph's glyphs that are math:
// glyphs of math fonts (CMMI, CMSY, CMEX, MSBM, LM Math, STIX Math, Cambria Math: MathFontOf),
// mathematical alphanumerics, operators and arrows, italic single letters beside operators, and
// raised or lowered glyphs after them, with the brackets, digits and function names between them.
//
// Rebuilding LaTeX (BuildLatex). Glyphs of TeX's math fonts whose names the font left without
// Unicode (integraldisplay) come back to their code and from it to their symbol (KaTeX's tables
// of the fonts' encodings, math_tables.h). Fraction bars (filled rules) split numerators from
// denominators, a radical sign with the rule over its radicand makes \sqrt (with an index set
// small above its left), large operators take the rows centred above and below them as limits,
// tall delimiters (or delimiter pieces U+239B-23AD) around rows make matrices and cases, and on
// each baseline smaller glyphs raised or lowered after a base are its scripts (recursively).
// Accents over a base become \hat, \bar, \tilde, \dot, \vec; upright letter runs become function
// names (\sin) or \text; Unicode becomes KaTeX's commands, and mathematical alphanumerics their
// alphabet (𝐀 → \mathbf{A}, ℝ → \mathbb{R}, 𝒮 → \mathcal{S}). Anything the rebuild cannot place
// or read (an unmapped glyph, a structure past the limits) leaves the formula as Unicode text.
#pragma once

#include "layout/page.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Layout {

enum class MathMode : uint8_t {
    Unicode,  // D6: math as Unicode text with <sup>/<sub>
    Latex,    // formulas as LaTeX between $ and $$
};

// The math role of a font, from its name (PageData::fontMath).
enum class MathFont : uint8_t {
    None,          // a text font (its glyphs may still be math: CMR digits, Times italic letters)
    Italic,        // math italic letters (CMMI, LM Math Italic, txmi)
    Symbol,        // math symbols (CMSY, LM Math Symbols, MnSymbol, txsy); its capitals are calligraphic
    Extension,     // large operators and delimiter pieces (CMEX, LM Math Extension, txex, esint)
    Blackboard,    // blackboard bold capitals (MSBM), with AMS symbols
    AmsSymbol,     // AMS symbols (MSAM)
    Fraktur,       // EUFM, EUFB
    Script,        // RSFS
    Calligraphic,  // Euler script (EUSM, EUSB)
    Unicode,       // an OpenType math font (Latin Modern Math, STIX Math, Cambria Math)
};

// The class of a font by its /BaseFont name without a subset tag.
MathFont MathFontOf(std::string_view baseName);

// PageData::fontMath bit: a math italic font whose glyphs got their text from glyph names (no
// ToUnicode), where the Adobe Glyph List reads TeX's "phi" as φ and "phi1" as ϕ, the other way
// round from TeX's \phi and \varphi (and "epsilon" as ε for TeX's \epsilon ϵ).
constexpr uint8_t kMathFontNamed = 0x80;
constexpr uint8_t kMathFontClass = 0x7F;

// Limits of one formula: more glyphs or rules, or deeper nesting, leave it as Unicode text.
constexpr uint32_t kMaxFormulaGlyphs = 1024;
constexpr uint32_t kMaxFormulaRules = 64;
constexpr int kMaxFormulaDepth = 12;
// Lines of a page grouped into one display formula at most.
constexpr uint32_t kMaxFormulaLines = 64;

// Glyph flags of a formula glyph.
enum : uint8_t {
    kMathItalic = 1u << 0,
    kMathBold = 1u << 1,
    kMathGuessed = 1u << 2,    // its text is the code read as WinAnsi (the font named it nothing known)
    kMathUnmapped = 1u << 3,   // no text at all
    kMathLineStart = 1u << 4,  // first glyph of a line of an inline formula broken over lines
    kMathNamed = 1u << 5,      // its font's text came from glyph names (kMathFontNamed)
};

// One glyph of a formula, in its baseline frame (u along the baseline, v upwards).
struct MathGlyph {
    float u0 = 0, u1 = 0;  // its advance along the baseline
    float v = 0;           // its baseline
    float size = 0;        // its font size
    // The ink of a large glyph (an operator, a delimiter, a radical sign of TeX's extension
    // fonts, which hang from their origin) as PrepareMathGlyphs placed it; both 0 when unknown.
    float bottom = 0, top = 0;
    uint32_t text = 0;     // its text: MathInput::text[text, text + length)
    uint16_t length = 0;
    uint8_t font = 0;      // MathFont
    uint8_t flags = 0;
};

// A filled or stroked bar of the formula's area: fraction bars, radical and over-bars.
struct MathRule {
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

struct MathInput {
    std::vector<MathGlyph> glyphs;
    std::vector<MathRule> rules;
    std::string text;
    void Clear() {
        glyphs.clear();
        rules.clear();
        text.clear();
    }
};

enum class MathStatus : uint8_t {
    Ok,
    Empty,       // no glyphs
    TooLarge,    // past kMaxFormulaGlyphs, kMaxFormulaRules or kMaxFormulaDepth
    Unreadable,  // a glyph without trustworthy text
    Unsure,      // a structure the rebuild cannot place
};

// The LaTeX of the formula in `in`, appended to `out` (unchanged unless Ok). `display` looks for
// an equation number set apart on the right ("(3)", "(2.1a)"): it goes to `tag` (without the
// brackets) and not into the formula; rows set one under the other (aligned equations) are
// separated by "\\\\". Inline formulas broken over lines (kMathLineStart) are rebuilt line by line.
MathStatus BuildLatex(const MathInput& in, bool display, std::string& out, std::string& tag);

// Adds `page`'s glyph `g` to `in` (its text, font class and flags).
void AddMathGlyph(const PageData& page, const Glyph& g, MathInput& in);

// Adds the bars of `page` inside the box u0..u1, vb..vt (direction 0) to `in`: horizontal
// rulings, and images one pixel high or flat (Ghostscript writes TeX's rules as 1×1 image masks).
void AddMathRules(const PageData& page, float u0, float u1, float vb, float vt, MathInput& in);

// Before BuildLines, LaTeX mode only: the large glyphs of TeX's extension fonts (∑, ∫, big
// delimiters and their pieces) and its radical signs hang from their origin, a line or more above
// the baseline they are set on; each such glyph of `page` gets its ink as its box (vb, vt) and a
// baseline a quarter em under the ink's middle (TeX centres them on the math axis), so that it
// joins the line it stands in.
void PrepareMathGlyphs(PageData& page);

// Display formulas of `page` (after OrderLines): each one's lines become one line flagged
// kLineFormula, in reading order where its first line was, the others leaving the order.
// Returns the formulas marked.
int MarkDisplayFormulas(PageData& page);

// Inline formulas of a run of glyph texts: per glyph of `glyphs` (as AddMathGlyph would read
// them, all of one block, with their script state), whether it is math. Used by the writer.
struct InlineGlyph {
    const Glyph* glyph = nullptr;
    uint8_t font = 0;      // MathFont
    bool wordStart = false;
    bool lineStart = false;
    bool script = false;   // raised or lowered in its line
    bool note = false;     // a footnote reference: never math
    std::string_view text;
};
// Marks math runs: `run[i]` = the end (exclusive) of the formula that starts at glyph i, or 0.
void FindInlineFormulas(const std::vector<InlineGlyph>& glyphs, std::vector<uint32_t>& run);
// Unmarks the runs that are only a number: digits (also mathematical alphanumeric ones), signs,
// ±, ∓, decimal points and commas, spaces and percent signs, nothing raised or lowered. A table
// cell's number is text ("−0.1", "308±6"), written as the default mode writes it.
void KeepNumbersText(const std::vector<InlineGlyph>& glyphs, std::vector<uint32_t>& run);
// Whether FindInlineFormulas takes the glyph (font, flags and text as in InlineGlyph) for strong
// math, the kind that starts a formula: a run of glyphs none of which it takes so has no formula.
// ASCII here; the rest in MayBeFormulaOther.
bool MayBeFormulaOther(uint8_t font, uint16_t flags, std::string_view text);
inline bool MayBeFormula(uint8_t font, uint16_t flags, std::string_view text) {
    const unsigned char lead = text.empty() ? 0x80 : static_cast<unsigned char>(text[0]);
    if (lead < 0x80) return lead != 0 && font != static_cast<uint8_t>(MathFont::None) && !(flags & kGlyphUnmapped);
    return MayBeFormulaOther(font, flags, text);
}

} // namespace RayoPdf::Layout
