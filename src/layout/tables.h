// SPDX-License-Identifier: Apache-2.0
// Tables (algorithms §6.7, roadmap M5, D6). Three detectors run on each page's lines and rulings,
// strongest evidence first, and a table found by one hides its area from the next:
//
// 1. Ruled lattices: the axis-aligned pieces of painted paths (PageData::rulings) become horizontal
//    and vertical edges (snapped and joined in the manner of pdfplumber after Nurminen); edges that
//    touch form a grid; a cell whose internal edge is missing spans the minimal cells it covers.
// 2. Partially ruled tables: stacks of horizontal rules of one width (booktabs top, mid and bottom
//    rules; one rule under a header row) with rows from text baselines between and around them, and
//    columns from vertical whitespace that crosses every row. The rows above the second rule are
//    the header.
// 3. Borderless tables: runs of rows that split into the same columns, accepted only with high
//    confidence (several rows, filled cells, aligned column edges, numeric cells, even row pitch).
//    Regions of lesser confidence keep their text, without comment: a false table costs more than a
//    missed one.
//
// Prose in columns (two texty columns) and lists (a column of list markers) are never tables.
// Cell text is the page's glyphs whose centre lies in the cell, as lines of their own in reading
// order inside the cell; the lines they came from leave the reading order, and an anchor line with
// the table's box takes their place there (page.h kLineTableAnchor).
#pragma once

#include "layout/page.h"
#include "layout/semantics.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RayoPdf::Layout {

enum class TableKind : uint8_t {
    Lattice,     // a grid of vertical and horizontal rulings
    Ruled,       // horizontal rules only; columns from whitespace
    Borderless,  // no rules; rows and columns from text alignment
};

// Hostile-input bounds (roadmap D8): beyond them the candidate is dropped (fail closed).
constexpr uint32_t kMaxTableEdges = 2048;  // merged edges per orientation and page for lattices
constexpr int kMaxTableRows = 500;
constexpr int kMaxTableCols = 40;
constexpr int kMaxTablesPerPage = 64;

// The columns of a table that ends its page: rows at the top of the next page that fit them
// continue it (a long table broken across pages, continued without rules or header). Pages are
// detected in order, each passing this to the next.
struct TableContinuation {
    bool valid = false;
    float x0 = 0, x1 = 0;
    std::vector<float> xs;  // column boundaries, ascending
};

// Finds the tables of a page: fills page.tables and page.tableCells, appends the cell lines and
// one anchor line per table to page.lines, and flags the lines whose text moved into cells with
// kLineTableCell. Upright text first; then text of each other baseline direction with lines
// enough for a table (a landscape table on a portrait page), in that direction's frame (page.h),
// its cell and anchor lines of that direction. Lines taken for running headers and footers that
// lie in a table return to the text (kLineRemoved cleared); the result is how many. Runs after
// BuildLines and RemoveRunningLines, before OrderLines. `continuation` (upright tables only) comes
// from the previous page and is replaced for the next.
int DetectTables(PageData& page, TableContinuation& continuation);

// What DetectTables changes on a page, to undo it (pages detected on several threads at once,
// layout/pipeline.cpp): it only appends to page.lines, page.lineGlyphs, page.tableCells and
// page.tables, and rewrites the flags of the lines there were. SaveTables before DetectTables;
// UndoTables puts the page back as it was then. (Inline here: tables.cpp's code stays as it is.)
struct TableUndo {
    size_t lines = 0, lineGlyphs = 0, tableCells = 0, tables = 0;
    std::vector<uint16_t> flags;  // of each line there was
};

inline void SaveTables(const PageData& page, TableUndo& undo) {
    undo.lines = page.lines.size();
    undo.lineGlyphs = page.lineGlyphs.size();
    undo.tableCells = page.tableCells.size();
    undo.tables = page.tables.size();
    undo.flags.resize(page.lines.size());
    for (size_t i = 0; i < page.lines.size(); ++i) undo.flags[i] = page.lines[i].flags;
}

inline void UndoTables(PageData& page, const TableUndo& undo) {
    page.lines.resize(undo.lines);
    page.lineGlyphs.resize(undo.lineGlyphs);
    page.tableCells.resize(undo.tableCells);
    page.tables.resize(undo.tables);
    for (size_t i = 0; i < undo.lines; ++i) page.lines[i].flags = undo.flags[i];
}

// Whether table `table` of page `page` continues the Table block `previous` (whose lines are in
// `lines`): its last part ends the page before and has as many columns.
bool TableContinues(const std::vector<PageData>& pages, const std::vector<BlockLine>& lines, const Block& previous,
                    uint32_t page, uint32_t table);

// One page's piece of a table; a table broken across pages is written from several parts with the
// same number of columns.
struct TablePart {
    const PageData* page = nullptr;
    const Table* table = nullptr;
    const std::string* cellText = nullptr;  // GFM: the inline Markdown of each cell, one line each
};

// D6: a table is written as HTML when a body cell with text spans rows, or spans columns under a
// header of several rows; any other as a GFM pipe table.
bool TableNeedsHtml(const PageData& page, const Table& table);

// How many leading rows of `part` repeat the header of `head` (a header repeated on each page):
// the writers drop them from a later part.
uint16_t RepeatedHeader(const TablePart& head, const TablePart& part);

// A GFM pipe table: the first row of the first part is the header row (header rows after it are
// written as body rows); header rows of a later part that repeat the first part's are dropped.
// `|` in cells is escaped; a cell spanning rows is written in its first row (the rows below it
// empty), a cell spanning columns in each of them with `repeatColumnSpans`, else in its first
// column (the columns after it empty). `latexCells` (TableFormat::PipeLatex): `%`, `#` and `&` of
// the cells outside `$…$` math and code spans are written as the text `\%`, `\#`, `\&` (Markdown
// `\\%`, ...), so that a reader taking the cells through LaTeX (READoc's evaluator) reads them.
void WriteGfmTable(const std::vector<TablePart>& parts, std::string& out, bool repeatColumnSpans, bool latexCells);

// An HTML <table> with <th> header rows, colspan and rowspan, and HTML-escaped plain cell text
// (a GFM HTML block: one line per row, no blank lines inside). `spaced`: a space between the tags
// of a row and around each cell's text (`<tr> <td> 12 </td> </tr>`), as HTML pretty-printers
// set them; a browser shows the same table.
void WriteHtmlTable(const std::vector<TablePart>& parts, std::string& out, bool spaced = false);

} // namespace RayoPdf::Layout
