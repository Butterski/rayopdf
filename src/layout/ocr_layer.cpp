// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_layer.h"

#include "image/image.h"
#include "layout/images.h"
#include "layout/interpreter.h"
#include "layout/ocr_style.h"

#include <algorithm>
#include <cmath>

namespace RayoPdf::Layout {

namespace {

// Where the baseline and the font size sit in an OCR line box (descenders below the baseline,
// detection boxes padded around the ink).
constexpr float kBaselineShare = 0.2f;
constexpr float kSizeShare = 0.75f;
// A measured line's extent around its baseline, in font sizes (the interpreter's default metrics).
constexpr float kOcrAscent = 0.8f;
constexpr float kOcrDescent = 0.2f;
constexpr uint16_t kOcrFont = 0xFFFF;  // a page-local font index of its own

bool SpaceCodePoint(uint32_t cp) {
    return cp == ' ' || cp == 0xA0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A);
}

// The next code point of valid UTF-8 (ParseResponse checked it) and its byte length.
uint32_t Decode(const std::string& text, size_t at, size_t& length) {
    const unsigned char b = static_cast<unsigned char>(text[at]);
    length = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    if (at + length > text.size()) {
        length = text.size() - at;
        return 0xFFFD;
    }
    if (length == 1) return b;
    uint32_t cp = b & (0x7F >> length);
    for (size_t i = 1; i < length; ++i) cp = (cp << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
    return cp;
}

} // namespace

bool StartCanvas(float x0, float y0, float x1, float y1, float scale, OcrCanvas& canvas) {
    canvas.pixels.clear();
    canvas.width = canvas.height = 0;
    const float w = x1 - x0, h = y1 - y0;
    if (!(w > 0 && h > 0) || !(scale == scale)) return false;
    scale = std::min(std::max(scale, kOcrMinScale), kOcrMaxScale);
    const double fit = std::sqrt(double(kOcrMaxCanvasPixels) / (double(w) * double(h)));
    if (scale > fit) scale = float(fit);
    if (scale * w > kOcrMaxCanvasSide) scale = kOcrMaxCanvasSide / w;
    if (scale * h > kOcrMaxCanvasSide) scale = kOcrMaxCanvasSide / h;
    const double cw = std::ceil(double(w) * scale - 1e-3), ch = std::ceil(double(h) * scale - 1e-3);
    if (!(cw >= 1 && ch >= 1)) return false;
    canvas.x0 = x0, canvas.y0 = y0, canvas.x1 = x1, canvas.y1 = y1;
    canvas.scale = scale;
    canvas.width = static_cast<uint32_t>(cw);
    canvas.height = static_cast<uint32_t>(ch);
    canvas.pixels.assign(size_t(canvas.width) * canvas.height, static_cast<char>(0xFF));
    return true;
}

float ImageScale(const ImageBox& image) {
    const double det = std::fabs(double(image.matrix[0]) * image.matrix[3] - double(image.matrix[1]) * image.matrix[2]);
    if (image.width == 0 || image.height == 0 || !(det > 1e-6)) return 0;
    return float(std::sqrt(double(image.width) * double(image.height) / det));
}

void DrawGray(OcrCanvas& canvas, const uint8_t* pixels, uint32_t width, uint32_t height, const float matrix[6]) {
    if (canvas.width == 0 || width == 0 || height == 0) return;
    const double a = matrix[0], b = matrix[1], c = matrix[2], d = matrix[3], e = matrix[4], f = matrix[5];
    const double det = a * d - b * c;
    if (!(std::fabs(det) > 1e-9)) return;
    // The placed unit square's box, in canvas pixels.
    double px0 = 1e300, px1 = -1e300, py0 = 1e300, py1 = -1e300;
    for (int k = 0; k < 4; ++k) {
        const double u = k & 1, v = k >> 1;
        const double x = a * u + c * v + e, y = b * u + d * v + f;
        const double cx = (x - canvas.x0) * canvas.scale, cy = (canvas.y1 - y) * canvas.scale;
        px0 = std::min(px0, cx), px1 = std::max(px1, cx), py0 = std::min(py0, cy), py1 = std::max(py1, cy);
    }
    const double cw = canvas.width, ch = canvas.height;
    if (!(px1 > 0 && py1 > 0 && px0 < cw && py0 < ch)) return;
    const uint32_t cx0 = static_cast<uint32_t>(std::max(0.0, std::floor(px0)));
    const uint32_t cx1 = static_cast<uint32_t>(std::min(cw, std::ceil(px1)));
    const uint32_t cy0 = static_cast<uint32_t>(std::max(0.0, std::floor(py0)));
    const uint32_t cy1 = static_cast<uint32_t>(std::min(ch, std::ceil(py1)));
    // Image space of a canvas pixel's centre: (u, v) = M^-1 (x - e, y - f); along a row u and v
    // change by the same step per pixel.
    const double inv = 1.0 / det, step = 1.0 / canvas.scale;
    const double du = d * inv * step, dv = -b * inv * step;
    for (uint32_t cy = cy0; cy < cy1; ++cy) {
        const double y = canvas.y1 - (cy + 0.5) * step - f;
        const double x = canvas.x0 + (cx0 + 0.5) * step - e;
        double u = (d * x - c * y) * inv, v = (a * y - b * x) * inv;
        uint8_t* out = reinterpret_cast<uint8_t*>(&canvas.pixels[size_t(cy) * canvas.width]);
        for (uint32_t cx = cx0; cx < cx1; ++cx, u += du, v += dv) {
            if (!(u >= 0 && u < 1 && v > 0 && v <= 1)) continue;
            const uint32_t ix = std::min(width - 1, static_cast<uint32_t>(u * width));
            const uint32_t iy = std::min(height - 1, static_cast<uint32_t>((1 - v) * height));
            const uint8_t value = pixels[size_t(iy) * width + ix];
            if (value < out[cx]) out[cx] = value;
        }
    }
}

uint32_t ComposeImages(const Document& document, const PageData& page, const std::vector<uint32_t>& images,
                       float x0, float y0, float x1, float y1, OcrCanvas& canvas) {
    float scale = 0;
    for (uint32_t i : images)
        if (i < page.images.size()) scale = std::max(scale, ImageScale(page.images[i]));
    if (!StartCanvas(x0, y0, x1, y1, scale > 0 ? scale : kOcrMinScale, canvas)) return 0;
    uint32_t drawn = 0;
    Image::GrayImage gray;
    for (uint32_t i : images) {
        if (i >= page.images.size()) continue;
        const ImageBox& image = page.images[i];
        // Decoded at about the canvas's resolution: no larger than the image will be drawn.
        uint32_t maxSide = 0;
        if (image.width > 0 && image.height > 0) {
            const float* m = image.matrix;
            const double along = std::sqrt(double(m[0]) * m[0] + double(m[1]) * m[1]) * canvas.scale;
            const double across = std::sqrt(double(m[2]) * m[2] + double(m[3]) * m[3]) * canvas.scale;
            const double factor = std::max(along / image.width, across / image.height);
            if (factor < 1)
                maxSide = static_cast<uint32_t>(
                    std::max(1.0, std::ceil(std::max(image.width, image.height) * std::max(factor, 1e-6))));
        }
        if (ImageGray(document, page, image, gray, maxSide) != Image::Status::Ok) continue;
        DrawGray(canvas, reinterpret_cast<const uint8_t*>(gray.pixels.data()), gray.width, gray.height, image.matrix);
        drawn++;
    }
    return drawn;
}

uint32_t AddOcrGlyphs(const OcrCanvas& canvas, const OcrProtocol::Response& response, PageData& page) {
    std::vector<uint32_t> order;
    order.reserve(response.lines.size());
    for (uint32_t i = 0; i < response.lines.size(); ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const OcrProtocol::Line& p = response.lines[a];
        const OcrProtocol::Line& q = response.lines[b];
        return p.y0 != q.y0 ? p.y0 < q.y0 : p.x0 < q.x0;
    });
    OcrStyle style;
    MeasureOcrStyle(canvas, response, style);
    const double step = 1.0 / canvas.scale;
    const int32_t cw = static_cast<int32_t>(canvas.width), ch = static_cast<int32_t>(canvas.height);
    uint32_t added = 0;
    for (uint32_t index : order) {
        const OcrProtocol::Line& line = response.lines[index];
        if (line.conf < kOcrMinConf || line.text.empty()) continue;
        const int32_t ly0 = std::max(0, line.y0), ly1 = std::min(ch, line.y1);
        if (ly1 <= ly0 || line.x1 <= 0 || line.x0 >= cw) continue;
        float top = float(canvas.y1 - ly0 * step), bottom = float(canvas.y1 - ly1 * step);
        float baseline = bottom + kBaselineShare * (top - bottom), size = kSizeShare * (top - bottom);
        // Measured from the ink: the baseline the letters stand on and the font size from their
        // x-height; the line keeps its box (the detector's padding included, as the gaps between
        // lines and rows were measured with), widened to a font's usual extent if it is smaller.
        const OcrLineStyle& measured = style.lines[index];
        if (measured.measured) {
            baseline = float(canvas.y1 - measured.baseline * step);
            size = float(measured.em * step);
            bottom = std::min(bottom, baseline - kOcrDescent * size);
            top = std::max(top, baseline + kOcrAscent * size);
        }
        size_t k = 0;
        for (size_t at = 0; at < line.text.size() && k < line.chars.size(); ++k) {
            size_t length = 0;
            const uint32_t cp = Decode(line.text, at, length);
            const std::string_view bytes(line.text.data() + at, length);
            at += length;
            if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) continue;
            if (page.glyphs.size() >= kMaxGlyphsPerPage || page.text.size() >= (size_t(1) << 30)) return added;
            double sx0 = std::min(std::max(line.chars[k].x0, 0), cw);
            double sx1 = std::min(std::max(line.chars[k].x1, line.chars[k].x0), cw);
            if (measured.pitch > 0) {
                // Fixed-pitch text: the character's cell on its block's grid.
                const double cell = std::round((0.5 * (sx0 + sx1) - measured.origin) / measured.pitch);
                sx0 = std::min(std::max(measured.origin + (cell - 0.5) * measured.pitch, 0.0), double(cw));
                sx1 = std::min(std::max(sx0 + measured.pitch, sx0), double(cw));
            }
            Glyph g;
            g.x0 = float(canvas.x0 + sx0 * step);
            g.x1 = std::max(float(canvas.x0 + std::max(sx1, sx0) * step), g.x0);
            g.y0 = bottom, g.y1 = top;
            g.u0 = g.x0, g.u1 = g.x1, g.vb = bottom, g.vt = top, g.v = baseline;
            g.size = size;
            g.text = static_cast<uint32_t>(page.text.size());
            g.textLength = static_cast<uint16_t>(bytes.size());
            g.flags = static_cast<uint16_t>(kGlyphInvisible | kGlyphOcr | style.flags[measured.first + k]);
            if (SpaceCodePoint(cp)) {
                g.flags |= kGlyphSpace;
            } else {
                added++;
            }
            g.font = kOcrFont;
            page.text.append(bytes.data(), bytes.size());
            page.glyphs.push_back(g);
        }
    }
    AddOcrRulings(canvas, style, page);
    return added;
}

size_t AppendOcrAltText(const OcrProtocol::Response& response, std::string& out) {
    std::vector<uint32_t> order;
    order.reserve(response.lines.size());
    for (uint32_t i = 0; i < response.lines.size(); ++i)
        if (response.lines[i].conf >= kOcrMinConf && !response.lines[i].text.empty()) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const OcrProtocol::Line& p = response.lines[a];
        const OcrProtocol::Line& q = response.lines[b];
        return p.y0 != q.y0 ? p.y0 < q.y0 : p.x0 < q.x0;
    });
    static const char kEllipsis[] = "\xE2\x80\xA6";
    const size_t start = out.size();
    for (uint32_t index : order) {
        const std::string& text = response.lines[index].text;
        bool space = out.size() > start;  // a line break is a space
        for (size_t at = 0; at < text.size();) {
            size_t length = 0;
            const uint32_t cp = Decode(text, at, length);
            if (length == 0) break;
            if (cp <= 0x20 || (cp >= 0x7F && cp < 0xA0) || SpaceCodePoint(cp)) {
                at += length;
                space = space || out.size() > start;
                continue;
            }
            if (out.size() - start + (space ? 1 : 0) + length > kOcrMaxAltBytes - (sizeof kEllipsis - 1)) {
                // Too long: cut after the last whole word, then the ellipsis.
                if (!space) {
                    size_t last = out.size();
                    while (last > start && out[last - 1] != ' ') --last;
                    if (last > start + 1) out.resize(last - 1);
                }
                out += kEllipsis;
                return out.size() - start;
            }
            if (space) out.push_back(' ');
            space = false;
            out.append(text, at, length);
            at += length;
        }
    }
    return out.size() - start;
}

} // namespace RayoPdf::Layout
