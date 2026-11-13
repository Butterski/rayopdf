// SPDX-License-Identifier: Apache-2.0
// PDF to Markdown (roadmap D4 steps 2-8): each page's content stream interpreted into glyphs,
// rulings and images; glyphs grouped into words, lines and blocks; reading order; headers and
// footers removed; document semantics across pages (headings, paragraphs, lists, code, quotes,
// links); the Markdown writer (D6). Uncertainty is never silent: a page with no extractable text,
// unmapped glyphs and a repaired file get a `<!-- rayopdf: ... -->` comment where they apply.
#pragma once

#include "layout/math.h"
#include "layout/ocr_options.h"
#include "pdf/document.h"

#include <cstdint>
#include <string>

namespace RayoPdf::Layout {

// ConvertOptions::pageWorkers 0: at most this many threads (the serial stages of a conversion
// leave little to gain from more).
constexpr unsigned kDefaultPageWorkers = 4;

// What Convert writes (roadmap D6): Markdown, or the document as one JSON object (schema 1, the
// blocks with their pages, boxes and Markdown; layout/markdown_writer.h WriteJson).
enum class OutputFormat : uint8_t { Markdown, Json };

// Page furniture (running headers, footers, page numbers and margin lines; layout/reading_order.h):
// left out (the default), or kept as text where it stands. A structure tree that is used still
// leaves out what it marks as artifacts.
enum class Furniture : uint8_t { Drop, Keep };

// Tables as GFM pipe tables unless a span needs HTML (the default), or every table as HTML.
enum class TableFormat : uint8_t { Auto, Html };

struct ConvertOptions {
    int firstPage = 1;         // 1-based, inclusive
    int lastPage = 0;          // 0: the last page
    bool annotations = true;   // link annotations become Markdown links
    bool structure = true;     // a structure tree that passes the checks decides blocks and order (D5)
    bool exact = true;         // a verified rayomd-source/1 payload is written as it is (D5, layout/exact.h)
    // Images (D6): each image worth keeping is written to `imageDirectory` (created when the first
    // image is written) as p<page>-<n>.<ext> and referenced as ![alt](imageLinkPrefix + that name).
    // An empty `imageDirectory` writes no image files and no image references.
    std::string imageDirectory;
    std::string imageLinkPrefix;
    // OCR where the text layer fails (layout/ocr.h); without an add-on the output is the same.
    OcrOptions ocr;
    // Formulas as Unicode (D6, the default) or as LaTeX between $ and $$ (layout/math.h).
    MathMode math = MathMode::Unicode;
    OutputFormat format = OutputFormat::Markdown;
    // Markdown only: `<!-- page N -->` before the blocks of each page (layout/markdown_writer.h).
    bool pageMarkers = false;
    // Markdown only: YAML front matter from the document's /Info first (layout/metadata.h), unless
    // a verified RayoMD payload that has its own is written.
    bool frontMatter = false;
    Furniture furniture = Furniture::Drop;
    // Text without inline markup: no emphasis, code spans, strikethrough, links (their text stays),
    // <sup>/<sub> tags (their text stays) or footnote marks ("[^1]" is written "1", a definition
    // "1 text"). Escapes and the block structure (headings, lists, tables) are written as always.
    bool plain = false;
    TableFormat tables = TableFormat::Auto;
    // Threads for the page-local stages of a document of several pages (roadmap D4): the pages are
    // interpreted, and their lines built, on up to this many threads; 0 is one per CPU the process
    // may run on, at most kDefaultPageWorkers, each started only once the pages read so far are
    // worth a thread (a document of small pages starts none). The output is the same for every
    // count; 1 (and any document of one page) runs everything on the calling thread.
    unsigned pageWorkers = 1;
    // The process converts this document only and ends when Convert has returned and the Markdown
    // is written (--convert): buffers kept for later documents are freed once read, so that later
    // stages reuse their memory instead of touching new pages; and what the conversion held when
    // it ended (its pages, fonts and structure, and its page threads, left waiting) is not freed
    // or joined, which the end of the process does at once.
    bool oneShot = false;
};

// What became of the document's structure tree (tagged PDF, roadmap D5; layout/structure.h).
enum class StructureUse : uint8_t {
    None,       // no structure tree: an untagged file
    Used,       // the tree decided blocks and reading order
    Broken,     // the tree could not be read (no content items, a limit, broken objects)
    RoleCycle,  // rejected, check 4: the RoleMap has a cycle
    Coverage,   // rejected, check 1: too little of the visible text belongs to elements
    Generic,    // rejected, check 2: no heading, list or table elements hold text
    Order,      // rejected, check 3: the tree's order contradicts the page's layout
};

// "none", "used", "broken", "role-cycle", "coverage", "generic", "order".
const char* StructureUseName(StructureUse use);

// What became of a RayoMD source payload (rayomd-source/1, layout/exact.h).
enum class ExactUse : uint8_t {
    Off,          // --no-exact: not looked for
    None,         // the file claims no payload
    Used,         // the payload passed both checks and is the Markdown written
    Pages,        // not used: a page range was asked for and the payload is the whole document
    Format,       // not used: the output asked for (JSON, page markers, plain text, HTML tables, kept
                  // furniture) needs the pages' layout
    Unsupported,  // rejected: another profile version or encoding
    Corrupt,      // rejected: the file or the profile's objects are not as the profile writes them
    Integrity,    // rejected: the payload's SHA-256 or length does not match its packet
    Limit,        // rejected: the PDF or the payload is past the profile's limits
    Utf8,         // rejected: the payload is not UTF-8
    Mismatch,     // rejected: the payload does not say what the pages show
};

// "off", "none", "used", "pages", "format", "unsupported", "corrupt", "integrity", "limit", "utf8",
// "mismatch".
const char* ExactUseName(ExactUse use);

// Counters of one conversion: `--report` prints them as one JSON line.
struct ConvertReport {
    int pages = 0;                   // pages converted
    int pagesWithoutText = 0;        // pages with no extractable text (image-only or empty)
    uint64_t glyphs = 0;             // shown codes on the converted pages
    uint64_t unmappedGlyphs = 0;     // codes with no Unicode text
    uint64_t guessedGlyphs = 0;      // codes whose text is a guess (UnicodeSource::Guess)
    uint64_t hiddenGlyphs = 0;       // codes dropped as invisible duplicates or clipped away
    int headings = 0;
    int paragraphs = 0;
    int listItems = 0;
    int codeBlocks = 0;
    int tables = 0;
    int links = 0;
    int footnotes = 0;               // footnote definitions written
    int images = 0;                  // image files written
    int imagesSkipped = 0;           // images not written (undecodable, too small, decoration)
    int imagesMaskDropped = 0;       // written images whose mask the file lacks (JPX, see image.h)
    int headerFooterLines = 0;       // repeated page headers, footers and page numbers removed
    int contentErrors = 0;           // content streams that could not be fully read or interpreted
    StructureUse structure = StructureUse::None;
    uint32_t structureElements = 0;  // structure elements read (0 without a tree)
    // Exact recovery (layout/exact.h). With Used the Markdown is the payload and the counters above
    // are those of the conversion its check ran (no image files written).
    ExactUse exact = ExactUse::None;
    // OCR (layout/ocr.h). Routed: a page sent whole (image-only), or with image regions whose pixels
    // look like text (counted only when an add-on runs to check them).
    int ocrPagesRouted = 0;
    int ocrPages = 0;                // routed pages that received OCR text
    int ocrFailures = 0;             // routed pages on which the add-on failed (or was not used after failing)
    bool ocrAddonMissing = false;    // pages were routed and no add-on was found
    int ocrBrokenPages = 0;          // pages whose text layer fails the broken-text checks
    int ocrVectorTextPages = 0;      // pages that look like text drawn as vector outlines
    // Glyph-shape recovery (layout/ocr_glyphs.h): fonts whose recognised glyphs replaced their text,
    // and the glyphs (shown codes) given text so.
    int ocrGlyphFonts = 0;
    uint64_t ocrGlyphs = 0;
    int ocrTables = 0;               // tables read from images by the OCR add-on
};

// Converts the pages of `document` to Markdown in `markdown` (replaced): a verified RayoMD source
// payload as it is (layout/exact.h), else the pages; with OutputFormat::Json the document as one
// JSON object instead. Never fails: what cannot be read is reported in `report` and marked in the
// text.
void Convert(const Document& document, const ConvertOptions& options, std::string& markdown, ConvertReport& report);

} // namespace RayoPdf::Layout
