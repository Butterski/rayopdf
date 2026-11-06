// SPDX-License-Identifier: Apache-2.0
// Core tests of the image module (exit codes 500-549): the PNG writer (CRC-32, row filters, the
// deflate data read back with the pdf module's inflater, compression), and image XObjects and
// inline images of files tests/pdf_builder.h makes: every colour space family, bit depths 1 to
// 16, /Decode, stencils, soft, stencil and colour-key masks, DCT and JPX pass-through, the
// filters that are not decoded, limits and truncated data. Every PNG written is decoded here by
// an independent reader (own CRC, own unfiltering) and compared pixel by pixel.
#include "check.h"
#include "pdf_builder.h"

#include "image/image.h"
#include "image/png.h"
#include "pdf/document.h"
#include "pdf/inflate.h"
#include "pdf/syntax.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf;
using RayoPdf::Test::PdfBuilder;
using RayoPdf::Test::ZlibStored;

namespace {

// ---------------------------------------------------------------- an independent PNG reader

uint32_t Be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint32_t BitwiseCrc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

struct Decoded {
    uint32_t width = 0;
    uint32_t height = 0;
    int channels = 0;
    std::vector<uint8_t> pixels;

    const uint8_t* At(uint32_t x, uint32_t y) const {
        return pixels.data() + (size_t(y) * width + x) * static_cast<size_t>(channels);
    }
};

int PaethPredict(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

// An 8-bit, non-interlaced PNG: every chunk's CRC checked, the zlib stream's Adler-32 too.
bool ReadPng(const std::string& file, Decoded& out) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(file.data());
    if (file.size() < 8 || std::memcmp(p, "\x89PNG\r\n\x1A\n", 8) != 0) return false;
    size_t at = 8;
    std::string idat;
    bool header = false, end = false;
    while (at + 12 <= file.size()) {
        const uint32_t length = Be32(p + at);
        if (length > file.size() - at - 12) return false;
        const std::string_view type(file.data() + at + 4, 4);
        if (BitwiseCrc(p + at + 4, 4 + length) != Be32(p + at + 8 + length)) return false;
        const uint8_t* data = p + at + 8;
        if (type == "IHDR") {
            if (length != 13 || data[8] != 8 || data[10] != 0 || data[11] != 0 || data[12] != 0) return false;
            out.width = Be32(data);
            out.height = Be32(data + 4);
            const int colorType = data[9];
            out.channels = colorType == 0 ? 1 : colorType == 4 ? 2 : colorType == 2 ? 3 : colorType == 6 ? 4 : 0;
            if (out.channels == 0) return false;
            header = true;
        } else if (type == "IDAT") {
            idat.append(reinterpret_cast<const char*>(data), length);
        } else if (type == "IEND") {
            end = true;
        }
        at += 12 + length;
    }
    if (!header || !end || at != file.size() || idat.size() < 6) return false;
    std::string raw;
    const size_t rowBytes = size_t(out.width) * static_cast<size_t>(out.channels);
    const size_t expected = (rowBytes + 1) * out.height;
    if (Inflate::Inflate(idat, raw, expected + 1) != Inflate::Result::Ok || raw.size() != expected) return false;
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    if (Be32(reinterpret_cast<const uint8_t*>(idat.data()) + idat.size() - 4) != (b << 16 | a)) return false;
    out.pixels.assign(rowBytes * out.height, 0);
    const size_t bpp = static_cast<size_t>(out.channels);
    for (uint32_t y = 0; y < out.height; y++) {
        const uint8_t* in = reinterpret_cast<const uint8_t*>(raw.data()) + size_t(y) * (rowBytes + 1);
        uint8_t* row = out.pixels.data() + size_t(y) * rowBytes;
        const uint8_t* up = y > 0 ? row - rowBytes : nullptr;
        for (size_t x = 0; x < rowBytes; x++) {
            const int left = x >= bpp ? row[x - bpp] : 0;
            const int above = up ? up[x] : 0;
            const int corner = up && x >= bpp ? up[x - bpp] : 0;
            int predicted = 0;
            switch (in[0]) {
            case 0: break;
            case 1: predicted = left; break;
            case 2: predicted = above; break;
            case 3: predicted = (left + above) / 2; break;
            case 4: predicted = PaethPredict(left, above, corner); break;
            default: return false;
            }
            row[x] = static_cast<uint8_t>(in[1 + x] + predicted);
        }
    }
    return true;
}

// ---------------------------------------------------------------- documents with images

uint32_t g_random = 1;
uint8_t RandomByte() {
    g_random = g_random * 1103515245u + 12345u;
    return static_cast<uint8_t>(g_random >> 16);
}

// A one-page document around the objects a test adds; Open after adding them.
struct Fixture {
    PdfBuilder b;
    int catalog = b.Reserve();
    int pages = b.Reserve();
    std::string file;
    std::unique_ptr<Document> doc;

    void Open() {
        const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 10 10] >>");
        b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
        b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
        file = b.Build(catalog);
        RAYOPDF_CHECK(Document::Open(file, "", doc) == OpenStatus::Ok, 500);
    }
    const PdfValue& Obj(int num) const { return doc->Object(static_cast<uint32_t>(num)); }
};

const PdfValue kNull{};

// One image XObject `dict` with `data` (Flate, stored blocks) in a document of its own, encoded.
Image::Status EncodeImage(const std::string& dict, std::string_view data, Image::Encoded& out, bool flate = true) {
    Fixture f;
    const int image = flate ? f.b.AddFlateStream(dict, data) : f.b.AddStream(dict, data);
    f.Open();
    return Image::EncodeXObject(*f.doc, f.Obj(image), kNull, out);
}

// Encodes and reads back a PNG; its pixels.
Decoded EncodeAndRead(const std::string& dict, std::string_view data, int code) {
    Image::Encoded out;
    RAYOPDF_CHECK(EncodeImage(dict, data, out) == Image::Status::Ok, code);
    RAYOPDF_CHECK(std::string_view(out.extension) == "png", code);
    Decoded png;
    RAYOPDF_CHECK(ReadPng(out.bytes, png), code);
    RAYOPDF_CHECK(png.width == out.width && png.height == out.height, code);
    return png;
}

bool Pixel(const Decoded& png, uint32_t x, uint32_t y, std::initializer_list<int> expected) {
    if (expected.size() != static_cast<size_t>(png.channels)) return false;
    const uint8_t* p = png.At(x, y);
    size_t i = 0;
    for (int v : expected) {
        if (p[i++] != v) return false;
    }
    return true;
}

std::string Bytes(std::initializer_list<int> values) {
    std::string out;
    for (int v : values) out.push_back(static_cast<char>(v));
    return out;
}

// ---------------------------------------------------------------- tests

void TestCrcAndFilters() {
    RAYOPDF_CHECK(Image::Png::Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, 501);
    std::vector<uint8_t> data(1000);
    for (auto& v : data) v = RandomByte();
    for (size_t size : {0, 1, 7, 8, 9, 100, 1000}) {
        RAYOPDF_CHECK(Image::Png::Crc32(data.data(), size) == BitwiseCrc(data.data(), size), 501);
    }
    // The SSE2 filters against the scalar reference, every filter, bpp and row length to 70, on
    // random rows and on smooth ones (where the filters' costs tie).
    std::vector<uint8_t> row(80), up(80), a(80), b(80);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t bpp = 1; bpp <= 4; bpp++) {
            for (size_t n = bpp; n <= 70; n++) {
                for (size_t i = 0; i < n; i++) {
                    row[i] = pass == 0 ? RandomByte() : static_cast<uint8_t>(i * 3);
                    up[i] = pass == 0 ? RandomByte() : static_cast<uint8_t>(i * 3 + 1);
                }
                RAYOPDF_CHECK(Image::Png::ChooseFilter(row.data(), up.data(), n, bpp) ==
                                  Image::Png::ChooseFilterScalar(row.data(), up.data(), n, bpp),
                              502);
                for (int filter = 0; filter < 5; filter++) {
                    Image::Png::FilterRow(filter, row.data(), up.data(), n, bpp, a.data());
                    Image::Png::FilterRowScalar(filter, row.data(), up.data(), n, bpp, b.data());
                    RAYOPDF_CHECK(std::memcmp(a.data(), b.data(), n) == 0, 502);
                }
            }
        }
    }
}

void TestEncodePng() {
    std::string out;
    uint8_t one = 0;
    RAYOPDF_CHECK(!Image::EncodePng(&one, 0, 1, 1, out) && !Image::EncodePng(&one, 1, 1, 5, out), 503);
    struct Shape {
        uint32_t w, h;
    };
    for (int channels = 1; channels <= 4; channels++) {
        for (Shape s : {Shape{1, 1}, Shape{17, 5}, Shape{100, 37}, Shape{3, 300}}) {
            for (int kind = 0; kind < 3; kind++) {
                std::vector<uint8_t> pixels(size_t(s.w) * s.h * static_cast<size_t>(channels));
                for (size_t i = 0; i < pixels.size(); i++) {
                    pixels[i] = kind == 0 ? RandomByte() : kind == 1 ? static_cast<uint8_t>(i / 7) : 200;
                }
                RAYOPDF_CHECK(Image::EncodePng(pixels.data(), s.w, s.h, channels, out), 503);
                Decoded png;
                RAYOPDF_CHECK(ReadPng(out, png), 504);
                RAYOPDF_CHECK(png.width == s.w && png.height == s.h && png.channels == channels, 504);
                RAYOPDF_CHECK(png.pixels == pixels, 504);
            }
        }
    }
    // Compression: a gradient photo-like image far below its raw size, noise barely above it.
    const uint32_t w = 640, h = 320;
    std::vector<uint8_t> smooth(size_t(w) * h * 3), noise(smooth.size());
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = smooth.data() + (size_t(y) * w + x) * 3;
            p[0] = static_cast<uint8_t>(x * 255 / w);
            p[1] = static_cast<uint8_t>(y * 255 / h);
            p[2] = static_cast<uint8_t>((x + y) / 4);
        }
    }
    for (auto& v : noise) v = RandomByte();
    RAYOPDF_CHECK(Image::EncodePng(smooth.data(), w, h, 3, out) && out.size() < smooth.size() / 20, 505);
    RAYOPDF_CHECK(Image::EncodePng(noise.data(), w, h, 3, out) && out.size() < noise.size() + noise.size() / 100 + 200,
                  505);
}

void TestDeviceSpaces() {
    // 8-bit gray and RGB as stored.
    std::string gray;
    for (int i = 0; i < 12; i++) gray.push_back(static_cast<char>(i * 20));
    Decoded png = EncodeAndRead("/Type /XObject /Subtype /Image /Width 4 /Height 3 /ColorSpace /DeviceGray "
                                "/BitsPerComponent 8",
                                gray, 506);
    RAYOPDF_CHECK(png.channels == 1 && png.width == 4 && png.height == 3, 506);
    RAYOPDF_CHECK(std::memcmp(png.pixels.data(), gray.data(), 12) == 0, 506);
    std::string rgb;
    for (int i = 0; i < 2 * 2 * 3; i++) rgb.push_back(static_cast<char>(i * 21));
    png = EncodeAndRead("/Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB /BitsPerComponent 8", rgb, 507);
    RAYOPDF_CHECK(png.channels == 3 && std::memcmp(png.pixels.data(), rgb.data(), 12) == 0, 507);

    // 1, 2 and 4 bits, rows padded to bytes: 3 pixels a row.
    png = EncodeAndRead("/Width 3 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 1", Bytes({0xA0, 0x40}), 508);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {255}) && Pixel(png, 1, 0, {0}) && Pixel(png, 2, 0, {255}), 508);
    RAYOPDF_CHECK(Pixel(png, 0, 1, {0}) && Pixel(png, 1, 1, {255}) && Pixel(png, 2, 1, {0}), 508);
    png = EncodeAndRead("/Width 3 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 2", Bytes({0x1B}), 509);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {0}) && Pixel(png, 1, 0, {85}) && Pixel(png, 2, 0, {170}), 509);
    png = EncodeAndRead("/Width 3 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 4", Bytes({0x0F, 0x50}), 510);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {0}) && Pixel(png, 1, 0, {255}) && Pixel(png, 2, 0, {85}), 510);
    // 16 bits rounded to 8.
    png = EncodeAndRead("/Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 16",
                        Bytes({0xFF, 0xFF, 0x80, 0x80, 0x00, 0x80}), 511);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {255, 128, 0}), 511);
    // /Decode [1 0] inverts; a 4-bit RGB /Decode maps each component on its own.
    png = EncodeAndRead("/Width 2 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8 /Decode [1 0]",
                        Bytes({0, 255}), 512);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {255}) && Pixel(png, 1, 0, {0}), 512);
    png = EncodeAndRead("/Width 1 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 4 /Decode [1 0 0 1 0 0.5]",
                        Bytes({0xFF, 0xF0}), 512);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {0, 255, 128}), 512);
    // CMYK by the naive formula: cyan, 50 % black, white.
    png = EncodeAndRead("/Width 3 /Height 1 /ColorSpace /DeviceCMYK /BitsPerComponent 8",
                        Bytes({255, 0, 0, 0, 0, 0, 0, 128, 0, 0, 0, 0}), 513);
    RAYOPDF_CHECK(png.channels == 3 && Pixel(png, 0, 0, {0, 255, 255}) && Pixel(png, 1, 0, {127, 127, 127}) &&
                      Pixel(png, 2, 0, {255, 255, 255}),
                  513);
    // CalGray and CalRGB as device spaces.
    png = EncodeAndRead("/Width 1 /Height 1 /ColorSpace [/CalRGB << /WhitePoint [0.95 1 1.09] /Gamma [2.2 2.2 2.2] >>] "
                        "/BitsPerComponent 8",
                        Bytes({10, 20, 30}), 514);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {10, 20, 30}), 514);
    png = EncodeAndRead("/Width 1 /Height 1 /ColorSpace [/CalGray << /WhitePoint [0.95 1 1.09] >>] /BitsPerComponent 8",
                        Bytes({77}), 514);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {77}), 514);
}

void TestOtherSpaces() {
    // ICCBased: by /N, or its /Alternate.
    {
        Fixture f;
        const int icc3 = f.b.AddStream("/N 3", "profile");
        const int icc1 = f.b.AddStream("/N 1 /Alternate /DeviceGray", "profile");
        const int icc4 = f.b.AddStream("/N 4", "profile");
        const int a = f.b.AddFlateStream("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace [/ICCBased " +
                                             std::to_string(icc3) + " 0 R]",
                                         Bytes({1, 2, 3}));
        const int b = f.b.AddFlateStream("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace [/ICCBased " +
                                             std::to_string(icc1) + " 0 R]",
                                         Bytes({9}));
        const int c = f.b.AddFlateStream("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace [/ICCBased " +
                                             std::to_string(icc4) + " 0 R]",
                                         Bytes({0, 255, 0, 0}));
        f.Open();
        Image::Encoded out;
        Decoded png;
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(a), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                          Pixel(png, 0, 0, {1, 2, 3}),
                      515);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(b), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                          Pixel(png, 0, 0, {9}),
                      515);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(c), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                          Pixel(png, 0, 0, {255, 0, 255}),
                      515);
    }
    // Indexed: a hex-string table over RGB at 2 bits, a stream table over CMYK, a short table
    // (missing entries black), an index past hival (clamped).
    Decoded png = EncodeAndRead("/Width 4 /Height 1 /BitsPerComponent 2 /ColorSpace [/Indexed /DeviceRGB 2 "
                                "<FF0000 00FF00 0000FF>]",
                                Bytes({0x1B}), 516);
    RAYOPDF_CHECK(png.channels == 3 && Pixel(png, 0, 0, {255, 0, 0}) && Pixel(png, 1, 0, {0, 255, 0}) &&
                      Pixel(png, 2, 0, {0, 0, 255}) && Pixel(png, 3, 0, {0, 0, 255}),
                  516);
    {
        Fixture f;
        const int table = f.b.AddFlateStream("", Bytes({0, 0, 0, 0, 255, 0, 0, 0}));
        const int image = f.b.AddFlateStream("/Width 3 /Height 1 /BitsPerComponent 8 /ColorSpace [/Indexed /DeviceCMYK 2 " +
                                                 std::to_string(table) + " 0 R]",
                                             Bytes({0, 1, 2}));
        f.Open();
        Image::Encoded out;
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(image), kNull, out) == Image::Status::Ok &&
                          ReadPng(out.bytes, png),
                      517);
        RAYOPDF_CHECK(Pixel(png, 0, 0, {255, 255, 255}) && Pixel(png, 1, 0, {0, 255, 255}) &&
                          Pixel(png, 2, 0, {255, 255, 255}),
                      517);
    }
    png = EncodeAndRead("/Width 2 /Height 1 /BitsPerComponent 8 /ColorSpace [/Indexed /DeviceGray 1 <80>]",
                        Bytes({0, 1}), 518);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {128}) && Pixel(png, 1, 0, {0}), 518);

    // Lab: white, black, and a red with a* > 0.
    png = EncodeAndRead("/Width 3 /Height 1 /BitsPerComponent 8 /Decode [0 100 -128 127 -128 127] /ColorSpace [/Lab << "
                        "/WhitePoint [0.9505 1 1.089] /Range [-128 127 -128 127] >>]",
                        Bytes({255, 128, 128, 0, 128, 128, 135, 208, 195}), 519);
    RAYOPDF_CHECK(png.channels == 3 && Pixel(png, 0, 0, {255, 255, 255}) && Pixel(png, 1, 0, {0, 0, 0}), 519);
    const uint8_t* red = png.At(2, 0);
    RAYOPDF_CHECK(red[0] > 200 && red[1] < 80 && red[2] < 80, 519);
}

void TestSeparation() {
    Fixture f;
    // Type 2 into CMYK: tint 1 is cyan.
    const int exp = f.b.Add("<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [1 0 0 0] /N 1 >>");
    // Type 0 into RGB: black to red.
    const int sampled = f.b.AddStream("/FunctionType 0 /Domain [0 1] /Range [0 1 0 1 0 1] /Size [2] /BitsPerSample 8",
                                      Bytes({0, 0, 0, 255, 0, 0}));
    // Type 4 (not evaluated): gray from the tint.
    const int ps = f.b.AddStream("/FunctionType 4 /Domain [0 1] /Range [0 1 0 1 0 1 0 1]", "{dup dup dup}");
    const int a = f.b.AddFlateStream("/Width 2 /Height 1 /BitsPerComponent 8 /ColorSpace [/Separation /Spot /DeviceCMYK " +
                                         std::to_string(exp) + " 0 R]",
                                     Bytes({255, 0}));
    const int b = f.b.AddFlateStream("/Width 3 /Height 1 /BitsPerComponent 8 /ColorSpace [/Separation /Spot /DeviceRGB " +
                                         std::to_string(sampled) + " 0 R]",
                                     Bytes({0, 255, 128}));
    const int c = f.b.AddFlateStream("/Width 2 /Height 1 /BitsPerComponent 8 /ColorSpace [/Separation /Spot /DeviceCMYK " +
                                         std::to_string(ps) + " 0 R]",
                                     Bytes({255, 0}));
    // DeviceN of process colorants, its type 4 function not evaluated: composed as CMYK.
    const int d = f.b.AddFlateStream("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace [/DeviceN [/Magenta /Yellow] "
                                     "/DeviceCMYK " +
                                         std::to_string(ps) + " 0 R]",
                                     Bytes({255, 255}));
    // DeviceN of spot colorants: gray from the summed tint.
    const int e = f.b.AddFlateStream("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace [/DeviceN [/SpotA /SpotB] "
                                     "/DeviceCMYK " +
                                         std::to_string(ps) + " 0 R]",
                                     Bytes({64, 64}));
    // A named space through the resources, an array of one-output type 2 functions (white at tint
    // 0, black at 1), 4-bit samples.
    const int down = f.b.Add("<< /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [0] /N 1 >>");
    const std::string d3 = std::to_string(down) + " 0 R ";
    const int resources = f.b.Add("<< /ColorSpace << /CS0 [/Separation /Spot /DeviceRGB [" + d3 + d3 + d3 +
                                  "]] >> >>");
    const int g = f.b.AddFlateStream("/Width 2 /Height 1 /BitsPerComponent 4 /ColorSpace /CS0", Bytes({0xF0}));
    f.Open();
    Image::Encoded out;
    Decoded png;
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(a), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                      Pixel(png, 0, 0, {0, 255, 255}) && Pixel(png, 1, 0, {255, 255, 255}),
                  520);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(b), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                      Pixel(png, 0, 0, {0, 0, 0}) && Pixel(png, 1, 0, {255, 0, 0}) && Pixel(png, 2, 0, {128, 0, 0}),
                  521);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(c), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                      Pixel(png, 0, 0, {0}) && Pixel(png, 1, 0, {255}),
                  522);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(d), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                      Pixel(png, 0, 0, {255, 0, 0}),
                  523);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(e), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                      Pixel(png, 0, 0, {127}),
                  524);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(g), kNull, out) == Image::Status::Corrupt, 525);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(g), f.Obj(resources), out) == Image::Status::Ok &&
                      ReadPng(out.bytes, png) && Pixel(png, 0, 0, {0, 0, 0}) && Pixel(png, 1, 0, {255, 255, 255}),
                  525);
}

void TestMasks() {
    // A stencil: black where 0 paints, white elsewhere; /Decode [1 0] the other way round.
    Decoded png = EncodeAndRead("/Width 4 /Height 1 /ImageMask true", Bytes({0x50}), 526);
    RAYOPDF_CHECK(png.channels == 1 && Pixel(png, 0, 0, {0}) && Pixel(png, 1, 0, {255}) && Pixel(png, 3, 0, {255}), 526);
    png = EncodeAndRead("/Width 4 /Height 1 /ImageMask true /BitsPerComponent 1 /Decode [1 0]", Bytes({0x50}), 526);
    RAYOPDF_CHECK(Pixel(png, 0, 0, {255}) && Pixel(png, 1, 0, {0}), 526);

    Fixture f;
    std::string rgb;
    for (int i = 0; i < 4 * 4; i++) rgb += Bytes({i * 10, 0, 255 - i * 10});
    // A soft mask of the same size, one of half the size (nearest neighbour), one all opaque.
    std::string alpha;
    for (int i = 0; i < 16; i++) alpha.push_back(static_cast<char>(i * 16));
    const int smask = f.b.AddFlateStream("/Width 4 /Height 4 /ColorSpace /DeviceGray /BitsPerComponent 8", alpha);
    const int small = f.b.AddFlateStream("/Width 2 /Height 2 /ColorSpace /DeviceGray /BitsPerComponent 8",
                                         Bytes({0, 50, 100, 150}));
    const int opaque = f.b.AddFlateStream("/Width 1 /Height 1 /BitsPerComponent 8", Bytes({255}));
    // A stencil mask (1 masks out) of 2x2 and a colour key.
    const int stencil = f.b.AddFlateStream("/Width 2 /Height 2 /ImageMask true", Bytes({0x40, 0x80}));
    const std::string dict = "/Width 4 /Height 4 /ColorSpace /DeviceRGB /BitsPerComponent 8 ";
    const int a = f.b.AddFlateStream(dict + "/SMask " + std::to_string(smask) + " 0 R", rgb);
    const int b = f.b.AddFlateStream(dict + "/SMask " + std::to_string(small) + " 0 R", rgb);
    const int c = f.b.AddFlateStream(dict + "/SMask " + std::to_string(opaque) + " 0 R", rgb);
    const int d = f.b.AddFlateStream(dict + "/Mask " + std::to_string(stencil) + " 0 R", rgb);
    const int e = f.b.AddFlateStream(dict + "/Mask [0 30 0 0 200 255]", rgb);
    // A soft mask in an undecodable filter: the image stays, opaque.
    const int jbig = f.b.AddStream("/Width 4 /Height 4 /ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /JBIG2Decode",
                                   "xx");
    const int g = f.b.AddFlateStream(dict + "/SMask " + std::to_string(jbig) + " 0 R", rgb);
    f.Open();
    Image::Encoded out;
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(a), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png), 527);
    RAYOPDF_CHECK(png.channels == 4 && Pixel(png, 0, 0, {0, 0, 255, 0}) && Pixel(png, 3, 3, {150, 0, 105, 240}), 527);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(b), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png), 528);
    RAYOPDF_CHECK(png.channels == 4 && png.At(1, 1)[3] == 0 && png.At(2, 0)[3] == 50 && png.At(0, 2)[3] == 100 &&
                      png.At(3, 3)[3] == 150,
                  528);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(c), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                      png.channels == 3,
                  529);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(d), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png), 530);
    RAYOPDF_CHECK(png.channels == 4 && png.At(0, 0)[3] == 255 && png.At(2, 0)[3] == 0 && png.At(0, 2)[3] == 0 &&
                      png.At(3, 3)[3] == 255,
                  530);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(e), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png), 531);
    // Pixels 0..3 (red 0..30, blue 255..225) are keyed out; pixel 4 (red 40) is not.
    RAYOPDF_CHECK(png.channels == 4 && png.At(0, 0)[3] == 0 && png.At(3, 0)[3] == 0 && png.At(0, 1)[3] == 255, 531);
    RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(g), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                      png.channels == 3,
                  532);
}

void TestPassThrough() {
    // DCT: written from the SOI marker on, the size from the SOF segment.
    const std::string jpeg = Bytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00,
                                    0x10, 0x00, 0x20, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9});
    Image::Encoded out;
    RAYOPDF_CHECK(EncodeImage("/Width 32 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /DCTDecode",
                              "\r\n" + jpeg, out, false) == Image::Status::Ok,
                  533);
    RAYOPDF_CHECK(std::string_view(out.extension) == "jpg" && out.bytes == jpeg && out.width == 32 && out.height == 16,
                  533);
    // DCT under Flate.
    RAYOPDF_CHECK(EncodeImage("/Width 32 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter [/FlateDecode "
                              "/DCTDecode]",
                              ZlibStored(jpeg), out, false) == Image::Status::Ok &&
                      out.bytes == jpeg,
                  534);
    RAYOPDF_CHECK(EncodeImage("/Width 32 /Height 16 /Filter /DCTDecode", "not a jpeg", out, false) ==
                      Image::Status::Corrupt,
                  534);
    // JPX: a JP2 file as stored; a raw codestream wrapped.
    const std::string jp2 = std::string("\0\0\0\x0CjP  \r\n\x87\n", 12) + "rest";
    RAYOPDF_CHECK(EncodeImage("/Width 2 /Height 2 /Filter /JPXDecode", jp2, out, false) == Image::Status::Ok &&
                      std::string_view(out.extension) == "jp2" && out.bytes == jp2,
                  535);
    std::string j2k = Bytes({0xFF, 0x4F, 0xFF, 0x51, 0x00, 0x29, 0x00, 0x00});
    for (uint32_t v : {32u, 16u, 0u, 0u, 32u, 16u, 0u, 0u}) j2k += Bytes({0, 0, static_cast<int>(v >> 8), static_cast<int>(v & 0xFF)});
    j2k += Bytes({0x00, 0x01, 0x07, 0x01, 0x01, 0xFF, 0xD9});
    RAYOPDF_CHECK(EncodeImage("/Width 32 /Height 16 /Filter /JPXDecode", j2k, out, false) == Image::Status::Ok, 536);
    RAYOPDF_CHECK(out.bytes.compare(0, 12, jp2, 0, 12) == 0 && out.width == 32 && out.height == 16 &&
                      out.bytes.size() == 12 + 20 + 45 + 8 + j2k.size() &&
                      out.bytes.compare(out.bytes.size() - j2k.size(), j2k.size(), j2k) == 0,
                  536);
    // Not decoded: JBIG2, CCITT, unknown filters; Pattern spaces.
    RAYOPDF_CHECK(EncodeImage("/Width 2 /Height 2 /BitsPerComponent 1 /ColorSpace /DeviceGray /Filter /JBIG2Decode", "x",
                              out, false) == Image::Status::Unsupported,
                  537);
    RAYOPDF_CHECK(EncodeImage("/Width 2 /Height 2 /BitsPerComponent 1 /ColorSpace /DeviceGray /Filter [/FlateDecode "
                              "/CCITTFaxDecode]",
                              ZlibStored("x"), out, false) == Image::Status::Unsupported,
                  537);
    RAYOPDF_CHECK(EncodeImage("/Width 2 /Height 2 /BitsPerComponent 8 /ColorSpace /DeviceGray /Filter /Foo", "xxxx", out,
                              false) == Image::Status::Unsupported,
                  537);
    RAYOPDF_CHECK(EncodeImage("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /Pattern", "x", out) ==
                      Image::Status::Unsupported,
                  537);
}

void TestLimits() {
    Image::Encoded out;
    // Too many pixels: by the default limit, and by a caller's.
    RAYOPDF_CHECK(EncodeImage("/Width 100000 /Height 100000 /ColorSpace /DeviceGray /BitsPerComponent 8", "x", out) ==
                      Image::Status::TooLarge,
                  538);
    {
        Fixture f;
        const int image = f.b.AddFlateStream("/Width 10 /Height 10 /ColorSpace /DeviceGray /BitsPerComponent 8",
                                             std::string(100, 'x'));
        const int jpeg = f.b.AddStream("/Width 10 /Height 10 /Filter /DCTDecode", Bytes({0xFF, 0xD8, 0xFF, 0xD9}));
        f.Open();
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(image), kNull, out, 99) == Image::Status::TooLarge, 538);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(image), kNull, out, 100) == Image::Status::Ok, 538);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(jpeg), kNull, out, 99) == Image::Status::TooLarge, 538);
    }
    // Truncated data: the rows present, the partial last one completed with zeros.
    Decoded png = EncodeAndRead("/Width 4 /Height 10 /ColorSpace /DeviceGray /BitsPerComponent 8",
                                std::string(10, '\x7F'), 539);
    RAYOPDF_CHECK(png.height == 3 && Pixel(png, 1, 2, {127}) && Pixel(png, 2, 2, {0}), 539);
    // Nothing usable, or no usable dictionary.
    RAYOPDF_CHECK(EncodeImage("/Width 4 /Height 4 /ColorSpace /DeviceGray /BitsPerComponent 8", "", out) ==
                      Image::Status::Corrupt,
                  540);
    RAYOPDF_CHECK(EncodeImage("/Height 4 /ColorSpace /DeviceGray /BitsPerComponent 8", "xxxx", out) ==
                      Image::Status::Corrupt,
                  540);
    RAYOPDF_CHECK(EncodeImage("/Width 1 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 3", "x", out) ==
                      Image::Status::Corrupt,
                  540);
    RAYOPDF_CHECK(EncodeImage("/Width 1 /Height 1 /BitsPerComponent 8", "x", out) == Image::Status::Corrupt, 540);
    RAYOPDF_CHECK(EncodeImage("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /Nowhere", "x", out) ==
                      Image::Status::Corrupt,
                  540);
    RAYOPDF_CHECK(EncodeImage("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace [/Indexed /DeviceRGB 0 5]", "x",
                              out) == Image::Status::Corrupt,
                  540);
    // A colour space naming itself ends at the depth limit.
    {
        Fixture f;
        const int resources = f.b.Add("<< /ColorSpace << /A /B /B /A >> >>");
        const int image = f.b.AddFlateStream("/Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /A", "x");
        f.Open();
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(image), f.Obj(resources), out) == Image::Status::Corrupt, 541);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(resources), kNull, out) == Image::Status::Corrupt, 541);
    }
}

// An inline image's dictionary as the content lexer reads it.
void TestInline() {
    Fixture f;
    const int resources = f.b.Add("<< /ColorSpace << /Spot [/Indexed /DeviceGray 1 <00FF>] >> >>");
    f.Open();
    PdfArena arena;
    Syntax::Builder builder(arena);
    const auto parse = [&](std::string_view text, PdfValue& value) {
        Syntax::Cursor cursor{text, 0};
        return Syntax::ParseValue(cursor, Syntax::Mode::Content, 0, builder, value);
    };
    PdfValue dict;
    Image::Encoded out;
    Decoded png;
    RAYOPDF_CHECK(parse("<< /W 2 /H 1 /BPC 8 /CS /RGB /F /AHx >>", dict), 542);
    RAYOPDF_CHECK(Image::EncodeInline(*f.doc, dict, "FF0000 00FF00>", kNull, out) == Image::Status::Ok &&
                      ReadPng(out.bytes, png) && Pixel(png, 0, 0, {255, 0, 0}) && Pixel(png, 1, 0, {0, 255, 0}),
                  542);
    RAYOPDF_CHECK(parse("<< /W 2 /H 1 /BPC 1 /CS [/I /G 1 <40C0>] /D [1 0] /F [/A85] >>", dict), 543);
    // "5Q" is the byte 0x40: indices 0 and 1, swapped by /D [1 0].
    RAYOPDF_CHECK(Image::EncodeInline(*f.doc, dict, "5Q~>", kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png),
                  543);
    RAYOPDF_CHECK(png.channels == 1 && Pixel(png, 0, 0, {192}) && Pixel(png, 1, 0, {64}), 543);
    RAYOPDF_CHECK(parse("<< /W 3 /H 1 /IM true /F /RL >>", dict), 544);
    RAYOPDF_CHECK(Image::EncodeInline(*f.doc, dict, Bytes({0, 0x40, 0x80}), kNull, out) == Image::Status::Ok &&
                      ReadPng(out.bytes, png) && Pixel(png, 0, 0, {0}) && Pixel(png, 1, 0, {255}),
                  544);
    RAYOPDF_CHECK(parse("<< /W 2 /H 1 /BPC 8 /CS /Spot >>", dict), 545);
    RAYOPDF_CHECK(Image::EncodeInline(*f.doc, dict, Bytes({0, 1}), f.Obj(resources), out) == Image::Status::Ok &&
                      ReadPng(out.bytes, png) && Pixel(png, 0, 0, {0}) && Pixel(png, 1, 0, {255}),
                  545);
    RAYOPDF_CHECK(parse("<< /W 32 /H 16 /BPC 8 /CS /G /F /DCT >>", dict), 546);
    RAYOPDF_CHECK(Image::EncodeInline(*f.doc, dict, Bytes({0xFF, 0xD8, 0xFF, 0xD9}), kNull, out) == Image::Status::Ok &&
                      std::string_view(out.extension) == "jpg" && out.width == 32,
                  546);
    RAYOPDF_CHECK(parse("<< /W 2 /H 1 /BPC 8 /CS /G /F /CCF >>", dict), 547);
    RAYOPDF_CHECK(Image::EncodeInline(*f.doc, dict, "xx", kNull, out) == Image::Status::Unsupported, 547);
}

} // namespace

int main() {
    TestCrcAndFilters();
    TestEncodePng();
    TestDeviceSpaces();
    TestOtherSpaces();
    TestSeparation();
    TestMasks();
    TestPassThrough();
    TestLimits();
    TestInline();
    std::printf("test_image: ok\n");
    return 0;
}
