// SPDX-License-Identifier: Apache-2.0
// Lookups over the generated tables and the Adobe glyph-name rules (AGL specification "Mapping
// glyph names to Unicode", algorithms §3.3).

#include "text/tables.h"

#include "common/utf8.h"
#include "text/font.h"

#include <cstring>

namespace RayoPdf::Text {
namespace Tables {

uint16_t FindGlyphName(std::string_view name) {
    size_t low = 0;
    size_t high = kGlyphNameCount;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        const int order = GlyphName(static_cast<uint16_t>(middle)).compare(name);
        if (order == 0) return static_cast<uint16_t>(middle);
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return kNoName;
}

size_t AglCodePoints(uint16_t index, uint32_t out[4]) {
    if (index >= kGlyphNameCount) return 0;
    const uint16_t value = kAglValues[index];
    if (value == 0) return 0;
    if (value < kAglMultiBase || value - kAglMultiBase >= kAglMultiCount) {
        out[0] = value;
        return 1;
    }
    const size_t multi = value - kAglMultiBase;
    size_t count = 0;
    for (uint16_t i = kAglMultiStarts[multi]; i < kAglMultiStarts[multi + 1] && count < 4; ++i) {
        out[count++] = kAglMultiCodePoints[i];
    }
    return count;
}

uint32_t ZapfCodePoint(uint16_t index) {
    size_t low = 0;
    size_t high = kZapfCount;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (kZapfNames[middle] == index) return kZapfValues[middle];
        if (kZapfNames[middle] < index) low = middle + 1;
        else high = middle;
    }
    return 0;
}

bool StandardWidth(int font, uint16_t index, uint16_t& width) {
    if (font < 0 || font >= 14) return false;
    const StandardFont& f = kStandardFonts[font];
    if (f.fixedWidth != 0) {
        width = f.fixedWidth;
        return true;
    }
    size_t low = f.widthStart;
    size_t high = static_cast<size_t>(f.widthStart) + f.widthCount;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (kStandardWidthNames[middle] == index) {
            width = kStandardWidthValues[middle];
            return true;
        }
        if (kStandardWidthNames[middle] < index) low = middle + 1;
        else high = middle;
    }
    return false;
}

int StandardFontByName(std::string_view name) {
    char normal[96];
    size_t length = 0;
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (length == sizeof normal) return -1;
        normal[length++] = c == ',' || c == '_' ? '-' : c;
    }
    const std::string_view key(normal, length);
    for (int i = 0; i < 14; ++i) {
        if (key == kStandardFonts[i].name) return i;
    }
    size_t low = 0;
    size_t high = kStandardAliasCount;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        const int order = std::string_view(kStandardAliases[middle].name).compare(key);
        if (order == 0) return kStandardAliases[middle].font;
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return -1;
}

} // namespace Tables

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool ParseHex(std::string_view digits, uint32_t& value) {
    value = 0;
    for (char c : digits) {
        const int v = HexValue(c);
        if (v < 0) return false;
        value = value << 4 | static_cast<uint32_t>(v);
    }
    return !digits.empty();
}

bool IsScalar(uint32_t cp) {
    return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

// One component of a glyph name (no '.' and no '_'), appended to `out`.
bool ComponentToUnicode(std::string_view name, bool zapf, std::string& out) {
    if (name.empty()) return false;
    const uint16_t index = Tables::FindGlyphName(name);
    if (index != Tables::kNoName) {
        if (zapf) {
            const uint32_t cp = Tables::ZapfCodePoint(index);
            if (cp != 0) {
                Utf8::Append(out, cp);
                return true;
            }
        }
        uint32_t cps[4];
        const size_t count = Tables::AglCodePoints(index, cps);
        for (size_t i = 0; i < count; ++i) Utf8::Append(out, cps[i]);
        if (count > 0) return true;
    }
    // uniXXXX[XXXX...]: groups of four hex digits, each a BMP scalar value. The AGL asks for
    // uppercase digits; producers write lowercase too, which pdf.js and PDFBox accept.
    if (name.size() >= 7 && name.substr(0, 3) == "uni" && (name.size() - 3) % 4 == 0) {
        const size_t before = out.size();
        bool ok = true;
        for (size_t at = 3; at < name.size() && ok; at += 4) {
            uint32_t cp = 0;
            ok = ParseHex(name.substr(at, 4), cp) && IsScalar(cp);
            if (ok) Utf8::Append(out, cp);
        }
        if (ok) return true;
        out.resize(before);
    }
    // uXXXX to uXXXXXX: one scalar value.
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u') {
        uint32_t cp = 0;
        if (ParseHex(name.substr(1), cp) && IsScalar(cp)) {
            Utf8::Append(out, cp);
            return true;
        }
    }
    return false;
}

} // namespace

bool GlyphNameToUnicode(std::string_view name, bool zapfDingbats, std::string& out) {
    const size_t dot = name.find('.');
    if (dot != std::string_view::npos) name = name.substr(0, dot);
    if (name.empty() || name.size() > 256) return false;
    bool any = false;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('_', start);
        if (end == std::string_view::npos) end = name.size();
        if (ComponentToUnicode(name.substr(start, end - start), zapfDingbats, out)) any = true;
        start = end + 1;
    }
    return any;
}

bool GlyphNameToUnicode(std::string_view name, std::string& out) {
    return GlyphNameToUnicode(name, false, out);
}

} // namespace RayoPdf::Text
