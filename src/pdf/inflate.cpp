// SPDX-License-Identifier: Apache-2.0
// Forked from RayoMD src/core/inflate.cpp at ef106ca (Apache-2.0, same author).
// The decode tables and the decoding loops are RayoMD's; the output goes to one buffer that grows
// (kept per thread between calls and copied out once, so it is never zero-filled), or for a
// Stream to a window of 32 KB and a piece, and every way the stream can end is a Result (inflate.h).
#include "pdf/inflate.h"

#include "common/compiler.h"
#include "common/cpu.h"
#include "common/per_thread.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#define RAYOPDF_INFLATE_HUGE 1
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define RAYOPDF_INFLATE_SSE2 1
#endif
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#define RAYOPDF_INFLATE_BMI2 1
#endif

namespace RayoPdf::Inflate {
namespace {

#if RAYOPDF_INFLATE_BMI2
// The decode loop compiled for BMI2 (BZHI, SHRX: fewer instructions for the bit fields) runs where
// the CPU has it (common/cpu.h); the portable one is its reference.
bool UseBmi2() { return (CpuFeatures() & kCpuBmi2) != 0; }
#endif

// Past the target the fast loop may write two literals and a match of 258 bytes, whose copy goes
// up to 31 bytes further.
constexpr size_t kSlack = 2 + 258 + 32 + 8;

// A decode table entry: the value in the high half (a literal, a base length or distance, or a
// subtable's offset), flags, a field, and in the low byte only the bits to take, so that a shift
// by the entry needs no mask on x86. A literal has the top bit (one sign test tells it); the end
// of the block, a subtable and an invalid code have kExceptional; a length, a distance and a code
// length have no flag. A length or distance takes its code and extra bits at once, the field
// giving the code's bits, under which the extra ones start; a subtable's field gives its bits.
enum : uint32_t {
    kPlain = 0,  // a length, a distance or a code length
    kLiteral = 1u << 31,
    kExceptional = 1u << 15,
    kSubtableBit = 1u << 14,
    kEndBit = 1u << 13,
    kSubtable = kExceptional | kSubtableBit,
    kEnd = kExceptional | kEndBit,
    kInvalid = kExceptional,
};
inline bool IsLiteral(uint32_t entry) { return static_cast<int32_t>(entry) < 0; }

constexpr uint32_t Entry(uint32_t value, uint32_t field, uint32_t kind, uint32_t bits) {
    return value << 16 | kind | field << 8 | bits;
}
inline unsigned TakenBits(uint32_t entry) { return entry & 63; }
inline unsigned Field(uint32_t entry) { return (entry >> 8) & 31; }
inline uint32_t Value(uint32_t entry) { return entry >> 16; }
// The masks of the low 0 to 63 bits: without BMI2's BZHI one load is fewer instructions than a
// mask made by a shift.
struct LowMasks {
    uint64_t of[64] = {};
    constexpr LowMasks() {
        for (unsigned n = 1; n < 64; n++) of[n] = (uint64_t(1) << n) - 1;
    }
};
constexpr LowMasks kLowMasks;
// The low `n` bits of `bits` (n < 64): BZHI where kBmi2, a mask from kLowMasks elsewhere.
template <bool kBmi2>
RAYOPDF_HOT_INLINE uint64_t LowBits(uint64_t bits, unsigned n) {
    return kBmi2 ? bits & ((uint64_t(1) << n) - 1) : bits & kLowMasks.of[n];
}
// The base plus the extra bits of a length or distance `entry` read from `bits`.
template <bool kBmi2>
RAYOPDF_HOT_INLINE size_t ValuePlusExtra(uint32_t entry, uint64_t bits) {
    return Value(entry) + static_cast<size_t>(LowBits<kBmi2>(bits, TakenBits(entry)) >> Field(entry));
}

// Codes are looked up by their first kRootBits bits; a longer code's prefix leads to a subtable of
// its remaining bits, at most 15 in all. The tables hold the root and at most this many subtables:
// a code is complete wherever it has codes longer than the root (BuildTable refuses an incomplete
// one but for a single code of one bit), so every prefix with a subtable leads to at least two
// codes: at most 286 / 2 literal/length and 30 / 2 distance subtables. Smaller tables than for
// every symbol a subtable: 26 KB instead of 43 KB (DynamicTables).
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

// A code's entry, from the symbol and the code's length: a base for the symbol plus the length
// times a step (1 for the bits to take; 0x101 also for the field of a length or distance).
template <unsigned kCount>
struct EntryTable {
    uint32_t base[kCount] = {};
    uint16_t step[kCount] = {};
    uint32_t operator()(unsigned symbol, unsigned bits) const { return base[symbol] + bits * step[symbol]; }
};

struct LitLenEntries : EntryTable<288> {
    constexpr LitLenEntries() {
        for (unsigned symbol = 0; symbol < 288; symbol++) {
            step[symbol] = 1;
            if (symbol < 256) {
                base[symbol] = Entry(symbol, 0, kLiteral, 0);
            } else if (symbol == 256) {
                base[symbol] = Entry(0, 0, kEnd, 0);
            } else if (symbol < 286) {
                base[symbol] = Entry(kLengthBase[symbol - 257], 0, kPlain, kLengthExtra[symbol - 257]);
                step[symbol] = 0x101;
            } else {
                base[symbol] = Entry(0, 0, kInvalid, 0);  // 286 and 287 have fixed codes but no meaning
            }
        }
    }
};
constexpr LitLenEntries kLitLenEntries;

struct DistanceEntries : EntryTable<32> {
    constexpr DistanceEntries() {
        for (unsigned symbol = 0; symbol < 32; symbol++) {
            step[symbol] = 1;
            if (symbol < 30) {
                base[symbol] = Entry(kDistanceBase[symbol], 0, kPlain, kDistanceExtra[symbol]);
                step[symbol] = 0x101;
            } else {
                base[symbol] = Entry(0, 0, kInvalid, 0);
            }
        }
    }
};
constexpr DistanceEntries kDistanceEntries;

struct PrecodeEntry {
    uint32_t operator()(unsigned symbol, unsigned bits) const { return Entry(symbol, 0, kPlain, bits); }
};

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

// `use(symbol)` for every symbol but those in runs of eight unused ones, skipped a word at a time.
template <typename Use>
RAYOPDF_HOT_INLINE void ForUsedSymbols(const uint8_t* lengths, unsigned count, Use use) {
    unsigned symbol = 0;
    for (; symbol + 8 <= count; symbol += 8) {
        uint64_t word;
        std::memcpy(&word, lengths + symbol, 8);
        if (word == 0) continue;
        for (unsigned at = symbol; at < symbol + 8; at++) use(at);
    }
    for (; symbol < count; symbol++) use(symbol);
}

// The decode table of the canonical code with `lengths` (0 for an unused symbol, at most 15),
// as zlib builds it: the number of entries it takes (the root and its subtables), or 0 for an
// over-subscribed code, and for an incomplete one unless it has a single code of one bit or, where
// `allowEmpty`, no code at all. `incompleteAllowed` false refuses every incomplete code (the code
// length code). 0 also when the subtables would pass `tableSize` entries (which the bound above
// rules out).
//
// A complete code is filled in canonical order (by length, then symbol), `code` running through
// the codes bit-reversed (an index). The codes of one length up to the root go into the first
// 2^length entries, which are then doubled for the next length, so that every root entry is
// written once and the copies go 16 bytes at a time. The longer codes follow, those under one root
// prefix one after another, each prefix's subtable after the last one's. `known`, when given,
// holds the number of symbols of each length (its entry 0 is not read), counted as the lengths
// were read; else they are counted here.
template <typename EntryOf>
size_t BuildTable(const uint8_t* lengths, unsigned count, unsigned root, uint32_t* table, size_t tableSize,
                  bool incompleteAllowed, bool allowEmpty, const EntryOf& entryOf, const unsigned* known = nullptr) {
    unsigned counts[16] = {};
    if (known) {
        for (unsigned bits = 1; bits <= 15; bits++) counts[bits] = known[bits];
    } else {
        ForUsedSymbols(lengths, count, [&](unsigned symbol) { counts[lengths[symbol]]++; });
    }
    counts[0] = 0;
    int left = 1;
    unsigned codes = 0;
    for (unsigned bits = 1; bits <= 15; bits++) {
        left <<= 1;
        left -= static_cast<int>(counts[bits]);
        if (left < 0) return 0;
        codes += counts[bits];
    }
    const size_t rootSize = size_t(1) << root;
    if (left > 0) {
        // A single code of one bit or none: its root entries and invalid ones between.
        const bool single = codes == 1 && counts[1] == 1;
        if (!incompleteAllowed || !(single || (codes == 0 && allowEmpty))) return 0;
        for (size_t index = 0; index < rootSize; index++) table[index] = Entry(0, 0, kInvalid, 1);
        for (unsigned symbol = 0; symbol < count; symbol++) {
            if (lengths[symbol] == 0) continue;
            for (size_t index = 0; index < rootSize; index += 2) table[index] = entryOf(symbol, 1);
        }
        return rootSize;
    }
    // The symbols sorted by length, then symbol, the unused ones last (not read).
    unsigned offsets[16];
    offsets[1] = 0;
    for (unsigned bits = 1; bits < 15; bits++) offsets[bits + 1] = offsets[bits] + counts[bits];
    offsets[0] = codes;
    uint16_t sorted[288];
    ForUsedSymbols(lengths, count,
                   [&](unsigned symbol) { sorted[offsets[lengths[symbol]]++] = static_cast<uint16_t>(symbol); });
    // The root: its first `filled` entries hold the codes so far, repeated.
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
                return rootSize;
            }
            // The next code: a bit-reversed increment.
            const unsigned bit = 1u << HighestBit(code ^ static_cast<unsigned>(filled - 1));
            code = (code & (bit - 1)) | bit;
        }
    }
    // Codes longer than the root (the root holds all others): a subtable for each root prefix.
    const unsigned subBits = 15 - root;
    const size_t subSize = size_t(1) << subBits;
    const unsigned rootMask = static_cast<unsigned>(rootSize - 1);
    size_t subNext = rootSize;
    unsigned prefix = ~0u;
    uint32_t* sub = nullptr;
    for (; length <= 15; length++) {
        const unsigned last = (1u << length) - 1;
        for (unsigned remaining = counts[length]; remaining > 0; remaining--) {
            if ((code & rootMask) != prefix) {
                prefix = code & rootMask;
                if (subNext + subSize > tableSize) return 0;
                table[prefix] = Entry(static_cast<uint32_t>(subNext), subBits, kSubtable, 0);  // takes no bits itself
                sub = table + subNext;
                subNext += subSize;
            }
            const uint32_t entry = entryOf(*nextSymbol++, length);
            for (size_t index = code >> root; index < subSize; index += size_t(1) << (length - root)) sub[index] = entry;
            if (code == last) return subNext;
            const unsigned bit = 1u << HighestBit(code ^ last);
            code = (code & (bit - 1)) | bit;
        }
    }
    return subNext;
}

inline uint64_t Load64(const uint8_t* at) {
    uint64_t value;
    std::memcpy(&value, at, 8);
    return value;  // little-endian targets only
}

inline void Store64(uint8_t* at, uint64_t value) { std::memcpy(at, &value, 8); }

// The entry for the code at the bottom of `bits`, through its subtable when it is longer than `root`.
template <bool kBmi2>
RAYOPDF_HOT_INLINE uint32_t Lookup(const uint32_t* table, uint64_t bits, unsigned root) {
    uint32_t entry = table[bits & ((uint64_t(1) << root) - 1)];
    if (entry & kSubtableBit) entry = table[Value(entry) + LowBits<kBmi2>(bits >> root, Field(entry))];
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
    // The step is back * (8 / back), from a table (a division costs 15 to 40 cycles).
    constexpr uint8_t kWholePeriods[8] = {0, 8, 8, 6, 8, 5, 6, 7};
    const size_t step = kWholePeriods[back];
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
        BuildTable(lengths, 288, kLitLenRoot, litlen, kFixedLitLenTableSize, false, false, kLitLenEntries);
        std::memset(lengths, 5, 32);
        BuildTable(lengths, 32, kDistRoot, distance, kFixedDistTableSize, false, false, kDistanceEntries);
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

// The decode tables of a dynamic block: the literal/length table, then the distance table right
// after the entries the first one takes, so that a block's tables usually fill no more pages than
// the root of 8 KB takes and one. Not on the stack: a frame of their 26 KB is probed page by page
// (stack-clash protection), so a fresh process faulted all of it in on its first inflate.
struct DynamicTables {
    uint32_t entries[kLitLenTableSize + kDistTableSize];
};

// Every byte of the decoders' output buffer that is kept is written first: it grows without
// zero-filling (Output).
class Inflater {
public:
    // Decodes into `out` from its byte `at` on (`at` 0 when streaming), building dynamic blocks'
    // tables in `tables`, which nothing else uses while the inflater works.
    Inflater(const uint8_t* data, size_t size, Output& out, size_t at, size_t maxOut, DynamicTables& tables)
        : begin_(data), in_(data), end_(data + size), out_(out), at_(at), maxOut_(maxOut), dynamic_(tables.entries) {}

    // Inflates the whole input into the buffer: Length() bytes of it.
    Result Run() {
        SkipHeader();
        // A first guess at the size: content streams inflate 3 to 6 times; the buffer doubles. A
        // buffer kept from an earlier stream is used whole.
        const size_t room = out_.Capacity() > at_ + kSlack ? out_.Capacity() - at_ - kSlack : 0;
        const size_t guess = std::max<size_t>(size_t(4096), static_cast<size_t>(end_ - begin_) * 4);
        if (!Resize(std::min(Cap(), std::max(guess, room)))) return Result::Corrupt;
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
        buf_ = out_.Reserve(room_ + kSlack);
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
        buf_ = out_.Reserve(at_ + capacity + kSlack) + at_;
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
        // The literal/length and the distance code's symbols of each length, counted as they are
        // read (BuildTable then counts nothing); zeros go uncounted.
        unsigned counts[2][16] = {};
        // A refill leaves enough bits for at least a code and its extra bits (7 + 7). Whether the
        // bits taken passed the end of the input is asked after the last code, and before a code
        // found corrupt: the answers asking before every code would give, as that only grows.
        for (unsigned index = 0; index < total;) {
            if (bitsleft_ < 14) Refill();
            const uint32_t entry = precode[bitbuf_ & ((1u << kPrecodeRoot) - 1)];
            const unsigned symbol = Value(entry);
            if (symbol < 16) {
                Bits(TakenBits(entry));
                counts[index >= litlenCount][symbol]++;
                lengths[index++] = static_cast<uint8_t>(symbol);
                continue;
            }
            const unsigned extraBits = symbol == 16 ? 2 : symbol == 17 ? 3 : 7;
            const unsigned repeat = (symbol == 18 ? 11 : 3) +
                                    static_cast<unsigned>((bitbuf_ >> TakenBits(entry)) & ((1u << extraBits) - 1));
            if ((symbol == 16 && index == 0) || repeat > total - index) {
                return Within() ? Step::Corrupt : Step::Truncated;
            }
            Bits(TakenBits(entry) + extraBits);
            const uint8_t value = symbol == 16 ? lengths[index - 1] : 0;
            if (value != 0) {
                const unsigned literal = index < litlenCount ? std::min(repeat, litlenCount - index) : 0;
                counts[0][value] += literal;
                counts[1][value] += repeat - literal;
            }
            std::memset(lengths + index, value, repeat);
            index += repeat;
        }
        if (!Within()) return Step::Truncated;
        if (lengths[256] == 0) return Step::Corrupt;
        const size_t litlenEntries = BuildTable(lengths, litlenCount, kLitLenRoot, dynamic_, kLitLenTableSize, true,
                                                false, kLitLenEntries, counts[0]);
        if (litlenEntries == 0 || !BuildTable(lengths + litlenCount, distanceCount, kDistRoot, dynamic_ + litlenEntries,
                                              kDistTableSize, true, true, kDistanceEntries, counts[1])) {
            return Step::Corrupt;
        }
        litlen_ = dynamic_;
        distance_ = dynamic_ + litlenEntries;
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
    // cap_, into the slack. With BMI2 (runtime dispatch) the same code compiled for it.
    Step DecodeCodes() {
#if RAYOPDF_INFLATE_BMI2
        if (UseBmi2()) return DecodeCodesBmi2();
#endif
        return DecodeCodesWith<false>();
    }

#if RAYOPDF_INFLATE_BMI2
    __attribute__((target("bmi2"), noinline)) Step DecodeCodesBmi2() { return DecodeCodesWith<true>(); }
#endif

    // DecodeCodes. While 16 input bytes are left, a refill is a single load, and the next code's
    // entry is looked up before it from the bits already there, which keeps the refill off the
    // chain of lookups; up to three literals go between refills. The last bytes go a code at a
    // time, each checked against the end of the input so that no byte comes from padding.
    template <bool kBmi2>
    RAYOPDF_HOT_INLINE Step DecodeCodesWith() {
        uint8_t* const w = buf_;
        size_t o = o_;
        const size_t target = cap_;
        const uint32_t* const lit = litlen_;
        const uint32_t* const dist = distance_;
        if (end_ - in_ >= 16) {
            const uint8_t* const fastEnd = end_ - 16;
            uint64_t bb = bitbuf_;
            // The bits left in the low byte of `bl`; above it whatever entries subtracted whole
            // (an entry's low byte is its bits to take, so no mask is needed per code).
            uint32_t bl = bitsleft_;
            const uint8_t* ip = in_;
            const auto refill = [&]() {
                bb |= Load64(ip) << (bl & 63);
                ip += 7 - ((bl >> 3) & 7);
                bl |= 56;
            };
            const auto consume = [&](uint32_t entry) {
                bb >>= TakenBits(entry);
                bl -= entry;
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
                if (IsLiteral(entry)) {
                    // Up to two more codes of 11 bits whole in the root table: 15 + 22 bits.
                    uint8_t literal = static_cast<uint8_t>(Value(entry));
                    entry = lit[bb & kRootMask];
                    if (IsLiteral(entry)) {
                        consume(entry);
                        w[o++] = literal;
                        literal = static_cast<uint8_t>(Value(entry));
                        entry = lit[bb & kRootMask];
                        if (IsLiteral(entry)) {
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
                if (entry & kExceptional) {
                    if (entry & kSubtableBit) {
                        entry = lit[Value(entry) + LowBits<kBmi2>(bb >> kLitLenRoot, Field(entry))];
                        saved = bb;
                        consume(entry);
                        if (IsLiteral(entry)) {
                            const uint8_t literal = static_cast<uint8_t>(Value(entry));
                            refill();
                            entry = lit[bb & kRootMask];
                            w[o++] = literal;
                            continue;
                        }
                    }
                    if (entry & kExceptional) {
                        if (entry & kEndBit) ended = true;
                        else ok = false;
                        break;
                    }
                }
                // At least 36 bits left for the distance's 15 and 13.
                const size_t length = ValuePlusExtra<kBmi2>(entry, saved);
                const uint32_t code = Lookup<kBmi2>(dist, bb, kDistRoot);
                const size_t back = ValuePlusExtra<kBmi2>(code, bb);
                consume(code);
                if ((code & kExceptional) || back > o) {  // invalid, or before the start of the output
                    ok = false;
                    break;
                }
                refill();
                entry = lit[bb & kRootMask];
                CopyMatch(w + o, back, length);
                o += length;
            }
            bitbuf_ = bb;
            bitsleft_ = bl & 63;
            in_ = ip;
            o_ = o;
            if (!ok) return Step::Corrupt;
            if (ended) return Step::Ok;
            if (o >= target) return Step::Full;
        }
        while (o < target) {
            Refill();
            const size_t before = o;
            const uint32_t entry = Lookup<kBmi2>(lit, bitbuf_, kLitLenRoot);
            const uint64_t lengthBits = bitbuf_;
            Bits(TakenBits(entry));
            if (IsLiteral(entry)) {
                w[o++] = static_cast<uint8_t>(Value(entry));
            } else if ((entry & kEnd) == kEnd) {
                o_ = o;
                return Within() ? Step::Ok : Step::Truncated;
            } else if (entry & kExceptional) {
                o_ = o;
                return Within() ? Step::Corrupt : Step::Truncated;
            } else {
                // At least 56 bits after the refill: 20 for the length, 28 for the distance.
                const size_t length = ValuePlusExtra<kBmi2>(entry, lengthBits);
                const uint32_t code = Lookup<kBmi2>(dist, bitbuf_, kDistRoot);
                const size_t back = ValuePlusExtra<kBmi2>(code, bitbuf_);
                Bits(TakenBits(code));
                if (!Within()) {
                    o_ = before;
                    return Step::Truncated;
                }
                if ((code & kExceptional) || back > o) {
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

    Output& out_;
    const size_t at_;  // where the output starts in out_
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
    uint32_t* const dynamic_;  // DynamicTables::entries
};

// A buffer kept per thread for whole outputs up to this size, so that the common stream's output
// needs no allocation of its own before it is copied out; a larger one is freed after its call.
constexpr size_t kKeptBuffer = size_t(1) << 20;
// The largest output limit: 1 TiB, far beyond any stream, or half the address space where size_t
// has 32 bits (the WebAssembly module), so that `limit + 1 + kSlack` stays in range.
constexpr size_t kMaxLimit = sizeof(size_t) >= 8 ? static_cast<size_t>(uint64_t(1) << 40) : SIZE_MAX / 2;

#if RAYOPDF_INFLATE_HUGE
// A block of at least this many bytes is a mapping of its own with transparent huge pages, in a
// process that asked for it (UseHugePages).
constexpr size_t kHugeOutput = size_t(1) << 20;
constexpr size_t kHugePage = size_t(2) << 20;
// Set before any thread converts, read after.
bool hugePages = false;
#endif

} // namespace

void UseHugePages(bool on) {
#if RAYOPDF_INFLATE_HUGE
    hugePages = on;
#else
    (void)on;
#endif
}

Output::~Output() { Release(); }

uint8_t* Output::Reserve(size_t capacity) {
    if (capacity > capacity_) {
#if RAYOPDF_INFLATE_HUGE
        if (hugePages && capacity >= kHugeOutput) return Map(capacity);
#endif
        void* grown = std::realloc(data_, capacity);
        // As operator new[] without exceptions: no memory ends the process.
        if (!grown) std::abort();
        data_ = static_cast<uint8_t*>(grown);
        capacity_ = capacity;
    }
    return data_;
}

#if RAYOPDF_INFLATE_HUGE
// The block as a 2 MiB-aligned mapping of whole 2 MiB pages, advised for transparent huge pages:
// one fault and about a quarter of the time per 4 KiB for a large page string
// (docs/research/speed.md § 5.3). Advice on a block of the heap would not do: a block grown in
// place has had its first half faulted in 4 KiB pages, so no aligned 2 MiB of it is untouched; and
// advice on part of a malloc mapping splits it, after which realloc copies instead of mremap.
// A mapping grows with mremap, which keeps the advice; the block it leaves is copied once.
uint8_t* Output::Map(size_t capacity) {
    if (capacity > SIZE_MAX - kHugePage * 2) std::abort();
    const size_t size = (capacity + kHugePage - 1) & ~(kHugePage - 1);
    if (mapped_) {
        void* moved = mremap(data_, capacity_, size, MREMAP_MAYMOVE);
        if (moved == MAP_FAILED) std::abort();
        data_ = static_cast<uint8_t*>(moved);
        capacity_ = size;
        return data_;
    }
    // One huge page more, then the unaligned ends given back.
    void* raw = mmap(nullptr, size + kHugePage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) std::abort();
    const uintptr_t begin = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t at = (begin + kHugePage - 1) & ~uintptr_t(kHugePage - 1);
    if (at > begin) munmap(raw, at - begin);
    if (at - begin < kHugePage) munmap(reinterpret_cast<void*>(at + size), kHugePage - (at - begin));
    uint8_t* block = reinterpret_cast<uint8_t*>(at);
    // Errors are ignored: the advice changes no byte.
    (void)madvise(block, size, MADV_HUGEPAGE);
    if (capacity_ > 0) std::memcpy(block, data_, capacity_);
    std::free(data_);
    data_ = block;
    capacity_ = size;
    mapped_ = true;
    return data_;
}
#endif

uint8_t* Output::Grow(size_t capacity) {
    if (capacity <= capacity_) return data_;
    return Reserve(std::max(capacity, capacity_ > SIZE_MAX / 2 ? SIZE_MAX : capacity_ * 2));
}

void Output::Release() {
#if RAYOPDF_INFLATE_HUGE
    if (mapped_) {
        munmap(data_, capacity_);
        mapped_ = false;
    } else {
        std::free(data_);
    }
#else
    std::free(data_);
#endif
    data_ = nullptr;
    capacity_ = 0;
}

Result Inflate(std::string_view in, std::string& out, size_t limit) {
    // An upper bound that keeps `limit + 1 + kSlack` in range.
    limit = std::min(limit, kMaxLimit);
    Output& buffer = PerThread<Output>();
    Inflater inflater(reinterpret_cast<const uint8_t*>(in.data()), in.size(), buffer, 0, limit,
                      PerThread<DynamicTables>());
    const Result result = inflater.Run();
    out.assign(reinterpret_cast<const char*>(buffer.Data()), inflater.Length());
    if (buffer.Capacity() > kKeptBuffer) buffer.Release();
    return result;
}

Result InflateInto(std::string_view in, Output& out, size_t at, size_t limit, size_t& length) {
    limit = std::min(limit, kMaxLimit);
    Inflater inflater(reinterpret_cast<const uint8_t*>(in.data()), in.size(), out, at, limit,
                      PerThread<DynamicTables>());
    const Result result = inflater.Run();
    length = inflater.Length();
    return result;
}

// A stream keeps its inflater between pieces, while other inflates may run on its thread: tables
// of its own.
struct Stream::State {
    Output buffer;
    std::unique_ptr<DynamicTables> tables;
    std::unique_ptr<Inflater> inflater;
};

Stream::Stream() = default;
Stream::~Stream() = default;

void Stream::Start(std::string_view in, size_t limit) {
    limit = std::min(limit, kMaxLimit);
    if (!state_) state_ = std::make_unique<State>();
    if (!state_->tables) state_->tables.reset(new DynamicTables);
    state_->inflater = std::make_unique<Inflater>(reinterpret_cast<const uint8_t*>(in.data()), in.size(),
                                                  state_->buffer, 0, limit, *state_->tables);
    state_->inflater->BeginStream(kPiece);
}

bool Stream::Next(const uint8_t*& data, size_t& size) {
    return state_ && state_->inflater && state_->inflater->NextPiece(data, size);
}

Result Stream::Finish() const {
    return state_ && state_->inflater ? state_->inflater->StreamResult() : Result::Corrupt;
}

} // namespace RayoPdf::Inflate
