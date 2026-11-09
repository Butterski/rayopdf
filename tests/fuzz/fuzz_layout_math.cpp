// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the LaTeX math output (layout/math.h). The input is decoded into a formula of
// glyphs from a small alphabet of letters, digits, operators, TeX font guesses, delimiter pieces,
// radicals, accents and mathematical alphanumerics, placed on a coarse grid (so that baselines,
// scripts, limits and rows line up often) or at raw float bit patterns, with rules among them;
// BuildLatex rebuilds it inline and displayed, FindInlineFormulas reads its glyphs as a run of
// words. A second part lays the same glyphs out as a page and runs the stages after
// interpretation in LaTeX mode: MarkDisplayFormulas, semantics and the writer.
#include "layout/math.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace RayoPdf::Layout;

namespace {

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t at = 0;
    bool Empty() const { return at >= size; }
    uint8_t Byte() { return at < size ? data[at++] : 0; }
    // A coordinate: mostly on a 1 pt grid near the origin, sometimes any float bit pattern.
    float Coord() {
        const uint8_t mode = Byte();
        if (mode < 248) return float(static_cast<int8_t>(Byte())) + float(mode % 4) * 0.25f;
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) bits = bits << 8 | Byte();
        float f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    }
};

// Glyph texts: letters, digits, operators, brackets, guesses read through TeX codes, pieces,
// radicals, accents, primes, alphanumerics, letterlike symbols, private use, a ligature.
const char* const kTexts[] = {
    "x", "y", "n", "A", "R", "1", "2", "=", "+", "-", "(", ")", "[", "]", "{", "}", "|", ",", ".", "X", "Z", "P", "k",
    "h", "i", "b", "e", "p", "!", "\xE2\x88\x91", "\xE2\x88\xAB", "\xE2\x88\x9A", "\xE2\x8E\x9B", "\xE2\x8E\x9C",
    "\xE2\x8E\x9D", "\xE2\x8E\x9E", "\xE2\x8E\xA0", "\xE2\x8E\xA7", "\xE2\x8E\xA8", "\xE2\x8E\xA9", "\xE2\x8E\xAA",
    "\xCB\x86", "\xCB\x9C", "\xC2\xAF", "\xE2\x83\x97", "\xE2\x80\xB2", "\xF0\x9D\x90\x80", "\xF0\x9D\x91\xA5",
    "\xE2\x84\x9D", "\xCE\xB1", "\xE2\x89\xA4", "\xE2\x86\x92", "\xEE\x80\x80", "fi", "sin", "lim", "x\xCC\x82",
    "(3)", "$", "\\", "_", "^", "~", "#", "%", "&", " ",
};
constexpr size_t kTextCount = sizeof kTexts / sizeof kTexts[0];

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1 << 14) return 0;
    Reader in{data, size};
    MathInput formula;
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    for (uint8_t f = 0; f <= static_cast<uint8_t>(MathFont::Unicode); ++f) page.fontMath.push_back(f);
    std::vector<Glyph> glyphs;
    while (!in.Empty() && formula.glyphs.size() < kMaxFormulaGlyphs + 8) {
        const uint8_t op = in.Byte();
        if (op % 8 == 7) {
            const float u0 = in.Coord(), u1 = in.Coord(), v = in.Coord(), w = float(in.Byte() % 4) * 0.2f;
            formula.rules.push_back(MathRule{u0, u1, v, v + w});
            page.rulings.push_back(Ruling{300 + u0, 400 + v, 300 + u1, 400 + v + w, kRulingFill});
            continue;
        }
        const std::string_view text = kTexts[in.Byte() % kTextCount];
        MathGlyph g;
        g.u0 = in.Coord();
        g.u1 = g.u0 + float(1 + in.Byte() % 12);
        g.v = in.Coord();
        g.size = float(4 + in.Byte() % 12);
        g.font = static_cast<uint8_t>(in.Byte() % (static_cast<uint8_t>(MathFont::Unicode) + 1));
        g.flags = static_cast<uint8_t>(in.Byte() & 0x1F);
        g.text = static_cast<uint32_t>(formula.text.size());
        g.length = static_cast<uint16_t>(text.size());
        formula.text.append(text);
        formula.glyphs.push_back(g);
        // The same glyph on a page, around (300, 400).
        Glyph pg;
        pg.u0 = pg.x0 = 300 + g.u0, pg.u1 = pg.x1 = 300 + g.u1;
        pg.v = 400 + g.v, pg.vb = pg.y0 = pg.v - 0.2f * g.size, pg.vt = pg.y1 = pg.v + 0.8f * g.size;
        pg.size = g.size;
        pg.font = g.font;
        pg.text = static_cast<uint32_t>(page.text.size());
        pg.textLength = static_cast<uint16_t>(text.size());
        page.text.append(text);
        if (g.flags & kMathItalic) pg.flags |= kGlyphItalic;
        if (g.flags & kMathGuessed) pg.flags |= kGlyphGuessed;
        if (text == " ") pg.flags |= kGlyphSpace;
        page.glyphs.push_back(pg);
    }
    std::string out, tag;
    BuildLatex(formula, false, out, tag);
    out.clear();
    BuildLatex(formula, true, out, tag);

    // The glyphs as words of a paragraph.
    std::vector<InlineGlyph> words;
    for (size_t i = 0; i < page.glyphs.size(); ++i) {
        InlineGlyph w;
        w.glyph = &page.glyphs[i];
        w.font = formula.glyphs[i].font;
        w.wordStart = i == 0 || (formula.glyphs[i].flags & kMathLineStart);
        w.lineStart = i == 0;
        w.script = (formula.glyphs[i].flags & kMathBold) != 0;
        w.note = (formula.glyphs[i].flags & kMathUnmapped) != 0;
        w.text = page.GlyphText(page.glyphs[i]);
        words.push_back(w);
    }
    std::vector<uint32_t> run;
    FindInlineFormulas(words, run);

    // The page in LaTeX mode.
    std::vector<PageData> pages(1);
    pages[0] = std::move(page);
    std::string markdown;
    ConvertReport report;
    std::vector<PageTextQuality> quality(1);
    ConvertPages(pages, quality, {}, false, markdown, report, nullptr, nullptr, MathMode::Latex);
    return 0;
}
