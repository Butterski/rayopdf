// SPDX-License-Identifier: Apache-2.0
// The Markdown writer (roadmap D6, D4 step 7): GFM blocks separated by one blank line, paragraphs
// as single lines with words joined across line breaks (dehyphenated, algorithms §4.4), emphasis
// from bold and italic runs over whole words (§6.6), inline code from monospace runs, links from
// link annotations over words (§6.8), superscripts and subscripts as <sup>/<sub> or, a formula's,
// in Unicode superscript and subscript characters (D6), footnote references as [^n] and
// definitions as [^n]: text (§6.9), fenced code with its spacing rebuilt on the character grid
// (§6.4). Escapes are limited to what would otherwise change the structure.
// With MathMode::Latex formulas are written as $…$ and $$…$$ LaTeX (layout/math.h).
//
// WriteJson writes the same blocks as one JSON object (roadmap D6, schema 1): the document's
// metadata and pages, then per block its type, first page, boxes on each page it covers, what its
// type adds (heading level; list depth, number and task; table cells; image file and alternate
// text; footnote number; quote depth and alert) and, last, the Markdown WriteMarkdown writes for it.
#pragma once

#include "layout/math.h"
#include "layout/metadata.h"
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

// A table cell as the JSON output gives it: its place in the whole table (a table broken across
// pages counts its parts' rows on, without the header rows a later part repeats) and its inline
// Markdown.
struct WrittenCell {
    uint32_t row = 0;
    uint16_t col = 0, rowSpan = 1, colSpan = 1;
    std::string text;
};

// A block WriteMarkdown wrote: its index in BlockList::blocks and its Markdown, out[begin, end)
// (without the blank line or quote marks that separate it from the block before). Tables: their
// size, header rows (at least 1, as written) and cells, also for a table written as HTML.
struct WrittenBlock {
    uint32_t block = 0;
    size_t begin = 0, end = 0;
    uint32_t rows = 0, cols = 0, headerRows = 0;
    std::vector<WrittenCell> cells;
};

// Appends the Markdown of `blocks` to `out`; `repaired` puts the repair comment first.
// `pageMarkers`: before a block whose page (its first line's; a comment's `number`) comes after
// the last page marked, `<!-- page N -->` (N: PageData::number) as a block of its own for each
// page up to it, and after the last block one for each page left; a marker ends a list or block
// quote that runs across the page break. `written`, when given, receives every block written, in
// order.
void WriteMarkdown(const std::vector<PageData>& pages, const BlockList& blocks, bool repaired, std::string& out,
                   WriteCounts& counts, MathMode math = MathMode::Unicode, bool pageMarkers = false,
                   std::vector<WrittenBlock>* written = nullptr);

// Appends the blocks as one compact JSON line (schema 1) to `out`:
// {"rayopdf":VERSION,"schema":1,"repaired":bool,"metadata":{non-empty fields},"pages":[{"page",
// "width","height"}...],"blocks":[{"type","page","boxes":[{"page","bbox":[x0,y0,x1,y1]}...],
// ...,"markdown"}...]}. Boxes are in page points, origin at the lower left; `metadata` null
// writes an empty object. `counts` as WriteMarkdown counts.
void WriteJson(const std::vector<PageData>& pages, const BlockList& blocks, bool repaired,
               const DocumentMetadata* metadata, std::string& out, WriteCounts& counts,
               MathMode math = MathMode::Unicode);

} // namespace RayoPdf::Layout
