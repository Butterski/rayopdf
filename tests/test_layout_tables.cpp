// SPDX-License-Identifier: Apache-2.0
// Core tests of src/layout/tables: lattice, partially ruled and borderless tables on pages built
// glyph by glyph, the false tables that must not appear (prose in two columns, lists), a table
// between columns of text, and hostile rulings. Exit codes 550-599, 850-899, 1700-1749 and
// 2300-2349.
//
// `test_layout_tables --dump FILE.pdf [PAGE]` prints the rulings and tables found on the pages of
// a PDF (a development aid; RAYOPDF_DUMP_LATEX=1 prepares the glyphs as --math=latex does).
#include "check.h"
#include "pdf_builder.h"

#include "common/file.h"
#include "common/utf8.h"
#include "layout/figures.h"
#include "layout/interpreter.h"
#include "layout/lines.h"
#include "layout/math.h"
#include "layout/page.h"
#include "layout/pipeline.h"
#include "layout/reading_order.h"
#include "layout/tables.h"
#include "layout/word_box.h"
#include "text/font.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

// Appends `text` to `page` as glyphs of `size` points from (x, y), each `advance` em wide; a
// space is a space glyph. Returns the x after the text.
float AddText(PageData& page, std::string_view text, float x, float y, float size = 10, uint16_t flags = 0,
              float advance = 0.5f) {
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        const float w = advance * size;
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

void HLine(PageData& page, float x0, float x1, float y, float width = 0.5f) {
    page.rulings.push_back(Ruling{x0, y - width / 2, x1, y + width / 2, kRulingStroke});
}

void VLine(PageData& page, float x, float y0, float y1, float width = 0.5f) {
    page.rulings.push_back(Ruling{x - width / 2, y0, x + width / 2, y1, kRulingStroke});
}

PageData NewPage(int number = 1) {
    PageData page;
    page.number = number;
    page.width = 612;
    page.height = 792;
    return page;
}

std::string Run(std::vector<PageData>& pages, ConvertReport* reportOut = nullptr, MathMode math = MathMode::Unicode) {
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report, nullptr, nullptr, math);
    if (reportOut) *reportOut = report;
    return md;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int Count(const std::string& haystack, const std::string& needle) {
    int n = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) ++n;
    return n;
}

void Show(const char* name, const std::string& md) {
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", name, md.c_str());
}

// Prose filling [x0, x0 + width) from y down, `lines` lines of words `space` em apart.
float AddProse(PageData& page, float x0, float width, float y, int lines, float size = 10, float space = 0.5f) {
    static const char* const kWords[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dogs", "while",
                                         "seven", "wizards", "quietly", "examine", "every", "document"};
    int w = 0;
    for (int l = 0; l < lines; ++l) {
        float x = x0;
        for (;;) {
            const char* word = kWords[w % 15];
            const float advance = (float(std::strlen(word)) * 0.5f + space) * size;
            if (x + advance > x0 + width) break;
            x = AddText(page, word, x, y, size);
            x = AddText(page, " ", x, y, size, 0, space);
            ++w;
        }
        y -= 1.2f * size;
    }
    return y;
}

// A 3 x 3 lattice with a header cell spanning the last two columns: a pipe table with the header
// label in both columns; with a body cell spanning them too, that cell's text stands in both. Under
// a header of two rows (an empty corner, sub-labels under "Results") the body span needs HTML.
void TestLatticeSpans() {
    for (int variant = 0; variant < 3; ++variant) {
        const bool bodySpan = variant > 0, levels = variant == 2;
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        AddText(page, "Intro text before the table.", 72, 740);
        const float xs[] = {72, 200, 320, 440};
        const float top = levels ? 720 : 700;
        const float ys[] = {720, 700, 680, 660, 640, 620};
        for (float y : ys)
            if (y <= top) HLine(page, 72, 440, y);
        VLine(page, 72, 620, top);
        VLine(page, 200, 620, top);
        // x = 320: missing in the header row ("Results" spans two columns), and in Beta's row with a
        // body span; under a header of two rows the sub-labels under "Results" have it.
        VLine(page, 320, 660, 680);
        if (!bodySpan) VLine(page, 320, 640, 660);
        VLine(page, 320, 620, 640);
        if (levels) VLine(page, 320, 680, 700);
        VLine(page, 440, 620, top);
        if (levels) {
            AddText(page, "Results", xs[1] + 4, 706);
            AddText(page, "Mean", xs[1] + 4, 686);
            AddText(page, "Max", xs[2] + 4, 686);
        } else {
            AddText(page, "Name", xs[0] + 4, 686);
            AddText(page, "Results", xs[1] + 4, 686);
        }
        AddText(page, "Alpha", xs[0] + 4, 666);
        AddText(page, "12", xs[1] + 4, 666);
        AddText(page, "34", xs[2] + 4, 666);
        AddText(page, "Beta", xs[0] + 4, 646);
        AddText(page, "56", xs[1] + 4, 646);
        if (!bodySpan) AddText(page, "78", xs[2] + 4, 646);
        AddText(page, "Gamma", xs[0] + 4, 626);
        AddText(page, "90", xs[1] + 4, 626);
        AddText(page, "11", xs[2] + 4, 626);
        AddText(page, "Text after the table.", 72, 600);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("lattice spans", md);
        RAYOPDF_CHECK(report.tables == 1, 550);
        RAYOPDF_CHECK(Count(md, "Alpha") == 1, 554);
        if (!levels) {
            RAYOPDF_CHECK(Contains(md, std::string("| Name | Results | Results |\n| --- | --- | --- |\n| Alpha | 12 | 34 |\n| Beta | 56 | ") +
                                           (bodySpan ? "56" : "78") + " |\n| Gamma | 90 | 11 |"),
                          887);
            continue;
        }
        RAYOPDF_CHECK(Contains(md, "<th colspan=\"2\">Results</th>") && Contains(md, "<td colspan=\"2\">56</td>"), 551);
        RAYOPDF_CHECK(Contains(md, "<tr><td>Alpha</td><td>12</td><td>34</td></tr>"), 552);
        RAYOPDF_CHECK(md.find("Intro text") < md.find("<table>") && md.find("</table>") < md.find("Text after"), 553);
    }
}

// Cells spanning rows in a lattice of three columns: "Method" spans the two header rows; a pipe
// table writes it in the first, the cell below it empty. "Group A" spanning the first two body
// rows needs HTML; an empty cell spanning them does not.
void TestLatticeRowSpans() {
    for (int variant = 0; variant < 3; ++variant) {
        const bool bodySpan = variant > 0, empty = variant == 2;
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        // Rules: none across the first column at y = 700 ("Method" spans the header rows), nor at
        // y = 660 with a body span.
        for (float y : {720.0f, 680.0f, 640.0f, 620.0f}) HLine(page, 72, 440, y);
        HLine(page, bodySpan ? 200 : 72, 440, 660);
        HLine(page, 200, 440, 700);
        VLine(page, 72, 620, 720);
        VLine(page, 200, 620, 720);
        VLine(page, 440, 620, 720);
        VLine(page, 320, 620, 700);
        AddText(page, "Method", 76, 696);
        AddText(page, "Results", 204, 706);
        AddText(page, "Mean", 204, 686);
        AddText(page, "Max", 324, 686);
        if (!empty) AddText(page, "Group A", 76, bodySpan ? 656 : 666);
        AddText(page, "12", 204, 666);
        AddText(page, "56", 204, 646);
        AddText(page, "Gamma", 76, 626);
        AddText(page, "90", 204, 626);
        AddText(page, "34", 324, 666);
        AddText(page, "78", 324, 646);
        AddText(page, "11", 324, 626);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("lattice row spans", md);
        RAYOPDF_CHECK(report.tables == 1, 1716);
        if (!bodySpan) {
            RAYOPDF_CHECK(Contains(md, "| Method | Results | Results |\n| --- | --- | --- |\n|  | Mean | Max |\n"
                                       "| Group A | 12 | 34 |\n|  | 56 | 78 |\n| Gamma | 90 | 11 |"),
                          1717);
        } else if (!empty) {
            RAYOPDF_CHECK(Contains(md, "<th rowspan=\"2\">Method</th>") && Contains(md, "<td rowspan=\"2\">Group A</td><td>12</td>") &&
                              !Contains(md, "| --- |"),
                          1718);
        } else {
            RAYOPDF_CHECK(Contains(md, "| Method | Results | Results |\n| --- | --- | --- |\n|  | Mean | Max |\n"
                                       "|  | 12 | 34 |\n|  | 56 | 78 |\n| Gamma | 90 | 11 |"),
                          1719);
        }
    }
}

// A lattice whose rule between two body rows is missing under the first column only: the label
// written there one line a row ("Baseline distance" over "of 3 meters", as LaTeX sets a label over
// two rows with \cline under the other columns) is a cell in each row, and the table a pipe
// table. One line set between the two rows ("Group") still spans them.
void TestSpanOneLineARow() {
    for (const bool twoLines : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        for (float y : {720.0f, 700.0f, 660.0f, 640.0f}) HLine(page, 72, 440, y);
        HLine(page, 200, 440, 680);
        for (float x : {72.0f, 200.0f, 320.0f, 440.0f}) VLine(page, x, 640, 720);
        AddText(page, "Setting", 76, 706);
        AddText(page, "Mean", 204, 706);
        AddText(page, "Max", 324, 706);
        if (twoLines) {
            AddText(page, "Baseline distance", 76, 686);
            AddText(page, "of 3 meters", 76, 666);
        } else {
            AddText(page, "Group", 76, 676);
        }
        AddText(page, "12", 204, 686);
        AddText(page, "34", 324, 686);
        AddText(page, "56", 204, 666);
        AddText(page, "78", 324, 666);
        AddText(page, "Gamma", 76, 646);
        AddText(page, "90", 204, 646);
        AddText(page, "11", 324, 646);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("span one line a row", md);
        if (twoLines) {
            RAYOPDF_CHECK(report.tables == 1 && !Contains(md, "<table") &&
                              Contains(md, "| Setting | Mean | Max |\n| --- | --- | --- |\n| Baseline distance | 12 | 34 |\n"
                                           "| of 3 meters | 56 | 78 |\n| Gamma | 90 | 11 |"),
                          1728);
        } else {
            RAYOPDF_CHECK(report.tables == 1 && Contains(md, "<td rowspan=\"2\">Group</td><td>12</td>"), 1729);
        }
    }
}

// A full lattice without spans: a GFM pipe table; a `|` in a cell is escaped.
void TestLatticeGfm() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f}) HLine(page, 72, 300, y);
    for (float x : {72.0f, 180.0f, 300.0f}) VLine(page, x, 660, 700);
    AddText(page, "Key", 76, 686);
    AddText(page, "Value", 184, 686);
    AddText(page, "a|b", 76, 666);
    AddText(page, "1.", 184, 666);
    const std::string md = Run(pages);
    Show("lattice gfm", md);
    RAYOPDF_CHECK(Contains(md, "| Key | Value |\n| --- | --- |\n| a\\|b | 1. |"), 555);
}

// Stars and underscores in pipe-table cells: a cell's edges are whitespace to CommonMark, so a
// closing or lone run there is written as it is, an opening one escaped; a run beside an escaped
// pipe or backslash as well; stars around spaces are no emphasis the table's styling would strip.
void TestCellEmphasisEscapes() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f, 640.0f, 620.0f, 600.0f}) HLine(page, 72, 300, y);
    for (float x : {72.0f, 180.0f, 300.0f}) VLine(page, x, 600, 700);
    const char* const cells[5][2] = {{"Key", "Value"}, {"***", "0.05**"}, {"a|**", "**b"}, {"** x **", "x\\**"}, {"x_", "_x"}};
    for (int r = 0; r < 5; ++r) {
        AddText(page, cells[r][0], 76, 686 - 20.0f * float(r));
        AddText(page, cells[r][1], 184, 686 - 20.0f * float(r));
    }
    const std::string md = Run(pages);
    Show("cell emphasis escapes", md);
    RAYOPDF_CHECK(Contains(md, "| Key | Value |\n| --- | --- |\n| *** | 0.05** |\n"), 1713);
    RAYOPDF_CHECK(Contains(md, "| a\\|** | \\*\\*b |\n| ** x ** | x\\\\** |\n"), 1714);
    RAYOPDF_CHECK(Contains(md, "| x_ | \\_x |"), 1715);
}

// Bold over a whole body cell that no other body cell of its row or column shares (a word set
// bold in a column of examples, under a bold header) is emphasis inside the table and stays; a
// bold column of row labels is the table's styling and goes, and so does the header's bold.
void TestLoneCellEmphasis() {
    for (const bool labels : {false, true}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        for (float y : {700.0f, 680.0f, 660.0f, 640.0f}) HLine(page, 72, 300, y);
        for (float x : {72.0f, 180.0f, 300.0f}) VLine(page, x, 640, 700);
        AddText(page, "Feature", 76, 686, 10, kGlyphBold);
        AddText(page, "Example", 184, 686, 10, kGlyphBold);
        AddText(page, "Bold", 76, 666, 10, labels ? kGlyphBold : 0);
        AddText(page, "important", 184, 666, 10, labels ? 0 : kGlyphBold);
        AddText(page, "Code", 76, 646, 10, labels ? kGlyphBold : 0);
        AddText(page, "plain", 184, 646);
        const std::string md = Run(pages);
        Show(labels ? "bold labels" : "lone bold cell", md);
        if (labels) RAYOPDF_CHECK(Contains(md, "| Feature | Example |\n| --- | --- |\n| Bold | important |\n| Code | plain |"), 1740);
        else RAYOPDF_CHECK(Contains(md, "| Feature | Example |\n| --- | --- |\n| Bold | **important** |\n| Code | plain |"), 1737);
    }
}

// Booktabs: top, mid and bottom rules only; columns from whitespace; header above the mid rule.
void TestBooktabs() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 740, 2);
    HLine(page, 72, 400, 712, 0.8f);
    HLine(page, 72, 400, 694, 0.5f);
    HLine(page, 72, 400, 628, 0.8f);
    AddText(page, "Region", 76, 700, 10, kGlyphBold);
    AddText(page, "Q1", 220, 700, 10, kGlyphBold);
    AddText(page, "Q2", 320, 700, 10, kGlyphBold);
    const char* const names[] = {"North", "South", "East", "West"};
    const char* const q1[] = {"1,200", "980", "1,050", "770"};
    const char* const q2[] = {"1,340", "1,010", "990", "805"};
    for (int r = 0; r < 4; ++r) {
        const float y = 682 - 14.0f * float(r);
        AddText(page, names[r], 76, y);
        AddText(page, q1[r], 220, y);
        AddText(page, q2[r], 320, y);
    }
    AddProse(page, 72, 400, 600, 2);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("booktabs", md);
    RAYOPDF_CHECK(report.tables == 1, 556);
    // Bold over a whole header cell is the table's styling: no emphasis written.
    RAYOPDF_CHECK(Contains(md, "| Region | Q1 | Q2 |\n| --- | --- | --- |\n| North | 1,200 | 1,340 |"), 557);
    RAYOPDF_CHECK(Contains(md, "| West | 770 | 805 |"), 558);
}

// A header rule only (a word processor's table style): header above it, body rows below it, until
// running text resumes.
void TestHeaderRule() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "Item", 76, 700);
    AddText(page, "Count", 250, 700);
    AddText(page, "Price", 350, 700);
    HLine(page, 72, 420, 695);
    const char* const items[] = {"Apples", "Pears", "Plums"};
    for (int r = 0; r < 3; ++r) {
        const float y = 682 - 14.0f * float(r);
        AddText(page, items[r], 76, y);
        AddText(page, std::to_string(10 + r), 250, y);
        AddText(page, std::to_string(3 * r + 1) + ".50", 350, y);
    }
    AddProse(page, 72, 400, 630, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("header rule", md);
    RAYOPDF_CHECK(report.tables == 1, 559);
    RAYOPDF_CHECK(Contains(md, "| Item | Count | Price |\n| --- | --- | --- |\n| Apples | 10 | 1.50 |"), 560);
    RAYOPDF_CHECK(Contains(md, "| Plums | 12 | 7.50 |\n\nthe quick"), 561);
}

// A borderless numeric table between paragraphs.
void TestBorderless() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    float y = AddProse(page, 72, 450, 740, 3);
    y -= 10;
    AddText(page, "Country", 72, y, 10, kGlyphBold);
    AddText(page, "2021", 250, y, 10, kGlyphBold);
    AddText(page, "2022", 330, y, 10, kGlyphBold);
    AddText(page, "Change", 410, y, 10, kGlyphBold);
    const char* const names[] = {"Austria", "Belgium", "Croatia", "Denmark", "Estonia"};
    for (int r = 0; r < 5; ++r) {
        y -= 14;
        AddText(page, names[r], 72, y);
        AddText(page, std::to_string(1200 + 37 * r), 250, y);
        AddText(page, std::to_string(1300 + 29 * r), 330, y);
        AddText(page, std::to_string(r + 2) + ".1%", 410, y);
    }
    AddProse(page, 72, 450, y - 24, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("borderless", md);
    RAYOPDF_CHECK(report.tables == 1, 562);
    RAYOPDF_CHECK(Contains(md, "| Austria | 1200 | 1300 | 2.1% |"), 563);
}

// The labels of a plot stand in rows and columns like a borderless table. Over a drawing (curves
// of many segments, tick marks) they are the figure's text: a figure region, no table, the text
// kept. The same rows inside frames (a box with rounded corners around them, a shading under each
// row) are a table: a box that holds text is no drawing.
void TestFigureText() {
    const auto build = [](bool plot) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        float y = AddProse(page, 72, 450, 740, 3) - 10;
        const float top = y + 10;
        const char* const names[] = {"Austria", "Belgium", "Croatia", "Denmark", "Estonia"};
        for (int r = 0; r < 5; ++r) {
            y -= 14;
            AddText(page, names[r], 72, y);
            AddText(page, std::to_string(1200 + 37 * r), 250, y);
            AddText(page, std::to_string(1300 + 29 * r), 330, y);
            AddText(page, std::to_string(r + 2) + ".1%", 410, y);
            if (!plot) {
                Shape shade;
                shade.x0 = 70, shade.x1 = 452, shade.y0 = y - 3, shade.y1 = y + 10;
                shade.kind = ShapeKind::Rect, shade.filled = true, shade.segments = 4;
                page.shapes.push_back(shade);
            }
        }
        // A curve across the labels (its box holds them), or a box with rounded corners around them.
        Shape s;
        s.x0 = 66, s.x1 = 456, s.y0 = y - 12, s.y1 = top;
        s.kind = ShapeKind::Other, s.stroked = true, s.curved = true;
        s.segments = plot ? 40 : 8;
        page.shapes.push_back(s);
        // Ten ticks along the lower edge.
        for (int k = 0; k < 10 && plot; ++k) {
            Shape tick;
            tick.x0 = tick.x1 = 70 + 40.0f * float(k), tick.y0 = y - 10, tick.y1 = y - 7;
            tick.kind = ShapeKind::Line, tick.stroked = true, tick.segments = 1;
            page.shapes.push_back(tick);
        }
        AddProse(page, 72, 450, y - 30, 3);
        return pages;
    };
    std::vector<PageData> plot = build(true), framed = build(false);
    std::vector<FigureRegion> regions;
    FigureRegions(plot[0], regions);
    RAYOPDF_CHECK(regions.size() == 1 && regions[0].x0 <= 70 && regions[0].x1 >= 430, 866);
    FigureRegions(framed[0], regions);
    RAYOPDF_CHECK(regions.empty(), 867);
    ConvertReport report;
    std::string md = Run(plot, &report);
    Show("figure text", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "Austria") && Contains(md, "1348"), 868);
    md = Run(framed, &report);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Austria | 1200 | 1300 | 2.1% |"), 869);
}

// On a page with a figure's caption, the text standing in or beside the plot is the figure's own
// (tick labels, the rows of names and numbers). It is kept by default (FigureText::Keep) and left
// out with Drop, where what reads as prose there stays: a paragraph of two lines just above the
// plot (both within two ems of it), or in its place a short line at the body size that ends a
// sentence ("Shares by country:"), a subfigure's caption inside the plot, the caption under it,
// and the paragraph below.
void TestFigureLabels() {
    const auto build = [](bool brief) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        if (brief) {
            AddText(page, "Shares by country:", 72, 680);
        } else {
            AddText(page, "Our survey counted the people of five countries over ten years,", 72, 692);
            AddText(page, "and the plot below shows how each of them has grown since then.", 72, 680);
        }
        const char* const names[] = {"Austria", "Belgium", "Croatia", "Denmark", "Estonia"};
        for (int r = 0; r < 5; ++r) {
            const float y = 660 - 14.0f * float(r);
            AddText(page, names[r], 72, y);
            AddText(page, std::to_string(1200 + 37 * r), 250, y);
            AddText(page, std::to_string(1300 + 29 * r), 330, y);
            AddText(page, std::to_string(r + 2) + ".1%", 410, y);
        }
        AddText(page, "(a) Growth of the population", 72, 594);
        Shape s;
        s.x0 = 66, s.x1 = 456, s.y0 = 584, s.y1 = 676;
        s.kind = ShapeKind::Other, s.stroked = true, s.curved = true, s.segments = 40;
        page.shapes.push_back(s);
        for (int k = 0; k < 10; ++k) {
            Shape tick;
            tick.x0 = tick.x1 = 70 + 40.0f * float(k), tick.y0 = 584, tick.y1 = 587;
            tick.kind = ShapeKind::Line, tick.stroked = true, tick.segments = 1;
            page.shapes.push_back(tick);
        }
        AddText(page, "Figure 1: Population of five countries.", 72, 566);
        AddProse(page, 72, 450, 540, 3);
        return pages;
    };
    const auto convert = [](std::vector<PageData>& pages, FigureText figureText) {
        std::string md;
        ConvertReport report;
        std::vector<PageTextQuality> quality(pages.size());
        OutputSettings output;
        output.figureText = figureText;
        ConvertPages(pages, quality, {}, false, md, report, nullptr, nullptr, MathMode::Unicode, nullptr, nullptr, &output);
        return md;
    };
    std::vector<PageData> pages = build(false);
    std::string md = convert(pages, FigureText::Keep);
    RAYOPDF_CHECK(Contains(md, "Austria") && Contains(md, "1348") && Contains(md, "2.1%"), 2323);
    pages = build(false);
    md = convert(pages, FigureText::Drop);
    Show("figure labels", md);
    RAYOPDF_CHECK(!Contains(md, "Austria") && !Contains(md, "1348") && !Contains(md, "2.1%"), 2320);
    RAYOPDF_CHECK(Contains(md, "over ten years, and the plot below") && Contains(md, "Figure 1: Population of five") &&
                      Contains(md, "(a) Growth of the population") && Contains(md, "wizards quietly examine"),
                  2321);
    // The paragraph above stands wholly within the figure's reach: the prose test keeps it.
    int flagged = 0;
    for (const Line& l : pages[0].lines) flagged += (l.flags & kLineFigure) && l.u1 - l.u0 > 300;
    RAYOPDF_CHECK(flagged == 2, 2322);
    pages = build(true);
    md = convert(pages, FigureText::Drop);
    flagged = 0;
    for (const Line& l : pages[0].lines) flagged += (l.flags & kLineFigure) && l.u1 - l.u0 < 100 && l.vb > 670;
    RAYOPDF_CHECK(flagged == 1 && Contains(md, "Shares by country:") && !Contains(md, "Austria"), 2324);
}

// Figure regions from marks whose cells touch only at their corners: eight marks set corner to
// corner on a diagonal are one cluster that weighs enough; the same eight with one empty cell
// between the fourth and the fifth are two that do not. Six hundred long slanted strokes far
// from them (no marks, no curves) cover so many cells over each other that the cells are counted
// instead of painted: the regions stay the same. What the page's strokes make is kept from the
// first search, but a curve weighs against the text as it is at each search: four marks and a
// curve are a region, and are none once a line of text stands inside the curve.
void TestFigureCorners() {
    const auto build = [](bool crowded) {
        PageData page = NewPage();
        // A mark over the 3 x 3 cells (cx, cy) to (cx + 2, cy + 2) of the page's 4-point grid.
        const auto mark = [&](int cx, int cy) {
            Shape s;
            s.x0 = 4.0f * float(cx) + 0.5f, s.x1 = 4.0f * float(cx + 2) + 3.5f;
            s.y0 = 4.0f * float(cy) + 0.5f, s.y1 = 4.0f * float(cy + 2) + 3.5f;
            s.kind = ShapeKind::Other, s.stroked = true, s.segments = 3;
            page.shapes.push_back(s);
        };
        for (int k = 0; k < 8; ++k) mark(20 + 3 * k, 20 + 3 * k);
        for (int k = 0; k < 8; ++k) mark(60 + 3 * k + (k >= 4), 20 + 3 * k + (k >= 4));
        for (int k = 0; k < 600 && crowded; ++k) {
            Shape s;
            s.x0 = 300, s.x1 = 500, s.y0 = 400, s.y1 = 700;
            s.kind = ShapeKind::Line, s.stroked = true, s.segments = 1;
            page.shapes.push_back(s);
        }
        return page;
    };
    PageData sparse = build(false), crowded = build(true);
    std::vector<FigureRegion> regions, counted;
    FigureRegions(sparse, regions);
    RAYOPDF_CHECK(regions.size() == 1 && regions[0].x0 == 80.5f && regions[0].y0 == 80.5f && regions[0].x1 == 175.5f &&
                      regions[0].y1 == 175.5f,
                  879);
    FigureRegions(crowded, counted);
    RAYOPDF_CHECK(counted.size() == 1 && std::memcmp(&counted[0], &regions[0], sizeof(FigureRegion)) == 0, 888);
    FigureRegions(sparse, counted);
    RAYOPDF_CHECK(counted.size() == 1 && std::memcmp(&counted[0], &regions[0], sizeof(FigureRegion)) == 0, 889);

    PageData curve = NewPage();
    Shape s;
    s.x0 = 300, s.x1 = 400, s.y0 = 300, s.y1 = 400;
    s.kind = ShapeKind::Other, s.stroked = true, s.curved = true, s.segments = 20;
    curve.shapes.push_back(s);
    for (int k = 0; k < 4; ++k) {
        Shape m;
        m.x0 = 310 + 20.0f * float(k), m.x1 = m.x0 + 3, m.y0 = 310, m.y1 = 313;
        m.kind = ShapeKind::Other, m.stroked = true, m.segments = 3;
        curve.shapes.push_back(m);
    }
    FigureRegions(curve, regions);
    RAYOPDF_CHECK(regions.size() == 1 && regions[0].x0 == 300 && regions[0].x1 == 400, 890);
    Line text;
    text.u0 = 330, text.u1 = 370, text.vb = 345, text.vt = 355, text.size = 10, text.chars = 5;
    curve.lines.push_back(text);
    FigureRegions(curve, regions);
    RAYOPDF_CHECK(regions.empty(), 891);
}

// A box of rulings with text in its cells drawn inside a plot (a legend) is part of the figure;
// the same box alone is a table.
void TestLatticeInFigure() {
    const auto build = [](bool plot) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        for (float y : {600.0f, 584.0f, 568.0f, 552.0f}) HLine(page, 200, 360, y);
        for (float x : {200.0f, 280.0f, 360.0f}) VLine(page, x, 552, 600);
        const char* const cells[] = {"Label", "Value", "alpha", "0.51", "beta", "0.52"};
        for (int k = 0; k < 6; ++k) AddText(page, cells[k], k % 2 ? 284.0f : 204.0f, 588 - 16.0f * float(k / 2));
        if (plot) {
            Shape s;
            s.x0 = 150, s.x1 = 420, s.y0 = 520, s.y1 = 640;
            s.kind = ShapeKind::Other, s.stroked = true, s.curved = true, s.segments = 40;
            page.shapes.push_back(s);
            for (int k = 0; k < 10; ++k) {
                Shape tick;
                tick.x0 = tick.x1 = 155 + 26.0f * float(k), tick.y0 = 522, tick.y1 = 525;
                tick.kind = ShapeKind::Line, tick.stroked = true, tick.segments = 1;
                page.shapes.push_back(tick);
            }
        }
        return pages;
    };
    std::vector<PageData> plot = build(true), alone = build(false);
    ConvertReport report;
    std::string md = Run(plot, &report);
    Show("lattice in a figure", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "alpha"), 875);
    md = Run(alone, &report);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| alpha | 0.51 |"), 876);
}

// A heat map: a grid of filled cells, a count in each, with tick marks along its left side and
// under it (a chart's axes, one tick a row and a column) and category labels beside the ticks.
// The counts are the chart's text: no table, the counts and labels kept. The same grid without
// ticks is a table.
void TestHeatMapAxes() {
    for (const bool ticks : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        AddProse(page, 72, 450, 740, 3);
        const char* const counts[4][3] = {
            {"16510", "0", "928"}, {"0", "583", "10676"}, {"33254", "0", "1511"}, {"95335", "581", "0"}};
        const char* const rows[] = {"B101", "B102", "B103", "B104"};
        const char* const cols[] = {"Low", "Medium", "High"};
        const auto stroke = [&](float x0, float y0, float x1, float y1) {
            Shape s;
            s.x0 = x0, s.y0 = y0, s.x1 = x1, s.y1 = y1;
            s.kind = ShapeKind::Line, s.stroked = true, s.segments = 1;
            page.shapes.push_back(s);
            page.rulings.push_back(Ruling{x0, y0, x1, y1, kRulingStroke});
        };
        for (int r = 0; r < 4; ++r) {
            const float y1 = 640 - 20.0f * float(r);
            for (int c = 0; c < 3; ++c) {
                const float x0 = 200 + 50.0f * float(c);
                Shape cell;
                cell.x0 = x0, cell.y0 = y1 - 20, cell.x1 = x0 + 50, cell.y1 = y1;
                cell.kind = ShapeKind::Rect, cell.filled = true, cell.segments = 4;
                page.shapes.push_back(cell);
                page.rulings.push_back(Ruling{x0, y1 - 20, x0 + 50, y1, kRulingFill});
                AddText(page, counts[r][c], x0 + 25 - 2.5f * float(std::strlen(counts[r][c])), y1 - 14);
            }
            if (ticks) stroke(196, y1 - 10, 200, y1 - 10);
            AddText(page, rows[r], 172, y1 - 14);
        }
        for (int c = 0; c < 3; ++c) {
            const float x = 225 + 50.0f * float(c);
            if (ticks) stroke(x, 556, x, 560);
            AddText(page, cols[c], x - 2.5f * float(std::strlen(cols[c])), 544);
        }
        AddProse(page, 72, 450, 500, 3);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(ticks ? "heat map" : "grid of counts", md);
        if (ticks) RAYOPDF_CHECK(report.tables == 0 && Contains(md, "33254") && Contains(md, "B103"), 1738);
        else RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| 33254 |"), 1739);
    }
}

// A ruling of a lattice running on past its top rule makes no empty row above the table.
void TestLatticeRulingPastTop() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {584.0f, 568.0f, 552.0f}) HLine(page, 200, 360, y);
    for (float x : {200.0f, 280.0f, 360.0f}) VLine(page, x, 552, 584);
    VLine(page, 280, 584, 600);
    const char* const cells[] = {"Label", "Value", "alpha", "0.51"};
    for (int k = 0; k < 4; ++k) AddText(page, cells[k], k % 2 ? 284.0f : 204.0f, 572 - 16.0f * float(k / 2));
    const std::string md = Run(pages);
    Show("lattice ruling past its top", md);
    RAYOPDF_CHECK(Contains(md, "| Label | Value |\n| --- | --- |\n| alpha | 0.51 |"), 877);
}

// A lattice whose body is one ruled row of two lines, each a label and numbers: two table rows.
void TestLatticeTwoLineRow() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {600.0f, 584.0f, 552.0f}) HLine(page, 100, 400, y);
    for (float x : {100.0f, 250.0f, 325.0f, 400.0f}) VLine(page, x, 552, 600);
    const char* const cells[] = {"Ratio", "Car", "Foot", "Node ratio", "0.52", "0.54", "Edge ratio", "0.64", "0.65"};
    const float xs[] = {104, 254, 329};
    for (int k = 0; k < 9; ++k) AddText(page, cells[k], xs[k % 3], k < 3 ? 588.0f : (k < 6 ? 571.0f : 559.0f));
    const std::string md = Run(pages);
    Show("lattice row of two lines", md);
    RAYOPDF_CHECK(Contains(md, "| Ratio | Car | Foot |\n| --- | --- | --- |\n| Node ratio | 0.52 | 0.54 |\n| Edge ratio | 0.64 | 0.65 |"), 878);
}

// Authors set in columns, each name over its affiliation and its e-mail address (one with a
// footnote mark): no table.
void TestAuthorBlock() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    const char* const cards[2][4] = {{"Ann Lee", "Department of Physics", "Oxford, UK", "ann.lee@ox.ac.uk\xE2\x88\x97"},
                                     {"Bo Chen", "Computing Division", "Berkeley, CA, USA", "bchen@lbl.gov"}};
    for (int c = 0; c < 2; ++c)
        for (int r = 0; r < 4; ++r) AddText(page, cards[c][r], c == 0 ? 100.0f : 330.0f, 700 - 12.0f * float(r));
    AddProse(page, 72, 450, 640, 4);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("author block", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "ann.lee@ox.ac.uk") && Contains(md, "bchen@lbl.gov"), 879);
}

// Rules between sections of a parameter list, the first section five lines: its lines are rows
// under the section's name, not a header of five lines.
void TestRuledSectionsNoHeader() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {712.0f, 644.0f, 600.0f}) HLine(page, 72, 330, y);
    const char* const lines[][2] = {{"Optics", ""},     {"Period", "4.0 cm"}, {"Field", "0.67 T"}, {"Wavelength", "10.3 um"},
                                    {"Power", "2.8 GW"}, {"Beam", ""},         {"Energy", "46 MeV"}, {"Charge", "50 pC"}};
    for (int k = 0; k < 8; ++k) {
        const float y = (k < 5 ? 700.0f : 696.0f) - 12.0f * float(k);
        AddText(page, lines[k][0], 76, y);
        if (*lines[k][1]) AddText(page, lines[k][1], 240, y);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("ruled sections", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Optics |  |\n| --- | --- |\n| Period | 4.0 cm |\n| Field | 0.67 T |"), 880);
    RAYOPDF_CHECK(Contains(md, "| Power | 2.8 GW |\n| Beam |  |\n| Energy | 46 MeV |"), 881);
}

// Regression statistics side by side in a ruled table ("(df=101;sig@0.000)"): an at sign between
// numbers is no e-mail address, the table stays.
void TestAtSignInCells() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 450, 740, 2);
    for (float y : {712.0f, 694.0f, 628.0f}) HLine(page, 72, 450, y);
    const char* const rows[][3] = {{"Factor", "Model I", "Model II"},
                                   {"Price", "-0.005", "0.420"},
                                   {"Points", "0.001", "0.002"},
                                   {"Customers", "1.002", "0.616"},
                                   {"Fit", "(df=101;sig@0.000)", "(df=99;sig@0.001)"}};
    for (int r = 0; r < 5; ++r) {
        const float y = r == 0 ? 700.0f : 682 - 14.0f * float(r - 1);
        for (int c = 0; c < 3; ++c) AddText(page, rows[r][c], 76 + 130.0f * float(c), y);
    }
    AddProse(page, 72, 450, 600, 2);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("at sign in cells", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Fit | (df=101;sig@0.000) | (df=99;sig@0.001) |"), 882);
}

// A tall header between the top and the second rule, four lines of wrapped labels without a
// number: the header of the table, not rows of a section.
void TestTallHeader() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 450, 760, 2);
    for (float y : {732.0f, 680.0f, 614.0f}) HLine(page, 72, 450, y);
    const char* const head[][3] = {{"Group", "Participants", "Participants"},
                                   {"", "with no", "with a"},
                                   {"", "suicide", "suicide"},
                                   {"", "attempt", "attempt"}};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            if (*head[r][c]) AddText(page, head[r][c], 76 + 130.0f * float(c), 720 - 11.0f * float(r));
    const char* const body[][3] = {{"F0", "1", "0"}, {"F1", "17", "12"}, {"F2", "1", "0"}, {"F3", "106", "31"}};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) AddText(page, body[r][c], 76 + 130.0f * float(c), 668 - 14.0f * float(r));
    AddProse(page, 72, 450, 590, 2);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("tall header", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| --- | --- | --- |\n| F0 | 1 | 0 |"), 883);
}

// The same table drawn bottom row first, with a cell's word drawn twice at one origin in another
// text: its words still sort top to bottom (the tie leaves them to std::sort), and it stays a table.
void TestBorderlessWordOrder() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 450, 740, 3);
    const char* const names[] = {"Austria", "Belgium", "Croatia", "Denmark", "Estonia"};
    for (int r = 4; r >= 0; --r) {
        const float y = 680 - 14.0f * float(r);
        AddText(page, names[r], 72, y);
        AddText(page, std::to_string(1200 + 37 * r), 250, y);
        AddText(page, std::to_string(1300 + 29 * r), 330, y);
        AddText(page, std::to_string(r + 2) + ".1%", 410, y);
    }
    AddText(page, "9999", 330, 680 - 14.0f * 2);
    AddText(page, "Country", 72, 694, 10, kGlyphBold);
    AddText(page, "2021", 250, 694, 10, kGlyphBold);
    AddText(page, "2022", 330, 694, 10, kGlyphBold);
    AddText(page, "Change", 410, 694, 10, kGlyphBold);
    AddProse(page, 72, 450, 590, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("borderless word order", md);
    RAYOPDF_CHECK(report.tables == 1, 740);
    const size_t first = md.find("| Austria |"), last = md.find("| Estonia |");
    RAYOPDF_CHECK(first != std::string::npos && last != std::string::npos && first < last, 741);
}

// Two columns of running text with a rule above them: prose, not a table.
void TestTwoColumnProse() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "A Title Over Two Columns", 72, 740, 16);
    HLine(page, 72, 540, 730);
    AddProse(page, 72, 220, 712, 30);
    AddProse(page, 318, 220, 712, 30);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("two columns", md);
    RAYOPDF_CHECK(report.tables == 0, 564);
    RAYOPDF_CHECK(!Contains(md, "|") && !Contains(md, "<table"), 565);
}

// A list whose markers sit apart from the text, under a rule: a list, not a table.
void TestListNotTable() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 500, 712);
    for (int r = 0; r < 5; ++r) {
        const float y = 700 - 14.0f * float(r);
        AddText(page, std::to_string(r + 1) + ".", 72, y);
        AddText(page, "Install the package and check", 100, y);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("list", md);
    RAYOPDF_CHECK(report.tables == 0, 566);
}

// An algorithm box: a rule, the title, a rule, the steps with their line numbers ("3:") apart from
// the text, a rule. The steps of a listing, not a table.
void TestAlgorithmBox() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 400, 728);
    AddText(page, "Algorithm 1 Largest gap method", 72, 716);
    HLine(page, 72, 400, 712);
    const char* const steps[] = {"Sort the received data", "Calculate the gaps between points", "Find the largest gap",
                                 "Count the points to its right"};
    for (int r = 0; r < 4; ++r) {
        const float y = 700 - 12.0f * float(r);
        AddText(page, std::to_string(r + 1) + ":", 76, y, 8);
        AddText(page, steps[r], 92, y);
    }
    HLine(page, 72, 400, 658);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("algorithm box", md);
    RAYOPDF_CHECK(report.tables == 0, 595);
    RAYOPDF_CHECK(Contains(md, "Sort the received data") && Contains(md, "Count the points to its right"), 599);
}

// A ruled table in sections: between the rules around each section's rows, a band with one label
// centred across the table. One table with its header; each label is a row, its text in the first
// cell. A label band under the last rows heads nothing and stays out.
void TestRuledSectionLabels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {712.0f, 698.0f, 684.0f, 642.0f, 628.0f, 586.0f, 572.0f}) HLine(page, 72, 330, y);
    AddText(page, "Covariate", 76, 702);
    AddText(page, "Lag 0", 190, 702);
    AddText(page, "Lag 1", 270, 702);
    const char* const names[] = {"NDVI", "NDWI", "Temp"};
    for (int s = 0; s < 2; ++s) {
        const float top = s == 0 ? 688.0f : 632.0f;
        AddText(page, s == 0 ? "Batch 1" : "Batch 2", 185, top);
        for (int r = 0; r < 3; ++r) {
            const float y = top - 16 - 12.0f * float(r);
            AddText(page, names[r], 76, y);
            AddText(page, "0." + std::to_string(31 + 7 * r + 40 * s), 190, y);
            AddText(page, "0." + std::to_string(12 + 5 * r + 40 * s), 270, y);
        }
    }
    AddText(page, "Values of the correlation", 150, 576);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("ruled section labels", md);
    RAYOPDF_CHECK(report.tables == 1, 862);
    RAYOPDF_CHECK(Contains(md, "| Covariate | Lag 0 | Lag 1 |\n| --- | --- | --- |\n| Batch 1 |  |  |\n| NDVI | 0.31 | 0.12 |"), 863);
    RAYOPDF_CHECK(Contains(md, "| Temp | 0.45 | 0.22 |\n| Batch 2 |  |  |\n| NDVI | 0.71 | 0.52 |"), 864);
    RAYOPDF_CHECK(!Contains(md, "| Values of the correlation") && Contains(md, "Values of the correlation"), 865);
}

// Booktabs with sections: a body line that is one label (its words a narrow word space apart),
// centred across the gap between two columns of figures that every other body line leaves open,
// names the rows under it. Each such line is a label row; the columns come from the other lines,
// not merged under the label.
void TestLoneSectionLabels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 740, 2);
    HLine(page, 72, 400, 712, 0.8f);
    HLine(page, 72, 400, 694, 0.5f);
    HLine(page, 72, 400, 590, 0.8f);
    AddText(page, "Setting", 76, 700);
    AddText(page, "N10", 200, 700);
    AddText(page, "N20", 260, 700);
    AddText(page, "N50", 320, 700);
    const char* const names[] = {"Model A", "Model B", "Model C"};
    for (int s = 0; s < 2; ++s) {
        const float top = 682 - 48.0f * float(s);
        float x = 150;
        for (const char* word : {s == 0 ? "Lower" : "Upper", "bounds", "of", "the", "error"}) x = AddText(page, word, x, top) + 3;
        for (int r = 0; r < 3; ++r) {
            const float y = top - 12 - 12.0f * float(r);
            AddText(page, names[r], 76, y);
            AddText(page, "0." + std::to_string(11 + r + 30 * s), 200, y);
            AddText(page, "0." + std::to_string(21 + r + 30 * s), 260, y);
            AddText(page, "0." + std::to_string(31 + r + 30 * s), 320, y);
        }
    }
    AddProse(page, 72, 400, 560, 2);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("lone section labels", md);
    RAYOPDF_CHECK(report.tables == 1, 1720);
    RAYOPDF_CHECK(Contains(md, "| Setting | N10 | N20 | N50 |\n| --- | --- | --- | --- |\n| Lower bounds of the error |  |  |  |\n"
                               "| Model A | 0.11 | 0.21 | 0.31 |"),
                  1721);
    RAYOPDF_CHECK(Contains(md, "| Model C | 0.13 | 0.23 | 0.33 |\n| Upper bounds of the error |  |  |  |\n| Model A | 0.41 | 0.51 | 0.61 |"),
                  1722);
}

// A ruled table with a rule around each group of rows: in a group, a line of numbers under a line
// of numbers is a row of its own (its first cell empty), while a line in smaller type right under
// a value (a standard error) is that value's second line and keeps the rule stack one table.
void TestRuledRowGroups() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {712.0f, 698.0f, 668.0f, 642.0f, 614.0f}) HLine(page, 72, 330, y);
    const auto line = [&](const char* name, const char* data, const char* a, const char* b, float y) {
        if (*name) AddText(page, name, 76, y);
        AddText(page, data, 150, y);
        AddText(page, a, 220, y);
        AddText(page, b, 280, y);
    };
    line("Model", "Data", "Score A", "Score B", 702);
    line("mbert", "Merged", "57.59", "74.83", 686);
    line("", "Mono", "61.45", "75.52", 674);
    line("Indic", "Merged", "59.62", "74.64", 656);
    AddText(page, "(0.03)", 220, 649, 7);
    line("Xlm", "Merged", "60.73", "78.58", 632);
    line("", "Mono", "60.97", "79.71", 620);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("ruled row groups", md);
    RAYOPDF_CHECK(report.tables == 1, 870);
    RAYOPDF_CHECK(Contains(md, "| mbert | Merged | 57.59 | 74.83 |\n|  | Mono | 61.45 | 75.52 |"), 871);
    RAYOPDF_CHECK(Contains(md, "| Indic | Merged | 59.62 (0.03) | 74.64 |\n| Xlm | Merged | 60.73 | 78.58 |"), 872);
    RAYOPDF_CHECK(Contains(md, "| Xlm | Merged | 60.73 | 78.58 |\n|  | Mono | 60.97 | 79.71 |"), 873);
}

// A booktabs table whose row labels are each centred over a group of four rows (LaTeX's
// \multirow), between the second and the third: the label belongs to a row and parts none, set
// half a row apart from the rows around it (closer than a line pitch, then wider than one).
void TestCentredRowLabels() {
    for (const bool wide : {false, true}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        const float size = wide ? 7.5f : 8.0f, pitch = wide ? 14.0f : 9.0f;
        const char* const labels[] = {"Compressor", "Vehicle"};
        const char* const methods[] = {"Central", "Local", "FedAvg", "FedMeta"};
        const char* const scores[2][4] = {{"0.637", "0.547", "0.602", "0.770"}, {"0.656", "0.646", "0.396", "0.607"}};
        HLine(page, 72, 330, 722, 0.8f);
        AddText(page, "Device", 76, 710, size);
        AddText(page, "Method", 150, 710, size);
        AddText(page, "F1", 230, 710, size);
        AddText(page, "MAE", 290, 710, size);
        float y = 704;
        for (int g = 0; g < 2; ++g) {
            HLine(page, 72, 330, y, 0.5f);
            const float first = y - pitch;
            for (int r = 0; r < 4; ++r) {
                const float base = first - pitch * float(r);
                AddText(page, methods[r], 150, base, size);
                AddText(page, scores[g][r], 230, base, size);
                AddText(page, r % 2 ? "0.1" : "0.2", 290, base, size);
            }
            AddText(page, labels[g], 76, first - 1.5f * pitch, size);
            y = first - 3 * pitch - 0.8f * pitch;
        }
        HLine(page, 72, 330, y, 0.8f);
        AddProse(page, 72, 400, y - 30, 2);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(wide ? "centred row labels, wide rows" : "centred row labels", md);
        const bool kept = report.tables == 1 && !Contains(md, "| Compressor |  |") && !Contains(md, "| Vehicle |  |") &&
                          Contains(md, "| Compressor | Local | 0.547 | 0.1 |\n|  | FedAvg | 0.602 | 0.2 |") &&
                          Contains(md, "| Vehicle | Local | 0.646 | 0.1 |\n|  | FedAvg | 0.396 | 0.2 |\n|  | FedMeta | 0.607 | 0.1 |");
        RAYOPDF_CHECK(kept, wide ? 1746 : 1745);
    }
}

// A booktabs table whose body leaves a blank line between two groups of rows: one table of every
// row. Under it, after a line of space, a table of the same columns with a header of its own (no
// number in its first row) stays a table of its own.
void TestBlankLineInBody() {
    for (const bool header : {false, true}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        AddProse(page, 72, 400, 760, 2);
        HLine(page, 72, 330, 732, 0.8f);
        AddText(page, "Name", 76, 720);
        AddText(page, "Redshift", 160, 720);
        AddText(page, "SFR", 260, 720);
        HLine(page, 72, 330, 714, 0.5f);
        const char* const names[] = {"3C 9", "3C 181", "3C 191", "4C 17.09", "4C 05.34", "4C 28.40"};
        const char* const z[] = {"2.014", "1.388", "1.954", "2.155", "2.876", "1.981"};
        const char* const sfr[] = {"310", "150", "300", "490", "750", "370"};
        float y = 702;
        for (int r = 0; r < 6; ++r) {
            if (r == 3) {
                y -= 13;  // the blank line
                if (header) {
                    AddText(page, "Source", 76, y);
                    AddText(page, "Epoch", 160, y);
                    AddText(page, "Flux", 260, y);
                    y -= 13;
                }
            }
            AddText(page, names[r], 76, y);
            AddText(page, z[r], 160, y);
            AddText(page, sfr[r], 260, y);
            y -= 13;
        }
        HLine(page, 72, 330, y + 6, 0.8f);
        AddProse(page, 72, 400, y - 24, 2);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(header ? "stacked tables" : "blank line in body", md);
        if (header) {
            RAYOPDF_CHECK(report.tables == 2 && Contains(md, "| Source | Epoch | Flux |\n| --- | --- | --- |\n| 4C 17.09 |"), 1748);
        } else {
            RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| 3C 191 | 1.954 | 300 |\n| 4C 17.09 | 2.155 | 490 |") &&
                              Count(md, "\n| --- |") == 1,
                          1747);
        }
    }
}

// A full-width ruled table between two columns of text: the columns are read before it, the table
// is one block, and the columns after it follow.
void TestTableAcrossColumns() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 220, 740, 6);
    AddProse(page, 318, 220, 740, 6);
    for (float y : {660.0f, 640.0f, 620.0f, 600.0f}) HLine(page, 72, 540, y);
    for (float x : {72.0f, 250.0f, 400.0f, 540.0f}) VLine(page, x, 600, 660);
    const char* const cells[3][3] = {{"Mode", "Speed", "Size"}, {"fast", "120", "4"}, {"small", "80", "2"}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) AddText(page, cells[r][c], (c == 0 ? 72.0f : c == 1 ? 250.0f : 400.0f) + 4, 646 - 20.0f * float(r));
    AddProse(page, 72, 220, 580, 6);
    AddProse(page, 318, 220, 580, 6);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("across columns", md);
    RAYOPDF_CHECK(report.tables == 1, 567);
    RAYOPDF_CHECK(Contains(md, "| Mode | Speed | Size |\n| --- | --- | --- |\n| fast | 120 | 4 |\n| small | 80 | 2 |"), 568);
    // Text before the table, then the table, then the text below it.
    const size_t table = md.find("| Mode");
    RAYOPDF_CHECK(table != std::string::npos && md.find("the quick") < table, 569);
    RAYOPDF_CHECK(md.rfind("the quick") > table, 570);
}

// Hostile rulings: empty, inverted, NaN, enormous and very many; bounded and quiet.
void TestHostileRulings() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 450, 740, 10);
    page.rulings.push_back(Ruling{100, 100, 100, 100, kRulingStroke});
    page.rulings.push_back(Ruling{200, 200, 100, 100, kRulingFill});
    page.rulings.push_back(Ruling{NAN, 0, 10, NAN, kRulingStroke});
    page.rulings.push_back(Ruling{-1e30f, 0, 1e30f, 1, kRulingStroke});
    page.rulings.push_back(Ruling{0, -1e30f, 1, 1e30f, kRulingStroke});
    // A dense mesh: 30000 horizontal and 30000 vertical hairlines over the page.
    for (int i = 0; i < 30000; ++i) HLine(page, 0, 612, float(i % 792), 0.1f);
    for (int i = 0; i < 30000; ++i) VLine(page, float(i % 612), 0, 792, 0.1f);
    const auto start = std::chrono::steady_clock::now();
    ConvertReport report;
    const std::string md = Run(pages, &report);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Show("hostile", md);
    RAYOPDF_CHECK(Contains(md, "the quick"), 571);
    RAYOPDF_CHECK(ms < 2000, 572);

    // A 600 x 600 lattice: past the row and column limits, dropped whole (fail closed).
    std::vector<PageData> big{NewPage()};
    for (int i = 0; i <= 600; ++i) HLine(big[0], 0, 600, float(i), 0.1f);
    for (int i = 0; i <= 600; ++i) VLine(big[0], float(i), 0, 600, 0.1f);
    for (int i = 0; i < 50; ++i) AddText(big[0], "x", float(i * 12) + 0.2f, float(i * 12) + 0.3f, 1.5f);
    ConvertReport bigReport;
    Run(big, &bigReport);
    RAYOPDF_CHECK(bigReport.tables == 0, 573);
}

// Malformed lattices: a frame with no inner edges, a single row, an empty grid: no tables.
void TestDegenerateGrids() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    // A frame around a paragraph.
    HLine(page, 60, 520, 750);
    HLine(page, 60, 520, 690);
    VLine(page, 60, 690, 750);
    VLine(page, 520, 690, 750);
    AddProse(page, 72, 440, 735, 3);
    // An empty 4 x 4 grid (a chart's gridlines).
    for (int i = 0; i < 5; ++i) HLine(page, 100, 300, 400 + 20.0f * float(i));
    for (int i = 0; i < 5; ++i) VLine(page, 100 + 50.0f * float(i), 400, 480);
    AddText(page, "Figure 1: a chart", 100, 380);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("degenerate", md);
    RAYOPDF_CHECK(report.tables == 0, 574);
    RAYOPDF_CHECK(Contains(md, "Figure 1: a chart"), 575);
}

// Displayed math: a fraction bar over italic letters and operators, a matrix below it: no table.
void TestFractionBar() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 450, 740, 2);
    AddText(page, "x =", 200, 680, 10, kGlyphItalic);
    AddText(page, "\xE2\x88\x92" "b \xC2\xB1", 230, 690, 10, kGlyphItalic);
    AddText(page, "b2 \xE2\x88\x92 4ac", 270, 690, 10, kGlyphItalic);
    HLine(page, 228, 330, 686);
    AddText(page, "2a", 272, 676, 10, kGlyphItalic);
    AddText(page, "x", 230, 660, 10, kGlyphItalic);
    AddText(page, "ax + by =", 270, 660, 10, kGlyphItalic);
    AddText(page, "y", 230, 648, 10, kGlyphItalic);
    AddText(page, "cx + dy", 270, 648, 10, kGlyphItalic);
    AddProse(page, 72, 450, 620, 2);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("fraction", md);
    RAYOPDF_CHECK(report.tables == 0, 576);
}

// A displayed fraction set in upright type, the formula's name beside its bar ("LT = 1 / (1/L1 +
// 1/L2 + 1/L3)"): three rows that part into columns under one rule are no table. A table of three
// rows under a header rule, prose a column away beside it, stays one.
void TestFractionBesideText() {
    {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        AddProse(page, 72, 450, 740, 3);
        AddText(page, "LT =", 100, 697);
        AddText(page, "1", 208, 706);
        HLine(page, 130, 290, 700);
        AddText(page, "1", 140, 688);
        AddText(page, "1", 208, 688);
        AddText(page, "1", 276, 688);
        AddText(page, "L1", 136, 676);
        AddText(page, "+", 172, 676);
        AddText(page, "L2", 204, 676);
        AddText(page, "+", 240, 676);
        AddText(page, "L3", 272, 676);
        AddProse(page, 72, 450, 650, 2);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("fraction beside text", md);
        RAYOPDF_CHECK(report.tables == 0 && Contains(md, "LT =") && Contains(md, "L3"), 1743);
    }
    {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        AddProse(page, 320, 220, 740, 8);
        AddText(page, "Method", 76, 704);
        AddText(page, "Score", 200, 704);
        HLine(page, 72, 280, 700);
        AddText(page, "Alpha", 76, 688);
        AddText(page, "2.10", 200, 688);
        AddText(page, "Beta", 76, 676);
        AddText(page, "3.40", 200, 676);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("table under a rule beside prose", md);
        RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Method | Score |\n| --- | --- |\n| Alpha | 2.10 |"), 1744);
    }
}

// Double rulings 3 pt apart around every column: the empty slivers between them are no columns.
void TestDoubleRules() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f, 640.0f}) HLine(page, 72, 375, y);
    for (float x : {72.0f, 172.0f, 272.0f, 372.0f}) {
        VLine(page, x, 640, 700);
        VLine(page, x + 3, 640, 700);
    }
    const char* const cells[3][3] = {{"Port", "Foreign", "Domestic"}, {"Manila", "2454", "6,125"}, {"Cebu", "1138", "79,500"}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) AddText(page, cells[r][c], 80 + 100.0f * float(c), 686 - 20.0f * float(r));
    const std::string md = Run(pages);
    Show("double rules", md);
    RAYOPDF_CHECK(Contains(md, "| Port | Foreign | Domestic |\n| --- | --- | --- |\n| Manila | 2454 | 6,125 |"), 577);
}

// A one-column grid of boxed rows: a table of one column; a single box is not.
void TestColumnLattice() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (int i = 0; i <= 4; ++i) HLine(page, 100, 400, 700 - 20.0f * float(i));
    VLine(page, 100, 620, 700);
    VLine(page, 400, 620, 700);
    const char* const rows[] = {"Competence Framework", "#1: Recycle", "#2: Lifecycle", "#3: Social"};
    for (int r = 0; r < 4; ++r) AddText(page, rows[r], 104, 686 - 20.0f * float(r));
    // A lone box around a note.
    HLine(page, 100, 400, 560);
    HLine(page, 100, 400, 520);
    VLine(page, 100, 520, 560);
    VLine(page, 400, 520, 560);
    AddText(page, "Note: a boxed remark.", 104, 536);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("column lattice", md);
    RAYOPDF_CHECK(report.tables == 1, 578);
    RAYOPDF_CHECK(Contains(md, "| Competence Framework |\n| --- |\n| #1: Recycle |"), 579);
}

// Shaded rows boxed one by one, white rows between them with no rulings: one table of all rows.
void TestStripes() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    const char* const names[] = {"Cambodia", "Indonesia", "Lao PDR", "Malaysia", "Myanmar"};
    for (int r = 0; r < 5; ++r) {
        const float top = 700 - 20.0f * float(r);
        if (r % 2 == 0) {
            page.rulings.push_back(Ruling{72, top - 20, 372, top, kRulingFill});
            for (float x : {172.0f, 272.0f}) VLine(page, x, top - 20, top);
        }
        AddText(page, names[r], 76, top - 14);
        AddText(page, std::to_string(10 + r) + ".5%", 176, top - 14);
        AddText(page, std::to_string(200 + 37 * r), 276, top - 14);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("stripes", md);
    RAYOPDF_CHECK(report.tables == 1, 580);
    RAYOPDF_CHECK(Contains(md, "| Indonesia | 11.5% | 237 |\n| Lao PDR | 12.5% | 274 |"), 581);
}

// A top and a bottom rule around a header and body rows whose second column is empty (a form):
// a table; the same rows without rules are not.
void TestClosedEmptyColumn() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 300, 712);
    AddText(page, "Added cation", 76, 700);
    AddText(page, "Relative size", 200, 700);
    const char* const ions[] = {"K+", "Na+", "Ca2+", "Al3+"};
    for (int r = 0; r < 4; ++r) AddText(page, ions[r], 76, 684 - 14.0f * float(r));
    HLine(page, 72, 300, 628);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("closed empty column", md);
    RAYOPDF_CHECK(report.tables == 1, 582);
    RAYOPDF_CHECK(Contains(md, "| Added cation | Relative size |\n| --- | --- |\n| K+ |  |"), 583);
}

// Booktabs whose header puts one label over two sub-columns: the body rows set the columns, the
// label spans them (written in both columns of a pipe table); dot leaders after a label are not
// cell text.
void TestSpanningHeaderLabel() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 400, 712);
    AddText(page, "Microbiota", 76, 700);
    // The label's words a word space (2.5 pt) apart.
    float x = 200;
    for (const char* word : {"Mean", "log", "DNA", "copies", "per", "gram"}) x = AddText(page, word, x, 700) + 2.5f;
    HLine(page, 72, 400, 694);
    const char* const names[] = {"Bifidobacterium ....", "Bacteroides .......", "Clostridium ......."};
    for (int r = 0; r < 3; ++r) {
        const float y = 682 - 14.0f * float(r);
        AddText(page, names[r], 76, y);
        AddText(page, std::to_string(4 + r) + ".79", 200, y);
        AddText(page, std::to_string(5 + r) + ".31", 300, y);
    }
    HLine(page, 72, 400, 638);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("spanning header label", md);
    RAYOPDF_CHECK(report.tables == 1, 584);
    RAYOPDF_CHECK(Contains(md, "| Microbiota | Mean log DNA copies per gram | Mean log DNA copies per gram |\n| --- | --- | --- |"), 585);
    RAYOPDF_CHECK(Contains(md, "| Bifidobacterium | 4.79 | 5.31 |"), 586);
}

// Appends `text` set upwards (baseline direction 1, page.h): in that direction's frame it starts
// at (u, v); on the page it runs up the line x = -v.
void AddTurnedText(PageData& page, std::string_view text, float u, float v, float size = 10) {
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        const float w = 0.5f * size;
        g.u0 = u, g.u1 = u + w, g.vb = v - 0.2f * size, g.vt = v + 0.8f * size, g.v = v;
        g.dir = 1;
        g.size = size;
        g.text = static_cast<uint32_t>(page.text.size());
        page.text.append(text.substr(at, length));
        g.textLength = static_cast<uint16_t>(length);
        if (cp == ' ') g.flags |= kGlyphSpace;
        page.glyphs.push_back(g);
        u += w;
        at += length;
    }
}

// A box with a rule under its header and none between the body rows: each line of the body,
// holding text in the first column and numbers, is a row.
void TestStackedLatticeRows() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 684.0f, 620.0f}) HLine(page, 72, 400, y);
    for (float x : {72.0f, 150.0f, 250.0f, 400.0f}) VLine(page, x, 620, 700);
    AddText(page, "Item", 76, 688);
    AddText(page, "Qty", 154, 688);
    AddText(page, "Part", 254, 688);
    const char* const cells[4][3] = {{"Bolt", "2", "31279"}, {"Nut", "4", "40380"}, {"Washer", "1", "80569"}, {"Pin", "3", "80124"}};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) AddText(page, cells[r][c], (c == 0 ? 76.0f : c == 1 ? 154.0f : 254.0f), 670 - 14.0f * float(r));
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("stacked lattice rows", md);
    RAYOPDF_CHECK(report.tables == 1, 587);
    RAYOPDF_CHECK(Contains(md, "| Item | Qty | Part |\n| --- | --- | --- |\n| Bolt | 2 | 31279 |\n| Nut | 4 | 40380 |\n"
                               "| Washer | 1 | 80569 |\n| Pin | 3 | 80124 |"),
                  588);
}

// A lattice ruled around a group of columns only: whitespace through every body row parts the
// group, and its label spans the parts.
void TestLatticeSubColumns() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 684.0f, 668.0f, 652.0f, 636.0f}) HLine(page, 72, 300, y);
    for (float x : {72.0f, 150.0f, 300.0f}) VLine(page, x, 636, 700);
    AddText(page, "Domain", 76, 688);
    AddText(page, "Results", 170, 688);
    const char* const names[] = {"Alpha", "Beta", "Gamma"};
    for (int r = 0; r < 3; ++r) {
        const float y = 672 - 16.0f * float(r);
        AddText(page, names[r], 76, y);
        AddText(page, std::to_string(12 + r), 154, y);
        AddText(page, "0." + std::to_string(51 + r), 230, y);
    }
    const std::string md = Run(pages);
    Show("lattice sub-columns", md);
    RAYOPDF_CHECK(Contains(md, "| Domain | Results | Results |\n| --- | --- | --- |\n| Alpha | 12 | 0.51 |\n| Beta | 13 | 0.52 |"), 589);
}

// The same lattice with a label over each part, the labels set closer than the parts' text: the
// boundary moves into the space between the labels, and each label heads its own column.
void TestLatticeSubColumnLabels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 684.0f, 668.0f, 652.0f, 636.0f}) HLine(page, 72, 300, y);
    for (float x : {72.0f, 150.0f, 300.0f}) VLine(page, x, 636, 700);
    AddText(page, "Domain", 76, 688);
    AddText(page, "Prior A", 154, 688);
    AddText(page, "Prior B", 196, 688);
    const char* const names[] = {"Alpha", "Beta", "Gamma"};
    for (int r = 0; r < 3; ++r) {
        const float y = 672 - 16.0f * float(r);
        AddText(page, names[r], 76, y);
        AddText(page, std::to_string(12 + r), 154, y);
        AddText(page, "0." + std::to_string(51 + r), 230, y);
    }
    const std::string md = Run(pages);
    Show("lattice sub-column labels", md);
    RAYOPDF_CHECK(Contains(md, "| Domain | Prior A | Prior B |\n| --- | --- | --- |\n| Alpha | 12 | 0.51 |"), 874);
}

// A box drawn around the body row alone, the header row right above it.
void TestHeaderAboveBox() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "Action Item", 76, 686);
    AddText(page, "Who Will Do", 204, 686);
    AddText(page, "Due Date", 304, 686);
    HLine(page, 72, 400, 680);
    HLine(page, 72, 400, 660);
    for (float x : {72.0f, 200.0f, 300.0f, 400.0f}) VLine(page, x, 660, 680);
    AddText(page, "Revise list", 76, 666);
    AddText(page, "Dr. Smith", 204, 666);
    AddText(page, "August 14", 304, 666);
    const std::string md = Run(pages);
    Show("header above box", md);
    RAYOPDF_CHECK(Contains(md, "| Action Item | Who Will Do | Due Date |\n| --- | --- | --- |\n| Revise list | Dr. Smith | August 14 |"),
                  590);
}

// A borderless table in sections: a label reaching into the first gap heads the rows after it,
// and figures of two columns stand closer than an em in a later row.
void TestSectionLabels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    float y = AddProse(page, 72, 450, 740, 3) - 10;
    const auto row = [&](const char* label, const char* a, const char* b, const char* c, const char* d) {
        AddText(page, label, 72, y);
        AddText(page, a, 160, y);
        AddText(page, b, 196, y);
        AddText(page, c, 240, y);
        AddText(page, d, 290, y);
        y -= 14;
    };
    row("Q3", "$448", "$427", "7%", "5%");
    row("9 Months", "$1,298", "$1,263", "5%", "3%");
    AddText(page, "Adjusted EPS*:", 64, y, 10, kGlyphBold);
    y -= 14;
    row("Q3", "0.84", "0.74", "22%", "14%");
    row("9 Months", "2.44", "2.12", "20%", "15%");
    AddProse(page, 72, 450, y - 20, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("section labels", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| 9 Months | $1,298 | $1,263 | 5% | 3% |\n"), 591);
    RAYOPDF_CHECK(Contains(md, "| Adjusted EPS\\*: |  |  |  |  |\n| Q3 | 0.84 | 0.74 | 22% | 14% |"), 592);
}

// Five name and affiliation pairs are a table, and a header row set further apart above them is
// its first row; five titles with page numbers beside them are a list of contents, not a table.
void TestPairsHeaderAndContents() {
    std::vector<PageData> pages{NewPage(), NewPage(2)};
    AddText(pages[0], "Name", 72, 704, 10, kGlyphBold);
    AddText(pages[0], "Affiliation", 240, 704, 10, kGlyphBold);
    const char* const pairs[5][2] = {{"Mick Heduan", "Transport for London"}, {"Edward Noble", "LB Merton"},
                                     {"David Kennett", "LB Wandsworth"},      {"Roseline Holt", "Sainsburys"},
                                     {"Paul Croxon", "LB Croydon"}};
    for (int r = 0; r < 5; ++r) {
        AddText(pages[0], pairs[r][0], 72, 682 - 13.0f * float(r));
        AddText(pages[0], pairs[r][1], 240, 682 - 13.0f * float(r));
    }
    const char* const contents[5][2] = {{"Acknowledgments", "v"}, {"About the Authors", "vii"}, {"Introduction", "ix"},
                                        {"Exploring Data", "xi"},  {"Missing Data", "xiv"}};
    AddProse(pages[1], 72, 400, 740, 2);
    for (int r = 0; r < 5; ++r) {
        AddText(pages[1], contents[r][0], 72, 700 - 13.0f * float(r));
        AddText(pages[1], contents[r][1], 300, 700 - 13.0f * float(r));
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("pairs header and contents", md);
    RAYOPDF_CHECK(Contains(md, "| Name | Affiliation |\n| --- | --- |\n| Mick Heduan | Transport for London |"), 593);
    RAYOPDF_CHECK(report.tables == 1 && !Contains(md, "| Introduction"), 598);
}

// A lattice with a title row inside its frame: the title and the column labels under it are the
// header, two rows of it: HTML, the title spanning the columns.
void TestLatticeTitleRow() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 684.0f, 668.0f, 652.0f, 636.0f, 620.0f}) HLine(page, 72, 400, y);
    VLine(page, 72, 620, 700);
    VLine(page, 400, 620, 700);
    for (float x : {250.0f, 320.0f}) VLine(page, x, 620, 684);
    AddText(page, "1st COURSE YEAR courses", 76, 688);
    AddText(page, "Learning activity", 76, 672);
    AddText(page, "Ects", 254, 672);
    AddText(page, "Sector", 324, 672);
    const char* const names[] = {"Labour Economics", "Labour Law", "Statistics"};
    for (int r = 0; r < 3; ++r) {
        AddText(page, names[r], 76, 656 - 16.0f * float(r));
        AddText(page, std::to_string(6 + 3 * r), 254, 656 - 16.0f * float(r));
        AddText(page, "SECS-P/0" + std::to_string(r + 1), 324, 656 - 16.0f * float(r));
    }
    const std::string md = Run(pages);
    Show("lattice title row", md);
    RAYOPDF_CHECK(Contains(md, "| 1st COURSE YEAR courses | 1st COURSE YEAR courses | 1st COURSE YEAR courses |\n| --- | --- | --- |\n| Learning activity | Ects | Sector |"), 594);
}

// A table turned on its page (set upwards, as a landscape table on a portrait page) is found in
// the frame of its text.
void TestTurnedTable() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    float v = -100;
    AddTurnedText(page, "Results of the survey are summarised below.", 72, v);
    v -= 24;
    const char* const names[] = {"Country", "Austria", "Belgium", "Croatia", "Denmark", "Estonia"};
    for (int r = 0; r < 6; ++r, v -= 14) {
        AddTurnedText(page, names[r], 72, v);
        AddTurnedText(page, r == 0 ? "2021" : std::to_string(1200 + 37 * r), 250, v);
        AddTurnedText(page, r == 0 ? "2022" : std::to_string(1300 + 29 * r), 330, v);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("turned table", md);
    RAYOPDF_CHECK(report.tables == 1, 596);
    RAYOPDF_CHECK(Contains(md, "| Country | 2021 | 2022 |\n| --- | --- | --- |\n| Austria | 1237 | 1329 |"), 597);
}

// Rows of evenly stepped plain numbers set upwards between rules (a plot's turned tick labels over
// its frame and reference lines): no table, the numbers stay text. Set upwards too, a header row
// of words over rows of numbers between rules is a table.
void TestTurnedTickLabels() {
    for (const bool header : {false, true}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        // A rule across the frame from u0 to u1 at v is, on the page, the line x = -v.
        const auto rule = [&](float v, float u0, float u1) { VLine(page, -v, u0, u1); };
        if (header) {
            rule(-100, 72, 352);
            AddTurnedText(page, "Year", 80, -112);
            AddTurnedText(page, "Value", 200, -112);
            rule(-117, 72, 352);
            const char* const rows[3][2] = {{"2019", "12.5"}, {"2020", "13.1"}, {"2021", "14.0"}};
            float v = -130;
            for (const auto& row : rows) {
                AddTurnedText(page, row[0], 80, v);
                AddTurnedText(page, row[1], 200, v);
                v -= 14;
            }
            rule(-164, 72, 352);
        } else {
            const char* const ticks[] = {"\xE2\x88\x92" "0.6", "\xE2\x88\x92" "0.4", "\xE2\x88\x92" "0.2", "0.0",
                                         "0.2", "0.4", "0.6"};
            rule(-100, 72, 352);
            for (const float v : {-112.0f, -130.0f, -144.0f, -158.0f})
                for (int i = 0; i < 7; ++i) AddTurnedText(page, ticks[i], 80 + 40.0f * float(i), v);
            rule(-117, 72, 352);
            rule(-164, 72, 352);
        }
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("turned tick labels", md);
        if (header) {
            RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Year | Value |\n| --- | --- |\n| 2019 | 12.5 |"), 1731);
        } else {
            RAYOPDF_CHECK(report.tables == 0 && Contains(md, "0.4") && !Contains(md, "| --- |"), 1730);
        }
    }
}

// A chart's frame drawn as a grid, its axis ticks in one cell: no table.
void TestChartFrame() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 600.0f, 580.0f}) HLine(page, 72, 400, y);
    for (float x : {72.0f, 110.0f, 400.0f}) VLine(page, x, 580, 700);
    const char* const ticks[] = {"0.8", "0.6", "0.4", "0.2"};
    for (int r = 0; r < 4; ++r) AddText(page, ticks[r], 80, 688 - 24.0f * float(r));
    AddText(page, "Gauss", 200, 650);
    AddText(page, "Radau", 200, 630);
    AddText(page, "x", 80, 586);
    AddText(page, "0 2 4 6 8 10", 150, 586);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("chart frame", md);
    RAYOPDF_CHECK(report.tables == 0, 850);
}

// A form to fill in: a header row and rows labelled in the first column, the rest empty, in a grid
// ruled both ways.
void TestFormGrid() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (int r = 0; r <= 5; ++r) HLine(page, 72, 450, 700 - 24.0f * float(r));
    for (float x : {72.0f, 200.0f, 320.0f, 450.0f}) VLine(page, x, 580, 700);
    AddText(page, "Mitosis", 204, 684);
    AddText(page, "Meiosis", 324, 684);
    const char* const labels[] = {"chromosomes", "replications", "divisions", "purpose"};
    for (int r = 0; r < 4; ++r) AddText(page, labels[r], 76, 660 - 24.0f * float(r));
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("form grid", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "|  | Mitosis | Meiosis |\n| --- | --- | --- |\n| chromosomes |  |  |"), 851);
}

// Header labels centred over narrow columns of figures, wider than the figures: the body keeps its
// columns and each label heads the column nearest it.
void TestWideHeaderLabels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    float y = AddProse(page, 72, 450, 740, 2) - 20;
    AddText(page, "Saccharometer", 90, y, 10, kGlyphBold);
    AddText(page, "DI Water", 190, y, 10, kGlyphBold);
    AddText(page, "Yeast Suspension", 300, y, 10, kGlyphBold);
    for (int r = 0; r < 4; ++r) {
        y -= 16;
        AddText(page, std::to_string(r + 1), 74, y);
        AddText(page, std::to_string(2 * r + 2) + " ml", 200, y);
        AddText(page, std::to_string(r + 6) + " ml", 330, y);
    }
    AddProse(page, 72, 450, y - 30, 2);
    const std::string md = Run(pages);
    Show("wide header labels", md);
    RAYOPDF_CHECK(Contains(md, "| Saccharometer | DI Water | Yeast Suspension |\n| --- | --- | --- |\n| 1 | 2 ml | 6 ml |"), 852);
}

// A top and a bottom rule only, labels over groups of columns: the rows without numbers above the
// figures are the header, and the figures' own columns part the body. "Instruction", centred over
// the gap between two columns that the line below fills both, spans them: the header keeps its two
// lines as rows.
void TestRuledGroupHeader() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 400, 712);
    AddText(page, "Instruction", 180, 702);
    AddText(page, "Properties", 76, 690);
    AddText(page, "Alpaca", 160, 690);
    AddText(page, "Orca", 210, 690);
    AddText(page, "Math", 260, 690);
    const char* const names[] = {"Samples", "Used", "Open"};
    const char* const values[3][3] = {{"52K", "2.91M", "126K"}, {"52K", "100K", "52K"}, {"1", "1", "0"}};
    for (int r = 0; r < 3; ++r) {
        AddText(page, names[r], 76, 676 - 14.0f * float(r));
        for (int c = 0; c < 3; ++c) AddText(page, values[r][c], 160 + 50.0f * float(c), 676 - 14.0f * float(r));
    }
    HLine(page, 72, 400, 640);
    const std::string md = Run(pages);
    Show("ruled group header", md);
    RAYOPDF_CHECK(Contains(md, "|  | Instruction | Instruction |  |\n| --- | --- | --- | --- |\n| Properties | Alpaca | Orca | Math |\n"
                               "| Samples | 52K | 2.91M | 126K |"),
                  853);
}

// Booktabs rules over two groups of two columns of figures, each group's label on two lines
// centred over it ("Chinese / experimenter"): the second line crosses the whitespace between its
// group's columns, which only rows of figures part. Those columns part all the same, the label
// spanning them; a value with its standard error a word space after it ("x (y)") stays one cell.
void TestRuledSubColumnsUnderLabel() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 420, 712);
    AddText(page, "Chinese", 187, 702);
    AddText(page, "White", 332, 702);
    AddText(page, "experimenter", 175, 690);
    AddText(page, "experimenter", 315, 690);
    AddText(page, "Response", 76, 678);
    for (float x : {150.0f, 290.0f}) {
        AddText(page, "Divided", x, 678);
        AddText(page, "Full", x + 65, 678);
    }
    HLine(page, 72, 420, 672);
    const char* const names[] = {"Nonverbal", "Verbal", "Written"};
    const char* const values[3][4] = {{".34 (.22)", ".54 (.17)", ".12 (.27)", ".20 (.24)"},
                                      {".25 (.23)", ".36 (.20)", ".12 (.27)", ".34 (.22)"},
                                      {".18 (.21)", ".41 (.19)", ".22 (.25)", ".30 (.23)"}};
    for (int r = 0; r < 3; ++r) {
        AddText(page, names[r], 76, 660 - 14.0f * float(r));
        for (int c = 0; c < 4; ++c) AddText(page, values[r][c], (c < 2 ? 150.0f : 290.0f) + 65.0f * float(c % 2), 660 - 14.0f * float(r));
    }
    HLine(page, 72, 420, 620);
    const std::string md = Run(pages);
    Show("ruled sub-columns under a label", md);
    RAYOPDF_CHECK(Contains(md, "| Nonverbal | .34 (.22) | .54 (.17) | .12 (.27) | .20 (.24) |"), 1745);
    RAYOPDF_CHECK(Contains(md, "| Response | Divided | Full | Divided | Full |"), 1746);
}

// A ruled table, a note under it, then its header row again at its columns over one more row (a
// second table of the same columns): the repeated header starts a table of two rows.
void TestRepeatedHeaderRow() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f, 640.0f}) HLine(page, 72, 360, y);
    for (float x : {72.0f, 160.0f, 260.0f, 360.0f}) VLine(page, x, 640, 700);
    const char* const cells[3][3] = {{"Item", "Count", "Price"}, {"pens", "12", "3.50"}, {"cups", "4", "8.00"}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) AddText(page, cells[r][c], c == 0 ? 76.0f : c == 1 ? 164.0f : 264.0f, 686 - 20.0f * float(r));
    AddText(page, "Note: prices in euros, see the second table below.", 72, 610);
    AddText(page, "Item", 76, 580);
    AddText(page, "Count", 164, 580);
    AddText(page, "Price", 264, 580);
    AddText(page, "mugs", 76, 564);
    AddText(page, "6", 164, 564);
    AddText(page, "5.25", 264, 564);
    AddText(page, "Text after the tables.", 72, 520);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("repeated header row", md);
    RAYOPDF_CHECK(report.tables == 2 && Contains(md, "| Item | Count | Price |\n| --- | --- | --- |\n| mugs | 6 | 5.25 |") &&
                      Contains(md, "| cups | 4 | 8.00 |"),
                  1736);
}

// The last rows of a ruled table set below its last rule, keeping its columns, belong to it.
void TestRowsBelowLattice() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f}) HLine(page, 72, 400, y);
    for (float x : {72.0f, 200.0f, 300.0f, 400.0f}) VLine(page, x, 660, 700);
    const char* const cells[4][3] = {{"Cambodia", "7.5%", "1,272"}, {"Indonesia", "9.4%", "9,651"}, {"Thailand", "-0.9%", "8,067"},
                                     {"Viet Nam", "11.5%", "17,200"}};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) AddText(page, cells[r][c], (c == 0 ? 76.0f : c == 1 ? 204.0f : 304.0f), 686 - 20.0f * float(r));
    AddProse(page, 72, 400, 590, 2);
    const std::string md = Run(pages);
    Show("rows below lattice", md);
    RAYOPDF_CHECK(Contains(md, "| Indonesia | 9.4% | 9,651 |\n| Thailand | -0.9% | 8,067 |\n| Viet Nam | 11.5% | 17,200 |"), 854);
}

// A header cell set to the middle of two header lines ("Time t" beside two-line labels) keeps the
// first line of the others in the table.
void TestMiddleHeaderCell() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    float y = AddProse(page, 72, 450, 740, 2) - 20;
    AddText(page, "Observed returns", 160, y);
    AddText(page, "Returns on new", 300, y);
    AddText(page, "Time t", 72, y - 5);
    AddText(page, "on the portfolio", 160, y - 10);
    AddText(page, "investment", 300, y - 10);
    y -= 28;
    for (int r = 0; r < 4; ++r, y -= 16) {
        AddText(page, std::to_string(2012 + r), 72, y);
        AddText(page, std::to_string(10 - r) + "%", 160, y);
        AddText(page, std::to_string(7 - r) + "%", 300, y);
    }
    AddProse(page, 72, 450, y - 20, 2);
    const std::string md = Run(pages);
    Show("middle header cell", md);
    RAYOPDF_CHECK(Contains(md, "| Time t | Observed returns on the portfolio | Returns on new investment |\n| --- | --- | --- |\n| 2012 |"), 855);
}

// A header row right above a box of several ruled rows is its first row.
void TestHeaderAboveRuledRows() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "OCR", 204, 706);
    AddText(page, "Search", 304, 706);
    for (float y : {700.0f, 680.0f, 660.0f}) HLine(page, 72, 400, y);
    for (float x : {72.0f, 200.0f, 300.0f, 400.0f}) VLine(page, x, 660, 700);
    AddText(page, "Pack", 76, 686);
    AddText(page, "Reads text", 204, 686);
    AddText(page, "Finds items", 304, 686);
    AddText(page, "Use", 76, 666);
    AddText(page, "Receipts", 204, 666);
    AddText(page, "Catalogues", 304, 666);
    const std::string md = Run(pages);
    Show("header above ruled rows", md);
    RAYOPDF_CHECK(Contains(md, "|  | OCR | Search |\n| --- | --- | --- |\n| Pack | Reads text | Finds items |"), 856);
}

// Diagrams drawn over a grid are no tables: boxes spanning most cells (a block diagram), a few
// one-word labels in a mostly empty grid (a graph's vertices).
void TestDiagramGrids() {
    std::vector<PageData> pages{NewPage(), NewPage(2)};
    // Block diagram: three rows of boxes that do not line up.
    PageData& a = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f, 640.0f}) HLine(a, 72, 400, y);
    VLine(a, 72, 640, 700);
    VLine(a, 400, 640, 700);
    VLine(a, 150, 680, 700);
    VLine(a, 250, 660, 680);
    VLine(a, 320, 640, 660);
    AddText(a, "Encoder", 80, 686);
    AddText(a, "Channel", 160, 686);
    AddText(a, "Decoder", 80, 666);
    AddText(a, "Output", 260, 666);
    AddText(a, "Radar", 80, 646);
    AddText(a, "Signal", 330, 646);
    // Graph: four labels in a 4 x 4 grid.
    PageData& b = pages[1];
    for (int k = 0; k <= 4; ++k) HLine(b, 72, 312, 700 - 30.0f * float(k)), VLine(b, 72 + 60.0f * float(k), 580, 700);
    AddText(b, "a", 76, 686);
    AddText(b, "b", 196, 686);
    AddText(b, "c", 76, 596);
    AddText(b, "d", 256, 596);
    AddText(b, "e", 136, 626);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("diagram grids", md);
    RAYOPDF_CHECK(report.tables == 0, 857);
}

// A sparse lattice: the expiries down the first column, the tenors along the first row and one
// value per row on the anti-diagonal (most cells empty, a word each) is a table, not a diagram.
void TestSparseLattice() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 740, 2);
    for (int k = 0; k <= 5; ++k) HLine(page, 72, 372, 700 - 20.0f * float(k)), VLine(page, 72 + 60.0f * float(k), 600, 700);
    const char* const tenors[] = {"Expiry", "1.0139", "2.0250", "3.0444", "4.0583"};
    const char* const expiries[] = {"0.5028", "1.0139", "2.0278", "3.0472"};
    const char* const values[] = {"0.004050", "0.005312", "0.006034", "0.005394"};
    for (int c = 0; c < 5; ++c) AddText(page, tenors[c], 76 + 60.0f * float(c), 686);
    for (int r = 0; r < 4; ++r) {
        AddText(page, expiries[r], 76, 666 - 20.0f * float(r));
        AddText(page, values[r], 76 + 60.0f * float(4 - r), 666 - 20.0f * float(r), 8);
    }
    AddProse(page, 72, 400, 580, 2);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("sparse lattice", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| 0.5028 |  |  |  | 0.004050 |\n| 1.0139 |  |  | 0.005312 |  |"), 884);
}

// The frames of a timing diagram between rules: labels at two heights in different columns
// ("CTS" then "ACK" for one station, the same shifted for the next), no table.
void TestTimingBoxes() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 740, 2);
    for (float y : {700.0f, 680.0f, 660.0f}) HLine(page, 72, 400, y);
    AddText(page, "CTS", 80, 686);
    AddText(page, "ACK", 240, 686);
    AddText(page, "CTS", 160, 666);
    AddText(page, "ACK", 320, 666);
    AddProse(page, 72, 400, 630, 2);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("timing boxes", md);
    RAYOPDF_CHECK(report.tables == 0 && Count(md, "CTS") == 2 && Count(md, "ACK") == 2, 885);
}

// An equation array between paragraphs, each line numbered at the right margin: displayed
// formulas, no table; each number follows its line.
void TestEquationArray() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    float y = AddProse(page, 72, 450, 740, 3);
    const char* const lines[][2] = {{"1 + 1 = 2e + 3f", "(3)"},
                                    {"1 + 3d = 2e + 2g", "(4)"},
                                    {"3b + 3d = 2e + 2g", "(5)"},
                                    {"1 + 2c = 3f + 2g", "(6)"},
                                    {"3b + 2c = 1 + 2g", "(7)"}};
    for (int r = 0; r < 5; ++r) {
        y -= 16;
        AddText(page, lines[r][0], 200, y);
        AddText(page, lines[r][1], 500, y);
    }
    AddProse(page, 72, 450, y - 24, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("equation array", md);
    const size_t first = md.find("1 + 1 = 2e + 3f"), number = md.find("(3)"), second = md.find("1 + 3d = 2e + 2g");
    RAYOPDF_CHECK(report.tables == 0 && first < number && number < second && second != std::string::npos, 886);
}

// Displayed formulas without equation numbers set in a grid: an array whose middle column holds an
// equals sign alone on every line; a tall bar built of "∣" pieces on every line around an
// expression. Neither is a table, every word stays.
void TestEquationGrids() {
    {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        float y = AddProse(page, 72, 450, 740, 3);
        const char* const left[] = {"X11", "X12", "X22"};
        const char* const right[] = {"Q(A + BK) + KQ,", "Q(B + FK),", "PEA + CPN"};
        for (int r = 0; r < 3; ++r) {
            y -= 16;
            AddText(page, left[r], 200, y);
            AddText(page, "=", 230, y);
            AddText(page, right[r], 250, y);
        }
        AddProse(page, 72, 450, y - 24, 3);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("relation column", md);
        RAYOPDF_CHECK(report.tables == 0 && Contains(md, "X12") && Contains(md, "PEA + CPN"), 2300);
    }
    {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        float y = AddProse(page, 72, 450, 740, 3);
        const char* const middle[] = {"a + b", "c + d", "e + f", "g + h", "k + m"};
        for (int r = 0; r < 5; ++r) {
            y -= 12;
            AddText(page, "\xE2\x88\xA3", 200, y);
            AddText(page, middle[r], 230, y);
            AddText(page, "\xE2\x88\xA3", 300, y);
        }
        AddProse(page, 72, 450, y - 24, 3);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("tall bars", md);
        RAYOPDF_CHECK(report.tables == 0 && Contains(md, "e + f"), 2302);
    }
}

// Pieces of tall brackets around aligned figures: a matrix in display math, not a table.
void TestBracketMatrix() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    const char* const left[] = {"\xE2\x8E\xA1", "\xE2\x8E\xA2", "\xE2\x8E\xA2", "\xE2\x8E\xA3"};
    for (int r = 0; r < 4; ++r) {
        const float y = 700 - 14.0f * float(r);
        AddText(page, left[r], 100, y);
        AddText(page, std::to_string(r) + ".25", 130, y);
        AddText(page, std::to_string(r + 1) + ".50", 200, y);
        AddText(page, std::to_string(r + 2) + ".75", 270, y);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("bracket matrix", md);
    RAYOPDF_CHECK(report.tables == 0, 858);
}

// A lattice without rules between its rows whose verticals are drawn row by row (TeX's | columns):
// each piece is a row, its cells of two lines one row.
void TestLatticeRowsByVerticals() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "Intro text before the table.", 72, 730);
    HLine(page, 72, 440, 710);
    HLine(page, 72, 440, 640);
    for (float x : {72.0f, 200.0f, 440.0f}) {
        VLine(page, x, 670, 710);
        VLine(page, x, 640, 670);
    }
    AddText(page, "Alpha", 76, 698);
    AddText(page, "(note)", 76, 686);
    AddText(page, "first line of text", 204, 698);
    AddText(page, "and its second line", 204, 686);
    AddText(page, "Beta", 76, 656);
    AddText(page, "third", 204, 656);
    AddText(page, "Text after the table.", 72, 620);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("lattice rows by verticals", md);
    RAYOPDF_CHECK(report.tables == 1 &&
                      Contains(md, "| Alpha (note) | first line of text and its second line |\n| --- | --- |\n| Beta | third |"),
                  888);
}

// A table in sections, each boxed under its label set between the rules (the second label's words
// on both sides of a column boundary): one table, each label a row, the first one the header.
void TestLatticeSections() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "Intro text before the table.", 72, 740);
    HLine(page, 72, 440, 720);
    AddText(page, "General analysis tools", 76, 707, 10, kGlyphBold);
    for (const float top : {700.0f, 638.0f}) {
        HLine(page, 72, 440, top);
        HLine(page, 72, 440, top - 40);
        if (top > 650) HLine(page, 72, 440, top - 42);  // a double rule under the first section
        for (float x : {72.0f, 200.0f, 440.0f}) {
            VLine(page, x, top - 20, top);
            VLine(page, x, top - 40, top - 20);
        }
    }
    AddText(page, "Enrichment", 150, 645, 10, kGlyphBold);
    AddText(page, "analysis", 203.5f, 645, 10, kGlyphBold);
    const char* const cells[4][2] = {{"Alpha", "12 units"}, {"Beta", "34 units"}, {"Gamma", "56 units"}, {"Delta", "78 units"}};
    const float bases[4] = {686, 666, 624, 604};
    for (int r = 0; r < 4; ++r) {
        AddText(page, cells[r][0], 76, bases[r]);
        AddText(page, cells[r][1], 204, bases[r]);
    }
    AddText(page, "Text after the table.", 72, 570);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("lattice sections", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| General analysis tools |  |\n| --- | --- |\n| Alpha | 12 units |\n"
                                                     "| Beta | 34 units |\n| Enrichment analysis |  |\n| Gamma | 56 units |"),
                  889);
}

// LaTeX mode: a number in a table cell ("308±6", "−0.1") is text as in the default mode; the same
// number in a paragraph is a formula.
void TestCellNumbersLatex() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f}) HLine(page, 72, 300, y);
    for (float x : {72.0f, 180.0f, 300.0f}) VLine(page, x, 660, 700);
    AddText(page, "Error", 76, 686);
    AddText(page, "308\xC2\xB1" "6", 184, 686);
    AddText(page, "Shift", 76, 666);
    AddText(page, "\xE2\x88\x92" "0.1", 184, 666);
    AddText(page, "The error is 308\xC2\xB1" "6 in all runs.", 72, 630);
    ConvertReport report;
    const std::string md = Run(pages, &report, MathMode::Latex);
    Show("cell numbers latex", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Error | 308\xC2\xB1" "6 |\n| --- | --- |\n| Shift | \xE2\x88\x92" "0.1 |"), 890);
    RAYOPDF_CHECK(Contains(md, "The error is $308\\pm 6$ in all runs."), 891);
}

// A ruled table a few points right of a chart's filled background, beside it on the page but much
// shorter: no grid with the chart (the chart's legend no column of it), a table of its own.
void TestLatticeBesideChart() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    page.rulings.push_back(Ruling{60, 560, 296, 700, kRulingFill});
    AddText(page, "Series A", 100, 636);
    AddText(page, "Series B", 100, 620);
    for (float y : {648.0f, 632.0f, 616.0f, 600.0f}) HLine(page, 300, 460, y);
    for (float x : {300.0f, 380.0f, 460.0f}) VLine(page, x, 600, 648);
    const char* const cells[] = {"Label", "Value", "alpha", "0.51", "beta", "0.52"};
    for (int k = 0; k < 6; ++k) AddText(page, cells[k], k % 2 ? 384.0f : 304.0f, 636 - 16.0f * float(k / 2));
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("lattice beside a chart", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Label | Value |\n| --- | --- |\n| alpha | 0.51 |\n| beta | 0.52 |"), 892);
}

// Filled areas that cross each other (the backgrounds of a figure's two panels) mark no cells;
// areas that tile the same place are the cells of a table.
void TestCrossingAreas() {
    for (const bool crossing : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        page.rulings.push_back(Ruling{100, 560, 300, 680, kRulingFill});
        page.rulings.push_back(Ruling{crossing ? 280.0f : 300.0f, 560, 480, 680, kRulingFill});
        HLine(page, 100, 480, 620);
        AddText(page, "alpha one", 110, 650);
        AddText(page, "beta two", 320, 650);
        AddText(page, "gamma", 110, 590);
        AddText(page, "delta", 320, 590);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(crossing ? "crossing areas" : "tiling areas", md);
        RAYOPDF_CHECK(crossing ? report.tables == 0 && Contains(md, "alpha one") : report.tables == 1, 893);
    }
}

// A filled box from (x0, y0) to (x1, y1) drawn as four lines (m l l l h f), as a slide draws a
// cell: a shape of three segments and, along its sides, four rulings of no thickness.
void AddBox(PageData& page, float x0, float y0, float x1, float y1) {
    Shape s;
    s.x0 = x0, s.y0 = y0, s.x1 = x1, s.y1 = y1;
    s.kind = ShapeKind::Other, s.filled = true, s.segments = 3;
    page.shapes.push_back(s);
    page.rulings.push_back(Ruling{x0, y1, x1, y1, kRulingFill});
    page.rulings.push_back(Ruling{x1, y0, x1, y1, kRulingFill});
    page.rulings.push_back(Ruling{x0, y0, x1, y0, kRulingFill});
    page.rulings.push_back(Ruling{x0, y0, x0, y1, kRulingFill});
}

// Cells drawn as separate filled boxes 12 points apart (a slide's table, no edge of a box near
// another's): four rows of three boxes, a short text in each, are a table of four rows and three
// columns. Two rows of three boxes holding paragraphs (cards of text) are no table.
void TestBoxGrid() {
    for (const bool cells : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        if (cells) {
            const char* const text[4][3] = {{"Stage", "Name", "Benefit"}, {"One", "Alpha", "Fast"}, {"Two", "Beta", "Safe"},
                                            {"Three", "Gamma", "Cheap"}};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 3; ++c) {
                    const float x0 = 72 + 112.0f * float(c), y1 = 710 - 32.0f * float(r);
                    AddBox(page, x0, y1 - 20, x0 + 100, y1);
                    AddText(page, text[r][c], x0 + 4, y1 - 14);
                }
        } else {
            for (int r = 0; r < 2; ++r)
                for (int c = 0; c < 3; ++c) {
                    const float x0 = 72 + 156.0f * float(c), y1 = 710 - 96.0f * float(r);
                    AddBox(page, x0, y1 - 90, x0 + 150, y1);
                    AddProse(page, x0 + 4, 142, y1 - 14, 6);
                }
        }
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(cells ? "box grid" : "box cards", md);
        if (cells) {
            RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Stage | Name | Benefit |\n| --- | --- | --- |\n| One | Alpha | Fast |\n"
                                                             "| Two | Beta | Safe |\n| Three | Gamma | Cheap |"),
                          1734);
        } else {
            RAYOPDF_CHECK(report.tables == 0 && Contains(md, "quick brown"), 1735);
        }
    }
}

// A ruled table with one vertical rule, after its first column, over two body rows: the other
// columns come from the wide whitespace between them.
void TestRuledColumnTwoRows() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {712.0f, 698.0f, 668.0f}) HLine(page, 72, 400, y);
    // The vertical rule drawn piece by piece (a header piece, a piece per body row).
    VLine(page, 140, 699, 711);
    VLine(page, 140, 684, 697);
    VLine(page, 140, 669, 680);
    const char* const cells[3][4] = {{"Setting", "Clash", "Bond", "Twist"}, {"MD", "1.000", "1.000", "0.518"},
                                     {"NE", "0.990", "1.000", "0.294"}};
    const float bases[3] = {702, 686, 674};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) AddText(page, cells[r][c], c == 0 ? 76.0f : 160.0f + 80.0f * float(c - 1), bases[r]);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("ruled column, two rows", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Setting | Clash | Bond | Twist |\n| --- | --- | --- | --- |\n"
                                                     "| MD | 1.000 | 1.000 | 0.518 |\n| NE | 0.990 | 1.000 | 0.294 |"),
                  894);
}

// A grid across two figure regions side by side, each as tall as the grid (the bars of a chart
// found as two drawings): part of the figure.
void TestLatticeAcrossFigures() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {600.0f, 584.0f, 568.0f, 552.0f}) HLine(page, 160, 390, y);
    for (float x : {160.0f, 275.0f, 390.0f}) VLine(page, x, 552, 600);
    const char* const cells[] = {"Label", "Value", "alpha", "0.51", "beta", "0.52"};
    for (int k = 0; k < 6; ++k) AddText(page, cells[k], k % 2 ? 279.0f : 164.0f, 588 - 16.0f * float(k / 2));
    for (const float x0 : {150.0f, 282.0f}) {
        Shape s;
        s.x0 = x0, s.x1 = x0 + 120, s.y0 = 520, s.y1 = 640;
        s.kind = ShapeKind::Other, s.stroked = true, s.curved = true, s.segments = 40;
        page.shapes.push_back(s);
        for (int k = 0; k < 10; ++k) {
            Shape tick;
            tick.x0 = tick.x1 = x0 + 5 + 12.0f * float(k), tick.y0 = 522, tick.y1 = 525;
            tick.kind = ShapeKind::Line, tick.stroked = true, tick.segments = 1;
            page.shapes.push_back(tick);
        }
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("lattice across figures", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "alpha"), 895);
}

// Booktabs whose last column is code (monospace parameters, wrapped) beside labels in proportional
// type: a table, not a code block.
void TestCodeColumnTable() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {712.0f, 698.0f, 626.0f}) HLine(page, 72, 460, y);
    AddText(page, "Model", 76, 702);
    AddText(page, "Name", 160, 702);
    AddText(page, "Parameters", 240, 702);
    const char* const rows[3][4] = {{"1-NN", "IBk", "-K 1 -W 0 -A", "LinearNNSearch -A"},
                                    {"3-NN", "IBk", "-K 3 -W 0 -A", "EuclideanDistance -R"},
                                    {"Tree", "J48", "-C 0.25 -M 2", nullptr}};
    float base = 686;
    for (const auto& row : rows) {
        AddText(page, row[0], 76, base);
        AddText(page, row[1], 160, base);
        AddText(page, row[2], 240, base, 10, kGlyphMonospace);
        if (row[3]) AddText(page, row[3], 240, base -= 12, 10, kGlyphMonospace);
        base -= 14;
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("code column table", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Model | Name | Parameters |\n| --- | --- | --- |\n| 1-NN | IBk |"), 896);
}

// Booktabs whose first header line holds one label centred over the gap between two columns that
// the third line fills ("Test" over "clean" and "other"), and a label set between the two lines
// ("Model", centred on the header's height): two header rows, the label spanning its columns.
void TestHeaderMiddleLevels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 400, 712);
    AddText(page, "Test", 248, 700);
    AddText(page, "Model", 76, 696);
    AddText(page, "LM", 160, 696);
    AddText(page, "clean", 220, 690);
    AddText(page, "other", 272, 690);
    HLine(page, 72, 400, 684);
    const char* const names[] = {"Alpha", "Beta", "Gamma"};
    for (int r = 0; r < 3; ++r) {
        const float y = 672 - 14.0f * float(r);
        AddText(page, names[r], 76, y);
        AddText(page, "-", 160, y);
        AddText(page, std::to_string(2 + r) + ".10", 220, y);
        AddText(page, std::to_string(4 + r) + ".60", 272, y);
    }
    HLine(page, 72, 400, 634);
    const std::string md = Run(pages);
    Show("header middle levels", md);
    RAYOPDF_CHECK(Contains(md, "| Model | LM | Test | Test |\n| --- | --- | --- | --- |\n|  |  | clean | other |\n"
                               "| Alpha | - | 2.10 | 4.60 |"),
                  1700);
}

// A rule under a header label narrower than the table (\cmidrule): the header lines stay rows,
// the label over the rule spans all the columns under it, and a label wider than its column,
// reaching past the middle of the gap beside it, keeps to its column (the gap moves clear of it).
void TestCmidruleHeader() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 400, 712);
    AddText(page, "Instruction", 175, 700);
    HLine(page, 160, 300, 695);
    AddText(page, "Name", 76, 686);
    AddText(page, "Alpaca", 160, 686);
    AddText(page, "Performances", 198, 686);
    AddText(page, "B", 290, 686);
    HLine(page, 72, 400, 680);
    const char* const names[] = {"Alpha", "Beta", "Gamma"};
    for (int r = 0; r < 3; ++r) {
        const float y = 668 - 14.0f * float(r);
        AddText(page, names[r], 76, y);
        AddText(page, std::to_string(10 + r), 165, y);
        AddText(page, std::to_string(2 + r) + ".10", 225, y);
        AddText(page, std::to_string(4 + r) + ".60", 290, y);
    }
    HLine(page, 72, 400, 626);
    const std::string md = Run(pages);
    Show("cmidrule header", md);
    RAYOPDF_CHECK(Contains(md, "|  | Instruction | Instruction | Instruction |\n| --- | --- | --- | --- |\n"
                               "| Name | Alpaca | Performances | B |\n| Alpha | 10 | 2.10 | 4.60 |"),
                  1701);
}

// Lattices of the same columns that touch, the rows between them taken as rows below the upper
// one: one table.
void TestTouchingStripes() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 680.0f, 660.0f, 642.4f, 622.4f, 602.4f}) HLine(page, 72, 372, y);
    for (float x : {72.0f, 172.0f, 272.0f, 372.0f}) VLine(page, x, 660, 700), VLine(page, x, 602.4f, 642.4f);
    const char* const cells[5][3] = {{"Cambodia", "7.5%", "1,272"}, {"Indonesia", "9.4%", "9,651"}, {"Lao PDR", "4.0%", "265"},
                                     {"Malaysia", "18.6%", "1,454"}, {"Myanmar", "2.7%", "2,250"}};
    const float bases[] = {686, 666, 646, 628.4f, 608.4f};
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 3; ++c) AddText(page, cells[r][c], 76 + 100.0f * float(c), bases[r]);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("touching stripes", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Indonesia | 9.4% | 9,651 |\n| Lao PDR | 4.0% | 265 |\n"
                                                     "| Malaysia | 18.6% | 1,454 |"),
                  1702);
}

// A header row whose labels, a word space apart, start where the columns of a borderless table
// start: its first row, a label to each column.
void TestHeaderAtColumns() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    float x = 74;
    for (const char* word : {"Saccharometer", "DI", "Water", "Glucose", "Solution", "Yeast", "Suspension"}) {
        if (std::strcmp(word, "Glucose") == 0) x = 210;
        if (std::strcmp(word, "Yeast") == 0) x = 306;
        x = AddText(page, word, x, 706, 12, kGlyphBold) + 2.6f;
    }
    const char* const cells[3][4] = {{"2", "24 ml", "0 ml", "4 ml"}, {"3", "12 ml", "12 ml", "4 ml"}, {"4", "4 ml", "12 ml", "12 ml"}};
    for (int r = 0; r < 3; ++r) {
        const float y = 690 - 17.0f * float(r);
        AddText(page, cells[r][0], 74, y, 12);
        AddText(page, cells[r][1], 154.6f, y, 12);
        AddText(page, cells[r][2], 210, y, 12);
        AddText(page, cells[r][3], 306, y, 12);
    }
    const std::string md = Run(pages);
    Show("header at columns", md);
    RAYOPDF_CHECK(Contains(md, "| Saccharometer | DI Water | Glucose Solution | Yeast Suspension |\n| --- | --- | --- | --- |\n"
                               "| 2 | 24 ml | 0 ml | 4 ml |"),
                  1703);
}

// A title set between the top rules of a table, over rows of two columns: its first row.
void TestTitleBand() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 684.0f, 620.0f}) HLine(page, 56, 228, y);
    AddText(page, "Fish species on IUCN Red List", 60, 690, 8);
    const char* const rows[4][2] = {{"Potosi Pupfish", "Cyprinodon alvarezi"}, {"La Palma Pupfish", "Cyprinodon longidorsalis"},
                                    {"Butterfly Splitfin", "Ameca splendens"}, {"Golden Skiffia", "Skiffia francesae"}};
    for (int r = 0; r < 4; ++r) {
        AddText(page, rows[r][0], 60, 672 - 14.0f * float(r), 8);
        AddText(page, rows[r][1], 140, 672 - 14.0f * float(r), 8);
    }
    const std::string md = Run(pages);
    Show("title band", md);
    RAYOPDF_CHECK(Contains(md, "| Fish species on IUCN Red List |  |\n| --- | --- |\n| Potosi Pupfish | Cyprinodon alvarezi |"), 1704);
}

// Dots drawn by the tens of thousands (a plot's points) are no rulings: the rule of a table drawn
// after them is recorded.
void TestDotsNotRulings() {
    std::string content = "0.1 w\n";
    for (int k = 0; k < 70000; ++k) {
        const int x = 100 + k % 400, y = 300 + (k / 400) % 400;
        content += std::to_string(x) + " " + std::to_string(y) + " m " + std::to_string(x) + ".2 " + std::to_string(y) + " l S\n";
    }
    content += "1 w 72 200 m 400 200 l S";
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int stream = b.AddFlateStream("", content);
    const int pageObject = b.Add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 612 792] /Contents " +
                                 std::to_string(stream) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(pageObject) + " 0 R] /Count 1 >>");
    const std::string pdf = b.Build(catalog);
    std::unique_ptr<Document> doc;
    RAYOPDF_CHECK(Document::Open(pdf, "", doc) == OpenStatus::Ok, 1705);
    Text::FontCache fonts(*doc);
    PageData page;
    PageStats stats;
    InterpretPage(*doc, fonts, 0, AnnotationMode::None, page, stats);
    RAYOPDF_CHECK(page.rulings.size() == 1 && page.rulings[0].x0 < 72 && page.rulings[0].x1 > 400, 1705);
}

// A plot (a curve of many segments over ticks) as a figure region at x0..x1, y0..y1.
void AddPlot(PageData& page, float x0, float x1, float y0, float y1) {
    Shape s;
    s.x0 = x0, s.x1 = x1, s.y0 = y0, s.y1 = y1;
    s.kind = ShapeKind::Other, s.stroked = true, s.curved = true, s.segments = 40;
    page.shapes.push_back(s);
    for (int k = 0; k < 10; ++k) {
        Shape tick;
        tick.x0 = tick.x1 = x0 + 4 + (x1 - x0 - 8) * float(k) / 9, tick.y0 = y0 + 2, tick.y1 = y0 + 5;
        tick.kind = ShapeKind::Line, tick.stroked = true, tick.segments = 1;
        page.shapes.push_back(tick);
    }
}

// Two plots side by side, each with its axis labels (wider than two ems) set left of it: the
// labels are the plots' text, no table of two columns.
void TestTickLabelsBesidePlots() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddPlot(page, 163, 300, 542, 710);
    AddPlot(page, 346, 482, 542, 710);
    const char* const left[] = {"TempN-R", "TempD-U", "TempN-R1", "TempN-U2", "TempD-R", "TempN-U", "NDVI-U", "NDWI-U"};
    const char* const right[] = {"TempD-R1", "NDVI-R1", "TempD-R", "NDWI-R2", "NDVI-R", "NDWI-R1", "TempD-R2", "NDVI-R2"};
    for (int r = 0; r < 8; ++r) {
        const float y = 700 - 6.0f * float(r);
        AddText(page, left[r], 160 - 3.0f * float(std::strlen(left[r])), y, 6);
        AddText(page, right[r], 343 - 3.0f * float(std::strlen(right[r])), y, 6);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("tick labels beside plots", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "TempN-R1"), 1706);
}

// A ruled grid nine tenths inside a plot's region (the panel titles above it aside): the figure's,
// no table.
void TestGridMostlyInFigure() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddPlot(page, 100, 500, 400, 690);
    for (float y : {722.0f, 702.0f, 560.0f, 425.0f}) HLine(page, 103, 497, y);
    for (float x : {103.0f, 234.0f, 365.0f, 497.0f}) VLine(page, x, 425, 722);
    const char* const titles[] = {"CaffeNet", "GoogleNet", "VGG-16"};
    for (int c = 0; c < 3; ++c) {
        AddText(page, titles[c], 110 + 131.0f * float(c), 708);
        AddText(page, "[i] [r]", 110 + 131.0f * float(c), 600);
        AddText(page, "[m] [n]", 110 + 131.0f * float(c), 500);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("grid mostly in figure", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "[m] [n]"), 1707);
}

// Lines of justified prose under a rule whose word spaces line up into a river: no table of two
// columns.
void TestProseRiverUnderRule() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 310, 560, 712);
    const char* const left[] = {"In general, we train", "Ensemble members are", "Each update follows."};
    const char* const right[] = {"The protagonist and its", "An outer loop wherein", "Both agents improve"};
    for (int r = 0; r < 3; ++r) {
        const float y = 700 - 12.0f * float(r);
        AddText(page, left[r], 312, y);
        AddText(page, right[r], 418, y);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("prose river under rule", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "outer loop"), 1708);
}

// Two header lines between a top and a mid rule: set as far apart as the body's rows (a line of
// units under the labels, as TeX sets two rows), two header rows; set closer than the body's rows
// (one cell's text wrapped), one header row.
void TestHeaderLinesAtRowPitch() {
    for (const bool apart : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        const float pitch = apart ? 14 : 18, header = apart ? 14 : 11;
        HLine(page, 72, 400, 714);
        AddText(page, "Platform", 76, 700);
        AddText(page, "Execution time", 180, 700);
        AddText(page, "Current", 300, 700);
        AddText(page, "(ms)", 180, 700 - header);
        AddText(page, "(uA)", 300, 700 - header);
        float y = 700 - header - 6;
        HLine(page, 72, 400, y);
        const char* const names[] = {"Alpha", "Beta", "Gamma"};
        y -= 12;
        for (int r = 0; r < 3; ++r, y -= pitch) {
            AddText(page, names[r], 76, y);
            AddText(page, std::to_string(2 + r) + ".10", 180, y);
            AddText(page, std::to_string(4 + r) + ".60", 300, y);
        }
        HLine(page, 72, 400, y + pitch - 6);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("header lines at row pitch", md);
        RAYOPDF_CHECK(report.tables == 1 &&
                          Contains(md, apart ? "| Platform | Execution time | Current |\n| --- | --- | --- |\n|  | (ms) | (uA) |\n| Alpha |"
                                             : "| Platform | Execution time (ms) | Current (uA) |\n| --- | --- | --- |\n| Alpha |"),
                      1709);
    }
}

// Two header lines between a top and a mid rule at the body's row pitch whose cells are set to the
// bottom: the last line fills every column, the first leaves the one-line "Model" empty, and
// "code" goes on from "PDB" in lower case: one header row of wrapped labels. With "Source" in its
// place every cell of the last line starts a label of its own: two header rows (TeX's rows).
void TestBottomAlignedHeader() {
    for (const bool wrapped : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        HLine(page, 72, 400, 714);
        AddText(page, "PDB", 76, 700);
        AddText(page, "Limit", 150, 700);
        AddText(page, "Rate", 300, 700);
        AddText(page, wrapped ? "code" : "Source", 76, 686);
        AddText(page, "(nm)", 150, 686);
        AddText(page, "Model", 220, 686);
        AddText(page, "(%)", 300, 686);
        HLine(page, 72, 400, 680);
        const char* const names[] = {"1abc", "2def", "3ghi"};
        float y = 668;
        for (int r = 0; r < 3; ++r, y -= 14) {
            AddText(page, names[r], 76, y);
            AddText(page, std::to_string(2 + r) + ".10", 150, y);
            AddText(page, r == 1 ? "Binary" : "None", 220, y);
            AddText(page, std::to_string(4 + r) + ".60", 300, y);
        }
        HLine(page, 72, 400, y + 8);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("bottom-aligned header", md);
        RAYOPDF_CHECK(report.tables == 1 &&
                          Contains(md, wrapped ? "| PDB code | Limit (nm) | Model | Rate (%) |\n| --- | --- | --- | --- |\n| 1abc |"
                                               : "| PDB | Limit |  | Rate |\n| --- | --- | --- | --- |\n| Source | (nm) | Model | (%) |\n"),
                      1741);
    }
    // A header of levels: group labels over fewer than two thirds of the columns ("Analysis"
    // centred over "df" between "X2" and "P"), the lower-case "df" no text wrapped: two rows.
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 440, 714);
    AddText(page, "Men", 150, 700);
    AddText(page, "Women", 220, 700);
    AddText(page, "Analysis", 322.5f, 700);
    AddText(page, "N", 150, 686);
    AddText(page, "N", 220, 686);
    AddText(page, "X2", 290, 686);
    AddText(page, "df", 340, 686);
    AddText(page, "P", 380, 686);
    HLine(page, 72, 440, 680);
    const char* const names[] = {"Age", "Stay", "Status"};
    float y = 668;
    for (int r = 0; r < 3; ++r, y -= 14) {
        AddText(page, names[r], 76, y);
        AddText(page, std::to_string(2 + r) + "1", 150, y);
        AddText(page, std::to_string(4 + r) + "3", 220, y);
        AddText(page, std::to_string(5 + r) + ".1", 290, y);
        AddText(page, std::to_string(1 + r), 340, y);
        AddText(page, "0.0" + std::to_string(1 + r), 380, y);
    }
    HLine(page, 72, 440, y + 8);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("header of levels", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Analysis |") && Contains(md, "| X2 | df | P |"), 1746);
}

// Rows between booktabs rules whose labels are set smaller than the figures (labels in small
// capitals, or symbols with subscripts that set their line's size): one table of every row.
void TestSmallerLabelRows() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 400, 714);
    AddText(page, "Model", 76, 700);
    AddText(page, "Error", 220, 700);
    AddText(page, "Rate", 300, 700);
    HLine(page, 72, 400, 694);
    const char* const names[] = {"Base", "LADDER NETWORK", "AUXILIARY MODEL", "Skip"};
    float y = 682;
    for (int r = 0; r < 4; ++r, y -= 12) {
        AddText(page, names[r], 76, y, r == 1 || r == 2 ? 8 : 10);
        AddText(page, std::to_string(2 + r) + ".10", 220, y);
        AddText(page, std::to_string(4 + r) + ".60", 300, y);
        if (r == 1) HLine(page, 72, 400, y - 4);
    }
    HLine(page, 72, 400, y + 6);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("smaller label rows", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Model | Error | Rate |\n| --- | --- | --- |\n| Base | 2.10 | 4.60 |\n"
                                                     "| LADDER NETWORK | 3.10 | 5.60 |\n| AUXILIARY MODEL | 4.10 | 6.60 |\n"
                                                     "| Skip | 5.10 | 7.60 |"),
                  1710);
}

// A table set without cell margins (ConTeXt's frames): its columns stand only a word space apart,
// less than whitespace alone parts. Its top, mid and bottom rules drawn cell by cell meet at the
// column boundaries: the joints part the columns, and a header label's words a word space from
// the next column's stay in their cell. The same rules drawn whole leave the narrow gaps closed.
void TestRuleJoints() {
    for (const bool pieces : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        AddProse(page, 72, 400, 760, 2);
        // Columns at 100, 128 and 174, the widest texts 3 pt before the next column, words a
        // word space (3 pt, no space glyphs: TeX's) apart.
        const float joints[] = {98, 126.5f, 172.5f, 210};
        const auto rule = [&](float y) {
            if (!pieces) return HLine(page, joints[0], joints[3], y);
            for (int k = 0; k < 3; ++k) HLine(page, joints[k], joints[k + 1], y);
        };
        rule(722);
        AddText(page, "Name", 100, 710);
        AddText(page, "title", AddText(page, "Job", 128, 710) + 3, 710);
        AddText(page, "Cost", 174, 710);
        rule(704);
        const char* const names[] = {"Anna", "Ben", "Chloe"};
        const char* const jobs[] = {"Engineer", "Designer", "Manager"};
        const char* const costs[] = {"12", "7", "305"};
        float y = 692;
        for (int r = 0; r < 3; ++r, y -= 14) {
            AddText(page, names[r], 100, y);
            AddText(page, jobs[r], 128, y);
            AddText(page, costs[r], 194 - 5.0f * float(std::strlen(costs[r])), y);
        }
        rule(y + 6);
        AddProse(page, 72, 400, y - 30, 2);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(pieces ? "rule joints" : "rules drawn whole", md);
        if (pieces) {
            RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Name | Job title | Cost |\n| --- | --- | --- |\n"
                                                             "| Anna | Engineer | 12 |\n| Ben | Designer | 7 |\n"
                                                             "| Chloe | Manager | 305 |"),
                          1732);
        } else {
            RAYOPDF_CHECK(!Contains(md, "| Anna | Engineer |") && !Contains(md, "| Job title |"), 1733);
        }
    }
}

// A ruled table whose cells are set to the middle of their rows (HTML's default): each row's
// description wraps over two lines and its one-line cells stand halfway between them. Each row
// is one row of the table. The same cells set to the top (the one-line cells on the first line)
// give the same rows.
void TestMiddleSetRows() {
    for (const bool middle : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        HLine(page, 96, 340, 712);
        AddText(page, "Key", 100, 700);
        AddText(page, "Value", 180, 700);
        AddText(page, "Description", 230, 700);
        HLine(page, 96, 340, 694);
        const char* const keys[] = {"alpha", "beta"};
        const char* const values[] = {"1", "2"};
        const char* const first[] = {"first line of", "second line of"};
        const char* const second[] = {"the first cell", "the second cell"};
        float y = 680;
        for (int r = 0; r < 2; ++r, y -= 30) {
            AddText(page, first[r], 230, y);
            AddText(page, second[r], 230, y - 12);
            const float at = middle ? y - 6 : y;
            AddText(page, keys[r], 100, at);
            AddText(page, values[r], 180, at);
        }
        HLine(page, 96, 340, y + 10);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(middle ? "middle-set rows" : "top-set rows", md);
        RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Key | Value | Description |\n| --- | --- | --- |\n"
                                                         "| alpha | 1 | first line of the first cell |\n"
                                                         "| beta | 2 | second line of the second cell |"),
                      middle ? 1741 : 1742);
    }
}

// A table ruled under its header and at its bottom, with no top rule: the header lines right above
// the first rule are its header (a label over two columns stands over a rule of their width); the
// caption above them, running across the columns, stays text.
void TestHeaderAboveRules() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "Table 2: the scores of each method over all of the runs we made", 72, 740);
    AddText(page, "Scores", 222, 716);
    HLine(page, 210, 330, 711);
    AddText(page, "Method", 76, 700);
    AddText(page, "Avg", 220, 700);
    AddText(page, "Std", 300, 700);
    HLine(page, 72, 400, 694);
    const char* const names[] = {"Alpha", "Beta", "Gamma"};
    float y = 682;
    for (int r = 0; r < 3; ++r, y -= 12) {
        AddText(page, names[r], 76, y);
        AddText(page, std::to_string(2 + r) + ".10", 220, y);
        AddText(page, std::to_string(4 + r) + ".60", 300, y);
    }
    HLine(page, 72, 400, y + 6);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("header above rules", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "Table 2: the scores") &&
                      Contains(md, "|  | Scores | Scores |\n| --- | --- | --- |\n| Method | Avg | Std |\n| Alpha | 2.10 | 4.60 |"),
                  1711);
}

// A boxed table whose columns of figures stand closer than two word spaces, under labels wider
// than the figures, each starting where its column's figures start: a column each.
void TestCloseNumberColumns() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 460, 760, 4);  // word spaces of half an em
    const float size = 8, step = 12 + 6;  // figures 12 wide, 6 apart (0.75 em: 1.5 word spaces)
    const float x0 = 130;
    const char* const labels[] = {"bike", "bird", "boat", "cats"};  // 16 wide, 2 apart
    for (int c = 0; c < 4; ++c) AddText(page, labels[c], x0 + step * float(c), 700, size);
    float y = 688;
    for (int r = 0; r < 6; ++r, y -= 10) {
        AddText(page, "M" + std::to_string(r), 76, y, size);
        for (int c = 0; c < 4; ++c) AddText(page, std::to_string(r + c) + ".5", x0 + step * float(c), y, size);
    }
    const float bottom = y + 6, right = x0 + step * 4 + 6;
    HLine(page, 72, right, 710);
    HLine(page, 72, right, 696);
    HLine(page, 72, right, bottom);
    VLine(page, 72, bottom, 710);
    VLine(page, 120, bottom, 710);
    VLine(page, right, bottom, 710);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("close number columns", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "|  | bike | bird | boat | cats |") && Contains(md, "| M1 | 1.5 | 2.5 | 3.5 | 4.5 |"), 1712);
}

int Dump(const char* path, int only) {
    File::InputFile file;
    if (file.Open(path, uint64_t(1) << 31) != File::ReadStatus::Ok) return 2;
    std::unique_ptr<Document> doc;
    if (Document::Open(file.Bytes(), "", doc) != OpenStatus::Ok) return 3;
    Text::FontCache fonts(*doc);
    std::vector<PageData> pages(static_cast<size_t>(doc->PageCount()));
    for (int i = 0; i < doc->PageCount(); ++i) {
        PageStats stats;
        InterpretPage(*doc, fonts, i, AnnotationMode::None, pages[static_cast<size_t>(i)], stats);
        if (std::getenv("RAYOPDF_DUMP_LATEX")) PrepareMathGlyphs(pages[static_cast<size_t>(i)]);
        BuildLines(pages[static_cast<size_t>(i)]);
    }
    RemoveRunningLines(pages);
    std::string text;
    TableContinuation continuation;
    for (PageData& page : pages) {
        if (only > 0 && page.number != only) continue;
        const auto start = std::chrono::steady_clock::now();
        DetectTables(page, continuation);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        std::printf("PAGE %d rulings=%zu tables=%zu (%.0f us)\n", page.number, page.rulings.size(), page.tables.size(), us);
        if (std::getenv("RAYOPDF_DUMP_RULINGS"))
            for (const Ruling& r : page.rulings)
                std::printf("  R %.1f,%.1f %.1f,%.1f %s%s\n", r.x0, r.y0, r.x1, r.y1, r.flags & kRulingStroke ? "S" : "",
                            r.flags & kRulingFill ? "F" : "");
        if (std::getenv("RAYOPDF_DUMP_SHAPES")) {
            for (const Shape& s : page.shapes)
                std::printf("  S %.1f,%.1f %.1f,%.1f k=%d %s%s%s n=%d\n", s.x0, s.y0, s.x1, s.y1, int(s.kind), s.filled ? "F" : "",
                            s.stroked ? "S" : "", s.curved ? "C" : "", int(s.segments));
            for (const ImageBox& im : page.images) std::printf("  I %.1f,%.1f %.1f,%.1f\n", im.x0, im.y0, im.x1, im.y1);
        }
        if (std::getenv("RAYOPDF_DUMP_FIGURES")) {
            std::vector<FigureRegion> regions;
            FigureRegions(page, regions);
            for (const FigureRegion& f : regions) std::printf("  FIG %.1f,%.1f %.1f,%.1f\n", f.x0, f.y0, f.x1, f.y1);
        }
        if (std::getenv("RAYOPDF_DUMP_LINES"))
            for (const Line& l : page.lines) {
                LineText(page, l, text);
                std::printf("  L %.1f..%.1f base=%.1f sz=%.1f fl=%x d=%d | %s\n", l.u0, l.u1, l.baseline, l.size, l.flags,
                            l.dir, text.c_str());
            }
        for (const Table& t : page.tables) {
            std::printf(" TABLE kind=%d %dx%d header=%d box=%.1f,%.1f %.1f,%.1f\n", t.kind, t.rows, t.cols, t.headerRows,
                        t.x0, t.y0, t.x1, t.y1);
            for (uint32_t i = 0; i < t.cellCount; ++i) {
                const TableCell& c = page.tableCells[t.firstCell + i];
                std::string cell;
                for (uint32_t l = c.first; l < c.first + c.count; ++l) {
                    LineText(page, page.lines[l], text);
                    if (!cell.empty()) cell += " / ";
                    cell += text;
                }
                std::printf("   [%d,%d %dx%d] %s\n", c.row, c.col, c.rowSpan, c.colSpan, cell.c_str());
            }
        }
    }
    return 0;
}

// layout/word_box.h: the SSE2 word box has the scalar reference's bits and ends where it does, on
// random glyphs with NaN, infinities and zeros of both signs mixed in; the reference gives the box
// of two plain words.
void TestWordBox() {
    uint64_t state = 7;
    const auto next = [&state]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 33);
    };
    const auto value = [&]() -> float {
        switch (next() % 10) {
        case 0: return (next() & 1) ? 0.0f : -0.0f;
        case 1: return NAN;
        case 2: return (next() & 1) ? INFINITY : -INFINITY;
        case 3: return static_cast<float>(static_cast<int>(next() % 5) - 2);
        default: return (static_cast<float>(next()) - 2147483648.0f) / 65536.0f;
        }
    };
    std::vector<Glyph> glyphs(64);
    std::vector<uint32_t> order(64);
    size_t nan = 0, words = 0;
    for (int round = 0; round < 20000; ++round) {
        const uint32_t count = 1 + next() % 64;
        for (uint32_t i = 0; i < count; ++i) {
            Glyph& g = glyphs[i];
            g.u0 = value(), g.u1 = value(), g.vb = value(), g.vt = value();
            g.flags = (next() % 4 == 0) ? kGlyphSpaceBefore : 0;
            order[i] = next() % count;
        }
        for (uint32_t k = 0; k < count;) {
            WordBox scalar{}, simd{};
            const uint32_t endScalar = WordBoxScalar(glyphs.data(), order.data(), k, count, scalar);
#if defined(RAYOPDF_WORD_BOX_SSE2)
            const uint32_t endSimd = WordBoxSse2(glyphs.data(), order.data(), k, count, simd);
#else
            const uint32_t endSimd = WordBoxScalar(glyphs.data(), order.data(), k, count, simd);
#endif
            const float a[4] = {scalar.u0, scalar.u1, scalar.vb, scalar.vt};
            const float b[4] = {simd.u0, simd.u1, simd.vb, simd.vt};
            RAYOPDF_CHECK(endScalar == endSimd && endScalar > k && std::memcmp(a, b, sizeof a) == 0 && scalar.nan == simd.nan, 859);
            nan += scalar.nan;
            words++;
            k = endScalar;
        }
    }
    RAYOPDF_CHECK(nan > 0 && nan < words, 860);
    // Glyphs 0..2 one word ("ab c" with c set apart), boxes 0..1, 1..2 and 3..4 by 0..1.
    glyphs[0].u0 = 0, glyphs[0].u1 = 1, glyphs[1].u0 = 1, glyphs[1].u1 = 2, glyphs[2].u0 = 3, glyphs[2].u1 = 4;
    for (int i = 0; i < 3; ++i) glyphs[i].vb = 0, glyphs[i].vt = 1, glyphs[i].flags = 0, order[i] = i;
    glyphs[2].flags = kGlyphSpaceBefore;
    WordBox box{};
    const uint32_t end = WordBoxOf(glyphs.data(), order.data(), 0, 3, box);
    RAYOPDF_CHECK(end == 2 && box.u0 == 0 && box.u1 == 2 && box.vb == 0 && box.vt == 1 && !box.nan, 861);
}

// A list of contents (entries, each with its page number at the right) is lines of text, not a
// table: ruled as a word processor's table (rules under the rows, a rule before the page column),
// or set without rules with front matter in roman numerals and a part title among the entries,
// which is an entry too, not a heading; leader dots set a space apart are written as one run. Two
// columns whose numbers go down are a table still.
void TestContentsLists() {
    std::vector<PageData> pages{NewPage(), NewPage(2), NewPage(3)};
    AddText(pages[0], "Table of Contents", 72, 640, 16, kGlyphBold);
    const char* const ruled[4][2] = {{"Executive Summary", "4"}, {"Legal Framework", "6"},
                                     {"Election Administration", "11"}, {"Civil Society Engagement", "15"}};
    for (int r = 0; r < 4; ++r) {
        const float y = 600 - 20.0f * float(r);
        AddText(pages[0], ruled[r][0], 76, y);
        AddText(pages[0], ruled[r][1], 304, y);
        HLine(pages[0], 72, 340, y - 6);
        VLine(pages[0], 300, y - 6, y + 12);
    }
    AddProse(pages[0], 72, 400, 480, 3);
    AddText(pages[1], "Contents", 72, 640, 16, kGlyphBold);
    const char* const plain[6][2] = {{"Acknowledgments", "v"}, {"Introduction . . . . . . .", "1"}, {"", ""},
                                     {"Section 1.1: Types of Variables", "3"}, {"Section 1.2: Descriptive Statistics", "5"},
                                     {"Section 1.3: Missing Data", "6"}};
    for (int r = 0; r < 6; ++r) {
        const float y = 600 - 18.0f * float(r);
        if (r == 2) {
            AddText(pages[1], "Part I. Exploring Your Data", 72, y, 11, kGlyphBold);
            continue;
        }
        AddText(pages[1], plain[r][0], 72, y);
        AddText(pages[1], plain[r][1], 380, y);
    }
    AddText(pages[2], "Regional Counts", 72, 640, 16, kGlyphBold);
    const char* const counts[4][2] = {{"North", "42"}, {"South", "31"}, {"East", "17"}, {"West", "9"}};
    AddText(pages[2], "Region", 76, 600, 10, kGlyphBold);
    AddText(pages[2], "Count", 204, 600, 10, kGlyphBold);
    HLine(pages[2], 72, 260, 612);
    HLine(pages[2], 72, 260, 594);
    for (int r = 0; r < 4; ++r) {
        const float y = 580 - 16.0f * float(r);
        AddText(pages[2], counts[r][0], 76, y);
        AddText(pages[2], counts[r][1], 204, y);
    }
    HLine(pages[2], 72, 260, 510);
    AddProse(pages[2], 72, 400, 480, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("contents lists", md);
    RAYOPDF_CHECK(report.tables == 1, 1723);
    RAYOPDF_CHECK(Contains(md, "Legal Framework") && !Contains(md, "| Legal Framework"), 1724);
    RAYOPDF_CHECK(Contains(md, "Part I. Exploring Your Data") && !Contains(md, "# Part I") &&
                      !Contains(md, "| Introduction"),
                  1725);
    RAYOPDF_CHECK(Contains(md, "| North | 42 |"), 1726);
    RAYOPDF_CHECK(Contains(md, "Introduction .......") && !Contains(md, ". . ."), 1727);
}

// Header lines between a top and a mid rule set further apart than a line pitch (a label over two
// columns, a rule under it, the sub-labels below): the band is no table of its own, but its lines
// head the rows under the mid rule. Two lines of a note set there instead, the first running from
// the first column across the others, head nothing.
void TestHeaderBandUnderTopRule() {
    for (const bool caption : {false, true}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        HLine(page, 72, 400, 624);
        if (caption) {
            AddText(page, "Results of every trial in the study", 76, 612, 10, 0, 0.4f);
            AddText(page, "second year", 76, 582, 10, 0, 0.4f);
        } else {
            AddText(page, "Name", 76, 612);
            AddText(page, "Cells", 250, 612);
            AddText(page, "Source", 340, 612);
            HLine(page, 230, 320, 597);
            AddText(page, "One", 236, 582);
            AddText(page, "Two", 290, 582);
        }
        HLine(page, 72, 400, 576);
        const char* const names[] = {"Alpha", "Beta", "Gamma", "Delta"};
        for (int r = 0; r < 4; ++r) {
            const float y = 562 - 14.0f * float(r);
            AddText(page, names[r], 76, y);
            AddText(page, std::to_string(2 + r) + ".10", 236, y);
            AddText(page, std::to_string(4 + r) + ".60", 290, y);
            AddText(page, "lab", 340, y);
        }
        HLine(page, 72, 400, 512);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("header band under top rule", md);
        if (caption)
            RAYOPDF_CHECK(report.tables == 1 && Contains(md, "Results of every trial in the study") && !Contains(md, "| Results") &&
                              Contains(md, "| Alpha | 2.10 | 4.60 | lab |"),
                          898);
        else
            RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Name | Cells | Cells | Source |\n| --- | --- | --- | --- |\n"
                                                             "|  | One | Two |  |\n| Alpha | 2.10 | 4.60 | lab |"),
                          897);
    }
}

// Booktabs rows whose labels start with a dash, figures in the other columns ("- Female  1102
// (52.73)"): a table, the dashes no list markers. The same dashed lines without figures stay a list.
void TestDashedRowLabels() {
    for (const bool figures : {true, false}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        HLine(page, 72, 400, 716);
        AddText(page, "Variable", 76, 704);
        AddText(page, "Cases", 236, 704);
        AddText(page, "Share", 320, 704);
        HLine(page, 72, 400, 698);
        const char* const labels[] = {"Female", "Male", "Young", "Old"};
        for (int r = 0; r < 4; ++r) {
            const float y = 684 - 14.0f * float(r);
            AddText(page, "-", 76, y);
            AddText(page, labels[r], 84, y);
            AddText(page, figures ? std::to_string(110 + 7 * r) : std::string("many"), 236, y);
            AddText(page, figures ? "(" + std::to_string(40 + r) + ".50)" : std::string("some"), 320, y);
        }
        HLine(page, 72, 400, 624);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show("dashed row labels", md);
        RAYOPDF_CHECK(figures ? report.tables == 1 && Contains(md, "Female | 110 | (40.50) |") : report.tables == 0, 899);
    }
}

// A lattice framed by rules, a group of two sub-columns drawn without a rule between them: in its
// header cell the group's label stands centred over the sub-labels on a line of their own. The
// header cell parts into the label's row and the sub-labels' row, the label spanning the group; a
// label of another column on three lines ("Full / long / name") stays one cell spanning both.
void TestLatticeHeaderLevels() {
    for (const bool wrapped : {false, true}) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        for (float y : {716.0f, 680.0f, 620.0f}) HLine(page, 72, 340, y);
        for (float x : {72.0f, 140.0f, 340.0f}) VLine(page, x, 620, 716);
        AddText(page, wrapped ? "Full" : "Name", 76, wrapped ? 706 : 702);
        if (wrapped) AddText(page, "long", 76, 697);
        if (wrapped) AddText(page, "name", 76, 688);
        AddText(page, "Group", 212.5f, 702);
        AddText(page, "A1", 160, 688);
        AddText(page, "A2", 280, 688);
        const char* const names[] = {"one", "two", "three"};
        for (int r = 0; r < 3; ++r) {
            AddText(page, names[r], 76, 666 - 14.0f * float(r));
            AddText(page, "0.2" + std::to_string(r), 160, 666 - 14.0f * float(r));
            AddText(page, "0.7" + std::to_string(r), 280, 666 - 14.0f * float(r));
        }
        const std::string md = Run(pages);
        Show("lattice header levels", md);
        if (wrapped)
            RAYOPDF_CHECK(Contains(md, "| Full long name | Group | Group |\n| --- | --- | --- |\n|  | A1 | A2 |\n| one | 0.20 | 0.70 |"), 2307);
        else
            RAYOPDF_CHECK(Contains(md, "| Name | Group | Group |\n| --- | --- | --- |\n|  | A1 | A2 |\n| one | 0.20 | 0.70 |"), 2305);
    }
}

// The same lattice, its group parted by whitespace whose middle the right column's label crosses
// (centred over its narrow column), the left column's label ending before the left column's text
// does: the boundary moves into the whitespace left of the right label, each label heading its
// own column.
void TestLatticeLabelInGap() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {716.0f, 696.0f, 620.0f}) HLine(page, 72, 340, y);
    for (float x : {72.0f, 140.0f, 340.0f}) VLine(page, x, 620, 716);
    AddText(page, "Name", 76, 702);
    AddText(page, "Left", 165, 702);
    AddText(page, "Right side", 208, 702);
    const char* const names[] = {"one", "two", "three"};
    for (int r = 0; r < 3; ++r) {
        AddText(page, names[r], 76, 680 - 14.0f * float(r));
        AddText(page, "12.34" + std::to_string(r), 160, 680 - 14.0f * float(r));
        AddText(page, std::to_string(7 + r), 230, 680 - 14.0f * float(r));
    }
    const std::string md = Run(pages);
    Show("lattice label in gap", md);
    RAYOPDF_CHECK(Contains(md, "| Name | Left | Right side |\n| --- | --- | --- |\n| one | 12.340 | 7 |"), 2306);
}

// Booktabs with a header of three rows: a group label centred over all the columns of figures
// (no rule under it), above two labels each over three columns with a \cmidrule under it, above
// the columns' own labels; the stub's label sits in the middle of the header. The group label
// spans the six columns (its centre lies over one column's labels too, but the label under it
// spans past them), and the stub's label spans the header's rows (a pipe table writes it in the
// first).
void TestRuledHeaderGroupLabel() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    constexpr float kSize = 7.2f, kAdvance = 0.42f;
    const auto text = [&](std::string_view t, float x, float y) { AddText(page, t, x, y, kSize, 0, kAdvance); };
    HLine(page, 77.9f, 517.4f, 770);
    text("Training Datasets", 312.05f, 762.1f);
    text("Properties", 103.4f, 753.1f);
    text("Instruction", 222.4f, 753.1f);
    text("Alignment", 402.2f, 753.1f);
    HLine(page, 161, 311, 748.5f);
    HLine(page, 315, 517.4f, 748.5f);
    text("Alpaca-GPT4", 165.0f, 740.6f);
    text("OpenOrca", 212.9f, 740.6f);
    text("Synth. Math-Instruct", 250.4f, 740.6f);
    text("Orca DPO Pairs", 318.8f, 740.6f);
    text("Ultrafeedback Cleaned", 373.0f, 740.6f);
    text("Synth. Math-Alignment", 447.1f, 740.6f);
    HLine(page, 77.9f, 517.4f, 735.5f);
    const char* const names[] = {"Total", "Maximum", "Open"};
    const float xs[] = {178.8f, 218.2f, 272.7f, 333.1f, 397.2f, 473.8f};
    const char* const values[3][6] = {{"52K", "2.91M", "126K", "12.9K", "60.8K", "126K"},
                                      {"52K", "100K", "52K", "12.9K", "60.8K", "20.1K"},
                                      {"O", "O", "X", "O", "O", "X"}};
    for (int r = 0; r < 3; ++r) {
        const float y = 728.0f - 9.0f * float(r);
        text(names[r], 90, y);
        for (int c = 0; c < 6; ++c) text(values[r][c], xs[c], y);
    }
    HLine(page, 77.9f, 517.4f, 705);
    const std::string md = Run(pages);
    Show("ruled header group label", md);
    RAYOPDF_CHECK(Contains(md, "| Properties | Training Datasets | Training Datasets | Training Datasets | Training Datasets | "
                               "Training Datasets | Training Datasets |\n| --- |"),
                  2308);
}

// A lattice whose last body line is set in a font with a padded bounding box (each glyph's box
// reaching two sizes below its baseline): the line stays in its cell, by its baseline, though its
// boxes' middles lie under the table's bottom rule.
void TestLatticeTallGlyphBoxes() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    for (float y : {700.0f, 684.0f, 640.0f}) HLine(page, 72, 372, y);
    for (float x : {72.0f, 222.0f, 372.0f}) VLine(page, x, 640, 700);
    AddText(page, "Reagents", 76, 688);
    AddText(page, "Equipment", 226, 688);
    AddText(page, "Buffer", 76, 672);
    AddText(page, "Tube rack", 226, 672);
    AddText(page, "Enzyme", 76, 658);
    const size_t first = page.glyphs.size();
    AddText(page, "Sterile water", 76, 644);
    for (size_t i = first; i < page.glyphs.size(); ++i) page.glyphs[i].vb = page.glyphs[i].v - 2.0f * page.glyphs[i].size;
    const std::string md = Run(pages);
    Show("lattice tall glyph boxes", md);
    RAYOPDF_CHECK(Contains(md, "| Buffer Enzyme Sterile water | Tube rack |"), 2311);
}

// Booktabs whose header stands between a top rule more than six ems above the header rule (but
// under twelve) and that rule: a stub label of two lines and two group labels, each over three
// columns with a \cmidrule under it, above the columns' own labels (BMC's tables). The group
// labels head the table (they used to stay text above it: only the line right over the header
// rule was lifted).
void TestRuledTallHeaderBand() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    const auto text = [&](std::string_view t, float x, float y) { AddText(page, t, x, y, 8.2f, 0, 0.37f); };
    HLine(page, 54, 553, 691);
    text("Concentration", 123.2f, 679.4f);
    text("Intra-assay variability", 237.8f, 679.4f);
    text("Inter-assay variability", 424.6f, 679.4f);
    text("[nM]", 139.6f, 670.2f);
    HLine(page, 182.8f, 361.9f, 663.4f, 0.2f);
    HLine(page, 369.9f, 553, 663.4f, 0.2f);
    const float labelXs[] = {208, 250.4f, 322.7f, 395, 437.5f, 509.9f};
    const char* const labels[] = {"n", "Accuracy (%)", "CV (%)", "n", "Accuracy (%)", "CV (%)"};
    for (int c = 0; c < 6; ++c) text(labels[c], labelXs[c], 647.9f);
    HLine(page, 54, 553, 641.1f, 0.2f);
    const float xs[] = {139.5f, 208, 266.3f, 332.7f, 395.1f, 453.3f, 519.8f};
    const char* const values[4][7] = {{"1000", "3", "89", "2", "5", "100", "2"},
                                      {"100", "3", "103", "6", "5", "103", "3"},
                                      {"10", "3", "111", "2", "5", "104", "9"},
                                      {"1.2", "3", "104", "8", "5", "108", "14"}};
    text("Paclitaxel", 69.7f, 625.6f);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 7; ++c) text(values[r][c], xs[c], 625.6f - 10.2f * float(r));
    HLine(page, 54, 553, 580, 0.2f);
    const std::string md = Run(pages);
    Show("ruled tall header band", md);
    RAYOPDF_CHECK(Contains(md, "| Intra-assay variability | Intra-assay variability | Intra-assay variability | Inter-assay variability |"),
                  2309);
}

// Two group labels over \cmidrules as wide as their columns with the cell padding (BMC's tables):
// the first rule ends short of the gap's middle after its columns, the second starts past it.
// Each label spans its own three columns, not the gap between the groups (both labels used to
// make one cell over the six columns).
void TestCmidruleInGap() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    const auto text = [&](std::string_view t, float x, float y) { AddText(page, t, x, y, 8.2f, 0, 0.42f); };
    HLine(page, 54.1f, 553, 691);
    text("First PC (42%)", 318.7f, 679.4f);
    text("Second PC (21%)", 453.9f, 679.4f);
    // The figures' columns end at 409 and start again at 433: the gap's middle is at 421.
    HLine(page, 277.2f, 408.8f, 672.6f, 0.2f);
    HLine(page, 416.8f, 553, 672.6f, 0.2f);
    text("CAM categories", 58.1f, 657.1f);
    const float labelXs[] = {300.3f, 342.1f, 392.3f, 443.3f, 481.5f, 539.5f};
    const char* const labels[] = {"lci", "Loading", "uci", "lci", "Loading", "uci"};
    for (int c = 0; c < 6; ++c) text(labels[c], labelXs[c], 657.1f);
    HLine(page, 54.1f, 553, 650.4f, 0.2f);
    const char* const names[] = {"Alternative systems", "Biological therapies", "Energy therapies", "Body-based therapies"};
    const float xs[] = {294.2f, 353.7f, 394.8f, 433.8f, 490.6f, 532.4f};
    for (int r = 0; r < 4; ++r) {
        const float y = 634.9f - 10.2f * float(r);
        text(names[r], 58.1f, y);
        for (int c = 0; c < 6; ++c) text("0." + std::to_string(11 + 7 * r + c), xs[c] + 3, y);
    }
    HLine(page, 54.1f, 553, 585, 0.2f);
    const std::string md = Run(pages);
    Show("cmidrule in gap", md);
    RAYOPDF_CHECK(Contains(md, "| First PC (42%) | First PC (42%) | First PC (42%) | Second PC (21%) |"), 2310);
}

// A ruled table's first rows, then a section label alone in the first column set a little further
// down than the rows' pitch, then the rest of the rows without rules: one table, the label a row
// of its own (it used to end the ruled table and leave the rest a second table without header).
void TestSectionLabelBetweenParts() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    constexpr float kSize = 8;
    const auto text = [&](std::string_view t, float x, float y) { AddText(page, t, x, y, kSize); };
    HLine(page, 60, 540, 700);
    text("Overall", 220, 690);
    text("Cases", 320, 690);
    text("Controls", 420, 690);
    text("P", 520, 690);
    HLine(page, 60, 540, 685);
    text("Maternal age", 64, 674);
    text("32.7 (29.1)", 220, 674);
    text("33.0 (29.0)", 320, 674);
    text("32.7 (29.1)", 420, 674);
    text("0.62", 520, 674);
    text("Body mass", 64, 660);
    text("23.9 (21.4)", 220, 660);
    text("29.8 (24.5)", 320, 660);
    text("23.8 (21.3)", 420, 660);
    text("0.01", 520, 660);
    text("Maternal race", 64, 644);
    const char* const names[] = {"White", "Black", "Asian", "Other"};
    for (int r = 0; r < 4; ++r) {
        const float y = 630 - 14.0f * float(r);
        text(names[r], 72, y);
        text(std::to_string(231 - 40 * r) + " (60.3%)", 220, y);
        text(std::to_string(21 + r) + " (75.0%)", 320, y);
        text(std::to_string(210 - 40 * r) + " (59.2%)", 420, y);
        text("0." + std::to_string(11 + r), 520, y);
    }
    const std::string md = Run(pages);
    Show("section label between parts", md);
    RAYOPDF_CHECK(Count(md, "| --- | --- | --- | --- | --- |\n") == 1 && Contains(md, "| Maternal race |  |  |  |  |\n| White | 231 (60.3%) |"), 2312);
}

// Labels over two columns each wrapped onto a second line ("Triglycerides" over "(mg/dL)", both
// over "Base" and "Diff") beside a stub label wrapped too ("Study" over "year": the second line no
// wrapped text of the first alone): one label's text, one header row over the row of sub-labels.
void TestWrappedLevelLabels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 360, 714);
    AddText(page, "Study", 76, 700);
    AddText(page, "year", 76, 689);
    AddText(page, "Triglycerides", 152.5f, 700);
    AddText(page, "Homocysteine", 255, 700);
    AddText(page, "(mg/dL)", 167.5f, 689);
    AddText(page, "(uM)", 275, 689);
    AddText(page, "Base", 150, 676);
    AddText(page, "Diff", 200, 676);
    AddText(page, "Base", 260, 676);
    AddText(page, "Diff", 310, 676);
    HLine(page, 72, 360, 670);
    const char* const names[] = {"Kim", "Lee", "Park", "Cho"};
    float y = 658;
    for (int r = 0; r < 4; ++r, y -= 14) {
        AddText(page, names[r], 76, y);
        AddText(page, std::to_string(10 + r) + ".4", 150, y);
        AddText(page, "-" + std::to_string(1 + r) + ".5", 200, y);
        AddText(page, std::to_string(20 + r) + ".1", 260, y);
        AddText(page, "+" + std::to_string(2 + r) + ".8", 310, y);
    }
    HLine(page, 72, 360, y + 8);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("wrapped level labels", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "Study year") && Contains(md, "Triglycerides (mg/dL)") && Contains(md, "Homocysteine (uM)") &&
                      !Contains(md, "(uM) Base"),
                  2316);
}

// A ruled table whose rows stand apart (padding) and whose cells wrap at the line pitch: a wrapped
// line filling the first and last cells but not the date between them, with no number, is the
// wrapped text of its row ("(approx. 90% level)" under "Complete final design"), not a row.
void TestWrappedCellsInRowBands() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 450, 714);
    AddText(page, "Milestone", 76, 702);
    AddText(page, "Date", 200, 702);
    AddText(page, "Deliverable", 300, 702);
    HLine(page, 72, 450, 696);
    float y = 684;
    AddText(page, "Initiate pre-design", 76, y);
    AddText(page, "Sept 2001", 200, y);
    AddText(page, "Notice to proceed", 300, y);
    y -= 16;
    AddText(page, "Complete pre-design", 76, y);
    AddText(page, "March 2002", 200, y);
    AddText(page, "Design report, submitted", 300, y);
    y -= 11;
    AddText(page, "to the agency", 300, y);
    y -= 16;
    AddText(page, "Complete final design", 76, y);
    AddText(page, "March 2003", 200, y);
    AddText(page, "Plans and specs submitted", 300, y);
    y -= 11;
    AddText(page, "(approx. 90% level)", 76, y);
    AddText(page, "for review", 300, y);
    y -= 16;
    AddText(page, "Notice to build", 76, y);
    AddText(page, "October 2003", 200, y);
    AddText(page, "Notice to proceed", 300, y);
    HLine(page, 72, 450, y - 6);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("wrapped cells in row bands", md);
    RAYOPDF_CHECK(report.tables == 1 &&
                      Contains(md, "| Complete final design (approx. 90% level) | March 2003 | Plans and specs submitted for review |") &&
                      Contains(md, "| Complete pre-design | March 2002 | Design report, submitted to the agency |"),
                  2317);
}

// A header line a blank line above a borderless body, its labels one word each and set closer than
// a column gap ("Likely Unlikely Unknown" over three columns of counts): each word heads the column
// nearest it, and the line is the header row.
void TestOneWordHeaderLabels() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "Situation", 76, 700);
    AddText(page, "Likely", 222, 700);
    AddText(page, "Unlikely", 260, 700);
    AddText(page, "Unknown", 309, 700);
    const char* const labels[] = {"Listening to speech", "Sitting in traffic", "Waiting for a bus", "Driving at night"};
    const char* const counts[][3] = {{"118", "27", "12"}, {"60", "82", "15"}, {"66", "60", "26"}, {"90", "47", "15"}};
    float y = 676;
    for (int r = 0; r < 4; ++r, y -= 12) {
        AddText(page, labels[r], 76, y);
        AddText(page, counts[r][0], 230, y);
        AddText(page, counts[r][1], 275, y);
        AddText(page, counts[r][2], 320, y);
    }
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("one-word header labels", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Situation | Likely | Unlikely | Unknown |\n| --- |") &&
                      Contains(md, "| Listening to speech | 118 | 27 | 12 |"),
                  2319);
}

// Booktabs rules over a header of four lines: group labels with a \cmidrule under each, then the
// columns' labels, their group sizes "(n=30)" a line under them, and a line of statistics set a
// row's distance lower. Most header lines hold a number, yet the rules narrower than the table
// mark them a header: four lines above the header rule head the columns. The label and its size,
// set closer than the body's rows, are one cell; the statistics line is a row of its own.
void TestNumberedHeaderUnderCmidrules() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    HLine(page, 72, 540, 712);
    AddText(page, "Group 1", 210, 702);
    AddText(page, "Group 2", 410, 702);
    HLine(page, 148, 307, 697);
    HLine(page, 348, 507, 697);
    AddText(page, "Factor", 76, 688);
    const char* const labels[] = {"1 dose", "2 doses", "3 doses", "4 doses"};
    const char* const sizes[] = {"(n=30)", "(n=31)", "(n=32)", "(n=33)"};
    const float xs[] = {150, 250, 350, 450};
    for (int c = 0; c < 4; ++c) {
        AddText(page, labels[c], xs[c], 688);
        AddText(page, sizes[c], xs[c], 679);
        AddText(page, "OR (95% CI)", xs[c], 666);
    }
    HLine(page, 72, 540, 660);
    const char* const names[] = {"Physical", "Emotional", "Neglect", "Domestic"};
    for (int r = 0; r < 4; ++r) {
        const float y = 648 - 14.0f * float(r);
        AddText(page, names[r], 76, y);
        for (int c = 0; c < 4; ++c) AddText(page, std::to_string(r + 1) + "." + std::to_string(10 + 17 * c) + " (0.98)", xs[c], y);
    }
    HLine(page, 72, 540, 600);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("numbered header under cmidrules", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Factor | Group 1 | Group 1 | Group 2 | Group 2 |\n| --- |"), 2330);
    RAYOPDF_CHECK(Contains(md, "(n=33) |\n|  | OR (95% CI) | OR (95% CI) | OR (95% CI) | OR (95% CI) |\n| Physical |"), 2331);
    RAYOPDF_CHECK(Contains(md, "|  | 1 dose (n=30) | 2 doses (n=31) |"), 2332);
}

// Booktabs rules around an airy table: three rows of figures set at one pitch two lines apart,
// under a header band of group labels over \cmidrules and the columns' labels, taller than twelve
// ems. The rows are the table's body, the band its header.
void TestAiryRuledRows() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 760, 3);
    HLine(page, 72, 420, 712);
    AddText(page, "Model 1", 185, 690);
    AddText(page, "Model 2", 325, 690);
    HLine(page, 140, 270, 670);
    HLine(page, 280, 410, 670);
    const char* const labels[] = {"Mean", "Total", "Mean", "Total"};
    const float xs[] = {150, 210, 290, 350};
    for (int c = 0; c < 4; ++c) AddText(page, labels[c], xs[c], 600);
    HLine(page, 72, 420, 580);
    const char* const names[] = {"Attendances", "Admissions", "Both events"};
    for (int r = 0; r < 3; ++r) {
        const float y = 550 - 30.0f * float(r);
        AddText(page, names[r], 76, y);
        for (int c = 0; c < 4; ++c) AddText(page, std::to_string(7992 - 1311 * r + 17 * c), xs[c], y);
    }
    HLine(page, 72, 420, 470);
    AddProse(page, 72, 400, 440, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("airy ruled rows", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "| Attendances | 7992 | 8009 | 8026 | 8043 |\n| Admissions |"), 2335);
    RAYOPDF_CHECK(Contains(md, "|  | Model 1 | Model 1 | Model 2 | Model 2 |\n| --- |"), 2336);
}

// Booktabs top, header and bottom rules around rows of sentences in two columns (programme
// components and how they were adapted), set sideways on the page: a landscape table of text, a
// table although its columns hold prose. Upright, the same columns stay running text.
void TestRuledSentenceTable() {
    const auto build = [](bool turned) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        // Upright (x, y), or the turned frame's (u, v) = (x, y - 812): on the page, x = -v.
        const auto text = [&](std::string_view t, float x, float y) {
            if (turned) AddTurnedText(page, t, x, y - 812);
            else AddText(page, t, x, y);
        };
        const auto rule = [&](float y) {
            if (turned) page.rulings.push_back(Ruling{812 - y - 0.25f, 72, 812 - y + 0.25f, 520, kRulingStroke});
            else HLine(page, 72, 520, y);
        };
        rule(712);
        text("Component", 76, 700);
        text("Original programme", 160, 700);
        text("Adaptation in the new setting", 345, 700);
        rule(694);
        const char* const names[] = {"Diet", "Activity", "Rewards"};
        const char* const left[3][3] = {{"Elimination of red foods each", "week until eating two servings", "of them per day"},
                                        {"Lifestyle goal of thirty minutes", "each day of moderate activity", "or more as reported"},
                                        {"Identifying chances for praise", "and changing the rewards to", "ones not based on food"}};
        const char* const right[3][3] = {{"Elimination of red foods each", "week until eating two of them", "on most days"},
                                         {"Increased baseline steps by a", "thousand steps and required the", "goal on five days"},
                                         {"Praise from the parents and", "rewards chosen together with", "the child every week"}};
        float y = 682;
        for (int r = 0; r < 3; ++r) {
            text(names[r], 76, y);
            for (int l = 0; l < 3; ++l, y -= 11) {
                text(left[r][l], 160, y);
                text(right[r][l], 345, y);
            }
            y -= 6;
        }
        rule(y + 4);
        ConvertReport report;
        const std::string md = Run(pages, &report);
        Show(turned ? "ruled sentence table, turned" : "ruled sentence table, upright", md);
        return std::make_pair(report.tables, md);
    };
    const auto [turnedTables, turned] = build(true);
    RAYOPDF_CHECK(turnedTables == 1, 2337);
    RAYOPDF_CHECK(Contains(turned, "| Diet | Elimination of red foods each week until eating two servings of them per day |"), 2338);
    RAYOPDF_CHECK(build(false).first == 0, 2339);
}

// Booktabs around a header of labels a word space apart (0.39 em, on a page of words 0.33 em
// apart), each centred over its own narrow column of figures and wider than it, beside an empty
// corner cell (arXiv 2310.14638's Table A1): the header band is no label across the table, and
// each label heads its own column.
void TestCentredLabelsOverNarrowColumns() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 700, 3, 10, 0.33f);
    HLine(page, 72, 210, 610);
    AddText(page, "HR10", 127.75f, 598, 9);
    AddText(page, "HR20", 149.25f, 598, 9);
    AddText(page, "HR1+HR2", 170.75f, 598, 9);
    HLine(page, 72, 210, 590);
    const char* const names[] = {"Damping", "Mass", "Stiffness"};
    const char* const values[3][3] = {{"1.4", "1.7", "\u22120.10"}, {"5.4", "9.0", "\u22124.00"}, {"0.9", "0.8", "\u22120.39"}};
    const float xs[] = {130, 151.5f, 175.25f};
    for (int r = 0; r < 3; ++r) {
        const float y = 575 - 13.0f * float(r);
        AddText(page, names[r], 76, y, 9);
        for (int c = 0; c < 3; ++c) AddText(page, values[r][c], xs[c], y, 9);
    }
    HLine(page, 72, 210, 540);
    AddProse(page, 72, 400, 510, 3, 10, 0.33f);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("centred labels over narrow columns", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "|  | HR10 | HR20 | HR1+HR2 |\n| --- | --- | --- | --- |\n| Damping | 1.4 | 1.7 | \u22120.10 |"),
                  2301);
}

// A displayed formula: a fraction (F² over 2η) whose bar touches a \framebox drawn around a second
// fraction (arXiv 2310.14638's energy formula). The bar beside the box rules no column: no table.
void TestFractionBesideFramebox() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 450, 700, 3);
    AddText(page, "F2", 404, 238);
    HLine(page, 402.5f, 415.2f, 234.5f, 0.4f);
    AddText(page, "2\u03b7", 404, 224);
    HLine(page, 416, 514.1f, 250.1f, 0.4f);
    HLine(page, 416, 514.1f, 219.5f, 0.4f);
    VLine(page, 416.4f, 219.5f, 250.1f, 0.4f);
    VLine(page, 513.7f, 219.5f, 250.1f, 0.4f);
    AddText(page, "\u03b32\u03c92", 455, 238);
    HLine(page, 420.5f, 509.5f, 234.5f, 0.4f);
    AddText(page, "(\u03c92 \u2212 \u03c92)2 + (\u03b3\u03c9)2", 422, 224);
    AddProse(page, 72, 450, 190, 3);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("fraction beside framebox", md);
    RAYOPDF_CHECK(report.tables == 0 && Contains(md, "2\u03b7"), 2303);
}

// Booktabs around a band of one line, `words` at 9 pt set `gaps` em apart, over as many columns of
// figures beside an empty corner cell, each word centred over its own column, on a page of words
// 0.33 em apart. Returns the Markdown.
std::string BandOverColumns(std::initializer_list<const char*> words, std::initializer_list<float> gaps,
                            ConvertReport& report) {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 700, 3, 10, 0.33f);
    HLine(page, 72, 280, 610);
    std::vector<float> centres;
    float x = 120;
    const float* gap = gaps.begin();
    for (const char* word : words) {
        const float end = AddText(page, word, x, 598, 9);
        centres.push_back((x + end) / 2);
        x = end + (gap != gaps.end() ? *gap++ : 0.0f) * 9;
    }
    HLine(page, 72, 280, 590);
    const char* const names[] = {"Damping", "Mass", "Stiffness"};
    for (int r = 0; r < 3; ++r) {
        const float y = 575 - 13.0f * float(r);
        AddText(page, names[r], 76, y, 9);
        for (size_t c = 0; c < centres.size(); ++c) {
            const std::string value = std::to_string(r + 1) + "." + std::to_string(c + 4);
            AddText(page, value, centres[c] - 6.75f, y, 9);
        }
    }
    HLine(page, 72, 280, 540);
    AddProse(page, 72, 400, 510, 3, 10, 0.33f);
    return Run(pages, &report);
}

// A band of one phrase over three columns of figures, each word centred over its own column and a
// little more than the page's word space apart (arXiv 2210.15829's "Homogeneous treatment
// effects"): words that go on in lower case are one label, never three header cells.
void TestPhraseOverNarrowColumns() {
    ConvertReport report;
    std::string md = BandOverColumns({"Homogeneous", "treatment", "effects"}, {0.4f, 0.4f}, report);
    Show("lower-case phrase over columns", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "Homogeneous treatment effects") && !Contains(md, "| treatment |"), 2340);
    // Capitals no further apart than the page's word space (olmOCR's "ORDERING INFORMATION").
    md = BandOverColumns({"MEDIAN", "AGE"}, {0.3f}, report);
    Show("capitals a word space apart over columns", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "MEDIAN AGE") && !Contains(md, "| AGE |"), 2342);
    // A parenthesis that qualifies the word before it ("Number (female)").
    md = BandOverColumns({"Number", "(female)"}, {0.4f}, report);
    Show("parenthesis over columns", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "Number (female)") && !Contains(md, "| (female) |"), 2343);
}

// A booktabs header of rooms over pairs of columns (T and RH), "Room" centred over the first and
// the room's number over the second, a little more than the page's word space apart, "Model"
// centred down the two header lines with the \cmidrules under the rooms just below its baseline
// (arXiv 2403.04326's Table I): a number after a word goes on with its label, never two header
// cells.
void TestRoomLabelsOverColumnPairs() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddProse(page, 72, 400, 700, 3, 10, 0.33f);
    HLine(page, 72, 286.4f, 620);
    AddText(page, "Model", 76, 605.6f, 8);
    const char* const rooms[] = {"103", "205"};
    const float xs[] = {200, 250};
    for (int g = 0; g < 2; ++g) {
        const float end = AddText(page, "Room", xs[g], 610, 8, 0, 0.6f);
        AddText(page, rooms[g], end + 3.2f, 610, 8, 0, 0.6f);
        HLine(page, xs[g] + 1.5f, xs[g] + 36.4f, 605.4f, 0.4f);
        AddText(page, "T", xs[g] + 7.6f, 596, 8);
        AddText(page, "RH", xs[g] + 25.6f, 596, 8);
    }
    HLine(page, 72, 286.4f, 590);
    const char* const names[] = {"LSTM", "TCN", "TFT", "TiDE"};
    for (int r = 0; r < 4; ++r) {
        const float y = 582 - 11.0f * float(r);
        AddText(page, names[r], 76, y, 8);
        for (int g = 0; g < 2; ++g) {
            AddText(page, std::to_string(r + 2) + "." + std::to_string(g + 1), xs[g] + 3.6f, y, 8);
            AddText(page, std::to_string(r + 5) + std::to_string(g + 3), xs[g] + 25.6f, y, 8);
        }
    }
    HLine(page, 72, 286.4f, 534);
    AddProse(page, 72, 400, 500, 3, 10, 0.33f);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("room labels over column pairs", md);
    RAYOPDF_CHECK(report.tables == 1 && Contains(md, "Room 205") && Contains(md, "Room 103") && !Contains(md, "| 205 |"), 2341);
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) return Dump(argv[2], argc >= 4 ? std::atoi(argv[3]) : 0);
    TestLatticeSpans();
    TestLatticeRowSpans();
    TestSpanOneLineARow();
    TestLatticeGfm();
    TestCellEmphasisEscapes();
    TestLoneCellEmphasis();
    TestBooktabs();
    TestHeaderRule();
    TestBorderless();
    TestFigureText();
    TestFigureLabels();
    TestFigureCorners();
    TestLatticeInFigure();
    TestHeatMapAxes();
    TestLatticeRulingPastTop();
    TestLatticeTwoLineRow();
    TestAuthorBlock();
    TestRuledSectionsNoHeader();
    TestAtSignInCells();
    TestTallHeader();
    TestBorderlessWordOrder();
    TestTwoColumnProse();
    TestListNotTable();
    TestAlgorithmBox();
    TestRuledSectionLabels();
    TestLoneSectionLabels();
    TestRuledRowGroups();
    TestCentredRowLabels();
    TestBlankLineInBody();
    TestTableAcrossColumns();
    TestHostileRulings();
    TestDegenerateGrids();
    TestFractionBar();
    TestFractionBesideText();
    TestDoubleRules();
    TestColumnLattice();
    TestStripes();
    TestClosedEmptyColumn();
    TestSpanningHeaderLabel();
    TestStackedLatticeRows();
    TestLatticeSubColumns();
    TestLatticeSubColumnLabels();
    TestHeaderAboveBox();
    TestSectionLabels();
    TestPairsHeaderAndContents();
    TestLatticeTitleRow();
    TestTurnedTable();
    TestTurnedTickLabels();
    TestChartFrame();
    TestFormGrid();
    TestWideHeaderLabels();
    TestRuledGroupHeader();
    TestRuledSubColumnsUnderLabel();
    TestRowsBelowLattice();
    TestRepeatedHeaderRow();
    TestMiddleHeaderCell();
    TestHeaderAboveRuledRows();
    TestDiagramGrids();
    TestSparseLattice();
    TestTimingBoxes();
    TestEquationArray();
    TestEquationGrids();
    TestBracketMatrix();
    TestLatticeRowsByVerticals();
    TestLatticeSections();
    TestCellNumbersLatex();
    TestLatticeBesideChart();
    TestCrossingAreas();
    TestBoxGrid();
    TestRuledColumnTwoRows();
    TestLatticeAcrossFigures();
    TestCodeColumnTable();
    TestWordBox();
    TestHeaderMiddleLevels();
    TestCmidruleHeader();
    TestTouchingStripes();
    TestHeaderAtColumns();
    TestTitleBand();
    TestDotsNotRulings();
    TestTickLabelsBesidePlots();
    TestGridMostlyInFigure();
    TestProseRiverUnderRule();
    TestHeaderLinesAtRowPitch();
    TestBottomAlignedHeader();
    TestSmallerLabelRows();
    TestRuleJoints();
    TestMiddleSetRows();
    TestHeaderAboveRules();
    TestCloseNumberColumns();
    TestContentsLists();
    TestHeaderBandUnderTopRule();
    TestDashedRowLabels();
    TestLatticeHeaderLevels();
    TestLatticeLabelInGap();
    TestRuledHeaderGroupLabel();
    TestLatticeTallGlyphBoxes();
    TestRuledTallHeaderBand();
    TestCmidruleInGap();
    TestSectionLabelBetweenParts();
    TestWrappedLevelLabels();
    TestWrappedCellsInRowBands();
    TestOneWordHeaderLabels();
    TestNumberedHeaderUnderCmidrules();
    TestAiryRuledRows();
    TestRuledSentenceTable();
    TestCentredLabelsOverNarrowColumns();
    TestFractionBesideFramebox();
    TestPhraseOverNarrowColumns();
    TestRoomLabelsOverColumnPairs();
    std::puts("test_layout_tables: ok");
    return 0;
}
