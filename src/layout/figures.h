// SPDX-License-Identifier: Apache-2.0
// Figure regions: where a page draws (a plot, a diagram, a picture) rather than sets text. The text
// inside one is the figure's own (tick labels, legends, the letters of a diagram): no table, and no
// part of the running text.
//
// Evidence is what is painted, never what a box encloses: a region is a cluster of drawing strokes
// (paths that are no axis-aligned rectangle or rule: curves, polygons, slanted lines, and lines as
// short as a tick), with marks enough that hold no text (ticks, markers, arrow heads, a curve of
// many segments), or a placed image. A frame or a shading around text (rectangles, rules, a box
// with rounded corners that holds its text) makes no region, and neither does an area mostly
// covered by text (text drawn as paths under a text layer).
//
// One page at a time, no state between pages; bounded by the page's shapes (kMaxShapesPerPage)
// and a fixed grid.
#pragma once

#include "layout/page.h"

#include <vector>

namespace RayoPdf::Layout {

constexpr size_t kMaxFigureRegions = 64;  // per page; beyond it none is reported (fail closed)

// A region's box on the upright page.
struct FigureRegion {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The figure regions of `page` (its shapes, images and lines as they are when called).
void FigureRegions(const PageData& page, std::vector<FigureRegion>& regions);

} // namespace RayoPdf::Layout
