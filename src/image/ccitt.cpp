// SPDX-License-Identifier: Apache-2.0
// CCITTFaxDecode (see ccitt.h). The code tables are ITU-T T.4 Tables 2 and 3 (terminating and
// make-up codes of white and black runs, the extended make-up codes shared by both colours) and
// T.4 Table 4 / T.6 Table 1 (the two-dimensional mode codes). Rows are kept as their changing
// elements (the columns where the colour changes, white first), which is what the
// two-dimensional modes refer to, and are expanded to packed bits only for the output.
#include "image/ccitt.h"

#include <cstring>
#include <utility>
#include <vector>

namespace RayoPdf::Image::Ccitt {
namespace {

// ---------------------------------------------------------------- code tables

struct Code {
    uint16_t bits;
    uint8_t length;
    uint16_t run;
};

// Marks the end-of-line pattern in the run tables (000000000001, a valid prefix in neither).
constexpr uint16_t kEolRun = 4095;

constexpr Code kWhiteCodes[] = {
    // terminating codes, runs 0-63
    {0b00110101, 8, 0}, {0b000111, 6, 1}, {0b0111, 4, 2}, {0b1000, 4, 3}, {0b1011, 4, 4},
    {0b1100, 4, 5}, {0b1110, 4, 6}, {0b1111, 4, 7}, {0b10011, 5, 8}, {0b10100, 5, 9},
    {0b00111, 5, 10}, {0b01000, 5, 11}, {0b001000, 6, 12}, {0b000011, 6, 13}, {0b110100, 6, 14},
    {0b110101, 6, 15}, {0b101010, 6, 16}, {0b101011, 6, 17}, {0b0100111, 7, 18},
    {0b0001100, 7, 19}, {0b0001000, 7, 20}, {0b0010111, 7, 21}, {0b0000011, 7, 22},
    {0b0000100, 7, 23}, {0b0101000, 7, 24}, {0b0101011, 7, 25}, {0b0010011, 7, 26},
    {0b0100100, 7, 27}, {0b0011000, 7, 28}, {0b00000010, 8, 29}, {0b00000011, 8, 30},
    {0b00011010, 8, 31}, {0b00011011, 8, 32}, {0b00010010, 8, 33}, {0b00010011, 8, 34},
    {0b00010100, 8, 35}, {0b00010101, 8, 36}, {0b00010110, 8, 37}, {0b00010111, 8, 38},
    {0b00101000, 8, 39}, {0b00101001, 8, 40}, {0b00101010, 8, 41}, {0b00101011, 8, 42},
    {0b00101100, 8, 43}, {0b00101101, 8, 44}, {0b00000100, 8, 45}, {0b00000101, 8, 46},
    {0b00001010, 8, 47}, {0b00001011, 8, 48}, {0b01010010, 8, 49}, {0b01010011, 8, 50},
    {0b01010100, 8, 51}, {0b01010101, 8, 52}, {0b00100100, 8, 53}, {0b00100101, 8, 54},
    {0b01011000, 8, 55}, {0b01011001, 8, 56}, {0b01011010, 8, 57}, {0b01011011, 8, 58},
    {0b01001010, 8, 59}, {0b01001011, 8, 60}, {0b00110010, 8, 61}, {0b00110011, 8, 62},
    {0b00110100, 8, 63},
    // make-up codes, runs 64-1728
    {0b11011, 5, 64}, {0b10010, 5, 128}, {0b010111, 6, 192}, {0b0110111, 7, 256},
    {0b00110110, 8, 320}, {0b00110111, 8, 384}, {0b01100100, 8, 448}, {0b01100101, 8, 512},
    {0b01101000, 8, 576}, {0b01100111, 8, 640}, {0b011001100, 9, 704}, {0b011001101, 9, 768},
    {0b011010010, 9, 832}, {0b011010011, 9, 896}, {0b011010100, 9, 960}, {0b011010101, 9, 1024},
    {0b011010110, 9, 1088}, {0b011010111, 9, 1152}, {0b011011000, 9, 1216},
    {0b011011001, 9, 1280}, {0b011011010, 9, 1344}, {0b011011011, 9, 1408},
    {0b010011000, 9, 1472}, {0b010011001, 9, 1536}, {0b010011010, 9, 1600}, {0b011000, 6, 1664},
    {0b010011011, 9, 1728},
    // extended make-up codes, runs 1792-2560
    {0b00000001000, 11, 1792}, {0b00000001100, 11, 1856}, {0b00000001101, 11, 1920},
    {0b000000010010, 12, 1984}, {0b000000010011, 12, 2048}, {0b000000010100, 12, 2112},
    {0b000000010101, 12, 2176}, {0b000000010110, 12, 2240}, {0b000000010111, 12, 2304},
    {0b000000011100, 12, 2368}, {0b000000011101, 12, 2432}, {0b000000011110, 12, 2496},
    {0b000000011111, 12, 2560},
    {0b000000000001, 12, kEolRun},
};

constexpr Code kBlackCodes[] = {
    // terminating codes, runs 0-63
    {0b0000110111, 10, 0}, {0b010, 3, 1}, {0b11, 2, 2}, {0b10, 2, 3}, {0b011, 3, 4},
    {0b0011, 4, 5}, {0b0010, 4, 6}, {0b00011, 5, 7}, {0b000101, 6, 8}, {0b000100, 6, 9},
    {0b0000100, 7, 10}, {0b0000101, 7, 11}, {0b0000111, 7, 12}, {0b00000100, 8, 13},
    {0b00000111, 8, 14}, {0b000011000, 9, 15}, {0b0000010111, 10, 16}, {0b0000011000, 10, 17},
    {0b0000001000, 10, 18}, {0b00001100111, 11, 19}, {0b00001101000, 11, 20},
    {0b00001101100, 11, 21}, {0b00000110111, 11, 22}, {0b00000101000, 11, 23},
    {0b00000010111, 11, 24}, {0b00000011000, 11, 25}, {0b000011001010, 12, 26},
    {0b000011001011, 12, 27}, {0b000011001100, 12, 28}, {0b000011001101, 12, 29},
    {0b000001101000, 12, 30}, {0b000001101001, 12, 31}, {0b000001101010, 12, 32},
    {0b000001101011, 12, 33}, {0b000011010010, 12, 34}, {0b000011010011, 12, 35},
    {0b000011010100, 12, 36}, {0b000011010101, 12, 37}, {0b000011010110, 12, 38},
    {0b000011010111, 12, 39}, {0b000001101100, 12, 40}, {0b000001101101, 12, 41},
    {0b000011011010, 12, 42}, {0b000011011011, 12, 43}, {0b000001010100, 12, 44},
    {0b000001010101, 12, 45}, {0b000001010110, 12, 46}, {0b000001010111, 12, 47},
    {0b000001100100, 12, 48}, {0b000001100101, 12, 49}, {0b000001010010, 12, 50},
    {0b000001010011, 12, 51}, {0b000000100100, 12, 52}, {0b000000110111, 12, 53},
    {0b000000111000, 12, 54}, {0b000000100111, 12, 55}, {0b000000101000, 12, 56},
    {0b000001011000, 12, 57}, {0b000001011001, 12, 58}, {0b000000101011, 12, 59},
    {0b000000101100, 12, 60}, {0b000001011010, 12, 61}, {0b000001100110, 12, 62},
    {0b000001100111, 12, 63},
    // make-up codes, runs 64-1728
    {0b0000001111, 10, 64}, {0b000011001000, 12, 128}, {0b000011001001, 12, 192},
    {0b000001011011, 12, 256}, {0b000000110011, 12, 320}, {0b000000110100, 12, 384},
    {0b000000110101, 12, 448}, {0b0000001101100, 13, 512}, {0b0000001101101, 13, 576},
    {0b0000001001010, 13, 640}, {0b0000001001011, 13, 704}, {0b0000001001100, 13, 768},
    {0b0000001001101, 13, 832}, {0b0000001110010, 13, 896}, {0b0000001110011, 13, 960},
    {0b0000001110100, 13, 1024}, {0b0000001110101, 13, 1088}, {0b0000001110110, 13, 1152},
    {0b0000001110111, 13, 1216}, {0b0000001010010, 13, 1280}, {0b0000001010011, 13, 1344},
    {0b0000001010100, 13, 1408}, {0b0000001010101, 13, 1472}, {0b0000001011010, 13, 1536},
    {0b0000001011011, 13, 1600}, {0b0000001100100, 13, 1664}, {0b0000001100101, 13, 1728},
    // extended make-up codes, runs 1792-2560
    {0b00000001000, 11, 1792}, {0b00000001100, 11, 1856}, {0b00000001101, 11, 1920},
    {0b000000010010, 12, 1984}, {0b000000010011, 12, 2048}, {0b000000010100, 12, 2112},
    {0b000000010101, 12, 2176}, {0b000000010110, 12, 2240}, {0b000000010111, 12, 2304},
    {0b000000011100, 12, 2368}, {0b000000011101, 12, 2432}, {0b000000011110, 12, 2496},
    {0b000000011111, 12, 2560},
    {0b000000000001, 12, kEolRun},
};

// A lookup table indexed by the next `Bits` input bits: (run << 4) | code length, 0 where no code
// starts. `conflicts` counts table slots two codes claim (codes that are prefixes of others).
template <int Bits>
struct RunTable {
    uint16_t entry[size_t(1) << Bits] = {};
    int conflicts = 0;
};

template <int Bits, size_t Count>
constexpr RunTable<Bits> BuildRunTable(const Code (&codes)[Count]) {
    RunTable<Bits> table;
    for (size_t c = 0; c < Count; c++) {
        const int spare = Bits - codes[c].length;
        const uint32_t first = uint32_t(codes[c].bits) << spare;
        for (uint32_t i = 0; i < (uint32_t(1) << spare); i++) {
            if (table.entry[first + i] != 0) table.conflicts++;
            table.entry[first + i] = static_cast<uint16_t>(codes[c].run << 4 | codes[c].length);
        }
    }
    return table;
}

constexpr int kWhiteBits = 12;
constexpr int kBlackBits = 13;
constexpr RunTable<kWhiteBits> kWhite = BuildRunTable<kWhiteBits>(kWhiteCodes);
constexpr RunTable<kBlackBits> kBlack = BuildRunTable<kBlackBits>(kBlackCodes);
static_assert(kWhite.conflicts == 0, "the white codes are a prefix code");
static_assert(kBlack.conflicts == 0, "the black codes are a prefix code");

// Two-dimensional mode codes, looked up by the next 7 bits.
enum class Mode : uint8_t { Vertical, Pass, Horizontal, Extension, Invalid };

struct ModeEntry {
    Mode mode = Mode::Invalid;
    int8_t delta = 0;  // a1 - b1 of the vertical modes
    uint8_t length = 0;
};

struct ModeTable {
    ModeEntry entry[128] = {};
};

constexpr ModeTable BuildModeTable() {
    struct ModeCode {
        uint8_t bits;
        uint8_t length;
        Mode mode;
        int8_t delta;
    };
    constexpr ModeCode kCodes[] = {
        {0b1, 1, Mode::Vertical, 0},        {0b011, 3, Mode::Vertical, 1},
        {0b000011, 6, Mode::Vertical, 2},   {0b0000011, 7, Mode::Vertical, 3},
        {0b010, 3, Mode::Vertical, -1},     {0b000010, 6, Mode::Vertical, -2},
        {0b0000010, 7, Mode::Vertical, -3}, {0b0001, 4, Mode::Pass, 0},
        {0b001, 3, Mode::Horizontal, 0},    {0b0000001, 7, Mode::Extension, 0},
    };
    ModeTable table;
    for (const ModeCode& code : kCodes) {
        const int spare = 7 - code.length;
        for (int i = 0; i < (1 << spare); i++) {
            ModeEntry& entry = table.entry[(code.bits << spare) + i];
            entry.mode = code.mode;
            entry.delta = code.delta;
            entry.length = code.length;
        }
    }
    return table;
}

constexpr ModeTable kModes = BuildModeTable();

// ---------------------------------------------------------------- bits

// Reads the data from its most significant bit; past the end it reads 0 bits, which no code is
// made of alone, so decoding stops there and Position() tells how far it got.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // The next `count` bits (1-25), the first in the highest place.
    uint32_t Peek(int count) {
        if (count_ < count) Refill();
        return static_cast<uint32_t>(bits_ >> (64 - count));
    }

    // Skips `count` bits of the last Peek.
    void Skip(int count) {
        bits_ <<= count;
        count_ -= count;
    }

    void AlignToByte() { Skip(count_ & 7); }

    // Bits consumed so far.
    uint64_t Position() const { return uint64_t(loaded_) * 8 - static_cast<uint64_t>(count_); }

private:
    void Refill() {
        if (loaded_ + 8 <= size_) {
            uint64_t word = 0;
            for (int i = 0; i < 8; i++) word = word << 8 | data_[loaded_ + static_cast<size_t>(i)];
            // The bytes past the whole ones taken are ORed in again, unchanged, by the next refill.
            bits_ |= word >> count_;
            const int bytes = (63 - count_) >> 3;
            loaded_ += static_cast<size_t>(bytes);
            count_ += bytes * 8;
            return;
        }
        while (count_ <= 56) {
            const uint64_t byte = loaded_ < size_ ? data_[loaded_] : 0;
            bits_ |= byte << (56 - count_);
            loaded_++;
            count_ += 8;
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t loaded_ = 0;  // bytes moved into bits_ (counting the 0 bytes past the end)
    uint64_t bits_ = 0;  // left-aligned
    int count_ = 0;      // valid bits in bits_
};

// Fill bits and an end-of-line pattern (eleven or more 0 bits, then a 1), read when the reader is
// at one. Zero bits that run into the end of the data are consumed, and false is returned.
bool ReadEol(BitReader& in, uint64_t end) {
    if (in.Peek(12) > 1 || in.Position() >= end) return false;
    for (;;) {
        const uint32_t bits = in.Peek(24);
        if (bits != 0) {
            in.Skip(__builtin_clz(bits) - 8 + 1);
            return true;
        }
        in.Skip(24);
        if (in.Position() >= end) return false;
    }
}

// After the second end-of-line pattern of RTC: the other four (for /K > 0 each pattern is
// followed by a tag bit 1), as far as they are there, so that bytesUsed covers them.
void ReadRtcRest(BitReader& in, uint64_t end, bool tags) {
    for (int i = 2; i < 6; i++) {
        if (tags) {
            const uint32_t next = in.Peek(13);
            if ((next >> 12) != 1 || (next & 0xFFF) > 1) return;
            in.Skip(1);
        }
        if (!ReadEol(in, end)) return;
    }
    if (tags && in.Peek(1) == 1 && in.Position() < end) in.Skip(1);
}

// When the row limit stops decoding: an EOFB (or RTC) that follows the last row directly, without
// fill bits, is read too, so that bytesUsed covers it (JBIG2's MMR planes follow each other).
void ReadEndOfBlockAfterLimit(BitReader& in, uint64_t end, int32_t k, Info& info) {
    if (k > 0) {
        if (in.Peek(25) != (0x001u << 13 | 1u << 12 | 0x001u)) return;  // EOL 1 EOL
        in.Skip(25);
        info.endOfBlock = true;
        ReadRtcRest(in, end, true);
        return;
    }
    if (in.Peek(24) != 0x001001u) return;  // EOL EOL
    in.Skip(24);
    info.endOfBlock = true;
    if (k == 0) ReadRtcRest(in, end, false);
}

// Moves past the next end-of-line pattern (a damaged row's end); false when the data ends first.
bool SkipToEol(BitReader& in, uint64_t end) {
    int zeros = 0;
    while (in.Position() < end) {
        const uint32_t bit = in.Peek(1);
        in.Skip(1);
        if (bit == 0) {
            zeros++;
        } else if (zeros >= 11) {
            return true;
        } else {
            zeros = 0;
        }
    }
    return false;
}

// ---------------------------------------------------------------- rows

// One row as its changing elements: v[0..n) strictly increasing columns below `columns`, where
// v[0] starts the first black run, v[1] the next white run, and so on. After a row is complete
// v[n..n+4) hold `columns`, which ends every search of the next row's decoding.
struct Changes {
    std::vector<uint32_t> v;
    size_t n = 0;
};

constexpr size_t kSentinels = 4;

// Adds a change at x (< columns, and not before the last change); a change at the last one's
// column cancels it (a run of length 0).
inline void Push(Changes& changes, uint32_t x) {
    if (changes.n > 0 && changes.v[changes.n - 1] == x) {
        changes.n--;
        return;
    }
    if (changes.n == changes.v.size()) changes.v.resize(changes.v.size() * 2);
    changes.v[changes.n++] = x;
}

void Terminate(Changes& changes, uint32_t columns) {
    if (changes.v.size() < changes.n + kSentinels) changes.v.resize(changes.n + kSentinels);
    for (size_t i = 0; i < kSentinels; i++) changes.v[changes.n + i] = columns;
}

enum class RowStatus : uint8_t { Ok, Bad, Uncompressed };

constexpr int32_t kBadRun = -1;
constexpr int32_t kMaxRun = int32_t(1) << 30;

// A run: make-up codes and a terminating code. kBadRun for a code that is not one (an end-of-line
// pattern included).
int32_t ReadRun(BitReader& in, bool black) {
    int32_t total = 0;
    for (;;) {
        const uint32_t entry = black ? kBlack.entry[in.Peek(kBlackBits)] : kWhite.entry[in.Peek(kWhiteBits)];
        const int length = static_cast<int>(entry & 15);
        const int32_t run = static_cast<int32_t>(entry >> 4);
        if (length == 0 || run == kEolRun) return kBadRun;
        in.Skip(length);
        total += run;
        if (run < 64) return total;
        if (total > kMaxRun) return kBadRun;
    }
}

// The uncompressed-mode entry code of one-dimensional rows (T.4 4.2.2): 000000001111.
bool AtUncompressed1D(BitReader& in) { return in.Peek(12) == 0b000000001111; }

RowStatus Decode1D(BitReader& in, uint32_t columns, Changes& row) {
    row.n = 0;
    uint32_t a0 = 0;
    bool black = false;
    while (a0 < columns) {
        const int32_t run = ReadRun(in, black);
        if (run < 0) return !black && AtUncompressed1D(in) ? RowStatus::Uncompressed : RowStatus::Bad;
        const uint64_t a1 = uint64_t(a0) + static_cast<uint64_t>(run);
        a0 = a1 < columns ? static_cast<uint32_t>(a1) : columns;
        if (a0 < columns) Push(row, a0);
        black = !black;
    }
    return RowStatus::Ok;
}

RowStatus Decode2D(BitReader& in, uint32_t columns, const Changes& reference, Changes& row) {
    row.n = 0;
    const uint32_t* ref = reference.v.data();
    int64_t a0 = -1;  // the imaginary white element before the row
    size_t i = 0;     // index of b1 in ref
    while (a0 < columns) {
        // b1: the first change of the reference row right of a0 to the colour opposite a0's; the
        // change at index i starts a black run when i is even, so its parity is the row's.
        if ((i & 1) != (row.n & 1)) i++;
        while (int64_t(ref[i]) <= a0) i += 2;
        const uint32_t b1 = ref[i];
        const ModeEntry mode = kModes.entry[in.Peek(7)];
        switch (mode.mode) {
        case Mode::Vertical: {
            in.Skip(mode.length);
            int64_t a1 = int64_t(b1) + mode.delta;
            if (a1 < 0 || a1 < a0) return RowStatus::Bad;
            if (a1 > columns) a1 = columns;
            if (a1 < columns) Push(row, static_cast<uint32_t>(a1));
            a0 = a1;
            if (mode.delta < 0 && i > 0) {
                i--;
            } else {
                i++;
            }
            break;
        }
        case Mode::Pass:
            in.Skip(mode.length);
            a0 = ref[i + 1];
            i += 2;
            break;
        case Mode::Horizontal: {
            in.Skip(mode.length);
            const bool black = (row.n & 1) != 0;
            const int32_t run1 = ReadRun(in, black);
            if (run1 < 0) return RowStatus::Bad;
            const int32_t run2 = ReadRun(in, !black);
            if (run2 < 0) return RowStatus::Bad;
            const int64_t start = a0 < 0 ? 0 : a0;
            int64_t a1 = start + run1;
            if (a1 > columns) a1 = columns;
            int64_t a2 = a1 + run2;
            if (a2 > columns) a2 = columns;
            if (a1 < columns) Push(row, static_cast<uint32_t>(a1));
            if (a2 < columns) Push(row, static_cast<uint32_t>(a2));
            a0 = a2;
            break;
        }
        case Mode::Extension:
            return (in.Peek(10) & 7) == 7 ? RowStatus::Uncompressed : RowStatus::Bad;
        case Mode::Invalid:
            return RowStatus::Bad;
        }
    }
    return RowStatus::Ok;
}

// Writes the row's pixels: white everywhere, then each black run.
void WriteRow(const Changes& row, uint32_t columns, bool blackIs1, uint8_t* out, size_t stride) {
    const uint8_t white = blackIs1 ? 0x00 : 0xFF;
    std::memset(out, white, stride);
    for (size_t k = 0; k < row.n; k += 2) {
        const uint32_t start = row.v[k];
        const uint32_t end = k + 1 < row.n ? row.v[k + 1] : columns;
        const size_t first = start >> 3;
        const size_t last = (end - 1) >> 3;
        const uint8_t head = static_cast<uint8_t>(0xFF >> (start & 7));
        const uint8_t tail = static_cast<uint8_t>(0xFF << (7 - ((end - 1) & 7)));
        if (first == last) {
            out[first] ^= static_cast<uint8_t>(head & tail);
            continue;
        }
        out[first] ^= head;
        if (last > first + 1) std::memset(out + first + 1, white ^ 0xFF, last - first - 1);
        out[last] ^= tail;
    }
    // The bits after the last column are a black pixel's, as in poppler's output.
    if ((columns & 7) != 0) {
        const uint8_t pad = static_cast<uint8_t>(0xFF >> (columns & 7));
        out[stride - 1] = static_cast<uint8_t>(blackIs1 ? out[stride - 1] | pad : out[stride - 1] & ~pad);
    }
}

} // namespace

Result Decode(const uint8_t* data, size_t size, const Params& params, uint32_t maxRows, uint64_t maxPixels,
              std::string& rows, Info& info) {
    rows.clear();
    info = Info();
    const uint32_t columns = params.columns;
    if (columns == 0) return Result::InvalidParams;
    if (columns > uint32_t(kMaxRun) || columns > maxPixels) return Result::TooLarge;
    const size_t stride = (size_t(columns) + 7) / 8;
    const uint64_t end = uint64_t(size) * 8;

    // Each row takes at least one input bit, so the data bounds the rows as well.
    uint64_t expected = maxRows;
    if (!params.endOfBlock && params.rows > 0 && params.rows < expected) expected = params.rows;
    if (end + 1 < expected) expected = end + 1;
    if (maxPixels / columns < expected) expected = maxPixels / columns;
    rows.reserve(static_cast<size_t>(expected) * stride);

    const size_t initial = (columns < 1024 ? columns : 1024) + kSentinels;
    Changes reference;
    Changes current;
    reference.v.resize(initial);
    current.v.resize(initial);
    Terminate(reference, columns);  // the white row above the first

    const bool tolerant = params.endOfLine && params.k >= 0;
    // End-of-line patterns are required with /EndOfLine, and (as poppler reads the data) when the
    // data starts with one: then every later row is found by searching for its pattern.
    bool requireEol = params.endOfLine;
    BitReader in(data, size);
    bool first = true;
    bool afterEol = false;     // the last row started after an end-of-line pattern
    bool eolPending = false;   // a damaged row's search consumed the next row's pattern
    bool lastDamaged = false;
    Result result = Result::Ok;
    for (;;) {
        if (info.rows >= maxRows) {
            if (params.endOfBlock) ReadEndOfBlockAfterLimit(in, end, params.k, info);
            break;
        }
        if (!params.endOfBlock && params.rows > 0 && info.rows >= params.rows) break;

        bool eol = eolPending;
        eolPending = false;
        if (!eol) {
            if (!first && params.encodedByteAlign && !afterEol) in.AlignToByte();
            if (!first && requireEol) {
                // Whatever stands between a row's end and the next pattern is no row.
                if (!ReadEol(in, end) && !SkipToEol(in, end)) break;
                eol = true;
            } else {
                eol = ReadEol(in, end);
                if (first && eol) requireEol = true;
            }
        }
        int tag = -1;  // the tag bit of /K > 0 rows, read before the check for RTC ("EOL 1" x 6)
        if (eol) {
            if (params.k > 0) {
                tag = static_cast<int>(in.Peek(1));
                in.Skip(1);
            }
            if (ReadEol(in, end)) {
                info.endOfBlock = true;
                if (params.k >= 0) ReadRtcRest(in, end, params.k > 0);
                break;
            }
        }
        if (in.Position() >= end) break;  // the data ends at a row boundary
        if ((uint64_t(info.rows) + 1) * columns > maxPixels) {
            result = Result::TooLarge;
            break;
        }
        first = false;
        afterEol = eol;

        bool twoDimensional = params.k < 0;
        if (params.k > 0) {
            if (tag < 0) {
                tag = static_cast<int>(in.Peek(1));
                in.Skip(1);
            }
            twoDimensional = tag == 0;
        }
        RowStatus status = twoDimensional ? Decode2D(in, columns, reference, current) : Decode1D(in, columns, current);
        if (status == RowStatus::Uncompressed) {
            result = Result::Unsupported;
            break;
        }
        if (status == RowStatus::Ok && in.Position() > end) status = RowStatus::Bad;  // cut off
        if (status == RowStatus::Bad) {
            if (!tolerant || info.damagedRows >= params.damagedRowsBeforeError || !SkipToEol(in, end)) {
                result = Result::Corrupt;
                break;
            }
            eolPending = true;
            info.damagedRows++;
            if (lastDamaged) {
                current.n = 0;
            } else {
                if (current.v.size() < reference.v.size()) current.v.resize(reference.v.size());
                std::memcpy(current.v.data(), reference.v.data(), reference.n * sizeof(uint32_t));
                current.n = reference.n;
            }
            lastDamaged = true;
        } else {
            lastDamaged = false;
        }

        const size_t offset = rows.size();
        rows.resize(offset + stride);
        WriteRow(current, columns, params.blackIs1, reinterpret_cast<uint8_t*>(&rows[offset]), stride);
        info.rows++;
        Terminate(current, columns);
        std::swap(reference, current);
    }
    const uint64_t used = (in.Position() + 7) / 8;
    info.bytesUsed = used < size ? static_cast<size_t>(used) : size;
    return result;
}

} // namespace RayoPdf::Image::Ccitt
