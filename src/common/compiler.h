// SPDX-License-Identifier: Apache-2.0
// Derived from RayoMD src/common/compiler.h at ef106ca (Apache-2.0, same author).
#pragma once

// Inlining hints for GCC and Clang; other compilers get plain `inline` or nothing.
// RAYOPDF_HOT_INLINE: always inlined (helpers on per-byte and per-glyph paths).
// RAYOPDF_NOINLINE: never inlined (rare paths kept out of hot callers).
// RAYOPDF_COLD: never inlined and optimised for size.
// RAYOPDF_HOT_LAMBDA: a lambda always inlined (after its parameter list).
#if defined(__GNUC__) || defined(__clang__)
#define RAYOPDF_HOT_INLINE inline __attribute__((always_inline))
#define RAYOPDF_NOINLINE __attribute__((noinline))
#define RAYOPDF_COLD __attribute__((cold, noinline))
#define RAYOPDF_HOT_LAMBDA __attribute__((always_inline))
#define RAYOPDF_LIKELY(x) __builtin_expect(!!(x), 1)
#define RAYOPDF_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RAYOPDF_HOT_INLINE inline
#define RAYOPDF_NOINLINE
#define RAYOPDF_COLD
#define RAYOPDF_HOT_LAMBDA
#define RAYOPDF_LIKELY(x) (x)
#define RAYOPDF_UNLIKELY(x) (x)
#endif

// RAYOPDF_SSE2: 1 where the SSE2 kernels are built (x86-64's baseline), 0 for the scalar paths,
// which each kernel keeps as its reference; RAYOPDF_SCALAR (CMake option RAYOPDF_SCALAR,
// tools/dev.py config scalar) builds those everywhere. Test it with `#if RAYOPDF_SSE2`, after
// including this header.
#if !defined(RAYOPDF_SCALAR) && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64))
#define RAYOPDF_SSE2 1
#else
#define RAYOPDF_SSE2 0
#endif
