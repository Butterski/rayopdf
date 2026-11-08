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

// A context is one word: its state's Qe in the high half, the state index times two plus the MPS
// in the low bits, so a decision reads Qe and the MPS without another load. kMqNext gives the
// words after decoding the MPS ([0]) and the LPS ([1]).
constexpr uint32_t MqWord(int state, int mps) { return (uint32_t(kMqRows[state].qe) << 16) | uint32_t(2 * state + mps); }

struct MqNext {
    uint32_t next[94][2];
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

void ResetContexts(uint32_t* contexts) {
    for (int i = 0; i < kContexts; i++) contexts[i] = MqWord(0, 0);
    contexts[0] = MqWord(4, 0);
    contexts[kCtxRunLength] = MqWord(3, 0);
    contexts[kCtxUniform] = MqWord(46, 0);
}

#if defined(__GNUC__)
#define JPX_INLINE inline __attribute__((always_inline))
#else
#define JPX_INLINE inline
#endif

// The MQ decoder (C.3) as OpenJPEG implements it: the segment ends in a 0xFF 0xFF marker that
// BYTEIN never passes, feeding 1-bits. The passes copy it into a local, so its registers stay in
// machine registers.
struct Mq {
    const uint8_t* bp;
    uint32_t a;
    uint32_t c;
    uint32_t ct;

    void Init(const uint8_t* start) {
        bp = start;
        c = static_cast<uint32_t>(*bp) << 16;
        ByteIn();
        c <<= 7;
        ct -= 7;
        a = 0x8000;
    }

    JPX_INLINE void ByteIn() {
        const uint32_t next = bp[1];
        if (*bp == 0xFF) {
            if (next > 0x8F) {
                c += 0xFF00;
                ct = 8;
            } else {
                bp++;
                c += next << 9;
                ct = 7;
            }
        } else {
            bp++;
            c += next << 8;
            ct = 8;
        }
    }

    // Shifts A (below 0x8000, not zero) and C left until A reaches 0x8000, reading a byte each
    // time CT runs out: the shifts at once when CT covers them.
    JPX_INLINE void Renormalize() {
#if defined(__GNUC__)
        const uint32_t n = static_cast<uint32_t>(__builtin_clz(a)) - 16;
        if (n <= ct) {
            a <<= n;
            c <<= n;
            ct -= n;
            return;
        }
#endif
        do {
            if (ct == 0) ByteIn();
            a <<= 1;
            c <<= 1;
            ct--;
        } while (a < 0x8000);
    }

    // C.3.2 with the conditional exchanges: `lps` is whether the decision is the LPS.
    JPX_INLINE uint32_t Decode(uint32_t& context) {
        const uint32_t word = context;
        const uint32_t qe = word >> 16;
        const uint32_t mps = word & 1u;
        a -= qe;
        if ((c >> 16) < qe) {
            const uint32_t lps = a < qe ? 0 : 1;
            context = kMqNext.next[word & 0x7F][lps];
            a = qe;
            Renormalize();
            return mps ^ lps;
        }
        c -= qe << 16;
        if ((a & 0x8000) == 0) {
            const uint32_t lps = a < qe ? 1 : 0;
            context = kMqNext.next[word & 0x7F][lps];
            Renormalize();
            return mps ^ lps;
        }
        return mps;
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
    uint32_t contexts[kContexts];

    // Row k of the column at `fp` (local word f) became significant with sign s.
    JPX_INLINE void SetSignificant(uint32_t& f, uint32_t* fp, int k, uint32_t s) const {
        const int shift = 3 * k;
        f |= (kSigThis | (s << kSignShift)) << shift;
        fp[-1] |= (1u << 5) << shift;
        fp[1] |= (1u << 3) << shift;
        if (k == 0 && !causal) {
            uint32_t* north = fp - stride;
            north[0] |= (1u << 16) | (s << 31);
            north[-1] |= 1u << 17;
            north[1] |= 1u << 15;
        }
        if (k == 3) {
            uint32_t* south = fp + stride;
            south[0] |= (1u << 1) | (s << 18);
            south[-1] |= 1u << 2;
            south[1] |= 1u << 0;
        }
    }

    // The sign context index of row k (Luts::sign).
    JPX_INLINE static uint32_t SignIndex(uint32_t f, const uint32_t* fp, int k) {
        const int shift = 3 * k;
        uint32_t lu = (f >> shift) & 0xAAu;
        lu |= (fp[-1] >> (kSignShift + shift)) & 1u;
        lu |= (fp[1] >> (kSignShift - 2 + shift)) & 4u;
        lu |= k == 0 ? (f >> 14) & 16u : (f >> (12 + shift)) & 16u;
        lu |= (f >> (16 + shift)) & 64u;
        return lu;
    }

    JPX_INLINE void DecodeSign(Mq& mq, uint32_t& f, uint32_t* fp, int k, int32_t* d, int32_t oneplushalf) {
        const uint8_t sc = kLuts.sign[SignIndex(f, fp, k)];
        const uint32_t s = mq.Decode(contexts[sc & 0x1F]) ^ (sc >> 7);
        *d = s ? -oneplushalf : oneplushalf;
        SetSignificant(f, fp, k, s);
    }

    JPX_INLINE void SignificanceStep(Mq& mq, uint32_t& f, uint32_t* fp, int k, int32_t* d, int32_t oneplushalf) {
        const uint32_t window = f >> (3 * k);
        if ((window & (kSigThis | kVisited)) != 0 || (window & kNeighbours) == 0) return;
        if (mq.Decode(contexts[zeroCoding[window & 0x1FF]])) DecodeSign(mq, f, fp, k, d, oneplushalf);
        f |= kVisited << (3 * k);
    }

    JPX_INLINE void SignificanceStepRaw(Raw& raw, uint32_t& f, uint32_t* fp, int k, int32_t* d, int32_t oneplushalf) {
        const uint32_t window = f >> (3 * k);
        if ((window & (kSigThis | kVisited)) != 0 || (window & kNeighbours) == 0) return;
        if (raw.Decode()) {
            const uint32_t s = raw.Decode();
            *d = s ? -oneplushalf : oneplushalf;
            SetSignificant(f, fp, k, s);
        }
        f |= kVisited << (3 * k);
    }

    JPX_INLINE void RefinementStep(Mq& mq, uint32_t& f, int k, int32_t* d, int32_t half) {
        const uint32_t window = f >> (3 * k);
        if ((window & (kSigThis | kVisited)) != kSigThis) return;
        const int context = (window & kRefined) ? 16 : ((window & kNeighbours) ? 15 : 14);
        const uint32_t bit = mq.Decode(contexts[context]);
        *d += (bit ^ static_cast<uint32_t>(*d < 0)) ? half : -half;
        f |= kRefined << (3 * k);
    }

    JPX_INLINE static void RefinementStepRaw(Raw& raw, uint32_t& f, int k, int32_t* d, int32_t half) {
        const uint32_t window = f >> (3 * k);
        if ((window & (kSigThis | kVisited)) != kSigThis) return;
        const uint32_t bit = raw.Decode();
        *d += (bit ^ static_cast<uint32_t>(*d < 0)) ? half : -half;
        f |= kRefined << (3 * k);
    }

    JPX_INLINE void CleanupStep(Mq& mq, uint32_t& f, uint32_t* fp, int k, int32_t* d, int32_t oneplushalf) {
        const uint32_t window = f >> (3 * k);
        if ((window & (kSigThis | kVisited)) == 0 && mq.Decode(contexts[zeroCoding[window & 0x1FF]])) {
            DecodeSign(mq, f, fp, k, d, oneplushalf);
        }
    }

    void SignificancePass(Mq& state, int bp) {
        Mq mq = state;
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint32_t* fp = flags + stripe * stride;
            int32_t* d = data + static_cast<size_t>(y0) * w;
            if (rows == 4) {
                for (size_t x = 0; x < w; x++, fp++, d++) {
                    uint32_t f = *fp;
                    if (f == 0) continue;
                    SignificanceStep(mq, f, fp, 0, d, oneplushalf);
                    SignificanceStep(mq, f, fp, 1, d + w, oneplushalf);
                    SignificanceStep(mq, f, fp, 2, d + 2 * w, oneplushalf);
                    SignificanceStep(mq, f, fp, 3, d + 3 * w, oneplushalf);
                    *fp = f;
                }
            } else {
                for (size_t x = 0; x < w; x++, fp++, d++) {
                    uint32_t f = *fp;
                    if (f == 0) continue;
                    for (int k = 0; k < rows; k++) SignificanceStep(mq, f, fp, k, d + k * w, oneplushalf);
                    *fp = f;
                }
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

    void RefinementPass(Mq& state, int bp) {
        Mq mq = state;
        const int32_t half = (1 << bp) >> 1;
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint32_t* fp = flags + stripe * stride;
            int32_t* d = data + static_cast<size_t>(y0) * w;
            if (rows == 4) {
                for (size_t x = 0; x < w; x++, fp++, d++) {
                    uint32_t f = *fp;
                    if ((f & kSigRows) == 0) continue;
                    RefinementStep(mq, f, 0, d, half);
                    RefinementStep(mq, f, 1, d + w, half);
                    RefinementStep(mq, f, 2, d + 2 * w, half);
                    RefinementStep(mq, f, 3, d + 3 * w, half);
                    *fp = f;
                }
            } else {
                for (size_t x = 0; x < w; x++, fp++, d++) {
                    uint32_t f = *fp;
                    if ((f & kSigRows) == 0) continue;
                    for (int k = 0; k < rows; k++) RefinementStep(mq, f, k, d + k * w, half);
                    *fp = f;
                }
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

    void CleanupPass(Mq& state, int bp, bool segmentation) {
        Mq mq = state;
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        const size_t w = static_cast<size_t>(width);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint32_t* fp = flags + stripe * stride;
            int32_t* d = data + static_cast<size_t>(y0) * w;
            if (rows == 4) {
                for (size_t x = 0; x < w; x++, fp++, d++) {
                    uint32_t f = *fp;
                    if (f == 0) {
                        // Run-length mode: no row significant, visited or with a significant
                        // neighbour.
                        if (!mq.Decode(contexts[kCtxRunLength])) continue;
                        uint32_t k = mq.Decode(contexts[kCtxUniform]) << 1;
                        k |= mq.Decode(contexts[kCtxUniform]);
                        switch (k) {
                        case 0:
                            DecodeSign(mq, f, fp, 0, d, oneplushalf);
                            CleanupStep(mq, f, fp, 1, d + w, oneplushalf);
                            CleanupStep(mq, f, fp, 2, d + 2 * w, oneplushalf);
                            CleanupStep(mq, f, fp, 3, d + 3 * w, oneplushalf);
                            break;
                        case 1:
                            DecodeSign(mq, f, fp, 1, d + w, oneplushalf);
                            CleanupStep(mq, f, fp, 2, d + 2 * w, oneplushalf);
                            CleanupStep(mq, f, fp, 3, d + 3 * w, oneplushalf);
                            break;
                        case 2:
                            DecodeSign(mq, f, fp, 2, d + 2 * w, oneplushalf);
                            CleanupStep(mq, f, fp, 3, d + 3 * w, oneplushalf);
                            break;
                        default:
                            DecodeSign(mq, f, fp, 3, d + 3 * w, oneplushalf);
                            break;
                        }
                        *fp = f;
                        continue;
                    }
                    CleanupStep(mq, f, fp, 0, d, oneplushalf);
                    CleanupStep(mq, f, fp, 1, d + w, oneplushalf);
                    CleanupStep(mq, f, fp, 2, d + 2 * w, oneplushalf);
                    CleanupStep(mq, f, fp, 3, d + 3 * w, oneplushalf);
                    *fp = f & ~kVisitedRows;
                }
            } else {
                for (size_t x = 0; x < w; x++, fp++, d++) {
                    uint32_t f = *fp;
                    for (int k = 0; k < rows; k++) CleanupStep(mq, f, fp, k, d + k * w, oneplushalf);
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
