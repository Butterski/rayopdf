// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the form-field reader (src/layout/forms.cpp, the JSON output's "fields"):
// the input is a small program that builds a document's field tree with tests/pdf_builder.h
// (nodes with partial names, kids and parents that may loop back, field types and flags, values of
// every PDF type, option lists, widgets on the page or not), which is then read by ReadFields and
// converted to JSON (the widgets drawn as page text, a value synthesized under NeedAppearances).
// Every input is a well-formed file, so the fuzzer spends its time in the field reader rather than
// in the object layer (fuzz_layout_document covers that). It needs no seeds.

#include "../pdf_builder.h"

#include "layout/convert.h"
#include "layout/forms.h"
#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kNodes = 16;

const char* const kTypes[] = {"Tx", "Btn", "Ch", "Sig", "Other"};
const char* const kValues[] = {"(plain value)", "<FEFF0416043E0020263A>", "/Yes", "/Off", "/1", "[(a) (b) /c 7]",
                               "12", "<< /V (x) >>", "()", "(multi\\nline\\rvalue)"};
const char* const kOptions[] = {"[(a) (b)]", "[[(e1) (Display one)] [(e2) (Display two)]]", "[3 /n []]", "7"};

std::string Ref(int object) { return std::to_string(object) + " 0 R "; }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4096) return 0;
    RayoPdf::Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int page = b.Reserve();
    const int text = b.AddStream("", "a text stream value");
    const int ap = b.AddStream("/Type /XObject /Subtype /Form /BBox [0 0 100 20] /Resources << /Font << /Helv << /Type "
                               "/Font /Subtype /Type1 /BaseFont /Helvetica >> >> >>",
                               "BT /Helv 9 Tf 2 5 Td (shown) Tj ET");
    int nodes[kNodes];
    for (int& n : nodes) n = b.Reserve();
    std::string body[kNodes], kids[kNodes], annots, roots;
    size_t i = 0;
    const auto next = [&]() -> uint8_t { return i < size ? data[i++] : 0; };
    while (i < size) {
        const uint8_t op = next();
        const int node = next() % kNodes;
        std::string& d = body[node];
        switch (op % 12) {
        case 0: d += " /T (n" + std::to_string(next()) + ")"; break;
        case 1: kids[node] += Ref(nodes[next() % kNodes]); break;  // may loop back
        case 2: d += " /Parent " + Ref(nodes[next() % kNodes]); break;
        case 3: d += std::string(" /FT /") + kTypes[next() % 5]; break;
        case 4: {
            static const int kFlags[] = {0, 1, 2, 4096, 8192, 16384, 32768, 49152, 65536, 131072, 2097152, 16777216};
            d += " /Ff " + std::to_string(kFlags[next() % 12]);
            break;
        }
        case 5: d += std::string(" /V ") + kValues[next() % 10]; break;
        case 6: d += std::string(" /Opt ") + kOptions[next() % 4]; break;
        case 7:
            d += " /Subtype /Widget /Rect [" + std::to_string(next()) + " " + std::to_string(next()) + " " +
                 std::to_string(200 + next()) + " " + std::to_string(30 + next()) + "]";
            annots += Ref(nodes[node]);
            break;
        case 8: d += " /AS /" + std::string(next() % 2 ? "Yes" : "Off") + " /AP << /N << /Yes " + Ref(ap) + ">> >>"; break;
        case 9: d += " /V " + Ref(text); break;
        case 10: d += " /AP << /N " + Ref(ap) + ">> /DA (/Helv " + std::to_string(next() % 20) + " Tf 0 g)"; break;
        default: roots += Ref(nodes[node]); break;
        }
    }
    for (int n = 0; n < kNodes; ++n)
        b.Set(nodes[n], "<<" + body[n] + (kids[n].empty() ? std::string() : " /Kids [" + kids[n] + "]") + " >>");
    if (roots.empty()) roots = Ref(nodes[0]);
    const bool need = size > 0 && (data[0] & 1);
    b.Set(page, "<< /Type /Page /Parent " + std::to_string(pages) +
                    " 0 R /MediaBox [0 0 612 792] /Rotate " + std::to_string(90 * (size > 1 ? data[1] % 4 : 0)) +
                    " /Annots [" + annots + "] >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R /AcroForm << /Fields [" + roots +
                       "] /NeedAppearances " + (need ? "true" : "false") +
                       " /DR << /Font << /Helv << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> >> >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string pdf = b.Build(catalog);
    std::unique_ptr<RayoPdf::Document> document;
    if (RayoPdf::Document::Open(pdf, "", document) != RayoPdf::OpenStatus::Ok) return 0;
    std::vector<RayoPdf::Layout::FormField> fields;
    RayoPdf::Layout::ReadFields(*document, 0, 0, fields);
    RayoPdf::Layout::ConvertOptions options;
    options.format = RayoPdf::Layout::OutputFormat::Json;
    options.ocr.mode = RayoPdf::Layout::OcrMode::Off;
    std::string json;
    RayoPdf::Layout::ConvertReport report;
    RayoPdf::Layout::Convert(*document, options, json, report);
    return 0;
}
