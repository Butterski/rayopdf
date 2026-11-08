// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the rayomd-source/1 reader (pdf/rayomd_source.h): any bytes as a whole PDF.
// What it accepts must hold up: the payload lies inside the input, has the length the packet
// declares and is UTF-8. Seed it with PDFs from `rayomd --export ... --embed-source`.
#include "pdf/rayomd_source.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf;
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    RayomdSource::Result result;
    RayomdSource::Read(bytes, result);
    if (result.status == RayomdSource::Status::Ok) {
        const bool inside = result.source.data() >= bytes.data() &&
                            result.source.data() + result.source.size() <= bytes.data() + bytes.size();
        if (!inside || result.source.size() != result.info.sourceBytes || !RayomdSource::IsValidUtf8(result.source)) {
            __builtin_trap();
        }
    } else if (!result.source.empty()) {
        __builtin_trap();
    }
    return 0;
}
