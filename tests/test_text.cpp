// SPDX-License-Identifier: Apache-2.0
// Core tests of src/text (exit codes 300-399): glyph names, CMaps, and fonts loaded from PDFs
// built in memory.

#include "check.h"
#include "pdf_builder.h"

#include "pdf/document.h"
#include "text/cmap.h"
#include "text/font.h"
#include "text/program.h"
#include "text/repair.h"
#include "text/tables.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Text;
using namespace std::string_view_literals;

namespace {

std::string Name(std::string_view glyph, bool* ok = nullptr) {
    std::string out;
    const bool found = GlyphNameToUnicode(glyph, out);
    if (ok) *ok = found;
    return out;
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

void TestGlyphNames() {
    bool ok = false;
    RAYOPDF_CHECK(Name("A") == "A", 300);
    RAYOPDF_CHECK(Name("Aacute") == "\xC3\x81", 301);
    RAYOPDF_CHECK(Name("uni0041") == "A", 302);
    RAYOPDF_CHECK(Name("uni00410042") == "AB", 303);
    RAYOPDF_CHECK(Name("u1F600") == "\xF0\x9F\x98\x80", 304);
    RAYOPDF_CHECK(Name("f_f_i") == "ffi", 305);
    RAYOPDF_CHECK(Name("a.sc") == "a", 306);
    Name("g12", &ok);
    RAYOPDF_CHECK(!ok, 307);
    Name("cid33", &ok);
    RAYOPDF_CHECK(!ok, 308);
    Name(".notdef", &ok);
    RAYOPDF_CHECK(!ok, 309);
    Name("uniD800", &ok);  // a surrogate is no scalar value
    RAYOPDF_CHECK(!ok, 310);
    RAYOPDF_CHECK(Name("dalethatafpatah") == "\xD7\x93\xD6\xB2", 311);  // two code points
    RAYOPDF_CHECK(Name("fi") == "\xEF\xAC\x81", 312);                     // ligatures stay as mapped
    RAYOPDF_CHECK(Name("Euro") == "\xE2\x82\xAC", 313);
    std::string zapf;
    RAYOPDF_CHECK(GlyphNameToUnicode("a20", true, zapf) && zapf == "\xE2\x9C\x94", 314);  // U+2714
    RAYOPDF_CHECK(Tables::StandardFontByName("Arial,Bold") == Tables::kHelvetica + 1, 315);
    RAYOPDF_CHECK(Tables::StandardFontByName("TimesNewRoman") == Tables::kTimesRoman, 316);
    RAYOPDF_CHECK(Tables::StandardFontByName("CourierNew") == Tables::kCourier, 317);
    uint16_t w = 0;
    RAYOPDF_CHECK(Tables::StandardWidth(Tables::kHelvetica, Tables::FindGlyphName("A"), w) && w == 667, 318);
    // Every name is found at its own index: by its bytes (a copy), which proves the names unique
    // and sorted, and as a view into the table (the offsets path).
    bool bytes = true, views = true;
    for (uint16_t i = 0; i < Tables::kGlyphNameCount; ++i) {
        const std::string copy(Tables::GlyphName(i));
        bytes = bytes && Tables::FindGlyphName(copy) == i;
        views = views && Tables::FindGlyphName(Tables::GlyphName(i)) == i;
    }
    RAYOPDF_CHECK(bytes, 353);
    RAYOPDF_CHECK(views, 354);
    // A view into the table that is not a whole name is looked up by its bytes.
    const std::string_view euro = Tables::GlyphName(Tables::FindGlyphName("Euro"));
    RAYOPDF_CHECK(Tables::FindGlyphName(euro.substr(0, 1)) == Tables::FindGlyphName("E"), 394);
}

void TestToUnicode() {
    const std::string cmap =
        "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        "/CMapName /Test def /CMapType 2 def\n"
        "2 begincodespacerange <00> <80> <8140> <9FFC> endcodespacerange\n"
        "5 beginbfchar\n"
        "<41> <0041>\n"
        "<42> <D840DC3E>\n"         // U+2003E through a surrogate pair
        "<43> <00660066>\n"         // ff as two code points
        "<44> /Euro\n"
        "<41> <0061>\n"             // a later definition wins
        "endbfchar\n"
        "3 beginbfrange\n"
        "<20> <22> <00FF>\n"        // the last unit carries: U+00FF, U+0100, U+0101
        "<8140> <8142> [<0066> <00660069> <0066006C>]\n"
        "<50> <52> <0050>\n"
        "endbfrange\n"
        "endcmap CMapName currentdict /CMap defineresource pop end end\n";
    ToUnicodeMap map;
    RAYOPDF_CHECK(ParseToUnicode(cmap, map), 320);
    const auto text = [&](uint32_t code) {
        const UnicodeEntry* e = map.Find(code);
        return e ? std::string(map.Text(*e)) : std::string("<none>");
    };
    RAYOPDF_CHECK(text(0x41) == "a", 321);
    RAYOPDF_CHECK(text(0x42) == "\xF0\xA0\x80\xBE", 322);
    RAYOPDF_CHECK(text(0x43) == "ff", 323);
    RAYOPDF_CHECK(text(0x44) == "\xE2\x82\xAC", 324);
    RAYOPDF_CHECK(text(0x20) == "\xC3\xBF" && text(0x21) == "\xC4\x80" && text(0x22) == "\xC4\x81", 325);
    RAYOPDF_CHECK(text(0x8140) == "f" && text(0x8141) == "fi" && text(0x8142) == "fl", 326);
    RAYOPDF_CHECK(text(0x51) == "Q" && text(0x53) == "<none>", 327);
    // Mixed codespaces: one byte below 0x81, two from 0x81.
    uint32_t code = 0;
    RAYOPDF_CHECK(map.codespace.Read("\x41\x81\x40", 0, code) == 1 && code == 0x41, 328);
    RAYOPDF_CHECK(map.codespace.Read("\x41\x81\x40", 1, code) == 2 && code == 0x8140, 329);
    RAYOPDF_CHECK(map.codespace.Read("\x81", 0, code) == 1, 330);  // cut short by the end
    // Malformed: unterminated strings, junk, stray delimiters, huge ranges. Never crashes; bounded.
    const char* const bad[] = {
        "beginbfchar <41> <00",
        "beginbfrange <0000> <FFFFFFFF> <0041> endbfrange",
        "beginbfrange <00> <10> [ <41> <42> endbfrange",
        "begincodespacerange <00> <FFFF> endcodespacerange beginbfchar ) ( } { >> << /x <zz> endbfchar",
        "beginbfchar <41> [ ] <42> 3 4 endbfchar beginbfrange <10> <05> <41> endbfrange",
        "",
        "beginbfrange",
    };
    for (const char* b : bad) {
        ToUnicodeMap m;
        ParseToUnicode(b, m);
        RAYOPDF_CHECK(m.entries.size() <= kMaxMappedCodes, 331);
    }
    ToUnicodeMap huge;
    ParseToUnicode("beginbfrange <00000000> <FFFFFFFF> <0041> endbfrange", huge);
    RAYOPDF_CHECK(huge.truncated && huge.entries.size() == kMaxRangeCodes, 332);
    // Entries of plain hex strings take a scanner of their own: the same map as the general lexer
    // gives, which reads them when every string has white space inside.
    const std::string plain = "beginbfchar <41> <0041> <42> <D840DC3E> <043> <0066006> <44> <> <0102030405> <41>\n"
                              "<45> <46> % comment\n<47> <48>\n<49>/Euro <4A> 66 endbfchar beginbfrange <20> <22> <00FF>"
                              " <30> <2F> <41> <50> <5> <0050> <60> <62> [<41> <42>] <70> <0170> <0041> <8> <8F> <0041>"
                              " endbfrange beginbfchar <10> <0041>";
    std::string spaced;
    for (size_t i = 0; i < plain.size(); ++i) {
        spaced += plain[i];
        if (plain[i] == '<') spaced += ' ';
    }
    ToUnicodeMap fast, general;
    ParseToUnicode(plain, fast);
    ParseToUnicode(spaced, general);
    bool same = fast.entries.size() == general.entries.size() && fast.entries.size() > 10 &&
                fast.truncated == general.truncated;
    for (size_t i = 0; same && i < fast.entries.size(); ++i) {
        same = fast.entries[i].code == general.entries[i].code && fast.Text(fast.entries[i]) == general.Text(general.entries[i]);
    }
    RAYOPDF_CHECK(same, 1454);
    // One-unit destinations: a single byte, two- and three-byte UTF-8, lone surrogates (U+FFFD),
    // and one-unit ranges running into the surrogates and wrapping past U+FFFF to U+0000.
    ToUnicodeMap units;
    ParseToUnicode("beginbfchar <01> <41> <02> <00E9> <03> <20AC> <04> <D800> <05> <DC00> endbfchar"
                   " beginbfrange <10> <12> <D7FF> <20> <21> <FFFF> endbfrange",
                   units);
    const auto unitText = [&](uint32_t c) {
        const UnicodeEntry* e = units.Find(c);
        return e ? std::string(units.Text(*e)) : std::string("<none>");
    };
    const std::string replacement = "\xEF\xBF\xBD";
    RAYOPDF_CHECK(unitText(1) == "A" && unitText(2) == "\xC3\xA9" && unitText(3) == "\xE2\x82\xAC" &&
                      unitText(4) == replacement && unitText(5) == replacement && unitText(0x10) == "\xED\x9F\xBF" &&
                      unitText(0x11) == replacement && unitText(0x12) == replacement &&
                      unitText(0x20) == "\xEF\xBF\xBF" && unitText(0x21) == std::string(1, '\0'),
                  1460);
}

void TestEncodingCMap() {
    const std::string cmap =
        "/CMapName /Test-V def /WMode 1 def\n"
        "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
        "2 begincidrange <0010> <001F> 100 <0100> <01FF> 500 endcidrange\n"
        "1 begincidchar <0005> 7 endcidchar\n"
        "1 beginnotdefrange <0000> <00FF> 1 endnotdefrange\n"
        "/Identity-H usecmap\n";
    EncodingCMap map;
    RAYOPDF_CHECK(ParseEncodingCMap(cmap, map), 335);
    uint32_t cid = 0;
    RAYOPDF_CHECK(map.Cid(0x12, cid) && cid == 102, 336);
    RAYOPDF_CHECK(map.Cid(0x5, cid) && cid == 7, 337);
    RAYOPDF_CHECK(map.Cid(0x150, cid) && cid == 580, 338);
    RAYOPDF_CHECK(map.Cid(0x30, cid) && cid == 1, 339);  // notdef range
    RAYOPDF_CHECK(!map.Cid(0x300, cid) && cid == 0, 340);
    RAYOPDF_CHECK(map.wmode == 1 && map.useCMap == "Identity-H", 341);
}

// ---- Font programs built in memory ---------------------------------------------------------------

void Put16(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v >> 8 & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}
void Put32(std::string& s, uint32_t v) {
    Put16(s, v >> 16);
    Put16(s, v & 0xFFFF);
}

// An sfnt with one format-4 cmap subtable (platform, encoding) mapping `first`.. to glyphs 1..,
// 'maxp', and a 'post' table: format 2 with `names` for glyphs 1.. when given, else format 3.
std::string Sfnt(uint16_t platform, uint16_t encoding, uint16_t first, uint16_t count,
                 const std::vector<std::string>& names) {
    std::string cmap;
    Put16(cmap, 0);
    Put16(cmap, 1);
    Put16(cmap, platform);
    Put16(cmap, encoding);
    Put32(cmap, 12);
    std::string sub;
    Put16(sub, 4);
    Put16(sub, 32);  // length: 14 + 4 * 2 * 2 + 2
    Put16(sub, 0);
    Put16(sub, 4);   // segCountX2
    Put16(sub, 4);
    Put16(sub, 1);
    Put16(sub, 0);
    Put16(sub, first + count - 1);
    Put16(sub, 0xFFFF);
    Put16(sub, 0);
    Put16(sub, first);
    Put16(sub, 0xFFFF);
    Put16(sub, static_cast<uint16_t>(1 - first));
    Put16(sub, 1);
    Put16(sub, 0);
    Put16(sub, 0);
    cmap += sub;
    std::string maxp;
    Put32(maxp, 0x00005000);
    Put16(maxp, count + 1u);
    std::string post;
    Put32(post, names.empty() ? 0x00030000 : 0x00020000);
    post.append(28, '\0');
    if (!names.empty()) {
        Put16(post, count + 1u);
        Put16(post, 0);
        std::string custom;
        uint32_t next = 258;
        for (const std::string& n : names) {
            uint32_t mac = 0;
            if (n == "A") mac = 36;
            if (mac) {
                Put16(post, mac);
            } else {
                Put16(post, next++);
                custom.push_back(static_cast<char>(n.size()));
                custom += n;
            }
        }
        post += custom;
    }
    const std::pair<const char*, std::string*> tables[] = {{"cmap", &cmap}, {"maxp", &maxp}, {"post", &post}};
    std::string out;
    Put32(out, 0x00010000);
    Put16(out, 3);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, 0);
    uint32_t offset = 12 + 3 * 16;
    std::string body;
    for (const auto& t : tables) {
        out.append(t.first, 4);
        Put32(out, 0);
        Put32(out, offset + static_cast<uint32_t>(body.size()));
        Put32(out, static_cast<uint32_t>(t.second->size()));
        body += *t.second;
        while (body.size() % 4) body.push_back('\0');
    }
    return out + body;
}

std::string CffIndex(const std::vector<std::string>& items) {
    std::string out;
    Put16(out, static_cast<uint32_t>(items.size()));
    if (items.empty()) return out;
    out.push_back(1);
    uint32_t at = 1;
    out.push_back(static_cast<char>(at));
    for (const std::string& item : items) {
        at += static_cast<uint32_t>(item.size());
        out.push_back(static_cast<char>(at));
    }
    for (const std::string& item : items) out += item;
    return out;
}

// A name-keyed CFF font of .notdef, "A" (SID 34) and "uni2022" (SID 391), with a custom
// encoding: code 0x61 -> glyph 1, 0x62 -> glyph 2.
std::string Cff() {
    const auto dict = [](uint32_t charset, uint32_t encoding, uint32_t charStrings) {
        std::string d;
        const auto op = [&](uint32_t value, char code) {
            d.push_back(29);
            Put32(d, value);
            d.push_back(code);
        };
        op(charset, 15);
        op(encoding, 16);
        op(charStrings, 17);
        return d;
    };
    const std::string header("\x01\x00\x04\x01", 4);
    const std::string names = CffIndex({"Test"});
    const std::string strings = CffIndex({"uni2022"});
    const std::string globals = CffIndex({});
    const size_t topSize = CffIndex({dict(0, 0, 0)}).size();
    const uint32_t charset = static_cast<uint32_t>(header.size() + names.size() + topSize + strings.size() + globals.size());
    std::string charsetData("\x00", 1);
    Put16(charsetData, 34);
    Put16(charsetData, 391);
    const uint32_t encoding = charset + static_cast<uint32_t>(charsetData.size());
    const std::string encodingData("\x00\x02\x61\x62", 4);
    const uint32_t charStrings = encoding + static_cast<uint32_t>(encodingData.size());
    return header + names + CffIndex({dict(charset, encoding, charStrings)}) + strings + globals + charsetData +
           encodingData + CffIndex({"\x0e", "\x0e", "\x0e"});
}

void TestPrograms() {
    Program::Sfnt sfnt;
    const std::string font = Sfnt(3, 0, 0xF041, 2, {"A", "uni00E9"});
    RAYOPDF_CHECK(Program::ReadSfnt(font, sfnt) && sfnt.glyphCount == 3, 345);
    const Program::CmapSubtable* symbol = sfnt.Cmap(3, 0);
    RAYOPDF_CHECK(symbol && symbol->Glyph(0xF041) == 1 && symbol->Glyph(0xF042) == 2 && symbol->Glyph(0x41) == 0, 346);
    Program::Names post;
    RAYOPDF_CHECK(Program::ReadPostNames(sfnt.post, 3, post) && post.Name(1) == "A" && post.Name(2) == "uni00E9", 347);
    Program::CffInfo cff;
    RAYOPDF_CHECK(Program::ReadCff(Cff(), cff) && !cff.cidKeyed && cff.glyphCount == 3, 348);
    RAYOPDF_CHECK(cff.glyphNames.Name(1) == "A" && cff.glyphNames.Name(2) == "uni2022", 349);
    RAYOPDF_CHECK(cff.encoding == 2 && cff.codeGlyphs[0x61] == 1 && cff.codeGlyphs[0x62] == 2, 350);
    Program::Type1Encoding type1;
    RAYOPDF_CHECK(Program::ReadType1Encoding("%!PS\n/Encoding 256 array 0 1 255 {1 index exch /.notdef put} for\n"
                                             "dup 65 /Omega put dup 66 /f_l put readonly def currentfile eexec XX",
                                             type1) &&
                      type1.codes.Name(65) == "Omega" && type1.codes.Name(66) == "f_l" && type1.codes.Name(67).empty(),
                  351);
    RAYOPDF_CHECK(Program::ReadType1Encoding("/Encoding StandardEncoding def", type1) && type1.standard, 352);
    // Truncated and corrupt programs fail without reading out of bounds.
    for (size_t cut = 0; cut < font.size(); cut += 7) Program::ReadSfnt(std::string_view(font).substr(0, cut), sfnt);
    const std::string cffData = Cff();
    for (size_t cut = 0; cut < cffData.size(); ++cut) Program::ReadCff(std::string_view(cffData).substr(0, cut), cff);
}

// ---- Fonts through Document ----------------------------------------------------------------------

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

// One page whose /Font resources are what `fonts` returns after adding its objects to `b`.
template <class AddFonts>
void Load(Loaded& out, AddFonts fonts, int code) {
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const std::string entries = fonts(b);
    const int stream = b.AddFlateStream("", "BT /F1 12 Tf (A) Tj ET");
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                           " 0 R /MediaBox [0 0 612 792] /Resources << /Font << " + entries + " >> >> /Contents " +
                           std::to_string(stream) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    out.bytes = b.Build(catalog);
    RAYOPDF_CHECK(Document::Open(out.bytes, "", out.doc) == OpenStatus::Ok, code);
    out.cache.reset(new FontCache(*out.doc));
}

std::string Ref(int num) { return std::to_string(num) + " 0 R"; }

// All codes of `bytes` with `font`.
std::vector<Code> Codes(const Font& font, std::string_view bytes) {
    std::vector<Code> out;
    size_t at = 0;
    Code code;
    while (font.Next(bytes, at, code)) out.push_back(code);
    return out;
}

void TestSimpleFonts() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        std::string f;
        f += "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
        f += " /F2 << /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding << /BaseEncoding /WinAnsiEncoding"
             " /Differences [65 /Euro /f_i 70 /g12] >> >>";
        f += " /F3 << /Type /Font /Subtype /TrueType /BaseFont /Arial,Bold /Encoding /WinAnsiEncoding >>";
        f += " /F4 << /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+CourierNew >>";
        const int toUnicode = b.AddStream("", "1 begincodespacerange <00> <FF> endcodespacerange\n"
                                              "2 beginbfchar <41> <0058> <42> <0000> endbfchar");
        f += " /F5 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar 65 /LastChar 66 /Widths [100 200]"
             " /FontDescriptor << /Type /FontDescriptor /Flags 32 /MissingWidth 333 /ItalicAngle -12 >> /ToUnicode " +
             Ref(toUnicode) + " >>";
        f += " /F6 " + Ref(b.Add("<< /Type /Font /Subtype /Type1 /BaseFont /TimesNewRoman >>"));
        return f;
    }, 355);

    const Font& helvetica = l.Get("F1");
    std::vector<Code> codes = Codes(helvetica, "A ");
    RAYOPDF_CHECK(codes.size() == 2 && codes[0].unicode == "A" && codes[0].source == UnicodeSource::Encoding, 356);
    RAYOPDF_CHECK(Near(codes[0].width, 0.667) && codes[1].wordSpace && Near(helvetica.SpaceWidth(), 0.278), 357);
    RAYOPDF_CHECK(helvetica.Kind() == FontKind::Type1 && !helvetica.Unmappable() && Near(helvetica.Ascent(), 0.718), 358);

    const Font& times = l.Get("F2");
    codes = Codes(times, "ABC\x80" "F");
    RAYOPDF_CHECK(codes[0].unicode == "\xE2\x82\xAC" && codes[1].unicode == "fi", 359);
    RAYOPDF_CHECK(codes[2].unicode == "C" && codes[3].unicode == "\xE2\x82\xAC", 360);
    // g12 means nothing: the code is guessed as WinAnsi.
    RAYOPDF_CHECK(codes[4].unicode == "F" && codes[4].source == UnicodeSource::Guess, 361);
    RAYOPDF_CHECK(Near(codes[2].width, 0.667) && !times.Style().bold, 362);  // Times-Roman C

    const Font& arial = l.Get("F3");
    codes = Codes(arial, "A");
    RAYOPDF_CHECK(Near(codes[0].width, 0.722) && arial.Style().bold && arial.BaseName() == "Arial,Bold", 363);

    const Font& courier = l.Get("F4");
    codes = Codes(courier, "Ai");
    RAYOPDF_CHECK(Near(codes[0].width, 0.6) && Near(codes[1].width, 0.6) && courier.Style().monospace, 364);
    RAYOPDF_CHECK(courier.BaseName() == "CourierNew" && codes[1].unicode == "i", 365);

    const Font& mapped = l.Get("F5");
    codes = Codes(mapped, "ABC");
    RAYOPDF_CHECK(codes[0].unicode == "X" && codes[0].source == UnicodeSource::ToUnicode, 366);
    // A ToUnicode entry to U+0000 counts as missing: the encoding (Standard) gives "B".
    RAYOPDF_CHECK(codes[1].unicode == "B" && codes[1].source == UnicodeSource::Encoding, 367);
    RAYOPDF_CHECK(Near(codes[0].width, 0.1) && Near(codes[1].width, 0.2) && Near(codes[2].width, 0.333), 368);
    RAYOPDF_CHECK(mapped.Style().italic, 369);

    const Font& timesNew = l.Get("F6");
    RAYOPDF_CHECK(Near(Codes(timesNew, "A")[0].width, 0.722), 370);
    RAYOPDF_CHECK(&l.Get("F6") == &timesNew, 371);  // loaded once
    const size_t count = l.cache->Count();
    l.Get("F6");
    RAYOPDF_CHECK(l.cache->Count() == count, 372);

    // Anything that is no font dictionary becomes the fallback.
    const Font& fallback = l.Get("Missing");
    codes = Codes(fallback, "a");
    RAYOPDF_CHECK(codes[0].unicode == "a" && codes[0].source == UnicodeSource::Guess && Near(codes[0].width, 0.5), 373);
    RAYOPDF_CHECK(fallback.Unmappable(), 374);
}

void TestType0AndType3() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        std::string f;
        const int toUnicode = b.AddStream("", "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                                              "2 beginbfchar <0001> <0048> <0003> <0020> endbfchar\n"
                                              "1 beginbfrange <0010> <0011> <0061> endbfrange");
        const std::string cidFont = "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /ABCDEF+Noto-Bold"
                                    " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"
                                    " /DW 1000 /W [1 [500 600 250] 16 17 444] >>";
        f += "/F1 << /Type /Font /Subtype /Type0 /BaseFont /ABCDEF+Noto-Bold /Encoding /Identity-H /DescendantFonts [" +
             Ref(b.Add(cidFont)) + "] /ToUnicode " + Ref(toUnicode) + " >>";
        // Identity-H without ToUnicode or program: no text.
        f += " /F2 << /Type /Font /Subtype /Type0 /BaseFont /Bare /Encoding /Identity-H /DescendantFonts [<< /Type /Font"
             " /Subtype /CIDFontType0 /BaseFont /Bare /DW 700 >>] >>";
        f += " /F3 << /Type /Font /Subtype /Type3 /FontMatrix [0.01 0 0 0.01 0 0] /FontBBox [0 -20 80 90]"
             " /FirstChar 1 /LastChar 2 /Widths [50 75] /Encoding << /Differences [1 /H /i] >>"
             " /CharProcs << >> /Resources << >> >>";
        f += " /F4 << /Type /Font /Subtype /Type0 /BaseFont /STSong /Encoding /UniGB-UCS2-H /DescendantFonts [<< /Type"
             " /Font /Subtype /CIDFontType0 /BaseFont /STSong /DW 1000 >>] >>";
        const int cmap = b.AddStream("/Type /CMap /CMapName /Custom /WMode 1",
                                     "2 begincodespacerange <00> <7F> <8000> <FFFF> endcodespacerange\n"
                                     "1 begincidrange <20> <7F> 1 endcidrange 1 begincidchar <8001> 200 endcidchar");
        f += " /F5 << /Type /Font /Subtype /Type0 /BaseFont /Custom /Encoding " + Ref(cmap) +
             " /DescendantFonts [<< /Type /Font /Subtype /CIDFontType0 /BaseFont /Custom /W [200 [300]]"
             " /DW2 [880 -900] /W2 [200 200 -500 400 800] >>] >>";
        return f;
    }, 375);

    const Font& type0 = l.Get("F1");
    std::vector<Code> codes = Codes(type0, "\x00\x01\x00\x02\x00\x03\x00\x11\x00\x09\x05"sv);
    RAYOPDF_CHECK(codes.size() == 6 && codes[0].bytes == 2 && codes[0].code == 1, 376);
    RAYOPDF_CHECK(codes[0].unicode == "H" && codes[0].source == UnicodeSource::ToUnicode && Near(codes[0].width, 0.5), 377);
    RAYOPDF_CHECK(codes[1].source == UnicodeSource::None && codes[1].unicode.empty() && Near(codes[1].width, 0.6), 378);
    RAYOPDF_CHECK(codes[2].unicode == " " && Near(codes[2].width, 0.25) && !codes[2].wordSpace, 379);
    RAYOPDF_CHECK(codes[3].unicode == "b" && Near(codes[3].width, 0.444), 380);
    RAYOPDF_CHECK(Near(codes[4].width, 1.0), 381);                                // DW
    RAYOPDF_CHECK(codes[5].bytes == 1 && codes[5].code == 5, 382);                // trailing partial code
    RAYOPDF_CHECK(type0.Kind() == FontKind::Type0 && type0.Style().bold && Near(type0.SpaceWidth(), 0.25), 383);
    RAYOPDF_CHECK(!type0.Unmappable() && !type0.Vertical() && type0.BaseName() == "Noto-Bold", 384);

    const Font& bare = l.Get("F2");
    codes = Codes(bare, "\x00\x41"sv);
    RAYOPDF_CHECK(codes[0].source == UnicodeSource::None && Near(codes[0].width, 0.7) && bare.Unmappable(), 385);

    const Font& type3 = l.Get("F3");
    codes = Codes(type3, "\x01\x02");
    RAYOPDF_CHECK(codes[0].unicode == "H" && codes[1].unicode == "i" && Near(codes[0].width, 0.5), 386);
    RAYOPDF_CHECK(Near(codes[1].width, 0.75) && Near(type3.Matrix()[0], 0.01) && Near(type3.Ascent(), 0.9), 387);
    RAYOPDF_CHECK(type3.Kind() == FontKind::Type3 && Near(type3.Descent(), -0.2), 388);

    const Font& ucs2 = l.Get("F4");
    codes = Codes(ucs2, "\x4E\x2D\x00\x41"sv);
    RAYOPDF_CHECK(codes.size() == 2 && codes[0].unicode == "\xE4\xB8\xAD" && codes[0].source == UnicodeSource::Collection, 389);
    RAYOPDF_CHECK(codes[1].unicode == "A" && !ucs2.Unmappable(), 390);

    const Font& custom = l.Get("F5");
    codes = Codes(custom, "\x41\x80\x01\xFF"sv);
    RAYOPDF_CHECK(codes.size() == 3 && codes[0].bytes == 1 && codes[1].bytes == 2 && codes[1].code == 0x8001, 391);
    RAYOPDF_CHECK(custom.Vertical() && Near(codes[1].width, 0.3) && Near(codes[1].verticalAdvance, -0.5), 392);
    RAYOPDF_CHECK(Near(codes[0].verticalAdvance, -0.9) && codes[2].bytes == 1, 393);  // 0xFF: shortest length
}

void TestEmbeddedPrograms() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        std::string f;
        const int tt = b.AddStream("", Sfnt(3, 0, 0xF041, 2, {"A", "uni00E9"}));
        f += "/F1 << /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+Wingish /FirstChar 65 /LastChar 66"
             " /Widths [500 600] /FontDescriptor << /Type /FontDescriptor /Flags 4 /FontFile2 " + Ref(tt) + " >> >>";
        const int cid = b.AddStream("", Sfnt(3, 1, 0x48, 2, {}));
        f += " /F2 << /Type /Font /Subtype /Type0 /BaseFont /Sub /Encoding /Identity-H /DescendantFonts [<< /Type /Font"
             " /Subtype /CIDFontType2 /BaseFont /Sub /CIDToGIDMap /Identity /FontDescriptor << /Flags 4 /FontFile2 " +
             Ref(cid) + " >> >>] >>";
        const int cff = b.AddStream("/Subtype /Type1C", Cff());
        f += " /F3 << /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+Cffish /FontDescriptor << /Flags 4 /FontFile3 " +
             Ref(cff) + " >> >>";
        const std::string clear = "%!PS-AdobeFont-1.0: T1\n/Encoding 256 array 0 1 255 {1 index exch /.notdef put} for\n"
                                  "dup 65 /Omega put dup 66 /f_l put readonly def\ncurrentfile eexec\n";
        const int t1 = b.AddStream("/Length1 " + std::to_string(clear.size()) + " /Length2 0 /Length3 0", clear);
        f += " /F4 << /Type /Font /Subtype /Type1 /BaseFont /T1 /FontDescriptor << /Flags 4 /FontFile " + Ref(t1) + " >> >>";
        // Type0 fonts whose ToUnicode lacks a code: one the /W lists, one mapped to U+FFFD.
        const int partial = b.AddStream("", "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                                            "1 beginbfchar <0001> <005A> endbfchar");
        const int broken = b.AddStream("", "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
                                           "2 beginbfchar <0001> <005A> <0002> <FFFD> endbfchar");
        for (const auto& [name, toUnicode, widths] :
             {std::tuple<const char*, int, const char*>{"F5", partial, " /W [1 [500 600]]"}, {"F6", broken, ""}}) {
            f += std::string(" /") + name + " << /Type /Font /Subtype /Type0 /BaseFont /Sub /Encoding /Identity-H"
                 " /ToUnicode " + Ref(toUnicode) + " /DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Sub"
                 " /CIDToGIDMap /Identity" + widths + " /FontDescriptor << /Flags 4 /FontFile2 " + Ref(cid) + " >> >>] >>";
        }
        return f;
    }, 395);

    std::vector<Code> codes = Codes(l.Get("F1"), "AB");
    RAYOPDF_CHECK(codes[0].unicode == "A" && codes[0].source == UnicodeSource::FontProgram && codes[1].unicode == "\xC3\xA9", 396);
    codes = Codes(l.Get("F2"), "\x00\x01\x00\x02"sv);
    RAYOPDF_CHECK(codes[0].unicode == "H" && codes[1].unicode == "I" && codes[0].source == UnicodeSource::FontProgram, 397);
    RAYOPDF_CHECK(!l.Get("F2").Unmappable(), 398);
    codes = Codes(l.Get("F3"), "ab");
    RAYOPDF_CHECK(codes[0].unicode == "A" && codes[1].unicode == "\xE2\x80\xA2" && codes[0].source == UnicodeSource::FontProgram, 399);
    codes = Codes(l.Get("F4"), "AB");
    RAYOPDF_CHECK(codes[0].unicode == "\xE2\x84\xA6" && codes[1].unicode == "fl", 319);
    // A ToUnicode that lacks a code: the program fills it (§3.1); the mapped code keeps ToUnicode.
    codes = Codes(l.Get("F5"), "\x00\x01\x00\x02"sv);
    RAYOPDF_CHECK(codes[0].unicode == "Z" && codes[0].source == UnicodeSource::ToUnicode, 333);
    RAYOPDF_CHECK(codes[1].unicode == "I" && codes[1].source == UnicodeSource::FontProgram, 334);
    RAYOPDF_CHECK(Near(codes[1].width, 0.6), 343);
    codes = Codes(l.Get("F6"), "\x00\x01\x00\x02"sv);
    RAYOPDF_CHECK(codes[0].unicode == "Z" && codes[1].unicode == "I", 342);
    RAYOPDF_CHECK(codes[1].source == UnicodeSource::FontProgram, 344);
}

// A font with a trusted ToUnicode and /Widths reads its program only when a shown code needs it
// (or SpaceWidth asks): the texts are those of an eager load, and views handed out before stay valid.
void TestLazyPrograms() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        const std::string clear = "%!PS-AdobeFont-1.0: T1\n/Encoding 256 array 0 1 255 {1 index exch /.notdef put} for\n"
                                  "dup 32 /space put dup 65 /Omega put dup 66 /f_l put readonly def\ncurrentfile eexec\n";
        const int t1 = b.AddStream("/Length1 " + std::to_string(clear.size()) + " /Length2 0 /Length3 0", clear);
        const int toUnicode = b.AddStream("", "1 begincodespacerange <00> <FF> endcodespacerange\n"
                                              "1 beginbfchar <41> <0058> endbfchar");
        std::string widths = "250";
        for (int c = 33; c <= 66; ++c) widths += c == 65 ? " 500" : c == 66 ? " 600" : " 0";
        std::string f;
        for (const char* name : {"F1", "F2"}) {
            f += std::string(" /") + name + " << /Type /Font /Subtype /Type1 /BaseFont /T1 /FirstChar 32 /Widths [" +
                 widths + "] /ToUnicode " + Ref(toUnicode) + " /FontDescriptor << /Flags 4 /FontFile " + Ref(t1) +
                 " >> >>";
        }
        return f;
    }, 1450);

    const Font& font = l.Get("F1");
    const std::vector<Code> first = Codes(font, "A");
    std::vector<Code> codes = Codes(font, "BA");
    RAYOPDF_CHECK(codes.size() == 2 && codes[0].unicode == "fl" && codes[0].source == UnicodeSource::FontProgram &&
                      Near(codes[0].width, 0.6), 1451);
    RAYOPDF_CHECK(codes[1].unicode == "X" && codes[1].source == UnicodeSource::ToUnicode && first[0].unicode == "X", 1452);
    // The space's width comes from the program's /space at code 32, which the ToUnicode lacks.
    RAYOPDF_CHECK(Near(l.Get("F2").SpaceWidth(), 0.25) && Near(font.SpaceWidth(), 0.25), 1453);
}

// /Widths read only for the codes it covers: /FirstChar below 0 or past 255, a real, a reference
// and a name among the widths; /MissingWidth everywhere else.
void TestWidthRanges() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        const std::string descriptor = " /FontDescriptor << /Type /FontDescriptor /Flags 32 /MissingWidth 333 >>";
        std::string f;
        f += "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar 250 /Widths [100 200.5 " +
             Ref(b.Add("300")) + " /x 400 500 600 700]" + descriptor + " >>";
        f += " /F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar -2 /Widths [1 2 3 4]" + descriptor + " >>";
        f += " /F3 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar 300 /Widths [5]" + descriptor + " >>";
        return f;
    }, 1455);

    const std::vector<Code> high = Codes(l.Get("F1"), "\xF9\xFA\xFB\xFC\xFD\xFE\xFF");
    RAYOPDF_CHECK(high.size() == 7 && Near(high[0].width, 0.333) && Near(high[1].width, 0.1) &&
                      Near(high[2].width, 0.2005) && Near(high[3].width, 0.3) && Near(high[4].width, 0.333) &&
                      Near(high[5].width, 0.4) && Near(high[6].width, 0.5),
                  1456);
    const std::vector<Code> low = Codes(l.Get("F2"), std::string_view("\x00\x01\x02", 3));
    const std::vector<Code> beyond = Codes(l.Get("F3"), "A");
    RAYOPDF_CHECK(low.size() == 3 && Near(low[0].width, 0.003) && Near(low[1].width, 0.004) &&
                      Near(low[2].width, 0.333) && beyond.size() == 1 && Near(beyond[0].width, 0.333),
                  1457);
}

// Texts Repair::RepairText rewrites, through every font path that repairs: a base encoding's
// private-use glyph (Symbol's parenlefttp, U+F8EB) and ToUnicode entries of a tab and U+F8EB.
void TestRepairedTexts() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        std::string f;
        f += "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Symbol >>";
        const int toUnicode = b.AddStream("", "1 begincodespacerange <00> <FF> endcodespacerange\n"
                                              "3 beginbfchar <41> <0009> <42> <F8EB> <43> <0041> endbfchar");
        f += " /F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /FirstChar 65 /Widths [500 500 500] /ToUnicode " +
             Ref(toUnicode) + " >>";
        return f;
    }, 1458);
    const std::vector<Code> symbol = Codes(l.Get("F1"), "\xE6");
    const std::vector<Code> mapped = Codes(l.Get("F2"), "ABC");
    RAYOPDF_CHECK(symbol.size() == 1 && symbol[0].unicode == "\xE2\x8E\x9B" && mapped.size() == 3 &&
                      mapped[0].unicode == " " && mapped[1].unicode == "\xE2\x8E\x9B" && mapped[2].unicode == "A",
                  1459);
}

// One-byte texts: a ToUnicode's DEL and C0 control are unusable (the encoding's text then), its
// printable byte usable; Repair::Count of one byte as of any text.
void TestOneByteTexts() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        const int toUnicode = b.AddStream("", "1 begincodespacerange <00> <FF> endcodespacerange\n"
                                              "3 beginbfchar <41> <7F> <42> <7E> <43> <1F> endbfchar");
        return "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode " + Ref(toUnicode) + " >>";
    }, 1461);
    const std::vector<Code> codes = Codes(l.Get("F1"), "ABC");
    RAYOPDF_CHECK(codes.size() == 3 && codes[0].unicode == "A" && codes[0].source == UnicodeSource::Encoding &&
                      codes[1].unicode == "~" && codes[1].source == UnicodeSource::ToUnicode && codes[2].unicode == "C",
                  1461);
    Repair::MapStats stats;
    for (const char* text : {"A", "~", " ", "\x7F", "\x80", "\x1F", "z", "@"}) Repair::Count(text, stats);
    RAYOPDF_CHECK(stats.entries == 8 && stats.latinLetters == 2 && stats.junk == 2 && stats.accented == 0 && !stats.thai,
                  1462);
}

// Slots viewing the pool's copies of a ToUnicode's texts and of the base encoding's texts, with a
// text of its own after them (the WinAnsi guess), and the space found among them.
void TestPooledTexts() {
    Loaded l;
    Load(l, [](Test::PdfBuilder& b) {
        const int toUnicode = b.AddStream("", "1 begincodespacerange <00> <FF> endcodespacerange\n"
                                              "2 beginbfchar <41> <0058> <42> <00660069> endbfchar");
        return "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding << /BaseEncoding /WinAnsiEncoding"
               " /Differences [67 /g12] >> /ToUnicode " + Ref(toUnicode) + " >>";
    }, 1463);
    const Font& font = l.Get("F1");
    const std::vector<Code> codes = Codes(font, "ABC\xE9 D");
    RAYOPDF_CHECK(codes.size() == 6 && codes[0].unicode == "X" && codes[1].unicode == "fi" &&
                      codes[2].unicode == "C" && codes[2].source == UnicodeSource::Guess &&
                      codes[3].unicode == "\xC3\xA9" && codes[3].source == UnicodeSource::Encoding &&
                      codes[4].unicode == " " && codes[5].unicode == "D" && Near(font.SpaceWidth(), 0.278),
                  1463);
}

} // namespace

int main() {
    TestGlyphNames();
    TestToUnicode();
    TestEncodingCMap();
    TestPrograms();
    TestSimpleFonts();
    TestType0AndType3();
    TestEmbeddedPrograms();
    TestLazyPrograms();
    TestWidthRanges();
    TestRepairedTexts();
    TestOneByteTexts();
    TestPooledTexts();
    std::puts("test_text: ok");
    return 0;
}
