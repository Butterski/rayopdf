// SPDX-License-Identifier: Apache-2.0
#include "layout/pipeline.h"

#include "layout/lines.h"
#include "layout/markdown_writer.h"
#include "layout/reading_order.h"
#include "layout/semantics.h"

namespace RayoPdf::Layout {

void ConvertPages(std::vector<PageData>& pages, const std::vector<PageTextQuality>& quality,
                  const std::vector<OutlineEntry>& outline, bool repaired, std::string& markdown, ConvertReport& report) {
    // Page-local stages first: they depend on nothing outside their page (D4: pages in parallel).
    for (PageData& page : pages) BuildLines(page);
    report.headerFooterLines += RemoveRunningLines(pages);
    std::vector<uint8_t> unmapped(pages.size(), 0);
    for (size_t p = 0; p < pages.size(); ++p) {
        PageData& page = pages[p];
        OrderLines(page);
        report.hiddenGlyphs += page.hiddenGlyphs;
        report.contentErrors += page.contentErrors;
        if (page.order.empty()) report.pagesWithoutText++;
        if (p < quality.size() && quality[p].glyphs >= 20 && quality[p].unreliable * 2 > quality[p].glyphs)
            unmapped[p] = 1;
    }
    std::vector<Block> blocks;
    std::vector<BlockLine> lines;
    BuildBlocks(pages, outline, unmapped, blocks, lines);
    WriteCounts counts;
    WriteMarkdown(pages, blocks, lines, repaired, markdown, counts);
    report.headings += counts.headings;
    report.paragraphs += counts.paragraphs;
    report.listItems += counts.listItems;
    report.codeBlocks += counts.codeBlocks;
    report.links += counts.links;
}

} // namespace RayoPdf::Layout
