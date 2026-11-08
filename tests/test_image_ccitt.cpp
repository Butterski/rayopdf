// SPDX-License-Identifier: Apache-2.0
// Core tests of the CCITTFaxDecode decoder (src/image/ccitt.h, exit codes 1050-1099): hand-coded
// bit strings, then an encoder written here from T.4/T.6 (its own copy of the code tables, as bit
// strings) that codes test images in every mode the filter's parameters select: Group 4, Group 3
// one-dimensional and mixed, with and without end-of-line patterns, each byte-alignment layout,
// EOFB and RTC, every run length of both colours. Then /Rows, /EndOfBlock, /BlackIs1, damaged
// rows, truncated data and the limits.
#include "check.h"

#include "image/ccitt.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace RayoPdf::Image;

namespace {

// ---------------------------------------------------------------- the encoder's code tables

const char* const kWhiteTerminating[64] = {
    "00110101", "000111",   "0111",     "1000",     "1011",     "1100",     "1110",     "1111",
    "10011",    "10100",    "00111",    "01000",    "001000",   "000011",   "110100",   "110101",
    "101010",   "101011",   "0100111",  "0001100",  "0001000",  "0010111",  "0000011",  "0000100",
    "0101000",  "0101011",  "0010011",  "0100100",  "0011000",  "00000010", "00000011", "00011010",
    "00011011", "00010010", "00010011", "00010100", "00010101", "00010110", "00010111", "00101000",
    "00101001", "00101010", "00101011", "00101100", "00101101", "00000100", "00000101", "00001010",
    "00001011", "01010010", "01010011", "01010100", "01010101", "00100100", "00100101", "01011000",
    "01011001", "01011010", "01011011", "01001010", "01001011", "00110010", "00110011", "00110100",
};

// Runs 64, 128, ..., 1728.
const char* const kWhiteMakeup[27] = {
    "11011",     "10010",     "010111",    "0110111",   "00110110",  "00110111",  "01100100",
    "01100101",  "01101000",  "01100111",  "011001100", "011001101", "011010010", "011010011",
    "011010100", "011010101", "011010110", "011010111", "011011000", "011011001", "011011010",
    "011011011", "010011000", "010011001", "010011010", "011000",    "010011011",
};

const char* const kBlackTerminating[64] = {
    "0000110111",   "010",          "11",           "10",           "011",
    "0011",         "0010",         "00011",        "000101",       "000100",
    "0000100",      "0000101",      "0000111",      "00000100",     "00000111",
    "000011000",    "0000010111",   "0000011000",   "0000001000",   "00001100111",
    "00001101000",  "00001101100",  "00000110111",  "00000101000",  "00000010111",
    "00000011000",  "000011001010", "000011001011", "000011001100", "000011001101",
    "000001101000", "000001101001", "000001101010", "000001101011", "000011010010",
    "000011010011", "000011010100", "000011010101", "000011010110", "000011010111",
    "000001101100", "000001101101", "000011011010", "000011011011", "000001010100",
    "000001010101", "000001010110", "000001010111", "000001100100", "000001100101",
    "000001010010", "000001010011", "000000100100", "000000110111", "000000111000",
    "000000100111", "000000101000", "000001011000", "000001011001", "000000101011",
    "000000101100", "000001011010", "000001100110", "000001100111",
};

const char* const kBlackMakeup[27] = {
    "0000001111",    "000011001000",  "000011001001",  "000001011011",  "000000110011",
    "000000110100",  "000000110101",  "0000001101100", "0000001101101", "0000001001010",
    "0000001001011", "0000001001100", "0000001001101", "0000001110010", "0000001110011",
    "0000001110100", "0000001110101", "0000001110110", "0000001110111", "0000001010010",
    "0000001010011", "0000001010100", "0000001010101", "0000001011010", "0000001011011",
    "0000001100100", "0000001100101",
};

// Runs 1792, 1856, ..., 2560, both colours.
const char* const kExtendedMakeup[13] = {
    "00000001000",  "00000001100",  "00000001101",  "000000010010", "000000010011",
    "000000010100", "000000010101", "000000010110", "000000010111", "000000011100",
    "000000011101", "000000011110", "000000011111",
};

const char* const kEol = "000000000001";

// ---------------------------------------------------------------- bit writer

class BitWriter {
public:
    void Put(const char* bits) {
        for (const char* p = bits; *p; p++) PutBit(*p == '1');
    }
    void PutBit(bool bit) {
        if (count_ % 8 == 0) bytes_.push_back(0);
        if (bit) bytes_.back() = static_cast<char>(bytes_.back() | (0x80 >> (count_ % 8)));
        count_++;
    }
    void Align() {
        while (count_ % 8 != 0) PutBit(false);
    }
    size_t Bits() const { return count_; }
    const std::string& Bytes() const { return bytes_; }

private:
    std::string bytes_;
    size_t count_ = 0;
};

// ---------------------------------------------------------------- encoder

using Row = std::vector<uint8_t>;  // one byte per pixel, 1 black
using Image = std::vector<Row>;

void PutRun(BitWriter& out, uint32_t run, bool black) {
    while (run >= 2560 + 64) {  // longer runs repeat the largest make-up code
        out.Put(kExtendedMakeup[12]);
        run -= 2560;
    }
    if (run >= 64) {
        const uint32_t makeup = run / 64;  // 1-40
        if (makeup <= 27) {
            out.Put(black ? kBlackMakeup[makeup - 1] : kWhiteMakeup[makeup - 1]);
        } else {
            out.Put(kExtendedMakeup[makeup - 28]);
        }
        run %= 64;
    }
    out.Put(black ? kBlackTerminating[run] : kWhiteTerminating[run]);
}

void Encode1D(BitWriter& out, const Row& row) {
    const uint32_t columns = static_cast<uint32_t>(row.size());
    uint32_t x = 0;
    bool black = false;
    while (x < columns) {
        uint32_t end = x;
        while (end < columns && (row[end] != 0) == black) end++;
        PutRun(out, end - x, black);
        x = end;
        black = !black;
    }
}

// The first changing element right of `from` (-1: the row's start) whose colour is `black`
// (a changing element's colour is that of its pixel), or columns.
uint32_t NextChangeOf(const Row& row, int64_t from, bool black) {
    const uint32_t columns = static_cast<uint32_t>(row.size());
    for (int64_t x = from + 1; x < columns; x++) {
        const bool previous = x > 0 ? row[size_t(x - 1)] != 0 : false;
        const bool here = row[size_t(x)] != 0;
        if (here != previous && here == black) return static_cast<uint32_t>(x);
    }
    return columns;
}

uint32_t NextChange(const Row& row, int64_t from) {
    const uint32_t columns = static_cast<uint32_t>(row.size());
    for (int64_t x = from + 1; x < columns; x++) {
        const bool previous = x > 0 ? row[size_t(x - 1)] != 0 : false;
        if ((row[size_t(x)] != 0) != previous) return static_cast<uint32_t>(x);
    }
    return columns;
}

// T.4 4.2.1.3.4: pass, vertical or horizontal mode for each changing element.
void Encode2D(BitWriter& out, const Row& reference, const Row& row) {
    static const char* const kVertical[7] = {"0000010", "000010", "010", "1", "011", "000011", "0000011"};
    const uint32_t columns = static_cast<uint32_t>(row.size());
    int64_t a0 = -1;
    bool black = false;
    while (a0 < int64_t(columns)) {
        const uint32_t a1 = NextChange(row, a0);
        const uint32_t b1 = NextChangeOf(reference, a0, !black);
        const uint32_t b2 = NextChange(reference, b1);
        if (b2 < a1) {
            out.Put("0001");
            a0 = b2;
        } else if (int64_t(a1) - b1 >= -3 && int64_t(a1) - b1 <= 3) {
            out.Put(kVertical[int64_t(a1) - b1 + 3]);
            a0 = a1;
            black = !black;
        } else {
            const uint32_t a2 = NextChange(row, a1);
            out.Put("001");
            const uint32_t start = a0 < 0 ? 0 : static_cast<uint32_t>(a0);
            PutRun(out, a1 - start, black);
            PutRun(out, a2 - a1, !black);
            a0 = a2;
        }
    }
}

enum class Align : uint8_t {
    None,
    Rows,         // every row starts on a byte boundary (no end-of-line patterns)
    FillThenEol,  // fill bits so that each end-of-line pattern ends on a byte boundary
    EolAtByte,    // each end-of-line pattern starts on a byte boundary, the row follows directly
};

struct Mode {
    int32_t k = -1;
    bool eol = false;
    Align align = Align::None;
    bool terminator = true;  // EOFB (K < 0) or RTC
};

void PutEol(BitWriter& out, Align align) {
    if (align == Align::FillThenEol) {
        while ((out.Bits() + 12) % 8 != 0) out.PutBit(false);
    } else if (align == Align::EolAtByte) {
        out.Align();
    }
    out.Put(kEol);
}

std::string Encode(const Image& image, const Mode& mode) {
    BitWriter out;
    Row white(image.empty() ? 0 : image[0].size(), 0);
    for (size_t y = 0; y < image.size(); y++) {
        if (mode.eol) {
            PutEol(out, mode.align);
        } else if (mode.align == Align::Rows) {
            out.Align();
        }
        const bool twoDimensional = mode.k < 0 || (mode.k > 0 && y % static_cast<size_t>(mode.k) != 0);
        if (mode.k > 0) out.PutBit(!twoDimensional);
        if (twoDimensional) {
            Encode2D(out, y == 0 ? white : image[y - 1], image[y]);
        } else {
            Encode1D(out, image[y]);
        }
    }
    if (mode.terminator) {
        if (mode.align == Align::Rows) out.Align();
        const int count = mode.k < 0 ? 2 : 6;
        for (int i = 0; i < count; i++) {
            PutEol(out, mode.align == Align::Rows ? Align::None : mode.align);
            if (mode.k > 0) out.PutBit(true);
        }
    }
    return out.Bytes();
}

// The filter's output of `image`: packed rows, 0 black (or 1 with blackIs1), the pad bits after
// the last column black.
std::string Pack(const Image& image, bool blackIs1) {
    std::string out;
    for (const Row& row : image) {
        const size_t stride = (row.size() + 7) / 8;
        std::string bytes(stride, '\0');
        for (size_t x = 0; x < stride * 8; x++) {
            const bool black = x >= row.size() || row[x] != 0;
            if (black == blackIs1) bytes[x / 8] = static_cast<char>(bytes[x / 8] | (0x80 >> (x % 8)));
        }
        out += bytes;
    }
    return out;
}

// ---------------------------------------------------------------- test images

uint32_t g_seed = 1;

uint32_t Next() {
    g_seed = (1103515245u * g_seed + 12345u) & 0x7FFFFFFFu;
    return g_seed >> 8;
}

// Rows that mostly follow the row above (so every two-dimensional mode occurs), with new runs,
// shifted edges and the occasional unrelated row.
Image MakeImage(uint32_t columns, uint32_t height) {
    Image image;
    Row previous(columns, 0);
    for (uint32_t y = 0; y < height; y++) {
        Row row = previous;
        const uint32_t kind = Next() % 8;
        if (kind == 0 || y == 0) {
            bool black = Next() % 2 != 0;
            for (uint32_t x = 0; x < columns;) {
                const uint32_t run = 1 + Next() % (Next() % 4 == 0 ? 300 : 12);
                for (uint32_t i = 0; i < run && x < columns; i++, x++) row[x] = black ? 1 : 0;
                black = !black;
            }
        } else {
            const uint32_t edits = 1 + Next() % 6;
            for (uint32_t e = 0; e < edits; e++) {
                const uint32_t x = Next() % columns;
                const uint32_t length = 1 + Next() % 9;
                const uint8_t value = static_cast<uint8_t>(Next() % 2);
                for (uint32_t i = 0; i < length && x + i < columns; i++) row[x + i] = value;
            }
            if (kind == 1 && columns > 4) {  // shift right by 1-4
                const uint32_t shift = 1 + Next() % 4;
                for (uint32_t x = columns; x-- > shift;) row[x] = row[x - shift];
            }
        }
        if (y % 17 == 5) row.assign(columns, 0);
        if (y % 23 == 7) row.assign(columns, 1);
        image.push_back(row);
        previous = row;
    }
    return image;
}

Ccitt::Result Run(const std::string& data, const Ccitt::Params& params, std::string& out, Ccitt::Info& info,
                  uint32_t maxRows = 0xFFFFFFFFu, uint64_t maxPixels = uint64_t(1) << 28) {
    return Ccitt::Decode(reinterpret_cast<const uint8_t*>(data.data()), data.size(), params, maxRows, maxPixels, out,
                         info);
}

std::string Bits(const char* bits) {
    BitWriter out;
    out.Put(bits);
    return out.Bytes();
}

// ---------------------------------------------------------------- tests

void TestHandCoded() {
    Ccitt::Params params;
    std::string out;
    Ccitt::Info info;

    // One-dimensional, 8 columns: white 3 (1000), black 2 (11), white 3 (1000).
    params.k = 0;
    params.columns = 8;
    RAYOPDF_CHECK(Run(Bits("1000" "11" "1000"), params, out, info) == Ccitt::Result::Ok, 1050);
    RAYOPDF_CHECK(info.rows == 1 && out == std::string(1, '\xE7'), 1051);

    // Group 4, 10 columns, two rows: H with white 2 (0111) and black 3 (10), then V0 (b1 is the
    // white row's end, 10); then V0 three times (copying the changes at 2 and 5, then the end),
    // then EOFB.
    params.k = -1;
    params.columns = 10;
    RAYOPDF_CHECK(Run(Bits("001" "0111" "10" "1" "1" "1" "1" "000000000001" "000000000001"), params, out, info) ==
                      Ccitt::Result::Ok,
                  1052);
    RAYOPDF_CHECK(info.rows == 2 && info.endOfBlock && out == std::string("\xC7\xC0\xC7\xC0", 4), 1053);

    // Long codes: black run 1728 + 63 (0000001100101 000001100111) after white 0, 1791 columns,
    // with /BlackIs1: all ones.
    params.k = 0;
    params.columns = 1791;
    params.blackIs1 = true;
    RAYOPDF_CHECK(Run(Bits("00110101" "0000001100101" "000001100111"), params, out, info) == Ccitt::Result::Ok, 1054);
    std::string expected(224, '\xFF');
    expected.back() = '\xFF';  // 1791 = 223 * 8 + 7: the pad bit is black (1)
    RAYOPDF_CHECK(info.rows == 1 && out == expected, 1055);
}

// Every run length 0-5200 of both colours, one-dimensional (each code of the tables, the extended
// make-up codes and repeated 2560 make-up codes).
void TestAllRuns() {
    const uint32_t columns = 5300;
    Image image;
    for (uint32_t run = 0; run <= 5200; run += run < 200 ? 1 : 37) {
        Row row(columns, 0);
        for (uint32_t x = 0; x < run; x++) row[x] = 0;         // white run `run`
        for (uint32_t x = run; x < columns; x++) row[x] = 1;   // then black to the end
        image.push_back(row);
        Row other(columns, 1);
        for (uint32_t x = 0; x < 1; x++) other[x] = 0;         // white 1, black `run`, white rest
        for (uint32_t x = 1 + run; x < columns; x++) other[x] = 0;
        image.push_back(other);
    }
    Mode mode;
    mode.k = 0;
    const std::string data = Encode(image, mode);
    Ccitt::Params params;
    params.columns = columns;
    std::string out;
    Ccitt::Info info;
    RAYOPDF_CHECK(Run(data, params, out, info) == Ccitt::Result::Ok, 1056);
    RAYOPDF_CHECK(info.rows == image.size() && out == Pack(image, false), 1057);
}

void CheckRoundTrip(const Image& image, const Mode& mode, bool blackIs1, int code) {
    const std::string data = Encode(image, mode);
    Ccitt::Params params;
    params.k = mode.k;
    params.endOfLine = mode.eol;
    params.encodedByteAlign = mode.align != Align::None;
    params.columns = static_cast<uint32_t>(image[0].size());
    params.rows = static_cast<uint32_t>(image.size());
    params.endOfBlock = mode.terminator;
    params.blackIs1 = blackIs1;
    std::string out;
    Ccitt::Info info;
    const Ccitt::Result result = Run(data, params, out, info);
    if (result != Ccitt::Result::Ok || info.rows != image.size() || out != Pack(image, blackIs1) ||
        info.endOfBlock != mode.terminator) {
        std::fprintf(stderr, "round trip: k %d eol %d align %d terminator %d columns %zu rows %u/%zu result %d\n",
                     mode.k, mode.eol, static_cast<int>(mode.align), mode.terminator, image[0].size(), info.rows,
                     image.size(), static_cast<int>(result));
    }
    RAYOPDF_CHECK(result == Ccitt::Result::Ok, code);
    RAYOPDF_CHECK(info.rows == image.size() && out == Pack(image, blackIs1), code + 1);
    RAYOPDF_CHECK(info.endOfBlock == mode.terminator, code + 2);
    RAYOPDF_CHECK(info.bytesUsed <= data.size() && (!mode.terminator || info.bytesUsed == data.size()), code + 3);
}

void TestModes() {
    const uint32_t widths[] = {1, 7, 8, 13, 64, 200, 1728, 2600};
    for (uint32_t columns : widths) {
        const Image image = MakeImage(columns, columns < 64 ? 40 : 90);
        Mode mode;
        mode.k = -1;  // Group 4
        CheckRoundTrip(image, mode, false, 1058);
        mode.align = Align::Rows;
        CheckRoundTrip(image, mode, true, 1058);
        mode.align = Align::None;
        mode.terminator = false;
        CheckRoundTrip(image, mode, false, 1058);

        for (int32_t k : {0, 1, 2, 4}) {  // Group 3: one-dimensional and mixed
            Mode g3;
            g3.k = k;
            g3.eol = true;
            CheckRoundTrip(image, g3, false, 1062);
            g3.align = Align::FillThenEol;
            CheckRoundTrip(image, g3, true, 1062);
            g3.align = Align::EolAtByte;
            CheckRoundTrip(image, g3, false, 1062);
            g3.eol = false;
            g3.align = Align::None;
            g3.terminator = false;
            CheckRoundTrip(image, g3, false, 1062);
            g3.align = Align::Rows;
            CheckRoundTrip(image, g3, true, 1062);
        }
    }
}

void TestStops() {
    const Image image = MakeImage(100, 30);
    Mode mode;
    mode.k = -1;
    const std::string data = Encode(image, mode);
    Ccitt::Params params;
    params.k = -1;
    params.columns = 100;
    std::string out;
    Ccitt::Info info;

    // /Rows stops only without /EndOfBlock; maxRows always.
    params.rows = 10;
    params.endOfBlock = false;
    RAYOPDF_CHECK(Run(data, params, out, info) == Ccitt::Result::Ok && info.rows == 10, 1066);
    RAYOPDF_CHECK(out == Pack(Image(image.begin(), image.begin() + 10), false), 1067);
    params.endOfBlock = true;
    RAYOPDF_CHECK(Run(data, params, out, info) == Ccitt::Result::Ok && info.rows == 30 && info.endOfBlock, 1068);
    RAYOPDF_CHECK(Run(data, params, out, info, 12) == Ccitt::Result::Ok && info.rows == 12 &&
                      out == Pack(Image(image.begin(), image.begin() + 12), false) && info.bytesUsed < data.size(),
                  1069);

    // EOFB before /Rows: the rows before it.
    params.rows = 50;
    RAYOPDF_CHECK(Run(data, params, out, info) == Ccitt::Result::Ok && info.rows == 30 && info.endOfBlock, 1070);

    // No terminator, data ending at a row boundary (zero fill after the last row): all rows, Ok.
    mode.terminator = false;
    std::string bare = Encode(image, mode);
    bare.push_back('\0');
    RAYOPDF_CHECK(Run(bare, params, out, info) == Ccitt::Result::Ok && info.rows == 30 && !info.endOfBlock, 1071);

    // Data cut inside a row: Corrupt, the complete rows before it kept.
    // (Half of the data ends inside a row of this image.)
    const std::string cut = data.substr(0, data.size() / 2);
    RAYOPDF_CHECK(Run(cut, params, out, info) == Ccitt::Result::Corrupt, 1072);
    RAYOPDF_CHECK(info.rows > 0 && info.rows < 30 &&
                      out == Pack(Image(image.begin(), image.begin() + info.rows), false),
                  1073);
}

void TestDamagedRows() {
    // Group 3 one-dimensional with end-of-line patterns; row 3 gets a code that is no code
    // (0000000 followed by more zeros within the row is invalid until the next pattern).
    const uint32_t columns = 40;
    const Image image = MakeImage(columns, 8);
    BitWriter out;
    for (size_t y = 0; y < image.size(); y++) {
        out.Put(kEol);
        if (y == 3 || y == 4) {
            out.Put("1011" "000000001");  // white 4, then 8 zeros and a 1: not a black code
            continue;
        }
        Encode1D(out, image[y]);
    }
    for (int i = 0; i < 6; i++) out.Put(kEol);
    Ccitt::Params params;
    params.k = 0;
    params.endOfLine = true;
    params.columns = columns;
    std::string decoded;
    Ccitt::Info info;

    // Not tolerated: the three rows before.
    RAYOPDF_CHECK(Run(out.Bytes(), params, decoded, info) == Ccitt::Result::Corrupt && info.rows == 3, 1074);
    RAYOPDF_CHECK(decoded == Pack(Image(image.begin(), image.begin() + 3), false), 1075);

    // Two tolerated: row 3 repeats row 2, row 4 (after a damaged row) is white.
    params.damagedRowsBeforeError = 2;
    RAYOPDF_CHECK(Run(out.Bytes(), params, decoded, info) == Ccitt::Result::Ok && info.rows == 8 &&
                      info.damagedRows == 2,
                  1076);
    Image expected = image;
    expected[3] = image[2];
    expected[4] = Row(columns, 0);
    RAYOPDF_CHECK(decoded == Pack(expected, false), 1077);

    // One tolerated: the second is the error.
    params.damagedRowsBeforeError = 1;
    RAYOPDF_CHECK(Run(out.Bytes(), params, decoded, info) == Ccitt::Result::Corrupt && info.rows == 4, 1078);

    // Without /EndOfLine the entry does not apply.
    params.damagedRowsBeforeError = 5;
    params.endOfLine = false;
    RAYOPDF_CHECK(Run(out.Bytes(), params, decoded, info) == Ccitt::Result::Corrupt && info.rows == 3, 1079);
}

void TestLimits() {
    const Image image = MakeImage(300, 20);
    Mode mode;
    mode.k = -1;
    const std::string data = Encode(image, mode);
    Ccitt::Params params;
    params.k = -1;
    params.columns = 300;
    std::string out;
    Ccitt::Info info;

    params.columns = 0;
    RAYOPDF_CHECK(Run(data, params, out, info) == Ccitt::Result::InvalidParams && out.empty(), 1080);
    params.columns = 300;
    // 10 rows fit in 3000 pixels: TooLarge with those rows.
    RAYOPDF_CHECK(Run(data, params, out, info, 0xFFFFFFFFu, 3000) == Ccitt::Result::TooLarge && info.rows == 10 &&
                      out == Pack(Image(image.begin(), image.begin() + 10), false),
                  1081);
    params.columns = 0x7FFFFFFFu;
    RAYOPDF_CHECK(Run(data, params, out, info) == Ccitt::Result::TooLarge, 1082);

    // Uncompressed mode, two-dimensional (0000001 111) and one-dimensional (000000001111).
    params.columns = 16;
    RAYOPDF_CHECK(Run(Bits("1" "0000001111" "1"), params, out, info) == Ccitt::Result::Unsupported, 1083);
    params.k = 0;
    RAYOPDF_CHECK(Run(Bits("000000001111" "1"), params, out, info) == Ccitt::Result::Unsupported, 1084);

    // Garbage: every result is a status, the output whole rows.
    uint32_t seed = 7;
    for (int round = 0; round < 3000; round++) {
        std::string junk(1 + round % 61, '\0');
        for (char& c : junk) {
            seed = seed * 1664525u + 1013904223u;
            c = static_cast<char>(seed >> 24);
        }
        params.k = round % 3 - 1;
        params.columns = 1 + seed % 77;
        params.endOfLine = (round & 4) != 0;
        params.encodedByteAlign = (round & 8) != 0;
        params.damagedRowsBeforeError = (round & 16) != 0 ? 3 : 0;
        Run(junk, params, out, info, 500);
        RAYOPDF_CHECK(out.size() == size_t(info.rows) * ((params.columns + 7) / 8) && info.rows <= 500, 1085);
        RAYOPDF_CHECK(info.bytesUsed <= junk.size(), 1086);
    }
}

// Mixed /K > 0: a one-dimensional row may follow two-dimensional ones in any order the tag bits
// say; RTC is "EOL 1" six times.
void TestTags() {
    const Image image = MakeImage(77, 12);
    BitWriter out;
    const bool oneD[12] = {true, false, false, true, true, false, true, false, false, false, true, false};
    for (size_t y = 0; y < image.size(); y++) {
        out.Put(kEol);
        out.PutBit(oneD[y]);
        if (oneD[y]) {
            Encode1D(out, image[y]);
        } else {
            Encode2D(out, image[y - 1], image[y]);
        }
    }
    for (int i = 0; i < 6; i++) {
        out.Put(kEol);
        out.PutBit(true);
    }
    out.Put("10110110");  // after RTC: not read
    Ccitt::Params params;
    params.k = 3;
    params.endOfLine = true;
    params.columns = 77;
    std::string decoded;
    Ccitt::Info info;
    RAYOPDF_CHECK(Run(out.Bytes(), params, decoded, info) == Ccitt::Result::Ok && info.rows == 12 && info.endOfBlock,
                  1087);
    RAYOPDF_CHECK(decoded == Pack(image, false) && info.bytesUsed == out.Bytes().size() - 1, 1088);
}

// Images that follow each other (JBIG2 halftone planes: Group 4 rows and EOFB each): stopped by
// the row limit, the decoder reads the EOFB after the last row as well, so that bytesUsed is
// where the next image starts.
void TestConsecutive() {
    const Image first = MakeImage(37, 9);
    const Image second = MakeImage(37, 5);
    Mode mode;
    mode.k = -1;
    const std::string a = Encode(first, mode);
    const std::string data = a + Encode(second, mode);
    Ccitt::Params params;
    params.k = -1;
    params.columns = 37;
    params.blackIs1 = true;
    std::string decoded;
    Ccitt::Info info;
    RAYOPDF_CHECK(Run(data, params, decoded, info, 9) == Ccitt::Result::Ok && info.rows == 9 && info.endOfBlock &&
                      info.bytesUsed == a.size() && decoded == Pack(first, true),
                  1093);
    RAYOPDF_CHECK(Run(data.substr(info.bytesUsed), params, decoded, info, 5) == Ccitt::Result::Ok && info.rows == 5 &&
                      info.endOfBlock && decoded == Pack(second, true),
                  1094);
    // T.88 H.1 15(i): the first plane of the halftone example, 8 bytes with its EOFB (8 x 9 with
    // these rows: V0 and VL1 codes).
    const std::string plane("\xAA\xAA\xAA\xAA\x80\x08\x00\x80", 8);
    params.columns = 8;
    RAYOPDF_CHECK(Run(plane + "\xFF", params, decoded, info, 9) == Ccitt::Result::Ok && info.rows == 9 &&
                      info.endOfBlock && info.bytesUsed == 8,
                  1095);
}

// Required end-of-line patterns (/EndOfLine, or a pattern before the first row): the bits
// between a row's end and the next pattern are skipped, a row without a pattern is lost.
void TestEolSearch() {
    const Image image = MakeImage(30, 6);
    Ccitt::Params params;
    params.k = 0;
    params.columns = 30;
    std::string decoded;
    Ccitt::Info info;

    // A pattern before the first row; junk ("1011", "1") after rows, before the next pattern.
    BitWriter junk;
    for (size_t y = 0; y < image.size(); y++) {
        junk.Put(kEol);
        Encode1D(junk, image[y]);
        junk.Put(y % 2 ? "1" : "1011");
    }
    RAYOPDF_CHECK(Run(junk.Bytes(), params, decoded, info) == Ccitt::Result::Ok && info.rows == 6 &&
                      decoded == Pack(image, false),
                  1089);

    // Row 1 without its pattern: its bits are skipped up to row 2's pattern.
    BitWriter missing;
    for (size_t y = 0; y < image.size(); y++) {
        if (y != 1) missing.Put(kEol);
        Encode1D(missing, y == 1 ? Row(30, 0) : image[y]);  // white 30: 00000011, no pattern
    }
    Image expected = image;
    expected.erase(expected.begin() + 1);
    RAYOPDF_CHECK(Run(missing.Bytes(), params, decoded, info) == Ccitt::Result::Ok && info.rows == 5 &&
                      decoded == Pack(expected, false),
                  1090);

    // No pattern before the first row: patterns are optional unless /EndOfLine says otherwise.
    BitWriter optional;
    for (size_t y = 0; y < image.size(); y++) {
        if (y == 2 || y == 4) optional.Put(kEol);
        Encode1D(optional, image[y]);
    }
    RAYOPDF_CHECK(Run(optional.Bytes(), params, decoded, info) == Ccitt::Result::Ok && info.rows == 6 &&
                      decoded == Pack(image, false),
                  1091);
    params.endOfLine = true;
    expected = Image{image[0], image[2], image[4]};
    RAYOPDF_CHECK(Run(optional.Bytes(), params, decoded, info) == Ccitt::Result::Ok && info.rows == 3 &&
                      decoded == Pack(expected, false),
                  1092);
}

} // namespace

int main() {
    TestHandCoded();
    TestAllRuns();
    TestModes();
    TestStops();
    TestDamagedRows();
    TestLimits();
    TestTags();
    TestEolSearch();
    TestConsecutive();
    std::printf("test_image_ccitt: ok\n");
    return 0;
}
