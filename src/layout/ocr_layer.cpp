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

// A point of the frame of baseline direction `dir` (interpreter.cpp ToFrame) back on the upright
// page.
void FromFrame(uint8_t dir, float u, float v, float& x, float& y) {
    switch (dir) {
    case 1: x = -v, y = u; break;
    case 2: x = -u, y = -v; break;
    case 3: x = v, y = -u; break;
    default: x = u, y = v; break;
    }
}

void ToFrame(uint8_t dir, float x, float y, float& u, float& v) {
    switch (dir) {
    case 1: u = y, v = -x; break;
    case 2: u = -x, v = -y; break;
    case 3: u = -y, v = x; break;
    default: u = x, v = y; break;
    }
}

// A rectangle of the frame of `dir` as the rectangle it covers on the upright page.
void RectFromFrame(uint8_t dir, float& x0, float& y0, float& x1, float& y1) {
    float ax, ay, bx, by;
    FromFrame(dir, x0, y0, ax, ay);
    FromFrame(dir, x1, y1, bx, by);
    x0 = std::min(ax, bx), x1 = std::max(ax, bx), y0 = std::min(ay, by), y1 = std::max(ay, by);
}

// `canvas` turned so that text of baseline direction `dir` stands upright on it: its pixels turned
// `dir` quarters clockwise, its region that of the frame of `dir` (the glyphs it gives are in that
// frame: u along x, v along y).
void TurnCanvas(const OcrCanvas& canvas, uint8_t dir, OcrCanvas& out) {
    const uint32_t w = canvas.width, h = canvas.height;
    const double step = 1.0 / canvas.scale;
    // The region the pixel grid covers, x0 .. x0 + w/scale by y1 - h/scale .. y1, into the frame.
    float fx0 = canvas.x0, fy0 = float(canvas.y1 - h * step), fx1 = float(canvas.x0 + w * step), fy1 = canvas.y1;
    float ax, ay, bx, by;
    ToFrame(dir, fx0, fy0, ax, ay);
    ToFrame(dir, fx1, fy1, bx, by);
    out.x0 = std::min(ax, bx), out.x1 = std::max(ax, bx), out.y0 = std::min(ay, by), out.y1 = std::max(ay, by);
    out.scale = canvas.scale;
    const bool quarter = (dir & 1) != 0;
    out.width = quarter ? h : w;
    out.height = quarter ? w : h;
    out.pixels.resize(canvas.pixels.size());
    const char* in = canvas.pixels.data();
    char* to = &out.pixels[0];
    for (uint32_t r = 0; r < out.height; ++r) {
        for (uint32_t c = 0; c < out.width; ++c) {
            // Turned clockwise once: out(r, c) = in(h-1-c, r); twice: in(h-1-r, w-1-c); three
            // times: in(c, w-1-r).
            size_t from;
            if (dir == 1) {
                from = size_t(h - 1 - c) * w + r;
            } else if (dir == 2) {
                from = size_t(h - 1 - r) * w + (w - 1 - c);
            } else {
                from = size_t(c) * w + (w - 1 - r);
            }
            to[size_t(r) * out.width + c] = in[from];
        }
    }
}

// A line of baseline direction `dir` in the pixels of a canvas w x h, as a left-to-right line in
// the pixels of that canvas turned by TurnCanvas.
OcrProtocol::Line TurnLine(const OcrProtocol::Line& line, uint8_t dir, int32_t w, int32_t h) {
    OcrProtocol::Line out = line;
    out.dir = 0;
    // Turned clockwise once: (x, y) -> (h - y, x); twice: (w - x, h - y); three times: (y, w - x).
    const auto point = [&](int32_t x, int32_t y, int32_t& tx, int32_t& ty) {
        if (dir == 1) {
            tx = h - y, ty = x;
        } else if (dir == 2) {
            tx = w - x, ty = h - y;
        } else {
            tx = y, ty = w - x;
        }
    };
    int32_t ax, ay, bx, by;
    point(line.x0, line.y0, ax, ay);
    point(line.x1, line.y1, bx, by);
    out.x0 = std::min(ax, bx), out.x1 = std::max(ax, bx), out.y0 = std::min(ay, by), out.y1 = std::max(ay, by);
    // Spans run along y for directions 1 and 3, along x for 2; the turned line's along x.
    for (OcrProtocol::CharSpan& span : out.chars) {
        const int32_t a = dir == 1 ? h - span.x0 : dir == 2 ? w - span.x0 : span.x0;
        const int32_t b = dir == 1 ? h - span.x1 : dir == 2 ? w - span.x1 : span.x1;
        span.x0 = std::min(a, b), span.x1 = std::max(a, b);
    }
    return out;
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

namespace {

// The text layer of lines that run left to right on `canvas` (AddOcrGlyphs); `dir` is the
// baseline direction they have on the page, whose frame `canvas` shows when it is not 0.
uint32_t AddUprightGlyphs(const OcrCanvas& canvas, const OcrProtocol::Response& response, uint8_t dir,
                          PageData& page) {
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
            if (dir != 0) {
                // The glyph's box (u0..u1, vb..vt) is in the frame; on the page it is the box it covers.
                RectFromFrame(dir, g.x0, g.y0, g.x1, g.y1);
                g.dir = dir;
            }
            page.glyphs.push_back(g);
        }
    }
    if (dir == 0) {
        AddOcrRulings(canvas, style, page);
        return added;
    }
    // Rules found on the turned canvas, back on the page (the page's own rulings set aside so that
    // they are compared in the same frame: none of them lie in it).
    std::vector<Ruling> rulings;
    std::vector<Shape> shapes;
    rulings.swap(page.rulings);
    shapes.swap(page.shapes);
    bool over = false;
    for (const Ruling& r : rulings) {
        float x0 = canvas.x0, y0 = canvas.y0, x1 = canvas.x1, y1 = canvas.y1;
        RectFromFrame(dir, x0, y0, x1, y1);
        over = over || (r.x1 > x0 && r.x0 < x1 && r.y1 > y0 && r.y0 < y1);
    }
    if (!over) AddOcrRulings(canvas, style, page);
    for (Ruling& r : page.rulings) RectFromFrame(dir, r.x0, r.y0, r.x1, r.y1);
    for (Shape& s : page.shapes) RectFromFrame(dir, s.x0, s.y0, s.x1, s.y1);
    rulings.insert(rulings.end(), page.rulings.begin(), page.rulings.end());
    shapes.insert(shapes.end(), page.shapes.begin(), page.shapes.end());
    page.rulings.swap(rulings);
    page.shapes.swap(shapes);
    return added;
}

} // namespace

uint32_t AddOcrGlyphs(const OcrCanvas& canvas, const OcrProtocol::Response& response, PageData& page) {
    bool turned = false;
    for (const OcrProtocol::Line& line : response.lines) turned = turned || line.dir != 0;
    if (!turned) return AddUprightGlyphs(canvas, response, 0, page);
    // Lines of each baseline direction on a canvas turned so that they run left to right.
    uint32_t added = 0;
    OcrProtocol::Response part;
    OcrCanvas turnedCanvas;
    const int32_t w = static_cast<int32_t>(canvas.width), h = static_cast<int32_t>(canvas.height);
    for (uint8_t dir = 0; dir < 4; ++dir) {
        part.lines.clear();
        for (const OcrProtocol::Line& line : response.lines) {
            if (line.dir == dir) part.lines.push_back(dir ? TurnLine(line, dir, w, h) : line);
        }
        if (part.lines.empty()) continue;
        if (dir) TurnCanvas(canvas, dir, turnedCanvas);
        added += AddUprightGlyphs(dir ? turnedCanvas : canvas, part, dir, page);
    }
    return added;
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
