// SPDX-License-Identifier: Apache-2.0
#include "layout/metadata.h"

#include "common/batch.h"
#include "layout/interpreter.h"

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
    static constexpr const char* kKeys[] = {"Title",   "Author",   "Subject",      "Keywords",
                                            "Creator", "Producer", "CreationDate", "ModDate"};
    static_assert(sizeof(kKeys) / sizeof(kKeys[0]) == sizeof(kMetadataFields) / sizeof(kMetadataFields[0]),
                  "one /Info key a field");
    const PdfValue& info = document.Info();
    if (!info.IsDict()) return;
    std::string text;
    for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); ++k) {
        const PdfValue& value = document.Get(info, kKeys[k]);
        if (!value.IsString()) continue;
        text.clear();
        AppendTextString(value.String(), text);
        size_t begin = 0, end = text.size();
        while (begin < end && IsBlank(text[begin])) ++begin;
        while (end > begin && IsBlank(text[end - 1])) --end;
        if (end - begin > kMaxMetadataBytes) continue;
        std::string& field = out.*kMetadataFields[k].member;
        const std::string_view trimmed(text.data() + begin, end - begin);
        // The dates (the last two fields) in ISO 8601 when they parse.
        if (k < 6 || !PdfDateToIso(trimmed, field)) field.assign(trimmed);
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
