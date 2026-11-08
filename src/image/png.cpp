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

// The layout of a PNG's rows: colour type, bit depth, bytes per complete pixel for the filters
// (1 below 8 bits) and bytes a row.
struct RowFormat {
    uint8_t colorType = 0;
    uint8_t bitDepth = 8;
    size_t filterBpp = 1;
    size_t rowBytes = 0;
};

// Gray values that a PNG of fewer bits holds exactly (PNG specification 13.12: a sample of depth
// d stands for v * 255 / (2^d - 1)): bit 0 for 1 bit (0 and 255), bit 1 for 2 bits (multiples of
// 85), bit 2 for 4 bits (multiples of 17).
struct SubByteDepths {
    uint8_t of[256] = {};
    constexpr SubByteDepths() {
        for (int v = 0; v < 256; v++) {
            of[v] = static_cast<uint8_t>((v == 0 || v == 255 ? 1 : 0) | (v % 85 == 0 ? 2 : 0) | (v % 17 == 0 ? 4 : 0));
        }
    }
};
constexpr SubByteDepths kSubByteDepths;

// The fewest bits a sample that holds every value of `pixels` exactly: 1, 2, 4 or 8.
int GrayDepth(const uint8_t* pixels, size_t count) {
    uint8_t allowed = 7;
    for (size_t i = 0; i < count && allowed != 0; i++) allowed &= kSubByteDepths.of[pixels[i]];
    return (allowed & 1) ? 1 : (allowed & 2) ? 2 : (allowed & 4) ? 4 : 8;
}

// Rows of 8-bit gray packed into `depth`-bit samples, most significant bits first (the values are
// multiples of 255 / (2^depth - 1), GrayDepth): a value's sample from a table, no division.
struct PackedRows {
    const uint8_t* pixels;
    uint32_t width;
    int depth;
    std::vector<uint8_t> rows[2];
    uint8_t sampleOf[256];
};

const uint8_t* PackedRow(void* context, uint32_t y) {
    PackedRows& r = *static_cast<PackedRows*>(context);
    const uint8_t* in = r.pixels + size_t(y) * r.width;
    uint8_t* out = r.rows[y & 1].data();
    const unsigned depth = static_cast<unsigned>(r.depth);
    const unsigned perByte = 8 / depth;
    const uint32_t whole = r.width / perByte;
    for (uint32_t o = 0; o < whole; o++, in += perByte) {
        unsigned byte = 0;
        for (unsigned k = 0; k < perByte; k++) byte = byte << depth | r.sampleOf[in[k]];
        out[o] = static_cast<uint8_t>(byte);
    }
    const unsigned rest = r.width % perByte;
    if (rest > 0) {
        unsigned byte = 0;
        for (unsigned k = 0; k < rest; k++) byte = byte << depth | r.sampleOf[in[k]];
        out[whole] = static_cast<uint8_t>(byte << depth * (perByte - rest));
    }
    return out;
}

// Packed gray rows as they are, or complemented, with the padding bits cleared.
struct GrayRows {
    const uint8_t* rows;
    size_t inBytes;      // bytes between rows in `rows`
    size_t outBytes;     // bytes a PNG row
    uint8_t lastMask;    // the bits of the last byte that hold samples
    uint8_t flip;        // 0xFF to complement
    std::vector<uint8_t> row[2];
};

const uint8_t* GrayRow(void* context, uint32_t y) {
    GrayRows& r = *static_cast<GrayRows*>(context);
    const uint8_t* in = r.rows + size_t(y) * r.inBytes;
    uint8_t* out = r.row[y & 1].data();
    for (size_t i = 0; i < r.outBytes; i++) out[i] = static_cast<uint8_t>(in[i] ^ r.flip);
    out[r.outBytes - 1] &= r.lastMask;
    return out;
}

// A PNG file in a string: appended to, its IDAT length set in place at the end.
class StringOutput final : public Png::Output {
public:
    explicit StringOutput(std::string& out) : out_(out) {}
    bool Append(const uint8_t* data, size_t size) override {
        out_.append(reinterpret_cast<const char*>(data), size);
        return true;
    }
    bool Patch(uint64_t offset, const uint8_t* data, size_t size) override {
        if (offset > out_.size() || size > out_.size() - offset) return false;
        std::memcpy(&out_[static_cast<size_t>(offset)], data, size);
        return true;
    }

private:
    std::string& out_;
};

struct IdatSink {
    Png::Output* output;
    uint32_t crc;
    uint64_t size;
};

bool AppendIdat(void* context, const uint8_t* data, size_t size) {
    IdatSink& sink = *static_cast<IdatSink*>(context);
    sink.crc = Png::Crc32(data, size, sink.crc);
    sink.size += size;
    return sink.output->Append(data, size);
}

// The PNG of the rows `source` gives, written to `output` as it is made: each row filtered and
// handed to the compressor, which keeps only its window, so no image-sized buffer is held but the
// file's (a file, or a string). One IDAT chunk, whose length is set when the stream ends.
bool EncodeFormatted(uint32_t width, uint32_t height, const RowFormat& format, Png::RowSource source, void* context,
                     Png::Output& output) {
    // PNG caps both dimensions at 2^31 - 1; the filtered rows must fit the compressor.
    if (!source || width == 0 || height == 0 || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu) return false;
    const size_t bpp = format.filterBpp;
    const size_t rowBytes = format.rowBytes;
    const uint64_t filtered64 = (uint64_t(rowBytes) + 1) * height;
    if (filtered64 > Deflate::kMaxInput) return false;

    // Signature, IHDR (8 + 13 + 4), then the IDAT chunk's length (set at the end) and type.
    uint8_t head[8 + 25 + 8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t* ihdr = head + 8;
    PutBe32(ihdr, 13);
    std::memcpy(ihdr + 4, "IHDR", 4);
    PutBe32(ihdr + 8, width);
    PutBe32(ihdr + 12, height);
    ihdr[16] = format.bitDepth;
    ihdr[17] = format.colorType;
    ihdr[18] = 0;  // deflate
    ihdr[19] = 0;  // adaptive filtering
    ihdr[20] = 0;  // no interlace
    PutBe32(ihdr + 21, Png::Crc32(ihdr + 4, 17));
    uint8_t* idat = head + 33;
    PutBe32(idat, 0);
    std::memcpy(idat + 4, "IDAT", 4);
    if (!output.Append(head, sizeof head)) return false;

    thread_local Deflate::Scratch scratch;
    IdatSink sink{&output, Png::Crc32(idat + 4, 4), 0};
    Deflate::ZlibStream stream(static_cast<size_t>(filtered64), scratch, AppendIdat, &sink);
    // One filter byte, then the residuals, per row; the source's rows are asked for once each.
    std::vector<uint8_t> filtered(rowBytes + 1);
    std::vector<uint8_t> zeros(rowBytes, 0);
    const uint8_t* up = zeros.data();
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = source(context, y);
        const int filter = Png::ChooseFilter(row, up, rowBytes, bpp);
        filtered[0] = static_cast<uint8_t>(filter);
        Png::FilterRow(filter, row, up, rowBytes, bpp, filtered.data() + 1);
        if (!stream.Write(filtered.data(), filtered.size())) return false;
        up = row;
    }
    if (!stream.Finish() || sink.size > 0x7FFFFFFFu) return false;

    uint8_t length[4], tail[4 + 12];
    PutBe32(length, static_cast<uint32_t>(sink.size));
    PutBe32(tail, sink.crc);
    PutBe32(tail + 4, 0);
    std::memcpy(tail + 8, "IEND", 4);
    PutBe32(tail + 12, Png::Crc32(tail + 8, 4));
    return output.Patch(33, length, 4) && output.Append(tail, sizeof tail);
}

// A PNG into a string: `out` holds the file, or is empty when it could not be made.
template <typename Encode>
bool IntoString(std::string& out, Encode encode) {
    out.clear();
    StringOutput output(out);
    if (encode(static_cast<Png::Output&>(output))) return true;
    out.clear();
    return false;
}

} // namespace

namespace Png {

bool EncodeRows(uint32_t width, uint32_t height, int channels, RowSource source, void* context, Output& out) {
    static constexpr uint8_t kColorType[5] = {0, 0, 4, 2, 6};  // gray, gray + alpha, RGB, RGBA
    if (channels < 1 || channels > 4 || uint64_t(width) * static_cast<size_t>(channels) > SIZE_MAX / 2) return false;
    RowFormat format;
    format.colorType = kColorType[channels];
    format.filterBpp = static_cast<size_t>(channels);
    format.rowBytes = static_cast<size_t>(uint64_t(width) * static_cast<size_t>(channels));
    return EncodeFormatted(width, height, format, source, context, out);
}

bool EncodeRows(uint32_t width, uint32_t height, int channels, RowSource source, void* context, std::string& out) {
    return IntoString(out, [&](Output& o) { return EncodeRows(width, height, channels, source, context, o); });
}

bool EncodeGray(const uint8_t* rows, size_t rowBytes, uint32_t width, uint32_t height, int depth, bool invert,
                Output& out) {
    if (!rows || (depth != 1 && depth != 2 && depth != 4) || width == 0 || width > 0x7FFFFFFFu) return false;
    RowFormat format;
    format.bitDepth = static_cast<uint8_t>(depth);
    format.rowBytes = (size_t(width) * static_cast<size_t>(depth) + 7) / 8;
    if (rowBytes < format.rowBytes) return false;
    const unsigned used = static_cast<unsigned>((size_t(width) * static_cast<size_t>(depth)) % 8);
    const uint8_t lastMask = used == 0 ? 0xFF : static_cast<uint8_t>(0xFF00u >> used);
    GrayRows source{rows, rowBytes, format.rowBytes, lastMask, static_cast<uint8_t>(invert ? 0xFF : 0),
                    {std::vector<uint8_t>(format.rowBytes), std::vector<uint8_t>(format.rowBytes)}};
    return EncodeFormatted(width, height, format, GrayRow, &source, out);
}

bool EncodeGray(const uint8_t* rows, size_t rowBytes, uint32_t width, uint32_t height, int depth, bool invert,
                std::string& out) {
    return IntoString(out, [&](Output& o) { return EncodeGray(rows, rowBytes, width, height, depth, invert, o); });
}

bool EncodePixels(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, Output& out) {
    if (!pixels) return false;
    if (channels == 1 && width > 0 && height > 0 && width <= 0x7FFFFFFFu && height <= 0x7FFFFFFFu) {
        // Gray at the fewest bits that hold its values exactly: a stencil's 0 and 255 in 1 bit, an
        // eighth of the bytes to filter and compress (same pixels when read back).
        const int depth = GrayDepth(pixels, size_t(width) * height);
        if (depth < 8) {
            RowFormat format;
            format.bitDepth = static_cast<uint8_t>(depth);
            format.rowBytes = (size_t(width) * static_cast<size_t>(depth) + 7) / 8;
            PackedRows rows{pixels, width, depth, {std::vector<uint8_t>(format.rowBytes), std::vector<uint8_t>(format.rowBytes)}, {}};
            const unsigned scale = 255u / ((1u << depth) - 1);
            for (unsigned v = 0; v < 256; v++) rows.sampleOf[v] = static_cast<uint8_t>(v / scale);
            return EncodeFormatted(width, height, format, PackedRow, &rows, out);
        }
    }
    std::pair<const uint8_t*, size_t> rows{pixels, size_t(width) * static_cast<size_t>(channels)};
    return EncodeRows(width, height, channels, PixelRow, &rows, out);
}

} // namespace Png

bool EncodePng(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, std::string& out) {
    return IntoString(out, [&](Png::Output& o) { return Png::EncodePixels(pixels, width, height, channels, o); });
}

} // namespace RayoPdf::Image
