// SPDX-License-Identifier: Apache-2.0
// Derived from zlib-ng arch/x86/crc32_pclmulqdq_tpl.h, crc32_pclmulqdq.c, adler32_avx2.c and
// adler32_avx2_p.h at 6727d88 (https://github.com/zlib-ng/zlib-ng, zlib licence: see
// third_party/licenses/zlib-ng.txt). Altered: C++ for RayoPDF, the copying variants, the Chorba
// and VPCLMULQDQ paths and the short-input fallbacks removed (the callers, Png::Crc32 and
// Deflate::Adler32, take the unaligned head and the short inputs and tails themselves).
//
// The original notices:
//
// crc32_pclmulqdq_tpl.h -- Compute the CRC32 using a parallelized folding approach with the
// PCLMULQDQ and VPCMULQDQ instructions. A white paper describing this algorithm can be found at:
// doc/crc-pclmulqdq.pdf
// Copyright (C) 2020 Wangyang Guo (wangyang.guo@intel.com) (VPCLMULQDQ support)
// Copyright (C) 2013 Intel Corporation. All rights reserved.
// Copyright (C) 2016 Marian Beermann (support for initial value)
// Authors:
//     Wajdi Feghali   <wajdi.k.feghali@intel.com>
//     Jim Guilford    <james.guilford@intel.com>
//     Vinodh Gopal    <vinodh.gopal@intel.com>
//     Erdinc Ozturk   <erdinc.ozturk@intel.com>
//     Jim Kukunas     <james.t.kukunas@linux.intel.com>
// For conditions of distribution and use, see copyright notice in zlib.h
//
// adler32_avx2.c -- compute the Adler-32 checksum of a data stream
// Copyright (C) 1995-2011 Mark Adler
// Copyright (C) 2022 Adam Stylinski
// Authors:
//   Brian Bockelman <bockelman@gmail.com>
//   Adam Stylinski <kungfujesus06@gmail.com>
// For conditions of distribution and use, see copyright notice in zlib.h
#include "image/checksum_x86.h"

#if RAYOPDF_CHECKSUM_X86

#include <immintrin.h>

#include <algorithm>
#include <cstring>

namespace RayoPdf::Image::Checksum {
namespace {

#define RAYOPDF_CRC_TARGET __attribute__((target("pclmul,sse4.2")))

// The four 128-bit lanes of the fold, each moved on by `n` x 16 bytes.
RAYOPDF_CRC_TARGET inline void FoldState1(__m128i& crc0, __m128i& crc1, __m128i& crc2, __m128i& crc3, __m128i fold4) {
    const __m128i low = _mm_clmulepi64_si128(crc0, fold4, 0x01);
    const __m128i high = _mm_clmulepi64_si128(crc0, fold4, 0x10);
    crc0 = crc1;
    crc1 = crc2;
    crc2 = crc3;
    crc3 = _mm_xor_si128(low, high);
}

RAYOPDF_CRC_TARGET inline void FoldState2(__m128i& crc0, __m128i& crc1, __m128i& crc2, __m128i& crc3, __m128i fold4) {
    const __m128i low0 = _mm_clmulepi64_si128(crc0, fold4, 0x01);
    const __m128i high0 = _mm_clmulepi64_si128(crc0, fold4, 0x10);
    const __m128i low1 = _mm_clmulepi64_si128(crc1, fold4, 0x01);
    const __m128i high1 = _mm_clmulepi64_si128(crc1, fold4, 0x10);
    crc0 = crc2;
    crc1 = crc3;
    crc2 = _mm_xor_si128(low0, high0);
    crc3 = _mm_xor_si128(low1, high1);
}

RAYOPDF_CRC_TARGET inline void FoldState3(__m128i& crc0, __m128i& crc1, __m128i& crc2, __m128i& crc3, __m128i fold4) {
    const __m128i low0 = _mm_clmulepi64_si128(crc0, fold4, 0x01);
    const __m128i high0 = _mm_clmulepi64_si128(crc0, fold4, 0x10);
    const __m128i low1 = _mm_clmulepi64_si128(crc1, fold4, 0x01);
    const __m128i high1 = _mm_clmulepi64_si128(crc1, fold4, 0x10);
    const __m128i low2 = _mm_clmulepi64_si128(crc2, fold4, 0x01);
    const __m128i high2 = _mm_clmulepi64_si128(crc2, fold4, 0x10);
    crc0 = crc3;
    crc1 = _mm_xor_si128(low0, high0);
    crc2 = _mm_xor_si128(low1, high1);
    crc3 = _mm_xor_si128(low2, high2);
}

RAYOPDF_CRC_TARGET inline void FoldState4(__m128i& crc0, __m128i& crc1, __m128i& crc2, __m128i& crc3, __m128i fold4) {
    const __m128i low0 = _mm_clmulepi64_si128(crc0, fold4, 0x01);
    const __m128i high0 = _mm_clmulepi64_si128(crc0, fold4, 0x10);
    const __m128i low1 = _mm_clmulepi64_si128(crc1, fold4, 0x01);
    const __m128i high1 = _mm_clmulepi64_si128(crc1, fold4, 0x10);
    const __m128i low2 = _mm_clmulepi64_si128(crc2, fold4, 0x01);
    const __m128i high2 = _mm_clmulepi64_si128(crc2, fold4, 0x10);
    const __m128i low3 = _mm_clmulepi64_si128(crc3, fold4, 0x01);
    const __m128i high3 = _mm_clmulepi64_si128(crc3, fold4, 0x10);
    crc0 = _mm_xor_si128(low0, high0);
    crc1 = _mm_xor_si128(low1, high1);
    crc2 = _mm_xor_si128(low2, high2);
    crc3 = _mm_xor_si128(low3, high3);
}

RAYOPDF_CRC_TARGET inline __m128i Xor3(__m128i a, __m128i b, __m128i c) { return _mm_xor_si128(_mm_xor_si128(a, b), c); }

#define RAYOPDF_ADLER_TARGET __attribute__((target("avx2")))

// 32-bit horizontal sums, adapted from Agner Fog's vector library: of all eight lanes, and of the
// even lanes (the odd ones of a _mm256_sad_epu8 sum are zero).
RAYOPDF_ADLER_TARGET inline uint32_t HorizontalSum(__m256i x) {
    const __m128i sum1 = _mm_add_epi32(_mm256_extracti128_si256(x, 1), _mm256_castsi256_si128(x));
    const __m128i sum2 = _mm_add_epi32(sum1, _mm_unpackhi_epi64(sum1, sum1));
    const __m128i sum3 = _mm_add_epi32(sum2, _mm_shuffle_epi32(sum2, 1));
    return static_cast<uint32_t>(_mm_cvtsi128_si32(sum3));
}

RAYOPDF_ADLER_TARGET inline uint32_t EvenLaneSum(__m256i x) {
    const __m256i evens = _mm256_permutevar8x32_epi32(x, _mm256_setr_epi32(0, 2, 4, 6, 1, 1, 1, 1));
    const __m128i low = _mm256_castsi256_si128(evens);
    const __m128i sum2 = _mm_add_epi32(low, _mm_unpackhi_epi64(low, low));
    const __m128i sum3 = _mm_add_epi32(sum2, _mm_shuffle_epi32(sum2, 1));
    return static_cast<uint32_t>(_mm_cvtsi128_si32(sum3));
}

constexpr uint32_t kAdlerBase = 65521;
constexpr size_t kAdlerMaxRun = 5552;  // zlib's NMAX: the most bytes before b can overflow

} // namespace

RAYOPDF_CRC_TARGET uint32_t Crc32Fold(const uint8_t* data, size_t size, uint32_t crc) {
    const __m128i fold4 = _mm_set_epi32(0x00000001, 0x54442bd4, 0x00000001, static_cast<int>(0xc6e41596));
    __m128i t0, t1, t2, t3;
    __m128i crc0 = _mm_cvtsi32_si128(static_cast<int>(0x9db42487));
    __m128i crc1 = _mm_setzero_si128();
    __m128i crc2 = _mm_setzero_si128();
    __m128i crc3 = _mm_setzero_si128();
    const __m128i* src = reinterpret_cast<const __m128i*>(data);

    if (crc != 0) {
        // The first 16 bytes with the initial CRC.
        size -= 16;
        t0 = _mm_load_si128(src);
        src += 1;
        FoldState1(crc0, crc1, crc2, crc3, fold4);
        crc3 = Xor3(crc3, t0, _mm_cvtsi32_si128(static_cast<int>(crc)));
    }

    while (size >= 64) {
        size -= 64;
        t0 = _mm_load_si128(src);
        t1 = _mm_load_si128(src + 1);
        t2 = _mm_load_si128(src + 2);
        t3 = _mm_load_si128(src + 3);
        src += 4;
        FoldState4(crc0, crc1, crc2, crc3, fold4);
        crc0 = _mm_xor_si128(crc0, t0);
        crc1 = _mm_xor_si128(crc1, t1);
        crc2 = _mm_xor_si128(crc2, t2);
        crc3 = _mm_xor_si128(crc3, t3);
    }

    // At most 63 bytes left.
    if (size >= 48) {
        size -= 48;
        t0 = _mm_load_si128(src);
        t1 = _mm_load_si128(src + 1);
        t2 = _mm_load_si128(src + 2);
        src += 3;
        FoldState3(crc0, crc1, crc2, crc3, fold4);
        crc1 = _mm_xor_si128(crc1, t0);
        crc2 = _mm_xor_si128(crc2, t1);
        crc3 = _mm_xor_si128(crc3, t2);
    } else if (size >= 32) {
        size -= 32;
        t0 = _mm_load_si128(src);
        t1 = _mm_load_si128(src + 1);
        src += 2;
        FoldState2(crc0, crc1, crc2, crc3, fold4);
        crc2 = _mm_xor_si128(crc2, t0);
        crc3 = _mm_xor_si128(crc3, t1);
    } else if (size >= 16) {
        size -= 16;
        t0 = _mm_load_si128(src);
        src += 1;
        FoldState1(crc0, crc1, crc2, crc3, fold4);
        crc3 = _mm_xor_si128(crc3, t0);
    }

    const __m128i k12 = _mm_set_epi32(0x00000001, 0x751997d0, 0x00000000, static_cast<int>(0xccaa009e));
    const __m128i barrett = _mm_set_epi32(0x00000001, static_cast<int>(0xdb710640), static_cast<int>(0xb4e5b025),
                                          static_cast<int>(0xf7011641));

    // Fold the four lanes into one with the k1/k2 constants.
    crc1 = Xor3(crc1, _mm_clmulepi64_si128(crc0, k12, 0x01), _mm_clmulepi64_si128(crc0, k12, 0x10));
    crc2 = Xor3(crc2, _mm_clmulepi64_si128(crc1, k12, 0x01), _mm_clmulepi64_si128(crc1, k12, 0x10));
    crc3 = Xor3(crc3, _mm_clmulepi64_si128(crc2, k12, 0x01), _mm_clmulepi64_si128(crc2, k12, 0x10));

    // The last 0-15 bytes: shift the state left by their count and fold out what falls off.
    if (size > 0) {
        const __m128i mask3 = _mm_set1_epi32(static_cast<int>(0x80808080));
        const __m128i sequence = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        const __m128i shiftLeft = _mm_add_epi8(sequence, _mm_set1_epi8(static_cast<char>(static_cast<int>(size) - 16)));
        const __m128i shiftRight = _mm_xor_si128(shiftLeft, mask3);
        const __m128i overflow = _mm_shuffle_epi8(crc3, shiftLeft);
        crc3 = _mm_shuffle_epi8(crc3, shiftRight);
        __m128i part = _mm_setzero_si128();
        std::memcpy(&part, src, size);
        crc3 = _mm_xor_si128(crc3, _mm_shuffle_epi8(part, shiftLeft));
        crc3 = Xor3(crc3, _mm_clmulepi64_si128(overflow, k12, 0x01), _mm_clmulepi64_si128(overflow, k12, 0x10));
    }

    // 128 bits to 32 by a two-stage Barrett reduction.
    __m128i tmp0 = _mm_clmulepi64_si128(crc3, barrett, 0x00);
    __m128i tmp1 = _mm_clmulepi64_si128(tmp0, barrett, 0x10);
    tmp1 = _mm_blend_epi16(tmp1, _mm_setzero_si128(), 0xcf);
    tmp0 = _mm_xor_si128(tmp1, crc3);
    const __m128i resultA = _mm_clmulepi64_si128(tmp0, barrett, 0x01);
    const __m128i resultB = _mm_clmulepi64_si128(resultA, barrett, 0x10);
    return ~static_cast<uint32_t>(_mm_extract_epi32(resultB, 2));
}

RAYOPDF_ADLER_TARGET uint32_t Adler32Avx2(const uint8_t* data, size_t size, uint32_t adler) {
    uint32_t adler0 = adler & 0xffff;
    uint32_t adler1 = (adler >> 16) & 0xffff;
    const __m256i dot2v = _mm256_setr_epi8(64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45,
                                           44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33);
    const __m256i dot2v0 = _mm256_setr_epi8(32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14,
                                            13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1);
    const __m256i dot3v = _mm256_set1_epi16(1);
    const __m256i zero = _mm256_setzero_si256();

    while (size >= 32) {
        __m256i vs1 = _mm256_zextsi128_si256(_mm_cvtsi32_si128(static_cast<int>(adler0)));
        __m256i vs2 = _mm256_zextsi128_si256(_mm_cvtsi32_si128(static_cast<int>(adler1)));
        __m256i vs1Before = vs1;
        __m256i vs3 = _mm256_setzero_si256();
        __m256i vs2Second = vs3;

        size_t k = std::min(size, kAdlerMaxRun) & ~size_t(31);
        size -= k;

        while (k >= 64) {
            const __m256i buf = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
            const __m256i buf0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + 32));
            data += 64;
            k -= 64;
            const __m256i sad = _mm256_sad_epu8(buf, zero);
            const __m256i sad2 = _mm256_sad_epu8(buf0, zero);
            vs1 = _mm256_add_epi32(vs1, sad);
            vs3 = _mm256_add_epi32(vs3, vs1Before);
            const __m256i shortSum = _mm256_maddubs_epi16(buf, dot2v);  // 32 bytes to 16 shorts
            const __m256i shortSum0 = _mm256_maddubs_epi16(buf0, dot2v0);
            const __m256i sum2 = _mm256_madd_epi16(shortSum, dot3v);  // 16 shorts to 8 ints
            const __m256i sum20 = _mm256_madd_epi16(shortSum0, dot3v);
            vs1 = _mm256_add_epi32(sad2, vs1);
            vs2 = _mm256_add_epi32(sum2, vs2);
            vs2Second = _mm256_add_epi32(sum20, vs2Second);
            vs1Before = vs1;
        }

        vs2 = _mm256_add_epi32(vs2Second, vs2);
        vs3 = _mm256_slli_epi32(vs3, 6);
        vs2 = _mm256_add_epi32(vs3, vs2);
        vs3 = _mm256_setzero_si256();

        while (k >= 32) {
            // vs1 = adler + sum(c[i]); vs2 = sum2 + 32 vs1 + sum((32 - i + 1) c[i])
            const __m256i buf = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
            data += 32;
            k -= 32;
            const __m256i sad = _mm256_sad_epu8(buf, zero);
            vs1 = _mm256_add_epi32(vs1, sad);
            vs3 = _mm256_add_epi32(vs3, vs1Before);
            const __m256i shortSum = _mm256_maddubs_epi16(buf, dot2v0);
            const __m256i sum2 = _mm256_madd_epi16(shortSum, dot3v);
            vs2 = _mm256_add_epi32(sum2, vs2);
            vs1Before = vs1;
        }

        // The multiplication by 32 deferred out of the loop.
        vs3 = _mm256_slli_epi32(vs3, 5);
        vs2 = _mm256_add_epi32(vs2, vs3);
        adler0 = EvenLaneSum(vs1) % kAdlerBase;
        adler1 = HorizontalSum(vs2) % kAdlerBase;
    }
    return adler0 | (adler1 << 16);
}

#undef RAYOPDF_CRC_TARGET
#undef RAYOPDF_ADLER_TARGET

} // namespace RayoPdf::Image::Checksum

#endif
