// SPDX-License-Identifier: Apache-2.0
// Page layout (algorithms §5, roadmap D4 step 5): running headers, footers and page numbers
// removed first (§5.2), by repetition across pages and, on any page, by position and style: the
// outermost row of the top or bottom 12 %, set apart from the text by more than its line spacing,
// that reaches both text edges, carries a page number at one end or is set below body size, and
// blocks of up to six such rows (a journal masthead, a name set off the text's edge over its URL);
// a folio row of separate pieces counts down to 16 %, and a page number or folio row reaching the
// text's edge already when set apart by its line spacing plus a quarter em; short rows marked as
// artifacts there go whatever their gap on a page of mostly unmarked text; sideways margin stamps
// go too. Then each page's lines are put in reading order by recursive XY cuts in the manner of
// XY-Cut++ (§5.1): lines at least 1.3× the median length that span a column gutter are masked as
// cross-layout separators that cut the page into horizontal bands; inside a band a vertical cut
// wins when the whitespace it follows separates columns of running text (Breuel's gutter
// criterion), with bridging lines set aside when no clean gutter exists; where no cut works,
// side-by-side text follows Breuel's partial order, otherwise rows are read top to bottom and each
// row left to right. Label columns ("[12]", line numbers) are read with the text on their
// baselines. Images join that order afterwards, after the paragraph they would split.
#pragma once

#include "layout/features.h"
#include "layout/page.h"

#include <vector>

namespace RayoPdf::Layout {

// Bands of the page height where running headers and footers live (§5.2: ~12 %).
constexpr float kRunningBand = 0.12f;

// Marks running headers, footers and page numbers on `pages` with kLineRemoved; how many lines:
// band lines whose repetition key recurs across the document, page numbers that count up from page
// to page (from four pages), and furniture at a single page's edges. The rules decide, and the
// edge-row model (layout/model.h) removes rows they keep in the outer tenth of a page. `sink`
// (layout/features.h) gets the edge rows examined there, with the rules' decisions.
int RemoveRunningLines(std::vector<PageData>& pages, const FeatureSink* sink = nullptr);

// Fills page.order with the lines not removed, in reading order: upright text first, then each
// other direction in its own frame.
void OrderLines(PageData& page);

} // namespace RayoPdf::Layout
