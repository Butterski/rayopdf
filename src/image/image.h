// SPDX-License-Identifier: Apache-2.0
// Images out of a PDF as files (roadmap D6), as 8-bit PNGs (gray, gray + alpha, RGB or RGBA) of
// the decoded pixels, except that a JPEG (DCT) is written as stored (.jpg) when that file shows
// what the PDF shows. Decoding covers ISO 32000-1 section 8.9: /ColorSpace DeviceGray, DeviceRGB,
// DeviceCMYK, CalGray, CalRGB, Lab, ICCBased (by /N, or its /Alternate), Indexed (any base),
// Separation and DeviceN (through their tint transform where it is a sampled or exponential
// function, else as gray from the tint); /BitsPerComponent 1, 2, 4, 8 and 16; /Decode; /ImageMask
// stencils; /SMask soft masks and /Mask colour-key or stencil masks as alpha; the image filters
// through RayoPDF's own decoders: DCTDecode (image/jpeg.h), CCITTFaxDecode (image/ccitt.h, inline
// /CCF too), JBIG2Decode with /JBIG2Globals (image/jbig2.h) and JPXDecode (image/jpx.h: the JP2
// header's colour when there is no /ColorSpace, /SMaskInData as the soft mask when there is no
// /SMask, /Decode ignored). A JPEG is decoded instead of stored when it has 4 components
// (stand-alone viewers read an Adobe APP14 marker as inverted CMYK, PDF does not), another colour
// space than gray or RGB, a /Decode other than the default, or a mask that hides part of it. A
// JPEG or JPEG 2000 image the decoders cannot read is written as stored (.jpg, .jp2).
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
    Unsupported,  // a filter not decoded here, a feature its decoder does not read, a colour space
                  // or function not handled
    Corrupt,      // missing or inconsistent /Width, /Height, /BitsPerComponent, or no usable data
    TooLarge,     // more than `maxPixels` pixels
    WriteFailed,  // a PNG made as it was written: the Sink failed (Open or a write)
};

// Where a PNG goes while it is made, instead of Encoded::bytes: the image's file. Open comes
// once the image is to be written as a PNG, before its first byte; then the bytes in order, and
// Patch sets bytes already written (the IDAT chunk's length, known at its end). Open may come
// again: the PNG begun anew, what was written before dropped (image data that ends before the
// image's height, known only once read). The file is only as large as the PNG, and the process
// holds no copy of it.
class Sink {
public:
    virtual bool Open(const char* extension) = 0;
    virtual bool Append(const uint8_t* data, size_t size) = 0;
    virtual bool Patch(uint64_t offset, const uint8_t* data, size_t size) = 0;

protected:
    ~Sink() = default;
};

// An image as file bytes.
struct Encoded {
    std::string bytes;
    const char* extension = "png";  // "png", "jpg" or "jp2"
    uint32_t width = 0;             // pixels (0 when passed through and unknown)
    uint32_t height = 0;
    // The image has a mask the file lacks: a JPEG or JPEG 2000 image the decoders cannot read with
    // a mask that hides part of it, or a mask whose own data cannot be decoded.
    bool maskDropped = false;
    // The PNG went to the Sink (`bytes` is empty); a JPEG or JPEG 2000 passed through never does.
    bool streamed = false;
};

constexpr uint64_t kDefaultMaxPixels = uint64_t(1) << 28;

// An image XObject (`image`, a stream, resolved or a reference). `resources` (the resources that
// named it; may be null) resolves named colour spaces. With a `sink`, a PNG is made into it.
Status EncodeXObject(const Document& document, const PdfValue& image, const PdfValue& resources, Encoded& out,
                     uint64_t maxPixels = kDefaultMaxPixels, Sink* sink = nullptr);

// An inline image: its dictionary as the content lexer returns it (abbreviated keys and values
// allowed: /W /H /BPC /CS /D /F /DP /IM /I, /G /RGB /CMYK /I for the colour spaces, /AHx /A85 /LZW
// /Fl /RL /CCF /DCT for the filters) and the bytes between ID and EI.
Status EncodeInline(const Document& document, const PdfValue& dictionary, std::string_view data,
                    const PdfValue& resources, Encoded& out, uint64_t maxPixels = kDefaultMaxPixels,
                    Sink* sink = nullptr);

// An image as 8-bit gray pixels, for OCR: decoded like a PNG of EncodeXObject (colour space,
// /Decode), then the luminance (77 R + 150 G + 29 B) / 256 of each pixel composited on white with
// its soft, stencil or colour-key mask; an /ImageMask stencil is black where it paints, white
// elsewhere. Rows top to bottom as the image's own (its first row is the top of image space).
struct GrayImage {
    std::string pixels;  // width * height bytes, no row padding
    uint32_t width = 0;
    uint32_t height = 0;
    // The image's own size before downscaling: its decoded width and its declared height; rows
    // its data lacks are white, so the pixels always cover the whole of image space.
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
};

// The gray pixels of image XObject `image` (as EncodeXObject takes it). With `maxSide` > 0 and a
// longer side above it, the image is reduced by area averaging so that its longer side is
// `maxSide` (the shorter one in proportion, at least 1); the reduction is made a source row at a
// time, so no full-size gray plane is held. `out` is emptied on failure. Unsupported: data in a
// filter not decoded (as EncodeXObject).
Status GrayXObject(const Document& document, const PdfValue& image, const PdfValue& resources, GrayImage& out,
                   uint32_t maxSide = 0, uint64_t maxPixels = kDefaultMaxPixels);

// The same for an inline image (as EncodeInline takes it).
Status GrayInline(const Document& document, const PdfValue& dictionary, std::string_view data,
                  const PdfValue& resources, GrayImage& out, uint32_t maxSide = 0,
                  uint64_t maxPixels = kDefaultMaxPixels);

// An 8-bit PNG of `pixels` (rows top to bottom, `channels` 1 gray, 2 gray + alpha, 3 RGB or 4 RGBA,
// no row padding), compressed with RayoPDF's own deflate. False for an empty image.
bool EncodePng(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, std::string& out);

} // namespace RayoPdf::Image
