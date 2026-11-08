// SPDX-License-Identifier: Apache-2.0
// The region decoding procedures of the JBIG2 decoder, written from ITU-T T.88 section 6: generic
// regions (6.2: templates 0-3 with AT pixels, TPGDON, skipped pixels; MMR through the CCITT G4
// decoder of image/ccitt.h), generic refinement regions (6.3: templates 0 and 1, TPGRON) and text
// regions (6.4), plus the bitmap operations they share (allocation, combination operators,
// symbol storage).
#include "image/ccitt.h"
#include "image/jbig2_internal.h"

#include <string>

namespace RayoPdf::Image::Jbig2::Internal {
namespace {

// The 8 pixels of `row` (`bytes` long) from bit position `bit` on; pixels outside the row are 0.
RAYOPDF_HOT_INLINE uint32_t Get8(const uint8_t* row, size_t bytes, int64_t bit) {
    const int64_t i = bit >> 3;  // arithmetic shift: floor
    const int shift = static_cast<int>(bit & 7);
    const uint32_t hi = (i >= 0 && i < int64_t(bytes)) ? row[i] : 0u;
    const uint32_t lo = (i + 1 >= 0 && i + 1 < int64_t(bytes)) ? row[i + 1] : 0u;
    return ((hi << 8 | lo) << shift >> 8) & 0xFFu;
}

RAYOPDF_HOT_INLINE uint8_t Apply(uint32_t d, uint32_t s, uint32_t mask, CombineOp op) {
    switch (op) {
    case CombineOp::Or: return static_cast<uint8_t>(d | (s & mask));
    case CombineOp::And: return static_cast<uint8_t>(d & (s | ~mask));
    case CombineOp::Xor: return static_cast<uint8_t>(d ^ (s & mask));
    case CombineOp::Xnor: return static_cast<uint8_t>(d ^ (~s & mask));
    case CombineOp::Replace: return static_cast<uint8_t>((d & ~mask) | (s & mask));
    }
    return static_cast<uint8_t>(d);
}

// floor(v / 2) (T.88 table 12).
int64_t FloorHalf(int64_t v) { return v >= 0 ? v / 2 : -((1 - v) / 2); }

// ---------------------------------------------------------------- generic regions

// The SLTP contexts of TPGDON (6.2.5.7, figures 8-11) in the bit order of figures 3-6.
constexpr uint32_t kSltpContext[4] = {0x9B25, 0x0795, 0x00E5, 0x0195};
// The context bits of the AT pixels A1-A4 in that order.
constexpr uint32_t kAtBit[4][4] = {{4, 10, 11, 15}, {3, 0, 0, 0}, {2, 0, 0, 0}, {4, 0, 0, 0}};

// An AT pixel: read from a row window when it lies within one, else from the bitmap.
struct AtPixel {
    int dx;
    int dy;
    int window;  // 0 current row, 1 row above, 2 two rows above, -1 none
    int shift;
    uint32_t bit;
};

// The row windows: w<k> holds pixel x + dx of row y - k in bit 15 - dx. The rows above are
// loaded a byte at a time (at x = 8n the byte of pixels x + 8 .. x + 15 enters bits 7 .. 0) and
// shifted one bit per pixel, so dx runs from -16 to at least 8; w0 holds the decoded pixels.
constexpr int kWindowLeft = -16;
constexpr int kWindowRight = 8;

// The fixed pixels of templates 0-3 (6.2.5.3, figures 3-6) from the windows.
template <int T>
RAYOPDF_HOT_INLINE uint32_t FixedContext(uint32_t w0, uint32_t w1, uint32_t w2) {
    if (T == 0) return ((w2 >> 14) & 7u) << 12 | ((w1 >> 13) & 0x1Fu) << 5 | ((w0 >> 16) & 0xFu);
    if (T == 1) return ((w2 >> 13) & 0xFu) << 9 | ((w1 >> 13) & 0x1Fu) << 4 | ((w0 >> 16) & 7u);
    if (T == 2) return ((w2 >> 14) & 7u) << 7 | ((w1 >> 14) & 0xFu) << 3 | ((w0 >> 16) & 3u);
    return ((w1 >> 14) & 0x1Fu) << 5 | ((w0 >> 16) & 0xFu);
}

// The whole context with the AT pixels at their nominal places (table 5: A1 (3, -1) for
// templates 0 and 1, (2, -1) for 2 and 3; A2-A4 (-3, -1), (2, -2), (-2, -2)), where each row's
// pixels are adjacent bits of the context in the window's order.
template <int T>
RAYOPDF_HOT_INLINE uint32_t NominalContext(uint32_t w0, uint32_t w1, uint32_t w2) {
    if (T == 0) return ((w2 >> 13) & 0x1Fu) << 11 | ((w1 >> 12) & 0x7Fu) << 4 | ((w0 >> 16) & 0xFu);
    if (T == 1) return ((w2 >> 13) & 0xFu) << 9 | ((w1 >> 12) & 0x3Fu) << 3 | ((w0 >> 16) & 7u);
    if (T == 2) return ((w2 >> 14) & 7u) << 7 | ((w1 >> 13) & 0x1Fu) << 2 | ((w0 >> 16) & 3u);
    return ((w1 >> 13) & 0x3Fu) << 4 | ((w0 >> 16) & 0xFu);
}

bool NominalAt(int t, const int* at) {
    if (t == 0) return at[0] == 3 && at[1] == -1 && at[2] == -3 && at[3] == -1 && at[4] == 2 && at[5] == -2 &&
                       at[6] == -2 && at[7] == -2;
    return at[0] == (t == 1 ? 3 : 2) && at[1] == -1;
}

template <int T, bool kNominal>
void GenericRows(MqDecoder& mq, Context* contexts, const GenericParams& params, const AtPixel* at, int atCount,
                 Bitmap& out) {
    const uint32_t width = out.width;
    const size_t stride = out.stride;
    const View view = ViewOf(out);
    const uint8_t* skip = params.skip != nullptr ? params.skip->data.data() : nullptr;
    const size_t skipStride = params.skip != nullptr ? params.skip->stride : 0;
    uint32_t ltp = 0;
    for (uint32_t y = 0; y < out.height; y++) {
        uint8_t* row = out.data.data() + size_t(y) * stride;
        if (params.tpgdon) {
            ltp ^= mq.Decode(contexts[kSltpContext[T]]);
            if (ltp) {
                if (y > 0) std::memcpy(row, row - stride, stride);
                continue;
            }
        }
        const uint8_t* above1 = y >= 1 ? row - stride : nullptr;
        const uint8_t* above2 = y >= 2 ? row - 2 * stride : nullptr;
        const uint8_t* skipRow = skip != nullptr ? skip + size_t(y) * skipStride : nullptr;
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        uint32_t w2 = 0;
        if (above1 != nullptr) w1 = uint32_t(above1[0]) << 8 | (stride > 1 ? above1[1] : 0u);
        if (above2 != nullptr) w2 = uint32_t(above2[0]) << 8 | (stride > 1 ? above2[1] : 0u);
        // A byte of pixels at a time: the rows above enter the windows a byte ahead, the decoded
        // pixels are stored when their byte is complete (an AT pixel read from the bitmap lies at
        // least 17 pixels back or not yet decoded, so never in the byte being built).
        for (size_t xb = 0; xb < stride; xb++) {
            if (xb != 0 && xb + 1 < stride) {
                if (above1 != nullptr) w1 |= above1[xb + 1];
                if (above2 != nullptr) w2 |= above2[xb + 1];
            }
            const uint32_t first = static_cast<uint32_t>(xb * 8);
            const uint32_t count = width - first < 8 ? width - first : 8;
            const uint32_t skipByte = skipRow != nullptr ? skipRow[xb] : 0u;
            uint32_t byte = 0;
            for (uint32_t k = 0; k < count; k++) {
                uint32_t context;
                if (kNominal) {
                    context = NominalContext<T>(w0, w1, w2);
                } else {
                    context = FixedContext<T>(w0, w1, w2);
                    for (int j = 0; j < atCount; j++) {
                        const AtPixel& a = at[j];
                        uint32_t v;
                        if (a.window == 0) v = (w0 >> a.shift) & 1u;
                        else if (a.window == 1) v = (w1 >> a.shift) & 1u;
                        else if (a.window == 2) v = (w2 >> a.shift) & 1u;
                        else v = view.Pixel(int64_t(first + k) + a.dx, int64_t(y) + a.dy);
                        context |= v << a.bit;
                    }
                }
                uint32_t bit = 0;
                if (((skipByte >> (7 - k)) & 1u) == 0) bit = mq.Decode(contexts[context]);
                byte |= bit << (7 - k);
                w0 = (w0 | bit << 15) << 1;
                w1 <<= 1;
                w2 <<= 1;
            }
            row[xb] = static_cast<uint8_t>(byte);
        }
    }
}

} // namespace

Status Allocate(Bitmap& b, int64_t width, int64_t height, uint32_t value, const Budget& budget) {
    if (width < 0 || height < 0) return Status::Corrupt;
    if (width > 0xFFFFFFFFll || height > 0xFFFFFFFFll || uint64_t(width) * uint64_t(height) > budget.maxPixels)
        return Status::TooLarge;
    b.width = static_cast<uint32_t>(width);
    b.height = static_cast<uint32_t>(height);
    b.stride = (size_t(width) + 7) / 8;
    b.data.assign(b.stride * b.height, value ? 0xFF : 0);
    if (value && (width & 7) != 0) {
        const uint8_t last = static_cast<uint8_t>(0xFFu << (8 - (width & 7)));
        for (size_t y = 0; y < b.height; y++) b.data[y * b.stride + b.stride - 1] = last;
    }
    return Status::Ok;
}

void Combine(Bitmap& dst, const View& src, int64_t x, int64_t y, CombineOp op) {
    const int64_t x0 = x > 0 ? x : 0;
    const int64_t x1 = x + int64_t(src.width) < int64_t(dst.width) ? x + int64_t(src.width) : int64_t(dst.width);
    const int64_t y0 = y > 0 ? y : 0;
    const int64_t y1 = y + int64_t(src.height) < int64_t(dst.height) ? y + int64_t(src.height) : int64_t(dst.height);
    if (x0 >= x1 || y0 >= y1) return;
    const int64_t firstByte = x0 >> 3;
    const int64_t lastByte = (x1 - 1) >> 3;
    const uint32_t firstMask = 0xFFu >> (x0 & 7);
    const uint32_t lastMask = (0xFFu << (7 - ((x1 - 1) & 7))) & 0xFFu;
    for (int64_t row = y0; row < y1; row++) {
        const uint8_t* s = src.data + size_t(row - y) * src.stride;
        uint8_t* d = dst.data.data() + size_t(row) * dst.stride;
        if ((x & 7) == 0) {
            // Byte-aligned: source byte i lands on destination byte x / 8 + i.
            const int64_t base = x >> 3;
            for (int64_t b = firstByte; b <= lastByte; b++) {
                uint32_t mask = 0xFFu;
                if (b == firstByte) mask &= firstMask;
                if (b == lastByte) mask &= lastMask;
                d[b] = Apply(d[b], s[b - base], mask, op);
            }
            continue;
        }
        for (int64_t b = firstByte; b <= lastByte; b++) {
            uint32_t mask = 0xFFu;
            if (b == firstByte) mask &= firstMask;
            if (b == lastByte) mask &= lastMask;
            d[b] = Apply(d[b], Get8(s, src.stride, b * 8 - x), mask, op);
        }
    }
}

Status SymbolPool::Append(const View& src, uint32_t x, uint32_t width, Budget& budget, uint32_t& id) {
    const size_t stride = (size_t(width) + 7) / 8;
    const size_t bytes = stride * src.height;
    if (!budget.Retain(bytes + sizeof(SymbolRef))) return Status::TooLarge;
    SymbolRef ref;
    ref.offset = pixels.size();
    ref.width = width;
    ref.height = src.height;
    pixels.resize(ref.offset + bytes);
    uint8_t* d = pixels.data() + ref.offset;
    const uint32_t lastMask = (width & 7) != 0 ? (0xFFu << (8 - (width & 7))) & 0xFFu : 0xFFu;
    // An empty symbol (width or height 0) may claim 2^32 - 1 rows of nothing: no rows to copy.
    for (uint32_t row = 0; bytes != 0 && row < src.height; row++) {
        const uint8_t* s = src.data + size_t(row) * src.stride;
        for (size_t b = 0; b < stride; b++) {
            uint32_t v = (x & 7) == 0 && (x >> 3) + b < src.stride ? s[(x >> 3) + b] : Get8(s, src.stride, int64_t(x) + int64_t(b) * 8);
            if (b + 1 == stride) v &= lastMask;
            d[size_t(row) * stride + b] = static_cast<uint8_t>(v);
        }
    }
    id = static_cast<uint32_t>(symbols.size());
    symbols.push_back(ref);
    return Status::Ok;
}

Status DecodeGeneric(MqDecoder& mq, Context* contexts, const GenericParams& params, Bitmap& out, Budget& budget) {
    if (params.extTemplate) return Status::Unsupported;
    if (!budget.Spend(uint64_t(out.width) * out.height)) return Status::TooLarge;
    if (out.width == 0 || out.height == 0) return Status::Ok;
    const int t = params.templateId & 3;
    AtPixel at[4];
    const int atCount = t == 0 ? 4 : 1;
    for (int k = 0; k < atCount; k++) {
        AtPixel& a = at[k];
        a.dx = params.at[2 * k];
        a.dy = params.at[2 * k + 1];
        a.bit = kAtBit[t][k];
        a.window = -1;
        a.shift = 15 - a.dx;
        if ((a.dy == -1 || a.dy == -2) && a.dx >= kWindowLeft && a.dx <= kWindowRight) a.window = -a.dy;
        else if (a.dy == 0 && a.dx >= kWindowLeft && a.dx <= -1) a.window = 0;
    }
    const bool nominal = NominalAt(t, params.at);
    switch (t * 2 + (nominal ? 1 : 0)) {
    case 0: GenericRows<0, false>(mq, contexts, params, at, atCount, out); break;
    case 1: GenericRows<0, true>(mq, contexts, params, at, atCount, out); break;
    case 2: GenericRows<1, false>(mq, contexts, params, at, atCount, out); break;
    case 3: GenericRows<1, true>(mq, contexts, params, at, atCount, out); break;
    case 4: GenericRows<2, false>(mq, contexts, params, at, atCount, out); break;
    case 5: GenericRows<2, true>(mq, contexts, params, at, atCount, out); break;
    case 6: GenericRows<3, false>(mq, contexts, params, at, atCount, out); break;
    default: GenericRows<3, true>(mq, contexts, params, at, atCount, out); break;
    }
    return Status::Ok;
}

Status DecodeMmr(const uint8_t* data, size_t size, Bitmap& out, size_t& used, Budget& budget) {
    used = 0;
    if (!budget.Spend(uint64_t(out.width) * out.height)) return Status::TooLarge;
    if (out.width == 0 || out.height == 0) return Status::Ok;
    Ccitt::Params params;
    params.k = -1;
    params.columns = out.width;
    params.rows = out.height;
    params.blackIs1 = true;
    params.endOfBlock = true;
    params.encodedByteAlign = false;
    Ccitt::Info info;
    std::string rows;
    const Ccitt::Result result = Ccitt::Decode(data, size, params, out.height, budget.maxPixels, rows, info);
    // The rows decoded before damage stay (as poppler shows them); the caller learns of the damage.
    const size_t bytes = rows.size() < out.data.size() ? rows.size() : out.data.size();
    if (bytes != 0) std::memcpy(out.data.data(), rows.data(), bytes);
    // The CCITT decoder pads rows with black pixels; this bitmap's padding is white.
    if ((out.width & 7) != 0) {
        const uint8_t mask = static_cast<uint8_t>(0xFFu << (8 - (out.width & 7)));
        for (size_t i = out.stride - 1; i < bytes; i += out.stride) out.data[i] &= mask;
    }
    used = info.bytesUsed < size ? info.bytesUsed : size;
    if (result == Ccitt::Result::TooLarge) return Status::TooLarge;
    if (result != Ccitt::Result::Ok) return Status::Corrupt;
    return Status::Ok;
}

namespace {

// 6.3.5: rows of the refined bitmap with windows as in GenericRows: c1 holds the row above,
// r0, r1, r2 the reference rows ry - 1, ry, ry + 1 (ry = y - dy), pixel x + i (x - dx + i in the
// reference) in bit 15 - i. The context bit order (any one order is valid within a decoder):
// template 0: bit 0 (-1, 0), 1 (1, -1), 2 (0, -1), 3 RA1, then reference (1, 1), (0, 1), (-1, 1),
// (1, 0), (0, 0), (-1, 0), (1, -1), (0, -1), 12 RA2; template 1: (-1, 0), (1, -1), (0, -1),
// (-1, -1), then reference (1, 1), (0, 1), (1, 0), (0, 0), (-1, 0), (0, -1). The SLTP context
// (figures 14, 15) is the reference pixel (0, 0) alone.
template <int T, bool kNominal>
void RefinementRows(MqDecoder& mq, Context* contexts, const RefinementParams& p, Bitmap& out) {
    const View cur = ViewOf(out);
    const View& ref = p.reference;
    const size_t stride = out.stride;
    const uint32_t sltp = T == 0 ? 1u << 8 : 1u << 7;
    const auto refRow = [&](int64_t ry) -> const uint8_t* {
        return ry >= 0 && ry < int64_t(ref.height) ? ref.data + size_t(ry) * ref.stride : nullptr;
    };
    const auto load = [&](const uint8_t* r, int64_t bit) { return r != nullptr ? Get8(r, ref.stride, bit) : 0u; };
    uint32_t ltp = 0;
    for (uint32_t y = 0; y < out.height; y++) {
        if (p.tpgron) ltp ^= mq.Decode(contexts[sltp]);
        uint8_t* row = out.data.data() + size_t(y) * stride;
        const uint8_t* above = y >= 1 ? row - stride : nullptr;
        const int64_t ry = int64_t(y) - p.dy;
        const uint8_t* ref0 = refRow(ry - 1);
        const uint8_t* ref1 = refRow(ry);
        const uint8_t* ref2 = refRow(ry + 1);
        const int64_t start = -p.dx;
        uint32_t w0 = 0;
        uint32_t c1 = above != nullptr ? (uint32_t(above[0]) << 8 | (stride > 1 ? above[1] : 0u)) : 0u;
        uint32_t r0 = load(ref0, start) << 8 | load(ref0, start + 8);
        uint32_t r1 = load(ref1, start) << 8 | load(ref1, start + 8);
        uint32_t r2 = load(ref2, start) << 8 | load(ref2, start + 8);
        for (uint32_t x = 0; x < out.width; x++) {
            if ((x & 7) == 0 && x != 0) {
                const size_t next = (size_t(x) >> 3) + 1;
                if (above != nullptr && next < stride) c1 |= above[next];
                const int64_t position = int64_t(x) + start + 8;
                r0 |= load(ref0, position);
                r1 |= load(ref1, position);
                r2 |= load(ref2, position);
            }
            uint32_t bit;
            // 6.3.5.6 3d: with LTP, a 3 x 3 reference neighbourhood of one value predicts the pixel.
            const uint32_t n0 = (r0 >> 14) & 7u;
            const uint32_t n1 = (r1 >> 14) & 7u;
            const uint32_t n2 = (r2 >> 14) & 7u;
            if (ltp && (n0 & n1 & n2) == 7u) {
                bit = 1;
            } else if (ltp && (n0 | n1 | n2) == 0) {
                bit = 0;
            } else {
                uint32_t context;
                if (T == 0) {
                    context = ((w0 >> 16) & 1u) | ((c1 >> 14) & 3u) << 1 | n2 << 4 | n1 << 7 | ((r0 >> 14) & 3u) << 10;
                    if (kNominal) {
                        context |= ((c1 >> 16) & 1u) << 3 | ((r0 >> 16) & 1u) << 12;
                    } else {
                        context |= cur.Pixel(int64_t(x) + p.at[0], int64_t(y) + p.at[1]) << 3 |
                                   ref.Pixel(int64_t(x) + start + p.at[2], ry + p.at[3]) << 12;
                    }
                } else {
                    context = ((w0 >> 16) & 1u) | ((c1 >> 14) & 7u) << 1 | ((r2 >> 14) & 3u) << 4 | n1 << 6 |
                              ((r0 >> 15) & 1u) << 9;
                }
                bit = mq.Decode(contexts[context]);
            }
            if (bit) row[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
            w0 = (w0 | bit << 15) << 1;
            c1 <<= 1;
            r0 <<= 1;
            r1 <<= 1;
            r2 <<= 1;
        }
    }
}

} // namespace

Status DecodeRefinement(MqDecoder& mq, Context* contexts, const RefinementParams& params, Bitmap& out,
                        Budget& budget) {
    if (!budget.Spend(uint64_t(out.width) * out.height)) return Status::TooLarge;
    if (out.width == 0 || out.height == 0) return Status::Ok;
    if (params.templateId != 0) {
        RefinementRows<1, false>(mq, contexts, params, out);
    } else if (params.at[0] == -1 && params.at[1] == -1 && params.at[2] == -1 && params.at[3] == -1) {
        RefinementRows<0, true>(mq, contexts, params, out);
    } else {
        RefinementRows<0, false>(mq, contexts, params, out);
    }
    return Status::Ok;
}

// ---------------------------------------------------------------- text regions

namespace {

// Text region coordinates (S, T) beyond this magnitude are corrupt.
constexpr int64_t kMaxCoordinate = int64_t(1) << 40;
// Units of work per symbol instance, besides its pixels: the integer decodes it costs.
constexpr uint64_t kInstanceWork = 32;

enum class Next : uint8_t { Value, Oob, Error };

Next HuffmanValue(BitReader* reader, const HuffTable* table, int64_t& value) {
    if (reader == nullptr || table == nullptr) return Next::Error;
    const Code c = DecodeHuffman(*reader, *table, value);
    return c == Code::Value ? Next::Value : (c == Code::Oob ? Next::Oob : Next::Error);
}

Next IntValue(MqDecoder& mq, IntContexts& contexts, int64_t& value) {
    return DecodeInt(mq, contexts, value) ? Next::Value : Next::Oob;
}

} // namespace

Status DecodeText(const TextParams& p, MqDecoder& mq, IntDecoders& ints, Context* grContexts, BitReader* reader,
                  const SymbolPool& pool, Scratch& scratch, Bitmap& out, Budget& budget) {
    Status status = Allocate(out, p.width, p.height, p.defaultPixel, budget);
    if (status != Status::Ok) return status;
    if (!budget.Spend(uint64_t(out.stride) * out.height)) return Status::TooLarge;
    const bool huffman = p.huffman;
    if (huffman && reader == nullptr) return Status::Corrupt;
    const int64_t strips = int64_t(1) << p.logStrips;
    // A value that must be present (anything but OOB).
    const auto required = [&](const HuffTable* table, IntContexts& contexts, int64_t& value) {
        const Next n = huffman ? HuffmanValue(reader, table, value) : IntValue(mq, contexts, value);
        return n == Next::Value;
    };
    int64_t value = 0;
    if (!required(p.dt, ints.iadt, value)) return Status::Corrupt;
    int64_t stripT = -value * strips;
    int64_t firstS = 0;
    uint32_t instances = 0;
    while (instances < p.numInstances) {
        if (!required(p.dt, ints.iadt, value)) return Status::Corrupt;
        stripT += value * strips;
        if (!required(p.fs, ints.iafs, value)) return Status::Corrupt;
        firstS += value;
        // No real stream places an instance 2^40 pixels away; the bound keeps every coordinate
        // sum below 2^63.
        if (stripT > kMaxCoordinate || stripT < -kMaxCoordinate || firstS > kMaxCoordinate || firstS < -kMaxCoordinate)
            return Status::Corrupt;
        int64_t curS = firstS;
        for (;;) {
            if (!budget.Spend(kInstanceWork)) return Status::TooLarge;
            int64_t curT = 0;
            if (strips > 1) {
                if (huffman) curT = reader->ReadBits(p.logStrips);
                else if (!DecodeInt(mq, ints.iait, curT)) return Status::Corrupt;
            }
            const int64_t t = stripT + curT;
            uint32_t id;
            if (huffman) {
                if (p.symbolCodes != nullptr) {
                    if (HuffmanValue(reader, p.symbolCodes, value) != Next::Value) return Status::Corrupt;
                    id = static_cast<uint32_t>(value);
                } else {
                    id = reader->ReadBits(p.symbolCodeLength);
                }
            } else {
                id = DecodeIaid(mq, ints.iaid.data(), p.symbolCodeLength);
            }
            if (id >= p.numSymbols) return Status::Corrupt;
            uint32_t refine = 0;
            if (p.refine) {
                if (huffman) refine = reader->ReadBit();
                else if (!DecodeInt(mq, ints.iari, value)) return Status::Corrupt;
                else refine = value != 0;
            }
            View symbol = pool.Get(p.symbols[id]);
            if (refine) {
                int64_t rdw, rdh, rdx, rdy;
                if (!required(p.rdw, ints.iardw, rdw) || !required(p.rdh, ints.iardh, rdh) ||
                    !required(p.rdx, ints.iardx, rdx) || !required(p.rdy, ints.iardy, rdy))
                    return Status::Corrupt;
                int64_t size = 0;
                if (huffman) {
                    if (HuffmanValue(reader, p.rsize, size) != Next::Value || size < 0) return Status::Corrupt;
                    reader->Align();
                }
                RefinementParams rp;
                rp.templateId = p.refinementTemplate;
                std::memcpy(rp.at, p.refinementAt, sizeof rp.at);
                rp.reference = symbol;
                rp.dx = FloorHalf(rdw) + rdx;
                rp.dy = FloorHalf(rdh) + rdy;
                status = Allocate(scratch.refined, int64_t(symbol.width) + rdw, int64_t(symbol.height) + rdh, 0, budget);
                if (status != Status::Ok) return status;
                if (huffman) {
                    // An arithmetic-coded refinement over the next `size` bytes (6.4.11 step 7).
                    const size_t start = reader->Position();
                    const size_t left = reader->Size() - start;
                    MqDecoder local;
                    local.Start(reader->Data() + start, uint64_t(size) < left ? size_t(size) : left);
                    status = DecodeRefinement(local, grContexts, rp, scratch.refined, budget);
                    if (uint64_t(size) > left) return Status::Corrupt;
                    reader->Seek(start + size_t(size));
                } else {
                    status = DecodeRefinement(mq, grContexts, rp, scratch.refined, budget);
                }
                if (status != Status::Ok) return status;
                symbol = ViewOf(scratch.refined);
            }
            const int64_t wi = symbol.width;
            const int64_t hi = symbol.height;
            const int corner = p.refCorner;
            const bool right = corner == 2 || corner == 3;
            const bool bottom = corner == 0 || corner == 2;
            // 6.4.5 3c vi-x: CURS advances by the extent along S, before or after drawing.
            if (!p.transposed && right) curS += wi - 1;
            else if (p.transposed && bottom) curS += hi - 1;
            const int64_t s = curS;
            int64_t x = p.transposed ? t : s;
            int64_t y = p.transposed ? s : t;
            if (right) x -= wi - 1;
            if (bottom) y -= hi - 1;
            if (!budget.Spend(uint64_t(symbol.stride) * symbol.height)) return Status::TooLarge;
            Combine(out, symbol, x, y, p.combineOp);
            if (!p.transposed && !right) curS += wi - 1;
            else if (p.transposed && !bottom) curS += hi - 1;
            instances++;
            // 6.4.8: the strip ends with OOB. A strip that runs on past the instance count ends
            // the region (no conforming stream has one).
            const Next n = huffman ? HuffmanValue(reader, p.ds, value) : IntValue(mq, ints.iads, value);
            if (n == Next::Error) return Status::Corrupt;
            if (n == Next::Oob) break;
            if (instances >= p.numInstances) return Status::Ok;
            curS += value + p.dsOffset;
            if (curS > kMaxCoordinate || curS < -kMaxCoordinate) return Status::Corrupt;
        }
        if (huffman && reader->overrun) return Status::Corrupt;
    }
    return Status::Ok;
}

} // namespace RayoPdf::Image::Jbig2::Internal
