// SPDX-License-Identifier: Apache-2.0
// Core tests of the layout stages that fit a document's own statistics in place of fixed
// thresholds (docs/research/layout-math.md): heading levels from size clusters (codes 2150-2199).
#include "check.h"

#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

PageData NewPage(int number) {
    PageData page;
    page.number = number;
    page.width = 612;
    page.height = 792;
    return page;
}

std::string Run(std::vector<PageData>& pages) {
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report);
    return md;
}

void Show(const char* name, const std::string& md) {
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", name, md.c_str());
}

// `text` on `page` from (x, y) in glyphs of `size` points, half an em wide, a space a space glyph.
void AddText(PageData& page, std::string_view text, float x, float y, float size, uint16_t flags = 0) {
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        g.u0 = x, g.u1 = x + 0.5f * size, g.vb = y - 0.2f * size, g.vt = y + 0.8f * size, g.v = y;
        g.size = size;
        g.text = static_cast<uint32_t>(page.text.size());
        page.text.append(text.substr(at, length));
        g.textLength = static_cast<uint16_t>(length);
        g.flags = flags;
        if (cp == ' ') g.flags |= kGlyphSpace;
        page.glyphs.push_back(g);
        x = g.u1;
        at += length;
    }
}

// Bold headings of `sizes` points, each over four lines of 10 pt body text.
std::vector<PageData> HeadingPage(const std::vector<float>& sizes) {
    std::vector<PageData> pages{NewPage(1)};
    float y = 740;
    const char* const names[] = {"First Heading", "Second Heading", "Third Heading"};
    for (size_t h = 0; h < sizes.size(); ++h) {
        AddText(pages[0], names[h], 72, y, sizes[h], kGlyphBold);
        y -= 2 * sizes[h];
        for (int i = 0; i < 4; ++i, y -= 12)
            AddText(pages[0], "Body text of this section runs on long enough for a line.", 72, y, 10);
        y -= 14;
    }
    return pages;
}

// The number of '#' of the heading line that holds `name`; 0 when none.
int LevelOf(const std::string& md, std::string_view name) {
    const size_t at = md.find(name);
    if (at == std::string::npos) return 0;
    size_t start = md.rfind('\n', at);
    start = start == std::string::npos ? 0 : start + 1;
    int level = 0;
    while (start + level < at && md[start + level] == '#') ++level;
    return level;
}

// Headings 0.06 pt apart around a half-point boundary (13.74, 13.76 and 13.80 pt) are one level;
// 18, 14 and 12 pt are three.
void TestHeadingLevels() {
    std::vector<PageData> close = HeadingPage({13.74f, 13.76f, 13.8f});
    const std::string same = Run(close);
    Show("close sizes", same);
    const int first = LevelOf(same, "First Heading");
    RAYOPDF_CHECK(first > 0 && LevelOf(same, "Second Heading") == first && LevelOf(same, "Third Heading") == first,
                  2152);
    std::vector<PageData> steps = HeadingPage({18, 14, 12});
    const std::string three = Run(steps);
    Show("three sizes", three);
    const int top = LevelOf(three, "First Heading");
    RAYOPDF_CHECK(top > 0 && LevelOf(three, "Second Heading") == top + 1 && LevelOf(three, "Third Heading") == top + 2,
                  2153);
}

} // namespace

int main() {
    TestHeadingLevels();
    std::puts("layout stats tests passed");
    return 0;
}
