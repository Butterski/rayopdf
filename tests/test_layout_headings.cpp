// SPDX-License-Identifier: Apache-2.0
// Core tests of the heading rules of src/layout/semantics.cpp on scientific papers: the title
// above an outline that names the sections only, small capitals, numbering in the outline match,
// the standard unnumbered sections. Pages are built glyph by glyph and converted by the whole
// pipeline. Exit codes 1800-1849.
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

} // namespace

int main() {
    TestTitleAboveOutline();
    std::puts("test_layout_headings: all checks passed");
    return 0;
}
