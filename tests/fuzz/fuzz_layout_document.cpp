// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the whole conversion: the input as a PDF file through Document::Open and
// Layout::Convert (fonts, content, layout, semantics and the writer, as `rayopdf --convert` runs
// them, without image files), then again as JSON (--format=json), which must be one object and a
// line break. Seeds: `python3 tools/fuzz.py seeds` (real PDFs).

#include "layout/convert.h"
#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::unique_ptr<RayoPdf::Document> document;
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    if (RayoPdf::Document::Open(bytes, "", document) != RayoPdf::OpenStatus::Ok) return 0;
    RayoPdf::Layout::ConvertOptions options;
    std::string markdown;
    RayoPdf::Layout::ConvertReport report;
    RayoPdf::Layout::Convert(*document, options, markdown, report);
    options.format = RayoPdf::Layout::OutputFormat::Json;
    RayoPdf::Layout::Convert(*document, options, markdown, report);
    if (markdown.size() < 3 || markdown.front() != '{' || markdown.compare(markdown.size() - 2, 2, "}\n") != 0)
        __builtin_trap();
    return 0;
}
