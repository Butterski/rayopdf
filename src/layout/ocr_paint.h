// SPDX-License-Identifier: Apache-2.0
// Content painted onto an OCR canvas (layout/ocr_layer.h) for glyph-shape recovery
// (layout/ocr_glyphs.h): the filled and stroked paths, image masks and images of a content stream
// (a page whose text is drawn as vector outlines, or a Type 3 glyph procedure), through Form
// XObjects. Text operators, shadings, clipping and transparency are not painted.
//
// Paths go through the rasterizer of raster/raster.h; a stroke is painted as one quadrilateral
// per flattened segment, `w` wide (at least one pixel). Images and image masks are decoded through
// the pixel API of image/image.h and drawn as layout/ocr_layer.h draws them (DrawGray).
//
// Bounded: kMaxPaintOperators operators in all, Form XObjects nested at most kMaxPaintFormDepth
// deep with a cycle check, q/Q at most kMaxPaintStates deep, paths of at most
// Raster::kMaxPathPoints points.
#pragma once

#include "layout/ocr_layer.h"
#include "pdf/document.h"
#include "raster/raster.h"

#include <cstdint>
#include <string_view>

namespace RayoPdf::Layout {

constexpr uint32_t kMaxPaintOperators = 1u << 22;
constexpr int kMaxPaintFormDepth = 8;
constexpr int kMaxPaintStates = 64;

enum class PaintMode : uint8_t {
    Page,   // colours as their luminance, painted over what is there
    Glyph,  // an uncoloured glyph: everything in black, only darkening
};

struct PaintStats {
    uint32_t fills = 0;      // paths filled (or stroked)
    uint32_t images = 0;     // images and image masks drawn
    uint32_t operators = 0;  // operators run
    bool truncated = false;  // a bound was hit
};

// Paints `content` (with `resources`) onto `canvas`, user space mapped to the canvas's region by
// `ctm` (the region's coordinates: canvas pixel (cx, cy) is (x0 + cx / scale, y1 - cy / scale)).
void PaintContent(const Document& document, std::string_view content, const PdfValue& resources,
                  const Raster::Matrix& ctm, PaintMode mode, OcrCanvas& canvas, Raster::Rasterizer& raster,
                  PaintStats& stats);

// The matrix from page `index`'s default user space to the upright page of page.h (CropBox
// origin, /Rotate applied), as the interpreter places a page.
Raster::Matrix PageMatrix(const Document& document, int index);

} // namespace RayoPdf::Layout
