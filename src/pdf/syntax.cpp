// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/syntax.cpp at 9fbbfaf (Apache-2.0, same author).
#include "pdf/syntax.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace RayoPdf {

int64_t PdfValue::Int(int64_t fallback) const {
    if (type == Type::Int) return integer;
    // 2^63 is exact in a double; a NaN fails both comparisons.
    if (type == Type::Real && real > -9223372036854775808.0 && real < 9223372036854775808.0) {
        return static_cast<int64_t>(real);
    }
    return fallback;
}

const PdfValue* PdfValue::Find(std::string_view key) const {
    for (const PdfDictEntry& entry : Entries()) {
        if (entry.key == key) return &entry.value;
    }
    return nullptr;
}

namespace {

// Chunks of the largest regular size (kBigChunk with their header) that arenas dropped, for the
// next arena: a batch of large documents fills a few MB of them per document, which the system
// would otherwise map afresh every time. At most kKeptChunks; never destroyed (arenas may go during
// the process's exit).
constexpr size_t kBigChunk = size_t(1) << 20;
constexpr size_t kKeptChunks = 16;

struct ChunkPool {
    std::mutex mutex;
    std::vector<void*> chunks;
};

ChunkPool& Chunks() {
    static ChunkPool* const pool = new ChunkPool;
    return *pool;
}

void FreeChunk(void* chunk, size_t size) {
    if (size == kBigChunk) {
        ChunkPool& pool = Chunks();
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (pool.chunks.size() < kKeptChunks) {
            pool.chunks.push_back(chunk);
            return;
        }
    }
    std::free(chunk);
}

void* NewChunk(size_t bytes) {
    if (bytes == kBigChunk) {
        ChunkPool& pool = Chunks();
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (!pool.chunks.empty()) {
            void* chunk = pool.chunks.back();
            pool.chunks.pop_back();
            return chunk;
        }
    }
    return std::malloc(bytes);
}

} // namespace

PdfArena::~PdfArena() {
    while (head_) {
        Chunk* next = head_->next;
        FreeChunk(head_, sizeof(Chunk) + head_->size);
        head_ = next;
    }
}

void* PdfArena::AllocateNew(size_t bytes) {
    // Far beyond any input: a size that cannot be meant.
    if (static_cast<uint64_t>(bytes) > (uint64_t(1) << 40)) return nullptr;
    bytes = (bytes + 7) & ~size_t(7);
    if (bytes > static_cast<size_t>(end_ - at_)) {
        // Chunks double from 4 KiB to 1 MiB with their header; a bigger block gets a chunk of its own.
        constexpr size_t kLargest = kBigChunk - sizeof(Chunk);
        const size_t grown = std::min<size_t>(std::max<size_t>(head_ ? head_->size * 2 : 0, 4096), kLargest);
        const size_t size = std::max(grown, bytes);
        Chunk* chunk = static_cast<Chunk*>(NewChunk(sizeof(Chunk) + size));
        if (!chunk) return nullptr;
        chunk->next = head_;
        chunk->size = size;
        head_ = chunk;
        at_ = reinterpret_cast<char*>(chunk + 1);
        end_ = at_ + size;
        capacity_ += size;
    }
    void* block = at_;
    at_ += bytes;
    return block;
}

void PdfArena::Rewind() {
    Chunk* rest = head_->next;
    while (rest) {
        Chunk* next = rest->next;
        FreeChunk(rest, sizeof(Chunk) + rest->size);
        rest = next;
    }
    head_->next = nullptr;
    at_ = reinterpret_cast<char*>(head_ + 1);
    end_ = at_ + head_->size;
    capacity_ = head_->size;
}

namespace Syntax {
namespace {

int HexDigit(unsigned char c) { return kHexValues.of[c]; }

// 10^n for n >= 0, exact up to 10^22; larger powers multiply up and saturate to infinity.
double PowerOfTen(size_t n) {
    static const double kExact[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (n <= 22) return kExact[n];
    double value = kExact[22];
    for (n -= 22; n > 0 && value < 1e308; n--) value *= 10.0;
    return n > 0 ? value * 10.0 : value;
}

bool AllDigits(std::string_view token) {
    if (token.empty()) return false;
    for (char ch : token) {
        if (ch < '0' || ch > '9') return false;
    }
    return true;
}

} // namespace

void SkipSpace(Cursor& c) {
    while (c.pos < c.data.size()) {
        const unsigned char ch = static_cast<unsigned char>(c.data[c.pos]);
        if (IsWhite(ch)) {
            c.pos++;
        } else if (ch == '%') {
            while (c.pos < c.data.size() && c.data[c.pos] != '\n' && c.data[c.pos] != '\r') c.pos++;
        } else {
            break;
        }
    }
}

void SkipWhite(Cursor& c) {
    while (c.pos < c.data.size() && IsWhite(static_cast<unsigned char>(c.data[c.pos]))) c.pos++;
}

std::string_view ReadToken(Cursor& c) {
    const size_t start = c.pos;
    while (c.pos < c.data.size() && IsRegular(static_cast<unsigned char>(c.data[c.pos]))) c.pos++;
    return c.data.substr(start, c.pos - start);
}

bool ParseNumber(std::string_view token, PdfValue& out) {
    size_t i = 0;
    bool negative = false;
    if (i < token.size() && (token[i] == '+' || token[i] == '-')) negative = token[i++] == '-';
    // Up to 19 significant digits in `mantissa`; integer digits past them raise `scale`,
    // fraction digits past them are dropped.
    uint64_t mantissa = 0;
    size_t significant = 0;
    size_t scale = 0;
    size_t fraction = 0;
    size_t digits = 0;
    bool point = false;
    for (; i < token.size(); i++) {
        const char ch = token[i];
        if (ch == '.') {
            if (point) return false;
            point = true;
            continue;
        }
        if (ch < '0' || ch > '9') return false;
        digits++;
        const unsigned d = static_cast<unsigned>(ch - '0');
        if (significant < 19) {
            if (mantissa != 0 || d != 0) significant++;
            mantissa = mantissa * 10 + d;
            if (point) fraction++;
        } else if (!point) {
            scale++;
        }
    }
    if (digits == 0) return false;
    // 19 digits stay below 2^64; past INT64_MAX the number is a Real.
    if (!point && scale == 0 && mantissa <= static_cast<uint64_t>(INT64_MAX)) {
        out = PdfValue();
        out.type = PdfValue::Type::Int;
        out.integer = negative ? -static_cast<int64_t>(mantissa) : static_cast<int64_t>(mantissa);
        return true;
    }
    double value = static_cast<double>(mantissa);
    if (scale > 0) value *= PowerOfTen(scale);
    if (fraction > 0) value /= PowerOfTen(fraction);
    out = PdfValue();
    out.type = PdfValue::Type::Real;
    out.real = negative ? -value : value;
    return true;
}

namespace {

// `bytes` copied into the arena (a value counts at most UINT32_MAX of them).
bool Store(Builder& b, std::string_view bytes, std::string_view& out) {
    if (bytes.empty()) {
        out = {};
        return true;
    }
    if (bytes.size() > UINT32_MAX) return false;
    char* block = static_cast<char*>(b.arena.Allocate(bytes.size()));
    if (!block) return false;
    std::memcpy(block, bytes.data(), bytes.size());
    out = std::string_view(block, bytes.size());
    return true;
}

// `bytes` copied into the arena, or with Builder::views the bytes themselves.
bool Keep(Builder& b, std::string_view bytes, std::string_view& out) {
    if (!b.views) return Store(b, bytes, out);
    if (bytes.size() > UINT32_MAX) return false;
    out = bytes.empty() ? std::string_view() : bytes;
    return true;
}

// One load a byte for the scans below. Name bytes: 0 regular, 1 '#' (regular, an escape), 2 the
// end of the name (white space or a delimiter). Literal string bytes: 1 for those that need a
// look ('\\', '(', ')', '\r'), 0 for the rest. Hexadecimal string bytes: a digit's value, or
// kHexWhite, kHexEnd ('>') or kHexOther.
struct ScanTables {
    static constexpr uint8_t kHexWhite = 16;
    static constexpr uint8_t kHexEnd = 17;
    static constexpr uint8_t kHexOther = 18;
    uint8_t name[256] = {};
    uint8_t literal[256] = {};
    uint8_t hex[256] = {};
    constexpr ScanTables() {
        for (int c = 0; c < 256; c++) {
            name[c] = kByteClasses.of[c] != 0 ? 2 : 0;
            hex[c] = kHexValues.of[c] >= 0 ? static_cast<uint8_t>(kHexValues.of[c])
                     : (kByteClasses.of[c] & ByteClasses::kWhite) != 0 ? kHexWhite
                                                                        : kHexOther;
        }
        name['#'] = 1;
        hex['>'] = kHexEnd;
        for (char c : {'\\', '(', ')', '\r'}) literal[static_cast<unsigned char>(c)] = 1;
    }
};
constexpr ScanTables kScan;

// The end of the literal string run from `at`: the first byte that needs a look, or `size`.
inline size_t LiteralRunEnd(const char* data, size_t at, size_t size) {
    while (at < size && kScan.literal[static_cast<unsigned char>(data[at])] == 0) at++;
    return at;
}

// A name with #xx escapes, `run` its bytes after '/': decoded into the arena.
RAYOPDF_NOINLINE bool DecodeName(Builder& b, std::string_view run, std::string_view& out) {
    std::string& decoded = b.scratch;
    decoded.clear();
    for (size_t i = 0; i < run.size(); i++) {
        const unsigned char ch = static_cast<unsigned char>(run[i]);
        if (ch == '#' && i + 2 < run.size() && HexDigit(static_cast<unsigned char>(run[i + 1])) >= 0 &&
            HexDigit(static_cast<unsigned char>(run[i + 2])) >= 0) {
            decoded.push_back(static_cast<char>(HexDigit(static_cast<unsigned char>(run[i + 1])) * 16 +
                                                HexDigit(static_cast<unsigned char>(run[i + 2]))));
            i += 2;
        } else {
            decoded.push_back(static_cast<char>(ch));
        }
    }
    return Store(b, decoded, out);
}

// A literal string past its first run (bytes c.pos to `runEnd`, then a byte that needs a look):
// escapes, nested parentheses and line ends decoded into b.scratch, then into the arena. Every
// step writes its run and at most one byte more, so the buffer grows ahead of each.
RAYOPDF_NOINLINE bool ReadEscapedString(Cursor& c, Builder& b, size_t runEnd, std::string_view& out) {
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    std::string& text = b.scratch;
    size_t n = 0;
    size_t pos = c.pos;
    int depth = 1;
    for (;;) {
        const size_t run = runEnd - pos;
        if (text.size() < n + run + 1) text.resize(std::max(n + run + 1, text.size() * 2));
        char* const w = &text[0];
        std::memcpy(w + n, data + pos, run);
        n += run;
        pos = runEnd;
        if (pos >= size) break;
        const char ch = data[pos++];
        if (ch == '\\') {
            if (pos >= size) break;
            const char e = data[pos++];
            switch (e) {
                case 'n': w[n++] = '\n'; break;
                case 'r': w[n++] = '\r'; break;
                case 't': w[n++] = '\t'; break;
                case 'b': w[n++] = '\b'; break;
                case 'f': w[n++] = '\f'; break;
                case '\r':  // a line continuation
                    if (pos < size && data[pos] == '\n') pos++;
                    break;
                case '\n': break;
                default:
                    if (e >= '0' && e <= '7') {
                        // Up to two more octal digits.
                        unsigned value = static_cast<unsigned>(e - '0');
                        unsigned d;
                        if (pos < size && (d = static_cast<unsigned>(static_cast<unsigned char>(data[pos])) - '0') < 8) {
                            value = value * 8 + d;
                            pos++;
                            if (pos < size &&
                                (d = static_cast<unsigned>(static_cast<unsigned char>(data[pos])) - '0') < 8) {
                                value = value * 8 + d;
                                pos++;
                            }
                        }
                        w[n++] = static_cast<char>(value & 0xFF);
                    } else {
                        w[n++] = e;  // \( \) \\ and an unknown escape: the character itself
                    }
                    break;
            }
        } else if (ch == '(') {
            depth++;
            w[n++] = ch;
        } else if (ch == ')') {
            if (--depth == 0) {
                c.pos = pos;
                return Store(b, std::string_view(w, n), out);
            }
            w[n++] = ch;
        } else {  // '\r': an end of line in a string is a line feed
            w[n++] = '\n';
            if (pos < size && data[pos] == '\n') pos++;
        }
        runEnd = LiteralRunEnd(data, pos, size);
    }
    c.pos = pos;
    return false;
}

// ReadName, inline where dictionaries read their keys and name values.
RAYOPDF_HOT_INLINE bool ReadNameInline(Cursor& c, Builder& b, std::string_view& out) {
    // A #xx escape is three regular bytes: the name ends where they do.
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    const size_t start = c.pos + 1;  // after '/'
    size_t at = start;
    bool escaped = false;
    // Four regular bytes without '#' a step, then a byte at a time.
    const auto kind = [&](size_t i) { return kScan.name[static_cast<unsigned char>(data[i])]; };
    for (; at + 4 <= size && (kind(at) | kind(at + 1) | kind(at + 2) | kind(at + 3)) == 0; at += 4) {
    }
    for (; at < size; at++) {
        const uint8_t k = kind(at);
        if (k != 0) {
            if (k == 2) break;
            escaped = true;
        }
    }
    c.pos = at;
    const std::string_view run(data + start, at - start);
    if (!escaped) return Keep(b, run, out);
    return DecodeName(b, run, out);
}

} // namespace

bool ReadName(Cursor& c, Builder& b, std::string_view& out) { return ReadNameInline(c, b, out); }

bool ReadLiteralString(Cursor& c, Builder& b, std::string_view& out) {
    c.pos++;  // '('
    const size_t size = c.data.size();
    // Most strings hold no escape, nested parenthesis or line end before their closing one: the
    // bytes up to it, read at once.
    const size_t end = LiteralRunEnd(c.data.data(), c.pos, size);
    if (end < size && c.data[end] == ')') {
        const std::string_view run = c.data.substr(c.pos, end - c.pos);
        c.pos = end + 1;
        return Keep(b, run, out);
    }
    return ReadEscapedString(c, b, end, out);
}

bool ReadHexString(Cursor& c, Builder& b, std::string_view& out) {
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    const size_t first = c.pos + 1;  // after '<'
    // To the closing '>', every byte a hex digit or white space; the digits counted.
    size_t at = first;
    size_t digits = 0;
    for (;;) {
        if (at >= size) {
            c.pos = size;
            return false;
        }
        const uint8_t kind = kScan.hex[static_cast<unsigned char>(data[at++])];
        if (kind < 16) {
            digits++;
        } else if (kind == ScanTables::kHexEnd) {
            break;
        } else if (kind == ScanTables::kHexOther) {
            c.pos = at;
            return false;
        }
    }
    c.pos = at;
    // Two digits a byte; an odd last digit is its byte's high half.
    const size_t bytes = (digits + 1) / 2;
    if (bytes == 0) {
        out = {};
        return true;
    }
    if (bytes > UINT32_MAX) return false;
    char* const block = static_cast<char*>(b.arena.Allocate(bytes));
    if (!block) return false;
    size_t n = 0;
    if (digits == at - 1 - first) {
        // No white space: the digits in pairs.
        const unsigned char* const p = reinterpret_cast<const unsigned char*>(data + first);
        for (size_t k = 0; k + 1 < digits; k += 2) {
            block[n++] = static_cast<char>(kScan.hex[p[k]] << 4 | kScan.hex[p[k + 1]]);
        }
        if (digits % 2 != 0) block[n++] = static_cast<char>(kScan.hex[p[digits - 1]] << 4);
        out = std::string_view(block, bytes);
        return true;
    }
    int high = -1;
    for (size_t i = first; i + 1 < at; ++i) {
        const int d = kScan.hex[static_cast<unsigned char>(data[i])];
        if (d >= 16) continue;  // white space
        if (high < 0) {
            high = d;
        } else {
            block[n++] = static_cast<char>(high << 4 | d);
            high = -1;
        }
    }
    if (high >= 0) block[n++] = static_cast<char>(high << 4);
    out = std::string_view(block, bytes);
    return true;
}

namespace {

// "n g R" after the Int `out` (all digits, at most UINT32_MAX): read ahead, the cursor and `out`
// moved to the reference only when it is one. The general form, for comments in between.
void ReadReferenceTokens(Cursor& c, PdfValue& out) {
    Cursor ahead = c;
    SkipSpace(ahead);
    const std::string_view generation = ReadToken(ahead);
    if (!AllDigits(generation) || generation.size() > 5) return;
    uint32_t gen = 0;
    for (char g : generation) gen = gen * 10 + static_cast<uint32_t>(g - '0');
    SkipSpace(ahead);
    if (gen > 65535 || ReadToken(ahead) != "R") return;
    const uint32_t num = static_cast<uint32_t>(out.integer);
    out = PdfValue();
    out.type = PdfValue::Type::Ref;
    out.refNum = num;
    out.refGen = static_cast<uint16_t>(gen);
    c = ahead;
}

// The same test byte by byte: white space, a token of at most five digits, white space, the
// token "R"; a '%' goes to the general form. Inline: every Int of an object outside an array
// runs it.
RAYOPDF_HOT_INLINE void ReadReference(Cursor& c, PdfValue& out) {
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    size_t at = c.pos;
    while (at < size && IsWhite(static_cast<unsigned char>(data[at]))) ++at;
    if (at < size && data[at] == '%') return ReadReferenceTokens(c, out);
    const size_t first = at;
    // At most six digits are read: a sixth means no generation.
    const size_t limit = size - first > 6 ? first + 6 : size;
    uint32_t gen = 0;
    for (unsigned d; at < limit && (d = static_cast<unsigned>(static_cast<unsigned char>(data[at])) - '0') < 10; ++at) {
        gen = gen * 10 + d;
    }
    // The token is all digits only if it ends here (a sixth digit or another regular byte: not).
    if (at == first || at - first > 5 || (at < size && IsRegular(static_cast<unsigned char>(data[at])))) return;
    if (gen > 65535) return;
    while (at < size && IsWhite(static_cast<unsigned char>(data[at]))) ++at;
    if (at < size && data[at] == '%') return ReadReferenceTokens(c, out);
    if (at >= size || data[at] != 'R' || (at + 1 < size && IsRegular(static_cast<unsigned char>(data[at + 1])))) return;
    const uint32_t num = static_cast<uint32_t>(out.integer);
    out = PdfValue();
    out.type = PdfValue::Type::Ref;
    out.refNum = num;
    out.refGen = static_cast<uint16_t>(gen);
    c.pos = at + 1;
}

// A name, read into `out`.
inline bool NameValue(Cursor& c, Builder& b, PdfValue& out) {
    std::string_view name;
    if (!ReadNameInline(c, b, name)) return false;
    out.type = PdfValue::Type::Name;
    out.count = static_cast<uint32_t>(name.size());
    out.data = name.data();
    return true;
}

bool ParseDict(Cursor& c, Mode mode, int depth, Builder& b, PdfValue& out);

// The array at the cursor ('[') at `depth`, as ParseValue reads it: read on the builder's stack
// above what the values around it hold there, then copied to the arena whole. Numbers, names,
// arrays and dictionaries are read here, every other item by ParseValue.
bool ParseArray(Cursor& c, Mode mode, int depth, Builder& b, PdfValue& out) {
    if (depth >= kMaxDepth) return false;
    c.pos++;
    const size_t base = b.items.size();
    bool ok = false;
    // In File mode "num gen R" is found at its R, from the two items before it: num a digits
    // token of at most UINT32_MAX and gen one of at most five digits and 65535, both read here
    // (a longer digits token goes through ParseValue, which tests for a reference itself). The
    // same references as a test at every number, without reading ahead.
    bool previousNum = false;  // the item before the last can be a reference's number
    bool lastNum = false;      // the last item can be one's number
    bool lastGen = false;      // the last item can be one's generation
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    for (;;) {
        size_t pos = c.pos;
        while (pos < size && IsWhite(static_cast<unsigned char>(data[pos]))) pos++;
        c.pos = pos;
        if (pos >= size) break;
        const unsigned char first = static_cast<unsigned char>(data[pos]);
        if (first == '%') {
            SkipSpace(c);
            continue;
        }
        if (first == ']') {
            c.pos++;
            ok = true;
            break;
        }
        if (first >= '0' && first <= '9') {
            // A token of at most 18 digits (most items: widths, rectangles, object numbers) is
            // the Int ReadSimpleNumber gives it, read here; any other number by it.
            uint64_t value = first - '0';
            size_t at = pos + 1;
            for (unsigned d; at < size && (d = static_cast<unsigned>(static_cast<unsigned char>(data[at])) - '0') < 10;
                 at++) {
                value = value * 10 + d;
            }
            if (at - pos <= 18 && (at >= size || !IsRegular(static_cast<unsigned char>(data[at])))) {
                c.pos = at;
                previousNum = lastNum;
                lastNum = value <= UINT32_MAX;
                lastGen = at - pos <= 5 && value <= 65535;
                PdfValue& item = b.items.emplace_back();
                item.type = PdfValue::Type::Int;
                item.integer = static_cast<int64_t>(value);
                continue;
            }
        } else if (first == 'R' && mode == Mode::File && previousNum && lastGen &&
                   (pos + 1 >= size || !IsRegular(static_cast<unsigned char>(data[pos + 1])))) {
            PdfValue* const pair = b.items.data() + b.items.size() - 2;
            const uint32_t num = static_cast<uint32_t>(pair[0].integer);
            const uint16_t gen = static_cast<uint16_t>(pair[1].integer);
            b.items.pop_back();
            pair[0] = PdfValue();
            pair[0].type = PdfValue::Type::Ref;
            pair[0].refNum = num;
            pair[0].refGen = gen;
            c.pos++;
            previousNum = lastNum = lastGen = false;
            continue;
        }
        // Anything else is no reference's generation, and only a digits token of 19 digits (an
        // Int from ReadSimpleNumber) can be its number.
        previousNum = lastNum;
        lastNum = lastGen = false;
        PdfValue item;
        if (first == '/') {
            if (!NameValue(c, b, item)) break;
        } else if (first == '[') {
            if (!ParseArray(c, mode, depth + 1, b, item)) break;
        } else if (first == '<' && c.Peek(1) == '<') {
            if (!ParseDict(c, mode, depth + 1, b, item)) break;
        } else if (StartsNumber(first) && ReadSimpleNumber(c, item)) {
            lastNum = first >= '0' && first <= '9' && item.type == PdfValue::Type::Int && item.integer <= UINT32_MAX;
        } else if (!ParseValue(c, mode, depth + 1, b, item)) {
            break;
        }
        b.items.push_back(item);
    }
    const size_t n = b.items.size() - base;
    PdfValue* items = nullptr;
    if (ok && n > 0) {
        items = n <= UINT32_MAX ? static_cast<PdfValue*>(b.arena.Allocate(n * sizeof(PdfValue))) : nullptr;
        if (items) std::memcpy(static_cast<void*>(items), b.items.data() + base, n * sizeof(PdfValue));
        ok = items != nullptr;
    }
    b.items.resize(base);
    if (!ok) return false;
    out = PdfValue();
    out.type = PdfValue::Type::Array;
    out.count = static_cast<uint32_t>(n);
    out.data = items;
    return true;
}

// The dictionary at the cursor ("<<") at `depth`, as ParseValue reads it, on the builder's
// stack like an array. Names, numbers, strings, arrays and dictionaries are read here, every
// other value by ParseValue.
bool ParseDict(Cursor& c, Mode mode, int depth, Builder& b, PdfValue& out) {
    if (depth >= kMaxDepth) return false;
    c.pos += 2;  // "<<"
    const size_t base = b.entries.size();
    bool ok = false;
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    // SkipSpace, its white space here.
    const auto skipSpace = [&]() {
        while (c.pos < size && IsWhite(static_cast<unsigned char>(data[c.pos]))) c.pos++;
        if (c.pos < size && data[c.pos] == '%') SkipSpace(c);
    };
    for (;;) {
        skipSpace();
        if (c.pos >= size) break;
        if (data[c.pos] == '>' && c.Peek(1) == '>') {
            c.pos += 2;
            ok = true;
            break;
        }
        if (data[c.pos] != '/') break;
        PdfDictEntry entry;
        if (!ReadNameInline(c, b, entry.key)) break;
        skipSpace();
        const unsigned char first = c.Peek();
        if (first == '>' && c.Peek(1) == '>') continue;  // a key without a value
        PdfValue& value = entry.value;
        bool read;
        if (first == '/') {
            read = NameValue(c, b, value);
        } else if (first >= '0' && first <= '9' && ReadSimpleNumber(c, value)) {
            if (mode == Mode::File && value.type == PdfValue::Type::Int && value.integer <= UINT32_MAX)
                ReadReference(c, value);
            read = true;
        } else if (first == '[') {
            read = ParseArray(c, mode, depth + 1, b, value);
        } else if (first == '<' && c.Peek(1) == '<') {
            read = ParseDict(c, mode, depth + 1, b, value);
        } else {
            read = ParseValue(c, mode, depth + 1, b, value);
        }
        if (!read) break;
        b.entries.push_back(entry);
    }
    const size_t n = b.entries.size() - base;
    PdfDictEntry* entries = nullptr;
    if (ok && n > 0) {
        entries = n <= UINT32_MAX ? static_cast<PdfDictEntry*>(b.arena.Allocate(n * sizeof(PdfDictEntry))) : nullptr;
        if (entries) std::memcpy(static_cast<void*>(entries), b.entries.data() + base, n * sizeof(PdfDictEntry));
        ok = entries != nullptr;
    }
    b.entries.resize(base);
    if (!ok) return false;
    out = PdfValue();
    out.type = PdfValue::Type::Dict;
    out.count = static_cast<uint32_t>(n);
    out.data = entries;
    return true;
}

} // namespace

bool ParseValue(Cursor& c, Mode mode, int depth, Builder& b, PdfValue& out) {
    if (c.AtEnd()) return false;
    const unsigned char ch = c.Peek();
    switch (ch) {
        case '/':
        case '(':
        case '<': {
            if (ch == '<' && c.Peek(1) == '<') break;
            std::string_view text;
            const bool read = ch == '/' ? ReadName(c, b, text)
                              : ch == '(' ? ReadLiteralString(c, b, text)
                                          : ReadHexString(c, b, text);
            if (!read) return false;
            out = PdfValue();
            out.type = ch == '/' ? PdfValue::Type::Name : PdfValue::Type::String;
            out.count = static_cast<uint32_t>(text.size());
            out.data = text.data();
            return true;
        }
        case '[':
            break;
        default: {
            if (!IsRegular(ch)) return false;
            // The common number at once; the reference test below as for any Int token (all
            // digits: no sign, and an Int has no point).
            if (StartsNumber(ch)) {
                PdfValue number;
                if (ReadSimpleNumber(c, number)) {
                    out = number;
                    if (mode == Mode::File && out.type == PdfValue::Type::Int && ch >= '0' && ch <= '9' &&
                        out.integer <= UINT32_MAX)
                        ReadReference(c, out);
                    return true;
                }
            }
            const std::string_view token = ReadToken(c);
            if (token == "true" || token == "false") {
                out = PdfValue();
                out.type = PdfValue::Type::Bool;
                out.boolean = token == "true";
                return true;
            }
            if (token == "null") {
                out = PdfValue();
                return true;
            }
            if (!ParseNumber(token, out)) return false;
            if (mode == Mode::File && out.type == PdfValue::Type::Int && AllDigits(token) && out.integer <= UINT32_MAX)
                ReadReference(c, out);
            return true;
        }
    }
    // An array or a dictionary.
    return ch == '[' ? ParseArray(c, mode, depth, b, out) : ParseDict(c, mode, depth, b, out);
}

} // namespace Syntax
} // namespace RayoPdf
