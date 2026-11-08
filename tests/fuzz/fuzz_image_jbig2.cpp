// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the JBIG2 decoder (src/image/jbig2.h): the first two bytes of the input give
// the length of the /JBIG2Globals part (little endian, cut to what is there), the rest is the
// embedded stream. A pixel limit of 2^18 keeps one input fast; every budget scales with it.
#include "image/jbig2.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf::Image;
    if (size < 2) return 0;
    size_t globalsSize = size_t(data[0]) | size_t(data[1]) << 8;
    data += 2;
    size -= 2;
    if (globalsSize > size) globalsSize = size;
    Jbig2::Info info;
    std::string rows;
    const Jbig2::Result result =
        Jbig2::Decode(data + globalsSize, size - globalsSize, data, globalsSize, uint64_t(1) << 18, info, rows);
    if (result == Jbig2::Result::Ok) {
        // The rows must be exactly the page's.
        if (rows.size() != (size_t(info.width) + 7) / 8 * info.height) __builtin_trap();
    } else if (!rows.empty()) {
        __builtin_trap();
    }
    return 0;
}
