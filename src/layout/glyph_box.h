// SPDX-License-Identifier: Apache-2.0
// The box of one glyph of a horizontal run (writing mode 0), the hot part of glyph placement
// (Interpreter::PlaceGlyphs in layout/interpreter.cpp): the four corners [x y] × Trm of the glyph
// space box (x from the run's left edge to the glyph's advance width, y from descent to ascent),
// their extremes on the upright page turned into the frame of the baseline direction, written as
// the glyph's frame box, and whether the box's centre lies outside the clip.
//
// The x and y of a corner are independent sums, so SSE2 computes both in one register: the same
// IEEE additions, multiplications and comparisons in the same order, so the floats have the bits of
// the scalar reference (BoxScalar), which other targets build and the core tests compare against.
#pragma once

#include "common/compiler.h"
#include "layout/page.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if RAYOPDF_SSE2
#include <emmintrin.h>
#define RAYOPDF_GLYPH_BOX_SSE2 1
#endif

namespace RayoPdf::Layout {

// What every glyph of a run shares. Lane 0 is x, lane 1 is y. With GlyphRun's terms (xa: x·a,
// xb: x·b of the box's left and right edges, yc: y·c, yd: y·d of its bottom and top). Aligned to
// 16 bytes, so that BoxSse2's arithmetic takes each pair from memory without a load of its own.
struct alignas(16) BoxTerms {
    double r[2];            // Trm's a and b: the glyph's width times these are its right edge's terms
    double s0[2];           // xa0 + yc0, xb0 + yd0: the bottom-left corner less Trm's translation
    double s1[2];           // xa0 + yc1, xb0 + yd1: the top-left corner less Trm's translation
    double y0[2];           // yc0, yd0: the bottom edge's terms
    double y1[2];           // yc1, yd1: the top edge's terms
    double clipLo[2];       // the clip less one unit, x and y
    double clipHi[2];       // the clip plus one unit
    bool clipEmpty;
};

// The corners' extremes, as Fmin and Fmax chained from the first corner pick them when no corner is
// NaN: plain comparisons, of equal values the later.
RAYOPDF_HOT_INLINE void BoxExtremes(double c0, double c1, double c2, double c3, double& lo, double& hi) {
    lo = c0, hi = c0;
    lo = lo < c1 ? lo : c1, hi = hi > c1 ? hi : c1;
    lo = lo < c2 ? lo : c2, hi = hi > c2 ? hi : c2;
    lo = lo < c3 ? lo : c3, hi = hi > c3 ? hi : c3;
}

// Writes the frame box g.u0..g.vt from the extremes (bx0, by0)-(bx1, by1) and the baseline direction
// `dir`: the frame's coordinates are the device ones permuted and negated (interpreter.cpp ToFrame),
// so their extremes are the device extremes, negated where the axis is.
RAYOPDF_HOT_INLINE void WriteBox(double bx0, double by0, double bx1, double by1, uint8_t dir, Glyph& g) {
    double fu0, fu1, fv0, fv1;
    switch (dir) {
    case 1: fu0 = by0, fu1 = by1, fv0 = -bx1, fv1 = -bx0; break;
    case 2: fu0 = -bx1, fu1 = -bx0, fv0 = -by1, fv1 = -by0; break;
    case 3: fu0 = -by1, fu1 = -by0, fv0 = bx0, fv1 = bx1; break;
    default: fu0 = bx0, fu1 = bx1, fv0 = by0, fv1 = by1; break;
    }
    g.u0 = float(fu0), g.u1 = float(fu1), g.vb = float(fv0), g.vt = float(fv1);
}

// The scalar reference: the glyph of advance width `width` at Trm's translation (e, f). Every term,
// `width`, `e` and `f` must be finite (no corner is then NaN). Returns whether it is clipped away.
inline bool BoxScalar(const BoxTerms& t, double width, double e, double f, uint8_t dir, Glyph& g) {
    const double xa1 = width * t.r[0], xb1 = width * t.r[1];
    double bx0, bx1, by0, by1;
    BoxExtremes(t.s0[0] + e, xa1 + t.y0[0] + e, t.s1[0] + e, xa1 + t.y1[0] + e, bx0, bx1);
    BoxExtremes(t.s0[1] + f, xb1 + t.y0[1] + f, t.s1[1] + f, xb1 + t.y1[1] + f, by0, by1);
    WriteBox(bx0, by0, bx1, by1, dir, g);
    const double mx = (bx0 + bx1) / 2, my = (by0 + by1) / 2;
    return mx < t.clipLo[0] || mx > t.clipHi[0] || my < t.clipLo[1] || my > t.clipHi[1] || t.clipEmpty;
}

#if defined(RAYOPDF_GLYPH_BOX_SSE2)
static_assert(offsetof(Glyph, u1) == offsetof(Glyph, u0) + 4 && offsetof(Glyph, vb) == offsetof(Glyph, u0) + 8 &&
                  offsetof(Glyph, vt) == offsetof(Glyph, u0) + 12,
              "Glyph's u0, u1, vb, vt are stored as four consecutive floats");

// BoxScalar with x and y in the two lanes of one register: _mm_min_pd(a, b) is a < b ? a : b and
// _mm_max_pd(a, b) is a > b ? a : b, lane by lane; float conversion rounds as float(double) does,
// and negating a float flips its sign as negating the double did.
inline bool BoxSse2(const BoxTerms& t, double width, double e, double f, uint8_t dir, Glyph& g) {
    const __m128d ef = _mm_set_pd(f, e);
    const __m128d edge = _mm_mul_pd(_mm_set1_pd(width), _mm_load_pd(t.r));
    const __m128d c0 = _mm_add_pd(_mm_load_pd(t.s0), ef);
    const __m128d c1 = _mm_add_pd(_mm_add_pd(edge, _mm_load_pd(t.y0)), ef);
    const __m128d c2 = _mm_add_pd(_mm_load_pd(t.s1), ef);
    const __m128d c3 = _mm_add_pd(_mm_add_pd(edge, _mm_load_pd(t.y1)), ef);
    const __m128d lo = _mm_min_pd(_mm_min_pd(_mm_min_pd(c0, c1), c2), c3);
    const __m128d hi = _mm_max_pd(_mm_max_pd(_mm_max_pd(c0, c1), c2), c3);
    // The frame's (u0 u1 vb vt): for direction 0 the page box (x0 x1 y0 y1).
    const __m128 box = _mm_movelh_ps(_mm_cvtpd_ps(lo), _mm_cvtpd_ps(hi));  // (x0 y0 x1 y1)
    const __m128 negative = _mm_set1_ps(-0.0f);
    __m128 frame;
    if (RAYOPDF_LIKELY(dir == 0)) {
        frame = _mm_shuffle_ps(box, box, _MM_SHUFFLE(3, 1, 2, 0));
    } else {
        switch (dir) {
        case 1: frame = _mm_xor_ps(_mm_shuffle_ps(box, box, _MM_SHUFFLE(0, 2, 3, 1)), _mm_set_ps(-0.0f, -0.0f, 0.0f, 0.0f)); break;
        case 2: frame = _mm_xor_ps(_mm_shuffle_ps(box, box, _MM_SHUFFLE(1, 3, 0, 2)), negative); break;
        case 3: frame = _mm_xor_ps(_mm_shuffle_ps(box, box, _MM_SHUFFLE(2, 0, 1, 3)), _mm_set_ps(0.0f, 0.0f, -0.0f, -0.0f)); break;
        default: frame = _mm_shuffle_ps(box, box, _MM_SHUFFLE(3, 1, 2, 0)); break;
        }
    }
    _mm_storeu_ps(&g.u0, frame);
    // The centre against the clip: (lo + hi) / 2 (halving is exact as a product by 0.5).
    const __m128d mid = _mm_mul_pd(_mm_add_pd(lo, hi), _mm_set1_pd(0.5));
    const __m128d out = _mm_or_pd(_mm_cmplt_pd(mid, _mm_load_pd(t.clipLo)), _mm_cmpgt_pd(mid, _mm_load_pd(t.clipHi)));
    return _mm_movemask_pd(out) != 0 || t.clipEmpty;
}
#endif

// The box of PlaceGlyphs' horizontal runs: BoxSse2 where SSE2 is, else the scalar reference.
RAYOPDF_HOT_INLINE bool HorizontalBox(const BoxTerms& t, double width, double e, double f, uint8_t dir, Glyph& g) {
#if defined(RAYOPDF_GLYPH_BOX_SSE2)
    return BoxSse2(t, width, e, f, dir, g);
#else
    return BoxScalar(t, width, e, f, dir, g);
#endif
}

} // namespace RayoPdf::Layout
