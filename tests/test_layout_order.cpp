// SPDX-License-Identifier: Apache-2.0
// Core tests of the reading-order stage (src/layout/reading_order.cpp): columns, cuts and page
// furniture, on pages built glyph by glyph and converted by the whole pipeline. Exit codes 750-799.
#include "check.h"

#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

// Appends `text` to `page` as upright glyphs of `size` points from (x, y), each half an em wide; a
// space is a space glyph.
void AddText(PageData& page, std::string_view text, float x, float y, float size) {
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
        if (cp == ' ') g.flags |= kGlyphSpace;
        page.glyphs.push_back(g);
        x += w;
        at += length;
    }
}

PageData NewPage() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    return page;
}

std::string Run(std::vector<PageData>& pages) {
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report);
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "---\n%s", md.c_str());
    return md;
}

size_t Find(const std::string& md, const char* needle) { return md.find(needle); }

// A running head inside the running band but below the outer margin, ending in a folio, set apart
// from the text: removed on a single page; the text stays.
void TestFolioHeader() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Journal of Testing 12(3) 39", 72, 735, 10);
    for (int i = 0; i < 20; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes here.", 72,
                700 - 12.0f * i, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Journal of Testing") == std::string::npos, 750);
    RAYOPDF_CHECK(Find(md, "Body line 0 of") != std::string::npos, 751);
    RAYOPDF_CHECK(Find(md, "Body line 19 of") != std::string::npos, 752);
}

// Reference labels set apart from their entries by a tab-sized gap are read with their entries,
// not as a column of their own.
void TestLabelColumn() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 6; ++i) {
        const float y = 700 - 30.0f * i;
        AddText(pages[0], "[" + std::to_string(i + 1) + "]", 57, y, 9);
        AddText(pages[0], "Reference number " + std::to_string(i + 1) + " by Author and Others,", 78, y, 9);
        AddText(pages[0], "Journal of Examples, volume " + std::to_string(i + 10) + ".", 78, y - 11, 9);
    }
    for (int i = 0; i < 14; ++i)
        AddText(pages[0], "Right column text line " + std::to_string(i) + " runs along here.", 330,
                700 - 11.0f * i, 9);
    const std::string md = Run(pages);
    const size_t entry1 = Find(md, "Reference number 1"), label2 = Find(md, "[2]"), entry2 = Find(md, "Reference number 2");
    RAYOPDF_CHECK(entry1 != std::string::npos && label2 != std::string::npos && entry2 != std::string::npos, 753);
    RAYOPDF_CHECK(entry1 < label2 && label2 < entry2, 754);
}

// A column holding a display formula (many short pieces with few characters) is still a column of
// text: the left column is read to its end before the right one starts.
void TestFormulaColumn() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 12; ++i)
        AddText(pages[0], "Left column line " + std::to_string(i) + " alpha beta gamma.", 72, 700 - 12.0f * i, 10);
    for (int i = 0; i < 3; ++i)
        AddText(pages[0], "Right top line " + std::to_string(i) + " alpha beta gamma.", 330, 700 - 12.0f * i, 10);
    const char* pieces[] = {"X", "E =", "w", "(1)", "i", "X", "F =", "v", "(2)"};
    for (int i = 0; i < 9; ++i) AddText(pages[0], pieces[i], 340 + 40.0f * (i % 4), 664 - 12.0f * (i / 2 + 1), 10);
    for (int i = 0; i < 3; ++i)
        AddText(pages[0], "Right end line " + std::to_string(i) + " alpha beta gamma.", 330, 580 - 12.0f * i, 10);
    const std::string md = Run(pages);
    const size_t leftEnd = Find(md, "Left column line 11"), rightTop = Find(md, "Right top line 0");
    RAYOPDF_CHECK(leftEnd != std::string::npos && rightTop != std::string::npos, 755);
    RAYOPDF_CHECK(leftEnd < rightTop, 756);
}

// Double-spaced text set high on the page keeps its first line: a header must be set apart by
// more than the text's own line spacing.
void TestDoubleSpacedTop() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 20; ++i)
        AddText(pages[0], "Spaced line " + std::to_string(i) + " of a double-spaced letter.", 72,
                760 - 30.0f * i, 12);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Spaced line 0 of") != std::string::npos, 757);
    RAYOPDF_CHECK(Find(md, "Spaced line 19 of") != std::string::npos, 758);
}


// Two rows of small print at the top (a journal line and its DOI) set close together and apart from
// the text as a block: both are furniture, even above a heading.
void TestMetadataBlock() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Journal of Examples 4(2), 2021", 72, 760, 8);
    AddText(pages[0], "DOI: https://doi.org/10.1000/example", 72, 750, 8);
    AddText(pages[0], "A Section Title", 72, 715, 14);
    for (int i = 0; i < 20; ++i)
        AddText(pages[0], "Text line " + std::to_string(i) + " of the article body runs here.", 72, 690 - 12.0f * i, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "doi.org") == std::string::npos, 759);
    RAYOPDF_CHECK(Find(md, "Journal of Examples") == std::string::npos, 760);
    RAYOPDF_CHECK(Find(md, "A Section Title") != std::string::npos && Find(md, "Text line 19 of") != std::string::npos, 761);
}

// A column of short list items beside a column of prose is a column too: the list is read to
// its end before the prose.
void TestListColumn() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 12; ++i) AddText(pages[0], "- item " + std::to_string(i + 1), 72, 700 - 12.0f * i, 10);
    AddText(pages[0], "More items", 72, 540, 12);
    for (int i = 0; i < 8; ++i) AddText(pages[0], "- extra " + std::to_string(i + 1), 72, 520 - 12.0f * i, 10);
    for (int i = 0; i < 10; ++i)
        AddText(pages[0], "Right prose line " + std::to_string(i) + " alpha beta gamma.", 330, 700 - 11.0f * i, 10);
    const std::string md = Run(pages);
    const size_t lastItem = Find(md, "item 12"), prose = Find(md, "Right prose line 0");
    RAYOPDF_CHECK(lastItem != std::string::npos && prose != std::string::npos, 762);
    RAYOPDF_CHECK(lastItem < prose, 763);
}

} // namespace

// A nearly empty page (a scanned page's text layer: a title line and a repository footer) loses
// its URL footer; its other lines stay.
void TestSparseMetadata() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "New Directions, Vol. 11, Art. 4", 242, 763, 10);
    AddText(pages[0], "http://dh.example.edu/newdirections/vol11/iss4/4", 22, 22, 10);
    AddText(pages[0], "NEW DIRECTIONS JULY 1984 2", 489, 19, 6);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "dh.example.edu") == std::string::npos, 764);
    RAYOPDF_CHECK(Find(md, "New Directions, Vol. 11") != std::string::npos, 765);
}

// A lone page number a little further in than the running band, set apart from the text, is
// still a page number.
void TestInnerFolio() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 30; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of a page with deep margins here.", 90,
                650 - 12.0f * i, 10);
    AddText(pages[0], "259", 300, 105, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "259") == std::string::npos, 766);
    RAYOPDF_CHECK(Find(md, "Body line 29 of") != std::string::npos, 767);
}

// A running title set off the text's left edge (right-aligned) inside the running band is removed;
// a footer whose folio sits on a slightly lower baseline beside it goes with it.
void TestOffsetRows() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Effects of Red Fruit Oil on Colitis Cancer", 330, 720, 10);
    for (int i = 0; i < 30; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the article text runs on.", 72, 690 - 12.0f * i, 10);
    AddText(pages[0], "J Example Med Assoc, Volume 63, Number 3, 2013", 330, 46, 8);
    AddText(pages[0], "110", 72, 43, 8);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Red Fruit Oil") == std::string::npos, 768);
    RAYOPDF_CHECK(Find(md, "Example Med Assoc") == std::string::npos, 769);
    RAYOPDF_CHECK(Find(md, "Body line 0 of") != std::string::npos && Find(md, "Body line 29 of") != std::string::npos, 770);
}

int main() {
    TestFolioHeader();
    TestLabelColumn();
    TestFormulaColumn();
    TestDoubleSpacedTop();
    TestMetadataBlock();
    TestListColumn();
    TestSparseMetadata();
    TestInnerFolio();
    TestOffsetRows();
    std::printf("test_layout_order: ok\n");
    return 0;
}
