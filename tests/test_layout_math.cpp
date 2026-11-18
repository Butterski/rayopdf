// SPDX-License-Identifier: Apache-2.0
// Core tests of the LaTeX math output (layout/math.h, --math=latex): formulas rebuilt from glyphs
// placed by hand (scripts, fractions, radicals, operators with limits, matrices, cases, accents,
// alphabets, function names, TeX font codes, equation numbers, aligned rows, fallback, binomials,
// over- and undersets, braces), inline formula runs (Greek prose stays text; the operand after a
// trailing operator and before a leading relation), and whole pages through ConvertPages in both
// math modes (scripts of scripts out of content order), and the arborescence the rebuild falls
// back on (stacked accents of olmOCR arxiv_math pages, a glyph cloud, not for a paragraph that is
// a formula alone), mathabx glyphs that a ToUnicode built from T1 glyph names misreads, read back
// by code, φ, ϕ, ε, ϵ by their code, a subscript's prime, overlines over x-height letters,
// blackboard fonts by name, a script's script out of content order, a display formula holding
// a word, TeX font codes KaTeX's tables leave out or WinAnsi guesses as a space (\Bigg(,
// \mapstochar, \lhook), stacked bar pieces, \not\in and \notin, \cong, extensible
// arrows with labels, \longmapsto, italic and bold letters an inline formula takes, fonts whose
// texts do not follow their codes (OAMathSymbols, newtx's txexs), fractions stacked as the rows of
// cases, a radical over a fraction, \Re, lim inf and arg max, readings by spacing ("..." and
// \ldots, \setminus and \backslash, \tfrac), neighbours in a row (a fraction after a subscript,
// \neq, a small radical, two operators' limits, Symbol's φ, MnSymbol's ○, a base outweighed by its
// subscript). Exit codes 1550-1599, 2202-2249 and 2350-2384.
#include "check.h"

#include "common/utf8.h"
#include "layout/math.h"
#include "layout/math_tree.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

constexpr uint8_t kText = static_cast<uint8_t>(MathFont::None);
constexpr uint8_t kItalic = static_cast<uint8_t>(MathFont::Italic);
constexpr uint8_t kSymbol = static_cast<uint8_t>(MathFont::Symbol);
constexpr uint8_t kExtension = static_cast<uint8_t>(MathFont::Extension);
constexpr uint8_t kBlackboard = static_cast<uint8_t>(MathFont::Blackboard);

// A formula built glyph by glyph: each character of `text` `advance` em wide from `u`.
struct Formula {
    MathInput in;
    float Add(std::string_view text, float u, float v, float size = 10, uint8_t font = kItalic, uint8_t flags = 0,
              float advance = 0.5f) {
        size_t at = 0, length = 0;
        uint32_t cp = 0;
        while (Utf8::Decode(text, at, cp, length)) {
            MathGlyph g;
            g.u0 = u, g.u1 = u + advance * size, g.v = v, g.size = size, g.font = font, g.flags = flags;
            g.text = static_cast<uint32_t>(in.text.size());
            g.length = static_cast<uint16_t>(length);
            in.text.append(text.substr(at, length));
            in.glyphs.push_back(g);
            u = g.u1;
            at += length;
        }
        return u;
    }
    void Rule(float u0, float u1, float v0, float v1) { in.rules.push_back(MathRule{u0, u1, v0, v1}); }
    std::string Latex(bool display = false, MathStatus* status = nullptr, std::string* tag = nullptr) const {
        std::string out, t;
        const MathStatus s = BuildLatex(in, display, out, t);
        if (status) *status = s;
        if (tag) *tag = t;
        if (s != MathStatus::Ok) return "<fail>";
        return out;
    }
};

void Expect(const std::string& got, const char* want, int code) {
    if (got != want) std::fprintf(stderr, "code %d: got \"%s\", want \"%s\"\n", code, got.c_str(), want);
    RAYOPDF_CHECK(got == want, code);
}

void TestScripts() {
    {
        Formula f;
        const float u = f.Add("x", 0, 0);
        f.Add("2", u, 4, 7, kText);
        Expect(f.Latex(), "x^{2}", 1550);
    }
    {
        Formula f;
        const float u = f.Add("x", 0, 0);
        f.Add("i", u, -1.5f, 7);
        Expect(f.Latex(), "x_{i}", 1551);
    }
    {
        Formula f;
        const float u = f.Add("x", 0, 0);
        f.Add("i", u, -2, 7);
        f.Add("2", u, 4, 7, kText);
        Expect(f.Latex(), "x_{i}^{2}", 1552);
    }
    {
        // e^{x_i}: a script's own script.
        Formula f;
        float u = f.Add("e", 0, 0);
        u = f.Add("x", u, 4, 7);
        f.Add("i", u, 2.6f, 5);
        Expect(f.Latex(), "e^{x_{i}}", 1553);
    }
    {
        // f'(x): a prime alone in the superscript.
        Formula f;
        float u = f.Add("f", 0, 0);
        u = f.Add("\xE2\x80\xB2", u, 4, 7, kSymbol);
        u = f.Add("(", u, 0, 10, kText);
        u = f.Add("x", u, 0);
        f.Add(")", u, 0, 10, kText);
        Expect(f.Latex(), "f'(x)", 1554);
    }
    {
        // g_{x'}: a prime of the subscript, lifted over it but not over the baseline; g_{x}' when
        // lifted as a superscript of g.
        Formula f;
        float u = f.Add("g", 0, 0);
        u = f.Add("x", u, -1.5f, 7);
        f.Add("\xE2\x80\xB2", u, 0.5f, 5, kSymbol);
        Expect(f.Latex(), "g_{x'}", 2231);
        Formula h;
        u = h.Add("g", 0, 0);
        u = h.Add("x", u, -1.5f, 7);
        h.Add("\xE2\x80\xB2", u, 3.6f, 7, kSymbol);
        Expect(h.Latex(), "g_{x}'", 2232);
    }
}

void TestFractions() {
    {
        Formula f;
        f.Add("a", 1.5f, 6);
        f.Add("b", 1.5f, -6);
        f.Rule(0, 8, 2.4f, 2.8f);
        Expect(f.Latex(), "\\frac{a}{b}", 1555);
    }
    {
        // y = (1/2)/b: a fraction in a numerator.
        Formula f;
        float u = f.Add("y", -12, 0);
        f.Add("=", u, 0, 10, kText);
        f.Add("1", 3, 18, 10, kText);
        f.Add("2", 3, 8, 10, kText);
        f.Rule(2, 9, 14.4f, 14.8f);
        f.Add("b", 3, -6);
        f.Rule(0, 12, 2.4f, 2.8f);
        Expect(f.Latex(), "y=\\frac{\\frac{1}{2}}{b}", 1556);
    }
    {
        // An overline: a bar over text with nothing above it.
        Formula f;
        f.Add("AB", 0, 0, 10, kItalic);
        f.Rule(0, 10, 8.5f, 8.9f);
        Expect(f.Latex(), "\\overline{AB}", 1557);
    }
    {
        // An overline over an x-height letter: TeX sets the bar three rule widths over the
        // x-height, closer to the letter's baseline than a fraction bar to its denominator's. The
        // overline stays on the letter's baseline, so the subscript after it is its subscript.
        Formula f;
        const float u = f.Add("u", 0, 0, 10, kItalic);
        f.Add("j", u, -1.5f, 7);
        f.Rule(0, 5, 5.5f, 5.9f);
        Expect(f.Latex(), "\\overline{u}_{j}", 2230);
        // A bar through the middle of a letter is no overline.
        Formula g;
        g.Add("u", 0, 0, 10, kItalic);
        g.Rule(0, 5, 2.4f, 2.8f);
        Expect(g.Latex(), "u", 2233);
        // Nor is a bar that low over a letter with an ascender, which reaches it.
        Formula h;
        h.Add("d", 0, 0, 10, kItalic);
        h.Rule(0, 5, 5.5f, 5.9f);
        Expect(h.Latex(), "d", 2238);
    }
}

void TestRadicals() {
    Formula f;
    f.Add("\xE2\x88\x9A", 0, 8, 10, kSymbol, 0, 0.8f);  // √, hanging from its origin as in TeX
    f.Rule(8, 16, 8, 8.4f);
    f.Add("x", 10, 0);
    Expect(f.Latex(), "\\sqrt{x}", 1558);
    f.Add("3", 1, 4, 6, kText);
    Expect(f.Latex(), "\\sqrt[3]{x}", 1559);
    // No rule over the radicand: the sign alone.
    Formula g;
    float u = g.Add("\xE2\x88\x9A", 0, 0, 10, kText);
    g.Add("2", u, 0, 10, kText);
    Expect(g.Latex(), "\\surd2", 1560);
}

void TestOperators() {
    {
        // A display sum from cmex10 (code 0x58, its name unknown to the text layer: 'X' as WinAnsi),
        // limits centred below and above.
        Formula f;
        f.Add("X", 0, 9, 10, kExtension, kMathGuessed, 1.4f);
        float u = f.Add("i", 2.5f, -12, 7);
        u = f.Add("=", u, -12, 7, kText);
        f.Add("1", u, -12, 7, kText);
        f.Add("n", 5.25f, 12, 7);
        f.Add("x", 16, 0);
        Expect(f.Latex(), "\\sum_{i=1}^{n}x", 1561);
    }
    {
        // An integral with its limits as scripts at its right.
        Formula f;
        f.Add("\xE2\x88\xAB", 0, 0, 10, kExtension, 0, 0.6f);
        f.Add("0", 6, -4, 7, kText);
        f.Add("1", 7, 7, 7, kText);
        f.Add("x", 12, 0);
        Expect(f.Latex(), "\\int_{0}^{1}x", 1562);
    }
    {
        // lim with its limit below; sin before its argument.
        Formula f;
        f.Add("lim", 0, 0, 10, kText);
        f.Add("n\xE2\x86\x92\xE2\x88\x9E", 0, -8, 7, kSymbol);
        float u = f.Add("sin", 18, 0, 10, kText);
        f.Add("x", u + 2, 0);
        Expect(f.Latex(), "\\lim_{n\\rightarrow\\infty}\\sin x", 1563);
    }
    {
        // An operator name without a KaTeX command: \operatorname.
        Formula f;
        float u = f.Add("Tr", 0, 0, 10, kText);
        f.Add("g", u + 2, 0);
        Expect(f.Latex(), "\\operatorname{Tr}g", 2360);
    }
}

void TestMatrices() {
    {
        Formula f;
        // ⎛ ⎜ ⎝ and ⎞ ⎟ ⎠ stacked, two rows of two entries between them.
        f.Add("\xE2\x8E\x9B", 0, 10, 10, kExtension);
        f.Add("\xE2\x8E\x9C", 0, 4, 10, kExtension);
        f.Add("\xE2\x8E\x9D", 0, -2, 10, kExtension);
        f.Add("a", 8, 5);
        f.Add("b", 20, 5);
        f.Add("c", 8, -7);
        f.Add("d", 20, -7);
        f.Add("\xE2\x8E\x9E", 30, 10, 10, kExtension);
        f.Add("\xE2\x8E\x9F", 30, 4, 10, kExtension);
        f.Add("\xE2\x8E\xA0", 30, -2, 10, kExtension);
        Expect(f.Latex(), "\\begin{pmatrix}a&b\\\\c&d\\end{pmatrix}", 1564);
    }
    {
        // Cases: a tall left brace and no closing delimiter.
        Formula f;
        f.Add("f", -12, -1);
        f.Add("=", -6, -1, 10, kText);
        f.Add("\xE2\x8E\xA7", 0, 10, 10, kExtension);
        f.Add("\xE2\x8E\xA8", 0, 4, 10, kExtension);
        f.Add("\xE2\x8E\xA9", 0, -2, 10, kExtension);
        f.Add("1", 8, 5, 10, kText);
        f.Add("x", 24, 5);
        f.Add("0", 8, -7, 10, kText);
        f.Add("y", 24, -7);
        Expect(f.Latex(), "f=\\begin{cases}1&x\\\\0&y\\end{cases}", 1565);
    }
}

void TestSymbols() {
    {
        Formula f;
        f.Add("x", 0, 0);
        f.Add("\xCB\x86", 1, 0, 10, kText);  // ˆ over the x
        Expect(f.Latex(), "\\hat{x}", 1566);
    }
    {
        // Mathematical alphanumerics, letterlike symbols, MSBM and CMSY capitals.
        Formula f;
        float u = f.Add("\xF0\x9D\x90\x80", 0, 0, 10, kText);   // 𝐀
        u = f.Add("\xE2\x84\x9D", u, 0, 10, kText);             // ℝ
        u = f.Add("\xF0\x9D\x92\xAE", u, 0, 10, kText);         // 𝒮
        u = f.Add("\xF0\x9D\x91\xA5", u, 0, 10, kText);         // 𝑥
        u = f.Add("R", u, 0, 10, kBlackboard);
        u = f.Add("L", u, 0, 10, kSymbol);
        f.Add("\xCE\xB1", u, 0, 10, kItalic);                   // α
        Expect(f.Latex(), "\\mathbf{A}\\mathbb{R}\\mathcal{S}x\\mathbb{R}\\mathcal{L}\\alpha", 1567);
    }
    {
        // TeX font codes behind WinAnsi guesses: CMSY 'k' is ∥, CMEX 'P' a text-size sum.
        Formula f;
        float u = f.Add("k", 0, 0, 10, kSymbol, kMathGuessed);
        u = f.Add("x", u, 0);
        u = f.Add("k", u, 0, 10, kSymbol, kMathGuessed);
        f.Add("P", u + 2, 7, 10, kExtension, kMathGuessed, 1.0f);
        Expect(f.Latex(), "\\|x\\|\\sum", 1568);
    }
    {
        // ≤, −, ∈ and an upright word in a display.
        Formula f;
        float u = f.Add("x", 0, 0);
        u = f.Add("\xE2\x89\xA4", u, 0, 10, kSymbol);
        u = f.Add("\xE2\x88\x92", u, 0, 10, kSymbol);
        u = f.Add("1", u, 0, 10, kText);
        u = f.Add("for", u + 12, 0, 10, kText);
        u = f.Add("x", u + 3, 0);
        u = f.Add("\xE2\x88\x88", u, 0, 10, kSymbol);
        f.Add("A", u, 0);
        Expect(f.Latex(true), "x\\leq-1\\quad\\text{for }x\\in A", 1569);
    }
}

void TestDisplay() {
    {
        // An equation number set apart on the right goes to the tag.
        Formula f;
        float u = f.Add("x", 0, 0);
        u = f.Add("=", u, 0, 10, kText);
        f.Add("1", u, 0, 10, kText);
        f.Add("(3.1)", 100, 0, 10, kText);
        std::string tag;
        Expect(f.Latex(true, nullptr, &tag), "x=1", 1570);
        Expect(tag, "3.1", 1571);
        // Inline, nothing is a tag.
        Expect(f.Latex(false), "x=1\\qquad(3.1)", 1572);
    }
    {
        // Two rows aligned on their relations.
        Formula f;
        f.Add("a", 0, 0);
        f.Add("=", 10, 0, 10, kText);
        f.Add("b", 20, 0);
        f.Add("=", 10, -14, 10, kText);
        f.Add("c", 20, -14);
        Expect(f.Latex(true), "\\begin{aligned}a&=b\\\\&=c\\end{aligned}", 1573);
    }
}

void TestFallback() {
    MathStatus s = MathStatus::Ok;
    Formula empty;
    empty.Latex(false, &s);
    RAYOPDF_CHECK(s == MathStatus::Empty, 1574);
    Formula unmapped;
    unmapped.Add("x", 0, 0, 10, kItalic, kMathUnmapped);
    unmapped.Latex(false, &s);
    RAYOPDF_CHECK(s == MathStatus::Unreadable, 1575);
    Formula pua;
    pua.Add("\xEE\x80\x80", 0, 0, 10, kSymbol);  // U+E000: no symbol known
    pua.Latex(false, &s);
    RAYOPDF_CHECK(s == MathStatus::Unreadable, 1576);
    Formula big;
    for (uint32_t i = 0; i <= kMaxFormulaGlyphs; ++i) big.Add("x", float(i) * 5, 0);
    big.Latex(false, &s);
    RAYOPDF_CHECK(s == MathStatus::TooLarge, 1577);
    Formula nan;
    nan.Add("x", 0, 0);
    nan.in.glyphs[0].v = NAN;
    nan.Latex(false, &s);
    RAYOPDF_CHECK(s == MathStatus::Unreadable, 1578);
    // A guess in a math font of no known encoding is no symbol.
    Formula guess;
    guess.Add("Q", 0, 0, 10, static_cast<uint8_t>(MathFont::Unicode), kMathGuessed);
    guess.Latex(false, &s);
    RAYOPDF_CHECK(s == MathStatus::Unreadable, 1579);
    // An accent over nothing cannot be placed.
    Formula accent;
    accent.Add("\xCB\x86", 0, 0, 10, kText);
    accent.Add("x", 30, 0);
    accent.Latex(false, &s);
    RAYOPDF_CHECK(s == MathStatus::Unsure, 1580);
}

void TestFonts() {
    RAYOPDF_CHECK(MathFontOf("CMMI10") == MathFont::Italic, 1581);
    RAYOPDF_CHECK(MathFontOf("CMSY7") == MathFont::Symbol && MathFontOf("cmex10") == MathFont::Extension, 1582);
    RAYOPDF_CHECK(MathFontOf("MSBM10") == MathFont::Blackboard && MathFontOf("EUFM10") == MathFont::Fraktur, 1583);
    RAYOPDF_CHECK(MathFontOf("LMMathItalic10-Regular") == MathFont::Italic &&
                      MathFontOf("LatinModernMath-Regular") == MathFont::Unicode &&
                      MathFontOf("CambriaMath") == MathFont::Unicode,
                  1584);
    RAYOPDF_CHECK(MathFontOf("CMR10") == MathFont::None && MathFontOf("CMTI10") == MathFont::None &&
                      MathFontOf("TimesNewRomanPSMT") == MathFont::None,
                  1585);
}

// Inline runs over words written glyph by glyph: `fonts` per glyph (spaces are no glyphs) of
// `text`: 'm' math italic, 'i' italic text font, 'b' bold text font, 's' an italic letter of the
// text font lowered (a script), anything else the upright text font; spaces start words.
std::vector<uint32_t> Runs(std::string_view text, std::string_view fonts, std::vector<Glyph>& store,
                           std::vector<InlineGlyph>* glyphsOut = nullptr) {
    store.assign(text.size(), Glyph());
    std::vector<InlineGlyph> glyphs;
    bool space = false;
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        if (cp == ' ') {
            space = true;
        } else {
            const size_t index = glyphs.size();
            InlineGlyph g;
            g.glyph = &store[index];
            g.font = index < fonts.size() && fonts[index] == 'm' ? kItalic : kText;
            if (index < fonts.size() && (fonts[index] == 'i' || fonts[index] == 's')) store[index].flags |= kGlyphItalic;
            if (index < fonts.size() && fonts[index] == 'b') store[index].flags |= kGlyphBold;
            g.script = index < fonts.size() && fonts[index] == 's';
            g.wordStart = space || glyphs.empty();
            g.text = text.substr(at, length);
            glyphs.push_back(g);
            space = false;
        }
        at += length;
    }
    std::vector<uint32_t> run;
    FindInlineFormulas(glyphs, run);
    if (glyphsOut) glyphsOut->swap(glyphs);
    return run;
}

void TestInline() {
    std::vector<Glyph> store;
    // "Let x = 2 be": x math italic; the run takes "x = 2". Glyphs: L e t x = 2 b e.
    std::vector<uint32_t> run = Runs("Let x = 2 be", "...m....", store);
    RAYOPDF_CHECK(run[3] == 6 && run[0] == 0 && run[6] == 0, 1586);
    // "x, then" leaves the comma out.
    run = Runs("x, then", "m", store);
    RAYOPDF_CHECK(run[0] == 1, 1587);
    // A Greek word is text; a Greek letter alone is math.
    run = Runs("\xCE\xB1\xCE\xB2\xCE\xB3 \xCE\xB1", "", store);
    RAYOPDF_CHECK(run[0] == 0 && run[3] == 4, 1588);
    // Greek prose with accents (tonos, polytonic) is text, not a formula of its plain letters:
    // "Φαίδρος ἢ περὶ ῥητορικῆς".
    run = Runs("\xCE\xA6\xCE\xB1\xCE\xAF\xCE\xB4\xCF\x81\xCE\xBF\xCF\x82 \xE1\xBC\xA2 \xCF\x80\xCE\xB5\xCF\x81\xE1\xBD\xB6 "
               "\xE1\xBF\xA5\xCE\xB7\xCF\x84\xCE\xBF\xCF\x81\xCE\xB9\xCE\xBA\xE1\xBF\x86\xCF\x82",
               "", store);
    bool none = true;
    for (uint32_t r : run) none = none && r == 0;
    RAYOPDF_CHECK(none, 2202);
    // "n-th": the letter alone; an italic letter of a text font alone starts no formula.
    run = Runs("n-th a b", "m...ii", store);
    RAYOPDF_CHECK(run[0] == 1 && run[4] == 0 && run[5] == 0, 1589);
    // "f(x) and (x)" keeps balanced brackets; "(see x)" leaves the unmatched one out.
    run = Runs("f(x) and (x)", "m.m.....m.", store);
    RAYOPDF_CHECK(run[0] == 4 && run[4] == 0 && run[7] == 10, 1590);
    run = Runs("(see x)", "....m.", store);
    RAYOPDF_CHECK(run[0] == 0 && run[4] == 5, 1596);
    // A run that ends in an operator takes the operand after it, an italic letter of a text font
    // with its scripts too: "map x→ S of" (x, → math italic), "x∈ dᵢ now".
    run = Runs("map x\xE2\x86\x92 S of", "...mmi..", store);
    RAYOPDF_CHECK(run[3] == 6 && run[5] == 0, 2226);
    run = Runs("x\xE2\x88\x88 di now", "mmis...", store);
    RAYOPDF_CHECK(run[0] == 4, 2227);
    // A run that opens with a relation takes a bold or italic letter alone before it ("u ∈ ℝ"),
    // not an upright one.
    run = Runs("with u \xE2\x88\x88 R hold", "....bmm....", store);
    RAYOPDF_CHECK(run[4] == 7 && run[5] == 0, 2228);
    run = Runs("type u \xE2\x88\x88 R hold", ".....mm....", store);
    RAYOPDF_CHECK(run[4] == 0 && run[5] == 7, 2229);
    // An operator name between math glyphs joins the formula: "|Tr g|<2" (| and g of the math
    // fonts), "diam T ≤ h" (T italic of the text font).
    run = Runs("|Tr g|<2", "m..mm..", store);
    RAYOPDF_CHECK(run[0] == 7, 2353);
    run = Runs("so diam T \xE2\x89\xA4 h", "......imm", store);
    RAYOPDF_CHECK(run[0] == 0 && run[2] == 9, 2354);
    // Math set in a text font's italics or bold: up to three such letters in a word with math
    // ("κ(BS)", a bold vector "σ(u)"), but not a suffix after a hyphen ("i-th").
    run = Runs("\xCE\xBA(BS)", "..ii.", store);
    RAYOPDF_CHECK(run[0] == 5, 2355);
    run = Runs("\xCF\x83(u)", "..b.", store);
    RAYOPDF_CHECK(run[0] == 4, 2356);
    run = Runs("the i-th one", "...m.ii...", store);
    RAYOPDF_CHECK(run[3] == 4, 2357);
    // A name of up to eight letters before an argument's bracket after math: "π(conv(w))".
    run = Runs("\xCF\x80(conv(w))", "m......m..", store);
    RAYOPDF_CHECK(run[0] == 10, 2358);
    // # between math: "X#Y".
    run = Runs("X#Y", "m.m", store);
    RAYOPDF_CHECK(run[0] == 3, 2359);
    // An italic letter of a text font with its scripts is math (Times Italic math): "Sₙ = Iₙ".
    run = Runs("Sn = In", "is.is", store);
    RAYOPDF_CHECK(run[0] == 5, 2361);
    // The operand before a relation may be a letter with its argument ("T(u) ≡ y"); the one after
    // an operator ending a run may be a letter of any face before punctuation ("x ⩽ b."), but not
    // the article a ("x = a set").
    run = Runs("so T(u) \xE2\x89\xA1 y", "..i.i.mm", store);
    RAYOPDF_CHECK(run[2] == 8, 2363);
    run = Runs("x \xE2\xA9\xBD b. Then", "mm......", store);
    RAYOPDF_CHECK(run[0] == 3, 2364);
    run = Runs("x = a set", "m.....", store);
    RAYOPDF_CHECK(run[0] == 2, 2365);
    // A name after a hyphen is a compound word's, not an operator's: "Val-Clash(↑)".
    run = Runs("Val-Clash(\xE2\x86\x91)", "", store);
    RAYOPDF_CHECK(run[4] == 0 && run[9] == 12, 2366);
}

// A table cell's number stays text (KeepNumbersText): signs, digits (bold ones too), ± and the
// decimal point; a run with a letter stays a formula.
void TestNumbersText() {
    std::vector<Glyph> store;
    std::vector<InlineGlyph> glyphs;
    // Glyphs: − 0 . 1 | a n d | 3 0 8 ± 6 | o r | x + 1 | a n d | 𝟐 𝟒 ± 𝟏
    std::vector<uint32_t> run = Runs("\xE2\x88\x92" "0.1 and 308\xC2\xB1" "6 or x+1 and \xF0\x9D\x9F\x90\xF0\x9D\x9F\x92\xC2\xB1\xF0\x9D\x9F\x8F",
                                     "..............m", store, &glyphs);
    const bool found = run[0] == 4 && run[7] == 12 && run[14] == 17 && run[20] == 24;
    KeepNumbersText(glyphs, run);
    RAYOPDF_CHECK(found && run[0] == 0 && run[7] == 0 && run[14] == 17 && run[20] == 0, 1597);
}

// A page: glyphs of `text` at (x, y) in font `font` (page-local: 0 text, 1 math italic).
float AddText(PageData& page, std::string_view text, float x, float y, float size, uint16_t font = 0) {
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        const float w = 0.5f * size;
        g.u0 = x, g.u1 = x + w, g.vb = y - 0.2f * size, g.vt = y + 0.8f * size, g.v = y;
        g.size = size;
        g.font = font;
        g.text = static_cast<uint32_t>(page.text.size());
        page.text.append(text.substr(at, length));
        g.textLength = static_cast<uint16_t>(length);
        if (cp == ' ') g.flags |= kGlyphSpace;
        if (font == 1) g.flags |= kGlyphItalic;
        page.glyphs.push_back(g);
        x += w;
        at += length;
    }
    return x;
}

std::string Convert(std::vector<PageData> pages, MathMode math) {
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report, nullptr, nullptr, math);
    return md;
}

void TestPages() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic};
    float x = AddText(page, "Let ", 72, 700, 10);
    x = AddText(page, "x", x, 700, 10, 1);
    AddText(page, " be a number that costs $5 or $A.", x, 700, 10);
    // A display formula: y = a/b, its numerator and denominator lines of their own.
    x = AddText(page, "y", 280, 674, 10, 1);
    AddText(page, " =", x, 674, 10);
    AddText(page, "a", 301, 682, 10, 1);
    AddText(page, "b", 301, 667, 10, 1);
    page.rulings.push_back(Ruling{299, 676.4f, 308, 676.8f, kRulingFill});
    AddText(page, "And so the text goes on after the formula.", 72, 640, 10);
    x = AddText(page, "Its square ", 72, 628, 10);
    x = AddText(page, "y", x, 628, 10, 1);
    x = AddText(page, "2", x, 632, 6, 1);
    AddText(page, " is no tag.", x, 628, 10);
    const std::string latex = Convert({page}, MathMode::Latex);
    const std::string unicode = Convert({page}, MathMode::Unicode);
    if (latex.find("Let $x$ be a number that costs $5 or \\$A.") == std::string::npos ||
        latex.find("$$y=\\frac{a}{b}$$") == std::string::npos)
        std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(latex.find("Let $x$ be a number that costs $5 or \\$A.") != std::string::npos, 1591);
    RAYOPDF_CHECK(latex.find("$$y=\\frac{a}{b}$$") != std::string::npos, 1592);
    RAYOPDF_CHECK(latex.find("And so the text goes on") != std::string::npos, 1593);
    // The default output has no LaTeX and no escaped dollar.
    RAYOPDF_CHECK(unicode.find("Let x be a number that costs $5 or $A.") != std::string::npos, 1594);
    RAYOPDF_CHECK(unicode.find("\\frac") == std::string::npos && unicode.find("$$") == std::string::npos, 1595);
    // A script of a math font in Unicode forms.
    RAYOPDF_CHECK(unicode.find("Its square y\xC2\xB2 is no tag.") != std::string::npos, 1598);
    // A sized bracket of the extension font hangs from its origin, its width telling its size
    // (cmex10's \bigg[ is 0.53 em wide and 2.4 em high): its baseline goes a quarter em under the
    // middle of that ink.
    PageData big;
    big.fontMath = {kText, kItalic, kExtension};
    AddText(big, "[", 72, 700, 10, 2);
    big.glyphs[0].u1 = 72 + 5.3f;
    PrepareMathGlyphs(big);
    RAYOPDF_CHECK(big.glyphs[0].v > 685 && big.glyphs[0].v < 687, 2362);
}

// \binom, \overset / \underset and \overbrace / \underbrace, each with a case that keeps the
// output it had.
void TestConstructs() {
    // Two rows between cmex10 parentheses 30 pt high, at baselines `top` and `bottom`.
    const auto stacked = [](float top, float bottom) {
        Formula f;
        f.Add("(", 0, 14, 10, kExtension, 0, 0.8f);
        f.Add("n", 10, top);
        f.Add("k", 10, bottom);
        f.Add(")", 18, 14, 10, kExtension, 0, 0.8f);
        return f.Latex();
    };
    // Baselines 1.36 em apart, as TeX sets a display binomial (ink 0.39 em apart).
    Expect(stacked(6.8f, -6.8f), "\\binom{n}{k}", 2205);
    // Baselines 1.2 em apart, a \baselineskip: a column vector (ink 0.23 em apart).
    Expect(stacked(6, -6), "\\begin{pmatrix}n\\\\k\\end{pmatrix}", 2206);
    {
        // "def" at 7 pt centred over the =.
        Formula f;
        f.Add("x", 0, 0);
        f.Add("=", 5, 0, 10, kText);
        f.Add("def", 2.25f, 7, 7);
        f.Add("y", 10, 0);
        Expect(f.Latex(), "x\\overset{def}{=}y", 2207);
    }
    {
        // A raised 2 right after the =: its superscript.
        Formula f;
        f.Add("x", 0, 0);
        f.Add("=", 5, 0, 10, kText);
        f.Add("2", 10, 4, 7, kText);
        f.Add("y", 16, 0);
        Expect(f.Latex(), "x=^{2}y", 2208);
    }
    {
        // n at 7 pt centred under the arrow; at 10 pt it is the arrow's subscript.
        for (const float size : {7.0f, 10.0f}) {
            Formula f;
            f.Add("x", 0, 0);
            f.Add("\xE2\x86\x92", 5, 0, 10, kSymbol);
            f.Add("n", 7.5f - 0.25f * size, -6, size);
            f.Add("y", 10, 0);
            if (size < 8)
                Expect(f.Latex(), "x\\underset{n}{\\rightarrow}y", 2209);
            else
                Expect(f.Latex(), "x\\rightarrow_{n}y", 2210);
        }
    }
    {
        // ⏞ over a+b with n over it, ⏟ under a+b with n under it.
        Formula over;
        over.Add("a+b", 0, 0, 10, kItalic);
        over.Add("\xE2\x8F\x9E", 0.5f, 9, 10, kText, 0, 1.4f);
        over.Add("n", 5.75f, 15, 7);
        Formula under;
        under.Add("a+b", 0, 0, 10, kItalic);
        under.Add("\xE2\x8F\x9F", 0.5f, -7, 10, kText, 0, 1.4f);
        under.Add("n", 5.75f, -14, 7);
        const std::string o = over.Latex(), u = under.Latex();
        if (o != "\\overbrace{a+b}^{n}" || u != "\\underbrace{a+b}_{n}")
            std::fprintf(stderr, "code 2211: got \"%s\" and \"%s\"\n", o.c_str(), u.c_str());
        RAYOPDF_CHECK(o == "\\overbrace{a+b}^{n}" && u == "\\underbrace{a+b}_{n}", 2211);
        // A brace over 70 % of what it overlaps is no \overbrace.
        Formula shortBrace;
        shortBrace.Add("a+b", 0, 0, 10, kItalic);
        shortBrace.Add("\xE2\x8F\x9E", 0, 9, 10, kText, 0, 0.7f);
        shortBrace.Add("n", 2, 15, 7);
        const std::string s = shortBrace.Latex();
        if (s.find("\\overbrace") != std::string::npos) std::fprintf(stderr, "code 2212: got \"%s\"\n", s.c_str());
        RAYOPDF_CHECK(s.find("\\overbrace") == std::string::npos, 2212);
    }
}

// A script's own script drawn as a run of its own (out of content order) joins its line in LaTeX
// mode: e^{x^2} from three runs, the 2 above the line's ink (glyph boxes as tight as x-heights);
// a small line 0.7 em under the baseline stays apart.
void TestNestedScripts() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic};
    const auto glyph = [&](const char* text, float x, float y, float size) {
        Glyph g;
        g.u0 = x, g.u1 = x + 0.5f * size, g.vb = y - 0.2f * size, g.vt = y + 0.45f * size, g.v = y;
        g.size = size;
        g.font = 1;
        g.flags = kGlyphItalic;
        g.text = static_cast<uint32_t>(page.text.size());
        page.text.append(text);
        g.textLength = static_cast<uint16_t>(std::string_view(text).size());
        page.glyphs.push_back(g);
        return g.u1;
    };
    float x = AddText(page, "The value ", 72, 700, 10);
    const float e = glyph("e", x, 700, 10);
    const float xs = glyph("x", e, 703.6f, 7);
    AddText(page, " grows fast and the text goes on.", xs + 3, 700, 10);
    glyph("2", xs, 708.5f, 5);  // its ink 0.75 pt above the line's (x's ascent): a line of its own before
    glyph("1", x, 693, 5);
    AddText(page, "A second paragraph starts here after a gap.", 72, 660, 10);
    const std::string latex = Convert({page}, MathMode::Latex);
    if (latex.find("$e^{x^{2}}$ grows fast") == std::string::npos || latex.find("_{1}") != std::string::npos)
        std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(latex.find("The value $e^{x^{2}}$ grows fast") != std::string::npos, 2215);
    RAYOPDF_CHECK(latex.find("_{1}") == std::string::npos && latex.find("1") != std::string::npos, 2216);
}

// One glyph of a formula as the text layer gave it on an olmOCR arxiv_math page.
struct PageGlyph {
    float u0, u1, v, size, bottom, top;
    uint8_t font, flags;
    const char* text;
};

MathInput InputOf(const PageGlyph* glyphs, size_t count) {
    MathInput in;
    for (size_t i = 0; i < count; ++i) {
        const PageGlyph& p = glyphs[i];
        MathGlyph g;
        g.u0 = p.u0, g.u1 = p.u1, g.v = p.v, g.size = p.size, g.bottom = p.bottom, g.top = p.top;
        g.font = p.font, g.flags = p.flags;
        g.text = static_cast<uint32_t>(in.text.size());
        g.length = static_cast<uint16_t>(std::string_view(p.text).size());
        in.text.append(p.text);
        in.glyphs.push_back(g);
    }
    return in;
}

std::string LatexOf(const MathInput& in) {
    std::string out, tag;
    return BuildLatex(in, false, out, tag) == MathStatus::Ok ? out : "<fail>";
}

// The arborescence (math_tree.h) where the greedy rebuild is Unsure: inline formulas of olmOCR
// arxiv_math development pages rebuilt glyph by glyph (stacked accents set on their base's
// baseline, which the greedy rebuild refuses), a formula through BuildLatexTree directly, and a
// glyph cloud.
void TestTree() {
    {
        // 2503.07449 page 17: a hat over a dot over q.
        static constexpr PageGlyph kGlyphs[] = {
            {464.73f, 469.33f, 592.48f, 8.97f, 590.74f, 598.70f, 0, 0, "\xCB\x86"},
            {465.75f, 468.31f, 592.48f, 8.97f, 590.74f, 598.70f, 0, 0, "\xCB\x99"},
            {464.04f, 468.16f, 592.48f, 8.97f, 590.74f, 598.70f, 1, 1, "q"},
        };
        Expect(LatexOf(InputOf(kGlyphs, 3)), "\\hat{\\dot{q}}", 2217);
    }
    {
        // 2503.09135 page 6: dots over bars, each over its letter.
        static constexpr PageGlyph kGlyphs[] = {
            {232.74f, 235.51f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, "\xCB\x99"},
            {231.64f, 236.62f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, "\xC2\xAF"},
            {227.30f, 234.77f, 293.27f, 9.96f, 291.34f, 300.18f, 1, 1, "A"},
            {234.77f, 239.41f, 293.27f, 9.96f, 291.34f, 300.18f, 1, 1, "z"},
            {239.85f, 243.72f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, "("},
            {243.72f, 247.32f, 293.27f, 9.96f, 291.34f, 300.18f, 1, 1, "t"},
            {247.32f, 251.29f, 291.77f, 6.97f, 290.42f, 296.61f, 0, 0, "1"},
            {251.79f, 255.66f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, ")"},
            {255.66f, 258.43f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, ","},
            {266.74f, 269.51f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, "\xCB\x99"},
            {265.64f, 270.62f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, "\xC2\xAF"},
            {261.75f, 269.10f, 293.27f, 9.96f, 291.34f, 300.18f, 1, 1, "E"},
            {269.68f, 272.66f, 293.27f, 9.96f, 291.34f, 300.18f, 1, 1, "l"},
            {272.85f, 276.72f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, "("},
            {276.72f, 280.32f, 293.27f, 9.96f, 291.34f, 300.18f, 1, 1, "t"},
            {280.32f, 284.29f, 291.77f, 6.97f, 290.42f, 296.61f, 0, 0, "1"},
            {284.79f, 288.66f, 293.27f, 9.96f, 291.34f, 300.18f, 0, 0, ")"},
        };
        Expect(LatexOf(InputOf(kGlyphs, sizeof(kGlyphs) / sizeof(kGlyphs[0]))),
               "\\dot{\\bar{A}}z(t_{1}),\\dot{\\bar{E}}l(t_{1})", 2218);
    }
    {
        // 2503.09123 page 4: scripts of scripts between big parentheses of cmex10.
        static constexpr PageGlyph kGlyphs[] = {
            {61.32f, 69.06f, 131.16f, 9.96f, 128.67f, 138.63f, 2, 0, "\xE2\x88\x92"},
            {69.12f, 73.16f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "\xC7\xAB"},
            {75.84f, 83.58f, 131.16f, 9.96f, 128.67f, 138.63f, 2, 0, "\xE2\x89\xA4"},
            {86.40f, 91.27f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "f"},
            {92.40f, 98.35f, 131.14f, 9.96f, 124.67f, 142.60f, 3, 0, "("},
            {99.96f, 102.72f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, "\xCB\x99"},
            {98.88f, 103.86f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, "\xCB\x9C"},
            {98.28f, 103.97f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "x"},
            {103.92f, 107.03f, 135.24f, 6.97f, 133.50f, 140.47f, 0, 0, "("},
            {107.04f, 111.27f, 135.24f, 6.97f, 133.82f, 140.22f, 1, 32, "k"},
            {111.36f, 114.47f, 135.24f, 6.97f, 133.50f, 140.47f, 0, 0, ")"},
            {115.08f, 118.95f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, "("},
            {118.92f, 124.10f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "d"},
            {124.08f, 127.19f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, "("},
            {127.20f, 131.43f, 136.32f, 6.97f, 134.90f, 141.30f, 1, 32, "k"},
            {131.52f, 134.63f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, ")"},
            {124.08f, 126.90f, 128.40f, 6.97f, 126.98f, 133.38f, 1, 32, "i"},
            {135.24f, 139.11f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, ")"},
            {139.08f, 141.84f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, ","},
            {144.24f, 149.22f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, "\xCB\x9C"},
            {143.52f, 149.21f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "x"},
            {149.16f, 152.27f, 135.24f, 6.97f, 133.50f, 140.47f, 0, 0, "("},
            {152.28f, 156.51f, 135.24f, 6.97f, 133.82f, 140.22f, 1, 32, "k"},
            {156.60f, 159.71f, 135.24f, 6.97f, 133.50f, 140.47f, 0, 0, ")"},
            {160.44f, 164.31f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, "("},
            {164.28f, 169.46f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "d"},
            {169.44f, 172.55f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, "("},
            {172.56f, 176.79f, 136.32f, 6.97f, 134.90f, 141.30f, 1, 32, "k"},
            {176.88f, 179.99f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, ")"},
            {169.44f, 172.26f, 128.40f, 6.97f, 126.98f, 133.38f, 1, 32, "i"},
            {180.60f, 184.47f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, ")"},
            {184.44f, 187.20f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, ","},
            {189.60f, 194.58f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, "\xCB\x9C"},
            {188.88f, 194.58f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "u"},
            {194.64f, 197.75f, 135.24f, 6.97f, 133.50f, 140.47f, 0, 0, "("},
            {197.76f, 201.99f, 135.24f, 6.97f, 133.82f, 140.22f, 1, 32, "k"},
            {202.08f, 205.19f, 135.24f, 6.97f, 133.50f, 140.47f, 0, 0, ")"},
            {205.68f, 209.55f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, "("},
            {209.52f, 214.70f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "d"},
            {214.68f, 217.79f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, "("},
            {217.80f, 222.03f, 136.32f, 6.97f, 134.90f, 141.30f, 1, 32, "k"},
            {222.12f, 225.23f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, ")"},
            {214.68f, 217.50f, 128.40f, 6.97f, 126.98f, 133.38f, 1, 32, "i"},
            {225.96f, 229.83f, 131.16f, 9.96f, 128.67f, 138.63f, 0, 0, ")"},
            {229.80f, 232.56f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, ","},
            {234.24f, 239.42f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "d"},
            {239.40f, 242.51f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, "("},
            {242.52f, 246.75f, 136.32f, 6.97f, 134.90f, 141.30f, 1, 32, "k"},
            {246.84f, 249.95f, 136.32f, 6.97f, 134.58f, 141.55f, 0, 0, ")"},
            {239.40f, 242.22f, 128.40f, 6.97f, 126.98f, 133.38f, 1, 32, "i"},
            {250.56f, 253.32f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, ","},
            {255.00f, 260.01f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "p"},
            {260.04f, 265.99f, 131.14f, 9.96f, 124.67f, 142.60f, 3, 0, ")"},
            {268.68f, 276.42f, 131.16f, 9.96f, 128.67f, 138.63f, 2, 0, "\xE2\x89\xA4"},
            {279.24f, 283.28f, 131.16f, 9.96f, 129.01f, 138.24f, 1, 0, "\xC7\xAB"},
        };
        Expect(LatexOf(InputOf(kGlyphs, sizeof(kGlyphs) / sizeof(kGlyphs[0]))),
               "-\\epsilon\\leq f\\Bigl(\\dot{\\tilde{x}}^{(k)}(d_{i}^{(k)}),\\tilde{x}^{(k)}(d_{i}^{(k)}),"
               "\\tilde{u}^{(k)}(d_{i}^{(k)}),d_{i}^{(k)},p\\Bigr)\\leq\\epsilon",
               2219);
    }
    {
        // 2503.07623 page 2: the tilde over the Delta set apart, a line start of its own at the end.
        static constexpr PageGlyph kGlyphs[] = {
            {246.48f, 254.78f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "\xE2\x88\x86"},
            {254.76f, 259.60f, 708.82f, 6.97f, 707.33f, 713.80f, 1, 32, "\xC2\xB5"},
            {260.04f, 265.74f, 710.26f, 9.96f, 707.77f, 717.73f, 1, 32, "u"},
            {268.56f, 271.32f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, ":"},
            {271.32f, 279.06f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "="},
            {281.88f, 287.41f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "d"},
            {287.40f, 290.16f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "i"},
            {290.16f, 295.41f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "v"},
            {295.56f, 300.40f, 708.82f, 6.97f, 707.33f, 713.80f, 1, 32, "\xC2\xB5"},
            {300.84f, 304.70f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "("},
            {304.68f, 310.49f, 710.26f, 9.96f, 707.77f, 717.73f, 1, 32, "V"},
            {312.84f, 316.70f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "("},
            {316.68f, 322.38f, 710.26f, 9.96f, 707.77f, 717.73f, 1, 32, "u"},
            {322.44f, 326.30f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, ")"},
            {326.28f, 334.52f, 710.26f, 9.96f, 707.77f, 717.73f, 1, 32, "D"},
            {334.80f, 340.50f, 710.26f, 9.96f, 707.77f, 717.73f, 1, 32, "u"},
            {340.56f, 344.42f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, ")"},
            {347.16f, 354.90f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "="},
            {357.60f, 362.58f, 710.26f, 9.96f, 707.77f, 717.73f, 0, 0, "0"},
            {362.64f, 365.40f, 710.26f, 9.96f, 707.77f, 717.73f, 1, 32, ","},
            {248.16f, 253.14f, 712.78f, 9.96f, 710.29f, 720.25f, 0, kMathLineStart, "\xCB\x9C"},
        };
        Expect(LatexOf(InputOf(kGlyphs, sizeof(kGlyphs) / sizeof(kGlyphs[0]))),
               "\\tilde{\\Delta}_{\\mu}u:=\\mathrm{div}_{\\mu}(V(u)Du)=0,", 2220);
    }
    {
        // 40 letters strewn at random: no arborescence of cheap edges spans them.
        Formula f;
        uint32_t seed = 12345;
        const auto next = [&]() {
            seed = seed * 1103515245u + 12345u;
            return static_cast<float>((seed >> 8) & 0xFFFF) / 65536.0f;
        };
        for (int i = 0; i < 40; ++i) {
            const char letter[2] = {static_cast<char>('a' + i % 26), 0};
            f.Add(letter, 100 * next(), 100 * next(), 6 + 6 * next());
        }
        std::string out, tag;
        RAYOPDF_CHECK(BuildLatexTree(f.in, false, out, tag) == MathStatus::Unsure && out.empty(), 2221);
    }
    {
        // y = a²/b + √x through the tree itself: a bar's numerator and denominator, a radical.
        Formula f;
        float u = f.Add("y", -12, 0);
        f.Add("=", u, 0, 10, kText);
        u = f.Add("a", 1.5f, 6);
        f.Add("2", u, 10, 7, kText);
        f.Add("b", 3, -6);
        f.Rule(0, 11, 2.4f, 2.8f);
        f.Add("+", 12, 0, 10, kText);
        f.Add("\xE2\x88\x9A", 18, 8, 10, kSymbol, 0, 0.8f);
        f.Rule(26, 34, 8, 8.4f);
        f.Add("x", 28, 0);
        std::string out, tag;
        const MathStatus s = BuildLatexTree(f.in, false, out, tag);
        Expect(s == MathStatus::Ok ? out : "<fail>", "y=\\frac{a^{2}}{b}+\\sqrt{x}", 2222);
    }
}

// The arborescence in the writer: a formula of stacked accents inside a sentence is rebuilt; the
// same formula as a paragraph's whole text (a display the lines did not mark) keeps the greedy
// rebuild's result, Unicode text.
void TestTreeAlone() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic};
    // A hat over a dot over q at (x, y), as the text layer of 2503.07449 page 17 gives them: the
    // accents on q's baseline.
    const auto accented = [&](float x, float y) {
        static constexpr const char* kAccents[] = {"\xCB\x86", "\xCB\x99"};
        for (int i = 0; i < 2; ++i) {
            Glyph g;
            g.u0 = x + 0.7f + i, g.u1 = x + 5.3f - i, g.vb = y - 1.8f, g.vt = y + 6.2f, g.v = y;
            g.size = 9;
            g.text = static_cast<uint32_t>(page.text.size());
            page.text.append(kAccents[i]);
            g.textLength = 2;
            page.glyphs.push_back(g);
        }
        return AddText(page, "q", x, y, 9, 1);
    };
    float x = AddText(page, "Let ", 72, 700, 9);
    x = accented(x, 700);
    AddText(page, " be the rate of the flow through the pipe.", x, 700, 9);
    x = accented(72, 670);
    x = AddText(page, " = ", x, 670, 9);
    AddText(page, "a", x, 670, 9, 1);
    AddText(page, "And so the text goes on after the formula.", 72, 640, 9);
    const std::string latex = Convert({page}, MathMode::Latex);
    if (latex.find("Let $\\hat{\\dot{q}}$ be the rate") == std::string::npos || latex.find("\\hat{\\dot{q}}=a") != std::string::npos)
        std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(latex.find("Let $\\hat{\\dot{q}}$ be the rate") != std::string::npos, 2223);
    RAYOPDF_CHECK(latex.find("\\hat{\\dot{q}}=a") == std::string::npos, 2224);
}

// mathabx's Type 1 fonts name their glyphs as T1 text (code 0x70 "(" is "p", 0xA4 "≤" is "ď"):
// --math=latex reads a glyph whose text is its code's T1 character by mathabx's encoding, and
// keeps any other text; the default output keeps the text.
void TestMathabx() {
    RAYOPDF_CHECK(MathFontByte("TeX-matha10") == (kSymbol | kMathFontAbxA) &&
                      MathFontByte("TeX-mathx10") == (kExtension | kMathFontAbxX) &&
                      MathFontByte("TeX-mathb8") == (kSymbol | kMathFontAbxB) && MathFontByte("CMSY10") == kSymbol,
                  2203);
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic, static_cast<uint8_t>(kSymbol | kMathFontAbxA)};
    const auto abx = [&page](std::string_view text, uint8_t code, float x) {
        page.mathCodes.push_back(MathCode{static_cast<uint32_t>(page.glyphs.size()), code});
        return AddText(page, text, x, 700, 10, 2);
    };
    float x = AddText(page, "Let ", 72, 700, 10);
    x = AddText(page, "f", x, 700, 10, 1);
    x = abx("p", 0x70, x);
    x = AddText(page, "y", x, 700, 10, 1);
    x = abx("q", 0x71, x);
    x = abx("\xC4\x8F", 0xA4, x);  // ď
    x = AddText(page, "g", x, 700, 10, 1);
    x = abx("x", 0x50, x);  // a text other than the code's T1 character stays
    AddText(page, " hold.", x, 700, 10);
    const std::string latex = Convert({page}, MathMode::Latex);
    const std::string unicode = Convert({page}, MathMode::Unicode);
    if (latex.find("$f(y)\\leq gx$") == std::string::npos) std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(latex.find("$f(y)\\leq gx$") != std::string::npos, 2204);
    RAYOPDF_CHECK(unicode.find("Let fpyq\xC4\x8Fgx hold.") != std::string::npos, 2213);
}

// A ToUnicode built on the Adobe Glyph List reads TeX's \phi (code 0x1E, "phi") as φ and \varphi
// (0x27, "phi1") as ϕ: --math=latex takes the form from the code, and keeps the text for a code of
// another letter.
void TestGreekVariants() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic};
    const auto greek = [&page](std::string_view text, uint8_t code, float x) {
        page.mathCodes.push_back(MathCode{static_cast<uint32_t>(page.glyphs.size()), code});
        return AddText(page, text, x, 700, 10, 1);
    };
    float x = AddText(page, "Let ", 72, 700, 10);
    x = greek("\xCF\x86", 0x1E, x);  // φ at \phi's code
    x = AddText(page, ", ", x, 700, 10);
    x = greek("\xCF\x95", 0x27, x);  // ϕ at \varphi's code
    x = AddText(page, ", ", x, 700, 10);
    x = greek("\xCE\xB5", 0x0F, x);  // ε at \epsilon's code
    x = AddText(page, " and ", x, 700, 10);
    x = greek("\xCF\x86", 0x41, x);  // φ at the code of A
    AddText(page, " hold.", x, 700, 10);
    const std::string latex = Convert({page}, MathMode::Latex);
    const char* want = "Let $\\phi,\\varphi,\\epsilon$ and $\\varphi$ hold.";
    if (latex.find(want) == std::string::npos) std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(latex.find(want) != std::string::npos, 2214);
    // A font read through glyph names keeps the swap of their Adobe Glyph List text.
    page.fontMath[1] = static_cast<uint8_t>(kItalic | kMathFontNamed);
    const std::string named = Convert({page}, MathMode::Latex);
    RAYOPDF_CHECK(named.find("Let $\\phi,\\varphi,\\epsilon$ and $\\phi$ hold.") != std::string::npos, 2225);
}

// newtx's and pxfonts' AMS-b fonts (txsyb, pxsyb: msbm's encoding, blackboard capitals) and
// double-struck alphabets are blackboard fonts, not the symbol fonts whose capitals are
// calligraphic.
void TestBlackboardFonts() {
    const uint8_t bb = static_cast<uint8_t>(MathFont::Blackboard);
    RAYOPDF_CHECK(MathFontByte("txsyb") == bb && MathFontByte("pxsyb") == bb &&
                      MathFontByte("BoondoxDoubleStruck-Regular") == bb && MathFontByte("txsys") == kSymbol &&
                      MathFontByte("txsya") == kSymbol,
                  2235);
    Formula f;
    const float u = f.Add("R", 0, 0, 10, kBlackboard);
    f.Add("d", u, 4, 7);
    Expect(f.Latex(), "\\mathbb{R}^{d}", 2236);
}

// A script's own script out of content order (Z^{W^{l}}): the lone fragment l joins W's fragment,
// which then joins Z's line; l goes along with it instead of being lost.
void TestScriptChain() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic};
    float x = AddText(page, "Let ", 72, 700, 10);
    const float z = AddText(page, "Z", x, 700, 10.9f, 1);
    AddText(page, " be open.", z + 18, 700, 10);
    // l drawn before W, as a fragment of its own.
    AddText(page, "l", z + 5.5f, 707.3f, 6, 1);
    AddText(page, "W", z + 0.5f, 704.5f, 8, 1);
    const std::string latex = Convert({page}, MathMode::Latex);
    const char* want = "$Z^{W^{l}}$";
    if (latex.find(want) == std::string::npos) std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(latex.find(want) != std::string::npos, 2237);
}

// A display formula holding a word ("a = b and c = d"), centred apart from the text around it,
// is a display.
void TestDisplayWithWord() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic};
    AddText(page, "We assume for the rest of this section that", 72, 700, 10);
    float x = AddText(page, "a", 220, 680, 10, 1);
    x = AddText(page, "=", x + 3, 680, 10);
    x = AddText(page, "b", x + 3, 680, 10, 1);
    x = AddText(page, " and ", x, 680, 10);
    x = AddText(page, "c", x, 680, 10, 1);
    x = AddText(page, "=", x + 3, 680, 10);
    AddText(page, "d", x + 3, 680, 10, 1);
    AddText(page, "hold for every pair of the families we study here.", 72, 660, 10);
    const std::string latex = Convert({page}, MathMode::Latex);
    const bool display = latex.find("\n$$a=b") != std::string::npos && latex.find("c=d$$\n") != std::string::npos;
    if (!display) std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(display, 2240);
}

// TeX's math fonts without ToUnicode, through codes the WinAnsi guess reads wrong (the
// interpreter keeps their codes in PageData::mathCodes): cmex10's \Bigg( at 0x20 (a space) and
// \Bigg) at 0x21 ("!"), cmsy10's \mapstochar (0x37, "7", no width) at the start of its arrow
// (0x21), cmmi10's \lhook (0x2C, ",") before an arrow with \joinrel's overlap. --math=latex
// reads them as \Biggl(, \mapsto and \hookrightarrow.
void TestTexCodes() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic, kSymbol, kExtension};
    const auto code = [&page](std::string_view text, uint8_t value, float x, float y, uint16_t font) {
        page.mathCodes.push_back(MathCode{static_cast<uint32_t>(page.glyphs.size()), value});
        const float end = AddText(page, text, x, y, 10, font);
        page.glyphs.back().flags = static_cast<uint16_t>(page.glyphs.back().flags | kGlyphGuessed);
        return end;
    };
    float x = AddText(page, "Let ", 72, 700, 10);
    x = AddText(page, "x", x, 700, 10, 1);
    x = AddText(page, "=", x, 700, 10);
    x = code(" ", 0x20, x, 717, 3);  // \Bigg( hangs from its top
    x = AddText(page, "a", x, 700, 10, 1);
    x = code("!", 0x21, x, 717, 3);
    x = AddText(page, " hold, and let ", x, 700, 10);
    x = AddText(page, "x", x, 700, 10, 1);
    code("7", 0x37, x, 700, 2);
    page.glyphs.back().u1 = x;  // \mapstochar has no width
    x = code("!", 0x21, x, 700, 2);
    x = AddText(page, "y", x, 700, 10, 1);
    x = AddText(page, " be the map, and ", x, 700, 10);
    x = AddText(page, "x", x, 700, 10, 1);
    x = code(",", 0x2C, x, 700, 1);
    x = code("!", 0x21, x - 1.67f, 700, 2);
    x = AddText(page, "y", x, 700, 10, 1);
    AddText(page, " an embedding.", x, 700, 10);
    const std::string md = Convert({page}, MathMode::Latex);
    const bool bigg = md.find("\\Biggl(a\\Biggr)") != std::string::npos;
    const bool arrows = md.find("$x\\mapsto y$") != std::string::npos && md.find("$x\\hookrightarrow y$") != std::string::npos;
    if (!bigg || !arrows) std::fprintf(stderr, "latex page:\n%s\n", md.c_str());
    RAYOPDF_CHECK(bigg, 2241);
    RAYOPDF_CHECK(arrows, 2242);
}

// TeX's extension font draws \Big| as three bar pieces, one under the other: one tall bar, which
// takes the subscript after it.
void TestBarPieces() {
    Formula f;
    const float u = f.Add("f", 0, 0);
    for (const float v : {6.0f, 0.0f, -6.0f}) f.Add("\xE2\x88\xA3", u, v, 10, kExtension, 0, 0.33f);  // ∣
    f.Add("x", u + 3.3f, -4, 7);
    Expect(f.Latex(), "f\\Big|_{x}", 2243);
}

// Relations TeX sets from two glyphs: \not\in (the negation slash before ∈), \notin (a slash
// over ∈, drawn after it), \cong (a ∼ over an =), \longmapsto (\mapstochar, no width, at the
// start of a long arrow's bar).
void TestComposedRelations() {
    {
        Formula f;
        float u = f.Add("x", 0, 0);
        f.Add("\xCC\xB8", u, 0, 10, kSymbol, 0, 0.0f);  // U+0338
        u = f.Add("\xE2\x88\x88", u, 0, 10, kSymbol, 0, 0.67f);  // ∈
        u = f.Add("A", u + 2.8f, 0);
        u = f.Add(",", u, 0, 10, kText, 0, 0.28f);
        u = f.Add("y", u + 1.7f, 0);
        f.Add("/", u + 2.9f, 0, 10, kItalic, 0, 0.5f);
        u = f.Add("\xE2\x88\x88", u + 2.8f, 0, 10, kSymbol, 0, 0.67f);
        f.Add("B", u + 2.8f, 0);
        Expect(f.Latex(), "x\\not\\in A,y\\notin B", 2245);
    }
    {
        Formula f;
        float u = f.Add("A", 0, 0);
        f.Add("\xE2\x88\xBC", u + 2.8f, 3, 10, kSymbol, 0, 0.78f);  // ∼ raised over the =
        u = f.Add("=", u + 2.8f, 0, 10, kText, 0, 0.78f);
        f.Add("B", u + 2.8f, 0);
        Expect(f.Latex(), "A\\cong B", 2244);
    }
    {
        Formula f;
        float u = f.Add("x", 0, 0);
        f.Add("\xE2\x86\xA6", u + 2.8f, 0, 10, kSymbol, 0, 0.0f);  // \mapstochar read as ↦
        f.Add("\xE2\x88\x92", u + 2.8f, 0, 10, kSymbol, 0, 0.78f);
        u = f.Add("\xE2\x86\x92", u + 8.8f, 0, 10, kSymbol, 0, 1.0f);
        f.Add("y", u + 2.8f, 0);
        Expect(f.Latex(), "x\\longmapsto y", 2239);
    }
}

// A ∼ drawn on a line of its own over the = of a line (\cong) joins that line.
void TestStackedTilde() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic, kSymbol};
    float x = AddText(page, "Then ", 72, 700, 10);
    x = AddText(page, "A", x, 700, 10, 1);
    const float equals = x + 2;
    x = AddText(page, "=", equals, 700, 10);
    x = AddText(page, "B", x + 2, 700, 10, 1);
    AddText(page, " holds.", x, 700, 10);
    AddText(page, "\xE2\x88\xBC", equals, 703.5f, 10, 2);
    const std::string latex = Convert({page}, MathMode::Latex);
    if (latex.find("$A\\cong B$") == std::string::npos) std::fprintf(stderr, "latex page:\n%s\n", latex.c_str());
    RAYOPDF_CHECK(latex.find("$A\\cong B$") != std::string::npos, 2246);
}

// amsmath's \xrightarrow: bars overlapping up to an arrowhead, with a smaller label centred over
// them.
void TestExtensibleArrow() {
    Formula f;
    const float u = f.Add("a", 0, 0);
    f.Add("\xE2\x88\x92", u + 1, 0, 10, kSymbol, 0, 0.78f);
    f.Add("\xE2\x88\x92", u + 5, 0, 10, kSymbol, 0, 0.78f);
    f.Add("\xE2\x86\x92", u + 9, 0, 10, kSymbol, 0, 1.0f);
    f.Add("f", u + 8, 5, 7);
    f.Add("b", u + 21, 0);
    Expect(f.Latex(), "a\\xrightarrow{f}b", 2247);
}

// Letters an inline formula takes: an italic letter of a text font alone in a word with math
// ("α(v)"), and a bold letter alone after a trailing relation ("T ≃ N").
void TestInlineLetters() {
    std::vector<Glyph> store;
    std::vector<uint32_t> run = Runs("Let \xCE\xB1(v) hold", "...m.i.....", store);
    RAYOPDF_CHECK(run[3] == 7, 2248);
    run = Runs("so T \xE2\x89\x83 N here", "..mmb....", store);
    RAYOPDF_CHECK(run[2] == 5, 2234);
}

// Fonts whose texts do not follow their TeX codes (kMathFontCoded): the Old Arrows font
// OAMathSymbols under a WinAnsi /Encoding (cmsy10's → at 0x21 reads "!", \mapstochar at 0x37
// "7"), and newtx's txexs, whose large operators past 0x7F its ToUnicode leaves to the WinAnsi
// guess (∏ at 0xD6 reads "Ö") and gives others a variation selector (∑ as U+2211 U+FE01).
void TestCodedFonts() {
    const uint8_t arrows = static_cast<uint8_t>(kSymbol | kMathFontCoded), ex = static_cast<uint8_t>(kExtension | kMathFontCoded);
    RAYOPDF_CHECK(MathFontByte("OAMathSymbols10") == arrows && MathFontByte("txexs") == ex &&
                      MathFontByte("txex") == kExtension && MathFontByte("TeX-matha10") == (kSymbol | kMathFontAbxA),
                  2249);
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic, arrows, ex};
    const auto code = [&page](std::string_view text, uint8_t value, float x, float y, uint16_t font, bool guessed) {
        page.mathCodes.push_back(MathCode{static_cast<uint32_t>(page.glyphs.size()), value});
        const float end = AddText(page, text, x, y, 10, font);
        if (guessed) page.glyphs.back().flags = static_cast<uint16_t>(page.glyphs.back().flags | kGlyphGuessed);
        return end;
    };
    float x = AddText(page, "Let ", 72, 700, 10);
    x = AddText(page, "c", x, 700, 10, 1);
    x = AddText(page, ":", x, 700, 10);
    x = AddText(page, "X", x + 2, 700, 10, 1);
    x = code("!", 0x21, x + 2, 700, 2, false);
    x = AddText(page, "S", x + 2, 700, 10, 1);
    x = AddText(page, " and ", x, 700, 10);
    x = AddText(page, "x", x, 700, 10, 1);
    code("7", 0x37, x + 2, 700, 2, false);
    page.glyphs.back().u1 = x + 2;  // \mapstochar has no width
    x = code("!", 0x21, x + 2, 700, 2, false);
    x = AddText(page, "y", x + 2, 700, 10, 1);
    x = AddText(page, " be maps, ", x, 700, 10);
    x = AddText(page, "p", x, 700, 10, 1);
    x = AddText(page, "=", x + 2, 700, 10);
    x = code("\xC3\x96", 0xD6, x + 2, 709, 3, true);  // Ö: ∏ hangs from its top
    x = AddText(page, "a", x + 1, 700, 10, 1);
    x = AddText(page, " and ", x, 700, 10);
    x = AddText(page, "q", x, 700, 10, 1);
    x = AddText(page, "=", x + 2, 700, 10);
    x = code("\xE2\x88\x91", 0xD5, x + 2, 709, 3, false);
    page.text.append("\xEF\xB8\x81");  // its text ∑ U+FE01, in one glyph
    page.glyphs.back().textLength = static_cast<uint16_t>(page.glyphs.back().textLength + 3);
    x = AddText(page, "b", x + 1, 700, 10, 1);
    AddText(page, " hold.", x, 700, 10);
    const std::string md = Convert({page}, MathMode::Latex);
    const bool oa = md.find("$c:X\\rightarrow S$") != std::string::npos && md.find("$x\\mapsto y$") != std::string::npos;
    const bool tx = md.find("$p=\\prod a$") != std::string::npos && md.find("$q=\\sum b$") != std::string::npos;
    if (!oa || !tx) std::fprintf(stderr, "latex page:\n%s\n", md.c_str());
    RAYOPDF_CHECK(oa, 2350);
    RAYOPDF_CHECK(tx, 2351);
}

// A display formula holding a word ("F = ∑ a and G = ∑ b") whose operators' limits are lines of
// their own, next to it in reading order: it is set off from the text around them, not from its
// limits, and is one display.
void TestDisplayWordLimits() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    page.fontMath = {kText, kItalic, kSymbol, kExtension};
    AddText(page, "The two polynomials of this section are written in the form", 72, 720, 10);
    float x = AddText(page, "F", 180, 690, 10, 1);
    x = AddText(page, "=", x + 3, 690, 10);
    const float sum1 = x + 3;
    x = AddText(page, "\xE2\x88\x91", sum1, 697, 10, 3);  // ∑ hangs from its top
    x = AddText(page, "a", x + 1, 690, 10, 1);
    x = AddText(page, " and ", x, 690, 10);
    x = AddText(page, "G", x, 690, 10, 1);
    x = AddText(page, "=", x + 3, 690, 10);
    const float sum2 = x + 3;
    x = AddText(page, "\xE2\x88\x91", sum2, 697, 10, 3);
    AddText(page, "b", x + 1, 690, 10, 1);
    AddText(page, "m", sum1 + 1, 702, 7, 1);
    AddText(page, "n", sum2 + 1, 702, 7, 1);
    AddText(page, "k=0", sum1 - 2, 682, 7, 1);
    AddText(page, "k=0", sum2 - 2, 682, 7, 1);
    AddText(page, "where the coefficients are numbers of the field we work over.", 72, 660, 10);
    const std::string md = Convert({page}, MathMode::Latex);
    const bool display = md.find("$$F=\\sum_{k=0}^{m}a") != std::string::npos && md.find("G=\\sum_{k=0}^{n}b$$") != std::string::npos;
    if (!display) std::fprintf(stderr, "latex page:\n%s\n", md.c_str());
    RAYOPDF_CHECK(display, 2352);
}

// Fractions set one over another (the rows of cases): a bar about as long as this one is another
// fraction's, whose parts are its own; a numerator stops where its ink leaves a gap (a comma of the
// row above). A radical's vinculum is the highest rule at its sign, a fraction under it starts
// there too; the rules of an inline formula reach up to a tall sign's ink.
void TestStackedFractions() {
    {
        Formula f;
        f.Add("x", -20, 4);  // a glyph at text size beside them: the bars stand 1.4 em apart
        f.Add("1", 0, 15.5f, 7, kText);
        f.Add("2", 0, 8, 7, kText);
        f.Rule(0, 4.3f, 13.8f, 14.2f);
        f.Add("1", 0, 1.5f, 7, kText);
        f.Add("k", 0, -6, 7);
        f.Rule(0, 4.6f, -0.2f, 0.2f);
        const std::string latex = f.Latex();
        if (latex.find("\\frac{1}{2}") == std::string::npos || latex.find("\\frac{1}{k}") == std::string::npos)
            std::fprintf(stderr, "code 2367: got \"%s\"\n", latex.c_str());
        RAYOPDF_CHECK(latex.find("\\frac{1}{2}") != std::string::npos && latex.find("\\frac{1}{k}") != std::string::npos, 2367);
    }
    {
        Formula f;
        f.Add("1", 0, 2, 7, kText);
        f.Add("2", 0, -6, 7, kText);
        f.Rule(0, 4, -0.2f, 0.2f);
        f.Add(",", 1, 15, 10, kItalic);
        const std::string latex = f.Latex();
        // The comma stays out of the numerator (it was \frac{1,}{2}).
        if (latex.find("\\frac{1}{2}") == std::string::npos) std::fprintf(stderr, "code 2368: got \"%s\"\n", latex.c_str());
        RAYOPDF_CHECK(latex.find("\\frac{1}{2}") != std::string::npos, 2368);
    }
    {
        // √ ink from -8 to 18.5; the fraction's bar starts 0.5 pt after the sign, the vinculum 0.9 pt.
        Formula f;
        f.Add("\xE2\x88\x9A", 0, 0, 10, kSymbol, 0, 0.8f);
        f.in.glyphs.back().bottom = -8, f.in.glyphs.back().top = 18.5f;
        f.Rule(8.9f, 22, 18, 18.4f);
        f.Rule(8.5f, 20, 2.4f, 2.8f);
        f.Add("a", 12, 8);
        f.Add("b", 12, -6);
        Expect(f.Latex(), "\\sqrt{\\frac{a}{b}}", 2369);
    }
    {
        PageData page;
        page.rulings.push_back(Ruling{10, 20.4f, 30, 20.8f, kRulingFill});
        MathInput in;
        MathGlyph g;
        g.u0 = 0, g.u1 = 10, g.v = 0, g.size = 10, g.bottom = -8, g.top = 20.6f;
        in.glyphs.push_back(g);
        AddMathRules(page, 0, 30, -4, 9, in);
        RAYOPDF_CHECK(in.rules.size() == 1, 2370);
    }
}

// Readings that depend on the font or the spacing: cmsy10's ℜ is \Re; lim inf and arg max with
// limits under both words are one operator; periods typed one after the other are "...", not
// \ldots; a backslash touching its neighbours is \backslash, not \setminus; a display's
// text-style fraction is \tfrac.
void TestSpacedSymbols() {
    {
        Formula f;
        float u = f.Add("\xE2\x84\x9C", 0, 0, 10, kSymbol);
        u = f.Add("(", u, 0, 10, kText);
        u = f.Add("z", u, 0);
        f.Add(")", u, 0, 10, kText);
        Expect(f.Latex(), "\\Re(z)", 2371);
    }
    for (const char* word : {"inf", "max"}) {
        Formula f;
        f.Add(word[0] == 'i' ? "lim" : "arg", 0, 0, 10, kText);
        const float u = f.Add(word, 18, 0, 10, kText);
        f.Add("k=1", 11.25f, -9, 7, kText);
        f.Add("a", u + 2, 0);
        Expect(f.Latex(), word[0] == 'i' ? "\\liminf_{k=1}a" : "\\operatorname*{argmax}_{k=1}a", word[0] == 'i' ? 2373 : 2374);
    }
    {
        Formula f;
        float u = f.Add("x,", 0, 0);
        u = f.Add("...", u, 0, 10, kItalic, 0, 0.28f);
        f.Add(",y", u, 0);
        Expect(f.Latex(), "x,...,y", 2375);
    }
    {
        Formula g;
        float u = g.Add("A", 0, 0);
        u = g.Add("\\", u, 0, 10, kSymbol);
        g.Add("B", u, 0);
        Expect(g.Latex(), "A\\backslash B", 2379);
    }
    {
        // x = 1/2 with its parts at 7 pt in a display: \tfrac.
        Formula f;
        float u = f.Add("x", 0, 0);
        f.Add("=", u + 2, 0, 10, kText);
        f.Add("1", 15, 4.5f, 7, kText);
        f.Add("2", 15, -3.5f, 7, kText);
        f.Rule(14, 19, 2.3f, 2.7f);
        Expect(f.Latex(true), "x=\\tfrac{1}{2}", 2380);
    }
}

// Rows whose neighbours were misread: a text-style fraction after a subscript is the subscript's
// sibling; \not's slash, which has no width, leaves no gap after \neq; a radical set small is a
// script; the limits of one large operator stop short of the next's; Symbol's φ is \phi, MnSymbol's
// ○ is \circ; a row's first glyph raised over a wider one of its size is the base, not a prescript.
void TestRowNeighbours() {
    {
        Formula f;
        float u = f.Add("c", 0, 0);
        u = f.Add("i", u, -1.5f, 7);
        f.Add("t", u + 1, 4.5f, 7);
        f.Add("2", u + 1, -3.5f, 7, kText);
        f.Rule(u + 0.5f, u + 5, 2.3f, 2.7f);
        Expect(f.Latex(), "c_{i}\\frac{t}{2}", 2372);
    }
    {
        Formula f;
        float u = f.Add("i", 0, 0);
        const float eq = u + 2;
        u = f.Add("=", eq, 0, 10, kText, 0, 0.78f);
        f.Add("\xCC\xB8", eq + 0.5f, 0, 10, kSymbol, 0, 0);  // \not: no width, drawn over the =
        f.Add("j", u + 2.8f, 0);
        Expect(f.Latex(), "i\\neq j", 2376);
    }
    {
        // e^{\sqrt{x}}: cmsy7's √ hanging from its origin, raised with its radicand.
        Formula f;
        f.Add("e", 0, 0);
        f.Add("\xE2\x88\x9A", 5.2f, 9.6f, 7, kSymbol, 0, 0.8f);
        f.Rule(10.8f, 15, 9.6f, 9.9f);
        f.Add("x", 11, 4, 7);
        Expect(f.Latex(), "e^{\\sqrt{x}}", 2377);
    }
    {
        Formula f;
        f.Add("X", 0, 9, 10, kExtension, kMathGuessed, 1.4f);
        float u = f.Add("n", 2.5f, -12, 7);
        u = f.Add("=", u, -12, 7, kText);
        f.Add("1", u, -12, 7, kText);
        f.Add("N", 5.25f, 12, 7);
        f.Add("X", 14.5f, 9, 10, kExtension, kMathGuessed, 1.4f);
        u = f.Add("m", 16.25f, -12, 7);
        u = f.Add("=", u, -12, 7, kText);
        f.Add("1", u, -12, 7, kText);
        f.Add("N", 19.75f, 12, 7);
        f.Add("x", 30, 0);
        Expect(f.Latex(), "\\sum_{n=1}^{N}\\sum_{m=1}^{N}x", 2378);
    }
    {
        RAYOPDF_CHECK(MathFontByte("StandardSymL-Slant_167") == kMathFontNamed && MathFontByte("Symbol") == kMathFontNamed &&
                          MathFontByte("Times-Roman") == 0,
                      2381);
        Formula f;
        float u = f.Add("\xCF\x86", 0, 0, 10, kText, kMathNamed);  // φ of a Symbol font
        u = f.Add(",", u, 0, 10, kText);
        f.Add("\xCF\x95", u, 0, 10, kText, kMathNamed);  // ϕ
        Expect(f.Latex(), "\\phi,\\varphi", 2382);
        Formula g;
        u = g.Add("K", 0, 0);
        g.Add("\xE2\x97\x8B", u, 4, 7, kSymbol);  // MnSymbol's ○
        Expect(g.Latex(), "K^{\\circ}", 2383);
    }
    {
        // r_{0} at scriptscript size: the 0 as large as the r and wider, dropped 0.32 em.
        Formula f;
        const float u = f.Add("r", 0, 0, 5);
        f.Add("0", u, -1.6f, 5, kText, 0, 0.7f);
        Expect(f.Latex(), "r_{0}", 2384);
    }
}
} // namespace

int main() {
    TestScripts();
    TestFractions();
    TestRadicals();
    TestOperators();
    TestMatrices();
    TestSymbols();
    TestDisplay();
    TestFallback();
    TestFonts();
    TestInline();
    TestNumbersText();
    TestPages();
    TestConstructs();
    TestNestedScripts();
    TestTree();
    TestTreeAlone();
    TestMathabx();
    TestGreekVariants();
    TestBlackboardFonts();
    TestScriptChain();
    TestDisplayWithWord();
    TestTexCodes();
    TestBarPieces();
    TestComposedRelations();
    TestStackedTilde();
    TestExtensibleArrow();
    TestInlineLetters();
    TestCodedFonts();
    TestDisplayWordLimits();
    TestStackedFractions();
    TestSpacedSymbols();
    TestRowNeighbours();
    std::puts("test_layout_math: ok");
    return 0;
}
