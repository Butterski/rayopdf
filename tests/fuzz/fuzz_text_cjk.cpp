// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the CJK tables (src/text/cjk.h) and the Type0 font chain through them. The
// first byte picks a predefined CMap and options; the input then splits in half: the first half is
// an embedded CMap (based on the picked CMap through usecmap) or a ToUnicode CMap, the second half
// the shown string. Both the direct table lookups and a font loaded through FontCache read it.
#include "../pdf_builder.h"

#include "pdf/document.h"
#include "text/cjk.h"
#include "text/cjk_tables.h"
#include "text/font.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

using namespace RayoPdf;
using namespace RayoPdf::Text;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    const uint8_t options = data[1];
    const int predefined = data[0] % Cjk::kPredefinedCount;
    const std::string_view rest(reinterpret_cast<const char*>(data + 2), size - 2);
    const std::string_view cmapText = rest.substr(0, rest.size() / 2);
    const std::string_view shown = rest.substr(rest.size() / 2, 512);

    // Direct lookups: codes split by the CMap's codespace, CIDs, their collection text.
    Codespace codespace;
    Cjk::AddCodespace(predefined, codespace);
    const Cjk::Collection collection = Cjk::CollectionOf(predefined);
    for (size_t at = 0; at < shown.size();) {
        uint32_t code = 0;
        uint8_t length = codespace.Empty() ? 2 : codespace.Read(shown, at, code);
        if (codespace.Empty()) {
            code = static_cast<uint8_t>(shown[at]);
            if (at + 1 < shown.size()) code = code << 8 | static_cast<uint8_t>(shown[at + 1]);
        }
        if (length == 0 || length > 4) __builtin_trap();
        at += length;
        uint32_t cid = 0;
        if (Cjk::Cid(predefined, code, cid)) {
            const std::string_view text = Cjk::CidText(collection, cid);
            if (text.size() > 4) __builtin_trap();
        }
        Cjk::CidToUnicode(static_cast<Cjk::Collection>(options % 6), code);
    }

    // The font chain.
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const std::string name = Cjk::kPredefined[predefined].name;
    std::string encoding = "/" + name;
    std::string toUnicode;
    if (options & 1) {
        encoding = std::to_string(b.AddStream("/Type /CMap /UseCMap /" + name, std::string(cmapText))) + " 0 R";
    } else if (options & 2) {
        toUnicode = " /ToUnicode " + std::to_string(b.AddStream("", std::string(cmapText))) + " 0 R";
    }
    static const char* const kOrderings[4] = {"Japan1", "GB1", "CNS1", "Korea1"};
    const char* ordering = (options & 4) ? kOrderings[(options >> 3) & 3] : "Identity";
    const char* subtype = (options & 32) ? "CIDFontType2" : "CIDFontType0";
    const int cidFont = b.Add(std::string("<< /Type /Font /Subtype /") + subtype +
                              " /BaseFont /F /CIDSystemInfo << /Registry (Adobe) /Ordering (" + ordering +
                              ") /Supplement 0 >> /DW 1000 /W [1 [250] 843 [500 600] 1000 2000 700] >>");
    const int font = b.Add("<< /Type /Font /Subtype /Type0 /BaseFont /F /Encoding " + encoding + toUnicode +
                           " /DescendantFonts [" + std::to_string(cidFont) + " 0 R] >>");
    const int content = b.AddStream("", "BT ET");
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 100 100]"
                           " /Resources << /Font << /F1 " + std::to_string(font) + " 0 R >> >> /Contents " +
                           std::to_string(content) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string bytes = b.Build(catalog);
    std::unique_ptr<Document> doc;
    if (Document::Open(bytes, "", doc) != OpenStatus::Ok) return 0;
    FontCache cache(*doc);
    const PdfValue& fonts = doc->Get(doc->Resources(0), "Font");
    const PdfValue* entry = fonts.Find("F1");
    if (!entry) return 0;
    const Font& f = cache.Get(*entry);
    size_t at = 0;
    Code code;
    size_t count = 0;
    while (f.Next(shown, at, code)) {
        if (code.bytes == 0 || code.bytes > 4 || ++count > shown.size()) __builtin_trap();
    }
    return 0;
}
