// SPDX-License-Identifier: Apache-2.0
// Core tests of the feature sink of layout/features.h (`rayopdf --features`; small models
// 2250-2299): the edge rows RemoveRunningLines examines and the gaps Detector::Borderless tries.
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

struct GapRecord {
    uint32_t page;
    std::vector<float> features;
    bool chosen;
};

struct Records {
    std::vector<EdgeRecord> edges;
    std::vector<GapRecord> gaps;
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
    sink.tableGap = [](void* context, uint32_t page, const float* f, float, float, bool chosen) {
        static_cast<Records*>(context)->gaps.push_back(GapRecord{page, std::vector<float>(f, f + kTableGapFeatures), chosen});
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

// A borderless table of three columns: its two gaps, chosen by the rules.
void TestTableGaps() {
    std::vector<PageData> pages{NewPage(1)};
    PageData& page = pages[0];
    AddText(page, "The stock at the end of the quarter was counted in every store.", 72, 740, 10);
    const char* const rows[6][3] = {{"Item", "Count", "Price"},  {"Apples", "12", "3.50"}, {"Pears", "7", "4.25"},
                                    {"Plums", "31", "2.10"},     {"Figs", "5", "8.00"},    {"Limes", "64", "0.75"}};
    for (int r = 0; r < 6; ++r) {
        AddText(page, rows[r][0], 72, 700 - 14.0f * r, 10);
        AddText(page, rows[r][1], 250, 700 - 14.0f * r, 10);
        AddText(page, rows[r][2], 400, 700 - 14.0f * r, 10);
    }
    AddText(page, "Prices are per kilogram and include the tax of the region.", 72, 600, 10);
    const Records records = Collect(pages);
    int chosen = 0;
    for (const GapRecord& g : records.gaps) chosen += g.chosen && g.features[11] == 1;
    RAYOPDF_CHECK(!records.gaps.empty() && chosen == 2, 2251);
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
    TestTableGaps();
    TestTextWords();
    return 0;
}
