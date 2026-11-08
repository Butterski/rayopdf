// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/pdf/content.h at 9fbbfaf (Apache-2.0, same author).
// The content-stream lexer (ISO 32000-1 section 7.8.2): operators with their operands, for the
// interpreter. Malformed input never stops it: a broken operand is dropped together
// with the operands before it, stray delimiters are skipped, and every step consumes input, so
// a stream of any bytes is read to its end in linear time.
#pragma once

#include "pdf/syntax.h"
#include "pdf/value.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace RayoPdf {

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

    // Next's operand writes with its locals: `values` and `numbers` are the slots' data, `slots`
    // their number, `count` the operands, `numeric` the finite numbers at their end.
    void Added(PdfValue*& values, double*& numbers, size_t& slots, size_t& count, size_t& numeric);
    void Add(PdfValue*& values, double*& numbers, size_t& slots, size_t& count, size_t& numeric, const PdfValue& value);
    void Grow();
    void ReadInlineImage();

    Syntax::Cursor cursor_;
    // Numbers starting before this offset end before it: the byte there is no digit, sign or point.
    size_t numbersEnd_ = 0;
    // The operands, and beside each number its value as a double.
    std::vector<PdfValue> values_;
    std::vector<double> numbers_;
    size_t count_ = 0;    // operands
    size_t numeric_ = 0;  // finite numbers at the end of the operands
    std::string_view inlineData_;
    PdfArena arena_;
    Syntax::Builder builder_;
};

} // namespace RayoPdf
