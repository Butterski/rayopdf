// SPDX-License-Identifier: Apache-2.0
// Entropy coding of the JBIG2 decoder, written from ITU-T T.88: the Qe table of the MQ decoder
// (table E.1), the arithmetic integer decoders (annex A) and Huffman tables (annex B: the prefix
// code assignment of B.3, code table segments of B.2, the standard tables of B.5).
#include "image/jbig2_internal.h"

namespace RayoPdf::Image::Jbig2::Internal {

const QeEntry kQeTable[47] = {
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

bool DecodeInt(MqDecoder& mq, IntContexts& contexts, int64_t& value) {
    // A.2: PREV keeps the last 8 bits decoded, with bit 8 set once more than 8 bits were.
    uint32_t prev = 1;
    const auto bit = [&]() {
        const uint32_t d = mq.Decode(contexts.cx[prev]);
        prev = prev < 256 ? (prev << 1 | d) : (((prev << 1 | d) & 511u) | 256u);
        return d;
    };
    const uint32_t sign = bit();
    int bits;
    int64_t offset;
    if (!bit()) {
        bits = 2, offset = 0;
    } else if (!bit()) {
        bits = 4, offset = 4;
    } else if (!bit()) {
        bits = 6, offset = 20;
    } else if (!bit()) {
        bits = 8, offset = 84;
    } else if (!bit()) {
        bits = 12, offset = 340;
    } else {
        bits = 32, offset = 4436;
    }
    int64_t v = 0;
    for (int i = 0; i < bits; i++) v = v << 1 | bit();
    v += offset;
    if (sign && v == 0) return false;  // OOB
    value = sign ? -v : v;
    return true;
}

uint32_t DecodeIaid(MqDecoder& mq, Context* contexts, int codeLength) {
    uint32_t prev = 1;
    for (int i = 0; i < codeLength; i++) prev = prev << 1 | mq.Decode(contexts[prev]);
    return prev - (uint32_t(1) << codeLength);
}

// ---------------------------------------------------------------- Huffman tables

Status BuildTable(const HuffLine* lines, size_t n, HuffTable& table) {
    table = HuffTable();
    uint32_t count[33] = {};
    for (size_t i = 0; i < n; i++) {
        if (lines[i].prefixLength > 32 || lines[i].rangeLength > 32) return Status::Corrupt;
        count[lines[i].prefixLength]++;
    }
    count[0] = 0;
    // B.3: FIRSTCODE[L] = (FIRSTCODE[L - 1] + LENCOUNT[L - 1]) * 2, codes of one length in line order.
    uint64_t first = 0;
    uint32_t offset = 0;
    for (int length = 1; length <= 32; length++) {
        first = (first + count[length - 1]) << 1;
        if (count[length] != 0) {
            if (first + count[length] > (uint64_t(1) << length)) return Status::Corrupt;
            table.maxLength = length;
        }
        table.firstCode[length] = static_cast<uint32_t>(first);
        table.count[length] = count[length];
        table.offset[length] = offset;
        offset += count[length];
    }
    table.lines.resize(offset);
    uint32_t next[33];
    std::memcpy(next, table.offset, sizeof next);
    for (size_t i = 0; i < n; i++)
        if (lines[i].prefixLength != 0) table.lines[next[lines[i].prefixLength]++] = lines[i];
    table.built = true;
    return Status::Ok;
}

Code DecodeHuffman(BitReader& reader, const HuffTable& table, int64_t& value) {
    uint32_t code = 0;
    for (int length = 1; length <= table.maxLength; length++) {
        code = code << 1 | reader.ReadBit();
        const uint32_t index = code - table.firstCode[length];
        if (index < table.count[length]) {
            const HuffLine& line = table.lines[table.offset[length] + index];
            if (line.kind == kLineOob) return reader.overrun ? Code::Error : Code::Oob;
            const int64_t offset = line.rangeLength ? reader.ReadBits(line.rangeLength) : 0;
            value = line.kind == kLineLower ? line.low - offset : line.low + offset;
            return reader.overrun ? Code::Error : Code::Value;
        }
    }
    return Code::Error;
}

Status ParseTableSegment(const uint8_t* data, size_t size, HuffTable& table) {
    if (size < 9) return Status::Corrupt;
    const uint32_t flags = data[0];
    const auto i32 = [&](size_t at) {
        return static_cast<int32_t>(uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 |
                                    uint32_t(data[at + 2]) << 8 | data[at + 3]);
    };
    const int64_t low = i32(1);
    const int64_t high = i32(5);
    const int prefixBits = int((flags >> 1) & 7) + 1;
    const int rangeBits = int((flags >> 4) & 7) + 1;
    if (high <= low) return Status::Corrupt;
    BitReader reader(data + 9, size - 9);
    // Each line codes at least one bit of the table data, so the data bounds the line count.
    std::vector<HuffLine> lines;
    int64_t current = low;
    while (current < high) {
        if (reader.overrun || lines.size() > (size - 9) * 8) return Status::Corrupt;
        HuffLine line;
        line.low = current;
        line.prefixLength = static_cast<uint8_t>(reader.ReadBits(prefixBits));
        const uint32_t range = reader.ReadBits(rangeBits);
        if (range > 32) return Status::Corrupt;
        line.rangeLength = static_cast<uint8_t>(range);
        lines.push_back(line);
        current += int64_t(1) << range;
    }
    HuffLine lower;
    lower.low = low - 1;
    lower.prefixLength = static_cast<uint8_t>(reader.ReadBits(prefixBits));
    lower.rangeLength = 32;
    lower.kind = kLineLower;
    lines.push_back(lower);
    HuffLine upper;
    upper.low = high;
    upper.prefixLength = static_cast<uint8_t>(reader.ReadBits(prefixBits));
    upper.rangeLength = 32;
    lines.push_back(upper);
    if (flags & 1) {
        HuffLine oob;
        oob.prefixLength = static_cast<uint8_t>(reader.ReadBits(prefixBits));
        oob.kind = kLineOob;
        lines.push_back(oob);
    }
    if (reader.overrun) return Status::Corrupt;
    return BuildTable(lines.data(), lines.size(), table);
}

Status ReadSymbolCodes(BitReader& reader, uint32_t count, std::vector<HuffLine>& lines, HuffTable& table) {
    // RUNCODE0-34 lengths (4 bits each), then runs (table 29): 0-31 one length, 32 the previous
    // length 3-6 times, 33 zeros 3-10 times, 34 zeros 11-138 times. A run past the last symbol
    // stops there.
    HuffLine runLines[35];
    for (int i = 0; i < 35; i++) {
        runLines[i].low = i;
        runLines[i].prefixLength = static_cast<uint8_t>(reader.ReadBits(4));
    }
    HuffTable runCodes;
    if (reader.overrun || BuildTable(runLines, 35, runCodes) != Status::Ok) return Status::Corrupt;
    lines.assign(count, HuffLine());
    uint32_t i = 0;
    while (i < count) {
        int64_t code = 0;
        if (DecodeHuffman(reader, runCodes, code) != Code::Value) return Status::Corrupt;
        uint32_t repeat = 1;
        uint8_t length = 0;
        if (code < 32) {
            length = static_cast<uint8_t>(code);
        } else if (code == 32) {
            if (i == 0) return Status::Corrupt;
            length = lines[i - 1].prefixLength;
            repeat = 3 + reader.ReadBits(2);
        } else if (code == 33) {
            repeat = 3 + reader.ReadBits(3);
        } else {
            repeat = 11 + reader.ReadBits(7);
        }
        for (uint32_t k = 0; k < repeat && i < count; k++, i++) {
            lines[i].low = i;
            lines[i].prefixLength = length;
        }
    }
    reader.Align();
    if (reader.overrun) return Status::Corrupt;
    return BuildTable(lines.data(), lines.size(), table);
}

namespace {

// B.5: the standard tables as (low, prefix length, range length, kind), in table line order; the
// upper range lines are ordinary lines with 32-bit ranges. B.3 gives them the codes of B.5.
struct StandardLine {
    int32_t low;
    uint8_t prefixLength;
    uint8_t rangeLength;
    uint8_t kind;
};

constexpr uint8_t N = kLineNormal;
constexpr uint8_t L = kLineLower;
constexpr uint8_t O = kLineOob;

constexpr StandardLine kB1[] = {{0, 1, 4, N}, {16, 2, 8, N}, {272, 3, 16, N}, {65808, 3, 32, N}};
constexpr StandardLine kB2[] = {{0, 1, 0, N},  {1, 2, 0, N},  {2, 3, 0, N}, {3, 4, 3, N},
                                {11, 5, 6, N}, {75, 6, 32, N}, {0, 6, 0, O}};
constexpr StandardLine kB3[] = {{-256, 8, 8, N}, {0, 1, 0, N},     {1, 2, 0, N},   {2, 3, 0, N}, {3, 4, 3, N},
                                {11, 5, 6, N},   {-257, 8, 32, L}, {75, 7, 32, N}, {0, 6, 0, O}};
constexpr StandardLine kB4[] = {{1, 1, 0, N}, {2, 2, 0, N}, {3, 3, 0, N}, {4, 4, 3, N}, {12, 5, 6, N}, {76, 5, 32, N}};
constexpr StandardLine kB5[] = {{-255, 7, 8, N}, {1, 1, 0, N},  {2, 2, 0, N},     {3, 3, 0, N},
                                {4, 4, 3, N},    {12, 5, 6, N}, {-256, 7, 32, L}, {76, 6, 32, N}};
constexpr StandardLine kB6[] = {{-2048, 5, 10, N}, {-1024, 4, 9, N}, {-512, 4, 8, N},   {-256, 4, 7, N}, {-128, 5, 6, N},
                                {-64, 5, 5, N},    {-32, 4, 5, N},   {0, 2, 7, N},      {128, 3, 7, N},  {256, 3, 8, N},
                                {512, 4, 9, N},    {1024, 4, 10, N}, {-2049, 6, 32, L}, {2048, 6, 32, N}};
constexpr StandardLine kB7[] = {{-1024, 4, 9, N}, {-512, 3, 8, N},  {-256, 4, 7, N},   {-128, 5, 6, N}, {-64, 5, 5, N},
                                {-32, 4, 5, N},   {0, 4, 5, N},     {32, 5, 5, N},     {64, 5, 6, N},   {128, 4, 7, N},
                                {256, 3, 8, N},   {512, 3, 9, N},   {1024, 3, 10, N},  {-1025, 5, 32, L},
                                {2048, 5, 32, N}};
constexpr StandardLine kB8[] = {{-15, 8, 3, N},   {-7, 9, 1, N},   {-5, 8, 1, N},   {-3, 9, 0, N},    {-2, 7, 0, N},
                                {-1, 4, 0, N},    {0, 2, 1, N},    {2, 5, 0, N},    {3, 6, 0, N},     {4, 3, 4, N},
                                {20, 6, 1, N},    {22, 4, 4, N},   {38, 4, 5, N},   {70, 5, 6, N},    {134, 5, 7, N},
                                {262, 6, 7, N},   {390, 7, 8, N},  {646, 6, 10, N}, {-16, 9, 32, L},  {1670, 9, 32, N},
                                {0, 2, 0, O}};
constexpr StandardLine kB9[] = {{-31, 8, 4, N},   {-15, 9, 2, N},  {-11, 8, 2, N},  {-7, 9, 1, N},    {-5, 7, 1, N},
                                {-3, 4, 1, N},    {-1, 3, 1, N},   {1, 3, 1, N},    {3, 5, 1, N},     {5, 6, 1, N},
                                {7, 3, 5, N},     {39, 6, 2, N},   {43, 4, 5, N},   {75, 4, 6, N},    {139, 5, 7, N},
                                {267, 5, 8, N},   {523, 6, 8, N},  {779, 7, 9, N},  {1291, 6, 11, N}, {-32, 9, 32, L},
                                {3339, 9, 32, N}, {0, 2, 0, O}};
constexpr StandardLine kB10[] = {{-21, 7, 4, N},  {-5, 8, 0, N},   {-4, 7, 0, N},    {-3, 5, 0, N},    {-2, 2, 2, N},
                                 {2, 5, 0, N},    {3, 6, 0, N},    {4, 7, 0, N},     {5, 8, 0, N},     {6, 2, 6, N},
                                 {70, 5, 5, N},   {102, 6, 5, N},  {134, 6, 6, N},   {198, 6, 7, N},   {326, 6, 8, N},
                                 {582, 6, 9, N},  {1094, 6, 10, N}, {2118, 7, 11, N}, {-22, 8, 32, L},  {4166, 8, 32, N},
                                 {0, 2, 0, O}};
constexpr StandardLine kB11[] = {{1, 1, 0, N},  {2, 2, 1, N},  {4, 4, 0, N},  {5, 4, 1, N},  {7, 5, 1, N},
                                 {9, 5, 2, N},  {13, 6, 2, N}, {17, 7, 2, N}, {21, 7, 3, N}, {29, 7, 4, N},
                                 {45, 7, 5, N}, {77, 7, 6, N}, {141, 7, 32, N}};
constexpr StandardLine kB12[] = {{1, 1, 0, N},  {2, 2, 0, N},  {3, 3, 1, N},  {5, 5, 0, N},  {6, 5, 1, N},
                                 {8, 6, 1, N},  {10, 7, 0, N}, {11, 7, 1, N}, {13, 7, 2, N}, {17, 7, 3, N},
                                 {25, 7, 4, N}, {41, 8, 5, N}, {73, 8, 32, N}};
constexpr StandardLine kB13[] = {{1, 1, 0, N},  {2, 3, 0, N},  {3, 4, 0, N},  {4, 5, 0, N},  {5, 4, 1, N},
                                 {7, 3, 3, N},  {15, 6, 1, N}, {17, 6, 2, N}, {21, 6, 3, N}, {29, 6, 4, N},
                                 {45, 6, 5, N}, {77, 7, 6, N}, {141, 7, 32, N}};
constexpr StandardLine kB14[] = {{-2, 3, 0, N}, {-1, 3, 0, N}, {0, 1, 0, N}, {1, 3, 0, N}, {2, 3, 0, N}};
constexpr StandardLine kB15[] = {{-24, 7, 4, N}, {-8, 6, 2, N}, {-4, 5, 1, N},   {-2, 4, 0, N},    {-1, 3, 0, N},
                                 {0, 1, 0, N},   {1, 3, 0, N},  {2, 4, 0, N},    {3, 5, 1, N},     {5, 6, 2, N},
                                 {9, 7, 4, N},   {-25, 7, 32, L}, {25, 7, 32, N}};

struct StandardSet {
    const StandardLine* lines;
    size_t count;
};

template <size_t K>
constexpr StandardSet Set(const StandardLine (&lines)[K]) {
    return StandardSet{lines, K};
}

constexpr StandardSet kStandard[15] = {Set(kB1),  Set(kB2),  Set(kB3),  Set(kB4),  Set(kB5),
                                       Set(kB6),  Set(kB7),  Set(kB8),  Set(kB9),  Set(kB10),
                                       Set(kB11), Set(kB12), Set(kB13), Set(kB14), Set(kB15)};

struct StandardTables {
    HuffTable tables[15];
    StandardTables() {
        for (int i = 0; i < 15; i++) {
            HuffLine lines[32];
            for (size_t k = 0; k < kStandard[i].count; k++) {
                lines[k].low = kStandard[i].lines[k].low;
                lines[k].prefixLength = kStandard[i].lines[k].prefixLength;
                lines[k].rangeLength = kStandard[i].lines[k].rangeLength;
                lines[k].kind = kStandard[i].lines[k].kind;
            }
            BuildTable(lines, kStandard[i].count, tables[i]);
        }
    }
};

} // namespace

const HuffTable& StandardTable(int number) {
    // Built once, on first use (a function-local static: thread-safe initialisation).
    static const StandardTables kTables;
    return kTables.tables[number - 1];
}

} // namespace RayoPdf::Image::Jbig2::Internal
