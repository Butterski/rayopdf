// SPDX-License-Identifier: Apache-2.0
// Core tests of src/layout: the layout and semantic stages on pages built glyph by glyph, then the
// whole conversion on PDFs built in memory (tests/pdf_builder.h). Exit codes 400-499 and 700-749.
#include "check.h"
#include "pdf_builder.h"

#include "common/file.h"
#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/glyph_box.h"
#include "layout/lines.h"
#include "layout/page.h"
#include "layout/path_box.h"
#include "layout/pipeline.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

// Appends `text` to `page` as glyphs of `size` points from (x, y), each `advance` em wide; a
// space is a space glyph. Returns the x after the text.
float AddText(PageData& page, std::string_view text, float x, float y, float size, uint16_t flags = 0,
              float advance = 0.5f) {
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        const float w = advance * size;
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

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void Show(const char* name, const std::string& md) {
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", name, md.c_str());
}

void TestWordGaps() {
    std::vector<PageData> pages{NewPage(1)};
    // "Hello" and "world" with a gap and no space glyph; kerning-sized gaps inside words.
    float x = AddText(pages[0], "Hel", 72, 700, 10);
    x = AddText(pages[0], "lo", x + 0.2f, 700, 10);
    AddText(pages[0], "world", x + 3, 700, 10);
    const std::string md = Run(pages);
    Show("word gaps", md);
    RAYOPDF_CHECK(md == "Hello world\n", 400);
}

void TestHeadingParagraph() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Big Title", 72, 700, 20);
    AddText(pages[0], "Body text that is long enough to be a paragraph line.", 72, 660, 10);
    AddText(pages[0], "Second line of the same paragraph goes here and ends.", 72, 648, 10);
    AddText(pages[0], "Another paragraph after a gap.", 72, 620, 10);
    const std::string md = Run(pages);
    Show("heading", md);
    RAYOPDF_CHECK(Contains(md, "# Big Title\n\n"), 401);
    RAYOPDF_CHECK(Contains(md, "paragraph line. Second line"), 402);
    RAYOPDF_CHECK(Contains(md, "ends.\n\nAnother paragraph"), 403);
}

void TestTwoColumns() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "A Title Across Both Columns Of The Page", 150, 740, 10, kGlyphBold);
    const char* left[] = {"left one alpha beta gamma delta", "left two alpha beta gamma delta",
                          "left three alpha beta gamma delt", "left four alpha beta gamma delta"};
    const char* right[] = {"right one alpha beta gamma delt", "right two alpha beta gamma delt",
                           "right three alpha beta gamma de", "right four alpha beta gamma delt"};
    for (int i = 0; i < 4; ++i) {
        AddText(pages[0], left[i], 72, 700 - 12.0f * i, 10);
        AddText(pages[0], right[i], 330, 700 - 12.0f * i, 10);
    }
    const std::string md = Run(pages);
    Show("two columns", md);
    const size_t l4 = md.find("left four"), r1 = md.find("right one"), title = md.find("A Title");
    RAYOPDF_CHECK(l4 != std::string::npos && r1 != std::string::npos && title != std::string::npos, 404);
    RAYOPDF_CHECK(title < l4 && l4 < r1, 405);
}

void TestLists() {
    std::vector<PageData> pages{NewPage(1)};
    float x = AddText(pages[0], "\xE2\x80\xA2", 72, 700, 10);
    AddText(pages[0], "first item", x + 8, 700, 10);
    x = AddText(pages[0], "\xE2\x80\xA2", 72, 686, 10);
    AddText(pages[0], "second item", x + 8, 686, 10);
    x = AddText(pages[0], "\xE2\x97\xA6", 90, 672, 10);
    AddText(pages[0], "nested item", x + 8, 672, 10);
    AddText(pages[0], "1. one", 72, 640, 10);
    AddText(pages[0], "2. two", 72, 626, 10);
    AddText(pages[0], "a) letter", 72, 600, 10);
    const std::string md = Run(pages);
    Show("lists", md);
    RAYOPDF_CHECK(Contains(md, "- first item\n- second item\n  - nested item"), 406);
    RAYOPDF_CHECK(Contains(md, "1. one\n2. two"), 407);
    RAYOPDF_CHECK(Contains(md, "1. letter"), 408);
}

void TestRunningHeaders() {
    std::vector<PageData> pages;
    for (int p = 1; p <= 3; ++p) {
        pages.push_back(NewPage(p));
        AddText(pages.back(), "ACME Quarterly Report 2026", 72, 760, 9);
        AddText(pages.back(), "Body text of page " + std::to_string(p) + " stays.", 72, 600, 10);
        AddText(pages.back(), std::to_string(p), 300, 40, 9);
    }
    const std::string md = Run(pages);
    Show("running headers", md);
    RAYOPDF_CHECK(!Contains(md, "ACME"), 409);
    RAYOPDF_CHECK(Contains(md, "Body text of page 3 stays."), 410);
    RAYOPDF_CHECK(!Contains(md, "\n2\n") && md.find("1\n") == std::string::npos, 411);
}

void TestSinglePageEdges() {
    // One page: a small running head in the top margin, a page number at the bottom, set apart
    // from the body: removed. A body-size first line above a heading stays.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Journal of Examples, Vol. 3", 72, 760, 8);
    for (int i = 0; i < 5; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the only page goes on here.", 72, 700 - 12.0f * i, 10);
    AddText(pages[0], "12", 300, 30, 10);
    ConvertReport report;
    std::string md;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report);
    Show("single-page edges", md);
    RAYOPDF_CHECK(!Contains(md, "Journal") && !Contains(md, "12") && Contains(md, "Body line 4"), 491);
    RAYOPDF_CHECK(report.headerFooterLines == 2, 492);
    std::vector<PageData> first{NewPage(1)};
    AddText(first[0], "First words of the document.", 72, 760, 10);
    AddText(first[0], "A Large Heading", 72, 720, 20, kGlyphBold);
    for (int i = 0; i < 4; ++i) AddText(first[0], "Body text line that runs on and on.", 72, 690 - 12.0f * i, 10);
    md = Run(first);
    Show("single-page edges: first line", md);
    RAYOPDF_CHECK(Contains(md, "First words of the document."), 493);
}

void TestDehyphenation() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "This is an experi-", 72, 700, 10);
    AddText(pages[0], "ment that works and a well-", 72, 688, 10);
    AddText(pages[0], "Known name.", 72, 676, 10);
    const std::string md = Run(pages);
    Show("dehyphenation", md);
    RAYOPDF_CHECK(Contains(md, "experiment that works"), 412);
    RAYOPDF_CHECK(Contains(md, "well- Known"), 413);
}

void TestCjkLineJoin() {
    // A paragraph wrapping between two ideographs joins them without a space.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE6\x96\x87\xE7\xAB\xA0\xE3\x81\x8C\xE7\xB6\x9A", 72,
            700, 10, 0, 1.0f);
    AddText(pages[0], "\xE3\x81\x8F\xE3\x80\x82", 72, 688, 10, 0, 1.0f);
    const std::string md = Run(pages);
    Show("cjk join", md);
    RAYOPDF_CHECK(Contains(md, "\xE7\xB6\x9A\xE3\x81\x8F\xE3\x80\x82"), 494);
}

void TestCodeBlock() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Some code follows.", 72, 720, 10);
    AddText(pages[0], "def f(x):", 72, 700, 10, kGlyphMonospace, 0.6f);
    AddText(pages[0], "return x * 2", 72 + 4 * 6, 688, 10, kGlyphMonospace, 0.6f);
    const std::string md = Run(pages);
    Show("code", md);
    RAYOPDF_CHECK(Contains(md, "```\ndef f(x):\n    return x * 2\n```"), 414);
}

void TestDuplicateBold() {
    std::vector<PageData> pages{NewPage(1)};
    float x = AddText(pages[0], "Plain ", 72, 700, 10);
    AddText(pages[0], "strong", x, 700, 10);
    AddText(pages[0], "strong", x + 0.3f, 700, 10);
    AddText(pages[0], " words.", x + 30, 700, 10);
    const std::string md = Run(pages);
    Show("duplicate bold", md);
    RAYOPDF_CHECK(md == "Plain **strong** words.\n", 415);
}

// A copy just inside the tolerance (0.15 em) folds whatever 4-unit cells the two straddle, with
// the filter's window found in float near the origin and in double beyond 2048 units.
void TestDuplicateCells() {
    for (const float x : {79.0f, 80.74f, 2046.6f, 2050.0f, -2049.0f}) {
        std::vector<PageData> pages{NewPage(1)};
        const float at = AddText(pages[0], "Plain ", x - 30, 700, 10);
        AddText(pages[0], "strong", at, 700, 10);
        AddText(pages[0], "strong", at + 1.49f, 701.49f, 10);
        const std::string md = Run(pages);
        Show("duplicate cells", md);
        RAYOPDF_CHECK(md == "Plain **strong**\n", 430);
    }
}

// A glyph with an infinite or NaN frame coordinate sends its page through the NaN-tested min and
// max; the other text still forms its lines.
void TestNonFiniteGlyph() {
    for (const float bad : {INFINITY, NAN}) {
        std::vector<PageData> pages{NewPage(1)};
        AddText(pages[0], "Words on a page.", 72, 700, 10);
        AddText(pages[0], "x", 72, 600, 10);
        pages[0].glyphs.back().u1 = bad;
        const std::string md = Run(pages);
        Show("non-finite glyph", md);
        RAYOPDF_CHECK(Contains(md, "Words on a page."), 431);
    }
}

void TestEmptyPage() {
    std::vector<PageData> pages{NewPage(1), NewPage(2)};
    AddText(pages[0], "Only page one has text.", 72, 700, 10);
    const std::string md = Run(pages);
    Show("empty page", md);
    RAYOPDF_CHECK(Contains(md, "<!-- rayopdf: page 2 has no extractable text -->"), 416);
}

void TestLinkAnnotation() {
    std::vector<PageData> pages{NewPage(1)};
    float x = AddText(pages[0], "Visit ", 72, 700, 10);
    const float x1 = AddText(pages[0], "our site", x, 700, 10);
    AddText(pages[0], " today.", x1, 700, 10);
    LinkArea link;
    link.x0 = x, link.x1 = x1, link.y0 = 695, link.y1 = 712;
    link.uri = static_cast<uint32_t>(pages[0].text.size());
    pages[0].text += "https://example.com/";
    link.uriLength = static_cast<uint32_t>(pages[0].text.size() - link.uri);
    pages[0].links.push_back(link);
    const std::string md = Run(pages);
    Show("link", md);
    RAYOPDF_CHECK(md == "Visit [our site](https://example.com/) today.\n", 417);
}

void TestEscaping() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "# not a heading and 2*3 a_b _x_ <b> [1] [x](y)", 72, 700, 10);
    const std::string md = Run(pages);
    Show("escaping", md);
    RAYOPDF_CHECK(md == "\\# not a heading and 2\\*3 a_b \\_x\\_ \\<b> [1] [x\\](y)\n", 418);
}

void TestBlanksAndUrlLinks() {
    // A form's blank stays unescaped; link text that shows its URL (broken by a line end) is text.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Name: ________ and see", 72, 700, 10);
    const float x0 = 72;
    const float x1 = AddText(pages[0], "https://example.org/a/ b-c.", x0, 680, 10);
    LinkArea link;
    link.x0 = x0, link.x1 = x1, link.y0 = 675, link.y1 = 692;
    link.uri = static_cast<uint32_t>(pages[0].text.size());
    pages[0].text += "https://example.org/a/b-c";
    link.uriLength = static_cast<uint32_t>(pages[0].text.size() - link.uri);
    pages[0].links.push_back(link);
    const std::string md = Run(pages);
    Show("blanks and url links", md);
    RAYOPDF_CHECK(Contains(md, "Name: ________ and see"), 495);
    RAYOPDF_CHECK(Contains(md, "https://example.org/a/ b-c.") && !Contains(md, "]("), 496);
}

void TestSuperscript() {
    std::vector<PageData> pages{NewPage(1)};
    float x = AddText(pages[0], "E = mc", 72, 700, 10);
    x = AddText(pages[0], "2", x, 704, 6);
    AddText(pages[0], " and H", x, 700, 10);
    const std::string md = Run(pages);
    Show("superscript", md);
    RAYOPDF_CHECK(Contains(md, "mc<sup>2</sup> and H"), 419);
}

void TestOutlineHeading() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Introduction", 72, 700, 10);
    AddText(pages[0], "Text of the introduction section is here.", 72, 680, 10);
    std::vector<OutlineEntry> outline{{"Introduction", 2}};
    const std::string md = Run(pages, outline);
    Show("outline", md);
    // The outline makes it a heading; set in the body size, it is a section, not a title.
    RAYOPDF_CHECK(Contains(md, "## Introduction\n\nText"), 420);
}

void TestHeadingLevels() {
    // Sections at 1.2 times the body size and no title: level 2, as in a title-less Markdown file.
    std::vector<PageData> sections{NewPage(1)};
    AddText(sections[0], "First Section", 72, 700, 12, kGlyphBold);
    AddText(sections[0], "Body text of the first section that runs long enough.", 72, 680, 10);
    AddText(sections[0], "Second Section", 72, 650, 12, kGlyphBold);
    AddText(sections[0], "Body text of the second section that runs long enough.", 72, 630, 10);
    std::string md = Run(sections);
    Show("heading levels: sections", md);
    RAYOPDF_CHECK(Contains(md, "## First Section\n") && Contains(md, "## Second Section\n"), 474);
    // A title twice the body size over sections at 1.5 times: levels 1 and 2.
    std::vector<PageData> titled{NewPage(1)};
    AddText(titled[0], "Document Title", 72, 740, 20, kGlyphBold);
    AddText(titled[0], "Section One", 72, 700, 15, kGlyphBold);
    AddText(titled[0], "Body text of the first section that runs long enough.", 72, 680, 10);
    AddText(titled[0], "Section Two", 72, 650, 15, kGlyphBold);
    AddText(titled[0], "Body text of the second section that runs long enough.", 72, 630, 10);
    md = Run(titled);
    Show("heading levels: titled", md);
    RAYOPDF_CHECK(Contains(md, "# Document Title\n") && Contains(md, "## Section One\n"), 475);
    // One large style heading several sections, no title above it: level 2 as well.
    std::vector<PageData> large{NewPage(1)};
    AddText(large[0], "Large One", 72, 740, 20, kGlyphBold);
    AddText(large[0], "Body text of the first section that runs long enough.", 72, 700, 10);
    AddText(large[0], "Large Two", 72, 660, 20, kGlyphBold);
    AddText(large[0], "Body text of the second section that runs long enough.", 72, 620, 10);
    md = Run(large);
    Show("heading levels: large", md);
    RAYOPDF_CHECK(Contains(md, "## Large One\n") && Contains(md, "## Large Two\n"), 476);
}

void TestFootnotes() {
    // References as raised small digits; notes at the page bottom in a smaller size, starting
    // with the same raised digits.
    std::vector<PageData> pages{NewPage(1)};
    float x = AddText(pages[0], "A claim needs a source.", 72, 700, 10);
    x = AddText(pages[0], "1", x, 704, 6);
    x = AddText(pages[0], " Another one does too.", x, 700, 10);
    AddText(pages[0], "2", x, 704, 6);
    AddText(pages[0], "3. not a note but a list item", 72, 670, 10);
    x = AddText(pages[0], "1", 72, 104, 5);
    AddText(pages[0], "Smith, Collected Essays, 2020.", x, 100, 8);
    x = AddText(pages[0], "2", 72, 94, 5);
    AddText(pages[0], "Doe, On Footnotes, 2021.", x, 90, 8);
    ConvertReport report;
    std::string md;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report);
    Show("footnotes", md);
    RAYOPDF_CHECK(Contains(md, "source.[^1] Another one does too.[^2]"), 477);
    RAYOPDF_CHECK(Contains(md, "\n\n[^1]: Smith, Collected Essays, 2020.\n\n[^2]: Doe, On Footnotes, 2021.\n"), 478);
    RAYOPDF_CHECK(Contains(md, "3. not a note but a list item") && report.footnotes == 2, 479);
    // A body-size numbered list under a raised number stays a list; so does a "References"
    // list in a small size on a later page.
    std::vector<PageData> refs{NewPage(1), NewPage(2)};
    x = AddText(refs[0], "Prior work exists.", 72, 700, 10);
    AddText(refs[0], "1", x, 704, 6);
    AddText(refs[0], "1. A numbered point in the body.", 72, 650, 10);
    AddText(refs[1], "References", 72, 700, 14, kGlyphBold);
    AddText(refs[1], "1. Smith, J. A cited work. 2020.", 72, 680, 8);
    md = Run(refs);
    Show("footnotes: references", md);
    RAYOPDF_CHECK(!Contains(md, "[^") && Contains(md, "1. Smith, J. A cited work. 2020."), 480);
}

void TestAffiliations() {
    // Affiliations under the author list, each after its raised mark, at the size of the text
    // below: they stay in the title block as text, no notes and no headings, and their marks are
    // no notes' for a numbered list on the next page. Nor is a mark of the author list that no
    // affiliation answers: a mark in larger print than the body refers to no endnote.
    std::vector<PageData> pages{NewPage(1), NewPage(2)};
    AddText(pages[0], "A Title of the Paper", 72, 740, 16);
    float x = AddText(pages[0], "Ann Author", 72, 710, 12);
    x = AddText(pages[0], "1", x, 715, 7);
    x = AddText(pages[0], " and Bob Writer", x, 710, 12);
    x = AddText(pages[0], "2", x, 715, 7);
    x = AddText(pages[0], " and Cy Third", x, 710, 12);
    AddText(pages[0], "3", x, 715, 7);
    x = AddText(pages[0], "1", 72, 694, 6);
    AddText(pages[0], "University of Alpha, Somewhere", x, 690, 10);
    x = AddText(pages[0], "2", 72, 680, 6);
    AddText(pages[0], "Institute of Beta, Elsewhere", x, 676, 10);
    for (int i = 0; i < 36; ++i) AddText(pages[0], "Body text of the paper that runs long enough to fill a line", 72, 640.0f - 14 * i, 10);
    AddText(pages[1], "The list on the next page follows its introduction here:", 72, 700, 10);
    AddText(pages[1], "1. First point of the list", 72, 680, 10);
    AddText(pages[1], "2. Second point of the list", 72, 666, 10);
    AddText(pages[1], "3. Third point of the list", 72, 652, 10);
    const std::string md = Run(pages);
    Show("affiliations", md);
    RAYOPDF_CHECK(!Contains(md, "[^"), 438);
    RAYOPDF_CHECK(md.find("University of Alpha") < md.find("Body text") && !Contains(md, "# <sup>"), 439);
    RAYOPDF_CHECK(Contains(md, "1. First point of the list") && Contains(md, "3. Third point of the list"), 440);
}

void TestRightToLeft() {
    // Hebrew placed in visual order (the last letter leftmost) is written in logical order.
    std::vector<PageData> pages{NewPage(1)};
    const float right = AddText(pages[0], "This English line sets the width of the text on the page.", 72, 720, 10);
    // Logical "שלום עולם" drawn right to left, "םלוע םולש" from the left, flush left: a short
    // line of a left-to-right paragraph.
    AddText(pages[0], "\xD7\x9D\xD7\x9C\xD7\x95\xD7\xA2 \xD7\x9D\xD7\x95\xD7\x9C\xD7\xA9", 72, 700, 10);
    // Logical "(שלום)" flush right: a right-to-left paragraph, whose brackets at an odd level are
    // mirrored (the glyph drawn leftmost, "(", is the closing bracket); unmirrored they would read
    // ")שלום(".
    AddText(pages[0], "(\xD7\x9D\xD7\x95\xD7\x9C\xD7\xA9)", right - 6 * 5, 650, 10);
    const std::string md = Run(pages);
    Show("right to left", md);
    RAYOPDF_CHECK(Contains(md, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D"), 481);
    RAYOPDF_CHECK(Contains(md, "(\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D)"), 482);
}

// --- Whole conversions of PDFs built in memory -------------------------------------------------

std::string ConvertPdf(const std::string& pdf, ConvertReport* reportOut = nullptr) {
    std::unique_ptr<Document> doc;
    const OpenStatus status = Document::Open(pdf, "", doc);
    RAYOPDF_CHECK(status == OpenStatus::Ok, 450);
    std::string md;
    ConvertReport report;
    Convert(*doc, ConvertOptions(), md, report);
    if (reportOut) *reportOut = report;
    return md;
}

void TestPdfTjTj() {
    // TJ kerning inside words, a large TJ gap between words, Tj words with explicit spaces.
    const std::string md = ConvertPdf(Test::OnePage(
        "BT /F1 12 Tf 72 700 Td [(Ke) 30 (rn) -20 (ing) -350 (gap)] TJ [-400 (here)] TJ ET\n"
        "BT /F1 12 Tf 72 680 Td (Spaces are here.) Tj ET\n"));
    Show("pdf Tj/TJ", md);
    RAYOPDF_CHECK(Contains(md, "Kerning gap here"), 451);
    RAYOPDF_CHECK(Contains(md, "Spaces are here."), 452);
}

void TestPdfTextState() {
    // Tc letter spacing and Tz scaling keep words; Tw widens word spaces; a raised Ts superscript.
    const std::string md = ConvertPdf(Test::OnePage(
        "BT /F1 12 Tf 0.5 Tc 72 700 Td (Spaced letters) Tj ET\n"
        "BT /F1 12 Tf 50 Tz 72 680 Td (Narrow text) Tj ET\n"
        "BT /F1 12 Tf 5 Tw 72 660 Td (Wide words) Tj ET\n"
        "BT /F1 12 Tf 72 640 Td (x) Tj /F1 7 Tf 5 Ts (2) Tj 0 Ts /F1 12 Tf ( squared) Tj ET\n"));
    Show("pdf text state", md);
    RAYOPDF_CHECK(Contains(md, "Spaced letters"), 453);
    RAYOPDF_CHECK(Contains(md, "Narrow text"), 454);
    RAYOPDF_CHECK(Contains(md, "Wide words"), 455);
    RAYOPDF_CHECK(Contains(md, "x<sup>2</sup> squared"), 456);
}

// The rules of a one-column lattice painted as dvips with Ghostscript paints every rule of TeX: a
// stencil mask of one sample scaled to the rule. The painted sample (0, or 1 under /D [1 0]) is a
// ruling and the rows are a table; a sample that is masked out, or a turned one, paints no rule.
void TestPdfStencilRules() {
    const auto page = [](const std::string& image, const std::string& turn) {
        std::string content;
        const auto rule = [&](double w, double h, double x, double y) {
            char cm[96];
            std::snprintf(cm, sizeof(cm), "q %s%.1f 0 0 %.1f %.1f %.1f cm ", turn.c_str(), w, h, x, y);
            content += cm + image + " Q\n";
        };
        for (int i = 0; i <= 4; ++i) rule(300, 0.4, 100, 700 - 20 * i);
        rule(0.4, 80, 100, 620);
        rule(0.4, 80, 400, 620);
        const char* const rows[] = {"Competence Framework", "Recycle", "Lifecycle", "Social"};
        for (int r = 0; r < 4; ++r)
            content += "BT /F1 10 Tf 104 " + std::to_string(686 - 20 * r) + " Td (" + rows[r] + ") Tj ET\n";
        return ConvertPdf(Test::OnePage(content));
    };
    const std::string table = "| Competence Framework |\n| --- |\n| Recycle |";
    const std::string zero = page(std::string("BI /IM true /W 1 /H 1 /BPC 1 ID ") + '\0' + "\nEI", "");
    Show("pdf stencil rules", zero);
    RAYOPDF_CHECK(Contains(zero, table), 434);
    const std::string one = page("BI /IM true /W 1 /H 1 /BPC 1 /D [1 0] ID \x80\nEI", "");
    RAYOPDF_CHECK(Contains(one, table), 435);
    const std::string masked = page("BI /IM true /W 1 /H 1 /BPC 1 ID \x80\nEI", "");
    RAYOPDF_CHECK(!Contains(masked, "| ---") && Contains(masked, "Competence Framework"), 436);
    const std::string turned = page(std::string("BI /IM true /W 1 /H 1 /BPC 1 ID ") + '\0' + "\nEI", "0.8 0.6 -0.6 0.8 0 0 cm ");
    RAYOPDF_CHECK(!Contains(turned, "| ---") && Contains(turned, "Competence Framework"), 437);
}

void TestPdfForm() {
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int form = b.AddStream("/Type /XObject /Subtype /Form /BBox [0 0 600 800] /Matrix [1 0 0 1 0 -100] "
                                 "/Resources << /Font << /F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >>",
                                 "BT /F2 12 Tf 72 700 Td (Inside the form.) Tj ET");
    const int content = b.AddFlateStream("", "BT /F1 12 Tf 72 700 Td (Page text.) Tj ET q /X1 Do Q /X1 Do");
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                           " 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
                           "/BaseFont /Helvetica >> >> /XObject << /X1 " + std::to_string(form) +
                           " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string md = ConvertPdf(b.Build(catalog));
    Show("pdf form", md);
    RAYOPDF_CHECK(Contains(md, "Page text."), 457);
    RAYOPDF_CHECK(Contains(md, "Inside the form."), 458);
    // The second Do draws the same text at the same place: one copy, as bold.
    RAYOPDF_CHECK(md.find("Inside the form.") == md.rfind("Inside the form."), 459);
}

// A form whose content is shorter than a string's inline buffer calls a second form, then reads on:
// the second form's buffer must not move the first's (a use after free before 2026-11-09).
void TestPdfNestedShortForm() {
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const std::string font = "/Font << /F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >>";
    const int inner = b.AddStream("/Type /XObject /Subtype /Form /BBox [0 0 600 800] /Resources << " + font + " >>",
                                  "BT /F2 12 Tf 72 700 Td (Nested form text.) Tj ET");
    const int second = b.AddStream("/Type /XObject /Subtype /Form /BBox [0 0 600 800] /Resources << " + font + " >>",
                                   "BT /F2 12 Tf 72 600 Td (Second inner form.) Tj ET");
    // 11 bytes: inside the std::string object.
    const int outer = b.AddStream("/Type /XObject /Subtype /Form /BBox [0 0 600 800] /Resources << /XObject << /I " +
                                      std::to_string(inner) + " 0 R /J " + std::to_string(second) + " 0 R >> >>",
                                  "/I Do /J Do");
    const int content = b.AddFlateStream("", "/O Do");
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                           " 0 R /MediaBox [0 0 612 792] /Resources << /XObject << /O " + std::to_string(outer) +
                           " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string md = ConvertPdf(b.Build(catalog));
    Show("pdf nested short form", md);
    // The outer form read on after the first inner one returned.
    RAYOPDF_CHECK(Contains(md, "Nested form text."), 432);
    RAYOPDF_CHECK(Contains(md, "Second inner form."), 433);
}

void TestPdfRotation() {
    // /Rotate 90 with text drawn rotated so that it reads upright on screen.
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    // In user space the text's "up" is -x: the line at x = 480 is above the one at x = 494.
    const int content = b.AddFlateStream("", "BT /F1 12 Tf 0 1 -1 0 480 72 Tm (Upright on screen.) Tj ET\n"
                                             "BT /F1 12 Tf 0 1 -1 0 494 72 Tm (Second line.) Tj ET");
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                           " 0 R /MediaBox [0 0 612 792] /Rotate 90 /Resources << /Font << /F1 << /Type /Font "
                           "/Subtype /Type1 /BaseFont /Helvetica >> >> >> /Contents " + std::to_string(content) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string md = ConvertPdf(b.Build(catalog));
    Show("pdf rotation", md);
    RAYOPDF_CHECK(Contains(md, "Upright on screen. Second line."), 460);
    // Rotated text on an unrotated page still reads as lines of its own direction.
    const std::string side = ConvertPdf(Test::OnePage("BT /F1 12 Tf 0 1 -1 0 300 100 Tm (Sideways words here) Tj ET"));
    Show("pdf sideways", side);
    RAYOPDF_CHECK(Contains(side, "Sideways words here"), 461);
}

void TestPdfColumnsAndHeading() {
    std::string content = "BT /F1 20 Tf 200 740 Td (Paper Title) Tj ET\n";
    for (int i = 0; i < 5; ++i) {
        const int y = 700 - 14 * i;
        content += "BT /F1 10 Tf 72 " + std::to_string(y) + " Td (Left column line number " + std::to_string(i) +
                   " has text) Tj ET\n";
        content += "BT /F1 10 Tf 320 " + std::to_string(y) + " Td (Right column line number " + std::to_string(i) +
                   " has text) Tj ET\n";
    }
    const std::string md = ConvertPdf(Test::OnePage(content));
    Show("pdf columns", md);
    RAYOPDF_CHECK(Contains(md, "# Paper Title"), 462);
    const size_t l4 = md.find("Left column line number 4"), r0 = md.find("Right column line number 0");
    RAYOPDF_CHECK(l4 != std::string::npos && r0 != std::string::npos && l4 < r0, 463);
}

void TestPdfActualText() {
    const std::string md = ConvertPdf(Test::OnePage(
        "BT /F1 12 Tf 72 700 Td (The ) Tj /Span << /ActualText (Drucker) >> BDC (Druk-) Tj EMC ( prints.) Tj ET"));
    Show("pdf actualtext", md);
    RAYOPDF_CHECK(Contains(md, "The Drucker prints."), 464);
}

void TestPdfLinkAndEmptyPage() {
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int content = b.AddFlateStream("", "BT /F1 12 Tf 72 700 Td (See the docs now.) Tj ET");
    const int empty = b.AddFlateStream("", "0 0 m 100 100 l S");
    const std::string fonts = "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >>";
    // "the docs" spans from 72 + width("See ") to before " now.": a generous rectangle over it.
    const int page1 = b.Add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 612 792] " + fonts +
                            " /Contents " + std::to_string(content) +
                            " 0 R /Annots [<< /Type /Annot /Subtype /Link /Rect [96 695 142 712] /A << /S /URI /URI "
                            "(https://example.org/docs) >> >>] >>");
    const int page2 = b.Add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 612 792] " + fonts +
                            " /Contents " + std::to_string(empty) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page1) + " 0 R " + std::to_string(page2) +
                     " 0 R] /Count 2 >>");
    ConvertReport report;
    const std::string md = ConvertPdf(b.Build(catalog), &report);
    Show("pdf link", md);
    RAYOPDF_CHECK(Contains(md, "[the docs](https://example.org/docs)"), 465);
    RAYOPDF_CHECK(Contains(md, "<!-- rayopdf: page 2 has no extractable text -->"), 466);
    RAYOPDF_CHECK(report.pages == 2 && report.pagesWithoutText == 1 && report.links == 1, 467);
}

void TestPdfOcrLayer() {
    // An invisible OCR layer (Tr 3) over a scan, in a fixed-pitch font with sizes fitted per word
    // box: plain paragraphs, no code spans, no code blocks, no headings, no scripts.
    const std::string fonts = "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Courier >>";
    std::string content = "q 612 0 0 792 0 0 cm Q 3 Tr\n";
    const char* words[] = {"Scanned", "pages", "carry", "an", "invisible", "text", "layer", "fitted", "to", "words."};
    const int sizes[] = {18, 9, 11, 7, 16, 10, 12, 8, 14, 10};
    for (int line = 0; line < 4; ++line) {
        float x = 72;
        for (int w = 0; w < 10; ++w) {
            const int size = sizes[(w + line) % 10];
            content += "BT /F1 " + std::to_string(size) + " Tf " + std::to_string(x) + " " +
                       std::to_string(700 - 14 * line) + " Td (" + words[w] + ") Tj ET\n";
            x += 0.6f * static_cast<float>(size) * static_cast<float>(std::string(words[w]).size()) + 4;
        }
    }
    const std::string md = ConvertPdf(Test::OnePage(content, fonts));
    Show("pdf ocr layer", md);
    RAYOPDF_CHECK(Contains(md, "Scanned pages carry an invisible text layer fitted to words."), 470);
    RAYOPDF_CHECK(md.find('`') == std::string::npos && md.find('#') == std::string::npos, 471);
    RAYOPDF_CHECK(md.find("<sup>") == std::string::npos && md.find("<sub>") == std::string::npos, 472);
    RAYOPDF_CHECK(md.find('*') == std::string::npos, 473);
}

void TestPdfMalformed() {
    // Broken operands, unbalanced q/Q and marked content, a missing font: no crash, errors counted.
    ConvertReport report;
    const std::string md = ConvertPdf(Test::OnePage("Q Q q q 1 0 0 cm BT /F9 12 Tf (x) Tj ET BT /F1 Tf (y) Tj "
                                                    "/F1 12 Tf 72 700 Td (Still read.) Tj ET EMC EMC BDC ] >> 1 2 3 Tj"),
                                      &report);
    Show("pdf malformed", md);
    RAYOPDF_CHECK(Contains(md, "Still read."), 468);
    RAYOPDF_CHECK(report.contentErrors >= 1, 469);
}

void TestPdfImages() {
    // Page 1: a photo drawn twice, a logo, text above and below. Page 2: the photo again, the logo
    // at the same place, a 10-pixel icon, a JBIG2 image and an inline image. Page 3: the photo
    // alone over the whole page.
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const std::string gray = "/Type /XObject /Subtype /Image /ColorSpace /DeviceGray /BitsPerComponent 8";
    const int photo = b.AddStream(gray + " /Width 40 /Height 40", std::string(1600, '\x70'));
    const int logo = b.AddStream(gray + " /Width 32 /Height 32", std::string(1024, '\x40'));
    const int icon = b.AddStream(gray + " /Width 10 /Height 10", std::string(100, '\x20'));
    // A filter no decoder knows, so the image is reported as an unsupported encoding.
    const int unknown = b.AddStream("/Type /XObject /Subtype /Image /Width 64 /Height 64 /ColorSpace /DeviceGray "
                                    "/BitsPerComponent 1 /Filter /FooDecode",
                                    std::string(64, '\x01'));
    const std::string xobjects = "/XObject << /Ph " + std::to_string(photo) + " 0 R /Lo " + std::to_string(logo) +
                                 " 0 R /Ic " + std::to_string(icon) + " 0 R /Jb " + std::to_string(unknown) + " 0 R >>";
    const std::string resources = "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
                                  ">> " + xobjects + " >>";
    const int c1 = b.AddFlateStream("", "BT /F1 12 Tf 72 720 Td (Before the figures.) Tj ET\n"
                                        "q 200 0 0 200 72 450 cm /Ph Do Q q 200 0 0 200 320 450 cm /Ph Do Q\n"
                                        "q 80 0 0 80 500 700 cm /Lo Do Q\n"
                                        "BT /F1 12 Tf 72 400 Td (After the figures.) Tj ET\n"
                                        "q 30 0 0 30 172 400 cm /Ph Do Q");
    const int c2 = b.AddFlateStream("", "BT /F1 12 Tf 72 720 Td (Second page text.) Tj ET\n"
                                        "q 150 0 0 150 72 500 cm /Ph Do Q q 80 0 0 80 500 700 cm /Lo Do Q\n"
                                        "q 100 0 0 100 300 500 cm /Ic Do Q q 100 0 0 100 72 300 cm /Jb Do Q\n"
                                        "q 150 0 0 150 300 300 cm BI /W 30 /H 30 /CS /G /BPC 8 ID " +
                                            std::string(900, '\x60') +
                                            " EI Q\n"
                                            "BT /F1 12 Tf 72 100 Td (End of page two.) Tj ET");
    const int c3 = b.AddFlateStream("", "q 612 0 0 792 0 0 cm /Ph Do Q");
    std::string kids;
    for (int content : {c1, c2, c3}) {
        const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 612 792] " +
                               resources + " /Contents " + std::to_string(content) + " 0 R >>");
        kids += std::to_string(page) + " 0 R ";
    }
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + kids + "] /Count 3 >>");
    const std::string pdf = b.Build(catalog);
    std::unique_ptr<Document> doc;
    RAYOPDF_CHECK(Document::Open(pdf, "", doc) == OpenStatus::Ok, 483);
    ConvertOptions options;
    // A fresh temporary directory, so the test leaves nothing in the working directory.
    std::string directory;
    RAYOPDF_CHECK(File::MakeTemporaryDirectory("rayopdf-test-layout-", directory), 498);
    options.imageDirectory = directory + "/images";
    options.imageLinkPrefix = "media/";
    std::string md;
    ConvertReport report;
    Convert(*doc, options, md, report);
    Show("pdf images", md);
    // Written once, referenced at each placement in reading order.
    RAYOPDF_CHECK(Contains(md, "Before the figures.\n\n![](media/p1-1.png)\n\n![](media/p1-1.png)\n\nAfter the figures."),
                  484);
    RAYOPDF_CHECK(Contains(md, "Second page text.\n\n![](media/p1-1.png)") && Contains(md, "![](media/p2-1.png)"), 485);
    // A small image on a text baseline, next to the text: written inline in its sentence.
    RAYOPDF_CHECK(Contains(md, "After the figures. ![](media/p1-1.png)"), 497);
    RAYOPDF_CHECK(Contains(md, "<!-- rayopdf: image on page 2 not extracted (unsupported encoding) -->"), 486);
    RAYOPDF_CHECK(Contains(md, "<!-- rayopdf: page 3 has no extractable text -->\n\n![](media/p1-1.png)"), 487);
    // Logo (2 placements), icon and the unknown-filter image skipped; photo and inline image written.
    RAYOPDF_CHECK(report.images == 2 && report.imagesSkipped == 4 && report.pagesWithoutText == 1, 488);
    {
        // Scoped: Windows removes no mapped file.
        File::InputFile file;
        RAYOPDF_CHECK(file.Open(options.imageDirectory + "/p1-1.png", 1u << 20) == File::ReadStatus::Ok &&
                          file.Bytes().substr(0, 4) == "\x89PNG",
                      489);
    }
    for (const char* name : {"/p1-1.png", "/p2-1.png"}) File::RemoveFile(options.imageDirectory + name);
    File::RemoveEmptyDirectory(options.imageDirectory);
    File::RemoveEmptyDirectory(directory);
    // No directory: no files and no references.
    std::string plain;
    Convert(*doc, ConvertOptions(), plain, report);
    RAYOPDF_CHECK(!Contains(plain, "![") && report.images == 0 && report.imagesSkipped == 0, 490);
}

void TestPdfBoldFontNames() {
    // URW's "-Medi" weight (LaTeX's Times-Bold clone) and TeX's "CMBX12" are bold: a body-size line
    // in them heads a section. "-DemiLight" stays regular.
    std::string widths = "/FirstChar 32 /LastChar 126 /Widths [";
    for (int c = 32; c <= 126; ++c) widths += "500 ";
    widths += "]";
    const std::string fonts = "/F1 << /Type /Font /Subtype /Type1 /BaseFont /NimbusRomNo9L-Regu " + widths + " >> " +
                              "/F2 << /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+NimbusRomNo9L-Medi " + widths +
                              " >> /F3 << /Type /Font /Subtype /Type1 /BaseFont /CMBX12 " + widths + " >> " +
                              "/F4 << /Type /Font /Subtype /Type1 /BaseFont /NotoSans-DemiLight " + widths + " >>";
    std::string content = "BT /F2 10 Tf 72 720 Td (B.3 Prompt Engineering) Tj ET\n";
    content += "BT /F1 10 Tf 72 700 Td (A key research area is the design of prompts for models.) Tj ET\n";
    content += "BT /F1 10 Tf 72 688 Td (It runs over a second line of plain body text here.) Tj ET\n";
    content += "BT /F3 10 Tf 72 660 Td (B.4 Instruction Tuning) Tj ET\n";
    content += "BT /F1 10 Tf 72 640 Td (Instruction tuning adjusts models with formatted data.) Tj ET\n";
    content += "BT /F4 10 Tf 72 612 Td (A light line) Tj ET\n";
    content += "BT /F1 10 Tf 72 592 Td (The last paragraph of body text closes the page here.) Tj ET\n";
    const std::string md = ConvertPdf(Test::OnePage(content, fonts));
    Show("pdf bold font names", md);
    RAYOPDF_CHECK(Contains(md, "## B.3 Prompt Engineering\n") && Contains(md, "## B.4 Instruction Tuning\n"), 700);
    RAYOPDF_CHECK(!Contains(md, "# A light line"), 701);
}

void TestHeadingCues() {
    std::vector<PageData> pages{NewPage(1)};
    // A bold body-size line may end in a colon or a question mark; a bold sentence is emphasis.
    AddText(pages[0], "Steps for Using the Microscope:", 72, 740, 10, kGlyphBold);
    AddText(pages[0], "Place the slide on the stage and line it up with the clips here.", 72, 720, 10);
    AddText(pages[0], "Are Smokers Time Discounters?", 72, 690, 10, kGlyphBold);
    AddText(pages[0], "Recall the distinction between two kinds of discounting in time.", 72, 670, 10);
    AddText(pages[0], "This sentence is bold.", 72, 640, 10, kGlyphBold);
    AddText(pages[0], "Body text after the bold sentence runs on for a while longer.", 72, 620, 10);
    // A caption set larger than the body is no heading.
    AddText(pages[0], "Figure 4.5. Breakdown of Fuel", 72, 590, 12);
    AddText(pages[0], "Body text under the caption runs on for a while longer too.", 72, 570, 10);
    const std::string md = Run(pages);
    Show("heading cues", md);
    RAYOPDF_CHECK(Contains(md, "## Steps for Using the Microscope:\n") &&
                      Contains(md, "## Are Smokers Time Discounters?\n"),
                  702);
    RAYOPDF_CHECK(!Contains(md, "# This sentence is bold."), 703);
    RAYOPDF_CHECK(!Contains(md, "# Figure") && Contains(md, "Figure 4.5. Breakdown of Fuel"), 704);
}

Shape Bar(float x, float y0, float y1) {
    Shape s;
    s.x0 = x, s.x1 = x + 3, s.y0 = y0, s.y1 = y1;
    s.kind = ShapeKind::Rect;
    s.filled = true;
    return s;
}

void TestQuoteBarsAndHeadings() {
    // An accent bar beside a slide title is no quote.
    std::vector<PageData> slide{NewPage(1)};
    AddText(slide[0], "Our Purpose Today", 80, 700, 22);
    slide[0].shapes.push_back(Bar(70, 694, 720));
    AddText(slide[0], "Body text of the slide runs long enough to set the body size.", 72, 660, 11);
    AddText(slide[0], "More body text of the slide runs long enough to set the size.", 72, 646, 11);
    std::string md = Run(slide);
    Show("quote bars: slide", md);
    RAYOPDF_CHECK(Contains(md, "# Our Purpose Today\n") && !Contains(md, ">"), 705);
    // A heading inside a quote whose paragraph goes on beside the same bar stays quoted.
    std::vector<PageData> quote{NewPage(1)};
    AddText(quote[0], "Body text before the quote runs long enough to set the body size.", 72, 740, 10);
    AddText(quote[0], "Quoted Heading", 80, 700, 14);
    AddText(quote[0], "The quoted paragraph follows the heading inside the same bar.", 80, 680, 10);
    quote[0].shapes.push_back(Bar(70, 676, 716));
    md = Run(quote);
    Show("quote bars: quote", md);
    RAYOPDF_CHECK(Contains(md, "> ## Quoted Heading\n") || Contains(md, "> # Quoted Heading\n"), 706);
}

void TestAffiliationNotes() {
    // Raised digits after author names and before affiliations under them: the affiliations are no
    // footnotes, since body text follows below them; they stay in place.
    std::vector<PageData> pages{NewPage(1)};
    float x = AddText(pages[0], "Ann Author", 200, 740, 10);
    AddText(pages[0], "1", x, 744, 6);
    x = AddText(pages[0], "1", 150, 724, 5);
    AddText(pages[0], "University of Somewhere, Some Town", x, 720, 8);
    AddText(pages[0], "Abstract text of the paper runs long enough to be the body text.", 72, 690, 10);
    AddText(pages[0], "It goes on over a second line so that the body size is clear.", 72, 678, 10);
    const std::string md = Run(pages);
    Show("affiliation notes", md);
    RAYOPDF_CHECK(!Contains(md, "[^"), 707);
    const size_t affiliation = md.find("University of Somewhere"), abstract = md.find("Abstract text");
    RAYOPDF_CHECK(affiliation != std::string::npos && abstract != std::string::npos && affiliation < abstract, 708);
}

void TestFigureObstacle() {
    // A figure on the left with its caption under it, a column of text on the right whose
    // paragraph gap lies beside the figure: the column is read whole, not cut at the gap.
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    ImageBox figure;
    figure.x0 = 72, figure.x1 = 250, figure.y0 = 300, figure.y1 = 500;
    page.images.push_back(figure);
    const char* first[] = {"First paragraph of the right column, line one.",
                           "First paragraph of the right column, line two.",
                           "First paragraph of the right column ends here."};
    const char* second[] = {"Second paragraph of the right column, line one.",
                            "Second paragraph of the right column, more text.",
                            "Second paragraph of the right column, line three.",
                            "Second paragraph of the right column ends here."};
    for (int i = 0; i < 3; ++i) AddText(page, first[i], 300, 490.0f - 12 * i, 10);
    // The second paragraph runs down beside the caption.
    for (int i = 0; i < 16; ++i) {
        const char* text = i == 15 ? second[3] : i == 0 ? second[0] : second[1 + i % 2];
        AddText(page, text, 300, 430.0f - 12 * i, 10);
    }
    AddText(page, "Figure 1. The caption under the figure.", 72, 285, 9);
    AddText(page, "It runs over two lines of small text.", 72, 274, 9);
    const std::string md = Run(pages);
    Show("figure obstacle", md);
    // The caption comes before the column or after it, never between its paragraphs.
    const size_t caption = md.find("Figure 1."), start = md.find("First paragraph"),
                 end = md.find("Second paragraph of the right column ends here.");
    RAYOPDF_CHECK(caption != std::string::npos && end != std::string::npos && (caption < start || caption > end), 709);
}

void TestNumberedListing() {
    // A listing with line numbers in the margin (Eisvogel, listings, minted): one code block, the
    // numbers dropped, the indentation and the blank line kept, the frame's side rules no quote.
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    AddText(page, "Prose before the listing runs long enough to be the body text here.", 72, 740, 10);
    const char* code[] = {"import json", "", "class Loader:", "    def load(self):", "        return 1"};
    for (int i = 0; i < 5; ++i) {
        const float y = 710.0f - 12 * i;
        AddText(page, std::to_string(i + 1), 82, y, 10);
        if (code[i][0]) {
            const char* text = code[i];
            float x = 98;
            while (*text == ' ') x += 5, ++text;
            AddText(page, text, x, y, 10, kGlyphMonospace);
        }
        Shape left, right;
        left.x0 = 70, left.x1 = 70.4f, right.x0 = 526, right.x1 = 526.4f;
        left.y0 = right.y0 = y - 4, left.y1 = right.y1 = y + 8;
        left.kind = right.kind = ShapeKind::Rect;
        left.filled = right.filled = true;
        page.shapes.push_back(left);
        page.shapes.push_back(right);
    }
    AddText(page, "Prose after the listing runs long enough to be the body text too.", 72, 630, 10);
    const std::string md = Run(pages);
    Show("numbered listing", md);
    RAYOPDF_CHECK(Contains(md, "```\nimport json\n\nclass Loader:\n    def load(self):\n        return 1\n```"), 710);
    RAYOPDF_CHECK(!Contains(md, ">") && !Contains(md, "|"), 711);
}

void TestMathIsNoHeading() {
    // Display math set large is no heading; neither is a raised digit after a math letter a note
    // reference that a later numbered line could take.
    std::vector<PageData> pages{NewPage(1), NewPage(2)};
    AddText(pages[0], "Body text before the formula runs long enough to set the body size.", 72, 740, 10);
    AddText(pages[0], "\xE2\x88\xAB \xF0\x9D\x91\xA5 \xF0\x9D\x91\x91\xF0\x9D\x91\xA5 =", 200, 710, 16);
    float x = AddText(pages[0], "The root needs \xF0\x9D\x91\x8F", 72, 680, 10);
    AddText(pages[0], "2", x, 684, 6);
    AddText(pages[0], "Body text after the formula runs long enough to set the body size.", 72, 660, 10);
    AddText(pages[1], "2. Second numbered item", 72, 700, 8);
    const std::string md = Run(pages);
    Show("math is no heading", md);
    RAYOPDF_CHECK(!Contains(md, "# \xE2\x88\xAB"), 712);
    RAYOPDF_CHECK(!Contains(md, "[^"), 713);
}

void TestJustifiedLineStaysWhole() {
    // A justified line with wide word spaces between short lines (a quote's last lines) is one
    // line, not columns: no line beside it has text on both sides of a gap.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Nested quote.", 97, 332, 10);
    float x = AddText(pages[0], "Lazy quote continuation starts here", 84, 285, 10);
    for (const char* word : {"and", "continues", "without", "another", "quote"}) x = AddText(pages[0], word, x + 7, 285, 10);
    AddText(pages[0], "marker.", 84, 269, 10);
    AddText(pages[0], "Short line after.", 86, 241, 10);
    const std::string md = Run(pages);
    Show("justified line", md);
    RAYOPDF_CHECK(Contains(md, "starts here and continues without another quote marker."), 714);
}

void TestSmallTitle() {
    // LaTeX at 11 pt: \section at 1.31 times the body, used once above \subsection at 1.09 times:
    // a title (H1) over sections (H2), not sections over subsections.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Document Title", 72, 740, 14.4f, kGlyphBold);
    AddText(pages[0], "Body text under the title runs long enough to set the body size here.", 72, 710, 11);
    AddText(pages[0], "Section One", 72, 680, 12, kGlyphBold);
    AddText(pages[0], "Body text of the first section runs long enough to set the body size.", 72, 655, 11);
    AddText(pages[0], "Section Two", 72, 625, 12, kGlyphBold);
    AddText(pages[0], "Body text of the second section runs long enough to set the body size.", 72, 600, 11);
    const std::string md = Run(pages);
    Show("small title", md);
    RAYOPDF_CHECK(md.compare(0, 17, "# Document Title\n") == 0 && Contains(md, "\n## Section One\n"), 715);
}

void TestRunInHeading() {
    // A bold run set off by a quad starts its paragraph: a heading one level below the last
    // heading of its own line. A bold lead-in that ends its sentence stays in the paragraph.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Subsection Title", 72, 740, 10, kGlyphBold);
    AddText(pages[0], "Body text under the subsection runs long enough to be the body here.", 72, 720, 10);
    float x = AddText(pages[0], "Topic Heading", 72, 690, 10, kGlyphBold);
    AddText(pages[0], "The topic text goes on beside its heading on this line.", x + 10, 690, 10);
    AddText(pages[0], "It continues on the next line of the same paragraph here.", 72, 678, 10);
    x = AddText(pages[0], "Results.", 72, 650, 10, kGlyphBold);
    AddText(pages[0], "We find that lead-ins ending a sentence stay in the text.", x + 10, 650, 10);
    AddText(pages[0], "The paragraph with the lead-in goes on for a second line.", 72, 638, 10);
    const std::string md = Run(pages);
    Show("run-in heading", md);
    RAYOPDF_CHECK(Contains(md, "## Subsection Title\n") && Contains(md, "\n### Topic Heading\n\nThe topic text"), 716);
    RAYOPDF_CHECK(Contains(md, "**Results.** We find"), 717);
}

void TestContentsAndChapterNumbers() {
    // Table-of-contents entries (a page number far right on the baseline) are no headings; a
    // chapter number set large above its title heads it with the title.
    std::vector<PageData> contents{NewPage(1)};
    AddText(contents[0], "Table of contents", 152, 553, 20);
    const char* entries[] = {"Introduction", "Part I: New Children", "Part II: Solitary Play"};
    for (int i = 0; i < 3; ++i) {
        AddText(contents[0], entries[i], 57, 515.0f - 20 * i, 12);
        AddText(contents[0], std::to_string(7 + 14 * i), 380, 515.0f - 20 * i, 10.5f);
        AddText(contents[0], "1. A chapter entry set in the body size of the page", 57, 505.0f - 20 * i, 10.5f);
        AddText(contents[0], std::to_string(8 + 14 * i), 380, 505.0f - 20 * i, 10.5f);
    }
    std::string md = Run(contents);
    Show("contents", md);
    RAYOPDF_CHECK(Contains(md, "# Table of contents") && !Contains(md, "# Introduction") && !Contains(md, "# Part"), 718);
    std::vector<PageData> chapter{NewPage(1)};
    AddText(chapter[0], "4", 61, 575, 24.8f, kGlyphBold);
    AddText(chapter[0], "Basis Fields", 61, 550, 17.2f, kGlyphBold);
    AddText(chapter[0], "A vector field may be written as a combination of basis fields.", 61, 519, 10.9f);
    AddText(chapter[0], "If n is the dimension, any n independent fields make a basis.", 61, 506, 10.9f);
    md = Run(chapter);
    Show("chapter number", md);
    RAYOPDF_CHECK(Contains(md, "# 4 Basis Fields\n"), 719);
}

void TestWrappedListingLine() {
    // A numbered listing breaks a long source line and indents the rest without a number: one
    // source line again.
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    AddText(page, "Prose before the listing runs long enough to be the body text here.", 72, 740, 10);
    AddText(page, "1", 82, 710, 10);
    AddText(page, "./configure --prefix=/usr/local --enable-static --disable-shared --with", 98, 710, 10,
            kGlyphMonospace);
    AddText(page, "-simd=avx2 --with-tests", 118, 698, 10, kGlyphMonospace);
    AddText(page, "2", 82, 686, 10);
    AddText(page, "make check", 98, 686, 10, kGlyphMonospace);
    AddText(page, "3", 82, 674, 10);
    AddText(page, "make install", 98, 674, 10, kGlyphMonospace);
    AddText(page, "Prose after the listing runs long enough to be the body text too.", 72, 640, 10);
    const std::string md = Run(pages);
    Show("wrapped listing", md);
    RAYOPDF_CHECK(Contains(md, "--with-simd=avx2 --with-tests\nmake check\nmake install\n"), 720);
}

void TestVerbatimEnumerators() {
    // "(2)" and "(iv)" have no Markdown list form: the item keeps them as text.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Body text before the list runs long enough to be the body text.", 72, 740, 10);
    AddText(pages[0], "(1) Voluntary service is one of the options here.", 72, 720, 10);
    AddText(pages[0], "(2) Paid service is the other of the options here.", 72, 706, 10);
    AddText(pages[0], "(iv) Aumento de la cobertura en todo el pais.", 72, 692, 10);
    const std::string md = Run(pages);
    Show("verbatim enumerators", md);
    RAYOPDF_CHECK(Contains(md, "(2) Paid service") && Contains(md, "(iv) Aumento") && !Contains(md, "2. Paid"), 721);
}

void TestMonospaceDocument() {
    // A page set in a monospace font: its lines make paragraphs, with no code spans.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Critical investigation of the roles public art plays in", 72, 740, 10, kGlyphMonospace);
    AddText(pages[0], "transnational social justice movements, historically and", 72, 728, 10, kGlyphMonospace);
    AddText(pages[0], "currently. Examination of the history of the mural.", 72, 716, 10, kGlyphMonospace);
    const std::string md = Run(pages);
    Show("monospace document", md);
    RAYOPDF_CHECK(Contains(md, "plays in transnational social justice movements, historically and currently.") &&
                      !Contains(md, "`"),
                  722);
}

Shape Box(float x0, float y0, float x1, float y1, ShapeKind kind, bool filled) {
    Shape s;
    s.x0 = x0, s.y0 = y0, s.x1 = x1, s.y1 = y1;
    s.kind = kind;
    s.filled = filled;
    s.stroked = !filled;
    return s;
}

void TestInlineStyles() {
    // A rule through the middle of a word strikes it; styles nest; punctuation set upright stays
    // outside a span; an italic letter alone is math, the pronoun "I" emphasis.
    std::vector<PageData> pages{NewPage(1)};
    float x = AddText(pages[0], "Some text with ", 72, 700, 10);
    const float x2 = AddText(pages[0], "deleted", x, 700, 10);
    AddText(pages[0], " words in it here.", x2, 700, 10);
    pages[0].shapes.push_back(Box(x, 702.8f, x2, 703.3f, ShapeKind::Line, false));
    x = AddText(pages[0], "Nesting: ", 72, 660, 10);
    x = AddText(pages[0], "strong ", x, 660, 10, kGlyphBold);
    x = AddText(pages[0], "inner", x, 660, 10, kGlyphBold | kGlyphItalic);
    x = AddText(pages[0], " text", x, 660, 10, kGlyphBold);
    AddText(pages[0], ", and plain words after it.", x, 660, 10);
    x = AddText(pages[0], "Let a page contain ", 72, 620, 10);
    x = AddText(pages[0], "n", x, 620, 10, kGlyphItalic);
    x = AddText(pages[0], " blocks, as ", x, 620, 10);
    x = AddText(pages[0], "I", x, 620, 10, kGlyphItalic);
    AddText(pages[0], " said before.", x, 620, 10);
    const std::string md = Run(pages);
    Show("inline styles", md);
    RAYOPDF_CHECK(Contains(md, "with ~~deleted~~ words"), 723);
    RAYOPDF_CHECK(Contains(md, "Nesting: **strong *inner* text**, and"), 724);
    // A URL broken over two lines under its link annotation is written whole.
    std::vector<PageData> url{NewPage(1)};
    const float u1 = AddText(url[0], "See https://example.com/do", 72, 700, 10);
    const float u2 = AddText(url[0], "cs/page for the details of this.", 72, 688, 10);
    for (int line = 0; line < 2; ++line) {
        LinkArea a;
        a.x0 = line == 0 ? 92 : 72, a.x1 = line == 0 ? u1 : u2 - 120, a.y0 = 698 - 12.0f * line, a.y1 = 708 - 12.0f * line;
        a.uri = static_cast<uint32_t>(url[0].text.size());
        url[0].text += "https://example.com/docs/page";
        a.uriLength = 29;
        url[0].links.push_back(a);
    }
    const std::string linked = Run(url);
    Show("broken url", linked);
    RAYOPDF_CHECK(Contains(linked, "See https://example.com/docs/page for"), 732);
    RAYOPDF_CHECK(Contains(md, "contain n blocks, as *I* said"), 725);
}

void TestAlertAndListings() {
    // A quote bar beside a lone "Note" and its text: a GitHub alert.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "A paragraph before the alert box.", 54, 650, 10);
    AddText(pages[0], "Note", 68, 615, 10, kGlyphBold);
    AddText(pages[0], "Useful information for readers here.", 68, 600, 10);
    pages[0].shapes.push_back(Box(54, 592, 57, 627, ShapeKind::Rect, true));
    std::string md = Run(pages);
    Show("alert", md);
    RAYOPDF_CHECK(Contains(md, "> [!NOTE]\n> Useful information for readers here."), 726);
    // Two listings 1.5 lines apart are two code blocks; a blank line inside one is a whole line.
    std::vector<PageData> code{NewPage(1)};
    AddText(code[0], "Two listings follow here.", 72, 720, 10);
    AddText(code[0], "a1 = 1", 72, 700, 10, kGlyphMonospace, 0.6f);
    AddText(code[0], "a2 = 2", 72, 688, 10, kGlyphMonospace, 0.6f);
    AddText(code[0], "a3 = 3", 72, 664, 10, kGlyphMonospace, 0.6f);
    AddText(code[0], "b1 = 4", 72, 646, 10, kGlyphMonospace, 0.6f);
    AddText(code[0], "b2 = 5", 72, 634, 10, kGlyphMonospace, 0.6f);
    AddText(code[0], "A closing paragraph after both listings.", 72, 610, 10);
    md = Run(code);
    Show("listings", md);
    RAYOPDF_CHECK(Contains(md, "```\na1 = 1\na2 = 2\n\na3 = 3\n```\n\n```\nb1 = 4\nb2 = 5\n```"), 727);
    // The dots of a dotted rule beside lines are no bullets.
    std::vector<PageData> dots{NewPage(1)};
    for (int i = 0; i < 4; ++i)
        AddText(dots[0], "Reference entry line number " + std::to_string(i) + " here.", 80, 700 - 10.0f * i, 7.5f);
    for (int i = 0; i < 10; ++i)
        dots[0].shapes.push_back(Box(70, 668 + 4.0f * i, 71, 669 + 4.0f * i, ShapeKind::Other, true));
    md = Run(dots);
    Show("dotted rule", md);
    RAYOPDF_CHECK(!Contains(md, "- ") && Contains(md, "number 0 here."), 728);
}

void TestTitleBlockAndRtlStop() {
    // A two-line centred title joins; the author line centred under it is no heading.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Pinching Systems:", 221, 720, 20);
    AddText(pages[0], "Curse?", 276, 696, 20);
    AddText(pages[0], "Jane Roe", 282, 664, 12);
    AddText(pages[0], "1 Introduction", 72, 630, 12, kGlyphBold);
    for (int i = 0; i < 5; ++i)
        AddText(pages[0], "Body text of the paper runs across the whole column here.", 72, 610 - 12.0f * i, 10);
    std::string md = Run(pages);
    Show("title block", md);
    RAYOPDF_CHECK(Contains(md, "# Pinching Systems: Curse?\n\nJane Roe\n\n"), 729);
    RAYOPDF_CHECK(Contains(md, "# 1 Introduction"), 730);
    // A full stop after a right-to-left word in a left-to-right line touches the word.
    std::vector<PageData> rtl{NewPage(1)};
    const float x = AddText(rtl[0], "\xD8\xA8\xD8\xA7\xD8\xAA\xD9\x83", 72, 700, 10);  // visual order of كتاب
    AddText(rtl[0], ".", x, 700, 10);
    AddText(rtl[0], "A long left-to-right line that sets the right edge of the text block.", 72, 680, 10);
    md = Run(rtl);
    Show("rtl stop", md);
    RAYOPDF_CHECK(Contains(md, "\xD9\x83\xD8\xAA\xD8\xA7\xD8\xA8."), 731);
}

void TestLinksMathAndNumbering() {
    // A link annotation over "reference," leaves the comma after the link.
    std::vector<PageData> pages{NewPage(1)};
    const float x = AddText(pages[0], "See the full ", 72, 700, 10);
    const float x2 = AddText(pages[0], "reference,", x, 700, 10);
    AddText(pages[0], " and more text after it.", x2, 700, 10);
    LinkArea a;
    a.x0 = x - 1, a.x1 = x2 + 1, a.y0 = 697, a.y1 = 709;
    a.uri = static_cast<uint32_t>(pages[0].text.size());
    pages[0].text += "https://x.org/";
    a.uriLength = 14;
    pages[0].links.push_back(a);
    // Mathematical alphanumerics in a fixed-pitch math font are no code.
    float m = AddText(pages[0], "The relation ", 72, 670, 10);
    m = AddText(pages[0], "\xF0\x9D\x90\xB8=\xF0\x9D\x91\x9A\xF0\x9D\x91\x90", m, 670, 10, kGlyphMonospace);
    AddText(pages[0], " holds for every body at rest.", m, 670, 10);
    std::string md = Run(pages);
    Show("link punctuation and math", md);
    RAYOPDF_CHECK(Contains(md, "full [reference](https://x.org/), and"), 733);
    RAYOPDF_CHECK(!Contains(md, "`"), 734);
    // Section numbers split one heading style into levels; "Abstract" sits with the sections.
    std::vector<PageData> paper{NewPage(1)};
    const char* heads[] = {"Abstract", "4 Results", "4.1 Setup", "4.1.1 Details", "5 Discussion"};
    AddText(paper[0], "A Paper Title", 72, 740, 20);
    for (int i = 0; i < 5; ++i) {
        AddText(paper[0], heads[i], 72, 700 - 60.0f * i, i == 0 ? 10 : 12, kGlyphBold);
        AddText(paper[0], "Body text of this part runs long enough to set the body size of the page.", 72,
                680 - 60.0f * i, 10);
        AddText(paper[0], "It goes on over a second line of body text in the same paragraph here.", 72,
                668 - 60.0f * i, 10);
    }
    md = Run(paper);
    Show("numbered levels", md);
    RAYOPDF_CHECK(Contains(md, "## Abstract") && Contains(md, "## 4 Results") && Contains(md, "### 4.1 Setup") &&
                      Contains(md, "#### 4.1.1 Details") && Contains(md, "## 5 Discussion"),
                  735);
    // A document set bold throughout keeps its "Table of Contents" title.
    std::vector<PageData> toc{NewPage(1)};
    AddText(toc[0], "Table of Contents", 72, 720, 12, kGlyphBold);
    for (int i = 0; i < 8; ++i)
        AddText(toc[0], "Laboratory exercise number " + std::to_string(i) + " ........................ " + std::to_string(3 * i + 3),
                72, 690 - 20.0f * i, 12, kGlyphBold);
    md = Run(toc);
    Show("bold contents", md);
    RAYOPDF_CHECK(Contains(md, "# Table of Contents\n"), 736);
}

// List markers: every bullet and task box, enumerators with their values, and look-alikes that
// are none (ParseMarker filters on the first byte before comparing).
void TestMarkers() {
    static const char* const kBullets[] = {
        "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA", "\xE2\x96\xAB", "\xE2\x97\x8F", "\xE2\x97\x8B",
        "\xE2\x80\xA3", "\xE2\x81\x83", "\xE2\x80\x93", "-",            "*",            "\xC2\xB7",
        "\xE2\x96\xA0", "\xE2\x96\xA1", "\xE2\x96\xBA", "\xE2\x96\xB6", "\xE2\x9E\xA2", "\xE2\x9E\xA4",
        "\xE2\x9D\x96", "\xE2\x99\xA6", "\xE2\x97\x86", "\xE2\x88\x99", "\xE2\x97\xBE", "\xEF\x82\xB7",
        "\xEF\x82\xA7", "\xEF\x83\x98", "\xEF\x81\xB6", "\xEF\x82\xA8", "\xE2\x80\x94", "\xE2\x9C\x93",
        "\xE2\x9C\x94",
    };
    uint32_t value = 7;
    for (const char* b : kBullets) RAYOPDF_CHECK(ParseMarker(b, value) == Marker::Bullet && value == 0, 421);
    RAYOPDF_CHECK(ParseMarker("\xE2\x98\x90", value) == Marker::TaskOpen, 422);
    RAYOPDF_CHECK(ParseMarker("\xE2\x98\x91", value) == Marker::TaskDone, 423);
    RAYOPDF_CHECK(ParseMarker("\xE2\x98\x92", value) == Marker::TaskDone, 424);
    for (const char* none : {"", "+", "--", "**", "\xC2\xB7\xC2\xB7", "\xE2\x80\xA0", "\xE2\x80\xA2x", "x", "\xEF\x82\xB6",
                             "\xC2\xA0", "1", "123456789."})
        RAYOPDF_CHECK(ParseMarker(none, value) == Marker::None, 425);
    RAYOPDF_CHECK(ParseMarker("12.", value) == Marker::Number && value == 12, 426);
    RAYOPDF_CHECK(ParseMarker("(iv)", value) == Marker::Roman && value == 4, 427);
    RAYOPDF_CHECK(ParseMarker("b)", value) == Marker::Letter && value == 2, 428);
    RAYOPDF_CHECK(ParseMarker("-.", value) == Marker::None, 429);
}

void TestQuotesTasksAndFormulas() {
    // A paragraph inset by about two and a half em on both sides between body paragraphs: a quote
    // set by indentation alone (LaTeX's quote environment).
    const std::string body = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sig.";
    std::vector<PageData> pages{NewPage(1)};
    for (int i = 0; i < 3; ++i) AddText(pages[0], body, 72, 700 - 12.0f * i, 10);
    AddText(pages[0], "The quoted words run on over two lines that are inset from both edges of the column", 96, 658, 10);
    AddText(pages[0], "and end the quotation here.", 96, 646, 10);
    for (int i = 0; i < 3; ++i) AddText(pages[0], body, 72, 622 - 12.0f * i, 10);
    std::string md = Run(pages);
    Show("indented quote", md);
    RAYOPDF_CHECK(Contains(md, "\n\n> The quoted words run on over two lines") && Contains(md, "quotation here.\n\nalpha"), 737);
    // LaTeX's third itemize level "∗", and task lists drawn as "⊠" (done) and "□" (open).
    std::vector<PageData> lists{NewPage(1)};
    float x = AddText(lists[0], "\xE2\x80\xA2", 72, 700, 10);
    AddText(lists[0], "Fruit", x + 4, 700, 10);
    x = AddText(lists[0], "\xE2\x80\x93", 90, 688, 10);
    AddText(lists[0], "Apples", x + 4, 688, 10);
    x = AddText(lists[0], "\xE2\x88\x97", 108, 676, 10);
    AddText(lists[0], "Granny Smith", x + 4, 676, 10);
    x = AddText(lists[0], "\xE2\x8A\xA0", 72, 650, 10);
    AddText(lists[0], "Write the draft", x + 4, 650, 10);
    x = AddText(lists[0], "\xE2\x96\xA1", 72, 638, 10);
    AddText(lists[0], "Review the draft", x + 4, 638, 10);
    md = Run(lists);
    Show("bullets and boxes", md);
    RAYOPDF_CHECK(Contains(md, "    - Granny Smith") && Contains(md, "- [x] Write the draft\n- [ ] Review the draft"), 738);
    // A display formula in pieces (a limit above, one below) is one paragraph.
    std::vector<PageData> formula{NewPage(1)};
    AddText(formula[0], "The integral of the square function over the unit interval follows here:", 72, 700, 10);
    AddText(formula[0], "1", 300, 670, 7);
    AddText(formula[0], "\xE2\x88\xAB \xF0\x9D\x91\xA5 \xF0\x9D\x91\x91\xF0\x9D\x91\xA5 = \xF0\x9D\x91\xA6", 290, 660, 10);
    AddText(formula[0], "0", 300, 652, 7);
    AddText(formula[0], "The sum of the first natural numbers comes next in the same way below.", 72, 620, 10);
    md = Run(formula);
    Show("formula pieces", md);
    const size_t integral = md.find("\xE2\x88\xAB");
    const size_t from = md.rfind("\n\n", integral), to = md.find("\n\n", integral);
    RAYOPDF_CHECK(integral != std::string::npos && from != std::string::npos && to != std::string::npos &&
                      md.compare(from, to - from, "\n\n1 \xE2\x88\xAB \xF0\x9D\x91\xA5 \xF0\x9D\x91\x91\xF0\x9D\x91\xA5 = "
                                                  "\xF0\x9D\x91\xA6 0") == 0,
                  739);
}

void TestShadingMathAndGutters() {
    // Lines shaded one by one in bands of one width, in a proportional font: a code listing.
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "The listing below prints a greeting when it runs.", 54, 720, 11);
    AddText(pages[0], "package main", 62, 700, 9.5f);
    AddText(pages[0], "func main() {", 62, 687, 9.5f);
    AddText(pages[0], "}", 62, 674, 9.5f);
    for (const float y : {700.0f, 687.0f, 674.0f}) pages[0].shapes.push_back(Box(54, y - 4, 541, y + 9.5f, ShapeKind::Rect, true));
    AddText(pages[0], "Text after the listing ends the example.", 54, 650, 11);
    std::string md = Run(pages);
    Show("shaded listing", md);
    RAYOPDF_CHECK(Contains(md, "```\npackage main\nfunc main() {\n}\n```"), 740);
    // Text that goes on in lowercase heads nothing, however large; italic letters of a product
    // ("4ac") and of an ordinal ("n-th") are math, not emphasis; a bold letter beside math is no
    // strong emphasis.
    std::vector<PageData> math{NewPage(1)};
    AddText(math[0], "Body text of the page runs long enough here to set the body size of it.", 72, 700, 10);
    AddText(math[0], "and so on", 72, 670, 14);
    float x = AddText(math[0], "The discriminant is b squared minus 4", 72, 640, 10);
    x = AddText(math[0], "ac", x, 640, 10, kGlyphItalic);
    x = AddText(math[0], ", for the ", x, 640, 10);
    x = AddText(math[0], "n", x, 640, 10, kGlyphItalic);
    x = AddText(math[0], "-th root. The field ", x, 640, 10);
    x = AddText(math[0], "E", x, 640, 10, kGlyphBold);
    AddText(math[0], " = \xF0\x9D\x9C\x8C holds.", x, 640, 10);
    md = Run(math);
    Show("lowercase and math styles", md);
    RAYOPDF_CHECK(!Contains(md, "# and so on") && Contains(md, "and so on"), 741);
    RAYOPDF_CHECK(Contains(md, "minus 4ac, for the n-th root. The field E = "), 742);
    // A column's first quote mark hanging into the gutter: the gutter still splits the line.
    PageData columns = NewPage(1);
    const char* left[] = {"left one alpha beta gamma delt", "left two alpha beta gamma and.", "left three alpha beta gamma de",
                          "left four alpha beta gamma del"};
    for (int i = 0; i < 4; ++i) {
        const float y = 700 - 12.0f * i;
        AddText(columns, left[i], 150, y, 10);
        if (i == 1) {
            // The left text ends at 300, the mark hangs at 304, the right text starts at 314.
            AddText(columns, "\xE2\x80\x9C", 304, y, 10);
            AddText(columns, "Would it be of use now said", 314, y, 10);
        } else {
            AddText(columns, "right column words go here ok", 314, y, 10);
        }
    }
    BuildLines(columns);
    bool split = false, joined = false;
    std::string text;
    for (const Line& line : columns.lines) {
        LineText(columns, line, text);
        split = split || (text.compare(0, 3, "\xE2\x80\x9C") == 0 && Contains(text, "Would"));
        joined = joined || (Contains(text, "and.") && Contains(text, "Would"));
    }
    RAYOPDF_CHECK(split && !joined, 743);
}

// The bits of a glyph's eight box floats.
bool SameBoxBits(const Glyph& a, const Glyph& b) {
    const float fa[8] = {a.x0, a.y0, a.x1, a.y1, a.u0, a.u1, a.vb, a.vt};
    const float fb[8] = {b.x0, b.y0, b.x1, b.y1, b.u0, b.u1, b.vb, b.vt};
    return std::memcmp(fa, fb, sizeof fa) == 0;
}

// layout/glyph_box.h: the SSE2 box has the scalar reference's bits on random terms and on the
// edges (±0, equal corners, sums overflowing to infinity, every direction, centres on the clip's
// edges, an empty clip), and the reference gives the box of a plain upright glyph.
void TestGlyphBox() {
    uint64_t state = 1;
    const auto next = [&state]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    };
    // A finite value: random magnitudes and signs, with zeros of both signs, small integers (equal
    // corners and centres on the clip) and huge values (overflow) mixed in.
    const auto value = [&]() -> double {
        switch (next() % 8) {
        case 0: return (next() & 1) ? 0.0 : -0.0;
        case 1: return static_cast<double>(static_cast<int>(next() % 9) - 4);
        case 2: return ((next() & 1) ? 1 : -1) * 1e308 * (1 + (next() % 7) / 8.0);
        case 3: return (static_cast<double>(next()) - 2147483648.0) / 65536.0 * 1e-300;
        default: return (static_cast<double>(next()) - 2147483648.0) / 4194304.0;
        }
    };
    size_t infinite = 0, clipped = 0, inside = 0;
    for (int i = 0; i < 200000; ++i) {
        const double xa0 = value(), xb0 = value(), yc0 = value(), yd0 = value(), yc1 = value(), yd1 = value();
        BoxTerms t;
        t.r[0] = value(), t.r[1] = value();
        t.s0[0] = xa0 + yc0, t.s0[1] = xb0 + yd0;
        t.s1[0] = xa0 + yc1, t.s1[1] = xb0 + yd1;
        t.y0[0] = yc0, t.y0[1] = yd0;
        t.y1[0] = yc1, t.y1[1] = yd1;
        t.clipLo[0] = value(), t.clipLo[1] = value();
        t.clipHi[0] = (next() & 3) ? t.clipLo[0] + std::fabs(value()) : value();
        t.clipHi[1] = (next() & 3) ? t.clipLo[1] + std::fabs(value()) : value();
        t.clipEmpty = next() % 16 == 0;
        const double width = value(), e = value(), f = value();
        const uint8_t dir = static_cast<uint8_t>(next() % 4);
        Glyph scalar, simd;
        const bool clipScalar = BoxScalar(t, width, e, f, dir, scalar);
#if defined(RAYOPDF_GLYPH_BOX_SSE2)
        const bool clipSimd = BoxSse2(t, width, e, f, dir, simd);
#else
        const bool clipSimd = BoxScalar(t, width, e, f, dir, simd);
#endif
        RAYOPDF_CHECK(SameBoxBits(scalar, simd), 744);
        RAYOPDF_CHECK(clipScalar == clipSimd, 745);
        infinite += std::isinf(scalar.x0) || std::isinf(scalar.y1);
        (clipScalar ? clipped : inside)++;
    }
    RAYOPDF_CHECK(infinite > 0 && clipped > 0 && inside > 0, 746);
    // An upright glyph 0.5 wide at size 10 from (100, 200), descent -0.25, ascent 0.75: Trm is
    // [10 0 0 10 100 200], so the box is 100..105 by 197.5..207.5; a clip around it keeps it.
    BoxTerms t{};
    t.r[0] = 10, t.r[1] = 0;
    t.s0[0] = 0, t.s0[1] = -2.5;
    t.s1[0] = 0, t.s1[1] = 7.5;
    t.y0[0] = 0, t.y0[1] = -2.5;
    t.y1[0] = 0, t.y1[1] = 7.5;
    t.clipLo[0] = -1, t.clipLo[1] = -1, t.clipHi[0] = 613, t.clipHi[1] = 793;
    t.clipEmpty = false;
    Glyph g;
    const bool cut = HorizontalBox(t, 0.5, 100, 200, 0, g);
    RAYOPDF_CHECK(!cut && g.x0 == 100 && g.x1 == 105 && g.y0 == 197.5f && g.y1 == 207.5f && g.u0 == 100 &&
                      g.u1 == 105 && g.vb == 197.5f && g.vt == 207.5f,
                  747);
}

// layout/path_box.h: the SSE2 path bounds have the scalar reference's bits, and the same answer
// whether any point was finite, on random CTMs and points with ±0, equal values, overflow to
// infinity, infinities and NaN mixed in, from an empty box and from a grown one.
void TestPathBox() {
    uint64_t state = 7;
    const auto next = [&state]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    };
    const auto value = [&]() -> double {
        switch (next() % 10) {
        case 0: return (next() & 1) ? 0.0 : -0.0;
        case 1: return static_cast<double>(static_cast<int>(next() % 5) - 2);
        case 2: return ((next() & 1) ? 1 : -1) * 1e308 * (1 + (next() % 7) / 8.0);
        case 3: return (next() & 1) ? INFINITY : -INFINITY;
        case 4: return (next() % 4 == 0) ? NAN : 1.5;
        default: return (static_cast<double>(next()) - 2147483648.0) / 4194304.0;
        }
    };
    size_t skipped = 0, grown = 0;
    for (int i = 0; i < 200000; ++i) {
        double m[6], xy[6], a[4], b[4];
        for (double& v : m) v = value();
        for (double& v : xy) v = value();
        if (next() & 1) {
            a[0] = a[1] = INFINITY, a[2] = a[3] = -INFINITY;
        } else {
            for (double& v : a) v = static_cast<double>(static_cast<int>(next() % 9) - 4);
        }
        std::memcpy(b, a, sizeof a);
        const int count = 1 + static_cast<int>(next() % 3);
        const bool anyScalar = PathScalar(m, xy, count, a);
#if defined(RAYOPDF_PATH_BOX_SSE2)
        const bool anySimd = PathSse2(m, xy, count, b);
#else
        const bool anySimd = PathScalar(m, xy, count, b);
#endif
        RAYOPDF_CHECK(anyScalar == anySimd && std::memcmp(a, b, sizeof a) == 0, 748);
        (anyScalar ? grown : skipped)++;
    }
    // [2 0 0 3 10 20]: (1, 1) and (-1, 2) give (12, 23) and (8, 26); an infinite point leaves.
    const double m[6] = {2, 0, 0, 3, 10, 20}, xy[6] = {1, 1, INFINITY, 0, -1, 2};
    double box[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
    const bool any = PathBounds(m, xy, 3, box);
    RAYOPDF_CHECK(any && grown > 0 && skipped > 0 && box[0] == 8 && box[1] == 23 && box[2] == 12 && box[3] == 26, 749);
}

} // namespace

int main() {
    TestWordGaps();
    TestHeadingParagraph();
    TestTwoColumns();
    TestLists();
    TestRunningHeaders();
    TestDehyphenation();
    TestCodeBlock();
    TestDuplicateBold();
    TestDuplicateCells();
    TestNonFiniteGlyph();
    TestEmptyPage();
    TestLinkAnnotation();
    TestEscaping();
    TestBlanksAndUrlLinks();
    TestSuperscript();
    TestOutlineHeading();
    TestHeadingLevels();
    TestFootnotes();
    TestAffiliations();
    TestRightToLeft();
    TestPdfTjTj();
    TestPdfTextState();
    TestPdfStencilRules();
    TestPdfForm();
    TestPdfNestedShortForm();
    TestPdfRotation();
    TestPdfColumnsAndHeading();
    TestPdfActualText();
    TestPdfLinkAndEmptyPage();
    TestPdfMalformed();
    TestPdfOcrLayer();
    TestPdfImages();
    TestSinglePageEdges();
    TestCjkLineJoin();
    TestPdfBoldFontNames();
    TestHeadingCues();
    TestQuoteBarsAndHeadings();
    TestAffiliationNotes();
    TestFigureObstacle();
    TestNumberedListing();
    TestMathIsNoHeading();
    TestJustifiedLineStaysWhole();
    TestSmallTitle();
    TestRunInHeading();
    TestContentsAndChapterNumbers();
    TestWrappedListingLine();
    TestVerbatimEnumerators();
    TestInlineStyles();
    TestAlertAndListings();
    TestLinksMathAndNumbering();
    TestQuotesTasksAndFormulas();
    TestShadingMathAndGutters();
    TestTitleBlockAndRtlStop();
    TestMarkers();
    TestMonospaceDocument();
    TestGlyphBox();
    TestPathBox();
    std::puts("test_layout: all checks passed");
    return 0;
}
