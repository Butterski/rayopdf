// SPDX-License-Identifier: Apache-2.0
#include "api/flags.h"

#include <cstdint>

namespace RayoPdf::Api {

namespace {

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

} // namespace

bool ParsePage(std::string_view text, int& out) {
    if (text.empty() || text.size() > 9) return false;
    int value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1) return false;
    out = value;
    return true;
}

bool ParsePages(std::string_view text, int& first, int& last) {
    const size_t dash = text.find('-');
    if (dash == std::string_view::npos) {
        if (!ParsePage(text, first)) return false;
        last = first;
        return true;
    }
    const std::string_view a = text.substr(0, dash);
    const std::string_view b = text.substr(dash + 1);
    if (a.empty() && b.empty()) return false;
    first = 1;
    last = 0;
    if (!a.empty() && !ParsePage(a, first)) return false;
    if (!b.empty() && !ParsePage(b, last)) return false;
    return last == 0 || first <= last;
}

FlagStatus ParseDocumentFlag(std::string_view arg, DocumentOptions& options, std::string& error) {
    if (StartsWith(arg, "--password=")) {
        options.password = std::string(arg.substr(11));
    } else if (StartsWith(arg, "--pages=")) {
        if (!ParsePages(arg.substr(8), options.convert.firstPage, options.convert.lastPage)) {
            error = "invalid page range: " + std::string(arg.substr(8));
            return FlagStatus::Invalid;
        }
    } else if (StartsWith(arg, "--images=")) {
        options.images = std::string(arg.substr(9));
        if (options.images.empty()) {
            error = "--images needs a directory or none";
            return FlagStatus::Invalid;
        }
        options.imagesGiven = true;
    } else if (arg == "--no-exact") {
        options.convert.exact = false;
    } else if (arg == "--no-annotations") {
        options.convert.annotations = false;
    } else if (arg == "--no-structure") {
        options.convert.structure = false;
    } else if (StartsWith(arg, "--ocr=")) {
        const std::string_view mode = arg.substr(6);
        if (mode == "auto") {
            options.convert.ocr.mode = Layout::OcrMode::Auto;
        } else if (mode == "off") {
            options.convert.ocr.mode = Layout::OcrMode::Off;
        } else if (mode == "force") {
            options.convert.ocr.mode = Layout::OcrMode::Force;
        } else {
            error = "--ocr takes auto, off or force: " + std::string(mode);
            return FlagStatus::Invalid;
        }
    } else if (StartsWith(arg, "--ocr-addon=")) {
        options.convert.ocr.addon = std::string(arg.substr(12));
        if (options.convert.ocr.addon.empty()) {
            error = "--ocr-addon needs a path";
            return FlagStatus::Invalid;
        }
    } else if (StartsWith(arg, "--math=")) {
        const std::string_view mode = arg.substr(7);
        if (mode == "unicode") {
            options.convert.math = Layout::MathMode::Unicode;
        } else if (mode == "latex") {
            options.convert.math = Layout::MathMode::Latex;
        } else {
            error = "--math takes unicode or latex: " + std::string(mode);
            return FlagStatus::Invalid;
        }
    } else if (StartsWith(arg, "--format=")) {
        const std::string_view format = arg.substr(9);
        if (format == "markdown") {
            options.convert.format = Layout::OutputFormat::Markdown;
        } else if (format == "json") {
            options.convert.format = Layout::OutputFormat::Json;
        } else if (format == "chunks") {
            options.convert.format = Layout::OutputFormat::Chunks;
        } else if (format == "inspect") {
            options.convert.format = Layout::OutputFormat::Inspect;
        } else {
            error = "--format takes markdown, json, chunks or inspect: " + std::string(format);
            return FlagStatus::Invalid;
        }
    } else if (StartsWith(arg, "--chunk-chars=")) {
        const std::string_view value = arg.substr(14);
        uint32_t chars = 0;
        bool valid = !value.empty() && value.size() <= 7;
        for (const char c : value) {
            valid = valid && c >= '0' && c <= '9';
            if (valid) chars = chars * 10 + static_cast<uint32_t>(c - '0');
        }
        if (!valid || chars < Layout::kMinChunkChars || chars > Layout::kMaxChunkChars) {
            error = "--chunk-chars takes 256 to 1000000: " + std::string(value);
            return FlagStatus::Invalid;
        }
        options.convert.chunkChars = chars;
    } else if (arg == "--page-markers") {
        options.convert.pageMarkers = true;
    } else if (arg == "--front-matter") {
        options.convert.frontMatter = true;
    } else if (StartsWith(arg, "--furniture=")) {
        const std::string_view mode = arg.substr(12);
        if (mode == "drop") {
            options.convert.furniture = Layout::Furniture::Drop;
        } else if (mode == "keep") {
            options.convert.furniture = Layout::Furniture::Keep;
        } else {
            error = "--furniture takes drop or keep: " + std::string(mode);
            return FlagStatus::Invalid;
        }
    } else if (arg == "--plain") {
        options.convert.plain = true;
    } else if (StartsWith(arg, "--tables=")) {
        const std::string_view mode = arg.substr(9);
        if (mode == "auto") {
            options.convert.tables = Layout::TableFormat::Auto;
        } else if (mode == "html") {
            options.convert.tables = Layout::TableFormat::Html;
        } else {
            error = "--tables takes auto or html: " + std::string(mode);
            return FlagStatus::Invalid;
        }
    } else {
        return FlagStatus::Unknown;
    }
    return FlagStatus::Taken;
}

} // namespace RayoPdf::Api
