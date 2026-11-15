// SPDX-License-Identifier: Apache-2.0
// Formula structure as a minimum-cost arborescence (--math=latex): BuildLatex's second try
// (layout/math.h) for a formula of at most kMaxTreeGlyphs glyphs whose greedy rebuild is first
// Unsure on a row's accents.
//
// Nodes. The formula's glyphs, read as symbols as the greedy rebuild reads them
// (ReadMathSymbols), after the compound merging: accents set over the glyph under them (the
// lowest first, so stacked accents nest; of accents on one baseline the one drawn later is the
// inner one, as TeX draws the outer first; a wide one over the run of base-size glyphs it spans),
// three dots joined into \ldots or \cdots, a negation slash and its relation, a bar and an arrow.
// Then its fraction bars, and its radical signs joined to the vinculum that starts at their
// right; a bar or radical has the size of the largest glyph it holds. An inline formula broken
// over lines is built line by line (an accent never starts a line).
//
// Edges. Each node's at most 6 nearest nodes in each of five sectors (right, upper right, lower
// right, above, below), when the segment between their centres crosses no third node's core.
// The cost comes from the normalised geometry of Eto and Suzuki: with h a node's size and c its
// centre (baseline + 0.25 size), H = 1000 h_b / h_a and D = 1000 (c_a - c_b) / h_a. Right:
// |D| <= 150 and 700 <= H <= 1400; Sup: -700 <= D <= -150 and 450 <= H <= 900; Sub:
// 100 <= D <= 600 and 450 <= H <= 900; each costs 30. The wider bands with 350 <= H <= 1600
// cost 70: Right |D| <= 250, Sup -900 <= D <= -100 and Sub 50 <= D <= 800 (both with H <= 1000:
// a script is never larger than its base). A glyph taller than 1.5 times its size (a big
// delimiter) neither takes nor is a script. A fraction bar's edge to the leftmost node of its
// numerator (Above) or denominator (Below), nodes whose centre its extent holds, costs 20; a
// radical's edge to the leftmost node of its radicand (Inside) 10; nothing else is an edge.
//
// Tree. The root is the leftmost node of the longest chain of Right edges outside every bar's
// and radical's parts; Chu-Liu/Edmonds gives the arborescence of least cost (ties: the nearer
// node). A node it cannot reach, a mean edge cost over 50, a node in a bar's or radical's parts
// that does not hang below it, a radical's index, a tall big operator, anything over or under a
// tall glyph, and limits over or under an operator or an upright word leave the formula Unsure.
//
// LaTeX from the tree: the nodes a Right chain reaches, in x order, each with its Sub and Sup
// children as _{…} and ^{…} (primes alone as they are); a bar with Above and Below children
// \frac{…}{…}; a radical \sqrt{…}; symbols, upright words, spaces and fixed-size delimiters as
// the greedy rebuild writes them (KaTeX's commands, math_data.cpp).
#pragma once

#include "layout/math.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Layout {

constexpr uint32_t kMaxTreeGlyphs = 256;

// The LaTeX of the formula in `in`, appended to `out` (unchanged unless Ok), with an equation
// number set apart in a display formula to `tag`, as BuildLatex: Unsure when the arborescence
// leaves a node out, costs too much or holds a structure it does not build (a delimiter piece, a
// horizontal brace, an accent without a base, limits); Unreadable when a glyph has no symbol;
// TooLarge past kMaxTreeGlyphs glyphs or kMaxFormulaRules rules. `resolved`: the calling thread's
// last BuildLatex ran its greedy rebuild on this very `in` and read every glyph (BuildLatex's own
// fallback), so the glyphs are not read again (ReadMathSymbols).
MathStatus BuildLatexTree(const MathInput& in, bool display, std::string& out, std::string& tag, bool resolved = false);

// A formula glyph read as a symbol, as BuildLatex reads it (math.cpp).
enum MathSymbolRole : uint8_t {
    kSymbolUpright = 1u << 0,     // an upright letter of a text font: `word` holds it
    kSymbolRadical = 1u << 1,     // a radical sign
    kSymbolPrime = 1u << 2,       // a prime
    kSymbolAccent = 1u << 3,      // an accent set over a base: `latex` holds its command
    kSymbolStructure = 1u << 4,   // a delimiter piece or a horizontal brace
    kSymbolBigOp = 1u << 5,       // an n-ary operator (∑, ∫)
};
struct MathSymbol {
    std::string latex;          // its LaTeX with its alphabet; a tall delimiter's with its size (\Bigl()
    std::string word;           // an upright letter's letters
    float bottom = 0, top = 0;  // its estimated ink
    uint32_t cp = 0;            // its character (0 for several)
    uint8_t role = 0;           // MathSymbolRole
};
// Reads the glyphs of `in` as BuildLatex does into `symbols` (one per glyph) and lists in `glyphs`
// those of the formula: in a display formula, an equation number set apart at the right end is
// left out and its text goes to `tag`. False when a glyph has no trustworthy symbol. `resolved`:
// as BuildLatexTree's.
bool ReadMathSymbols(const MathInput& in, bool display, std::vector<MathSymbol>& symbols, std::vector<uint32_t>& glyphs,
                     std::string& tag, bool resolved = false);
// Appends an upright word as BuildLatex writes it: \sin, \operatorname*{argmax}, \mathrm{d},
// \text{for} (with a space inside when `spaceAfter`).
void AppendUprightWord(const std::string& word, bool spaceAfter, std::string& out);
// Appends a LaTeX token, with a space after a command name that a letter would continue.
void AppendLatexToken(std::string& out, std::string_view token);
// Three of the dot `cp` in a row: \ldots or \cdots; empty for other characters.
std::string_view DotsOf(uint32_t cp);
// Two symbols side by side, `gap` apart, that make one: a negation slash and the relation (\neq,
// \notin, \not…), a bar and an arrow (\longrightarrow). False for others.
bool JoinMathSymbols(uint32_t a, uint32_t b, std::string_view latexA, std::string_view latexB, float gap, float em,
                     std::string& joined);

} // namespace RayoPdf::Layout
