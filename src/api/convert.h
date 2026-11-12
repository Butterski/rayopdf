// SPDX-License-Identifier: Apache-2.0
// The steps of a conversion that the C++ API (include/rayopdf/rayopdf.h) and the command line
// share: reading the input, opening the document, and the report's counters.
#pragma once

#include "common/file.h"
#include "layout/convert.h"
#include "pdf/document.h"
#include "rayopdf/rayopdf.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace RayoPdf::Api {

// Inputs larger than this are refused (Status::Unreadable): far beyond any document a converter
// meets.
constexpr uint64_t kMaxInputBytes = uint64_t(4) << 30;

// Reads the input (into `buffer` when it is small enough and the caller keeps one, see
// File::InputFile::Open); a status other than Ok, with `error`, when it cannot.
Status ReadInput(const std::string& path, File::InputFile& file, std::string& error, std::string* buffer = nullptr);

// Opens the document over `bytes`; `path` names the input in `error`.
Status OpenDocument(const std::string& path, std::string_view bytes, std::string_view password,
                    std::unique_ptr<Document>& document, std::string& error);

// One member of a JSON object being written into `out` (which starts with '{' or ','): a comma
// before it unless it is the first.
void AppendField(std::string& out, const char* key, uint64_t value);
void AppendField(std::string& out, const char* key, bool value);
void AppendField(std::string& out, const char* key, std::string_view value);

// The record's counters after "ms" (`,"pages_without_text":..,...`): the conversion's and the
// reader's.
void AppendCounters(std::string& out, const Layout::ConvertReport& r, const DocumentReport& d);

} // namespace RayoPdf::Api
