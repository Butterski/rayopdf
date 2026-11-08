// SPDX-License-Identifier: Apache-2.0
// The engines of the OCR add-on rayopdf-ocr behind one interface: an Engine turns a gray page into
// lines of text (detect + recognise), or one line image into its text. PpOcrEngine runs PP-OCRv6's
// detection and recognition networks through a Runtime (ncnn or ONNX Runtime, src/ocr/addon/) and
// does all pre- and post-processing itself; TesseractEngine (tesseract.h) runs an external
// `tesseract`.
//
// PpOcrEngine, one page:
//  1. Detection input: the page resampled so that its longer side is at most `detMaxSide` (never
//     enlarged), each side rounded to a multiple of 32 (at least 32), normalised with ImageNet's
//     mean and deviation; the network returns a text probability map of the same size.
//  2. DetectBoxes (detect.h) on that map, boxes scaled back to page pixels, SortIntoLines.
//  3. Every box is cut out of the full-resolution page (SampleRect, at its own angle); a crop at
//     least 1.5 times as tall as wide is turned a quarter counter-clockwise (vertical text).
//  4. Recognition input: the crop resampled to height 48 (width in proportion, at most
//     kMaxRecWidth), padded to at least 320 columns as PaddleOCR does, normalised to [-1, 1];
//     DecodeCtc + InsertSpaces + AssembleText give text, confidence and character spans, mapped
//     back onto the page's x axis.
// Lines with no text are left out. At most kMaxPageLines lines per page. Inference is single-
// threaded (the runtimes are set up so), and every step is deterministic: the same page gives the
// same lines on every run on a machine. Buffers are kept between calls.
#pragma once

#include "common/ocr_protocol.h"
#include "ocr/detect.h"
#include "ocr/raster.h"
#include "ocr/recognize.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Ocr {

constexpr uint32_t kRecHeight = 48;
constexpr uint32_t kMaxRecWidth = 3200;
constexpr uint32_t kMaxPageLines = 4096;
constexpr uint32_t kDefaultDetMaxSide = 1280;
constexpr uint32_t kMaxDetSide = 4096;

enum class RuntimeStatus : uint8_t { Ok, Failed };

// One inference runtime with a detection and a recognition network loaded.
class Runtime {
public:
    virtual ~Runtime() = default;
    // Input 1x3xHxW planes; output H x W probabilities (replaced).
    virtual RuntimeStatus Detect(const float* planes, uint32_t width, uint32_t height, std::vector<float>& prob) = 0;
    // Input 1x3x48xW planes; output steps x classes probabilities (replaced).
    virtual RuntimeStatus Recognize(const float* planes, uint32_t width, std::vector<float>& probs, uint32_t& steps,
                                    uint32_t& classes) = 0;
};

enum class EngineStatus : uint8_t { Ok, Failed };

class Engine {
public:
    virtual ~Engine() = default;
    // The lines of a page (replaced), boxes and spans in its pixels, in reading order.
    virtual EngineStatus Page(const GrayView& page, uint32_t dpi, std::vector<OcrProtocol::Line>& lines) = 0;
    // The text of one line image; its box is the whole image.
    virtual EngineStatus Line(const GrayView& image, OcrProtocol::Line& line) = 0;
};

struct PpOcrSettings {
    DetectParams detect;
    uint32_t detMaxSide = kDefaultDetMaxSide;
    float spaceThreshold = kDefaultSpaceThreshold;  // InsertSpaces (recognize.h)
};

class PpOcrEngine final : public Engine {
public:
    PpOcrEngine(Runtime& runtime, const Dictionary& dictionary, const PpOcrSettings& settings)
        : runtime_(runtime), dictionary_(dictionary), settings_(settings) {}
    EngineStatus Page(const GrayView& page, uint32_t dpi, std::vector<OcrProtocol::Line>& lines) override;
    EngineStatus Line(const GrayView& image, OcrProtocol::Line& line) override;

private:
    // Recognises `crop` (cropWidth x cropHeight gray) into `text`, spans in crop pixels.
    EngineStatus RecognizeCrop(const std::vector<uint8_t>& crop, uint32_t cropWidth, uint32_t cropHeight,
                               CropText& text);

    Runtime& runtime_;
    const Dictionary& dictionary_;
    PpOcrSettings settings_;
    ResizeScratch resize_;
    DetectScratch detect_;
    std::vector<uint8_t> small_, crop_, turned_, line_;
    std::vector<float> planes_, prob_, probs_;
    std::vector<TextBox> boxes_;
    std::vector<DecodedChar> decoded_;
    CropText text_;
};

} // namespace RayoPdf::Ocr
