// SPDX-License-Identifier: Apache-2.0
// The Markdown writer (roadmap D6, D4 step 7): GFM blocks separated by one blank line, paragraphs
// as single lines with words joined across line breaks (dehyphenated, algorithms §4.4), emphasis
// from bold and italic runs over whole words (§6.6), inline code from monospace runs, links from
// link annotations over words (§6.8), superscripts and subscripts as <sup>/<sub> or, a formula's,
// in Unicode superscript and subscript characters (D6), footnote references as [^n] and
// definitions as [^n]: text (§6.9), fenced code with its spacing rebuilt on the character grid
// (§6.4). Escapes are limited to what would otherwise change the structure.
// With MathMode::Latex formulas are written as $…$ and $$…$$ LaTeX (layout/math.h).
#pragma once

#include "layout/math.h"
#include "layout/page.h"
#include "layout/semantics.h"

#include <string>
#include <vector>

namespace RayoPdf::Layout {

struct WriteCounts {
    int headings = 0;
    int paragraphs = 0;
    int listItems = 0;
    int codeBlocks = 0;
    int links = 0;
    int footnotes = 0;
    int tables = 0;
};

// Appends the Markdown of `blocks` to `out`; `repaired` puts the repair comment first.
void WriteMarkdown(const std::vector<PageData>& pages, const BlockList& blocks, bool repaired, std::string& out,
                   WriteCounts& counts, MathMode math = MathMode::Unicode);

} // namespace RayoPdf::Layout
