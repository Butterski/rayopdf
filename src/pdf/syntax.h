// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/syntax.h at 9fbbfaf (Apache-2.0, same author).
// PDF's lexical conventions (ISO 32000-1 section 7.2-7.3), shared by the object reader and the
// content-stream lexer. Every function stays within the cursor's data and consumes at least one
// byte per token, so a scan over any input ends.
#pragma once

#include "common/compiler.h"
#include "pdf/value.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Syntax {

// Arrays and dictionaries nest at most this deep; a deeper value fails.
constexpr int kMaxDepth = 64;

// The classes of the 256 bytes, one load per test: white space (NUL, HT, LF, FF, CR, space) and
// delimiters; the rest are regular characters.
struct ByteClasses {
    static constexpr uint8_t kWhite = 1;
    static constexpr uint8_t kDelimiter = 2;
    uint8_t of[256] = {};
    constexpr ByteClasses() {
        for (int c : {0, 9, 10, 12, 13, 32}) of[c] = kWhite;
        for (char c : {'(', ')', '<', '>', '[', ']', '{', '}', '/', '%'}) of[static_cast<unsigned char>(c)] = kDelimiter;
    }
};
inline constexpr ByteClasses kByteClasses;

inline bool IsWhite(unsigned char c) { return (kByteClasses.of[c] & ByteClasses::kWhite) != 0; }
inline bool IsDelimiter(unsigned char c) { return (kByteClasses.of[c] & ByteClasses::kDelimiter) != 0; }
inline bool IsRegular(unsigned char c) { return kByteClasses.of[c] == 0; }
// The first byte of a number: a digit, a sign or a decimal point.
inline bool StartsNumber(unsigned char c) { return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'; }
// The value of a hexadecimal digit, -1 for any other byte.
struct HexValues {
    int8_t of[256] = {};
    constexpr HexValues() {
        for (int c = 0; c < 256; ++c) of[c] = -1;
        for (int c = 0; c < 10; ++c) of['0' + c] = static_cast<int8_t>(c);
        for (int c = 0; c < 6; ++c) of['a' + c] = of['A' + c] = static_cast<int8_t>(10 + c);
    }
};
inline constexpr HexValues kHexValues;

struct Cursor {
    std::string_view data;
    size_t pos = 0;

    bool AtEnd() const { return pos >= data.size(); }
    // The byte `ahead` past the cursor, 0 past the end.
    unsigned char Peek(size_t ahead = 0) const {
        return pos + ahead < data.size() ? static_cast<unsigned char>(data[pos + ahead]) : 0;
    }
};

// Skips white space and % comments.
void SkipSpace(Cursor& c);
// Skips white space only.
void SkipWhite(Cursor& c);
// A run of regular characters at the cursor, possibly empty.
std::string_view ReadToken(Cursor& c);
// `token` as an Int or a Real: [+-]digits[.digits] or [+-].digits. An integer beyond int64 is a
// Real. False for anything else.
bool ParseNumber(std::string_view token, PdfValue& out);

// The common number at the cursor: [+-]digits[.digits] or [+-].digits with at most 19 digits,
// ending at a white-space or delimiter byte (or the end). Reads it into `value` (a default
// PdfValue) and moves the cursor past it, with exactly the value ParseNumber gives the token: up
// to 19 digits all count, so its mantissa, fraction count and Int/Real choice are the same, and
// 10^fraction comes from the same exact powers. False, with the cursor and `value` untouched, for
// anything else (longer numbers, a second point, letters): ReadToken and ParseNumber read those.
RAYOPDF_HOT_INLINE bool ReadSimpleNumber(Cursor& c, PdfValue& value) {
    static constexpr double kPowers[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                                         1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19};
    const char* p = c.data.data() + c.pos;
    const char* const end = c.data.data() + c.data.size();
    bool negative = false;
    if (p < end && (*p == '+' || *p == '-')) negative = *p++ == '-';
    // The integer digits, then after a point the fraction digits: two loops with one test a byte.
    // Past 19 digits the mantissa wraps, but such a number is left to the general path.
    uint64_t mantissa = 0;
    const char* const first = p;
    for (unsigned d; p < end && (d = static_cast<unsigned>(static_cast<unsigned char>(*p)) - '0') < 10; ++p) {
        mantissa = mantissa * 10 + d;
    }
    size_t digits = static_cast<size_t>(p - first);
    size_t fraction = 0;
    const bool point = p < end && *p == '.';
    if (point) {
        const char* const after = ++p;
        for (unsigned d; p < end && (d = static_cast<unsigned>(static_cast<unsigned char>(*p)) - '0') < 10; ++p) {
            mantissa = mantissa * 10 + d;
        }
        fraction = static_cast<size_t>(p - after);
        digits += fraction;
    }
    if (digits == 0 || digits > 19 || (p < end && IsRegular(static_cast<unsigned char>(*p)))) return false;
    if (!point && mantissa <= static_cast<uint64_t>(INT64_MAX)) {
        value.type = PdfValue::Type::Int;
        value.integer = negative ? -static_cast<int64_t>(mantissa) : static_cast<int64_t>(mantissa);
    } else {
        double real = static_cast<double>(mantissa);
        if (fraction > 0) real /= kPowers[fraction];
        value.type = PdfValue::Type::Real;
        value.real = negative ? -real : real;
    }
    c.pos = static_cast<size_t>(p - c.data.data());
    return true;
}

// Where ParseValue puts what it reads: strings, items and entries go to `arena`; arrays and
// dictionaries are read on the stacks (kept from value to value, so they stop allocating) and
// copied to the arena whole when they end. With `views`, a name without #xx escapes and a
// literal string without escapes, nested parentheses or line ends view the cursor's data instead
// of a copy: for readers whose data outlives the values they give.
struct Builder {
    explicit Builder(PdfArena& storage, bool viewData = false) : arena(storage), views(viewData) {}
    PdfArena& arena;
    bool views;
    std::vector<PdfValue> items;
    std::vector<PdfDictEntry> entries;
    std::string scratch;
};

// A name into the arena (or a view, Builder::views); the cursor is at '/'. False when the arena
// cannot hold it.
bool ReadName(Cursor& c, Builder& b, std::string_view& out);
// A literal string with its escapes and balanced parentheses (into the arena, or a view,
// Builder::views); the cursor is at '('. False when it does not end.
bool ReadLiteralString(Cursor& c, Builder& b, std::string_view& out);
// A hexadecimal string; the cursor is at '<' (not "<<"). False on a byte that is neither a hex
// digit nor white space, or when it does not end.
bool ReadHexString(Cursor& c, Builder& b, std::string_view& out);

enum class Mode : uint8_t {
    File,     // "n g R" is a reference
    Content,  // no references
};

// The value at the cursor (the caller skipped space before it): a number, true, false, null,
// a name, a string, an array or a dictionary, or in File mode a reference. A bare keyword, a
// stray delimiter or nesting deeper than kMaxDepth fails; the cursor is then left anywhere
// at or after where it started, and what it read so far stays in the arena unused.
bool ParseValue(Cursor& c, Mode mode, int depth, Builder& b, PdfValue& out);

} // namespace RayoPdf::Syntax
