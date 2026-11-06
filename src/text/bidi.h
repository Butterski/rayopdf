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

// One line in visual order, item by item (an item is a glyph; `classes[i]` is the class of its
// first code point), to logical order: `logical` receives the visual indices in reading order and
// `levels` (when not null) the resolved embedding level of each visual item; an item at an odd
// level reads right to left and is written with Mirror applied. `direction` -1 decides the
// paragraph direction from the line's strong characters (see the implementation for how visual
// input is read); 0 forces left-to-right, 1 right-to-left (a caller that knows the paragraph's
// alignment). Returns true when the paragraph direction used is right-to-left. A line with no R,
// AL or AN item comes back in visual order with every level 0 (whatever `direction` says).
bool VisualToLogical(const Class* classes, size_t count, std::vector<uint32_t>& logical,
                     std::vector<uint8_t>* levels = nullptr, int direction = -1);

} // namespace RayoPdf::Text::Bidi
