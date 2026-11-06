// SPDX-License-Identifier: Apache-2.0
// Derived from RayoMD src/common/compiler.h at ef106ca (Apache-2.0, same author).
#pragma once

// Inlining hints for GCC and Clang; other compilers get plain `inline` or nothing.
// RAYOPDF_HOT_INLINE: always inlined (helpers on per-byte and per-glyph paths).
// RAYOPDF_NOINLINE: never inlined (rare paths kept out of hot callers).
// RAYOPDF_COLD: never inlined and optimised for size.
#if defined(__GNUC__) || defined(__clang__)
#define RAYOPDF_HOT_INLINE inline __attribute__((always_inline))
#define RAYOPDF_NOINLINE __attribute__((noinline))
#define RAYOPDF_COLD __attribute__((cold, noinline))
#define RAYOPDF_LIKELY(x) __builtin_expect(!!(x), 1)
#define RAYOPDF_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RAYOPDF_HOT_INLINE inline
#define RAYOPDF_NOINLINE
#define RAYOPDF_COLD
#define RAYOPDF_LIKELY(x) (x)
#define RAYOPDF_UNLIKELY(x) (x)
#endif
