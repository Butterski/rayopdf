// SPDX-License-Identifier: Apache-2.0

#include "text/unicode_text.h"

#include "common/utf8.h"

#include <cstring>
#include <string>

namespace RayoPdf::Text {

std::string_view BmpText(uint32_t cp) {
    // Three bytes per code point (the longest BMP UTF-8 form) and each one's length.
    struct Table {
        char bytes[0x10000 * 3];
        uint8_t lengths[0x10000];
        Table() {
            std::string one;
            for (uint32_t c = 0; c < 0x10000; ++c) {
                one.clear();
                Utf8::Append(one, c);
                std::memcpy(bytes + c * 3, one.data(), one.size());
                lengths[c] = static_cast<uint8_t>(one.size());
            }
        }
    };
    static const Table* const table = new Table();
    if (cp >= 0x10000 || (cp >= 0xD800 && cp <= 0xDFFF)) return {};
    return std::string_view(table->bytes + cp * 3, table->lengths[cp]);
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
