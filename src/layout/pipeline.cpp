// SPDX-License-Identifier: Apache-2.0
#include "layout/pipeline.h"

#include "layout/figures.h"
#include "layout/inspect_writer.h"
#include "layout/lines.h"
#include "layout/markdown_writer.h"
#include "layout/page_pool.h"
#include "layout/reading_order.h"
#include "layout/semantics.h"
#include "layout/structure.h"
#include "layout/tables.h"

#include <algorithm>

namespace RayoPdf::Layout {

namespace {

// The lines a paragraph keeps when DropEdgeModelBlocks takes the edge-row model's rows off its
// start or end: a short block (the lines of a title block) stays whole.
constexpr uint32_t kTrimmedLines = 2;

// Leaves out the paragraphs and headings made only of rows the edge-row model took for furniture
// (kLineEdgeModel), and such rows at the start or the end of a paragraph that keeps at least
// kTrimmedLines other lines (a running head joined to the paragraph continued from the page
// before). Those rows took part in every earlier decision as text, as without the model, so no
// other block changes; a row between other lines of a block stays. Returns the lines left out.
uint32_t DropEdgeModelBlocks(const std::vector<PageData>& pages, BlockList& list) {
    uint32_t dropped = 0;
    const auto model = [&](uint32_t i) {
        const BlockLine& bl = list.lines[i];
        return (pages[bl.page].lines[bl.line].flags & kLineEdgeModel) != 0;
    };
    size_t kept = 0;
    for (size_t k = 0; k < list.blocks.size(); ++k) {
        Block b = list.blocks[k];
        if ((b.kind == BlockKind::Paragraph || b.kind == BlockKind::Heading) && b.count > 0) {
            uint32_t lead = 0, trail = 0;
            while (lead < b.count && model(b.first + lead)) ++lead;
            if (lead == b.count) {
                dropped += b.count;
                continue;
            }
            while (model(b.first + b.count - 1 - trail)) ++trail;
            if (b.kind == BlockKind::Paragraph && b.count - lead - trail >= kTrimmedLines) {
                b.first += lead;
                b.count -= lead + trail;
                dropped += lead + trail;
            }
        }
        list.blocks[kept++] = b;
    }
    list.blocks.resize(kept);
    return dropped;
}

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
        MarkFigureText(pages[p]);
        OrderLines(pages[p]);
        SplitRunInHeadings(pages[p]);
        if (math == MathMode::Latex) MarkDisplayFormulas(pages[p]);
    });
    return total;
}

// Quotes::Ascii: each glyph text holding a typographic quotation mark (U+2018-U+201B, written
// ', and U+201C-U+201F, written "; UTF-8 E2 80 98-9F) points to a copy with them replaced,
// appended to the page's text (a text may be shared by several glyphs, so none is changed in
// place). Image glyphs hold Markdown links and stay.
void AsciiQuotes(PageData& page, std::string& scratch) {
    const auto quote = [](std::string_view text, size_t i) {
        return i + 2 < text.size() && text[i] == '\xE2' && text[i + 1] == '\x80' &&
               static_cast<unsigned char>(text[i + 2]) >= 0x98 && static_cast<unsigned char>(text[i + 2]) <= 0x9F;
    };
    for (Glyph& g : page.glyphs) {
        if (g.flags & kGlyphImage) continue;
        const std::string_view text = page.GlyphText(g);
        size_t at = 0;
        while (at < text.size() && !quote(text, at)) ++at;
        if (at == text.size()) continue;
        scratch.assign(text.data(), at);
        for (size_t i = at; i < text.size(); ++i) {
            if (quote(text, i)) {
                scratch.push_back(static_cast<unsigned char>(text[i + 2]) <= 0x9B ? '\'' : '"');
                i += 2;
            } else {
                scratch.push_back(text[i]);
            }
        }
        if (page.text.size() + scratch.size() > UINT32_MAX) return;
        g.text = static_cast<uint32_t>(page.text.size());
        g.textLength = static_cast<uint16_t>(scratch.size());
        page.text += scratch;
    }
}

} // namespace

void ConvertPages(std::vector<PageData>& pages, const std::vector<PageTextQuality>& quality,
                  const std::vector<OutlineEntry>& outline, bool repaired, std::string& markdown, ConvertReport& report,
                  const ImageOutput* images, const Structure* structure, MathMode math, PagePool* pool,
                  const DocumentRead* document, const OutputSettings* output) {
    // Page-local stages first: they depend on nothing outside their page (D4: pages in parallel).
    // The caller's reads of the document (DocumentRead) are the round's first job: the calling
    // thread takes it while the pool's threads build lines.
    const size_t reads = document ? 1 : 0;
    if (pool) {
        pool->Run(reads + pages.size(), [&](unsigned, size_t job) {
            if (job < reads) {
                repaired = document->read(document->context);
                return;
            }
            PageData& page = pages[job - reads];
            if (math == MathMode::Latex) PrepareMathGlyphs(page);
            BuildLines(page, math == MathMode::Latex);
        });
    } else {
        if (document) repaired = document->read(document->context);
        for (PageData& page : pages) {
            if (math == MathMode::Latex) PrepareMathGlyphs(page);
            BuildLines(page, math == MathMode::Latex);
        }
    }
    // A conversion in a process of its own: the line builder's storage this thread kept for later
    // pages goes back to the heap, where the stages after it reuse the memory.
    if (output && output->oneShot) ReleaseLineScratch();
    // A structure tree that passes the checks decides blocks and order (D5, layout/structure.h).
    TaggedLayout tagged;
    if (structure && structure->use != StructureUse::None) {
        report.structure = tagged.Prepare(pages, *structure);
        report.structureElements = static_cast<uint32_t>(structure->nodes.size());
    }
    const bool tree = report.structure == StructureUse::Used;
    // Kept furniture: no running line, edge row or margin line is removed (a used tree still leaves
    // out its artifacts). The lines the rules would remove stay as text marked kLineFurniture:
    // they head nothing and make no block with the text. Rows only the edge-row model takes for
    // furniture (kLineEdgeModel) stay plain text. Each keeps its kind (FurnitureKind) in
    // Line::reserved for the JSON output's "role".
    const bool keepFurniture = output && output->furniture == Furniture::Keep;
    const FeatureSink* features = output ? output->features : nullptr;
    if (!tree && !keepFurniture) report.headerFooterLines += RemoveRunningLines(pages, features);
    if (!tree && keepFurniture && RemoveRunningLines(pages) > 0) {
        std::string text, key;
        for (PageData& page : pages)
            for (Line& line : page.lines)
                if (line.flags & kLineRemoved) {
                    line.flags = static_cast<uint16_t>((line.flags & ~kLineRemoved) | kLineFurniture);
                    line.reserved = static_cast<uint8_t>(FurnitureKind(page, line, text, key));
                }
    }
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
            MarkFigureText(page);
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
    // A conversion in a process of its own: the working storage this thread kept for later pages'
    // tables, figures and order goes back to the heap, where the blocks and the writer reuse it
    // instead of touching new pages.
    if (output && output->oneShot) {
        ReleaseTableScratch();
        ReleaseFigureScratch();
        ReleaseOrderScratch();
    }
    BlockList blocks;
    BuildBlocks(pages, outline, unmapped, output && output->figureText == FigureText::Drop, blocks);
    if (tree) tagged.Blocks(pages, blocks);
    if (!tree && !keepFurniture) report.headerFooterLines += DropEdgeModelBlocks(pages, blocks);
    if (output && output->quotes == Quotes::Ascii) {
        std::string scratch;
        for (PageData& page : pages) AsciiQuotes(page, scratch);
    }
    WriteCounts counts;
    WriteSettings settings;
    if (output) {
        settings.pageMarkers = output->pageMarkers;
        settings.plain = output->plain;
        settings.tables = output->tables;
        settings.smallCaps = output->smallCaps;
        settings.chunkChars = output->chunkChars;
        settings.jsonDetail = output->jsonDetail;
    }
    if (output && output->format == OutputFormat::Json) {
        WriteJson(pages, blocks, repaired, output->metadata, &outline, markdown, counts, math, settings);
    } else if (output && output->format == OutputFormat::Chunks) {
        WriteChunks(pages, blocks, markdown, counts, math, settings);
    } else if (output && output->format == OutputFormat::Text) {
        WriteText(pages, blocks, markdown, counts, math, settings);
    } else if (output && output->format == OutputFormat::Inspect) {
        // The blocks' Markdown (page markers aside) for the inspector's tooltips.
        std::string text;
        std::vector<WrittenBlock> written;
        settings.pageMarkers = false;
        WriteMarkdown(pages, blocks, false, text, counts, math, settings, &written);
        WriteInspect(pages, blocks, text, written, markdown);
    } else {
        WriteMarkdown(pages, blocks, repaired, markdown, counts, math, settings);
    }
    report.headings += counts.headings;
    report.paragraphs += counts.paragraphs;
    report.listItems += counts.listItems;
    report.codeBlocks += counts.codeBlocks;
    report.links += counts.links;
    report.tables += counts.tables;
    report.footnotes += counts.footnotes;
}

} // namespace RayoPdf::Layout
