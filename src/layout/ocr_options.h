// SPDX-License-Identifier: Apache-2.0
// How a conversion uses the OCR add-on (layout/ocr.h, docs/development/ocr-protocol.md): the
// options ConvertOptions carries, kept apart so that convert.h stays light.
#pragma once

#include <cstdint>
#include <string>

namespace RayoPdf::Layout {

class OcrSession;

enum class OcrMode : uint8_t {
    Auto,   // OCR where the text layer fails: image-only pages, text-bearing images on digital pages
    Off,    // never: no add-on is looked for or started
    Force,  // every page image and every large image, an existing invisible text layer replaced
};

constexpr uint32_t kOcrRequestTimeoutMs = 120000;   // one page or region, then the add-on is killed
constexpr uint32_t kOcrHandshakeTimeoutMs = 30000;  // start-up, models loaded

struct OcrOptions {
    OcrMode mode = OcrMode::Auto;
    std::string addon;  // --ocr-addon=PATH; empty: discovery (layout/ocr_client.h FindOcrAddon)
    uint32_t requestTimeoutMs = kOcrRequestTimeoutMs;
    uint32_t handshakeTimeoutMs = kOcrHandshakeTimeoutMs;
    // A worker's add-on, kept running from document to document (the batch modes); null: the
    // conversion starts one when a page needs it and stops it at the end.
    OcrSession* session = nullptr;
};

} // namespace RayoPdf::Layout
