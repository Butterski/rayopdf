// SPDX-License-Identifier: Apache-2.0
// The JPEG 2000 codestream (T.800 annex A) and tier-2 (annex B) of the JPX decoder: the main and
// tile-part headers, tile, component, resolution, band, precinct and code-block geometry, the
// packet sequence of the five progression orders and of progression order changes, packet
// headers with their tag trees, then per tile the tier-1 decoding, dequantization (annex E),
// inverse wavelet transform, inverse component transform and DC level shift.
//
// Where T.800 leaves a choice or a damaged stream needs one, the decoder does what OpenJPEG 2.5
// does: a missing or short SOP marker is skipped over silently, a code-block whose data runs past
// the tile ends the tile's data (that code-block and the rest of the packet decode as empty), a
// tile-part longer than the data is cut at the data's end, a codestream without EOC ends where
// its data ends.
#include "image/jpx_internal.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace RayoPdf::Image::Jpx::Internal {
namespace {

// ------------------------------------------------------------------------------- utilities

inline uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

inline uint32_t CeilDiv(uint32_t a, uint32_t b) { return static_cast<uint32_t>((uint64_t(a) + b - 1) / b); }
inline uint32_t CeilDivPow2(uint32_t a, uint32_t n) { return static_cast<uint32_t>((uint64_t(a) + (uint64_t(1) << n) - 1) >> n); }
inline int32_t FloorLog2(uint32_t v) {
    int32_t l = 0;
    while (v > 1) {
        v >>= 1;
        l++;
    }
    return l;
}

constexpr uint32_t kMarkerSoc = 0xFF4F;
constexpr uint32_t kMarkerCap = 0xFF50;
constexpr uint32_t kMarkerSiz = 0xFF51;
constexpr uint32_t kMarkerCod = 0xFF52;
constexpr uint32_t kMarkerCoc = 0xFF53;
constexpr uint32_t kMarkerQcd = 0xFF5C;
constexpr uint32_t kMarkerQcc = 0xFF5D;
constexpr uint32_t kMarkerRgn = 0xFF5E;
constexpr uint32_t kMarkerPoc = 0xFF5F;
constexpr uint32_t kMarkerPpm = 0xFF60;
constexpr uint32_t kMarkerPpt = 0xFF61;
constexpr uint32_t kMarkerSot = 0xFF90;
constexpr uint32_t kMarkerSod = 0xFF93;
constexpr uint32_t kMarkerEoc = 0xFFD9;

// Part 2 and Part 15 marker segments: CPF (FF59), DCO (FF70), MCT (FF74), MCC (FF75), MCO (FF77),
// CBD (FF78), NLT (FF76), ATK (FF79), and CAP (FF50).
bool IsExtensionMarker(uint32_t marker) {
    return marker == kMarkerCap || marker == 0xFF59 || (marker >= 0xFF70 && marker <= 0xFF7F);
}

constexpr int kMaxSteps = 3 * kMaxLevels + 1;
constexpr int kMaxSegmentsPerBlock = 128;
constexpr uint64_t kMaxStructures = uint64_t(1) << 22;  // code-blocks, and precincts, per tile
constexpr int32_t kTagTreeInfinity = 999;  // OpenJPEG's "not yet known" tag tree value

// ------------------------------------------------------------------------------ parameters

struct CodingStyle {
    uint8_t levels = 0;
    uint8_t blockWidth = 6;   // exponents
    uint8_t blockHeight = 6;
    uint8_t style = 0;
    bool reversible = false;
    uint8_t precinctWidth[kMaxLevels + 1];
    uint8_t precinctHeight[kMaxLevels + 1];
};

struct Quantization {
    uint8_t style = 0;  // 0 none, 1 scalar derived, 2 scalar expounded
    uint8_t guard = 0;
    uint16_t exponent[kMaxSteps] = {};
    uint16_t mantissa[kMaxSteps] = {};
};

struct ProgressionChange {
    uint32_t resStart = 0;
    uint32_t resEnd = 0;
    uint32_t compStart = 0;
    uint32_t compEnd = 0;
    uint32_t layerEnd = 0;
    uint8_t order = 0;
};

// COD's code-stream-wide fields.
struct CodDefaults {
    uint8_t scod = 0;
    uint8_t order = 0;
    uint32_t layers = 1;
    uint8_t mct = 0;
};

struct SizComponent {
    uint32_t dx = 1;
    uint32_t dy = 1;
    int precision = 8;
    bool isSigned = false;
};

struct Header {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t tileWidth = 0, tileHeight = 0, tileX0 = 0, tileY0 = 0;
    uint32_t tilesX = 0, tilesY = 0;
    std::vector<SizComponent> components;
    bool hasCod = false;
    CodDefaults cod;
    CodingStyle codStyle;
    std::vector<CodingStyle> coc;
    std::vector<uint8_t> hasCoc;
    bool hasQcd = false;
    Quantization qcd;
    std::vector<Quantization> qcc;
    std::vector<uint8_t> hasQcc;
    std::vector<uint8_t> roiShift;
    std::vector<ProgressionChange> pocs;
    bool hasPpm = false;
    std::vector<uint8_t> ppm;
    size_t end = 0;  // offset of the first SOT
};

// A marker segment's body.
struct Segment {
    const uint8_t* p;
    size_t size;
};

Result ReadSiz(Segment s, Header& h) {
    if (s.size < 36) return Result::Corrupt;
    const uint32_t rsiz = Be16(s.p);
    if (rsiz & 0x4000) return Result::Unsupported;  // HTJ2K
    h.x1 = Be32(s.p + 2);
    h.y1 = Be32(s.p + 6);
    h.x0 = Be32(s.p + 10);
    h.y0 = Be32(s.p + 14);
    h.tileWidth = Be32(s.p + 18);
    h.tileHeight = Be32(s.p + 22);
    h.tileX0 = Be32(s.p + 26);
    h.tileY0 = Be32(s.p + 30);
    const uint32_t count = Be16(s.p + 34);
    if (count == 0 || s.size < 36 + size_t(count) * 3) return Result::Corrupt;
    if (count > kMaxComponents) return Result::Unsupported;
    if (h.x1 <= h.x0 || h.y1 <= h.y0 || h.tileWidth == 0 || h.tileHeight == 0) return Result::Corrupt;
    if (h.tileX0 > h.x0 || h.tileY0 > h.y0 || uint64_t(h.tileX0) + h.tileWidth <= h.x0 ||
        uint64_t(h.tileY0) + h.tileHeight <= h.y0)
        return Result::Corrupt;
    const uint64_t tilesX = (uint64_t(h.x1) - h.tileX0 + h.tileWidth - 1) / h.tileWidth;
    const uint64_t tilesY = (uint64_t(h.y1) - h.tileY0 + h.tileHeight - 1) / h.tileHeight;
    if (tilesX * tilesY > 65535) return Result::Corrupt;  // Isot is 16-bit
    h.tilesX = static_cast<uint32_t>(tilesX);
    h.tilesY = static_cast<uint32_t>(tilesY);
    h.components.resize(count);
    for (uint32_t c = 0; c < count; c++) {
        const uint8_t* q = s.p + 36 + 3 * c;
        SizComponent& comp = h.components[c];
        comp.precision = (q[0] & 0x7F) + 1;
        comp.isSigned = (q[0] & 0x80) != 0;
        comp.dx = q[1];
        comp.dy = q[2];
        if (comp.dx == 0 || comp.dy == 0) return Result::Corrupt;
        if (comp.precision > kMaxPrecision) return Result::Unsupported;
    }
    h.coc.resize(count);
    h.hasCoc.assign(count, 0);
    h.qcc.resize(count);
    h.hasQcc.assign(count, 0);
    h.roiShift.assign(count, 0);
    return Result::Ok;
}

// SPcod or SPcoc, with Scod/Scoc's precinct bit.
Result ReadCodingStyle(const uint8_t* p, size_t size, bool precincts, CodingStyle& cs) {
    if (size < 5) return Result::Corrupt;
    cs.levels = p[0];
    if (cs.levels > kMaxLevels) return Result::Corrupt;
    const uint32_t xcb = p[1] + 2u;
    const uint32_t ycb = p[2] + 2u;
    if (xcb > 10 || ycb > 10 || xcb + ycb > 12) return Result::Corrupt;
    cs.blockWidth = static_cast<uint8_t>(xcb);
    cs.blockHeight = static_cast<uint8_t>(ycb);
    cs.style = p[3];
    if (cs.style & (kStyleHt | 0x80)) return Result::Unsupported;
    if (p[4] > 1) return Result::Unsupported;  // Part 2 arbitrary wavelets
    cs.reversible = p[4] == 1;
    for (int r = 0; r <= cs.levels; r++) {
        if (!precincts) {
            cs.precinctWidth[r] = 15;
            cs.precinctHeight[r] = 15;
            continue;
        }
        if (size < 5 + size_t(r) + 1) return Result::Corrupt;
        const uint8_t v = p[5 + r];
        cs.precinctWidth[r] = v & 0xF;
        cs.precinctHeight[r] = v >> 4;
        if (r > 0 && (cs.precinctWidth[r] == 0 || cs.precinctHeight[r] == 0)) return Result::Corrupt;
    }
    return Result::Ok;
}

Result ReadCod(Segment s, CodDefaults& cod, CodingStyle& cs) {
    if (s.size < 5) return Result::Corrupt;
    cod.scod = s.p[0];
    if (cod.scod & 0xF8) return Result::Unsupported;  // Part 2 code-block anchors
    cod.order = s.p[1];
    if (cod.order > 4) return Result::Corrupt;
    cod.layers = Be16(s.p + 2);
    if (cod.layers == 0) return Result::Corrupt;
    cod.mct = s.p[4];
    if (cod.mct > 1) return cod.mct == 2 ? Result::Unsupported : Result::Corrupt;
    return ReadCodingStyle(s.p + 5, s.size - 5, (cod.scod & 1) != 0, cs);
}

// The component index of COC, QCC and RGN: one byte, two with more than 256 components.
bool ReadComponentIndex(Segment& s, size_t components, uint32_t& index) {
    const size_t width = components > 256 ? 2 : 1;
    if (s.size < width) return false;
    index = width == 2 ? Be16(s.p) : s.p[0];
    s.p += width;
    s.size -= width;
    return index < components;
}

Result ReadCoc(Segment s, size_t components, std::vector<CodingStyle>& coc, std::vector<uint8_t>& has) {
    uint32_t c = 0;
    if (!ReadComponentIndex(s, components, c) || s.size < 1) return Result::Corrupt;
    const bool precincts = (s.p[0] & 1) != 0;
    const Result r = ReadCodingStyle(s.p + 1, s.size - 1, precincts, coc[c]);
    if (r == Result::Ok) has[c] = 1;
    return r;
}

Result ReadQuantization(Segment s, Quantization& q) {
    if (s.size < 1) return Result::Corrupt;
    q.style = s.p[0] & 0x1F;
    q.guard = s.p[0] >> 5;
    const uint8_t* p = s.p + 1;
    const size_t size = s.size - 1;
    std::fill(q.exponent, q.exponent + kMaxSteps, 0);
    std::fill(q.mantissa, q.mantissa + kMaxSteps, 0);
    if (q.style == 0) {
        const size_t n = std::min<size_t>(size, kMaxSteps);
        for (size_t i = 0; i < n; i++) q.exponent[i] = p[i] >> 3;
    } else if (q.style == 1 || q.style == 2) {
        if (size < 2) return Result::Corrupt;
        const size_t n = q.style == 1 ? 1 : std::min<size_t>(size / 2, kMaxSteps);
        for (size_t i = 0; i < n; i++) {
            const uint32_t v = Be16(p + 2 * i);
            q.exponent[i] = static_cast<uint16_t>(v >> 11);
            q.mantissa[i] = static_cast<uint16_t>(v & 0x7FF);
        }
        if (q.style == 1) {
            for (int b = 1; b < kMaxSteps; b++) {
                const int e = int(q.exponent[0]) - (b - 1) / 3;
                q.exponent[b] = static_cast<uint16_t>(e > 0 ? e : 0);
                q.mantissa[b] = q.mantissa[0];
            }
        }
    } else {
        return Result::Corrupt;
    }
    return Result::Ok;
}

Result ReadQcc(Segment s, size_t components, std::vector<Quantization>& qcc, std::vector<uint8_t>& has) {
    uint32_t c = 0;
    if (!ReadComponentIndex(s, components, c)) return Result::Corrupt;
    const Result r = ReadQuantization(s, qcc[c]);
    if (r == Result::Ok) has[c] = 1;
    return r;
}

Result ReadRgn(Segment s, size_t components, std::vector<uint8_t>& shift) {
    uint32_t c = 0;
    if (!ReadComponentIndex(s, components, c) || s.size < 2) return Result::Corrupt;
    if (s.p[0] != 0) return Result::Unsupported;  // Part 2 ROI styles
    shift[c] = s.p[1];
    return Result::Ok;
}

Result ReadPoc(Segment s, size_t components, std::vector<ProgressionChange>& pocs) {
    const size_t width = components > 256 ? 2 : 1;
    const size_t entry = 5 + 2 * width;
    if (s.size < entry || s.size % entry != 0) return Result::Corrupt;
    for (size_t off = 0; off < s.size; off += entry) {
        const uint8_t* p = s.p + off;
        ProgressionChange poc;
        poc.resStart = p[0];
        poc.compStart = width == 2 ? Be16(p + 1) : p[1];
        poc.layerEnd = Be16(p + 1 + width);
        poc.resEnd = p[3 + width];
        poc.compEnd = width == 2 ? Be16(p + 4 + width) : p[4 + width];
        if (poc.compEnd == 0) poc.compEnd = width == 2 ? 16384 : 256;
        poc.order = p[4 + 2 * width];
        if (poc.order > 4) return Result::Corrupt;
        pocs.push_back(poc);
    }
    return Result::Ok;
}

// Reads the next marker segment at `pos`: its marker, body and the position after it.
bool NextSegment(const uint8_t* data, size_t size, size_t pos, uint32_t& marker, Segment& s, size_t& next) {
    if (pos + 4 > size) return false;
    marker = Be16(data + pos);
    if (marker < 0xFF00) return false;
    const uint32_t length = Be16(data + pos + 2);
    if (length < 2 || pos + 2 + length > size) return false;
    s.p = data + pos + 4;
    s.size = length - 2;
    next = pos + 2 + length;
    return true;
}

Result ParseMainHeader(const uint8_t* data, size_t size, Header& h) {
    if (size < 4 || Be16(data) != kMarkerSoc || Be16(data + 2) != kMarkerSiz) return Result::Corrupt;
    size_t pos = 2;
    bool first = true;
    for (int guard = 0; guard < 1 << 20; guard++) {
        if (pos + 2 <= size && Be16(data + pos) == kMarkerSot) {
            h.end = pos;
            break;
        }
        uint32_t marker = 0;
        Segment s{};
        size_t next = 0;
        if (!NextSegment(data, size, pos, marker, s, next)) return Result::Corrupt;
        Result r = Result::Ok;
        if (first) {
            r = ReadSiz(s, h);
            first = false;
        } else if (IsExtensionMarker(marker)) {
            r = Result::Unsupported;
        } else {
            switch (marker) {
            case kMarkerCod:
                r = ReadCod(s, h.cod, h.codStyle);
                h.hasCod = true;
                break;
            case kMarkerCoc: r = ReadCoc(s, h.components.size(), h.coc, h.hasCoc); break;
            case kMarkerQcd:
                r = ReadQuantization(s, h.qcd);
                h.hasQcd = true;
                break;
            case kMarkerQcc: r = ReadQcc(s, h.components.size(), h.qcc, h.hasQcc); break;
            case kMarkerRgn: r = ReadRgn(s, h.components.size(), h.roiShift); break;
            case kMarkerPoc: r = ReadPoc(s, h.components.size(), h.pocs); break;
            case kMarkerPpm:
                if (s.size < 1) return Result::Corrupt;
                h.hasPpm = true;
                h.ppm.insert(h.ppm.end(), s.p + 1, s.p + s.size);
                break;
            case kMarkerSiz: r = Result::Corrupt; break;
            default: break;  // TLM, PLM, CRG, COM and unknown segments
            }
        }
        if (r != Result::Ok) return r;
        pos = next;
    }
    if (h.end == 0) return Result::Corrupt;
    if (!h.hasCod || !h.hasQcd) return Result::Corrupt;
    // A COC must not have fewer levels than... nothing to check; the components' styles default
    // to COD's.
    for (size_t c = 0; c < h.components.size(); c++) {
        if (!h.hasCoc[c]) h.coc[c] = h.codStyle;
        if (!h.hasQcc[c]) h.qcc[c] = h.qcd;
    }
    return Result::Ok;
}

// ------------------------------------------------------------------------------ tile-parts

struct TilePart {
    uint32_t tile = 0;
    size_t headerStart = 0;  // the tile-part header's marker segments
    size_t headerEnd = 0;
    size_t dataStart = 0;
    size_t dataEnd = 0;
    size_t ppmStart = 0;  // this tile-part's packet headers within Header::ppm
    size_t ppmEnd = 0;
};

Result ScanTileParts(const uint8_t* data, size_t size, Header& h, std::vector<TilePart>& parts) {
    size_t pos = h.end;
    size_t ppmPos = 0;
    const uint32_t tiles = h.tilesX * h.tilesY;
    while (pos + 2 <= size) {
        const uint32_t marker = Be16(data + pos);
        if (marker == kMarkerEoc || marker != kMarkerSot) break;
        if (pos + 12 > size || Be16(data + pos + 2) != 10) break;
        TilePart part;
        part.tile = Be16(data + pos + 4);
        const uint32_t psot = Be32(data + pos + 6);
        if (part.tile >= tiles) return Result::Corrupt;
        size_t end = size;
        if (psot != 0 && psot >= 14 && pos + psot <= size) end = pos + psot;
        // The tile-part header's segments, up to SOD.
        size_t p = pos + 12;
        part.headerStart = p;
        bool sod = false;
        while (p + 2 <= end) {
            if (Be16(data + p) == kMarkerSod) {
                sod = true;
                break;
            }
            uint32_t m = 0;
            Segment s{};
            size_t next = 0;
            if (!NextSegment(data, end, p, m, s, next)) break;
            p = next;
        }
        if (!sod) break;
        part.headerEnd = p;
        part.dataStart = p + 2;
        part.dataEnd = end;
        if (psot == 0 && end == size && size >= part.dataStart + 2 && Be16(data + size - 2) == kMarkerEoc)
            part.dataEnd = size - 2;
        if (h.hasPpm) {
            // Nppm, then that many bytes of packet headers for this tile-part.
            if (ppmPos + 4 > h.ppm.size()) return Result::Corrupt;
            const size_t n = Be32(h.ppm.data() + ppmPos);
            ppmPos += 4;
            part.ppmStart = ppmPos;
            part.ppmEnd = std::min(h.ppm.size(), ppmPos + n);
            ppmPos = part.ppmEnd;
        }
        parts.push_back(part);
        if (end == size) break;
        pos = end;
    }
    return Result::Ok;
}

// ------------------------------------------------------------------------------ tile state

struct TagNode {
    int32_t value;
    int32_t low;
    int32_t parent;
};

struct BlockSegment {
    uint32_t length;
    uint32_t passes;
    uint32_t maxPasses;
    uint32_t newPasses;
    uint32_t newLength;
    int32_t next;
};

struct Chunk {
    size_t offset;
    uint32_t length;
    int32_t next;
};

struct CodeBlock {
    int32_t x0, y0, x1, y1;   // band coordinates
    int32_t bitplanes = 0;
    uint32_t lengthBits = 3;
    uint32_t newPasses = 0;
    uint32_t segmentCount = 0;  // committed segments
    uint32_t allocated = 0;     // segments in the chain
    int32_t segmentHead = -1;
    int32_t segmentTail = -1;   // the last committed segment
    int32_t chunkHead = -1;
    int32_t chunkTail = -1;
    uint32_t length = 0;
    bool corrupted = false;
};

struct PrecinctBand {
    uint32_t cw = 0, ch = 0;
    uint32_t firstBlock = 0;
    uint32_t inclusion = 0;  // tag tree node offsets
    uint32_t zeroPlanes = 0;
};

struct Band {
    int32_t x0, y0, x1, y1;
    int orientation;  // 0 LL, 1 HL, 2 LH, 3 HH
    int32_t magnitudeBits;  // Mb
    float step;
};

struct Resolution {
    uint32_t x0, y0, x1, y1;
    uint32_t pdx, pdy;
    uint32_t pw, ph;
    int bands;
    Band band[3];
    uint32_t firstPrecinct;      // its first precinct's index in nextLayer_
    size_t firstPrecinctBand;    // its first precinct's first band in precinctBands_ (bands fastest)
};

struct TileComp {
    uint32_t x0, y0, x1, y1;  // on the component's grid
    int resolutions;          // levels + 1
    int decoded;              // resolutions decoded (resolutions - reduce)
    CodingStyle style;
    Quantization quant;
    int roiShift;
    Resolution res[kMaxLevels + 1];
};

struct Source {
    const uint8_t* data;
    size_t pos;
    size_t end;
};

// The packet header bit reader (B.10.1): after a 0xFF byte, the next byte gives 7 bits.
struct Bio {
    const uint8_t* start;
    const uint8_t* bp;
    const uint8_t* end;
    uint32_t buf = 0;
    uint32_t ct = 0;

    void ByteIn() {
        buf = (buf << 8) & 0xFFFF;
        ct = buf == 0xFF00 ? 7 : 8;
        if (bp < end) buf |= *bp++;
    }
    uint32_t Read(uint32_t n) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (ct == 0) ByteIn();
            ct--;
            v = (v << 1) | ((buf >> ct) & 1u);
        }
        return v;
    }
    void InAlign() {
        if ((buf & 0xFF) == 0xFF) ByteIn();
        ct = 0;
    }
};

// A tile component's samples. Unlike std::vector's, growth leaves the samples unset: the
// decoder writes or clears every sample before any is read.
template <typename T>
class SampleBuffer {
public:
    T* Resize(size_t count) {
        if (count > capacity_) {
            data_.reset(new T[count]);
            capacity_ = count;
        }
        return data_.get();
    }
    T* Data() { return data_.get(); }

private:
    std::unique_ptr<T[]> data_;
    size_t capacity_ = 0;
};

class TileDecoder {
public:
    TileDecoder(const uint8_t* data, const Header& h, int reduce) : data_(data), h_(h), reduce_(reduce) {}

    Result Decode(uint32_t tile, const TilePart* parts, size_t partCount, TileSink& sink);

private:
    Result ReadTileHeaders(const TilePart* parts, size_t partCount);
    Result Build(uint32_t tile);
    Result Progress();
    Result Packet(uint32_t layer, uint32_t r, uint32_t c, uint32_t precinct);
    Result DecodeBlocks();
    void Transform(TileSink& sink);

    uint32_t NewTree(uint32_t w, uint32_t h);
    bool TagDecode(Bio& bio, uint32_t tree, uint32_t leaf, int32_t threshold);
    int32_t NextBlockSegment(CodeBlock& block, int32_t s);
    void InitSegment(int32_t s, bool first, uint32_t previousMax, uint8_t style);

    const uint8_t* data_;
    const Header& h_;
    int reduce_;

    // Per tile.
    uint32_t tx0_ = 0, ty0_ = 0, tx1_ = 0, ty1_ = 0;
    CodDefaults cod_;
    std::vector<ProgressionChange> pocs_;
    std::vector<TileComp> comps_;
    std::vector<uint8_t> body_;        // the tile-parts' data, concatenated
    std::vector<uint8_t> headers_;     // PPM or PPT packet headers, concatenated
    bool separateHeaders_ = false;
    Source bodySource_{};
    Source headerSource_{};
    std::vector<CodeBlock> blocks_;
    std::vector<PrecinctBand> precinctBands_;
    std::vector<uint32_t> nextLayer_;  // per precinct
    std::vector<TagNode> nodes_;
    std::vector<BlockSegment> segments_;
    std::vector<Chunk> chunks_;

    // Tier-1 and transform scratch.
    T1Decoder t1_;
    std::vector<int32_t> coefficients_;
    std::vector<uint8_t> blockData_;
    std::vector<uint32_t> segmentLengths_;
    std::vector<uint32_t> segmentPasses_;
    std::vector<SampleBuffer<int32_t>> ints_;
    std::vector<SampleBuffer<float>> floats_;
    std::vector<int32_t> intScratch_;
    std::vector<float> floatScratch_;
};

Result TileDecoder::ReadTileHeaders(const TilePart* parts, size_t partCount) {
    // COD, COC, QCD, QCC and RGN of the tile's first tile-part override the main header's; POC and
    // PPT may be in any tile-part.
    const size_t n = h_.components.size();
    std::vector<uint8_t> hasCoc(n, 0), hasQcc(n, 0);
    bool hasCod = false, hasQcd = false;
    CodingStyle codStyle;
    Quantization qcd;
    std::vector<CodingStyle> coc(n);
    std::vector<Quantization> qcc(n);
    std::vector<uint8_t> roi = h_.roiShift;
    cod_ = h_.cod;
    pocs_.clear();
    headers_.clear();
    separateHeaders_ = h_.hasPpm;
    bool hasPpt = false;
    for (size_t i = 0; i < partCount; i++) {
        size_t pos = parts[i].headerStart;
        while (pos < parts[i].headerEnd) {
            uint32_t marker = 0;
            Segment s{};
            size_t next = 0;
            if (!NextSegment(data_, parts[i].headerEnd, pos, marker, s, next)) break;
            Result r = Result::Ok;
            if (IsExtensionMarker(marker)) return Result::Unsupported;
            if (marker == kMarkerPoc) {
                r = ReadPoc(s, n, pocs_);
            } else if (marker == kMarkerPpt) {
                if (s.size < 1 || h_.hasPpm) return Result::Corrupt;
                hasPpt = true;
                headers_.insert(headers_.end(), s.p + 1, s.p + s.size);
            } else if (i == 0) {
                switch (marker) {
                case kMarkerCod:
                    r = ReadCod(s, cod_, codStyle);
                    hasCod = true;
                    break;
                case kMarkerCoc: r = ReadCoc(s, n, coc, hasCoc); break;
                case kMarkerQcd:
                    r = ReadQuantization(s, qcd);
                    hasQcd = true;
                    break;
                case kMarkerQcc: r = ReadQcc(s, n, qcc, hasQcc); break;
                case kMarkerRgn: r = ReadRgn(s, n, roi); break;
                default: break;
                }
            }
            if (r != Result::Ok) return r;
            pos = next;
        }
        if (h_.hasPpm) headers_.insert(headers_.end(), h_.ppm.begin() + parts[i].ppmStart, h_.ppm.begin() + parts[i].ppmEnd);
    }
    if (hasPpt) separateHeaders_ = true;
    if (pocs_.empty()) pocs_ = h_.pocs;
    comps_.resize(n);
    for (size_t c = 0; c < n; c++) {
        // Precedence (A.6.1): tile COC > tile COD > main COC > main COD, likewise QCC and QCD.
        comps_[c].style = hasCoc[c] ? coc[c] : (hasCod ? codStyle : h_.coc[c]);
        comps_[c].quant = hasQcc[c] ? qcc[c] : (hasQcd ? qcd : h_.qcc[c]);
        comps_[c].roiShift = roi[c];
    }
    return Result::Ok;
}

uint32_t TileDecoder::NewTree(uint32_t w, uint32_t h) {
    const uint32_t base = static_cast<uint32_t>(nodes_.size());
    if (w == 0 || h == 0) return base;
    uint32_t lw = w, lh = h;
    uint32_t levelStart = base;
    for (;;) {
        const uint32_t count = lw * lh;
        const uint32_t nw = (lw + 1) / 2, nh = (lh + 1) / 2;
        const uint32_t nextStart = levelStart + count;
        for (uint32_t j = 0; j < lh; j++) {
            for (uint32_t i = 0; i < lw; i++) {
                const int32_t parent = count > 1 ? static_cast<int32_t>(nextStart + (j / 2) * nw + i / 2) : -1;
                nodes_.push_back({kTagTreeInfinity, 0, parent});
            }
        }
        if (count <= 1) break;
        levelStart = nextStart;
        lw = nw;
        lh = nh;
    }
    return base;
}

// B.10.2: whether the leaf's value is below `threshold`, reading the bits needed to know.
bool TileDecoder::TagDecode(Bio& bio, uint32_t tree, uint32_t leaf, int32_t threshold) {
    int32_t path[40];
    int depth = 0;
    int32_t node = static_cast<int32_t>(tree + leaf);
    while (nodes_[node].parent >= 0 && depth < 40) {
        path[depth++] = node;
        node = nodes_[node].parent;
    }
    int32_t low = 0;
    for (;;) {
        TagNode& n = nodes_[node];
        if (low > n.low) n.low = low;
        else low = n.low;
        while (low < threshold && low < n.value) {
            if (bio.Read(1)) n.value = low;
            else low++;
        }
        n.low = low;
        if (depth == 0) break;
        node = path[--depth];
    }
    return nodes_[node].value < threshold;
}

Result TileDecoder::Build(uint32_t tile) {
    const uint32_t p = tile % h_.tilesX;
    const uint32_t q = tile / h_.tilesX;
    tx0_ = std::max<uint32_t>(static_cast<uint32_t>(std::min<uint64_t>(uint64_t(h_.tileX0) + uint64_t(p) * h_.tileWidth, 0xFFFFFFFFu)), h_.x0);
    ty0_ = std::max<uint32_t>(static_cast<uint32_t>(std::min<uint64_t>(uint64_t(h_.tileY0) + uint64_t(q) * h_.tileHeight, 0xFFFFFFFFu)), h_.y0);
    tx1_ = static_cast<uint32_t>(std::min<uint64_t>(uint64_t(h_.tileX0) + uint64_t(p + 1) * h_.tileWidth, h_.x1));
    ty1_ = static_cast<uint32_t>(std::min<uint64_t>(uint64_t(h_.tileY0) + uint64_t(q + 1) * h_.tileHeight, h_.y1));
    blocks_.clear();
    precinctBands_.clear();
    nextLayer_.clear();
    nodes_.clear();
    segments_.clear();
    chunks_.clear();
    // The tile's code-blocks and precincts are bounded by its samples at full resolution (every
    // code-block covers at least one sample, every precinct one of its resolution, and the
    // resolutions together have fewer than twice the samples) and absolutely, since a reduced
    // decode still parses every resolution.
    uint64_t samples = 0;
    for (size_t c = 0; c < comps_.size(); c++) {
        const SizComponent& sc = h_.components[c];
        samples += uint64_t(CeilDiv(tx1_, sc.dx) - CeilDiv(tx0_, sc.dx)) * (CeilDiv(ty1_, sc.dy) - CeilDiv(ty0_, sc.dy));
    }
    const uint64_t budget = std::min<uint64_t>(kMaxStructures, std::max<uint64_t>(4096, 2 * samples));
    for (size_t c = 0; c < comps_.size(); c++) {
        TileComp& tc = comps_[c];
        const SizComponent& sc = h_.components[c];
        tc.x0 = CeilDiv(tx0_, sc.dx);
        tc.y0 = CeilDiv(ty0_, sc.dy);
        tc.x1 = CeilDiv(tx1_, sc.dx);
        tc.y1 = CeilDiv(ty1_, sc.dy);
        tc.resolutions = tc.style.levels + 1;
        if (reduce_ > tc.style.levels) return Result::Unsupported;
        tc.decoded = tc.resolutions - reduce_;
        const int precision = sc.precision;
        for (int r = 0; r < tc.resolutions; r++) {
            Resolution& res = tc.res[r];
            const uint32_t level = static_cast<uint32_t>(tc.resolutions - 1 - r);
            res.x0 = CeilDivPow2(tc.x0, level);
            res.y0 = CeilDivPow2(tc.y0, level);
            res.x1 = CeilDivPow2(tc.x1, level);
            res.y1 = CeilDivPow2(tc.y1, level);
            res.pdx = tc.style.precinctWidth[r];
            res.pdy = tc.style.precinctHeight[r];
            const uint64_t px0 = (uint64_t(res.x0) >> res.pdx) << res.pdx;
            const uint64_t py0 = (uint64_t(res.y0) >> res.pdy) << res.pdy;
            const uint64_t px1 = ((uint64_t(res.x1) + (uint64_t(1) << res.pdx) - 1) >> res.pdx) << res.pdx;
            const uint64_t py1 = ((uint64_t(res.y1) + (uint64_t(1) << res.pdy) - 1) >> res.pdy) << res.pdy;
            res.pw = res.x0 == res.x1 ? 0 : static_cast<uint32_t>((px1 - px0) >> res.pdx);
            res.ph = res.y0 == res.y1 ? 0 : static_cast<uint32_t>((py1 - py0) >> res.pdy);
            res.bands = r == 0 ? 1 : 3;
            res.firstPrecinct = static_cast<uint32_t>(nextLayer_.size());
            const uint64_t precincts = uint64_t(res.pw) * res.ph;
            if (precincts + nextLayer_.size() > budget) return Result::TooLarge;
            nextLayer_.resize(nextLayer_.size() + precincts, 0);
            // The precinct partition of the bands (B.6): half size above resolution 0.
            const int64_t cbgX = r == 0 ? int64_t(px0) : int64_t((px0 + 1) >> 1);
            const int64_t cbgY = r == 0 ? int64_t(py0) : int64_t((py0 + 1) >> 1);
            const uint32_t cbgW = r == 0 ? res.pdx : res.pdx - 1;
            const uint32_t cbgH = r == 0 ? res.pdy : res.pdy - 1;
            const uint32_t blockW = std::min<uint32_t>(tc.style.blockWidth, cbgW);
            const uint32_t blockH = std::min<uint32_t>(tc.style.blockHeight, cbgH);
            const size_t precinctBase = precinctBands_.size();
            res.firstPrecinctBand = precinctBase;
            precinctBands_.resize(precinctBase + precincts * res.bands);
            for (int b = 0; b < res.bands; b++) {
                Band& band = res.band[b];
                band.orientation = r == 0 ? 0 : b + 1;
                if (r == 0) {
                    band.x0 = static_cast<int32_t>(res.x0);
                    band.y0 = static_cast<int32_t>(res.y0);
                    band.x1 = static_cast<int32_t>(res.x1);
                    band.y1 = static_cast<int32_t>(res.y1);
                } else {
                    // B-15: the subband of decomposition level nb = resolutions - r.
                    const uint32_t nb = level + 1;
                    const uint64_t xo = band.orientation & 1, yo = band.orientation >> 1;
                    const uint64_t ox = xo << (nb - 1), oy = yo << (nb - 1);
                    auto ceilShift = [nb](uint64_t v, uint64_t o) {
                        return static_cast<int32_t>(v >= o ? (v - o + (uint64_t(1) << nb) - 1) >> nb : 0);
                    };
                    band.x0 = ceilShift(tc.x0, ox);
                    band.y0 = ceilShift(tc.y0, oy);
                    band.x1 = ceilShift(tc.x1, ox);
                    band.y1 = ceilShift(tc.y1, oy);
                }
                // E.1: Mb and the step size (OpenJPEG takes the 9/7 gain into its synthesis).
                const int stepIndex = r == 0 ? 0 : 3 * (r - 1) + 1 + b;
                const int exponent = tc.quant.exponent[stepIndex];
                band.magnitudeBits = exponent + tc.quant.guard - 1;
                const int gain = tc.style.reversible ? (band.orientation == 0 ? 0 : (band.orientation == 3 ? 2 : 1)) : 0;
                double scale = 1.0;
                int e = precision + gain - exponent;
                for (; e > 0; e--) scale *= 2.0;
                for (; e < 0; e++) scale *= 0.5;
                band.step = static_cast<float>((1.0 + tc.quant.mantissa[stepIndex] / 2048.0) * scale);
                const bool empty = band.x0 >= band.x1 || band.y0 >= band.y1;
                for (uint64_t k = 0; k < precincts; k++) {
                    PrecinctBand& pb = precinctBands_[precinctBase + k * res.bands + b];
                    pb = PrecinctBand();
                    pb.firstBlock = static_cast<uint32_t>(blocks_.size());
                    pb.inclusion = pb.zeroPlanes = static_cast<uint32_t>(nodes_.size());
                    if (empty) continue;
                    const int64_t sx = cbgX + int64_t(k % res.pw) * (int64_t(1) << cbgW);
                    const int64_t sy = cbgY + int64_t(k / res.pw) * (int64_t(1) << cbgH);
                    const int64_t x0 = std::max<int64_t>(sx, band.x0);
                    const int64_t y0 = std::max<int64_t>(sy, band.y0);
                    const int64_t x1 = std::min<int64_t>(sx + (int64_t(1) << cbgW), band.x1);
                    const int64_t y1 = std::min<int64_t>(sy + (int64_t(1) << cbgH), band.y1);
                    if (x1 <= x0 || y1 <= y0) continue;
                    const int64_t bx0 = (x0 >> blockW) << blockW;
                    const int64_t by0 = (y0 >> blockH) << blockH;
                    const int64_t bx1 = ((x1 + (int64_t(1) << blockW) - 1) >> blockW) << blockW;
                    const int64_t by1 = ((y1 + (int64_t(1) << blockH) - 1) >> blockH) << blockH;
                    pb.cw = static_cast<uint32_t>((bx1 - bx0) >> blockW);
                    pb.ch = static_cast<uint32_t>((by1 - by0) >> blockH);
                    if (uint64_t(pb.cw) * pb.ch + blocks_.size() > budget) return Result::TooLarge;
                    for (uint32_t j = 0; j < pb.ch; j++) {
                        for (uint32_t i = 0; i < pb.cw; i++) {
                            CodeBlock block;
                            block.x0 = static_cast<int32_t>(std::max<int64_t>(bx0 + (int64_t(i) << blockW), x0));
                            block.y0 = static_cast<int32_t>(std::max<int64_t>(by0 + (int64_t(j) << blockH), y0));
                            block.x1 = static_cast<int32_t>(std::min<int64_t>(bx0 + (int64_t(i + 1) << blockW), x1));
                            block.y1 = static_cast<int32_t>(std::min<int64_t>(by0 + (int64_t(j + 1) << blockH), y1));
                            blocks_.push_back(block);
                        }
                    }
                    pb.inclusion = NewTree(pb.cw, pb.ch);
                    pb.zeroPlanes = NewTree(pb.cw, pb.ch);
                }
            }
        }
    }
    return Result::Ok;
}

int32_t TileDecoder::NextBlockSegment(CodeBlock& block, int32_t s) {
    if (segments_[s].next < 0) {
        if (block.allocated >= kMaxSegmentsPerBlock) return -1;
        segments_[s].next = static_cast<int32_t>(segments_.size());
        segments_.push_back(BlockSegment{0, 0, 0, 0, 0, -1});
        block.allocated++;
    }
    return segments_[s].next;
}

// OpenJPEG's opj_t2_init_seg: the passes a segment holds by the code-block style.
void TileDecoder::InitSegment(int32_t s, bool first, uint32_t previousMax, uint8_t style) {
    BlockSegment& seg = segments_[s];
    const int32_t next = seg.next;
    seg = BlockSegment{0, 0, 0, 0, 0, next};
    if (style & kStyleTermAll) seg.maxPasses = 1;
    else if (style & kStyleBypass) seg.maxPasses = first ? 10 : ((previousMax == 1 || previousMax == 10) ? 2 : 1);
    else seg.maxPasses = 109;
}

Result TileDecoder::Packet(uint32_t layer, uint32_t r, uint32_t c, uint32_t precinct) {
    TileComp& tc = comps_[c];
    Resolution& res = tc.res[r];
    uint32_t& next = nextLayer_[res.firstPrecinct + precinct];
    if (layer < next) return Result::Ok;  // already read (progression order changes)
    next = layer + 1;
    Source& body = bodySource_;
    Source& header = separateHeaders_ ? headerSource_ : bodySource_;
    if (cod_.scod & 2) {  // SOP marker segments may precede packets
        if (body.end - body.pos >= 6 && body.data[body.pos] == 0xFF && body.data[body.pos + 1] == 0x91) body.pos += 6;
    }
    Bio bio{header.data + header.pos, header.data + header.pos, header.data + header.end};
    const uint8_t style = tc.style.style;
    const size_t pbIndex = res.firstPrecinctBand + size_t(precinct) * res.bands;
    bool present = bio.Read(1) != 0;
    if (present) {
        for (int b = 0; b < res.bands; b++) {
            const Band& band = res.band[b];
            if (band.x0 >= band.x1 || band.y0 >= band.y1) continue;
            PrecinctBand& pb = precinctBands_[pbIndex + b];
            const uint32_t count = pb.cw * pb.ch;
            for (uint32_t k = 0; k < count; k++) {
                CodeBlock& block = blocks_[pb.firstBlock + k];
                bool included;
                if (block.segmentCount == 0) included = TagDecode(bio, pb.inclusion, k, static_cast<int32_t>(layer) + 1);
                else included = bio.Read(1) != 0;
                if (!included) {
                    block.newPasses = 0;
                    continue;
                }
                if (block.segmentCount == 0) {
                    int32_t i = 0;
                    while (!TagDecode(bio, pb.zeroPlanes, k, i)) i++;
                    block.bitplanes = band.magnitudeBits + 1 - i;
                    block.lengthBits = 3;
                }
                // B.10.6: the number of new coding passes.
                uint32_t passes = 1;
                if (bio.Read(1)) {
                    passes = 2;
                    if (bio.Read(1)) {
                        const uint32_t two = bio.Read(2);
                        if (two != 3) {
                            passes = 3 + two;
                        } else {
                            const uint32_t five = bio.Read(5);
                            passes = five != 31 ? 6 + five : 37 + bio.Read(7);
                        }
                    }
                }
                block.newPasses = passes;
                // B.10.7.1: Lblock increments.
                for (uint32_t guard = 0; guard < 64 && bio.Read(1); guard++) block.lengthBits++;
                int32_t s;
                if (block.segmentCount == 0) {
                    if (block.segmentHead < 0) {
                        block.segmentHead = static_cast<int32_t>(segments_.size());
                        segments_.push_back(BlockSegment{0, 0, 0, 0, 0, -1});
                        block.allocated = 1;
                    }
                    s = block.segmentHead;
                    InitSegment(s, true, 0, style);
                } else {
                    s = block.segmentTail;
                    if (segments_[s].passes == segments_[s].maxPasses) {
                        const uint32_t previousMax = segments_[s].maxPasses;
                        s = NextBlockSegment(block, s);
                        if (s < 0) return Result::Corrupt;
                        InitSegment(s, false, previousMax, style);
                    }
                }
                int32_t n = static_cast<int32_t>(passes);
                for (;;) {
                    BlockSegment& seg = segments_[s];
                    seg.newPasses = std::min<uint32_t>(seg.maxPasses - seg.passes, static_cast<uint32_t>(n));
                    const uint32_t bits = block.lengthBits + static_cast<uint32_t>(FloorLog2(seg.newPasses));
                    if (bits > 32) return Result::Corrupt;
                    seg.newLength = bio.Read(bits);
                    n -= static_cast<int32_t>(seg.newPasses);
                    if (n <= 0) break;
                    const uint32_t previousMax = seg.maxPasses;
                    s = NextBlockSegment(block, s);
                    if (s < 0) return Result::Corrupt;
                    InitSegment(s, false, previousMax, style);
                }
            }
        }
    }
    bio.InAlign();
    header.pos += static_cast<size_t>(bio.bp - bio.start);
    if (cod_.scod & 4) {  // EPH required
        if (header.end - header.pos < 2 || header.data[header.pos] != 0xFF || header.data[header.pos + 1] != 0x92)
            return Result::Corrupt;
        header.pos += 2;
    }
    if (!present) return Result::Ok;
    if (bio.bp == bio.start) return Result::Corrupt;

    // The packet body: each included code-block's new segments.
    bool partial = false;
    for (int b = 0; b < res.bands; b++) {
        const Band& band = res.band[b];
        if (band.x0 >= band.x1 || band.y0 >= band.y1) continue;
        PrecinctBand& pb = precinctBands_[pbIndex + b];
        const uint32_t count = pb.cw * pb.ch;
        for (uint32_t k = 0; k < count; k++) {
            CodeBlock& block = blocks_[pb.firstBlock + k];
            if (block.newPasses == 0) continue;
            if (partial || block.corrupted) {
                block.chunkHead = block.chunkTail = -1;
                block.length = 0;
                block.corrupted = true;
                continue;
            }
            int32_t s;
            if (block.segmentCount == 0) {
                s = block.segmentHead;
                block.segmentCount = 1;
            } else {
                s = block.segmentTail;
                if (segments_[s].passes == segments_[s].maxPasses) {
                    s = segments_[s].next;
                    block.segmentCount++;
                }
            }
            for (;;) {
                BlockSegment& seg = segments_[s];
                if (seg.newLength > body.end - body.pos) {
                    partial = true;
                    block.corrupted = true;
                    block.chunkHead = block.chunkTail = -1;
                    block.length = 0;
                    break;
                }
                if (seg.newLength > 0) {
                    const int32_t chunk = static_cast<int32_t>(chunks_.size());
                    chunks_.push_back(Chunk{body.pos, seg.newLength, -1});
                    if (block.chunkTail >= 0) chunks_[block.chunkTail].next = chunk;
                    else block.chunkHead = chunk;
                    block.chunkTail = chunk;
                    block.length += seg.newLength;
                }
                body.pos += seg.newLength;
                seg.length += seg.newLength;
                seg.passes += seg.newPasses;
                block.newPasses -= seg.newPasses;
                if (block.newPasses == 0) break;
                s = seg.next;
                block.segmentCount++;
            }
            block.segmentTail = s;
        }
    }
    if (partial) body.pos = body.end;
    return Result::Ok;
}

// The precinct of component c at resolution r that starts at reference grid position (x, y), as
// B.12 and OpenJPEG's packet iterator decide it.
bool PrecinctAt(const TileComp& tc, const SizComponent& sc, uint32_t r, uint64_t x, uint64_t y, uint64_t tx0,
                uint64_t ty0, uint32_t& precinct) {
    if (r >= static_cast<uint32_t>(tc.resolutions)) return false;
    const Resolution& res = tc.res[r];
    const uint32_t level = static_cast<uint32_t>(tc.resolutions) - 1 - r;
    const uint32_t rpx = res.pdx + level;
    const uint32_t rpy = res.pdy + level;
    const bool rowStart = y % (uint64_t(sc.dy) << rpy) == 0 ||
                          (y == ty0 && ((uint64_t(res.y0) << level) % (uint64_t(1) << rpy)) != 0);
    if (!rowStart) return false;
    const bool columnStart = x % (uint64_t(sc.dx) << rpx) == 0 ||
                             (x == tx0 && ((uint64_t(res.x0) << level) % (uint64_t(1) << rpx)) != 0);
    if (!columnStart) return false;
    if (res.pw == 0 || res.ph == 0 || res.x0 == res.x1 || res.y0 == res.y1) return false;
    const uint64_t cx = (x + (uint64_t(sc.dx) << level) - 1) / (uint64_t(sc.dx) << level);
    const uint64_t cy = (y + (uint64_t(sc.dy) << level) - 1) / (uint64_t(sc.dy) << level);
    const uint64_t i = (cx >> res.pdx) - (uint64_t(res.x0) >> res.pdx);
    const uint64_t j = (cy >> res.pdy) - (uint64_t(res.y0) >> res.pdy);
    if (i >= res.pw || j >= res.ph) return false;
    precinct = static_cast<uint32_t>(i + j * res.pw);
    return true;
}

// The smallest precinct step on the reference grid of the given components, as OpenJPEG takes
// it (steps that do not fit 32 bits left out). False when there is none.
bool PositionStep(const std::vector<TileComp>& comps, const std::vector<SizComponent>& sizes, size_t first,
                  size_t last, uint64_t& dx, uint64_t& dy) {
    dx = 0;
    dy = 0;
    for (size_t c = first; c < last; c++) {
        const TileComp& tc = comps[c];
        for (int r = 0; r < tc.resolutions; r++) {
            const uint32_t level = static_cast<uint32_t>(tc.resolutions - 1 - r);
            const uint32_t sx = tc.res[r].pdx + level;
            const uint32_t sy = tc.res[r].pdy + level;
            if (sx < 32 && sizes[c].dx <= (0xFFFFFFFFu >> sx)) {
                const uint64_t v = uint64_t(sizes[c].dx) << sx;
                dx = dx == 0 ? v : std::min(dx, v);
            }
            if (sy < 32 && sizes[c].dy <= (0xFFFFFFFFu >> sy)) {
                const uint64_t v = uint64_t(sizes[c].dy) << sy;
                dy = dy == 0 ? v : std::min(dy, v);
            }
        }
    }
    return dx != 0 && dy != 0;
}

Result TileDecoder::Progress() {
    const uint32_t comps = static_cast<uint32_t>(comps_.size());
    uint32_t maxRes = 0;
    for (const TileComp& tc : comps_) maxRes = std::max<uint32_t>(maxRes, static_cast<uint32_t>(tc.resolutions));
    std::vector<ProgressionChange> records;
    if (pocs_.empty()) {
        ProgressionChange all;
        all.resEnd = maxRes;
        all.compEnd = comps;
        all.layerEnd = cod_.layers;
        all.order = cod_.order;
        records.push_back(all);
    } else {
        for (ProgressionChange poc : pocs_) {
            poc.resEnd = std::min(poc.resEnd, maxRes);
            poc.compEnd = std::min(poc.compEnd, comps);
            poc.layerEnd = std::min(poc.layerEnd, cod_.layers);
            records.push_back(poc);
        }
    }
    const Source& header = separateHeaders_ ? headerSource_ : bodySource_;
    Result status = Result::Ok;
    // Reads one packet; false ends the tile (an error, or no packet data left).
    auto visit = [&](uint32_t l, uint32_t r, uint32_t c, uint32_t p) {
        status = Packet(l, r, c, p);
        return status == Result::Ok && header.pos < header.end;
    };
    for (const ProgressionChange& rec : records) {
        if (rec.compStart >= rec.compEnd || rec.resStart >= rec.resEnd) continue;
        const uint32_t c0 = rec.compStart, c1 = rec.compEnd;
        switch (rec.order) {
        case 0:  // layer, resolution, component, position
        case 1:  // resolution, layer, component, position
            for (uint32_t a = 0; a < (rec.order == 0 ? rec.layerEnd : rec.resEnd); a++) {
                for (uint32_t b = 0; b < (rec.order == 0 ? rec.resEnd : rec.layerEnd); b++) {
                    const uint32_t l = rec.order == 0 ? a : b;
                    const uint32_t r = rec.order == 0 ? b : a;
                    if (r < rec.resStart) continue;
                    for (uint32_t c = c0; c < c1; c++) {
                        if (r >= static_cast<uint32_t>(comps_[c].resolutions)) continue;
                        const Resolution& res = comps_[c].res[r];
                        const uint32_t n = res.pw * res.ph;
                        for (uint32_t p = 0; p < n; p++) {
                            if (!visit(l, r, c, p)) return status;
                        }
                    }
                }
            }
            break;
        case 2: {  // resolution, position, component, layer
            uint64_t dx = 0, dy = 0;
            if (!PositionStep(comps_, h_.components, 0, comps, dx, dy)) break;
            for (uint32_t r = rec.resStart; r < rec.resEnd; r++) {
                for (uint64_t y = ty0_; y < ty1_; y += dy - (y % dy)) {
                    for (uint64_t x = tx0_; x < tx1_; x += dx - (x % dx)) {
                        for (uint32_t c = c0; c < c1; c++) {
                            uint32_t p = 0;
                            if (!PrecinctAt(comps_[c], h_.components[c], r, x, y, tx0_, ty0_, p)) continue;
                            for (uint32_t l = 0; l < rec.layerEnd; l++) {
                                if (!visit(l, r, c, p)) return status;
                            }
                        }
                    }
                }
            }
            break;
        }
        case 3: {  // position, component, resolution, layer
            uint64_t dx = 0, dy = 0;
            if (!PositionStep(comps_, h_.components, 0, comps, dx, dy)) break;
            for (uint64_t y = ty0_; y < ty1_; y += dy - (y % dy)) {
                for (uint64_t x = tx0_; x < tx1_; x += dx - (x % dx)) {
                    for (uint32_t c = c0; c < c1; c++) {
                        for (uint32_t r = rec.resStart; r < rec.resEnd; r++) {
                            uint32_t p = 0;
                            if (!PrecinctAt(comps_[c], h_.components[c], r, x, y, tx0_, ty0_, p)) continue;
                            for (uint32_t l = 0; l < rec.layerEnd; l++) {
                                if (!visit(l, r, c, p)) return status;
                            }
                        }
                    }
                }
            }
            break;
        }
        default: {  // component, position, resolution, layer
            for (uint32_t c = c0; c < c1; c++) {
                uint64_t dx = 0, dy = 0;
                if (!PositionStep(comps_, h_.components, c, c + 1, dx, dy)) return status;
                for (uint64_t y = ty0_; y < ty1_; y += dy - (y % dy)) {
                    for (uint64_t x = tx0_; x < tx1_; x += dx - (x % dx)) {
                        for (uint32_t r = rec.resStart; r < rec.resEnd; r++) {
                            uint32_t p = 0;
                            if (!PrecinctAt(comps_[c], h_.components[c], r, x, y, tx0_, ty0_, p)) continue;
                            for (uint32_t l = 0; l < rec.layerEnd; l++) {
                                if (!visit(l, r, c, p)) return status;
                            }
                        }
                    }
                }
            }
            break;
        }
        }
    }
    return status;
}

Result TileDecoder::DecodeBlocks() {
    ints_.resize(comps_.size());
    floats_.resize(comps_.size());
    for (size_t c = 0; c < comps_.size(); c++) {
        TileComp& tc = comps_[c];
        const Resolution& top = tc.res[tc.decoded - 1];
        const size_t stride = top.x1 - top.x0;
        const size_t count = stride * (top.y1 - top.y0);
        // Code-blocks without data leave zeros. The code-blocks of a band never overlap, so when
        // their areas add up to the tile component's, they cover it and only those without data
        // need clearing; otherwise the whole buffer is cleared first.
        uint64_t covered = 0;
        for (int r = 0; r < tc.decoded; r++) {
            const Resolution& res = tc.res[r];
            for (uint64_t k = 0; k < uint64_t(res.pw) * res.ph * res.bands; k++) {
                const PrecinctBand& pb = precinctBands_[res.firstPrecinctBand + k];
                for (uint32_t i = 0; i < pb.cw * pb.ch; i++) {
                    const CodeBlock& block = blocks_[pb.firstBlock + i];
                    covered += uint64_t(block.x1 - block.x0) * uint64_t(block.y1 - block.y0);
                }
            }
        }
        const bool exact = covered == count;
        if (tc.style.reversible) {
            int32_t* samples = ints_[c].Resize(count);
            if (!exact) std::memset(samples, 0, count * sizeof(int32_t));
        } else {
            float* samples = floats_[c].Resize(count);
            if (!exact) std::memset(samples, 0, count * sizeof(float));
        }
        for (int r = 0; r < tc.decoded; r++) {
            const Resolution& res = tc.res[r];
            const uint32_t precincts = res.pw * res.ph;
            for (int b = 0; b < res.bands; b++) {
                const Band& band = res.band[b];
                if (band.x0 >= band.x1 || band.y0 >= band.y1) continue;
                size_t offsetX = 0, offsetY = 0;
                if (r > 0) {
                    if (band.orientation & 1) offsetX = tc.res[r - 1].x1 - tc.res[r - 1].x0;
                    if (band.orientation & 2) offsetY = tc.res[r - 1].y1 - tc.res[r - 1].y0;
                }
                const float step = 0.5f * band.step;
                for (uint32_t k = 0; k < precincts; k++) {
                    const PrecinctBand& pb = precinctBands_[res.firstPrecinctBand + size_t(k) * res.bands + b];
                    const uint32_t blocks = pb.cw * pb.ch;
                    for (uint32_t i = 0; i < blocks; i++) {
                        const CodeBlock& block = blocks_[pb.firstBlock + i];
                        const int w = block.x1 - block.x0;
                        const int hgt = block.y1 - block.y0;
                        const size_t x = size_t(block.x0 - band.x0) + offsetX;
                        const size_t y = size_t(block.y0 - band.y0) + offsetY;
                        if (block.segmentCount == 0 || block.corrupted) {
                            if (!exact) continue;
                            // Both zero kinds are all-zero bytes.
                            const size_t bytes = size_t(w) * (tc.style.reversible ? sizeof(int32_t) : sizeof(float));
                            for (int j = 0; j < hgt; j++) {
                                if (tc.style.reversible) std::memset(ints_[c].Data() + (y + j) * stride + x, 0, bytes);
                                else std::memset(floats_[c].Data() + (y + j) * stride + x, 0, bytes);
                            }
                            continue;
                        }
                        blockData_.resize(size_t(block.length) + 2);
                        size_t at = 0;
                        for (int32_t ch = block.chunkHead; ch >= 0; ch = chunks_[ch].next) {
                            std::memcpy(blockData_.data() + at, body_.data() + chunks_[ch].offset, chunks_[ch].length);
                            at += chunks_[ch].length;
                        }
                        segmentLengths_.clear();
                        segmentPasses_.clear();
                        int32_t s = block.segmentHead;
                        for (uint32_t n = 0; n < block.segmentCount && s >= 0; n++, s = segments_[s].next) {
                            segmentLengths_.push_back(segments_[s].length);
                            segmentPasses_.push_back(segments_[s].passes);
                        }
                        CodeBlockJob job;
                        job.data = blockData_.data();
                        job.segmentLengths = segmentLengths_.data();
                        job.segmentPasses = segmentPasses_.data();
                        job.segments = static_cast<int>(segmentLengths_.size());
                        job.width = w;
                        job.height = hgt;
                        job.orientation = band.orientation;
                        job.bitplanes = block.bitplanes;
                        job.roiShift = tc.roiShift;
                        job.style = tc.style.style;
                        coefficients_.resize(size_t(w) * hgt);
                        if (!t1_.Decode(job, coefficients_.data())) return Result::Corrupt;
                        const int32_t* in = coefficients_.data();
                        for (int j = 0; j < hgt; j++, in += w) {
                            if (tc.style.reversible) Dequantize53(in, ints_[c].Data() + (y + j) * stride + x, size_t(w));
                            else Dequantize97(in, floats_[c].Data() + (y + j) * stride + x, size_t(w), step);
                        }
                    }
                }
            }
        }
    }
    return Result::Ok;
}

void TileDecoder::Transform(TileSink& sink) {
    const size_t n = comps_.size();
    for (size_t c = 0; c < n; c++) {
        TileComp& tc = comps_[c];
        const Resolution& top = tc.res[tc.decoded - 1];
        const size_t stride = top.x1 - top.x0;
        for (int r = 1; r < tc.decoded; r++) {
            const Resolution& res = tc.res[r];
            const Resolution& low = tc.res[r - 1];
            if (tc.style.reversible) {
                Inverse53(ints_[c].Data(), stride, res.x1 - res.x0, res.y1 - res.y0, low.x1 - low.x0, low.y1 - low.y0,
                          (res.x0 & 1) != 0, (res.y0 & 1) != 0, intScratch_);
            } else {
                Inverse97(floats_[c].Data(), stride, res.x1 - res.x0, res.y1 - res.y0, low.x1 - low.x0, low.y1 - low.y0,
                          (res.x0 & 1) != 0, (res.y0 & 1) != 0, floatScratch_);
            }
        }
    }
    if (cod_.mct == 1 && n >= 3) {
        const Resolution& a = comps_[0].res[comps_[0].decoded - 1];
        bool same = true;
        for (size_t c = 1; c < 3; c++) {
            const Resolution& b = comps_[c].res[comps_[c].decoded - 1];
            same = same && b.x1 - b.x0 == a.x1 - a.x0 && b.y1 - b.y0 == a.y1 - a.y0 &&
                   comps_[c].style.reversible == comps_[0].style.reversible;
        }
        if (same) {
            const size_t count = size_t(a.x1 - a.x0) * (a.y1 - a.y0);
            if (comps_[0].style.reversible) InverseRct(ints_[0].Data(), ints_[1].Data(), ints_[2].Data(), count);
            else InverseIct(floats_[0].Data(), floats_[1].Data(), floats_[2].Data(), count);
        }
    }
    std::vector<TileComponent> out(n);
    for (size_t c = 0; c < n; c++) {
        const TileComp& tc = comps_[c];
        const SizComponent& sc = h_.components[c];
        const Resolution& top = tc.res[tc.decoded - 1];
        TileComponent& t = out[c];
        if (tc.style.reversible) t.ints = ints_[c].Data();
        else t.floats = floats_[c].Data();
        t.stride = top.x1 - top.x0;
        t.x0 = top.x0;
        t.y0 = top.y0;
        t.width = top.x1 - top.x0;
        t.height = top.y1 - top.y0;
        t.shift = sc.isSigned ? 0 : int32_t(1) << (sc.precision - 1);
        t.low = sc.isSigned ? -(int32_t(1) << (sc.precision - 1)) : 0;
        t.high = sc.isSigned ? (int32_t(1) << (sc.precision - 1)) - 1 : (int32_t(1) << sc.precision) - 1;
    }
    sink.Tile(out.data(), static_cast<int>(n));
}

Result TileDecoder::Decode(uint32_t tile, const TilePart* parts, size_t partCount, TileSink& sink) {
    Result r = ReadTileHeaders(parts, partCount);
    if (r != Result::Ok) return r;
    r = Build(tile);
    if (r != Result::Ok) return r;
    body_.clear();
    for (size_t i = 0; i < partCount; i++) body_.insert(body_.end(), data_ + parts[i].dataStart, data_ + parts[i].dataEnd);
    bodySource_ = Source{body_.data(), 0, body_.size()};
    headerSource_ = Source{headers_.data(), 0, headers_.size()};
    if ((separateHeaders_ ? headerSource_ : bodySource_).end > 0) {
        r = Progress();
        if (r != Result::Ok) return r;
    }
    r = DecodeBlocks();
    if (r != Result::Ok) return r;
    Transform(sink);
    return Result::Ok;
}

void FillInfo(const Header& h, int reduce, CodestreamInfo& info) {
    info.x0 = h.x0;
    info.y0 = h.y0;
    info.x1 = h.x1;
    info.y1 = h.y1;
    info.reversible = h.codStyle.reversible;
    int levels = kMaxLevels;
    for (const CodingStyle& cs : h.coc) levels = std::min<int>(levels, cs.levels);
    info.levels = levels;
    info.reduce = std::max(0, std::min(reduce, levels));
    info.components.resize(h.components.size());
    for (size_t c = 0; c < h.components.size(); c++) {
        const SizComponent& sc = h.components[c];
        ComponentInfo& ci = info.components[c];
        ci.dx = sc.dx;
        ci.dy = sc.dy;
        ci.precision = sc.precision;
        ci.isSigned = sc.isSigned;
        const uint32_t r = static_cast<uint32_t>(info.reduce);
        ci.x0 = CeilDivPow2(CeilDiv(h.x0, sc.dx), r);
        ci.y0 = CeilDivPow2(CeilDiv(h.y0, sc.dy), r);
        ci.width = CeilDivPow2(CeilDiv(h.x1, sc.dx), r) - ci.x0;
        ci.height = CeilDivPow2(CeilDiv(h.y1, sc.dy), r) - ci.y0;
    }
}

} // namespace

Result ReadMainHeader(const uint8_t* data, size_t size, CodestreamInfo& info) {
    Header h;
    const Result r = ParseMainHeader(data, size, h);
    if (r != Result::Ok) return r;
    FillInfo(h, 0, info);
    return Result::Ok;
}

Result DecodeCodestream(const uint8_t* data, size_t size, int reduce, uint64_t maxPixels, TileSink& sink) {
    Header h;
    Result r = ParseMainHeader(data, size, h);
    if (r != Result::Ok) return r;
    CodestreamInfo info;
    FillInfo(h, reduce, info);
    const uint32_t shift = static_cast<uint32_t>(info.reduce);
    const uint64_t width = CeilDivPow2(h.x1, shift) - CeilDivPow2(h.x0, shift);
    const uint64_t height = CeilDivPow2(h.y1, shift) - CeilDivPow2(h.y0, shift);
    if (width * height > maxPixels) return Result::TooLarge;
    uint64_t samples = 0;
    for (const ComponentInfo& ci : info.components) samples += uint64_t(ci.width) * ci.height;
    if (samples > maxPixels * 8) return Result::TooLarge;
    std::vector<TilePart> parts;
    r = ScanTileParts(data, size, h, parts);
    if (r != Result::Ok) return r;
    r = sink.Begin(info);
    if (r != Result::Ok) return r;
    // The tile-parts grouped by tile, in codestream order within a tile.
    const uint32_t tiles = h.tilesX * h.tilesY;
    std::vector<uint32_t> start(size_t(tiles) + 1, 0);
    for (const TilePart& part : parts) start[part.tile + 1]++;
    for (uint32_t t = 0; t < tiles; t++) start[t + 1] += start[t];
    std::vector<TilePart> grouped(parts.size());
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (const TilePart& part : parts) grouped[fill[part.tile]++] = part;
    TileDecoder decoder(data, h, info.reduce);
    for (uint32_t t = 0; t < tiles; t++) {
        if (start[t] == start[t + 1]) continue;  // a tile without data stays zero, as in OpenJPEG
        r = decoder.Decode(t, grouped.data() + start[t], start[t + 1] - start[t], sink);
        if (r != Result::Ok) return r;
    }
    return Result::Ok;
}

} // namespace RayoPdf::Image::Jpx::Internal
