// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of Document::Open and everything a reader then asks of a document: every
// object, every stream decoded, every page's attributes and content.
#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf;
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    std::unique_ptr<Document> doc;
    if (Document::Open(bytes, "", doc) != OpenStatus::Ok) return 0;
    std::string out;
    // Bounded so that one input stays fast: objects and pages past these are what the first
    // ones already exercise.
    const uint32_t objects = doc->ObjectCount() < 4096 ? doc->ObjectCount() : 4096;
    for (uint32_t num = 0; num < objects; num++) {
        const PdfValue& value = doc->Object(num);
        if (value.IsStream()) doc->StreamData(value, out, size_t(1) << 20);
        PdfRect rect;
        doc->Rect(value, rect);
        double number = 0;
        doc->Number(value, number);
        doc->Get(value, "Type");
    }
    const int pages = doc->PageCount() < 256 ? doc->PageCount() : 256;
    for (int i = 0; i < pages; i++) {
        doc->PageDict(i);
        doc->MediaBox(i);
        doc->CropBox(i);
        doc->Rotate(i);
        doc->Resources(i);
        doc->PageContent(i, out);
    }
    doc->Version();
    doc->Info();
    doc->Catalog();
    doc->Trailer();
    return 0;
}
