// SPDX-License-Identifier: Apache-2.0
// Lookups over the generated CJK tables (cjk_tables.h). A predefined CMap is a chain: its own runs,
// then its base CMap's (a -V CMap's -H), and for Unicode-based CMaps finally the collection's
// UTF32-H, rebuilt here once as a sorted Unicode -> CID array from the CID -> Unicode table.

#include "text/cjk.h"

#include "common/utf8.h"
#include "text/cjk_tables.h"
#include "text/unicode_text.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace RayoPdf::Text::Cjk {
namespace {

// A base chain is at most -V -> -H -> (UCS2-H); the bound only guards against a bad table.
constexpr int kMaxChain = 4;

const CollectionTable* TableOf(Collection collection) {
    const int index = static_cast<int>(collection) - 1;
    return index >= 0 && index < kCollectionCount ? &kCollections[index] : nullptr;
}

// The code point a kCidUnicode value stands for.
uint32_t Decode(const CollectionTable& table, uint16_t value) {
    if (value >= 0xD800 && value <= 0xDFFF) {
        const uint32_t index = value - 0xD800u;
        return index < table.supplementaryCount ? kSupplementary[table.supplementaryStart + index] : 0;
    }
    return value;
}

// UTF32-H as (code point << 16 | CID), sorted.
std::vector<uint64_t> BuildInverse(const CollectionTable& table) {
    std::vector<uint64_t> out;
    out.reserve(table.cidCount + table.extrasCount);
    const uint8_t* bits = kInverseBits + table.inverseBitsStart;
    for (uint32_t cid = 0; cid < table.cidCount; ++cid) {
        if (!(bits[cid >> 3] & (1u << (cid & 7)))) continue;
        const uint32_t cp = Decode(table, kCidUnicode[table.tableStart + cid]);
        if (cp == 0) continue;
        out.push_back(static_cast<uint64_t>(cp) << 16 | cid);
    }
    for (uint32_t i = 0; i < table.extrasCount; ++i) {
        const uint32_t at = table.extrasStart + i;
        out.push_back(static_cast<uint64_t>(kExtraCodes[at]) << 16 | kExtraCids[at]);
    }
    std::sort(out.begin(), out.end());
    return out;
}

const std::vector<uint64_t>& InverseOf(Collection collection) {
    // One function-local static per collection: thread-safe, built only when used.
    switch (collection) {
    case Collection::Japan1: {
        static const std::vector<uint64_t> inverse = BuildInverse(kCollections[0]);
        return inverse;
    }
    case Collection::GB1: {
        static const std::vector<uint64_t> inverse = BuildInverse(kCollections[1]);
        return inverse;
    }
    case Collection::CNS1: {
        static const std::vector<uint64_t> inverse = BuildInverse(kCollections[2]);
        return inverse;
    }
    case Collection::Korea1: {
        static const std::vector<uint64_t> inverse = BuildInverse(kCollections[3]);
        return inverse;
    }
    default: {
        static const std::vector<uint64_t> empty;
        return empty;
    }
    }
}

// The UTF-8 of every supplementary entry, four bytes apart (built once on first use).
std::string_view SupplementaryText(uint32_t index) {
    struct Table {
        std::vector<char> bytes;
        std::vector<uint8_t> lengths;
        Table() {
            uint32_t count = 0;
            for (const CollectionTable& c : kCollections) {
                const uint32_t end = static_cast<uint32_t>(c.supplementaryStart) + c.supplementaryCount;
                count = end > count ? end : count;
            }
            bytes.assign(static_cast<size_t>(count) * 4, 0);
            lengths.assign(count, 0);
            std::string one;
            for (uint32_t i = 0; i < count; ++i) {
                one.clear();
                Utf8::Append(one, kSupplementary[i]);
                if (one.size() > 4) continue;
                std::memcpy(bytes.data() + static_cast<size_t>(i) * 4, one.data(), one.size());
                lengths[i] = static_cast<uint8_t>(one.size());
            }
        }
    };
    static const Table table;
    if (index >= table.lengths.size()) return {};
    return std::string_view(table.bytes.data() + static_cast<size_t>(index) * 4, table.lengths[index]);
}

bool ValidPredefined(int predefined) {
    return predefined >= 0 && predefined < kPredefinedCount;
}

bool FindRun(const PredefinedTable& t, uint32_t key, uint32_t& cid) {
    const CidRun* begin = kRuns + t.runStart;
    const CidRun* end = begin + t.runCount;
    const CidRun* it = std::upper_bound(begin, end, key, [](uint32_t k, const CidRun& r) { return k < r.low; });
    if (it == begin) return false;
    --it;
    if (key - it->low >= it->count) return false;
    cid = it->cid + (key - it->low);
    return true;
}

} // namespace

Collection CollectionByName(std::string_view registry, std::string_view ordering) {
    if (registry != "Adobe") return Collection::None;
    for (int i = 0; i < kCollectionCount; ++i) {
        if (ordering == kCollections[i].ordering) return static_cast<Collection>(i + 1);
    }
    return Collection::None;
}

uint32_t CidToUnicode(Collection collection, uint32_t cid) {
    const CollectionTable* table = TableOf(collection);
    if (!table || cid >= table->cidCount) return 0;
    return Decode(*table, kCidUnicode[table->tableStart + cid]);
}

std::string_view CidText(Collection collection, uint32_t cid) {
    const CollectionTable* table = TableOf(collection);
    if (!table || cid >= table->cidCount) return {};
    const uint16_t value = kCidUnicode[table->tableStart + cid];
    if (value == 0) return {};
    if (value >= 0xD800 && value <= 0xDFFF) {
        const uint32_t index = value - 0xD800u;
        if (index >= table->supplementaryCount) return {};
        return SupplementaryText(table->supplementaryStart + index);
    }
    return BmpText(value);
}

int FindPredefined(std::string_view name) {
    for (int i = 0; i < kPredefinedCount; ++i) {
        if (name == kPredefined[i].name) return i;
    }
    return -1;
}

Collection CollectionOf(int predefined) {
    return ValidPredefined(predefined) ? static_cast<Collection>(kPredefined[predefined].collection) : Collection::None;
}

CMapEncoding EncodingOf(int predefined) {
    if (!ValidPredefined(predefined)) return CMapEncoding::Legacy;
    return static_cast<CMapEncoding>(kPredefined[predefined].encoding);
}

bool IsVertical(int predefined) {
    return ValidPredefined(predefined) && kPredefined[predefined].wmode == 1;
}

void AddCodespace(int predefined, Codespace& codespace) {
    for (int depth = 0; depth < kMaxChain && ValidPredefined(predefined); ++depth) {
        const PredefinedTable& t = kPredefined[predefined];
        if (t.codespaceCount > 0) {
            for (uint32_t i = 0; i < t.codespaceCount; ++i) {
                const CodespaceSpan& span = kCodespaces[t.codespaceStart + i];
                char low[4], high[4];
                for (uint8_t b = 0; b < span.bytes && b < 4; ++b) {
                    const int shift = 8 * (span.bytes - 1 - b);
                    low[b] = static_cast<char>(span.low >> shift);
                    high[b] = static_cast<char>(span.high >> shift);
                }
                codespace.Add(std::string_view(low, span.bytes), std::string_view(high, span.bytes));
            }
            return;
        }
        predefined = t.base;
    }
}

bool Cid(int predefined, uint32_t key, uint32_t& cid) {
    if (!ValidPredefined(predefined)) return false;
    const PredefinedTable& first = kPredefined[predefined];
    int at = predefined;
    for (int depth = 0; depth < kMaxChain && ValidPredefined(at); ++depth) {
        if (FindRun(kPredefined[at], key, cid)) return true;
        at = kPredefined[at].base;
    }
    if (first.encoding != static_cast<uint8_t>(CMapEncoding::Legacy) && key <= 0x10FFFF) {
        const std::vector<uint64_t>& inverse = InverseOf(static_cast<Collection>(first.collection));
        const uint64_t probe = static_cast<uint64_t>(key) << 16;
        auto it = std::lower_bound(inverse.begin(), inverse.end(), probe);
        if (it != inverse.end() && (*it >> 16) == key) {
            cid = static_cast<uint32_t>(*it & 0xFFFF);
            return true;
        }
    }
    at = predefined;
    for (int depth = 0; depth < kMaxChain && ValidPredefined(at); ++depth) {
        const PredefinedTable& t = kPredefined[at];
        if (t.hasNotdef && key >= t.notdefLow && key <= t.notdefHigh) {
            cid = t.notdefCid;
            return true;
        }
        at = t.base;
    }
    return false;
}

} // namespace RayoPdf::Text::Cjk
