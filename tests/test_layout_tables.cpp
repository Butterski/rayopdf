// SPDX-License-Identifier: Apache-2.0
// Core tests of src/layout/tables: lattice, partially ruled and borderless tables on pages built
// glyph by glyph, the false tables that must not appear (prose in two columns, lists), a table
// between columns of text, and hostile rulings. Exit codes 550-599.
//
// `test_layout_tables --dump FILE.pdf [PAGE]` prints the rulings and tables found on the pages of
// a PDF (a development aid).
#include "check.h"

#include "common/file.h"
#include "common/utf8.h"
#include "layout/figures.h"
#include "layout/interpreter.h"
#include "layout/lines.h"
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

std::string Run(std::vector<PageData>& pages, ConvertReport* reportOut = nullptr) {
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report);
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

// Prose filling [x0, x0 + width) from y down, `lines` lines of words.
float AddProse(PageData& page, float x0, float width, float y, int lines, float size = 10) {
    static const char* const kWords[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dogs", "while",
                                         "seven", "wizards", "quietly", "examine", "every", "document"};
    int w = 0;
    for (int l = 0; l < lines; ++l) {
        float x = x0;
        for (;;) {
            const char* word = kWords[w % 15];
            const float advance = float(std::strlen(word) + 1) * 0.5f * size;
            if (x + advance > x0 + width) break;
            x = AddText(page, word, x, y, size);
            x = AddText(page, " ", x, y, size);
            ++w;
        }
        y -= 1.2f * size;
    }
    return y;
}

// A 3 x 3 lattice with a header cell spanning the last two columns.
void TestLatticeSpans() {
    std::vector<PageData> pages{NewPage()};
    PageData& page = pages[0];
    AddText(page, "Intro text before the table.", 72, 720);
    const float xs[] = {72, 200, 320, 440};
    const float ys[] = {700, 680, 660, 640};
    for (float y : ys) HLine(page, 72, 440, y);
    VLine(page, 72, 640, 700);
    VLine(page, 200, 640, 700);
    VLine(page, 320, 640, 680);  // missing in the header row: "Results" spans two columns
    VLine(page, 440, 640, 700);
    AddText(page, "Name", xs[0] + 4, 686);
    AddText(page, "Results", xs[1] + 4, 686);
    AddText(page, "Alpha", xs[0] + 4, 666);
    AddText(page, "12", xs[1] + 4, 666);
    AddText(page, "34", xs[2] + 4, 666);
    AddText(page, "Beta", xs[0] + 4, 646);
    AddText(page, "56", xs[1] + 4, 646);
    AddText(page, "78", xs[2] + 4, 646);
    AddText(page, "Text after the table.", 72, 620);
    ConvertReport report;
    const std::string md = Run(pages, &report);
    Show("lattice spans", md);
    RAYOPDF_CHECK(report.tables == 1, 550);
    RAYOPDF_CHECK(Contains(md, "<th colspan=\"2\">Results</th>"), 551);
    RAYOPDF_CHECK(Contains(md, "<tr><td>Alpha</td><td>12</td><td>34</td></tr>"), 552);
    RAYOPDF_CHECK(md.find("Intro text") < md.find("<table>") && md.find("</table>") < md.find("Text after"), 553);
    RAYOPDF_CHECK(Count(md, "Alpha") == 1, 554);
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

// Figure regions from marks whose cells touch only at their corners: eight marks set corner to
// corner on a diagonal are one cluster that weighs enough; the same eight with one empty cell
// between the fourth and the fifth are two that do not. Six hundred long slanted strokes far
// from them (no marks, no curves) cover so many cells over each other that the cells are counted
// instead of painted: the regions stay the same.
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
    const PageData sparse = build(false), crowded = build(true);
    std::vector<FigureRegion> regions, counted;
    FigureRegions(sparse, regions);
    RAYOPDF_CHECK(regions.size() == 1 && regions[0].x0 == 80.5f && regions[0].y0 == 80.5f && regions[0].x1 == 175.5f &&
                      regions[0].y1 == 175.5f,
                  879);
    FigureRegions(crowded, counted);
    RAYOPDF_CHECK(counted.size() == 1 && std::memcmp(&counted[0], &regions[0], sizeof(FigureRegion)) == 0, 880);
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
// label spans them (HTML colspan); dot leaders after a label are not cell text.
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
    RAYOPDF_CHECK(Contains(md, "<th colspan=\"2\">Mean log DNA copies per gram</th>"), 585);
    RAYOPDF_CHECK(Contains(md, "<tr><td>Bifidobacterium</td><td>4.79</td><td>5.31</td></tr>"), 586);
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
        g.x0 = -g.vt, g.x1 = -g.vb, g.y0 = g.u0, g.y1 = g.u1;
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
    RAYOPDF_CHECK(Contains(md, "<th colspan=\"2\">Results</th>") && Contains(md, "<tr><td>Beta</td><td>13</td><td>0.52</td></tr>"),
                  589);
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
// header.
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
    RAYOPDF_CHECK(Contains(md, "<tr><th>Learning activity</th><th>Ects</th><th>Sector</th></tr>"), 594);
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
// figures are the header, and the figures' own columns part the body.
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
    RAYOPDF_CHECK(Contains(md, "<td>Samples</td><td>52K</td><td>2.91M</td><td>126K</td>"), 853);
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

int Dump(const char* path, int only) {
    File::InputFile file;
    if (file.Open(path, uint64_t(1) << 31) != File::ReadStatus::Ok) return 2;
    std::unique_ptr<Document> doc;
    if (Document::Open(file.Bytes(), "", doc) != OpenStatus::Ok) return 3;
    Text::FontCache fonts(*doc);
    std::vector<PageData> pages(static_cast<size_t>(doc->PageCount()));
    for (int i = 0; i < doc->PageCount(); ++i) {
        PageStats stats;
        InterpretPage(*doc, fonts, i, false, pages[static_cast<size_t>(i)], stats);
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

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) return Dump(argv[2], argc >= 4 ? std::atoi(argv[3]) : 0);
    TestLatticeSpans();
    TestLatticeGfm();
    TestBooktabs();
    TestHeaderRule();
    TestBorderless();
    TestFigureText();
    TestFigureCorners();
    TestLatticeInFigure();
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
    TestRuledRowGroups();
    TestTableAcrossColumns();
    TestHostileRulings();
    TestDegenerateGrids();
    TestFractionBar();
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
    TestChartFrame();
    TestFormGrid();
    TestWideHeaderLabels();
    TestRuledGroupHeader();
    TestRowsBelowLattice();
    TestMiddleHeaderCell();
    TestHeaderAboveRuledRows();
    TestDiagramGrids();
    TestBracketMatrix();
    TestWordBox();
    std::puts("test_layout_tables: ok");
    return 0;
}
