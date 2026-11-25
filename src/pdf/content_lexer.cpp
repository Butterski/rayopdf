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

using Classes = ContentClasses;
constexpr const ContentClasses& kClasses = kContentClasses;

// Syntax::ReadSimpleNumber at `p`, a digit, sign or point before the lexer's numbersEnd_, for at
// most 18 digits and without its end tests: the byte at numbersEnd_ (no digit, sign or point)
// stops every scan before the end of the data. The same steps give the same value (below 10^18 the
// mantissa is an Int, and a fraction of no digits divides by 1). Makes `value`, whose stream
// fields are 0 (an operand slot's, a fresh value's), the number, sets `number` to it as a double
// (finite) and moves `p` past the number and one white-space byte after it; false, with nothing
// changed, where ReadSimpleNumber fails or reads 19 digits (the token's ParseNumber then gives the
// same value).
RAYOPDF_HOT_INLINE bool ReadShortNumber(const char*& p, PdfValue& value, double& number) {
    const char* q = p;
    bool negative = false;
    if (*q == '+' || *q == '-') negative = *q++ == '-';
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
    const uint8_t next = kClasses.of[static_cast<unsigned char>(*q)];
    if (digits - 1 >= 18 || !(next & Classes::kNotRegular)) return false;
    const int64_t magnitude = static_cast<int64_t>(mantissa);
    // The fields of a fresh value besides the stream's, stored together.
    value.type = point ? PdfValue::Type::Real : PdfValue::Type::Int;
    value.boolean = false;
    value.refGen = 0;
    value.count = 0;
    if (!point) {
        const int64_t integer = negative ? -magnitude : magnitude;
        value.integer = integer;
        number = static_cast<double>(integer);
    } else {
        double real = static_cast<double>(magnitude) / kPowers[fraction];
        real = negative ? -real : real;
        value.real = real;
        number = real;
    }
    p = q + (next & Classes::kWhite);
    return true;
}

// The bytes Syntax::ReadLiteralString looks at in a string: '\\', '(', ')' and '\r'.
struct LiteralStops {
    bool of[256] = {};
    constexpr LiteralStops() {
        of[static_cast<unsigned char>('\\')] = of[static_cast<unsigned char>('(')] = true;
        of[static_cast<unsigned char>(')')] = of[static_cast<unsigned char>('\r')] = true;
    }
};
constexpr LiteralStops kLiteralStops;

// A name, a literal string or a hexadecimal string at the cursor (at '/', '(' or a '<' not
// followed by '<'): Syntax::ParseValue's own case for them.
bool ReadText(Cursor& c, Syntax::Builder& b, unsigned char ch, PdfValue& out) {
    std::string_view text;
    const bool read = ch == '/'   ? Syntax::ReadName(c, b, text)
                      : ch == '(' ? Syntax::ReadLiteralString(c, b, text)
                                  : Syntax::ReadHexString(c, b, text);
    if (!read) return false;
    out = PdfValue();
    out.type = ch == '/' ? PdfValue::Type::Name : PdfValue::Type::String;
    out.count = static_cast<uint32_t>(text.size());
    out.data = text.data();
    return true;
}

} // namespace

// The array at the cursor ('['), as Syntax::ParseValue reads it in Content mode at depth 0 (a TJ
// array, most often): the same items read the same way, with the cursor left where ParseValue
// leaves it; short numbers (in the operand loop's steps), names and strings here, every other item
// by ParseValue at depth 1. `numbersEnd` is the lexer's numbersEnd_.
bool ContentLexer::ReadArray(Cursor& c, size_t numbersEnd, Syntax::Builder& b, PdfValue& out) {
    const char* const data = c.data.data();
    const char* const end = data + c.data.size();
    const char* const shortEnd = data + numbersEnd;
    std::vector<PdfValue>& items = b.items;
    const size_t base = items.size();
    bool ok = false;
    const char* p = data + c.pos + 1;
    for (;;) {
        while (p < end && kClasses.of[static_cast<unsigned char>(*p)] == Classes::kWhite) p++;
        if (p >= end) break;
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch == ']') {
            p++;
            ok = true;
            break;
        }
        const uint8_t kind = kClasses.of[ch];
        if ((kind & (Classes::kDigit | Classes::kSign)) && p < shortEnd) {
            // Read in the item's slot; the byte at numbersEnd stops every scan before the end.
            PdfValue& item = items.emplace_back();
            double number;
            const bool read = kind == Classes::kDigit ? ReadDigits<false>(p, p, &item, &number)
                              : ch == '-' && kClasses.of[static_cast<unsigned char>(p[1])] == Classes::kDigit
                                  ? ReadDigits<true>(p, p + 1, &item, &number)
                                  : ReadShortNumber(p, item, number);
            if (read) continue;
            items.pop_back();
        }
        if (ch == '(' && b.views) {
            // A string whose first byte that needs a look is its closing ')' (most): read here,
            // as ReadLiteralString reads it when the builder views the data.
            const char* q = p + 1;
            while (q < end && !kLiteralStops.of[static_cast<unsigned char>(*q)]) q++;
            if (q < end && *q == ')' && static_cast<size_t>(q - p - 1) <= UINT32_MAX) {
                PdfValue& item = items.emplace_back();
                item.type = PdfValue::Type::String;
                item.count = static_cast<uint32_t>(q - p - 1);
                item.data = q > p + 1 ? p + 1 : nullptr;
                p = q + 1;
                continue;
            }
        }
        c.pos = static_cast<size_t>(p - data);
        if (ch == '%') {
            Syntax::SkipSpace(c);
            p = data + c.pos;
            continue;
        }
        PdfValue item;
        const bool read = ch == '/' || ch == '(' || (ch == '<' && !(p + 1 < end && p[1] == '<'))
                              ? ReadText(c, b, ch, item)
                              : Syntax::ParseValue(c, Syntax::Mode::Content, 1, b, item);
        p = data + c.pos;
        if (!read) break;
        items.push_back(item);
    }
    c.pos = static_cast<size_t>(p - data);
    const size_t n = items.size() - base;
    PdfValue* stored = nullptr;
    if (ok && n > 0) {
        stored = n <= UINT32_MAX ? static_cast<PdfValue*>(b.arena.Allocate(n * sizeof(PdfValue))) : nullptr;
        if (stored) std::memcpy(static_cast<void*>(stored), items.data() + base, n * sizeof(PdfValue));
        ok = stored != nullptr;
    }
    items.resize(base);
    if (!ok) return false;
    out = PdfValue();
    out.type = PdfValue::Type::Array;
    out.count = static_cast<uint32_t>(n);
    out.data = stored;
    return true;
}

// The value at the cursor ('/', '(', '<' or '['), as Syntax::ParseValue reads it in Content mode at
// depth 0: arrays, names and strings here, dictionaries by ParseValue. Out of line, so the
// lexer's loop around it keeps its registers.
bool ContentLexer::ReadValue(Cursor& c, size_t numbersEnd, Syntax::Builder& b, PdfValue& out) {
    const unsigned char ch = c.Peek();
    if (ch == '[') return ReadArray(c, numbersEnd, b, out);
    if (ch == '<' && c.Peek(1) == '<') return Syntax::ParseValue(c, Syntax::Mode::Content, 0, b, out);
    return ReadText(c, b, ch, out);
}

namespace {

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
    while (at > 0 && Syntax::IsRegular(static_cast<unsigned char>(data[at - 1]))) at--;
    fast_ = data.data() + (at > 0 ? at - 1 : 0);
    slotsEnd_ = values_.data() + values_.size();
}

bool ContentLexer::Next(std::string_view& op) { return Scan(op); }

bool ContentLexer::Next(std::string_view& op, std::vector<PdfValue>& operands) {
    const bool more = Scan(op);
    operands.assign(values_.data(), values_.data() + count_);
    return more;
}

// The slots are full (count_ of them): twice the slots, at most kMaxOperands + 1, or past
// kMaxOperands only the newest operand kept, in the first.
void ContentLexer::Overflow() {
    if (count_ > kMaxOperands) {
        values_[0] = values_[count_ - 1];
        numbers_[0] = numbers_[count_ - 1];
        count_ = 1;
        numeric_ = numeric_ > 0 ? 1 : 0;
        return;
    }
    const size_t slots = values_.size() * 2 < kMaxOperands + 1 ? values_.size() * 2 : kMaxOperands + 1;
    values_.resize(slots);
    numbers_.resize(slots);
    slotsEnd_ = values_.data() + values_.size();
}

// `value` as the next operand.
void ContentLexer::Push(const PdfValue& value) {
    const double number = value.Number();
    values_[count_] = value;
    numbers_[count_] = number;
    count_++;
    numeric_ = value.IsNumber() && std::isfinite(number) ? numeric_ + 1 : 0;
    if (count_ == values_.size()) Overflow();
}

// At a delimiter: a comment, a name, a string, an array or a dictionary (an operand), or a stray
// ')', ']', '>', '{' or '}'.
void ContentLexer::Delimiter() {
    const unsigned char ch = cursor_.Peek();
    if (ch == '%') {  // a comment, rare in content streams
        Syntax::SkipSpace(cursor_);
        return;
    }
    if (ch == '/' || ch == '(' || ch == '<' || ch == '[') {
        const size_t start = cursor_.pos;
        PdfValue value;
        if (ReadValue(cursor_, numbersEnd_, builder_, value)) {
            Push(value);
        } else {
            if (cursor_.pos == start) cursor_.pos++;
            count_ = 0;
            numeric_ = 0;
        }
        return;
    }
    cursor_.pos++;
}

// The regular token [start, end) that starts with 't', 'f', 'n', a digit, a sign or a point:
// true, false, null or a number becomes the next operand; anything else is the operator `op`.
bool ContentLexer::Token(const char* start, const char* end, std::string_view& op) {
    const std::string_view token(start, static_cast<size_t>(end - start));
    cursor_.pos = static_cast<size_t>(end - cursor_.data.data());
    const unsigned char ch = static_cast<unsigned char>(*start);
    switch (ch) {
    case 't':
    case 'f':
        if (token == "true" || token == "false") {
            PdfValue value;
            value.type = PdfValue::Type::Bool;
            value.boolean = ch == 't';
            Push(value);
            return false;
        }
        break;
    case 'n':
        if (token == "null") {
            Push(PdfValue());
            return false;
        }
        break;
    default:
        if (Syntax::StartsNumber(ch)) {
            PdfValue value;
            if (Syntax::ParseNumber(token, value)) {
                Push(value);
                return false;
            }
        }
        break;
    }
    // The one white-space byte after the operator is read with it, as in Scan.
    if (cursor_.pos < cursor_.data.size() &&
        kClasses.of[static_cast<unsigned char>(cursor_.data[cursor_.pos])] == Classes::kWhite)
        cursor_.pos++;
    op = token;
    return true;
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
