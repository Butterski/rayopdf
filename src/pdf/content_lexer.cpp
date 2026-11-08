// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/content.cpp at 9fbbfaf (Apache-2.0, same author).
#include "pdf/content_lexer.h"

#include "common/compiler.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace RayoPdf {
namespace {

using Syntax::Cursor;

// The powers of ten Syntax::ReadSimpleNumber divides by.
constexpr double kPowers[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,
                              1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19};

// An inline image entry under its abbreviation or its full name.
const PdfValue* InlineEntry(const PdfValue& dict, std::string_view abbreviation, std::string_view name) {
    const PdfValue* entry = dict.Find(abbreviation);
    return entry ? entry : dict.Find(name);
}

// The byte size of an unfiltered inline image's data, when its dictionary tells it.
bool InlineImageSize(const PdfValue& dict, uint64_t& size) {
    const PdfValue* filter = InlineEntry(dict, "F", "Filter");
    if (filter && !filter->IsNull() && !(filter->IsArray() && filter->Array().empty())) return false;
    const PdfValue* widthEntry = InlineEntry(dict, "W", "Width");
    const PdfValue* heightEntry = InlineEntry(dict, "H", "Height");
    if (!widthEntry || !heightEntry) return false;
    const int64_t width = widthEntry->Int(-1);
    const int64_t height = heightEntry->Int(-1);
    if (width < 1 || height < 1 || width > (1 << 24) || height > (1 << 24)) return false;
    const PdfValue* maskEntry = InlineEntry(dict, "IM", "ImageMask");
    const bool mask = maskEntry && maskEntry->Bool();
    int64_t bits = 1;
    int64_t components = 1;
    if (!mask) {
        const PdfValue* bpcEntry = InlineEntry(dict, "BPC", "BitsPerComponent");
        bits = bpcEntry ? bpcEntry->Int(0) : 0;
        if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) return false;
        const PdfValue* space = InlineEntry(dict, "CS", "ColorSpace");
        if (!space) return false;
        const PdfValue* family = space->IsArray() && !space->Array().empty() ? &space->Array()[0] : space;
        const std::string_view name = family->Name();
        if (name == "G" || name == "DeviceGray" || name == "CalGray" || name == "I" || name == "Indexed") {
            components = 1;
        } else if (name == "RGB" || name == "DeviceRGB" || name == "CalRGB") {
            components = 3;
        } else if (name == "CMYK" || name == "DeviceCMYK") {
            components = 4;
        } else {
            return false;  // a named space of the resources: unknown here
        }
    }
    const uint64_t row = (static_cast<uint64_t>(width) * static_cast<uint64_t>(components) * static_cast<uint64_t>(bits) + 7) / 8;
    size = row * static_cast<uint64_t>(height);
    return true;
}

} // namespace

ContentLexer::ContentLexer(std::string_view data)
    : cursor_{data, 0}, values_(kFirstSlots), numbers_(kFirstSlots), builder_(arena_, true) {
    size_t at = data.size();
    while (at > 0 && Syntax::StartsNumber(static_cast<unsigned char>(data[at - 1]))) at--;
    numbersEnd_ = at > 0 ? at - 1 : 0;
}

bool ContentLexer::Next(std::string_view& op, std::vector<PdfValue>& operands) {
    const bool more = Next(op);
    operands.assign(values_.data(), values_.data() + count_);
    return more;
}

// Twice the slots, at most kMaxOperands + 1.
void ContentLexer::Grow() {
    const size_t slots = values_.size() * 2 < kMaxOperands + 1 ? values_.size() * 2 : kMaxOperands + 1;
    values_.resize(slots);
    numbers_.resize(slots);
}

// After an operand went into the slot `count` - 1: when that was the last slot, more slots, or
// past kMaxOperands only the newest operand kept, in the first.
RAYOPDF_HOT_INLINE void ContentLexer::Added(PdfValue*& values, double*& numbers, size_t& slots, size_t& count,
                                            size_t& numeric) {
    if (RAYOPDF_UNLIKELY(count == slots)) {
        if (slots > kMaxOperands) {
            values[0] = values[count - 1];
            numbers[0] = numbers[count - 1];
            count = 1;
            numeric = numeric > 0 ? 1 : 0;
        } else {
            Grow();
            values = values_.data();
            numbers = numbers_.data();
            slots = values_.size();
        }
    }
}

// `value` as the next operand.
RAYOPDF_HOT_INLINE void ContentLexer::Add(PdfValue*& values, double*& numbers, size_t& slots, size_t& count,
                                          size_t& numeric, const PdfValue& value) {
    const double number = value.Number();
    values[count] = value;
    numbers[count] = number;
    count++;
    numeric = value.IsNumber() && std::isfinite(number) ? numeric + 1 : 0;
    Added(values, numbers, slots, count, numeric);
}

bool ContentLexer::Next(std::string_view& op) {
    inlineData_ = {};
    arena_.Reset();
    // The cursor and the operand count in locals while reading; the members get them back before
    // a call that reads them and at the end.
    PdfValue* values = values_.data();
    double* numbers = numbers_.data();
    size_t slots = values_.size();
    const char* const data = cursor_.data.data();
    const char* const end = data + cursor_.data.size();
    const char* const numbersEnd = data + numbersEnd_;
    const char* p = data + cursor_.pos;
    size_t count = 0;
    size_t numeric = 0;
    for (;;) {
        while (p < end && Syntax::IsWhite(static_cast<unsigned char>(*p))) p++;
        if (p >= end) break;
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (Syntax::IsRegular(ch)) {
            if (Syntax::StartsNumber(ch)) {
                if (RAYOPDF_LIKELY(p < numbersEnd)) {
                    // Syntax::ReadSimpleNumber without the end tests: the byte at numbersEnd stops
                    // every scan before the end of the data. The same steps give the same value.
                    const char* q = p;
                    bool negative = false;
                    if (ch == '+' || ch == '-') negative = *q++ == '-';
                    uint64_t mantissa = 0;
                    const char* const first = q;
                    for (unsigned d; (d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0') < 10; ++q) {
                        mantissa = mantissa * 10 + d;
                    }
                    size_t digits = static_cast<size_t>(q - first);
                    size_t fraction = 0;
                    const bool point = *q == '.';
                    if (point) {
                        const char* const after = ++q;
                        for (unsigned d; (d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0') < 10; ++q) {
                            mantissa = mantissa * 10 + d;
                        }
                        fraction = static_cast<size_t>(q - after);
                        digits += fraction;
                    }
                    if (digits - 1 < 19 && !Syntax::IsRegular(static_cast<unsigned char>(*q))) {
                        // A fresh value's fields with the number: Int and short Real are finite.
                        PdfValue& value = values[count];
                        value.boolean = false;
                        value.refGen = 0;
                        value.count = 0;
                        if (!point && mantissa <= static_cast<uint64_t>(INT64_MAX)) {
                            const int64_t integer = negative ? -static_cast<int64_t>(mantissa) : static_cast<int64_t>(mantissa);
                            value.type = PdfValue::Type::Int;
                            value.integer = integer;
                            numbers[count] = static_cast<double>(integer);
                        } else {
                            double real = static_cast<double>(mantissa);
                            if (fraction > 0) real /= kPowers[fraction];
                            real = negative ? -real : real;
                            value.type = PdfValue::Type::Real;
                            value.real = real;
                            numbers[count] = real;
                        }
                        count++;
                        numeric++;
                        Added(values, numbers, slots, count, numeric);
                        p = q;
                        continue;
                    }
                } else {
                    Cursor c{cursor_.data, static_cast<size_t>(p - data)};
                    PdfValue value;
                    if (Syntax::ReadSimpleNumber(c, value)) {
                        Add(values, numbers, slots, count, numeric, value);
                        p = data + c.pos;
                        continue;
                    }
                }
            }
            const char* const start = p;
            while (p < end && Syntax::IsRegular(static_cast<unsigned char>(*p))) p++;
            const std::string_view token(start, static_cast<size_t>(p - start));
            // The operands among the tokens: true, false, null and the numbers left above; the
            // first byte tells which one a token can be.
            switch (ch) {
            case 't':
            case 'f':
                if (token == "true" || token == "false") {
                    PdfValue value;
                    value.type = PdfValue::Type::Bool;
                    value.boolean = ch == 't';
                    Add(values, numbers, slots, count, numeric, value);
                    continue;
                }
                break;
            case 'n':
                if (token == "null") {
                    Add(values, numbers, slots, count, numeric, PdfValue());
                    continue;
                }
                break;
            default:
                if (Syntax::StartsNumber(ch)) {
                    PdfValue value;
                    if (Syntax::ParseNumber(token, value)) {
                        Add(values, numbers, slots, count, numeric, value);
                        continue;
                    }
                }
                break;
            }
            cursor_.pos = static_cast<size_t>(p - data);
            count_ = count;
            numeric_ = numeric;
            op = token;
            if (token == "BI") ReadInlineImage();
            return true;
        }
        if (ch == '%') {  // a comment, rare in content streams
            cursor_.pos = static_cast<size_t>(p - data);
            Syntax::SkipSpace(cursor_);
            p = data + cursor_.pos;
            continue;
        }
        if (ch == '/' || ch == '(' || ch == '<' || ch == '[') {
            const size_t start = static_cast<size_t>(p - data);
            cursor_.pos = start;
            PdfValue value;
            if (Syntax::ParseValue(cursor_, Syntax::Mode::Content, 0, builder_, value)) {
                Add(values, numbers, slots, count, numeric, value);
            } else {
                if (cursor_.pos == start) cursor_.pos++;
                count = 0;
                numeric = 0;
            }
            p = data + cursor_.pos;
            continue;
        }
        p++;  // a stray ')', ']', '>', '{' or '}'
    }
    cursor_.pos = static_cast<size_t>(p - data);
    count_ = count;
    numeric_ = numeric;
    return false;
}

void ContentLexer::ReadInlineImage() {
    Cursor& c = cursor_;
    count_ = 0;
    numeric_ = 0;
    std::vector<PdfDictEntry>& entries = builder_.entries;
    entries.clear();
    std::string_view key;
    bool hasKey = false;
    bool data = false;
    for (;;) {
        Syntax::SkipSpace(c);
        if (c.AtEnd()) break;
        const unsigned char ch = c.Peek();
        if (Syntax::IsRegular(ch)) {
            Cursor ahead = c;
            const std::string_view token = Syntax::ReadToken(ahead);
            if (token == "ID" || token == "EI") {
                c = ahead;
                data = token == "ID";
                break;
            }
        }
        if (ch == '/' && !hasKey) {
            hasKey = Syntax::ReadName(c, builder_, key);
            continue;
        }
        const size_t start = c.pos;
        PdfValue value;
        if (Syntax::ParseValue(c, Syntax::Mode::Content, 1, builder_, value)) {
            if (hasKey) entries.push_back({key, value});
        } else if (c.pos == start) {
            c.pos++;
        }
        hasKey = false;
    }
    PdfValue dict;
    dict.type = PdfValue::Type::Dict;
    if (!entries.empty()) {
        void* block = arena_.Allocate(entries.size() * sizeof(PdfDictEntry));
        if (block) {
            std::memcpy(block, entries.data(), entries.size() * sizeof(PdfDictEntry));
            dict.count = static_cast<uint32_t>(entries.size());
            dict.data = block;
        }
    }
    entries.clear();
    values_[0] = dict;
    count_ = 1;
    if (!data) return;

    // One white-space byte separates "ID" from the data.
    const std::string_view bytes = c.data;
    // Peek() gives 0, a white-space byte, past the end: check the end first.
    if (!c.AtEnd() && Syntax::IsWhite(c.Peek())) c.pos++;
    const size_t start = c.pos;
    size_t end = std::string_view::npos;
    size_t resume = bytes.size();
    // With the size known from the dictionary, "EI" must follow the data.
    uint64_t size = 0;
    if (InlineImageSize(values_[0], size) && size <= bytes.size() - start) {
        Cursor after{bytes, start + static_cast<size_t>(size)};
        Syntax::SkipWhite(after);
        if (after.Peek() == 'E' && after.Peek(1) == 'I' && !Syntax::IsRegular(after.Peek(2))) {
            end = start + static_cast<size_t>(size);
            resume = after.pos + 2;
        }
    }
    // Otherwise the first "EI" that stands alone between white space (or the end).
    if (end == std::string_view::npos) {
        for (size_t at = bytes.find("EI", start); at != std::string_view::npos; at = bytes.find("EI", at + 1)) {
            const bool before = at == start || Syntax::IsWhite(static_cast<unsigned char>(bytes[at - 1]));
            const bool after = at + 2 >= bytes.size() || !Syntax::IsRegular(static_cast<unsigned char>(bytes[at + 2]));
            if (before && after) {
                end = at > start ? at - 1 : at;
                resume = at + 2;
                break;
            }
        }
    }
    if (end == std::string_view::npos) end = bytes.size();
    inlineData_ = bytes.substr(start, end - start);
    c.pos = resume;
}

} // namespace RayoPdf
