// SPDX-License-Identifier: Apache-2.0
// The row filters are forked from RayoMD src/core/png_filter.cpp at ef106ca (Apache-2.0, same
// author): the scalar reference and the SSE2 versions of choosing and applying a filter.
#include "image/png.h"

#include "common/compiler.h"
#include "image/deflate.h"
#include "image/image.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if RAYOPDF_PNG_SSE2
#include <emmintrin.h>
#endif

namespace RayoPdf::Image {
namespace Png {
namespace {

// ---------------------------------------------------------------- CRC-32

constexpr std::array<std::array<uint32_t, 256>, 8> MakeCrcTables() {
    std::array<std::array<uint32_t, 256>, 8> tables{};
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        tables[0][n] = c;
    }
    for (uint32_t n = 0; n < 256; n++) {
        for (size_t t = 1; t < 8; t++) tables[t][n] = (tables[t - 1][n] >> 8) ^ tables[0][tables[t - 1][n] & 0xFF];
    }
    return tables;
}
constexpr std::array<std::array<uint32_t, 256>, 8> kCrc = MakeCrcTables();

// ---------------------------------------------------------------- filters

// The magnitude of a residual read as a signed byte: r < 128 ? r : 256 - r.
inline uint64_t ResidualCost(int residual) {
    const uint32_t r = static_cast<uint32_t>(residual) & 0xffu;
    return r < 128 ? r : 256 - r;
}

// The Paeth predictor (whichever of a, b, c is nearest to p = a + b - c, in that order on ties)
// without branches: p - a = b - c, p - b = a - c, p - c = (b - c) + (a - c).
inline int Paeth(int a, int b, int c) {
    const int pa = std::abs(b - c);
    const int pb = std::abs(a - c);
    const int pc = std::abs(a + b - 2 * c);
    const int bc = pb <= pc ? b : c;
    return (pa <= pb && pa <= pc) ? a : bc;
}

struct FilterCosts {
    uint64_t sum[5] = {};

    // Bytes [from, to): the first `bpp` of the row have no left neighbour, so left = upLeft = 0
    // there and Paeth(0, b, 0) is b.
    void Add(const uint8_t* row, const uint8_t* up, size_t from, size_t to, size_t bpp) {
        size_t x = from;
        for (; x < to && x < bpp; x++) {
            const int v = row[x], b = up[x];
            sum[0] += ResidualCost(v);
            sum[1] += ResidualCost(v);
            sum[2] += ResidualCost(v - b);
            sum[3] += ResidualCost(v - (b >> 1));
            sum[4] += ResidualCost(v - b);
        }
        for (; x < to; x++) {
            const int v = row[x], a = row[x - bpp], b = up[x], c = up[x - bpp];
            sum[0] += ResidualCost(v);
            sum[1] += ResidualCost(v - a);
            sum[2] += ResidualCost(v - b);
            sum[3] += ResidualCost(v - ((a + b) >> 1));
            sum[4] += ResidualCost(v - Paeth(a, b, c));
        }
    }

    int Best() const {
        int best = 0;
        for (int filter = 1; filter < 5; filter++) {
            if (sum[filter] < sum[best]) best = filter;
        }
        return best;
    }
};

// The residuals of bytes [from, to) under filter 1 to 4; the row's first `bpp` bytes have no left
// neighbour.
void FilterBytes(int filter, const uint8_t* row, const uint8_t* up, size_t from, size_t to, size_t bpp, uint8_t* out) {
    size_t x = from;
    for (; x < to && x < bpp; x++) {
        const int b = up[x];
        out[x] = static_cast<uint8_t>(row[x] - (filter == 1 ? 0 : filter == 3 ? b >> 1 : b));
    }
    switch (filter) {
    case 1:
        for (; x < to; x++) out[x] = static_cast<uint8_t>(row[x] - row[x - bpp]);
        break;
    case 2:
        for (; x < to; x++) out[x] = static_cast<uint8_t>(row[x] - up[x]);
        break;
    case 3:
        for (; x < to; x++) out[x] = static_cast<uint8_t>(row[x] - ((row[x - bpp] + up[x]) >> 1));
        break;
    default:
        for (; x < to; x++) out[x] = static_cast<uint8_t>(row[x] - Paeth(row[x - bpp], up[x], up[x - bpp]));
        break;
    }
}

#if RAYOPDF_PNG_SSE2
// Sixteen residual magnitudes, min(r, 256 - r) per byte, added up into two 64-bit lanes.
RAYOPDF_HOT_INLINE __m128i Cost16(__m128i value, __m128i predictor) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i r = _mm_sub_epi8(value, predictor);
    return _mm_sad_epu8(_mm_min_epu8(r, _mm_sub_epi8(zero, r)), zero);
}

// floor((a + b) / 2) per byte: pavgb rounds up, so take back the odd bit.
RAYOPDF_HOT_INLINE __m128i Average16(__m128i a, __m128i b) {
    return _mm_sub_epi8(_mm_avg_epu8(a, b), _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1)));
}

RAYOPDF_HOT_INLINE __m128i Abs16(__m128i x) { return _mm_max_epi16(x, _mm_sub_epi16(_mm_setzero_si128(), x)); }

// Paeth on eight 16-bit lanes, the same comparisons as the scalar one.
RAYOPDF_HOT_INLINE __m128i PaethLanes(__m128i a, __m128i b, __m128i c) {
    const __m128i bc = _mm_sub_epi16(b, c);
    const __m128i ac = _mm_sub_epi16(a, c);
    const __m128i pa = Abs16(bc);
    const __m128i pb = Abs16(ac);
    const __m128i pc = Abs16(_mm_add_epi16(bc, ac));
    const __m128i notA = _mm_or_si128(_mm_cmpgt_epi16(pa, pb), _mm_cmpgt_epi16(pa, pc));
    const __m128i useC = _mm_cmpgt_epi16(pb, pc);
    const __m128i bOrC = _mm_or_si128(_mm_andnot_si128(useC, b), _mm_and_si128(useC, c));
    return _mm_or_si128(_mm_andnot_si128(notA, a), _mm_and_si128(notA, bOrC));
}

RAYOPDF_HOT_INLINE __m128i Paeth16(__m128i a, __m128i b, __m128i c) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i lo = PaethLanes(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero), _mm_unpacklo_epi8(c, zero));
    const __m128i hi = PaethLanes(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero), _mm_unpackhi_epi8(c, zero));
    return _mm_packus_epi16(lo, hi);
}

RAYOPDF_HOT_INLINE __m128i Load16(const uint8_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }

inline uint64_t LaneSum(__m128i sums) {
    uint64_t lanes[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(lanes), sums);
    return lanes[0] + lanes[1];
}

// 16 zero bytes, then 16 of 0xFF: the 16 bytes loaded from kTailMask + n keep the last n bytes of
// a step. A row's last bytes take its last full step again, those counted before cleared,
// instead of a scalar loop.
constexpr std::array<uint8_t, 32> MakeTailMask() {
    std::array<uint8_t, 32> mask{};
    for (size_t index = 16; index < mask.size(); index++) mask[index] = 0xFF;
    return mask;
}
constexpr std::array<uint8_t, 32> kTailMask = MakeTailMask();

// Adds the cost of `predictor` for `value` to `sum`, of the bytes `keep` keeps when Masked.
template <bool Masked>
RAYOPDF_HOT_INLINE __m128i AddCost16(__m128i sum, __m128i value, __m128i predictor, __m128i keep) {
    if constexpr (!Masked) {
        (void)keep;
        return _mm_add_epi64(sum, Cost16(value, predictor));
    } else {
        const __m128i zero = _mm_setzero_si128();
        const __m128i r = _mm_sub_epi8(value, predictor);
        return _mm_add_epi64(sum, _mm_sad_epu8(_mm_and_si128(_mm_min_epu8(r, _mm_sub_epi8(zero, r)), keep), zero));
    }
}

// The five filters' costs of the 16 bytes at x (x >= bpp).
template <bool Masked>
RAYOPDF_HOT_INLINE void AddCosts16(const uint8_t* row, const uint8_t* up, size_t x, size_t bpp, __m128i keep,
                                   __m128i& s0, __m128i& s1, __m128i& s2, __m128i& s3, __m128i& s4) {
    const __m128i v = Load16(row + x);
    const __m128i a = Load16(row + x - bpp);
    const __m128i b = Load16(up + x);
    const __m128i c = Load16(up + x - bpp);
    s0 = AddCost16<Masked>(s0, v, _mm_setzero_si128(), keep);
    s1 = AddCost16<Masked>(s1, v, a, keep);
    s2 = AddCost16<Masked>(s2, v, b, keep);
    s3 = AddCost16<Masked>(s3, v, Average16(a, b), keep);
    s4 = AddCost16<Masked>(s4, v, Paeth16(a, b, c), keep);
}

template <int Filter>
RAYOPDF_HOT_INLINE __m128i Predictor16(const uint8_t* row, const uint8_t* up, size_t x, size_t bpp) {
    if constexpr (Filter == 1) return Load16(row + x - bpp);
    else if constexpr (Filter == 2) return Load16(up + x);
    else if constexpr (Filter == 3) return Average16(Load16(row + x - bpp), Load16(up + x));
    else return Paeth16(Load16(row + x - bpp), Load16(up + x), Load16(up + x - bpp));
}

int ChooseFilterSse2(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
    FilterCosts costs;
    size_t x = std::min(bpp, rowBytes);
    costs.Add(row, up, 0, x, bpp);
    if (rowBytes >= bpp + 16) {
        const __m128i zero = _mm_setzero_si128();
        __m128i s0 = zero, s1 = zero, s2 = zero, s3 = zero, s4 = zero;
        for (; x + 16 <= rowBytes; x += 16) AddCosts16<false>(row, up, x, bpp, zero, s0, s1, s2, s3, s4);
        if (x < rowBytes) {
            const __m128i keep = Load16(kTailMask.data() + (rowBytes - x));
            AddCosts16<true>(row, up, rowBytes - 16, bpp, keep, s0, s1, s2, s3, s4);
            x = rowBytes;
        }
        costs.sum[0] += LaneSum(s0);
        costs.sum[1] += LaneSum(s1);
        costs.sum[2] += LaneSum(s2);
        costs.sum[3] += LaneSum(s3);
        costs.sum[4] += LaneSum(s4);
    }
    costs.Add(row, up, x, rowBytes, bpp);
    return costs.Best();
}

// Bytes [bpp, rowBytes) of a row of at least bpp + 16, 16 a step; the last step is the row's last
// 16 bytes, which writes some of the step before's again, the same values (Sub, Average and
// Paeth read the input row only, never `out`).
template <int Filter>
void FilterSteps16(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out) {
    for (size_t x = bpp;; x += 16) {
        if (x + 16 > rowBytes) x = rowBytes - 16;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x),
                         _mm_sub_epi8(Load16(row + x), Predictor16<Filter>(row, up, x, bpp)));
        if (x + 16 == rowBytes) return;
    }
}

void FilterRowSse2(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out) {
    if (filter == 0) {
        std::memcpy(out, row, rowBytes);
        return;
    }
    if (rowBytes < bpp + 16) {
        FilterBytes(filter, row, up, 0, rowBytes, bpp, out);
        return;
    }
    FilterBytes(filter, row, up, 0, bpp, bpp, out);
    switch (filter) {
    case 1: FilterSteps16<1>(row, up, rowBytes, bpp, out); break;
    case 2: FilterSteps16<2>(row, up, rowBytes, bpp, out); break;
    case 3: FilterSteps16<3>(row, up, rowBytes, bpp, out); break;
    default: FilterSteps16<4>(row, up, rowBytes, bpp, out); break;
    }
}
#endif

} // namespace

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    crc = ~crc;
    while (size >= 8) {
        const uint32_t low = crc ^ (static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
                                    static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24);
        crc = kCrc[7][low & 0xFF] ^ kCrc[6][(low >> 8) & 0xFF] ^ kCrc[5][(low >> 16) & 0xFF] ^ kCrc[4][low >> 24] ^
              kCrc[3][data[4]] ^ kCrc[2][data[5]] ^ kCrc[1][data[6]] ^ kCrc[0][data[7]];
        data += 8;
        size -= 8;
    }
    while (size-- > 0) crc = kCrc[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int ChooseFilterScalar(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
    FilterCosts costs;
    costs.Add(row, up, 0, rowBytes, bpp);
    return costs.Best();
}

void FilterRowScalar(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out) {
    if (filter == 0) std::memcpy(out, row, rowBytes);
    else FilterBytes(filter, row, up, 0, rowBytes, bpp, out);
}

int ChooseFilter(const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp) {
#if RAYOPDF_PNG_SSE2
    return ChooseFilterSse2(row, up, rowBytes, bpp);
#else
    return ChooseFilterScalar(row, up, rowBytes, bpp);
#endif
}

void FilterRow(int filter, const uint8_t* row, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* out) {
#if RAYOPDF_PNG_SSE2
    FilterRowSse2(filter, row, up, rowBytes, bpp, out);
#else
    FilterRowScalar(filter, row, up, rowBytes, bpp, out);
#endif
}

} // namespace Png

namespace {

void PutBe32(uint8_t* at, uint32_t value) {
    at[0] = static_cast<uint8_t>(value >> 24);
    at[1] = static_cast<uint8_t>(value >> 16);
    at[2] = static_cast<uint8_t>(value >> 8);
    at[3] = static_cast<uint8_t>(value);
}

const uint8_t* PixelRow(void* context, uint32_t y) {
    const auto* rows = static_cast<const std::pair<const uint8_t*, size_t>*>(context);
    return rows->first + size_t(y) * rows->second;
}

} // namespace

namespace Png {

bool EncodeRows(uint32_t width, uint32_t height, int channels, RowSource source, void* context, std::string& out) {
    out.clear();
    // PNG caps both dimensions at 2^31 - 1; the filtered rows must fit the compressor.
    if (!source || width == 0 || height == 0 || channels < 1 || channels > 4 || width > 0x7FFFFFFFu ||
        height > 0x7FFFFFFFu) {
        return false;
    }
    const size_t bpp = static_cast<size_t>(channels);
    const uint64_t rowBytes64 = uint64_t(width) * bpp;
    const uint64_t filtered64 = (rowBytes64 + 1) * height;
    if (filtered64 > Deflate::kMaxInput) return false;
    const size_t rowBytes = static_cast<size_t>(rowBytes64);
    const size_t filteredSize = static_cast<size_t>(filtered64);

    // One filter byte, then the residuals, per row; the source's rows are asked for once each.
    std::vector<uint8_t> filtered(filteredSize);
    std::vector<uint8_t> zeros(rowBytes, 0);
    const uint8_t* up = zeros.data();
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = source(context, y);
        uint8_t* target = filtered.data() + size_t(y) * (rowBytes + 1);
        const int filter = ChooseFilter(row, up, rowBytes, bpp);
        target[0] = static_cast<uint8_t>(filter);
        FilterRow(filter, row, up, rowBytes, bpp, target + 1);
        up = row;
    }

    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    static constexpr uint8_t kColorType[5] = {0, 0, 4, 2, 6};  // gray, gray + alpha, RGB, RGBA
    const size_t bound = Deflate::ZlibBound(filteredSize);
    // Signature, IHDR (8 + 13 + 4), IDAT header, data, IDAT CRC, IEND (12); the bound's 8 bytes of
    // slack lie within the IDAT CRC and IEND that follow. The file is put together in a buffer
    // kept per thread and left uninitialized, then copied: only the pages written count towards
    // the process's memory (a zero-filled string of the bound holds as much again as the filtered
    // rows), and a batch maps it once instead of for every image.
    const size_t need = 8 + 25 + 8 + bound + 4 + 12;
    thread_local std::unique_ptr<uint8_t[]> file;
    thread_local size_t capacity = 0;
    if (capacity < need) {
        file.reset(new (std::nothrow) uint8_t[need]);
        capacity = file ? need : 0;
        if (!file) return false;
    }
    uint8_t* const base = file.get();
    std::memcpy(base, kSignature, 8);
    uint8_t* ihdr = base + 8;
    PutBe32(ihdr, 13);
    std::memcpy(ihdr + 4, "IHDR", 4);
    PutBe32(ihdr + 8, width);
    PutBe32(ihdr + 12, height);
    ihdr[16] = 8;  // bit depth
    ihdr[17] = kColorType[channels];
    ihdr[18] = 0;  // deflate
    ihdr[19] = 0;  // adaptive filtering
    ihdr[20] = 0;  // no interlace
    PutBe32(ihdr + 21, Crc32(ihdr + 4, 17));

    uint8_t* idat = base + 33;
    std::memcpy(idat + 4, "IDAT", 4);
    thread_local Deflate::Scratch scratch;
    const size_t compressed = Deflate::ZlibCompress(
        std::string_view(reinterpret_cast<const char*>(filtered.data()), filteredSize), idat + 8, scratch);
    PutBe32(idat, static_cast<uint32_t>(compressed));
    PutBe32(idat + 8 + compressed, Crc32(idat + 4, 4 + compressed));

    uint8_t* iend = idat + 8 + compressed + 4;
    PutBe32(iend, 0);
    std::memcpy(iend + 4, "IEND", 4);
    PutBe32(iend + 8, Crc32(iend + 4, 4));
    out.assign(reinterpret_cast<const char*>(base), static_cast<size_t>(iend + 12 - base));
    return true;
}

} // namespace Png

bool EncodePng(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, std::string& out) {
    if (!pixels) {
        out.clear();
        return false;
    }
    std::pair<const uint8_t*, size_t> rows{pixels, size_t(width) * static_cast<size_t>(channels)};
    return Png::EncodeRows(width, height, channels, PixelRow, &rows, out);
}

} // namespace RayoPdf::Image
