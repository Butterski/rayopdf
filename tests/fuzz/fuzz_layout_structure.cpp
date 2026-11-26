// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the tagged path (src/layout/structure.cpp): the input is a small program
// that builds a tagged PDF with tests/pdf_builder.h (elements of any role and attribute, MCIDs with
// and without /Pg, marked text, artifacts, untagged text, kids that loop back, MCRs into a Form
// XObject, RoleMap entries that may cycle, OBJR kids naming the form fields each page draws or
// any other object), which is then converted with the structure tree on.
// Every input is a well-formed file, so the fuzzer spends its time in the structure reader, the
// checks and the block builder rather than in the object layer (fuzz_layout_document covers that).
// It needs no seeds.

#include "../pdf_builder.h"

#include "layout/convert.h"
#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

const char* const kRoles[] = {"Document", "Part", "Sect", "Div", "NonStruct", "P", "H", "H1", "H2", "H3",
                              "H6", "H9", "L", "LI", "Lbl", "LBody", "Table", "THead", "TBody", "TR",
                              "TH", "TD", "Span", "Link", "Em", "Strong", "Code", "BlockQuote", "Note",
                              "Figure", "Formula", "Caption", "Artifact", "Custom", "Other", "Quote"};
constexpr size_t kRoleCount = sizeof(kRoles) / sizeof(kRoles[0]);

const char* const kAttributes[] = {"",
                                   "/A << /O /Table /ColSpan 2 >>",
                                   "/A << /O /Table /RowSpan 3 >>",
                                   "/A [<< /O /Layout /TextDecorationType /LineThrough >> 0]",
                                   "/A << /O /Layout /Placement /Block >>",
                                   "/A << /O /Layout /Placement /Inline >>",
                                   "/A << /O /List /ListNumbering /Decimal >>",
                                   "/A << /O /Layout /StartIndent 20 /EndIndent 20 >>",
                                   "/Alt (alternate text) /ActualText (x)",
                                   "/C /Cls",
                                   "/A << /O /Table /ColSpan 1000 /RowSpan 0 >>",
                                   "/A 7 0 R"};
constexpr size_t kAttributeCount = sizeof(kAttributes) / sizeof(kAttributes[0]);

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4096) return 0;
    RayoPdf::Test::TaggedBuilder t(2);
    const int formObject = t.Pdf().Reserve();
    // A filled text field on each page, drawn as page text; OBJR kids may name them.
    int widgets[2];
    for (int p = 0; p < 2; ++p) {
        const int ap = t.Pdf().AddStream("/Type /XObject /Subtype /Form /BBox [0 0 180 17] /Resources << /Font << /Helv "
                                         "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >>",
                                         "/Tx BMC BT /Helv 10 Tf 2 4 Td (field value) Tj ET EMC");
        widgets[p] = t.Pdf().Add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (f" + std::to_string(p) +
                                 ") /V (field value) /Rect [" + std::to_string(100 + 200 * p) + " 600 " +
                                 std::to_string(280 + 200 * p) + " 617] /AP << /N " + std::to_string(ap) + " 0 R >> >>");
        t.PageExtra(p, "/Annots [" + std::to_string(widgets[p]) + " 0 R]");
    }
    std::vector<int> stack{0};
    std::vector<int> elements;
    std::string content[2];
    std::string form;
    std::string roleMap;
    int page = 0;
    int nextMcid[2] = {0, 0};
    size_t i = 0;
    const auto next = [&]() -> uint8_t { return i < size ? data[i++] : 0; };
    while (i < size) {
        const uint8_t op = next();
        switch (op % 15) {
        case 0: {
            const char* role = kRoles[next() % kRoleCount];
            const char* attributes = kAttributes[next() % kAttributeCount];
            const int e = t.Element(stack.back(), role, attributes);
            elements.push_back(e);
            if (stack.size() < 600) stack.push_back(e);
            break;
        }
        case 1:
            if (stack.size() > 1) stack.pop_back();
            break;
        case 2:
        case 3: {
            // Marked text at a position the input chooses; its MCID belongs to the current element.
            const int mcid = nextMcid[page]++;
            const double x = 20 + 2 * next(), y = 40 + 3 * next();
            const double fontSize = 6 + next() % 18;
            if (stack.back() != 0) t.Mcid(stack.back(), page, mcid, op % 15 == 2);
            content[page] += RayoPdf::Test::MarkedText("P", mcid, x, y, fontSize, "Word w" + std::to_string(mcid));
            break;
        }
        case 4:
            content[page] += RayoPdf::Test::MarkedText("Artifact", -1, 20 + next(), 40 + 3 * next(), 9, "Folio");
            break;
        case 5:
            content[page] += "BT /F1 10 Tf " + std::to_string(20 + next()) + " " + std::to_string(40 + 3 * next()) +
                             " Td (untagged) Tj ET\n";
            break;
        case 6:
            // A kid that points back at an element (a cycle or a shared kid).
            if (!elements.empty() && stack.back() != 0)
                t.Kid(stack.back(), std::to_string(t.ObjectOf(elements[next() % elements.size()])) + " 0 R");
            break;
        case 7:
            // A marked-content reference into the form the first page draws.
            if (stack.back() != 0) {
                const int mcid = next() % 8;
                t.Kid(stack.back(), "<< /Type /MCR /MCID " + std::to_string(mcid) + " /Pg " +
                                        std::to_string(t.PageObject(0)) + " 0 R /Stm " + std::to_string(formObject) +
                                        " 0 R >>");
                form += RayoPdf::Test::MarkedText("Span", mcid, 30 + next(), 50 + 3 * next(), 10, "form");
            }
            break;
        case 8: {
            static const char* const kMaps[] = {"/Custom /H1 ", "/Custom /Other /Other /Custom ", "/Other /TD ",
                                                "/Custom /Custom ", "/H1 /P ", "/Other /Div "};
            roleMap += kMaps[next() % 6];
            break;
        }
        case 9: page = 1 - page; break;
        case 10:
            if (stack.back() != 0) t.Kid(stack.back(), std::to_string(next()));  // an MCID with no text
            break;
        case 11:
            if (stack.back() != 0)
                t.Kid(stack.back(), "<< /Type /OBJR /Obj " +
                                        std::to_string(next() % 2 ? widgets[next() % 2] : next() % 16) + " 0 R >>");
            break;
        case 12:
            // Text right inside nested marked content of one MCID.
            content[page] += "/Span << /MCID " + std::to_string(next() % 32) + " >> BDC /Span BMC BT /F1 11 Tf " +
                             std::to_string(30 + next()) + " " + std::to_string(40 + 3 * next()) +
                             " Td (nested) Tj ET EMC EMC\n";
            break;
        case 13: {
            // An inline image in marked content of its own, maybe set in a line of text (PlaceImages
            // then adds a glyph to that line), maybe large enough to stand alone.
            const int mcid = nextMcid[page]++;
            if (stack.back() != 0) t.Mcid(stack.back(), page, mcid);
            const uint8_t shade = next();
            const double side = 6 + next() % 160;
            content[page] += "/Figure << /MCID " + std::to_string(mcid) + " >> BDC q " + std::to_string(side) + " 0 0 " +
                             std::to_string(side) + " " + std::to_string(20 + 2 * next()) + " " +
                             std::to_string(40 + 3 * next()) + " cm BI /W 32 /H 32 /CS /G /BPC 8 ID\n" +
                             std::string(32 * 32, static_cast<char>(shade | 0x80)) + "\nEI Q EMC\n";
            break;
        }
        default:
            content[page] += "/P << /MCID " + std::to_string(next() % 32) + " >> BDC\n";  // left open
            break;
        }
    }
    // The form, as a stream written by hand into the object reserved for it.
    t.Pdf().Set(formObject, "<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << /Font << /F1 << "
                            "/Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> /Length " +
                                std::to_string(form.size()) + " >>\nstream\n" + form + "\nendstream");
    content[0] += "/Fm1 Do\n";
    const std::string pdf = t.Build({content[0], content[1]},
                                    "/RoleMap << " + roleMap + ">> /ClassMap << /Cls << /O /Table /ColSpan 3 >> >>",
                                    "/XObject << /Fm1 " + std::to_string(formObject) + " 0 R >>");
    std::unique_ptr<RayoPdf::Document> document;
    if (RayoPdf::Document::Open(pdf, "", document) != RayoPdf::OpenStatus::Ok) return 0;
    RayoPdf::Layout::ConvertOptions options;
    // Images are written (PlaceImages runs between the tree's stages and changes the lines).
    options.imageDirectory = "/tmp/fuzz_layout_structure-images";
    std::string markdown;
    RayoPdf::Layout::ConvertReport report;
    RayoPdf::Layout::Convert(*document, options, markdown, report);
    return 0;
}
