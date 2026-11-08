// SPDX-License-Identifier: Apache-2.0
// Tier-1 of the JPEG 2000 decoder: the MQ arithmetic decoder (T.800 annex C) and the embedded
// block decoding of one code-block (annex D): significance propagation, magnitude refinement and
// cleanup passes with the run-length mode, the raw (bypass) passes, and the reset, termination,
// vertically causal and segmentation-symbol styles. The reconstruction follows OpenJPEG: values
// carry one fractional bit and a newly significant coefficient starts at the midpoint of its
// interval (1.5 at its bit-plane), which a refinement bit moves up or down by half a step.
//
// Flags: one 16-bit word per coefficient, laid out stripe column by stripe column (the four rows
// of a column adjacent) with a border of one coefficient so that the neighbours of every
// coefficient exist. A coefficient that becomes significant sets its bit in the words of its
// eight neighbours, so a context is a table lookup on one word, and one 64-bit load tells whether
// a column has anything to code in a pass.
#include "image/jpx_internal.h"

#include <algorithm>
#include <cstring>

namespace RayoPdf::Image::Jpx::Internal {
namespace {

// The significance of the eight neighbours, the signs of the four direct ones.
constexpr uint16_t kSigN = 1u << 0;
constexpr uint16_t kSigE = 1u << 1;
constexpr uint16_t kSigS = 1u << 2;
constexpr uint16_t kSigW = 1u << 3;
constexpr uint16_t kSigNE = 1u << 4;
constexpr uint16_t kSigSE = 1u << 5;
constexpr uint16_t kSigSW = 1u << 6;
constexpr uint16_t kSigNW = 1u << 7;
constexpr uint16_t kSgnN = 1u << 8;
constexpr uint16_t kSgnE = 1u << 9;
constexpr uint16_t kSgnS = 1u << 10;
constexpr uint16_t kSgnW = 1u << 11;
constexpr uint16_t kSig = 1u << 12;      // the coefficient is significant
constexpr uint16_t kRefined = 1u << 13;  // it has had a magnitude refinement bit
constexpr uint16_t kVisited = 1u << 14;  // coded in this bit-plane's significance pass
constexpr uint16_t kNeighbours = 0xFF;
// With the vertically causal style, the next stripe's coefficients count as insignificant for the
// last row of a stripe.
constexpr uint16_t kSouth = kSigS | kSigSE | kSigSW | kSgnS;

// Contexts (T.800 tables D.1 to D.4): 0-8 zero coding, 9-13 sign, 14-16 magnitude refinement,
// 17 run length, 18 uniform.
constexpr int kCtxRunLength = 17;
constexpr int kCtxUniform = 18;
constexpr int kContexts = 19;

struct Luts {
    uint8_t zeroCoding[4][256];  // by orientation (LL, HL, LH, HH) and the neighbour bits
    uint8_t sign[256];           // context | (xor bit << 7), by the direct neighbours' bits
};

constexpr Luts MakeLuts() {
    Luts luts{};
    for (int orientation = 0; orientation < 4; orientation++) {
        for (int f = 0; f < 256; f++) {
            int h = ((f & kSigE) != 0) + ((f & kSigW) != 0);
            int v = ((f & kSigN) != 0) + ((f & kSigS) != 0);
            const int d = ((f & kSigNE) != 0) + ((f & kSigSE) != 0) + ((f & kSigSW) != 0) + ((f & kSigNW) != 0);
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
        // i: significance of N E S W in bits 0-3, their signs in bits 4-7.
        auto contribution = [i](int sigBit, int sgnBit) { return (i & sigBit) ? ((i & sgnBit) ? -1 : 1) : 0; };
        int h = contribution(2, 32) + contribution(8, 128);
        int v = contribution(1, 16) + contribution(4, 64);
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

inline int SignIndex(uint16_t f) { return (f & 0xF) | ((f >> 4) & 0xF0); }

// T.800 table C.2: Qe, the next state after an MPS and after an LPS, and whether an LPS switches
// the MPS sense.
struct MqState {
    uint16_t qe;
    uint8_t nmps;
    uint8_t nlps;
    uint8_t switchMps;
};
constexpr MqState kStates[47] = {
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

// A context: its state index times two plus its MPS.
void ResetContexts(uint8_t* contexts) {
    std::memset(contexts, 0, kContexts);
    contexts[0] = 4 << 1;
    contexts[kCtxRunLength] = 3 << 1;
    contexts[kCtxUniform] = 46 << 1;
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

    JPX_INLINE void Renormalize() {
        do {
            if (ct == 0) ByteIn();
            a <<= 1;
            c <<= 1;
            ct--;
        } while (a < 0x8000);
    }

    JPX_INLINE uint32_t Decode(uint8_t& context) {
        const MqState& state = kStates[context >> 1];
        const uint32_t mps = context & 1u;
        const uint32_t qe = state.qe;
        a -= qe;
        if ((c >> 16) < qe) {
            uint32_t d;
            if (a < qe) {
                d = mps;
                context = static_cast<uint8_t>((state.nmps << 1) | mps);
            } else {
                d = mps ^ 1u;
                context = static_cast<uint8_t>((state.nlps << 1) | (mps ^ state.switchMps));
            }
            a = qe;
            Renormalize();
            return d;
        }
        c -= qe << 16;
        if ((a & 0x8000) == 0) {
            uint32_t d;
            if (a < qe) {
                d = mps ^ 1u;
                context = static_cast<uint8_t>((state.nlps << 1) | (mps ^ state.switchMps));
            } else {
                d = mps;
                context = static_cast<uint8_t>((state.nmps << 1) | mps);
            }
            Renormalize();
            return d;
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

// Four flag words, one per row of a stripe column, in one 64-bit word.
constexpr uint64_t kRows = 0x0001000100010001ull;

inline uint64_t Column(const uint16_t* f) {
    uint64_t v;
    std::memcpy(&v, f, sizeof v);
    return v;
}

// The code-block's flags, stripe by stripe: a stripe holds its columns (plus a border column on
// each side) one after the other, a column its four rows; a border stripe above and below. The
// four rows of a column load as one 64-bit word.
struct Block {
    int width;
    int height;
    ptrdiff_t stripeSize;  // flag words per stripe: (width + 2) * 4
    uint16_t* flags;       // stripe 0, column 0, row 0
    int32_t* data;
    const uint8_t* zeroCoding;
    uint16_t viewMask[4];  // what row k sees of its flags (the vertically causal style hides south)
    ptrdiff_t north[4];  // offset of the row above, by row within the stripe
    ptrdiff_t south[4];
    uint8_t contexts[kContexts];

    void Init() {
        for (int k = 0; k < 4; k++) {
            north[k] = k > 0 ? -1 : -stripeSize + 3;
            south[k] = k < 3 ? 1 : stripeSize - 3;
        }
    }

    uint16_t* ColumnFlags(int stripe, int x) const { return flags + stripe * stripeSize + 4 * ptrdiff_t(x); }

    JPX_INLINE void SetSignificant(uint16_t* f, int k, uint32_t negative) {
        const ptrdiff_t n = north[k];
        const ptrdiff_t s = south[k];
        f[0] |= kSig;
        f[n] |= static_cast<uint16_t>(kSigS | (negative ? kSgnS : 0));
        f[s] |= static_cast<uint16_t>(kSigN | (negative ? kSgnN : 0));
        f[-4] |= static_cast<uint16_t>(kSigE | (negative ? kSgnE : 0));
        f[4] |= static_cast<uint16_t>(kSigW | (negative ? kSgnW : 0));
        f[n - 4] |= kSigSE;
        f[n + 4] |= kSigSW;
        f[s - 4] |= kSigNE;
        f[s + 4] |= kSigNW;
    }

    // The flags of row k of a stripe as its contexts see them.
    JPX_INLINE uint16_t View(const uint16_t* f, int k) const { return static_cast<uint16_t>(*f & viewMask[k]); }

    JPX_INLINE void DecodeSign(Mq& mq, uint16_t* f, int k, uint16_t view, int32_t* d, int32_t oneplushalf) {
        const uint8_t sc = kLuts.sign[SignIndex(view)];
        const uint32_t negative = mq.Decode(contexts[sc & 0x7F]) ^ (sc >> 7);
        *d = negative ? -oneplushalf : oneplushalf;
        SetSignificant(f, k, negative);
    }

    void SignificancePass(Mq& state, int bp) {
        Mq mq = state;
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint16_t* f = ColumnFlags(stripe, 0);
            int32_t* d = data + static_cast<size_t>(y0) * width;
            for (int x = 0; x < width; x++, f += 4, d++) {
                // Nothing significant around and nothing significant here: no decision.
                if (Column(f) == 0) continue;
                for (int k = 0; k < rows; k++) {
                    const uint16_t view = View(f + k, k);
                    if ((view & (kSig | kVisited)) != 0 || (view & kNeighbours) == 0) continue;
                    if (mq.Decode(contexts[zeroCoding[view & kNeighbours]])) {
                        DecodeSign(mq, f + k, k, view, d + static_cast<size_t>(k) * width, oneplushalf);
                    }
                    f[k] |= kVisited;
                }
            }
        }
        state = mq;
    }

    void SignificancePassRaw(Raw& raw, int bp) {
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint16_t* f = ColumnFlags(stripe, 0);
            int32_t* d = data + static_cast<size_t>(y0) * width;
            for (int x = 0; x < width; x++, f += 4, d++) {
                if (Column(f) == 0) continue;
                for (int k = 0; k < rows; k++) {
                    const uint16_t view = View(f + k, k);
                    if ((view & (kSig | kVisited)) != 0 || (view & kNeighbours) == 0) continue;
                    if (raw.Decode()) {
                        const uint32_t negative = raw.Decode();
                        d[static_cast<size_t>(k) * width] = negative ? -oneplushalf : oneplushalf;
                        SetSignificant(f + k, k, negative);
                    }
                    f[k] |= kVisited;
                }
            }
        }
    }

    void RefinementPass(Mq& state, int bp) {
        Mq mq = state;
        const int32_t half = (1 << bp) >> 1;
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint16_t* f = ColumnFlags(stripe, 0);
            int32_t* d = data + static_cast<size_t>(y0) * width;
            for (int x = 0; x < width; x++, f += 4, d++) {
                if ((Column(f) & (kRows * kSig)) == 0) continue;
                for (int k = 0; k < rows; k++) {
                    const uint16_t view = View(f + k, k);
                    if ((view & (kSig | kVisited)) != kSig) continue;
                    const int context = (view & kRefined) ? 16 : ((view & kNeighbours) ? 15 : 14);
                    const uint32_t bit = mq.Decode(contexts[context]);
                    int32_t& v = d[static_cast<size_t>(k) * width];
                    v += (bit ^ static_cast<uint32_t>(v < 0)) ? half : -half;
                    f[k] |= kRefined;
                }
            }
        }
        state = mq;
    }

    void RefinementPassRaw(Raw& raw, int bp) {
        const int32_t half = (1 << bp) >> 1;
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint16_t* f = ColumnFlags(stripe, 0);
            int32_t* d = data + static_cast<size_t>(y0) * width;
            for (int x = 0; x < width; x++, f += 4, d++) {
                if ((Column(f) & (kRows * kSig)) == 0) continue;
                for (int k = 0; k < rows; k++) {
                    if ((f[k] & (kSig | kVisited)) != kSig) continue;
                    const uint32_t bit = raw.Decode();
                    int32_t& v = d[static_cast<size_t>(k) * width];
                    v += (bit ^ static_cast<uint32_t>(v < 0)) ? half : -half;
                    f[k] |= kRefined;
                }
            }
        }
    }

    void CleanupPass(Mq& state, int bp, bool segmentation) {
        Mq mq = state;
        const int32_t one = 1 << bp;
        const int32_t oneplushalf = one | (one >> 1);
        // A column starts in run-length mode when all four rows are insignificant, not visited
        // and without significant neighbours (the last row's south ones not counting with the
        // vertically causal style).
        const uint64_t busy = kSig | kVisited | kNeighbours;
        const uint64_t runMask = (kRows * busy) & ~(uint64_t(static_cast<uint16_t>(~viewMask[3])) << 48);
        for (int y0 = 0, stripe = 0; y0 < height; y0 += 4, stripe++) {
            const int rows = std::min(4, height - y0);
            uint16_t* f = ColumnFlags(stripe, 0);
            int32_t* d = data + static_cast<size_t>(y0) * width;
            for (int x = 0; x < width; x++, f += 4, d++) {
                int k = 0;
                if (rows == 4 && (Column(f) & runMask) == 0) {
                    if (!mq.Decode(contexts[kCtxRunLength])) continue;
                    k = static_cast<int>(mq.Decode(contexts[kCtxUniform]) << 1);
                    k |= static_cast<int>(mq.Decode(contexts[kCtxUniform]));
                    DecodeSign(mq, f + k, k, View(f + k, k), d + static_cast<size_t>(k) * width, oneplushalf);
                    k++;
                }
                for (; k < rows; k++) {
                    const uint16_t view = View(f + k, k);
                    if ((view & (kSig | kVisited)) == 0 && mq.Decode(contexts[zeroCoding[view & kNeighbours]])) {
                        DecodeSign(mq, f + k, k, view, d + static_cast<size_t>(k) * width, oneplushalf);
                    }
                    f[k] &= static_cast<uint16_t>(~kVisited);
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
    block.stripeSize = (static_cast<ptrdiff_t>(width) + 2) * 4;
    const int stripes = (height + 3) / 4;
    flags_.assign(static_cast<size_t>(block.stripeSize) * (static_cast<size_t>(stripes) + 2), 0);
    block.flags = flags_.data() + block.stripeSize + 4;
    block.data = out;
    block.zeroCoding = kLuts.zeroCoding[job.orientation & 3];
    const bool causal = (job.style & kStyleCausal) != 0;
    for (int k = 0; k < 4; k++) block.viewMask[k] = causal && k == 3 ? static_cast<uint16_t>(~kSouth) : 0xFFFF;
    block.Init();
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
