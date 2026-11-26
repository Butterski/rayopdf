// SPDX-License-Identifier: Apache-2.0
// Core tests of glyph-shape recovery (exit codes 1350-1399): the rasterizer (raster/raster.h), the
// outline readers of TrueType, CFF and Type 1 programs (text/outline.h), and whole conversions
// whose fonts have no Unicode (TrueType, CFF, Type 1 and Type 3 fonts built in memory) recovered
// through the stand-in add-on tests/ocr_mock_addon.cpp in its mode "glyphs", which reads a glyph
// drawn as k vertical bars as "a", "b", "c" or the ligature "fi" (layout/ocr_glyphs.h): the
// ligature split, the rejection gates, ActualText left alone, isolated letters from symbol glyphs
// dropped, and a page of vector-drawn text.
#include "check.h"
#include "pdf_builder.h"

#include "layout/convert.h"
#include "layout/interpreter.h"
#include "layout/ocr_glyphs.h"
#include "pdf/document.h"
#include "raster/raster.h"
#include "text/font.h"
#include "text/outline.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef RAYOPDF_OCR_MOCK
#error "RAYOPDF_OCR_MOCK names the stand-in add-on (CMakeLists.txt)"
#endif

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

const char* const kMock = RAYOPDF_OCR_MOCK;
const char* const kNoAddon = "/nonexistent/rayopdf-ocr-missing";

void Mode(const char* mode) {
#ifdef _WIN32
    _putenv_s("RAYOPDF_OCR_MOCK_MODE", mode);
#else
    setenv("RAYOPDF_OCR_MOCK_MODE", mode, 1);
#endif
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void Show(const char* name, const std::string& md) {
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", name, md.c_str());
}

// ---------------------------------------------------------------- bytes

void U16(std::string& out, uint32_t v) {
    out += static_cast<char>(v >> 8 & 0xFF);
    out += static_cast<char>(v & 0xFF);
}

void U32(std::string& out, uint32_t v) {
    U16(out, v >> 16);
    U16(out, v & 0xFFFF);
}

// Glyph k (1-4) of the test fonts: k vertical bars, each kBar units wide and kHeight high, kSpace
// apart, from x = kLeft.
constexpr int kBar = 80, kSpace = 120, kHeight = 700, kLeft = 50, kAdvance = 800;

// ---------------------------------------------------------------- TrueType

// A simple glyph of `bars` rectangles (on-curve points, coordinates as words).
std::string BarGlyf(int bars) {
    std::string g;
    U16(g, static_cast<uint32_t>(bars));
    U16(g, kLeft), U16(g, 0), U16(g, kLeft + bars * kBar + (bars - 1) * kSpace), U16(g, kHeight);
    for (int b = 0; b < bars; ++b) U16(g, static_cast<uint32_t>(4 * b + 3));
    U16(g, 0);  // no instructions
    const int n = 4 * bars;
    for (int i = 0; i < n; ++i) g += '\x01';  // on curve, x and y as words
    int x = 0;
    std::vector<int> xs, ys;
    for (int b = 0; b < bars; ++b) {
        const int x0 = kLeft + b * (kBar + kSpace), x1 = x0 + kBar;
        const int px[4] = {x0, x0, x1, x1}, py[4] = {0, kHeight, kHeight, 0};
        for (int k = 0; k < 4; ++k) xs.push_back(px[k]), ys.push_back(py[k]);
    }
    for (int v : xs) U16(g, static_cast<uint32_t>(static_cast<uint16_t>(v - x))), x = v;
    int y = 0;
    for (int v : ys) U16(g, static_cast<uint32_t>(static_cast<uint16_t>(v - y))), y = v;
    if (g.size() % 2) g += '\0';
    return g;
}

// An sfnt of .notdef, glyphs 1-4 (bars) and glyph 5, a composite of glyph 2 moved by (30, 10);
// a (1,0) cmap maps codes 1-5 to glyphs 1-5.
std::string TrueTypeFont() {
    std::vector<std::string> glyphs = {"", BarGlyf(1), BarGlyf(2), BarGlyf(3), BarGlyf(4)};
    std::string composite;
    U16(composite, 0xFFFF);  // -1 contours
    U16(composite, 0), U16(composite, 0), U16(composite, 1000), U16(composite, 1000);
    U16(composite, 0x0003), U16(composite, 2), U16(composite, 30), U16(composite, 10);
    glyphs.push_back(composite);
    std::string glyf, loca;
    for (const std::string& g : glyphs) {
        U16(loca, static_cast<uint32_t>(glyf.size() / 2));
        glyf += g;
    }
    U16(loca, static_cast<uint32_t>(glyf.size() / 2));
    std::string head(54, '\0');
    head[18] = 0x03, head[19] = static_cast<char>(0xE8);  // unitsPerEm 1000; indexToLocFormat 0
    std::string maxp(6, '\0');
    maxp[0] = 0, maxp[1] = 1, maxp[5] = static_cast<char>(glyphs.size());
    std::string cmap;
    U16(cmap, 0), U16(cmap, 1), U16(cmap, 1), U16(cmap, 0), U32(cmap, 12);
    U16(cmap, 0), U16(cmap, 262), U16(cmap, 0);  // format 0
    for (int c = 0; c < 256; ++c) cmap += static_cast<char>(c >= 1 && c <= 5 ? c : 0);
    const std::pair<const char*, std::string*> tables[] = {{"cmap", &cmap}, {"glyf", &glyf}, {"head", &head},
                                                           {"loca", &loca}, {"maxp", &maxp}};
    std::string font;
    U32(font, 0x00010000), U16(font, 5), U16(font, 64), U16(font, 2), U16(font, 16);
    uint32_t offset = 12 + 16 * 5;
    std::string data;
    for (const auto& t : tables) {
        font += t.first;
        U32(font, 0), U32(font, offset + static_cast<uint32_t>(data.size())), U32(font, static_cast<uint32_t>(t.second->size()));
        data += *t.second;
        while (data.size() % 4) data += '\0';
    }
    return font + data;
}

// ---------------------------------------------------------------- CFF

// A charstring number (Type 2: 28 and a 16-bit integer).
void T2Number(std::string& out, int v) {
    out += '\x1C';
    U16(out, static_cast<uint32_t>(static_cast<uint16_t>(v)));
}

// One bar from the current point moved by (dx, dy): rmoveto, three rlinetos, ending at its top left.
std::string T2Bar(int dx, int dy) {
    std::string s;
    T2Number(s, dx), T2Number(s, dy), s += '\x15';  // rmoveto
    T2Number(s, kBar), T2Number(s, 0), s += '\x05';
    T2Number(s, 0), T2Number(s, kHeight), s += '\x05';
    T2Number(s, -kBar), T2Number(s, 0), s += '\x05';
    return s;
}

std::string Index(const std::vector<std::string>& items) {
    std::string out;
    U16(out, static_cast<uint32_t>(items.size()));
    if (items.empty()) return out;
    out += '\x04';
    uint32_t offset = 1;
    U32(out, offset);
    for (const std::string& item : items) U32(out, offset += static_cast<uint32_t>(item.size()));
    for (const std::string& item : items) out += item;
    return out;
}

void DictInt(std::string& out, uint32_t v) {
    out += '\x1D';
    U32(out, v);
}

// A name-keyed CFF: .notdef and glyph1-glyph4 (names in the String INDEX), bars; glyph 2 draws
// its second bar through a local subroutine, glyph 3 its third through a global one.
std::string CffFont() {
    const std::vector<std::string> strings = {"glyph1", "glyph2", "glyph3", "glyph4"};
    std::string notdef = "\x0E";
    std::vector<std::string> glyphs = {notdef};
    for (int bars = 1; bars <= 4; ++bars) {
        std::string s;
        T2Number(s, kAdvance);  // the width
        for (int b = 0; b < bars; ++b) {
            const int dx = b == 0 ? kLeft : kBar + kSpace, dy = b == 0 ? 0 : -kHeight;
            if (bars == 2 && b == 1) {
                T2Number(s, -107), s += '\x0A';  // callsubr 0
            } else if (bars == 3 && b == 2) {
                T2Number(s, -107), s += '\x1D';  // callgsubr 0
            } else {
                s += T2Bar(dx, dy);
            }
        }
        s += '\x0E';
        glyphs.push_back(s);
    }
    const std::string subr = T2Bar(kBar + kSpace, -kHeight) + '\x0B';  // return
    const std::string header("\x01\x00\x04\x04", 4);
    const std::string names = Index({"T"});
    const std::string stringIndex = Index(strings);
    const std::string globalSubrs = Index({subr});
    std::string charset("\x00", 1);
    for (uint32_t sid = 391; sid < 395; ++sid) U16(charset, sid);
    const std::string charStrings = Index(glyphs);
    const std::string localSubrs = Index({subr});
    std::string privateDict;
    DictInt(privateDict, 6), privateDict += '\x13';  // Subrs right after the 6-byte DICT
    // Top DICT: charset, CharStrings, Private; offsets fixed width so its size is known first.
    const auto top = [&](uint32_t charsetAt, uint32_t charStringsAt, uint32_t privateAt) {
        std::string d;
        DictInt(d, charsetAt), d += '\x0F';
        DictInt(d, charStringsAt), d += '\x11';
        DictInt(d, static_cast<uint32_t>(privateDict.size())), DictInt(d, privateAt), d += '\x12';
        return d;
    };
    const size_t topSize = Index({top(0, 0, 0)}).size();
    const uint32_t charsetAt = static_cast<uint32_t>(header.size() + names.size() + topSize + stringIndex.size() +
                                                     globalSubrs.size());
    const uint32_t charStringsAt = charsetAt + static_cast<uint32_t>(charset.size());
    const uint32_t privateAt = charStringsAt + static_cast<uint32_t>(charStrings.size());
    return header + names + Index({top(charsetAt, charStringsAt, privateAt)}) + stringIndex + globalSubrs + charset +
           charStrings + privateDict + localSubrs;
}

// ---------------------------------------------------------------- Type 1

void T1Number(std::string& out, int v) {
    if (v >= -107 && v <= 107) {
        out += static_cast<char>(v + 139);
        return;
    }
    out += '\xFF';
    U32(out, static_cast<uint32_t>(v));
}

std::string Encrypt(std::string_view plain, uint16_t key) {
    std::string out;
    uint16_t r = key;
    for (const char ch : plain) {
        const uint8_t c = static_cast<uint8_t>(static_cast<uint8_t>(ch) ^ (r >> 8));
        r = static_cast<uint16_t>((c + r) * 52845u + 22719u);
        out += static_cast<char>(c);
    }
    return out;
}

// A Type 1 program: glyph1-glyph4 of bars, each bar after the first through Subrs 0.
std::string Type1Font() {
    std::string subr;
    // closepath leaves the current point at the bar's top left (FreeType's reading).
    T1Number(subr, kBar + kSpace), T1Number(subr, -kHeight), subr += '\x15';  // rmoveto
    T1Number(subr, kBar), subr += '\x06';                              // hlineto
    T1Number(subr, kHeight), subr += '\x07';                           // vlineto
    T1Number(subr, -kBar), subr += '\x06';
    subr += '\x09', subr += '\x0B';  // closepath return
    std::string priv = "dup /Private 8 dict dup begin /lenIV 4 def\n/Subrs 1 array\n";
    const auto entry = [](const std::string& charstring) {
        const std::string encrypted = Encrypt(std::string(4, 'x') + charstring, 4330);
        return std::to_string(encrypted.size()) + " RD " + encrypted;
    };
    priv += "dup 0 " + entry(subr) + " NP\nND\n2 index /CharStrings 4 dict dup begin\n";
    for (int bars = 1; bars <= 4; ++bars) {
        std::string s;
        T1Number(s, 0), T1Number(s, kAdvance), s += '\x0D';  // hsbw
        T1Number(s, kLeft), T1Number(s, 0), s += '\x15';
        T1Number(s, kBar), s += '\x06';
        T1Number(s, kHeight), s += '\x07';
        T1Number(s, -kBar), s += '\x06';
        s += '\x09';
        for (int b = 1; b < bars; ++b) T1Number(s, 0), s += '\x0A';  // callsubr 0
        s += '\x0E';
        priv += "/glyph" + std::to_string(bars) + " " + entry(s) + " ND\n";
    }
    priv += "end\nend\n";
    return "%!FontType1-1.0: T\n/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n/Encoding StandardEncoding def\n"
           "currentfile eexec\n" +
           Encrypt(std::string("abcd") + priv, 55665) + std::string(64, '0') + "\ncleartomark\n";
}

// ---------------------------------------------------------------- documents

enum class Kind { TrueType, Cff, Type1, Type3 };

// The text of the pages: words of codes 1-4 (bars), code 32 a space.
const char* const kWords[] = {"\x01\x02\x03", "\x02\x01\x04\x03", "\x03\x03\x01", "\x04\x02", "\x01\x01\x02\x03"};

std::string ShowWords(int line) {
    std::string bytes;
    for (int w = 0; w < 5; ++w) {
        if (w) bytes += ' ';
        bytes += kWords[(w + line) % 5];
    }
    std::string hex = "<";
    static const char kHex[] = "0123456789ABCDEF";
    for (unsigned char c : bytes) hex += kHex[c >> 4], hex += kHex[c & 15];
    return hex + ">";
}

// One page of `lines` lines of bar words in font /F1 of `kind`, its codes without Unicode
// (/Differences names glyph1-glyph4 that mean nothing). `extra` is added to the content.
std::string MakeDocument(Kind kind, int lines, const std::string& extra = "", const std::string& toUnicode = "") {
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const std::string encoding = "/Encoding << /Type /Encoding /Differences [1 /glyph1 /glyph2 /glyph3 /glyph4 32 /space] >>";
    std::string font = "<< /Type /Font /FirstChar 1 /LastChar 32 /Widths [";
    for (int c = 1; c <= 32; ++c) font += c <= 4 ? "800 " : c == 32 ? "300 " : "0 ";
    font += "] " + encoding;
    if (!toUnicode.empty()) font += " /ToUnicode " + std::to_string(b.AddStream("", toUnicode)) + " 0 R";
    const std::string descriptor = "/Type /FontDescriptor /FontName /T /Flags 32 /FontBBox [0 0 1000 1000] "
                                   "/ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 700 /StemV 80";
    if (kind == Kind::Type3) {
        std::string procs;
        for (int bars = 1; bars <= 4; ++bars) {
            std::string content = "800 0 d0\n";
            for (int k = 0; k < bars; ++k)
                content += std::to_string(kLeft + k * (kBar + kSpace)) + " 0 " + std::to_string(kBar) + " " +
                           std::to_string(kHeight) + " re f\n";
            procs += "/glyph" + std::to_string(bars) + " " + std::to_string(b.AddStream("", content)) + " 0 R ";
        }
        procs += "/space " + std::to_string(b.AddStream("", "300 0 d0")) + " 0 R";
        font += " /Subtype /Type3 /FontMatrix [0.001 0 0 0.001 0 0] /FontBBox [0 0 1000 1000] /CharProcs << " + procs +
                " >> /Resources << >> >>";
    } else {
        std::string file;
        if (kind == Kind::TrueType) {
            file = "/FontFile2 " + std::to_string(b.AddStream("", TrueTypeFont())) + " 0 R";
            font += " /Subtype /TrueType /BaseFont /T";
        } else if (kind == Kind::Cff) {
            file = "/FontFile3 " + std::to_string(b.AddStream("/Subtype /Type1C", CffFont())) + " 0 R";
            font += " /Subtype /Type1 /BaseFont /T";
        } else {
            file = "/FontFile " + std::to_string(b.AddStream("", Type1Font())) + " 0 R";
            font += " /Subtype /Type1 /BaseFont /T";
        }
        font += " /FontDescriptor " + std::to_string(b.Add("<< " + descriptor + " " + file + " >>")) + " 0 R >>";
    }
    const int fontObject = b.Add(font);
    std::string content = "BT /F1 20 Tf 72 720 Td\n";
    for (int l = 0; l < lines; ++l) content += ShowWords(l) + " Tj 0 -30 Td\n";
    content += "ET\n" + extra;
    const int stream = b.AddFlateStream("", content);
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                           " 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " + std::to_string(fontObject) +
                           " 0 R /F2 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> /Contents " +
                           std::to_string(stream) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    return b.Build(catalog);
}

std::string ConvertWith(const std::string& pdf, const char* addon, ConvertReport& report) {
    std::unique_ptr<Document> doc;
    RAYOPDF_CHECK(Document::Open(pdf, "", doc) == OpenStatus::Ok, 1370);
    ConvertOptions options;
    options.ocr.addon = addon;
    std::string md;
    Convert(*doc, options, md, report);
    return md;
}

// The words of line `l` as the mock reads them.
std::string ReadWords(int line) {
    std::string out;
    for (int w = 0; w < 5; ++w) {
        if (w) out += ' ';
        for (const char c : std::string(kWords[(w + line) % 5])) out += c == 1 ? "a" : c == 2 ? "b" : c == 3 ? "c" : "fi";
    }
    return out;
}

// ---------------------------------------------------------------- tests

void TestRasterizer() {
    // A 10 x 10 square from (2.5, 2.5): inside black, its edges half covered, its corners a quarter.
    std::string px(16 * 16, static_cast<char>(0xFF));
    Raster::Path square;
    square.AddRect(2.5f, 2.5f, 10, 10);
    Raster::Rasterizer raster;
    raster.Fill(square, Raster::Matrix(), Raster::FillRule::NonZero, reinterpret_cast<uint8_t*>(&px[0]), 16, 16);
    const auto at = [&](int x, int y) { return static_cast<int>(static_cast<uint8_t>(px[size_t(y) * 16 + x])); };
    RAYOPDF_CHECK(at(5, 5) == 0 && at(0, 0) == 255 && at(15, 15) == 255, 1350);
    RAYOPDF_CHECK(at(2, 6) >= 126 && at(2, 6) <= 130 && at(6, 12) >= 126 && at(6, 12) <= 130, 1350);
    RAYOPDF_CHECK(at(2, 2) >= 190 && at(2, 2) <= 193, 1350);
    // Two nested squares of one direction: non-zero fills the hole, even-odd leaves it.
    Raster::Path nested;
    nested.AddRect(0, 0, 16, 16);
    nested.AddRect(4, 4, 8, 8);
    std::string nonZero(256, static_cast<char>(0xFF)), evenOdd = nonZero;
    raster.Fill(nested, Raster::Matrix(), Raster::FillRule::NonZero, reinterpret_cast<uint8_t*>(&nonZero[0]), 16, 16);
    raster.Fill(nested, Raster::Matrix(), Raster::FillRule::EvenOdd, reinterpret_cast<uint8_t*>(&evenOdd[0]), 16, 16);
    RAYOPDF_CHECK(nonZero[8 * 16 + 8] == 0 && static_cast<uint8_t>(evenOdd[8 * 16 + 8]) == 255 && evenOdd[1 * 16 + 1] == 0,
                  1351);
    // Hostile coordinates (NaN, huge) draw nothing and do not crash; a curve far off the canvas too.
    Raster::Path wild;
    wild.MoveTo(1e30f, -1e30f);
    wild.CubicTo(0.0f / 0.0f, 3, 1e38f, 1e38f, 5, 5);
    wild.LineTo(-1e20f, 8);
    std::string untouched(256, static_cast<char>(0xFF));
    raster.Fill(wild, Raster::Matrix(), Raster::FillRule::NonZero, reinterpret_cast<uint8_t*>(&untouched[0]), 16, 16);
    RAYOPDF_CHECK(!raster.Overflowed(), 1351);
}

// The bounds of an outline, in its own units.
bool Bounds(const Raster::Path& path, float& x0, float& y0, float& x1, float& y1) {
    return path.Bounds(x0, y0, x1, y1);
}

void TestOutlines() {
    float x0, y0, x1, y1;
    // TrueType: glyph 3 (three bars), and the composite of glyph 2 moved by (30, 10).
    const std::string tt = TrueTypeFont();
    Text::Program::Sfnt sfnt;
    Text::Program::GlyfOutlines glyf;
    RAYOPDF_CHECK(Text::Program::ReadSfnt(tt, sfnt) && Text::Program::ReadGlyfOutlines(sfnt, glyf) && glyf.glyphCount == 6 &&
                      glyf.unitsPerEm == 1000,
                  1352);
    Raster::Path path;
    RAYOPDF_CHECK(Text::Program::GlyfOutline(glyf, 3, Raster::Matrix(), path) && Bounds(path, x0, y0, x1, y1) &&
                      x0 == kLeft && x1 == kLeft + 3 * kBar + 2 * kSpace && y0 == 0 && y1 == kHeight,
                  1352);
    path.Clear();
    RAYOPDF_CHECK(Text::Program::GlyfOutline(glyf, 5, Raster::Matrix(), path) && Bounds(path, x0, y0, x1, y1) &&
                      x0 == kLeft + 30 && y0 == 10 && y1 == kHeight + 10,
                  1352);
    path.Clear();
    RAYOPDF_CHECK(!Text::Program::GlyfOutline(glyf, 99, Raster::Matrix(), path), 1352);

    // CFF: a local and a global subroutine.
    const std::string cffData = CffFont();  // the outlines view it
    Text::Program::CffOutlines cff;
    RAYOPDF_CHECK(Text::Program::ReadCffOutlines(cffData, cff) && cff.charStrings.size() == 5, 1353);
    RAYOPDF_CHECK(Text::Program::CffGlyphByName(cff, "glyph3") == 3 && Text::Program::CffGlyphByName(cff, "a") < 0, 1353);
    for (int bars = 1; bars <= 4; ++bars) {
        path.Clear();
        RAYOPDF_CHECK(Text::Program::CffGlyphOutline(cff, static_cast<uint32_t>(bars), Raster::Matrix(), path) &&
                          Bounds(path, x0, y0, x1, y1) && x0 == kLeft &&
                          x1 == kLeft + bars * kBar + (bars - 1) * kSpace && y1 == kHeight,
                      1353);
    }
    // Type 1: eexec, lenIV, Subrs.
    Text::Program::Type1Outlines t1;
    RAYOPDF_CHECK(Text::Program::ReadType1Outlines(Type1Font(), t1) && t1.charStrings.size() == 4 && t1.subrs.size() == 1 &&
                      t1.matrix[0] == 0.001,
                  1354);
    path.Clear();
    RAYOPDF_CHECK(Text::Program::Type1GlyphOutline(t1, "glyph4", Raster::Matrix(), path) && Bounds(path, x0, y0, x1, y1) &&
                      x0 == kLeft && x1 == kLeft + 4 * kBar + 3 * kSpace && y1 == kHeight,
                  1354);
    path.Clear();
    RAYOPDF_CHECK(!Text::Program::Type1GlyphOutline(t1, "nothing", Raster::Matrix(), path), 1354);
    // Truncated programs fail without reading out of bounds.
    for (size_t cut : {size_t(5), size_t(40), size_t(120)}) {
        Text::Program::CffOutlines c;
        Text::Program::Type1Outlines t;
        Text::Program::Sfnt s;
        const std::string head = cffData.substr(0, cut);
        Text::Program::ReadCffOutlines(head, c);
        Text::Program::ReadType1Outlines(Type1Font().substr(0, cut), t);
        Text::Program::ReadSfnt(tt.substr(0, cut), s);
    }
}

void TestFontGlyphs() {
    // The fonts of the documents: codes reach the glyphs.
    for (const Kind kind : {Kind::TrueType, Kind::Cff, Kind::Type1, Kind::Type3}) {
        const std::string pdf = MakeDocument(kind, 1);
        std::unique_ptr<Document> doc;
        RAYOPDF_CHECK(Document::Open(pdf, "", doc) == OpenStatus::Ok, 1355);
        Text::FontCache fonts(*doc);
        PageData page;
        PageStats stats;
        std::vector<GlyphUse> uses;
        stats.uses = &uses;
        InterpretPage(*doc, fonts, 0, AnnotationMode::None, page, stats);
        RAYOPDF_CHECK(!uses.empty() && uses.size() == page.glyphs.size() && uses[0].code == 1 && stats.unmapped > 0, 1355);
        Text::FontGlyphs glyphs;
        const Text::FontGlyphs::Kind loaded = glyphs.Load(*doc, *uses[0].font);
        RAYOPDF_CHECK(loaded != Text::FontGlyphs::Kind::None, 1355);
        uint32_t g1 = 0, g4 = 0;
        RAYOPDF_CHECK(glyphs.GlyphOf(1, 1, g1) && glyphs.GlyphOf(4, 1, g4) && g1 != g4 && !glyphs.GlyphOf(9, 1, g1), 1355);
        if (kind == Kind::Type3) {
            RAYOPDF_CHECK(loaded == Text::FontGlyphs::Kind::Type3 && glyphs.Procedure(g4) != nullptr, 1355);
        } else {
            Raster::Path path;
            float x0, y0, x1, y1;
            // Text space at size 1: the bars of glyph 4 end at 0.73 em.
            RAYOPDF_CHECK(glyphs.Outline(g4, path) && path.Bounds(x0, y0, x1, y1) && x1 > 0.72f && x1 < 0.74f &&
                              y1 > 0.69f && y1 < 0.71f,
                          1355);
        }
    }
}

void TestRecovery() {
    for (const Kind kind : {Kind::TrueType, Kind::Cff, Kind::Type1, Kind::Type3}) {
        const std::string pdf = MakeDocument(kind, 6);
        // Without an add-on: the page says it has no text, nothing is recovered.
        ConvertReport none;
        const std::string without = ConvertWith(pdf, kNoAddon, none);
        Show("without", without);
        RAYOPDF_CHECK(Contains(without, "<!-- rayopdf: page 1 ") && none.ocrGlyphFonts == 0 && none.ocrGlyphs == 0, 1356);
        // With the add-on: every line reads as its bars, the ligature glyph as "fi".
        Mode("glyphs");
        ConvertReport report;
        const std::string md = ConvertWith(pdf, kMock, report);
        Show("recovered", md);
        for (int l = 0; l < 6; ++l) RAYOPDF_CHECK(Contains(md, ReadWords(l)), 1357);
        RAYOPDF_CHECK(Contains(md, "bafic") && !Contains(md, "<!-- rayopdf") && report.ocrGlyphFonts == 1 &&
                          report.ocrGlyphs > 0,
                      1358);
        // Low confidence: the font is not accepted; the output is as without the add-on.
        Mode("low-conf");
        ConvertReport low;
        RAYOPDF_CHECK(ConvertWith(pdf, kMock, low) == without && low.ocrGlyphFonts == 0, 1359);
    }
    Mode("ok");
}

void TestActualText() {
    // A span with ActualText over the first line keeps it; the rest is recovered.
    Mode("glyphs");
    ConvertReport report;
    const std::string spanPdf = MakeDocument(
        Kind::Type3, 0,
        "BT /F1 20 Tf 72 400 Td /Span << /ActualText (Replaced words here) >> BDC " + ShowWords(0) + " Tj EMC 0 -30 Td " +
            ShowWords(1) + " Tj 0 -30 Td " + ShowWords(2) + " Tj 0 -30 Td " + ShowWords(3) + " Tj ET\n");
    const std::string md = ConvertWith(spanPdf, kMock, report);
    Show("actualtext", md);
    RAYOPDF_CHECK(Contains(md, "Replaced words here") && !Contains(md, ReadWords(0)) && Contains(md, ReadWords(1)) &&
                      report.ocrGlyphFonts == 1,
                  1360);
    Mode("ok");
}

void TestSymbols() {
    // A font whose ToUnicode maps code 1 (one bar) to a private-use character, the others to
    // letters: only code 1 is examined. Standing alone (a bullet before a space), its reading "a"
    // is dropped; inside a word (letters on both sides) it is kept.
    Mode("glyphs");
    const std::string cmap =
        "/CIDInit /ProcSet findresource begin 12 dict begin begincmap 1 begincodespacerange <00> <FF> "
        "endcodespacerange 5 beginbfchar <01> <E000> <02> <0062> <03> <0063> <04> <0064> <20> <0020> endbfchar "
        "endcmap CMapName currentdict /CMap defineresource pop end end";
    // Lines of "<01> <02><03><04>": code 1 alone, then a word; or code 1 inside "<02><01><03>".
    std::string alone, glued;
    for (int l = 0; l < 6; ++l) {
        alone += "<0120020304020304> Tj 0 -30 Td\n";
        glued += "<020103040203040120> Tj 0 -30 Td\n";
    }
    for (int variant = 0; variant < 2; ++variant) {
        const std::string text = "BT /F1 20 Tf 72 400 Td\n" + (variant == 0 ? alone : glued) + "ET\n";
        const std::string pdf = MakeDocument(Kind::Type3, 0, text, cmap);
        ConvertReport report;
        const std::string md = ConvertWith(pdf, kMock, report);
        Show(variant == 0 ? "alone" : "glued", md);
        if (variant == 0)
            RAYOPDF_CHECK(Contains(md, "\xEE\x80\x80") && report.ocrGlyphFonts == 0, 1361);
        else
            RAYOPDF_CHECK(Contains(md, "bacdbcda") && !Contains(md, "\xEE\x80\x80") && report.ocrGlyphFonts == 1, 1362);
    }
    Mode("ok");
}

void TestVectorText() {
    // 400 glyph-sized filled curves in rows and no text: painted and recognised as a page.
    std::string content = "0 g\n";
    for (int i = 0; i < 400; ++i) {
        const int x = 60 + 7 * (i % 70), y = 700 - 40 * (i / 70);
        content += std::to_string(x) + " " + std::to_string(y) + " m " + std::to_string(x + 5) + " " +
                   std::to_string(y) + " " + std::to_string(x + 5) + " " + std::to_string(y + 9) + " " +
                   std::to_string(x) + " " + std::to_string(y + 9) + " c f\n";
    }
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int stream = b.AddFlateStream("", content);
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                           " 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents " + std::to_string(stream) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string pdf = b.Build(catalog);
    ConvertReport none;
    const std::string without = ConvertWith(pdf, kNoAddon, none);
    RAYOPDF_CHECK(!Contains(without, "Mock line") && none.ocrVectorTextPages == 1 && none.ocrPagesRouted == 0, 1363);
    Mode("ok");
    ConvertReport report;
    const std::string md = ConvertWith(pdf, kMock, report);
    Show("vector", md);
    RAYOPDF_CHECK(Contains(md, "Mock line 1") && Contains(md, "Mock line 6") && report.ocrVectorTextPages == 1 &&
                      report.ocrPagesRouted == 1 && report.ocrPages == 1,
                  1364);
}

} // namespace

int main() {
    TestRasterizer();
    TestOutlines();
    TestFontGlyphs();
    TestRecovery();
    TestActualText();
    TestSymbols();
    TestVectorText();
    std::printf("layout glyph tests passed\n");
    return 0;
}
