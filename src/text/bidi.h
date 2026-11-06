// SPDX-License-Identifier: Apache-2.0
// The Unicode Bidirectional Algorithm (UAX #9) as layout needs it: a PDF shows right-to-left text
// in visual order (glyphs placed left to right on the page), and Markdown needs it in logical
// order. Bidi classes and mirrored pairs come from the Unicode Character Database (generated
// tables, Unicode License v3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RayoPdf::Text::Bidi {

// UAX #9 Table 4.
enum class Class : uint8_t {
    L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON, LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI,
};

Class ClassOf(uint32_t codePoint);

// The Bidi_Mirroring_Glyph of `codePoint` ("(" for ")"), or `codePoint` itself when it has none.
uint32_t Mirror(uint32_t codePoint);

// The level ResolveLevels gives the items rule X9 removes (LRE, RLE, LRO, RLO, PDF, BN).
constexpr uint8_t kRemovedLevel = 0xFF;

// UAX #9 on one paragraph in logical order that is also one line: P2-P3 (unless `direction`
// forces the paragraph level: 0 left-to-right, 1 right-to-left; -1 decides), the explicit levels
// and directions X1-X10 (embeddings, overrides and isolates, depth 125), W1-W7, the paired brackets
// of N0 (when `codePoints` is not null; `codePoints[i]` is the code point of item i), N1-N2, I1-I2
// and L1. `levels` receives each item's level, kRemovedLevel for those X9 removes. Returns the
// paragraph level. Conforms to the Unicode BidiCharacterTest.txt of the tables' version.
uint8_t ResolveLevels(const Class* classes, const uint32_t* codePoints, size_t count, int direction,
                      std::vector<uint8_t>& levels);

// L2: the indices of the items of `levels` (one line, in logical order) from left to right, items
// at kRemovedLevel left out.
void LogicalToVisual(const uint8_t* levels, size_t count, std::vector<uint32_t>& visual);

// One line in visual order, item by item (an item is a glyph; `classes[i]` is the class of its
// first code point), to logical order: `logical` receives the visual indices in reading order and
// `levels` (when not null) the resolved embedding level of each visual item; an item at an odd
// level reads right to left and is written with Mirror applied. `direction` -1 decides the
// paragraph direction from the line's strong characters (see the implementation for how visual
// input is read); 0 forces left-to-right, 1 right-to-left (a caller that knows the paragraph's
// alignment). Returns true when the paragraph direction used is right-to-left. A line with no R,
// AL, AN or explicit formatting item comes back in visual order with every level 0 (whatever
// `direction` says).
bool VisualToLogical(const Class* classes, size_t count, std::vector<uint32_t>& logical,
                     std::vector<uint8_t>* levels = nullptr, int direction = -1);
// The same with each item's (first) code point, so that paired brackets resolve together (N0).
bool VisualToLogical(const Class* classes, const uint32_t* codePoints, size_t count, std::vector<uint32_t>& logical,
                     std::vector<uint8_t>* levels = nullptr, int direction = -1);

} // namespace RayoPdf::Text::Bidi
