// SPDX-License-Identifier: Apache-2.0
// CMaps (ISO 32000-1 sections 9.7.5 and 9.10.3, Adobe TN #5014 and #5411, algorithms §3.2 and
// §3.4): the codespace that splits a shown string into codes, ToUnicode CMaps (code -> UTF-16BE
// text) and the embedded CMaps of Type0 /Encoding streams (code -> CID). Parsing is one linear
// pass over untrusted bytes; what cannot be read is skipped, never fatal.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Text {

// Bounds of a parsed CMap (algorithms §3.2): at most kMaxMappedCodes codes in all, at most
// kMaxRangeCodes codes from one range (the rest of a longer range is dropped), at most
// kMaxCodespaceRanges codespace ranges. A map that reaches a limit keeps what it read.
constexpr uint32_t kMaxMappedCodes = 1u << 20;
constexpr uint32_t kMaxRangeCodes = 1u << 16;
constexpr size_t kMaxCodespaceRanges = 256;

// A codespace range of 1-4 bytes: a code of `bytes` bytes is in it when every byte lies within
// the range's bounds for that byte (§9.7.6.2).
struct CodespaceRange {
    uint8_t bytes = 0;
    uint8_t low[4] = {};
    uint8_t high[4] = {};
};

struct Codespace {
    std::vector<CodespaceRange> ranges;

    bool Empty() const { return ranges.empty(); }
    // Adds the range from `low` to `high` (big-endian byte strings of the same 1-4 bytes); false
    // when they are unusable or the codespace is full.
    bool Add(std::string_view low, std::string_view high);
    // The code at `at` (< bytes.size()) and its length. The first length whose bytes lie in a
    // range wins. A code in no range takes the length of the shortest range whose first byte
    // matches, else of the shortest range (§9.7.6.3); the end of the string cuts it short. An
    // empty codespace reads 2-byte codes.
    uint8_t Read(std::string_view bytes, size_t at, uint32_t& code) const;
};

// code -> UTF-8 text, sorted by code, one entry per code (a later definition wins).
struct UnicodeEntry {
    uint32_t code = 0;
    uint32_t offset = 0;  // into ToUnicodeMap::text
    uint32_t length = 0;
};

// Entries a one-unit bfrange of kMinRunCodes codes or more added: entry `first` + i has the text of
// UTF-16 unit `unit` + i (modulo 2^16; a surrogate unit as U+FFFD), so the code point of its text is
// known unread. (Shorter ranges would not pay for the list.)
constexpr size_t kMinRunCodes = 8;
struct UnicodeRun {
    uint32_t first = 0;
    uint32_t count = 0;
    uint16_t unit = 0;
};

struct ToUnicodeMap {
    Codespace codespace;
    std::vector<UnicodeEntry> entries;
    std::string text;
    // The runs among the entries as ParseToUnicode read them, in order; empty once it has sorted
    // them (codes out of order or defined twice). LoadToUnicode reads and clears them.
    std::vector<UnicodeRun> runs;
    bool truncated = false;  // a limit was reached

    const UnicodeEntry* Find(uint32_t code) const;
    std::string_view Text(const UnicodeEntry& entry) const { return std::string_view(text).substr(entry.offset, entry.length); }
};

// Reads a ToUnicode CMap: codespacerange, bfchar and bfrange (string and array destinations,
// UTF-16BE with surrogate pairs, several code points per code), also glyph-name destinations and
// cidchar/cidrange in mislabelled streams (a CID read as a code point). A bfrange string
// destination counts up in its last UTF-16 unit, carrying into the unit's high byte as pdf.js does
// (the "low-byte overflow" of ranges like <00FF> + 1 -> U+0100). usecmap is ignored. False when
// nothing was mapped.
bool ParseToUnicode(std::string_view data, ToUnicodeMap& out);

struct CidRange {
    uint32_t low = 0;
    uint32_t high = 0;
    uint32_t cid = 0;  // the CID of `low`
};

// An embedded CMap of a Type0 /Encoding stream.
struct EncodingCMap {
    Codespace codespace;
    std::vector<CidRange> cids;      // cidchar and cidrange, sorted by low
    std::vector<CidRange> notdefs;   // notdefrange, sorted by low
    int wmode = 0;
    std::string useCMap;             // the usecmap name, if any
    bool truncated = false;

    // The CID of `code`: its cidrange, else its notdefrange, else 0. False for a code in neither.
    bool Cid(uint32_t code, uint32_t& cid) const;
};

// Reads an embedded CMap: codespacerange, cidchar, cidrange, notdefrange, /WMode, usecmap.
// bfchar/bfrange in a mislabelled stream map to the destination string's value as a CID. False
// when it holds neither a codespace nor a mapping.
bool ParseEncodingCMap(std::string_view data, EncodingCMap& out);

} // namespace RayoPdf::Text
