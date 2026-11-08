// SPDX-License-Identifier: Apache-2.0
// Core tests of the JBIG2 decoder (exit codes 1100-1149): the MQ decoder and a test-side MQ
// encoder on the test sequence of T.88 H.2; the arithmetic integer and IAID decoders; the standard
// Huffman tables on the codes T.88 annex H quotes, a code table segment, the symbol ID table of
// 7.4.3.1.7 example 1; the datastream of T.88 H.1 (page 1 Huffman and MMR coded, page 2 the same
// arithmetic coded, page 3 refinement/aggregate coded), against the page poppler decodes; then
// streams built here with the test-side encoders (written straight from the context templates of
// T.88 6.2.5.3 and 6.3.5.3, pixel by pixel): generic regions of every template with TPGDON and
// nominal, near and far AT pixels, an unknown data length, refinement of the page and of
// intermediate regions with TPGRON, every combination operator, a page of unknown height with end
// of stripe segments, text regions in every reference corner, transposed or not, with strips and
// refinement, symbol dictionaries that reuse retained statistics, a pattern dictionary and a
// halftone region with a rotated grid and skipped cells; and the limits and damaged input.
#include "check.h"

#include "image/jbig2.h"
#include "image/jbig2_internal.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace RayoPdf::Image;
using namespace RayoPdf::Image::Jbig2::Internal;

namespace {

// ---------------------------------------------------------------- bits and bitmaps

std::string Hex(const char* hex) {
    std::string out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        const auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        out += static_cast<char>(nibble(hex[i]) << 4 | nibble(hex[i + 1]));
    }
    return out;
}

void Put32(std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; i--) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}
void Put16(std::string& s, uint32_t v) {
    s += static_cast<char>((v >> 8) & 0xFF);
    s += static_cast<char>(v & 0xFF);
}

class BitWriter {
public:
    void Bits(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; i--) Bit(uint32_t(v >> i) & 1u);
    }
    void Bit(uint32_t b) {
        if (count_ % 8 == 0) bytes_ += '\0';
        if (b) bytes_.back() = static_cast<char>(bytes_.back() | (0x80 >> (count_ % 8)));
        count_++;
    }
    void Code(const char* bits) {
        for (const char* p = bits; *p; p++)
            if (*p == '0' || *p == '1') Bit(uint32_t(*p - '0'));
    }
    const std::string& Bytes() const { return bytes_; }

private:
    std::string bytes_;
    size_t count_ = 0;
};

// One byte per pixel, 1 = black.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> px;
    Image() = default;
    Image(int w, int h, uint8_t v = 0) : width(w), height(h), px(size_t(w) * size_t(h), v) {}
    uint32_t Get(int64_t x, int64_t y) const {
        return x < 0 || y < 0 || x >= width || y >= height ? 0u : px[size_t(y) * size_t(width) + size_t(x)];
    }
    void Set(int x, int y, uint32_t v) { px[size_t(y) * size_t(width) + size_t(x)] = static_cast<uint8_t>(v); }
};

uint32_t g_seed = 12345;
uint32_t Random() {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) & 0x7FFF;
}

// Blobs and noise, with repeated and blank rows (so typical prediction has rows to skip).
Image RandomImage(int w, int h, int density) {
    Image img(w, h);
    for (int y = 0; y < h; y++) {
        if (y > 0 && Random() % 5 == 0) {
            for (int x = 0; x < w; x++) img.Set(x, y, img.Get(x, y - 1));
            continue;
        }
        if (Random() % 7 == 0) continue;
        for (int x = 0; x < w; x++) {
            const uint32_t near = img.Get(x - 1, y) + img.Get(x, y - 1);
            img.Set(x, y, Random() % 100 < uint32_t(density + int(near) * 25) ? 1u : 0u);
        }
    }
    return img;
}

// A decoded page against `expected` (1 = black): the output is 0 for black, padding bits 1.
bool SamePage(const std::string& rows, const Jbig2::Info& info, const Image& expected) {
    if (int(info.width) != expected.width || int(info.height) != expected.height) return false;
    const size_t stride = (size_t(info.width) + 7) / 8;
    if (rows.size() != stride * info.height) return false;
    for (int y = 0; y < expected.height; y++)
        for (size_t x = 0; x < stride * 8; x++) {
            const uint32_t bit = (uint8_t(rows[size_t(y) * stride + x / 8]) >> (7 - x % 8)) & 1u;
            const uint32_t want = x < size_t(expected.width) ? expected.Get(int64_t(x), y) ^ 1u : 1u;
            if (bit != want) return false;
        }
    return true;
}

// ---------------------------------------------------------------- test-side encoders

// The MQ encoder of T.88 E.2 (INITENC, CODEMPS/CODELPS, RENORME, BYTEOUT, FLUSH).
class MqEncoder {
public:
    MqEncoder() { out_.push_back(0); }  // the byte before the first (B = 0)

    void Encode(Context& cx, uint32_t d) {
        const QeEntry& e = kQeTable[cx >> 1];
        uint32_t mps = cx & 1u;
        const uint32_t qe = e.qe;
        a_ -= qe;
        if (d == mps) {
            if ((a_ & 0x8000) != 0) {
                c_ += qe;
                return;
            }
            if (a_ < qe) a_ = qe;
            else c_ += qe;
            cx = static_cast<Context>(e.nmps << 1 | mps);
        } else {
            if (a_ < qe) c_ += qe;
            else a_ = qe;
            if (e.switchMps) mps ^= 1u;
            cx = static_cast<Context>(e.nlps << 1 | mps);
        }
        do {
            a_ <<= 1;
            c_ <<= 1;
            if (--ct_ == 0) ByteOut();
        } while ((a_ & 0x8000) == 0);
    }

    std::string Finish() {
        const uint32_t top = c_ + a_;
        c_ |= 0xFFFF;
        if (c_ >= top) c_ -= 0x8000;
        c_ <<= ct_;
        ByteOut();
        c_ <<= ct_;
        ByteOut();
        if (out_.back() != 0xFF) out_.push_back(0xFF);
        out_.push_back(0xAC);
        return std::string(out_.begin() + 1, out_.end());
    }

private:
    void ByteOut() {
        if (out_.back() == 0xFF) {
            out_.push_back(static_cast<uint8_t>(c_ >> 20));
            c_ &= 0xFFFFF;
            ct_ = 7;
        } else if (c_ < 0x8000000) {
            out_.push_back(static_cast<uint8_t>(c_ >> 19));
            c_ &= 0x7FFFF;
            ct_ = 8;
        } else {
            out_.back()++;
            if (out_.back() == 0xFF) {
                c_ &= 0x7FFFFFF;
                out_.push_back(static_cast<uint8_t>(c_ >> 20));
                c_ &= 0xFFFFF;
                ct_ = 7;
            } else {
                out_.push_back(static_cast<uint8_t>(c_ >> 19));
                c_ &= 0x7FFFF;
                ct_ = 8;
            }
        }
    }

    std::vector<uint8_t> out_;
    uint32_t a_ = 0x8000;
    uint32_t c_ = 0;
    int ct_ = 12;
};

// Annex A.2 in reverse; `oob` codes OOB.
void EncodeInt(MqEncoder& e, IntContexts& cx, int64_t value, bool oob = false) {
    uint32_t prev = 1;
    const auto bit = [&](uint32_t d) {
        e.Encode(cx.cx[prev], d);
        prev = prev < 256 ? (prev << 1 | d) : (((prev << 1 | d) & 511u) | 256u);
    };
    if (oob) {
        bit(1), bit(0), bit(0), bit(0);
        return;
    }
    const uint64_t magnitude = value < 0 ? uint64_t(-value) : uint64_t(value);
    struct Range {
        uint64_t last;
        uint32_t prefix;
        int prefixBits;
        int bits;
        uint64_t offset;
    };
    static const Range kRanges[] = {{3, 0, 1, 2, 0},        {19, 2, 2, 4, 4},      {83, 6, 3, 6, 20},
                                    {339, 14, 4, 8, 84},    {4435, 30, 5, 12, 340}, {~0ull, 31, 5, 32, 4436}};
    bit(value < 0 ? 1u : 0u);
    for (const Range& r : kRanges) {
        if (magnitude > r.last) continue;
        for (int i = r.prefixBits - 1; i >= 0; i--) bit((r.prefix >> i) & 1u);
        const uint64_t v = magnitude - r.offset;
        for (int i = r.bits - 1; i >= 0; i--) bit(uint32_t(v >> i) & 1u);
        return;
    }
}

void EncodeIaid(MqEncoder& e, Context* cx, int length, uint32_t id) {
    uint32_t prev = 1;
    for (int i = length - 1; i >= 0; i--) {
        const uint32_t d = (id >> i) & 1u;
        e.Encode(cx[prev], d);
        prev = prev << 1 | d;
    }
}

// The generic region templates of T.88 figures 3-6 as (dx, dy) by context bit, bit 0 first;
// (99, k) marks AT pixel A<k+1>.
struct Pixel {
    int dx;
    int dy;
};
const std::vector<Pixel> kGenericTemplates[4] = {
    {{-1, 0}, {-2, 0}, {-3, 0}, {-4, 0}, {99, 0}, {2, -1}, {1, -1}, {0, -1}, {-1, -1}, {-2, -1}, {99, 1}, {99, 2},
     {1, -2}, {0, -2}, {-1, -2}, {99, 3}},
    {{-1, 0}, {-2, 0}, {-3, 0}, {99, 0}, {2, -1}, {1, -1}, {0, -1}, {-1, -1}, {-2, -1}, {2, -2}, {1, -2}, {0, -2},
     {-1, -2}},
    {{-1, 0}, {-2, 0}, {99, 0}, {1, -1}, {0, -1}, {-1, -1}, {-2, -1}, {1, -2}, {0, -2}, {-1, -2}},
    {{-1, 0}, {-2, 0}, {-3, 0}, {-4, 0}, {99, 0}, {1, -1}, {0, -1}, {-1, -1}, {-2, -1}, {-3, -1}},
};
constexpr uint32_t kSltp[4] = {0x9B25, 0x0795, 0x00E5, 0x0195};

// 6.2.5.7 in reverse. `skip` (may be null) marks pixels not coded (they must be 0).
void EncodeGeneric(MqEncoder& e, std::vector<Context>& cx, const Image& img, int templ, bool tpgdon, const int* at,
                   const Image* skip = nullptr) {
    uint32_t ltp = 0;
    for (int y = 0; y < img.height; y++) {
        if (tpgdon) {
            bool same = true;
            for (int x = 0; x < img.width && same; x++) same = img.Get(x, y) == img.Get(x, y - 1);
            e.Encode(cx[kSltp[templ]], (same ? 1u : 0u) ^ ltp);
            ltp = same ? 1u : 0u;
            if (same) continue;
        }
        for (int x = 0; x < img.width; x++) {
            if (skip != nullptr && skip->Get(x, y)) continue;
            uint32_t context = 0;
            const std::vector<Pixel>& t = kGenericTemplates[templ];
            for (size_t b = 0; b < t.size(); b++) {
                const Pixel p = t[b].dx == 99 ? Pixel{at[2 * t[b].dy], at[2 * t[b].dy + 1]} : t[b];
                context |= img.Get(x + p.dx, y + p.dy) << b;
            }
            e.Encode(cx[context], img.Get(x, y));
        }
    }
}

// 6.3.5.6 in reverse, with the decoder's context bit order (any one order is valid as long as
// encoder and decoder agree, apart from the SLTP context: the reference pixel alone).
void EncodeRefinement(MqEncoder& e, std::vector<Context>& cx, const Image& img, const Image& ref, int templ,
                      bool tpgron, const int* at, int64_t dx, int64_t dy) {
    const auto context = [&](int64_t x, int64_t y) {
        const int64_t rx = x - dx;
        const int64_t ry = y - dy;
        if (templ == 0)
            return img.Get(x - 1, y) | img.Get(x + 1, y - 1) << 1 | img.Get(x, y - 1) << 2 |
                   img.Get(x + at[0], y + at[1]) << 3 | ref.Get(rx + 1, ry + 1) << 4 | ref.Get(rx, ry + 1) << 5 |
                   ref.Get(rx - 1, ry + 1) << 6 | ref.Get(rx + 1, ry) << 7 | ref.Get(rx, ry) << 8 |
                   ref.Get(rx - 1, ry) << 9 | ref.Get(rx + 1, ry - 1) << 10 | ref.Get(rx, ry - 1) << 11 |
                   ref.Get(rx + at[2], ry + at[3]) << 12;
        return img.Get(x - 1, y) | img.Get(x + 1, y - 1) << 1 | img.Get(x, y - 1) << 2 | img.Get(x - 1, y - 1) << 3 |
               ref.Get(rx + 1, ry + 1) << 4 | ref.Get(rx, ry + 1) << 5 | ref.Get(rx + 1, ry) << 6 |
               ref.Get(rx, ry) << 7 | ref.Get(rx - 1, ry) << 8 | ref.Get(rx, ry - 1) << 9;
    };
    // The prediction of a pixel: 0 or 1 when its 3 x 3 reference neighbourhood is uniform, else -1.
    const auto predicted = [&](int64_t x, int64_t y) {
        const uint32_t v = ref.Get(x - dx, y - dy);
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++)
                if (ref.Get(x - dx + i, y - dy + j) != v) return -1;
        return int(v);
    };
    uint32_t ltp = 0;
    for (int y = 0; y < img.height; y++) {
        if (tpgron) {
            bool typical = true;
            for (int x = 0; x < img.width && typical; x++) {
                const int p = predicted(x, y);
                typical = p < 0 || uint32_t(p) == img.Get(x, y);
            }
            e.Encode(cx[templ == 0 ? 1u << 8 : 1u << 7], (typical ? 1u : 0u) ^ ltp);
            ltp = typical ? 1u : 0u;
        }
        for (int x = 0; x < img.width; x++) {
            if (ltp && predicted(x, y) >= 0) continue;
            e.Encode(cx[context(x, y)], img.Get(x, y));
        }
    }
}

// ---------------------------------------------------------------- segments

std::string RegionInfo(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t flags) {
    std::string s;
    Put32(s, w), Put32(s, h), Put32(s, x), Put32(s, y);
    s += static_cast<char>(flags);
    return s;
}

std::string PageInfo(uint32_t w, uint32_t h, uint32_t flags, uint32_t striping = 0) {
    std::string s;
    Put32(s, w), Put32(s, h), Put32(s, 0), Put32(s, 0);
    s += static_cast<char>(flags);
    Put16(s, striping);
    return s;
}

// A segment (7.2) with short-form fields; `length` overrides the data length field.
void Segment(std::string& out, uint32_t number, uint32_t type, const std::vector<uint32_t>& refs,
             const std::string& data, uint32_t length = 0xFFFFFFFEu) {
    Put32(out, number);
    out += static_cast<char>(type);
    if (refs.size() <= 4) {
        out += static_cast<char>(refs.size() << 5);
    } else {
        Put32(out, 0xE0000000u | uint32_t(refs.size()));
        out.append((refs.size() + 8) / 8, '\0');
    }
    for (uint32_t r : refs) {
        if (number <= 256) out += static_cast<char>(r);
        else if (number <= 65536) Put16(out, r);
        else Put32(out, r);
    }
    out += '\1';  // page 1
    Put32(out, length == 0xFFFFFFFEu ? uint32_t(data.size()) : length);
    out += data;
}

std::string GenericData(const Image& img, uint32_t x, uint32_t y, uint32_t op, int templ, bool tpgdon, const int* at) {
    std::string d = RegionInfo(uint32_t(img.width), uint32_t(img.height), x, y, op);
    d += static_cast<char>(templ << 1 | (tpgdon ? 8 : 0));
    for (int i = 0; i < (templ == 0 ? 8 : 2); i++) d += static_cast<char>(at[i]);
    MqEncoder e;
    std::vector<Context> cx(65536, 0);
    EncodeGeneric(e, cx, img, templ, tpgdon, at);
    return d + e.Finish();
}

Jbig2::Result Run(const std::string& data, Jbig2::Info& info, std::string& rows, const std::string& globals = "",
                  uint64_t maxPixels = uint64_t(1) << 24) {
    return Jbig2::Decode(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
                         reinterpret_cast<const uint8_t*>(globals.data()), globals.size(), maxPixels, info, rows);
}

void Draw(Image& page, const Image& src, int64_t x, int64_t y, uint32_t op) {
    for (int j = 0; j < src.height; j++)
        for (int i = 0; i < src.width; i++) {
            const int64_t px = x + i;
            const int64_t py = y + j;
            if (px < 0 || py < 0 || px >= page.width || py >= page.height) continue;
            const uint32_t d = page.Get(px, py);
            const uint32_t s = src.Get(i, j);
            uint32_t v = s;
            if (op == 0) v = d | s;
            else if (op == 1) v = d & s;
            else if (op == 2) v = d ^ s;
            else if (op == 3) v = (d ^ s) ^ 1u;
            page.Set(int(px), int(py), v);
        }
}

// ---------------------------------------------------------------- tests

void TestArithmetic() {
    // T.88 H.2: 256 decisions with one context, and the 30 bytes they code to.
    const std::string decisions = Hex("00020051000000C00352872AAAAAAAAA82C02000FCD79EF6BF7FED904F46A3BF");
    const std::string coded = Hex("84C73BFCE1A1430402200000410DBB86F4317FFF88FF37471ADB6ADFFFAC");
    MqDecoder mq;
    mq.Start(reinterpret_cast<const uint8_t*>(coded.data()), coded.size());
    Context cx = 0;
    std::string out(32, '\0');
    for (int i = 0; i < 256; i++)
        if (mq.Decode(cx)) out[size_t(i / 8)] = static_cast<char>(out[size_t(i / 8)] | (0x80 >> (i % 8)));
    RAYOPDF_CHECK(out == decisions, 1100);
    MqEncoder e;
    Context ecx = 0;
    for (int i = 0; i < 256; i++) e.Encode(ecx, (uint8_t(decisions[size_t(i / 8)]) >> (7 - i % 8)) & 1u);
    RAYOPDF_CHECK(e.Finish() == coded, 1101);

    // Integers (A.2) over every range, OOB, and IAID (A.3).
    const int64_t values[] = {0, 1, -1, 3, -3, 4, 19, -20, 83, 84, -339, 340, 4435, -4436, 70000, -1000000,
                              4294967295ll + 4436};
    MqEncoder ie;
    IntContexts icx;
    icx.Reset();
    for (int64_t v : values) EncodeInt(ie, icx, v);
    EncodeInt(ie, icx, 0, true);
    std::vector<Context> idcx(1 << 10, 0);
    for (uint32_t id = 0; id < 1024; id += 37) EncodeIaid(ie, idcx.data(), 10, id);
    const std::string ints = ie.Finish();
    MqDecoder id;
    id.Start(reinterpret_cast<const uint8_t*>(ints.data()), ints.size());
    IntContexts dcx;
    dcx.Reset();
    for (int64_t v : values) {
        int64_t got = -7;
        RAYOPDF_CHECK(DecodeInt(id, dcx, got) && got == v, 1102);
    }
    int64_t oob = 0;
    RAYOPDF_CHECK(!DecodeInt(id, dcx, oob), 1102);
    std::vector<Context> didcx(1 << 10, 0);
    for (uint32_t v = 0; v < 1024; v += 37) RAYOPDF_CHECK(DecodeIaid(id, didcx.data(), 10) == v, 1103);
}

Code DecodeBits(const char* bits, int table, int64_t& value) {
    BitWriter w;
    w.Code(bits);
    BitReader r(reinterpret_cast<const uint8_t*>(w.Bytes().data()), w.Bytes().size());
    return DecodeHuffman(r, StandardTable(table), value);
}

void TestHuffman() {
    // Codes quoted in T.88 H.1.
    int64_t v = 0;
    RAYOPDF_CHECK(DecodeBits("1110 100", 4, v) == Code::Value && v == 8, 1104);
    RAYOPDF_CHECK(DecodeBits("1110 010", 2, v) == Code::Value && v == 5, 1104);
    RAYOPDF_CHECK(DecodeBits("1110 011", 2, v) == Code::Value && v == 6, 1104);
    RAYOPDF_CHECK(DecodeBits("111111", 2, v) == Code::Oob, 1104);
    RAYOPDF_CHECK(DecodeBits("01000", 1, v) == Code::Value && v == 8, 1104);
    RAYOPDF_CHECK(DecodeBits("00010", 1, v) == Code::Value && v == 2, 1104);
    RAYOPDF_CHECK(DecodeBits("00 0000000", 6, v) == Code::Value && v == 0, 1104);
    RAYOPDF_CHECK(DecodeBits("00 0", 8, v) == Code::Value && v == 0, 1104);
    RAYOPDF_CHECK(DecodeBits("0", 12, v) == Code::Value && v == 1, 1104);
    RAYOPDF_CHECK(DecodeBits("10", 12, v) == Code::Value && v == 2, 1104);
    // Range lines: B.3's lower line (11111111 + 32 bits: -257 - n), B.15's upper (1111111: 25 + n),
    // B.8's OOB (01), B.14 (no ranges).
    RAYOPDF_CHECK(DecodeBits("11111111 00000000000000000000000000000101", 3, v) == Code::Value && v == -262, 1105);
    RAYOPDF_CHECK(DecodeBits("1111111 00000000000000000000000000000011", 15, v) == Code::Value && v == 28, 1105);
    RAYOPDF_CHECK(DecodeBits("01", 8, v) == Code::Oob, 1105);
    RAYOPDF_CHECK(DecodeBits("100", 14, v) == Code::Value && v == -2, 1105);
    RAYOPDF_CHECK(DecodeBits("1", 1, v) == Code::Error, 1105);  // data ends inside a code

    // A code table segment (B.2): OOB, prefix lengths in 3 bits, range lengths in 4, HTLOW -10,
    // HTHIGH 3. Lines -10..-7 (prefix 3, range 2), -6 (1, 0), -5..2 (3, 3); lower 4, upper 4,
    // OOB 3. B.3 codes: -6 '0', -10.. '100', -5.. '101', OOB '110', lower '1110', upper '1111'.
    std::string good;
    good += static_cast<char>(1 | (3 - 1) << 1 | (4 - 1) << 4);
    Put32(good, uint32_t(-10)), Put32(good, 3);
    BitWriter gw;
    gw.Bits(3, 3), gw.Bits(2, 4), gw.Bits(1, 3), gw.Bits(0, 4), gw.Bits(3, 3), gw.Bits(3, 4);
    gw.Bits(4, 3), gw.Bits(4, 3), gw.Bits(3, 3);
    good += gw.Bytes();
    HuffTable user;
    RAYOPDF_CHECK(ParseTableSegment(reinterpret_cast<const uint8_t*>(good.data()), good.size(), user) == Status::Ok,
                  1106);
    const auto decode = [&](const char* bits, int64_t& value) {
        BitWriter w;
        w.Code(bits);
        BitReader r(reinterpret_cast<const uint8_t*>(w.Bytes().data()), w.Bytes().size());
        return DecodeHuffman(r, user, value);
    };
    RAYOPDF_CHECK(decode("0", v) == Code::Value && v == -6, 1106);
    RAYOPDF_CHECK(decode("100 11", v) == Code::Value && v == -7, 1106);
    RAYOPDF_CHECK(decode("101 111", v) == Code::Value && v == 2, 1106);
    RAYOPDF_CHECK(decode("110", v) == Code::Oob, 1106);
    RAYOPDF_CHECK(decode("1110 00000000000000000000000000000000", v) == Code::Value && v == -11, 1106);
    RAYOPDF_CHECK(decode("1111 00000000000000000000000000001000", v) == Code::Value && v == 11, 1106);
    // Refused: prefix lengths 2, 1, 3, 4, 4, 3 overflow the 4-bit codes (B.3 gives the 4-bit lines
    // the first code 10000); a range length past 32; data ending inside the lines.
    std::string overflow = good.substr(0, 9);
    BitWriter ow;
    ow.Bits(2, 3), ow.Bits(2, 4), ow.Bits(1, 3), ow.Bits(0, 4), ow.Bits(3, 3), ow.Bits(3, 4);
    ow.Bits(4, 3), ow.Bits(4, 3), ow.Bits(3, 3);
    overflow += ow.Bytes();
    RAYOPDF_CHECK(ParseTableSegment(reinterpret_cast<const uint8_t*>(overflow.data()), overflow.size(), user) ==
                      Status::Corrupt,
                  1106);
    std::string wide = good.substr(0, 9);
    wide[0] = static_cast<char>((3 - 1) << 1 | (8 - 1) << 4);
    BitWriter ww;
    ww.Bits(1, 3), ww.Bits(33, 8), ww.Bits(0, 16);
    wide += ww.Bytes();
    RAYOPDF_CHECK(ParseTableSegment(reinterpret_cast<const uint8_t*>(wide.data()), wide.size(), user) == Status::Corrupt,
                  1106);
    RAYOPDF_CHECK(ParseTableSegment(reinterpret_cast<const uint8_t*>(good.data()), 10, user) == Status::Corrupt, 1106);

    // 7.4.3.1.7 example 1: 32 symbol ID code lengths.
    const std::string ids = Hex("500335325300000000" "00000000000000350F" "8B309EB85F1DD28300");
    BitReader r(reinterpret_cast<const uint8_t*>(ids.data()), 27);
    std::vector<HuffLine> lines;
    HuffTable codes;
    RAYOPDF_CHECK(ReadSymbolCodes(r, 32, lines, codes) == Status::Ok && r.Position() == 27, 1107);
    const int expected[32] = {0, 0, 0, 9, 6, 6, 6, 6, 3, 4, 4, 4, 4, 4, 4, 0, 7, 9, 8, 7, 5, 5, 5, 5, 5, 5, 3, 6, 7, 4, 7, 7};
    for (int i = 0; i < 32; i++) RAYOPDF_CHECK(lines[size_t(i)].prefixLength == expected[i], 1107);
}

// T.88 H.1: a file header, then 21 segments; page 1 (segments 1-7) and page 2 (8-14) code the
// same 64 x 56 page with Huffman/MMR and with arithmetic coding, page 3 (15-19) a 37 x 8 page.
constexpr char kAnnexH[] =
    "974A42320D0A1A0A0100000003000000000001000000001800010000000100000001E9CBF40026AF04BFF0782FE00040"
    "000000013000010000001300000040000000380000000000000000010000000000020001010000001C00010000000200"
    "000002E5CDF80079E0841081F08210861079F00080000000030742000201000000310000002500000008000000040000"
    "0001000C0900100000000501100000000000000000000000000000000C4007087041D0000000042700010000002C0000"
    "00360000002C000000040000000B000126A071CEA7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF8F0000000051001"
    "010000002D0104040000000F20D184611845F2F97C8F11C39E45F2F97D42850AAA84622FEEEC446222352A0A83B9DCEE"
    "77800000000617200501000000570000002000000024000000100000000F000100000008000000090000000000000000"
    "04000000AAAAAAAA8008008036D5556B5AD40040042EE952D2D2D28AA54A00200223E09524B4928A4A925492D24A292A"
    "494004004000000007310001000000000000000830000200000013000000400000003800000000000000000100000000"
    "00090001020000001B080002FF00000002000000024FE78C200E1DC7CF0111C4B26FFFAC0000000A0740000902000000"
    "1F00000025000000080000000400000001000C08000000058D6E5A124085FFAC0000000B270002000000230000003600"
    "00002C000000040000000B000803FFFDFF02FEFEFE04EEED87FBCB2BFFAC0000000C1001020000001C0604040000000F"
    "90716B6D99A7AA497DF2E5481FDC68BC6E40BBFFAC0000000D17200C020000003E000000200000002400000010000000"
    "0F0002000000080000000900000000000000000400000087CB821E66A414EB3C4A15FACCD6F3B16F4CEDBFA7BFFFAC00"
    "00000E310002000000000000000F30000300000013000000250000000800000000000000000100000000001000010000"
    "000016080002FF00000001000000014FE78D681B142F3FFFAC000000110021100300000020080202FFFFFFFFFF000000"
    "03000000024FE9D7D590C3B526A7FB6D14983FFFAC000000120720110300000025000000250000000800000000000000"
    "00008C1200000004A95C8BF4C37D966A28E5768FFFAC00000013310003000000000000001433000000000000";

// The page of H.1 as poppler 26.10 decodes page 2 (pdfimages, PBM rows: 1 = black).
constexpr char kAnnexHPage[] =
    "000000000000000007878F0F0F00000008404880908000000807C88F90000000080848909000000008484F10908000000787C80F8F000000"
    "00000800000000000000080000000000000000000000000000000000000000000FFFFFFFFFFFFFC00FFFFFFFFFFFFFC00C000000000000C0"
    "0C000000000000C00C000000022200C00C00026666EE00C00C000002666600C00C000000000400C00C000000222200C00C0026666EEE00C0"
    "0C000026666700C00C000000004400C00C000002222600C00C006666EEEE00C00C000266667F00C00C000000044400C00C000022226600C0"
    "0C00666EEEEE00C00C00266667FF00C00C000000444600C00C000222266600C00C0066EEEEEF00C00C0066667FFF00C00C000004446600C0"
    "0C002222666E00C00C006EEEEEFF00C00C006667FFFF00C00C000044466600C00C00222666EF00C00C00EEEEEFFF00C00C00667FFFFF00C0"
    "0C000444666600C00C0022666EFF00C00C00EEEEFFFF00C00C0067FFFFFF00C00C004446666700C00C002666EFFF00C00C00EEEFFFFF00C0"
    "0C007FFFFFFF00C00C004466667F00C00C000000000000C00C000000000000C00FFFFFFFFFFFFFC00FFFFFFFFFFFFFC00000000000000000";

void TestAnnexH() {
    const std::string file = Hex(kAnnexH);
    RAYOPDF_CHECK(file.size() == 0x35C, 1110);
    const std::string globals = file.substr(0x0D, 0x30 - 0x0D);  // segment 0, no page
    const std::string page1 = file.substr(0x30, 0x190 - 0x30);
    const std::string page2 = file.substr(0x190, 0x2AA - 0x190);
    const std::string page3 = file.substr(0x2AA, 0x351 - 0x2AA);
    const std::string poppler = Hex(kAnnexHPage);
    Image expected(64, 56);
    for (int y = 0; y < 56; y++)
        for (int x = 0; x < 64; x++) expected.Set(x, y, (uint8_t(poppler[size_t(y * 8 + x / 8)]) >> (7 - x % 8)) & 1u);
    Jbig2::Info info;
    std::string rows;
    RAYOPDF_CHECK(Run(page1, info, rows, globals) == Jbig2::Result::Ok && !info.damaged, 1110);
    RAYOPDF_CHECK(SamePage(rows, info, expected), 1110);
    RAYOPDF_CHECK(Run(page2, info, rows, globals) == Jbig2::Result::Ok && !info.damaged, 1111);
    RAYOPDF_CHECK(SamePage(rows, info, expected), 1111);
    // Page 3 is page 1's text region: the 37 x 8 pixels at (4, 1).
    Image text(37, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 37; x++) text.Set(x, y, expected.Get(x + 4, y + 1));
    RAYOPDF_CHECK(Run(page3, info, rows) == Jbig2::Result::Ok && !info.damaged, 1112);
    RAYOPDF_CHECK(SamePage(rows, info, text), 1112);
    // Page 1 without its globals: the text region's dictionary is missing.
    RAYOPDF_CHECK(Run(page1, info, rows) == Jbig2::Result::Ok && info.damaged, 1113);
    // The whole file as one stream (its file header is no segment): corrupt.
    RAYOPDF_CHECK(Run(file, info, rows) == Jbig2::Result::Corrupt && rows.empty(), 1113);
    // Globals and page in one stream decode the first page and stop at its end of page.
    RAYOPDF_CHECK(Run(file.substr(0x0D), info, rows) == Jbig2::Result::Ok && SamePage(rows, info, expected), 1113);
}

void TestGeneric() {
    // Every template, with and without TPGDON, AT pixels nominal, near (inside the row windows)
    // and far (read from the bitmap); the region at (3, 4) of an 80 x 50 page.
    const int atSets[3][8] = {{3, -1, -3, -1, 2, -2, -2, -2}, {7, -1, -8, -2, -8, 0, 5, -2}, {20, -3, -9, 0, 0, -5, 100, -1}};
    int tested = 0;
    for (int templ = 0; templ < 4; templ++)
        for (int tpgdon = 0; tpgdon < 2; tpgdon++)
            for (const int* at : atSets) {
                int nominal[8];
                std::memcpy(nominal, at, sizeof nominal);
                if (templ != 0 && at == atSets[0]) nominal[0] = templ <= 1 ? 3 : 2;
                const Image img = RandomImage(70, 37, 20 + templ * 5);
                std::string s;
                Segment(s, 0, 48, {}, PageInfo(80, 50, 0));
                Segment(s, 1, 39, {}, GenericData(img, 3, 4, 0, templ, tpgdon != 0, nominal));
                Segment(s, 2, 49, {}, "");
                Image page(80, 50);
                Draw(page, img, 3, 4, 0);
                Jbig2::Info info;
                std::string rows;
                RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && !info.damaged, 1115);
                RAYOPDF_CHECK(SamePage(rows, info, page), 1115);
                tested++;
            }
    RAYOPDF_CHECK(tested == 24, 1115);

    // An immediate generic region of unknown length (7.2.7): the row count after 0xFF 0xAC
    // cuts the region to its first 30 rows.
    {
        const int at[8] = {3, -1, -3, -1, 2, -2, -2, -2};
        const Image img = RandomImage(33, 40, 30);
        std::string data = GenericData(img, 0, 0, 0, 0, true, at);
        Put32(data, 30);
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(33, 0xFFFFFFFFu, 0, 0x8000 | 64));
        Segment(s, 1, 38, {}, data, 0xFFFFFFFFu);
        Segment(s, 2, 49, {}, "");
        Image page(33, 30);
        Draw(page, img, 0, 0, 0);
        Jbig2::Info info;
        std::string rows;
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && SamePage(rows, info, page), 1116);
        // The marker missing: corrupt before the page has anything; the page so far is kept.
        std::string cut = s.substr(0, s.size() - 11 - 6);
        RAYOPDF_CHECK(Run(cut, info, rows) == Jbig2::Result::Ok && info.damaged && info.height == 0, 1116);
    }

    // The 12 AT pixels of EXTTEMPLATE: not read.
    {
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(8, 8, 0));
        std::string d = RegionInfo(8, 8, 0, 0, 0);
        d += static_cast<char>(0x10);
        d.append(24, '\0');
        Segment(s, 1, 39, {}, d);
        Jbig2::Info info;
        std::string rows;
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Unsupported && rows.empty(), 1117);
    }
}

void TestRefinementAndComposition() {
    const int at[8] = {3, -1, -3, -1, 2, -2, -2, -2};
    const int rat[4] = {-1, -1, -1, -1};
    const int rat2[4] = {2, -3, -2, 4};
    // An immediate refinement of the page (no reference segment) replaces the region it refines:
    // templates 0 (nominal and moved AT pixels) and 1, with TPGRON.
    for (int variant = 0; variant < 3; variant++) {
        const int templ = variant == 2 ? 1 : 0;
        const int* ra = variant == 1 ? rat2 : rat;
        const Image base = RandomImage(60, 40, 30);
        Image refined(31, 22);
        for (int y = 0; y < 22; y++)
            for (int x = 0; x < 31; x++) refined.Set(x, y, base.Get(x + 12, y + 9) ^ (Random() % 23 == 0 ? 1u : 0u));
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(60, 40, 2));
        Segment(s, 1, 39, {}, GenericData(base, 0, 0, 0, 0, false, at));
        std::string d = RegionInfo(31, 22, 12, 9, 4);
        d += static_cast<char>(templ | 2);
        if (templ == 0)
            for (int i = 0; i < 4; i++) d += static_cast<char>(ra[i]);
        Image reference(31, 22);
        for (int y = 0; y < 22; y++)
            for (int x = 0; x < 31; x++) reference.Set(x, y, base.Get(x + 12, y + 9));
        MqEncoder e;
        std::vector<Context> cx(8192, 0);
        EncodeRefinement(e, cx, refined, reference, templ, true, ra, 0, 0);
        Segment(s, 2, 43, {}, d + e.Finish());
        Segment(s, 3, 49, {}, "");
        Image page = base;
        Draw(page, refined, 12, 9, 4);
        Jbig2::Info info;
        std::string rows;
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && !info.damaged, 1118);
        RAYOPDF_CHECK(SamePage(rows, info, page), 1118);
    }

    // Intermediate generic region, refined by an intermediate refinement region, refined again by
    // an immediate one drawn with XOR on a page of default pixel 1 (8.2 step 5 b, e, d).
    {
        const Image first = RandomImage(40, 25, 25);
        Image second = first;
        Image third = first;
        for (int k = 0; k < 40; k++) second.Set(int(Random() % 40), int(Random() % 25), 1);
        third = second;
        for (int k = 0; k < 40; k++) third.Set(int(Random() % 40), int(Random() % 25), 0);
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(50, 30, 4));
        Segment(s, 1, 36, {}, GenericData(first, 5, 2, 2, 0, false, at));
        for (int step = 0; step < 2; step++) {
            std::string d = RegionInfo(40, 25, 5, 2, 2);
            d += static_cast<char>(1);  // template 1, no TPGRON
            MqEncoder e;
            std::vector<Context> cx(8192, 0);
            EncodeRefinement(e, cx, step == 0 ? second : third, step == 0 ? first : second, 1, false, rat, 0, 0);
            Segment(s, uint32_t(2 + step), step == 0 ? 40 : 42, {uint32_t(1 + step)}, d + e.Finish());
        }
        Segment(s, 4, 49, {}, "");
        Image page(50, 30, 1);
        Draw(page, third, 5, 2, 2);
        Jbig2::Info info;
        std::string rows;
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && !info.damaged && SamePage(rows, info, page), 1119);
    }

    // Every external combination operator, on a page of default pixel 1, regions overlapping.
    {
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(45, 31, 4 | 64));
        Image page(45, 31, 1);
        for (uint32_t op = 0; op < 5; op++) {
            const Image img = RandomImage(17 + int(op), 13, 40);
            const uint32_t x = op * 7;
            const uint32_t y = op * 4;
            Segment(s, 1 + op, 39, {}, GenericData(img, x, y, op, 1, false, at));
            Draw(page, img, x, y, op);
        }
        // A region partly outside the page is cut.
        const Image wide = RandomImage(30, 10, 50);
        Segment(s, 6, 39, {}, GenericData(wide, 30, 25, 2, 2, true, at));
        Draw(page, wide, 30, 25, 2);
        Segment(s, 7, 49, {}, "");
        Jbig2::Info info;
        std::string rows;
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && SamePage(rows, info, page), 1120);
    }

    // A striped page of unknown height (default pixel 1) grows with its regions and end of stripe
    // segments (7.4.8.2, 7.4.10).
    {
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(21, 0xFFFFFFFFu, 4, 0x8000 | 16));
        const Image a = RandomImage(21, 10, 30);
        const Image b = RandomImage(15, 5, 30);
        Segment(s, 1, 39, {}, GenericData(a, 0, 0, 2, 3, false, at));
        std::string row;
        Put32(row, 14);
        Segment(s, 2, 50, {}, row);
        Segment(s, 3, 39, {}, GenericData(b, 2, 15, 1, 3, false, at));
        row.clear();
        Put32(row, 24);
        Segment(s, 4, 50, {}, row);
        Segment(s, 5, 49, {}, "");
        Image page(21, 25, 1);
        Draw(page, a, 0, 0, 2);
        Draw(page, b, 2, 15, 1);
        Jbig2::Info info;
        std::string rows;
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && SamePage(rows, info, page), 1121);
    }
}

// A symbol dictionary segment's data (arithmetic, template `templ`, nominal AT), coding `symbols`
// in height classes of their order (each class one height), exporting all of them.
std::string SymbolDictionary(const std::vector<Image>& symbols, int templ, std::vector<Context>& gb, uint32_t flags = 0,
                             uint32_t numInputs = 0) {
    std::string d;
    Put16(d, flags | uint32_t(templ) << 10);
    const int at[8] = {templ <= 1 ? 3 : 2, -1, -3, -1, 2, -2, -2, -2};
    for (int i = 0; i < (templ == 0 ? 8 : 2); i++) d += static_cast<char>(at[i]);
    Put32(d, uint32_t(symbols.size()) + numInputs);
    Put32(d, uint32_t(symbols.size()));
    MqEncoder e;
    IntContexts iadh, iadw, iaex;
    iadh.Reset(), iadw.Reset(), iaex.Reset();
    int height = 0;
    size_t i = 0;
    while (i < symbols.size()) {
        EncodeInt(e, iadh, symbols[i].height - height);
        height = symbols[i].height;
        int width = 0;
        for (; i < symbols.size() && symbols[i].height == height; i++) {
            EncodeInt(e, iadw, symbols[i].width - width);
            width = symbols[i].width;
            EncodeGeneric(e, gb, symbols[i], templ, false, at);
        }
        EncodeInt(e, iadw, 0, true);
    }
    // Export the inputs and the new symbols: a run of 0 not exported, then all.
    EncodeInt(e, iaex, 0);
    EncodeInt(e, iaex, int64_t(symbols.size() + numInputs));
    return d + e.Finish();
}

void TestText() {
    std::vector<Image> symbols;
    symbols.push_back(RandomImage(5, 7, 45));
    symbols.push_back(RandomImage(9, 7, 45));
    symbols.push_back(RandomImage(4, 11, 45));
    symbols.push_back(RandomImage(13, 11, 45));
    std::vector<Context> gb(65536, 0);
    const std::string dictionary = SymbolDictionary(symbols, 1, gb);

    // Instances by (symbol, S of the leading edge, T within the strip), two strips of 4 rows (T
    // bases 4 and 16); the third refined. The page position follows from the reference corner
    // (6.4.5 3c vi-x): along S the instance starts at its S; along T its corner sits at T.
    struct Instance {
        uint32_t id;
        int s;
        int t;
        bool refine;
    };
    const Instance instances[] = {{0, 2, 0, false}, {1, 9, 1, false}, {3, 20, 3, true},  {2, 40, 2, false},
                                  {2, 3, 0, false}, {0, 6, 3, false}, {1, 30, 1, false}, {3, 44, 2, false}};
    const int bases[2] = {4, 16};
    const int rat[4] = {-1, -1, -1, -1};
    for (int transposed = 0; transposed < 2; transposed++)
        for (int corner = 0; corner < 4; corner++) {
            const bool right = corner == 2 || corner == 3;
            const bool bottom = corner == 0 || corner == 2;
            const int strips = 4;
            const int dsOffset = -2;
            std::string d = RegionInfo(64, 50, 6, 3, 0);
            const uint32_t flags = 2 /* refine */ | 2u << 2 /* 4 strips */ | uint32_t(corner) << 4 |
                                   uint32_t(transposed) << 6 | 2u << 7 /* XOR */ | 1u << 9 /* default 1 */ |
                                   (uint32_t(dsOffset) & 31u) << 10;
            Put16(d, flags);
            for (int i = 0; i < 4; i++) d += static_cast<char>(rat[i]);
            Put32(d, 8);
            MqEncoder e;
            IntContexts iadt, iafs, iads, iait, iari, iardw, iardh, iardx, iardy;
            for (IntContexts* c : {&iadt, &iafs, &iads, &iait, &iari, &iardw, &iardh, &iardx, &iardy}) c->Reset();
            std::vector<Context> iaid(4, 0), gr(8192, 0);
            Image region(64, 50, 1);
            int stripT = 0;
            int firstS = 0;
            EncodeInt(e, iadt, 0);  // initial STRIPT 0
            for (int strip = 0; strip < 2; strip++) {
                const Instance* group = instances + strip * 4;
                EncodeInt(e, iadt, (bases[strip] - stripT) / strips);
                stripT = bases[strip];
                EncodeInt(e, iafs, group[0].s - firstS);
                firstS = group[0].s;
                int curS = 0;
                for (int k = 0; k < 4; k++) {
                    const Instance& in = group[k];
                    Image img = symbols[in.id];
                    if (k > 0) EncodeInt(e, iads, in.s - curS - dsOffset);
                    EncodeInt(e, iait, in.t);
                    EncodeIaid(e, iaid.data(), 2, in.id);
                    EncodeInt(e, iari, in.refine ? 1 : 0);
                    if (in.refine) {
                        // RDW 2, RDH -1, RDX -1, RDY 1: GRREFERENCEDX = floor(2 / 2) - 1 = 0,
                        // GRREFERENCEDY = floor(-1 / 2) + 1 = 0 (table 12).
                        Image r(img.width + 2, img.height - 1);
                        for (int y = 0; y < r.height; y++)
                            for (int x = 0; x < r.width; x++) r.Set(x, y, img.Get(x, y) ^ ((x + y) % 9 == 0 ? 1u : 0u));
                        EncodeInt(e, iardw, 2), EncodeInt(e, iardh, -1), EncodeInt(e, iardx, -1), EncodeInt(e, iardy, 1);
                        EncodeRefinement(e, gr, r, img, 0, false, rat, 0, 0);
                        img = r;
                    }
                    const int t = stripT + in.t;
                    const int x = transposed ? (right ? t - img.width + 1 : t) : in.s;
                    const int y = transposed ? in.s : (bottom ? t - img.height + 1 : t);
                    Draw(region, img, x, y, 2);
                    curS = in.s + (transposed ? img.height : img.width) - 1;
                }
                EncodeInt(e, iads, 0, true);
            }
            std::string s;
            Segment(s, 1, 0, {}, dictionary);
            Segment(s, 2, 48, {}, PageInfo(70, 45, 0));
            Segment(s, 3, 6, {1}, d + e.Finish());
            Segment(s, 4, 49, {}, "");
            Image page(70, 45);
            Draw(page, region, 6, 3, 0);
            Jbig2::Info info;
            std::string rows;
            RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && !info.damaged, 1122);
            RAYOPDF_CHECK(SamePage(rows, info, page), 1122);
        }

    // Two dictionaries, the second reusing the first's retained statistics (7.4.2.2 steps 3 and
    // 7) and re-exporting its symbols; a text region of one instance of each of the six symbols.
    {
        std::vector<Context> stats(65536, 0);
        std::vector<Image> first = {RandomImage(6, 8, 40), RandomImage(7, 8, 40)};
        std::vector<Image> second = {RandomImage(5, 9, 40), RandomImage(8, 9, 40), RandomImage(3, 10, 40),
                                     RandomImage(4, 10, 40)};
        const std::string a = SymbolDictionary(first, 0, stats, 1u << 9);
        const std::string b = SymbolDictionary(second, 0, stats, 1u << 8 | 1u << 9, 2);
        std::string d = RegionInfo(60, 12, 0, 0, 0);
        Put16(d, 1u << 4 /* TOPLEFT */);
        Put32(d, 6);
        MqEncoder e;
        IntContexts iadt, iafs, iads;
        iadt.Reset(), iafs.Reset(), iads.Reset();
        std::vector<Context> iaid(8, 0);
        EncodeInt(e, iadt, 0);
        EncodeInt(e, iadt, 0);
        EncodeInt(e, iafs, 1);
        Image page(60, 12);
        int x = 1;
        const Image* all[6] = {&first[0], &first[1], &second[0], &second[1], &second[2], &second[3]};
        for (uint32_t k = 0; k < 6; k++) {
            if (k > 0) EncodeInt(e, iads, 1);
            EncodeIaid(e, iaid.data(), 3, k);
            Draw(page, *all[k], x, 0, 0);
            x += all[k]->width;  // CURS = x + width - 1, plus IDS 1
        }
        EncodeInt(e, iads, 0, true);
        std::string s;
        Segment(s, 0, 0, {}, a);
        Segment(s, 1, 0, {0}, b);
        Segment(s, 2, 48, {}, PageInfo(60, 12, 0));
        Segment(s, 3, 7, {1}, d + e.Finish());
        Segment(s, 4, 49, {}, "");
        Jbig2::Info info;
        std::string rows;
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && !info.damaged && SamePage(rows, info, page), 1123);
        // The second dictionary without the retained statistics decodes other symbols.
        std::string t;
        Segment(t, 0, 0, {}, a);
        std::string b2 = b;
        b2[0] = static_cast<char>(uint8_t(b2[0]) & ~1u);  // "bitmap coding context used" off
        Segment(t, 1, 0, {0}, b2);
        Segment(t, 2, 48, {}, PageInfo(60, 12, 0));
        Segment(t, 3, 7, {1}, d + e.Finish());
        const Jbig2::Result fresh = Run(t, info, rows);
        RAYOPDF_CHECK(fresh != Jbig2::Result::Ok || !SamePage(rows, info, page), 1123);
    }
}

void TestHalftone() {
    // A pattern dictionary of 6 patterns 3 x 2 (template 0, A1 = (-HDPW, 0)), and a halftone
    // region (template 2, HENABLESKIP, XOR) on a rotated grid, some cells outside the region.
    const int width = 3;
    const int height = 2;
    const int count = 6;
    Image collective(width * count, height);
    for (int i = 0; i < count; i++)
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) collective.Set(i * width + x, y, (y * width + x) < i ? 1u : 0u);
    std::string pd;
    pd += static_cast<char>(0);
    pd += static_cast<char>(width);
    pd += static_cast<char>(height);
    Put32(pd, count - 1);
    {
        MqEncoder e;
        std::vector<Context> cx(65536, 0);
        const int at[8] = {-width, 0, -3, -1, 2, -2, -2, -2};
        EncodeGeneric(e, cx, collective, 0, false, at);
        pd += e.Finish();
    }
    const uint32_t gw = 9;
    const uint32_t gh = 7;
    const int64_t hgx = -300;
    const int64_t hgy = 200;
    const int64_t hrx = 700;
    const int64_t hry = 150;
    const int rw = 30;
    const int rh = 26;
    Image values(static_cast<int>(gw), static_cast<int>(gh));
    Image skip(static_cast<int>(gw), static_cast<int>(gh));
    Image region(rw, rh);
    const auto floor8 = [](int64_t v) { return v >= 0 ? v / 256 : -((-v + 255) / 256); };
    for (uint32_t m = 0; m < gh; m++)
        for (uint32_t n = 0; n < gw; n++) {
            const int64_t x = floor8(hgx + int64_t(m) * hry + int64_t(n) * hrx);
            const int64_t y = floor8(hgy + int64_t(m) * hrx - int64_t(n) * hry);
            const uint32_t v = Random() % count;
            values.Set(int(n), int(m), v);
            if (x + width <= 0 || x >= rw || y + height <= 0 || y >= rh) {
                skip.Set(int(n), int(m), 1);
                continue;
            }
            Image pattern(width, height);
            for (int j = 0; j < height; j++)
                for (int i = 0; i < width; i++) pattern.Set(i, j, collective.Get(int(v) * width + i, j));
            Draw(region, pattern, x, y, 2);
        }
    RAYOPDF_CHECK(skip.Get(8, 0) == 1, 1124);  // the grid leaves the region
    std::string hd = RegionInfo(rw, rh, 2, 1, 0);
    hd += static_cast<char>(2 << 1 | 8 | 2 << 4);  // template 2, skip, XOR, default 0
    Put32(hd, gw), Put32(hd, gh), Put32(hd, uint32_t(hgx)), Put32(hd, uint32_t(hgy));
    Put16(hd, uint32_t(hrx)), Put16(hd, uint32_t(hry));
    {
        // Gray-coded planes, most significant first, skipped cells 0.
        MqEncoder e;
        std::vector<Context> cx(65536, 0);
        const int at[8] = {2, -1, -3, -1, 2, -2, -2, -2};
        for (int j = 2; j >= 0; j--) {
            Image plane(static_cast<int>(gw), static_cast<int>(gh));
            for (uint32_t m = 0; m < gh; m++)
                for (uint32_t n = 0; n < gw; n++) {
                    const uint32_t v = values.Get(n, m);
                    plane.Set(int(n), int(m), skip.Get(n, m) ? 0u : (((v >> j) ^ (v >> (j + 1))) & 1u));
                }
            EncodeGeneric(e, cx, plane, 2, false, at, &skip);
        }
        hd += e.Finish();
    }
    std::string s;
    Segment(s, 0, 48, {}, PageInfo(40, 30, 0));
    Segment(s, 1, 16, {}, pd);
    Segment(s, 2, 22, {1}, hd);
    Segment(s, 3, 49, {}, "");
    Image page(40, 30);
    Draw(page, region, 2, 1, 0);
    Jbig2::Info info;
    std::string rows;
    RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && !info.damaged, 1124);
    RAYOPDF_CHECK(SamePage(rows, info, page), 1124);
}

void TestLimits() {
    Jbig2::Info info;
    std::string rows;
    const int at[8] = {3, -1, -3, -1, 2, -2, -2, -2};
    // No page information: corrupt; empty input too.
    RAYOPDF_CHECK(Run("", info, rows) == Jbig2::Result::Corrupt && rows.empty(), 1130);
    {
        std::string s;
        Segment(s, 0, 49, {}, "");
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Corrupt, 1130);
    }
    // A page above the pixel limit; a region above it after the page.
    {
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(100000, 100000, 0));
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::TooLarge && rows.empty(), 1131);
        std::string t;
        Segment(t, 0, 48, {}, PageInfo(10, 10, 0));
        std::string d = RegionInfo(0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0);
        d += '\0';
        d.append(8, '\0');
        Segment(t, 1, 39, {}, d);
        RAYOPDF_CHECK(Run(t, info, rows) == Jbig2::Result::TooLarge, 1131);
        // A page of unknown height grown past the limit by an end of stripe row.
        std::string u;
        Segment(u, 0, 48, {}, PageInfo(4096, 0xFFFFFFFFu, 0, 0x8000 | 1));
        std::string row;
        Put32(row, 0x7FFFFFFF);
        Segment(u, 1, 50, {}, row);
        RAYOPDF_CHECK(Run(u, info, rows) == Jbig2::Result::TooLarge, 1131);
    }
    // Truncated data: the page as far as it was decoded, marked damaged; a header cut short ends
    // the stream with what came before.
    {
        const Image img = RandomImage(40, 30, 30);
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(40, 30, 0));
        Segment(s, 1, 39, {}, GenericData(img, 0, 0, 0, 0, false, at));
        RAYOPDF_CHECK(Run(s.substr(0, s.size() - 20), info, rows) == Jbig2::Result::Ok && info.damaged, 1132);
        RAYOPDF_CHECK(info.width == 40 && info.height == 30 && rows.size() == 5 * 30, 1132);
        std::string t = s;
        t += std::string("\0\0\0\2\x31", 5);
        RAYOPDF_CHECK(Run(t, info, rows) == Jbig2::Result::Ok, 1132);
    }
    // Reference and count limits: a segment referring to 2^17 segments; a missing segment; a
    // dictionary of 2^21 symbols; an unknown length on another type; a second page ends the first.
    {
        std::string s;
        Segment(s, 0, 48, {}, PageInfo(8, 8, 0));
        Put32(s, 1);
        s += static_cast<char>(6);
        Put32(s, 0xE0000000u | (1u << 17));
        s.append(64, '\0');
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::TooLarge, 1133);
        std::string t;
        Segment(t, 0, 48, {}, PageInfo(8, 8, 0));
        std::string d = RegionInfo(8, 8, 0, 0, 0);
        d += std::string("\0\0\0\0\0\0\0\0\0", 9);
        Segment(t, 1, 6, {7}, d);
        RAYOPDF_CHECK(Run(t, info, rows) == Jbig2::Result::Ok && info.damaged, 1133);
        std::string u;
        std::string sd;
        Put16(sd, 0);
        sd.append(8, '\0');
        Put32(sd, 1u << 21), Put32(sd, 1u << 21);
        Segment(u, 0, 0, {}, sd);
        RAYOPDF_CHECK(Run(u, info, rows) == Jbig2::Result::TooLarge, 1133);
        std::string v;
        Segment(v, 0, 48, {}, PageInfo(8, 8, 0));
        Segment(v, 1, 0, {}, sd, 0xFFFFFFFFu);
        RAYOPDF_CHECK(Run(v, info, rows) == Jbig2::Result::Ok && info.damaged, 1133);
        std::string w;
        Segment(w, 0, 48, {}, PageInfo(8, 8, 0));
        Segment(w, 1, 48, {}, PageInfo(100000, 100000, 0));
        RAYOPDF_CHECK(Run(w, info, rows) == Jbig2::Result::Ok && info.width == 8, 1133);
    }
    // Budgets: a text region of 2^32 - 1 instances with no data, a halftone grid of 2^31 cells,
    // a generic region of maxPixels pixels: each ends quickly with TooLarge.
    {
        const auto start = std::chrono::steady_clock::now();
        std::string s;
        std::vector<Context> gb(65536, 0);
        Segment(s, 0, 0, {}, SymbolDictionary({RandomImage(1, 1, 50)}, 0, gb));
        Segment(s, 1, 48, {}, PageInfo(64, 64, 0));
        std::string d = RegionInfo(64, 64, 0, 0, 0);
        Put16(d, 0);
        Put32(d, 0xFFFFFFFFu);
        Segment(s, 2, 6, {0}, d);
        const Jbig2::Result many = Run(s, info, rows, "", uint64_t(1) << 16);
        RAYOPDF_CHECK(many == Jbig2::Result::TooLarge || (many == Jbig2::Result::Ok && info.damaged), 1134);
        std::string t;
        Segment(t, 0, 48, {}, PageInfo(64, 64, 0));
        std::string pd;
        pd += static_cast<char>(0);
        pd += static_cast<char>(1);
        pd += static_cast<char>(1);
        Put32(pd, 1);
        Segment(t, 1, 16, {}, pd);
        std::string hd = RegionInfo(64, 64, 0, 0, 0);
        hd += '\0';
        Put32(hd, 1u << 16), Put32(hd, 1u << 15), Put32(hd, 0), Put32(hd, 0), Put16(hd, 256), Put16(hd, 0);
        Segment(t, 2, 22, {1}, hd);
        RAYOPDF_CHECK(Run(t, info, rows, "", uint64_t(1) << 20) == Jbig2::Result::TooLarge, 1134);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        RAYOPDF_CHECK(seconds < 20.0, 1134);
    }
    // Empty bitmaps that claim 2^32 - 1 rows (fuzz finding: a symbol 0 wide copied row by row) and
    // an empty halftone grid 2^32 - 1 cells high: nothing to do, done at once.
    {
        const auto start = std::chrono::steady_clock::now();
        std::string d;
        Put16(d, 0);
        d.append(8, '\0');
        for (int i = 0; i < 8; i++) d[size_t(2 + i)] = static_cast<char>(at[i]);
        Put32(d, 1), Put32(d, 1);
        MqEncoder e;
        IntContexts iadh, iadw, iaex;
        iadh.Reset(), iadw.Reset(), iaex.Reset();
        EncodeInt(e, iadh, 0xFFFFFFFFll);
        EncodeInt(e, iadw, 0);
        EncodeInt(e, iadw, 0, true);
        EncodeInt(e, iaex, 0);
        EncodeInt(e, iaex, 1);
        std::string s;
        Segment(s, 0, 0, {}, d + e.Finish());
        Segment(s, 1, 48, {}, PageInfo(16, 16, 0));
        std::string pd;
        pd += static_cast<char>(0);
        pd += static_cast<char>(2);
        pd += static_cast<char>(2);
        Put32(pd, 0);
        Segment(s, 2, 16, {}, pd);
        std::string hd = RegionInfo(16, 16, 0, 0, 0);
        hd += '\0';
        Put32(hd, 0), Put32(hd, 0xFFFFFFFFu), Put32(hd, 0), Put32(hd, 0), Put16(hd, 512), Put16(hd, 0);
        Segment(s, 3, 22, {2}, hd);
        Segment(s, 4, 49, {}, "");
        RAYOPDF_CHECK(Run(s, info, rows) == Jbig2::Result::Ok && !info.damaged && info.width == 16, 1135);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        RAYOPDF_CHECK(seconds < 5.0, 1135);
    }
}

} // namespace

int main() {
    TestArithmetic();
    TestHuffman();
    TestAnnexH();
    TestGeneric();
    TestRefinementAndComposition();
    TestText();
    TestHalftone();
    TestLimits();
    std::printf("image jbig2 tests passed\n");
    return 0;
}
