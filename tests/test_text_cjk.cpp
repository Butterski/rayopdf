// SPDX-License-Identifier: Apache-2.0
// Core tests of src/text/cjk and the Type0 font chain through it (exit codes 640-699): the
// generated Adobe collection and predefined CMap tables, and CJK fonts without ToUnicode built in
// memory.

#include "check.h"
#include "pdf_builder.h"

#include "pdf/document.h"
#include "text/cjk.h"
#include "text/cmap.h"
#include "text/font.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Text;
using namespace std::string_view_literals;

namespace {

bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

uint32_t CidOf(std::string_view cmap, uint32_t key) {
    uint32_t cid = 0xFFFFFFFF;
    if (!Cjk::Cid(Cjk::FindPredefined(cmap), key, cid)) return 0xFFFFFFFF;
    return cid;
}

void TestTables() {
    using Cjk::Collection;
    RAYOPDF_CHECK(Cjk::CollectionByName("Adobe", "Japan1") == Collection::Japan1, 640);
    RAYOPDF_CHECK(Cjk::CollectionByName("Adobe", "GB1") == Collection::GB1 &&
                      Cjk::CollectionByName("Adobe", "CNS1") == Collection::CNS1 &&
                      Cjk::CollectionByName("Adobe", "Korea1") == Collection::Korea1, 641);
    RAYOPDF_CHECK(Cjk::CollectionByName("Adobe", "Identity") == Collection::None &&
                      Cjk::CollectionByName("Other", "Japan1") == Collection::None, 642);

    // CID -> Unicode.
    RAYOPDF_CHECK(Cjk::CidToUnicode(Collection::Japan1, 1) == 0x20 &&
                      Cjk::CidToUnicode(Collection::Japan1, 843) == 0x3042, 643);
    RAYOPDF_CHECK(Cjk::CidToUnicode(Collection::Japan1, 264) == 'A', 644);       // half-width Roman (UCS2-HW)
    RAYOPDF_CHECK(Cjk::CidToUnicode(Collection::Japan1, 7887) == 0x3001, 645);   // vertical comma: the ordinary one
    RAYOPDF_CHECK(Cjk::CidToUnicode(Collection::Japan1, 13803) == 0x20B9F, 646); // supplementary plane
    RAYOPDF_CHECK(Cjk::CidText(Collection::Japan1, 13803) == "\xF0\xA0\xAE\x9F" &&
                      Cjk::CidText(Collection::Japan1, 843) == "\xE3\x81\x82", 647);
    RAYOPDF_CHECK(Cjk::CidToUnicode(Collection::GB1, 4559) == 0x4E2D &&
                      Cjk::CidToUnicode(Collection::CNS1, 661) == 0x4E2D, 648);
    RAYOPDF_CHECK(Cjk::CidToUnicode(Collection::Korea1, 3296) == 0xD55C, 649);
    RAYOPDF_CHECK(Cjk::CidToUnicode(Collection::Japan1, 0) == 0 && Cjk::CidToUnicode(Collection::Japan1, 999999) == 0 &&
                      Cjk::CidText(Collection::None, 1).empty(), 650);

    // Code -> CID through the legacy CMaps.
    RAYOPDF_CHECK(CidOf("90ms-RKSJ-H", 0x82A0) == 843 && CidOf("90ms-RKSJ-H", 0x93FA) == 3284, 651);
    RAYOPDF_CHECK(CidOf("90ms-RKSJ-H", 0x41) == 264 && CidOf("90ms-RKSJ-H", 0xB1) == 343, 652);
    RAYOPDF_CHECK(CidOf("90ms-RKSJ-H", 0x8141) == 634 && CidOf("90ms-RKSJ-V", 0x8141) == 7887, 653);
    RAYOPDF_CHECK(CidOf("90ms-RKSJ-V", 0x82A0) == 843 && Cjk::IsVertical(Cjk::FindPredefined("90ms-RKSJ-V")), 654);
    RAYOPDF_CHECK(CidOf("90ms-RKSJ-H", 0x05) == 231, 655);  // notdefrange
    RAYOPDF_CHECK(CidOf("90ms-RKSJ-H", 0x7FFF) == 0xFFFFFFFF, 656);
    RAYOPDF_CHECK(CidOf("90msp-RKSJ-H", 0x82A0) == 843 && CidOf("EUC-H", 0xA4A2) == 843 &&
                      CidOf("V", 0x2122) == 7887, 657);
    RAYOPDF_CHECK(CidOf("GBK-EUC-H", 0xD6D0) == 4559 && CidOf("GBpc-EUC-H", 0xD6D0) == 4559 &&
                      CidOf("GB-EUC-H", 0xD6D0) == 4559, 658);
    RAYOPDF_CHECK(CidOf("B5pc-H", 0xA4A4) == 661 && CidOf("ETen-B5-H", 0xA4A4) == 661 &&
                      CidOf("HKscs-B5-H", 0xA4A4) == 661, 659);
    RAYOPDF_CHECK(CidOf("KSCms-UHC-H", 0xC7D1) == 3296 && CidOf("KSC-EUC-H", 0xC7D1) == 3296, 660);

    // Unicode-based CMaps (keys are code points).
    RAYOPDF_CHECK(CidOf("UniJIS-UCS2-H", 0x3042) == 843 && CidOf("UniJIS-UTF16-H", 0x65E5) == 3284, 661);
    RAYOPDF_CHECK(CidOf("UniJIS-UTF16-V", 0x3001) == 7887 && CidOf("UniJIS-UCS2-V", 0x3042) == 843, 662);
    // UCS2-H and UTF32-H disagree on '|': the CMap's own run wins over the collection's UTF32-H.
    RAYOPDF_CHECK(CidOf("UniJIS-UCS2-H", 0x7C) == 93 && CidOf("UniJIS-UTF32-H", 0x7C) == 99, 663);
    RAYOPDF_CHECK(CidOf("UniJIS-UTF32-H", 0x20B9F) == 13803 && CidOf("UniJIS-UCS2-HW-H", 'A') == 264, 664);
    RAYOPDF_CHECK(CidOf("UniGB-UCS2-H", 0x4E2D) == 4559 && CidOf("UniCNS-UCS2-H", 0x4E2D) == 661 &&
                      CidOf("UniKS-UTF16-H", 0xD55C) == 3296, 665);
    // Code points several CIDs share in the CID -> Unicode direction still map back exactly.
    RAYOPDF_CHECK(CidOf("UniJIS-UTF32-H", 0xA0) == 1 && CidOf("UniJIS-UTF32-H", 0x20) == 1, 666);
    RAYOPDF_CHECK(Cjk::FindPredefined("Identity-H") < 0 && Cjk::FindPredefined("") < 0 &&
                      CidOf("Nope", 1) == 0xFFFFFFFF, 667);
    RAYOPDF_CHECK(Cjk::EncodingOf(Cjk::FindPredefined("UniKS-UTF8-H")) == Cjk::CMapEncoding::Utf8 &&
                      Cjk::CollectionOf(Cjk::FindPredefined("B5pc-V")) == Collection::CNS1, 668);

    // The codespace of a legacy CMap splits Shift-JIS strings.
    Codespace codespace;
    Cjk::AddCodespace(Cjk::FindPredefined("90ms-RKSJ-V"), codespace);
    uint32_t code = 0;
    const std::string_view bytes = "\x82\xA0" "A" "\xB1"sv;
    RAYOPDF_CHECK(codespace.Read(bytes, 0, code) == 2 && code == 0x82A0, 669);
    RAYOPDF_CHECK(codespace.Read(bytes, 2, code) == 1 && code == 'A', 670);
    RAYOPDF_CHECK(codespace.Read(bytes, 3, code) == 1 && code == 0xB1, 690);
}

struct Loaded {
    std::string bytes;
    std::unique_ptr<Document> doc;
    std::unique_ptr<FontCache> cache;

    const Font& Get(std::string_view resource) {
        const PdfValue& fonts = doc->Get(doc->Resources(0), "Font");
        const PdfValue* entry = fonts.Find(resource);
        static const PdfValue kNull{};
        return cache->Get(entry ? *entry : kNull);
    }
};

std::string Ref(int num) { return std::to_string(num) + " 0 R"; }

std::vector<Code> Codes(const Font& font, std::string_view bytes) {
    std::vector<Code> out;
    size_t at = 0;
    Code code;
    while (font.Next(bytes, at, code)) out.push_back(code);
    return out;
}

std::string Text(const std::vector<Code>& codes) {
    std::string out;
    for (const Code& c : codes) out.append(c.unicode.data(), c.unicode.size());
    return out;
}

// A Type0 font over a CIDFontType0 descendant (no embedded program).
std::string Type0(Test::PdfBuilder& b, std::string_view encoding, std::string_view ordering, std::string_view extra,
                  std::string_view toUnicode = {}) {
    std::string cid = "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /Ryumin-Light /DW 1000";
    if (!ordering.empty()) {
        cid += " /CIDSystemInfo << /Registry (Adobe) /Ordering (" + std::string(ordering) + ") /Supplement 2 >>";
    }
    cid += " " + std::string(extra) + " >>";
    std::string font = "<< /Type /Font /Subtype /Type0 /BaseFont /Ryumin-Light /Encoding " + std::string(encoding) +
                       " /DescendantFonts [" + Ref(b.Add(cid)) + "]";
    if (!toUnicode.empty()) font += " /ToUnicode " + Ref(b.AddStream("", std::string(toUnicode)));
    return font + " >>";
}

void TestFonts() {
    Loaded l;
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    std::string f;
    f += "/F1 " + Type0(b, "/90ms-RKSJ-H", "Japan1", "/W [1 [250] 264 [480] 843 [900]]");
    f += " /F2 " + Type0(b, "/90ms-RKSJ-V", "Japan1", "");
    f += " /F3 " + Type0(b, "/Identity-H", "Japan1", "");
    f += " /F4 " + Type0(b, "/Identity-H", "Japan1", "",
                         "1 begincodespacerange <0000> <FFFF> endcodespacerange 1 beginbfchar <0001> <0058> endbfchar");
    const int cmap = b.AddStream("/Type /CMap /CMapName /Custom-RKSJ /UseCMap /90ms-RKSJ-H",
                                 "/90ms-RKSJ-H usecmap\n1 begincidchar <82a0> 3284 endcidchar");
    f += " /F5 " + Type0(b, Ref(cmap), "Japan1", "");
    f += " /F6 " + Type0(b, "/UniJIS-UTF16-H", "Japan1", "/W [13803 [700]]");
    f += " /F7 " + Type0(b, "/UniJIS2004-UTF16-H", "Japan1", "/W [843 [650]]");
    f += " /F8 " + Type0(b, "/GBK-EUC-H", "", "");
    f += " /F9 " + Type0(b, "/KSCms-UHC-H", "Korea1", "");
    f += " /F10 " + Type0(b, "/B5pc-H", "CNS1", "/W [661 [800]]");
    f += " /F11 " + Type0(b, "/Identity-H", "Identity", "");
    const int stream = b.AddFlateStream("", "BT /F1 12 Tf (A) Tj ET");
    const int page = b.Add("<< /Type /Page /Parent " + Ref(pages) + " /MediaBox [0 0 612 792] /Resources << /Font << " +
                           f + " >> >> /Contents " + Ref(stream) + " >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + Ref(pages) + " >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + Ref(page) + "] /Count 1 >>");
    l.bytes = b.Build(catalog);
    RAYOPDF_CHECK(Document::Open(l.bytes, "", l.doc) == OpenStatus::Ok, 671);
    l.cache.reset(new FontCache(*l.doc));

    // Shift-JIS without ToUnicode: "日本語あA" through Adobe-Japan1, widths by CID.
    const Font& rksj = l.Get("F1");
    std::vector<Code> codes = Codes(rksj, "\x93\xFA\x96\x7B\x8C\xEA\x82\xA0" "A"sv);
    RAYOPDF_CHECK(codes.size() == 5 && Text(codes) == "日本語あA", 672);
    RAYOPDF_CHECK(codes[0].source == UnicodeSource::Collection && codes[4].source == UnicodeSource::Collection, 673);
    RAYOPDF_CHECK(codes[0].bytes == 2 && codes[4].bytes == 1 && codes[0].code == 0x93FA, 674);
    RAYOPDF_CHECK(Near(codes[0].width, 1.0) && Near(codes[3].width, 0.9) && Near(codes[4].width, 0.48), 675);
    RAYOPDF_CHECK(!rksj.Unmappable() && !rksj.Vertical() && Near(rksj.SpaceWidth(), 0.25), 676);

    const Font& vertical = l.Get("F2");
    codes = Codes(vertical, "\x81\x41\x82\xA0"sv);
    RAYOPDF_CHECK(vertical.Vertical() && Text(codes) == "、あ", 677);

    // Identity-H with an Adobe-Japan1 /CIDSystemInfo: codes are CIDs of the collection.
    codes = Codes(l.Get("F3"), "\x03\x4B\x0C\xD4"sv);
    RAYOPDF_CHECK(Text(codes) == "あ日" && codes[0].source == UnicodeSource::Collection, 678);
    RAYOPDF_CHECK(!l.Get("F3").Unmappable(), 691);

    // A ToUnicode that lacks a code: the collection fills it.
    codes = Codes(l.Get("F4"), "\x00\x01\x03\x4B"sv);
    RAYOPDF_CHECK(Text(codes) == "Xあ" && codes[0].source == UnicodeSource::ToUnicode &&
                      codes[1].source == UnicodeSource::Collection, 679);

    // An embedded CMap over a predefined one: its own entry first, then the predefined mapping.
    codes = Codes(l.Get("F5"), "\x82\xA0\x96\x7B" "A"sv);
    RAYOPDF_CHECK(codes.size() == 3 && Text(codes) == "日本A", 680);

    // UTF-16 with a surrogate pair: the supplementary character through the collection.
    codes = Codes(l.Get("F6"), "\xD8\x42\xDF\x9F\x30\x42"sv);
    RAYOPDF_CHECK(codes.size() == 2 && codes[0].bytes == 4 && Text(codes) == "\xF0\xA0\xAE\x9F" "あ", 681);
    RAYOPDF_CHECK(Near(codes[0].width, 0.7) && Near(codes[1].width, 1.0), 682);

    // A Unicode CMap the tables lack: text from the code, CIDs from the collection's UTF32-H.
    codes = Codes(l.Get("F7"), "\x30\x42"sv);
    RAYOPDF_CHECK(Text(codes) == "あ" && Near(codes[0].width, 0.65), 683);

    // Other collections; GBK-EUC-H without /CIDSystemInfo takes the CMap's collection.
    RAYOPDF_CHECK(Text(Codes(l.Get("F8"), "\xD6\xD0" "A"sv)) == "中A", 684);
    RAYOPDF_CHECK(Text(Codes(l.Get("F9"), "\xC7\xD1\xB1\xDB"sv)) == "한글", 685);
    codes = Codes(l.Get("F10"), "\xA4\xA4\xA4\xE5"sv);
    RAYOPDF_CHECK(Text(codes) == "中文" && Near(codes[0].width, 0.8), 686);

    // Adobe-Identity without ToUnicode or program stays unmapped.
    codes = Codes(l.Get("F11"), "\x03\x4B"sv);
    RAYOPDF_CHECK(codes[0].source == UnicodeSource::None && l.Get("F11").Unmappable(), 687);
}

} // namespace

int main() {
    TestTables();
    TestFonts();
    std::puts("test_text_cjk: ok");
    return 0;
}
