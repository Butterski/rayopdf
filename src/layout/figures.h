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
// One page at a time; bounded by the page's shapes (kMaxShapesPerPage) and a fixed grid. What the
// shapes and images make (PageData::figureStrokes) is found once a page and kept; the text is
// weighed at every search.
#pragma once

#include "layout/page.h"

#include <vector>

namespace RayoPdf::Layout {

constexpr size_t kMaxFigureRegions = 64;  // per page; beyond it none is reported (fail closed)

// A region's box on the upright page.
struct FigureRegion {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The figure regions of `page` (its shapes, images and lines as they are when called; its
// figureStrokes found on the first call).
void FigureRegions(PageData& page, std::vector<FigureRegion>& regions);

// Flags kLineFigure on the upright lines of text standing in a figure region or within two ems of
// it: a figure's own text (tick labels, legends, axis titles, the labels of a diagram).
void MarkFigureText(PageData& page);

// The axes of the page's charts, each the box of its tick marks on the upright page: three ticks
// at least, short thin strokes across the axis that start on one line, end free (no stroke runs
// on from that end or crosses it, as the pieces of a table's rules do), stand evenly spaced, and
// lie outside the lines of text (an underscore drawn as a rule). None when the page has more
// candidate ticks or shapes than the search may look at (fail closed).
void ChartAxes(const PageData& page, std::vector<FigureRegion>& axes);

// Frees the calling thread's storage the three above keep for their next page (a conversion in a
// process of its own, once its pages' tables and order are done: what follows reuses the memory).
void ReleaseFigureScratch();

} // namespace RayoPdf::Layout
