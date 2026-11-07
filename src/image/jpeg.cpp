// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/jpeg.cpp and the scalar islow IDCT and YCbCr conversion of
// src/gfx/kernels.cpp at 9fbbfaf (Apache-2.0, same author). Written from ITU-T T.81 (the marker
// syntax of annex B, the Huffman decoding of annex F and the progressive procedures of annex G),
// with libjpeg's conventions where T.81 leaves a choice: the Adobe APP14 colour transform,
// refusing a Huffman table that uses an all-ones code, quantization tables latched at a
// component's first scan. RayoPDF's changes: samples out instead of BGRA pixels (the caller
// applies the PDF colour space), no reduced-size decoding, status codes instead of reason text.
#include "image/jpeg.h"

#include <cstring>
#include <vector>

namespace RayoPdf::Image::Jpeg {
namespace {

// The natural (row-major) index of the k-th coefficient in zigzag order.
constexpr uint8_t kNaturalOrder[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                       12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                       35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                       58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

constexpr int kMaxDimension = 65535;
// More scans than any encoder writes (a fully successive-approximated progressive image has a
// few dozen).
constexpr int kMaxScans = 1000;
// The scans of a frame together visit at most this many times each of its coefficients (a
// sequential frame visits each once, libjpeg's progressive script about 4 times): progressive
// scans cost their band in every block whatever their data size, which bounds the work of a
// hostile file.
constexpr int kMaxPasses = 32;
constexpr int kLookupBits = 9;

int16_t Saturate16(int32_t v) { return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

// T.81 F.2.2.1 EXTEND: the s-bit magnitude v as a signed value.
int Extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

// ---------------------------------------------------------------- islow IDCT (libjpeg jidctint.c)

// int32 arithmetic that wraps, as libjpeg's does on overflowing input.
inline int32_t Add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
inline int32_t Mul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
inline int32_t Shl(int32_t a, int n) { return static_cast<int32_t>(static_cast<uint32_t>(a) << n); }
inline int32_t Descale(int32_t x, int n) { return Add(x, 1 << (n - 1)) >> n; }
inline uint8_t ClampSample(int32_t x) {
    if (x < -128) return 0;
    if (x > 127) return 255;
    return static_cast<uint8_t>(x + 128);
}

constexpr int kConstBits = 13;
constexpr int kPass1Bits = 2;
constexpr int32_t FIX_0_298631336 = 2446;
constexpr int32_t FIX_0_390180644 = 3196;
constexpr int32_t FIX_0_541196100 = 4433;
constexpr int32_t FIX_0_765366865 = 6270;
constexpr int32_t FIX_0_899976223 = 7373;
constexpr int32_t FIX_1_175875602 = 9633;
constexpr int32_t FIX_1_501321110 = 12299;
constexpr int32_t FIX_1_847759065 = 15137;
constexpr int32_t FIX_1_961570560 = 16069;
constexpr int32_t FIX_2_053119869 = 16819;
constexpr int32_t FIX_2_562915447 = 20995;
constexpr int32_t FIX_3_072711026 = 25172;

// One 1-D pass on eight inputs; Outputs gives them before descaling.
struct Idct1D {
    int32_t tmp10, tmp11, tmp12, tmp13, tmp0, tmp1, tmp2, tmp3;

    void Run(int32_t i0, int32_t i1, int32_t i2, int32_t i3, int32_t i4, int32_t i5, int32_t i6, int32_t i7) {
        // Even part.
        int32_t z2 = i2;
        int32_t z3 = i6;
        int32_t z1 = Mul(Add(z2, z3), FIX_0_541196100);
        tmp2 = Add(z1, Mul(z3, -FIX_1_847759065));
        tmp3 = Add(z1, Mul(z2, FIX_0_765366865));
        z2 = i0;
        z3 = i4;
        tmp0 = Shl(Add(z2, z3), kConstBits);
        tmp1 = Shl(Sub(z2, z3), kConstBits);
        tmp10 = Add(tmp0, tmp3);
        tmp13 = Sub(tmp0, tmp3);
        tmp11 = Add(tmp1, tmp2);
        tmp12 = Sub(tmp1, tmp2);
        // Odd part.
        tmp0 = i7;
        tmp1 = i5;
        tmp2 = i3;
        tmp3 = i1;
        z1 = Add(tmp0, tmp3);
        z2 = Add(tmp1, tmp2);
        z3 = Add(tmp0, tmp2);
        int32_t z4 = Add(tmp1, tmp3);
        const int32_t z5 = Mul(Add(z3, z4), FIX_1_175875602);
        tmp0 = Mul(tmp0, FIX_0_298631336);
        tmp1 = Mul(tmp1, FIX_2_053119869);
        tmp2 = Mul(tmp2, FIX_3_072711026);
        tmp3 = Mul(tmp3, FIX_1_501321110);
        z1 = Mul(z1, -FIX_0_899976223);
        z2 = Mul(z2, -FIX_2_562915447);
        z3 = Mul(z3, -FIX_1_961570560);
        z4 = Mul(z4, -FIX_0_390180644);
        z3 = Add(z3, z5);
        z4 = Add(z4, z5);
        tmp0 = Add(tmp0, Add(z1, z3));
        tmp1 = Add(tmp1, Add(z2, z4));
        tmp2 = Add(tmp2, Add(z2, z3));
        tmp3 = Add(tmp3, Add(z1, z4));
    }
    void Outputs(int32_t (&o)[8]) const {
        o[0] = Add(tmp10, tmp3);
        o[7] = Sub(tmp10, tmp3);
        o[1] = Add(tmp11, tmp2);
        o[6] = Sub(tmp11, tmp2);
        o[2] = Add(tmp12, tmp1);
        o[5] = Sub(tmp12, tmp1);
        o[3] = Add(tmp13, tmp0);
        o[4] = Sub(tmp13, tmp0);
    }
};

// The 8x8 block of dequantized coefficients (natural order) to samples, `stride` bytes a row.
void IdctBlock(const int16_t* coefficients, uint8_t* out, size_t stride) {
    int16_t workspace[64];
    for (int column = 0; column < 8; column++) {
        const int16_t* in = coefficients + column;
        Idct1D pass;
        pass.Run(in[0], in[8], in[16], in[24], in[32], in[40], in[48], in[56]);
        int32_t o[8];
        pass.Outputs(o);
        for (int row = 0; row < 8; row++) workspace[row * 8 + column] = Saturate16(Descale(o[row], kConstBits - kPass1Bits));
    }
    for (int row = 0; row < 8; row++) {
        const int16_t* w = workspace + row * 8;
        Idct1D pass;
        pass.Run(w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        int32_t o[8];
        pass.Outputs(o);
        uint8_t* line = out + static_cast<size_t>(row) * stride;
        for (int column = 0; column < 8; column++) line[column] = ClampSample(Descale(o[column], kConstBits + kPass1Bits + 3));
    }
}

inline uint8_t ClampChannel(int value) { return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value)); }

// libjpeg's YCbCr to RGB (jdcolor.c, 16-bit fixed point).
inline void YccToRgb(int y, int cb, int cr, uint8_t* rgb) {
    cb -= 128;
    cr -= 128;
    rgb[0] = ClampChannel(y + ((91881 * cr + 32768) >> 16));
    rgb[1] = ClampChannel(y + ((-22554 * cb - 46802 * cr + 32768) >> 16));
    rgb[2] = ClampChannel(y + ((116130 * cb + 32768) >> 16));
}

// ---------------------------------------------------------------- entropy decoding

struct HuffmanTable {
    bool defined = false;
    int total = 0;
    // (length << 8) | symbol for every 9-bit prefix that starts with a code of length <= 9;
    // 0 where the code is longer.
    uint16_t lookup[1 << kLookupBits] = {};
    int32_t maxCode[17] = {};      // the largest code of each length, -1 when there is none
    int32_t valueOffset[17] = {};  // values[code + valueOffset[length]]
    uint8_t values[256] = {};
};

// Builds the canonical code of T.81 C.2 from the 16 counts. False for a table whose codes do
// not fit their lengths or use an all-ones code (libjpeg refuses those too).
bool BuildHuffman(const uint8_t* counts, const uint8_t* symbols, int total, HuffmanTable& table) {
    table = HuffmanTable{};
    int32_t code = 0;
    int k = 0;
    for (int length = 1; length <= 16; length++) {
        const int n = counts[length - 1];
        table.valueOffset[length] = k - code;
        table.maxCode[length] = n ? code + n - 1 : -1;
        for (int i = 0; i < n; i++, k++, code++) {
            if (length <= kLookupBits) {
                const int shift = kLookupBits - length;
                const int base = code << shift;
                if (base + (1 << shift) > (1 << kLookupBits)) return false;
                for (int j = 0; j < (1 << shift); j++) table.lookup[base + j] = static_cast<uint16_t>((length << 8) | symbols[k]);
            }
        }
        if (code >= (int32_t(1) << length)) return false;
        code <<= 1;
    }
    std::memcpy(table.values, symbols, static_cast<size_t>(total));
    table.total = total;
    table.defined = true;
    return true;
}

// Reads the entropy-coded data of a scan: removes the stuffed zero after each 0xFF and stops at
// a marker. Past the marker (or the end of the data) it supplies zero bits and counts them; a
// decoder that consumed any of them ran past its data.
struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;       // the next byte to read; at the 0xFF of the marker once `marker` is set
    uint64_t buffer = 0;  // left-aligned
    int count = 0;        // valid bits in `buffer`
    int fake = 0;         // how many of them, at the end, are padding past the data
    bool marker = false;

    void Start(size_t at) {
        pos = at;
        buffer = 0;
        count = 0;
        fake = 0;
        marker = false;
    }

    void Fill() {
        while (count <= 56) {
            uint32_t byte = 0;
            if (!marker && pos < size) {
                byte = data[pos];
                if (byte == 0xFF) {
                    size_t next = pos + 1;
                    while (next < size && data[next] == 0xFF) next++;
                    if (next < size && data[next] == 0x00) {
                        pos = next + 1;
                    } else {
                        marker = true;
                        pos = next - 1;
                        byte = 0;
                        fake += 8;
                    }
                } else {
                    pos++;
                }
            } else {
                fake += 8;
            }
            buffer |= static_cast<uint64_t>(byte) << (56 - count);
            count += 8;
        }
    }

    bool Overrun() const { return count < fake; }

    uint32_t Peek16() {
        if (count < 16) Fill();
        return static_cast<uint32_t>(buffer >> 48);
    }

    void Consume(int n) {
        buffer <<= n;
        count -= n;
    }

    // n in [0, 16].
    int Bits(int n) {
        if (n == 0) return 0;
        if (count < n) Fill();
        const int v = static_cast<int>(buffer >> (64 - n));
        Consume(n);
        return v;
    }

    int Bit() { return Bits(1); }

    // The symbol of the next code, -1 for a code the table does not have.
    int Decode(const HuffmanTable& table) {
        const uint32_t bits16 = Peek16();
        const uint16_t entry = table.lookup[bits16 >> (16 - kLookupBits)];
        if (entry) {
            Consume(entry >> 8);
            return entry & 0xFF;
        }
        for (int length = kLookupBits + 1; length <= 16; length++) {
            const int32_t code = static_cast<int32_t>(bits16 >> (16 - length));
            if (code <= table.maxCode[length]) {
                const int32_t index = code + table.valueOffset[length];
                if (index < 0 || index >= table.total) return -1;
                Consume(length);
                return table.values[index];
            }
        }
        return -1;
    }
};

struct Component {
    int id = 0;
    int h = 1;
    int v = 1;
    int quantSlot = 0;
    int widthBlocks = 0;  // blocks a non-interleaved scan codes: ceil(component width / 8)
    int heightBlocks = 0;
    int blocksW = 0;  // blocks of the MCU-padded component (planes and coefficients)
    int blocksH = 0;
    bool latched = false;     // quantization table taken (at the component's first scan)
    uint16_t quant[64] = {};  // natural order
    int dcTable = 0;
    int acTable = 0;
    int dcPredictor = 0;
    std::vector<int16_t> coefficients;  // progressive: blocksW * blocksH blocks of 64
    std::vector<uint8_t> plane;         // blocksW * 8 by blocksH * 8 samples

    size_t PlaneStride() const { return static_cast<size_t>(blocksW) * 8; }
};

enum class ScanMode { Sequential, DcFirst, DcRefine, AcFirst, AcRefine };

class Decoder {
public:
    Decoder(const uint8_t* data, size_t size, bool headerOnly, uint64_t maxPixels)
        : data_(data), size_(size), headerOnly_(headerOnly), maxPixels_(maxPixels) {
        bits_.data = data;
        bits_.size = size;
    }

    Result Run();
    Info GetInfo() const {
        Info info;
        info.width = static_cast<uint32_t>(width_);
        info.height = static_cast<uint32_t>(height_);
        info.components = componentCount_;
        info.progressive = progressive_;
        info.adobeTransform = adobe_ ? adobeTransform_ : -1;
        return info;
    }
    void ToSamples(std::string& out) const;

private:
    bool Fail(Result result) {
        result_ = result;
        return false;
    }
    bool NextMarker(int& marker);
    bool Segment(const uint8_t*& segment, size_t& length);
    bool ReadFrame(int marker, const uint8_t* s, size_t n);
    bool ReadQuant(const uint8_t* s, size_t n);
    bool ReadHuffman(const uint8_t* s, size_t n);
    bool ReadScan(const uint8_t* s, size_t n);
    bool Allocate();
    bool DecodeScan();
    bool Restart();
    bool FinishScan();
    bool DecodeBlock(Component& c, int bx, int by);
    bool DecodeSequential(Component& c, int bx, int by);
    bool DecodeDcFirst(Component& c, int16_t* block);
    bool DecodeDcRefine(int16_t* block);
    bool DecodeAcFirst(const Component& c, int16_t* block);
    bool DecodeAcRefine(const Component& c, int16_t* block);
    void InverseTransformAll();
    // The dequantized block at (bx, by) of `c` transformed into its plane.
    static void Store(Component& c, const int16_t* block, int bx, int by);

    const uint8_t* data_;
    size_t size_;
    bool headerOnly_;
    uint64_t maxPixels_;
    size_t pos_ = 0;
    Result result_ = Result::Corrupt;

    bool frame_ = false;
    bool progressive_ = false;
    int width_ = 0;
    int height_ = 0;
    int componentCount_ = 0;
    int hMax_ = 1;
    int vMax_ = 1;
    int mcusX_ = 0;
    int mcusY_ = 0;
    Component components_[4];
    bool allocated_ = false;
    int scans_ = 0;
    // Coefficients the scans visit, each scan its blocks times its band (Se - Ss + 1), and the
    // most allowed: kMaxPasses passes over every coefficient of the frame.
    uint64_t work_ = 0;
    uint64_t maxWork_ = 0;

    bool adobe_ = false;
    int adobeTransform_ = -1;

    bool quantDefined_[4] = {};
    uint16_t quant_[4][64] = {};  // natural order
    HuffmanTable dc_[4];
    HuffmanTable ac_[4];
    int restartInterval_ = 0;

    // The current scan.
    int scanCount_ = 0;
    int scanComponents_[4] = {};
    int ss_ = 0;
    int se_ = 63;
    int ah_ = 0;
    int al_ = 0;
    ScanMode mode_ = ScanMode::Sequential;
    int eobRun_ = 0;
    int nextRestart_ = 0;
    BitReader bits_;
};

bool Decoder::NextMarker(int& marker) {
    // Skip anything before the 0xFF (libjpeg does the same, with a warning), then fill bytes.
    while (pos_ < size_ && data_[pos_] != 0xFF) pos_++;
    while (pos_ < size_ && data_[pos_] == 0xFF) pos_++;
    if (pos_ >= size_) return false;
    marker = data_[pos_++];
    return true;
}

bool Decoder::Segment(const uint8_t*& segment, size_t& length) {
    if (size_ - pos_ < 2) return Fail(Result::Corrupt);
    const size_t total = (static_cast<size_t>(data_[pos_]) << 8) | data_[pos_ + 1];
    if (total < 2 || size_ - pos_ < total) return Fail(Result::Corrupt);
    segment = data_ + pos_ + 2;
    length = total - 2;
    pos_ += total;
    return true;
}

Result Decoder::Run() {
    if (size_ < 4 || data_[0] != 0xFF || data_[1] != 0xD8) return Result::Corrupt;
    pos_ = 2;
    for (;;) {
        int marker = 0;
        if (!NextMarker(marker)) return Result::Corrupt;
        if (marker == 0xD9) {  // EOI
            if (!frame_ || scans_ == 0) return Result::Corrupt;
            if (progressive_) InverseTransformAll();
            return Result::Ok;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // TEM, stray RSTn
        if (marker == 0xD8) return Result::Corrupt;
        if (marker == 0xDC || marker == 0xCC) return Result::Unsupported;  // DNL, DAC (arithmetic)
        // Arithmetic, lossless, hierarchical and reserved frames.
        if (marker >= 0xC3 && marker <= 0xCF && marker != 0xC4) return Result::Unsupported;
        const uint8_t* s = nullptr;
        size_t n = 0;
        if (!Segment(s, n)) return result_;
        switch (marker) {
        case 0xC0:
        case 0xC1:
        case 0xC2:
            if (!ReadFrame(marker, s, n)) return result_;
            if (headerOnly_) return Result::Ok;
            break;
        case 0xC4:
            if (!ReadHuffman(s, n)) return result_;
            break;
        case 0xDB:
            if (!ReadQuant(s, n)) return result_;
            break;
        case 0xDD:
            if (n != 2) return Result::Corrupt;
            restartInterval_ = (s[0] << 8) | s[1];
            break;
        case 0xDA:
            if (!ReadScan(s, n) || !Allocate() || !DecodeScan()) return result_;
            break;
        case 0xEE:  // APP14
            if (n >= 12 && std::memcmp(s, "Adobe", 5) == 0) {
                adobe_ = true;
                adobeTransform_ = s[11];
            }
            break;
        default:  // APPn, COM and reserved markers: skipped
            break;
        }
    }
}

bool Decoder::ReadFrame(int marker, const uint8_t* s, size_t n) {
    if (frame_ || n < 6) return Fail(Result::Corrupt);
    if (s[0] == 12) return Fail(Result::Unsupported);
    if (s[0] != 8) return Fail(Result::Corrupt);
    height_ = (s[1] << 8) | s[2];
    width_ = (s[3] << 8) | s[4];
    componentCount_ = s[5];
    if (height_ == 0) return Fail(Result::Unsupported);  // the height comes in a DNL marker
    if (width_ == 0 || width_ > kMaxDimension || height_ > kMaxDimension) return Fail(Result::Corrupt);
    if (static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_) > maxPixels_) return Fail(Result::TooLarge);
    if (componentCount_ != 1 && componentCount_ != 3 && componentCount_ != 4) return Fail(Result::Unsupported);
    if (n != 6 + 3 * static_cast<size_t>(componentCount_)) return Fail(Result::Corrupt);
    progressive_ = marker == 0xC2;
    hMax_ = 1;
    vMax_ = 1;
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        c.id = s[6 + 3 * i];
        c.h = s[7 + 3 * i] >> 4;
        c.v = s[7 + 3 * i] & 15;
        c.quantSlot = s[8 + 3 * i];
        if (c.h < 1 || c.v < 1 || c.quantSlot > 3) return Fail(Result::Corrupt);
        if (c.h > 2 || c.v > 2) return Fail(Result::Unsupported);
        for (int j = 0; j < i; j++) {
            if (components_[j].id == c.id) return Fail(Result::Corrupt);
        }
        if (c.h > hMax_) hMax_ = c.h;
        if (c.v > vMax_) vMax_ = c.v;
    }
    mcusX_ = (width_ + 8 * hMax_ - 1) / (8 * hMax_);
    mcusY_ = (height_ + 8 * vMax_ - 1) / (8 * vMax_);
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        const int w = (width_ * c.h + hMax_ - 1) / hMax_;
        const int h = (height_ * c.v + vMax_ - 1) / vMax_;
        c.widthBlocks = (w + 7) / 8;
        c.heightBlocks = (h + 7) / 8;
        c.blocksW = mcusX_ * c.h;
        c.blocksH = mcusY_ * c.v;
    }
    frame_ = true;
    return true;
}

bool Decoder::ReadQuant(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const int precision = s[i] >> 4;
        const int slot = s[i] & 15;
        if (precision > 1 || slot > 3) return Fail(Result::Corrupt);
        const size_t bytes = precision ? 128 : 64;
        if (n - i - 1 < bytes) return Fail(Result::Corrupt);
        const uint8_t* q = s + i + 1;
        for (int k = 0; k < 64; k++) {
            quant_[slot][kNaturalOrder[k]] = precision ? static_cast<uint16_t>((q[2 * k] << 8) | q[2 * k + 1]) : q[k];
        }
        quantDefined_[slot] = true;
        i += 1 + bytes;
    }
    return true;
}

bool Decoder::ReadHuffman(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        const int tableClass = s[i] >> 4;
        const int slot = s[i] & 15;
        if (tableClass > 1 || slot > 3 || n - i - 1 < 16) return Fail(Result::Corrupt);
        const uint8_t* counts = s + i + 1;
        int total = 0;
        for (int k = 0; k < 16; k++) total += counts[k];
        if (total > 256 || n - i - 17 < static_cast<size_t>(total)) return Fail(Result::Corrupt);
        HuffmanTable& table = tableClass ? ac_[slot] : dc_[slot];
        if (!BuildHuffman(counts, counts + 16, total, table)) return Fail(Result::Corrupt);
        i += 17 + static_cast<size_t>(total);
    }
    return true;
}

bool Decoder::ReadScan(const uint8_t* s, size_t n) {
    if (!frame_ || n < 1) return Fail(Result::Corrupt);
    if (++scans_ > kMaxScans) return Fail(Result::Corrupt);
    scanCount_ = s[0];
    if (scanCount_ < 1 || scanCount_ > componentCount_ || n != 4 + 2 * static_cast<size_t>(scanCount_)) {
        return Fail(Result::Corrupt);
    }
    int blocksPerMcu = 0;
    for (int i = 0; i < scanCount_; i++) {
        const int id = s[1 + 2 * i];
        int index = -1;
        for (int j = 0; j < componentCount_; j++) {
            if (components_[j].id == id) index = j;
        }
        if (index < 0) return Fail(Result::Corrupt);
        for (int j = 0; j < i; j++) {
            if (scanComponents_[j] == index) return Fail(Result::Corrupt);
        }
        scanComponents_[i] = index;
        Component& c = components_[index];
        c.dcTable = s[2 + 2 * i] >> 4;
        c.acTable = s[2 + 2 * i] & 15;
        if (c.dcTable > 3 || c.acTable > 3) return Fail(Result::Corrupt);
        blocksPerMcu += c.h * c.v;
    }
    if (scanCount_ > 1 && blocksPerMcu > 10) return Fail(Result::Corrupt);
    const uint8_t* p = s + 1 + 2 * scanCount_;
    ss_ = p[0];
    se_ = p[1];
    ah_ = p[2] >> 4;
    al_ = p[2] & 15;
    if (!progressive_) {
        // libjpeg ignores Ss, Se, Ah and Al of a sequential scan; so do we.
        ss_ = 0;
        se_ = 63;
        ah_ = 0;
        al_ = 0;
        mode_ = ScanMode::Sequential;
    } else {
        if (ss_ > se_ || se_ > 63 || al_ > 13) return Fail(Result::Corrupt);
        if (ss_ == 0 && se_ != 0) return Fail(Result::Corrupt);
        if (ss_ > 0 && scanCount_ != 1) return Fail(Result::Corrupt);
        if (ah_ != 0 && al_ != ah_ - 1) return Fail(Result::Corrupt);
        if (ss_ == 0) {
            mode_ = ah_ == 0 ? ScanMode::DcFirst : ScanMode::DcRefine;
        } else {
            mode_ = ah_ == 0 ? ScanMode::AcFirst : ScanMode::AcRefine;
        }
    }
    for (int i = 0; i < scanCount_; i++) {
        Component& c = components_[scanComponents_[i]];
        const bool needsDc = mode_ == ScanMode::Sequential || mode_ == ScanMode::DcFirst;
        const bool needsAc = mode_ == ScanMode::Sequential || mode_ == ScanMode::AcFirst || mode_ == ScanMode::AcRefine;
        if (needsDc && !dc_[c.dcTable].defined) return Fail(Result::Corrupt);
        if (needsAc && !ac_[c.acTable].defined) return Fail(Result::Corrupt);
        if (!c.latched) {
            if (!quantDefined_[c.quantSlot]) return Fail(Result::Corrupt);
            std::memcpy(c.quant, quant_[c.quantSlot], sizeof(c.quant));
            c.latched = true;
        }
    }
    return true;
}

bool Decoder::Allocate() {
    if (allocated_) return true;
    // Every block of every component is coded by at least one Huffman code of at least one bit
    // (a sequential scan or the first DC scan), so a frame cannot have more blocks than eight
    // times the bytes after its first scan header. This refuses a forged size before the buffers
    // it would need are allocated.
    uint64_t blocks = 0;
    for (int i = 0; i < componentCount_; i++) {
        blocks += static_cast<uint64_t>(components_[i].widthBlocks) * static_cast<uint64_t>(components_[i].heightBlocks);
    }
    if (blocks > 8 * static_cast<uint64_t>(size_ - pos_)) return Fail(Result::Corrupt);
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        const size_t count = static_cast<size_t>(c.blocksW) * static_cast<size_t>(c.blocksH);
        if (progressive_) {
            c.coefficients.assign(count * 64, 0);
        } else {
            c.plane.assign(count * 64, 128);
        }
        maxWork_ += static_cast<uint64_t>(kMaxPasses) * 64 * count;
    }
    allocated_ = true;
    return true;
}

bool Decoder::DecodeScan() {
    int mcusX = mcusX_;
    int mcusY = mcusY_;
    if (scanCount_ == 1) {
        const Component& c = components_[scanComponents_[0]];
        mcusX = c.widthBlocks;
        mcusY = c.heightBlocks;
    }
    // The coefficients this scan visits. A progressive scan costs its band in every block whatever
    // its data says (an end-of-band run of 32767 blocks is a few bits), so the sum over the scans
    // is what bounds the work, not the data size.
    uint64_t blocks = static_cast<uint64_t>(mcusX) * static_cast<uint64_t>(mcusY);
    if (scanCount_ > 1) {
        uint64_t perMcu = 0;
        for (int i = 0; i < scanCount_; i++) {
            perMcu += static_cast<uint64_t>(components_[scanComponents_[i]].h * components_[scanComponents_[i]].v);
        }
        blocks *= perMcu;
    }
    work_ += blocks * static_cast<uint64_t>(se_ - ss_ + 1);
    if (work_ > maxWork_) return Fail(Result::Corrupt);
    for (int i = 0; i < scanCount_; i++) components_[scanComponents_[i]].dcPredictor = 0;
    eobRun_ = 0;
    nextRestart_ = 0;
    bits_.Start(pos_);
    int restartsLeft = restartInterval_;
    for (int my = 0; my < mcusY; my++) {
        for (int mx = 0; mx < mcusX; mx++) {
            if (restartInterval_) {
                if (restartsLeft == 0) {
                    if (!Restart()) return false;
                    restartsLeft = restartInterval_;
                }
                restartsLeft--;
            }
            if (scanCount_ == 1) {
                if (!DecodeBlock(components_[scanComponents_[0]], mx, my)) return false;
            } else {
                for (int i = 0; i < scanCount_; i++) {
                    Component& c = components_[scanComponents_[i]];
                    for (int y = 0; y < c.v; y++) {
                        for (int x = 0; x < c.h; x++) {
                            if (!DecodeBlock(c, mx * c.h + x, my * c.v + y)) return false;
                        }
                    }
                }
            }
            if (bits_.Overrun()) return Fail(Result::Corrupt);
        }
    }
    return FinishScan();
}

// Leaves the entropy-coded data of an interval at its marker: what is left in the bit buffer is
// padding. Bytes before the marker that no MCU used are skipped (libjpeg does the same).
size_t SkipToMarker(const uint8_t* data, size_t size, const BitReader& bits) {
    size_t pos = bits.pos;
    if (!bits.marker) {
        while (pos + 1 < size && !(data[pos] == 0xFF && data[pos + 1] != 0x00 && data[pos + 1] != 0xFF)) pos++;
    }
    return pos;
}

bool Decoder::Restart() {
    if (bits_.Overrun()) return Fail(Result::Corrupt);
    size_t pos = SkipToMarker(data_, size_, bits_);
    while (pos < size_ && data_[pos] == 0xFF) pos++;
    if (pos >= size_ || data_[pos] != 0xD0 + (nextRestart_ & 7)) return Fail(Result::Corrupt);
    nextRestart_++;
    for (int i = 0; i < scanCount_; i++) components_[scanComponents_[i]].dcPredictor = 0;
    eobRun_ = 0;
    bits_.Start(pos + 1);
    return true;
}

bool Decoder::FinishScan() {
    if (bits_.Overrun()) return Fail(Result::Corrupt);
    pos_ = SkipToMarker(data_, size_, bits_);
    return true;
}

bool Decoder::DecodeBlock(Component& c, int bx, int by) {
    if (mode_ == ScanMode::Sequential) return DecodeSequential(c, bx, by);
    int16_t* block = c.coefficients.data() + (static_cast<size_t>(by) * static_cast<size_t>(c.blocksW) + static_cast<size_t>(bx)) * 64;
    switch (mode_) {
    case ScanMode::DcFirst: return DecodeDcFirst(c, block);
    case ScanMode::DcRefine: return DecodeDcRefine(block);
    case ScanMode::AcFirst: return DecodeAcFirst(c, block);
    default: return DecodeAcRefine(c, block);
    }
}

// A sequential block: decoded, dequantized and transformed into the component plane.
bool Decoder::DecodeSequential(Component& c, int bx, int by) {
    int16_t block[64] = {};
    const int s = bits_.Decode(dc_[c.dcTable]);
    if (s < 0 || s > 15) return Fail(Result::Corrupt);
    const int diff = s ? Extend(bits_.Bits(s), s) : 0;
    const int dc = c.dcPredictor + diff;
    if (dc < -32768 || dc > 32767) return Fail(Result::Corrupt);
    c.dcPredictor = dc;
    block[0] = Saturate16(dc * static_cast<int32_t>(c.quant[0]));
    const HuffmanTable& ac = ac_[c.acTable];
    for (int k = 1; k < 64; k++) {
        const int rs = bits_.Decode(ac);
        if (rs < 0) return Fail(Result::Corrupt);
        const int r = rs >> 4;
        const int size = rs & 15;
        if (size == 0) {
            if (r != 15) break;
            k += 15;
            continue;
        }
        k += r;
        if (k > 63) return Fail(Result::Corrupt);
        const int natural = kNaturalOrder[k];
        block[natural] = Saturate16(Extend(bits_.Bits(size), size) * static_cast<int32_t>(c.quant[natural]));
    }
    Store(c, block, bx, by);
    return true;
}

bool Decoder::DecodeDcFirst(Component& c, int16_t* block) {
    const int s = bits_.Decode(dc_[c.dcTable]);
    if (s < 0 || s > 15) return Fail(Result::Corrupt);
    const int diff = s ? Extend(bits_.Bits(s), s) : 0;
    const int dc = c.dcPredictor + diff;
    if (dc < -32768 || dc > 32767) return Fail(Result::Corrupt);
    c.dcPredictor = dc;
    block[0] = Saturate16(dc * (int32_t(1) << al_));
    return true;
}

bool Decoder::DecodeDcRefine(int16_t* block) {
    if (bits_.Bit()) block[0] = static_cast<int16_t>(block[0] | (1 << al_));
    return true;
}

bool Decoder::DecodeAcFirst(const Component& c, int16_t* block) {
    if (eobRun_ > 0) {
        eobRun_--;
        return true;
    }
    const HuffmanTable& ac = ac_[c.acTable];
    for (int k = ss_; k <= se_; k++) {
        const int rs = bits_.Decode(ac);
        if (rs < 0) return Fail(Result::Corrupt);
        const int r = rs >> 4;
        const int s = rs & 15;
        if (s == 0) {
            if (r < 15) {
                eobRun_ = (1 << r) - 1;
                if (r) eobRun_ += bits_.Bits(r);
                break;
            }
            k += 15;
            continue;
        }
        k += r;
        if (k > se_) return Fail(Result::Corrupt);
        block[kNaturalOrder[k]] = Saturate16(Extend(bits_.Bits(s), s) * (int32_t(1) << al_));
    }
    return true;
}

// T.81 G.1.2.3, in libjpeg's formulation (decode_mcu_AC_refine).
bool Decoder::DecodeAcRefine(const Component& c, int16_t* block) {
    const int p1 = 1 << al_;
    const int m1 = -p1;
    int k = ss_;
    auto refine = [&](int16_t& coefficient) {
        if (bits_.Bit() && (coefficient & p1) == 0) coefficient = Saturate16(coefficient + (coefficient >= 0 ? p1 : m1));
    };
    if (eobRun_ == 0) {
        const HuffmanTable& ac = ac_[c.acTable];
        for (; k <= se_; k++) {
            const int rs = bits_.Decode(ac);
            if (rs < 0) return Fail(Result::Corrupt);
            int r = rs >> 4;
            int s = rs & 15;
            if (s) {
                if (s != 1) return Fail(Result::Corrupt);
                s = bits_.Bit() ? p1 : m1;
            } else if (r != 15) {
                eobRun_ = 1 << r;
                if (r) eobRun_ += bits_.Bits(r);
                break;  // the rest of this block is handled with the end-of-band run below
            }
            // Skip r zero coefficients (refining the nonzero ones passed on the way), then
            // place the new coefficient, if any.
            for (; k <= se_; k++) {
                int16_t& coefficient = block[kNaturalOrder[k]];
                if (coefficient != 0) {
                    refine(coefficient);
                } else {
                    if (r == 0) break;
                    r--;
                }
            }
            if (s) {
                if (k > se_) return Fail(Result::Corrupt);
                block[kNaturalOrder[k]] = static_cast<int16_t>(s);
            }
        }
    }
    if (eobRun_ > 0) {
        for (; k <= se_; k++) {
            int16_t& coefficient = block[kNaturalOrder[k]];
            if (coefficient != 0) refine(coefficient);
        }
        eobRun_--;
    }
    return true;
}

void Decoder::Store(Component& c, const int16_t* block, int bx, int by) {
    const size_t stride = c.PlaneStride();
    IdctBlock(block, c.plane.data() + static_cast<size_t>(by) * 8 * stride + static_cast<size_t>(bx) * 8, stride);
}

void Decoder::InverseTransformAll() {
    for (int i = 0; i < componentCount_; i++) {
        Component& c = components_[i];
        c.plane.assign(c.PlaneStride() * static_cast<size_t>(c.blocksH) * 8, 0);
        if (c.coefficients.empty()) continue;  // a component no scan coded stays 0
        int16_t block[64];
        for (int by = 0; by < c.blocksH; by++) {
            for (int bx = 0; bx < c.blocksW; bx++) {
                const int16_t* in = c.coefficients.data() + (static_cast<size_t>(by) * static_cast<size_t>(c.blocksW) + static_cast<size_t>(bx)) * 64;
                for (int k = 0; k < 64; k++) block[k] = Saturate16(in[k] * static_cast<int32_t>(c.quant[k]));
                Store(c, block, bx, by);
            }
        }
        std::vector<int16_t>().swap(c.coefficients);
    }
}

// Row `y` of the frame from component `c` sampled at half the frame's rate on one axis or both,
// at full width into `out` (at least 2 * c.PlaneStride() bytes): libjpeg's default "fancy"
// upsampling (jdsample.c, libjpeg-turbo 3), which is what libjpeg-based viewers and poppler show.
// Each new sample weighs its nearest input 3/4 and the next nearest 1/4 per doubled axis, with
// libjpeg's alternating rounding; the rows above the first and below the last input row repeat
// them; components two samples wide or less are replicated, as libjpeg does.
void UpsampleRow(const Component& c, int hExpand, int vExpand, int y, int downWidth, int downHeight, uint8_t* out) {
    const size_t stride = c.PlaneStride();
    const int row = vExpand == 2 ? y / 2 : y;
    const uint8_t* in0 = c.plane.data() + static_cast<size_t>(row) * stride;
    const uint8_t* in1 = in0;  // the next nearest row, for vertical expansion
    int bias = 0;
    if (vExpand == 2) {
        const int neighbour = y % 2 == 0 ? (row > 0 ? row - 1 : 0) : (row + 1 < downHeight ? row + 1 : downHeight - 1);
        in1 = c.plane.data() + static_cast<size_t>(neighbour) * stride;
        bias = y % 2 == 0 ? 1 : 2;
    }
    const int w = downWidth;
    if (hExpand == 1) {  // vertical only (h1v2_fancy_upsample)
        for (int x = 0; x < w; x++) out[x] = static_cast<uint8_t>((in0[x] * 3 + in1[x] + bias) >> 2);
        return;
    }
    if (w <= 2) {  // plain replication (h2v1_upsample, h2v2_upsample)
        for (int x = 0; x < w; x++) out[2 * x] = out[2 * x + 1] = in0[x];
        return;
    }
    if (vExpand == 1) {  // h2v1_fancy_upsample
        int value = in0[0];
        out[0] = static_cast<uint8_t>(value);
        out[1] = static_cast<uint8_t>((value * 3 + in0[1] + 2) >> 2);
        for (int x = 1; x < w - 1; x++) {
            value = in0[x] * 3;
            out[2 * x] = static_cast<uint8_t>((value + in0[x - 1] + 1) >> 2);
            out[2 * x + 1] = static_cast<uint8_t>((value + in0[x + 1] + 2) >> 2);
        }
        value = in0[w - 1];
        out[2 * w - 2] = static_cast<uint8_t>((value * 3 + in0[w - 2] + 1) >> 2);
        out[2 * w - 1] = static_cast<uint8_t>(value);
        return;
    }
    // h2v2_fancy_upsample: column sums of the two rows, then the same horizontally in sixteenths.
    int thisSum = in0[0] * 3 + in1[0];
    int nextSum = in0[1] * 3 + in1[1];
    out[0] = static_cast<uint8_t>((thisSum * 4 + 8) >> 4);
    out[1] = static_cast<uint8_t>((thisSum * 3 + nextSum + 7) >> 4);
    int lastSum = thisSum;
    thisSum = nextSum;
    for (int x = 1; x < w - 1; x++) {
        nextSum = in0[x + 1] * 3 + in1[x + 1];
        out[2 * x] = static_cast<uint8_t>((thisSum * 3 + lastSum + 8) >> 4);
        out[2 * x + 1] = static_cast<uint8_t>((thisSum * 3 + nextSum + 7) >> 4);
        lastSum = thisSum;
        thisSum = nextSum;
    }
    out[2 * w - 2] = static_cast<uint8_t>((thisSum * 3 + lastSum + 8) >> 4);
    out[2 * w - 1] = static_cast<uint8_t>((thisSum * 4 + 7) >> 4);
}

void Decoder::ToSamples(std::string& out) const {
    const size_t width = static_cast<size_t>(width_);
    const size_t n = static_cast<size_t>(componentCount_);
    out.resize(width * n * static_cast<size_t>(height_));
    // Rows of the components at full width: the plane row itself, or an upsampled copy (a
    // component sampled at half the largest factor on an axis).
    std::vector<uint8_t> expanded[4];
    int hExpand[4] = {1, 1, 1, 1}, vExpand[4] = {1, 1, 1, 1}, downWidth[4] = {}, downHeight[4] = {};
    for (int i = 0; i < componentCount_; i++) {
        const Component& c = components_[i];
        hExpand[i] = hMax_ / c.h;
        vExpand[i] = vMax_ / c.v;
        downWidth[i] = (width_ * c.h + hMax_ - 1) / hMax_;
        downHeight[i] = (height_ * c.v + vMax_ - 1) / vMax_;
        if (hExpand[i] != 1 || vExpand[i] != 1) expanded[i].resize(2 * c.PlaneStride());
    }
    const bool ycc = (componentCount_ == 3 && !(adobe_ && adobeTransform_ == 0)) ||
                     (componentCount_ == 4 && adobe_ && adobeTransform_ == 2);
    for (int y = 0; y < height_; y++) {
        const uint8_t* rows[4] = {};
        for (int i = 0; i < componentCount_; i++) {
            const Component& c = components_[i];
            if (hExpand[i] == 1 && vExpand[i] == 1) {
                rows[i] = c.plane.data() + static_cast<size_t>(y) * c.PlaneStride();
            } else {
                UpsampleRow(c, hExpand[i], vExpand[i], y, downWidth[i], downHeight[i], expanded[i].data());
                rows[i] = expanded[i].data();
            }
        }
        uint8_t* dst = reinterpret_cast<uint8_t*>(&out[0]) + static_cast<size_t>(y) * width * n;
        if (n == 1) {
            std::memcpy(dst, rows[0], width);
        } else if (!ycc) {
            for (size_t x = 0; x < width; x++) {
                for (size_t i = 0; i < n; i++) dst[x * n + i] = rows[i][x];
            }
        } else if (n == 3) {
            for (size_t x = 0; x < width; x++) YccToRgb(rows[0][x], rows[1][x], rows[2][x], dst + 3 * x);
        } else {
            for (size_t x = 0; x < width; x++) {
                uint8_t rgb[3];
                YccToRgb(rows[0][x], rows[1][x], rows[2][x], rgb);
                dst[4 * x] = static_cast<uint8_t>(255 - rgb[0]);
                dst[4 * x + 1] = static_cast<uint8_t>(255 - rgb[1]);
                dst[4 * x + 2] = static_cast<uint8_t>(255 - rgb[2]);
                dst[4 * x + 3] = rows[3][x];
            }
        }
    }
}

} // namespace

Result ReadInfo(const uint8_t* data, size_t size, Info& info) {
    Decoder decoder(data, size, true, ~uint64_t(0));
    const Result result = decoder.Run();
    info = result == Result::Ok ? decoder.GetInfo() : Info();
    return result;
}

Result Decode(const uint8_t* data, size_t size, uint64_t maxPixels, Info& info, std::string& samples) {
    samples.clear();
    Decoder decoder(data, size, false, maxPixels);
    const Result result = decoder.Run();
    if (result != Result::Ok) {
        info = Info();
        return result;
    }
    info = decoder.GetInfo();
    decoder.ToSamples(samples);
    return Result::Ok;
}

} // namespace RayoPdf::Image::Jpeg
