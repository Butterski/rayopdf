// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/content.h at 9fbbfaf (Apache-2.0, same author).
// The content-stream lexer (ISO 32000-1 section 7.8.2): operators with their operands, for the
// interpreter. Malformed input never stops it: a broken operand is dropped together
// with the operands before it, stray delimiters are skipped, and every step consumes input, so
// a stream of any bytes is read to its end in linear time.
#pragma once

#include "common/compiler.h"
#include "pdf/syntax.h"
#include "pdf/value.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace RayoPdf {

// Syntax::ByteClasses with digits and the other bytes that can start a number (signs and the
// point) in classes of their own: one load tells white space, a delimiter, a digit, a sign
// or point, or another regular byte (0).
struct ContentClasses {
    static constexpr uint8_t kWhite = Syntax::ByteClasses::kWhite;
    static constexpr uint8_t kDelimiter = Syntax::ByteClasses::kDelimiter;
    static constexpr uint8_t kNotRegular = kWhite | kDelimiter;
    static constexpr uint8_t kDigit = 4;
    static constexpr uint8_t kSign = 8;  // '+', '-' or '.'
    uint8_t of[256] = {};
    constexpr ContentClasses() {
        for (int c = 0; c < 256; ++c) of[c] = Syntax::kByteClasses.of[c];
        for (int c = '0'; c <= '9'; ++c) of[c] = kDigit;
        of['+'] = of['-'] = of['.'] = kSign;
    }
};
inline constexpr ContentClasses kContentClasses;

class ContentLexer {
public:
    // At most this many operands are kept for one operator; more drop all before them.
    static constexpr size_t kMaxOperands = 256;

    explicit ContentLexer(std::string_view data);
    ContentLexer(const ContentLexer&) = delete;
    ContentLexer& operator=(const ContentLexer&) = delete;

    // The next operator; Operands() and Numbers() give the operands before it (numbers, names,
    // strings, arrays, dictionaries, booleans, null; no references). False at the end of the
    // data; operands left without an operator there are dropped.
    //
    // An inline image comes as the operator "BI" with one operand, its dictionary (keys as
    // written, abbreviations included), and InlineImageData() holding the bytes between "ID"
    // and "EI"; the lexer continues after "EI". `op`, InlineImageData() and the operands' names and
    // strings that need no decoding point into the data; the operands themselves, the other
    // strings and the items and entries live in the lexer until its next call of Next.
    bool Next(std::string_view& op);
    // Next with the operands copied into `operands`.
    bool Next(std::string_view& op, std::vector<PdfValue>& operands);
    // Calls `onOperator(op)` for every operator Next would return, in order, until it returns
    // false or the data ends: Next's loop inlined into the caller's.
    template <class OnOperator>
    RAYOPDF_HOT_INLINE void Run(OnOperator&& onOperator) {
        std::string_view op;
        while (Scan(op)) {
            if (!onOperator(op)) return;
        }
    }

    // The operands of the operator Next just returned.
    PdfSpan<PdfValue> Operands() const { return PdfSpan<PdfValue>(values_.data(), count_); }
    // The last `n` operands as doubles (PdfValue::Number) when there are `n` or more and each of
    // the last `n` is a finite number; nullptr otherwise.
    const double* Numbers(size_t n) const { return n <= numeric_ ? numbers_.data() + count_ - n : nullptr; }

    // The data of the inline image Next just returned, empty after any other operator.
    std::string_view InlineImageData() const { return inlineData_; }
    // Bytes of the data read so far.
    size_t Offset() const { return cursor_.pos; }
    // Continues at `offset` (at most the data's size), an Offset() between two operators.
    void Seek(size_t offset) {
        cursor_.pos = offset < cursor_.data.size() ? offset : cursor_.data.size();
        inlineData_ = {};
    }

private:
    // Operand slots to start with; they double as needed up to kMaxOperands + 1.
    static constexpr size_t kFirstSlots = 8;

    using Classes = ContentClasses;

    bool Scan(std::string_view& op);
    template <bool kNegative>
    static bool ReadDigits(const char*& p, const char* digits, PdfValue* value, double* number);
    // Scan's slow steps, out of line so its loop keeps its registers. Each takes the cursor at
    // cursor_.pos and the operand count and numeric run in count_ and numeric_, and leaves them
    // there.
    RAYOPDF_NOINLINE void Overflow();
    RAYOPDF_NOINLINE void Delimiter();
    RAYOPDF_NOINLINE bool Token(const char* start, const char* end, std::string_view& op);
    void Push(const PdfValue& value);
    // An operand that starts with '/', '(', '<' or '[' (ReadValue) and its array case (ReadArray):
    // Syntax::ParseValue's values in Content mode at depth 0. `numbersEnd` is numbersEnd_.
    RAYOPDF_NOINLINE static bool ReadValue(Syntax::Cursor& c, size_t numbersEnd, Syntax::Builder& b, PdfValue& out);
    static bool ReadArray(Syntax::Cursor& c, size_t numbersEnd, Syntax::Builder& b, PdfValue& out);
    void ReadInlineImage();

    Syntax::Cursor cursor_;
    // Scan reads bytes before this one without end checks: the data's last white-space or
    // delimiter byte (its start when there is none), where every run of regular bytes stops.
    const char* fast_ = nullptr;
    // Numbers starting before this offset end before it: the byte there is no digit, sign or point.
    size_t numbersEnd_ = 0;
    // The operands, and beside each number its value as a double.
    std::vector<PdfValue> values_;
    std::vector<double> numbers_;
    PdfValue* slotsEnd_ = nullptr;  // the end of values_
    size_t count_ = 0;    // operands
    size_t numeric_ = 0;  // finite numbers at the end of the operands
    std::string_view inlineData_;
    PdfArena arena_;
    Syntax::Builder builder_;
};

// A number of at most 18 digits whose first digit is at `digits` (after the '-' at `p` when
// kNegative), before the data's last white-space or delimiter byte: ReadSimpleNumber's two loops
// and steps, so the same value. Writes it to `value` (whose stream fields are 0, an operand slot's)
// and `number` and moves `p` past it and one white-space byte after it; false, with nothing
// changed, for a longer number or one that a regular byte ends.
template <bool kNegative>
RAYOPDF_HOT_INLINE bool ContentLexer::ReadDigits(const char*& p, const char* digits, PdfValue* value, double* number) {
    // The powers of ten Syntax::ReadSimpleNumber divides by.
    static constexpr double kPowers[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                                         1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19};
    // A byte less '0': at most 9 for a digit, kPoint for the point.
    constexpr unsigned kPoint = static_cast<unsigned>('.' - '0');
    const char* q = digits + 1;
    uint64_t mantissa = static_cast<unsigned>(static_cast<unsigned char>(*digits)) - '0';
    unsigned d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0';
    while (d < 10) {
        mantissa = mantissa * 10 + d;
        d = static_cast<unsigned>(static_cast<unsigned char>(*++q)) - '0';
    }
    if (d != kPoint) {
        const uint8_t next = kContentClasses.of[static_cast<unsigned char>(*q)];
        if (RAYOPDF_UNLIKELY(q - digits > 18 || !(next & ContentClasses::kNotRegular))) return false;
        const int64_t magnitude = static_cast<int64_t>(mantissa);
        const int64_t integer = kNegative ? -magnitude : magnitude;
        value->type = PdfValue::Type::Int;
        value->boolean = false;
        value->refGen = 0;
        value->count = 0;
        value->integer = integer;
        *number = static_cast<double>(integer);
        p = q + (next & ContentClasses::kWhite);
        return true;
    }
    const char* const fraction = ++q;
    d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0';
    while (d < 10) {
        mantissa = mantissa * 10 + d;
        d = static_cast<unsigned>(static_cast<unsigned char>(*++q)) - '0';
    }
    const uint8_t next = kContentClasses.of[static_cast<unsigned char>(*q)];
    // At most 18 digits besides the point.
    if (RAYOPDF_UNLIKELY(q - digits > 19 || !(next & ContentClasses::kNotRegular))) return false;
    double real = static_cast<double>(static_cast<int64_t>(mantissa)) / kPowers[q - fraction];
    if (kNegative) real = -real;
    value->type = PdfValue::Type::Real;
    value->boolean = false;
    value->refGen = 0;
    value->count = 0;
    value->real = real;
    *number = real;
    p = q + (next & ContentClasses::kWhite);
    return true;
}

// The common operators' loop: numbers of at most 18 digits read here (in Syntax::ReadSimpleNumber's
// steps, so to the same values), regular tokens found here, the rest by the out-of-line steps.
// The cursor, the next operand slot and the start of the numeric run stay in locals; the data's
// start and end are read from the members where needed, so they hold no registers.
RAYOPDF_HOT_INLINE bool ContentLexer::Scan(std::string_view& op) {
    // The powers of ten Syntax::ReadSimpleNumber divides by.
    static constexpr double kPowers[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                                         1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19};
    // The value of a digit byte less '0': at most 9 for a digit, kPointDigit for the point.
    constexpr unsigned kPointDigit = static_cast<unsigned>('.' - '0');
    inlineData_ = {};
    arena_.Reset();
    const char* p = cursor_.data.data() + cursor_.pos;
    PdfValue* value = values_.data();  // the next operand's slot
    double* number = numbers_.data();
    PdfValue* run = value;  // the first slot of the finite numbers at the end
    for (;;) {
        unsigned char c;
        if (RAYOPDF_LIKELY(p < fast_)) {
            // Every scan from here stops at the byte at fast_ at the latest.
            c = static_cast<unsigned char>(*p);
            const uint8_t kind = kContentClasses.of[c];
            if (kind == Classes::kDigit) {
                if (RAYOPDF_LIKELY(ReadDigits<false>(p, p, value, number))) goto pushed;
                goto token;
            }
            if (kind == Classes::kWhite) {
                ++p;
                continue;
            }
            if (kind == Classes::kSign) {
                if (c == '-' && kContentClasses.of[static_cast<unsigned char>(p[1])] == Classes::kDigit) {
                    if (RAYOPDF_LIKELY(ReadDigits<true>(p, p + 1, value, number))) goto pushed;
                    goto token;
                }
                // ReadSimpleNumber with a sign or a leading point, without its end tests.
                const bool negative = c == '-';
                const char* q = p + (c != '.');
                const char* const first = q;
                uint64_t mantissa = 0;
                unsigned d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0';
                while (d < 10) {
                    mantissa = mantissa * 10 + d;
                    d = static_cast<unsigned>(static_cast<unsigned char>(*++q)) - '0';
                }
                size_t digits = static_cast<size_t>(q - first);
                size_t fraction = 0;
                const bool point = d == kPointDigit;
                if (point) {
                    const char* const after = ++q;
                    d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0';
                    while (d < 10) {
                        mantissa = mantissa * 10 + d;
                        d = static_cast<unsigned>(static_cast<unsigned char>(*++q)) - '0';
                    }
                    fraction = static_cast<size_t>(q - after);
                    digits += fraction;
                }
                const uint8_t next = kContentClasses.of[static_cast<unsigned char>(*q)];
                if (RAYOPDF_UNLIKELY(digits - 1 >= 18 || !(next & Classes::kNotRegular))) goto token;
                const int64_t magnitude = static_cast<int64_t>(mantissa);
                value->boolean = false;
                value->refGen = 0;
                value->count = 0;
                if (!point) {
                    const int64_t integer = negative ? -magnitude : magnitude;
                    value->type = PdfValue::Type::Int;
                    value->integer = integer;
                    *number = static_cast<double>(integer);
                } else {
                    double real = static_cast<double>(magnitude) / kPowers[fraction];
                    real = negative ? -real : real;
                    value->type = PdfValue::Type::Real;
                    value->real = real;
                    *number = real;
                }
                p = q + (next & Classes::kWhite);
                goto pushed;
            }
            if (kind == 0) goto token;
        } else {
            // The data's last token: tests against its end.
            const char* const data = cursor_.data.data();
            if (p >= data + cursor_.data.size()) break;
            c = static_cast<unsigned char>(*p);
            const uint8_t kind = kContentClasses.of[c];
            if (kind == Classes::kWhite) {
                ++p;
                continue;
            }
            if (kind & (Classes::kDigit | Classes::kSign)) {
                Syntax::Cursor at{cursor_.data, static_cast<size_t>(p - data)};
                PdfValue read;
                if (Syntax::ReadSimpleNumber(at, read)) {
                    const double n = read.Number();
                    *value = read;
                    *number = n;
                    p = data + at.pos;
                    goto pushed;
                }
                goto token;
            }
            if (kind == 0) goto token;
        }
        // A delimiter: a comment, a name, string, array or dictionary, or a stray byte.
        cursor_.pos = static_cast<size_t>(p - cursor_.data.data());
        count_ = static_cast<size_t>(value - values_.data());
        numeric_ = static_cast<size_t>(value - run);
        Delimiter();
        p = cursor_.data.data() + cursor_.pos;
        value = values_.data() + count_;
        number = numbers_.data() + count_;
        run = value - numeric_;
        continue;
    pushed:
        // A finite number went into the slot at `value`.
        ++value;
        ++number;
        if (RAYOPDF_UNLIKELY(value == slotsEnd_)) {
            count_ = static_cast<size_t>(value - values_.data());
            numeric_ = static_cast<size_t>(value - run);
            Overflow();
            value = values_.data() + count_;
            number = numbers_.data() + count_;
            run = value - numeric_;
        }
        continue;
    token:
        // A regular token at p: an operator, or true, false, null or a number of another form.
        count_ = static_cast<size_t>(value - values_.data());
        numeric_ = static_cast<size_t>(value - run);
        {
            const char* q = p + 1;
            if (p < fast_) {
                while (!(kContentClasses.of[static_cast<unsigned char>(*q)] & Classes::kNotRegular)) ++q;
            } else {
                const char* const end = cursor_.data.data() + cursor_.data.size();
                while (q < end && !(kContentClasses.of[static_cast<unsigned char>(*q)] & Classes::kNotRegular)) ++q;
            }
            // The operators start with letters or quotes: most need no test of what they are.
            if (RAYOPDF_LIKELY(c != 't' && c != 'f' && c != 'n' &&
                               !(kContentClasses.of[c] & (Classes::kDigit | Classes::kSign)))) {
                cursor_.pos = static_cast<size_t>(q - cursor_.data.data());
                op = std::string_view(p, static_cast<size_t>(q - p));
                if (RAYOPDF_UNLIKELY(q - p == 2 && c == 'B' && p[1] == 'I')) ReadInlineImage();
                return true;
            }
            if (Token(p, q, op)) return true;
        }
        p = cursor_.data.data() + cursor_.pos;
        value = values_.data() + count_;
        number = numbers_.data() + count_;
        run = value - numeric_;
    }
    cursor_.pos = static_cast<size_t>(p - cursor_.data.data());
    count_ = static_cast<size_t>(value - values_.data());
    numeric_ = static_cast<size_t>(value - run);
    return false;
}

} // namespace RayoPdf
