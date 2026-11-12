// SPDX-License-Identifier: Apache-2.0
// Tables drawn as images (OCR add-on, layout/ocr.h): the rules of a canvas found from its pixels,
// so that the text the add-on reads in an image region can make a table with them
// (layout/tables.h). A rule is a run of dark pixels at least kImageRuleMin pixels and a tenth of
// the canvas long, its neighbouring runs in the rows (or columns) next to it merged into one
// segment at most kImageRuleThick pixels thick; a thicker stack is a filled area, not a rule.
#pragma once

#include "layout/ocr_layer.h"
#include "layout/page.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Layout {

constexpr uint32_t kImageRuleMin = 24;        // pixels
constexpr uint32_t kImageRuleThick = 4;       // pixels
constexpr uint32_t kMaxImageRulings = 2048;   // per canvas: more, and none is kept (fail closed)
constexpr uint8_t kImageRuleDark = 128;       // a gray level below it is ink

// Appends the horizontal and then the vertical rules of `canvas` to `out` in page points
// (x = canvas.x0 + px / canvas.scale, y = canvas.y1 - py / canvas.scale), as kRulingFill rulings.
void ImageRulings(const OcrCanvas& canvas, std::vector<Ruling>& out);

} // namespace RayoPdf::Layout
