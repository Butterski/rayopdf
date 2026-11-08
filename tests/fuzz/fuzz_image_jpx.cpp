// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the JPEG 2000 decoder (src/image/jpx.h): the input is a JP2 file or a raw
// codestream, read for its header and then decoded for a PDF, at full and at reduced resolution,
// and as plain components, with a pixel limit that keeps one input fast. The first byte of the
// input picks the options (palette ignored, SMaskInData, the PDF's component count).
#include "image/jpx.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf::Image;
    if (size < 1) return 0;
    const uint8_t selector = data[0];
    data++;
    size--;
    Jpx::Info info;
    // A reduced decode still parses every resolution: keep the full image small for speed.
    if (Jpx::ReadInfo(data, size, info) == Jpx::Result::Ok &&
        uint64_t(info.width) * info.height * uint64_t(info.components) > (uint64_t(1) << 22)) {
        return 0;
    }
    Jpx::Options options;
    options.maxPixels = uint64_t(1) << 18;
    options.ignorePalette = (selector & 1) != 0;
    options.smaskInData = (selector >> 1) & 3;
    options.pdfComponents = (selector >> 3) & 7;
    options.reduce = selector >> 6;
    Jpx::Image image;
    if (Jpx::Decode(data, size, options, image) == Jpx::Result::Ok &&
        image.samples.size() != size_t(image.width) * image.height * size_t(image.channels) *
                                    size_t(image.bitsPerComponent / 8)) {
        __builtin_trap();  // the samples must be exactly the image's
    }
    std::vector<Jpx::Component> components;
    if (Jpx::DecodeComponents(data, size, options, info, components) == Jpx::Result::Ok) {
        for (const Jpx::Component& c : components) {
            if (c.samples.size() != size_t(c.width) * c.height) __builtin_trap();
            for (int32_t v : c.samples) {
                const int32_t low = c.isSigned ? -(int32_t(1) << (c.precision - 1)) : 0;
                const int32_t high = c.isSigned ? (int32_t(1) << (c.precision - 1)) - 1 : (int32_t(1) << c.precision) - 1;
                if (v < low || v > high) __builtin_trap();  // clipped to the precision
            }
        }
    }
    return 0;
}
