// SPDX-License-Identifier: Apache-2.0
// The generated UAX #9 tables (bidi_data.cpp, written by tools/gen_bidi_tables.py from the Unicode
// Character Database; Unicode License v3, see NOTICE).
#pragma once

#include <cstdint>

namespace RayoPdf::Text::Bidi {

// Bidi_Class ranges: entry i is start << 5 | class (bidi.h Class order) and covers the code points
// from its start up to the next entry's start; entry 0 starts at U+0000. Sorted by start.
extern const uint16_t kClassRangeCount;
extern const uint32_t kClassRanges[];
// The classes of U+0000..U+007F (the common case, read without a search).
extern const uint8_t kAsciiClasses[128];
// Bidi_Mirroring_Glyph: (code point, mirror) pairs sorted by code point.
extern const uint16_t kMirrorCount;
extern const uint32_t kMirrorPairs[][2];

} // namespace RayoPdf::Text::Bidi
