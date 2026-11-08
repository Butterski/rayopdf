// SPDX-License-Identifier: Apache-2.0
// Text detection post-processing of the OCR add-on (DB, "Real-time Scene Text Detection with
// Differentiable Binarization", as PaddleOCR's DBPostProcess does it): the network's probability
// map is binarised at `thresh`; every 8-connected component (from runs of a row, joined row to
// row by union-find) gives the minimum-area rectangle of its convex hull; rectangles thinner than
// 3 map pixels or whose mean probability inside is below `boxThresh` are dropped; the rest grow by
// d = area * unclipRatio / perimeter on every side (the minimum-area rectangle of PaddleOCR's round
// polygon offset), are dropped when still thinner than 5, and are scaled to page pixels.
//
// Bounded: at most `maxCandidates` components (in raster order of their first pixel) are examined;
// the probability of each rectangle is averaged over its pixels until the map has been read
// kScoreBudget times in all, and over the component's own pixels after that. Work and memory are
// linear in the map size apart from that budget. Deterministic.
#pragma once

#include "ocr/raster.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Ocr {

struct DetectParams {
    float thresh = 0.2f;
    float boxThresh = 0.45f;
    float unclipRatio = 1.4f;
    uint32_t maxCandidates = 3000;
};

// Pixel reads for rectangle scores, as a multiple of the map's pixel count.
constexpr uint32_t kScoreBudget = 8;

struct TextBox {
    RotatedRect rect;  // page pixels; ux > 0 or (ux == 0 and uy > 0): the axis nearer horizontal
    float score = 0;
};

// Scratch buffers of DetectBoxes, kept by the caller.
struct DetectScratch {
    struct Run {
        uint32_t y, x0, x1;  // x1 inclusive
    };
    std::vector<Run> runs;
    std::vector<uint32_t> parent, component, order, start;
    std::vector<uint32_t> rowStart;
    std::vector<double> probSum;
    std::vector<uint32_t> pixels;
    std::vector<float> points, hull;
};

// The boxes of a mapWidth x mapHeight probability map, scaled by (scaleX, scaleY) page pixels per
// map pixel, appended to `boxes` (cleared first).
void DetectBoxes(const float* prob, uint32_t mapWidth, uint32_t mapHeight, float scaleX, float scaleY,
                 const DetectParams& params, DetectScratch& scratch, std::vector<TextBox>& boxes);

// Axis-aligned bounds of a rectangle, in whole pixels [x0, x1) x [y0, y1), clipped to the image.
struct PixelBox {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
PixelBox Bounds(const RotatedRect& rect, uint32_t width, uint32_t height);

// Puts `boxes` into reading order of lines: rows top to bottom (a box joins a row when it overlaps
// the row's first box vertically by at least half the smaller height), left to right in a row.
// Columns are not separated: the core orders the lines it receives itself (a recursive XY cut was
// measured worse on the scanned development pages: docs/development/ocr-engine.md).
void SortIntoLines(std::vector<TextBox>& boxes);

} // namespace RayoPdf::Ocr
