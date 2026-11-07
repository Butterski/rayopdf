// SPDX-License-Identifier: Apache-2.0
// Core tests of src/text/repair and the font chain's handling of wrong or junk ToUnicode maps
// (exit codes 820-849): whitespace controls, private-use symbol and Thai forms, untrustworthy maps,
// numeric glyph names.

#include "check.h"
#include "pdf_builder.h"

#include "pdf/document.h"
#include "text/font.h"
#include "text/repair.h"
#include "text/tables.h"

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

std::string Repaired(std::string_view text, const Repair::Context& context) {
    std::string out;
    return Repair::RepairText(text, context, out) ? out : std::string(text);
}

void TestRepairText() {
    const Repair::Context none;
    RAYOPDF_CHECK(Repaired("\t", none) == " " && Repaired("\r\n", none) == " " && Repaired("a\tb", none) == "a b", 820);
    std::string out = "keep";
    RAYOPDF_CHECK(!Repair::RepairText("plain", none, out) && out == "keep", 821);
    // Adobe's Symbol pieces in the corporate-use area (AGL parenlefttp, bracerightbt).
    std::string agl;
    RAYOPDF_CHECK(GlyphNameToUnicode("parenlefttp", agl) && agl == "\xEF\xA3\xAB", 822);
    RAYOPDF_CHECK(Repaired(agl, none) == "\xE2\x8E\x9B" && Repaired("\xEF\xA3\xBE", none) == "\xE2\x8E\xAD", 823);
    // Symbol fonts in U+F0xx: the byte through the Symbol encoding.
    Repair::Context symbol = Repair::ContextForName("SymbolMT");
    RAYOPDF_CHECK(symbol.symbol && !symbol.wingdings && !Repair::ContextForName("OpenSymbol").symbol, 824);
    RAYOPDF_CHECK(Repaired("\xEF\x82\xB7", symbol) == "\xE2\x80\xA2" &&
                      Repaired("\xEF\x81\xA1", symbol) == "\xCE\xB1",
                  825);
    RAYOPDF_CHECK(Repaired("\xEF\x82\xB7", none) == "\xEF\x82\xB7", 826);  // no context: unchanged
    // Symbol's bracket pieces come out as the Unicode pieces (F0E6 parenlefttp -> U+239B).
    RAYOPDF_CHECK(Repaired("\xEF\x83\xA6", symbol) == "\xE2\x8E\x9B", 827);
    const Repair::Context wingdings = Repair::ContextForName("Wingdings-Regular");
    RAYOPDF_CHECK(Repaired("\xEF\x81\xAE", wingdings) == "\xE2\x96\xA0" &&
                      Repaired("\xEF\x83\xBC", wingdings) == "\xE2\x9C\x93",
                  828);
    // Thai presentation forms only with Thai context.
    Repair::Context thai;
    thai.thai = true;
    RAYOPDF_CHECK(Repaired("\xEF\x9C\x8B", thai) == "\xE0\xB9\x89" &&
                      Repaired("\xEF\x9C\x8B", none) == "\xEF\x9C\x8B",
                  829);
    RAYOPDF_CHECK(Repair::IsJunk(0xF0B7) && Repair::IsJunk(7) && Repair::IsJunk(0xFFFD) && !Repair::IsJunk('\t') &&
                      !Repair::IsJunk(0x2022) && Repair::IsJunk(0x85),
                  830);
}

void TestUntrusted() {
    Repair::MapStats stats;
    for (int i = 0; i < 6; ++i) Repair::Count("\xEF\x80\x80", stats);
    RAYOPDF_CHECK(!Repair::Untrusted(stats), 831);  // too small to judge
    for (int i = 0; i < 6; ++i) Repair::Count("a", stats);
    RAYOPDF_CHECK(stats.entries == 12 && stats.junk == 6 && !Repair::Untrusted(stats), 832);
    Repair::Count("\xEF\x80\x80", stats);
    RAYOPDF_CHECK(Repair::Untrusted(stats), 833);  // junk majority
    Repair::MapStats accented;
    const char* letters[] = {"\xC3\xA1", "\xC3\xA9", "\xC3\xAD", "\xC3\xB3", "\xC3\xBA", "\xC3\xA4", "a", "e"};
    for (int i = 0; i < 40; ++i) Repair::Count(letters[i % 8], accented);
    RAYOPDF_CHECK(Repair::Untrusted(accented), 834);  // 75 % accented Latin letters
    Repair::MapStats latin1;  // a map of the whole Latin-1 range: 57 % accented, trusted
    for (uint32_t cp = 'A'; cp <= 0xFF; ++cp) {
        if (cp > 'Z' && cp < 'a') continue;
        if (cp > 'z' && cp < 0xC0) continue;
        std::string t;
        if (cp < 0x80) t.push_back(static_cast<char>(cp));
        else t = std::string{static_cast<char>(0xC0 | (cp >> 6)), static_cast<char>(0x80 | (cp & 0x3F))};
        Repair::Count(t, latin1);
    }
    RAYOPDF_CHECK(!Repair::Untrusted(latin1), 848);
    Repair::MapStats plain;
    for (int i = 0; i < 40; ++i) Repair::Count(i % 10 == 0 ? "\xC3\xA9" : "e", plain);
    plain.largestShare = 4;
    RAYOPDF_CHECK(!Repair::Untrusted(plain), 835);
    plain.largestShare = 30;
    RAYOPDF_CHECK(Repair::Untrusted(plain), 836);  // most codes onto one text
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

void Put16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v >> 8 & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

// A TrueType program of `glyphs` glyphs with only a 'maxp' table: no cmap, no glyph names.
std::string BareSfnt(uint16_t glyphs) {
    std::string out;
    Put16(out, 1), Put16(out, 0);  // version 1.0
    Put16(out, 1), Put16(out, 16), Put16(out, 0), Put16(out, 0);
    out += "maxp";
    Put16(out, 0), Put16(out, 0);          // checksum
    Put16(out, 0), Put16(out, 12 + 16);    // offset
    Put16(out, 0), Put16(out, 6);          // length
    Put16(out, 0), Put16(out, 0x5000);     // version 0.5
    Put16(out, glyphs);
    return out;
}

std::vector<Code> Codes(const Font& font, std::string_view bytes) {
    std::vector<Code> out;
    size_t at = 0;
    Code code;
    while (font.Next(bytes, at, code)) out.push_back(code);
    return out;
}

void TestFonts() {
    Loaded l;
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    std::string f;
    // Word's ToUnicode mapping the space glyph to a tab.
    const int tab = b.AddStream("", "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                                    "2 beginbfchar <0003> <0009> <0004> <0041> endbfchar");
    f += "/F1 << /Type /Font /Subtype /Type0 /BaseFont /ABCDEF+Calibri /Encoding /Identity-H /ToUnicode " + Ref(tab) +
         " /DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Calibri /DW 500 >>] >>";
    // A Symbol font whose ToUnicode maps the bullet into U+F0B7.
    const int bullet = b.AddStream("", "1 begincodespacerange <00> <FF> endcodespacerange\n"
                                       "1 beginbfchar <B7> <F0B7> endbfchar");
    f += " /F2 << /Type /Font /Subtype /TrueType /BaseFont /SymbolMT /FirstChar 183 /LastChar 183 /Widths [460]"
         " /ToUnicode " + Ref(bullet) + " >>";
    // A ToUnicode that maps 20 codes onto "x": untrustworthy; the encoding names win.
    std::string collapse = "1 begincodespacerange <00> <FF> endcodespacerange\n20 beginbfchar";
    for (int c = 0x41; c < 0x55; ++c) {
        char entry[24];
        std::snprintf(entry, sizeof entry, " <%02X> <0078>", c);
        collapse += entry;
    }
    collapse += " endbfchar\n1 beginbfchar <01> <0079> endbfchar";
    const int collapsed = b.AddStream("", collapse);
    f += " /F3 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode " + Ref(collapsed) + " >>";
    // A Type 3 font whose glyph names are character codes.
    f += " /F4 << /Type /Font /Subtype /Type3 /FontMatrix [0.001 0 0 0.001 0 0] /FontBBox [0 0 1000 1000]"
         " /FirstChar 0 /LastChar 5 /Widths [500 500 500 500 500 500] /Encoding << /Differences [0 /72 /101 /108 /108"
         " /111 /33] >> /CharProcs << >> /Resources << >> >>";
    // ... and one whose numbers are glyph indices (values below 32): not read as characters.
    f += " /F5 << /Type /Font /Subtype /Type3 /FontMatrix [0.001 0 0 0.001 0 0] /FontBBox [0 0 1000 1000]"
         " /FirstChar 0 /LastChar 5 /Widths [500 500 500 500 500 500] /Encoding << /Differences [0 /1 /2 /3 /4 /5"
         " /65] >> /CharProcs << >> /Resources << >> >>";
    // A Wingdings font without any text: its known bullet positions.
    f += " /F6 << /Type /Font /Subtype /TrueType /BaseFont /Wingdings /FirstChar 110 /LastChar 110 /Widths [600]"
         " /FontDescriptor << /Type /FontDescriptor /FontName /Wingdings /Flags 4 >> >>";
    // A Thai font whose ToUnicode maps its shifted tone mark into U+F70B.
    const int thai = b.AddStream("", "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                                     "3 beginbfchar <0001> <0E19> <0002> <0E44> <0003> <F70B> endbfchar");
    f += " /F7 << /Type /Font /Subtype /Type0 /BaseFont /CordiaNew /Encoding /Identity-H /ToUnicode " + Ref(thai) +
         " /DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /CordiaNew >>] >>";
    // A subset without cmap, names or ToUnicode whose widths show the Macintosh glyph order.
    const int bare = b.AddStream("", BareSfnt(300));
    f += " /F8 << /Type /Font /Subtype /Type0 /BaseFont /ABCDEF+ArialMT /Encoding /Identity-H /DescendantFonts [<<"
         " /Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEF+ArialMT /CIDToGIDMap /Identity /W [36 [667 667 722 722"
         " 667 611 778 722 278 500]] /FontDescriptor << /Flags 32 /FontFile2 " + Ref(bare) + " >> >>] >>";
    // ... and one whose widths do not.
    f += " /F9 << /Type /Font /Subtype /Type0 /BaseFont /ABCDEF+ArialMT /Encoding /Identity-H /DescendantFonts [<<"
         " /Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEF+ArialMT /CIDToGIDMap /Identity /W [36 [500 500 500 500"
         " 500 500 500 500 500 500]] /FontDescriptor << /Flags 32 /FontFile2 " + Ref(bare) + " >> >>] >>";
    const int stream = b.AddFlateStream("", "BT /F1 12 Tf (A) Tj ET");
    const int page = b.Add("<< /Type /Page /Parent " + Ref(pages) + " /MediaBox [0 0 612 792] /Resources << /Font << " +
                           f + " >> >> /Contents " + Ref(stream) + " >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + Ref(pages) + " >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + Ref(page) + "] /Count 1 >>");
    l.bytes = b.Build(catalog);
    RAYOPDF_CHECK(Document::Open(l.bytes, "", l.doc) == OpenStatus::Ok, 837);
    l.cache.reset(new FontCache(*l.doc));

    std::vector<Code> codes = Codes(l.Get("F1"), "\x00\x03\x00\x04"sv);
    RAYOPDF_CHECK(codes.size() == 2 && codes[0].unicode == " " && codes[0].source == UnicodeSource::ToUnicode &&
                      codes[1].unicode == "A",
                  838);
    codes = Codes(l.Get("F2"), "\xB7");
    RAYOPDF_CHECK(codes[0].unicode == "\xE2\x80\xA2" && codes[0].source == UnicodeSource::ToUnicode, 839);
    codes = Codes(l.Get("F3"), "AB\x01");
    RAYOPDF_CHECK(codes[0].unicode == "A" && codes[0].source == UnicodeSource::Encoding && codes[1].unicode == "B",
                  840);
    // A code only the untrusted map gives keeps its text, as a guess.
    RAYOPDF_CHECK(codes[2].unicode == "y" && codes[2].source == UnicodeSource::Guess, 841);
    codes = Codes(l.Get("F4"), "\x00\x01\x02\x03\x04\x05"sv);
    std::string hello;
    for (const Code& c : codes) hello.append(c.unicode.data(), c.unicode.size());
    RAYOPDF_CHECK(hello == "Hello!" && codes[0].source == UnicodeSource::Guess, 842);
    codes = Codes(l.Get("F5"), "\x00\x01"sv);
    RAYOPDF_CHECK(codes[0].unicode.empty() && codes[1].unicode.empty(), 843);
    codes = Codes(l.Get("F6"), "n");
    RAYOPDF_CHECK(codes[0].unicode == "\xE2\x96\xA0" && codes[0].source == UnicodeSource::Encoding, 844);
    codes = Codes(l.Get("F7"), "\x00\x01\x00\x03\x00\x02"sv);
    RAYOPDF_CHECK(codes.size() == 3 && codes[1].unicode == "\xE0\xB9\x89" &&
                      codes[1].source == UnicodeSource::ToUnicode,
                  845);
    codes = Codes(l.Get("F8"), "\x00\x24\x00\x2C"sv);
    RAYOPDF_CHECK(codes[0].unicode == "A" && codes[1].unicode == "I" && codes[0].source == UnicodeSource::Guess, 846);
    codes = Codes(l.Get("F9"), "\x00\x24"sv);
    RAYOPDF_CHECK(codes[0].unicode.empty() && codes[0].source == UnicodeSource::None, 847);
}

} // namespace

int main() {
    TestRepairText();
    TestUntrusted();
    TestFonts();
    std::puts("test_text_repair: ok");
    return 0;
}
