// SPDX-License-Identifier: Apache-2.0
// Feature vectors of the decisions small models may learn (roadmap M9: edge rows as page
// furniture), reported to a sink as the rules make those decisions; the developer mode
// `rayopdf --features` writes them as JSON Lines for eval/train/. The edge-row features also feed
// the edge-row model (layout/model.h) of every conversion that drops furniture.
#pragma once

#include <cstdint>
#include <string_view>

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

// Receives feature vectors. Edge rows: every row RemoveEdgeRows examines in the outer
// kWideGapBand (10 %) of a page's height, with whether the rules removed it; boxes in PDF points.
// Calls come from the converting thread, in page order.
struct FeatureSink {
    void* context = nullptr;
    void (*edgeRow)(void* context, uint32_t page, const float* features, float x0, float y0, float x1, float y1,
                    std::string_view text, bool removedByRules) = nullptr;
};

// Text features of a row: the share of digits among its non-space bytes, of capitals among its
// ASCII letters, and whether its last non-space byte is a digit.
void TextShares(std::string_view text, float& digitShare, float& capitalShare, bool& endsInDigit);

// Word features of a row: its words (runs of non-space bytes), the share of those starting with an
// ASCII letter that start with a lower-case one, and whether it ends a sentence (features 16-18).
void TextWords(std::string_view text, float& words, float& lowerShare, bool& endsSentence);

} // namespace RayoPdf::Layout
