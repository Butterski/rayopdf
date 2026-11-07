// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the JPEG decoder (src/image/jpeg.h): the input is JPEG data, read for its
// header and then decoded with a pixel limit that keeps one input fast.
#include "image/jpeg.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf::Image;
    Jpeg::Info info;
    Jpeg::ReadInfo(data, size, info);
    std::string samples;
    if (Jpeg::Decode(data, size, uint64_t(1) << 22, info, samples) == Jpeg::Result::Ok &&
        samples.size() != size_t(info.width) * info.height * static_cast<size_t>(info.components)) {
        __builtin_trap();  // the samples must be exactly the frame's
    }
    return 0;
}
