// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the whole conversion of one page whose content stream is the input. The page
// has a simple font, an Identity-H Type0 font without a font program, an ExtGState, marked-content
// properties and a Form XObject that draws the input again (so form recursion and its cycle check
// are reached), a link annotation and an outline.
#include "../pdf_builder.h"

#include "layout/convert.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view content(reinterpret_cast<const char*>(data), size);
    RayoPdf::Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int page = b.Reserve();
    const int form = b.Reserve();
    const std::string resources =
        "<< /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> "
        "/F2 << /Type /Font /Subtype /Type0 /BaseFont /X /Encoding /Identity-H /DescendantFonts [<< /Type /Font "
        "/Subtype /CIDFontType2 /BaseFont /X /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
        "/DW 500 >>] >> >> "
        "/ExtGState << /G1 << /ca 0 /Font [" + std::to_string(b.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>")) +
        " 0 R 9] >> >> "
        "/Properties << /P1 << /MCID 3 /ActualText (alt) >> >> "
        "/XObject << /X1 " + std::to_string(form) + " 0 R >> >>";
    const std::string stream(content);
    std::string formBody = "<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Matrix [1 0 0 1 5 5] /Resources " +
                           resources + " /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream";
    b.Set(form, formBody);
    const int contents = b.AddStream("", stream);
    b.Set(page, "<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 612 792] /Resources " +
                    resources + " /Contents " + std::to_string(contents) +
                    " 0 R /Annots [<< /Subtype /Link /Rect [0 0 300 800] /A << /S /URI /URI (https://e.x/) >> >>] >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) +
                       " 0 R /Outlines << /First << /Title (Title) >> >> >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string pdf = b.Build(catalog);
    std::unique_ptr<RayoPdf::Document> document;
    if (RayoPdf::Document::Open(pdf, "", document) != RayoPdf::OpenStatus::Ok) return 0;
    std::string markdown;
    RayoPdf::Layout::ConvertReport report;
    RayoPdf::Layout::Convert(*document, RayoPdf::Layout::ConvertOptions(), markdown, report);
    return 0;
}
