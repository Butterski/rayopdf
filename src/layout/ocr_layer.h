// SPDX-License-Identifier: Apache-2.0
// OCR input and output in page space (docs/development/ocr-protocol.md § Pixels and § Text layer):
// a page region rendered from its images alone onto a gray canvas, and the add-on's lines turned
// into glyphs of an OCR text layer. The canvas shows the upright page (CropBox origin, /Rotate
// applied) with y down: canvas pixel (cx, cy) is the point (x0 + cx / scale, y1 - cy / scale).
// Images are drawn through their placement matrix with nearest-neighbour sampling of their gray
// pixels (already reduced by area averaging to about the canvas resolution); where images
// overlap the darker pixel wins, which keeps the text of mixed raster content (a JBIG2 or CCITT
// text mask over a JPEG background) whatever the drawing order.
#pragma once

#include "common/ocr_protocol.h"
#include "layout/page.h"
#include "pdf/document.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RayoPdf::Layout {

constexpr float kOcrMinScale = 1.0f;                    // pixels per point: 72 dpi
constexpr float kOcrMaxScale = 300.0f / 72.0f;          // 300 dpi: no OCR engine needs more
constexpr uint64_t kOcrMaxCanvasPixels = uint64_t(1) << 26;
constexpr uint32_t kOcrMaxCanvasSide = 32768;
constexpr uint16_t kOcrMinConf = 5000;                  // lines below 0.5 confidence are dropped

struct OcrCanvas {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the page region shown (points)
    float scale = 1;                        // pixels per point
    uint32_t width = 0, height = 0;
    std::string pixels;                     // 8-bit gray, white where no image is drawn
};

// A white canvas over the region at `scale`, clamped to kOcrMinScale..kOcrMaxScale and lowered
// until the canvas fits kOcrMaxCanvasPixels and kOcrMaxCanvasSide. False for an empty region.
bool StartCanvas(float x0, float y0, float x1, float y1, float scale, OcrCanvas& canvas);

// Pixels per point of `image` where it is placed (its /Width x /Height over the area its matrix
// gives it); 0 when unknown.
float ImageScale(const ImageBox& image);

// Draws `pixels` (`width` x `height` gray, rows top to bottom), placed by `matrix` (a b c d e f:
// the image's unit square, first row at y = 1, to page space), onto `canvas`, darker winning.
void DrawGray(OcrCanvas& canvas, const uint8_t* pixels, uint32_t width, uint32_t height, const float matrix[6]);

// The canvas over the region showing `images` (indices into page.images), at the resolution of the
// finest of them: how many could be decoded and drawn (0: nothing to OCR).
uint32_t ComposeImages(const Document& document, const PageData& page, const std::vector<uint32_t>& images,
                       float x0, float y0, float x1, float y1, OcrCanvas& canvas);

// The response's lines (conf at least kOcrMinConf, inside the canvas) as glyphs of `page`, one per
// code point with its box from `chars` and the line's box, flagged kGlyphInvisible | kGlyphOcr: an
// OCR text layer with the size, baseline, weight, slant and pitch measured from the canvas
// (layout/ocr_style.h), and the canvas's rules added to page.rulings and page.shapes. Lines are
// added top to bottom, then left to right. Returns the non-space glyphs added.
uint32_t AddOcrGlyphs(const OcrCanvas& canvas, const OcrProtocol::Response& response, PageData& page);

// The longest alternate text taken from OCR, in bytes (cut at a word, with "…" after it).
constexpr size_t kOcrMaxAltBytes = 1000;

// The response's text (lines with conf at least kOcrMinConf, top to bottom, then left to right) as
// one line for an image's alternate text, appended to `out`: lines joined by a space, runs of white
// space and control characters as one space, at most kOcrMaxAltBytes. Returns the bytes appended.
size_t AppendOcrAltText(const OcrProtocol::Response& response, std::string& out);

} // namespace RayoPdf::Layout
