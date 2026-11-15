// SPDX-License-Identifier: Apache-2.0
// Feature vectors of the decisions small models may learn (plan M9: edge rows as page furniture,
// borderless table column gaps), reported to a sink as the rules make those decisions; the
// developer mode `rayopdf --features` writes them as JSON Lines for eval/train/. The edge-row
// features also feed the edge-row model (layout/model.h) of every conversion that drops furniture;
// table-gap features are computed for a sink only.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace RayoPdf::Layout {

// Edge-row features, in this order:
//   0 distance of the row's centre to the nearer page edge / page height
//   1 top (0) or bottom (1)
//   2 gap to the nearest body line towards the page centre / body size
//   3 row size / body size
//   4 characters
//   5 digit share of its non-space characters
//   6 all bold (0 or 1)
//   7 all italic (0 or 1)
//   8 page-number key (IsPageNumberKey of its running key: 0 or 1)
//   9 artifact glyph share (characters of lines in /Artifact marked content)
//  10 horizontal centre offset from the page centre / page width
//  11 row width / text width
//  12 upright lines on the page
//  13 pages holding the row's running key (same band) / (pages - 1); 0 for one page
//  14 capital share of its letters
//  15 ends in a digit (0 or 1)
//  16 ends a sentence: its last word, of two letters or digits at least one of them a letter, ends
//     in '.', '?' or '!' (0 or 1)
//  17 share of its words starting with a letter that start with a lower-case one
//  18 words
//  19 size of the line inside it, across the gap / body size (0 when none)
//  20 the row's pitch to that line / the text's line pitch ((gap + body) / (spacing + body))
//  21 rows outside it on its side (0: the outermost)
//  22 offset of its left edge from the text's left edge / text width
constexpr int kEdgeRowFeatures = 23;

// Table-gap features, in this order:
//   0 gap width / em
//   1 share of the run's rows with ink in the gap
//   2 MAD of the right edges of the words left of the gap / em
//   3 MAD of the left edges of the words right of the gap / em
//   4 numeric share of the cells left of the gap
//   5 numeric share of the cells right of the gap
//   6 fill of the two adjoining columns (filled cells / 2 rows)
//   7 the gap's rank among the run's gaps by width (0: the widest)
//   8 rows in the run
//   9 run size / body size
//  10 second pass (closer gaps: 0 or 1)
//  11 chosen by the rules (the run became a table: 0 or 1)
constexpr int kTableGapFeatures = 12;

// Receives feature vectors. Edge rows: every row RemoveEdgeRows examines in the outer
// kWideGapBand (10 %) of a page's height, with whether the rules removed it; boxes in PDF points.
// Table gaps: every column gap of a candidate run of Detector::Borderless (three rows and two
// columns at least), with whether the rules made the run a table; x0, x1 bound the gap. Table gaps
// are reported by conversions on one page thread only (pages detected again on several threads
// would report twice). Calls come from the converting thread, in page order.
struct FeatureSink {
    void* context = nullptr;
    void (*edgeRow)(void* context, uint32_t page, const float* features, float x0, float y0, float x1, float y1,
                    std::string_view text, bool removedByRules) = nullptr;
    void (*tableGap)(void* context, uint32_t page, const float* features, float x0, float x1,
                     bool chosenByRules) = nullptr;
};

// Text features of a row: the share of digits among its non-space bytes, of capitals among its
// ASCII letters, and whether its last non-space byte is a digit.
void TextShares(std::string_view text, float& digitShare, float& capitalShare, bool& endsInDigit);

// Word features of a row: its words (runs of non-space bytes), the share of those starting with an
// ASCII letter that start with a lower-case one, and whether it ends a sentence (features 16-18).
void TextWords(std::string_view text, float& words, float& lowerShare, bool& endsSentence);

// The median absolute deviation from the median of `values` (reordered); 0 for none.
float MedianDeviation(std::vector<float>& values);

} // namespace RayoPdf::Layout
