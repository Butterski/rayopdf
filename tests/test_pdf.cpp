// SPDX-License-Identifier: Apache-2.0
// Core tests of the pdf module (exit codes 200-299): the filters with known vectors (zlib data
// made once with Python's zlib, the LZW example of ISO 32000-1 section 7.4.4.2), and the object
// layer on files tests/pdf_builder.h makes: cross-reference tables and streams, object streams,
// hybrid files, /Prev chains, repair, the page tree and the Standard security handler.
#include "check.h"
#include "pdf_builder.h"

#include "crypt/crypt.h"
#include "pdf/content_lexer.h"
#include "pdf/document.h"
#include "pdf/filters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf;
using RayoPdf::Test::BuildOptions;
using RayoPdf::Test::OnePage;
using RayoPdf::Test::PdfBuilder;
using RayoPdf::Test::ZlibStored;

namespace {

// zlib.compress(SampleText(), 9), zlib.compressobj(9, DEFLATED, -15) of the same, and
// zlib.compressobj(9, DEFLATED, 15, 9, Z_FIXED) of "Hello, Hello, Hello! Fixed codes.".
constexpr const char* kZlibHex =
    "78da95d83b8a95411040e1dc5574a8915dd5ef74c0890cef0604efa0e20bbc81cb37fc0f1308670127fbe8aeaaa75b79ff1c"
    "25b2dc5ecacab26a2db7cfe5edc7af3fefa5965f2fe5f1e55efe7cfaf1fbfbbd3cee7f1fefcaed5bf9707bf3f42a9ce75c61"
    "a8705f61aa705d6153e1bcc2aec271854385fd0aa70adb152e15e6156e15c6151e15424e283a9b74949d0d3ba1f06ce009a5"
    "67434f283e1b7c42f9d9f0130ad006a0508236048522b4412894a10d43a90c2d184a6568f1fd5186160ca532b4602895a105"
    "43a90c2d184a6568c1502a430b8652195a3094cad082a1a60c4d186acad084a1a60c4d7e62cad084a1a60c4d186acad084a1"
    "a60c4d186acad084a1a60c4d186acad084a1ae0c0d18eacad080a1ae0c0d18eacad0e024a40c0d18eacad080a1ae0c0d18ea"
    "cad080a1ae0c0d18eacad080a1a10c75181aca5087a1a10c75181aca5087a1a10c758ed3ca5087a1a10c75181aca5087a1a1"
    "0c75181aca5087a1a90c35189aca5083a1a90c35189aca5083a1a90c35189aca50e34ea60c35189aca5083a1a90c35189aca"
    "5083a1a50c250c2d65286168294309434b194a185aca50c2d0528612869632945cec95a184a1a50c250c2d652861682b4301"
    "435b190a18daca50c0d056860286b6321430b495a180a1ad0c050c6d6528781d52860286b63214307494a10a434719aa3074"
    "94a10a434719aa307494a10a434719aa307494a10a434719aa307494a1ca13e3ff0cfd0391ee2f40";
constexpr const char* kRawHex =
    "95d83b8a95411040e1dc5574a8915dd5ef74c0890cef0604efa0e20bbc81cb37fc0f1308670127fbe8aeaaa75b79ff1c25b2"
    "dc5ecacab26a2db7cfe5edc7af3fefa5965f2fe5f1e55efe7cfaf1fbfbbd3cee7f1fefcaed5bf9707bf3f42a9ce75c61a870"
    "5f61aa705d6153e1bcc2aec271854385fd0aa70adb152e15e6156e15c6151e15424e283a9b74949d0d3ba1f06ce009a56743"
    "4f283e1b7c42f9d9f0130ad006a0508236048522b4412894a10d43a90c2d184a6568f1fd5186160ca532b4602895a10543a9"
    "0c2d184a6568c1502a430b8652195a3094cad082a1a60c4d186acad084a1a60c4d7e62cad084a1a60c4d186acad084a1a60c"
    "4d186acad084a1a60c4d186acad084a1ae0c0d18eacad080a1ae0c0d18eacad0e024a40c0d18eacad080a1ae0c0d18eacad0"
    "80a1ae0c0d18eacad080a1a10c75181aca5087a1a10c75181aca5087a1a10c758ed3ca5087a1a10c75181aca5087a1a10c75"
    "181aca5087a1a90c35189aca5083a1a90c35189aca5083a1a90c35189aca50e34ea60c35189aca5083a1a90c35189aca5083"
    "a1a50c250c2d65286168294309434b194a185aca50c2d0528612869632945cec95a184a1a50c250c2d652861682b4301435b"
    "190a18daca50c0d056860286b6321430b495a180a1ad0c050c6d6528781d52860286b63214307494a10a434719aa307494a1"
    "0a434719aa307494a10a434719aa307494a10a434719aa307494a1ca13e3ff0cfd03";
constexpr const char* kFixedHex = "7801f348cdc9c9d751f040a21415dc322b52531492f353528bf500b9d40b02";

std::string Unhex(std::string_view hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        out.push_back(static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    }
    return out;
}

std::string Hex(std::string_view bytes) {
    static const char* const kDigits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 15]);
    }
    return out;
}

// The text kZlibHex and kRawHex hold.
std::string SampleText() {
    std::string text;
    char line[96];
    for (int i = 0; i < 100; i++) {
        std::snprintf(line, sizeof line, "BT /F1 12 Tf 72 %d Td (Line %d of the sample text) Tj ET\n", 700 - i, i);
        text += line;
    }
    return text;
}

// Filters::Decode, checked against Filters::Decoder: the same bytes, a piece at a time, and the
// same status.
StreamStatus Decode(Filters::Kind kind, std::string_view in, std::string& out, size_t limit = size_t(1) << 30,
                    const Filters::Params& params = Filters::Params()) {
    const StreamStatus status = Filters::Decode(kind, params, in, out, limit);
    Filters::Decoder decoder;
    for (int pass = 0; pass < 2; pass++) {
        decoder.Start(kind, params, in, limit);
        std::string pieces;
        std::string_view piece;
        while (decoder.Next(piece)) {
            RAYOPDF_CHECK(!piece.empty(), 252);
            pieces.append(piece.data(), piece.size());
        }
        RAYOPDF_CHECK(pieces == out && decoder.Status() == status, 252);
    }
    return status;
}

// Bits for a deflate stream, least significant first; Huffman codes most significant bit first.
struct BitWriter {
    std::string out;
    uint32_t buffer = 0;
    int count = 0;
    void Put(uint32_t value, int bits) {
        for (int i = 0; i < bits; i++) {
            buffer |= ((value >> i) & 1u) << count;
            if (++count == 8) {
                out.push_back(static_cast<char>(buffer));
                buffer = 0;
                count = 0;
            }
        }
    }
    void Code(uint32_t code, int bits) {
        for (int i = bits - 1; i >= 0; i--) Put((code >> i) & 1u, 1);
    }
    std::string Finish() {
        if (count > 0) out.push_back(static_cast<char>(buffer));
        return out;
    }
};

// A raw deflate block of a zero byte then `matches` matches of 258 zeros at distance 1, with
// dynamic codes of one bit for length 258 and distance 1: two bits for 258 bytes of output.
std::string DeflateZeros(size_t matches) {
    BitWriter w;
    w.Put(1, 1);    // final
    w.Put(2, 2);    // dynamic codes
    w.Put(29, 5);   // 286 literal/length codes
    w.Put(0, 5);    // 1 distance code
    w.Put(14, 4);   // 18 code length codes
    // Code length code lengths in their order 16 17 18 0 8 7 9 6 10 5 11 4 12 3 13 2 14 1: 18 has
    // one bit (code 0), 1 and 2 have two (codes 10 and 11).
    const int order[18] = {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2};
    for (int length : order) w.Put(static_cast<uint32_t>(length), 3);
    // Literal/length lengths: 0 -> 2, 1-255 -> 0, 256 -> 2, 257-284 -> 0, 285 -> 1; distance 0 -> 1.
    w.Code(3, 2);                // 2
    w.Code(0, 1), w.Put(127, 7); // 18: 138 zeros
    w.Code(0, 1), w.Put(106, 7); // 18: 117 zeros
    w.Code(3, 2);                // 2
    w.Code(0, 1), w.Put(17, 7);  // 18: 28 zeros
    w.Code(2, 2);                // 1
    w.Code(2, 2);                // 1 (the distance code)
    // Codes: 285 -> 0, 0 -> 10, 256 -> 11; distance 0 -> 0.
    w.Code(2, 2);
    for (size_t i = 0; i < matches; i++) {
        w.Code(0, 1);
        w.Code(0, 1);
    }
    w.Code(3, 2);
    return w.Finish();
}

// A raw deflate block with codes of every length up to 15: literal/length lengths 'a' + i -> i + 1
// (i < 13), 257 (match length 3) -> 14, 256 and 'z' -> 15; distance lengths i -> i + 1 (i < 9),
// 9 -> 9. Codes longer than the decoder's first-level tables. `expected` gets the output.
std::string DeflateLongCodes(std::string& expected) {
    uint8_t lengths[258 + 10] = {};
    for (int i = 0; i < 13; i++) lengths['a' + i] = static_cast<uint8_t>(i + 1);
    lengths[257] = 14;
    lengths[256] = 15;
    lengths['z'] = 15;
    for (int i = 0; i < 9; i++) lengths[258 + i] = static_cast<uint8_t>(i + 1);
    lengths[258 + 9] = 9;
    // Canonical codes of the two codes.
    uint32_t codes[258 + 10] = {};
    for (const auto& [from, count] : {std::pair<int, int>{0, 258}, std::pair<int, int>{258, 10}}) {
        uint32_t next[17] = {};
        uint32_t perLength[16] = {};
        for (int s = from; s < from + count; s++) perLength[lengths[s]]++;
        perLength[0] = 0;
        for (int bits = 1, code = 0; bits <= 15; bits++) {
            code = (code + static_cast<int>(perLength[bits - 1])) << 1;
            next[bits] = static_cast<uint32_t>(code);
        }
        for (int s = from; s < from + count; s++) {
            if (lengths[s] != 0) codes[s] = next[lengths[s]]++;
        }
    }
    BitWriter w;
    w.Put(1, 1);    // final
    w.Put(2, 2);    // dynamic codes
    w.Put(1, 5);    // 258 literal/length codes
    w.Put(9, 5);    // 10 distance codes
    w.Put(15, 4);   // 19 code length codes
    // Code length codes 0-15 of four bits each (code = symbol), none for 16-18.
    const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    for (int symbol : order) w.Put(symbol >= 16 ? 0 : 4, 3);
    for (uint8_t length : lengths) w.Code(length, 4);
    const auto literal = [&](char c) {
        w.Code(codes[static_cast<uint8_t>(c)], lengths[static_cast<uint8_t>(c)]);
        expected.push_back(c);
    };
    for (int round = 0; round < 3; round++) {
        for (char c = 'a'; c < 'a' + 13; c++) literal(c);
        literal('z');
    }
    // Matches of 3 at each distance code, the extra bits all ones.
    const int extra[10] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3};
    const int base[10] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25};
    for (int d = 0; d < 10; d++) {
        w.Code(codes[257], lengths[257]);
        w.Code(codes[258 + d], lengths[258 + d]);
        w.Put((1u << extra[d]) - 1, extra[d]);
        const size_t back = static_cast<size_t>(base[d] + (1 << extra[d]) - 1);
        for (int i = 0; i < 3; i++) expected.push_back(expected[expected.size() - back]);
    }
    w.Code(codes[256], lengths[256]);
    return w.Finish();
}

// An LZW encoder (ISO 32000-1 section 7.4.4.2) for round trips.
std::string LzwEncode(std::string_view in, int earlyChange) {
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    const auto put = [&](unsigned code, unsigned width) {
        buffer = (buffer << width) | code;
        bits += static_cast<int>(width);
        while (bits >= 8) {
            out.push_back(static_cast<char>(buffer >> (bits - 8)));
            bits -= 8;
        }
    };
    // The table as (prefix code << 8 | byte) -> code, searched linearly (test sizes only).
    std::vector<uint32_t> table;
    unsigned next = 258;
    unsigned width = 9;
    put(256, width);
    int current = -1;
    for (unsigned char c : in) {
        if (current < 0) {
            current = c;
            continue;
        }
        const uint32_t key = static_cast<uint32_t>(current) << 8 | c;
        const auto found = std::find(table.begin(), table.end(), key);
        if (found != table.end()) {
            current = static_cast<int>(258 + (found - table.begin()));
            continue;
        }
        put(static_cast<unsigned>(current), width);
        if (next < 4096) {
            table.push_back(key);
            next++;
        }
        if (next + (earlyChange ? 1u : 0u) > (1u << width) && width < 12) width++;
        if (next == 4096) {
            put(256, width);
            table.clear();
            next = 258;
            width = 9;
        }
        current = c;
    }
    if (current >= 0) put(static_cast<unsigned>(current), width);
    // The decoder adds an entry for the last code before it reads the end-of-data code.
    if (next + (earlyChange ? 1u : 0u) >= (1u << width) && width < 12) width++;
    put(257, width);
    if (bits > 0) out.push_back(static_cast<char>(buffer << (8 - bits)));
    return out;
}

std::string Ascii85Encode(std::string_view in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i += 4) {
        const size_t n = std::min<size_t>(4, in.size() - i);
        uint32_t value = 0;
        for (size_t k = 0; k < 4; k++) value = value << 8 | (k < n ? static_cast<unsigned char>(in[i + k]) : 0u);
        if (n == 4 && value == 0) {
            out.push_back('z');
            continue;
        }
        char group[5];
        for (int k = 4; k >= 0; k--) {
            group[k] = static_cast<char>('!' + value % 85);
            value /= 85;
        }
        out.append(group, n + 1);
        if (i % 40 == 0) out.push_back('\n');  // white space anywhere
    }
    return out + "~>";
}

// The PNG filter of type `type` for one row (the encoder side of the predictors).
std::string PngFilterRow(int type, std::string_view row, std::string_view up, size_t bpp) {
    std::string out(1, static_cast<char>(type));
    for (size_t x = 0; x < row.size(); x++) {
        const int a = x >= bpp ? static_cast<unsigned char>(row[x - bpp]) : 0;
        const int b = static_cast<unsigned char>(up[x]);
        const int c = x >= bpp ? static_cast<unsigned char>(up[x - bpp]) : 0;
        int predictor = 0;
        if (type == 1) predictor = a;
        else if (type == 2) predictor = b;
        else if (type == 3) predictor = (a + b) / 2;
        else if (type == 4) {
            const int p = a + b - c;
            const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            predictor = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        }
        out.push_back(static_cast<char>(static_cast<unsigned char>(row[x]) - predictor));
    }
    return out;
}

// Deterministic bytes (an LCG) for image-like test data.
std::string Noise(size_t n, uint32_t seed) {
    std::string out;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        out.push_back(static_cast<char>((seed >> 16) & 0xFF));
    }
    return out;
}

// A document with the bytes it views (a Document never copies them).
struct Opened {
    std::unique_ptr<std::string> bytes;
    std::unique_ptr<Document> doc;
    Document* operator->() const { return doc.get(); }
    Document& operator*() const { return *doc; }
};

Opened OpenOk(std::string bytes, int code, std::string_view password = "") {
    Opened opened;
    opened.bytes = std::make_unique<std::string>(std::move(bytes));
    const OpenStatus status = Document::Open(*opened.bytes, password, opened.doc);
    if (status != OpenStatus::Ok) std::fprintf(stderr, "open status %d\n", static_cast<int>(status));
    RAYOPDF_CHECK(status == OpenStatus::Ok && opened.doc != nullptr, code);
    return opened;
}

std::string Content(const Document& doc, int page) {
    std::string_view out;
    doc.PageContent(page, out);
    return std::string(out);
}

// OnePage: catalog 1, pages 2, content 3, page 4.
std::string SimpleDocument(std::string_view content = "BT (Hi) Tj ET") { return OnePage(content); }

// ------------------------------------------------------------------- filters

void TestKinds() {
    RAYOPDF_CHECK(Filters::KindOf("FlateDecode") == Filters::Kind::Flate &&
                      Filters::KindOf("Fl") == Filters::Kind::Flate,
                  200);
    RAYOPDF_CHECK(Filters::KindOf("LZW") == Filters::Kind::Lzw && Filters::KindOf("A85") == Filters::Kind::Ascii85 &&
                      Filters::KindOf("AHx") == Filters::Kind::AsciiHex &&
                      Filters::KindOf("RL") == Filters::Kind::RunLength &&
                      Filters::KindOf("CCF") == Filters::Kind::Ccitt && Filters::KindOf("DCT") == Filters::Kind::Dct &&
                      Filters::KindOf("BrotliDecode") == Filters::Kind::Brotli &&
                      Filters::KindOf("Nonsense") == Filters::Kind::Unknown,
                  200);
    RAYOPDF_CHECK(Filters::IsImageFilter(Filters::Kind::Jbig2) && !Filters::IsImageFilter(Filters::Kind::Flate), 200);
}

void TestFlate() {
    const std::string text = SampleText();
    std::string out;
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, Unhex(kZlibHex), out) == StreamStatus::Ok && out == text, 201);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, Unhex(kRawHex), out) == StreamStatus::Ok && out == text, 202);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, Unhex(kFixedHex), out) == StreamStatus::Ok &&
                      out == "Hello, Hello, Hello! Fixed codes.",
                  203);
    const std::string big = Noise(200000, 7);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(big), out) == StreamStatus::Ok && out == big, 204);
    // Stored blocks across the pieces Filters::Decoder gives (Inflate::Stream::kPiece).
    const std::string large = Noise(700000, 9);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(large), out) == StreamStatus::Ok && out == large, 204);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(large).substr(0, 500000), out) == StreamStatus::Salvaged &&
                      large.compare(0, out.size(), out) == 0,
                  206);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, "", out) == StreamStatus::Corrupt && out.empty(), 205);
    // A bad Adler-32 is ignored; a stray end of line before the header is skipped.
    std::string badAdler = Unhex(kZlibHex);
    badAdler.back() ^= 0x55;
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, badAdler, out) == StreamStatus::Ok && out == text, 204);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, "\n" + Unhex(kZlibHex), out) == StreamStatus::Ok && out == text, 204);

    // Truncated: what was decoded before the end is kept, never a byte from past it.
    const std::string zlib = Unhex(kZlibHex);
    for (size_t cut : {size_t(40), zlib.size() / 2, zlib.size() - 6}) {
        const StreamStatus status = Decode(Filters::Kind::Flate, zlib.substr(0, cut), out);
        RAYOPDF_CHECK(status == StreamStatus::Salvaged || (status == StreamStatus::Corrupt && out.empty()), 206);
        RAYOPDF_CHECK(text.compare(0, out.size(), out) == 0, 206);
    }
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, zlib.substr(0, zlib.size() / 2), out) == StreamStatus::Salvaged &&
                      out.size() > 100,
                  206);
    // Ends of the stream before any output.
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, zlib.substr(0, 3), out) == StreamStatus::Corrupt && out.empty(), 207);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, "\x78\x9c\xff\xff\xff\xff", out) == StreamStatus::Corrupt, 207);
    // Corrupt data after some output: salvaged. A stored block that is not the last, then a block
    // of the reserved type 3.
    std::string corrupt = std::string("\x78\x01\x00\xe8\x03\x17\xfc", 7) + std::string(1000, 'a') + "\x07\xff\xff";
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, corrupt, out) == StreamStatus::Salvaged && out == std::string(1000, 'a'),
                  207);

    // The limit cuts the output.
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, zlib, out, 100) == StreamStatus::TooLarge && out == text.substr(0, 100),
                  208);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, zlib, out, text.size()) == StreamStatus::Ok && out == text, 208);
    // The bomb guard: 40,000 matches of 258 bytes in 10 KB pass 1000 times the input.
    const std::string bomb = DeflateZeros(40000);
    const StreamStatus bombStatus = Decode(Filters::Kind::Flate, bomb, out);
    RAYOPDF_CHECK(bombStatus == StreamStatus::TooLarge && out.size() == bomb.size() * 1000 + (64 << 10), 209);
    RAYOPDF_CHECK(out.find_first_not_of('\0') == std::string::npos, 209);
    const std::string small = DeflateZeros(10);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, small, out) == StreamStatus::Ok && out == std::string(2581, '\0'), 209);
    // Codes of every length up to 15 (second-level tables) in both codes; cut anywhere, the output
    // decoded before the cut is kept.
    std::string longExpected;
    const std::string longCodes = DeflateLongCodes(longExpected);
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, longCodes, out) == StreamStatus::Ok && out == longExpected, 270);
    for (size_t cut = 0; cut < longCodes.size(); cut++) {
        const StreamStatus status = Decode(Filters::Kind::Flate, longCodes.substr(0, cut), out);
        RAYOPDF_CHECK(status == StreamStatus::Salvaged || (status == StreamStatus::Corrupt && out.empty()), 270);
        RAYOPDF_CHECK(longExpected.compare(0, out.size(), out) == 0, 270);
    }
}

void TestLzw() {
    std::string out;
    RAYOPDF_CHECK(Decode(Filters::Kind::Lzw, Unhex("800b6050220c0c8501"), out) == StreamStatus::Ok &&
                      out == "-----A---B",
                  210);
    // Round trips through 12-bit codes and a full table, with and without EarlyChange.
    std::string text = SampleText() + Noise(3000, 3) + SampleText();
    for (int early : {1, 0}) {
        Filters::Params params;
        params.earlyChange = early;
        RAYOPDF_CHECK(Decode(Filters::Kind::Lzw, LzwEncode(text, early), out, size_t(1) << 30, params) ==
                              StreamStatus::Ok &&
                          out == text,
                      211);
    }
    // A code past the table: the output before it is kept.
    RAYOPDF_CHECK(Decode(Filters::Kind::Lzw, Unhex("800b7fe0"), out) == StreamStatus::Salvaged && out == "-", 211);
    RAYOPDF_CHECK(Decode(Filters::Kind::Lzw, Unhex("800b6050220c0c8501"), out, 4) == StreamStatus::TooLarge &&
                      out == "----",
                  211);
    // More than one piece of output (64 KB), and cut inside a later one.
    const std::string many = Noise(150000, 5) + std::string(100000, 'q');
    RAYOPDF_CHECK(Decode(Filters::Kind::Lzw, LzwEncode(many, 1), out) == StreamStatus::Ok && out == many, 211);
    RAYOPDF_CHECK(Decode(Filters::Kind::Lzw, LzwEncode(many, 1), out, 200000) == StreamStatus::TooLarge &&
                      out == many.substr(0, 200000),
                  211);
}

void TestAscii() {
    std::string out;
    RAYOPDF_CHECK(Decode(Filters::Kind::Ascii85, "9jqo^BlbD-BleB1DJ+*+F(f,q~>", out) == StreamStatus::Ok &&
                      out == "Man is distinguished",
                  212);
    for (size_t n = 0; n < 13; n++) {
        std::string data = Noise(n, static_cast<uint32_t>(n));
        if (n >= 8) data.replace(4, 4, std::string(4, '\0'));  // a 'z' group
        RAYOPDF_CHECK(Decode(Filters::Kind::Ascii85, Ascii85Encode(data), out) == StreamStatus::Ok && out == data, 212);
    }
    // "<~" before, no "~>" after, and a bad byte.
    RAYOPDF_CHECK(Decode(Filters::Kind::Ascii85, "<~9jqo^", out) == StreamStatus::Ok && out == "Man ", 212);
    RAYOPDF_CHECK(Decode(Filters::Kind::Ascii85, "9jqo^Bl\x01", out) == StreamStatus::Salvaged && out == "Man ", 212);

    RAYOPDF_CHECK(Decode(Filters::Kind::AsciiHex, "48 65\n6C6c6f>", out) == StreamStatus::Ok && out == "Hello", 213);
    RAYOPDF_CHECK(Decode(Filters::Kind::AsciiHex, "414>", out) == StreamStatus::Ok && out == "A@", 213);
    RAYOPDF_CHECK(Decode(Filters::Kind::AsciiHex, "4142", out) == StreamStatus::Ok && out == "AB", 213);
    RAYOPDF_CHECK(Decode(Filters::Kind::AsciiHex, "41zz", out) == StreamStatus::Salvaged && out == "A", 213);
    RAYOPDF_CHECK(Decode(Filters::Kind::AsciiHex, "zz", out) == StreamStatus::Corrupt, 213);

    RAYOPDF_CHECK(Decode(Filters::Kind::RunLength, std::string("\x02" "abc" "\xfe" "x" "\x80" "junk", 11), out) ==
                          StreamStatus::Ok &&
                      out == "abcxxx",
                  214);
    RAYOPDF_CHECK(Decode(Filters::Kind::RunLength, std::string("\x05" "ab", 3), out) == StreamStatus::Salvaged &&
                      out == "ab",
                  214);
    RAYOPDF_CHECK(Decode(Filters::Kind::RunLength, std::string("\x81" "y", 2), out, 10) == StreamStatus::TooLarge &&
                      out == std::string(10, 'y'),
                  214);
    std::string runs;
    for (int i = 0; i < 1000; i++) runs += std::string("\x81", 1) + static_cast<char>('a' + i % 26) + "\x01" "xy";
    RAYOPDF_CHECK(Decode(Filters::Kind::RunLength, runs, out) == StreamStatus::Ok && out.size() == 1000 * 130, 214);
    RAYOPDF_CHECK(Decode(Filters::Kind::RunLength, runs, out, 70000) == StreamStatus::TooLarge && out.size() == 70000,
                  214);
}

void TestPredictors() {
    std::string out;
    // PNG: every filter type, for 8 and 16 bits a component and for 1 bit.
    struct Shape {
        int colors, bpc, columns;
    };
    for (const Shape shape :
         {Shape{3, 8, 5}, Shape{2, 16, 7}, Shape{1, 1, 13}, Shape{4, 8, 1}, Shape{5, 8, 9}, Shape{3, 8, 1000}}) {
        const size_t rowBytes = (static_cast<size_t>(shape.colors * shape.bpc * shape.columns) + 7) / 8;
        const size_t bpp = std::max<size_t>(1, static_cast<size_t>(shape.colors * shape.bpc) / 8);
        std::string image;
        std::string filtered;
        std::string up(rowBytes, '\0');
        // The wide shape's rows pass the pieces of Filters::Decoder's inflate output.
        const int rows = shape.columns >= 1000 ? 150 : 10;
        for (int row = 0; row < rows; row++) {
            const std::string data = Noise(rowBytes, static_cast<uint32_t>(row * 31 + shape.columns));
            filtered += PngFilterRow(row % 5, data, up, bpp);
            image += data;
            up = data;
        }
        Filters::Params params;
        params.predictor = 12 + shape.columns % 4;  // the type byte of each row decides, whatever 10-15
        params.colors = shape.colors;
        params.bitsPerComponent = shape.bpc;
        params.columns = shape.columns;
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(filtered), out, size_t(1) << 30, params) ==
                              StreamStatus::Ok &&
                          out == image,
                      215);
        // A short last row is kept as far as it goes.
        const std::string cut = filtered.substr(0, filtered.size() - 2);
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(cut), out, size_t(1) << 30, params) ==
                              StreamStatus::Salvaged &&
                          out == image.substr(0, image.size() - 2),
                      217);
        // The limit counts predicted bytes.
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(filtered), out, rowBytes * 3, params) ==
                              StreamStatus::TooLarge &&
                          out == image.substr(0, rowBytes * 3),
                      217);
        // A filter byte alone at the end, and a row of an unknown filter type, kept as it stands.
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(filtered + "\x02"), out, size_t(1) << 30, params) ==
                              StreamStatus::Salvaged &&
                          out == image,
                      217);
        std::string unknown = filtered;
        unknown[rowBytes + 1] = 9;
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(unknown), out, size_t(1) << 30, params) ==
                              StreamStatus::Salvaged &&
                          out.size() == image.size(),
                      217);
    }
    // TIFF predictor 2.
    {
        Filters::Params params;
        params.predictor = 2;
        params.colors = 3;
        params.columns = 4;
        // Rows of 12 bytes: differences to the pixel before, per component.
        const std::string coded = std::string("\x01\x02\x03\x01\x01\x01\x10\x00\xff\x00\x00\x00", 12);
        const std::string expect = std::string("\x01\x02\x03\x02\x03\x04\x12\x03\x03\x12\x03\x03", 12);
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(coded + coded), out, size_t(1) << 30, params) ==
                              StreamStatus::Ok &&
                          out == expect + expect,
                      216);
        params.colors = 1;
        params.bitsPerComponent = 16;
        params.columns = 3;
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(std::string("\x01\x00\x00\xff\x00\x02", 6)), out,
                             size_t(1) << 30, params) == StreamStatus::Ok &&
                          out == std::string("\x01\x00\x01\xff\x02\x01", 6),
                      216);
        params.bitsPerComponent = 4;
        params.columns = 4;
        RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored(std::string("\x31\x1f", 2)), out, size_t(1) << 30,
                             params) == StreamStatus::Ok &&
                          out == std::string("\x34\x54", 2),
                      216);
    }
    // A shape no image has.
    Filters::Params bad;
    bad.predictor = 12;
    bad.bitsPerComponent = 3;
    RAYOPDF_CHECK(Decode(Filters::Kind::Flate, ZlibStored("abc"), out, size_t(1) << 30, bad) == StreamStatus::Corrupt,
                  216);
    // Image filters and unknown names are left as they are.
    RAYOPDF_CHECK(Decode(Filters::Kind::Dct, "\xff\xd8 jpeg", out) == StreamStatus::Unsupported &&
                      out == "\xff\xd8 jpeg",
                  218);
    RAYOPDF_CHECK(Decode(Filters::Kind::Unknown, "x", out) == StreamStatus::Unsupported && out == "x", 218);
}

// ------------------------------------------------------------------- documents

void TestClassic() {
    const auto doc = OpenOk(SimpleDocument(), 220);
    RAYOPDF_CHECK(doc->PageCount() == 1 && Content(*doc, 0) == "BT (Hi) Tj ET", 220);
    RAYOPDF_CHECK(doc->Version() == "1.7" && !doc->Report().repaired && doc->Report().xrefSections == 1, 220);
    const PdfRect box = doc->MediaBox(0);
    RAYOPDF_CHECK(box.x0 == 0 && box.y0 == 0 && box.x1 == 612 && box.y1 == 792 && doc->Rotate(0) == 0, 220);
    RAYOPDF_CHECK(doc->Get(doc->Catalog(), "Type").IsName("Catalog") && doc->Info().IsNull(), 220);
    RAYOPDF_CHECK(doc->Get(doc->Resources(0), "Font").IsDict() && doc->ObjectCount() == 5, 220);
    RAYOPDF_CHECK(doc->PageDict(1).IsNull() && doc->PageDict(-1).IsNull() && doc->Object(0).IsNull() &&
                      doc->Object(99).IsNull(),
                  220);
    std::string out;
    std::string_view content;
    RAYOPDF_CHECK(doc->PageContent(5, content) == StreamStatus::NotStream && content.empty(), 220);
    RAYOPDF_CHECK(doc->StreamData(doc->Catalog(), out) == StreamStatus::NotStream, 238);
}

void BuildThreeObjects(PdfBuilder& b, int& catalog, const std::string& content = "BT (Stream) Tj ET") {
    catalog = b.Reserve();
    const int pages = b.Reserve();
    const int stream = b.AddFlateStream("", content);
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /Contents " +
                           std::to_string(stream) + " 0 R /Resources << >> >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 /MediaBox [0 0 200 300] >>");
    b.Add("<< /Title (Info) >>");
}

void TestXrefStreams() {
    for (int variant = 0; variant < 3; variant++) {
        PdfBuilder b;
        int catalog = 0;
        BuildThreeObjects(b, catalog);
        BuildOptions options;
        options.xrefStream = true;
        options.pngPredictor = variant >= 1;
        options.objectStream = variant == 2;
        const std::string file = b.Build(catalog, "/Info 5 0 R", options);
        const auto doc = OpenOk(file, 221 + (variant == 2 ? 1 : 0));
        RAYOPDF_CHECK(doc->PageCount() == 1 && Content(*doc, 0) == "BT (Stream) Tj ET", 221);
        RAYOPDF_CHECK(doc->MediaBox(0).x1 == 200 && doc->MediaBox(0).y1 == 300 && !doc->Report().repaired, 221);
        RAYOPDF_CHECK(doc->Get(doc->Info(), "Title").String() == "Info", 222);
        RAYOPDF_CHECK(doc->Trailer().Find("Root") != nullptr && doc->Trailer().IsDict(), 222);
    }
}

// Catalog, pages and page (1-3) in an object stream (5); content (4) direct; 6 the hybrid's
// cross-reference stream. The table marks 1-3 free, as writers of hybrid files do.
std::string HybridDocument(bool withXrefStm) {
    std::string out = "%PDF-1.5\n";
    const std::string objects[3] = {"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                                    "<< /Type /Page /Parent 2 0 R /Contents 4 0 R /MediaBox [0 0 10 10] >>"};
    std::string header, packed;
    for (int i = 0; i < 3; i++) {
        header += std::to_string(i + 1) + " " + std::to_string(packed.size()) + " ";
        packed += objects[i] + "\n";
    }
    size_t offsets[7] = {};
    offsets[4] = out.size();
    out += "4 0 obj\n<< /Length 14 >>\nstream\nBT (Hyb) Tj ET\nendstream\nendobj\n";
    offsets[5] = out.size();
    const std::string data = ZlibStored(header + packed);
    out += "5 0 obj\n<< /Type /ObjStm /N 3 /First " + std::to_string(header.size()) +
           " /Filter /FlateDecode /Length " + std::to_string(data.size()) + " >>\nstream\n" + data +
           "\nendstream\nendobj\n";
    offsets[6] = out.size();
    std::string rows;
    for (int num = 0; num < 7; num++) {
        unsigned char row[4] = {0, 0, 0, 0};  // /W [1 2 1]
        if (num >= 1 && num <= 3) {
            row[0] = 2;
            row[2] = 5;
            row[3] = static_cast<unsigned char>(num - 1);
        } else if (num >= 4) {
            row[0] = 1;
            row[1] = static_cast<unsigned char>(offsets[num] >> 8);
            row[2] = static_cast<unsigned char>(offsets[num]);
        }
        rows.append(reinterpret_cast<const char*>(row), 4);
    }
    out += "6 0 obj\n<< /Type /XRef /Size 7 /W [1 2 1] /Length " + std::to_string(rows.size()) + " >>\nstream\n" +
           rows + "\nendstream\nendobj\n";
    const size_t xref = out.size();
    out += "xref\n0 7\n0000000000 65535 f \n0000000000 00000 f \n0000000000 00000 f \n0000000000 00000 f \n";
    char line[32];
    for (int num = 4; num < 7; num++) {
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[num]);
        out += line;
    }
    out += "trailer\n<< /Size 7 /Root 1 0 R";
    if (withXrefStm) out += " /XRefStm " + std::to_string(offsets[6]);
    out += " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

void TestHybrid() {
    const auto doc = OpenOk(HybridDocument(true), 223);
    RAYOPDF_CHECK(doc->PageCount() == 1 && Content(*doc, 0) == "BT (Hyb) Tj ET" && !doc->Report().repaired, 223);
    RAYOPDF_CHECK(doc->Report().xrefSections == 2 && doc->Version() == "1.5", 223);
    // Names read from the file view its bytes; an object stream's are copies (FullRepair drops the
    // streams' data).
    const auto inFile = [&](std::string_view text) {
        const auto at = reinterpret_cast<uintptr_t>(text.data()), begin = reinterpret_cast<uintptr_t>(doc->Bytes().data());
        return at >= begin && at + text.size() <= begin + doc->Bytes().size();
    };
    const PdfValue& type = doc->Get(doc->Catalog(), "Type");
    RAYOPDF_CHECK(doc->Object(4).Entries().size() == 1 && doc->Object(4).Entries()[0].key == "Length" &&
                      inFile(doc->Object(4).Entries()[0].key) && type.IsName("Catalog") && !inFile(type.Name()),
                  281);
    // Without /XRefStm the compressed objects are free: found again by repair.
    const auto repaired = OpenOk(HybridDocument(false), 223);
    RAYOPDF_CHECK(repaired->PageCount() == 1 && repaired->Report().repaired, 223);
}

// `base` with an incremental update replacing object 3 (the content of BuildThreeObjects); /Prev
// is written with ten digits so that tests can rewrite it in place.
std::string Update(const std::string& base, const std::string& content) {
    const size_t start = base.rfind("startxref");
    const size_t oldXref = std::stoul(base.substr(start + 10));
    std::string out = base;
    const size_t offset = out.size();
    const std::string data = ZlibStored(content);
    out += "3 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(data.size()) + " >>\nstream\n" + data +
           "\nendstream\nendobj\n";
    const size_t xref = out.size();
    char line[64];
    std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
    out += "xref\n0 1\n0000000000 65535 f \n3 1\n" + std::string(line) + "trailer\n<< /Size 6 /Root 1 0 R /Prev ";
    std::snprintf(line, sizeof line, "%010zu", oldXref);
    out += std::string(line) + " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

void TestPrevChain() {
    for (const bool placeholder : {false, true}) {
        PdfBuilder b;
        int catalog = 0;
        BuildThreeObjects(b, catalog, "BT (Old) Tj ET");
        // The oldest trailer's /Prev 0 points at the header, no section: the chain ends there.
        const std::string file = b.Build(catalog, placeholder ? "/Prev 0000000000" : "");
        const std::string updated = Update(file, "BT (New) Tj ET");
        const auto doc = OpenOk(updated, 224);
        RAYOPDF_CHECK(Content(*doc, 0) == "BT (New) Tj ET" && doc->Report().xrefSections == 2, 224);
        // Two updates: the newest wins.
        std::string twice = Update(updated, "BT (Newest) Tj ET");
        const auto doc2 = OpenOk(twice, 224);
        RAYOPDF_CHECK(Content(*doc2, 0) == "BT (Newest) Tj ET" && doc2->Report().xrefSections == 3, 224);
        if (!placeholder) continue;
        // The oldest section's /Prev naming the newest: a cycle, cut, every section read once.
        const size_t newest = std::stoul(twice.substr(twice.rfind("startxref") + 10));
        char digits[16];
        std::snprintf(digits, sizeof digits, "%010zu", newest);
        twice.replace(twice.find("/Prev 0000000000") + 6, 10, digits);
        const auto doc3 = OpenOk(twice, 225);
        RAYOPDF_CHECK(Content(*doc3, 0) == "BT (Newest) Tj ET" && doc3->Report().xrefSections == 3, 225);
        RAYOPDF_CHECK(doc3->Report().limitHits > 0 && !doc3->Report().repaired, 225);
    }
}

void TestGarbageAndRepair() {
    const std::string file = SimpleDocument();
    // Garbage before the header: offsets count from the header.
    const auto shifted = OpenOk("GARBAGE GARBAGE\r\n" + file, 226);
    RAYOPDF_CHECK(Content(*shifted, 0) == "BT (Hi) Tj ET" && !shifted->Report().repaired, 226);
    // A startxref pointing nowhere.
    std::string badStart = file;
    const size_t at = badStart.rfind("startxref\n") + 10;
    badStart.replace(at, badStart.find('\n', at) - at, "12345");
    const auto repaired = OpenOk(badStart, 227);
    RAYOPDF_CHECK(Content(*repaired, 0) == "BT (Hi) Tj ET" && repaired->Report().repaired, 227);
    // No startxref at all, no table: the scan finds the objects and the trailer.
    const std::string noXref = file.substr(0, file.find("xref\n")) + "trailer\n<< /Root 1 0 R >>\n";
    const auto scanned = OpenOk(noXref, 229);
    RAYOPDF_CHECK(Content(*scanned, 0) == "BT (Hi) Tj ET" && scanned->Report().repaired, 229);
    // No trailer either: the catalog is found by its /Type.
    const auto bare = OpenOk(file.substr(0, file.find("xref\n")), 229);
    RAYOPDF_CHECK(bare->PageCount() == 1 && Content(*bare, 0) == "BT (Hi) Tj ET", 229);
    // Every offset of the table 3 bytes off: entries point at no "N G obj".
    std::string badOffsets = file;
    for (size_t p = badOffsets.find(" 00000 n"); p != std::string::npos; p = badOffsets.find(" 00000 n", p + 1)) {
        const size_t offset = std::stoul(badOffsets.substr(p - 10, 10)) + 3;
        char digits[16];
        std::snprintf(digits, sizeof digits, "%010zu", offset);
        badOffsets.replace(p - 10, 10, digits);
    }
    const auto lazy = OpenOk(badOffsets, 228);
    RAYOPDF_CHECK(Content(*lazy, 0) == "BT (Hi) Tj ET" && lazy->Report().repaired, 228);
    // The scan skips stream data and takes the last definition of a number.
    std::string twice = file.substr(0, file.find("xref\n"));
    const std::string data = ZlibStored("BT (Later) Tj ET");
    twice += "3 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(data.size()) +
             " >>\nstream\n" + data + "\nendstream\nendobj\n";
    const auto last = OpenOk(twice, 249);
    RAYOPDF_CHECK(Content(*last, 0) == "BT (Later) Tj ET", 249);
    std::string inside = file.substr(0, file.find("xref\n"));
    inside += "9 0 obj\n<< /Length 30 >>\nstream\n1 0 obj << /Type /Bogus >> endobj\nendstream\nendobj\n";
    const auto skipped = OpenOk(inside, 249);
    RAYOPDF_CHECK(skipped->PageCount() == 1 && skipped->Get(skipped->Catalog(), "Type").IsName("Catalog"), 249);
    // Not a PDF; a header and nothing else.
    std::unique_ptr<Document> none;
    RAYOPDF_CHECK(Document::Open("hello world, no pdf here", "", none) == OpenStatus::NotPdf && !none, 236);
    RAYOPDF_CHECK(Document::Open("", "", none) == OpenStatus::NotPdf, 236);
    RAYOPDF_CHECK(Document::Open("%PDF-1.4\n%%EOF\n", "", none) == OpenStatus::Corrupt && !none, 236);
}

void TestStreams() {
    // /Length too short, too long, missing, and by reference.
    for (const std::string length : {"5", "500", "", "6 0 R"}) {
        PdfBuilder b;
        const int catalog = b.Reserve();
        const int pages = b.Reserve();
        const std::string data = "BT (Length) Tj ET";
        const int stream = b.Add("<< " + (length.empty() ? std::string() : "/Length " + length) + " >>\nstream\n" +
                                 data + "\nendstream");
        const int page = b.Add("<< /Type /Page /Parent 2 0 R /Contents " + std::to_string(stream) + " 0 R >>");
        b.Add(std::to_string(data.size()));  // 5: unused
        b.Add(std::to_string(data.size()));  // 6: the length by reference
        b.Set(catalog, "<< /Type /Catalog /Pages 2 0 R >>");
        b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] >>");
        const auto doc = OpenOk(b.Build(catalog), 230);
        RAYOPDF_CHECK(Content(*doc, 0) == data, 230);
    }
    // /Contents arrays are joined with a newline; a broken piece makes the worst status.
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int a = b.AddFlateStream("", "BT (A) Tj");
    const int c = b.AddStream("/Filter [/AHx /Fl] /DecodeParms [null << /Predictor 12 /Columns 3 >>]",
                              Hex(ZlibStored(std::string("\x00" "ET " "\x02" "\x00\x00\x00", 8))) + ">");
    std::string truncated = ZlibStored(SampleText());
    truncated.resize(truncated.size() / 2);
    const int t = b.AddStream("/Filter /FlateDecode", truncated);
    const int image = b.AddStream("/Filter [/AHx /DCTDecode]", "4142>");
    const int page1 = b.Add("<< /Type /Page /Parent 2 0 R /Contents [" + std::to_string(a) + " 0 R " +
                            std::to_string(c) + " 0 R] >>");
    const int page2 = b.Add("<< /Type /Page /Parent 2 0 R /Contents [" + std::to_string(a) + " 0 R " +
                            std::to_string(t) + " 0 R] >>");
    b.Set(catalog, "<< /Type /Catalog /Pages 2 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page1) + " 0 R " + std::to_string(page2) + " 0 R] >>");
    const auto doc = OpenOk(b.Build(catalog), 237);
    std::string out;
    std::string_view content;
    RAYOPDF_CHECK(doc->PageContent(0, content) == StreamStatus::Ok && content == "BT (A) Tj\nET ET ", 237);
    RAYOPDF_CHECK(doc->PageContent(1, content) == StreamStatus::Salvaged && content.compare(0, 10, "BT (A) Tj\n") == 0,
                  243);
    RAYOPDF_CHECK(doc->Report().salvagedStreams == 1 && doc->Report().failedStreams == 0, 243);
    PdfValue ref;
    ref.type = PdfValue::Type::Ref;
    ref.refNum = static_cast<uint32_t>(image);
    RAYOPDF_CHECK(doc->StreamData(ref, out) == StreamStatus::Unsupported && out == "AB", 238);
    ref.refNum = static_cast<uint32_t>(a);
    RAYOPDF_CHECK(doc->StreamData(ref, out, 4) == StreamStatus::TooLarge && out == "BT (", 238);
    // A reference with the wrong generation is null.
    ref.refGen = 1;
    RAYOPDF_CHECK(doc->Resolve(ref).IsNull() && doc->StreamData(ref, out) == StreamStatus::NotStream, 240);
}

// Document::StreamReader against StreamData: the same bytes, status and report counts, however the
// reader is read (to its end, twice, one piece and then dropped, or not read at all).
void TestStreamReader() {
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    std::string truncated = ZlibStored(SampleText());
    truncated.resize(truncated.size() / 2);
    const std::vector<int> streams = {
        b.AddStream("/Filter /FlateDecode", DeflateZeros(5000)),  // 1.3 MB: several pieces
        b.AddStream("/Filter /FlateDecode", truncated),
        b.AddStream("/Filter /FlateDecode", "no deflate data"),
        b.AddStream("/Filter [/A85 /Fl]", Ascii85Encode(ZlibStored(SampleText())) + "~>"),
        b.AddStream("/Filter [/AHx /DCTDecode]", "4142>"),
        b.AddStream("/Filter [/AHx /Fl] /DecodeParms [null << /Predictor 12 /Columns 3 >>]",
                    Hex(ZlibStored(std::string("\x00" "ET " "\x02" "\x00\x00\x00" "\x09", 9))) + ">"),
        b.AddStream("", SampleText()),
        b.Add("<< /NotA /Stream >>"),
    };
    const int page = b.Add("<< /Type /Page /Parent 2 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages 2 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] >>");
    const std::string file = b.Build(catalog);
    const auto sameReport = [](const DocumentReport& x, const DocumentReport& y) {
        return x.salvagedStreams == y.salvagedStreams && x.failedStreams == y.failedStreams &&
               x.limitHits == y.limitHits && x.brokenObjects == y.brokenObjects;
    };
    for (const size_t limit : {size_t(1) << 30, size_t(1000)}) {
        for (const int stream : streams) {
            PdfValue ref;
            ref.type = PdfValue::Type::Ref;
            ref.refNum = static_cast<uint32_t>(stream);
            for (int mode = 0; mode < 4; mode++) {
                const auto whole = OpenOk(file, 253);
                const auto read = OpenOk(file, 253);
                std::string expected;
                const StreamStatus status = whole->StreamData(ref, expected, limit);
                {
                    Document::StreamReader reader;
                    const StreamStatus open = reader.Open(*read, ref, limit);
                    std::string_view piece;
                    if (open != StreamStatus::Ok) {
                        RAYOPDF_CHECK(open == status && expected.empty() && !reader.Next(piece), 253);
                        RAYOPDF_CHECK(reader.Finish() == status, 253);
                    } else if (mode < 2) {
                        // To the end; mode 1 a second time from the start.
                        for (int pass = 0; pass <= mode; pass++) {
                            if (pass > 0) reader.Restart();
                            std::string got;
                            while (reader.Next(piece)) got.append(piece.data(), piece.size());
                            RAYOPDF_CHECK(got == expected && reader.Size() == expected.size(), 254);
                            RAYOPDF_CHECK(sameReport(read->Report(), whole->Report()), 255);
                        }
                        RAYOPDF_CHECK(reader.Finish() == status, 254);
                    } else if (mode == 2) {
                        // One piece, then the reader goes: it reads the rest and counts the stream then.
                        if (reader.Next(piece)) RAYOPDF_CHECK(expected.compare(0, piece.size(), piece) == 0, 254);
                    }
                }
                RAYOPDF_CHECK(sameReport(read->Report(), whole->Report()), 255);
            }
        }
    }
    // Readers held open while other streams decode: every count comes once.
    const auto whole = OpenOk(file, 256);
    const auto read = OpenOk(file, 256);
    std::string out;
    for (const int stream : streams) {
        PdfValue ref;
        ref.type = PdfValue::Type::Ref;
        ref.refNum = static_cast<uint32_t>(stream);
        whole->StreamData(ref, out);
        whole->StreamData(ref, out);
    }
    {
        std::vector<std::unique_ptr<Document::StreamReader>> readers;
        for (const int stream : streams) {
            PdfValue ref;
            ref.type = PdfValue::Type::Ref;
            ref.refNum = static_cast<uint32_t>(stream);
            readers.push_back(std::make_unique<Document::StreamReader>());
            readers.back()->Open(*read, ref);
            read->StreamData(ref, out);
        }
    }
    RAYOPDF_CHECK(sameReport(read->Report(), whole->Report()), 256);
}

void TestNesting() {
    // Arrays nested past the limit: the object is broken, the document still opens.
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int deep = b.Add(std::string(100, '[') + std::string(100, ']'));
    const int ok = b.Add(std::string(60, '[') + "1" + std::string(60, ']'));
    const int page = b.Add("<< /Type /Page /Parent 2 0 R >>");
    // A reference chain longer than 32 resolves to null.
    int chain = b.Add("42");
    for (int i = 0; i < 40; i++) chain = b.Add(std::to_string(chain) + " 0 R");
    b.Set(catalog, "<< /Type /Catalog /Pages 2 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] >>");
    const auto doc = OpenOk(b.Build(catalog), 231);
    RAYOPDF_CHECK(doc->Object(static_cast<uint32_t>(deep)).IsNull() && doc->Report().brokenObjects == 1, 231);
    RAYOPDF_CHECK(doc->Object(static_cast<uint32_t>(ok)).IsArray(), 231);
    PdfValue ref;
    ref.type = PdfValue::Type::Ref;
    ref.refNum = static_cast<uint32_t>(chain);
    RAYOPDF_CHECK(doc->Resolve(ref).IsNull() && doc->Report().limitHits > 0, 231);
    ref.refNum = static_cast<uint32_t>(chain - 30);
    double number = 0;
    RAYOPDF_CHECK(doc->Number(ref, number) && number == 42, 240);
}

void TestPageTree() {
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int root = b.Reserve();
    const int middle = b.Reserve();
    const int resources = b.Add("<< /Font << >> >>");
    // A node without /Type but with /Kids is a Pages node; a leaf without /Type is a page. The
    // root lists itself (a cycle) and the middle node twice (visited once).
    const int p1 = b.Add("<< /Type /Page /Parent " + std::to_string(middle) + " 0 R >>");
    const int p2 = b.Add("<< /Parent " + std::to_string(middle) + " 0 R /Rotate -90 /CropBox [-10 -10 100 2000] >>");
    const int p3 = b.Add("<< /Type /Page /MediaBox [0 0 0 0] /Rotate 45 /Resources << /Own true >> >>");
    b.Set(middle, "<< /Kids [" + std::to_string(p1) + " 0 R " + std::to_string(p2) +
                      " 0 R] /MediaBox [0 0 300 400] /Rotate 180 >>");
    b.Set(root, "<< /Type /Pages /Kids [" + std::to_string(middle) + " 0 R " + std::to_string(root) + " 0 R " +
                    std::to_string(middle) + " 0 R " + std::to_string(p3) + " 0 R] /Resources " +
                    std::to_string(resources) + " 0 R /MediaBox [0 0 500 600] >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(root) + " 0 R /Version /2.0 >>");
    const auto doc = OpenOk(b.Build(catalog), 232);
    RAYOPDF_CHECK(doc->PageCount() == 3 && doc->Report().limitHits >= 2, 233);
    RAYOPDF_CHECK(doc->MediaBox(0).x1 == 300 && doc->Rotate(0) == 180 && doc->Get(doc->Resources(0), "Font").IsDict(),
                  232);
    const PdfRect crop = doc->CropBox(1);
    RAYOPDF_CHECK(doc->Rotate(1) == 270 && crop.x0 == 0 && crop.y0 == 0 && crop.x1 == 100 && crop.y1 == 400, 232);
    RAYOPDF_CHECK(doc->MediaBox(2).x1 == 612 && doc->MediaBox(2).y1 == 792 && doc->Rotate(2) == 0, 232);
    RAYOPDF_CHECK(doc->CropBox(2).x1 == 612 && doc->Get(doc->Resources(2), "Own").Bool(), 232);
    RAYOPDF_CHECK(doc->PageDict(1).Find("Rotate") != nullptr && doc->Version() == "2.0", 234);

    // A broken tree (/Pages names a missing object): pages come from their /Type, with what
    // they inherit through /Parent.
    PdfBuilder broken;
    const int c2 = broken.Reserve();
    const int parent = broken.Add("<< /Type /Pages /MediaBox [0 0 50 60] >>");
    broken.Add("<< /Type /Page /Parent " + std::to_string(parent) + " 0 R >>");
    broken.Add("<< /Type /Page /Parent " + std::to_string(parent) + " 0 R /MediaBox [0 0 70 80] >>");
    broken.Set(c2, "<< /Type /Catalog /Pages 99 0 R >>");
    const auto fallback = OpenOk(broken.Build(c2), 235);
    RAYOPDF_CHECK(fallback->PageCount() == 2 && fallback->MediaBox(0).x1 == 50 && fallback->MediaBox(1).x1 == 70 &&
                      fallback->Report().repaired,
                  235);
}

void TestXrefQuirks() {
    const std::string file = SimpleDocument();
    // The table's first subsection numbered from 1 (a writer bug).
    std::string fromOne = file;
    fromOne.replace(fromOne.find("xref\n0 "), 7, "xref\n1 ");
    const auto one = OpenOk(fromOne, 241);
    RAYOPDF_CHECK(Content(*one, 0) == "BT (Hi) Tj ET" && !one->Report().repaired, 241);
    // Entries with other line ends and spacing (19 and 21 bytes, tokens apart).
    std::string spacing = file;
    for (size_t p = spacing.find(" n \n"); p != std::string::npos; p = spacing.find(" n \n", p + 1)) {
        spacing.replace(p, 4, " n\r\n");
    }
    const size_t first = spacing.find(" 00000 n");
    spacing.replace(first, 7, "   0 ");
    const auto spaced = OpenOk(spacing, 242);
    RAYOPDF_CHECK(Content(*spaced, 0) == "BT (Hi) Tj ET" && !spaced->Report().repaired, 242);
    // Fewer entries than the subsection says.
    std::string fewer = file;
    fewer.replace(fewer.find("xref\n0 5"), 8, "xref\n0 9");
    const auto few = OpenOk(fewer, 242);
    RAYOPDF_CHECK(Content(*few, 0) == "BT (Hi) Tj ET", 242);
    // A linearized file's first object.
    PdfBuilder b;
    b.Add("<< /Linearized 1 /L 1000 >>");
    int catalog = 0;
    BuildThreeObjects(b, catalog);
    const std::string withInfo = b.Build(catalog);
    const auto linear = OpenOk(withInfo, 245);
    RAYOPDF_CHECK(linear->Report().linearized && linear->PageCount() == 1, 245);
    RAYOPDF_CHECK(linear->Get(linear->Object(6), "Title").String() == "Info", 250);
    // A free entry is null: the info dictionary, object 6, the table's last.
    std::string freed = withInfo;
    freed.replace(freed.rfind(" 00000 n") + 7, 1, "f");
    const auto free = OpenOk(freed, 250);
    RAYOPDF_CHECK(free->Object(6).IsNull() && free->ObjectCount() == 7 && free->PageCount() == 1, 250);
}

// ------------------------------------------------------------------- encryption

constexpr unsigned char kPad[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                                    0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                                    0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

std::string Md5(std::string_view data) {
    uint8_t digest[16];
    Crypt::Md5(data, digest);
    return std::string(reinterpret_cast<const char*>(digest), 16);
}

std::string Rc4(std::string key, std::string data) {
    Crypt::Rc4(reinterpret_cast<const uint8_t*>(key.data()), key.size(), reinterpret_cast<uint8_t*>(&data[0]),
               data.size());
    return data;
}

// A Standard security handler (ISO 32000-1 section 7.6.3) for revision 2 (RC4, 40 bits) or 4
// (AESV2), with user password `password`, made the way Algorithms 2, 3 and 4/5 say.
struct Encryptor {
    int revision;
    std::string key;
    std::string o = std::string(32, 'O');
    std::string u;
    std::string id = "0123456789abcdef";

    Encryptor(int r, std::string_view password) : revision(r) {
        std::string padded(password.substr(0, 32));
        padded.append(reinterpret_cast<const char*>(kPad), 32 - padded.size());
        const std::string p("\xfc\xff\xff\xff", 4);  // /P -4
        const size_t n = r == 2 ? 5 : 16;
        std::string h = Md5(padded + o + p + id);
        if (r >= 3) {
            for (int i = 0; i < 50; i++) h = Md5(h.substr(0, n));
        }
        key = h.substr(0, n);
        const std::string pad(reinterpret_cast<const char*>(kPad), 32);
        if (r == 2) {
            u = Rc4(key, pad);
        } else {
            std::string x = Rc4(key, Md5(pad + id));
            for (int i = 1; i <= 19; i++) {
                std::string k = key;
                for (char& c : k) c = static_cast<char>(c ^ i);
                x = Rc4(k, x);
            }
            u = x + std::string(16, '\0');
        }
    }

    std::string Encrypt(int num, std::string_view data) const {
        std::string seed = key;
        seed.push_back(static_cast<char>(num & 0xFF));
        seed.push_back(static_cast<char>((num >> 8) & 0xFF));
        seed.push_back(static_cast<char>((num >> 16) & 0xFF));
        seed.append(2, '\0');
        if (revision == 4) seed += "sAlT";
        const std::string objectKey = Md5(seed).substr(0, std::min<size_t>(key.size() + 5, 16));
        if (revision != 4) return Rc4(objectKey, std::string(data));
        std::string padded(data);
        const size_t pad = 16 - padded.size() % 16;
        padded.append(pad, static_cast<char>(pad));
        const uint8_t iv[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        std::string out;
        Crypt::AesCbcEncrypt(reinterpret_cast<const uint8_t*>(objectKey.data()), 16, iv, padded, out);
        return std::string(reinterpret_cast<const char*>(iv), 16) + out;
    }

    std::string Dictionary() const {
        std::string d = "<< /Filter /Standard /O <" + Hex(o) + "> /U <" + Hex(u) + "> /P -4 ";
        if (revision == 2) return d + "/V 1 /R 2 >>";
        return d + "/V 4 /R 4 /Length 128 /CF << /StdCF << /CFM /AESV2 /AuthEvent /DocOpen /Length 16 >> >> "
                   "/StmF /StdCF /StrF /StdCF >>";
    }
};

std::string EncryptedDocument(const Encryptor& e, std::string_view content) {
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int stream = b.Reserve();
    const int page = b.Add("<< /Type /Page /Parent 2 0 R /Contents " + std::to_string(stream) + " 0 R >>");
    const int info = b.Reserve();
    const int encrypt = b.Add(e.Dictionary());
    const std::string data = e.Encrypt(stream, ZlibStored(content));
    b.Set(stream, "<< /Filter /FlateDecode /Length " + std::to_string(data.size()) + " >>\nstream\n" + data +
                      "\nendstream");
    b.Set(info, "<< /Title <" + Hex(e.Encrypt(info, "Secret title")) + "> >>");
    b.Set(catalog, "<< /Type /Catalog /Pages 2 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] >>");
    return b.Build(catalog, "/Info " + std::to_string(info) + " 0 R /Encrypt " + std::to_string(encrypt) +
                                " 0 R /ID [<" + Hex(e.id) + "> <" + Hex(e.id) + ">]");
}

void TestEncryption() {
    for (int revision : {2, 4}) {
        const Encryptor open(revision, "");
        const auto doc = OpenOk(EncryptedDocument(open, "BT (Plain) Tj ET"), 244);
        RAYOPDF_CHECK(doc->Report().encrypted && Content(*doc, 0) == "BT (Plain) Tj ET", 244);
        RAYOPDF_CHECK(doc->Get(doc->Info(), "Title").String() == "Secret title", 244);
        const Encryptor locked(revision, "secret");
        std::unique_ptr<Document> none;
        RAYOPDF_CHECK(Document::Open(EncryptedDocument(locked, "x"), "", none) == OpenStatus::PasswordRequired, 246);
        const auto unlocked = OpenOk(EncryptedDocument(locked, "BT (Locked) Tj ET"), 246, "secret");
        RAYOPDF_CHECK(Content(*unlocked, 0) == "BT (Locked) Tj ET", 246);
    }
    // A security handler RayoPDF does not have.
    std::string file = OnePage("x");
    const size_t at = file.rfind(">>\nstartxref");
    file.insert(at, "/Encrypt << /Filter /Adobe.PubSec /V 4 /R 4 >> ");
    std::unique_ptr<Document> none;
    RAYOPDF_CHECK(Document::Open(file, "", none) == OpenStatus::Unsupported, 247);
}

// Content ending right after "BI ID": the inline image's data is empty, nothing is read past the
// end (a fuzz finding of Layout's content target).
void TestInlineImageAtEnd() {
    for (const std::string_view data : {std::string_view("q BI ID"), std::string_view("q BI /W 4 ID "),
                                        std::string_view("BI ID\nab EI Q")}) {
        ContentLexer lexer(data);
        std::string_view op;
        std::vector<PdfValue> operands;
        bool inlineImage = false;
        while (lexer.Next(op, operands)) {
            if (op == "BI") {
                inlineImage = true;
                RAYOPDF_CHECK(lexer.InlineImageData().size() <= data.size(), 251);
            }
            RAYOPDF_CHECK(lexer.Offset() <= data.size(), 251);
        }
        RAYOPDF_CHECK(inlineImage, 251);
    }
}

// The lexer's operands and their numbers: Numbers gives the last ones only when all are finite
// numbers; a number at the data's very end reads as anywhere else; past kMaxOperands only the
// newest operand stays, its number with it.
void TestContentOperands() {
    const std::string infinite = "1" + std::string(400, '0') + " 2 l 1 2 m -.5 3. 7";
    ContentLexer lexer(infinite);
    std::string_view op;
    RAYOPDF_CHECK(lexer.Next(op) && op == "l" && lexer.Operands().size() == 2, 261);
    RAYOPDF_CHECK(std::isinf(lexer.Operands()[0].real) && lexer.Numbers(2) == nullptr && *lexer.Numbers(1) == 2, 261);
    RAYOPDF_CHECK(lexer.Next(op) && op == "m" && lexer.Numbers(2) && lexer.Numbers(2)[0] == 1 && !lexer.Numbers(3), 261);
    std::vector<PdfValue> rest;
    RAYOPDF_CHECK(!lexer.Next(op, rest) && rest.size() == 3 && rest[2].type == PdfValue::Type::Int &&
                      rest[2].integer == 7 && rest[1].real == 3.0 && lexer.Numbers(3)[0] == -0.5,
                  261);
    ContentLexer mixed("/F1 12 Tf 1 true 2 (x) 0.25 Tj");
    RAYOPDF_CHECK(mixed.Next(op) && op == "Tf" && *mixed.Numbers(1) == 12 && !mixed.Numbers(2), 261);
    RAYOPDF_CHECK(mixed.Next(op) && op == "Tj" && mixed.Operands().size() == 5 && *mixed.Numbers(1) == 0.25 &&
                      !mixed.Numbers(2) && mixed.Operands()[3].String() == "x",
                  261);

    std::string many;
    for (size_t i = 0; i < ContentLexer::kMaxOperands; i++) many += "/N ";
    many += "5 4 re";
    ContentLexer overflow(many);
    RAYOPDF_CHECK(overflow.Next(op) && op == "re" && overflow.Operands().size() == 2 && overflow.Numbers(2) &&
                      overflow.Numbers(2)[0] == 5 && overflow.Numbers(2)[1] == 4,
                  262);
}

// The lexer's numbers have the type and bits Syntax::ParseNumber gives their tokens: signs, a
// point without digits on one side, 18 and 19 digits (past INT64_MAX a Real), leading zeros
// beyond 19 digits.
void TestContentNumbers() {
    const char* const tokens[] = {"0", "-0", "+7", "5.", ".5", "-.25", "123456789012345678", "-1234567890123456789",
                                  "9999999999999999999", "0.000000000000000001", "00000000000000000012.5", "4.000"};
    std::string data;
    for (const char* token : tokens) data += std::string(token) + " ";
    data += "op";
    ContentLexer lexer(data);
    std::string_view op;
    RAYOPDF_CHECK(lexer.Next(op) && op == "op" && lexer.Operands().size() == std::size(tokens), 264);
    const PdfSpan<PdfValue> operands = lexer.Operands();
    for (size_t i = 0; i < operands.size() && i < std::size(tokens); i++) {
        PdfValue expected;
        RAYOPDF_CHECK(Syntax::ParseNumber(tokens[i], expected) && operands[i].type == expected.type &&
                          operands[i].integer == expected.integer &&
                          lexer.Numbers(std::size(tokens) - i)[0] == operands[i].Number(),
                      264);
    }
}

// The lexer's arrays as Syntax::ParseValue reads them: strings with and without escapes, hex
// strings, names, nested arrays, keywords, a comment and a dictionary among numbers; an item that
// is no value fails the array (its operands go, the lexer goes on after it); an unended one ends
// the data.
void TestContentArrays() {
    const std::string_view text = "[(a) -12 (b\\)c) 3.5 <4142> /N [1 (x)] true null %c\n 7 <<>> ] TJ";
    ContentLexer lexer(text);
    std::string_view op;
    RAYOPDF_CHECK(lexer.Next(op) && op == "TJ" && lexer.Operands().size() == 1 && lexer.Operands()[0].IsArray(), 263);
    PdfArena arena;
    Syntax::Builder copies(arena);
    Syntax::Cursor cursor{text, 0};
    PdfValue expected;
    RAYOPDF_CHECK(Syntax::ParseValue(cursor, Syntax::Mode::Content, 0, copies, expected), 263);
    const PdfSpan<PdfValue> got = lexer.Operands()[0].Array();
    const PdfSpan<PdfValue> want = expected.Array();
    RAYOPDF_CHECK(got.size() == 11 && want.size() == 11, 263);
    for (size_t i = 0; i < got.size() && i < want.size(); i++) {
        RAYOPDF_CHECK(got[i].type == want[i].type && got[i].boolean == want[i].boolean && got[i].count == want[i].count,
                      263);
        if (got[i].IsNumber()) RAYOPDF_CHECK(got[i].integer == want[i].integer, 263);
        if (got[i].IsString() || got[i].IsName()) RAYOPDF_CHECK(got[i].Text() == want[i].Text(), 263);
        if (got[i].IsArray()) RAYOPDF_CHECK(got[i].Array()[1].String() == "x" && got[i].Array()[0].integer == 1, 263);
    }
    RAYOPDF_CHECK(got.size() == 11 && got[2].String() == "b)c" && got[4].String() == "AB" && got[3].real == 3.5 &&
                      got[9].integer == 7,
                  263);

    ContentLexer failed("[1 2 1e3] 5 TJ");
    RAYOPDF_CHECK(failed.Next(op) && op == "TJ" && failed.Operands().size() == 1 && *failed.Numbers(1) == 5, 263);
    ContentLexer unended("[(ab");
    RAYOPDF_CHECK(!unended.Next(op) && unended.Offset() == 4, 263);
}

// The string, name and number readers: hex strings with white space and an odd last digit, the
// cursor after a failure, literal strings with and without escapes, names and strings viewing the
// data with Builder::views, and numbers read at once with the reference test of File mode.
void TestSyntaxReaders() {
    using Syntax::Cursor;
    PdfArena arena;
    Syntax::Builder copies(arena);
    Syntax::Builder views(arena, true);
    std::string_view out;
    Cursor hex{"<48 65 6C6c\n6> x", 0};
    RAYOPDF_CHECK(Syntax::ReadHexString(hex, copies, out) && out == "Hel\x6C\x60" && hex.pos == 14, 257);
    Cursor empty{"< >", 0};
    RAYOPDF_CHECK(Syntax::ReadHexString(empty, copies, out) && out.empty() && empty.pos == 3, 257);
    Cursor bad{"<41x42>", 0};
    RAYOPDF_CHECK(!Syntax::ReadHexString(bad, copies, out) && bad.pos == 4, 257);
    Cursor open{"<4142", 0};
    RAYOPDF_CHECK(!Syntax::ReadHexString(open, copies, out) && open.pos == 5, 257);

    const std::string_view plain = "(plain text) Tj";
    Cursor viewed{plain, 0};
    RAYOPDF_CHECK(Syntax::ReadLiteralString(viewed, views, out) && out == "plain text" &&
                      out.data() == plain.data() + 1 && viewed.pos == 12,
                  258);
    Cursor copied{plain, 0};
    RAYOPDF_CHECK(Syntax::ReadLiteralString(copied, copies, out) && out == "plain text" &&
                      out.data() != plain.data() + 1 && copied.pos == 12,
                  258);
    Cursor escaped{"(a\\)b(c)d\r\ne\\101) Tj", 0};
    RAYOPDF_CHECK(Syntax::ReadLiteralString(escaped, views, out) && out == "a)b(c)d\neA" && escaped.pos == 17, 258);
    Cursor unended{"(abc", 0};
    RAYOPDF_CHECK(!Syntax::ReadLiteralString(unended, views, out), 258);
    const std::string_view name = "/F1 12 Tf";
    Cursor named{name, 0};
    RAYOPDF_CHECK(Syntax::ReadName(named, views, out) && out == "F1" && out.data() == name.data() + 1, 258);
    Cursor hashed{"/A#42 ", 0};
    RAYOPDF_CHECK(Syntax::ReadName(hashed, views, out) && out == "AB" && hashed.pos == 5, 258);

    PdfValue value;
    Cursor reference{"12 0 R", 0};
    RAYOPDF_CHECK(Syntax::ParseValue(reference, Syntax::Mode::File, 0, copies, value) && value.IsRef() &&
                      value.refNum == 12 && reference.pos == 6,
                  259);
    Cursor sign{"+12 0 R", 0};
    RAYOPDF_CHECK(Syntax::ParseValue(sign, Syntax::Mode::File, 0, copies, value) &&
                      value.type == PdfValue::Type::Int && value.integer == 12 && sign.pos == 3,
                  259);
    Cursor content{"12 0 R", 0};
    RAYOPDF_CHECK(Syntax::ParseValue(content, Syntax::Mode::Content, 0, copies, value) &&
                      value.type == PdfValue::Type::Int && content.pos == 2,
                  259);
    Cursor numbers{"[-.5 3. 12345678901234567890]", 0};
    RAYOPDF_CHECK(Syntax::ParseValue(numbers, Syntax::Mode::Content, 0, copies, value) && value.IsArray() &&
                      value.Array().size() == 3 && value.Array()[0].real == -0.5 && value.Array()[1].real == 3.0 &&
                      value.Array()[2].type == PdfValue::Type::Real && value.Array()[2].real == 12345678901234567890.0,
                  259);
    // The reference look-ahead: comments and line ends between the tokens; and what is no
    // reference (a six-digit generation, "Rx", a generation past 65535, a name after).
    const struct {
        const char* text;
        bool ref;
        size_t end;
        uint16_t gen;
    } refs[] = {
        {"7 0 R]", true, 5, 0},           {"7\r\n2 % c\n R/F", true, 11, 2}, {"7 %c\n00012 R", true, 12, 12},
        {"7 000012 R", false, 1, 0},      {"7 0 Rx", false, 1, 0},           {"7 65536 R", false, 1, 0},
        {"7 65535 R", true, 9, 65535},    {"7 0/R", false, 1, 0},            {"7 0", false, 1, 0},
        {"7 0 R", true, 5, 0},            {"7 0a R", false, 1, 0},           {"7 0 %R\n", false, 1, 0},
    };
    for (const auto& r : refs) {
        Cursor at{r.text, 0};
        RAYOPDF_CHECK(Syntax::ParseValue(at, Syntax::Mode::File, 0, copies, value) && value.IsRef() == r.ref &&
                          at.pos == r.end && (!r.ref || (value.refNum == 7 && value.refGen == r.gen)),
                      260);
    }
    // File-mode arrays: a number's reference test reads the next item, which is then not read
    // again; every item as ParseValue gives it alone.
    Cursor items{"[1 2 3 4 0 R 5 6 R 70000 8 0 R 9 %c\n 0 R 10 0.5 11 00012 R 1 65536 2 R 123456 0 R -3 7 0 Rx]", 0};
    const struct {
        PdfValue::Type type;
        int64_t number;
        uint16_t gen;
    } expected[] = {
        {PdfValue::Type::Int, 1, 0},     {PdfValue::Type::Int, 2, 0},     {PdfValue::Type::Int, 3, 0},
        {PdfValue::Type::Ref, 4, 0},     {PdfValue::Type::Ref, 5, 6},     {PdfValue::Type::Int, 70000, 0},
        {PdfValue::Type::Ref, 8, 0},     {PdfValue::Type::Ref, 9, 0},     {PdfValue::Type::Int, 10, 0},
        {PdfValue::Type::Real, 0, 0},    {PdfValue::Type::Ref, 11, 12},   {PdfValue::Type::Int, 1, 0},
        {PdfValue::Type::Ref, 65536, 2}, {PdfValue::Type::Ref, 123456, 0}, {PdfValue::Type::Int, -3, 0},
        {PdfValue::Type::Int, 7, 0},     {PdfValue::Type::Int, 0, 0},
    };
    RAYOPDF_CHECK(!Syntax::ParseValue(items, Syntax::Mode::File, 0, copies, value), 280);  // "Rx" ends it
    Cursor itemsEnded{"[1 2 3 4 0 R 5 6 R 70000 8 0 R 9 %c\n 0 R 10 0.5 11 00012 R 1 65536 2 R 123456 0 R -3 7 0]", 0};
    RAYOPDF_CHECK(Syntax::ParseValue(itemsEnded, Syntax::Mode::File, 0, copies, value) && value.IsArray() &&
                      value.Array().size() == sizeof expected / sizeof expected[0] && itemsEnded.AtEnd(),
                  280);
    const PdfSpan<PdfValue> got = value.Array();
    for (size_t i = 0; i < got.size() && i < sizeof expected / sizeof expected[0]; ++i) {
        const PdfValue& item = got[i];
        const bool same = item.type == expected[i].type &&
                          (item.type == PdfValue::Type::Ref
                               ? item.refNum == expected[i].number && item.refGen == expected[i].gen
                               : item.type == PdfValue::Type::Int ? item.integer == expected[i].number
                                                                  : item.real == 0.5);
        RAYOPDF_CHECK(same, 280);
    }
}

} // namespace

int main() {
    TestKinds();
    TestFlate();
    TestLzw();
    TestAscii();
    TestPredictors();
    TestClassic();
    TestXrefStreams();
    TestHybrid();
    TestPrevChain();
    TestGarbageAndRepair();
    TestStreams();
    TestStreamReader();
    TestNesting();
    TestPageTree();
    TestXrefQuirks();
    TestEncryption();
    TestInlineImageAtEnd();
    TestContentOperands();
    TestContentNumbers();
    TestContentArrays();
    TestSyntaxReaders();
    std::puts("test_pdf: all checks passed");
    return 0;
}
