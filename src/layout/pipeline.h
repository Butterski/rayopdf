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

class PagePool;
struct Structure;

// Per page: whether most of its text came from unmapped or guessed codes, and whether OCR failed on
// it (layout/ocr.h).
struct PageTextQuality {
    uint64_t glyphs = 0;
    uint64_t unreliable = 0;  // unmapped + guessed
    bool ocrFailed = false;
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
// one (layout/page_pool.h), with the same result.
void ConvertPages(std::vector<PageData>& pages, const std::vector<PageTextQuality>& quality,
                  const std::vector<OutlineEntry>& outline, bool repaired, std::string& markdown, ConvertReport& report,
                  const ImageOutput* images = nullptr, const Structure* structure = nullptr,
                  MathMode math = MathMode::Unicode, PagePool* pool = nullptr);

} // namespace RayoPdf::Layout
