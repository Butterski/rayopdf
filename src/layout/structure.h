// SPDX-License-Identifier: Apache-2.0
// Tagged PDF (ISO 32000-1 §14.7-14.9, algorithms §6.10, roadmap M6; D5 precedence: exact recovery >
// validated structure tree > outline > geometry).
//
// ReadStructure reads the structure tree once per document: its elements depth-first, which is
// the logical order (§14.8.2.3.1); their roles, standard types first and other names through the
// RoleMap (transitively, a cycle rejects the tree); the attributes Markdown needs (table row and
// column spans, list numbering, line-through, placement); /Alt and /ActualText; and the content
// items they own: marked-content ids on the converted pages and in Form XObjects (integer kids and
// MCR dictionaries, the page from /Pg or, failing that, from the ParentTree), and annotations
// (OBJR). Elements reached twice (a shared kid or a cycle) are read once; every walk is bounded.
//
// TaggedLayout checks the tree against the interpreted pages before trusting it (§6.10):
//   1. at least 95 % of the visible glyphs that are no artifacts belong to an element;
//   2. it says more than "paragraph": headings, list items, table cells, code, quotes, notes,
//      figures with their images or inline emphasis hold content (not only P, Span and Div);
//   3. on each page, the tree's order of the text breaks Breuel's rule 1 (of two pieces of text
//      that overlap horizontally, the upper one comes first) for at most 5 % of such pairs;
//   4. the RoleMap has no cycle (found while reading).
// A tree that fails, and a file without one, take the geometry path unchanged.
//
// A trusted tree decides the blocks and their order. Each block-level element owns the text of
// its marked content: P, H and H1-H6 (levels), LI (its Lbl gives the marker: ordered or not and
// the number; nesting from L inside L or LI, or from marker positions when the tree lists items
// flat), Code, Caption, TH/TD (tables with row and column spans through Table/THead/TBody/TR),
// Figure (its images, with /Alt as the alternate text), Note; BlockQuote sets the quote depth;
// a Formula whose /Alt (else /ActualText) is LaTeX gives it to its glyphs (PageData::formulaAlts),
// which --math=latex writes in their place ($$…$$ when the Formula is a block of its own);
// inline elements (Span, Link, Em, Strong, Code inside text, Quote, Reference, Lbl) belong to the
// block around them and lend it emphasis (Strong, Em), code spans (Code) and strike-through
// (TextDecorationType LineThrough) through the glyph flags. Artifacts and untagged content are
// left out. Lines are split where the element changes. Text, words, scripts, links and
// dehyphenation still come from the glyphs. From the geometric blocks of the same lines
// (semantics.h) come what the trees met in practice do not say reliably: footnotes, code
// set in a monospace font in plain paragraphs, list items tagged as paragraphs (or as text of an
// LI without a Lbl), and paragraph breaks inside one element where its marked content changes.
// Two more general rules: an element set on the line the block before it ends in joins that
// block (a formula tagged as a block inside a sentence), and a paragraph indented from both sides
// (/StartIndent and /EndIndent) is a quote when the tree has no BlockQuote. A lone H1 opening the
// document in the style of a deeper heading takes that level (PDF/UA writers renumber the first
// heading to H1).
#pragma once

#include "layout/convert.h"
#include "layout/page.h"
#include "layout/semantics.h"
#include "pdf/document.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RayoPdf::Layout {

// Hostile-input bounds (roadmap D8): past them the tree is Broken and the geometry path runs.
constexpr uint32_t kMaxStructureNodes = 1u << 20;    // elements
constexpr uint32_t kMaxStructureItems = 1u << 22;    // marked-content ids and annotations
constexpr uint32_t kMaxStructureDepth = 512;         // element nesting
constexpr uint32_t kMaxRoleMapChain = 32;            // RoleMap hops from a name to a standard type
constexpr uint32_t kMaxNumberTreeDepth = 32;         // ParentTree /Kids levels
constexpr uint32_t kMaxStructureText = 1u << 24;     // bytes of /Alt and /ActualText kept

// The checks (algorithms §6.10).
constexpr float kMinStructureCoverage = 0.95f;   // check 1
constexpr float kMaxOrderViolations = 0.05f;     // check 3
constexpr uint32_t kMaxOrderUnits = 512;         // pieces of text per page compared pairwise in check 3

// The role of an element, after the RoleMap.
enum class StructRole : uint8_t {
    Unknown,     // no standard type and no RoleMap entry: by its /Placement, else by context
    Group,       // Document, Part, Art, Sect, Div, NonStruct, Private, TOC, Index, Aside, ...
    BlockQuote,
    Paragraph,   // P, Caption, TOCI, BibEntry, Title
    Heading,     // H, H1-H6
    List,        // L
    ListItem,    // LI
    Label,       // Lbl
    ListBody,    // LBody
    Table,
    TableHead,   // THead
    TableBody,   // TBody, TFoot
    TableRow,    // TR
    TableHeader, // TH
    TableCell,   // TD
    Inline,      // Span, Quote, Reference, Annot, Ruby, Warichu and their parts, Sub
    Emphasis,    // Em
    Strong,
    Link,
    Code,
    Note,        // Note, FENote
    Figure,
    Formula,
    Form,
    Artifact,
};

enum : uint8_t {
    kNodeLineThrough = 1u << 0,      // /TextDecorationType /LineThrough
    kNodeBlock = 1u << 1,            // /Placement /Block
    kNodeInline = 1u << 2,           // /Placement /Inline
    kNodeOrdered = 1u << 3,          // /ListNumbering of an ordered kind
    kNodeUnordered = 1u << 4,        // /ListNumbering Disc, Circle, Square
    kNodeStartIndent = 1u << 5,      // /StartIndent above 0
    kNodeEndIndent = 1u << 6,        // /EndIndent above 0
};

struct StructNode {
    uint32_t parent = UINT32_MAX;    // UINT32_MAX for the tree's top elements
    StructRole role = StructRole::Unknown;
    uint8_t level = 0;               // headings: 1-6, 0 for H
    uint8_t flags = 0;
    uint8_t reserved = 0;
    uint16_t rowSpan = 1, colSpan = 1;
    uint32_t alt = 0, altLength = 0;        // /Alt in Structure::text
    uint32_t actual = 0, actualLength = 0;  // /ActualText in Structure::text
};

// A marked-content id (or an annotation, mcid -1) of a converted page and the element it belongs
// to; `seq` is its place in the tree's depth-first order.
struct StructItem {
    uint32_t page = 0;     // index into the converted pages
    uint32_t stream = 0;   // object number of the Form XObject the MCID counts in; 0 for the page
    int32_t mcid = -1;
    uint32_t annotation = 0;  // object number of an OBJR's annotation
    uint32_t node = 0;
    uint32_t seq = 0;
};

struct Structure {
    StructureUse use = StructureUse::None;  // None, Broken, RoleCycle, or Used once read (a candidate)
    std::vector<StructNode> nodes;          // depth-first: the logical order
    std::vector<StructItem> items;          // sorted by page, stream, mcid
    std::vector<uint32_t> pageItems;        // items of page p: [pageItems[p], pageItems[p + 1])
    std::string text;
};

// Reads the structure tree of `document` for pages [first, last] (0-based, inclusive) into `out`
// (replaced). Never fails: a missing tree leaves use None, an unreadable one Broken or RoleCycle.
void ReadStructure(const Document& document, int first, int last, Structure& out);

// The tagged path through the pipeline stages (pipeline.cpp), for one document.
class TaggedLayout {
public:
    TaggedLayout();
    ~TaggedLayout();
    TaggedLayout(const TaggedLayout&) = delete;
    TaggedLayout& operator=(const TaggedLayout&) = delete;

    // After BuildLines: runs checks 1-3 on `pages` and returns the verdict. When Used, splits the
    // lines where the element changes, removes untagged and artifact lines, sets the glyph flags
    // inline elements imply, and skips images outside the tree (before PlaceImages).
    StructureUse Prepare(std::vector<PageData>& pages, const Structure& structure);
    // After PlaceImages, per page, instead of DetectTables and OrderLines: tables from the tree,
    // the alternate text of images, and the reading order of the tree in page.order.
    void LayOut(std::vector<PageData>& pages, uint32_t page);
    // After BuildBlocks over the tree's order: replaces `blocks` with the tree's blocks, keeping
    // from the geometric ones what the tree does not say (footnotes, code in plain paragraphs,
    // list items tagged as paragraphs, page comments).
    void Blocks(const std::vector<PageData>& pages, BlockList& blocks);

private:
    struct Owner;
    struct TableInfo;
    void FindOwners();
    void LayOutTables(PageData& page, uint32_t p);
    const Structure* structure_ = nullptr;
    std::vector<Owner> owners_;
    std::vector<uint32_t> nodeOwner_;                // per node
    std::vector<uint8_t> nodeStyle_;                 // per node: glyph flags (bits of kStyle*)
    std::vector<uint32_t> nodeFigure_;               // per node: the Figure around it, or UINT32_MAX
    // per node: the outermost Formula around it whose /Alt (else /ActualText) reads as LaTeX
    // (layout/math.h LatexAlt), or UINT32_MAX
    std::vector<uint32_t> nodeFormula_;
    std::vector<uint8_t> nodeLabel_;                 // per node: inside the Lbl of a list item
    std::vector<TableInfo> tables_;
    std::vector<std::vector<uint32_t>> glyphOwner_;  // per page, per glyph: owner (top bit: label)
    std::vector<std::vector<uint32_t>> glyphSeq_;    // per page, per glyph: item seq
    std::vector<std::vector<uint32_t>> lineOwner_;   // per page, per line: owner
    std::vector<std::vector<uint64_t>> lineKey_;     // per page, per line: order inside its owner
    std::vector<uint32_t> ownerPage_;                // per owner: the first page with its text
};

} // namespace RayoPdf::Layout
