// SPDX-License-Identifier: Apache-2.0
// Images out of a PDF as files (roadmap D6): DCT data is written as it is stored (.jpg), JPX as
// .jp2, and every other image is decoded to pixels and written as an 8-bit PNG (gray, gray +
// alpha, RGB or RGBA). Decoding covers ISO 32000-1 section 8.9: /ColorSpace DeviceGray, DeviceRGB,
// DeviceCMYK, CalGray, CalRGB, Lab, ICCBased (by /N, or its /Alternate), Indexed (any base),
// Separation and DeviceN (through their tint transform where it is a sampled or exponential
// function, else as gray from the tint); /BitsPerComponent 1, 2, 4, 8 and 16; /Decode;
// /ImageMask stencils; /SMask soft masks and /Mask colour-key or stencil masks as alpha.
// JBIG2 and CCITT data cannot be decoded and are reported Unsupported.
//
// Every decode is bounded: at most `maxPixels` pixels per image and the document's stream limits.
#pragma once

#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::Image {

enum class Status : uint8_t {
    Ok,
    Unsupported,  // JBIG2 or CCITT data, or a colour space / function not handled
    Corrupt,      // missing or inconsistent /Width, /Height, /BitsPerComponent, or no usable data
    TooLarge,     // more than `maxPixels` pixels
};

// An image as file bytes.
struct Encoded {
    std::string bytes;
    const char* extension = "png";  // "png", "jpg" or "jp2"
    uint32_t width = 0;             // pixels (0 when passed through and unknown)
    uint32_t height = 0;
};

constexpr uint64_t kDefaultMaxPixels = uint64_t(1) << 28;

// An image XObject (`image`, a stream, resolved or a reference). `resources` (the resources that
// named it; may be null) resolves named colour spaces.
Status EncodeXObject(const Document& document, const PdfValue& image, const PdfValue& resources, Encoded& out,
                     uint64_t maxPixels = kDefaultMaxPixels);

// An inline image: its dictionary as the content lexer returns it (abbreviated keys and values
// allowed: /W /H /BPC /CS /D /F /DP /IM /I, /G /RGB /CMYK /I for the colour spaces, /AHx /A85 /LZW
// /Fl /RL /CCF /DCT for the filters) and the bytes between ID and EI.
Status EncodeInline(const Document& document, const PdfValue& dictionary, std::string_view data,
                    const PdfValue& resources, Encoded& out, uint64_t maxPixels = kDefaultMaxPixels);

// An 8-bit PNG of `pixels` (rows top to bottom, `channels` 1 gray, 2 gray + alpha, 3 RGB or 4 RGBA,
// no row padding), compressed with RayoPDF's own deflate. False for an empty image.
bool EncodePng(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, std::string& out);

} // namespace RayoPdf::Image
