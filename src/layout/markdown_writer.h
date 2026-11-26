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
//
// WriteChunks writes the same blocks as chunks for retrieval (roadmap D6), one JSON object a line:
// consecutive blocks under one heading path, each chunk at most WriteSettings::chunkChars bytes of
// Markdown where a block allows it.
#pragma once

#include "layout/convert.h"
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

// How WriteMarkdown writes. `pageMarkers`: before a block whose page (its first line's; a
// comment's `number`) comes after the last page marked, `<!-- page N -->` (N: PageData::number) as
// a block of its own for each page up to it, and after the last block one for each page left; a
// marker ends a list or block quote that runs across the page break. `plain`: no inline markup
// (emphasis, code spans, strikethrough, links: their text stays; <sup>/<sub>: their text stays,
// Unicode script forms too), footnote references as their bare number and definitions as
// "N text". `tables`: Html writes every table as HTML, not only those whose spans need it, with a
// space between a row's tags and around each cell's text (WriteHtmlTable `spaced`); Pipe every
// table as a GFM pipe table, a spanning cell's text in its first row and column only (the covered
// cells empty); PipeLatex as Pipe, its cells' `%`, `#` and `&` outside math as the text `\%`,
// `\#`, `\&` (WriteGfmTable `latexCells`). `smallCaps`: Keep writes a heading set in small
// capitals (Block::smallCaps) as printed, every letter a capital, not in the source's case.
// `chunkChars`: WriteChunks only, clamped to [kMinChunkChars, kMaxChunkChars]. `jsonDetail`:
// WriteJson only, each block's lines (Lines) or lines and words (Words).
struct WriteSettings {
    bool pageMarkers = false;
    bool plain = false;
    TableFormat tables = TableFormat::Auto;
    SmallCaps smallCaps = SmallCaps::Lower;
    uint32_t chunkChars = kDefaultChunkChars;
    JsonDetail jsonDetail = JsonDetail::Blocks;
};

// A block's box on one page (`page` an index into the pages), on the upright page, in points with
// the origin at the lower left.
struct PageBox {
    uint32_t page;
    float x0, y0, x1, y1;
};

// The boxes of `block` into `boxes` (replaced), one a page it covers, in the order of its lines:
// an image's, a table's parts' (their tables), any other block's lines'. A comment has none.
void BlockBoxes(const std::vector<PageData>& pages, const BlockList& list, const Block& block,
                std::vector<PageBox>& boxes);

// Appends `boxes` as JSON: [{"page":N,"bbox":[x0,y0,x1,y1]}...] (N: PageData::number; `%.2f`).
void AppendBoxes(std::string& out, const std::vector<PageData>& pages, const PageBox* boxes, size_t count);

// The JSON output's `type` of a block: "heading", "paragraph", "list_item", "code", "table",
// "image", "footnote" or "comment".
const char* BlockTypeName(BlockKind kind);

// Appends the Markdown of `blocks` to `out`; `repaired` puts the repair comment first. `written`,
// when given, receives every block written, in order.
void WriteMarkdown(const std::vector<PageData>& pages, const BlockList& blocks, bool repaired, std::string& out,
                   WriteCounts& counts, MathMode math = MathMode::Unicode, const WriteSettings& settings = {},
                   std::vector<WrittenBlock>* written = nullptr);

// Appends the blocks as one compact JSON line (schema 1) to `out`:
// {"rayopdf":VERSION,"schema":1,"repaired":bool,"metadata":{non-empty fields},"pages":[{"page",
// "width","height"}...],"blocks":[{"type","page","boxes":[{"page","bbox":[x0,y0,x1,y1]}...],
// ...,"markdown"}...]}. Boxes are in page points, origin at the lower left; `metadata` null
// writes an empty object. A non-empty `outline` is written after "pages" as
// "outline":[{"level","title","page"}...] (page null when its destination led to none).
// `counts` as WriteMarkdown counts; each block's Markdown as WriteMarkdown
// writes it with `settings` (page markers aside). With `settings.jsonDetail` Lines or Words every
// block also has "lines":[{"page","box":[x0,y0,x1,y1],"text"}...] before "markdown": its lines in
// order (a table's: its cells' lines, cell by cell; an image or a comment: none), each line's box
// on the upright page and its text as the page shows it (words joined by one space, a list item's
// marker and inline images left out); with Words each line also has "words":[{"box","text"}...],
// split at the line's word breaks.
void WriteJson(const std::vector<PageData>& pages, const BlockList& blocks, bool repaired,
               const DocumentMetadata* metadata, const std::vector<OutlineEntry>* outline, std::string& out,
               WriteCounts& counts, MathMode math = MathMode::Unicode, const WriteSettings& settings = {});

// Appends the blocks as chunks for retrieval, one JSON object and a line break each (JSON Lines):
// {"chunk":N,"heading_path":[...],"pages":[first,last],"boxes":[{"page","bbox"}...],"blocks":[...],
// "markdown":"..."}. N counts from 0; `blocks` are indices into WriteJson's "blocks"; `boxes` are
// those blocks' boxes (BlockBoxes) in order; `pages` the first and last page numbers they cover;
// `heading_path` the text of the headings that enclose the chunk, outermost first, without markup.
// A chunk's Markdown is its blocks' Markdown (WriteMarkdown's, page markers aside) joined by a
// blank line. A heading of level L ends the path's entries of level L or deeper and is pushed on
// it; it starts a chunk unless the chunk open holds only headings. A block that would take the
// chunk past `settings.chunkChars` bytes starts a new one. A block longer than that alone becomes
// chunks of its own: text at sentence ends (". ", "? ", "! " before a capital or digit), else at
// the last space, else at a character; a table by rows with its header rows repeated in each; code
// at line ends with its fence reopened. A row or code line longer than the limit stays whole.
void WriteChunks(const std::vector<PageData>& pages, const BlockList& blocks, std::string& out, WriteCounts& counts,
                 MathMode math = MathMode::Unicode, const WriteSettings& settings = {});

} // namespace RayoPdf::Layout
