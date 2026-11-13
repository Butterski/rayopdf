// SPDX-License-Identifier: Apache-2.0
// The C ABI of include/rayopdf/rayopdf_c.h over the C++ API's conversion. Outside the api module's
// folder: only what exports the ABI links it (test_api_c, the shared library, the WebAssembly
// module), never the command line.
#include "rayopdf/rayopdf_c.h"

#include "api/convert.h"
#include "api/flags.h"
#include "common/batch.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using namespace RayoPdf;

constexpr int kUsage = 2;

// A malloc'd NUL-terminated copy of `text`; the process ends when memory is exhausted, as it does
// when a std::string cannot grow (no exceptions, roadmap D1).
char* Copy(std::string_view text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) std::abort();
    if (!text.empty()) std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return out;
}

int Finish(rayopdf_output* out, int status, int pages, std::string_view text, std::string_view report,
           std::string_view error) {
    out->status = status;
    out->pages = pages;
    out->text = Copy(text);
    out->text_size = text.size();
    out->report = Copy(report);
    out->error = Copy(error);
    return status;
}

// The flags into `options`: false, with `error`, for a usage error.
bool ParseArgs(const char* const* args, size_t count, Layout::ConvertOptions& options, std::string& password,
               std::string& error) {
    Api::DocumentOptions document;
    for (size_t i = 0; i < count; ++i) {
        if (args[i] == nullptr) {
            error = "argument " + std::to_string(i) + " is null";
            return false;
        }
        const std::string_view arg = args[i];
        if (arg.substr(0, 10) == "--workers=") {
            int workers = 0;
            if (!Api::ParsePage(arg.substr(10), workers) || workers > static_cast<int>(Batch::kMaxWorkers)) {
                error = "--workers must be between 1 and 64: " + std::string(arg.substr(10));
                return false;
            }
            document.convert.pageWorkers = static_cast<unsigned>(workers);
            continue;
        }
        switch (Api::ParseDocumentFlag(arg, document, error)) {
            case Api::FlagStatus::Taken: break;
            case Api::FlagStatus::Invalid: return false;
            case Api::FlagStatus::Unknown: error = "unknown option: " + std::string(arg); return false;
        }
    }
    options = document.convert;
#ifdef __EMSCRIPTEN__
    options.pageWorkers = 1;  // the WebAssembly module has no threads; --workers is taken and has no effect
#endif
    password = document.password;
    options.imageDirectory.clear();
    options.imageLinkPrefix.clear();
    if (document.imagesGiven && document.images != "none") {
        options.imageDirectory = document.images;
        options.imageLinkPrefix = document.images;
        if (options.imageLinkPrefix.back() != '/') options.imageLinkPrefix += '/';
    }
    return true;
}

} // namespace

extern "C" {

int rayopdf_convert(const void* pdf, size_t pdf_size, const char* const* args, size_t arg_count,
                    rayopdf_output* out) {
    if (out == nullptr) return kUsage;
    if ((pdf == nullptr && pdf_size != 0) || (args == nullptr && arg_count != 0)) {
        return Finish(out, kUsage, 0, {}, {}, pdf == nullptr ? "the input is null" : "the arguments are null");
    }
    Layout::ConvertOptions options;
    std::string password;
    std::string error;
    if (!ParseArgs(args, arg_count, options, password, error)) return Finish(out, kUsage, 0, {}, {}, error);
    std::string text;
    const Api::Result result = Api::ConvertMemory(
        std::string_view(pdf_size == 0 ? "" : static_cast<const char*>(pdf), pdf_size), password, options, text);
    const bool ok = result.status == Api::Status::Ok;
    return Finish(out, static_cast<int>(result.status), result.pages, text, result.report,
                  ok ? std::string_view() : std::string_view(result.error));
}

void rayopdf_output_free(rayopdf_output* out) {
    if (out == nullptr) return;
    std::free(out->text);
    std::free(out->report);
    std::free(out->error);
    std::memset(out, 0, sizeof *out);
}

const char* rayopdf_version(void) {
    return RayoPdf::Api::Version();
}

} // extern "C"
