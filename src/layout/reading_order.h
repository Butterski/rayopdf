// SPDX-License-Identifier: Apache-2.0
// Page layout (algorithms §5, roadmap D4 step 5): running headers, footers and page numbers
// removed across pages first (§5.2), then each page's lines put in reading order by recursive
// XY cuts in the manner of XY-Cut++ (§5.1): lines that span a column gutter are masked as
// cross-layout separators that cut the page into horizontal bands, and inside a band a vertical
// cut wins when the whitespace it follows separates columns of running text (Breuel's gutter
// criterion), otherwise rows are read top to bottom and each row left to right.
#pragma once

#include "layout/page.h"

#include <vector>

namespace RayoPdf::Layout {

// Bands of the page height where running headers and footers live (§5.2: ~12 %).
constexpr float kRunningBand = 0.12f;

// Marks running headers, footers and page numbers on `pages` with kLineRemoved; how many lines.
int RemoveRunningLines(std::vector<PageData>& pages);

// Fills page.order with the lines not removed, in reading order: upright text first, then each
// other direction in its own frame.
void OrderLines(PageData& page);

} // namespace RayoPdf::Layout
