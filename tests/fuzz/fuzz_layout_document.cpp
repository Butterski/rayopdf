// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the whole conversion: the input as a PDF file through Document::Open and
// Layout::Convert (fonts, content, layout, semantics and the writer, as `rayopdf --convert` runs
// them, without image files), then once more in one other output, chosen by the input's size so
// that each input costs two conversions: JSON (--format=json, with --json-detail=blocks, lines or
// words by the size again), which must be one object and a line break; chunks (--format=chunks
// with the smallest chunk size), every line one object; the
// inspector (--format=inspect), one HTML file without a script, or plain text (--format=text, by
// the size again), empty or ending in a line break or a form feed; Markdown with the output options
// --plain, --tables=html, --tables=pipe or --tables=pipe-latex (by the size again),
// --furniture=keep, --quotes=ascii, --small-caps=keep and --figure-text=drop;
// or Markdown with a feature sink (`rayopdf --features`) counting the edge rows, which must be
// the same Markdown.
// Seeds: `python3 tools/fuzz.py seeds` (real PDFs).

#include "layout/convert.h"
#include "layout/features.h"
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
    std::string out;
    RayoPdf::Layout::ConvertReport report;
    RayoPdf::Layout::Convert(*document, options, out, report);
    switch (size % 5) {
    case 0:
        options.format = RayoPdf::Layout::OutputFormat::Json;
        options.jsonDetail = size % 15 == 0   ? RayoPdf::Layout::JsonDetail::Blocks
                             : size % 15 == 5 ? RayoPdf::Layout::JsonDetail::Lines
                                              : RayoPdf::Layout::JsonDetail::Words;
        RayoPdf::Layout::Convert(*document, options, out, report);
        if (out.size() < 3 || out.front() != '{' || out.compare(out.size() - 2, 2, "}\n") != 0) __builtin_trap();
        break;
    case 1:
        options.format = RayoPdf::Layout::OutputFormat::Chunks;
        options.chunkChars = RayoPdf::Layout::kMinChunkChars;
        RayoPdf::Layout::Convert(*document, options, out, report);
        for (size_t at = 0; at < out.size();) {
            const size_t end = out.find('\n', at);
            if (end == std::string::npos || end - at < 2 || out[at] != '{' || out[end - 1] != '}') __builtin_trap();
            at = end + 1;
        }
        break;
    case 2:
        options.format = size % 10 == 2 ? RayoPdf::Layout::OutputFormat::Inspect : RayoPdf::Layout::OutputFormat::Text;
        RayoPdf::Layout::Convert(*document, options, out, report);
        if (options.format == RayoPdf::Layout::OutputFormat::Inspect
                ? out.compare(0, 15, "<!DOCTYPE html>") != 0 || out.find("<script") != std::string::npos
                : !out.empty() && out.back() != '\n' && out.back() != '\f')
            __builtin_trap();
        break;
    case 3:
        options.plain = true;
        options.tables = size % 15 == 3   ? RayoPdf::Layout::TableFormat::Html
                         : size % 15 == 8 ? RayoPdf::Layout::TableFormat::Pipe
                                          : RayoPdf::Layout::TableFormat::PipeLatex;
        options.furniture = RayoPdf::Layout::Furniture::Keep;
        options.quotes = RayoPdf::Layout::Quotes::Ascii;
        options.smallCaps = RayoPdf::Layout::SmallCaps::Keep;
        options.figureText = RayoPdf::Layout::FigureText::Drop;
        RayoPdf::Layout::Convert(*document, options, out, report);
        break;
    default: {
        size_t calls = 0;
        RayoPdf::Layout::FeatureSink sink;
        sink.context = &calls;
        sink.edgeRow = [](void* context, uint32_t, const float*, float, float, float, float, std::string_view, bool) {
            ++*static_cast<size_t*>(context);
        };
        options.features = &sink;
        std::string again;
        RayoPdf::Layout::Convert(*document, options, again, report);
        if (again != out) __builtin_trap();
        break;
    }
    }
    return 0;
}
