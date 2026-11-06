// SPDX-License-Identifier: Apache-2.0
// Forked from RayoMD src/core/cff.cpp at ef106ca (Apache-2.0, same author).
// The INDEX and DICT readers of RayoMD's CFF module, plus the charset and encoding readers it
// lacked (CFF specification, Adobe TN #5176, sections 12-13; algorithms §3.3). Outlines, private
// DICTs and subroutines are not read.

#include "text/program.h"

#include "text/tables.h"

namespace RayoPdf::Text::Program {
namespace {

constexpr size_t kMaxDictEntries = 64;

// A CFF INDEX: `count` items whose offsets, `offSize` bytes each, start at `offsets`; item i
// spans [dataStart + offset[i], dataStart + offset[i + 1]) of the table, all of it before dataEnd.
struct Index {
    uint32_t count = 0;
    uint32_t dataStart = 0;
    uint32_t dataEnd = 0;
    const uint8_t* offsets = nullptr;
    uint8_t offSize = 0;
};

struct DictEntry {
    uint16_t op = 0;
    uint8_t operandCount = 0;
    double operands[48] = {};
};

uint32_t ReadOffset(const uint8_t* at, uint8_t size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++) value = value << 8 | at[i];
    return value;
}

uint16_t ReadU16(const uint8_t* at) {
    return static_cast<uint16_t>(at[0] << 8 | at[1]);
}

// A DICT operand that is a whole number from 0 to 2^31 - 1, as offsets and sizes are.
bool Offset(const DictEntry& entry, int index, uint32_t& value) {
    if (index >= entry.operandCount) return false;
    const double operand = entry.operands[index];
    if (!(operand >= 0.0 && operand <= 2147483647.0)) return false;
    value = static_cast<uint32_t>(operand);
    return static_cast<double>(value) == operand;
}

// The value of a real number operand, nibbles from `at` (after the 30) up to the nibble 0xf.
bool ReadReal(const uint8_t* data, size_t size, size_t& at, double& value) {
    double mantissa = 0.0;
    double scale = 1.0;
    int exponent = 0;
    bool negative = false;
    bool fraction = false;
    bool inExponent = false;
    bool negativeExponent = false;
    while (at < size) {
        const uint8_t byte = data[at++];
        for (int shift = 4; shift >= 0; shift -= 4) {
            const uint8_t nibble = (byte >> shift) & 15;
            if (nibble == 0xf) {
                if (inExponent) exponent = negativeExponent ? -exponent : exponent;
                double result = mantissa;
                for (int i = 0; i < exponent && i < 64; i++) result *= 10.0;
                for (int i = 0; i > exponent && i > -64; i--) result /= 10.0;
                value = negative ? -result : result;
                return true;
            }
            if (nibble <= 9) {
                if (inExponent) {
                    if (exponent < 1000) exponent = exponent * 10 + nibble;
                } else if (fraction) {
                    scale /= 10.0;
                    mantissa += nibble * scale;
                } else {
                    mantissa = mantissa * 10.0 + nibble;
                }
            } else if (nibble == 0xa) {
                fraction = true;
            } else if (nibble == 0xb || nibble == 0xc) {
                inExponent = true;
                negativeExponent = nibble == 0xc;
            } else if (nibble == 0xe) {
                negative = true;
            } else {
                return false;
            }
        }
    }
    return false;
}

bool ReadIndex(const uint8_t* base, size_t size, size_t at, Index& index, size_t& next) {
    index = Index();
    if (at > size || size - at < 2) return false;
    const uint32_t count = ReadU16(base + at);
    if (count == 0) {
        index.dataStart = index.dataEnd = static_cast<uint32_t>(at + 2);
        next = at + 2;
        return true;
    }
    if (size - at < 3) return false;
    const uint8_t offSize = base[at + 2];
    if (offSize < 1 || offSize > 4) return false;
    const size_t offsetsAt = at + 3;
    const size_t offsetsBytes = static_cast<size_t>(count + 1) * offSize;
    if (offsetsBytes > size - offsetsAt) return false;
    const size_t dataStart = offsetsAt + offsetsBytes - 1;
    const uint32_t first = ReadOffset(base + offsetsAt, offSize);
    const uint32_t last = ReadOffset(base + offsetsAt + static_cast<size_t>(count) * offSize, offSize);
    if (first != 1 || last < 1 || last > size - dataStart || dataStart + last > 0xffffffffu) return false;
    index.count = count;
    index.offSize = offSize;
    index.offsets = base + offsetsAt;
    index.dataStart = static_cast<uint32_t>(dataStart);
    index.dataEnd = static_cast<uint32_t>(dataStart + last);
    next = dataStart + last;
    return true;
}

// Item `i` of `index`; a view with a null data pointer when its offsets are out of range.
std::string_view IndexItem(const uint8_t* base, const Index& index, uint32_t i) {
    if (i >= index.count) return {};
    const uint32_t start = ReadOffset(index.offsets + static_cast<size_t>(i) * index.offSize, index.offSize);
    const uint32_t end = ReadOffset(index.offsets + static_cast<size_t>(i + 1) * index.offSize, index.offSize);
    if (start < 1 || end < start || static_cast<uint64_t>(index.dataStart) + end > index.dataEnd) return {};
    return std::string_view(reinterpret_cast<const char*>(base) + index.dataStart + start, end - start);
}

// Parses a DICT into at most `max` entries; false when it is malformed or has more.
bool ParseDict(std::string_view dict, DictEntry* out, size_t max, size_t& count) {
    count = 0;
    const uint8_t* const data = reinterpret_cast<const uint8_t*>(dict.data());
    const size_t size = dict.size();
    double values[48];
    uint8_t operands = 0;
    size_t at = 0;
    while (at < size) {
        const uint8_t b0 = data[at];
        if (b0 <= 21) {
            uint16_t op = b0;
            size_t length = 1;
            if (b0 == 12) {
                if (at + 1 >= size) return false;
                op = static_cast<uint16_t>(1200 + data[at + 1]);
                length = 2;
            }
            if (count >= max) return false;
            DictEntry& entry = out[count++];
            entry.op = op;
            entry.operandCount = operands;
            for (uint8_t i = 0; i < operands; i++) entry.operands[i] = values[i];
            at += length;
            operands = 0;
            continue;
        }
        double value = 0.0;
        if (b0 == 28) {
            if (size - at < 3) return false;
            value = static_cast<int16_t>(ReadU16(data + at + 1));
            at += 3;
        } else if (b0 == 29) {
            if (size - at < 5) return false;
            value = static_cast<int32_t>(ReadOffset(data + at + 1, 4));
            at += 5;
        } else if (b0 == 30) {
            at++;
            if (!ReadReal(data, size, at, value)) return false;
        } else if (b0 >= 32 && b0 <= 246) {
            value = b0 - 139;
            at++;
        } else if (b0 >= 247 && b0 <= 254) {
            if (size - at < 2) return false;
            value = b0 <= 250 ? (b0 - 247) * 256 + data[at + 1] + 108 : -(b0 - 251) * 256 - data[at + 1] - 108;
            at += 2;
        } else {
            return false;
        }
        if (operands >= 48) return false;
        values[operands++] = value;
    }
    return operands == 0;
}

// The name of string id `sid`: a standard string below 391, else item sid - 391 of the String INDEX.
std::string_view SidName(const uint8_t* data, const Index& strings, uint32_t sid) {
    if (sid < 391) return Tables::GlyphName(Tables::kCffStandardStrings[sid]);
    const std::string_view name = IndexItem(data, strings, sid - 391);
    return name.data() == nullptr ? std::string_view() : name;
}

// The charset at `offset` (> 2): the SID (or CID) of every glyph after .notdef, into `ids`.
bool ReadCharset(const uint8_t* data, size_t size, uint32_t offset, uint32_t glyphCount, std::vector<uint16_t>& ids) {
    ids.assign(glyphCount, 0);
    if (offset >= size) return false;
    const uint8_t format = data[offset];
    size_t at = offset + 1;
    uint32_t glyph = 1;
    if (format == 0) {
        for (; glyph < glyphCount; ++glyph, at += 2) {
            if (size - at < 2) return false;
            ids[glyph] = ReadU16(data + at);
        }
        return true;
    }
    if (format != 1 && format != 2) return false;
    const size_t rangeBytes = format == 1 ? 3 : 4;
    while (glyph < glyphCount) {
        if (size - at < rangeBytes) return false;
        const uint32_t first = ReadU16(data + at);
        const uint32_t left = format == 1 ? data[at + 2] : ReadU16(data + at + 2);
        at += rangeBytes;
        for (uint32_t i = 0; i <= left && glyph < glyphCount; ++i) ids[glyph++] = static_cast<uint16_t>(first + i);
    }
    return true;
}

bool ReadEncoding(const uint8_t* data, size_t size, uint32_t offset, uint32_t glyphCount,
                  const std::vector<uint16_t>& sids, CffInfo& out) {
    for (int32_t& g : out.codeGlyphs) g = -1;
    if (offset >= size) return false;
    const uint8_t format = data[offset];
    size_t at = offset + 1;
    if (at >= size) return false;
    if ((format & 0x7F) == 0) {
        const uint32_t codes = data[at++];
        if (size - at < codes) return false;
        for (uint32_t i = 0; i < codes && i + 1 < glyphCount; ++i) out.codeGlyphs[data[at + i]] = static_cast<int32_t>(i + 1);
        at += codes;
    } else if ((format & 0x7F) == 1) {
        const uint32_t ranges = data[at++];
        uint32_t glyph = 1;
        for (uint32_t r = 0; r < ranges; ++r, at += 2) {
            if (size - at < 2) return false;
            const uint32_t first = data[at];
            const uint32_t left = data[at + 1];
            for (uint32_t i = 0; i <= left && first + i < 256 && glyph < glyphCount; ++i) {
                out.codeGlyphs[first + i] = static_cast<int32_t>(glyph++);
            }
        }
    } else {
        return false;
    }
    if (format & 0x80) {
        if (at >= size) return false;
        const uint32_t supplements = data[at++];
        for (uint32_t s = 0; s < supplements; ++s, at += 3) {
            if (size - at < 3) return false;
            const uint16_t sid = ReadU16(data + at + 1);
            for (uint32_t g = 0; g < sids.size(); ++g) {
                if (sids[g] == sid) {
                    out.codeGlyphs[data[at]] = static_cast<int32_t>(g);
                    break;
                }
            }
        }
    }
    return true;
}

} // namespace

bool ReadCff(std::string_view bytes, CffInfo& out) {
    out = CffInfo();
    const uint8_t* const data = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t size = bytes.size();
    if (size < 4 || data[0] != 1) return false;
    const size_t headerSize = data[2];
    if (headerSize < 4 || headerSize > size) return false;
    Index names, topDicts, strings, globalSubrs;
    size_t next = 0;
    if (!ReadIndex(data, size, headerSize, names, next) || !ReadIndex(data, size, next, topDicts, next) ||
        !ReadIndex(data, size, next, strings, next) || !ReadIndex(data, size, next, globalSubrs, next) ||
        topDicts.count == 0) {
        return false;
    }
    const std::string_view top = IndexItem(data, topDicts, 0);
    if (top.data() == nullptr) return false;
    DictEntry entries[kMaxDictEntries];
    size_t count = 0;
    if (!ParseDict(top, entries, kMaxDictEntries, count)) return false;
    uint32_t charStrings = 0, charset = 0, encoding = 0;
    bool hasCharStrings = false;
    for (size_t i = 0; i < count; i++) {
        const DictEntry& entry = entries[i];
        if (entry.op == 17) hasCharStrings = Offset(entry, 0, charStrings);
        else if (entry.op == 15) Offset(entry, 0, charset);
        else if (entry.op == 16) Offset(entry, 0, encoding);
        else if (entry.op == 1230) out.cidKeyed = true;
    }
    Index glyphs;
    if (!hasCharStrings || !ReadIndex(data, size, charStrings, glyphs, next) || glyphs.count == 0) return false;
    out.glyphCount = glyphs.count;

    std::vector<uint16_t> ids;
    if (out.cidKeyed) {
        // The charset of a CID-keyed font holds CIDs; the predefined ones do not apply.
        if (charset <= 2 || !ReadCharset(data, size, charset, out.glyphCount, ids)) {
            ids.resize(out.glyphCount);
            for (uint32_t g = 0; g < out.glyphCount; ++g) ids[g] = static_cast<uint16_t>(g);
        }
        out.glyphCids = std::move(ids);
        return true;
    }

    out.glyphNames.ends.reserve(out.glyphCount);
    if (charset <= 2) {
        const uint16_t* table = nullptr;
        uint32_t tableCount = 0;
        if (charset == 1) table = Tables::kCffExpertCharset, tableCount = Tables::kCffExpertCharsetCount;
        if (charset == 2) table = Tables::kCffExpertSubsetCharset, tableCount = Tables::kCffExpertSubsetCharsetCount;
        ids.assign(out.glyphCount, 0);
        for (uint32_t g = 0; g < out.glyphCount; ++g) {
            if (table) {
                out.glyphNames.Push(g < tableCount ? Tables::GlyphName(table[g]) : std::string_view());
            } else {
                // ISOAdobe: glyph g is SID g.
                ids[g] = static_cast<uint16_t>(g < 229 ? g : 0);
                out.glyphNames.Push(g < 229 ? SidName(data, strings, g) : std::string_view());
            }
        }
    } else {
        if (!ReadCharset(data, size, charset, out.glyphCount, ids)) return false;
        for (uint32_t g = 0; g < out.glyphCount; ++g) out.glyphNames.Push(SidName(data, strings, ids[g]));
    }

    if (encoding <= 1) {
        out.encoding = static_cast<int>(encoding);
    } else {
        out.encoding = 2;
        if (!ReadEncoding(data, size, encoding, out.glyphCount, ids, out)) out.encoding = 0;
    }
    return true;
}

} // namespace RayoPdf::Text::Program
