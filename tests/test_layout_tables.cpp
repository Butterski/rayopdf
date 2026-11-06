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
#include "layout/interpreter.h"
#include "layout/lines.h"
#include "layout/page.h"
#include "layout/pipeline.h"
#include "layout/reading_order.h"
#include "layout/tables.h"
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
    RAYOPDF_CHECK(Contains(md, "| **Region** | **Q1** | **Q2** |\n| --- | --- | --- |\n| North | 1,200 | 1,340 |"), 557);
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
        if (std::getenv("RAYOPDF_DUMP_LINES"))
            for (const Line& l : page.lines) {
                LineText(page, l, text);
                std::printf("  L %.1f..%.1f base=%.1f sz=%.1f fl=%x | %s\n", l.u0, l.u1, l.baseline, l.size, l.flags,
                            text.c_str());
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

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--dump") == 0) return Dump(argv[2], argc >= 4 ? std::atoi(argv[3]) : 0);
    TestLatticeSpans();
    TestLatticeGfm();
    TestBooktabs();
    TestHeaderRule();
    TestBorderless();
    TestTwoColumnProse();
    TestListNotTable();
    TestTableAcrossColumns();
    TestHostileRulings();
    TestDegenerateGrids();
    TestFractionBar();
    TestDoubleRules();
    TestColumnLattice();
    std::puts("test_layout_tables: ok");
    return 0;
}
