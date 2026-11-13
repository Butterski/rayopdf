// SPDX-License-Identifier: Apache-2.0
// Core tests of the output options (layout/convert.h, layout/markdown_writer.h WriteSettings):
// plain text without inline markup, every table as HTML, page furniture kept, and the exact
// recovery they turn off. Exit codes 2000-2049.
#include "check.h"
#include "exact_builder.h"

#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/page.h"
#include "layout/pipeline.h"
#include "pdf/document.h"

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
    RAYOPDF_CHECK(Contains(html, "<table>") && !Contains(html, "| --- |") && Contains(html, "<th>Name</th>") &&
                      Contains(html, "<td>34</td>"),
                  2002);
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
    for (int variant = 0; variant < 3; ++variant) {
        ConvertOptions changed = options;
        if (variant == 0) changed.plain = true;
        if (variant == 1) changed.tables = TableFormat::Html;
        if (variant == 2) changed.furniture = Furniture::Keep;
        Convert(*document, changed, markdown, report);
        format = format && report.exact == ExactUse::Format && Contains(markdown, "Revenue grew");
    }
    RAYOPDF_CHECK(format, 2004);
}

} // namespace

int main() {
    TestPlain();
    TestHtmlTables();
    TestFurniture();
    TestExactFormat();
    std::puts("layout output tests passed");
    return 0;
}
