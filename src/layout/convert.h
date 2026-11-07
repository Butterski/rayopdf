// SPDX-License-Identifier: Apache-2.0
// PDF to Markdown (roadmap D4 steps 2-8): each page's content stream interpreted into glyphs,
// rulings and images; glyphs grouped into words, lines and blocks; reading order; headers and
// footers removed; document semantics across pages (headings, paragraphs, lists, code, quotes,
// links); the Markdown writer (D6). Uncertainty is never silent: a page with no extractable text,
// unmapped glyphs and a repaired file get a `<!-- rayopdf: ... -->` comment where they apply.
#pragma once

#include "pdf/document.h"

#include <cstdint>
#include <string>

namespace RayoPdf::Layout {

struct ConvertOptions {
    int firstPage = 1;         // 1-based, inclusive
    int lastPage = 0;          // 0: the last page
    bool annotations = true;   // link annotations become Markdown links
    bool structure = true;     // a structure tree that passes the checks decides blocks and order (D5)
    // Images (D6): each image worth keeping is written to `imageDirectory` (created when the first
    // image is written) as p<page>-<n>.<ext> and referenced as ![alt](imageLinkPrefix + that name).
    // An empty `imageDirectory` writes no image files and no image references.
    std::string imageDirectory;
    std::string imageLinkPrefix;
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
};

// Converts the pages of `document` to Markdown in `markdown` (replaced). Never fails: what cannot
// be read is reported in `report` and marked in the text.
void Convert(const Document& document, const ConvertOptions& options, std::string& markdown, ConvertReport& report);

} // namespace RayoPdf::Layout
