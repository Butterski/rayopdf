// SPDX-License-Identifier: Apache-2.0
// The flags of one conversion (`--pages`, `--format`, ...), parsed once for the command line, its
// serve requests and the C ABI (include/rayopdf/rayopdf_c.h).
#pragma once

#include "layout/convert.h"

#include <string>
#include <string_view>

namespace RayoPdf::Api {

// A positive page number of at most 9 digits; false for anything else.
bool ParsePage(std::string_view text, int& out);

// "A", "A-B", "A-", "-B": `last` 0 for an open end.
bool ParsePages(std::string_view text, int& first, int& last);

// The flags of one conversion: those of --convert, a batch's for each document, a serve session's
// with each request's `args` over them, and a C ABI call's.
struct DocumentOptions {
    std::string password;
    Layout::ConvertOptions convert;  // pages, format, OCR...; the caller chooses the images
    std::string images;              // --images's value
    bool imagesGiven = false;
};

enum class FlagStatus { Taken, Unknown, Invalid };

// One flag of a conversion into `options`; Invalid with `error` for a known flag with a bad value.
FlagStatus ParseDocumentFlag(std::string_view arg, DocumentOptions& options, std::string& error);

} // namespace RayoPdf::Api
