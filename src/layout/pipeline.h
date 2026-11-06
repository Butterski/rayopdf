// SPDX-License-Identifier: Apache-2.0
// The stages after interpretation (roadmap D4 steps 4-8), from interpreted pages to Markdown. Kept
// apart from Convert so that tests can feed pages built by hand, with no document behind them.
#pragma once

#include "layout/convert.h"
#include "layout/page.h"

#include <string>
#include <vector>

namespace RayoPdf::Layout {

// Per page: whether most of its text came from unmapped or guessed codes.
struct PageTextQuality {
    uint64_t glyphs = 0;
    uint64_t unreliable = 0;  // unmapped + guessed
};

// Lines, header/footer removal, reading order, semantics and the writer over `pages` (each with
// its glyphs, shapes and links filled). Appends to `markdown`; adds to the counters of `report`
// that these stages own (pagesWithoutText, hiddenGlyphs, headings, paragraphs, listItems,
// codeBlocks, links, headerFooterLines, contentErrors from the pages).
void ConvertPages(std::vector<PageData>& pages, const std::vector<PageTextQuality>& quality,
                  const std::vector<OutlineEntry>& outline, bool repaired, std::string& markdown, ConvertReport& report);

} // namespace RayoPdf::Layout
