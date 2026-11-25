// SPDX-License-Identifier: Apache-2.0

#include "text/unicode_text.h"

namespace RayoPdf::Text {
namespace {

// The UTF-8 of every BMP code point, three bytes apart (the longest BMP form), made by the
// compiler: a constant table costs no time at start-up and is paged in from the binary only where
// read (built at run time it cost 6.5 M instructions and 64 page faults in every process reading
// CJK text through a character collection).
struct BmpTable {
    char bytes[0x10000 * 3];
};

constexpr BmpTable MakeBmpTable() {
    BmpTable table{};
    for (uint32_t cp = 0; cp < 0x10000; ++cp) {
        const uint32_t at = cp * 3;
        if (cp < 0x80) {
            table.bytes[at] = static_cast<char>(cp);
        } else if (cp < 0x800) {
            table.bytes[at] = static_cast<char>(0xC0 | (cp >> 6));
            table.bytes[at + 1] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            table.bytes[at] = static_cast<char>(0xE0 | (cp >> 12));
            table.bytes[at + 1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            table.bytes[at + 2] = static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return table;
}

constexpr BmpTable kBmp = MakeBmpTable();

} // namespace

std::string_view BmpText(uint32_t cp) {
    if (cp >= 0x10000 || (cp >= 0xD800 && cp <= 0xDFFF)) return {};
    return std::string_view(kBmp.bytes + cp * 3, cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3);
}

namespace {

// UTF-8 of every code point of supplementary plane `plane`, four bytes each.
struct PlaneTable {
    char bytes[0x10000 * 4];
    explicit PlaneTable(uint32_t plane) {
        for (uint32_t low = 0; low < 0x10000; ++low) {
            const uint32_t cp = plane << 16 | low;
            char* out = bytes + low * 4;
            out[0] = static_cast<char>(0xF0 | (cp >> 18));
            out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
};

} // namespace

std::string_view CodePointText(uint32_t cp) {
    if (cp < 0x10000) return BmpText(cp);
    const uint32_t plane = cp >> 16;
    const PlaneTable* table = nullptr;
    if (plane == 1) {
        static const PlaneTable* const one = new PlaneTable(1);
        table = one;
    } else if (plane == 2) {
        static const PlaneTable* const two = new PlaneTable(2);
        table = two;
    } else {
        return {};
    }
    return std::string_view(table->bytes + (cp & 0xFFFF) * 4, 4);
}

} // namespace RayoPdf::Text
