// SPDX-License-Identifier: Apache-2.0
// UTF-8 encoding and decoding of single code points.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::Utf8 {

constexpr uint32_t kReplacement = 0xFFFD;

// Appends `codePoint` as UTF-8; a surrogate or a value past U+10FFFF appends U+FFFD.
inline void Append(std::string& out, uint32_t codePoint) {
    if ((codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) codePoint = kReplacement;
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// The code point at `at` and its length in bytes. A malformed sequence (overlong, surrogate,
// truncated, past U+10FFFF) reads as U+FFFD of length 1. False at the end.
inline bool Decode(std::string_view text, size_t at, uint32_t& codePoint, size_t& length) {
    if (at >= text.size()) return false;
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[at + i]); };
    const unsigned char b0 = byte(0);
    codePoint = kReplacement;
    length = 1;
    if (b0 < 0x80) {
        codePoint = b0;
        return true;
    }
    size_t need = 0;
    uint32_t value = 0;
    uint32_t minimum = 0;
    if ((b0 & 0xE0) == 0xC0) {
        need = 1, value = b0 & 0x1F, minimum = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2, value = b0 & 0x0F, minimum = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3, value = b0 & 0x07, minimum = 0x10000;
    } else {
        return true;
    }
    if (at + need >= text.size()) return true;
    for (size_t i = 1; i <= need; ++i) {
        if ((byte(i) & 0xC0) != 0x80) return true;
        value = (value << 6) | (byte(i) & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return true;
    codePoint = value;
    length = need + 1;
    return true;
}

} // namespace RayoPdf::Utf8
