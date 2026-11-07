// SPDX-License-Identifier: Apache-2.0
// fmin and fmax inlined. std::fmin and std::fmax are calls into libm (through the PLT in the
// dynamic build) on every glyph corner and box union: 6 % of all instructions on odl in 0.0.7
// (docs/development/performance.md). These give glibc's x86-64 results exactly (s_fmin.S,
// s_fmax.S: minsd/maxsd, so of two equal values such as -0 and +0 the second; a NaN argument
// yields the other one; two NaNs their sum), so Linux output is unchanged. MinGW's fmin and
// fmax return the first of two equal values; the Windows build now follows glibc there.
#pragma once

#include "common/compiler.h"

#include <type_traits>

namespace RayoPdf {

template <typename T>
RAYOPDF_HOT_INLINE T Fmin(T a, T b) {
    static_assert(std::is_floating_point<T>::value, "Fmin takes float or double");
    if (RAYOPDF_UNLIKELY(a != a || b != b)) return b != b ? (a != a ? a + b : a) : b;
    return a < b ? a : b;
}

template <typename T>
RAYOPDF_HOT_INLINE T Fmax(T a, T b) {
    static_assert(std::is_floating_point<T>::value, "Fmax takes float or double");
    if (RAYOPDF_UNLIKELY(a != a || b != b)) return b != b ? (a != a ? a + b : a) : b;
    return a > b ? a : b;
}

} // namespace RayoPdf
