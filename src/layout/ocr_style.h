// SPDX-License-Identifier: Apache-2.0
// Typography and rules read from the pixels of an OCR canvas (docs/development/ocr-protocol.md
// § Text layer): what a scan shows of its text besides the characters the add-on recognised.
//
// The add-on reports a box per line and a span per character. From the canvas under them each
// line gets its baseline and font size from the ink of its letters (the x-height of letters like
// "a" and "n", else the cap height of capitals and digits), a weight per word from the width of
// its strokes (bold against the page's body text), and a pitch test for fixed-pitch text (every
// character, spaces included, one cell wide). Rules and bars drawn on the page (table rulings,
// quote bars) are found as long thin runs of dark pixels.
#pragma once

#include "common/ocr_protocol.h"
#include "layout/ocr_layer.h"
#include "layout/page.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Layout {

struct OcrLineStyle {
    bool measured = false;  // false: the line keeps the size and baseline of its box
    float baseline = 0;     // canvas row (pixels, y down) under its letters' feet
    float em = 0;           // font size in canvas pixels
    float pitch = 0;        // fixed-pitch text: the cell width of its grid (canvas pixels), else 0
    float origin = 0;       // ... and the centre of the grid's cell 0 (canvas column)
    uint32_t first = 0;     // its code points' glyph flags: OcrStyle::flags[first, first + code points)
};

struct OcrStyle {
    std::vector<OcrLineStyle> lines;  // one per response line, in response order
    std::vector<uint16_t> flags;      // kGlyphBold and kGlyphMonospace per code point
    float bodyEm = 0;                 // the page's body size in canvas pixels, 0 when nothing was measured
    int paper = 255, ink = 0;         // gray levels of paper and ink under the lines (Otsu's classes)
};

// Measures the lines of `response` on `canvas`. Pages whose text shows too little contrast
// (paper and ink closer than kOcrMinContrast) get nothing measured.
void MeasureOcrStyle(const OcrCanvas& canvas, const OcrProtocol::Response& response, OcrStyle& style);

// Rules on `canvas` (dark runs at least 3 em long and horizontal, or 1.3 em long and vertical, at
// most 0.3 em thick, `em` in canvas pixels) appended to page.rulings and page.shapes as filled
// rectangles, unless the page already has rulings over the canvas. Returns how many.
uint32_t AddOcrRulings(const OcrCanvas& canvas, const OcrStyle& style, PageData& page);

constexpr int kOcrMinContrast = 48;

} // namespace RayoPdf::Layout
