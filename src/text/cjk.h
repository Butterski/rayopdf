// SPDX-License-Identifier: Apache-2.0
// The Adobe CJK character collections and predefined CMaps (ISO 32000-1 section 9.7.5.2,
// algorithms §3.4) over the tables tools/gen_cjk_tables.py generates from Adobe's cmap-resources
// and the Unicode IVD: CID -> Unicode for Adobe-Japan1, -GB1, -CNS1, -Korea1 and -KR, and code ->
// CID for every predefined CMap of cmap-resources (JIS, Shift-JIS, EUC, GBK, GB 18030, Big Five and
// its Hong Kong sets, CNS, UHC, Johab, and the Unicode-based Uni*-UCS2/-UTF16/-UTF32/-UTF8 ones).
// Everything is a binary search over constant data except the Unicode -> CID inverse of a
// collection, built once per process on first use.
#pragma once

#include "text/cmap.h"

#include <cstdint>
#include <string_view>

namespace RayoPdf::Text::Cjk {

enum class Collection : uint8_t { None, Japan1, GB1, CNS1, Korea1, KR };

// How a predefined CMap's codes are read: legacy multi-byte codes, or Unicode in one of four forms.
enum class CMapEncoding : uint8_t { Legacy, Ucs2, Utf16, Utf32, Utf8 };

// The collection a /CIDSystemInfo names: /Registry (Adobe) and /Ordering (Japan1, GB1, CNS1, Korea1, KR).
Collection CollectionByName(std::string_view registry, std::string_view ordering);

// The Unicode code point of `cid` in `collection`, 0 when it has none.
uint32_t CidToUnicode(Collection collection, uint32_t cid);
// The same as UTF-8, valid for the life of the process; empty when the CID has none.
std::string_view CidText(Collection collection, uint32_t cid);

// The index of the predefined CMap `name` ("90ms-RKSJ-H", "UniGB-UCS2-H", ...), -1 when the tables
// do not hold it. Identity-H/V are not in the tables.
int FindPredefined(std::string_view name);
Collection CollectionOf(int predefined);
CMapEncoding EncodingOf(int predefined);
bool IsVertical(int predefined);
// Adds the CMap's codespace ranges to `codespace` (legacy CMaps; the Unicode-based ones have the
// fixed codespaces of their encoding form).
void AddCodespace(int predefined, Codespace& codespace);
// The CID of `key`: the code for a legacy CMap, the code point for a Unicode-based one. A code in
// no mapping but in the CMap's notdefrange gets its notdef CID. False when neither applies.
bool Cid(int predefined, uint32_t key, uint32_t& cid);

} // namespace RayoPdf::Text::Cjk
