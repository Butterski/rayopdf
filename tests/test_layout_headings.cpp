// SPDX-License-Identifier: Apache-2.0
// Core tests of the heading rules of src/layout/semantics.cpp on scientific papers: the title
// above an outline that names the sections only, small capitals, numbering in the outline match,
// the standard unnumbered sections; and the kicker over a slide's title. Pages are built glyph by
// glyph and converted by the whole pipeline. Exit codes 1800-1849.
#include "check.h"

#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

// Appends `text` to `page` as glyphs of `size` points from (x, y), each half an em wide; a space
// is a space glyph. Returns the x after the text.
float AddText(PageData& page, std::string_view text, float x, float y, float size, uint16_t flags = 0) {
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        const float w = 0.5f * size;
        g.x0 = x, g.x1 = x + w, g.y0 = y - 0.2f * size, g.y1 = y + 0.8f * size;
        g.u0 = g.x0, g.u1 = g.x1, g.vb = g.y0, g.vt = g.y1, g.v = y;
        g.size = size;
        g.text = static_cast<uint32_t>(page.text.size());
        page.text.append(text.substr(at, length));
        g.textLength = static_cast<uint16_t>(length);
        g.flags = flags;
        if (cp == ' ') g.flags |= kGlyphSpace;
        page.glyphs.push_back(g);
        x += w;
        at += length;
    }
    return x;
}

PageData NewPage(int number) {
    PageData page;
    page.number = number;
    page.width = 612;
    page.height = 792;
    return page;
}

std::string Run(std::vector<PageData>& pages, const std::vector<OutlineEntry>& outline = {}) {
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, outline, false, md, report);
    return md;
}

bool Contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

void Show(const char* name, const std::string& md) {
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", name, md.c_str());
}

// Body lines from `y` down, 12 points apart.
void AddBody(PageData& page, float y, int count) {
    for (int i = 0; i < count; ++i)
        AddText(page, "Body text of the section that runs long enough to be a paragraph line.", 72, y - 12.0f * i, 10);
}

void TestTitleAboveOutline() {
    // LaTeX's bookmarks name the sections, not the title: the title, set larger on the first page
    // in a style of its own, is the top level and the outline's sections sit below it.
    std::vector<PageData> paper{NewPage(1)};
    AddText(paper[0], "A Paper About Headings", 150, 740, 17);
    AddText(paper[0], "1 Introduction", 72, 700, 14, kGlyphBold);
    AddBody(paper[0], 680, 3);
    AddText(paper[0], "1.1 Motivation", 72, 620, 12, kGlyphBold);
    AddBody(paper[0], 600, 3);
    AddText(paper[0], "2 Method", 72, 540, 14, kGlyphBold);
    AddBody(paper[0], 520, 3);
    const std::vector<OutlineEntry> outline{{"Introduction", 1}, {"Motivation", 2}, {"Method", 1}};
    std::string md = Run(paper, outline);
    Show("title above outline", md);
    RAYOPDF_CHECK(Contains(md, "# A Paper About Headings\n"), 1800);
    RAYOPDF_CHECK(Contains(md, "\n## 1 Introduction\n") && Contains(md, "\n## 2 Method\n"), 1801);
    RAYOPDF_CHECK(Contains(md, "\n### 1.1 Motivation\n"), 1802);
    // A first heading in the sections' own style is one of them (its bookmark is written another
    // way): it stays their sibling.
    std::vector<PageData> readme{NewPage(1)};
    AddText(readme[0], "Overview", 72, 740, 14, kGlyphBold);
    AddBody(readme[0], 720, 3);
    AddText(readme[0], "Install", 72, 660, 14, kGlyphBold);
    AddBody(readme[0], 640, 3);
    AddText(readme[0], "Usage", 72, 580, 14, kGlyphBold);
    AddBody(readme[0], 560, 3);
    md = Run(readme, {{"Install", 1}, {"Usage", 1}});
    Show("first heading in the sections' style", md);
    RAYOPDF_CHECK(Contains(md, "## Overview\n") && Contains(md, "\n## Install\n") && Contains(md, "\n## Usage\n"), 1803);
}

// Text set in small capitals made by scaling: a lower-case letter of `text` as its capital in
// `small` points, everything else in `large` points. Returns the x after the text.
float AddSmallCaps(PageData& page, std::string_view text, float x, float y, float large, float small,
                   uint16_t flags = 0) {
    for (const char c : text) {
        const bool lower = c >= 'a' && c <= 'z';
        const char upper = lower ? static_cast<char>(c - 0x20) : c;
        x = AddText(page, std::string_view(&upper, 1), x, y, lower ? small : large, flags);
    }
    return x;
}

void TestSmallCapitals() {
    // Sections in small capitals (capitals of two sizes on one baseline, the small ones in the
    // body's size or below): written in the source's case. The outline names them.
    std::vector<PageData> paper{NewPage(1)};
    AddSmallCaps(paper[0], "A Title In Small Capitals With A Bro-", 100, 740, 20, 16);
    AddSmallCaps(paper[0], "ken Word And CNN Layers", 150, 716, 20, 16);
    AddSmallCaps(paper[0], "I. Introduction", 72, 680, 10, 8);
    AddBody(paper[0], 660, 3);
    AddSmallCaps(paper[0], "II. Related Work on Headings", 72, 600, 10, 8);
    AddBody(paper[0], 580, 3);
    AddText(paper[0], "III. RESULTS", 72, 520, 10);
    AddBody(paper[0], 500, 3);
    const std::vector<OutlineEntry> outline{{"I Introduction", 1}, {"II Related Work on Headings", 1}, {"III Results", 1}};
    const std::string md = Run(paper, outline);
    Show("small capitals", md);
    RAYOPDF_CHECK(Contains(md, "# A Title In Small Capitals With A Broken Word And CNN Layers\n"), 1804);
    RAYOPDF_CHECK(Contains(md, "\n## I. Introduction\n") && Contains(md, "\n## II. Related Work on Headings\n"), 1805);
    // Capitals of one size are the text's own case.
    RAYOPDF_CHECK(Contains(md, "\n## III. RESULTS\n"), 1806);
}

void TestOutlineNumbering() {
    // The bookmark numbers a subsection "II-B", its line "B.": the same title. Set in italics in
    // the body's size and pitch right above its paragraph, it is still a block of its own. The
    // bookmark's hyphen may be another character than the line's.
    std::vector<PageData> paper{NewPage(1), NewPage(2)};
    AddText(paper[0], "Contents", 72, 680, 12, kGlyphBold);
    AddText(paper[0], "II. Related Work", 72, 656, 10);
    AddText(paper[0], "2", 520, 656, 10);
    AddText(paper[0], "II-B. GAN-based Predictions", 72, 644, 10);
    AddText(paper[0], "2", 520, 644, 10);
    AddBody(paper[1], 690, 2);
    AddText(paper[1], "II. Related Work", 72, 640, 10, kGlyphBold);
    AddBody(paper[1], 620, 3);
    AddText(paper[1], "B. GAN-based Predictions", 72, 572, 10, kGlyphItalic);
    AddBody(paper[1], 560, 3);
    AddText(paper[1], "C. Plain Subsection", 72, 512, 10);
    AddBody(paper[1], 500, 3);
    AddText(paper[1], "Appendix A. Proofs", 72, 440, 10, kGlyphBold);
    AddBody(paper[1], 420, 3);
    const std::vector<OutlineEntry> outline{{"II Related Work", 1},
                                            {"II-B GAN\xE2\x80\x91" "based Predictions", 2},
                                            {"II-C Plain Subsection", 2},
                                            {"Proofs", 1}};
    const std::string md = Run(paper, outline);
    Show("outline numbering", md);
    RAYOPDF_CHECK(Contains(md, "\n## II. Related Work\n"), 1807);
    RAYOPDF_CHECK(Contains(md, "\n### B. GAN-based Predictions\n\nBody text"), 1808);
    RAYOPDF_CHECK(Contains(md, "\n### C. Plain Subsection\n\nBody text"), 1811);
    RAYOPDF_CHECK(Contains(md, "\n## Appendix A. Proofs\n"), 1809);
    // The table of contents repeats the titles beside their page numbers: no headings.
    RAYOPDF_CHECK(!Contains(md, "# II-B") && Contains(md, "II-B. GAN-based Predictions"), 1810);
}

void TestStandardSections() {
    // "Abstract" alone on its line in the abstract's smaller size, "References" in the body's
    // face: a paper's standard sections, headings in any style.
    std::vector<PageData> paper{NewPage(1)};
    AddText(paper[0], "A Paper With An Abstract", 150, 690, 20);
    AddText(paper[0], "Abstract", 280, 650, 9, kGlyphBold);
    for (int i = 0; i < 3; ++i)
        AddText(paper[0], "Text of the abstract in a smaller size than the body of the paper.", 100, 636 - 11.0f * i, 9);
    AddText(paper[0], "1 Introduction", 72, 580, 14, kGlyphBold);
    AddBody(paper[0], 560, 6);
    AddText(paper[0], "References", 72, 470, 10);
    AddBody(paper[0], 450, 3);
    std::string md = Run(paper);
    Show("standard sections", md);
    RAYOPDF_CHECK(Contains(md, "# Abstract\n\nText of the abstract"), 1812);
    RAYOPDF_CHECK(Contains(md, "# References\n\nBody text"), 1813);
    RAYOPDF_CHECK(Contains(md, "# A Paper With An Abstract\n") && Contains(md, "\n## 1 Introduction\n"), 1814);
    // The label run in with the abstract ("Abstract—We show"): a heading, and the paragraph
    // starts after the label and its dash. A sentence that starts with the word is none, and so
    // is a label closed by a period ("Abstract. We show"), which typesets a paragraph's label.
    std::vector<PageData> runIn{NewPage(1)};
    AddText(runIn[0], "Another Paper", 200, 690, 20);
    float x = AddText(runIn[0], "Abstract\xE2\x80\x94", 72, 650, 9, kGlyphBold | kGlyphItalic);
    AddText(runIn[0], "We show that a label heads its paragraph when", x, 650, 9, kGlyphBold);
    AddText(runIn[0], "it is set off by a dash or a colon from the text.", 72, 639, 9, kGlyphBold);
    AddText(runIn[0], "I. First Section", 72, 600, 12, kGlyphBold);
    AddText(runIn[0], "Abstract algebra studies structures such as groups and rings,", 72, 580, 10);
    AddText(runIn[0], "and this paragraph of the body is about it and nothing else.", 72, 568, 10);
    md = Run(runIn);
    Show("abstract run in", md);
    RAYOPDF_CHECK(Contains(md, "# Abstract\n\n") && !Contains(md, "Abstract\xE2\x80\x94"), 1815);
    RAYOPDF_CHECK(Contains(md, "We show that a label heads its paragraph when it is set off"), 1816);
    RAYOPDF_CHECK(Contains(md, "\nAbstract algebra studies structures"), 1817);
    std::vector<PageData> period{NewPage(1)};
    AddText(period[0], "A Third Paper", 200, 690, 20);
    x = AddText(period[0], "Abstract.", 72, 650, 9, kGlyphBold);
    AddText(period[0], " We show that a label closed by a period stays in its paragraph,", x, 650, 9);
    AddText(period[0], "as a paragraph's label set in bold does in many templates.", 72, 639, 9);
    AddText(period[0], "1 Introduction", 72, 600, 12, kGlyphBold);
    md = Run(period);
    Show("abstract label with a period", md);
    RAYOPDF_CHECK(!Contains(md, "# Abstract") && Contains(md, "We show that a label closed by a period"), 1818);
}

void TestKicker() {
    // A short line over a slide's title in a smaller size, the page's topmost text, names what the
    // title belongs to: a paragraph, and the title the top heading.
    std::vector<PageData> slide{NewPage(1)};
    AddText(slide[0], "AI Pack", 72, 740, 13);
    AddText(slide[0], "Upstage Offers Packs", 72, 710, 22);
    AddBody(slide[0], 670, 3);
    std::string md = Run(slide);
    Show("kicker", md);
    RAYOPDF_CHECK(Contains(md, "AI Pack\n") && !Contains(md, "# AI Pack") && Contains(md, "# Upstage Offers Packs\n"), 1819);
    // Text above it: the line heads what follows it, a section of its own.
    std::vector<PageData> below{NewPage(1)};
    AddBody(below[0], 770, 1);
    AddText(below[0], "AI Pack", 72, 740, 13);
    AddText(below[0], "Upstage Offers Packs", 72, 710, 22);
    AddBody(below[0], 670, 3);
    md = Run(below);
    Show("not topmost", md);
    RAYOPDF_CHECK(Contains(md, "## AI Pack\n"), 1820);
    // A chapter's label in nearly the title's size is a heading of its own.
    std::vector<PageData> chapter{NewPage(1)};
    AddText(chapter[0], "Chapter 3", 72, 740, 20, kGlyphBold);
    AddText(chapter[0], "Numerical Methods", 72, 700, 24, kGlyphBold);
    AddBody(chapter[0], 660, 3);
    md = Run(chapter);
    Show("chapter label", md);
    RAYOPDF_CHECK(Contains(md, "## Chapter 3\n") && Contains(md, "# Numerical Methods\n"), 1821);
}

void TestRunningTextStyle() {
    // A long list of authors in a smaller size makes that size the most common one; the running
    // text set larger keeps its lone lines (a reference) as paragraphs, its bold sections headings.
    std::vector<PageData> paper{NewPage(1), NewPage(2)};
    AddText(paper[0], "1 Introduction", 72, 740, 12, kGlyphBold);
    for (int i = 0; i < 18; ++i)
        AddText(paper[0], "Running text of the paper set larger than the list of its authors at the end.", 72,
                720 - 14.0f * i, 11);
    AddText(paper[0], "[1] A. Author et al. Journal 12 (2020) 34.", 72, 440, 11);
    AddText(paper[0], "[2] B. Author et al. Journal 13 (2021) 56.", 72, 410, 11);
    for (int i = 0; i < 30; ++i)
        AddText(paper[1], "A. Name, B. Name, C. Name, D. Name, E. Name, F. Name, G. Name, H. Name, I. Name.", 72,
                740 - 10.0f * i, 8);
    const std::string md = Run(paper);
    Show("running text style", md);
    RAYOPDF_CHECK(Contains(md, "\n[1] A. Author et al.") && !Contains(md, "# [1]"), 1822);
    RAYOPDF_CHECK(Contains(md, "# 1 Introduction\n"), 1823);
}

} // namespace

int main() {
    TestTitleAboveOutline();
    TestSmallCapitals();
    TestOutlineNumbering();
    TestStandardSections();
    TestKicker();
    TestRunningTextStyle();
    std::puts("test_layout_headings: all checks passed");
    return 0;
}
