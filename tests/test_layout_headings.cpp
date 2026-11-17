// SPDX-License-Identifier: Apache-2.0
// Core tests of the heading rules of src/layout/semantics.cpp on scientific papers: the title
// above an outline that names the sections only, small capitals, numbering in the outline match,
// the standard unnumbered sections; the kicker over a slide's title, the titles of a slide's
// cards, a page's lead line and an italic subtitle under a heading; a cover's title and date, a
// chapter's number, a division's label and a display title's stacked lines; the centred headings
// of a page's own OCR layer; numbered section titles in italics on their own line and centred
// over their text.
// Pages are built glyph by glyph and converted by the whole pipeline. Exit codes 1800-1849 and
// 2500-2549.
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
        g.u0 = x, g.u1 = x + w, g.vb = y - 0.2f * size, g.vt = y + 0.8f * size, g.v = y;
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
    // text set larger keeps its lone lines (reference entries) out of the headings, its bold
    // sections headings.
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
    RAYOPDF_CHECK(Contains(md, "\n- [1] A. Author et al.") && !Contains(md, "# [1]"), 1822);
    RAYOPDF_CHECK(Contains(md, "# 1 Introduction\n"), 1823);
}

// Appends `text` centred on the page's axis (x = 306) in glyphs of `size` points.
void AddCentred(PageData& page, std::string_view text, float y, float size, uint16_t flags = 0) {
    AddText(page, text, 306 - 0.25f * size * static_cast<float>(text.size()), y, size, flags);
}

void TestTitleBlock() {
    // Authors centred under the title, each over affiliations of lines of many widths, one of them
    // nearly as wide as the text: the block runs on past the wide affiliations to the abstract.
    std::vector<PageData> wide{NewPage(1)};
    AddCentred(wide[0], "A Paper With Wide Affiliations", 740, 17);
    AddCentred(wide[0], "Grace Hopper", 710, 13);
    AddCentred(wide[0], "Department of Computer Science and Engineering, University of Somewhere, Some City", 695, 10);
    AddCentred(wide[0], "Another Institute of Technology", 683, 10);
    AddCentred(wide[0], "grace@example.org", 671, 10);
    AddCentred(wide[0], "Ada Lovelace", 645, 13);
    AddCentred(wide[0], "Analytical Engine Society", 630, 10);
    AddCentred(wide[0], "Abstract", 600, 11, kGlyphBold);
    AddBody(wide[0], 585, 3);
    AddText(wide[0], "1 Introduction", 72, 530, 13, kGlyphBold);
    AddBody(wide[0], 510, 3);
    std::string md = Run(wide);
    Show("wide affiliations", md);
    RAYOPDF_CHECK(Contains(md, "Ada Lovelace") && !Contains(md, "# Ada Lovelace") && !Contains(md, "# Grace Hopper"), 1824);
    // A paper's front matter set flush left (JMLR): the authors in bold over their affiliations,
    // then the abstract.
    std::vector<PageData> flush{NewPage(1)};
    AddCentred(flush[0], "Parametric Inference Using Diagrams", 740, 14, kGlyphBold);
    AddText(flush[0], "Kevin Author", 72, 700, 10, kGlyphBold);
    AddText(flush[0], "Columbia University, New York, NY.", 72, 688, 10);
    AddText(flush[0], "Dana Writer", 72, 664, 10, kGlyphBold);
    AddText(flush[0], "University of Barcelona, Spain.", 72, 652, 10);
    AddCentred(flush[0], "Abstract", 620, 12, kGlyphBold);
    AddBody(flush[0], 600, 3);
    AddText(flush[0], "1. Introduction", 72, 540, 12, kGlyphBold);
    AddBody(flush[0], 520, 3);
    md = Run(flush);
    Show("flush-left front matter", md);
    RAYOPDF_CHECK(Contains(md, "Kevin Author") && !Contains(md, "# Kevin Author") && !Contains(md, "# Dana Writer"), 1825);
    // Without an abstract, a flush-left heading under a flush-left title is a section of its own.
    std::vector<PageData> report{NewPage(1)};
    AddText(report[0], "Annual Report", 72, 740, 18);
    AddText(report[0], "Overview", 72, 700, 14, kGlyphBold);
    AddBody(report[0], 680, 3);
    md = Run(report);
    Show("report", md);
    RAYOPDF_CHECK(Contains(md, "# Overview\n"), 1826);
    // A title set in two blocks runs on to the authors under it.
    std::vector<PageData> split{NewPage(1)};
    AddCentred(split[0], "A Title Set", 740, 14, kGlyphBold);
    AddCentred(split[0], "In Two Blocks", 700, 14, kGlyphBold);
    AddCentred(split[0], "Jane Author", 670, 12, kGlyphItalic);
    AddCentred(split[0], "ABSTRACT", 640, 10, kGlyphBold);
    AddBody(split[0], 620, 3);
    md = Run(split);
    Show("split title", md);
    RAYOPDF_CHECK(Contains(md, "Jane Author") && !Contains(md, "# Jane Author") && Contains(md, "# In Two Blocks"), 1827);
}

void TestAddresses() {
    // Short lines set larger than the body that hold an e-mail address or a web address say where
    // to write or to look: paragraphs. An '@' between spaces is no address.
    std::vector<PageData> page{NewPage(1)};
    AddText(page[0], "1 Contact", 72, 740, 14, kGlyphBold);
    AddBody(page[0], 720, 3);
    AddText(page[0], "Write to jane.doe@example.org", 72, 670, 12);
    AddBody(page[0], 650, 3);
    AddText(page[0], "Code at https://example.org/repo", 72, 600, 12);
    AddBody(page[0], 580, 3);
    AddText(page[0], "2 Meet @ Noon", 72, 530, 14, kGlyphBold);
    AddBody(page[0], 510, 3);
    const std::string md = Run(page);
    Show("addresses", md);
    RAYOPDF_CHECK(Contains(md, "\nWrite to jane.doe@example.org") && !Contains(md, "# Write to"), 1828);
    RAYOPDF_CHECK(Contains(md, "\nCode at https://example.org/repo") && !Contains(md, "# Code at"), 1829);
    RAYOPDF_CHECK(Contains(md, "# 2 Meet @ Noon\n"), 1830);
}

void TestCardTitles() {
    // Three titles set larger on one baseline, each over a column of text: they title their
    // columns, though they are row peers.
    std::vector<PageData> cards{NewPage(1)};
    const char* const kTitles[] = {"Our Purpose", "Our Mission", "What We Do"};
    for (int c = 0; c < 3; ++c) {
        const float x = 72 + 180.0f * c;
        AddText(cards[0], kTitles[c], x, 700, 13);
        AddText(cards[0], "Text of the card that says what", x, 680, 10);
        AddText(cards[0], "the title above it is about and", x, 668, 10);
        AddText(cards[0], "goes on for a third line here.", x, 656, 10);
    }
    std::string md = Run(cards);
    Show("card titles", md);
    RAYOPDF_CHECK(Contains(md, "# Our Purpose\n") && Contains(md, "# Our Mission\n") && Contains(md, "# What We Do\n"), 1831);
    // Labels over numbers are a table's header row: no headings.
    std::vector<PageData> figures{NewPage(1)};
    const char* const kLabels[] = {"Revenue", "Profit", "Growth"};
    const char* const kNumbers[] = {"1,234", "567", "8.9%"};
    for (int c = 0; c < 3; ++c) {
        const float x = 72 + 180.0f * c;
        AddText(figures[0], kLabels[c], x, 700, 13);
        AddText(figures[0], kNumbers[c], x, 680, 10);
    }
    AddBody(figures[0], 640, 4);
    md = Run(figures);
    Show("labels over numbers", md);
    RAYOPDF_CHECK(Contains(md, "Revenue") && !Contains(md, "# Revenue") && !Contains(md, "# Profit") &&
                      !Contains(md, "# Growth"),
                  1832);
}

void TestLeadLine() {
    // A line in the body's size that opens the page, set apart from the paragraph under it, heads
    // the page.
    std::vector<PageData> page{NewPage(1)};
    AddText(page[0], "Print vs. Digital", 72, 700, 10);
    AddBody(page[0], 676, 3);
    AddBody(page[0], 630, 3);
    std::string md = Run(page);
    Show("lead line", md);
    RAYOPDF_CHECK(Contains(md, "# Print vs. Digital\n"), 1833);
    // One that ends a sentence is a paragraph.
    std::vector<PageData> sentence{NewPage(1)};
    AddText(sentence[0], "This page opens with a sentence.", 72, 700, 10);
    AddBody(sentence[0], 676, 3);
    AddBody(sentence[0], 630, 3);
    md = Run(sentence);
    Show("lead sentence", md);
    RAYOPDF_CHECK(Contains(md, "This page opens with a sentence.") && !Contains(md, "# This page"), 1834);
}

// The Markdown line that starts with `#` and holds `part`, or an empty view.
std::string_view HeadingWith(const std::string& md, std::string_view part) {
    for (size_t at = 0; at < md.size();) {
        size_t end = md.find('\n', at);
        if (end == std::string::npos) end = md.size();
        const std::string_view l(md.data() + at, end - at);
        if (!l.empty() && l[0] == '#' && l.find(part) != std::string_view::npos) return l;
        at = end + 1;
    }
    return {};
}

void TestSubtitle() {
    // An italic line under a heading in its size completes it: one heading.
    std::vector<PageData> page{NewPage(1)};
    AddText(page[0], "1. Shipping as a vector", 72, 700, 12, kGlyphBold);
    AddText(page[0], "List of ports in Appendix 3", 72, 676, 12, kGlyphBold | kGlyphItalic);
    AddBody(page[0], 650, 3);
    std::string md = Run(page);
    Show("italic subtitle", md);
    RAYOPDF_CHECK(HeadingWith(md, "Shipping as a vector").find("List of ports in Appendix 3") != std::string_view::npos, 1835);
    // A larger line under a chapter's label is a heading of its own.
    std::vector<PageData> chapter{NewPage(1)};
    AddText(chapter[0], "Chapter 3", 72, 740, 20);
    AddText(chapter[0], "Numerical differentiation", 72, 700, 24, kGlyphItalic);
    AddBody(chapter[0], 660, 3);
    md = Run(chapter);
    Show("chapter title", md);
    RAYOPDF_CHECK(!HeadingWith(md, "Chapter 3").empty() &&
                      HeadingWith(md, "Chapter 3").find("Numerical") == std::string_view::npos &&
                      !HeadingWith(md, "Numerical differentiation").empty(),
                  1836);
}

// Card titles whose columns hold larger text than they do (a slide's labels over its statements)
// still title them when most of the row stands over a column of two lines or 40 characters, and
// titles of two lines are titles too.
void TestCardContent() {
    std::vector<PageData> page{NewPage(1)};
    const char* const kTitles[] = {"Our Purpose", "Our Mission", "What We Do"};
    const char* const kFirst[] = {"Making AI Beneficial", "Easy-to-apply AI,", "Providing the best AI"};
    const char* const kSecond[] = {nullptr, "Everywhere", "solutions for everyone"};
    for (int c = 0; c < 3; ++c) {
        const float x = 40 + 190.0f * c;
        AddText(page[0], kTitles[c], x, 700, 13);
        AddText(page[0], kFirst[c], x, 676, 18);
        if (kSecond[c]) AddText(page[0], kSecond[c], x, 654, 18);
    }
    AddBody(page[0], 560, 5);
    std::string md = Run(page);
    Show("card statements", md);
    RAYOPDF_CHECK(Contains(md, "# Our Purpose\n") && Contains(md, "# Our Mission\n") && Contains(md, "# What We Do\n") &&
                      !Contains(md, "# Making AI") && !Contains(md, "# Easy-to"),
                  1837);
    std::vector<PageData> two{NewPage(1)};
    // Panels: titles of two lines on one baseline, each over a caption of its own (one or two
    // lines, on baselines of their own) and a chart's labels.
    const char* const kLine1[] = {"Comparison with Beauty", "Domestic Subscription", "Education Content"};
    const char* const kLine2[] = {"Recommendation Models", "Platform Model", "Platform PoC Case"};
    AddText(two[0], "Hit ratio of the models", 40, 666, 10);
    AddText(two[0], "on commerce service data", 40, 654, 10);
    AddText(two[0], "Quantitative evaluations", 230, 668, 10);
    AddText(two[0], "of the recommendations", 230, 656, 10);
    AddText(two[0], "Prediction rates", 420, 664, 10);
    for (int c = 0; c < 3; ++c) {
        const float x = 40 + 190.0f * c;
        AddText(two[0], kLine1[c], x, 700, 12);
        AddText(two[0], kLine2[c], x, 685, 12);
        for (int k = 0; k < 4; ++k) AddText(two[0], std::to_string(40 - 10 * k), x + 20.0f * k, 620.0f - 13 * (c + k), 9);
    }
    AddBody(two[0], 560, 5);
    md = Run(two);
    Show("two-line card titles", md);
    RAYOPDF_CHECK(!HeadingWith(md, "Comparison with Beauty").empty() &&
                      HeadingWith(md, "Comparison with Beauty").find("Recommendation Models") != std::string_view::npos &&
                      !HeadingWith(md, "Education Content").empty(),
                  1838);
}

// A cover page: its title, the one block of its style set at title size, heads the document though
// it holds more than 30 % of the cover's few characters; the date under it titles nothing.
void TestCoverTitle() {
    std::vector<PageData> page{NewPage(1)};
    AddText(page[0], "Land Ownership by Foreigners", 124, 520, 26, kGlyphBold);
    AddText(page[0], "in Selected Jurisdictions", 143, 489, 26, kGlyphBold);
    AddText(page[0], "June 2023", 270, 400, 15);
    AddText(page[0], "LL File No. 2023-022255, a report of the Library", 160, 160, 11);
    AddText(page[0], "The Law Library of Congress, Legal Research", 170, 146, 11);
    const std::string md = Run(page);
    Show("cover", md);
    RAYOPDF_CHECK(!HeadingWith(md, "Land Ownership by Foreigners").empty(), 1839);
    RAYOPDF_CHECK(Contains(md, "June 2023") && HeadingWith(md, "June 2023").empty(), 1840);
}

// A chapter's number set large over its title is a heading of its own; a display title's stacked
// lines in two sizes are one heading, its capitals written as they stand.
void TestChapterNumberAndStack() {
    std::vector<PageData> chapter{NewPage(1)};
    AddText(chapter[0], "2", 204, 700, 30);
    AddText(chapter[0], "The Lost Homeland", 114, 665, 24, kGlyphBold);
    AddBody(chapter[0], 620, 6);
    std::string md = Run(chapter);
    Show("chapter number", md);
    RAYOPDF_CHECK(Contains(md, "# 2\n") && !HeadingWith(md, "The Lost Homeland").empty() &&
                      HeadingWith(md, "The Lost Homeland").find('2') == std::string_view::npos,
                  1841);
    // A leaflet's panel: a smaller heading and its text, then the display title. "SEAGRASS" (8
    // glyphs of 16.6 points: 132.8 wide) over "IN SOUTH FLORIDA" (16 of 12: 192 wide), both
    // centred on x 300, the lower one's top 4 points below the upper one's bottom.
    std::vector<PageData> stack{NewPage(1)};
    AddText(stack[0], "How Can You Help?", 72, 740, 16, kGlyphBold);
    AddBody(stack[0], 716, 4);
    AddText(stack[0], "SEAGRASS", 300 - 66.4f, 600, 33.2f, kGlyphBold);
    AddText(stack[0], "IN SOUTH FLORIDA", 300 - 96, 600 - 6.64f - 4 - 19.2f, 24, kGlyphBold);
    AddBody(stack[0], 520, 4);
    md = Run(stack);
    Show("display stack", md);
    RAYOPDF_CHECK(!HeadingWith(md, "SEAGRASS IN SOUTH FLORIDA").empty(), 1842);
}

// A division's label in the body's size and face right over its title ("CHAPTER 1." over a bold
// "CALIFORNIA") is a heading at the title's level; a sentence naming a chapter and a label over
// body text are not.
void TestDivisionLabel() {
    const auto page = [](std::string_view label, bool titled) {
        std::vector<PageData> doc{NewPage(1)};
        AddText(doc[0], label, 72, 700, 10);
        if (titled)
            AddText(doc[0], "CALIFORNIA", 72, 680, 11, kGlyphBold);
        else
            AddBody(doc[0], 686, 1);
        AddText(doc[0], "JAMES SMITH", 72, 650, 8);
        AddBody(doc[0], 620, 6);
        return Run(doc);
    };
    std::string md = page("CHAPTER 1.", true);
    Show("division label", md);
    const std::string_view label = HeadingWith(md, "CHAPTER 1."), title = HeadingWith(md, "CALIFORNIA");
    RAYOPDF_CHECK(!label.empty() && !title.empty() && label.find(' ') == title.find(' '), 1848);
    md = page("Chapter 1 tells", true);
    RAYOPDF_CHECK(Contains(md, "Chapter 1 tells") && HeadingWith(md, "Chapter 1 tells").empty(), 2510);
    md = page("Part IV:", false);
    RAYOPDF_CHECK(Contains(md, "Part IV:") && HeadingWith(md, "Part IV:").empty(), 2511);
}

// A page's own OCR layer (invisible text: no faces) heads a section with a block set centred in
// its column, set off from the paragraph right under it; a table's label as OCR spells it
// ("TABLE12"), the title under it and a "-continued" line head nothing.
void TestOcrLayerHeadings() {
    std::vector<PageData> scan{NewPage(1)};
    PageData& page = scan[0];
    const auto body = [&](float y, int count) {
        for (int i = 0; i < count; ++i)
            AddText(page, "Body text of the section that runs long enough to be a paragraph line.", 72, y - 12.0f * i, 10,
                    kGlyphInvisible);
    };
    // Body lines span 72 to 427 (71 glyphs of 5 points): centred text is centred on 249.5.
    body(740, 4);
    AddText(page, "Example 4. Loading of the Liposomes", 249.5f - 87.5f, 680, 10, kGlyphInvisible);
    body(668, 4);
    AddText(page, "TABLE12", 249.5f - 17.5f, 600, 10, kGlyphInvisible);
    AddText(page, "Results of the Loading", 249.5f - 55, 584, 10, kGlyphInvisible);
    body(560, 4);
    AddText(page, "-continued", 249.5f - 25, 490, 10, kGlyphInvisible);
    body(470, 4);
    const std::string md = Run(scan);
    Show("ocr layer headings", md);
    const std::string_view heading = HeadingWith(md, "Example 4. Loading of the Liposomes");
    RAYOPDF_CHECK(!heading.empty() && heading.find("Body") == std::string_view::npos, 1843);
    RAYOPDF_CHECK(HeadingWith(md, "TABLE12").empty() && HeadingWith(md, "continued").empty(), 1844);
    RAYOPDF_CHECK(Contains(md, "Results of the Loading") && HeadingWith(md, "Results of the Loading").empty(), 1845);

    // A layer set in its type's sizes (each line in one size) heads a section with a short line set
    // larger; one whose sizes are fitted to its word boxes, word by word, does not.
    for (const bool fitted : {false, true}) {
        std::vector<PageData> sized{NewPage(1)};
        PageData& p = sized[0];
        const auto lines = [&](float y, int count) {
            static const char* const kWords[] = {"Body", "text", "of", "the", "section", "that", "runs", "long",
                                                 "enough", "to", "be", "a", "paragraph", "line."};
            for (int i = 0; i < count; ++i) {
                float x = 72;
                for (int w = 0; w < 14; ++w) {
                    const float size = fitted ? (w % 2 ? 9.0f : 11.0f) : 10.0f;
                    x = AddText(p, kWords[w], x, y - 12.0f * i, size, kGlyphInvisible);
                    x = AddText(p, " ", x, y - 12.0f * i, size, kGlyphInvisible);
                }
            }
        };
        lines(740, 4);
        AddText(p, "Installation", 72, 680, 13, kGlyphInvisible);
        lines(656, 4);
        const std::string out = Run(sized);
        Show(fitted ? "ocr layer fitted sizes" : "ocr layer typeset sizes", out);
        RAYOPDF_CHECK(Contains(out, "Installation") && HeadingWith(out, "Installation").empty() == fitted, fitted ? 1847 : 1846);
    }
}

void TestNumberedTitles() {
    // elsarticle's subsection: an italic line opening with a number of two levels, its section's
    // text going on upright at the paragraph's pitch.
    std::vector<PageData> italic{NewPage(1)};
    AddBody(italic[0], 740, 3);
    AddText(italic[0], "2.1. Spatial and temporal discretization", 72, 700, 10, kGlyphItalic);
    AddText(italic[0], "For the sake of brevity we set the lattice spacing and the time step here.", 86, 688, 10);
    AddBody(italic[0], 676, 2);
    std::string md = Run(italic);
    Show("italic numbered title", md);
    RAYOPDF_CHECK(!HeadingWith(md, "2.1. Spatial and temporal discretization").empty() &&
                      HeadingWith(md, "2.1. Spatial").find("brevity") == std::string_view::npos,
                  2500);
    // In a paragraph set in italics throughout, the numbered line is its text.
    std::vector<PageData> quote{NewPage(1)};
    AddBody(quote[0], 740, 3);
    AddText(quote[0], "2.1. Spatial and temporal discretization", 72, 700, 10, kGlyphItalic);
    AddText(quote[0], "for the sake of brevity we set the lattice spacing and the time step here.", 72, 688, 10, kGlyphItalic);
    AddBody(quote[0], 676, 2);
    md = Run(quote);
    Show("italic numbered line in italic text", md);
    RAYOPDF_CHECK(Contains(md, "2.1. Spatial") && HeadingWith(md, "2.1. Spatial").empty(), 2501);
    // Centred over the paragraph under it in the body's size: amsart's "5. Conclusion", IEEE's
    // "IV. SURVEY" in small capitals. The body lines run from 72 to 427 (glyphs of 5 points).
    const auto centred = [](std::string_view text) { return 249.5f - 2.5f * static_cast<float>(text.size()); };
    std::vector<PageData> centre{NewPage(1)};
    AddBody(centre[0], 740, 3);
    AddText(centre[0], "5. Conclusion", centred("5. Conclusion"), 700, 10);
    AddBody(centre[0], 680, 3);
    AddSmallCaps(centre[0], "IV. Survey of Practitioners", centred("IV. Survey of Practitioners"), 630, 10, 8);
    AddBody(centre[0], 610, 3);
    md = Run(centre);
    Show("centred numbered titles", md);
    RAYOPDF_CHECK(!HeadingWith(md, "5. Conclusion").empty(), 2502);
    RAYOPDF_CHECK(!HeadingWith(md, "IV. Survey of Practitioners").empty(), 2503);
    // A numbered item at the paragraph's left edge is a list's.
    std::vector<PageData> list{NewPage(1)};
    AddBody(list[0], 740, 3);
    AddText(list[0], "5. Conclusion", 72, 700, 10);
    AddBody(list[0], 680, 3);
    md = Run(list);
    Show("numbered item at the left edge", md);
    RAYOPDF_CHECK(Contains(md, "5. Conclusion") && HeadingWith(md, "5. Conclusion").empty(), 2504);
    // An italic line in capitals opening with "70.5." and naming its chapter is a manual's
    // running head, not a title.
    std::vector<PageData> head{NewPage(1)};
    AddBody(head[0], 740, 3);
    AddText(head[0], "70.5. EXAMINING THE DUMP FUNCTIONS CHAPTER 70. PERLGUTS", 72, 700, 10, kGlyphItalic);
    AddBody(head[0], 680, 3);
    md = Run(head);
    Show("italic numbered line in capitals", md);
    RAYOPDF_CHECK(Contains(md, "EXAMINING THE DUMP FUNCTIONS") && HeadingWith(md, "EXAMINING").empty(), 2505);
    // Centred over a paragraph but no section's title: a contents entry with leader dots, a note's
    // number in parentheses, a lower-case roman item, and a sentence's first line whose paragraph
    // goes on in lower case.
    const auto centredOver = [&](std::string_view text, std::string_view under, int lines) {
        std::vector<PageData> page{NewPage(1)};
        AddBody(page[0], 740, 3);
        AddText(page[0], text, centred(text), 700, 10);
        for (int i = 0; i < lines; ++i) AddText(page[0], under, 72, 680 - 12.0f * i, 10);
        const std::string out = Run(page);
        Show("centred numbered line", out);
        return out;
    };
    const std::string body = "Body text of the section that runs long enough to be a paragraph line.";
    md = centredOver("2. Financial evaluation .......... 12", body, 3);
    RAYOPDF_CHECK(Contains(md, "Financial evaluation") && HeadingWith(md, "Financial").empty(), 2506);
    md = centredOver("(2) Gross amount of goodwill", body, 3);
    RAYOPDF_CHECK(Contains(md, "Gross amount") && HeadingWith(md, "Gross amount").empty(), 2507);
    md = centredOver("ii. Benefits Financing", body, 3);
    RAYOPDF_CHECK(Contains(md, "Benefits Financing") && HeadingWith(md, "Benefits Financing").empty(), 2508);
    md = centredOver("27. A program product as in claim 26, wherein the",
                     "executable code of the product runs on a processor of the device so it", 3);
    RAYOPDF_CHECK(Contains(md, "A program product") && HeadingWith(md, "A program product").empty(), 2509);
}

} // namespace

int main() {
    TestTitleAboveOutline();
    TestSmallCapitals();
    TestOutlineNumbering();
    TestStandardSections();
    TestKicker();
    TestRunningTextStyle();
    TestTitleBlock();
    TestAddresses();
    TestCardTitles();
    TestLeadLine();
    TestSubtitle();
    TestCardContent();
    TestCoverTitle();
    TestChapterNumberAndStack();
    TestDivisionLabel();
    TestOcrLayerHeadings();
    TestNumberedTitles();
    std::puts("test_layout_headings: all checks passed");
    return 0;
}
