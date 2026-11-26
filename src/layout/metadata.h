// SPDX-License-Identifier: Apache-2.0
// The document information dictionary (ISO 32000-1 §14.3.3, /Info in the trailer) and the
// catalog's /Lang as UTF-8 text, the dates as ISO 8601, and the YAML front matter --front-matter
// writes before the Markdown (roadmap D6). The JSON output (--format=json) carries the same fields
// as its "metadata", and the pages' labels (§12.4.2, /PageLabels) as their "label".
#pragma once

#include "pdf/document.h"

#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Layout {

// A field longer than this (in UTF-8 bytes) is left empty: no reader needs more of a title.
constexpr size_t kMaxMetadataBytes = 64 * 1024;
// A page label's prefix (/P) is read from at most this many bytes of its string; a Roman number
// takes at most this many M, and a letter number repeats its letter at most this many times
// (larger numbers are written in decimal).
constexpr size_t kMaxLabelPrefix = 64;
constexpr size_t kMaxLabelRepeats = 64;

// The /Info strings and the catalog's /Lang, UTF-8 with surrounding white space trimmed; empty when
// absent. The dates as PdfDateToIso writes them, or as the file has them when they do not parse.
struct DocumentMetadata {
    std::string title, author, subject, keywords, creator, producer, created, modified, language;
    // JSON only (ReadPageLabels): per converted page its label; empty when the document has none.
    std::vector<std::string> labels;
};

// The fields in the order the front matter and the JSON output write them, with their names.
struct MetadataField {
    const char* name;
    std::string DocumentMetadata::*member;
};
constexpr MetadataField kMetadataFields[] = {
    {"title", &DocumentMetadata::title},       {"author", &DocumentMetadata::author},
    {"subject", &DocumentMetadata::subject},   {"keywords", &DocumentMetadata::keywords},
    {"creator", &DocumentMetadata::creator},   {"producer", &DocumentMetadata::producer},
    {"created", &DocumentMetadata::created},   {"modified", &DocumentMetadata::modified},
    {"language", &DocumentMetadata::language},
};

// /Title, /Author, /Subject, /Keywords, /Creator, /Producer, /CreationDate and /ModDate of
// `document`'s /Info, and its catalog's /Lang, into `out` (replaced; `labels` left empty). Strings
// go through AppendTextString (layout/interpreter.h).
void ReadMetadata(const Document& document, DocumentMetadata& out);

// The labels of pages `first` to `last` (0-based) into `out` (replaced; one a page), from the
// catalog's /PageLabels number tree (pdf/trees.h): the range whose key is the greatest at most the
// page's index gives its prefix /P and its number, /St (default 1) plus the page's place in the
// range, in style /S: D decimal, R and r Roman, A and a letters (A to Z, then AA to ZZ, ...); no
// /S: the prefix alone. A page before every range has an empty label. `out` stays empty when the
// document has no labels.
void ReadPageLabels(const Document& document, int first, int last, std::vector<std::string>& out);

// A PDF date (§7.9.4, "D:YYYYMMDDHHmmSSOHH'mm'", "D:" optional, everything after the year
// optional: month and day default to 01, the time to 00:00:00) as `YYYY-MM-DDTHH:MM:SS`, with `Z`
// or `+HH:MM`/`-HH:MM` when the date gives its offset, in `out` (replaced). False, `out` empty,
// for digits missing or out of range, or anything else after the date.
bool PdfDateToIso(std::string_view date, std::string& out);

// YAML front matter: "---\n", one `name: "value"` line per non-empty field (the value as a JSON
// string, which YAML reads as a double-quoted scalar), `pages: N`, then "---\n\n"; appended to
// `out`.
void AppendFrontMatter(const DocumentMetadata& metadata, int pages, std::string& out);

} // namespace RayoPdf::Layout
