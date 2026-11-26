// SPDX-License-Identifier: Apache-2.0
// RayoPDF's C++ API: PDF to Markdown (or the JSON document) in the calling process, with the
// command line's conversion (`rayopdf --convert`). Link librayopdf.a and the C++ runtime's threads:
//
//   g++ -std=c++17 example.cpp -Iinclude lib/librayopdf.a -pthread
//
// The functions keep no state between calls and may run on several threads at once. The library
// is built without exceptions: a function reports failure in Result::status, never by throwing.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::Api {

// What is written: Markdown, the document as one JSON object (schema 1: its metadata and its
// blocks with their pages, boxes and Markdown; `rayopdf --format=json`), chunks for retrieval as
// JSON Lines (`rayopdf --format=chunks`), the layout inspector's HTML (`--format=inspect`), or
// plain text without markup, pages separated by form feeds (`--format=text`).
enum class Format : uint8_t { Markdown, Json, Chunks, Inspect, Text };
// Formulas as Unicode text, or rebuilt as LaTeX between $ and $$.
enum class Math : uint8_t { Unicode, Latex };
// OCR through the rayopdf-ocr add-on: never; where the text layer fails (when the add-on is found);
// every page image and large image.
enum class Ocr : uint8_t { Off, Auto, Force };
// Page furniture (running headers, footers, page numbers): left out, or kept as text.
enum class Furniture : uint8_t { Drop, Keep };
// Tables as GFM pipe tables unless a span needs HTML, every table as HTML, every table as a GFM
// pipe table (spans flattened), or as one whose cells' `%`, `#` and `&` outside math are escaped
// for a LaTeX reader (the text `\%`, `\#`, `\&`: READoc's convention).
enum class Tables : uint8_t { Auto, Html, Pipe, PipeLatex };
// Quotation marks as printed, or the typographic ones (U+2018-U+201F) as ' and ".
enum class Quotes : uint8_t { Keep, Ascii };
// Headings set in small capitals in the source's case (small capitals as lower-case letters), or
// as printed (every letter a capital).
enum class SmallCaps : uint8_t { Lower, Keep };
// A figure's own text (tick labels, legends, the labels of a diagram) kept as paragraphs, as
// printed, or left out (captions and sentences near a figure stay).
enum class FigureText : uint8_t { Keep, Drop };
// What the JSON document gives besides each block's boxes: nothing, each block's lines (page, box,
// text), or each block's lines with their words (box, text).
enum class JsonDetail : uint8_t { Blocks, Lines, Words };

// The command line's exit code for the same outcome.
enum class Status : int {
    Ok = 0,
    Unreadable = 3,             // the file cannot be read, or is larger than 4 GiB
    Corrupt = 10,               // not a PDF, corrupt beyond repair, or past a document limit
    PasswordRequired = 11,      // encrypted, and the password does not open it
    Unwritable = 12,            // an output could not be written
    UnsupportedEncryption = 13, // encrypted with a handler RayoPDF does not implement
};

struct Options {
    std::string password;                    // user or owner password of an encrypted PDF
    int firstPage = 1, lastPage = 0;         // 1-based, inclusive; 0: the last page
    Format format = Format::Markdown;
    Math math = Math::Unicode;
    Ocr ocr = Ocr::Off;                      // Auto finds the add-on as the CLI does
    std::string ocrAddon;                    // the add-on to run; empty: $RAYOPDF_OCR, beside the program, PATH
    bool annotations = true;                 // link annotations become Markdown links
    bool structure = true;                   // a tagged PDF's structure tree is used when it passes the checks
    bool exact = true;                       // a verified RayoMD source payload is written as it is
    bool pageMarkers = false;                // Markdown: <!-- page N --> before each page's blocks
    bool frontMatter = false;                // Markdown: YAML front matter from the document's information
    Furniture furniture = Furniture::Drop;   // page furniture left out or kept
    bool plain = false;                      // no inline markup (emphasis, code spans, links, <sup>, [^n])
    Tables tables = Tables::Auto;            // Html: every table as HTML; Pipe: every table as GFM; PipeLatex: LaTeX-escaped
    Quotes quotes = Quotes::Keep;            // Ascii: typographic quotation marks as ' and "
    SmallCaps smallCaps = SmallCaps::Lower;  // Keep: small-capitals headings as printed, in capitals
    FigureText figureText = FigureText::Keep;  // Drop: a figure's own text left out
    uint32_t chunkChars = 4000;              // Chunks: Markdown bytes a chunk, 256 to 1,000,000 (clamped)
    JsonDetail jsonDetail = JsonDetail::Blocks;  // Json: each block's lines, or lines and words, too
    std::string imageDirectory, imageLinkPrefix;  // empty directory: no images
    unsigned workers = 1;                    // page threads, 0: automatic
};

struct Result {
    Status status = Status::Ok;
    std::string error;                       // why it failed, for a status other than Ok
    int pages = 0;                           // pages converted
    std::string report;                      // the --report counters as one JSON object
};

// Converts the PDF in `pdf` into `output` (replaced). `output` is left empty on failure.
Result ConvertMemory(std::string_view pdf, const Options& options, std::string& output);
// The same for the PDF file at `path` (UTF-8).
Result ConvertFile(const std::string& path, const Options& options, std::string& output);
// The library's version, "MAJOR.MINOR.PATCH".
const char* Version();

} // namespace RayoPdf::Api
