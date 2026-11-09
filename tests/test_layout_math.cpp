// SPDX-License-Identifier: Apache-2.0
// Core tests of the LaTeX math output (layout/math.h, --math=latex): formulas rebuilt from glyphs
// placed by hand (scripts, fractions, radicals, operators with limits, matrices, cases, accents,
// alphabets, function names, TeX font codes, equation numbers, aligned rows, fallback), inline
// formula runs, and whole pages through ConvertPages in both math modes. Exit codes 1550-1599.
#include "check.h"

#include "common/utf8.h"
#include "layout/math.h"
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
// `text`: 'm' math italic, 'i' italic text font, anything else the upright text font; spaces start
// words.
std::vector<uint32_t> Runs(std::string_view text, std::string_view fonts, std::vector<Glyph>& store) {
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
            if (index < fonts.size() && fonts[index] == 'i') store[index].flags |= kGlyphItalic;
            g.wordStart = space || glyphs.empty();
            g.text = text.substr(at, length);
            glyphs.push_back(g);
            space = false;
        }
        at += length;
    }
    std::vector<uint32_t> run;
    FindInlineFormulas(glyphs, run);
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
    // "n-th": the letter alone; an italic letter of a text font alone starts no formula.
    run = Runs("n-th a b", "m...ii", store);
    RAYOPDF_CHECK(run[0] == 1 && run[4] == 0 && run[5] == 0, 1589);
    // "f(x) and (x)" keeps balanced brackets; "(see x)" leaves the unmatched one out.
    run = Runs("f(x) and (x)", "m.m.....m.", store);
    RAYOPDF_CHECK(run[0] == 4 && run[4] == 0 && run[7] == 10, 1590);
    run = Runs("(see x)", "....m.", store);
    RAYOPDF_CHECK(run[0] == 0 && run[4] == 5, 1596);
}

// A page: glyphs of `text` at (x, y) in font `font` (page-local: 0 text, 1 math italic).
float AddText(PageData& page, std::string_view text, float x, float y, float size, uint16_t font = 0) {
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        const float w = 0.5f * size;
        g.x0 = x, g.x1 = x + w, g.y0 = y - 0.2f * size, g.y1 = y + 0.8f * size;
        g.u0 = g.x0, g.u1 = g.x1, g.vb = g.y0, g.vt = g.y1, g.v = y;
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
    TestPages();
    std::puts("test_layout_math: ok");
    return 0;
}
