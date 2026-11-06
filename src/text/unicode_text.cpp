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

} // namespace RayoPdf::Text
