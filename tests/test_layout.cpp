// SPDX-License-Identifier: Apache-2.0
// Core tests of src/layout: the layout and semantic stages on pages built glyph by glyph, then the
// whole conversion on PDFs built in memory (tests/pdf_builder.h). Exit codes 400-499.
#include "check.h"
#include "pdf_builder.h"

#include "common/file.h"
#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/lines.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
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
    const int jbig2 = b.AddStream("/Type /XObject /Subtype /Image /Width 64 /Height 64 /ColorSpace /DeviceGray "
                                  "/BitsPerComponent 1 /Filter /JBIG2Decode",
                                  std::string(64, '\x01'));
    const std::string xobjects = "/XObject << /Ph " + std::to_string(photo) + " 0 R /Lo " + std::to_string(logo) +
                                 " 0 R /Ic " + std::to_string(icon) + " 0 R /Jb " + std::to_string(jbig2) + " 0 R >>";
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
    // A fresh directory under /tmp, so the test leaves nothing in the working directory.
    char directory[] = "/tmp/rayopdf-test-layout-XXXXXX";
    RAYOPDF_CHECK(mkdtemp(directory) != nullptr, 498);
    options.imageDirectory = std::string(directory) + "/images";
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
    // Logo (2 placements), icon and JBIG2 image skipped; photo and inline image written.
    RAYOPDF_CHECK(report.images == 2 && report.imagesSkipped == 4 && report.pagesWithoutText == 1, 488);
    File::InputFile file;
    RAYOPDF_CHECK(file.Open(options.imageDirectory + "/p1-1.png", 1u << 20) == File::ReadStatus::Ok &&
                      file.Bytes().substr(0, 4) == "\x89PNG",
                  489);
    for (const char* name : {"/p1-1.png", "/p2-1.png"}) unlink((options.imageDirectory + name).c_str());
    rmdir(options.imageDirectory.c_str());
    rmdir(directory);
    // No directory: no files and no references.
    std::string plain;
    Convert(*doc, ConvertOptions(), plain, report);
    RAYOPDF_CHECK(!Contains(plain, "![") && report.images == 0 && report.imagesSkipped == 0, 490);
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
    TestEmptyPage();
    TestLinkAnnotation();
    TestEscaping();
    TestBlanksAndUrlLinks();
    TestSuperscript();
    TestOutlineHeading();
    TestHeadingLevels();
    TestFootnotes();
    TestRightToLeft();
    TestPdfTjTj();
    TestPdfTextState();
    TestPdfForm();
    TestPdfRotation();
    TestPdfColumnsAndHeading();
    TestPdfActualText();
    TestPdfLinkAndEmptyPage();
    TestPdfMalformed();
    TestPdfOcrLayer();
    TestPdfImages();
    TestSinglePageEdges();
    TestCjkLineJoin();
    std::puts("test_layout: all checks passed");
    return 0;
}
