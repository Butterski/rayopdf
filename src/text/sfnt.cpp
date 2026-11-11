// SPDX-License-Identifier: Apache-2.0
// TrueType / OpenType tables for the text layer (OpenType specification: 'cmap', 'post', 'maxp';
// algorithms §3.3, §3.4): the table directory, cmap subtables of formats 0, 4, 6, 12 and 13, and
// 'post' glyph names.

#include "text/program.h"

#include "text/tables.h"

namespace RayoPdf::Text::Program {
namespace {

// A 'ttcf' collection nests at most this deep (one level is all the format has).
constexpr int kMaxCollectionDepth = 1;
// What ForEach takes from its budget for a format 4 code it finds by Format4Glyph's search
// (segments out of order or overlapping): about the cost of that many codes read directly.
constexpr uint32_t kSearchedCodeCost = 16;

uint32_t U16(std::string_view d, size_t at) {
    if (at > d.size() || d.size() - at < 2) return 0;
    return static_cast<uint32_t>(static_cast<unsigned char>(d[at]) << 8 | static_cast<unsigned char>(d[at + 1]));
}

uint32_t U32(std::string_view d, size_t at) {
    if (at > d.size() || d.size() - at < 4) return 0;
    return U16(d, at) << 16 | U16(d, at + 2);
}

bool Has(std::string_view d, size_t at, size_t bytes) {
    return at <= d.size() && d.size() - at >= bytes;
}

std::string_view Table(std::string_view font, uint32_t offset, uint32_t length) {
    if (offset > font.size()) return {};
    const size_t available = font.size() - offset;
    return font.substr(offset, length < available ? length : available);
}

// The table directory at `at` (0, or a collection's first font); table offsets count from the
// start of the file in both cases.
bool ReadDirectory(std::string_view data, size_t at, Sfnt& out, int depth) {
    const uint32_t version = U32(data, at);
    if (version == 0x74746366u) {  // 'ttcf'
        if (depth >= kMaxCollectionDepth || U32(data, at + 8) == 0) return false;
        const uint32_t first = U32(data, at + 12);
        if (first == 0 || first >= data.size()) return false;
        return ReadDirectory(data, first, out, depth + 1);
    }
    if (version != 0x00010000u && version != 0x74727565u && version != 0x4F54544Fu) return false;  // 'true', 'OTTO'
    const uint32_t numTables = U16(data, at + 4);
    if (numTables == 0 || !Has(data, at + 12, static_cast<size_t>(numTables) * 16)) return false;
    std::string_view cmap;
    for (uint32_t i = 0; i < numTables; ++i) {
        const size_t record = at + 12 + static_cast<size_t>(i) * 16;
        const std::string_view tag = data.substr(record, 4);
        const std::string_view table = Table(data, U32(data, record + 8), U32(data, record + 12));
        if (tag == "cmap") cmap = table;
        else if (tag == "post") out.post = table;
        else if (tag == "maxp") out.glyphCount = U16(table, 4);
        else if (tag == "CFF ") out.cff = table;
        else if (tag == "head") out.head = table;
        else if (tag == "loca") out.loca = table;
        else if (tag == "glyf") out.glyf = table;
    }
    const uint32_t subtables = U16(cmap, 2);
    for (uint32_t i = 0; i < subtables && out.cmaps.size() < 64 && Has(cmap, 4 + static_cast<size_t>(i) * 8, 8); ++i) {
        const size_t record = 4 + static_cast<size_t>(i) * 8;
        const uint32_t offset = U32(cmap, record + 4);
        if (!Has(cmap, offset, 4)) continue;
        CmapSubtable sub;
        sub.platform = static_cast<uint16_t>(U16(cmap, record));
        sub.encoding = static_cast<uint16_t>(U16(cmap, record + 2));
        sub.format = static_cast<uint16_t>(U16(cmap, offset));
        uint32_t length = 0;
        if (sub.format == 0 || sub.format == 2 || sub.format == 4 || sub.format == 6) length = U16(cmap, offset + 2);
        else if (sub.format == 8 || sub.format == 10 || sub.format == 12 || sub.format == 13) length = U32(cmap, offset + 4);
        else continue;
        sub.data = Table(cmap, offset, length);
        out.cmaps.push_back(sub);
    }
    return true;
}

// Format 4: the glyph of `code` in segment `s` of a subtable of `segX2` (> 0) bytes of end codes
// and room for its arrays, `code` not below the segment's start `start`.
uint32_t Format4SegmentGlyph(std::string_view d, uint32_t segX2, uint32_t s, uint32_t start, uint32_t code) {
    const size_t rangeOffsets = 16 + 3 * static_cast<size_t>(segX2);
    const uint32_t delta = U16(d, 16 + 2 * static_cast<size_t>(segX2) + s * 2);
    const uint32_t rangeOffset = U16(d, rangeOffsets + s * 2);
    if (rangeOffset == 0) return (code + delta) & 0xFFFF;
    const size_t at = rangeOffsets + s * 2 + rangeOffset + 2 * static_cast<size_t>(code - start);
    if (!Has(d, at, 2)) return 0;
    const uint32_t glyph = U16(d, at);
    return glyph == 0 ? 0 : (glyph + delta) & 0xFFFF;
}

// Format 4: the segment holding `code` and the glyph it gives.
uint32_t Format4Glyph(std::string_view d, uint32_t code) {
    if (code > 0xFFFF) return 0;
    const uint32_t segX2 = U16(d, 6);
    if (segX2 == 0 || !Has(d, 16 + 3 * static_cast<size_t>(segX2), segX2)) return 0;
    const uint32_t segments = segX2 / 2;
    // Segments are sorted by end code: the first whose end is not below `code`.
    uint32_t low = 0, high = segments;
    while (low < high) {
        const uint32_t middle = (low + high) / 2;
        if (U16(d, 14 + middle * 2) < code) low = middle + 1;
        else high = middle;
    }
    if (low >= segments) return 0;
    const uint32_t start = U16(d, 16 + static_cast<size_t>(segX2) + low * 2);
    if (code < start) return 0;
    return Format4SegmentGlyph(d, segX2, low, start, code);
}

uint32_t Format12Glyph(std::string_view d, uint32_t code, bool constant) {
    const uint32_t groups = U32(d, 12);
    if (!Has(d, 16, static_cast<size_t>(groups) * 12)) return 0;
    uint32_t low = 0, high = groups;
    while (low < high) {
        const uint32_t middle = (low + high) / 2;
        if (U32(d, 16 + static_cast<size_t>(middle) * 12 + 4) < code) low = middle + 1;
        else high = middle;
    }
    if (low >= groups) return 0;
    const size_t g = 16 + static_cast<size_t>(low) * 12;
    const uint32_t start = U32(d, g);
    if (code < start) return 0;
    return constant ? U32(d, g + 8) : U32(d, g + 8) + (code - start);
}

} // namespace

uint32_t CmapSubtable::Glyph(uint32_t code) const {
    switch (format) {
    case 0:
        return code < 256 && Has(data, 6 + code, 1) ? static_cast<unsigned char>(data[6 + code]) : 0;
    case 4:
        return Format4Glyph(data, code);
    case 6: {
        const uint32_t first = U16(data, 6);
        const uint32_t count = U16(data, 8);
        if (code < first || code - first >= count) return 0;
        return U16(data, 10 + 2 * static_cast<size_t>(code - first));
    }
    case 12:
    case 13:
        return Format12Glyph(data, code, format == 13);
    default:
        return 0;
    }
}

void ForEachMapping(const CmapSubtable& table, uint32_t& budget, void (*visit)(void*, uint32_t, uint32_t), void* context) {
    const std::string_view d = table.data;
    const auto emit = [&](uint32_t code, uint32_t glyph) {
        --budget;
        if (glyph != 0) visit(context, code, glyph);
    };
    switch (table.format) {
    case 0:
        for (uint32_t c = 0; c < 256 && budget > 0; ++c) emit(c, table.Glyph(c));
        break;
    case 4: {
        const uint32_t segX2 = U16(d, 6);
        if (segX2 == 0 || !Has(d, 16 + 3 * static_cast<size_t>(segX2), segX2)) break;
        const uint32_t segments = segX2 / 2;
        // With the end codes in order, Format4Glyph's search gives segment s for each of its codes
        // above the end of segment s - 1: those are read from s without the search.
        bool sorted = true;
        for (uint32_t s = 1; s < segments && sorted; ++s) sorted = U16(d, 14 + (s - 1) * 2) <= U16(d, 14 + s * 2);
        for (uint32_t s = 0; s < segments && budget > 0; ++s) {
            const uint32_t start = U16(d, 16 + static_cast<size_t>(segX2) + s * 2);
            const uint32_t end = U16(d, 14 + s * 2);
            const uint32_t direct = !sorted ? 0x10000 : s == 0 ? 0 : U16(d, 14 + (s - 1) * 2) + 1;
            for (uint32_t c = start; c <= end && c != 0xFFFF && budget > 0; ++c) {
                if (c >= direct) {
                    emit(c, Format4SegmentGlyph(d, segX2, s, start, c));
                    continue;
                }
                if (budget < kSearchedCodeCost) {
                    budget = 0;
                    break;
                }
                budget -= kSearchedCodeCost - 1;
                emit(c, Format4Glyph(d, c));
            }
        }
        break;
    }
    case 6: {
        const uint32_t first = U16(d, 6);
        const uint32_t count = U16(d, 8);
        for (uint32_t i = 0; i < count && budget > 0 && Has(d, 10 + 2 * static_cast<size_t>(i), 2); ++i) {
            emit(first + i, U16(d, 10 + 2 * static_cast<size_t>(i)));
        }
        break;
    }
    case 12:
    case 13: {
        const uint32_t groups = U32(d, 12);
        for (uint32_t g = 0; g < groups && budget > 0 && Has(d, 16 + static_cast<size_t>(g) * 12, 12); ++g) {
            const size_t at = 16 + static_cast<size_t>(g) * 12;
            const uint32_t start = U32(d, at), end = U32(d, at + 4), glyph = U32(d, at + 8);
            for (uint32_t c = start; c <= end && c <= 0x10FFFF && budget > 0; ++c) {
                emit(c, table.format == 13 ? glyph : glyph + (c - start));
            }
        }
        break;
    }
    default:
        break;
    }
}

const CmapSubtable* Sfnt::Cmap(uint16_t platform, uint16_t encoding) const {
    for (const CmapSubtable& sub : cmaps) {
        if (sub.platform == platform && sub.encoding == encoding) return &sub;
    }
    return nullptr;
}

bool ReadSfnt(std::string_view data, Sfnt& out) {
    out = Sfnt();
    return ReadDirectory(data, 0, out, 0);
}

bool ReadPostNames(std::string_view post, uint32_t glyphCount, Names& out) {
    out = Names();
    const uint32_t version = U32(post, 0);
    if (version == 0x00010000u) {
        for (uint32_t g = 0; g < 258 && g < glyphCount; ++g) out.Push(Tables::GlyphName(Tables::kMacGlyphNames[g]));
        return !out.ends.empty();
    }
    if (version != 0x00020000u) return false;
    const uint32_t count = U16(post, 32);
    if (!Has(post, 34, static_cast<size_t>(count) * 2)) return false;
    // The custom names, Pascal strings after the index array.
    std::vector<std::string_view> custom;
    size_t at = 34 + static_cast<size_t>(count) * 2;
    while (at < post.size() && custom.size() < 65536) {
        const size_t length = static_cast<unsigned char>(post[at]);
        if (post.size() - at - 1 < length) break;
        custom.push_back(post.substr(at + 1, length));
        at += 1 + length;
    }
    out.ends.reserve(count);
    for (uint32_t g = 0; g < count; ++g) {
        const uint32_t index = U16(post, 34 + static_cast<size_t>(g) * 2);
        if (index < 258) out.Push(Tables::GlyphName(Tables::kMacGlyphNames[index]));
        else out.Push(index - 258 < custom.size() ? custom[index - 258] : std::string_view());
    }
    return !out.ends.empty();
}

void InvertUnicodeCmap(const Sfnt& sfnt, uint32_t glyphCount, std::vector<uint32_t>& unicodeOfGlyph) {
    unicodeOfGlyph.assign(glyphCount, 0);
    // Glyph 0 is never mapped (ForEach skips it): the glyphs left to find.
    uint32_t left = glyphCount > 1 ? glyphCount - 1 : 0;
    if (left == 0) return;
    const CmapSubtable* order[8];
    size_t n = 0;
    if (const CmapSubtable* t = sfnt.Cmap(3, 10)) order[n++] = t;
    if (const CmapSubtable* t = sfnt.Cmap(3, 1)) order[n++] = t;
    for (const CmapSubtable& sub : sfnt.cmaps) {
        if (sub.platform == 0 && n < 8) order[n++] = &sub;
    }
    uint32_t budget = kMaxInvertedCodes;
    // Once every glyph has its code, nothing read later changes one: the walk stops.
    auto visit = [&](uint32_t code, uint32_t glyph) {
        if (glyph < glyphCount && unicodeOfGlyph[glyph] == 0 && code >= 0x20) {
            unicodeOfGlyph[glyph] = code;
            if (--left == 0) budget = 0;
        }
    };
    for (size_t i = 0; i < n && budget > 0; ++i) {
        // A subtable under several encodings (one offset) maps nothing new after its first walk.
        bool seen = false;
        for (size_t j = 0; j < i && !seen; ++j) {
            seen = order[j]->format == order[i]->format && order[j]->data.data() == order[i]->data.data() &&
                   order[j]->data.size() == order[i]->data.size();
        }
        if (!seen) order[i]->ForEach(visit, budget);
    }
}

} // namespace RayoPdf::Text::Program
