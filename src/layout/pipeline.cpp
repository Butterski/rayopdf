// SPDX-License-Identifier: Apache-2.0
#include "layout/pipeline.h"

#include "layout/lines.h"
#include "layout/markdown_writer.h"
#include "layout/reading_order.h"
#include "layout/semantics.h"
#include "layout/structure.h"
#include "layout/tables.h"

namespace RayoPdf::Layout {

void ConvertPages(std::vector<PageData>& pages, const std::vector<PageTextQuality>& quality,
                  const std::vector<OutlineEntry>& outline, bool repaired, std::string& markdown, ConvertReport& report,
                  const ImageOutput* images, const Structure* structure) {
    // Page-local stages first: they depend on nothing outside their page (D4: pages in parallel).
    for (PageData& page : pages) BuildLines(page);
    // A structure tree that passes the checks decides blocks and order (D5, layout/structure.h).
    TaggedLayout tagged;
    if (structure && structure->use != StructureUse::None) {
        report.structure = tagged.Prepare(pages, *structure);
        report.structureElements = static_cast<uint32_t>(structure->nodes.size());
    }
    const bool tree = report.structure == StructureUse::Used;
    if (!tree) report.headerFooterLines += RemoveRunningLines(pages);
    if (images) PlaceImages(pages, *images, report);
    std::vector<uint8_t> unmapped(pages.size(), 0);
    TableContinuation continuation;  // tables run on from page to page: detected in page order
    for (size_t p = 0; p < pages.size(); ++p) {
        PageData& page = pages[p];
        if (tree) {
            tagged.LayOut(pages, static_cast<uint32_t>(p));
        } else {
            report.headerFooterLines -= DetectTables(page, continuation);
            OrderLines(page);
            SplitRunInHeadings(page);
        }
        report.hiddenGlyphs += page.hiddenGlyphs;
        report.contentErrors += page.contentErrors;
        if (!page.HasText()) report.pagesWithoutText++;
        if (p < quality.size() && quality[p].glyphs >= 20 && quality[p].unreliable * 2 > quality[p].glyphs)
            unmapped[p] = 1;
    }
    BlockList blocks;
    BuildBlocks(pages, outline, unmapped, blocks);
    if (tree) tagged.Blocks(pages, blocks);
    WriteCounts counts;
    WriteMarkdown(pages, blocks, repaired, markdown, counts);
    report.headings += counts.headings;
    report.paragraphs += counts.paragraphs;
    report.listItems += counts.listItems;
    report.codeBlocks += counts.codeBlocks;
    report.links += counts.links;
    report.tables += counts.tables;
    report.footnotes += counts.footnotes;
}

} // namespace RayoPdf::Layout
