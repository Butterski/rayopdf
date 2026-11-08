// SPDX-License-Identifier: Apache-2.0
// The content-stream interpreter (algorithms §2, roadmap D4 step 2): one page's content and the
// Form XObjects it draws, read into the page model of page.h. Text positions follow ISO 32000-1
// §9.4.4 (algorithms §2.3); marked content follows §14.6 (ActualText, without control characters,
// replaces the glyphs it encloses, Artifact spans are flagged, MCIDs kept on glyphs and images with
// the stream they count in, the page's or a Form XObject's); paths become shapes for rulings, quote
// bars and vector bullets.
//
// Hostile input: q/Q nesting is capped at kMaxStateDepth, Form XObjects at kMaxFormDepth with a
// cycle check, and every page has an operator and a glyph budget. A broken operand skips its
// operator; a stream that cannot be decoded or interpreted fully counts as a content error.
#pragma once

#include "layout/page.h"
#include "pdf/document.h"
#include "text/font.h"

#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Layout {

constexpr int kMaxStateDepth = 256;
constexpr int kMaxFormDepth = 40;
constexpr uint32_t kMaxOperatorsPerPage = 4u << 20;
constexpr uint32_t kMaxGlyphsPerPage = 1u << 19;
constexpr uint32_t kMaxShapesPerPage = 1u << 16;
constexpr uint32_t kMaxLinksPerPage = 1u << 14;
constexpr size_t kMaxInlineImageBytes = size_t(64) << 20;  // inline image sources kept per page
constexpr uint32_t kMaxRulingsPerPage = 1u << 16;
constexpr uint32_t kMaxPiecesPerPath = 4096;  // straight axis-aligned pieces kept of one path
// Forms with marked content of their own per page (PageData::markStreams); MCIDs in further forms
// are dropped (-1).
constexpr size_t kMaxMarkStreams = 254;
constexpr uint8_t kNoMarkStream = 255;

// One glyph as glyph-shape recovery (layout/ocr_glyphs.h) needs it: its font, code and placement.
struct GlyphUse {
    uint32_t glyph = 0;               // index in PageData::glyphs
    uint32_t text = 0;                // its Glyph::text when emitted (ActualText moves it)
    const Text::Font* font = nullptr;
    uint32_t code = 0;
    uint8_t bytes = 1;                // the code's length in the string
    float matrix[6] = {};             // text space at size 1 to the upright page (Trm)
};

// Counters of one page for ConvertReport.
struct PageStats {
    uint64_t glyphs = 0;    // shown codes
    uint64_t unmapped = 0;  // codes with no Unicode text
    uint64_t guessed = 0;   // codes whose Unicode text is a guess
    // When set (glyph-shape recovery interprets a page again), every glyph kept on the page.
    std::vector<GlyphUse>* uses = nullptr;
};

// Interprets page `index` (0-based) of `document` into `page` (cleared first). `withLinks` reads
// the page's link annotations too.
void InterpretPage(const Document& document, Text::FontCache& fonts, int index, bool withLinks, PageData& page,
                   PageStats& stats);

// The document outline (§12.3.3), depth-first, at most kMaxOutlineEntries entries.
constexpr int kMaxOutlineEntries = 20000;
void ReadOutline(const Document& document, std::vector<OutlineEntry>& out);

// A PDF text string (§7.9.2.2: UTF-16BE or UTF-8 with a byte order mark, else PDFDocEncoding)
// appended to `out` as UTF-8.
void AppendTextString(std::string_view bytes, std::string& out);

} // namespace RayoPdf::Layout
