// SPDX-License-Identifier: Apache-2.0
// Document semantics (algorithms §6, roadmap D4 step 6, D5 precedence outline > style > numbering):
// the ordered lines of every page grouped into blocks (paragraph breaks from line pitch,
// indentation, short last lines and style changes, §6.2) and classified: headings from the outline
// and from font-size clusters above the body size (§6.1), list items from glyph bullets, vector
// bullets, enumerators and task boxes with nesting by marker position (§6.3), code from monospace
// lines (§6.4), quotes from a left rule (§6.5). Paragraphs continue across columns and pages when
// the first ends without terminal punctuation and the next starts in lowercase.
#pragma once

#include "layout/page.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Layout {

enum class BlockKind : uint8_t {
    Paragraph,
    Heading,
    ListItem,
    Code,
    Comment,  // an uncertainty comment; `number` is the page and `level` the CommentKind
};

enum class CommentKind : uint8_t {
    NoText,    // the page has no extractable text
    Unmapped,  // most of the page's text has no reliable Unicode mapping
};

// One line of a block: `skip` glyphs at its start (a list marker) are not text.
struct BlockLine {
    uint32_t page = 0;   // index into the pages vector
    uint32_t line = 0;
    uint32_t skip = 0;
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    uint8_t level = 0;      // heading level 1-6
    uint8_t depth = 0;      // list nesting depth (0 = top level)
    uint8_t quote = 0;      // block-quote depth
    uint8_t task = 0;       // list items: 0 none, 1 open, 2 done
    bool ordered = false;   // list items
    bool listParagraph = false; // a further paragraph of the list item before it, at its `depth`
    uint32_t number = 0;    // ordered list items: the enumerator's value; comments: the page number
    uint32_t first = 0;     // lines: BlockLine[first, first + count)
    uint32_t count = 0;
    float size = 0;         // the size of most of its characters
};

// Pages carry their lines in reading order (PageData::order). Pages whose text is mostly unmapped
// are listed in `unmappedPages` (indices into `pages`).
void BuildBlocks(const std::vector<PageData>& pages, const std::vector<OutlineEntry>& outline,
                 const std::vector<uint8_t>& unmappedPages, std::vector<Block>& blocks, std::vector<BlockLine>& lines);

} // namespace RayoPdf::Layout
