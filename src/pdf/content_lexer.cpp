// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/content.cpp at 9fbbfaf (Apache-2.0, same author).
#include "pdf/content_lexer.h"

#include "common/compiler.h"

#include <cstdint>
#include <cstring>
#include <utility>

namespace RayoPdf {
namespace {

using Syntax::Cursor;

// The common operand at the cursor: [+-]digits[.digits] or [+-].digits with at most 19 digits,
// ending at a white-space or delimiter byte (or the end). Reads it into `value` (a default
// PdfValue) and moves the cursor past it, with exactly the value Syntax::ParseNumber gives the
// token: up to 19 digits all count, so its mantissa, fraction count and Int/Real choice are the
// same, and 10^fraction comes from the same exact powers. False, with the cursor and `value`
// untouched, for anything else (longer numbers, a second point, letters): the general path reads
// those.
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
    if (digits == 0 || digits > 19 || (p < end && Syntax::IsRegular(static_cast<unsigned char>(*p)))) return false;
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

// The operand just added stays; past kMaxOperands it is the only one.
void Kept(std::vector<PdfValue>& operands) {
    if (operands.size() > ContentLexer::kMaxOperands) operands.erase(operands.begin(), operands.end() - 1);
}

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

bool ContentLexer::Next(std::string_view& op, std::vector<PdfValue>& operands) {
    operands.clear();
    inlineData_ = {};
    arena_.Reset();
    Cursor& c = cursor_;
    for (;;) {
        // White space inline; comments (rare in content streams) through SkipSpace.
        while (c.pos < c.data.size() && Syntax::IsWhite(static_cast<unsigned char>(c.data[c.pos]))) c.pos++;
        if (c.pos < c.data.size() && c.data[c.pos] == '%') Syntax::SkipSpace(c);
        if (c.AtEnd()) return false;
        const unsigned char ch = c.Peek();
        if (Syntax::IsRegular(ch)) {
            // Read into a new last operand, dropped again for an operator.
            PdfValue& value = operands.emplace_back();
            if (Syntax::StartsNumber(ch) && ReadSimpleNumber(c, value)) {
                Kept(operands);
                continue;
            }
            const size_t start = c.pos;
            while (c.pos < c.data.size() && Syntax::IsRegular(static_cast<unsigned char>(c.data[c.pos]))) c.pos++;
            const std::string_view token = c.data.substr(start, c.pos - start);
            if (token == "true" || token == "false") {
                value.type = PdfValue::Type::Bool;
                value.boolean = token == "true";
            } else if (token != "null" && !(Syntax::StartsNumber(ch) && Syntax::ParseNumber(token, value))) {
                operands.pop_back();
                op = token;
                if (token == "BI") ReadInlineImage(operands);
                return true;
            }
            Kept(operands);
            continue;
        }
        if (ch == '/' || ch == '(' || ch == '<' || ch == '[') {
            const size_t start = c.pos;
            PdfValue value;
            if (Syntax::ParseValue(c, Syntax::Mode::Content, 0, builder_, value)) {
                operands.push_back(value);
                Kept(operands);
            } else {
                if (c.pos == start) c.pos++;
                operands.clear();
            }
            continue;
        }
        c.pos++;  // a stray ')', ']', '>', '{' or '}'
    }
}

void ContentLexer::ReadInlineImage(std::vector<PdfValue>& operands) {
    Cursor& c = cursor_;
    operands.clear();
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
    operands.push_back(dict);
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
    if (InlineImageSize(operands[0], size) && size <= bytes.size() - start) {
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
