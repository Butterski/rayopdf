// SPDX-License-Identifier: Apache-2.0
// The optional external backend of the OCR add-on (`rayopdf-ocr --engine tesseract`): every request
// starts the `tesseract` found on PATH (or given), hands it the image as a binary PGM on standard
// input and reads its TSV output from standard output:
//
//   tesseract stdin stdout -l <lang> --psm <3 page | 7 line> [--dpi <dpi>] tsv
//
// with OMP_THREAD_LIMIT=1 (single-threaded, deterministic). Tesseract is Apache-2.0 and is not
// linked: it is a separate program the user installs. TSV level-5 rows are words; the words of one
// (page, block, paragraph, line) become a line whose box is their union, whose text is the words
// joined by one space, and whose confidence is the mean word confidence / 100. Tesseract gives no
// character positions: each word's code points share its box evenly, and a space spans the gap
// between two words.
#pragma once

#include "common/ocr_protocol.h"
#include "ocr/engine.h"

#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Ocr {

// Output of one tesseract run read at most (a page's TSV is some kilobytes).
constexpr size_t kMaxTesseractOutput = size_t(64) << 20;

// TSV of `tesseract ... tsv` into lines (replaced). False when the header row is missing.
bool ParseTesseractTsv(std::string_view tsv, std::vector<OcrProtocol::Line>& lines);

enum class ProcessStatus : uint8_t { Ok, NotStarted, Failed, TooLarge };

// Runs `argv` (argv[0] looked up on PATH unless it holds a separator) with `input` on its standard
// input and its standard output into `output`, standard error discarded, OMP_THREAD_LIMIT=1 in its
// environment. Ok when it exited with status 0.
ProcessStatus RunProcess(const std::vector<std::string>& argv, std::string_view input, std::string& output);

class TesseractEngine final : public Engine {
public:
    TesseractEngine(std::string program, std::string language)
        : program_(std::move(program)), language_(std::move(language)) {}
    // The first line of `tesseract --version`, or false when it cannot run.
    bool Probe(std::string& version);
    EngineStatus Page(const GrayView& page, uint32_t dpi, std::vector<OcrProtocol::Line>& lines) override;
    EngineStatus Line(const GrayView& image, OcrProtocol::Line& line) override;

private:
    EngineStatus Run(const GrayView& image, uint32_t dpi, const char* psm, std::vector<OcrProtocol::Line>& lines);

    std::string program_, language_;
    std::string input_, output_;
    std::vector<OcrProtocol::Line> lines_;
};

} // namespace RayoPdf::Ocr
