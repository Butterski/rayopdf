// SPDX-License-Identifier: Apache-2.0
// Forked from RayoMD src/core/inflate.cpp at ef106ca (Apache-2.0, same author).
// The decode tables and the decoding loops are RayoMD's; the output goes to one buffer that grows
// (kept per thread between calls and copied out once, so it is never zero-filled), or for a
// Stream to a window of 32 KB and a piece, and every way the stream can end is a Result (inflate.h).
#include "pdf/inflate.h"

#include "common/compiler.h"

#include <algorithm>
#include <cstring>
#include <memory>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOPDF_INFLATE_SSE2 1
#endif

namespace RayoPdf::Inflate {
namespace {

// Past the target the fast loop may write two literals and a match of 258 bytes, whose copy goes
// up to 31 bytes further.
constexpr size_t kSlack = 2 + 258 + 32 + 8;

// A decode table entry: the value in the high half (a literal, a base length or distance, or a
// subtable's offset), the code's kind, a field, and in the low byte only the bits to take, so
// that a shift by the entry needs no mask on x86. A length or distance takes its code and extra
// bits at once, the field giving the code's bits, under which the extra ones start; a subtable's
// field gives its bits.
enum : uint32_t {
    kLiteral = 0,  // also a distance or a code length
    kLength = 1u << 13,
    kEnd = 2u << 13,
    kSubtable = 3u << 13,
    kInvalid = 4u << 13,
};
constexpr uint32_t kKindMask = 7u << 13;

constexpr uint32_t Entry(uint32_t value, uint32_t field, uint32_t kind, uint32_t bits) {
    return value << 16 | kind | field << 8 | bits;
}
inline unsigned TakenBits(uint32_t entry) { return entry & 63; }
inline unsigned Field(uint32_t entry) { return (entry >> 8) & 31; }
inline uint32_t Value(uint32_t entry) { return entry >> 16; }
// The base plus the extra bits of a length or distance `entry` read from `bits`.
inline size_t ValuePlusExtra(uint32_t entry, uint64_t bits) {
    return Value(entry) + static_cast<size_t>((bits & ((uint64_t(1) << TakenBits(entry)) - 1)) >> Field(entry));
}

// Codes are looked up by their first kRootBits bits; a longer code's prefix leads to a subtable of
// its remaining bits, at most 15 in all. The tables hold the root and at most this many subtables:
// a code is complete wherever it has codes longer than the root (BuildTable refuses an incomplete
// one but for a single code of one bit), so every prefix with a subtable leads to at least two
// codes: at most 286 / 2 literal/length and 30 / 2 distance subtables. Smaller tables than for
// every symbol a subtable: 26 KB instead of 43 KB of stack touched by a fresh process's first
// dynamic block.
constexpr unsigned kLitLenRoot = 11;
constexpr unsigned kDistRoot = 8;
constexpr unsigned kPrecodeRoot = 7;
constexpr size_t kLitLenTableSize = (size_t(1) << kLitLenRoot) + 143 * (size_t(1) << (15 - kLitLenRoot));
constexpr size_t kDistTableSize = (size_t(1) << kDistRoot) + 15 * (size_t(1) << (15 - kDistRoot));
// The fixed codes are at most 9 and 5 bits long: no subtables.
constexpr size_t kFixedLitLenTableSize = size_t(1) << kLitLenRoot;
constexpr size_t kFixedDistTableSize = size_t(1) << kDistRoot;

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                        33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kPrecodeOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct LitLenEntry {
    uint32_t operator()(unsigned symbol, unsigned bits) const {
        if (symbol < 256) return Entry(symbol, 0, kLiteral, bits);
        if (symbol == 256) return Entry(0, 0, kEnd, bits);
        if (symbol < 286) return Entry(kLengthBase[symbol - 257], bits, kLength, bits + kLengthExtra[symbol - 257]);
        return Entry(0, 0, kInvalid, bits);  // 286 and 287 have fixed codes but no meaning
    }
};

struct DistanceEntry {
    uint32_t operator()(unsigned symbol, unsigned bits) const {
        if (symbol < 30) return Entry(kDistanceBase[symbol], bits, kLiteral, bits + kDistanceExtra[symbol]);
        return Entry(0, 0, kInvalid, bits);
    }
};

struct PrecodeEntry {
    uint32_t operator()(unsigned symbol, unsigned bits) const { return Entry(symbol, 0, kLiteral, bits); }
};

struct ByteReversal {
    uint8_t of[256] = {};
    constexpr ByteReversal() {
        for (unsigned byte = 0; byte < 256; byte++) {
            for (unsigned bit = 0; bit < 8; bit++) of[byte] |= static_cast<uint8_t>(((byte >> bit) & 1) << (7 - bit));
        }
    }
};
constexpr ByteReversal kReversed;

// The low `bits` bits of `code` in reverse order, `bits` at most 16.
inline unsigned Reverse(unsigned code, unsigned bits) {
    return (static_cast<unsigned>(kReversed.of[code & 255]) << 8 | kReversed.of[(code >> 8) & 255]) >> (16 - bits);
}

inline unsigned HighestBit(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return 31u - static_cast<unsigned>(__builtin_clz(value));
#else
    unsigned bit = 0;
    while (value >>= 1) bit++;
    return bit;
#endif
}

// Copies the first `size` entries of `table` (a power of two) right after themselves.
inline void DoubleTable(uint32_t* table, size_t size) {
    uint32_t* const to = table + size;
#if RAYOPDF_INFLATE_SSE2
    const auto copy = [&](size_t index) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + index),
                         _mm_loadu_si128(reinterpret_cast<const __m128i*>(table + index)));
    };
    if (size >= 16) {
        for (size_t index = 0; index < size; index += 16) {
            copy(index);
            copy(index + 4);
            copy(index + 8);
            copy(index + 12);
        }
        return;
    }
    if (size >= 4) {
        for (size_t index = 0; index < size; index += 4) copy(index);
        return;
    }
#endif
    for (size_t index = 0; index < size; index++) to[index] = table[index];
}

// The decode table of the canonical code with `lengths` (0 for an unused symbol, at most 15),
// as zlib builds it: false for an over-subscribed code, and for an incomplete one unless it has a
// single code of one bit or, where `allowEmpty`, no code at all. `incompleteAllowed` false refuses
// every incomplete code (the code length code). False also when the subtables would pass
// `tableSize` entries (which the bound above rules out).
//
// A complete code's root is filled in canonical order (by length, then symbol): the codes of one
// length go into the first 2^length entries, which are then doubled for the next length, so every
// entry is written once and the copies go 16 bytes at a time. The codes longer than the root then
// fill their subtables in symbol order, which places the subtables where a symbol-order build
// would.
template <typename EntryOf>
bool BuildTable(const uint8_t* lengths, unsigned count, unsigned root, uint32_t* table, size_t tableSize,
                bool incompleteAllowed, bool allowEmpty, EntryOf entryOf) {
    unsigned counts[16] = {};
    for (unsigned symbol = 0; symbol < count; symbol++) counts[lengths[symbol]]++;
    const unsigned unused = counts[0];
    counts[0] = 0;
    int left = 1;
    unsigned codes = 0;
    for (unsigned bits = 1; bits <= 15; bits++) {
        left <<= 1;
        left -= static_cast<int>(counts[bits]);
        if (left < 0) return false;
        codes += counts[bits];
    }
    const size_t rootSize = size_t(1) << root;
    if (left > 0) {
        // A single code of one bit or none: its root entries and invalid ones between.
        const bool single = codes == 1 && counts[1] == 1;
        if (!incompleteAllowed || !(single || (codes == 0 && allowEmpty))) return false;
        for (size_t index = 0; index < rootSize; index++) table[index] = Entry(0, 0, kInvalid, 1);
        for (unsigned symbol = 0; symbol < count; symbol++) {
            if (lengths[symbol] == 0) continue;
            for (size_t index = 0; index < rootSize; index += 2) table[index] = entryOf(symbol, 1);
        }
        return true;
    }
    // The symbols sorted by length, then symbol: those of the codes up to the root first, then the
    // unused and the longer ones (not read).
    unsigned offsets[16];
    offsets[1] = 0;
    for (unsigned bits = 1; bits < root; bits++) offsets[bits + 1] = offsets[bits] + counts[bits];
    offsets[0] = offsets[root] + counts[root];
    for (unsigned bits = root + 1, at = offsets[0] + unused; bits <= 15; bits++) {
        offsets[bits] = at;
        at += counts[bits];
    }
    uint16_t sorted[288];
    for (unsigned symbol = 0; symbol < count; symbol++) sorted[offsets[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
    // The root, by length: `code` is the next code of `length` bits, bit-reversed (its index), and
    // the first `filled` entries hold the codes so far, repeated.
    const uint16_t* nextSymbol = sorted;
    unsigned length = 1;
    while (counts[length] == 0) length++;
    size_t filled = size_t(1) << length;
    unsigned code = 0;
    for (; length <= root; length++) {
        if ((size_t(1) << length) > filled) {
            DoubleTable(table, filled);
            filled <<= 1;
        }
        for (unsigned remaining = counts[length]; remaining > 0; remaining--) {
            table[code] = entryOf(*nextSymbol++, length);
            if (code == filled - 1) {
                // The last code (all ones): the code ends here.
                for (; filled < rootSize; filled <<= 1) DoubleTable(table, filled);
                return true;
            }
            // The next code: a bit-reversed increment.
            const unsigned bit = 1u << HighestBit(code ^ static_cast<unsigned>(filled - 1));
            code = (code & (bit - 1)) | bit;
        }
    }
    // Codes longer than the root (only they are left): each root prefix of them leads to a subtable.
    unsigned next[16] = {};
    for (unsigned bits = 1, first = 0; bits <= 15; bits++) {
        first = (first + counts[bits - 1]) << 1;
        next[bits] = first;
    }
    const unsigned subBits = 15 - root;
    size_t subNext = rootSize;
    uint16_t subOf[1 << 11];  // the subtable of each root prefix, 0 for none
    std::memset(subOf, 0, sizeof(uint16_t) << root);
    for (unsigned symbol = 0; symbol < count; symbol++) {
        const unsigned bits = lengths[symbol];
        if (bits <= root) continue;
        const unsigned reversed = Reverse(next[bits]++, bits);
        const uint32_t entry = entryOf(symbol, bits);
        const unsigned prefix = reversed & static_cast<unsigned>(rootSize - 1);
        if (subOf[prefix] == 0) {
            if (subNext + (size_t(1) << subBits) > tableSize) return false;
            subOf[prefix] = static_cast<uint16_t>(subNext);
            table[prefix] = Entry(static_cast<uint32_t>(subNext), subBits, kSubtable, 0);  // takes no bits itself
            subNext += size_t(1) << subBits;
        }
        uint32_t* const sub = table + subOf[prefix];
        for (size_t index = reversed >> root; index < (size_t(1) << subBits); index += size_t(1) << (bits - root)) {
            sub[index] = entry;
        }
    }
    return true;
}

inline uint64_t Load64(const uint8_t* at) {
    uint64_t value;
    std::memcpy(&value, at, 8);
    return value;  // little-endian targets only
}

inline void Store64(uint8_t* at, uint64_t value) { std::memcpy(at, &value, 8); }

// The entry for the code at the bottom of `bits`, through its subtable when it is longer than `root`.
RAYOPDF_HOT_INLINE uint32_t Lookup(const uint32_t* table, uint64_t bits, unsigned root) {
    uint32_t entry = table[bits & ((uint64_t(1) << root) - 1)];
    if ((entry & kKindMask) == kSubtable) {
        entry = table[Value(entry) + ((bits >> root) & ((uint64_t(1) << Field(entry)) - 1))];
    }
    return entry;
}

// Copies a match of `length` bytes from `back` bytes before `to`, up to 31 bytes past its end.
RAYOPDF_HOT_INLINE void CopyMatch(uint8_t* to, size_t back, size_t length) {
    const uint8_t* from = to - back;
    uint8_t* const stop = to + length;
#if RAYOPDF_INFLATE_SSE2
    if (back >= 16) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to), _mm_loadu_si128(reinterpret_cast<const __m128i*>(from)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 16),
                         _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + 16)));
        for (to += 32, from += 32; to < stop; to += 16, from += 16) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(to), _mm_loadu_si128(reinterpret_cast<const __m128i*>(from)));
        }
        return;
    }
    if (back == 1) {
        const __m128i repeated = _mm_set1_epi8(static_cast<char>(from[0]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to), repeated);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(to + 16), repeated);
        for (to += 32; to < stop; to += 16) _mm_storeu_si128(reinterpret_cast<__m128i*>(to), repeated);
        return;
    }
#endif
    if (back >= 8) {
        Store64(to, Load64(from));
        Store64(to + 8, Load64(from + 8));
        for (to += 16, from += 16; to < stop; to += 8, from += 8) Store64(to, Load64(from));
        return;
    }
    if (back == 1) {
        const uint64_t repeated = 0x0101010101010101ull * from[0];
        Store64(to, repeated);
        Store64(to + 8, repeated);
        for (to += 16; to < stop; to += 8) Store64(to, repeated);
        return;
    }
    // Words a period apart, each right in its first `back` bytes, until the whole periods that fit
    // in a word are there; then words that far apart. Each load reads what the store before wrote.
    const size_t step = back * (8 / back);
    uint8_t* at = to;
    for (uint8_t* const periods = to + step; at < periods; at += back) Store64(at, Load64(at - back));
    for (; at < stop; at += step) Store64(at, Load64(at - step));
}

struct FixedTables {
    uint32_t litlen[kFixedLitLenTableSize];
    uint32_t distance[kFixedDistTableSize];
    FixedTables() {
        uint8_t lengths[288];
        std::memset(lengths, 8, 144);
        std::memset(lengths + 144, 9, 112);
        std::memset(lengths + 256, 7, 24);
        std::memset(lengths + 280, 8, 8);
        BuildTable(lengths, 288, kLitLenRoot, litlen, kFixedLitLenTableSize, false, false, LitLenEntry());
        std::memset(lengths, 5, 32);
        BuildTable(lengths, 32, kDistRoot, distance, kFixedDistTableSize, false, false, DistanceEntry());
    }
};

const FixedTables& Fixed() {
    static const FixedTables tables;
    return tables;
}

// How a step of decoding ended.
enum class Step : uint8_t {
    Ok,         // the block (or its header, or a stored piece) is done
    Full,       // the output reached the buffer's end
    Truncated,  // the input ends inside it
    Corrupt,    // invalid data
};

bool ZlibHeader(const uint8_t* p) {
    return (p[0] & 15) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8) | p[1]) % 31 == 0;
}

// The decoders' output buffer: grown without zero-filling (every byte kept is written first).
struct Buffer {
    std::unique_ptr<uint8_t[]> bytes;
    size_t size = 0;

    // At least `want` bytes, the first `keep` as they were.
    uint8_t* Reserve(size_t want, size_t keep) {
        if (want > size) {
            std::unique_ptr<uint8_t[]> grown(new uint8_t[want]);
            if (keep > 0) std::memcpy(grown.get(), bytes.get(), keep);
            bytes.swap(grown);
            size = want;
        }
        return bytes.get();
    }
};

class Inflater {
public:
    Inflater(const uint8_t* data, size_t size, Buffer& out, size_t maxOut)
        : begin_(data), in_(data), end_(data + size), out_(out), maxOut_(maxOut) {}

    // Inflates the whole input into the buffer: Length() bytes of it.
    Result Run() {
        SkipHeader();
        // A first guess at the size: content streams inflate 3 to 6 times; the buffer doubles. A
        // buffer kept from an earlier stream is used whole.
        const size_t kept = out_.size > kSlack ? out_.size - kSlack : 0;
        const size_t guess = std::max<size_t>(size_t(4096), static_cast<size_t>(end_ - begin_) * 4);
        if (!Resize(std::min(Cap(), std::max(guess, kept)))) return Result::Corrupt;
        Step step = Step::Ok;
        bool done = false;
        while (!done) {
            if (final_) {
                done = true;
                break;
            }
            step = ReadBlockHeader();
            if (step != Step::Ok) break;
            if (stored_) {
                step = CopyStored();
            } else {
                for (;;) {
                    step = DecodeCodes();
                    if (step != Step::Full) break;
                    if (o_ > maxOut_ || !Grow()) break;
                }
            }
            if (step != Step::Ok) break;
        }
        if (o_ > maxOut_) return Result::TooLarge;
        if (done) return Result::Ok;
        if (step == Step::Full) {
            // Grow failed: the output reached the limit with more to come.
            return Result::TooLarge;
        }
        return o_ > 0 ? Result::Salvaged : Result::Corrupt;
    }

    // The bytes Run leaves in the buffer: the output, cut at the limit.
    size_t Length() const { return std::min(o_, maxOut_); }

    // Streaming (Stream): the buffer holds the 32 KB window before the output not yet given out
    // and room for a piece (or the whole output, when that is less); it never grows.
    void BeginStream(size_t piece) {
        SkipHeader();
        growable_ = false;
        room_ = std::min(kWindow + piece, Cap());
        buf_ = out_.Reserve(room_ + kSlack, 0);
        cap_ = room_;
    }

    // The next piece of output, as Run would have it: false at its end, `result_` then set.
    bool NextPiece(const uint8_t*& data, size_t& size) {
        for (;;) {
            // base_ (the output moved out of the buffer) never passes maxOut_.
            const size_t end = std::min(o_, maxOut_ - base_);
            if (delivered_ < end) {
                data = buf_ + delivered_;
                size = end - delivered_;
                delivered_ = end;
                return true;
            }
            if (ended_) return false;
            if (o_ > maxOut_ - base_) {
                result_ = Result::TooLarge;
                ended_ = true;
                continue;
            }
            if (o_ >= cap_) Slide();
            const Step step = Pump();
            if (step == Step::Full) continue;
            ended_ = true;
            if (o_ > maxOut_ - base_) result_ = Result::TooLarge;
            else if (step == Step::Ok) result_ = Result::Ok;
            else result_ = base_ + o_ > 0 ? Result::Salvaged : Result::Corrupt;
        }
    }

    Result StreamResult() const { return result_; }

private:
    static constexpr size_t kWindow = size_t(1) << 15;

    // The buffer's size beyond which output is too large: one byte past the limit tells "more".
    size_t Cap() const { return maxOut_ + 1; }

    bool Resize(size_t capacity) {
        buf_ = out_.Reserve(capacity + kSlack, o_);
        cap_ = capacity;
        return true;
    }

    bool Grow() {
        if (!growable_ || cap_ >= Cap()) return false;
        return Resize(std::min(Cap(), std::max<size_t>(cap_ * 2, size_t(64) << 10)));
    }

    // Decodes blocks until the buffer is full (Full), the final block has ended (Ok), or the data
    // is truncated or corrupt; resumes where the last call stopped.
    Step Pump() {
        for (;;) {
            if (!inBlock_) {
                if (final_) return Step::Ok;
                const Step header = ReadBlockHeader();
                if (header != Step::Ok) return header;
                inBlock_ = true;
            }
            const Step step = stored_ ? CopyStored() : DecodeCodes();
            if (step != Step::Ok) return step;
            inBlock_ = false;
        }
    }

    // Keeps the last 32 KB of a full buffer (every byte of it given out) at its start: what a
    // match can reach back to. Matches then never reach before the buffer's start.
    void Slide() {
        const size_t keep = std::min(o_, kWindow);
        const size_t drop = o_ - keep;
        std::memmove(buf_, buf_ + drop, keep);
        base_ += drop;
        delivered_ -= drop;
        o_ = keep;
        cap_ = std::min(room_, Cap() - base_);
    }

    // A zlib header (with a preset dictionary's id skipped over: references into it fail later,
    // after the output before them), else raw deflate. A stray white-space byte before the
    // header (a stream keyword's end of line counted wrong) is skipped.
    void SkipHeader() {
        const size_t size = static_cast<size_t>(end_ - begin_);
        size_t at = 0;
        if (size >= 3 && !ZlibHeader(begin_) &&
            (begin_[0] == '\r' || begin_[0] == '\n' || begin_[0] == ' ') && ZlibHeader(begin_ + 1)) {
            at = 1;
        }
        if (size - at >= 2 && ZlibHeader(begin_ + at)) {
            at += (begin_[at + 1] & 0x20) ? 6 : 2;
            in_ = begin_ + std::min(at, size);
        }
    }

    void Refill() {
        if (end_ - in_ >= 8) {
            bitbuf_ |= Load64(in_) << bitsleft_;
            in_ += (63 - bitsleft_) >> 3;
            bitsleft_ |= 56;
            return;
        }
        while (bitsleft_ <= 56) {
            uint64_t byte = 0;
            if (in_ < end_) byte = *in_++;
            else overread_++;
            bitbuf_ |= byte << bitsleft_;
            bitsleft_ += 8;
        }
    }

    // `count` bits, at most 32, after a refill that left them in the buffer.
    uint32_t Bits(unsigned count) {
        const uint32_t value = static_cast<uint32_t>(bitbuf_ & ((uint64_t(1) << count) - 1));
        bitbuf_ >>= count;
        bitsleft_ -= count;
        return value;
    }

    // False once the bits taken reach past the end of the input (into the zeros Refill adds).
    bool Within() const {
        const uint64_t consumed = (static_cast<uint64_t>(in_ - begin_) + overread_) * 8 - bitsleft_;
        return consumed <= static_cast<uint64_t>(end_ - begin_) * 8;
    }

    // Puts the whole bytes still in the bit buffer back into the input; false when some of the
    // bits taken were past its end.
    bool AlignToByte() {
        bitbuf_ >>= bitsleft_ & 7;
        bitsleft_ &= ~7u;
        if (overread_ * 8 > bitsleft_) return false;
        in_ -= bitsleft_ / 8 - overread_;
        bitbuf_ = 0;
        bitsleft_ = 0;
        overread_ = 0;
        return true;
    }

    Step ReadDynamicTables() {
        Refill();
        const unsigned litlenCount = 257 + Bits(5);
        const unsigned distanceCount = 1 + Bits(5);
        const unsigned precodeCount = 4 + Bits(4);
        if (litlenCount > 286 || distanceCount > 30) return Within() ? Step::Corrupt : Step::Truncated;
        uint8_t precodeLengths[19] = {};
        for (unsigned index = 0; index < precodeCount; index++) {
            if (bitsleft_ < 3) Refill();
            precodeLengths[kPrecodeOrder[index]] = static_cast<uint8_t>(Bits(3));
        }
        if (!Within()) return Step::Truncated;
        uint32_t precode[size_t(1) << kPrecodeRoot];
        if (!BuildTable(precodeLengths, 19, kPrecodeRoot, precode, size_t(1) << kPrecodeRoot, false, false,
                        PrecodeEntry())) {
            return Step::Corrupt;
        }
        uint8_t lengths[286 + 30];
        const unsigned total = litlenCount + distanceCount;
        for (unsigned index = 0; index < total;) {
            Refill();
            if (!Within()) return Step::Truncated;
            const uint32_t entry = precode[bitbuf_ & ((1u << kPrecodeRoot) - 1)];
            Bits(TakenBits(entry));
            const unsigned symbol = Value(entry);
            if (symbol < 16) {
                lengths[index++] = static_cast<uint8_t>(symbol);
                continue;
            }
            uint8_t value = 0;
            unsigned repeat = 0;
            if (symbol == 16) {
                if (index == 0) return Step::Corrupt;
                value = lengths[index - 1];
                repeat = 3 + Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + Bits(3);
            } else {
                repeat = 11 + Bits(7);
            }
            if (repeat > total - index) return Step::Corrupt;
            std::memset(lengths + index, value, repeat);
            index += repeat;
        }
        if (!Within()) return Step::Truncated;
        if (lengths[256] == 0) return Step::Corrupt;
        if (!BuildTable(lengths, litlenCount, kLitLenRoot, dynamicLitlen_, kLitLenTableSize, true, false,
                        LitLenEntry()) ||
            !BuildTable(lengths + litlenCount, distanceCount, kDistRoot, dynamicDistance_, kDistTableSize, true, true,
                        DistanceEntry())) {
            return Step::Corrupt;
        }
        litlen_ = dynamicLitlen_;
        distance_ = dynamicDistance_;
        return Step::Ok;
    }

    Step ReadBlockHeader() {
        if (bitsleft_ < 3) Refill();
        final_ = Bits(1) != 0;
        const unsigned type = Bits(2);
        if (!Within()) return Step::Truncated;
        if (type == 0) {
            if (!AlignToByte() || end_ - in_ < 4) return Step::Truncated;
            const unsigned length = in_[0] | in_[1] << 8;
            const unsigned complement = in_[2] | in_[3] << 8;
            if (length != (~complement & 0xFFFF)) return Step::Corrupt;
            in_ += 4;
            storedLeft_ = length;
            stored_ = true;
            return Step::Ok;
        }
        stored_ = false;
        if (type == 1) {
            litlen_ = Fixed().litlen;
            distance_ = Fixed().distance;
            return Step::Ok;
        }
        if (type != 2) return Step::Corrupt;
        return ReadDynamicTables();
    }

    Step CopyStored() {
        while (storedLeft_ > 0) {
            if (o_ >= cap_ && !Grow()) return Step::Full;
            const size_t count = std::min(storedLeft_, cap_ - o_);
            const size_t available = static_cast<size_t>(end_ - in_);
            if (available < count) {
                std::memcpy(buf_ + o_, in_, available);
                o_ += available;
                in_ = end_;
                return Step::Truncated;
            }
            std::memcpy(buf_ + o_, in_, count);
            in_ += count;
            o_ += count;
            storedLeft_ -= count;
        }
        return Step::Ok;
    }

    // Decodes codes until the buffer holds cap_ bytes or the block ends; a match may run past
    // cap_, into the slack. While 16 input bytes are left, a refill is a single load, and the next
    // code's entry is looked up before it from the bits already there, which keeps the refill off
    // the chain of lookups; up to three literals go between refills. The last bytes go a code at a
    // time, each checked against the end of the input so that no byte comes from padding.
    Step DecodeCodes() {
        uint8_t* const w = buf_;
        size_t o = o_;
        const size_t target = cap_;
        const uint32_t* const lit = litlen_;
        const uint32_t* const dist = distance_;
        if (end_ - in_ >= 16) {
            const uint8_t* const fastEnd = end_ - 16;
            uint64_t bb = bitbuf_;
            unsigned bl = bitsleft_;
            const uint8_t* ip = in_;
            const auto refill = [&]() {
                bb |= Load64(ip) << bl;
                ip += (63 - bl) >> 3;
                bl |= 56;
            };
            const auto consume = [&](uint32_t entry) {
                bb >>= TakenBits(entry);
                bl -= TakenBits(entry);
            };
            constexpr uint64_t kRootMask = (1u << kLitLenRoot) - 1;
            bool ok = true;
            bool ended = false;
            refill();
            uint32_t entry = lit[bb & kRootMask];
            // At the top at least 56 bits, and `entry` is the next code's.
            while (o < target && ip <= fastEnd) {
                uint64_t saved = bb;
                consume(entry);  // nothing for a subtable
                if ((entry & kKindMask) == kSubtable) {
                    entry = lit[Value(entry) + ((bb >> kLitLenRoot) & ((1u << Field(entry)) - 1))];
                    saved = bb;
                    consume(entry);
                }
                if ((entry & kKindMask) == kLiteral) {
                    // Up to two more codes of 11 bits whole in the root table: 15 + 22 bits.
                    uint8_t literal = static_cast<uint8_t>(Value(entry));
                    entry = lit[bb & kRootMask];
                    if ((entry & kKindMask) == kLiteral) {
                        consume(entry);
                        w[o++] = literal;
                        literal = static_cast<uint8_t>(Value(entry));
                        entry = lit[bb & kRootMask];
                        if ((entry & kKindMask) == kLiteral) {
                            consume(entry);
                            w[o++] = literal;
                            literal = static_cast<uint8_t>(Value(entry));
                            entry = lit[bb & kRootMask];
                        }
                    }
                    refill();  // the bits under the entry stay
                    w[o++] = literal;
                    continue;
                }
                const uint32_t kind = entry & kKindMask;
                if (kind != kLength) {
                    if (kind == kEnd) ended = true;
                    else ok = false;
                    break;
                }
                // At least 36 bits left for the distance's 15 and 13.
                const size_t length = ValuePlusExtra(entry, saved);
                const uint32_t code = Lookup(dist, bb, kDistRoot);
                const size_t back = ValuePlusExtra(code, bb);
                consume(code);
                if ((code & kKindMask) == kInvalid || back > o) {  // before the start of the output
                    ok = false;
                    break;
                }
                refill();
                entry = lit[bb & kRootMask];
                CopyMatch(w + o, back, length);
                o += length;
            }
            bitbuf_ = bb;
            bitsleft_ = bl;
            in_ = ip;
            o_ = o;
            if (!ok) return Step::Corrupt;
            if (ended) return Step::Ok;
            if (o >= target) return Step::Full;
        }
        while (o < target) {
            Refill();
            const size_t before = o;
            const uint32_t entry = Lookup(lit, bitbuf_, kLitLenRoot);
            const uint64_t lengthBits = bitbuf_;
            Bits(TakenBits(entry));
            const uint32_t kind = entry & kKindMask;
            if (kind == kLiteral) {
                w[o++] = static_cast<uint8_t>(Value(entry));
            } else if (kind == kEnd) {
                o_ = o;
                return Within() ? Step::Ok : Step::Truncated;
            } else if (kind == kInvalid) {
                o_ = o;
                return Within() ? Step::Corrupt : Step::Truncated;
            } else {
                // At least 56 bits after the refill: 20 for the length, 28 for the distance.
                const size_t length = ValuePlusExtra(entry, lengthBits);
                const uint32_t code = Lookup(dist, bitbuf_, kDistRoot);
                const size_t back = ValuePlusExtra(code, bitbuf_);
                Bits(TakenBits(code));
                if (!Within()) {
                    o_ = before;
                    return Step::Truncated;
                }
                if ((code & kKindMask) == kInvalid || back > o) {
                    o_ = o;
                    return Step::Corrupt;
                }
                CopyMatch(w + o, back, length);
                o += length;
            }
            if (!Within()) {
                o_ = before;
                return Step::Truncated;
            }
        }
        o_ = o;
        return Step::Full;
    }

    const uint8_t* const begin_;
    const uint8_t* in_;
    const uint8_t* const end_;
    uint64_t bitbuf_ = 0;
    unsigned bitsleft_ = 0;
    unsigned overread_ = 0;  // zero bytes put in the bit buffer past the end of the input

    Buffer& out_;
    uint8_t* buf_ = nullptr;
    size_t cap_ = 0;  // bytes of buf_ before the slack
    size_t o_ = 0;    // bytes of output (in the buffer, when streaming)
    const size_t maxOut_;
    // Streaming only.
    bool growable_ = true;
    bool inBlock_ = false;
    bool ended_ = false;
    Result result_ = Result::Corrupt;
    size_t room_ = 0;       // the buffer's bytes before the slack
    size_t base_ = 0;       // output moved out of the buffer before buf_[0]
    size_t delivered_ = 0;  // bytes of the buffer given out

    bool final_ = false;
    bool stored_ = false;
    size_t storedLeft_ = 0;
    const uint32_t* litlen_ = nullptr;
    const uint32_t* distance_ = nullptr;
    uint32_t dynamicLitlen_[kLitLenTableSize];
    uint32_t dynamicDistance_[kDistTableSize];
};

// A buffer kept per thread for whole outputs up to this size, so that the common stream's output
// needs no allocation of its own before it is copied out; a larger one is freed after its call.
constexpr size_t kKeptBuffer = size_t(1) << 20;

} // namespace

Result Inflate(std::string_view in, std::string& out, size_t limit) {
    // An upper bound that keeps `limit + 1 + kSlack` in range.
    limit = std::min(limit, (size_t(1) << 40));
    thread_local Buffer buffer;
    Inflater inflater(reinterpret_cast<const uint8_t*>(in.data()), in.size(), buffer, limit);
    const Result result = inflater.Run();
    out.assign(reinterpret_cast<const char*>(buffer.bytes.get()), inflater.Length());
    if (buffer.size > kKeptBuffer) buffer = Buffer();
    return result;
}

struct Stream::State {
    Buffer buffer;
    std::unique_ptr<Inflater> inflater;
};

Stream::Stream() = default;
Stream::~Stream() = default;

void Stream::Start(std::string_view in, size_t limit) {
    limit = std::min(limit, (size_t(1) << 40));
    if (!state_) state_ = std::make_unique<State>();
    state_->inflater = std::make_unique<Inflater>(reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                                  state_->buffer, limit);
    state_->inflater->BeginStream(kPiece);
}

bool Stream::Next(const uint8_t*& data, size_t& size) {
    return state_ && state_->inflater && state_->inflater->NextPiece(data, size);
}

Result Stream::Finish() const {
    return state_ && state_->inflater ? state_->inflater->StreamResult() : Result::Corrupt;
}

} // namespace RayoPdf::Inflate
