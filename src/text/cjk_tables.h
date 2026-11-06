// SPDX-License-Identifier: Apache-2.0
// The generated CJK tables (cjk_data.cpp, written by tools/gen_cjk_tables.py from Adobe's
// cmap-resources, BSD-3-Clause, see NOTICE); cjk.cpp searches them.
#pragma once

#include <cstdint>

namespace RayoPdf::Text::Cjk {

constexpr int kCollectionCount = 5;

// One Adobe character collection. Its CIDs 0..cidCount-1 index kCidUnicode from tableStart: 0 is
// no Unicode, 0xD800 + i is kSupplementary[supplementaryStart + i], anything else a BMP code
// point. The collection's Uni*-UTF32-H CMap (Unicode -> CID) is the inverse of that table over the
// CIDs whose bit is set in kInverseBits (from byte inverseBitsStart, bit cid & 7 of byte cid >> 3),
// plus the kExtraCodes/kExtraCids pairs (sorted by code point).
struct CollectionTable {
    const char* ordering;
    uint16_t cidCount;
    uint32_t tableStart;
    uint16_t supplementaryStart;
    uint16_t supplementaryCount;
    uint16_t extrasStart;
    uint16_t extrasCount;
    uint32_t inverseBitsStart;
};

// Codes low..low+count-1 map to CIDs cid..cid+count-1; a run whose cid is 0xFFFF lists codes the
// CMap does not map although its base does. Unicode-based CMaps list code points.
struct CidRun {
    uint32_t low;
    uint16_t count;
    uint16_t cid;
};

// A codespace range of `bytes` bytes; `low` and `high` are big-endian byte strings as numbers.
struct CodespaceSpan {
    uint8_t bytes;
    uint32_t low;
    uint32_t high;
};

// One predefined CMap. Its runs (kRuns from runStart, sorted by low) are searched first, then the
// CMap `base` (an index into kPredefined, -1 for none) and its bases, and for Unicode-based CMaps
// finally the collection's UTF32-H. Codespace, WMode and notdefrange are the CMap's own (resolved
// over usecmap by the generator).
struct PredefinedTable {
    const char* name;
    uint8_t collection;   // 1-based index into kCollections
    uint8_t encoding;     // cjk.h CMapEncoding
    uint8_t wmode;
    int16_t base;
    uint16_t runStart;
    uint16_t runCount;
    uint16_t codespaceStart;
    uint16_t codespaceCount;
    uint8_t hasNotdef;    // one notdefrange: codes notdefLow..notdefHigh -> notdefCid
    uint32_t notdefLow;
    uint32_t notdefHigh;
    uint16_t notdefCid;
};

extern const uint16_t kCidUnicode[];
extern const uint32_t kSupplementary[];
extern const uint32_t kExtraCodes[];
extern const uint16_t kExtraCids[];
extern const uint8_t kInverseBits[];
extern const CollectionTable kCollections[kCollectionCount];
extern const CidRun kRuns[];
extern const CodespaceSpan kCodespaces[];
extern const uint16_t kPredefinedCount;
extern const PredefinedTable kPredefined[];

} // namespace RayoPdf::Text::Cjk
