// SPDX-License-Identifier: Apache-2.0
// A model-free stand-in for the PP-OCR networks, for tests/test_ocr.cpp and the fuzz target
// fuzz_ocr_server: it lets the add-on's whole pre- and post-processing run without a runtime.
//
// Detect: the text probability of a pixel is its darkness, 1 - gray / 255 (gray recovered from
// the first normalised plane). Recognize: every 8 input columns are one timestep; the darkest of
// its rows (mean darkness d over the 8 columns) gives the class: blank below 0.55 (white, and the
// mid-gray padding), class 1 up to 0.7, class 2 up to 0.85, class 3 above, each with probability
// 0.9 and the rest spread evenly.
#pragma once

#include "ocr/engine.h"

#include <vector>

namespace RayoPdf::Ocr::Testing {

class FakeRuntime final : public Runtime {
public:
    explicit FakeRuntime(uint32_t classes) : classes_(classes) {}
    bool failDetect = false;
    bool failRecognize = false;

    RuntimeStatus Detect(const float* planes, uint32_t width, uint32_t height, std::vector<float>& prob) override {
        if (failDetect) return RuntimeStatus::Failed;
        prob.resize(size_t(width) * height);
        for (size_t i = 0; i < prob.size(); ++i) {
            float gray = planes[i] * 0.229f + 0.485f;
            if (gray < 0) gray = 0;
            if (gray > 1) gray = 1;
            prob[i] = 1.0f - gray;
        }
        return RuntimeStatus::Ok;
    }

    RuntimeStatus Recognize(const float* planes, uint32_t width, std::vector<float>& probs, uint32_t& steps,
                            uint32_t& classes) override {
        if (failRecognize) return RuntimeStatus::Failed;
        steps = width / 8 ? width / 8 : 1;
        classes = classes_;
        probs.assign(size_t(steps) * classes, classes > 1 ? 0.1f / float(classes - 1) : 0.0f);
        for (uint32_t t = 0; t < steps; ++t) {
            double d = 0;
            for (uint32_t y = 0; y < kRecHeight; ++y) {
                double sum = 0;
                uint32_t count = 0;
                for (uint32_t x = t * 8; x < t * 8 + 8 && x < width; ++x) {
                    sum += 1.0 - (planes[size_t(y) * width + x] + 1.0) / 2.0;
                    ++count;
                }
                if (count && sum / count > d) d = sum / count;
            }
            uint32_t cls = d < 0.55 ? 0 : d < 0.7 ? 1 : d < 0.85 ? 2 : 3;
            if (cls >= classes) cls = classes - 1;
            probs[size_t(t) * classes + cls] = 0.9f;
        }
        return RuntimeStatus::Ok;
    }

private:
    uint32_t classes_;
};

} // namespace RayoPdf::Ocr::Testing
