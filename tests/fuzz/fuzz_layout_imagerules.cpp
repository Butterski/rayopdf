// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of what reads a table drawn as an image from its pixels (layout/ocr_tables.h
// ImageRulings, layout/ocr_route.h LooksLikeText): the first two bytes give the canvas's width and
// height (1-512 each, from the byte's value scaled), the rest its gray pixels, padded with white.
// Every rule found must lie in the canvas's region and be no thicker than kImageRuleThick pixels.
#include "layout/ocr_route.h"
#include "layout/ocr_tables.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace RayoPdf::Layout;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    OcrCanvas canvas;
    canvas.width = 1 + uint32_t(data[0]) * 2 + (data[0] & 1);
    canvas.height = 1 + uint32_t(data[1]) * 2 + (data[1] & 1);
    canvas.x0 = 10, canvas.y0 = 20;
    canvas.scale = 2;
    canvas.x1 = canvas.x0 + float(canvas.width) / canvas.scale;
    canvas.y1 = canvas.y0 + float(canvas.height) / canvas.scale;
    const size_t n = size_t(canvas.width) * canvas.height;
    canvas.pixels.assign(n, static_cast<char>(0xFF));
    std::memcpy(&canvas.pixels[0], data + 2, size - 2 < n ? size - 2 : n);

    std::vector<Ruling> rulings;
    ImageRulings(canvas, rulings);
    if (rulings.size() > kMaxImageRulings) std::abort();
    const float thick = float(kImageRuleThick) / canvas.scale + 1e-3f;
    for (const Ruling& r : rulings) {
        if (!(r.x0 >= canvas.x0 && r.x1 <= canvas.x1 + 1e-3f && r.y0 >= canvas.y0 - 1e-3f && r.y1 <= canvas.y1 &&
              r.x0 < r.x1 && r.y0 < r.y1 && (r.x1 - r.x0 <= thick || r.y1 - r.y0 <= thick)))
            std::abort();
    }
    TextEvidence evidence;
    LooksLikeText(reinterpret_cast<const uint8_t*>(canvas.pixels.data()), canvas.width, canvas.height, &evidence);
    return 0;
}
