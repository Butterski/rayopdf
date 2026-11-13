// SPDX-License-Identifier: Apache-2.0
// Where OCR runs (docs/development/ocr-protocol.md § Routing; the OCR/ML decision,
// docs/development/ocr_ml_decision.md): only where the text layer fails, decided per page from what
// interpretation found, before lines are built.
//
// - A page with fewer than kPageMinChars characters of text (visible or an invisible OCR layer) and
//   images covering at least kPageMinCoverage of it (census class image_only) is OCRed as a whole:
//   its images composed onto one page raster.
// - On any other page, an image covering at least kRegionMinShare of the page, at least
//   kRegionMinPixels wide and high, with no text drawn over it, is a candidate region; in auto mode
//   it is OCRed only when its pixels look like printed text (LooksLikeText).
// - A page whose text layer fails the broken-text checks (the census's four checks of docling,
//   unmapped or guessed codes for most glyphs), or shows junk characters, goes to glyph-shape
//   recovery (layout/ocr_glyphs.h); a page that looks like text drawn as vector outlines is
//   rendered and recognised as a whole.
// - An existing invisible text layer is the page's text: such a page is not routed (force mode
//   replaces it when the OCR answer has text).
#pragma once

#include "layout/ocr_options.h"
#include "layout/page.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Layout {

constexpr uint32_t kPageMinChars = 50;        // census: fewer is no text layer
constexpr float kPageMinCoverage = 0.8f;      // census image_only
constexpr float kRegionMinShare = 0.05f;      // ocr_ml_decision.md image-region rule
constexpr uint32_t kRegionMinPixels = 64;     // narrower or lower images hold no line of text worth OCR
constexpr uint32_t kRegionMaxCoveringChars = 10;  // text drawn over an image: it has a text layer
constexpr uint32_t kVectorTextMinShapes = 300;    // glyph-sized filled curves on a page without text

// The broken-text checks a page fails (PageSignals::checks).
enum : uint8_t {
    kCheckJunk = 1,        // junk characters >= 3 %
    kCheckAccented = 2,    // >= 50 % of >= 50 Latin letters accented
    kCheckVowels = 4,      // vowel share < 0.25 over >= 200 letters in words of 3-20 letters
    kCheckUnreliable = 8,  // unmapped or guessed codes for more than half of >= 20 glyphs
};

// What routing reads of a page.
struct PageSignals {
    uint32_t visibleChars = 0;    // non-space glyphs BuildLines would keep, drawn visibly
    uint32_t invisibleChars = 0;  // ... drawn invisibly (an OCR layer)
    float coverage = 0;           // image area over page area, summed over placements, at most 1
    bool broken = false;          // the text layer fails a broken-text check
    bool vectorText = false;      // many glyph-sized filled curves, little text
    uint8_t checks = 0;           // the checks it fails (kCheck*)
    uint32_t chars = 0;           // non-space characters of the counted glyphs
    uint32_t junk = 0;            // ... that are junk (Text::Repair::IsJunk)
    uint32_t latin = 0, accented = 0;           // Latin letters, of them U+00C0 and up
    uint32_t wordLetters = 0, wordVowels = 0;   // letters (vowels) in words of 3-20 Latin letters
};

// Junk characters that send a page to glyph-shape recovery even when no check fires.
constexpr uint32_t kGlyphRecoveryMinJunk = 3;

// Signals of `page` (interpreted, lines not built). `glyphs` and `unreliable` are the page's shown
// and unmapped + guessed codes (PageStats).
void MeasurePage(const PageData& page, uint64_t glyphs, uint64_t unreliable, PageSignals& signals);

enum class OcrRoute : uint8_t {
    None,
    Page,     // the whole page: `images` composed onto a page raster
    Regions,  // each of `images` on its own
};

struct OcrPlan {
    OcrRoute route = OcrRoute::None;
    std::vector<uint32_t> images;    // indices into PageData::images
    bool checkText = false;          // Regions: confirm each by LooksLikeText (auto mode)
    bool replaceInvisible = false;   // Page: an existing invisible layer gives way to OCR text
    // Regions: the page has a text layer (at least kPageMinChars characters), so a region's text is
    // a figure's, not the page's.
    bool textLayer = false;
};

void PlanPage(const PageData& page, const PageSignals& signals, OcrMode mode, OcrPlan& plan);

// What LooksLikeText measured.
struct TextEvidence {
    float ink = 0;          // share of pixels darker than the Otsu threshold
    float midtones = 0;     // share of pixels well between ink and paper
    float contrast = 0;     // paper mean - ink mean, 0-255
    uint32_t bands = 0;     // runs of rows holding ink
    uint32_t textBands = 0; // bands shaped like lines of text (height, stroke density), of a common height
    float textRows = 0;     // share of rows in those bands
};

// Whether 8-bit gray pixels (rows top to bottom) look like printed text: dark ink on light paper
// with few midtones (not a photo), and at least kMinTextBands rows of characters of a common height
// (not a chart with a few labels). Built for no false triggers on digital documents' figures.
constexpr uint32_t kMinTextBands = 4;
bool LooksLikeText(const uint8_t* pixels, uint32_t width, uint32_t height, TextEvidence* evidence = nullptr);

} // namespace RayoPdf::Layout
