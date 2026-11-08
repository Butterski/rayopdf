// SPDX-License-Identifier: Apache-2.0
#include "ocr/engine.h"

#include <cmath>

namespace RayoPdf::Ocr {
namespace {

constexpr float kDetMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kDetDeviation[3] = {0.229f, 0.224f, 0.225f};
constexpr float kRecMean[3] = {0.5f, 0.5f, 0.5f};
constexpr float kRecDeviation[3] = {0.5f, 0.5f, 0.5f};
// PaddleOCR pads every recognition input to at least this width (with 0, mid-gray once normalised).
constexpr uint32_t kMinRecWidth = 320;
// A crop this much taller than wide holds vertical text.
constexpr float kVerticalRatio = 1.5f;
// Largest crop sampled for one line: across and along its text.
constexpr float kMaxCropShort = 4.0f * kRecHeight;
constexpr float kMaxCropLong = 4.0f * kMaxRecWidth;

uint32_t RoundTo32(double value) {
    uint32_t v = uint32_t(value / 32 + 0.5) * 32;
    return v < 32 ? 32 : v;
}

int32_t ToPixel(float value) {
    return int32_t(std::floor(value + 0.5f));
}

} // namespace

bool HasCjk(std::string_view text) {
    // Every code point at or above U+2E80 takes 3 or 4 UTF-8 bytes, led by 0xE2 0xBA or higher.
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        const uint8_t b0 = uint8_t(text[i]);
        if (b0 < 0xE2 || b0 > 0xEF) continue;
        const uint32_t cp = (uint32_t(b0 & 0x0F) << 12) | (uint32_t(uint8_t(text[i + 1]) & 0x3F) << 6) |
                            (uint32_t(uint8_t(text[i + 2]) & 0x3F));
        if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF)) {
            return true;
        }
        i += 2;
    }
    return false;
}

EngineStatus PpOcrEngine::RecognizeCrop(const Recognizer& recognizer, const std::vector<uint8_t>& crop,
                                        uint32_t cropWidth, uint32_t cropHeight, CropText& text) {
    text.text.clear();
    text.x0.clear();
    text.x1.clear();
    text.conf = 0;
    if (cropWidth == 0 || cropHeight == 0) return EngineStatus::Ok;
    double scaled = std::ceil(double(kRecHeight) * cropWidth / cropHeight);
    if (scaled < 1) scaled = 1;
    if (scaled > kMaxRecWidth) scaled = kMaxRecWidth;
    const uint32_t width = uint32_t(scaled);
    Resize(GrayView{crop.data(), cropWidth, cropHeight}, width, kRecHeight, resize_, line_);
    const uint32_t padded = width < kMinRecWidth ? kMinRecWidth : width;
    Normalise(line_.data(), line_.size(), kRecMean, kRecDeviation, prob_);  // prob_ as scratch for the planes
    planes_.assign(size_t(3) * kRecHeight * padded, 0.0f);
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t y = 0; y < kRecHeight; ++y) {
            const float* from = prob_.data() + (size_t(c) * kRecHeight + y) * width;
            float* to = planes_.data() + (size_t(c) * kRecHeight + y) * padded;
            for (uint32_t x = 0; x < width; ++x) to[x] = from[x];
        }
    }
    uint32_t steps = 0, classes = 0;
    if (recognizer.runtime->Recognize(planes_.data(), padded, probs_, steps, classes) != RuntimeStatus::Ok) {
        return EngineStatus::Failed;
    }
    const Dictionary& dictionary = *recognizer.dictionary;
    if (classes != dictionary.Classes() || steps == 0 || probs_.size() != size_t(steps) * classes) {
        return EngineStatus::Failed;
    }
    DecodeCtc(probs_.data(), steps, classes, decoded_);
    InsertSpaces(probs_.data(), classes, classes - 1, settings_.spaceThreshold, decoded_);
    // The timesteps cover the padded input; one input pixel is cropWidth / width crop pixels.
    AssembleText(decoded_, dictionary, steps, float(padded) * float(cropWidth) / float(width), text);
    return EngineStatus::Ok;
}

EngineStatus PpOcrEngine::Page(const GrayView& page, uint32_t, std::vector<OcrProtocol::Line>& lines) {
    lines.clear();
    if (page.width == 0 || page.height == 0) return EngineStatus::Ok;
    const uint32_t longer = page.width > page.height ? page.width : page.height;
    uint32_t side = settings_.detMaxSide;
    if (side == 0 || side > kMaxDetSide) side = kMaxDetSide;
    const double ratio = longer > side ? double(side) / longer : 1.0;
    const uint32_t width = RoundTo32(page.width * ratio), height = RoundTo32(page.height * ratio);
    Resize(page, width, height, resize_, small_);
    Normalise(small_.data(), small_.size(), kDetMean, kDetDeviation, planes_);
    if (recognizer_.runtime->Detect(planes_.data(), width, height, prob_) != RuntimeStatus::Ok ||
        prob_.size() != size_t(width) * height) {
        return EngineStatus::Failed;
    }
    DetectBoxes(prob_.data(), width, height, float(page.width) / width, float(page.height) / height, settings_.detect,
                detect_, boxes_);
    SortIntoLines(boxes_);
    if (boxes_.size() > kMaxPageLines) boxes_.resize(kMaxPageLines);

    // First pass: lines read so far, and how many of them went to the recogniser again.
    uint32_t read = 0, reread = 0;
    bool recognizerOnly = !firstPass_;
    for (const TextBox& box : boxes_) {
        const RotatedRect& rect = box.rect;
        const PixelBox bounds = Bounds(rect, page.width, page.height);
        if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0) continue;
        // Sampled at most kMaxCropShort pixels across and kMaxCropLong along (the recogniser reads
        // 48 rows and kMaxRecWidth columns), so a huge box costs no more than a normal line.
        const float shorter = rect.width < rect.height ? rect.width : rect.height;
        const float longer = rect.width < rect.height ? rect.height : rect.width;
        float factor = 1.0f;
        if (shorter > kMaxCropShort) factor = kMaxCropShort / shorter;
        if (longer * factor > kMaxCropLong) factor = kMaxCropLong / longer;
        const float w = rect.width * factor, h = rect.height * factor;
        const uint32_t cropWidth = w < 1 ? 1 : uint32_t(w + 0.5f);
        const uint32_t cropHeight = h < 1 ? 1 : uint32_t(h + 0.5f);
        SampleRect(page, rect, cropWidth, cropHeight, crop_);
        const bool vertical = float(cropHeight) >= kVerticalRatio * float(cropWidth);
        if (vertical) {
            RotateCounterClockwise(crop_, cropWidth, cropHeight, turned_);
            crop_.swap(turned_);
        }
        const uint32_t readWidth = vertical ? cropHeight : cropWidth;
        const uint32_t readHeight = vertical ? cropWidth : cropHeight;
        EngineStatus status;
        if (recognizerOnly) {
            status = RecognizeCrop(recognizer_, crop_, readWidth, readHeight, text_);
        } else {
            status = RecognizeCrop(first_, crop_, readWidth, readHeight, text_);
            if (status != EngineStatus::Ok) return status;
            ++read;
            if (text_.conf < settings_.rereadConf || HasCjk(text_.text)) {
                ++reread;
                status = RecognizeCrop(recognizer_, crop_, readWidth, readHeight, text_);
            }
            if (read >= settings_.switchAfter && double(reread) > double(settings_.switchShare) * read) {
                recognizerOnly = true;
            }
        }
        if (status != EngineStatus::Ok) return status;
        if (text_.text.empty()) continue;

        OcrProtocol::Line line;
        line.x0 = bounds.x0;
        line.y0 = bounds.y0;
        line.x1 = bounds.x1;
        line.y1 = bounds.y1;
        line.text = text_.text;
        line.conf = text_.conf;
        line.chars.resize(text_.x0.size());
        // A crop position t (0 at its left edge) lies on the page at centre + (t - length / 2) * axis;
        // the axis is the rectangle's width (horizontal) or height (vertical text) direction.
        const float axisX = vertical ? -rect.uy : rect.ux;
        const float length = vertical ? float(cropHeight) : float(cropWidth);
        const float scale = vertical ? rect.height / length : rect.width / length;
        for (size_t i = 0; i < text_.x0.size(); ++i) {
            float a = rect.cx + (text_.x0[i] - length * 0.5f) * scale * axisX;
            float b = rect.cx + (text_.x1[i] - length * 0.5f) * scale * axisX;
            if (a > b) {
                const float t = a;
                a = b;
                b = t;
            }
            int32_t x0 = ToPixel(a), x1 = ToPixel(b);
            if (x0 < line.x0) x0 = line.x0;
            if (x1 > line.x1) x1 = line.x1;
            if (x0 > line.x1) x0 = line.x1;
            if (x1 < x0) x1 = x0;
            line.chars[i] = {x0, x1};
        }
        lines.push_back(std::move(line));
    }
    return EngineStatus::Ok;
}

EngineStatus PpOcrEngine::Line(const GrayView& image, OcrProtocol::Line& line) {
    line = OcrProtocol::Line();
    line.x1 = int32_t(image.width);
    line.y1 = int32_t(image.height);
    if (image.width == 0 || image.height == 0) return EngineStatus::Ok;
    crop_.assign(image.pixels, image.pixels + size_t(image.width) * image.height);
    const bool vertical = float(image.height) >= kVerticalRatio * float(image.width);
    EngineStatus status;
    if (vertical) {
        RotateCounterClockwise(crop_, image.width, image.height, turned_);
        status = RecognizeCrop(recognizer_, turned_, image.height, image.width, text_);
    } else {
        status = RecognizeCrop(recognizer_, crop_, image.width, image.height, text_);
    }
    if (status != EngineStatus::Ok) return status;
    line.text = text_.text;
    line.conf = text_.conf;
    line.chars.resize(text_.x0.size());
    for (size_t i = 0; i < text_.x0.size(); ++i) {
        int32_t x0 = vertical ? 0 : ToPixel(text_.x0[i]);
        int32_t x1 = vertical ? line.x1 : ToPixel(text_.x1[i]);
        if (x0 < 0) x0 = 0;
        if (x1 > line.x1) x1 = line.x1;
        if (x0 > line.x1) x0 = line.x1;
        if (x1 < x0) x1 = x0;
        line.chars[i] = {x0, x1};
    }
    return EngineStatus::Ok;
}

} // namespace RayoPdf::Ocr
