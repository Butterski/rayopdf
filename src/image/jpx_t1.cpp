// SPDX-License-Identifier: Apache-2.0
// Tier-1 of the JPEG 2000 decoder: the MQ arithmetic decoder (T.800 annex C) and the embedded
// block decoding of one code-block (annex D): significance propagation, magnitude refinement and
// cleanup passes with the run-length mode, the raw (bypass) passes, and the reset, termination,
// vertically causal and segmentation-symbol styles. The reconstruction follows OpenJPEG: values
// carry one fractional bit and a newly significant coefficient starts at the midpoint of its
// interval (1.5 at its bit-plane), which a refinement bit moves up or down by half a step.
//
// Flags: one 32-bit word per column of a stripe (four rows), OpenJPEG's layout. Bits 0-17 hold
// the significance of the six rows from the one above the stripe to the one below it, in this
// column and its two neighbours: row r (-1 to 4) at bit 3 * (r + 1), west, this column, east. So
// the 3 x 3 neighbourhood of row k is the nine bits from 3k, its zero-coding context one table
// lookup, and a coefficient that becomes significant sets one bit in its own word and in each
// neighbouring column's (and in the next or previous stripe's for the first and last row). Bit 18
// is the sign of the row above, bit 31 the sign of the row below; row k has its sign, refined and
// visited bits at 19, 20 and 21 plus 3k. A column whose word is zero has nothing to code in the
// significance pass and starts the run-length mode in the cleanup pass. With the vertically causal
// style the first row never marks the stripe above, so the last row of a stripe never sees the
// next one.
#include "image/jpx_internal.h"

#include <algorithm>
#include <cstring>

namespace RayoPdf::Image::Jpx::Internal {
namespace {

constexpr uint32_t kSigThis = 1u << 4;     // row 0's own significance (shifted by 3k for row k)
constexpr uint32_t kSignShift = 19;        // row 0's sign
constexpr uint32_t kRefined = 1u << 20;    // row 0 has had a magnitude refinement bit
constexpr uint32_t kVisited = 1u << 21;    // row 0 was coded in this bit-plane's significance pass
constexpr uint32_t kNeighbours = 0x1EF;    // the 3 x 3 window without its centre
constexpr uint32_t kSigRows = kSigThis * 0x249u;     // own significance of the four rows
constexpr uint32_t kVisitedRows = kVisited * 0x249u;  // visited bits of the four rows

// Contexts (T.800 tables D.1 to D.4): 0-8 zero coding, 9-13 sign, 14-16 magnitude refinement,
// 17 run length, 18 uniform.
constexpr int kCtxRunLength = 17;
constexpr int kCtxUniform = 18;
constexpr int kContexts = 19;

struct Luts {
    uint8_t zeroCoding[4][512];  // by orientation (LL, HL, LH, HH) and the 3 x 3 window
    // By the direct neighbours: bits 1, 3, 5, 7 the significance of N, W, E, S (as in the
    // window), bits 0, 2, 4, 6 the signs of W, E, N, S. Context | (xor bit << 7).
    uint8_t sign[256];
};

constexpr Luts MakeLuts() {
    Luts luts{};
    for (int orientation = 0; orientation < 4; orientation++) {
        for (int f = 0; f < 512; f++) {
            // Window bits: 0 NW, 1 N, 2 NE, 3 W, 4 centre, 5 E, 6 SW, 7 S, 8 SE.
            int h = ((f >> 3) & 1) + ((f >> 5) & 1);
            int v = ((f >> 1) & 1) + ((f >> 7) & 1);
            const int d = (f & 1) + ((f >> 2) & 1) + ((f >> 6) & 1) + ((f >> 8) & 1);
            int n = 0;
            if (orientation == 3) {
                const int hv = h + v;
                if (d == 0) n = hv == 0 ? 0 : (hv == 1 ? 1 : 2);
                else if (d == 1) n = hv == 0 ? 3 : (hv == 1 ? 4 : 5);
                else if (d == 2) n = hv == 0 ? 6 : 7;
                else n = 8;
            } else {
                if (orientation == 1) {  // HL: the vertical neighbours lead
                    const int t = h;
                    h = v;
                    v = t;
                }
                if (h == 0) {
                    if (v == 0) n = d == 0 ? 0 : (d == 1 ? 1 : 2);
                    else n = v == 1 ? 3 : 4;
                } else if (h == 1) {
                    n = v == 0 ? (d == 0 ? 5 : 6) : 7;
                } else {
                    n = 8;
                }
            }
            luts.zeroCoding[orientation][f] = static_cast<uint8_t>(n);
        }
    }
    for (int i = 0; i < 256; i++) {
        auto contribution = [i](int sigBit, int sgnBit) { return (i & sigBit) ? ((i & sgnBit) ? -1 : 1) : 0; };
        int h = contribution(8, 1) + contribution(32, 4);
        int v = contribution(2, 16) + contribution(128, 64);
        h = h > 0 ? 1 : (h < 0 ? -1 : 0);
        v = v > 0 ? 1 : (v < 0 ? -1 : 0);
        int context = 9;
        int flip = 0;
        if (h == 0) {
            context = v == 0 ? 9 : 10;
            flip = v < 0;
        } else {
            context = v == 0 ? 12 : (v == h ? 13 : 11);
            flip = h < 0;
        }
        luts.sign[i] = static_cast<uint8_t>(context | (flip << 7));
    }
    return luts;
}

constexpr Luts kLuts = MakeLuts();

// T.800 table C.2: Qe, the next state after an MPS and after an LPS, and whether an LPS switches
// the MPS sense.
struct MqRow {
    uint16_t qe;
    uint8_t nmps;
    uint8_t nlps;
    uint8_t switchMps;
};
constexpr MqRow kMqRows[47] = {
    {0x5601, 1, 1, 1},   {0x3401, 2, 6, 0},   {0x1801, 3, 9, 0},   {0x0AC1, 4, 12, 0},  {0x0521, 5, 29, 0},
    {0x0221, 38, 33, 0}, {0x5601, 7, 6, 1},   {0x5401, 8, 14, 0},  {0x4801, 9, 14, 0},  {0x3801, 10, 14, 0},
    {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0}, {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0}, {0x1C01, 25, 22, 0},
    {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0},
    {0x02A1, 36, 33, 0}, {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0},
};

// A context is one word: its state's Qe in the top 16 bits (where the decoder keeps A and
// Chigh), the state index times two plus the MPS in the low bits, so a decision reads Qe and the
// MPS without another load. kMqNext gives the words after decoding the MPS ([0]) and the LPS ([1]).
constexpr uint64_t MqWord(int state, int mps) { return (uint64_t(kMqRows[state].qe) << 48) | uint64_t(2 * state + mps); }

struct MqNext {
    uint64_t next[94][2];
};
constexpr MqNext MakeMqNext() {
    MqNext t{};
    for (int state = 0; state < 47; state++) {
        for (int mps = 0; mps < 2; mps++) {
            t.next[2 * state + mps][0] = MqWord(kMqRows[state].nmps, mps);
            t.next[2 * state + mps][1] = MqWord(kMqRows[state].nlps, mps ^ kMqRows[state].switchMps);
        }
    }
    return t;
}
constexpr MqNext kMqNext = MakeMqNext();

void ResetContexts(uint64_t* contexts) {
    for (int i = 0; i < kContexts; i++) contexts[i] = MqWord(0, 0);
    contexts[0] = MqWord(4, 0);
    contexts[kCtxRunLength] = MqWord(3, 0);
    contexts[kCtxUniform] = MqWord(46, 0);
}

// Each pass is a function of its own, so that its loop has the registers to itself.
#if defined(__GNUC__)
#define JPX_INLINE inline __attribute__((always_inline))
#define JPX_NOINLINE __attribute__((noinline))
#else
#define JPX_INLINE inline
#define JPX_NOINLINE
#endif

// The index of the lowest set bit of `v` (not zero).
JPX_INLINE int LowestBit(uint64_t v) {
#if defined(__GNUC__)
    return __builtin_ctzll(v);
#else
    int n = 0;
    while ((v & 1u) == 0) {
        v >>= 1;
        n++;
    }
    return n;
#endif
}

// The number of leading zero bits of `v` (not zero).
JPX_INLINE uint32_t LeadingZeros(uint64_t v) {
#if defined(__GNUC__)
    return static_cast<uint32_t>(__builtin_clzll(v));
#else
    uint32_t n = 0;
    while ((v & 0x8000000000000000u) == 0) {
        v <<= 1;
        n++;
    }
    return n;
#endif
}

// The MQ decoder (C.3) as OpenJPEG implements it: the segment ends in a 0xFF 0xFF marker that
// BYTEIN never passes, feeding 1-bits. The passes copy it into a local, so its registers stay in
// machine registers.
//
// C is held in 64 bits: OpenJPEG's 32-bit C register is bits 32-63 (Chigh, compared with Qe, is
// bits 48-63), and below it CT bits of bytes read ahead, up to 48. Bytes come in as BYTEIN takes
// them (a byte after 0xFF gives 7 bits, the marker endless 1-bits), only earlier, which adds the
// same bits at the same places into bits that are still zero, so the decisions do not change.
// The one exception waits: a byte after 0xFF with its top bit set carries into the 0xFF byte, so
// it is added only when CT runs out, as BYTEIN adds it. A is held at bits 48-63 too, its low
// bits zero, as Qe is in a context word.
struct Mq {
    const uint8_t* bp;  // the last byte taken in
    uint64_t a;
    uint32_t ct;
    uint64_t c;

    // The input side, passed by value to the rare refills so that the decoder itself never
    // has its address taken and stays in registers.
    struct Feed {
        const uint8_t* bp;
        uint64_t c;
        uint32_t ct;
    };

    void Init(const uint8_t* start) {
        const Feed in = Refill(Feed{start, static_cast<uint64_t>(*start) << 48, 0});
        bp = in.bp;
        c = in.c << 7;
        ct = in.ct - 7;
        a = uint64_t(0x8000) << 48;
    }

    // Takes in bytes while CT leaves room for one; at least one when CT is 0.
    static Feed Refill(Feed in) {
        while (in.ct <= 40) {
            const uint32_t next = in.bp[1];
            if (*in.bp == 0xFF) {
                if (next > 0x8F) {
                    in.c += uint64_t(0xFF) << (40 - in.ct);
                    in.ct += 8;
                    continue;
                }
                if (next >= 0x80 && in.ct != 0) break;
                in.bp++;
                in.c += uint64_t(next) << (41 - in.ct);
                in.ct += 7;
            } else {
                in.bp++;
                in.c += uint64_t(next) << (40 - in.ct);
                in.ct += 8;
            }
        }
        return in;
    }

    // Shifts C left by n (more than CT), taking in bytes, each when CT runs out.
    static Feed ShiftIn(Feed in, uint32_t n) {
        in = Refill(in);
        while (n > 0) {
            if (in.ct == 0) in = Refill(in);
            const uint32_t s = n < in.ct ? n : in.ct;
            in.c <<= s;
            in.ct -= s;
            n -= s;
        }
        return in;
    }

    // Shifts A (below 0x8000, not zero) and C left until A reaches 0x8000: at once when CT
    // covers the shift, else taking in bytes.
    JPX_INLINE void Renormalize() {
        const uint32_t n = LeadingZeros(a);
        a <<= n;
        if (n <= ct) {
            c <<= n;
            ct -= n;
            return;
        }
        const Feed in = ShiftIn(Feed{bp, c, ct}, n);
        bp = in.bp;
        c = in.c;
        ct = in.ct;
    }

    // C.3.2 with the conditional exchanges. The common case, the MPS without renormalisation,
    // is one branch; the others are worked out without branches: `lower` is whether Chigh lies
    // below Qe, `lps` whether the decision is the LPS.
    JPX_INLINE uint32_t Decode(uint64_t& context) {
        const uint64_t word = context;
        const uint64_t qe = word & ~uint64_t(0xFFFF);
        a -= qe;
        const uint32_t lower = c < qe;
        if ((lower | static_cast<uint32_t>((a >> 63) ^ 1u)) == 0) {
            c -= qe;
            return static_cast<uint32_t>(word) & 1u;
        }
        const uint32_t lps = lower ^ static_cast<uint32_t>(a < qe);
        c -= qe & (uint64_t(lower) - 1);
        a = lower ? qe : a;
        context = kMqNext.next[word & 0x7F][lps];
        Renormalize();
        return (static_cast<uint32_t>(word) & 1u) ^ lps;
    }
};

// The raw decoder of the bypass passes (D.6), with OpenJPEG's handling of the end marker.
struct Raw {
    const uint8_t* bp;
    uint32_t c;
    uint32_t ct;

    void Init(const uint8_t* start) {
        bp = start;
        c = 0;
        ct = 0;
    }

    JPX_INLINE uint32_t Decode() {
        if (ct == 0) {
            if (c == 0xFF) {
                if (*bp > 0x8F) {
                    c = 0xFF;
                    ct = 8;
                } else {
                    c = *bp++;
                    ct = 7;
                }
            } else {
                c = *bp++;
                ct = 8;
            }
        }
        ct--;
        return (c >> ct) & 1u;
    }
};

// The code-block's flags: stripe by stripe, a word per column with a border column on each side,
// and a border stripe above and below. The passes keep the current column's word in a local and
// store it when they leave the column.
struct Block {
    int width;
    int height;
    ptrdiff_t stride;  // words per stripe: width + 2
    uint32_t* flags;   // stripe 0, column 0
    int32_t* data;
    const uint8_t* zeroCoding;
    bool causal;
    size_t rowAt[14];  // by a row's own significance bit (4 + 3k): its offset in `data`, k * width
    // By a bit of two adjacent column words read as one 64-bit word (the second at bit 32 + 4 +
    // 3k): the row's offset in `data` from the first column.
    size_t pairAt[46];
    uint64_t contexts[kContexts];

    // The row at `shift` (3k) of the column at `fp` (local word f) became significant with sign s.
    JPX_INLINE void SetSignificant(uint32_t& f, uint32_t* fp, int shift, uint32_t s) const {
        f |= (kSigThis | (s << kSignShift)) << shift;
        fp[-1] |= (1u << 5) << shift;
        fp[1] |= (1u << 3) << shift;
        if (shift == 0 && !causal) {
            uint32_t* north = fp - stride;
            north[0] |= (1u << 16) | (s << 31);
            north[-1] |= 1u << 17;
            north[1] |= 1u << 15;
        }
        if (shift == 9) {
            uint32_t* south = fp + stride;
            south[0] |= (1u << 1) | (s << 18);
            south[-1] |= 1u << 2;
            south[1] |= 1u << 0;
        }
    }

    // The sign context index of the row at `shift` (Luts::sign). The sign of the row above is bit
    // 18 for row 0, else that row's sign bit 19 + shift - 3.
    JPX_INLINE static uint32_t SignIndex(uint32_t f, const uint32_t* fp, int shift) {
        uint32_t lu = (f >> shift) & 0xAAu;
        lu |= (fp[-1] >> (kSignShift + shift)) & 1u;
        lu |= (fp[1] >> (kSignShift - 2 + shift)) & 4u;
        lu |= (f >> (12 + shift + (shift == 0 ? 2 : 0))) & 16u;
        lu |= (f >> (16 + shift)) & 64u;
        return lu;
    }

    JPX_INLINE void DecodeSign(Mq& mq, uint32_t& f, uint32_t* fp, int shift, int32_t* d, int32_t oneplushalf) {
        const uint8_t sc = kLuts.sign[SignIndex(f, fp, shift)];
        const uint32_t s = mq.Decode(contexts[sc & 0x1F]) ^ (sc >> 7);
        *d = s ? -oneplushalf : oneplushalf;
        SetSignificant(f, fp, shift, s);
    }

    // The rows of the column word `f` that the significance pass codes, as their own significance
    // bits (kSigThis << 3k): not significant, not yet visited, and with a significant neighbour
    // in the 3 x 3 window (rows k - 1 and k + 1 whole, W and E of row k).
    JPX_INLINE static uint32_t SignificanceRows(uint32_t f, uint32_t rowMask) {
        const uint32_t g = f & 0x3FFFFu;
        const uint32_t rowAny = (g | (g >> 1) | (g >> 2)) & 0x9249u;  // bit 3j: a row j - 1 bit set
        const uint32_t neighbours = (rowAny << 4) | (rowAny >> 2) | (g << 1) | (g >> 1);
        return neighbours & ~(f | (f >> 17)) & rowMask;
    }

    // The row at `shift`'s zero coding (and its sign when it becomes significant); the row is
    // marked visited. A newly significant row makes the row below it a candidate of `rows`.
    JPX_INLINE void SignificanceRow(Mq& mq, uint32_t& f, uint32_t* fp, int shift, int32_t* d, int32_t oneplushalf,
                                    uint32_t& rows, uint32_t rowMask) {
        const uint32_t window = f >> shift;
        if (mq.Decode(contexts[zeroCoding[window & 0x1FF]])) {
            DecodeSign(mq, f, fp, shift, d, oneplushalf);
            rows |= (kSigThis << (shift + 3)) & ~(f | (f >> 17)) & rowMask;
        }
        f |= kVisited << shift;
    }

    JPX_INLINE void SignificanceStepRaw(Raw& raw, uint32_t& f, uint32_t* fp, int k, int32_t* d, int32_t oneplushalf) {
        const uint32_t window = f >> (3 * k);
        if ((window & (kSigThis | kVisited)) != 0 || (window & kNeighbours) == 0) return;
        if (raw.Decode()) {
            const uint32_t s = raw.Decode();
            *d = s ? -oneplushalf : oneplushalf;
            SetSignificant(f, fp, 3 * k, s);
        }
        f |= kVisited << (3 * k);
    }

    // The refinement contexts of two adjacent columns' rows (the words read as one 64-bit word),
    // two bits a row at its own significance bit (4 + 3k, 32 + 4 + 3k): context 14 plus 2 after
    // the row's first refinement, else plus 1 with a significant neighbour. Bits that one half
    // shifts into the other are masked off. The refinement pass changes no significance, so the
    // contexts hold for the whole pass over the two columns.
    JPX_INLINE static uint64_t RefinementContexts(uint64_t pair) {
        constexpr uint64_t kBoth = 0x100000001u;
        const uint64_t g = pair & (0x3FFFFu * kBoth);
        const uint64_t rowAny = (g | (g >> 1) | (g >> 2)) & (0x9249u * kBoth);
        const uint64_t sigRows = kSigRows * kBoth;
        const uint64_t neighbours = ((rowAny << 4) | (rowAny >> 2) | (g << 1) | (g >> 1)) & sigRows;
        const uint64_t refined = (pair >> 16) & sigRows;
        return (neighbours & ~refined) | (refined << 1);
    }

    // The magnitude refinement of the row at bit `bit` of the column pair whose contexts are
    // `selector`.
    JPX_INLINE void RefinementRow(Mq& mq, uint64_t selector, int bit, int32_t* d, int32_t half) {
        const uint32_t refine = mq.Decode(contexts[14 + ((selector >> bit) & 3u)]);
        *d += (refine ^ static_cast<uint32_t>(*d < 0)) ? half : -half;
    }

    JPX_INLINE static void RefinementStepRaw(Raw& raw, uint32_t& f, int k, int32_t* d, int32_t half) {
        const uint32_t window = f >> (3 * k);
        if ((window & (kSigThis | kVisited)) != kSigThis) return;
        const uint32_t bit = raw.Decode();
        *d += (bit ^ static_cast<uint32_t>(*d < 0)) ? half : -half;
        f |= kRefined << (3 * k);
    }

    // The row at `shift`'s zero coding in the cleanup pass (the row is neither significant nor
    // visited).
    JPX_INLINE void CleanupRow(Mq& mq, uint32_t& f, uint32_t* fp, int shift, int32_t* d, int32_t oneplushalf) {
        if (mq.Decode(contexts[zeroCoding[(f >> shift) & 0x1FF]])) DecodeSign(mq, f, fp, shift, d, oneplushalf);
    }

    // The rows of a stripe of `rows` rows, as their own significance bits.
    static uint32_t RowMask(int rows) { return kSigRows & ((kSigThis << (3 * rows)) - 1); }

    // Each pass codes the rows of a column it has to, found from the column word, lowest first:
    // a set bit at 4 + 3k is row k, so the bit's index less 4 is the row's shift in the word.

    JPX_NOINLINE void SignificancePass(Mq& state, int bp) {
        Mq mq = state;
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const uint32_t rowMask = RowMask(std::min(4, height - y0));
            uint32_t* const start = flags + stripe * stride;
            uint32_t* const end = start + w;
            int32_t* const dStart = data + static_cast<size_t>(y0) * w;
            // Columns without a flag are skipped in a loop of their own, so that the pointers
            // the coding derives from the column's are not stepped along for every column.
            for (uint32_t* fp = start;; fp++) {
                while (fp < end && *fp == 0) fp++;
                if (fp >= end) break;
                uint32_t f = *fp;
                uint32_t rows = SignificanceRows(f, rowMask);
                if (rows == 0) continue;
                int32_t* const d = dStart + (fp - start);
                do {
                    const int bit = LowestBit(rows);
                    rows &= rows - 1;
                    SignificanceRow(mq, f, fp, bit - 4, d + rowAt[bit], oneplushalf, rows, rowMask);
                } while (rows != 0);
                *fp = f;
            }
        }
        state = mq;
    }

    void SignificancePassRaw(Raw& state, int bp) {
        Raw raw = state;
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint32_t* fp = flags + stripe * stride;
            int32_t* d = data + static_cast<size_t>(y0) * w;
            for (size_t x = 0; x < w; x++, fp++, d++) {
                uint32_t f = *fp;
                if (f == 0) continue;
                for (int k = 0; k < rows; k++) SignificanceStepRaw(raw, f, fp, k, d + k * w, oneplushalf);
                *fp = f;
            }
        }
        state = raw;
    }

    JPX_NOINLINE void RefinementPass(Mq& state, int bp) {
        Mq mq = state;
        const int32_t half = (1 << bp) >> 1;
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const uint64_t rowMask = RowMask(std::min(4, height - y0));
            const uint64_t pairMask = rowMask | (rowMask << 32);
            uint32_t* fp = flags + stripe * stride;
            uint32_t* const end = fp + w;
            int32_t* d = data + static_cast<size_t>(y0) * w;
            // Two columns at a time: every bit operation stays within its 32-bit half where
            // it matters, and the border column after an odd last one has no significant row.
            for (; fp < end; fp += 2, d += 2) {
                uint64_t pair;
                std::memcpy(&pair, fp, sizeof pair);
                // Significant and not visited in this bit-plane's significance pass.
                const uint64_t candidates = pair & ~(pair >> 17) & pairMask;
                if (candidates == 0) continue;
                const uint64_t selector = RefinementContexts(pair);
                const uint64_t refined = pair | (candidates << 16);
                std::memcpy(fp, &refined, sizeof refined);
                uint64_t rows = candidates;
                do {
                    const int bit = LowestBit(rows);
                    rows &= rows - 1;
                    RefinementRow(mq, selector, bit, d + pairAt[bit], half);
                } while (rows != 0);
            }
        }
        state = mq;
    }

    void RefinementPassRaw(Raw& state, int bp) {
        Raw raw = state;
        const int32_t half = (1 << bp) >> 1;
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint32_t* fp = flags + stripe * stride;
            int32_t* d = data + static_cast<size_t>(y0) * w;
            for (size_t x = 0; x < w; x++, fp++, d++) {
                uint32_t f = *fp;
                if ((f & kSigRows) == 0) continue;
                for (int k = 0; k < rows; k++) RefinementStepRaw(raw, f, k, d + k * w, half);
                *fp = f;
            }
        }
        state = raw;
    }

    JPX_NOINLINE void CleanupPass(Mq& state, int bp, bool segmentation) {
        Mq mq = state;
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint32_t* fp = flags + stripe * stride;
            int32_t* d = data + static_cast<size_t>(y0) * w;
            if (rows == 4) {
                uint32_t* const start = fp;
                uint32_t* const end = start + w;
                int32_t* const dStart = d;
                for (fp = start;; fp++) {
                    // Run-length mode (no row significant, visited or with a significant
                    // neighbour) with a run-length decision of 0 codes nothing else: such
                    // columns are passed in a loop of their own.
                    while (fp < end && *fp == 0 && !mq.Decode(contexts[kCtxRunLength])) fp++;
                    if (fp >= end) break;
                    uint32_t f = *fp;
                    int32_t* const column = dStart + (fp - start);
                    uint32_t todo;
                    if (f == 0) {
                        // The run-length decision was 1: the first significant row follows,
                        // then the rows below it are coded as usual.
                        uint32_t k = mq.Decode(contexts[kCtxUniform]) << 1;
                        k |= mq.Decode(contexts[kCtxUniform]);
                        DecodeSign(mq, f, fp, static_cast<int>(3 * k), column + rowAt[4 + 3 * k], oneplushalf);
                        todo = kSigRows & ~((kSigThis << (3 * k + 3)) - 1);
                    } else {
                        todo = ~(f | (f >> 17)) & kSigRows;
                    }
                    while (todo != 0) {
                        const int bit = LowestBit(todo);
                        todo &= todo - 1;
                        CleanupRow(mq, f, fp, bit - 4, column + rowAt[bit], oneplushalf);
                    }
                    *fp = f & ~kVisitedRows;
                }
            } else {
                const uint32_t rowMask = RowMask(rows);
                for (size_t x = 0; x < w; x++, fp++, d++) {
                    uint32_t f = *fp;
                    uint32_t todo = ~(f | (f >> 17)) & rowMask;
                    while (todo != 0) {
                        const int bit = LowestBit(todo);
                        todo &= todo - 1;
                        CleanupRow(mq, f, fp, bit - 4, d + rowAt[bit], oneplushalf);
                    }
                    *fp = f & ~kVisitedRows;
                }
            }
        }
        if (segmentation) {
            for (int i = 0; i < 4; i++) mq.Decode(contexts[kCtxUniform]);
        }
        state = mq;
    }
};

} // namespace

bool T1Decoder::Decode(const CodeBlockJob& job, int32_t* out) {
    const int width = job.width;
    const int height = job.height;
    std::fill(out, out + static_cast<size_t>(width) * height, 0);
    int bp = job.roiShift + job.bitplanes;  // OpenJPEG's bpno_plus_one
    if (bp >= 31) return false;

    Block block;
    block.width = width;
    block.height = height;
    block.stride = static_cast<ptrdiff_t>(width) + 2;
    const size_t words = static_cast<size_t>(block.stride) * (static_cast<size_t>(height + 3) / 4 + 2);
    if (flags_.size() < words) flags_.resize(words);
    std::memset(flags_.data(), 0, words * sizeof(uint32_t));
    block.flags = flags_.data() + block.stride + 1;
    block.data = out;
    block.zeroCoding = kLuts.zeroCoding[job.orientation & 3];
    block.causal = (job.style & kStyleCausal) != 0;
    for (int k = 0; k < 4; k++) {
        const size_t row = static_cast<size_t>(k) * static_cast<size_t>(width);
        block.rowAt[4 + 3 * k] = row;
        block.pairAt[4 + 3 * k] = row;
        block.pairAt[36 + 3 * k] = row + 1;
    }
    ResetContexts(block.contexts);

    int passType = 2;  // the first pass is a cleanup pass
    uint8_t* segment = job.data;
    Mq mq{};
    Raw raw{};
    for (int s = 0; s < job.segments; s++) {
        const uint32_t length = job.segmentLengths[s];
        const bool isRaw = (job.style & kStyleBypass) && passType < 2 && bp <= job.bitplanes - 4;
        uint8_t* end = segment + length;
        const uint8_t saved[2] = {end[0], end[1]};
        end[0] = 0xFF;
        end[1] = 0xFF;
        if (isRaw) raw.Init(segment);
        else mq.Init(segment);
        for (uint32_t pass = 0; pass < job.segmentPasses[s] && bp >= 1; pass++) {
            switch (passType) {
            case 0:
                if (isRaw) block.SignificancePassRaw(raw, bp);
                else block.SignificancePass(mq, bp);
                break;
            case 1:
                if (isRaw) block.RefinementPassRaw(raw, bp);
                else block.RefinementPass(mq, bp);
                break;
            default:
                block.CleanupPass(mq, bp, (job.style & kStyleSegmentation) != 0);
                break;
            }
            if ((job.style & kStyleReset) && !isRaw) ResetContexts(block.contexts);
            if (++passType == 3) {
                passType = 0;
                bp--;
            }
        }
        end[0] = saved[0];
        end[1] = saved[1];
        segment = end;
    }

    if (job.roiShift > 0) {
        const size_t count = static_cast<size_t>(width) * height;
        if (job.roiShift >= 31) {
            std::fill(out, out + count, 0);
        } else {
            const int32_t threshold = int32_t(1) << job.roiShift;
            for (size_t i = 0; i < count; i++) {
                const int32_t v = out[i];
                int32_t magnitude = v < 0 ? -v : v;
                if (magnitude >= threshold) {
                    magnitude >>= job.roiShift;
                    out[i] = v < 0 ? -magnitude : magnitude;
                }
            }
        }
    }
    return true;
}

} // namespace RayoPdf::Image::Jpx::Internal
