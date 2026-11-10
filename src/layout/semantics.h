// SPDX-License-Identifier: Apache-2.0
// Document semantics (algorithms §6, roadmap D4 step 6, D5 precedence outline > style > numbering):
// the ordered lines of every page grouped into blocks (paragraph breaks from line pitch,
// indentation, short last lines and style changes, §6.2) and classified: headings from the outline
// and from font-size clusters above the body size (§6.1), list items from glyph bullets, vector
// bullets, enumerators and task boxes with nesting by marker position (§6.3), code from monospace
// lines (§6.4), quotes from a left rule (§6.5). Paragraphs continue across columns and pages when
// the first ends without terminal punctuation and the next starts in lowercase. Footnotes (§6.9):
// a note starts with a marker (a superscript, or an enumerator in a smaller size or ending in a
// back link) that matches a superscript reference earlier on its page, or anywhere before for
// endnotes at the end of the document; notes are numbered in document order and written after
// the last block.
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
    Footnote, // a footnote definition; `number` is its number
    Table,    // a table (layout/tables.h): `number` is its index in its page's tables; lines: its anchor
              // line, then its cell lines in cell order
    Image,    // an image (layout/images.h): its one line is the anchor; `number` is its page number
};

enum class CommentKind : uint8_t {
    NoText,     // the page has no extractable text
    Unmapped,   // most of the page's text has no reliable Unicode mapping
    OcrFailed,  // the page has no extractable text and the OCR add-on failed on it (layout/ocr.h)
};

// GitHub alerts (`> [!NOTE]`): a block quote titled by one of these words alone on its first line.
enum class AlertKind : uint8_t { None, Note, Tip, Important, Warning, Caution };

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
    uint8_t alert = 0;      // AlertKind: the first block of a GitHub alert (quote >= 1)
    uint8_t task = 0;       // list items: 0 none, 1 open, 2 done
    bool ordered = false;   // list items
    bool listParagraph = false; // a further paragraph of the list item before it, at its `depth`
    // Ordered list items: the enumerator's value; comments: the page number; headings: when not
    // 0, the heading is the first `number` glyphs of its line only (a label run in with its
    // paragraph, which starts later on the same line).
    uint32_t number = 0;
    uint32_t first = 0;     // lines: BlockLine[first, first + count)
    uint32_t count = 0;
    float size = 0;         // the size of most of its characters
    // Headings set in small capitals (capitals of two sizes): its capitals below this size stand
    // for lower-case letters and are written so; 0 otherwise.
    float smallCaps = 0;
};

// A footnote reference: superscript glyphs [glyph, glyph + count) of a line in page `page`'s
// lineGlyphs order, written as [^number].
struct NoteRef {
    uint32_t page = 0;
    uint32_t glyph = 0;   // index into PageData::glyphs of the first marker glyph
    uint32_t count = 0;   // marker glyphs
    uint32_t number = 0;
};

struct BlockList {
    std::vector<Block> blocks;
    std::vector<BlockLine> lines;
    std::vector<NoteRef> noteRefs;  // sorted by page, then glyph
    bool monoDocument = false;      // typeset in a monospace font: monospace text is the body, not code
    // Bold (italic) glyphs make more than 60 % of the glyphs of the pages' ordered lines: that face
    // is the document's normal one, not emphasis.
    bool boldBody = false;
    bool italicBody = false;
};

// Pages carry their lines in reading order (PageData::order). `unmappedPages` holds per page (index
// into `pages`) bit 1 when its text is mostly unmapped and bit 2 when OCR failed on it. `out` is
// replaced.
void BuildBlocks(const std::vector<PageData>& pages, const std::vector<OutlineEntry>& outline,
                 const std::vector<uint8_t>& unmappedPages, BlockList& out);

} // namespace RayoPdf::Layout
