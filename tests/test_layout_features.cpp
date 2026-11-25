// SPDX-License-Identifier: Apache-2.0
// Core tests of the feature sink of layout/features.h (`rayopdf --features`; small models
// 2250-2299): the edge rows RemoveRunningLines examines.
#include "check.h"

#include "common/utf8.h"
#include "layout/features.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

// Appends `text` to `page` as glyphs of `size` points from (x, y), each half an em wide; a space
// is a space glyph.
void AddText(PageData& page, std::string_view text, float x, float y, float size) {
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
        if (cp == ' ') g.flags |= kGlyphSpace;
        page.glyphs.push_back(g);
        x += w;
        at += length;
    }
}

PageData NewPage(int number) {
    PageData page;
    page.number = number;
    page.width = 612;
    page.height = 792;
    return page;
}

struct EdgeRecord {
    uint32_t page;
    std::vector<float> features;
    std::string text;
    bool removed;
};

struct Records {
    std::vector<EdgeRecord> edges;
};

// The records of converting `pages` with a sink.
Records Collect(std::vector<PageData> pages) {
    Records records;
    FeatureSink sink;
    sink.context = &records;
    sink.edgeRow = [](void* context, uint32_t page, const float* f, float, float, float, float, std::string_view text,
                      bool removed) {
        static_cast<Records*>(context)->edges.push_back(
            EdgeRecord{page, std::vector<float>(f, f + kEdgeRowFeatures), std::string(text), removed});
    };
    OutputSettings output;
    output.features = &sink;
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report, nullptr, nullptr, MathMode::Unicode, nullptr, nullptr, &output);
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "%s", md.c_str());
    return records;
}

// A page of body text over a page footer: one edge row in the outer tenth, removed as a page
// number.
void TestEdgeRow() {
    std::vector<PageData> pages{NewPage(3)};
    for (int i = 0; i < 6; ++i)
        AddText(pages[0], "Body text line number " + std::to_string(i + 1) + " of the page.", 72, 700 - 14.0f * i, 10);
    AddText(pages[0], "Page 3 of 9", 280, 30, 8);
    const Records records = Collect(pages);
    RAYOPDF_CHECK(records.edges.size() == 1 && records.edges[0].page == 3 && records.edges[0].removed &&
                      records.edges[0].text == "Page 3 of 9" && records.edges[0].features[8] == 1 &&
                      records.edges[0].features[1] == 1 && records.edges[0].features[0] < 0.1f,
                  2250);
}

// The word features of a row (16-18): a sentence ends in '.', '?' or '!' after a word of two
// letters or digits with a letter; a page number, an initial or a number with a point does not.
void TestTextWords() {
    const auto ends = [](std::string_view text) {
        float words = 0, lower = 0;
        bool sentence = false;
        TextWords(text, words, lower, sentence);
        return sentence;
    };
    float words = 0, lower = 0;
    bool sentence = false;
    TextWords("Sentinel start C01.", words, lower, sentence);
    const bool first = words == 3 && std::fabs(lower - 1.0f / 3) < 1e-6f && sentence;
    TextWords("  ANNUAL  REPORT 2019 ", words, lower, sentence);
    const bool second = words == 3 && lower == 0 && !sentence;
    RAYOPDF_CHECK(first && second && ends("Is it so?") && ends("Done!") && !ends("Page 3.") && !ends("J.") &&
                      !ends("Chapter 4") && !ends(""),
                  2256);
}

} // namespace

int main() {
    TestEdgeRow();
    TestTextWords();
    return 0;
}
