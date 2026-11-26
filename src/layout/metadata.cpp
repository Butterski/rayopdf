// SPDX-License-Identifier: Apache-2.0
#include "layout/metadata.h"

#include "common/batch.h"
#include "layout/interpreter.h"
#include "pdf/trees.h"

#include <algorithm>
#include <utility>

namespace RayoPdf::Layout {

namespace {

bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; }

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Two digits at `at` with a value from `low` to `high` into `value`, `at` past them.
bool TwoDigits(std::string_view text, size_t& at, int low, int high, int& value) {
    if (at + 2 > text.size() || !IsDigit(text[at]) || !IsDigit(text[at + 1])) return false;
    value = (text[at] - '0') * 10 + (text[at + 1] - '0');
    at += 2;
    return value >= low && value <= high;
}

void AppendTwo(std::string& out, int value) {
    out.push_back(static_cast<char>('0' + value / 10));
    out.push_back(static_cast<char>('0' + value % 10));
}

// The offset's HH'mm' after its sign (or after a Z, where producers repeat 00'00'): hours, then an
// optional apostrophe, minutes and apostrophe.
bool Offset(std::string_view text, size_t& at, int& hours, int& minutes) {
    minutes = 0;
    if (!TwoDigits(text, at, 0, 23, hours)) return false;
    if (at < text.size() && text[at] == '\'') ++at;
    if (at < text.size() && IsDigit(text[at])) {
        if (!TwoDigits(text, at, 0, 59, minutes)) return false;
        if (at < text.size() && text[at] == '\'') ++at;
    }
    return true;
}

// A page label's number in style `style` (§12.4.2: D decimal, R and r Roman, A and a letters)
// appended to `out`. Roman numerals take thousands as repeated M, letters repeat one letter (A to Z,
// then AA to ZZ, ...), each at most kMaxLabelRepeats times; a value past that, or below 1, is
// written in decimal.
void AppendLabelNumber(char style, int64_t value, std::string& out) {
    constexpr auto kRepeats = static_cast<int64_t>(kMaxLabelRepeats);
    if ((style == 'R' || style == 'r') && value >= 1 && value / 1000 <= kRepeats) {
        static constexpr const char* kOnes[] = {"", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix"};
        static constexpr const char* kTens[] = {"", "x", "xx", "xxx", "xl", "l", "lx", "lxx", "lxxx", "xc"};
        static constexpr const char* kHundreds[] = {"", "c", "cc", "ccc", "cd", "d", "dc", "dcc", "dccc", "cm"};
        const size_t start = out.size();
        out.append(static_cast<size_t>(value / 1000), 'm');
        out += kHundreds[value / 100 % 10];
        out += kTens[value / 10 % 10];
        out += kOnes[value % 10];
        if (style == 'R')
            for (size_t i = start; i < out.size(); ++i) out[i] = static_cast<char>(out[i] - 'a' + 'A');
        return;
    }
    if ((style == 'A' || style == 'a') && value >= 1 && (value - 1) / 26 < kRepeats) {
        out.append(static_cast<size_t>((value - 1) / 26 + 1), static_cast<char>(style + (value - 1) % 26));
        return;
    }
    out += std::to_string(value);
}

} // namespace

bool PdfDateToIso(std::string_view date, std::string& out) {
    out.clear();
    size_t at = date.substr(0, 2) == "D:" ? 2 : 0;
    if (at + 4 > date.size()) return false;
    for (size_t i = at; i < at + 4; ++i)
        if (!IsDigit(date[i])) return false;
    const std::string_view year = date.substr(at, 4);
    at += 4;
    // Month, day, hours, minutes and seconds, each optional once the one before is there.
    static constexpr int kLow[5] = {1, 1, 0, 0, 0};
    static constexpr int kHigh[5] = {12, 31, 23, 59, 59};
    int parts[5] = {1, 1, 0, 0, 0};
    for (int i = 0; i < 5 && at < date.size() && IsDigit(date[at]); ++i)
        if (!TwoDigits(date, at, kLow[i], kHigh[i], parts[i])) return false;
    char sign = 0;
    int hours = 0, minutes = 0;
    if (at < date.size() && (date[at] == 'Z' || date[at] == '+' || date[at] == '-')) {
        sign = date[at++];
        if (sign != 'Z' || (at < date.size() && IsDigit(date[at]))) {
            if (!Offset(date, at, hours, minutes)) return false;
        }
    }
    while (at < date.size() && IsBlank(date[at])) ++at;
    if (at != date.size()) return false;
    out.append(year);
    for (int i = 0; i < 5; ++i) {
        out.push_back(i < 2 ? '-' : i == 2 ? 'T' : ':');
        AppendTwo(out, parts[i]);
    }
    if (sign == 'Z') {
        out.push_back('Z');
    } else if (sign != 0) {
        out.push_back(sign);
        AppendTwo(out, hours);
        out.push_back(':');
        AppendTwo(out, minutes);
    }
    return true;
}

void ReadMetadata(const Document& document, DocumentMetadata& out) {
    out = DocumentMetadata();
    // The /Info keys of the fields before "language", which the catalog's /Lang gives.
    static constexpr const char* kKeys[] = {"Title",   "Author",   "Subject",      "Keywords",
                                            "Creator", "Producer", "CreationDate", "ModDate"};
    constexpr size_t kInfoKeys = sizeof(kKeys) / sizeof(kKeys[0]);
    static_assert(kInfoKeys + 1 == sizeof(kMetadataFields) / sizeof(kMetadataFields[0]),
                  "one /Info key a field, then /Lang");
    std::string text;
    const auto read = [&](const PdfValue& value, size_t k) {
        if (!value.IsString()) return;
        text.clear();
        AppendTextString(value.String(), text);
        size_t begin = 0, end = text.size();
        while (begin < end && IsBlank(text[begin])) ++begin;
        while (end > begin && IsBlank(text[end - 1])) --end;
        if (end - begin > kMaxMetadataBytes) return;
        std::string& field = out.*kMetadataFields[k].member;
        const std::string_view trimmed(text.data() + begin, end - begin);
        // The dates in ISO 8601 when they parse.
        if ((k != 6 && k != 7) || !PdfDateToIso(trimmed, field)) field.assign(trimmed);
    };
    const PdfValue& info = document.Info();
    if (info.IsDict())
        for (size_t k = 0; k < kInfoKeys; ++k) read(document.Get(info, kKeys[k]), k);
    read(document.Get(document.Catalog(), "Lang"), kInfoKeys);
}

void ReadPageLabels(const Document& document, int first, int last, std::vector<std::string>& out) {
    out.clear();
    std::vector<std::pair<int64_t, const PdfValue*>> ranges;
    ReadNumberTree(document, document.Get(document.Catalog(), "PageLabels"), ranges);
    if (ranges.empty() || first > last) return;
    out.resize(static_cast<size_t>(last - first + 1));
    size_t r = 0;  // the page's range: the last whose key is at most its index
    for (int index = first; index <= last; ++index) {
        while (r + 1 < ranges.size() && ranges[r + 1].first <= index) ++r;
        if (ranges[r].first > index) continue;
        const PdfValue& range = document.Resolve(*ranges[r].second);
        std::string& label = out[static_cast<size_t>(index - first)];
        AppendTextString(document.Get(range, "P").String().substr(0, kMaxLabelPrefix), label);
        const std::string_view style = document.Get(range, "S").Name();
        if (style.size() != 1 || std::string_view("DRrAa").find(style[0]) == std::string_view::npos) continue;
        const int64_t start = std::max<int64_t>(1, std::min<int64_t>(document.Get(range, "St").Int(1), int64_t(1) << 40));
        AppendLabelNumber(style[0], start + (index - ranges[r].first), label);
    }
}

void AppendFrontMatter(const DocumentMetadata& metadata, int pages, std::string& out) {
    out += "---\n";
    for (const MetadataField& field : kMetadataFields) {
        const std::string& value = metadata.*field.member;
        if (value.empty()) continue;
        out += field.name;
        out += ": ";
        Batch::AppendJsonString(out, value);
        out.push_back('\n');
    }
    out += "pages: ";
    out += std::to_string(pages < 0 ? 0 : pages);
    out += "\n---\n\n";
}

} // namespace RayoPdf::Layout
