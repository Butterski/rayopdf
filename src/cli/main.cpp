// SPDX-License-Identifier: Apache-2.0
// The rayopdf command line (roadmap D7).
//
//   rayopdf --convert IN.pdf OUT.md [--images=DIR|none] [--password=PW] [--pages=A-B]
//                                   [--no-annotations] [--report]
//   rayopdf --inspect IN.pdf [--password=PW]
//   rayopdf --version | --help
//
// OUT.md "-" writes the Markdown to standard output. Images go to OUT-images/ beside OUT.md
// unless --images says otherwise (none with "-"). Exit codes: 0 converted, 2 usage error,
// 3 unreadable input, 10 corrupt beyond repair, 11 password required, 12 unwritable output,
// 13 unsupported encryption.

#include "common/file.h"
#include "layout/convert.h"
#include "pdf/document.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#ifndef RAYOPDF_VERSION
#define RAYOPDF_VERSION "0.0.0"
#endif

namespace {

using namespace RayoPdf;

enum Exit : int {
    kExitOk = 0,
    kExitUsage = 2,
    kExitUnreadable = 3,
    kExitCorrupt = 10,
    kExitPassword = 11,
    kExitUnwritable = 12,
    kExitUnsupported = 13,
};

// Inputs larger than this are refused (exit 3): far beyond any document a converter meets.
constexpr uint64_t kMaxInputBytes = uint64_t(4) << 30;

void PrintUsage(std::FILE* out) {
    std::fputs(
        "rayopdf " RAYOPDF_VERSION " - PDF to Markdown\n"
        "\n"
        "Usage:\n"
        "  rayopdf --convert IN.pdf OUT.md [options]   convert (OUT.md \"-\" is standard output)\n"
        "  rayopdf --inspect IN.pdf [--password=PW]     print the document's structure as JSON\n"
        "  rayopdf --version | --help\n"
        "\n"
        "Options:\n"
        "  --password=PW      user or owner password of an encrypted PDF\n"
        "  --pages=A-B        convert pages A to B (1-based; \"A\", \"A-\", \"-B\" also work)\n"
        "  --images=DIR|none  where image files go (default: OUT-images/ beside OUT.md;\n"
        "                     none for standard output)\n"
        "  --no-annotations   do not turn link annotations into Markdown links\n"
        "  --report           print one JSON line of conversion counters\n"
        "\n"
        "Exit codes: 0 converted, 2 usage, 3 unreadable input, 10 corrupt beyond repair,\n"
        "11 password required, 12 unwritable output, 13 unsupported encryption.\n",
        out);
}

int Usage(const char* message, std::string_view detail = {}) {
    std::fprintf(stderr, "rayopdf: %s%.*s\n", message, static_cast<int>(detail.size()), detail.data());
    std::fputs("Try 'rayopdf --help'.\n", stderr);
    return kExitUsage;
}

// A positive page number; false for anything else.
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

// "A", "A-B", "A-", "-B".
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

void AppendJsonString(std::string& out, std::string_view text) {
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void AppendField(std::string& out, const char* key, uint64_t value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "\"%s\":%" PRIu64, key, value);
    if (out.size() > 1) out.push_back(',');
    out += buf;
}

void AppendField(std::string& out, const char* key, bool value) {
    if (out.size() > 1) out.push_back(',');
    out += '"';
    out += key;
    out += value ? "\":true" : "\":false";
}

void AppendField(std::string& out, const char* key, std::string_view value) {
    if (out.size() > 1) out.push_back(',');
    out += '"';
    out += key;
    out += "\":";
    AppendJsonString(out, value);
}

// Reads the input; an exit code other than kExitOk when it cannot.
int ReadInput(const std::string& path, File::InputFile& file) {
    switch (file.Open(path, kMaxInputBytes)) {
        case File::ReadStatus::Ok: return kExitOk;
        case File::ReadStatus::NotFound:
            std::fprintf(stderr, "rayopdf: cannot open %s\n", path.c_str());
            return kExitUnreadable;
        case File::ReadStatus::NotRegular:
            std::fprintf(stderr, "rayopdf: %s is not a regular file\n", path.c_str());
            return kExitUnreadable;
        case File::ReadStatus::TooLarge:
            std::fprintf(stderr, "rayopdf: %s is too large\n", path.c_str());
            return kExitUnreadable;
        case File::ReadStatus::Failed: break;
    }
    std::fprintf(stderr, "rayopdf: cannot read %s\n", path.c_str());
    return kExitUnreadable;
}

int OpenDocument(const std::string& path, std::string_view bytes, std::string_view password,
                 std::unique_ptr<Document>& document) {
    switch (Document::Open(bytes, password, document)) {
        case OpenStatus::Ok: return kExitOk;
        case OpenStatus::NotPdf:
            std::fprintf(stderr, "rayopdf: %s is not a PDF file\n", path.c_str());
            return kExitCorrupt;
        case OpenStatus::Corrupt:
            std::fprintf(stderr, "rayopdf: %s is corrupt beyond repair\n", path.c_str());
            return kExitCorrupt;
        case OpenStatus::TooLarge:
            std::fprintf(stderr, "rayopdf: %s exceeds a document limit\n", path.c_str());
            return kExitCorrupt;
        case OpenStatus::PasswordRequired:
            std::fprintf(stderr, "rayopdf: %s needs a password (--password=PW)\n", path.c_str());
            return kExitPassword;
        case OpenStatus::Unsupported: break;
    }
    std::fprintf(stderr, "rayopdf: %s uses an unsupported encryption\n", path.c_str());
    return kExitUnsupported;
}

int Inspect(const std::string& input, std::string_view password) {
    File::InputFile file;
    if (const int code = ReadInput(input, file)) return code;
    std::unique_ptr<Document> document;
    if (const int code = OpenDocument(input, file.Bytes(), password, document)) return code;

    const DocumentReport& report = document->Report();
    std::string json = "{";
    AppendField(json, "file", std::string_view(input));
    AppendField(json, "bytes", static_cast<uint64_t>(file.Bytes().size()));
    AppendField(json, "version", document->Version());
    AppendField(json, "pages", static_cast<uint64_t>(document->PageCount()));
    AppendField(json, "objects", static_cast<uint64_t>(document->ObjectCount()));
    AppendField(json, "encrypted", report.encrypted);
    AppendField(json, "repaired", report.repaired);
    AppendField(json, "linearized", report.linearized);
    AppendField(json, "xref_sections", static_cast<uint64_t>(report.xrefSections));
    const PdfValue& info = document->Info();
    for (const char* key : {"Title", "Producer", "Creator"}) {
        const PdfValue& value = document->Get(info, key);
        if (value.IsString()) {
            std::string name = key;
            name[0] = static_cast<char>(name[0] - 'A' + 'a');
            AppendField(json, name.c_str(), value.String());
        }
    }
    const PdfValue& markInfo = document->Get(document->Catalog(), "MarkInfo");
    AppendField(json, "tagged", document->Get(markInfo, "Marked").Bool());
    if (document->PageCount() > 0) {
        const PdfRect box = document->CropBox(0);
        char buf[96];
        std::snprintf(buf, sizeof buf, ",\"first_page\":{\"width\":%.2f,\"height\":%.2f,\"rotate\":%d}", box.Width(),
                      box.Height(), document->Rotate(0));
        json += buf;
    }
    AppendField(json, "broken_objects", static_cast<uint64_t>(report.brokenObjects));
    json += "}\n";
    std::fwrite(json.data(), 1, json.size(), stdout);
    return kExitOk;
}

std::string ReportJson(const std::string& input, const Document& document, const Layout::ConvertReport& r) {
    const DocumentReport& d = document.Report();
    std::string json = "{";
    AppendField(json, "file", std::string_view(input));
    AppendField(json, "pages", static_cast<uint64_t>(r.pages));
    AppendField(json, "pages_without_text", static_cast<uint64_t>(r.pagesWithoutText));
    AppendField(json, "glyphs", r.glyphs);
    AppendField(json, "unmapped_glyphs", r.unmappedGlyphs);
    AppendField(json, "guessed_glyphs", r.guessedGlyphs);
    AppendField(json, "hidden_glyphs", r.hiddenGlyphs);
    AppendField(json, "headings", static_cast<uint64_t>(r.headings));
    AppendField(json, "paragraphs", static_cast<uint64_t>(r.paragraphs));
    AppendField(json, "list_items", static_cast<uint64_t>(r.listItems));
    AppendField(json, "code_blocks", static_cast<uint64_t>(r.codeBlocks));
    AppendField(json, "tables", static_cast<uint64_t>(r.tables));
    AppendField(json, "links", static_cast<uint64_t>(r.links));
    AppendField(json, "footnotes", static_cast<uint64_t>(r.footnotes));
    AppendField(json, "images", static_cast<uint64_t>(r.images));
    AppendField(json, "images_skipped", static_cast<uint64_t>(r.imagesSkipped));
    AppendField(json, "header_footer_lines", static_cast<uint64_t>(r.headerFooterLines));
    AppendField(json, "content_errors", static_cast<uint64_t>(r.contentErrors));
    AppendField(json, "repaired", d.repaired);
    AppendField(json, "encrypted", d.encrypted);
    AppendField(json, "broken_objects", static_cast<uint64_t>(d.brokenObjects));
    AppendField(json, "salvaged_streams", static_cast<uint64_t>(d.salvagedStreams));
    AppendField(json, "failed_streams", static_cast<uint64_t>(d.failedStreams));
    AppendField(json, "limit_hits", static_cast<uint64_t>(d.limitHits));
    json += "}\n";
    return json;
}

int ConvertFile(const std::string& input, const std::string& output, std::string_view password,
                const Layout::ConvertOptions& options, bool report) {
    File::InputFile file;
    if (const int code = ReadInput(input, file)) return code;
    std::unique_ptr<Document> document;
    if (const int code = OpenDocument(input, file.Bytes(), password, document)) return code;

    std::string markdown;
    Layout::ConvertReport counters;
    Layout::Convert(*document, options, markdown, counters);
    if (!File::WriteAll(output, markdown)) {
        std::fprintf(stderr, "rayopdf: cannot write %s\n", output.c_str());
        return kExitUnwritable;
    }
    if (report) {
        const std::string json = ReportJson(input, *document, counters);
        std::fwrite(json.data(), 1, json.size(), output == "-" ? stderr : stdout);
    }
    return kExitOk;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

// `path` made absolute against the working directory, with "." and ".." segments resolved
// lexically (links are written as paths; no file system lookups).
std::string AbsolutePath(const std::string& path) {
    std::string full = path;
    if (path.empty() || path[0] != '/') {
        char cwd[4096];
        full = (getcwd(cwd, sizeof cwd) ? std::string(cwd) : std::string(".")) + "/" + path;
    }
    std::string out;
    size_t at = 0;
    while (at <= full.size()) {
        size_t next = full.find('/', at);
        if (next == std::string::npos) next = full.size();
        const std::string_view part(full.data() + at, next - at);
        if (part == "..") {
            const size_t cut = out.rfind('/');
            out.resize(cut == std::string::npos ? 0 : cut);
        } else if (!part.empty() && part != ".") {
            out += '/';
            out += part;
        }
        at = next + 1;
    }
    return out.empty() ? "/" : out;
}

// The directory part of an absolute path ("/a/b" for "/a/b/c.md").
std::string Parent(const std::string& absolute) {
    const size_t slash = absolute.rfind('/');
    return slash == 0 || slash == std::string::npos ? "/" : absolute.substr(0, slash);
}

// `target` relative to the directory `from` (both absolute), "" when they are the same.
std::string Relative(const std::string& from, const std::string& target) {
    std::vector<std::string_view> a, b;
    for (const std::string* path : {&from, &target}) {
        std::vector<std::string_view>& parts = path == &from ? a : b;
        size_t at = 1;
        while (at < path->size()) {
            size_t next = path->find('/', at);
            if (next == std::string::npos) next = path->size();
            parts.emplace_back(path->data() + at, next - at);
            at = next + 1;
        }
    }
    size_t common = 0;
    while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
    std::string out;
    for (size_t i = common; i < a.size(); ++i) out += "../";
    for (size_t i = common; i < b.size(); ++i) {
        out += b[i];
        out += '/';
    }
    return out;
}

// Where images go and how the Markdown links them (D6): by default OUT-images/ beside OUT.md;
// --images=none or Markdown on standard output: none; --images=DIR: DIR, linked relative to OUT.md.
void ChooseImageTarget(const std::string& output, const std::string* images, Layout::ConvertOptions& options) {
    if ((images && *images == "none") || (!images && output == "-")) return;
    std::string directory;
    if (images) {
        directory = *images;
    } else {
        directory = output;
        const size_t slash = directory.rfind('/');
        const size_t dot = directory.rfind('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) directory.resize(dot);
        directory += "-images";
    }
    const std::string from = output == "-" ? AbsolutePath(".") : Parent(AbsolutePath(output));
    options.imageDirectory = directory;
    options.imageLinkPrefix = Relative(from, AbsolutePath(directory));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(stderr);
        return kExitUsage;
    }
    enum class Mode { None, Convert, Inspect } mode = Mode::None;
    std::string input;
    std::string output;
    std::string password;
    Layout::ConvertOptions options;
    bool report = false;
    std::string images;
    bool imagesGiven = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(stdout);
            return kExitOk;
        }
        if (arg == "--version" || arg == "-V") {
            std::fputs("rayopdf " RAYOPDF_VERSION "\n", stdout);
            return kExitOk;
        }
        if (arg == "--convert") {
            if (mode != Mode::None) return Usage("only one of --convert and --inspect");
            if (i + 2 >= argc) return Usage("--convert needs IN.pdf and OUT.md");
            mode = Mode::Convert;
            input = argv[++i];
            output = argv[++i];
        } else if (arg == "--inspect") {
            if (mode != Mode::None) return Usage("only one of --convert and --inspect");
            if (i + 1 >= argc) return Usage("--inspect needs IN.pdf");
            mode = Mode::Inspect;
            input = argv[++i];
        } else if (StartsWith(arg, "--password=")) {
            password = std::string(arg.substr(11));
        } else if (StartsWith(arg, "--pages=")) {
            if (!ParsePages(arg.substr(8), options.firstPage, options.lastPage)) {
                return Usage("invalid page range: ", arg.substr(8));
            }
        } else if (StartsWith(arg, "--images=")) {
            images = std::string(arg.substr(9));
            if (images.empty()) return Usage("--images needs a directory or none");
            imagesGiven = true;
        } else if (arg == "--no-annotations") {
            options.annotations = false;
        } else if (arg == "--report") {
            report = true;
        } else {
            return Usage("unknown argument: ", arg);
        }
    }
    if (mode == Mode::Inspect) return Inspect(input, password);
    if (mode == Mode::Convert) {
        ChooseImageTarget(output, imagesGiven ? &images : nullptr, options);
        return ConvertFile(input, output, password, options, report);
    }
    return Usage("nothing to do: give --convert or --inspect");
}
