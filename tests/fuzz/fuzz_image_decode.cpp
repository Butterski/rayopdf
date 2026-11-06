// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of Image::EncodeXObject and Image::EncodeInline. An input starting with "%PDF"
// is a whole file: every image XObject among its first objects is encoded with the first page's
// resources. Any other input is the entries of an image dictionary (PDF text without "<<" and
// ">>"), a line "ID", and the image's data: it becomes the stream of an image XObject in a small
// document that also holds the objects a dictionary may name (6 an ICC profile, 7 a type 2
// function into CMYK, 8 a sampled function into RGB, 9 a soft mask, 10 a stencil mask, 11 an
// Indexed lookup stream, 12 a type 4 function, resources 13 with /CS0 and /CS1), and the same
// entries, read as content-stream syntax, are encoded as an inline image of that data.
#include "../pdf_builder.h"

#include "image/image.h"
#include "pdf/document.h"
#include "pdf/syntax.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace {

// Small enough that one input stays fast; the code paths do not depend on the size.
constexpr uint64_t kMaxPixels = uint64_t(1) << 20;

void EncodeAll(const RayoPdf::Document& doc) {
    using namespace RayoPdf;
    const uint32_t objects = doc.ObjectCount() < 512 ? doc.ObjectCount() : 512;
    const PdfValue& resources = doc.PageCount() > 0 ? doc.Resources(0) : doc.Object(0);
    Image::Encoded out;
    for (uint32_t num = 0; num < objects; num++) {
        const PdfValue& value = doc.Object(num);
        if (!value.IsStream()) continue;
        Image::EncodeXObject(doc, value, resources, out, kMaxPixels);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf;
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    if (input.substr(0, 4) == "%PDF") {
        std::unique_ptr<Document> doc;
        if (Document::Open(input, "", doc) == OpenStatus::Ok) EncodeAll(*doc);
        return 0;
    }
    const size_t split = input.find("\nID\n");
    const std::string_view dict = input.substr(0, split);
    const std::string_view bytes = split == std::string_view::npos ? std::string_view() : input.substr(split + 4);

    Test::PdfBuilder b;
    const int catalog = b.Reserve();             // 1
    const int pages = b.Reserve();               // 2
    const int page = b.Reserve();                // 3
    const int image = b.Reserve();               // 4
    b.Add("<< /Type /XObject >>");               // 5, a placeholder
    b.AddStream("/N 3", "profile");              // 6
    b.Add("<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [1 0 0 0] /N 1.5 >>");  // 7
    b.AddStream("/FunctionType 0 /Domain [0 1] /Range [0 1 0 1 0 1] /Size [3] /BitsPerSample 4",
                std::string("\x01\x23\x45\x67\x89", 5));  // 8
    b.AddFlateStream("/Width 3 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 8",
                     std::string("\x00\x40\x80\xC0\xFF\x10", 6));  // 9
    b.AddFlateStream("/Width 2 /Height 2 /ImageMask true", std::string("\x40\x80", 2));  // 10
    b.AddFlateStream("", std::string("\xFF\x00\x00\x00\xFF\x00", 6));  // 11
    b.AddStream("/FunctionType 4 /Domain [0 1] /Range [0 1 0 1 0 1 0 1]", "{dup dup dup}");  // 12
    const int resources = b.Add(
        "<< /ColorSpace << /CS0 [/Separation /Spot /DeviceRGB 8 0 R] /CS1 [/Indexed /DeviceRGB 1 11 0 R] >> >>");
    b.Set(image, "<< " + std::string(dict) + " /Length " + std::to_string(bytes.size()) + " >>\nstream\n" +
                     std::string(bytes) + "\nendstream");
    b.Set(page, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] /Resources " + std::to_string(resources) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages 2 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    const std::string file = b.Build(catalog);
    std::unique_ptr<Document> doc;
    if (Document::Open(file, "", doc) != OpenStatus::Ok) return 0;
    Image::Encoded out;
    const PdfValue& res = doc->Object(static_cast<uint32_t>(resources));
    Image::EncodeXObject(*doc, doc->Object(static_cast<uint32_t>(image)), res, out, kMaxPixels);

    PdfArena arena;
    Syntax::Builder builder(arena);
    const std::string inlineText = "<< " + std::string(dict) + " >>";
    Syntax::Cursor cursor{inlineText, 0};
    PdfValue inlineDict;
    if (Syntax::ParseValue(cursor, Syntax::Mode::Content, 0, builder, inlineDict)) {
        Image::EncodeInline(*doc, inlineDict, bytes, res, out, kMaxPixels);
    }
    return 0;
}
