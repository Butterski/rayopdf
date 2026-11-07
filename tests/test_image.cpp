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
#include "image/jpeg.h"
#include "image/png.h"
#include "pdf/document.h"
#include "pdf/inflate.h"
#include "pdf/syntax.h"

#include <algorithm>
#include <cmath>
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

// ---------------------------------------------------------------- a JPEG encoder for the tests

// What MakeJpeg writes: baseline (SOF0, one interleaved scan) or progressive (SOF2: a DC scan of
// every component, then one AC scan per component, no successive approximation); every
// quantization step 1, so that decoding gives the samples back within the IDCT's rounding, and
// exactly for a block of one value; fixed-length Huffman codes (DC sizes in 4 bits, every AC
// symbol in 8).
struct JpegSpec {
    uint32_t width = 8;
    uint32_t height = 8;
    int components = 1;      // 1, 3 or 4
    bool subsample = false;  // component 0 sampled 2x2, the others 1x1 (4:2:0)
    int adobe = -1;          // the APP14 transform byte, -1 for no APP14 marker
    bool progressive = false;
    int restart = 0;  // restart interval in MCUs (baseline only)
};

struct JpegWriter {
    std::string out;
    uint32_t bits = 0;
    int count = 0;

    void Put(uint32_t value, int n) {
        for (int i = n - 1; i >= 0; i--) {
            bits = bits << 1 | ((value >> i) & 1);
            if (++count == 8) {
                out.push_back(static_cast<char>(bits));
                if ((bits & 0xFF) == 0xFF) out.push_back('\0');
                bits = 0;
                count = 0;
            }
        }
    }
    void Flush() {
        while (count != 0) Put(1, 1);
    }
    void Marker(int marker, const std::string& body) {
        out += Bytes({0xFF, marker, static_cast<int>((body.size() + 2) >> 8), static_cast<int>((body.size() + 2) & 0xFF)});
        out += body;
    }
};

int SizeOf(int v) {
    int magnitude = v < 0 ? -v : v, size = 0;
    while (magnitude) {
        size++;
        magnitude >>= 1;
    }
    return size;
}

// The AC symbols in code order: EOB, ZRL, run/size for sizes 1 to 10, the EOB runs EOB1-EOB14.
std::vector<int> AcSymbols() {
    std::vector<int> symbols = {0x00, 0xF0};
    for (int r = 0; r < 16; r++)
        for (int s = 1; s <= 10; s++) symbols.push_back(r << 4 | s);
    for (int r = 1; r < 15; r++) symbols.push_back(r << 4);
    return symbols;
}

constexpr double kPi = 3.14159265358979323846;

std::string MakeJpeg(const JpegSpec& spec, const std::vector<std::vector<uint8_t>>& planes) {
    const int n = spec.components;
    const int hMax = spec.subsample ? 2 : 1;
    const uint32_t mcusX = (spec.width + 8 * hMax - 1) / (8 * hMax), mcusY = (spec.height + 8 * hMax - 1) / (8 * hMax);
    const std::vector<int> ac = AcSymbols();
    int acCode[256] = {};
    for (size_t i = 0; i < ac.size(); i++) acCode[ac[i]] = static_cast<int>(i);
    static constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    // Per component: its sampling, block grid (MCU-padded) and coefficients in zigzag order.
    struct Plane {
        int h;
        uint32_t width, height, blocksW, blocksH;
        std::vector<int> coefficients;
    };
    std::vector<Plane> comps(static_cast<size_t>(n));
    for (int c = 0; c < n; c++) {
        Plane& p = comps[static_cast<size_t>(c)];
        p.h = c == 0 ? hMax : 1;
        p.width = (spec.width * static_cast<uint32_t>(p.h) + hMax - 1) / hMax;
        p.height = (spec.height * static_cast<uint32_t>(p.h) + hMax - 1) / hMax;
        p.blocksW = mcusX * static_cast<uint32_t>(p.h);
        p.blocksH = mcusY * static_cast<uint32_t>(p.h);
        p.coefficients.assign(size_t(p.blocksW) * p.blocksH * 64, 0);
        const int scale = hMax / p.h;  // full-size samples per component sample, per axis
        const auto sample = [&](uint32_t x, uint32_t y) {
            x = std::min(x, p.width - 1);
            y = std::min(y, p.height - 1);
            int sum = 0;
            for (int v = 0; v < scale; v++)
                for (int u = 0; u < scale; u++) {
                    const uint32_t fx = std::min(x * scale + u, spec.width - 1), fy = std::min(y * scale + v, spec.height - 1);
                    sum += planes[static_cast<size_t>(c)][size_t(fy) * spec.width + fx];
                }
            return (sum + scale * scale / 2) / (scale * scale) - 128.0;
        };
        for (uint32_t by = 0; by < p.blocksH; by++)
            for (uint32_t bx = 0; bx < p.blocksW; bx++) {
                int* block = p.coefficients.data() + (size_t(by) * p.blocksW + bx) * 64;
                for (int k = 0; k < 64; k++) {
                    const int u = kZigzag[k] % 8, v = kZigzag[k] / 8;
                    double sum = 0;
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                            sum += sample(bx * 8 + x, by * 8 + y) * std::cos((2 * x + 1) * u * kPi / 16) *
                                   std::cos((2 * y + 1) * v * kPi / 16);
                    const double cu = u ? 1 : std::sqrt(0.5), cv = v ? 1 : std::sqrt(0.5);
                    block[k] = static_cast<int>(std::lround(sum * cu * cv / 4));
                }
            }
    }
    JpegWriter w;
    w.out = Bytes({0xFF, 0xD8});
    if (spec.adobe >= 0) w.Marker(0xEE, "Adobe" + Bytes({0, 100, 0, 0, 0, 0, spec.adobe}));
    w.Marker(0xDB, std::string(1, '\0') + std::string(64, '\1'));
    std::string sof = Bytes({8, static_cast<int>(spec.height >> 8), static_cast<int>(spec.height & 0xFF),
                             static_cast<int>(spec.width >> 8), static_cast<int>(spec.width & 0xFF), n});
    for (int c = 0; c < n; c++) sof += Bytes({c + 1, comps[static_cast<size_t>(c)].h * 17, 0});
    w.Marker(spec.progressive ? 0xC2 : 0xC0, sof);
    std::string dht = Bytes({0x00, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    for (int s = 0; s < 12; s++) dht.push_back(static_cast<char>(s));
    dht += Bytes({0x10, 0, 0, 0, 0, 0, 0, 0, static_cast<int>(ac.size()), 0, 0, 0, 0, 0, 0, 0, 0});
    for (int s : ac) dht.push_back(static_cast<char>(s));
    w.Marker(0xC4, dht);
    if (spec.restart) w.Marker(0xDD, Bytes({spec.restart >> 8, spec.restart & 0xFF}));

    std::vector<int> predictors(static_cast<size_t>(n), 0);
    const auto dc = [&](int c, const int* block) {
        const int diff = block[0] - predictors[static_cast<size_t>(c)];
        predictors[static_cast<size_t>(c)] = block[0];
        const int size = SizeOf(diff);
        w.Put(static_cast<uint32_t>(size), 4);
        if (size) w.Put(static_cast<uint32_t>(diff >= 0 ? diff : diff + (1 << size) - 1), size);
    };
    const auto acs = [&](const int* block) {
        int run = 0;
        for (int k = 1; k < 64; k++) {
            if (block[k] == 0) {
                run++;
                continue;
            }
            for (; run > 15; run -= 16) w.Put(static_cast<uint32_t>(acCode[0xF0]), 8);
            const int size = SizeOf(block[k]);
            w.Put(static_cast<uint32_t>(acCode[run << 4 | size]), 8);
            w.Put(static_cast<uint32_t>(block[k] >= 0 ? block[k] : block[k] + (1 << size) - 1), size);
            run = 0;
        }
        if (run) w.Put(static_cast<uint32_t>(acCode[0x00]), 8);
    };
    const auto blockAt = [&](int c, uint32_t bx, uint32_t by) {
        const Plane& p = comps[static_cast<size_t>(c)];
        return p.coefficients.data() + (size_t(by) * p.blocksW + bx) * 64;
    };
    // The interleaved scan: every MCU, its blocks component by component.
    const auto interleaved = [&](bool withAc) {
        std::string sos(1, static_cast<char>(n));
        for (int c = 0; c < n; c++) sos += Bytes({c + 1, 0x00});
        sos += withAc ? Bytes({0, 63, 0}) : Bytes({0, 0, 0});
        w.Marker(0xDA, sos);
        uint32_t mcu = 0;
        int restarts = 0;
        for (uint32_t my = 0; my < mcusY; my++)
            for (uint32_t mx = 0; mx < mcusX; mx++, mcu++) {
                if (spec.restart && mcu > 0 && mcu % static_cast<uint32_t>(spec.restart) == 0) {
                    w.Flush();
                    w.out += Bytes({0xFF, 0xD0 + (restarts++ & 7)});
                    std::fill(predictors.begin(), predictors.end(), 0);
                }
                for (int c = 0; c < n; c++) {
                    const int h = comps[static_cast<size_t>(c)].h;
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < h; x++) {
                            const int* block = blockAt(c, mx * static_cast<uint32_t>(h) + static_cast<uint32_t>(x),
                                                       my * static_cast<uint32_t>(h) + static_cast<uint32_t>(y));
                            dc(c, block);
                            if (withAc) acs(block);
                        }
                }
            }
        w.Flush();
    };
    interleaved(!spec.progressive);
    if (spec.progressive) {
        // One AC scan per component over its own (unpadded) block grid.
        for (int c = 0; c < n; c++) {
            const Plane& p = comps[static_cast<size_t>(c)];
            w.Marker(0xDA, Bytes({1, c + 1, 0x00, 1, 63, 0}));
            for (uint32_t by = 0; by < (p.height + 7) / 8; by++)
                for (uint32_t bx = 0; bx < (p.width + 7) / 8; bx++) acs(blockAt(c, bx, by));
            w.Flush();
        }
    }
    w.out += Bytes({0xFF, 0xD9});
    return w.out;
}

// libjpeg's YCbCr to RGB, as the decoder must give it.
void YccToRgb(int y, int cb, int cr, int* rgb) {
    const auto clamp = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
    rgb[0] = clamp(y + ((91881 * (cr - 128) + 32768) >> 16));
    rgb[1] = clamp(y + ((-22554 * (cb - 128) - 46802 * (cr - 128) + 32768) >> 16));
    rgb[2] = clamp(y + ((116130 * (cb - 128) + 32768) >> 16));
}

// Planes of `n` components, `width` x `height`, each 16x16 tile one value per component:
// tile (tx, ty) of component c holds values[c][(tx + ty) % values[c].size()].
std::vector<std::vector<uint8_t>> Tiles(uint32_t width, uint32_t height, const std::vector<std::vector<int>>& values) {
    std::vector<std::vector<uint8_t>> planes(values.size(), std::vector<uint8_t>(size_t(width) * height));
    for (size_t c = 0; c < values.size(); c++)
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
                planes[c][size_t(y) * width + x] = static_cast<uint8_t>(values[c][(x / 16 + y / 16) % values[c].size()]);
    return planes;
}

void TestJpegDecoder() {
    using Image::Jpeg::Result;
    const auto decode = [](const std::string& jpeg, Image::Jpeg::Info& info, std::string& samples,
                           uint64_t maxPixels = uint64_t(1) << 28) {
        return Image::Jpeg::Decode(reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), maxPixels, info, samples);
    };
    Image::Jpeg::Info info;
    std::string a, b;
    // A gray gradient (37 x 21, edges not on blocks): back within the IDCT's rounding; the same
    // image progressive decodes to the same samples.
    JpegSpec spec;
    spec.width = 37;
    spec.height = 21;
    std::vector<std::vector<uint8_t>> gray(1, std::vector<uint8_t>(37 * 21));
    for (uint32_t i = 0; i < 37 * 21; i++) gray[0][i] = static_cast<uint8_t>((i % 37) * 6 + (i / 37) * 2);
    const std::string sequential = MakeJpeg(spec, gray);
    RAYOPDF_CHECK(decode(sequential, info, a) == Result::Ok && info.width == 37 && info.height == 21 &&
                      info.components == 1 && !info.progressive && a.size() == 37 * 21,
                  548);
    for (size_t i = 0; i < a.size(); i++) RAYOPDF_CHECK(std::abs(static_cast<uint8_t>(a[i]) - gray[0][i]) <= 2, 548);
    spec.progressive = true;
    const std::string progressive = MakeJpeg(spec, gray);
    RAYOPDF_CHECK(decode(progressive, info, b) == Result::Ok && info.progressive && b == a, 548);
    RAYOPDF_CHECK(Image::Jpeg::ReadInfo(reinterpret_cast<const uint8_t*>(progressive.data()), progressive.size(), info) ==
                      Result::Ok &&
                      info.progressive && info.width == 37 && info.adobeTransform == -1,
                  548);

    // YCbCr 4:2:0 with restart markers after every MCU: RGB by libjpeg's formula.
    JpegSpec ycc;
    ycc.width = 40;
    ycc.height = 24;
    ycc.components = 3;
    ycc.subsample = true;
    ycc.restart = 1;
    const std::vector<std::vector<int>> yccValues = {{200, 60, 120}, {90, 160, 128}, {180, 100, 128}};
    RAYOPDF_CHECK(decode(MakeJpeg(ycc, Tiles(40, 24, yccValues)), info, a) == Result::Ok && info.components == 3, 548);
    // Away from the tiles' edges, where upsampling blends the chroma of two tiles.
    for (uint32_t y = 0; y < 24; y++)
        for (uint32_t x = 0; x < 40; x++) {
            if (x % 16 < 3 || x % 16 > 12 || (y >= 16 && y < 19) || (y > 12 && y < 16)) continue;
            const size_t t = x / 16 + y / 16;
            int rgb[3];
            YccToRgb(yccValues[0][t % 3], yccValues[1][t % 3], yccValues[2][t % 3], rgb);
            for (int c = 0; c < 3; c++) RAYOPDF_CHECK(static_cast<uint8_t>(a[(size_t(y) * 40 + x) * 3 + c]) == rgb[c], 548);
        }
    // Adobe transform 0: RGB as stored; CMYK as stored, APP14 or not (no inversion: jpeg.h);
    // YCCK: C, M, Y from 255 - RGB, K as stored.
    JpegSpec rgb = ycc;
    rgb.subsample = false;
    rgb.restart = 0;
    rgb.adobe = 0;
    RAYOPDF_CHECK(decode(MakeJpeg(rgb, Tiles(40, 24, {{10}, {20}, {250}})), info, a) == Result::Ok &&
                      info.adobeTransform == 0 && Bytes({10, 20, 250}) == a.substr(0, 3) && a.substr(3 * 39, 3) == a.substr(0, 3),
                  548);
    JpegSpec cmyk = rgb;
    cmyk.components = 4;
    for (int adobe : {-1, 0}) {
        cmyk.adobe = adobe;
        RAYOPDF_CHECK(decode(MakeJpeg(cmyk, Tiles(40, 24, {{255}, {0}, {30}, {7}})), info, a) == Result::Ok &&
                          info.components == 4 && a.substr(0, 4) == Bytes({255, 0, 30, 7}),
                      548);
    }
    cmyk.adobe = 2;
    RAYOPDF_CHECK(decode(MakeJpeg(cmyk, Tiles(40, 24, {{200}, {90}, {180}, {40}})), info, a) == Result::Ok, 548);
    int expected[3];
    YccToRgb(200, 90, 180, expected);
    RAYOPDF_CHECK(a.substr(0, 4) == Bytes({255 - expected[0], 255 - expected[1], 255 - expected[2], 40}), 548);

    // What the decoder refuses: an arithmetic-coded frame, a pixel limit, cut and foreign data.
    std::string arithmetic = sequential;
    arithmetic[arithmetic.find("\xFF\xC0")] = '\xFF';
    arithmetic[arithmetic.find("\xFF\xC0") + 1] = '\xC9';
    RAYOPDF_CHECK(decode(arithmetic, info, a) == Result::Unsupported && a.empty(), 548);
    RAYOPDF_CHECK(decode(sequential, info, a, 100) == Result::TooLarge, 548);
    RAYOPDF_CHECK(decode(sequential.substr(0, sequential.size() / 2), info, a) == Result::Corrupt, 548);
    RAYOPDF_CHECK(decode(progressive.substr(0, progressive.size() - 30), info, a) != Result::Ok, 548);
    RAYOPDF_CHECK(decode("not a jpeg at all", info, a) == Result::Corrupt, 548);
}

// DCT images in PDFs: which are decoded (and how the PDF reads their samples) and which stored.
void TestDctImages() {
    Image::Encoded out;
    Decoded png;
    JpegSpec spec;
    spec.width = 32;
    spec.height = 16;
    spec.components = 4;
    spec.adobe = 0;
    // CMYK with an APP14 marker: ink as stored (cyan, white); /Decode [1 0 ...] inverts it.
    const std::string cmyk = MakeJpeg(spec, Tiles(32, 16, {{255, 0}, {0, 0}, {0, 0}, {0, 0}}));
    const std::string dict = "/Width 32 /Height 16 /BitsPerComponent 8 /Filter /DCTDecode ";
    RAYOPDF_CHECK(EncodeImage(dict + "/ColorSpace /DeviceCMYK", cmyk, out, false) == Image::Status::Ok &&
                      std::string_view(out.extension) == "png" && ReadPng(out.bytes, png) && png.channels == 3 &&
                      Pixel(png, 0, 0, {0, 255, 255}) && Pixel(png, 31, 15, {255, 255, 255}),
                  549);
    // Inverted: C ink 0 and M, Y ink 1 (red), then every ink 1; K stored 255 is no black.
    const std::string inverted = MakeJpeg(spec, Tiles(32, 16, {{255, 0}, {0, 0}, {0, 0}, {255, 255}}));
    RAYOPDF_CHECK(EncodeImage(dict + "/ColorSpace /DeviceCMYK /Decode [1 0 1 0 1 0 1 0]", inverted, out, false) ==
                      Image::Status::Ok &&
                      ReadPng(out.bytes, png) && Pixel(png, 0, 0, {255, 0, 0}) && Pixel(png, 31, 15, {0, 0, 0}),
                  549);
    // YCCK: Y 255 with neutral chroma is no ink, K as stored.
    spec.adobe = 2;
    RAYOPDF_CHECK(EncodeImage(dict + "/ColorSpace /DeviceCMYK", MakeJpeg(spec, Tiles(32, 16, {{255}, {128}, {128}, {0, 255}})),
                              out, false) == Image::Status::Ok &&
                      ReadPng(out.bytes, png) && Pixel(png, 0, 0, {255, 255, 255}) && Pixel(png, 31, 15, {0, 0, 0}),
                  549);
    // Gray: stored as it is, or decoded for an inverting /Decode or a spot colour space.
    JpegSpec graySpec;
    graySpec.width = 32;
    graySpec.height = 16;
    const std::string gray = MakeJpeg(graySpec, Tiles(32, 16, {{255, 64}}));
    RAYOPDF_CHECK(EncodeImage(dict + "/ColorSpace /DeviceGray", gray, out, false) == Image::Status::Ok &&
                      std::string_view(out.extension) == "jpg" && out.bytes == gray,
                  549);
    RAYOPDF_CHECK(EncodeImage(dict + "/ColorSpace /DeviceGray /Decode [1 0]", gray, out, false) == Image::Status::Ok &&
                      ReadPng(out.bytes, png) && Pixel(png, 0, 0, {0}) && Pixel(png, 31, 15, {191}),
                  549);
    {
        Fixture f;
        const int exp = f.b.Add("<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [1 0 0 0] /N 1 >>");
        const int spot = f.b.AddStream(dict + "/ColorSpace [/Separation /Spot /DeviceCMYK " + std::to_string(exp) + " 0 R]", gray);
        f.Open();
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(spot), kNull, out) == Image::Status::Ok && ReadPng(out.bytes, png) &&
                          Pixel(png, 0, 0, {0, 255, 255}),
                      549);
    }
    // RGB with masks: a soft mask that hides something is applied (PNG with alpha), one that is
    // opaque everywhere leaves the JPEG stored; a JPEG the decoder cannot read keeps its bytes and
    // counts the mask as dropped; JPX always does.
    JpegSpec rgbSpec = graySpec;
    rgbSpec.components = 3;
    rgbSpec.adobe = 0;
    const std::string rgb = MakeJpeg(rgbSpec, Tiles(32, 16, {{250}, {10}, {20}}));
    std::string arithmetic = rgb;
    arithmetic[arithmetic.find("\xFF\xC0") + 1] = '\xC9';
    const std::string jp2 = std::string("\0\0\0\x0CjP  \r\n\x87\n", 12) + "rest";
    {
        Fixture f;
        const int soft = f.b.AddFlateStream("/Width 2 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8", Bytes({0, 255}));
        const int opaque = f.b.AddFlateStream("/Width 2 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8", Bytes({255, 255}));
        const int jbig = f.b.AddStream("/Width 4 /Height 4 /ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /JBIG2Decode", "xx");
        const int jpegMask = f.b.AddStream(dict + "/ColorSpace /DeviceGray", gray);
        const auto image = [&](const std::string& data, int mask, const char* filter = "/DCTDecode") {
            return f.b.AddStream("/Width 32 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter " +
                                     std::string(filter) + " /SMask " + std::to_string(mask) + " 0 R",
                                 data);
        };
        const int masked = image(rgb, soft), stored = image(rgb, opaque), unreadable = image(arithmetic, soft);
        const int jpx = image(jp2, soft, "/JPXDecode"), jpxOpaque = image(jp2, opaque, "/JPXDecode");
        const int jbigMasked = image(rgb, jbig);
        std::string pixels;
        for (int i = 0; i < 32 * 16; i++) pixels += Bytes({1, 2, 3});
        const int flate = f.b.AddFlateStream("/Width 32 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask " +
                                                 std::to_string(jpegMask) + " 0 R",
                                             pixels);
        f.Open();
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(masked), kNull, out) == Image::Status::Ok && !out.maskDropped &&
                          ReadPng(out.bytes, png) && png.channels == 4 && Pixel(png, 0, 0, {250, 10, 20, 0}) &&
                          Pixel(png, 31, 15, {250, 10, 20, 255}),
                      549);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(stored), kNull, out) == Image::Status::Ok && !out.maskDropped &&
                          out.bytes == rgb,
                      549);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(unreadable), kNull, out) == Image::Status::Ok && out.maskDropped &&
                          out.bytes == arithmetic,
                      549);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(jpx), kNull, out) == Image::Status::Ok && out.maskDropped &&
                          std::string_view(out.extension) == "jp2",
                      549);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(jpxOpaque), kNull, out) == Image::Status::Ok && !out.maskDropped, 549);
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(jbigMasked), kNull, out) == Image::Status::Ok && out.maskDropped &&
                          out.bytes == rgb,
                      549);
        // A JPEG soft mask over Flate pixels.
        RAYOPDF_CHECK(Image::EncodeXObject(*f.doc, f.Obj(flate), kNull, out) == Image::Status::Ok && !out.maskDropped &&
                          ReadPng(out.bytes, png) && Pixel(png, 0, 0, {1, 2, 3, 255}) && Pixel(png, 16, 0, {1, 2, 3, 64}),
                      549);
    }
    // An inline JPEG with an inverting /Decode.
    {
        Fixture f;
        f.Open();
        PdfArena arena;
        Syntax::Builder builder(arena);
        const std::string text = "<< /W 32 /H 16 /BPC 8 /CS /G /F /DCT /D [1 0] >>";
        Syntax::Cursor cursor{text, 0};
        PdfValue inlineDict;
        RAYOPDF_CHECK(Syntax::ParseValue(cursor, Syntax::Mode::Content, 0, builder, inlineDict), 549);
        RAYOPDF_CHECK(Image::EncodeInline(*f.doc, inlineDict, gray, kNull, out) == Image::Status::Ok &&
                          ReadPng(out.bytes, png) && Pixel(png, 0, 0, {0}),
                      549);
    }
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
    TestJpegDecoder();
    TestDctImages();
    std::printf("test_image: ok\n");
    return 0;
}
