// SPDX-License-Identifier: Apache-2.0
// The inverse wavelet transforms of the JPEG 2000 decoder (T.800 annex F: the 5/3 reversible
// integer lifting and the 9/7 irreversible lifting with symmetric extension, rows first, then
// columns) and the inverse component transforms (annex G: RCT and ICT), with the DC level shift.
//
// The 9/7 transform and the ICT reproduce OpenJPEG 2.5's single-precision arithmetic operation by
// operation (its lifting constants, low-pass samples scaled by K and high-pass ones by 2/K, each
// lifting step x += (left + right) * c), so the samples agree with opj_decompress. Every float
// operation is a separately rounded IEEE single-precision operation, whether done on SSE2 lanes
// or in scalar code: the builds target x86-64 without FMA, and the pragma below keeps Clang from
// contracting a multiply and an add where FMA exists; GCC contracts only with FMA in the target,
// which RayoPDF's builds never enable (the Linux and Windows objects hold no FMA instruction).
//
// Both transforms run on eight lines at once, as OpenJPEG's do: eight rows gathered into eight
// lanes for the horizontal pass, eight columns loaded per row for the vertical one.
#include "image/jpx_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace RayoPdf::Image::Jpx::Internal {
namespace {

// T.800 table F.4 as OpenJPEG writes them, and OpenJPEG's high-pass scale 2/K.
constexpr float kAlpha = -1.586134342f;
constexpr float kBeta = -0.052980118f;
constexpr float kGamma = 0.882911075f;
constexpr float kDelta = 0.443506852f;
constexpr float kK = 1.230174105f;
constexpr float kTwoInvK = 1.625732422f;

// ------------------------------------------------------------------------------ eight lanes

#if defined(__SSE2__)
struct F8 {
    __m128 a, b;
};
struct I8 {
    __m128i a, b;
};
inline F8 Splat(float v) { return F8{_mm_set1_ps(v), _mm_set1_ps(v)}; }
inline F8 Add(F8 x, F8 y) { return F8{_mm_add_ps(x.a, y.a), _mm_add_ps(x.b, y.b)}; }
inline F8 Mul(F8 x, F8 y) { return F8{_mm_mul_ps(x.a, y.a), _mm_mul_ps(x.b, y.b)}; }
inline F8 Load(const float* p) { return F8{_mm_loadu_ps(p), _mm_loadu_ps(p + 4)}; }
inline void Store(float* p, F8 v) {
    _mm_storeu_ps(p, v.a);
    _mm_storeu_ps(p + 4, v.b);
}
inline I8 SplatI(int32_t v) { return I8{_mm_set1_epi32(v), _mm_set1_epi32(v)}; }
inline I8 Add(I8 x, I8 y) { return I8{_mm_add_epi32(x.a, y.a), _mm_add_epi32(x.b, y.b)}; }
inline I8 Sub(I8 x, I8 y) { return I8{_mm_sub_epi32(x.a, y.a), _mm_sub_epi32(x.b, y.b)}; }
template <int n>
inline I8 Sra(I8 x) {
    return I8{_mm_srai_epi32(x.a, n), _mm_srai_epi32(x.b, n)};
}
inline I8 Load(const int32_t* p) {
    return I8{_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 4))};
}
inline void Store(int32_t* p, I8 v) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v.a);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 4), v.b);
}
#else
struct F8 {
    float v[8];
};
struct I8 {
    int32_t v[8];
};
inline F8 Splat(float s) {
    F8 r;
    for (float& x : r.v) x = s;
    return r;
}
inline F8 Add(F8 x, F8 y) {
    for (int k = 0; k < 8; k++) x.v[k] = x.v[k] + y.v[k];
    return x;
}
inline F8 Mul(F8 x, F8 y) {
    for (int k = 0; k < 8; k++) x.v[k] = x.v[k] * y.v[k];
    return x;
}
inline F8 Load(const float* p) {
    F8 r;
    std::memcpy(r.v, p, sizeof r.v);
    return r;
}
inline void Store(float* p, F8 v) { std::memcpy(p, v.v, sizeof v.v); }
inline I8 SplatI(int32_t s) {
    I8 r;
    for (int32_t& x : r.v) x = s;
    return r;
}
inline I8 Add(I8 x, I8 y) {
    for (int k = 0; k < 8; k++) x.v[k] = static_cast<int32_t>(static_cast<uint32_t>(x.v[k]) + static_cast<uint32_t>(y.v[k]));
    return x;
}
inline I8 Sub(I8 x, I8 y) {
    for (int k = 0; k < 8; k++) x.v[k] = static_cast<int32_t>(static_cast<uint32_t>(x.v[k]) - static_cast<uint32_t>(y.v[k]));
    return x;
}
template <int n>
inline I8 Sra(I8 x) {
    for (int k = 0; k < 8; k++) x.v[k] >>= n;
    return x;
}
inline I8 Load(const int32_t* p) {
    I8 r;
    std::memcpy(r.v, p, sizeof r.v);
    return r;
}
inline void Store(int32_t* p, I8 v) { std::memcpy(p, v.v, sizeof v.v); }
#endif

template <typename T>
struct LanesOf;
template <>
struct LanesOf<float> {
    using Type = F8;
};
template <>
struct LanesOf<int32_t> {
    using Type = I8;
};

inline int32_t Clamp(int32_t i, int32_t n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); }

// ---------------------------------------------------------------------------- lifting
//
// `low` (sn lanes) and `high` (dn lanes) are the two halves of eight lines; `odd`: the lines start
// on an odd coordinate (their first sample is a high-pass one). Neighbour indices are clamped,
// which is the symmetric extension of F.3.7.

// One 9/7 lifting step: target(i) += (source(i - 1 + shift) + source(i + shift)) * c.
inline void Step97(F8* target, int32_t tn, const F8* source, int32_t n, int32_t shift, float c) {
    const F8 k = Splat(c);
    // Both neighbours exist for 1 - shift <= i <= n - 1 - shift; clamp only outside.
    const int32_t first = std::min(tn, std::max<int32_t>(0, 1 - shift));
    const int32_t last = std::max(first, std::min(tn, n - shift));
    for (int32_t i = 0; i < first; i++) {
        target[i] = Add(target[i], Mul(Add(source[Clamp(i - 1 + shift, n)], source[Clamp(i + shift, n)]), k));
    }
    const F8* s = source + shift;
    for (int32_t i = first; i < last; i++) target[i] = Add(target[i], Mul(Add(s[i - 1], s[i]), k));
    for (int32_t i = last; i < tn; i++) {
        target[i] = Add(target[i], Mul(Add(source[Clamp(i - 1 + shift, n)], source[Clamp(i + shift, n)]), k));
    }
}

void Lift97(F8* low, F8* high, int32_t sn, int32_t dn, bool odd) {
    // A line of one sample stays as it is (as in OpenJPEG).
    if (!odd ? !(dn > 0 || sn > 1) : !(sn > 0 || dn > 1)) return;
    const F8 k = Splat(kK);
    const F8 twoInvK = Splat(kTwoInvK);
    for (int32_t i = 0; i < sn; i++) low[i] = Mul(low[i], k);
    for (int32_t i = 0; i < dn; i++) high[i] = Mul(high[i], twoInvK);
    // Even lines: low i sits between high i-1 and high i, high i between low i and low i+1.
    // Odd lines: low i sits between high i and high i+1, high i between low i-1 and low i.
    const int32_t lowShift = odd ? 1 : 0;
    const int32_t highShift = odd ? 0 : 1;
    Step97(low, sn, high, dn, lowShift, -kDelta);
    Step97(high, dn, low, sn, highShift, -kGamma);
    Step97(low, sn, high, dn, lowShift, -kBeta);
    Step97(high, dn, low, sn, highShift, -kAlpha);
}

void Lift53(I8* low, I8* high, int32_t sn, int32_t dn, bool odd) {
    const I8 two = SplatI(2);
    if (!odd) {
        if (sn + dn <= 1) return;
        // low(i) -= (high(i-1) + high(i) + 2) >> 2; high(i) += (low(i) + low(i+1)) >> 1
        for (int32_t i = 0; i < sn; i++)
            low[i] = Sub(low[i], Sra<2>(Add(Add(high[Clamp(i - 1, dn)], high[Clamp(i, dn)]), two)));
        for (int32_t i = 0; i < dn; i++) high[i] = Add(high[i], Sra<1>(Add(low[Clamp(i, sn)], low[Clamp(i + 1, sn)])));
    } else {
        if (sn == 0 && dn == 1) {
            // A single odd sample is halved, truncating as C division does (OpenJPEG).
            alignas(16) int32_t v[8];
            Store(v, high[0]);
            for (int32_t& x : v) x /= 2;
            high[0] = Load(v);
            return;
        }
        // low(i) -= (high(i) + high(i+1) + 2) >> 2; high(i) += (low(i-1) + low(i)) >> 1
        for (int32_t i = 0; i < sn; i++)
            low[i] = Sub(low[i], Sra<2>(Add(Add(high[Clamp(i, dn)], high[Clamp(i + 1, dn)]), two)));
        for (int32_t i = 0; i < dn; i++) high[i] = Add(high[i], Sra<1>(Add(low[Clamp(i - 1, sn)], low[Clamp(i, sn)])));
    }
}

inline void Lift(F8* low, F8* high, int32_t sn, int32_t dn, bool odd) { Lift97(low, high, sn, dn, odd); }
inline void Lift(I8* low, I8* high, int32_t sn, int32_t dn, bool odd) { Lift53(low, high, sn, dn, odd); }

// Eight full rows to lane vectors and back, four samples at a time through 4x4 transposes (the
// shuffles move bits, so integers ride in float registers unchanged). They return how many
// samples they did; the caller does the rest one at a time.
#if defined(__SSE2__)
inline __m128 AsFloats(const float* p) { return _mm_loadu_ps(p); }
inline __m128 AsFloats(const int32_t* p) { return _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))); }
inline void FromFloats(float* p, __m128 v) { _mm_storeu_ps(p, v); }
inline void FromFloats(int32_t* p, __m128 v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_castps_si128(v)); }
inline void Halves(const F8& v, __m128& a, __m128& b) {
    a = v.a;
    b = v.b;
}
inline void Halves(const I8& v, __m128& a, __m128& b) {
    a = _mm_castsi128_ps(v.a);
    b = _mm_castsi128_ps(v.b);
}
inline void Join(F8& v, __m128 a, __m128 b) { v = F8{a, b}; }
inline void Join(I8& v, __m128 a, __m128 b) { v = I8{_mm_castps_si128(a), _mm_castps_si128(b)}; }

template <typename T, typename V>
uint32_t GatherRows(const T* first, size_t stride, uint32_t width, V* buf) {
    uint32_t i = 0;
    for (; i + 4 <= width; i += 4) {
        const T* p = first + i;
        __m128 r0 = AsFloats(p), r1 = AsFloats(p + stride), r2 = AsFloats(p + 2 * stride), r3 = AsFloats(p + 3 * stride);
        __m128 r4 = AsFloats(p + 4 * stride), r5 = AsFloats(p + 5 * stride), r6 = AsFloats(p + 6 * stride);
        __m128 r7 = AsFloats(p + 7 * stride);
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
        _MM_TRANSPOSE4_PS(r4, r5, r6, r7);
        Join(buf[i], r0, r4);
        Join(buf[i + 1], r1, r5);
        Join(buf[i + 2], r2, r6);
        Join(buf[i + 3], r3, r7);
    }
    return i;
}

template <typename T, typename V>
uint32_t ScatterRows(const V* buf, int32_t sn, uint32_t lowStart, uint32_t width, T* first, size_t stride) {
    // Position p takes lane vector p / 2 of the low half when p has the low parity.
    const V* even = lowStart == 0 ? buf : buf + sn;
    const V* odd = lowStart == 0 ? buf + sn : buf;
    uint32_t p = 0;
    for (; p + 4 <= width; p += 4) {
        __m128 r0, r1, r2, r3, r4, r5, r6, r7;
        Halves(even[p >> 1], r0, r4);
        Halves(odd[p >> 1], r1, r5);
        Halves(even[(p >> 1) + 1], r2, r6);
        Halves(odd[(p >> 1) + 1], r3, r7);
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
        _MM_TRANSPOSE4_PS(r4, r5, r6, r7);
        T* q = first + p;
        FromFloats(q, r0);
        FromFloats(q + stride, r1);
        FromFloats(q + 2 * stride, r2);
        FromFloats(q + 3 * stride, r3);
        FromFloats(q + 4 * stride, r4);
        FromFloats(q + 5 * stride, r5);
        FromFloats(q + 6 * stride, r6);
        FromFloats(q + 7 * stride, r7);
    }
    return p;
}
#else
template <typename T, typename V>
uint32_t GatherRows(const T*, size_t, uint32_t, V*) {
    return 0;
}
template <typename T, typename V>
uint32_t ScatterRows(const V*, int32_t, uint32_t, uint32_t, T*, size_t) {
    return 0;
}
#endif

// ---------------------------------------------------------------------------- 2-D

// Room for two buffers of `lines` lane vectors each, the first returned: padded so that every
// lane vector starts 16-byte aligned.
template <typename T>
typename LanesOf<T>::Type* LaneBuffers(std::vector<T>& scratch, size_t lines) {
    if (scratch.size() < lines * 16 + 4) scratch.resize(lines * 16 + 4);
    T* base = scratch.data();
    while (reinterpret_cast<uintptr_t>(base) % 16 != 0) base++;
    return reinterpret_cast<typename LanesOf<T>::Type*>(base);
}

// Rows [first, end), eight at a time from `first`: gather sample i of each row into lane vector
// i, lift, scatter back interleaved.
template <typename T>
void InverseRows(T* data, size_t stride, uint32_t width, uint32_t lowWidth, bool xOdd, uint32_t first, uint32_t end,
                 std::vector<T>& scratch) {
    using V = typename LanesOf<T>::Type;
    V* buf = LaneBuffers(scratch, width);
    alignas(16) T lane[8];
    const int32_t sn = static_cast<int32_t>(lowWidth);
    const int32_t dn = static_cast<int32_t>(width - lowWidth);
    const uint32_t lowStart = xOdd ? 1 : 0;
    for (uint32_t y0 = first; y0 < end; y0 += 8) {
        const uint32_t rows = std::min<uint32_t>(8, end - y0);
        T* top = data + y0 * stride;
        uint32_t i = 0;
        if (rows == 8) i = GatherRows(top, stride, width, buf);
        for (; i < width; i++) {
            for (uint32_t k = 0; k < 8; k++) lane[k] = k < rows ? top[k * stride + i] : T(0);
            buf[i] = Load(lane);
        }
        Lift(buf, buf + sn, sn, dn, xOdd);
        // Output position p takes lane vector p / 2 of the low half when p has the low
        // parity, of the high half otherwise.
        uint32_t p = 0;
        if (rows == 8) p = ScatterRows(buf, sn, lowStart, width, top, stride);
        for (; p < width; p++) {
            Store(lane, (p & 1) == lowStart ? buf[p >> 1] : buf[sn + (p >> 1)]);
            for (uint32_t k = 0; k < rows; k++) top[k * stride + p] = lane[k];
        }
    }
}

// Columns [first, end), sixteen (one cache line of floats) or eight at a time from `first`: each
// row's eight samples are one lane vector.
template <typename T>
void InverseColumns(T* data, size_t stride, uint32_t height, uint32_t lowHeight, bool yOdd, uint32_t first, uint32_t end,
                    std::vector<T>& scratch) {
    using V = typename LanesOf<T>::Type;
    V* buf = LaneBuffers(scratch, height);
    V* buf2 = buf + height;
    alignas(16) T lane[8];
    const int32_t sn = static_cast<int32_t>(lowHeight);
    const int32_t dn = static_cast<int32_t>(height - lowHeight);
    const uint32_t lowStart = yOdd ? 1 : 0;
    const uint32_t highStart = yOdd ? 0 : 1;
    // Lane vector i goes to row lowStart + 2i (low) or highStart + 2(i - sn) (high).
    auto rowOf = [&](uint32_t i) { return int32_t(i) < sn ? lowStart + 2 * i : highStart + 2 * (i - uint32_t(sn)); };
    uint32_t x0 = first;
    for (; x0 + 16 <= end; x0 += 16) {
        for (uint32_t y = 0; y < height; y++) {
#if defined(__SSE2__)
            if (y + 16 < height) {
                _mm_prefetch(reinterpret_cast<const char*>(data + (y + 16) * stride + x0), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(data + (y + 16) * stride + x0 + 15), _MM_HINT_T0);
            }
#endif
            buf[y] = Load(data + y * stride + x0);
            buf2[y] = Load(data + y * stride + x0 + 8);
        }
        Lift(buf, buf + sn, sn, dn, yOdd);
        Lift(buf2, buf2 + sn, sn, dn, yOdd);
        for (uint32_t i = 0; i < height; i++) {
            T* row = data + rowOf(i) * stride + x0;
            Store(row, buf[i]);
            Store(row + 8, buf2[i]);
        }
    }
    for (; x0 < end; x0 += 8) {
        const uint32_t columns = std::min<uint32_t>(8, end - x0);
        if (columns == 8) {
            for (uint32_t y = 0; y < height; y++) buf[y] = Load(data + y * stride + x0);
        } else {
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t k = 0; k < 8; k++) lane[k] = k < columns ? data[y * stride + x0 + k] : T(0);
                buf[y] = Load(lane);
            }
        }
        Lift(buf, buf + sn, sn, dn, yOdd);
        for (uint32_t i = 0; i < height; i++) {
            const uint32_t y = rowOf(i);
            if (columns == 8) {
                Store(data + y * stride + x0, buf[i]);
            } else {
                Store(lane, buf[i]);
                std::memcpy(data + y * stride + x0, lane, columns * sizeof(T));
            }
        }
    }
}

template <typename T>
void Inverse2D(T* data, size_t stride, uint32_t width, uint32_t height, uint32_t lowWidth, uint32_t lowHeight, bool xOdd,
               bool yOdd, std::vector<T>& scratch) {
    if (width == 0 || height == 0) return;
    LaneBuffers(scratch, std::max(width, height));  // sized once for both passes
    InverseRows(data, stride, width, lowWidth, xOdd, 0, height, scratch);
    InverseColumns(data, stride, height, lowHeight, yOdd, 0, width, scratch);
}

} // namespace

void Inverse53(int32_t* data, size_t stride, uint32_t width, uint32_t height, uint32_t lowWidth, uint32_t lowHeight,
               bool xOdd, bool yOdd, std::vector<int32_t>& scratch) {
    Inverse2D(data, stride, width, height, lowWidth, lowHeight, xOdd, yOdd, scratch);
}

void Inverse97(float* data, size_t stride, uint32_t width, uint32_t height, uint32_t lowWidth, uint32_t lowHeight,
               bool xOdd, bool yOdd, std::vector<float>& scratch) {
    Inverse2D(data, stride, width, height, lowWidth, lowHeight, xOdd, yOdd, scratch);
}

void Inverse53Rows(int32_t* data, size_t stride, uint32_t width, uint32_t lowWidth, bool xOdd, uint32_t first,
                   uint32_t end, std::vector<int32_t>& scratch) {
    InverseRows(data, stride, width, lowWidth, xOdd, first, end, scratch);
}

void Inverse53Columns(int32_t* data, size_t stride, uint32_t height, uint32_t lowHeight, bool yOdd, uint32_t first,
                      uint32_t end, std::vector<int32_t>& scratch) {
    InverseColumns(data, stride, height, lowHeight, yOdd, first, end, scratch);
}

void Inverse97Rows(float* data, size_t stride, uint32_t width, uint32_t lowWidth, bool xOdd, uint32_t first,
                   uint32_t end, std::vector<float>& scratch) {
    InverseRows(data, stride, width, lowWidth, xOdd, first, end, scratch);
}

void Inverse97Columns(float* data, size_t stride, uint32_t height, uint32_t lowHeight, bool yOdd, uint32_t first,
                      uint32_t end, std::vector<float>& scratch) {
    InverseColumns(data, stride, height, lowHeight, yOdd, first, end, scratch);
}

void InverseRct(int32_t* c0, int32_t* c1, int32_t* c2, size_t count) {
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const I8 y = Load(c0 + i);
        const I8 u = Load(c1 + i);
        const I8 v = Load(c2 + i);
        const I8 g = Sub(y, Sra<2>(Add(u, v)));
        Store(c0 + i, Add(v, g));
        Store(c1 + i, g);
        Store(c2 + i, Add(u, g));
    }
    for (; i < count; i++) {
        const uint32_t y = static_cast<uint32_t>(c0[i]);
        const uint32_t u = static_cast<uint32_t>(c1[i]);
        const uint32_t v = static_cast<uint32_t>(c2[i]);
        const uint32_t g = y - static_cast<uint32_t>(static_cast<int32_t>(u + v) >> 2);
        c0[i] = static_cast<int32_t>(v + g);
        c1[i] = static_cast<int32_t>(g);
        c2[i] = static_cast<int32_t>(u + g);
    }
}

void InverseIct(float* c0, float* c1, float* c2, size_t count) {
    size_t i = 0;
    const F8 rv = Splat(1.402f), gu = Splat(-0.34413f), gv = Splat(-0.71414f), bu = Splat(1.772f);
    for (; i + 8 <= count; i += 8) {
        const F8 y = Load(c0 + i);
        const F8 u = Load(c1 + i);
        const F8 v = Load(c2 + i);
        // y - u*a - v*b with a, b negated: x - p and x + (-p) round alike.
        Store(c0 + i, Add(y, Mul(v, rv)));
        Store(c1 + i, Add(Add(y, Mul(u, gu)), Mul(v, gv)));
        Store(c2 + i, Add(y, Mul(u, bu)));
    }
    for (; i < count; i++) {
        const float y = c0[i];
        const float u = c1[i];
        const float v = c2[i];
        c0[i] = y + (v * 1.402f);
        c1[i] = y - (u * 0.34413f) - (v * 0.71414f);
        c2[i] = y + (u * 1.772f);
    }
}

void ShiftClamp(const int32_t* in, int32_t* out, size_t count, int32_t shift, int32_t low, int32_t high) {
    size_t i = 0;
#if defined(__SSE2__)
    // Clipping to [low - shift, high - shift] before the shift cannot overflow (precisions are at
    // most 16 bits) and gives what clipping the exact sum gives.
    const __m128i s = _mm_set1_epi32(shift), l = _mm_set1_epi32(low - shift), h = _mm_set1_epi32(high - shift);
    for (; i + 4 <= count; i += 4) {
        __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
        const __m128i below = _mm_cmplt_epi32(r, l);
        r = _mm_or_si128(_mm_and_si128(below, l), _mm_andnot_si128(below, r));
        const __m128i above = _mm_cmpgt_epi32(r, h);
        r = _mm_or_si128(_mm_and_si128(above, h), _mm_andnot_si128(above, r));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_add_epi32(r, s));
    }
#endif
    for (; i < count; i++) {
        const int64_t v = int64_t(in[i]) + shift;
        out[i] = static_cast<int32_t>(v < low ? low : (v > high ? high : v));
    }
}

namespace {

int32_t ShiftClampOne(float v, int32_t shift, int32_t low, int32_t high) {
    if (v > 2147483648.0f) return high;
    if (!(v >= -2147483648.0f)) return low;  // also NaN, which lrintf turns into INT_MIN
    const int64_t r = static_cast<int64_t>(std::llrint(v)) + shift;
    return static_cast<int32_t>(r < low ? low : (r > high ? high : r));
}

#if defined(__SSE2__)
// Values beyond +-2^30 clip anyway (precisions are at most 16 bits); _mm_max_ps returns its second
// operand for NaN, which then clips to `low` as lrintf's INT_MIN would. cvtps2dq rounds to
// nearest even, as lrintf.
__m128i RoundClipped(const float* p) {
    const __m128 lo = _mm_set1_ps(-1073741824.0f), hi = _mm_set1_ps(1073741824.0f);
    return _mm_cvtps_epi32(_mm_min_ps(_mm_max_ps(_mm_loadu_ps(p), lo), hi));
}

// Sixteen values within +-2^30, plus 128, clipped to [0, 255]: the saturating packs clip a value
// at or beyond the int16 range to that range's end, where adding 128 and clipping to [0, 255]
// gives what it gives for the value itself.
void StoreShiftedBytes(uint8_t* out, __m128i a, __m128i b, __m128i c, __m128i d) {
    const __m128i s = _mm_set1_epi16(128);
    const __m128i ab = _mm_adds_epi16(_mm_packs_epi32(a, b), s);
    const __m128i cd = _mm_adds_epi16(_mm_packs_epi32(c, d), s);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_packus_epi16(ab, cd));
}
#endif

} // namespace

void ShiftClampFloat(const float* in, int32_t* out, size_t count, int32_t shift, int32_t low, int32_t high) {
    size_t i = 0;
#if defined(__SSE2__)
    const __m128i s = _mm_set1_epi32(shift), l = _mm_set1_epi32(low), h = _mm_set1_epi32(high);
    for (; i + 4 <= count; i += 4) {
        __m128i r = _mm_add_epi32(RoundClipped(in + i), s);
        const __m128i below = _mm_cmplt_epi32(r, l);
        r = _mm_or_si128(_mm_and_si128(below, l), _mm_andnot_si128(below, r));
        const __m128i above = _mm_cmpgt_epi32(r, h);
        r = _mm_or_si128(_mm_and_si128(above, h), _mm_andnot_si128(above, r));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), r);
    }
#endif
    for (; i < count; i++) out[i] = ShiftClampOne(in[i], shift, low, high);
}

void ShiftClampBytes(const int32_t* in, uint8_t* out, size_t count) {
    size_t i = 0;
#if defined(__SSE2__)
    for (; i + 16 <= count; i += 16) {
        const __m128i* p = reinterpret_cast<const __m128i*>(in + i);
        StoreShiftedBytes(out + i, _mm_loadu_si128(p), _mm_loadu_si128(p + 1), _mm_loadu_si128(p + 2),
                          _mm_loadu_si128(p + 3));
    }
#endif
    for (; i < count; i++) {
        const int64_t v = int64_t(in[i]) + 128;
        out[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

void ShiftClampFloatBytes(const float* in, uint8_t* out, size_t count) {
    size_t i = 0;
#if defined(__SSE2__)
    for (; i + 16 <= count; i += 16) {
        StoreShiftedBytes(out + i, RoundClipped(in + i), RoundClipped(in + i + 4), RoundClipped(in + i + 8),
                          RoundClipped(in + i + 12));
    }
#endif
    for (; i < count; i++) out[i] = static_cast<uint8_t>(ShiftClampOne(in[i], 128, 0, 255));
}

void Dequantize53(const int32_t* in, int32_t* out, size_t count) {
    size_t i = 0;
#if defined(__SSE2__)
    for (; i + 4 <= count; i += 4) {
        // v / 2 rounds toward zero: add the sign bit before the arithmetic shift.
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_srai_epi32(_mm_add_epi32(v, _mm_srli_epi32(v, 31)), 1));
    }
#endif
    for (; i < count; i++) out[i] = in[i] / 2;
}

void Dequantize97(const int32_t* in, float* out, size_t count, float step) {
    size_t i = 0;
#if defined(__SSE2__)
    // cvtdq2ps rounds as the scalar conversion does (to nearest even).
    const __m128 s = _mm_set1_ps(step);
    for (; i + 4 <= count; i += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
        _mm_storeu_ps(out + i, _mm_mul_ps(_mm_cvtepi32_ps(v), s));
    }
#endif
    for (; i < count; i++) out[i] = static_cast<float>(in[i]) * step;
}

void NarrowBytes(const int32_t* in, uint8_t* out, size_t count, size_t stride) {
    size_t i = 0;
#if defined(__SSE2__)
    if (stride == 1) {
        // The saturating packs keep values in [0, 255] as they are.
        for (; i + 16 <= count; i += 16) {
            const __m128i* p = reinterpret_cast<const __m128i*>(in + i);
            const __m128i a = _mm_packs_epi32(_mm_loadu_si128(p), _mm_loadu_si128(p + 1));
            const __m128i b = _mm_packs_epi32(_mm_loadu_si128(p + 2), _mm_loadu_si128(p + 3));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), _mm_packus_epi16(a, b));
        }
    }
#endif
    for (; i < count; i++) out[i * stride] = static_cast<uint8_t>(in[i]);
}

} // namespace RayoPdf::Image::Jpx::Internal
