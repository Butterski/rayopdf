// SPDX-License-Identifier: Apache-2.0
// The rayopdf command line (roadmap D7).
//
//   rayopdf --convert IN.pdf OUT.md [--images=DIR|none] [--password=PW] [--pages=A-B]
//                                   [--no-exact] [--no-annotations] [--no-structure] [--ocr=auto|off|force]
//                                   [--ocr-addon=PATH] [--report] [--math=unicode|latex] [--workers=N]
//                                   [--format=markdown|json] [--page-markers] [--front-matter]
//   rayopdf --batch IN-FOLDER OUT-FOLDER [--recursive] [--skip-unchanged] [--report[=FILE]]
//                                        [--workers=N] [--images=none] [--password=PW] ...
//   rayopdf --serve --json [--workers=N] [--images=none] [--password=PW] ...
//   rayopdf --inspect IN.pdf [--password=PW]
//   rayopdf --bench IN.pdf TMPDIR N [--images=none] [--no-structure] [--workers=N] ...   (developer option)
//   rayopdf --version | --help
//
// OUT.md "-" writes the Markdown to standard output. Images go to OUT-images/ beside OUT.md
// unless --images says otherwise (none with "-"). Exit codes: 0 converted, 2 usage error,
// 3 unreadable input, 10 corrupt beyond repair, 11 password required, 12 unwritable output,
// 13 unsupported encryption. A batch exits with the code of its last failed document in input
// order. The batch modes' records and files never depend on the number of workers: documents are
// converted independently and records are written in input order. --convert (and --bench with
// --workers) reads the pages of a document of several pages on several threads instead, with the
// same output (layout/convert.h). Each worker keeps its own OCR
// add-on process (started when a document first needs it), and the add-on answers the same
// request the same way, so OCR does not change that.

#include "api/convert.h"
#include "common/batch.h"
#include "common/file.h"
#include "common/serve.h"
#include "layout/convert.h"
#include "layout/ocr_client.h"
#include "pdf/document.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#ifndef RAYOPDF_VERSION
#define RAYOPDF_VERSION "0.0.0"
#endif

namespace {

using namespace RayoPdf;

// Exit codes: 2 for a usage error; the others are the C++ API's Status values (rayopdf/rayopdf.h).
enum Exit : int {
    kExitUsage = 2,
};
constexpr int kExitOk = static_cast<int>(Api::Status::Ok);
constexpr int kExitUnreadable = static_cast<int>(Api::Status::Unreadable);
constexpr int kExitUnwritable = static_cast<int>(Api::Status::Unwritable);

using Api::AppendField;

// A batch folder tree: at most this deep below the input folder, and at most this many PDFs.
constexpr int kMaxFolderDepth = 64;
constexpr size_t kMaxBatchFiles = size_t(1) << 20;

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

void PrintUsage(std::FILE* out) {
    std::fputs(
        "rayopdf " RAYOPDF_VERSION " - PDF to Markdown\n"
        "\n"
        "Usage:\n"
        "  rayopdf --convert IN.pdf OUT.md [options]   convert (OUT.md \"-\" is standard output)\n"
        "  rayopdf --batch IN-FOLDER OUT-FOLDER [options] [batch options]\n"
        "                                               convert every *.pdf of a folder to OUT-FOLDER\n"
        "  rayopdf --serve --json [options] [--workers=N]\n"
        "                                               answer JSON requests, one a line (protocol 1)\n"
        "  rayopdf --inspect IN.pdf [--password=PW]     print the document's structure as JSON\n"
        "  rayopdf --version | --help\n"
        "\n"
        "Developer option (performance measurements, docs/development/performance.md):\n"
        "  rayopdf --bench IN.pdf TMPDIR N [options]    convert IN.pdf once, then N times more in\n"
        "                                               this process into TMPDIR/out.md (images in\n"
        "                                               TMPDIR/out-images/); prints the fastest and\n"
        "                                               median milliseconds of the N timed runs\n"
        "\n"
        "Options:\n"
        "  --password=PW      user or owner password of an encrypted PDF\n"
        "  --pages=A-B        convert pages A to B (1-based; \"A\", \"A-\", \"-B\" also work)\n"
        "  --images=DIR|none  where image files go (default: OUT-images/ beside OUT.md;\n"
        "                     none for standard output; only none in the batch modes)\n"
        "  --no-exact         convert the pages even when the PDF carries its RayoMD source\n"
        "                     (by default a verified rayomd-source/1 payload is written as it is)\n"
        "  --no-annotations   do not turn link annotations into Markdown links\n"
        "  --no-structure     ignore the structure tree of a tagged PDF (layout analysis only)\n"
        "  --math=MODE        unicode (default): formulas as Unicode text, scripts as Unicode\n"
        "                     superscripts and subscripts or else <sup>/<sub>;\n"
        "                     latex: formulas rebuilt as LaTeX, $...$ inline and $$...$$ displayed\n"
        "  --format=FORMAT    markdown (default), or json: one JSON object with the document's\n"
        "                     metadata and its blocks (type, page, boxes, Markdown; schema 1)\n"
        "  --page-markers     <!-- page N --> before the Markdown of each page\n"
        "  --front-matter     YAML front matter from the document's information (title, author,\n"
        "                     dates, pages) before the Markdown\n"
        "  --ocr=MODE         auto (default): OCR pages and images whose text the file lacks, when\n"
        "                     the rayopdf-ocr add-on is installed; off: never; force: every page\n"
        "                     image and large image, replacing an existing invisible text layer\n"
        "  --ocr-addon=PATH   the OCR add-on to run (default: $RAYOPDF_OCR, then rayopdf-ocr\n"
        "                     beside rayopdf, then on PATH)\n"
        "  --report           print one JSON record of the document (its counters)\n"
        "  --workers=N        1 to 64; never changes the output. --convert: pages of a document\n"
        "                     read at once (default: one a CPU, at most 4); --bench: the same\n"
        "                     (default 1); --batch and --serve: documents converted at once\n"
        "                     (default: --batch one a CPU, at most 6; --serve 1)\n"
        "\n"
        "Batch options:\n"
        "  --recursive        subfolders too (not hidden ones), mirrored under OUT-FOLDER\n"
        "  --skip-unchanged   keep Markdown files at least as new as their PDF\n"
        "  --report[=FILE]    one JSON record a document, in input order (standard output\n"
        "                     without FILE)\n"
        "\n"
        "--serve --json reads requests {\"id\":\"1\",\"input\":\"a.pdf\",\"output\":\"a.md\",\"args\":[...]}\n"
        "(\"output\" left out: the answer carries the Markdown, or with --format=json the JSON as\n"
        "\"json\") and answers each with its record, in request order, after a first line\n"
        "{\"rayopdf\":\"VERSION\",\"protocol\":1}; quit ends it. --batch writes NAME.json with\n"
        "--format=json.\n"
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

void PrintError(const std::string& message) {
    File::WriteStandardError("rayopdf: " + message + "\n");
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
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

// The flags of one conversion: those of --convert, a batch's for each document, and a serve
// session's with each request's `args` over them.
struct DocumentOptions {
    std::string password;
    Layout::ConvertOptions convert;  // pages and annotations; ChooseImageTarget sets the images
    std::string images;
    bool imagesGiven = false;
};

enum class FlagStatus { Taken, Unknown, Invalid };

// One flag of a conversion into `options`; Invalid with `error` for a known flag with a bad value.
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
        } else {
            error = "--format takes markdown or json: " + std::string(format);
            return FlagStatus::Invalid;
        }
    } else if (arg == "--page-markers") {
        options.convert.pageMarkers = true;
    } else if (arg == "--front-matter") {
        options.convert.frontMatter = true;
    } else {
        return FlagStatus::Unknown;
    }
    return FlagStatus::Taken;
}

// The directory part of an absolute path ("/a/b" for "/a/b/c.md", "C:/" for "C:/c.md").
std::string Parent(const std::string& absolute) {
    const size_t root = absolute.size() >= 3 && absolute[1] == ':' && absolute[2] == '/' ? 3 : 1;
    const size_t slash = absolute.rfind('/');
    return slash == std::string::npos || slash < root ? absolute.substr(0, root) : absolute.substr(0, slash);
}

bool SamePart(std::string_view a, std::string_view b) {
    if (!kWindows) return a == b;
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// `target` relative to the directory `from` (both absolute, '/'-separated), "" when they are the
// same, with a trailing '/'. On another root (a Windows drive) `target` itself.
std::string Relative(const std::string& from, const std::string& target) {
    std::vector<std::string_view> a, b;
    for (const std::string* path : {&from, &target}) {
        std::vector<std::string_view>& parts = path == &from ? a : b;
        size_t at = 0;
        while (at < path->size()) {
            size_t next = path->find('/', at);
            if (next == std::string::npos) next = path->size();
            if (next > at) parts.emplace_back(path->data() + at, next - at);
            at = next + 1;
        }
    }
    const bool rooted = !from.empty() && from[0] == '/';
    if (rooted != (!target.empty() && target[0] == '/') ||
        (!rooted && (a.empty() || b.empty() || !SamePart(a[0], b[0])))) {
        return target.back() == '/' ? target : target + "/";
    }
    size_t common = 0;
    while (common < a.size() && common < b.size() && SamePart(a[common], b[common])) ++common;
    std::string out;
    for (size_t i = common; i < a.size(); ++i) out += "../";
    for (size_t i = common; i < b.size(); ++i) {
        out += b[i];
        out += '/';
    }
    return out;
}

// Where images go and how the Markdown links them (D6): by default OUT-images/ beside OUT.md;
// --images=none or Markdown on standard output (or in a serve answer, `output` empty): none;
// --images=DIR: DIR, linked relative to OUT.md (to the working directory for "-" and "").
void ChooseImageTarget(const std::string& output, const DocumentOptions& document, Layout::ConvertOptions& options) {
    options.imageDirectory.clear();
    options.imageLinkPrefix.clear();
    const bool inline_ = output.empty() || output == "-";
    if ((document.imagesGiven && document.images == "none") || (!document.imagesGiven && inline_)) return;
    std::string directory;
    if (document.imagesGiven) {
        directory = document.images;
    } else {
        directory = output;
        const size_t slash = directory.find_last_of(kWindows ? "/\\" : "/");
        const size_t dot = directory.rfind('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) directory.resize(dot);
        directory += "-images";
    }
    const std::string from = inline_ ? File::AbsolutePath(".") : Parent(File::AbsolutePath(output));
    options.imageDirectory = directory;
    options.imageLinkPrefix = Relative(from, File::AbsolutePath(directory));
}

int Inspect(const std::string& input, std::string_view password) {
    File::InputFile file;
    std::string error;
    std::unique_ptr<Document> document;
    int code = static_cast<int>(Api::ReadInput(input, file, error));
    if (code == kExitOk) code = static_cast<int>(Api::OpenDocument(input, file.Bytes(), password, document, error));
    if (code != kExitOk) {
        PrintError(error);
        return code;
    }

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
    // One unchecked write(2): stdio's first write to stdout would also fstat it and allocate its
    // buffer, which a fresh process pays for (--inspect is timed fresh-process).
    File::WriteAll("-", json);
    return kExitOk;
}

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// glibc's allocator for one conversion in a process of its own (--convert): blocks of up to 1 MiB
// from the heap rather than each from a mapping of its own (the threshold starts at 128 KiB), so a
// freed block's pages are used again instead of being unmapped and faulted in afresh: 4.3 % fewer
// page faults over odl, 313 -> 263 at the 95th percentile. Larger blocks (decoded images) are
// still mapped and given back when freed, so peak memory stays as it was. The heap grows 8 MiB
// of address space at a time rather than by what each request needs: a page thread's heap, which
// glibc otherwise makes writable a few pages at a time (one mprotect each), is writable from the
// start (a 54-page document at the automatic thread count: 151 -> 8 mprotect calls, 25 -> 6 brk;
// the 27 documents of the pages measurement: 3.7 % fewer page faults, 1 % more on one thread).
// Pages nothing writes to are never resident. --batch and --bench have settings of their own
// (SetBatchAllocator); --serve and --inspect have the C library's.
void SetOneShotAllocator() {
#if defined(__GLIBC__)
    mallopt(M_MMAP_THRESHOLD, 1 << 20);
    mallopt(M_TOP_PAD, 8 << 20);
#endif
}

// What a conversion reads: the input's bytes and the document over them.
struct Source {
    File::InputFile file;
    std::unique_ptr<Document> document;
};

// Converts `input` into `markdown` and writes it to `output` ("-": standard output; "": not
// written, the caller takes `markdown`). `record` gets the outcome: code, error, pages, bytes and,
// when `counters`, the counters (ms is the caller's). `source` holds the input and the document
// until the caller drops it. `inputBuffer`: a buffer the caller keeps from document to document
// for small inputs (the batch modes' workers), or null.
void ConvertSource(const std::string& input, const std::string& output, std::string_view password,
                   const Layout::ConvertOptions& options, std::string& markdown, Batch::Record& record,
                   bool counters, Source& source, std::string* inputBuffer) {
    int code = static_cast<int>(Api::ReadInput(input, source.file, record.error, inputBuffer));
    if (code == kExitOk) {
        code = static_cast<int>(Api::OpenDocument(input, source.file.Bytes(), password, source.document, record.error));
    }
    if (code != kExitOk) {
        record.status = "error";
        record.code = code;
        return;
    }
    Layout::ConvertReport report;
    Layout::Convert(*source.document, options, markdown, report);
    if (!output.empty() && !File::WriteAll(output, markdown)) {
        record.status = "error";
        record.code = kExitUnwritable;
        record.error = "cannot write " + output;
        return;
    }
    record.pages = static_cast<uint64_t>(report.pages);
    record.bytes = markdown.size();
    if (counters) Api::AppendCounters(record.counters, report, source.document->Report());
}

// ConvertSource with a source of its own, dropped when the conversion is done.
void ConvertDocument(const std::string& input, const std::string& output, std::string_view password,
                     const Layout::ConvertOptions& options, std::string& markdown, Batch::Record& record,
                     bool counters, std::string* inputBuffer = nullptr) {
    Source source;
    ConvertSource(input, output, password, options, markdown, record, counters, source, inputBuffer);
}

// --convert: one conversion in a process of its own, which main ends with _Exit when this
// returns. The input's mapping, the document and the Markdown are never taken apart: the kernel
// takes the address space back whole at the end, so the unmapping of the input and every free
// would be work for nothing.
int ConvertFile(const std::string& input, const std::string& output, const DocumentOptions& document,
                const Layout::ConvertOptions& options, bool report) {
    const Clock::time_point start = report ? Clock::now() : Clock::time_point();
    std::string& markdown = *new std::string();
    Batch::Record record;
    ConvertSource(input, output, document.password, options, markdown, record, report, *new Source(), nullptr);
    if (record.code != kExitOk) PrintError(record.error);
    if (report) {
        record.input = input;
        record.output = output;
        record.milliseconds = MillisecondsSince(start);
        std::string line;
        Batch::AppendRecord(line, record);
        if (output == "-") {
            File::WriteStandardError(line);
        } else {
            File::WriteStandardOutput(line);
        }
    }
    return record.code;
}

// --bench: the warm loop of the performance campaign. One untimed conversion, then `runs` timed
// ones with the same Markdown and input buffers, as a worker of the batch modes converts one
// document after another (their allocator settings too, SetBatchAllocator), each the whole path
// of a conversion (read, open, convert, write the Markdown and the images). Prints
// `runs=N best_ms=B median_ms=M pages=P`.
int Bench(const std::string& input, const std::string& folder, std::string_view count, const DocumentOptions& document) {
    int runs = 0;
    if (!ParsePage(count, runs)) return Usage("--bench needs a run count of at least 1: ", count);
    if (!File::EnsureDirectory(folder)) {
        PrintError("cannot create the folder " + folder);
        return kExitUnwritable;
    }
    const std::string output = folder + (folder.empty() || folder.back() == '/' ? "out.md" : "/out.md");
    Layout::ConvertOptions options = document.convert;
    ChooseImageTarget(output, document, options);
    std::string markdown;
    std::string inputBuffer;
    std::vector<double> times;
    times.reserve(static_cast<size_t>(runs));
    uint64_t pages = 0;
    for (int run = 0; run <= runs; ++run) {
        const Clock::time_point start = Clock::now();
        Batch::Record record;
        ConvertDocument(input, output, document.password, options, markdown, record, false, &inputBuffer);
        if (record.code != kExitOk) {
            PrintError(record.error);
            return record.code;
        }
        if (run > 0) times.push_back(MillisecondsSince(start));
        pages = record.pages;
    }
    std::sort(times.begin(), times.end());
    char line[160];
    std::snprintf(line, sizeof line, "runs=%d best_ms=%.6f median_ms=%.6f pages=%" PRIu64 "\n", runs, times.front(),
                  times[times.size() / 2], pages);
    File::WriteStandardOutput(line);
    return kExitOk;
}

// Flags of the batch modes.
struct BatchOptions {
    bool recursive = false;
    bool skipUnchanged = false;
    bool report = false;
    std::string reportPath = "-";
    unsigned workers = 0;  // 0: automatic
    bool workersGiven = false;
};

std::string Join(const std::string& folder, std::string_view name) {
    std::string out = folder;
    if (!out.empty() && out.back() != '/' && !(kWindows && out.back() == '\\')) out += '/';
    out += name;
    return out;
}

bool IsPdfName(std::string_view name) {
    if (name.size() < 5) return false;
    const std::string_view extension = name.substr(name.size() - 4);
    return extension[0] == '.' && (extension[1] | 0x20) == 'p' && (extension[2] | 0x20) == 'd' &&
           (extension[3] | 0x20) == 'f';
}

std::string Lowered(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

struct BatchJob {
    std::string input;
    std::string output;
    uint64_t modified = 0;
    Batch::Record record;
};

// The PDFs below `folder` as paths relative to it ('/'-separated), sorted by their UTF-8 bytes:
// with `recursive`, subfolders too, except hidden ones and links to folders. False, with `error`,
// when a folder cannot be read or the tree is too large.
bool CollectPdfs(const std::string& folder, bool recursive, std::vector<std::string>& files, std::string& error) {
    std::vector<std::pair<std::string, int>> pending{{std::string(), 0}};
    std::vector<File::DirectoryEntry> entries;
    while (!pending.empty()) {
        const std::string relative = pending.back().first;
        const int depth = pending.back().second;
        pending.pop_back();
        const std::string path = relative.empty() ? folder : Join(folder, relative);
        if (!File::ListDirectory(path, entries)) {
            error = "cannot read the folder " + path;
            return false;
        }
        for (const File::DirectoryEntry& entry : entries) {
            const std::string name = relative.empty() ? entry.name : relative + "/" + entry.name;
            if (entry.type == File::FileType::Directory) {
                if (recursive && !entry.link && entry.name[0] != '.' && depth < kMaxFolderDepth) {
                    pending.emplace_back(name, depth + 1);
                }
            } else if (entry.type == File::FileType::Regular && IsPdfName(entry.name)) {
                if (files.size() == kMaxBatchFiles) {
                    error = "more than " + std::to_string(kMaxBatchFiles) + " PDF files below " + folder;
                    return false;
                }
                files.push_back(name);
            }
        }
    }
    std::sort(files.begin(), files.end());
    return true;
}

void BatchDocument(BatchJob& job, const DocumentOptions& document, const BatchOptions& batch, std::string& markdown,
                   std::string& inputBuffer, Layout::OcrSession& ocr) {
    const Clock::time_point start = Clock::now();
    Batch::Record& record = job.record;
    record.input = job.input;
    record.output = job.output;
    if (record.code != kExitOk) return;  // refused before the workers started
    if (batch.skipUnchanged) {
        File::FileInfo in, out;
        if (File::StatusOf(job.input, in) && File::StatusOf(job.output, out) && out.type == File::FileType::Regular &&
            out.modified >= in.modified) {
            record.status = "skipped";
            record.bytes = out.size;
            return;
        }
    }
    Layout::ConvertOptions options = document.convert;
    ChooseImageTarget(job.output, document, options);
    options.ocr.session = &ocr;
    ConvertDocument(job.input, job.output, document.password, options, markdown, record, batch.report, &inputBuffer);
    record.milliseconds = MillisecondsSince(start);
}

// glibc's allocator for a run of documents (--batch, and --bench, which measures what a batch
// worker does): blocks of up to 32 MiB from the heap rather than each from a mapping of its own,
// and freed memory at the heap's top kept up to 256 MiB rather than given back to the kernel
// after each document, so that the next document does not map and fault the same pages in again
// (odl: 30 % fewer page faults; scanned pages with images written: 71 % less system time). Peak
// memory stays that of the largest document. Not for --serve: over 1,000 documents its time was
// the same within noise and the process held 705 MiB rather than 525 MiB at the end.
void SetBatchAllocator() {
#if defined(__GLIBC__)
    mallopt(M_MMAP_THRESHOLD, 32 << 20);
    mallopt(M_TRIM_THRESHOLD, 256 << 20);
#endif
}

int RunBatch(const std::string& inputFolder, const std::string& outputFolder, const DocumentOptions& document,
             const BatchOptions& batch) {
    File::FileInfo info;
    if (!File::StatusOf(inputFolder, info) || info.type != File::FileType::Directory) {
        PrintError("the input folder is not a readable directory: " + inputFolder);
        return kExitUnreadable;
    }
    if (!File::EnsureDirectory(outputFolder)) {
        PrintError("cannot create the output folder " + outputFolder);
        return kExitUnwritable;
    }
    std::vector<std::string> files;
    std::string error;
    if (!CollectPdfs(inputFolder, batch.recursive, files, error)) {
        PrintError(error);
        return kExitUnreadable;
    }

    // Jobs in input order. Output subfolders are made here, before the workers start, and two
    // inputs whose Markdown would be the same file (a.pdf, a.PDF) leave the second refused.
    std::vector<BatchJob> jobs(files.size());
    std::set<std::string> outputs;
    std::set<std::string> folders;
    for (size_t i = 0; i < files.size(); ++i) {
        BatchJob& job = jobs[i];
        const std::string& relative = files[i];
        job.input = Join(inputFolder, relative);
        job.output = Join(outputFolder, std::string_view(relative).substr(0, relative.size() - 4)) +
                     (document.convert.format == Layout::OutputFormat::Json ? ".json" : ".md");
        if (!outputs.insert(kWindows ? Lowered(job.output) : job.output).second) {
            job.record.status = "error";
            job.record.code = kExitUnwritable;
            job.record.error = "another input has the same output " + job.output;
            continue;
        }
        for (size_t slash = relative.find('/'); slash != std::string::npos; slash = relative.find('/', slash + 1)) {
            const std::string folder = Join(outputFolder, std::string_view(relative).substr(0, slash));
            if (!folders.insert(folder).second) continue;
            if (!File::EnsureDirectory(folder)) {
                job.record.status = "error";
                job.record.code = kExitUnwritable;
                job.record.error = "cannot create the output folder " + folder;
                break;
            }
        }
    }

    Batch::OrderedOutput report;
    if (batch.report && !report.Open(batch.reportPath, false)) {
        PrintError("cannot create the report file " + batch.reportPath);
        return kExitUnwritable;
    }
    const unsigned workers = Batch::WorkerCount(batch.workers, jobs.size());
    // A Markdown buffer, an input buffer and an OCR add-on session per worker, reused from
    // document to document.
    std::vector<std::string> buffers(workers);
    std::vector<std::string> inputBuffers(workers);
    std::vector<std::unique_ptr<Layout::OcrSession>> sessions(workers);
    for (std::unique_ptr<Layout::OcrSession>& s : sessions) s.reset(new Layout::OcrSession(document.convert.ocr));
    Batch::RunJobs(jobs.size(), workers, [&](unsigned worker, size_t index) {
        BatchDocument(jobs[index], document, batch, buffers[worker], inputBuffers[worker], *sessions[worker]);
        if (report.IsOpen()) {
            std::string line;
            Batch::AppendRecord(line, jobs[index].record);
            report.Put(index, std::move(line));
        }
    });
    sessions.clear();  // the add-ons quit
    const bool reported = !batch.report || report.Close();

    size_t failures = 0;
    int last = kExitOk;
    for (const BatchJob& job : jobs) {
        if (job.record.code == kExitOk) continue;
        ++failures;
        last = job.record.code;
        PrintError(job.record.error);
    }
    if (!reported) PrintError("cannot write the report file " + batch.reportPath);
    if (failures != 0) {
        PrintError("the batch failed for " + std::to_string(failures) + " of " + std::to_string(jobs.size()) +
                   " file(s)");
        return last;
    }
    return reported ? kExitOk : kExitUnwritable;
}

// One serve request into `record`: the session's flags with the request's `args` over them. `ocr`
// is the worker's add-on, used unless the request names another one.
void ServeRequest(const Serve::Request& request, const DocumentOptions& session, std::string& markdown,
                  Batch::Record& record, Layout::OcrSession& ocr) {
    const Clock::time_point start = Clock::now();
    record.input = request.input;
    record.output = request.hasOutput ? request.output : std::string("-");
    DocumentOptions document = session;
    for (const std::string& arg : request.args) {
        std::string error;
        const FlagStatus status = ParseDocumentFlag(arg, document, error);
        if (status == FlagStatus::Taken) continue;
        record.status = "error";
        record.code = kExitUsage;
        record.error = status == FlagStatus::Invalid ? error : "not a flag of a serve request: " + arg;
        return;
    }
    Layout::ConvertOptions options = document.convert;
    ChooseImageTarget(request.hasOutput ? request.output : std::string(), document, options);
    if (options.ocr.addon == session.convert.ocr.addon) options.ocr.session = &ocr;
    ConvertDocument(request.input, request.hasOutput ? request.output : std::string(), document.password, options,
                    markdown, record, true);
    if (record.code == kExitOk && !request.hasOutput) {
        if (options.format == Layout::OutputFormat::Json) {
            record.hasJson = true;
            record.json.assign(markdown, 0, markdown.empty() ? 0 : markdown.size() - 1);  // without its line break
        } else {
            record.hasMarkdown = true;
            record.markdown = markdown;
        }
    }
    record.milliseconds = MillisecondsSince(start);
}

std::string ServeError(const std::string& id, const std::string& error) {
    Batch::Record record;
    record.id = id;
    record.status = "error";
    record.code = kExitUsage;
    record.error = error;
    std::string line;
    Batch::AppendRecord(line, record);
    return line;
}

size_t ReadStandardInput(void*, char* buffer, size_t size) {
    return File::ReadStandardInput(buffer, size);
}

// Requests waiting for a serve worker, at most `capacity`; the reader waits while it is full.
struct ServeQueue {
    struct Task {
        size_t index;
        Serve::Request request;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> tasks;
    size_t capacity = 1;
    bool closed = false;
};

// --serve --json: protocol 1, one JSON request a line and one record a request, in request order.
// Standard output carries the protocol only.
int RunServe(const DocumentOptions& session, unsigned workers) {
    const std::string hello = "{\"rayopdf\":\"" RAYOPDF_VERSION "\",\"protocol\":" + std::to_string(Serve::kProtocol) +
                              "}\n";
    if (!File::WriteStandardOutput(hello)) return kExitUnwritable;
    Batch::OrderedOutput answers;
    answers.Open("-", true);

    ServeQueue queue;
    queue.capacity = size_t(2) * workers;
    std::vector<std::thread> threads;
    const auto work = [&] {
        std::string markdown;
        Layout::OcrSession ocr(session.convert.ocr);
        for (;;) {
            ServeQueue::Task task;
            {
                std::unique_lock<std::mutex> lock(queue.mutex);
                queue.changed.wait(lock, [&] { return queue.closed || !queue.tasks.empty(); });
                if (queue.tasks.empty()) return;
                task = std::move(queue.tasks.front());
                queue.tasks.pop_front();
            }
            queue.changed.notify_all();
            Batch::Record record;
            record.id = task.request.id;
            ServeRequest(task.request, session, markdown, record, ocr);
            std::string line;
            Batch::AppendRecord(line, record);
            answers.Put(task.index, std::move(line));
        }
    };
    if (workers > 1) {
        threads.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) threads.emplace_back(work);
    }

    Serve::LineReader lines(ReadStandardInput, nullptr);
    Serve::Request request;
    std::string line;
    std::string error;
    std::string markdown;
    Layout::OcrSession ocr(session.convert.ocr);
    size_t index = 0;
    while (answers.Healthy()) {
        const Serve::LineReader::Status status = lines.Next(line, Serve::kMaxRequestBytes);
        if (status == Serve::LineReader::Status::End) break;
        if (status == Serve::LineReader::Status::TooLong) {
            answers.Put(index++, ServeError(std::string(),
                                            "request line exceeds " + std::to_string(Serve::kMaxRequestBytes) + " bytes"));
            continue;
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line.size() < 16) {
            const size_t last = line.find_last_not_of(" \t");
            const std::string_view word(line.data() + first, last + 1 - first);
            if (word == "quit" || word == "exit") break;
        }
        if (!Serve::ParseRequest(line, request, error)) {
            answers.Put(index++, ServeError(request.id, error));
            continue;
        }
        if (workers <= 1) {
            Batch::Record record;
            record.id = request.id;
            ServeRequest(request, session, markdown, record, ocr);
            std::string answer;
            Batch::AppendRecord(answer, record);
            answers.Put(index++, std::move(answer));
            continue;
        }
        {
            std::unique_lock<std::mutex> lock(queue.mutex);
            queue.changed.wait(lock, [&] { return queue.tasks.size() < queue.capacity; });
            queue.tasks.push_back({index++, std::move(request)});
        }
        queue.changed.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        queue.closed = true;
    }
    queue.changed.notify_all();
    for (std::thread& thread : threads) thread.join();
    return answers.Close() ? kExitOk : kExitUnwritable;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = File::Arguments(argc, argv);
    if (args.size() < 2) {
        PrintUsage(stderr);
        return kExitUsage;
    }
    enum class Mode { None, Convert, Inspect, Batch, Serve, Bench } mode = Mode::None;
    std::string input;
    std::string output;
    DocumentOptions document;
    std::string benchCount;
    BatchOptions batch;
    bool report = false;
    bool json = false;
    const char* kModes = "only one of --convert, --batch, --serve, --inspect and --bench";

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        std::string error;
        if (arg == "--help" || arg == "-h") {
            PrintUsage(stdout);
            return kExitOk;
        }
        if (arg == "--version" || arg == "-V") {
            std::fputs("rayopdf " RAYOPDF_VERSION "\n", stdout);
            return kExitOk;
        }
        if (arg == "--convert") {
            if (mode != Mode::None) return Usage(kModes);
            if (i + 2 >= args.size()) return Usage("--convert needs IN.pdf and OUT.md");
            mode = Mode::Convert;
            input = args[++i];
            output = args[++i];
        } else if (arg == "--batch") {
            if (mode != Mode::None) return Usage(kModes);
            if (i + 2 >= args.size()) return Usage("--batch needs IN-FOLDER and OUT-FOLDER");
            mode = Mode::Batch;
            input = args[++i];
            output = args[++i];
        } else if (arg == "--serve") {
            if (mode != Mode::None) return Usage(kModes);
            mode = Mode::Serve;
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--inspect") {
            if (mode != Mode::None) return Usage(kModes);
            if (i + 1 >= args.size()) return Usage("--inspect needs IN.pdf");
            mode = Mode::Inspect;
            input = args[++i];
        } else if (arg == "--bench") {
            if (mode != Mode::None) return Usage(kModes);
            if (i + 3 >= args.size()) return Usage("--bench needs IN.pdf, TMPDIR and N");
            mode = Mode::Bench;
            input = args[++i];
            output = args[++i];
            benchCount = args[++i];
        } else if (arg == "--report") {
            report = true;
        } else if (StartsWith(arg, "--report=")) {
            report = true;
            batch.reportPath = std::string(arg.substr(9));
            if (batch.reportPath.empty()) return Usage("--report= needs a file name, or - for standard output");
        } else if (StartsWith(arg, "--workers=")) {
            int count = 0;
            if (!ParsePage(arg.substr(10), count) || count > static_cast<int>(Batch::kMaxWorkers)) {
                return Usage("--workers must be between 1 and 64: ", arg.substr(10));
            }
            batch.workers = static_cast<unsigned>(count);
            batch.workersGiven = true;
        } else if (arg == "--recursive") {
            batch.recursive = true;
        } else if (arg == "--skip-unchanged") {
            batch.skipUnchanged = true;
        } else {
            switch (ParseDocumentFlag(arg, document, error)) {
                case FlagStatus::Taken: break;
                case FlagStatus::Invalid: return Usage(error.c_str());
                case FlagStatus::Unknown: return Usage("unknown argument: ", arg);
            }
        }
    }
    if (json && mode != Mode::Serve) return Usage("--json goes with --serve");
    if (mode != Mode::Batch && (batch.recursive || batch.skipUnchanged || batch.reportPath != "-")) {
        return Usage("--recursive, --skip-unchanged and --report=FILE apply to --batch");
    }
    if (mode == Mode::Inspect && batch.workersGiven) return Usage("--workers does not apply to --inspect");
    // --convert: the pages of a document at once; --bench too when asked (layout/convert.h).
    if (mode == Mode::Convert) document.convert.pageWorkers = batch.workersGiven ? batch.workers : 0;
    if (mode == Mode::Bench && batch.workersGiven) document.convert.pageWorkers = batch.workers;
    if ((mode == Mode::Batch || mode == Mode::Serve) && document.imagesGiven && document.images != "none") {
        return Usage("--images=DIR applies to one document: --batch and --serve take --images=none");
    }
    switch (mode) {
        case Mode::Inspect: return Inspect(input, document.password);
        case Mode::Convert: {
            SetOneShotAllocator();
            Layout::ConvertOptions options = document.convert;
            options.oneShot = true;
            ChooseImageTarget(output, document, options);
            const int code = ConvertFile(input, output, document, options, report);
            // No exit handlers, static destructors or frees after the output is written (ConvertFile).
            std::fflush(nullptr);
            std::_Exit(code);
        }
        case Mode::Batch:
            batch.report = report;
            SetBatchAllocator();
            return RunBatch(input, output, document, batch);
        case Mode::Serve:
            if (!json) return Usage("--serve needs --json");
            if (report) return Usage("--serve --json takes no --report: its answers are the records");
            return RunServe(document, batch.workersGiven ? batch.workers : 1);
        case Mode::Bench:
            SetBatchAllocator();
            return Bench(input, output, benchCount, document);
        case Mode::None: break;
    }
    return Usage("nothing to do: give --convert, --batch, --serve --json or --inspect");
}
