// SPDX-License-Identifier: Apache-2.0
// The stages after interpretation (roadmap D4 steps 4-8), from interpreted pages to Markdown. Kept
// apart from Convert so that tests can feed pages built by hand, with no document behind them.
#pragma once

#include "layout/convert.h"
#include "layout/images.h"
#include "layout/page.h"

#include <string>
#include <vector>

namespace RayoPdf::Layout {

struct DocumentMetadata;
class PagePool;
struct Structure;

// Per page: whether most of its text came from unmapped or guessed codes, and whether OCR failed on
// it (layout/ocr.h).
struct PageTextQuality {
    uint64_t glyphs = 0;
    uint64_t unreliable = 0;  // unmapped + guessed
    bool ocrFailed = false;
};

// The reads of the document that Convert leaves to ConvertPages: `read(context)` is called once,
// before anything looks at `outline` or `structure`, and fills the objects the caller passed as
// these (the document's outline and structure tree); what it returns replaces `repaired`. It runs
// while the pages' lines are built: on the calling thread, or with a pool on whichever thread
// takes it while the others build lines, which read no document. So the document is still read by
// one thread at a time and in the order of a conversion on one thread (pages, OCR, outline,
// structure), and nothing can depend on the thread count.
struct DocumentRead {
    bool (*read)(void* context) = nullptr;
    void* context = nullptr;
};

// What the writer writes when it is not plain Markdown (ConvertOptions::format, pageMarkers).
struct OutputSettings {
    OutputFormat format = OutputFormat::Markdown;
    bool pageMarkers = false;                    // Markdown only (WriteMarkdown)
    const DocumentMetadata* metadata = nullptr;  // JSON's "metadata"; read by then when a DocumentRead fills it
};

// Lines, header/footer removal, images, reading order, semantics and the writer over `pages`
// (each with its glyphs, shapes and links filled). Appends to `markdown`; adds to the counters of
// `report` that these stages own (pagesWithoutText, hiddenGlyphs, headings, paragraphs, listItems,
// codeBlocks, tables, links, footnotes, images, imagesSkipped, imagesMaskDropped,
// headerFooterLines, contentErrors
// from the pages, structure). `images` null (tests) places no images; `structure` (ReadStructure),
// when it passes the checks, decides blocks and order instead of the geometry. `math` Latex marks
// display formulas (layout/math.h) and has the writer write formulas as LaTeX. The page-local
// stages (math glyphs, lines, tables, reading order) run on the threads of `pool` when there is
// one (layout/page_pool.h), with the same result. `output` null writes plain Markdown; else JSON
// (WriteJson) or Markdown with page markers as it says.
void ConvertPages(std::vector<PageData>& pages, const std::vector<PageTextQuality>& quality,
                  const std::vector<OutlineEntry>& outline, bool repaired, std::string& markdown, ConvertReport& report,
                  const ImageOutput* images = nullptr, const Structure* structure = nullptr,
                  MathMode math = MathMode::Unicode, PagePool* pool = nullptr, const DocumentRead* document = nullptr,
                  const OutputSettings* output = nullptr);

} // namespace RayoPdf::Layout
