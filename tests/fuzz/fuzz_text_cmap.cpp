// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the CMap parsers (src/text/cmap.h): the input as a ToUnicode CMap and as an
// embedded Type0 encoding CMap, then its own bytes split into codes by both codespaces and looked
// up in both maps.
#include "text/cmap.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace RayoPdf::Text;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    ToUnicodeMap unicode;
    ParseToUnicode(input, unicode);
    EncodingCMap encoding;
    ParseEncodingCMap(input, encoding);
    const std::string_view sample = input.substr(0, 256);
    for (const Codespace* codespace : {&unicode.codespace, &encoding.codespace}) {
        size_t at = 0;
        while (at < sample.size()) {
            uint32_t code = 0;
            const uint8_t length = codespace->Read(sample, at, code);
            if (length == 0 || length > 4) __builtin_trap();
            at += length;
            if (const UnicodeEntry* entry = unicode.Find(code)) {
                if (unicode.Text(*entry).size() != entry->length) __builtin_trap();
            }
            uint32_t cid = 0;
            encoding.Cid(code, cid);
        }
    }
    return 0;
}
