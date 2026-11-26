// SPDX-License-Identifier: Apache-2.0
// Glyph-shape recovery (docs/development/ocr-protocol.md § Glyph shapes): text from the shapes of
// the glyphs of fonts whose text is missing or wrong, and from text drawn as vector outlines.
//
// Fonts. On a page that fails the broken-text checks or shows junk characters (ocr_route.h), each
// font's own text is judged: a font with mostly unmapped or guessed codes, or whose Latin text is
// mostly accented letters or lacks vowels, is examined as a whole; in any other font only the
// codes with junk, unmapped or guessed text are. Each distinct glyph of those codes, over the
// whole document, is drawn once from its embedded program (text/outline.h; a Type 3 glyph
// procedure through ocr_paint.h) on a common baseline and scale, kGlyphLineHeight pixels high,
// the glyphs side by side with a gap, at most kGlyphsPerLine to a line; the lines go to the
// add-on in one `lines` request per font. Each recognised character belongs to the glyph whose
// cell holds the middle of its span, so one glyph may get several characters (a ligature); a
// glyph that gets none, or lies on a line below the confidence floor, keeps its old text.
//
// Acceptance per font: the recovered text replaces the font's text on the pages that sent it only
// when those pages then fail fewer of the checks, or as many but with less junk or fewer unreliable
// codes (and no check fires that did not before); otherwise the font keeps its text. An accepted
// font's glyphs get their text on every page of the document. Glyphs covered by ActualText or
// dropped keep theirs.
//
// Vector text: a page that looks like text drawn as vector outlines is painted (filled and stroked
// paths, ocr_paint.h) onto a page canvas and recognised as a whole, its lines added as an OCR text
// layer (ocr_layer.h).
#pragma once

#include "layout/convert.h"
#include "layout/ocr_client.h"
#include "layout/page.h"
#include "layout/pipeline.h"
#include "pdf/document.h"
#include "text/font.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Layout {

constexpr uint32_t kGlyphLineHeight = 48;    // pixels: the recogniser's own input height
constexpr uint32_t kGlyphsPerLine = 24;
constexpr uint32_t kMaxGlyphsPerFont = 2048;  // distinct glyphs drawn for one font
constexpr uint32_t kMaxRecoveryFonts = 64;    // fonts examined in one document
constexpr size_t kMaxGlyphUses = size_t(1) << 22;  // glyph uses held for one document
constexpr float kVectorPageScale = 300.0f / 72.0f;  // vector text rendered at 300 dpi

// What glyph-shape recovery did, for the report and the tests.
struct GlyphRecoveryStats {
    uint32_t fontsExamined = 0;   // fonts whose glyphs were sent
    uint32_t fontsAccepted = 0;
    uint32_t glyphsSent = 0;      // distinct glyphs drawn
    uint32_t glyphsRead = 0;      // ... that got text
    uint64_t usesReplaced = 0;    // shown glyphs whose text was replaced
    uint32_t requests = 0;
};

// Recovers the text of the fonts on `candidates` (indices into `pages`, interpreted with `fonts`
// and `annotations`, lines not built). Updates the glyphs' text and flags and
// quality[p].unreliable. Returns the first add-on failure (Ok when every request was answered or
// none was needed).
OcrStatus RecoverGlyphShapes(const Document& document, Text::FontCache& fonts, OcrSession& session,
                             AnnotationMode annotations, const std::vector<uint32_t>& candidates,
                             std::vector<PageData>& pages, std::vector<PageTextQuality>& quality,
                             GlyphRecoveryStats& stats);

// Paints the vector content of `page` (page.number - 1 of `document`) onto a page canvas at
// kVectorPageScale and recognises it, adding its lines as an OCR text layer: the non-space glyphs
// added, or the add-on's failure.
OcrStatus RecogniseVectorPage(const Document& document, OcrSession& session, PageData& page, uint32_t& added);

} // namespace RayoPdf::Layout
