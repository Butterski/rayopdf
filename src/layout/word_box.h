// SPDX-License-Identifier: Apache-2.0
// The box of a word in the frame of its baseline direction (layout/tables.cpp's words): from the
// glyph at a line's position k to the next one that starts a word (kGlyphSpaceBefore) or the
// line's end, the extremes of their u0, u1, vb and vt taken as plain comparisons take them from
// ±infinity (x0 = x0 < u0 ? x0 : u0, x1 = x1 > u1 ? x1 : u1, and so for vb and vt), and whether any
// of those floats is NaN.
//
// SSE2 takes the four floats of a glyph in one register: _mm_min_ps(a, b) is a < b ? a : b and
// _mm_max_ps(a, b) is a > b ? a : b, lane by lane, so the floats have the bits of the scalar
// reference (WordBoxScalar), which other targets build and the core tests compare against.
#pragma once

#include "common/compiler.h"
#include "layout/page.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#if RAYOPDF_SSE2
#include <xmmintrin.h>
#define RAYOPDF_WORD_BOX_SSE2 1
#endif

namespace RayoPdf::Layout {

struct WordBox {
    float u0, u1, vb, vt;
    bool nan;  // some u0, u1, vb or vt of the word's glyphs is NaN
};

// The word of the line's glyphs order[k, count) (k < count) from k; returns the position past it.
inline uint32_t WordBoxScalar(const Glyph* glyphs, const uint32_t* order, uint32_t k, uint32_t count, WordBox& box) {
    float x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    bool nan = false;
    const Glyph* g = &glyphs[order[k]];
    for (;;) {
        x0 = x0 < g->u0 ? x0 : g->u0, x1 = x1 > g->u1 ? x1 : g->u1;
        y0 = y0 < g->vb ? y0 : g->vb, y1 = y1 > g->vt ? y1 : g->vt;
        nan = nan || g->u0 != g->u0 || g->u1 != g->u1 || g->vb != g->vb || g->vt != g->vt;
        if (++k == count) break;
        g = &glyphs[order[k]];
        if (g->flags & kGlyphSpaceBefore) break;
    }
    box = WordBox{x0, x1, y0, y1, nan};
    return k;
}

#if defined(RAYOPDF_WORD_BOX_SSE2)
static_assert(offsetof(Glyph, u1) == offsetof(Glyph, u0) + 4 && offsetof(Glyph, vb) == offsetof(Glyph, u0) + 8 &&
                  offsetof(Glyph, vt) == offsetof(Glyph, u0) + 12,
              "Glyph's u0, u1, vb, vt are stored as four consecutive floats");

inline uint32_t WordBoxSse2(const Glyph* glyphs, const uint32_t* order, uint32_t k, uint32_t count, WordBox& box) {
    __m128 lo = _mm_set1_ps(INFINITY), hi = _mm_set1_ps(-INFINITY), nan = _mm_setzero_ps();
    const Glyph* g = &glyphs[order[k]];
    for (;;) {
        const __m128 v = _mm_loadu_ps(&g->u0);
        lo = _mm_min_ps(lo, v);
        hi = _mm_max_ps(hi, v);
        nan = _mm_or_ps(nan, _mm_cmpunord_ps(v, v));
        if (++k == count) break;
        g = &glyphs[order[k]];
        if (g->flags & kGlyphSpaceBefore) break;
    }
    float l[4], h[4];
    _mm_storeu_ps(l, lo);
    _mm_storeu_ps(h, hi);
    box = WordBox{l[0], h[1], l[2], h[3], _mm_movemask_ps(nan) != 0};
    return k;
}
#endif

RAYOPDF_HOT_INLINE uint32_t WordBoxOf(const Glyph* glyphs, const uint32_t* order, uint32_t k, uint32_t count, WordBox& box) {
#if defined(RAYOPDF_WORD_BOX_SSE2)
    return WordBoxSse2(glyphs, order, k, count, box);
#else
    return WordBoxScalar(glyphs, order, k, count, box);
#endif
}

} // namespace RayoPdf::Layout
