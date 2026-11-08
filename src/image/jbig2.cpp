// SPDX-License-Identifier: Apache-2.0
// The JBIG2 decoder's control procedure, written from ITU-T T.88 section 7 (segment headers and
// the data of each segment type) and section 8.2 (page composition): the segments of
// /JBIG2Globals, then those of the stream, decoded in order into the first page's bitmap.
#include "image/jbig2.h"

#include "image/jbig2_internal.h"

#include <algorithm>

namespace RayoPdf::Image::Jbig2 {
namespace {

using namespace Internal;

// Work budget: kBaseWork + kWorkPerPixel * maxPixels units (a unit is about one pixel decoded or
// eight drawn); retained bitmaps: kBaseRetained + maxPixels / 2 bytes (four pages of maxPixels
// pixels). The default 2^28 pixels allow about 2^31 units.
constexpr uint64_t kBaseWork = uint64_t(1) << 22;
constexpr uint64_t kWorkPerPixel = 8;
constexpr uint64_t kBaseRetained = uint64_t(1) << 22;
// Units of work per symbol or pattern, besides its pixels.
constexpr uint64_t kSymbolWork = 32;
// Units of work per segment: about the statistics it may reset (kGenericContexts / 8).
constexpr uint64_t kSegmentWork = 8192;
// Contexts of the generic (16-bit templates) and refinement (13-bit) statistics.
constexpr size_t kGenericContexts = size_t(1) << 16;
constexpr size_t kRefinementContexts = size_t(1) << 13;

enum SegmentType : uint8_t {
    kSymbolDictionary = 0,
    kIntermediateText = 4,
    kImmediateText = 6,
    kImmediateLosslessText = 7,
    kPatternDictionary = 16,
    kIntermediateHalftone = 20,
    kImmediateHalftone = 22,
    kImmediateLosslessHalftone = 23,
    kIntermediateGeneric = 36,
    kImmediateGeneric = 38,
    kImmediateLosslessGeneric = 39,
    kIntermediateRefinement = 40,
    kImmediateRefinement = 42,
    kImmediateLosslessRefinement = 43,
    kPageInformation = 48,
    kEndOfPage = 49,
    kEndOfStripe = 50,
    kEndOfFile = 51,
    kTables = 53,
};

uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }

// ceil(log2(n)), 0 for n <= 1.
int CeilLog2(uint64_t n) {
    int bits = 0;
    while (bits < 63 && (uint64_t(1) << bits) < n) bits++;
    return bits;
}

// floor(v / 256) (the >> of T.88 6.6.5.1 on signed values).
int64_t FloorShift8(int64_t v) { return v >= 0 ? v >> 8 : -((-v + 255) >> 8); }

struct Header {
    uint32_t number = 0;
    uint8_t type = 0;
    uint32_t page = 0;
    uint32_t dataLength = 0;
    size_t firstReference = 0;  // into Decoder::references_
    size_t referenceCount = 0;
    uint32_t rowCount = 0xFFFFFFFFu;  // of a generic region of unknown data length
};

struct RegionInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t flags = 0;
};

bool ReadRegionInfo(const uint8_t* d, size_t n, RegionInfo& info) {
    if (n < 17) return false;
    info.width = Be32(d);
    info.height = Be32(d + 4);
    info.x = Be32(d + 8);
    info.y = Be32(d + 12);
    info.flags = d[16];
    return true;
}

// A decoded segment that later segments may refer to.
struct Stored {
    uint32_t number = 0;
    uint8_t type = 0;
    uint32_t first = 0;      // symbol or pattern dictionary: exports_[first, first + count)
    uint32_t count = 0;
    int32_t contexts = -1;   // symbol dictionary: retained statistics (index into retained_)
    const uint8_t* data = nullptr;  // tables
    size_t size = 0;
    int32_t region = -1;     // intermediate region: index into regions_
    RegionInfo info;
};

struct RetainedContexts {
    std::vector<Context> generic;
    std::vector<Context> refinement;
};

class Decoder {
public:
    explicit Decoder(uint64_t maxPixels) {
        budget_.maxPixels = maxPixels;
        budget_.work = kBaseWork + kWorkPerPixel * maxPixels;
        budget_.retained = kBaseRetained + maxPixels / 2;
    }

    Result Run(const uint8_t* data, size_t size, const uint8_t* globals, size_t globalsSize, Info& info,
               std::string& rows);

private:
    Status ReadStream(const uint8_t* data, size_t size);
    Status ReadHeader(const uint8_t* data, size_t size, size_t& pos, Header& h);
    Status DecodeSegment(const Header& h, const uint8_t* d, size_t n);
    Status SymbolDictionary(const Header& h, const uint8_t* d, size_t n);
    Status TextRegion(const Header& h, const uint8_t* d, size_t n);
    Status PatternDictionary(const Header& h, const uint8_t* d, size_t n);
    Status HalftoneRegion(const Header& h, const uint8_t* d, size_t n);
    Status GenericRegion(const Header& h, const uint8_t* d, size_t n);
    Status RefinementRegion(const Header& h, const uint8_t* d, size_t n);
    Status PageInformation(const uint8_t* d, size_t n);
    Status GrowPage(int64_t height);
    // An immediate region into the page, an intermediate one kept for refinement.
    Status Place(const Header& h, Bitmap& region, const RegionInfo& info, bool immediate);
    Status Store(const Stored& s);
    Stored* Find(uint32_t number);
    Status UserTable(const std::vector<const Stored*>& tables, size_t& next, HuffTable& out, const HuffTable*& table);

    Budget budget_;
    std::vector<uint32_t> references_;
    std::vector<Stored> stored_;  // sorted by segment number
    std::vector<uint32_t> exports_;
    std::vector<Bitmap> regions_;
    std::vector<RetainedContexts> retained_;
    SymbolPool pool_;
    MqDecoder mq_;
    IntDecoders ints_;
    std::vector<Context> generic_;
    std::vector<Context> refinement_;
    Scratch scratch_;
    Bitmap symbol_;
    Bitmap region_;
    Bitmap reference_;
    HuffTable userTables_[8];
    std::vector<uint32_t> symbols_;   // a segment's input symbols (SDINSYMS, SBSYMS)
    std::vector<uint32_t> newSymbols_;
    std::vector<uint32_t> widths_;
    std::vector<HuffLine> lines_;
    Bitmap page_;
    bool havePage_ = false;
    bool pageDone_ = false;
    bool unknownHeight_ = false;
    uint32_t pageDefault_ = 0;
    bool damaged_ = false;
    uint32_t segments_ = 0;
};

Stored* Decoder::Find(uint32_t number) {
    const auto it = std::lower_bound(stored_.begin(), stored_.end(), number,
                                     [](const Stored& s, uint32_t n) { return s.number < n; });
    return it != stored_.end() && it->number == number ? &*it : nullptr;
}

Status Decoder::Store(const Stored& s) {
    if (!budget_.Retain(sizeof(Stored))) return Status::TooLarge;
    const auto it = std::lower_bound(stored_.begin(), stored_.end(), s.number,
                                     [](const Stored& a, uint32_t n) { return a.number < n; });
    if (it != stored_.end() && it->number == s.number) *it = s;  // a repeated number replaces
    else stored_.insert(it, s);
    return Status::Ok;
}

Status Decoder::ReadHeader(const uint8_t* data, size_t size, size_t& pos, Header& h) {
    // 7.2: number (4), flags (1), referred-to segment count and retention flags (1 or 4 + ...),
    // referred-to segment numbers, page association (1 or 4), data length (4).
    const size_t left = size - pos;
    if (left < 6) return Status::Corrupt;
    const uint8_t* p = data + pos;
    h.number = Be32(p);
    const uint32_t flags = p[4];
    h.type = static_cast<uint8_t>(flags & 63);
    size_t at = 5;
    uint64_t count = p[5] >> 5;
    if (count <= 4) {
        at += 1;
    } else if (count == 7) {
        if (left < at + 4) return Status::Corrupt;
        count = Be32(p + at) & 0x1FFFFFFFu;
        at += 4 + (count + 8) / 8;
    } else {
        return Status::Corrupt;
    }
    if (references_.size() + count > kMaxReferences) return Status::TooLarge;
    const size_t referenceSize = h.number <= 256 ? 1 : (h.number <= 65536 ? 2 : 4);
    if (left < at || (left - at) / referenceSize < count) return Status::Corrupt;
    h.firstReference = references_.size();
    h.referenceCount = static_cast<size_t>(count);
    for (uint64_t i = 0; i < count; i++, at += referenceSize) {
        const uint8_t* r = p + at;
        references_.push_back(referenceSize == 1 ? r[0] : (referenceSize == 2 ? Be16(r) : Be32(r)));
    }
    const size_t pageSize = (flags & 64) ? 4 : 1;
    if (left < at + pageSize + 4) return Status::Corrupt;
    h.page = pageSize == 4 ? Be32(p + at) : p[at];
    at += pageSize;
    h.dataLength = Be32(p + at);
    at += 4;
    pos += at;
    h.rowCount = 0xFFFFFFFFu;
    if (h.dataLength == 0xFFFFFFFFu) {
        // 7.2.7: an immediate generic region of unknown length ends with 0xFF 0xAC (arithmetic) or
        // 0x00 0x00 (MMR) and a 4-byte row count.
        if (h.type != kImmediateGeneric && h.type != kImmediateLosslessGeneric) return Status::Corrupt;
        const uint8_t* d = data + pos;
        const size_t n = size - pos;
        if (n < 18) return Status::Corrupt;
        const bool mmr = d[17] & 1;
        const size_t start = mmr ? 18 : (((d[17] >> 1) & 3) == 0 ? 26 : 20);
        const uint8_t first = mmr ? 0x00 : 0xFF;
        const uint8_t second = mmr ? 0x00 : 0xAC;
        size_t i = start;
        for (; i + 6 <= n; i++)
            if (d[i] == first && d[i + 1] == second) break;
        if (i + 6 > n) return Status::Corrupt;
        h.rowCount = Be32(d + i + 2);
        h.dataLength = static_cast<uint32_t>(i + 6);
    }
    return Status::Ok;
}

Status Decoder::ReadStream(const uint8_t* data, size_t size) {
    size_t pos = 0;
    while (pos < size && !pageDone_) {
        if (++segments_ > kMaxSegments) return Status::TooLarge;
        if (!budget_.Spend(kSegmentWork)) return Status::TooLarge;
        // Trailing bytes too short for a segment header (a line end after the data) end the stream.
        if (size - pos < 11) break;
        Header h;
        Status status = ReadHeader(data, size, pos, h);
        if (status != Status::Ok) return status;
        size_t n = h.dataLength;
        if (n > size - pos) {
            // Truncated data: decoded as far as it goes (the arithmetic decoder reads 0xFF past it).
            n = size - pos;
            damaged_ = true;
        }
        status = DecodeSegment(h, data + pos, n);
        if (status != Status::Ok) return status;
        pos += n;
        if (h.type == kEndOfPage || h.type == kEndOfFile) break;
    }
    return Status::Ok;
}

Status Decoder::DecodeSegment(const Header& h, const uint8_t* d, size_t n) {
    switch (h.type) {
    case kSymbolDictionary: return SymbolDictionary(h, d, n);
    case kIntermediateText:
    case kImmediateText:
    case kImmediateLosslessText: return TextRegion(h, d, n);
    case kPatternDictionary: return PatternDictionary(h, d, n);
    case kIntermediateHalftone:
    case kImmediateHalftone:
    case kImmediateLosslessHalftone: return HalftoneRegion(h, d, n);
    case kIntermediateGeneric:
    case kImmediateGeneric:
    case kImmediateLosslessGeneric: return GenericRegion(h, d, n);
    case kIntermediateRefinement:
    case kImmediateRefinement:
    case kImmediateLosslessRefinement: return RefinementRegion(h, d, n);
    case kPageInformation:
        if (havePage_) {
            pageDone_ = true;  // the next page: this decoder keeps the first
            return Status::Ok;
        }
        return PageInformation(d, n);
    case kEndOfPage:
    case kEndOfFile: pageDone_ = havePage_; return Status::Ok;
    case kEndOfStripe:
        // 7.4.10: the end row; a page of unknown height reaches at least to it.
        if (n < 4) return Status::Corrupt;
        if (havePage_ && unknownHeight_) return GrowPage(int64_t(Be32(d)) + 1);
        return Status::Ok;
    case kTables: {
        Stored s;
        s.number = h.number;
        s.type = h.type;
        s.data = d;
        s.size = n;
        return Store(s);
    }
    default: return Status::Ok;  // profiles, extensions, reserved types
    }
}

Status Decoder::PageInformation(const uint8_t* d, size_t n) {
    // 7.4.8: width, height, X and Y resolution (4 each), flags (1), striping (2).
    if (n < 19) return Status::Corrupt;
    const uint32_t width = Be32(d);
    const uint32_t height = Be32(d + 4);
    pageDefault_ = (d[16] >> 2) & 1u;
    unknownHeight_ = height == 0xFFFFFFFFu;
    const int64_t initial = unknownHeight_ ? 0 : height;
    const Status status = Allocate(page_, width, initial, pageDefault_, budget_);
    if (status != Status::Ok) return status;
    if (!budget_.Retain(uint64_t(page_.stride) * page_.height)) return Status::TooLarge;
    havePage_ = true;
    return Status::Ok;
}

Status Decoder::GrowPage(int64_t height) {
    if (height <= int64_t(page_.height)) return Status::Ok;
    if (height > 0xFFFFFFFFll || uint64_t(page_.width) * uint64_t(height) > budget_.maxPixels) return Status::TooLarge;
    const uint64_t added = uint64_t(page_.stride) * uint64_t(height - page_.height);
    if (!budget_.Retain(added) || !budget_.Spend(added)) return Status::TooLarge;
    Bitmap row;
    Status status = Allocate(row, page_.width, 1, pageDefault_, budget_);
    if (status != Status::Ok) return status;
    const size_t oldBytes = page_.data.size();
    page_.data.resize(page_.stride * size_t(height));
    for (size_t at = oldBytes; at < page_.data.size(); at += page_.stride)
        std::memcpy(page_.data.data() + at, row.data.data(), page_.stride);
    page_.height = static_cast<uint32_t>(height);
    return Status::Ok;
}

Status Decoder::Place(const Header& h, Bitmap& region, const RegionInfo& info, bool immediate) {
    const uint32_t op = info.flags & 7;
    if (op > 4) return Status::Corrupt;
    if (!immediate) {
        // An intermediate region: an auxiliary buffer for a refinement to come.
        if (!budget_.Retain(region.data.size() + sizeof(Bitmap))) return Status::TooLarge;
        Stored s;
        s.number = h.number;
        s.type = h.type;
        s.region = static_cast<int32_t>(regions_.size());
        s.info = info;
        regions_.push_back(Bitmap());
        regions_.back().width = region.width;
        regions_.back().height = region.height;
        regions_.back().stride = region.stride;
        regions_.back().data.swap(region.data);
        return Store(s);
    }
    if (!havePage_) return Status::Corrupt;
    if (unknownHeight_) {
        const Status status = GrowPage(int64_t(info.y) + region.height);
        if (status != Status::Ok) return status;
    }
    if (!budget_.Spend(region.data.size())) return Status::TooLarge;
    Combine(page_, ViewOf(region), info.x, info.y, static_cast<CombineOp>(op));
    return Status::Ok;
}

Status Decoder::UserTable(const std::vector<const Stored*>& tables, size_t& next, HuffTable& out,
                          const HuffTable*& table) {
    if (next >= tables.size()) return Status::Corrupt;
    const Stored* s = tables[next++];
    // A table segment may be referred to again and again: each parse costs its size.
    if (!budget_.Spend(s->size)) return Status::TooLarge;
    const Status status = ParseTableSegment(s->data, s->size, out);
    if (status != Status::Ok) return status;
    table = &out;
    return Status::Ok;
}

// ---------------------------------------------------------------- symbol dictionaries (7.4.2, 6.5)

Status Decoder::SymbolDictionary(const Header& h, const uint8_t* d, size_t n) {
    if (n < 2) return Status::Corrupt;
    const uint32_t flags = Be16(d);
    const bool huffman = flags & 1;
    const bool refagg = (flags >> 1) & 1;
    const uint32_t dhSelect = (flags >> 2) & 3;
    const uint32_t dwSelect = (flags >> 4) & 3;
    const uint32_t bmSizeSelect = (flags >> 6) & 1;
    const uint32_t aggSelect = (flags >> 7) & 1;
    const bool contextUsed = (flags >> 8) & 1;
    const bool contextRetained = (flags >> 9) & 1;
    const int templateId = static_cast<int>((flags >> 10) & 3);
    const int refinementTemplate = static_cast<int>((flags >> 12) & 1);
    size_t pos = 2;
    GenericParams generic;
    generic.templateId = templateId;
    if (!huffman) {
        const size_t atBytes = templateId == 0 ? 8 : 2;
        if (n < pos + atBytes) return Status::Corrupt;
        for (size_t i = 0; i < atBytes; i++) generic.at[i] = static_cast<int8_t>(d[pos + i]);
        pos += atBytes;
    }
    int8_t refinementAt[4] = {};
    if (refagg && refinementTemplate == 0) {
        if (n < pos + 4) return Status::Corrupt;
        for (size_t i = 0; i < 4; i++) refinementAt[i] = static_cast<int8_t>(d[pos + i]);
        pos += 4;
    }
    if (n < pos + 8) return Status::Corrupt;
    const uint32_t numExported = Be32(d + pos);
    const uint32_t numNew = Be32(d + pos + 4);
    pos += 8;

    // Input symbols and tables from the referred-to segments, in order.
    symbols_.clear();
    std::vector<const Stored*> tables;
    const Stored* lastDictionary = nullptr;
    for (size_t i = 0; i < h.referenceCount; i++) {
        const Stored* s = Find(references_[h.firstReference + i]);
        if (s == nullptr) return Status::Corrupt;
        if (s->type == kSymbolDictionary) {
            if (uint64_t(symbols_.size()) + s->count > kMaxSymbols) return Status::TooLarge;
            symbols_.insert(symbols_.end(), exports_.begin() + s->first, exports_.begin() + s->first + s->count);
            lastDictionary = s;
        } else if (s->type == kTables) {
            tables.push_back(s);
        }
    }
    const uint32_t numInput = static_cast<uint32_t>(symbols_.size());
    // Referring to one dictionary many times makes long input lists cheaply: they cost their size.
    if (!budget_.Spend(numInput)) return Status::TooLarge;
    if (uint64_t(numInput) + numNew > kMaxSymbols) return Status::TooLarge;
    const uint32_t total = numInput + numNew;

    const HuffTable* dh = nullptr;
    const HuffTable* dw = nullptr;
    const HuffTable* bmSize = nullptr;
    const HuffTable* aggInst = nullptr;
    if (huffman) {
        size_t next = 0;
        Status status = Status::Ok;
        if (dhSelect == 0) dh = &StandardTable(4);
        else if (dhSelect == 1) dh = &StandardTable(5);
        else if (dhSelect == 3) status = UserTable(tables, next, userTables_[0], dh);
        else return Status::Corrupt;
        if (status != Status::Ok) return status;
        if (dwSelect == 0) dw = &StandardTable(2);
        else if (dwSelect == 1) dw = &StandardTable(3);
        else if (dwSelect == 3) status = UserTable(tables, next, userTables_[1], dw);
        else return Status::Corrupt;
        if (status != Status::Ok) return status;
        if (bmSizeSelect == 0) bmSize = &StandardTable(1);
        else status = UserTable(tables, next, userTables_[2], bmSize);
        if (status != Status::Ok) return status;
        if (aggSelect == 0) aggInst = &StandardTable(1);
        else status = UserTable(tables, next, userTables_[3], aggInst);
        if (status != Status::Ok) return status;
    }

    // 7.4.2.2 steps 3-5: statistics.
    const int codeLength = CeilLog2(total);
    // The IAID contexts (2^codeLength) of a hostile header cost their size.
    if (!budget_.Spend(uint64_t(1) << codeLength)) return Status::TooLarge;
    ints_.Reset(codeLength);
    if (contextUsed && lastDictionary != nullptr && lastDictionary->contexts >= 0) {
        generic_ = retained_[size_t(lastDictionary->contexts)].generic;
        refinement_ = retained_[size_t(lastDictionary->contexts)].refinement;
    } else {
        generic_.assign(kGenericContexts, 0);
        refinement_.assign(kRefinementContexts, 0);
    }
    if (!huffman) mq_.Start(d + pos, n - pos);
    BitReader reader(d + pos, n - pos);

    // 6.5.5
    newSymbols_.clear();
    widths_.clear();
    TextParams aggregate;
    aggregate.huffman = huffman;
    aggregate.refine = true;
    aggregate.logStrips = 0;
    aggregate.symbolCodeLength = huffman ? std::max(codeLength, 1) : codeLength;
    aggregate.refCorner = 1;  // TOPLEFT
    aggregate.fs = &StandardTable(6);
    aggregate.ds = &StandardTable(8);
    aggregate.dt = &StandardTable(11);
    aggregate.rdw = aggregate.rdh = aggregate.rdx = aggregate.rdy = &StandardTable(15);
    aggregate.rsize = &StandardTable(1);
    aggregate.refinementTemplate = refinementTemplate;
    std::memcpy(aggregate.refinementAt, refinementAt, sizeof refinementAt);

    const auto value = [&](const HuffTable* table, IntContexts& contexts, int64_t& v) -> Code {
        if (huffman) return DecodeHuffman(reader, *table, v);
        return DecodeInt(mq_, contexts, v) ? Code::Value : Code::Oob;
    };
    int64_t height = 0;
    uint32_t decoded = 0;
    while (decoded < numNew) {
        int64_t v = 0;
        if (value(dh, ints_.iadh, v) != Code::Value) return Status::Corrupt;
        height += v;
        if (!budget_.Spend(kSymbolWork)) return Status::TooLarge;  // a class may hold no symbol
        if (height < 0 || height > 0xFFFFFFFFll) return Status::Corrupt;
        int64_t width = 0;
        int64_t totalWidth = 0;
        const uint32_t firstInClass = decoded;
        for (;;) {
            const Code c = value(dw, ints_.iadw, v);
            if (c == Code::Error) return Status::Corrupt;
            if (c == Code::Oob) break;
            if (decoded >= numNew) return Status::Corrupt;
            width += v;
            totalWidth += width;
            if (width < 0 || width > 0xFFFFFFFFll || totalWidth > 0xFFFFFFFFll) return Status::Corrupt;
            if (!budget_.Spend(kSymbolWork)) return Status::TooLarge;
            if (huffman && !refagg) {
                widths_.push_back(static_cast<uint32_t>(width));
                decoded++;
                continue;
            }
            Status status;
            if (!refagg) {
                // 6.5.8.1: a generic region of the symbol's size.
                status = Allocate(symbol_, width, height, 0, budget_);
                if (status == Status::Ok) status = DecodeGeneric(mq_, generic_.data(), generic, symbol_, budget_);
            } else {
                // 6.5.8.2: an aggregate of REFAGGNINST instances, or one refined symbol.
                int64_t instances = 0;
                if (value(aggInst, ints_.iaai, instances) != Code::Value || instances < 1 || instances > 0xFFFFFFFFll)
                    return Status::Corrupt;
                if (instances > 1) {
                    aggregate.width = static_cast<uint32_t>(width);
                    aggregate.height = static_cast<uint32_t>(height);
                    aggregate.numInstances = static_cast<uint32_t>(instances);
                    aggregate.symbols = symbols_.data();
                    aggregate.numSymbols = static_cast<uint32_t>(symbols_.size());
                    status = DecodeText(aggregate, mq_, ints_, refinement_.data(), &reader, pool_, scratch_, symbol_,
                                        budget_);
                } else {
                    uint32_t id;
                    int64_t rdx = 0;
                    int64_t rdy = 0;
                    int64_t size = 0;
                    const HuffTable* b15 = &StandardTable(15);
                    if (huffman) {
                        id = reader.ReadBits(std::max(codeLength, 1));
                        if (DecodeHuffman(reader, *b15, rdx) != Code::Value ||
                            DecodeHuffman(reader, *b15, rdy) != Code::Value ||
                            DecodeHuffman(reader, StandardTable(1), size) != Code::Value)
                            return Status::Corrupt;
                        reader.Align();
                    } else {
                        id = DecodeIaid(mq_, ints_.iaid.data(), codeLength);
                        if (!DecodeInt(mq_, ints_.iardx, rdx) || !DecodeInt(mq_, ints_.iardy, rdy)) return Status::Corrupt;
                    }
                    if (id >= symbols_.size()) return Status::Corrupt;
                    RefinementParams rp;
                    rp.templateId = refinementTemplate;
                    std::memcpy(rp.at, refinementAt, sizeof rp.at);
                    rp.reference = pool_.Get(symbols_[id]);
                    rp.dx = rdx;
                    rp.dy = rdy;
                    status = Allocate(symbol_, width, height, 0, budget_);
                    if (status == Status::Ok) {
                        if (huffman) {
                            const size_t start = reader.Position();
                            const size_t left = reader.Size() - start;
                            if (size < 0 || uint64_t(size) > left) return Status::Corrupt;
                            MqDecoder local;
                            local.Start(reader.Data() + start, size_t(size));
                            status = DecodeRefinement(local, refinement_.data(), rp, symbol_, budget_);
                            reader.Seek(start + size_t(size));
                        } else {
                            status = DecodeRefinement(mq_, refinement_.data(), rp, symbol_, budget_);
                        }
                    }
                }
            }
            if (status != Status::Ok) return status;
            uint32_t id;
            status = pool_.Append(ViewOf(symbol_), 0, symbol_.width, budget_, id);
            if (status != Status::Ok) return status;
            newSymbols_.push_back(id);
            symbols_.push_back(id);  // SBSYMS of later aggregates (6.5.8.2.4)
            decoded++;
        }
        if (huffman && !refagg) {
            // 6.5.9: the height class collective bitmap, uncompressed or MMR.
            int64_t size = 0;
            if (DecodeHuffman(reader, *bmSize, size) != Code::Value || size < 0) return Status::Corrupt;
            reader.Align();
            Status status = Allocate(symbol_, totalWidth, height, 0, budget_);
            if (status != Status::Ok) return status;
            const size_t start = reader.Position();
            const size_t left = reader.Size() - start;
            if (size == 0) {
                const uint64_t bytes = uint64_t(symbol_.stride) * symbol_.height;
                if (bytes > left) return Status::Corrupt;
                if (!budget_.Spend(bytes)) return Status::TooLarge;
                if (bytes != 0) std::memcpy(symbol_.data.data(), reader.Data() + start, size_t(bytes));
                if ((symbol_.width & 7) != 0) {
                    const uint8_t mask = static_cast<uint8_t>(0xFFu << (8 - (symbol_.width & 7)));
                    for (size_t i = symbol_.stride - 1; i < symbol_.data.size(); i += symbol_.stride)
                        symbol_.data[i] &= mask;
                }
                reader.Seek(start + size_t(bytes));
            } else {
                if (uint64_t(size) > left) return Status::Corrupt;
                size_t used = 0;
                status = DecodeMmr(reader.Data() + start, size_t(size), symbol_, used, budget_);
                if (status != Status::Ok) return status;
                reader.Seek(start + size_t(size));
            }
            uint32_t x = 0;
            for (uint32_t i = firstInClass; i < decoded; i++) {
                const uint32_t w = widths_[i - firstInClass];
                uint32_t id;
                status = pool_.Append(ViewOf(symbol_), x, w, budget_, id);
                if (status != Status::Ok) return status;
                newSymbols_.push_back(id);
                x += w;
            }
            widths_.clear();
        }
    }

    // 6.5.10: the export flags, as runs.
    Stored s;
    s.number = h.number;
    s.type = h.type;
    s.first = static_cast<uint32_t>(exports_.size());
    uint32_t index = 0;
    bool exported = false;
    while (index < total) {
        int64_t run = 0;
        const Code c = huffman ? DecodeHuffman(reader, StandardTable(1), run)
                               : (DecodeInt(mq_, ints_.iaex, run) ? Code::Value : Code::Oob);
        if (c != Code::Value || run < 0 || run > int64_t(total - index)) return Status::Corrupt;
        if (!budget_.Spend(1 + uint64_t(run))) return Status::TooLarge;
        if (exported) {
            if (s.count + uint64_t(run) > numExported) return Status::Corrupt;
            if (!budget_.Retain(uint64_t(run) * sizeof(uint32_t))) return Status::TooLarge;
            for (uint32_t i = index; i < index + uint32_t(run); i++)
                exports_.push_back(i < numInput ? symbols_[i] : newSymbols_[i - numInput]);
            s.count += static_cast<uint32_t>(run);
        }
        index += static_cast<uint32_t>(run);
        exported = !exported;
    }
    if (contextRetained) {
        if (!budget_.Retain(kGenericContexts + kRefinementContexts)) return Status::TooLarge;
        s.contexts = static_cast<int32_t>(retained_.size());
        retained_.push_back(RetainedContexts{generic_, refinement_});
    }
    return Store(s);
}

// ---------------------------------------------------------------- text regions (7.4.3)

Status Decoder::TextRegion(const Header& h, const uint8_t* d, size_t n) {
    RegionInfo info;
    if (!ReadRegionInfo(d, n, info) || n < 19) return Status::Corrupt;
    const uint32_t flags = Be16(d + 17);
    size_t pos = 19;
    TextParams p;
    p.huffman = flags & 1;
    p.refine = (flags >> 1) & 1;
    p.logStrips = static_cast<int>((flags >> 2) & 3);
    p.refCorner = static_cast<int>((flags >> 4) & 3);
    p.transposed = (flags >> 6) & 1;
    p.combineOp = static_cast<CombineOp>((flags >> 7) & 3);
    p.defaultPixel = (flags >> 9) & 1;
    p.dsOffset = static_cast<int>((flags >> 10) & 31);
    if (p.dsOffset >= 16) p.dsOffset -= 32;  // signed 5-bit
    p.refinementTemplate = static_cast<int>((flags >> 15) & 1);
    uint32_t huffmanFlags = 0;
    if (p.huffman) {
        if (n < pos + 2) return Status::Corrupt;
        huffmanFlags = Be16(d + pos);
        pos += 2;
    }
    if (p.refine && p.refinementTemplate == 0) {
        if (n < pos + 4) return Status::Corrupt;
        for (size_t i = 0; i < 4; i++) p.refinementAt[i] = static_cast<int8_t>(d[pos + i]);
        pos += 4;
    }
    if (n < pos + 4) return Status::Corrupt;
    p.numInstances = Be32(d + pos);
    pos += 4;

    symbols_.clear();
    std::vector<const Stored*> tables;
    for (size_t i = 0; i < h.referenceCount; i++) {
        const Stored* s = Find(references_[h.firstReference + i]);
        if (s == nullptr) return Status::Corrupt;
        if (s->type == kSymbolDictionary) {
            if (uint64_t(symbols_.size()) + s->count > kMaxSymbols) return Status::TooLarge;
            symbols_.insert(symbols_.end(), exports_.begin() + s->first, exports_.begin() + s->first + s->count);
        } else if (s->type == kTables) {
            tables.push_back(s);
        }
    }
    p.symbols = symbols_.data();
    p.numSymbols = static_cast<uint32_t>(symbols_.size());
    if (!budget_.Spend(p.numSymbols)) return Status::TooLarge;
    p.width = info.width;
    p.height = info.height;

    HuffTable symbolCodes;
    BitReader reader(d + pos, n - pos);
    if (p.huffman) {
        // 7.4.3.1.6: the tables, standard or user-supplied in the order referred to.
        size_t next = 0;
        Status status = Status::Ok;
        const auto select = [&](uint32_t sel, int a, int b, int c, HuffTable& user, const HuffTable*& table) {
            if (sel == 0) table = &StandardTable(a);
            else if (sel == 1) table = &StandardTable(b);
            else if (sel == 2 && c != 0) table = &StandardTable(c);
            else if (sel == 3) return UserTable(tables, next, user, table);
            else return Status::Corrupt;
            return Status::Ok;
        };
        if ((status = select(huffmanFlags & 3, 6, 7, 0, userTables_[0], p.fs)) != Status::Ok ||
            (status = select((huffmanFlags >> 2) & 3, 8, 9, 10, userTables_[1], p.ds)) != Status::Ok ||
            (status = select((huffmanFlags >> 4) & 3, 11, 12, 13, userTables_[2], p.dt)) != Status::Ok ||
            (status = select((huffmanFlags >> 6) & 3, 14, 15, 0, userTables_[3], p.rdw)) != Status::Ok ||
            (status = select((huffmanFlags >> 8) & 3, 14, 15, 0, userTables_[4], p.rdh)) != Status::Ok ||
            (status = select((huffmanFlags >> 10) & 3, 14, 15, 0, userTables_[5], p.rdx)) != Status::Ok ||
            (status = select((huffmanFlags >> 12) & 3, 14, 15, 0, userTables_[6], p.rdy)) != Status::Ok ||
            (status = select((huffmanFlags >> 14) & 1 ? 3 : 0, 1, 1, 0, userTables_[7], p.rsize)) != Status::Ok)
            return status;
        // 7.4.3.1.7: the symbol ID Huffman table.
        if (!budget_.Spend(p.numSymbols)) return Status::TooLarge;
        if (ReadSymbolCodes(reader, p.numSymbols, lines_, symbolCodes) != Status::Ok) return Status::Corrupt;
        p.symbolCodes = &symbolCodes;
    } else {
        p.symbolCodeLength = CeilLog2(p.numSymbols);
    }
    if (!p.huffman && !budget_.Spend(uint64_t(1) << p.symbolCodeLength)) return Status::TooLarge;
    ints_.Reset(p.huffman ? 0 : p.symbolCodeLength);
    refinement_.assign(kRefinementContexts, 0);
    if (!p.huffman) mq_.Start(d + pos, n - pos);
    const Status status =
        DecodeText(p, mq_, ints_, refinement_.data(), p.huffman ? &reader : nullptr, pool_, scratch_, region_, budget_);
    if (status != Status::Ok) return status;
    return Place(h, region_, info, h.type != kIntermediateText);
}

// ---------------------------------------------------------------- pattern dictionaries and halftones

Status Decoder::PatternDictionary(const Header& h, const uint8_t* d, size_t n) {
    // 7.4.4: flags (1), HDPW (1), HDPH (1), GRAYMAX (4).
    if (n < 7) return Status::Corrupt;
    const bool mmr = d[0] & 1;
    const int templateId = (d[0] >> 1) & 3;
    const uint32_t width = d[1];
    const uint32_t height = d[2];
    const uint64_t count = uint64_t(Be32(d + 3)) + 1;
    if (width == 0 || height == 0) return Status::Corrupt;
    if (count > kMaxSymbols) return Status::TooLarge;
    Status status = Allocate(symbol_, int64_t(count * width), height, 0, budget_);
    if (status != Status::Ok) return status;
    if (mmr) {
        size_t used = 0;
        status = DecodeMmr(d + 7, n - 7, symbol_, used, budget_);
    } else {
        // Table 27: A1 = (-HDPW, 0), the others nominal.
        GenericParams gp;
        gp.templateId = templateId;
        const int at[8] = {-int(width), 0, -3, -1, 2, -2, -2, -2};
        std::memcpy(gp.at, at, sizeof at);
        generic_.assign(kGenericContexts, 0);
        mq_.Start(d + 7, n - 7);
        status = DecodeGeneric(mq_, generic_.data(), gp, symbol_, budget_);
    }
    if (status != Status::Ok) return status;
    Stored s;
    s.number = h.number;
    s.type = h.type;
    s.first = static_cast<uint32_t>(exports_.size());
    s.count = static_cast<uint32_t>(count);
    if (!budget_.Spend(count * kSymbolWork) || !budget_.Retain(count * sizeof(uint32_t))) return Status::TooLarge;
    for (uint64_t i = 0; i < count; i++) {
        uint32_t id;
        status = pool_.Append(ViewOf(symbol_), static_cast<uint32_t>(i * width), width, budget_, id);
        if (status != Status::Ok) return status;
        exports_.push_back(id);
    }
    return Store(s);
}

Status Decoder::HalftoneRegion(const Header& h, const uint8_t* d, size_t n) {
    // 7.4.5: region info, flags (1), HGW, HGH, HGX, HGY (4 each), HRX, HRY (2 each).
    RegionInfo info;
    if (!ReadRegionInfo(d, n, info) || n < 38) return Status::Corrupt;
    const uint32_t flags = d[17];
    const bool mmr = flags & 1;
    const int templateId = static_cast<int>((flags >> 1) & 3);
    const bool enableSkip = (flags >> 3) & 1;
    const uint32_t op = (flags >> 4) & 7;
    const uint32_t defaultPixel = (flags >> 7) & 1;
    const uint32_t gridWidth = Be32(d + 18);
    const uint32_t gridHeight = Be32(d + 22);
    const int64_t gridX = static_cast<int32_t>(Be32(d + 26));
    const int64_t gridY = static_cast<int32_t>(Be32(d + 30));
    const int64_t stepX = Be16(d + 34);
    const int64_t stepY = Be16(d + 36);
    if (op > 4) return Status::Corrupt;
    const Stored* dictionary = nullptr;
    for (size_t i = 0; i < h.referenceCount && dictionary == nullptr; i++) {
        const Stored* s = Find(references_[h.firstReference + i]);
        if (s != nullptr && s->type == kPatternDictionary) dictionary = s;
    }
    if (dictionary == nullptr || dictionary->count == 0) return Status::Corrupt;
    const View first = pool_.Get(exports_[dictionary->first]);
    const int64_t patternWidth = first.width;
    const int64_t patternHeight = first.height;

    Status status = Allocate(region_, info.width, info.height, defaultPixel, budget_);
    if (status != Status::Ok) return status;
    const uint64_t cells = uint64_t(gridWidth) * gridHeight;
    if (cells > budget_.maxPixels) return Status::TooLarge;
    // An empty grid (one side 0, the other up to 2^32 - 1) draws nothing.
    if (cells == 0) return Place(h, region_, info, h.type != kIntermediateHalftone);
    const int bits = CeilLog2(dictionary->count);
    // The gray-scale values (and the planes) are transient but sized by the grid: charged as retained.
    if (!budget_.Retain(cells * sizeof(uint32_t) + uint64_t(bits) * (cells / 8 + gridHeight)))
        return Status::TooLarge;
    const auto position = [&](uint64_t m, uint64_t g, int64_t& x, int64_t& y) {
        x = FloorShift8(gridX + int64_t(m) * stepY + int64_t(g) * stepX);
        y = FloorShift8(gridY + int64_t(m) * stepX - int64_t(g) * stepY);
    };
    Bitmap skip;
    if (enableSkip) {
        status = Allocate(skip, gridWidth, gridHeight, 0, budget_);
        if (status != Status::Ok) return status;
        for (uint32_t m = 0; m < gridHeight; m++)
            for (uint32_t g = 0; g < gridWidth; g++) {
                int64_t x, y;
                position(m, g, x, y);
                if (x + patternWidth <= 0 || x >= int64_t(info.width) || y + patternHeight <= 0 ||
                    y >= int64_t(info.height))
                    skip.data[size_t(m) * skip.stride + (g >> 3)] |= static_cast<uint8_t>(0x80u >> (g & 7));
            }
    }
    // Annex C: the gray-scale image, bit planes from the most significant, Gray-coded.
    std::vector<uint32_t> gray(size_t(cells), 0);
    if (bits > 0) {
        GenericParams gp;
        gp.templateId = templateId;
        gp.skip = enableSkip ? &skip : nullptr;
        const int at[8] = {templateId <= 1 ? 3 : 2, -1, -3, -1, 2, -2, -2, -2};
        std::memcpy(gp.at, at, sizeof at);
        if (!mmr) {
            generic_.assign(kGenericContexts, 0);
            mq_.Start(d + 38, n - 38);
        }
        size_t offset = 38;
        Bitmap plane;
        Bitmap previous;
        for (int j = bits - 1; j >= 0; j--) {
            status = Allocate(plane, gridWidth, gridHeight, 0, budget_);
            if (status != Status::Ok) return status;
            if (mmr) {
                size_t used = 0;
                status = DecodeMmr(d + offset, n - offset, plane, used, budget_);
                offset += used;
            } else {
                status = DecodeGeneric(mq_, generic_.data(), gp, plane, budget_);
            }
            if (status != Status::Ok) return status;
            if (j != bits - 1)
                for (size_t i = 0; i < plane.data.size(); i++) plane.data[i] ^= previous.data[i];
            for (uint32_t m = 0; m < gridHeight; m++) {
                const uint8_t* row = plane.data.data() + size_t(m) * plane.stride;
                uint32_t* values = gray.data() + size_t(m) * gridWidth;
                for (uint32_t g = 0; g < gridWidth; g++) values[g] |= ((row[g >> 3] >> (7 - (g & 7))) & 1u) << j;
            }
            plane.data.swap(previous.data);
        }
    }
    // 6.6.5.2: the patterns on the grid. A value with no pattern draws nothing.
    for (uint32_t m = 0; m < gridHeight; m++)
        for (uint32_t g = 0; g < gridWidth; g++) {
            const uint32_t v = gray[size_t(m) * gridWidth + g];
            if (v >= dictionary->count) continue;
            int64_t x, y;
            position(m, g, x, y);
            const View pattern = pool_.Get(exports_[dictionary->first + v]);
            if (!budget_.Spend(1 + uint64_t(pattern.stride) * pattern.height)) return Status::TooLarge;
            Combine(region_, pattern, x, y, static_cast<CombineOp>(op));
        }
    return Place(h, region_, info, h.type != kIntermediateHalftone);
}

// ---------------------------------------------------------------- generic and refinement regions

Status Decoder::GenericRegion(const Header& h, const uint8_t* d, size_t n) {
    // 7.4.6: region info, flags (1), AT pixels (8 or 2 bytes, arithmetic only), data.
    RegionInfo info;
    if (!ReadRegionInfo(d, n, info) || n < 18) return Status::Corrupt;
    const uint32_t flags = d[17];
    const bool mmr = flags & 1;
    GenericParams gp;
    gp.templateId = static_cast<int>((flags >> 1) & 3);
    gp.tpgdon = (flags >> 3) & 1;
    gp.extTemplate = (flags >> 4) & 1;
    if (gp.extTemplate && !mmr) return Status::Unsupported;
    size_t pos = 18;
    if (!mmr) {
        const size_t atBytes = gp.templateId == 0 ? 8 : 2;
        if (n < pos + atBytes) return Status::Corrupt;
        for (size_t i = 0; i < atBytes; i++) gp.at[i] = static_cast<int8_t>(d[pos + i]);
        pos += atBytes;
    }
    // An unknown data length: the row count after the end marker gives the height.
    uint32_t height = info.height;
    size_t end = n;
    if (h.rowCount != 0xFFFFFFFFu) {
        if (h.rowCount < height) height = h.rowCount;
        end = n - 4;
    }
    Status status = Allocate(region_, info.width, height, 0, budget_);
    if (status != Status::Ok) return status;
    if (mmr) {
        size_t used = 0;
        status = DecodeMmr(d + pos, end - pos, region_, used, budget_);
        // Damaged MMR data: the rows before the damage are placed, then decoding stops.
        if (status == Status::Corrupt) {
            const Status placed = Place(h, region_, info, h.type != kIntermediateGeneric);
            return placed != Status::Ok ? placed : Status::Corrupt;
        }
    } else {
        generic_.assign(kGenericContexts, 0);
        mq_.Start(d + pos, end - pos);
        status = DecodeGeneric(mq_, generic_.data(), gp, region_, budget_);
    }
    if (status != Status::Ok) return status;
    return Place(h, region_, info, h.type != kIntermediateGeneric);
}

Status Decoder::RefinementRegion(const Header& h, const uint8_t* d, size_t n) {
    // 7.4.7: region info, flags (1), AT pixels (4 bytes for template 0), data.
    RegionInfo info;
    if (!ReadRegionInfo(d, n, info) || n < 18) return Status::Corrupt;
    const uint32_t flags = d[17];
    RefinementParams rp;
    rp.templateId = static_cast<int>(flags & 1);
    rp.tpgron = (flags >> 1) & 1;
    size_t pos = 18;
    if (rp.templateId == 0) {
        if (n < pos + 4) return Status::Corrupt;
        for (size_t i = 0; i < 4; i++) rp.at[i] = static_cast<int8_t>(d[pos + i]);
        pos += 4;
    }
    // 7.4.7.4: the referred-to intermediate region, else the page under the region.
    Stored* source = nullptr;
    for (size_t i = 0; i < h.referenceCount && source == nullptr; i++) {
        Stored* s = Find(references_[h.firstReference + i]);
        if (s != nullptr && s->region >= 0) source = s;
    }
    if (source != nullptr) {
        rp.reference = ViewOf(regions_[size_t(source->region)]);
    } else {
        if (!havePage_) return Status::Corrupt;
        Status status = Allocate(reference_, info.width, info.height, 0, budget_);
        if (status != Status::Ok) return status;
        if (!budget_.Spend(reference_.data.size())) return Status::TooLarge;
        Combine(reference_, ViewOf(page_), -int64_t(info.x), -int64_t(info.y), CombineOp::Replace);
        rp.reference = ViewOf(reference_);
    }
    Status status = Allocate(region_, info.width, info.height, 0, budget_);
    if (status != Status::Ok) return status;
    refinement_.assign(kRefinementContexts, 0);
    mq_.Start(d + pos, n - pos);
    status = DecodeRefinement(mq_, refinement_.data(), rp, region_, budget_);
    if (status != Status::Ok) return status;
    if (source != nullptr) {
        // The auxiliary buffer is refined: it is discarded or moves to this segment.
        regions_[size_t(source->region)].data = std::vector<uint8_t>();
        source->region = -1;
    }
    return Place(h, region_, info, h.type != kIntermediateRefinement);
}

Result Decoder::Run(const uint8_t* data, size_t size, const uint8_t* globals, size_t globalsSize, Info& info,
                    std::string& rows) {
    Status status = Status::Ok;
    if (globals != nullptr && globalsSize != 0) status = ReadStream(globals, globalsSize);
    if (status == Status::Ok && !pageDone_) status = ReadStream(data, size);
    if (status == Status::TooLarge) return Result::TooLarge;
    if (status == Status::Unsupported) return Result::Unsupported;
    if (!havePage_) return Result::Corrupt;
    info.width = page_.width;
    info.height = page_.height;
    info.damaged = damaged_ || status == Status::Corrupt;
    // The filter's sense: JBIG2's 1 = black becomes 0.
    rows.resize(page_.data.size());
    for (size_t i = 0; i < page_.data.size(); i++) rows[i] = static_cast<char>(page_.data[i] ^ 0xFFu);
    return Result::Ok;
}

} // namespace

Result Decode(const uint8_t* data, size_t size, const uint8_t* globals, size_t globalsSize, uint64_t maxPixels,
              Info& info, std::string& rows) {
    info = Info();
    rows.clear();
    Decoder decoder(maxPixels);
    return decoder.Run(data, size, globals, globalsSize, info, rows);
}

} // namespace RayoPdf::Image::Jbig2
