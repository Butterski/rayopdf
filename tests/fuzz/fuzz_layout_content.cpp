// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the whole conversion of one page whose content stream is the input. The page
// has a simple font, an Identity-H Type0 font without a font program, an ExtGState, marked-content
// properties and a Form XObject that draws the input again (so form recursion and its cycle check
// are reached), a link annotation and an outline, and form fields and a FreeText annotation drawn
// as page text: appearances that are that form (directly and through a state dictionary), a
// comb field, a check box, and a choice field whose value and /DA are the input's first bytes (a
// synthesized appearance); and a highlight over the page whose comment, author and date are those
// bytes, written as JSON (the text its quads cover).
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
    // The input's first bytes as a PDF hex string.
    std::string hex = "<";
    for (size_t i = 0; i < size && i < 64; ++i) {
        static const char kHex[] = "0123456789ABCDEF";
        hex.push_back(kHex[data[i] >> 4]);
        hex.push_back(kHex[data[i] & 15]);
    }
    hex += ">";
    const std::string x1 = std::to_string(form) + " 0 R";
    const std::string annots =
        "<< /Subtype /Link /Rect [0 0 300 800] /A << /S /URI /URI (https://e.x/) >> >> "
        "<< /Subtype /Widget /FT /Tx /T (a) /V (value) /Rect [50 50 300 80] /AP << /N " + x1 + " >> >> "
        "<< /Subtype /Widget /FT /Tx /Ff 16777216 /MaxLen 4 /V (abcd) /Rect [50 90 300 120] /AS /On "
        "/AP << /N << /On " + x1 + " /Off " + x1 + " >> >> >> "
        "<< /Subtype /Widget /Parent << /FT /Btn /V /On >> /Rect [10 10 20 20] /AS /On >> "
        "<< /Subtype /Widget /FT /Ch /Opt [[(a) (Alpha)] (b)] /V [" + hex + " (a)] /DA " + hex +
        " /Rect [50 130 300 160] >> "
        "<< /Subtype /FreeText /Rect [300 300 400 400] /AP << /N " + x1 + " >> >> "
        "<< /Subtype /Highlight /Rect [0 0 612 792] /QuadPoints [0 792 612 792 0 0 612 0 10 20 30] /Contents " + hex +
        " /T " + hex + " /M " + hex + " >>";
    b.Set(page, "<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 612 792] /Resources " +
                    resources + " /Contents " + std::to_string(contents) + " 0 R /Annots [" + annots + "] >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) +
                       " 0 R /Outlines << /First << /Title (Title) >> >> /AcroForm << /DR << /Font << /Helv << /Type "
                       "/Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> /DA (/Helv 0 Tf 0 g) >> >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string pdf = b.Build(catalog);
    std::unique_ptr<RayoPdf::Document> document;
    if (RayoPdf::Document::Open(pdf, "", document) != RayoPdf::OpenStatus::Ok) return 0;
    std::string markdown;
    RayoPdf::Layout::ConvertReport report;
    RayoPdf::Layout::Convert(*document, RayoPdf::Layout::ConvertOptions(), markdown, report);
    RayoPdf::Layout::ConvertOptions json;
    json.format = RayoPdf::Layout::OutputFormat::Json;
    RayoPdf::Layout::Convert(*document, json, markdown, report);
    return 0;
}
