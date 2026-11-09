// SPDX-License-Identifier: Apache-2.0
#include "layout/pipeline.h"

#include "layout/lines.h"
#include "layout/markdown_writer.h"
#include "layout/page_pool.h"
#include "layout/reading_order.h"
#include "layout/semantics.h"
#include "layout/structure.h"
#include "layout/tables.h"

namespace RayoPdf::Layout {

namespace {

// The tables, reading order, run-in headings and (`math` Latex) display formulas of every page on
// the threads of `pool`, as the loop of ConvertPages does them one page after another: each page's
// tables are first detected as if no table ran on from the page before (TableContinuation); then,
// in page order, a page whose previous page does end in a table is put back (UndoTables) and
// detected again with it, and only then are the pages ordered. Returns the header and footer
// lines that returned to the text (DetectTables).
int LayOutPages(std::vector<PageData>& pages, PagePool& pool, MathMode math) {
    const size_t count = pages.size();
    std::vector<TableUndo> undo(count);
    std::vector<TableContinuation> next(count);  // each page's continuation, for the page after it
    std::vector<int> restored(count, 0);
    pool.Run(count, [&](unsigned, size_t p) {
        if (p > 0) SaveTables(pages[p], undo[p]);
        restored[p] = DetectTables(pages[p], next[p]);
    });
    int total = restored[0];
    for (size_t p = 1; p < count; ++p) {
        if (next[p - 1].valid) {
            UndoTables(pages[p], undo[p]);
            next[p] = next[p - 1];
            restored[p] = DetectTables(pages[p], next[p]);
        }
        total += restored[p];
    }
    pool.Run(count, [&](unsigned, size_t p) {
        OrderLines(pages[p]);
        SplitRunInHeadings(pages[p]);
        if (math == MathMode::Latex) MarkDisplayFormulas(pages[p]);
    });
    return total;
}

} // namespace

void ConvertPages(std::vector<PageData>& pages, const std::vector<PageTextQuality>& quality,
                  const std::vector<OutlineEntry>& outline, bool repaired, std::string& markdown, ConvertReport& report,
                  const ImageOutput* images, const Structure* structure, MathMode math, PagePool* pool) {
    // Page-local stages first: they depend on nothing outside their page (D4: pages in parallel).
    if (pool) {
        pool->Run(pages.size(), [&](unsigned, size_t p) {
            if (math == MathMode::Latex) PrepareMathGlyphs(pages[p]);
            BuildLines(pages[p]);
        });
    } else {
        for (PageData& page : pages) {
            if (math == MathMode::Latex) PrepareMathGlyphs(page);
            BuildLines(page);
        }
    }
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
    const bool parallel = pool && !tree && !pages.empty();
    if (parallel) report.headerFooterLines -= LayOutPages(pages, *pool, math);
    TableContinuation continuation;  // tables run on from page to page: detected in page order
    for (size_t p = 0; p < pages.size(); ++p) {
        PageData& page = pages[p];
        if (tree) {
            tagged.LayOut(pages, static_cast<uint32_t>(p));
        } else if (!parallel) {
            report.headerFooterLines -= DetectTables(page, continuation);
            OrderLines(page);
            SplitRunInHeadings(page);
            if (math == MathMode::Latex) MarkDisplayFormulas(page);
        }
        report.hiddenGlyphs += page.hiddenGlyphs;
        report.contentErrors += page.contentErrors;
        if (!page.HasText()) report.pagesWithoutText++;
        if (p < quality.size() && quality[p].glyphs >= 20 && quality[p].unreliable * 2 > quality[p].glyphs)
            unmapped[p] = 1;
        if (p < quality.size() && quality[p].ocrFailed) unmapped[p] |= 2;
    }
    BlockList blocks;
    BuildBlocks(pages, outline, unmapped, blocks);
    if (tree) tagged.Blocks(pages, blocks);
    WriteCounts counts;
    WriteMarkdown(pages, blocks, repaired, markdown, counts, math);
    report.headings += counts.headings;
    report.paragraphs += counts.paragraphs;
    report.listItems += counts.listItems;
    report.codeBlocks += counts.codeBlocks;
    report.links += counts.links;
    report.tables += counts.tables;
    report.footnotes += counts.footnotes;
}

} // namespace RayoPdf::Layout
