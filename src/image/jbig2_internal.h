// SPDX-License-Identifier: Apache-2.0
// Internals of the JBIG2 decoder (image/jbig2.h): bitmaps, the budget, the MQ arithmetic decoder
// (T.88 annex E), the arithmetic integer decoders (annex A), Huffman tables (annex B) and the
// region decoding procedures of T.88 section 6, shared by jbig2.cpp (segments and page),
// jbig2_coding.cpp (entropy coding) and jbig2_regions.cpp (the procedures).
#pragma once

#include "common/compiler.h"
#include "image/jbig2.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace RayoPdf::Image::Jbig2::Internal {

enum class Status : uint8_t { Ok, Unsupported, TooLarge, Corrupt };

// ---------------------------------------------------------------- bitmaps

// A bitmap in JBIG2's sense (1 = black): rows of `stride` bytes, the leftmost pixel in the most
// significant bit; padding bits are 0.
struct Bitmap {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t stride = 0;
    std::vector<uint8_t> data;
};

// A read-only bitmap: a Bitmap, a symbol or pattern in the pool, a part of the page.
struct View {
    const uint8_t* data = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t stride = 0;

    // The pixel at (x, y); 0 outside the bitmap (T.88 6.2.5.2, 6.3.5.2).
    RAYOPDF_HOT_INLINE uint32_t Pixel(int64_t x, int64_t y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        return (data[size_t(y) * stride + size_t(x >> 3)] >> (7 - (x & 7))) & 1u;
    }
};

inline View ViewOf(const Bitmap& b) { return View{b.data.data(), b.width, b.height, b.stride}; }

// Combination operators (T.88 7.4.1.5): OR, AND, XOR, XNOR, REPLACE.
enum class CombineOp : uint8_t { Or = 0, And = 1, Xor = 2, Xnor = 3, Replace = 4 };

// Work and memory limits of one decode (jbig2.h).
struct Budget {
    uint64_t maxPixels = 0;
    uint64_t work = 0;      // units left: pixels decoded or drawn, instances, codes
    uint64_t retained = 0;  // bytes left for retained bitmaps

    bool Spend(uint64_t units) {
        if (units > work) {
            work = 0;
            return false;
        }
        work -= units;
        return true;
    }
    bool Retain(uint64_t bytes) {
        if (bytes > retained) return false;
        retained -= bytes;
        return true;
    }
};

// Sizes `b` to width x height, every pixel `value`. TooLarge past budget.maxPixels.
Status Allocate(Bitmap& b, int64_t width, int64_t height, uint32_t value, const Budget& budget);
// Combines `src` into `dst` with its top left pixel at (x, y); parts outside `dst` are ignored.
void Combine(Bitmap& dst, const View& src, int64_t x, int64_t y, CombineOp op);

// ---------------------------------------------------------------- MQ arithmetic decoder (annex E)

// The Qe table (T.88 table E.1): Qe, NMPS, NLPS, SWITCH.
struct QeEntry {
    uint16_t qe;
    uint8_t nmps;
    uint8_t nlps;
    uint8_t switchMps;
};
extern const QeEntry kQeTable[47];

// A context: (index into kQeTable) << 1 | MPS. Zero is the reset state.
using Context = uint8_t;

// The decoder of T.88 annex E with the software conventions of annex G (the C register as in
// T.800 C.3); bytes past the end of the data read as 0xFF, as a marker would.
class MqDecoder {
public:
    void Start(const uint8_t* data, size_t size) {
        data_ = data;
        size_ = size;
        pos_ = 0;
        c_ = uint32_t(Byte(0)) << 16;
        ByteIn();
        c_ <<= 7;
        ct_ -= 7;
        a_ = 0x8000;
    }

    RAYOPDF_HOT_INLINE uint32_t Decode(Context& cx) {
        const QeEntry& e = kQeTable[cx >> 1];
        const uint32_t mps = cx & 1u;
        const uint32_t qe = e.qe;
        uint32_t d;
        a_ -= qe;
        if ((c_ >> 16) < qe) {
            // LPS exchange
            if (a_ < qe) {
                d = mps;
                cx = static_cast<Context>(e.nmps << 1 | mps);
            } else {
                d = mps ^ 1u;
                cx = static_cast<Context>(e.nlps << 1 | (mps ^ e.switchMps));
            }
            a_ = qe;
            Renormalize();
        } else {
            c_ -= qe << 16;
            if ((a_ & 0x8000) != 0) return mps;
            // MPS exchange
            if (a_ < qe) {
                d = mps ^ 1u;
                cx = static_cast<Context>(e.nlps << 1 | (mps ^ e.switchMps));
            } else {
                d = mps;
                cx = static_cast<Context>(e.nmps << 1 | mps);
            }
            Renormalize();
        }
        return d;
    }

    // Bytes of the data consumed so far (the byte the decoder reads next, as T.88 counts).
    size_t Position() const { return pos_; }

private:
    RAYOPDF_HOT_INLINE uint32_t Byte(size_t i) const { return i < size_ ? data_[i] : 0xFFu; }

    RAYOPDF_HOT_INLINE void ByteIn() {
        if (Byte(pos_) == 0xFF) {
            const uint32_t next = Byte(pos_ + 1);
            if (next > 0x8F) {
                c_ += 0xFF00;
                ct_ = 8;
            } else {
                pos_++;
                c_ += next << 9;
                ct_ = 7;
            }
        } else {
            pos_++;
            c_ += Byte(pos_) << 8;
            ct_ = 8;
        }
    }

    RAYOPDF_HOT_INLINE void Renormalize() {
        do {
            if (ct_ == 0) ByteIn();
            a_ <<= 1;
            c_ <<= 1;
            ct_--;
        } while ((a_ & 0x8000) == 0);
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    uint32_t c_ = 0;
    uint32_t a_ = 0;
    int ct_ = 0;
};

// An arithmetic integer decoder's contexts (annex A.2): 512 of them.
struct IntContexts {
    Context cx[512];
    void Reset() { std::memset(cx, 0, sizeof cx); }
};

// Annex A.2: false for OOB, else `value`.
bool DecodeInt(MqDecoder& mq, IntContexts& contexts, int64_t& value);
// Annex A.3, IAID with `codeLength` bits; `contexts` holds 1 << codeLength entries.
uint32_t DecodeIaid(MqDecoder& mq, Context* contexts, int codeLength);

// ---------------------------------------------------------------- Huffman coding (annex B)

// Reads bits most significant first; reads past the end set `overrun` and return 0 bits.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t ReadBit() {
        if (pos_ >= size_) {
            overrun = true;
            return 0;
        }
        const uint32_t bit = (data_[pos_] >> (7 - bit_)) & 1u;
        if (++bit_ == 8) {
            bit_ = 0;
            pos_++;
        }
        return bit;
    }
    // n <= 32 bits.
    uint32_t ReadBits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = v << 1 | ReadBit();
        return v;
    }
    // Skips the rest of the current byte.
    void Align() {
        if (bit_ != 0) {
            bit_ = 0;
            pos_++;
        }
    }
    // The byte position (after Align).
    size_t Position() const { return pos_; }
    void Seek(size_t position) {
        pos_ = position;
        bit_ = 0;
        if (pos_ > size_) {
            pos_ = size_;
            overrun = true;
        }
    }
    const uint8_t* Data() const { return data_; }
    size_t Size() const { return size_; }

    bool overrun = false;

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    int bit_ = 0;
};

// A table line (B.2): values low .. low + 2^rangeLength - 1, or the lower range line
// (low - offset, 32-bit offset; low = HTLOW - 1), or OOB.
struct HuffLine {
    int64_t low = 0;
    uint8_t prefixLength = 0;  // 0: no code
    uint8_t rangeLength = 0;
    uint8_t kind = 0;          // kLineNormal, kLineLower, kLineOob
};
constexpr uint8_t kLineNormal = 0;
constexpr uint8_t kLineLower = 1;
constexpr uint8_t kLineOob = 2;

// A table with the prefix codes of B.3: lines ordered by code.
struct HuffTable {
    std::vector<HuffLine> lines;
    uint32_t firstCode[33] = {};
    uint32_t count[33] = {};
    uint32_t offset[33] = {};
    int maxLength = 0;
    bool built = false;
};

// B.3 over `lines` (in table order). Corrupt when the lengths overflow the code space or exceed 32.
Status BuildTable(const HuffLine* lines, size_t n, HuffTable& table);
// 7.4.3.1.7: a text region's symbol ID Huffman table (run-coded code lengths for `count`
// symbols, then the rest of the byte skipped) into `table`; `lines` is scratch.
Status ReadSymbolCodes(BitReader& reader, uint32_t count, std::vector<HuffLine>& lines, HuffTable& table);
// Standard table B.1 .. B.15.
const HuffTable& StandardTable(int number);
// A code table segment (B.2) into `table`.
Status ParseTableSegment(const uint8_t* data, size_t size, HuffTable& table);

enum class Code : uint8_t { Value, Oob, Error };
// B.4: one value with `table`.
Code DecodeHuffman(BitReader& reader, const HuffTable& table, int64_t& value);

// ---------------------------------------------------------------- region decoding procedures

struct GenericParams {
    int templateId = 0;
    bool tpgdon = false;
    bool extTemplate = false;
    int at[8] = {};             // AT pixels A1..A4 as (x, y) pairs (a pattern dictionary's A1 is -HDPW)
    const Bitmap* skip = nullptr;  // USESKIP: 1 = pixel not coded (0)
};

// 6.2.5: `out` (allocated, all 0) decoded with `contexts` (GB statistics, 1 << 16 entries).
Status DecodeGeneric(MqDecoder& mq, Context* contexts, const GenericParams& params, Bitmap& out, Budget& budget);
// 6.2.6: `out` (allocated, all 0) from MMR data; `used` = bytes consumed (with EOFB).
Status DecodeMmr(const uint8_t* data, size_t size, Bitmap& out, size_t& used, Budget& budget);

struct RefinementParams {
    int templateId = 0;
    bool tpgron = false;
    int8_t at[4] = {};  // RA1 (in the refined bitmap), RA2 (in the reference)
    View reference;
    int64_t dx = 0;     // GRREFERENCEDX, GRREFERENCEDY
    int64_t dy = 0;
};

// 6.3.5: `out` (allocated, all 0) decoded with `contexts` (GR statistics, 1 << 13 entries).
Status DecodeRefinement(MqDecoder& mq, Context* contexts, const RefinementParams& params, Bitmap& out,
                        Budget& budget);

// The integer decoders of symbol dictionaries and text regions, reset per segment.
struct IntDecoders {
    IntContexts iadh, iadw, iaex, iaai, iadt, iafs, iads, iait, iari, iardw, iardh, iardx, iardy;
    std::vector<Context> iaid;
    void Reset(int symbolCodeLength) {
        for (IntContexts* c : {&iadh, &iadw, &iaex, &iaai, &iadt, &iafs, &iads, &iait, &iari, &iardw, &iardh, &iardx,
                               &iardy})
            c->Reset();
        iaid.assign(size_t(1) << symbolCodeLength, 0);
    }
};

// Symbol and pattern storage: the bitmaps of every dictionary in one pool.
struct SymbolRef {
    size_t offset = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct SymbolPool {
    std::vector<uint8_t> pixels;
    std::vector<SymbolRef> symbols;

    View Get(uint32_t id) const {
        const SymbolRef& s = symbols[id];
        return View{pixels.data() + s.offset, s.width, s.height, (size_t(s.width) + 7) / 8};
    }
    // Appends columns [x, x + width) of `src` as a new symbol; returns its id.
    Status Append(const View& src, uint32_t x, uint32_t width, Budget& budget, uint32_t& id);
};

// The text region decoding procedure's parameters (6.4.2) plus the decoders it reads from.
struct TextParams {
    bool huffman = false;
    bool refine = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t defaultPixel = 0;
    uint32_t numInstances = 0;
    int logStrips = 0;
    const uint32_t* symbols = nullptr;  // SBSYMS: pool ids
    uint32_t numSymbols = 0;
    int symbolCodeLength = 0;           // IAID bits; Huffman fixed-length codes without symbolCodes
    const HuffTable* symbolCodes = nullptr;
    bool transposed = false;
    int dsOffset = 0;
    int refCorner = 0;                  // 0 BOTTOMLEFT, 1 TOPLEFT, 2 BOTTOMRIGHT, 3 TOPRIGHT
    CombineOp combineOp = CombineOp::Or;
    const HuffTable* fs = nullptr;
    const HuffTable* ds = nullptr;
    const HuffTable* dt = nullptr;
    const HuffTable* rdw = nullptr;
    const HuffTable* rdh = nullptr;
    const HuffTable* rdx = nullptr;
    const HuffTable* rdy = nullptr;
    const HuffTable* rsize = nullptr;
    int refinementTemplate = 0;
    int8_t refinementAt[4] = {};
};

// Scratch bitmaps of the procedures, reused across symbols and instances.
struct Scratch {
    Bitmap refined;
};

// 6.4: decodes into `out` (allocated here). Arithmetic: `mq` with `ints` and `grContexts`;
// Huffman: `reader` (refinements decode with an arithmetic decoder of their own over the
// RSIZE bytes).
Status DecodeText(const TextParams& params, MqDecoder& mq, IntDecoders& ints, Context* grContexts, BitReader* reader,
                  const SymbolPool& pool, Scratch& scratch, Bitmap& out, Budget& budget);

} // namespace RayoPdf::Image::Jbig2::Internal
