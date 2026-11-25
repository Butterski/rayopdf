// SPDX-License-Identifier: Apache-2.0
// The bounds of a path's points (Interpreter::PathPoints in layout/interpreter.cpp): each point
// [x y] × CTM whose two coordinates are finite grows the box by plain comparisons, the values Fmin
// and Fmax give without a NaN.
//
// The x and y of a point are independent sums, so SSE2 computes both in one register: the same
// IEEE multiplications, additions and comparisons in the same order, so the box has the bits of
// the scalar reference (PathScalar), which other targets build and the core tests compare against.
#pragma once

#include "common/compiler.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#define RAYOPDF_PATH_BOX_SSE2 1
#endif

namespace RayoPdf::Layout {

// The scalar reference. `m` is the CTM (a b c d e f), `xy` the `count` points, `box` the bounds
// (x0 y0 x1 y1), grown in place. Returns whether any point was finite.
inline bool PathScalar(const double* m, const double* xy, int count, double* box) {
    bool any = false;
    for (int i = 0; i < count; ++i) {
        const double x = xy[2 * i], y = xy[2 * i + 1];
        const double dx = x * m[0] + y * m[2] + m[4];
        const double dy = x * m[1] + y * m[3] + m[5];
        // Both finite: x - x is 0 for a finite x and NaN for an infinity or NaN.
        if (!((dx - dx) + (dy - dy) == 0)) continue;
        box[0] = box[0] < dx ? box[0] : dx;
        box[1] = box[1] < dy ? box[1] : dy;
        box[2] = box[2] > dx ? box[2] : dx;
        box[3] = box[3] > dy ? box[3] : dy;
        any = true;
    }
    return any;
}

#if defined(RAYOPDF_PATH_BOX_SSE2)
// PathScalar with x and y in the two lanes of one register: _mm_min_pd(a, b) is a < b ? a : b and
// _mm_max_pd(a, b) is a > b ? a : b, lane by lane; d - d is 0 in both lanes exactly when the sum
// of the two differences is 0.
RAYOPDF_HOT_INLINE bool PathSse2(const double* m, const double* xy, int count, double* box) {
    const __m128d ab = _mm_loadu_pd(m), cd = _mm_loadu_pd(m + 2), ef = _mm_loadu_pd(m + 4);
    __m128d lo = _mm_loadu_pd(box), hi = _mm_loadu_pd(box + 2);
    bool any = false;
    for (int i = 0; i < count; ++i) {
        const __m128d d = _mm_add_pd(_mm_add_pd(_mm_mul_pd(_mm_set1_pd(xy[2 * i]), ab), _mm_mul_pd(_mm_set1_pd(xy[2 * i + 1]), cd)), ef);
        if (_mm_movemask_pd(_mm_cmpeq_pd(_mm_sub_pd(d, d), _mm_setzero_pd())) != 3) continue;
        lo = _mm_min_pd(lo, d);
        hi = _mm_max_pd(hi, d);
        any = true;
    }
    _mm_storeu_pd(box, lo);
    _mm_storeu_pd(box + 2, hi);
    return any;
}
#endif

// The bounds of PathPoints: PathSse2 where SSE2 is, else the scalar reference.
RAYOPDF_HOT_INLINE bool PathBounds(const double* m, const double* xy, int count, double* box) {
#if defined(RAYOPDF_PATH_BOX_SSE2)
    return PathSse2(m, xy, count, box);
#else
    return PathScalar(m, xy, count, box);
#endif
}

} // namespace RayoPdf::Layout
