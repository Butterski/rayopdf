// SPDX-License-Identifier: Apache-2.0
// Core tests of the output options (layout/convert.h, layout/markdown_writer.h WriteSettings):
// plain text without inline markup, every table as HTML or as a pipe table, page furniture kept,
// the exact recovery they turn off, chunks for retrieval (WriteChunks) and the layout inspector
// (WriteInspect). Exit codes 2000-2049.
#include "check.h"
#include "exact_builder.h"

#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/page.h"
#include "layout/pipeline.h"
#include "pdf/document.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

// A link annotation over [x0, x1] x [y0, y1] to `uri`.
void AddLink(PageData& page, float x0, float y0, float x1, float y1, std::string_view uri) {
    LinkArea link;
    link.x0 = x0, link.y0 = y0, link.x1 = x1, link.y1 = y1;
    link.uri = static_cast<uint32_t>(page.text.size());
    link.uriLength = static_cast<uint32_t>(uri.size());
    page.text.append(uri);
    page.links.push_back(link);
}

void HLine(PageData& page, float x0, float x1, float y) {
    page.rulings.push_back(Ruling{x0, y - 0.25f, x1, y + 0.25f, kRulingStroke});
}

void VLine(PageData& page, float x, float y0, float y1) {
    page.rulings.push_back(Ruling{x - 0.25f, y0, x + 0.25f, y1, kRulingStroke});
}

PageData NewPage(int number) {
    PageData page;
    page.number = number;
    page.width = 612;
    page.height = 792;
    return page;
}

// The pages after ConvertPages with `output` (the pages are built again for each run: the
// stages change them).
template <typename Build>
std::string Run(Build build, const OutputSettings& output) {
    std::vector<PageData> pages = build();
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report, nullptr, nullptr, MathMode::Unicode, nullptr, nullptr, &output);
    return md;
}

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

int Count(const std::string& haystack, std::string_view needle) {
    int n = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) ++n;
    return n;
}

void Show(const char* name, const std::string& md) {
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", name, md.c_str());
}

// A paragraph with a bold word, an italic word, a word under a link, an ordinal's raised suffix
// and a footnote reference, and the footnote at the foot of the page.
std::vector<PageData> StyledPage() {
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    float x = AddText(page, "Plain words with ", 72, 700, 10);
    x = AddText(page, "strong", x, 700, 10, kGlyphBold);
    x = AddText(page, " and ", x, 700, 10);
    x = AddText(page, "slanted", x, 700, 10, kGlyphItalic);
    x = AddText(page, " text on the ", x, 700, 10);
    const float link = x;
    x = AddText(page, "website", x, 700, 10);
    AddLink(page, link, 697, x, 709, "https://example.org/");
    x = AddText(page, " of the 1", x, 700, 10);
    x = AddText(page, "st", x, 704, 6);
    x = AddText(page, " edition.", x, 700, 10);
    AddText(page, "1", x, 704, 6);
    x = AddText(page, "1", 72, 104, 5);
    AddText(page, "Smith, Collected Essays, 2020.", x, 100, 8);
    return pages;
}

void TestPlain() {
    const std::string marked = Run(StyledPage, OutputSettings());
    OutputSettings settings;
    settings.plain = true;
    const std::string plain = Run(StyledPage, settings);
    Show("styled", marked);
    Show("plain", plain);
    // The default output has the markup the plain output leaves out.
    RAYOPDF_CHECK(Contains(marked, "**strong**") && Contains(marked, "*slanted*") &&
                      Contains(marked, "[website](https://example.org/)") && Contains(marked, "1<sup>st</sup>") &&
                      Contains(marked, "edition.[^1]") && Contains(marked, "[^1]: Smith"),
                  2005);
    RAYOPDF_CHECK(!Contains(plain, "*") && !Contains(plain, "[") && !Contains(plain, "](") && !Contains(plain, "<sup>") &&
                      !Contains(plain, "[^") &&
                      Contains(plain, "Plain words with strong and slanted text on the website of the 1st edition.1"),
                  2000);
    RAYOPDF_CHECK(Contains(plain, "\n\n1 Smith, Collected Essays, 2020.\n"), 2001);
}

// A ruled table of three rows and two columns, no spans.
std::vector<PageData> TablePage() {
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    AddText(page, "Intro text before the table.", 72, 740, 10);
    const float xs[] = {72, 200, 320};
    const float ys[] = {720, 700, 680, 660};
    for (float y : ys) HLine(page, xs[0], xs[2], y);
    for (float x : xs) VLine(page, x, ys[3], ys[0]);
    const char* const cells[3][2] = {{"Name", "Value"}, {"Alpha", "12"}, {"Beta", "34"}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 2; ++c) AddText(page, cells[r][c], xs[c] + 6, ys[r] - 14, 10);
    AddText(page, "Closing text after the table.", 72, 630, 10);
    return pages;
}

void TestHtmlTables() {
    const std::string gfm = Run(TablePage, OutputSettings());
    OutputSettings settings;
    settings.tables = TableFormat::Html;
    const std::string html = Run(TablePage, settings);
    Show("table auto", gfm);
    Show("table html", html);
    RAYOPDF_CHECK(Contains(gfm, "| --- |") && !Contains(gfm, "<table>"), 2006);
    RAYOPDF_CHECK(Contains(html, "<table>") && !Contains(html, "| --- |") && Contains(html, "<th> Name </th>") &&
                      Contains(html, "<td> 34 </td>"),
                  2002);
    // One row a line, a space between its tags and around each cell's text.
    RAYOPDF_CHECK(Contains(html, "<table>\n<tr> <th> Name </th> <th> Value </th> </tr>\n<tr> <td> Alpha </td> <td> 12 </td> </tr>\n"
                                 "<tr> <td> Beta </td> <td> 34 </td> </tr>\n</table>"),
                  2019);
}

// A lattice of three columns under a header of two rows: "Method" spans both header rows,
// "Results" the two columns over "Mean" and "Max"; "Group A" spans the first two body rows (by
// default the table is HTML).
std::vector<PageData> RowSpanPage() {
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    for (float y : {720.0f, 680.0f, 640.0f, 620.0f}) HLine(page, 72, 440, y);
    HLine(page, 200, 440, 660);
    HLine(page, 200, 440, 700);
    VLine(page, 72, 620, 720);
    VLine(page, 200, 620, 720);
    VLine(page, 440, 620, 720);
    VLine(page, 320, 620, 700);
    AddText(page, "Method", 76, 696, 10);
    AddText(page, "Results", 204, 706, 10);
    AddText(page, "Mean", 204, 686, 10);
    AddText(page, "Max", 324, 686, 10);
    AddText(page, "Group A", 76, 656, 10);
    AddText(page, "12", 204, 666, 10);
    AddText(page, "56", 204, 646, 10);
    AddText(page, "Gamma", 76, 626, 10);
    AddText(page, "90", 204, 626, 10);
    AddText(page, "34", 324, 666, 10);
    AddText(page, "78", 324, 646, 10);
    AddText(page, "11", 324, 626, 10);
    return pages;
}

// --tables=pipe: every table a pipe table, a spanning cell's text in its first row and column
// only, the header's second row a body row; a table without spans as by default.
void TestPipeTables() {
    const std::string spans = Run(RowSpanPage, OutputSettings());
    OutputSettings settings;
    settings.tables = TableFormat::Pipe;
    const std::string pipe = Run(RowSpanPage, settings);
    Show("row spans auto", spans);
    Show("row spans pipe", pipe);
    RAYOPDF_CHECK(Contains(spans, "<td rowspan=\"2\">Group A</td>"), 2009);
    RAYOPDF_CHECK(Contains(pipe, "|\n| Group A | 12 | 34 |\n|  | 56 | 78 |\n| Gamma | 90 | 11 |"), 2022);
    RAYOPDF_CHECK(Contains(pipe, "| Method | Results |  |\n| --- | --- | --- |\n|  | Mean | Max |\n| Group A |"), 2023);
    RAYOPDF_CHECK(!Contains(pipe, "<t") && Run(TablePage, settings) == Run(TablePage, OutputSettings()), 2024);
}

// Three pages under one running header and over page numbers.
std::vector<PageData> RunningPages() {
    std::vector<PageData> pages;
    for (int p = 1; p <= 3; ++p) {
        pages.push_back(NewPage(p));
        AddText(pages.back(), "ACME Quarterly Report 2026", 72, 760, 9);
        AddText(pages.back(), "Body text of page " + std::to_string(p) + " stays.", 72, 600, 10);
        AddText(pages.back(), std::to_string(p), 300, 40, 9);
    }
    return pages;
}

void TestFurniture() {
    const std::string dropped = Run(RunningPages, OutputSettings());
    OutputSettings settings;
    settings.furniture = Furniture::Keep;
    const std::string kept = Run(RunningPages, settings);
    Show("furniture drop", dropped);
    Show("furniture keep", kept);
    RAYOPDF_CHECK(Count(dropped, "ACME Quarterly Report 2026") == 0 && Count(kept, "ACME Quarterly Report 2026") == 3 &&
                      Contains(kept, "Body text of page 3 stays."),
                  2003);
}

// Three pages under a bold running head in about the body's size, the first opening with a title.
std::vector<PageData> BoldRunningPages() {
    std::vector<PageData> pages;
    for (int p = 1; p <= 3; ++p) {
        pages.push_back(NewPage(p));
        PageData& page = pages.back();
        AddText(page, "Jailed for Doing Business", 72, 760, 10, kGlyphBold);
        float y = 700;
        if (p == 1) {
            AddText(page, "Executive Summary", 72, y, 24);
            y = 650;
        }
        for (int i = 0; i < 4; ++i, y -= 13)
            AddText(page, "Rules of trade weigh on firms in line " + std::to_string(i) + " of page " + std::to_string(p) + ",",
                    72, y, 10.5f);
    }
    return pages;
}

// One page under a head the producer marked as an artifact, larger than the body.
std::vector<PageData> MarkedHeadPage() {
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    AddText(page, "MOHAVE COMMUNITY COLLEGE BIO181", 72, 745, 14, kGlyphArtifact);
    float y = 700;
    for (int i = 1; i <= 6; ++i, y -= 24)
        AddText(page, std::to_string(i) + ". Place the tubes in the rack and spin them for three minutes.", 72, y, 10);
    return pages;
}

// Kept furniture is text, never a heading, and no block with the text under it.
void TestKeptFurnitureText() {
    OutputSettings settings;
    settings.furniture = Furniture::Keep;
    const std::string dropped = Run(MarkedHeadPage, OutputSettings());
    const std::string marked = Run(MarkedHeadPage, settings);
    Show("marked head dropped", dropped);
    Show("marked head kept", marked);
    RAYOPDF_CHECK(!Contains(dropped, "MOHAVE") && !Contains(marked, "# MOHAVE") &&
                      Contains(marked, "MOHAVE COMMUNITY COLLEGE BIO181\n\n1. Place the tubes"),
                  2016);
    const std::string running = Run(BoldRunningPages, settings);
    Show("kept bold running head", running);
    RAYOPDF_CHECK(Count(running, "Jailed for Doing Business") == 3 && !Contains(running, "# Jailed") &&
                      Contains(running, "# Executive Summary"),
                  2015);
}

// Three two-column pages under a running head over the right column and over a page number
// under the left one.
std::vector<PageData> ColumnHeadPages() {
    std::vector<PageData> pages;
    for (int p = 1; p <= 3; ++p) {
        pages.push_back(NewPage(p));
        PageData& page = pages.back();
        AddText(page, "Studies in Rivers", 420, 765, 9);
        for (int i = 0; i < 12; ++i) {
            const std::string n = std::to_string(i);
            AddText(page, "Left column line " + n + " runs on here,", 72, 740 - 13.0f * i, 10);
            AddText(page, "Right column line " + n + " runs on too,", 320, 740 - 13.0f * i, 10);
        }
        AddText(page, std::to_string(16 + p), 72, 40, 9);
    }
    return pages;
}

// Kept furniture is read apart from the text: a head before it, a foot after it.
void TestKeptFurnitureOrder() {
    const std::string dropped = Run(ColumnHeadPages, OutputSettings());
    OutputSettings settings;
    settings.furniture = Furniture::Keep;
    const std::string kept = Run(ColumnHeadPages, settings);
    Show("column head dropped", dropped);
    Show("column head kept", kept);
    const size_t head = kept.find("Studies in Rivers"), left = kept.find("Left column line 0"),
                 right = kept.find("Right column line 11"), number = kept.find("17");
    RAYOPDF_CHECK(!Contains(dropped, "Studies in Rivers") && Count(kept, "Studies in Rivers") == 3 && head < left &&
                      right < number && number != std::string::npos,
                  2017);
}

// Three pages over a running footer, the last with a footnote printed above its footer.
std::vector<PageData> FootnoteFooterPages() {
    std::vector<PageData> pages;
    for (int p = 1; p <= 3; ++p) {
        pages.push_back(NewPage(p));
        PageData& page = pages.back();
        float x = AddText(page, "Body text of page " + std::to_string(p) + " stays.", 72, 600, 10);
        if (p == 3) {
            AddText(page, "1", x, 604, 6);
            x = AddText(page, "1", 72, 104, 5);
            AddText(page, "Smith, Collected Essays, 2020.", x, 100, 8);
        }
        AddText(page, "ACME Quarterly Report 2026", 72, 40, 9);
    }
    return pages;
}

// Kept furniture at a page's foot is read after the page's notes, as printed.
void TestKeptFootUnderNotes() {
    OutputSettings settings;
    settings.furniture = Furniture::Keep;
    const std::string kept = Run(FootnoteFooterPages, settings);
    Show("footer under a note", kept);
    const size_t note = kept.find("[^1]: Smith"), footer = kept.rfind("ACME Quarterly Report 2026");
    RAYOPDF_CHECK(Count(kept, "ACME Quarterly Report 2026") == 3 && note != std::string::npos && note < footer &&
                      Contains(kept, "Body text of page 3 stays.[^1]"),
                  2018);
}

// A paragraph with curly, low and straight quotation marks, an apostrophe and dashes.
std::vector<PageData> QuotesPage() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "He said \xE2\x80\x9C" "fine\xE2\x80\x9D and \xE2\x80\x98" "so\xE2\x80\x99 \xE2\x80\x94 it\xE2\x80\x99s "
            "\xE2\x80\x9E" "gut\xE2\x80\x9C, 1990\xE2\x80\x93" "99, \"plain\".", 72, 700, 10);
    return pages;
}

// --quotes=ascii: typographic quotation marks as ' and ", dashes and straight quotes as printed.
void TestAsciiQuotes() {
    const std::string kept = Run(QuotesPage, OutputSettings());
    OutputSettings settings;
    settings.quotes = Quotes::Ascii;
    const std::string ascii = Run(QuotesPage, settings);
    Show("quotes keep", kept);
    Show("quotes ascii", ascii);
    RAYOPDF_CHECK(Contains(kept, "He said \xE2\x80\x9C" "fine\xE2\x80\x9D and \xE2\x80\x98" "so\xE2\x80\x99"), 2020);
    RAYOPDF_CHECK(Contains(ascii, "He said \"fine\" and 'so' \xE2\x80\x94 it's \"gut\", 1990\xE2\x80\x93" "99, \"plain\"."),
                  2021);
}

// A RayoMD PDF with its source: the output options need the pages, so the payload is not used.
void TestExactFormat() {
    Test::ProfilePdf spec;
    spec.source = "# Quarterly note\n\nRevenue grew in every region during the third quarter.\n";
    spec.lines = {"Quarterly note", "Revenue grew in every region during the third quarter."};
    const std::string pdf = Test::BuildProfilePdf(spec);
    std::unique_ptr<Document> document;
    RAYOPDF_CHECK(Document::Open(pdf, "", document) == OpenStatus::Ok, 2007);
    ConvertOptions options;
    options.ocr.mode = OcrMode::Off;
    std::string markdown;
    ConvertReport report;
    Convert(*document, options, markdown, report);
    RAYOPDF_CHECK(report.exact == ExactUse::Used, 2008);
    bool format = true;
    for (int variant = 0; variant < 5; ++variant) {
        ConvertOptions changed = options;
        if (variant == 0) changed.plain = true;
        if (variant == 1) changed.tables = TableFormat::Html;
        if (variant == 2) changed.furniture = Furniture::Keep;
        if (variant == 3) changed.quotes = Quotes::Ascii;
        if (variant == 4) changed.tables = TableFormat::Pipe;
        Convert(*document, changed, markdown, report);
        format = format && report.exact == ExactUse::Format && Contains(markdown, "Revenue grew");
    }
    RAYOPDF_CHECK(format, 2004);
}

// The values of `key` (a JSON string member) in the lines of `jsonl`, one a line, unescaped
// (\n, \", \\ and \uXXXX below U+0080: what the tests' text needs).
std::vector<std::string> Strings(const std::string& jsonl, std::string_view key) {
    std::vector<std::string> values;
    const std::string marker = "\"" + std::string(key) + "\":\"";
    for (size_t at = jsonl.find(marker); at != std::string::npos; at = jsonl.find(marker, at)) {
        at += marker.size();
        std::string value;
        for (; at < jsonl.size() && jsonl[at] != '"'; ++at) {
            if (jsonl[at] != '\\' || at + 1 >= jsonl.size()) {
                value.push_back(jsonl[at]);
                continue;
            }
            const char e = jsonl[++at];
            if (e == 'n') {
                value.push_back('\n');
            } else if (e == 'u' && at + 4 < jsonl.size()) {
                value.push_back(static_cast<char>(std::strtol(jsonl.substr(at + 1, 4).c_str(), nullptr, 16)));
                at += 4;
            } else {
                value.push_back(e);
            }
        }
        values.push_back(value);
    }
    return values;
}

// The text inside the brackets of `"key":[...]` on each line of `jsonl` (arrays of numbers or
// strings without brackets).
std::vector<std::string> Arrays(const std::string& jsonl, std::string_view key) {
    std::vector<std::string> values;
    const std::string marker = "\"" + std::string(key) + "\":[";
    for (size_t at = jsonl.find(marker); at != std::string::npos; at = jsonl.find(marker, at)) {
        at += marker.size();
        const size_t end = jsonl.find(']', at);
        values.push_back(jsonl.substr(at, end - at));
    }
    return values;
}

OutputSettings ChunkSettings(uint32_t chars) {
    OutputSettings settings;
    settings.format = OutputFormat::Chunks;
    settings.chunkChars = chars;
    return settings;
}

// A title, a paragraph, a section of two paragraphs and a section of one.
std::vector<PageData> SectionsPage() {
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    AddText(page, "Annual Review", 72, 740, 24);
    AddText(page, "The year brought steady growth across every region.", 72, 700, 10);
    AddText(page, "Results", 72, 660, 16);
    AddText(page, "Sales rose by a tenth in the first half.", 72, 630, 10);
    AddText(page, "Costs fell slightly over the same period.", 72, 600, 10);
    AddText(page, "Outlook", 72, 560, 16);
    AddText(page, "The next year should bring more of the same.", 72, 530, 10);
    return pages;
}

// One paragraph of about 1,000 bytes: sentences of about 50 bytes on lines of at most 90.
std::string LongParagraph() {
    std::string text;
    for (int i = 1; text.size() < 1000; ++i) {
        if (!text.empty()) text.push_back(' ');
        text += "Clause " + std::to_string(i) + " describes the quiet river near the old mill.";
    }
    return text;
}

std::vector<PageData> LongParagraphPage() {
    std::vector<PageData> pages{NewPage(1)};
    const std::string text = LongParagraph();
    float y = 740;
    for (size_t at = 0; at < text.size(); y -= 12) {
        size_t end = std::min(text.size(), at + 90);
        if (end < text.size()) end = text.rfind(' ', end);
        AddText(pages[0], std::string_view(text).substr(at, end - at), 72, y, 10);
        at = end + 1;
    }
    return pages;
}

// A ruled table of a header row and 40 rows, two columns.
std::vector<PageData> LongTablePage() {
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    const float xs[] = {72, 200, 320};
    const int rows = 41;
    for (int r = 0; r <= rows; ++r) HLine(page, xs[0], xs[2], 760 - 16.0f * r);
    for (float x : xs) VLine(page, x, 760 - 16.0f * rows, 760);
    for (int r = 0; r < rows; ++r) {
        const float y = 760 - 16.0f * r - 12;
        AddText(page, r == 0 ? std::string("Item") : "Item " + std::to_string(r), xs[0] + 6, y, 10);
        AddText(page, r == 0 ? std::string("Amount") : std::to_string(100 + r), xs[1] + 6, y, 10);
    }
    return pages;
}

void TestChunks() {
    const std::string sections = Run(SectionsPage, ChunkSettings(kDefaultChunkChars));
    Show("chunks", sections);
    const std::vector<std::string> paths = Arrays(sections, "heading_path");
    const std::vector<std::string> texts = Strings(sections, "markdown");
    RAYOPDF_CHECK(Count(sections, "\n") == 3 && paths.size() == 3 && texts.size() == 3 &&
                      paths[0] == "\"Annual Review\"" && paths[1] == "\"Annual Review\",\"Results\"" &&
                      paths[2] == "\"Annual Review\",\"Outlook\"" && texts[1].compare(0, 11, "## Results\n") == 0 &&
                      texts[2].compare(0, 11, "## Outlook\n") == 0,
                  2010);
    RAYOPDF_CHECK(texts.size() == 3 && texts[1] == "## Results\n\nSales rose by a tenth in the first half.\n\n"
                                                   "Costs fell slightly over the same period.",
                  2011);

    const std::string paragraph = Run(LongParagraphPage, ChunkSettings(300));
    Show("chunks of a paragraph", paragraph);
    const std::vector<std::string> pieces = Strings(paragraph, "markdown");
    bool split = pieces.size() >= 4;
    std::string joined;
    for (size_t p = 0; p < pieces.size(); ++p) {
        split = split && pieces[p].size() <= 300 && pieces[p].back() == '.';
        joined += (p > 0 ? " " : "") + pieces[p];
    }
    RAYOPDF_CHECK(split && joined == LongParagraph() && Arrays(paragraph, "blocks") ==
                                                            std::vector<std::string>(pieces.size(), "0"),
                  2012);

    const std::string table = Run(LongTablePage, ChunkSettings(300));
    Show("chunks of a table", table);
    const std::vector<std::string> parts = Strings(table, "markdown");
    bool repeated = parts.size() >= 3;
    int rows = 0;
    for (const std::string& part : parts) {
        repeated = repeated && part.compare(0, 34, "| Item | Amount |\n| --- | --- |\n| ") == 0;
        rows += Count(part, "\n| Item ");
    }
    RAYOPDF_CHECK(repeated && rows == 40 && Count(table, "| 140 |") == 1, 2013);

    // Unsplit blocks: the chunks' Markdown joined is the Markdown output, block by block in order.
    bool whole = true;
    for (auto build : {SectionsPage, LongTablePage}) {
        const std::string markdown = Run(build, OutputSettings());
        const std::string chunks = Run(build, ChunkSettings(kDefaultChunkChars));
        std::string text, order, expected;
        for (const std::string& part : Strings(chunks, "markdown")) text += (text.empty() ? "" : "\n\n") + part;
        for (const std::string& list : Arrays(chunks, "blocks")) order += (order.empty() ? "" : ",") + list;
        for (int b = 0, n = Count(markdown, "\n\n") + 1; b < n; ++b) expected += (b > 0 ? "," : "") + std::to_string(b);
        whole = whole && text + "\n" == markdown && order == expected;
    }
    RAYOPDF_CHECK(whole, 2014);
}

// A title and a paragraph.
std::vector<PageData> HeadingPage() {
    std::vector<PageData> pages{NewPage(1)};
    AddText(pages[0], "Annual Review", 72, 740, 24);
    AddText(pages[0], "The year brought steady growth across every region.", 72, 700, 10);
    return pages;
}

void TestInspect() {
    OutputSettings settings;
    settings.format = OutputFormat::Inspect;
    const std::string heading = Run(HeadingPage, settings);
    Show("inspect", heading);
    RAYOPDF_CHECK(Count(heading, "<rect class=\"t-heading\"") == 1 && Count(heading, "<rect class=\"t-paragraph\"") == 1 &&
                      Contains(heading, "<title>#1 heading 1&#10;# Annual Review</title>") &&
                      Contains(heading, "<title>#2 paragraph&#10;The year brought") &&
                      Contains(heading, "\">1</text>") && Contains(heading, "\">2</text>") &&
                      Count(heading, "<polyline class=\"order\"") == 1 && Contains(heading, "<section id=\"page-1\">") &&
                      Contains(heading, ">Annual Review</text>"),
                  2030);
    // The running header: drawn on each of the three pages, struck through as removed.
    const std::string running = Run(RunningPages, settings);
    int removed = 0;
    for (size_t at = running.find(">ACME Quarterly Report 2026</text>"); at != std::string::npos;
         at = running.find(">ACME Quarterly Report 2026</text>", at + 1)) {
        const size_t open = running.rfind("<text", at);
        removed += running.compare(open, 21, "<text class=\"removed\"") == 0 ? 1 : 0;
    }
    RAYOPDF_CHECK(removed == 3 && Contains(running, ">Body text of page 2 stays.</text>"), 2031);
    const std::string table = Run(TablePage, settings);
    Show("inspect table", table);
    RAYOPDF_CHECK(Count(table, "<rect class=\"cell\"") == 6 && Count(table, "<rect class=\"t-table\"") == 1, 2032);
    RAYOPDF_CHECK(!Contains(heading, "<script") && !Contains(running, "<script") && !Contains(table, "<script") &&
                      heading.compare(0, 15, "<!DOCTYPE html>") == 0 && Contains(heading, "<style>"),
                  2033);
}

} // namespace

int main() {
    TestPlain();
    TestHtmlTables();
    TestPipeTables();
    TestFurniture();
    TestKeptFurnitureText();
    TestKeptFurnitureOrder();
    TestKeptFootUnderNotes();
    TestAsciiQuotes();
    TestExactFormat();
    TestChunks();
    TestInspect();
    std::puts("layout output tests passed");
    return 0;
}
