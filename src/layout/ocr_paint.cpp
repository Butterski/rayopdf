// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_paint.h"

#include "image/image.h"
#include "pdf/content_lexer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace RayoPdf::Layout {

namespace {

struct State {
    Raster::Matrix ctm;
    uint8_t fill = 0, stroke = 0;   // luminance of the colours
    uint8_t fillComponents = 1, strokeComponents = 1;
    bool fillSeparation = false, strokeSeparation = false;  // one tint: 0 paper, 1 full ink
    double lineWidth = 1;
};

uint8_t Luminance(double r, double g, double b) {
    const double v = (77 * r + 150 * g + 29 * b) / 256 * 255;
    return static_cast<uint8_t>(v <= 0 ? 0 : v >= 255 ? 255 : v + 0.5);
}

// The colour of `n` operands of a colour space with `components` components (1 gray, 3 RGB, 4
// CMYK; a separation's tint darkens).
uint8_t ColourOf(const double* v, int n, int components, bool separation) {
    const auto unit = [](double x) { return x < 0 ? 0.0 : x > 1 ? 1.0 : x; };
    if (separation && n >= 1) return Luminance(1 - unit(v[0]), 1 - unit(v[0]), 1 - unit(v[0]));
    if (components == 3 && n >= 3) return Luminance(unit(v[0]), unit(v[1]), unit(v[2]));
    if (components == 4 && n >= 4) {
        const double k = unit(v[3]);
        return Luminance((1 - unit(v[0])) * (1 - k), (1 - unit(v[1])) * (1 - k), (1 - unit(v[2])) * (1 - k));
    }
    if (n >= 1) return Luminance(unit(v[0]), unit(v[0]), unit(v[0]));
    return 0;
}

class Painter {
public:
    Painter(const Document& doc, PaintMode mode, OcrCanvas& canvas, Raster::Rasterizer& raster, PaintStats& stats)
        : doc_(doc), mode_(mode), canvas_(canvas), raster_(raster), stats_(stats) {
        toPixels_ = Raster::Matrix{canvas.scale, 0, 0, -canvas.scale, -double(canvas.x0) * canvas.scale,
                                   double(canvas.y1) * canvas.scale};
    }

    void Run(std::string_view content, const PdfValue& resources, const Raster::Matrix& ctm, int depth) {
        State saved = gs_;
        const size_t base = stack_.size();
        gs_.ctm = ctm;
        ContentLexer lexer(content);
        std::string_view op;
        std::vector<PdfValue> operands;
        while (lexer.Next(op, operands)) {
            if (++stats_.operators > kMaxPaintOperators) {
                stats_.truncated = true;
                break;
            }
            Execute(op, operands, resources, lexer, depth);
        }
        stack_.resize(base);
        gs_ = saved;
        path_.Clear();
    }

private:
    bool Numbers(const std::vector<PdfValue>& operands, size_t count, double* out) const {
        if (operands.size() < count) return false;
        const size_t first = operands.size() - count;
        for (size_t i = 0; i < count; ++i) {
            if (!operands[first + i].IsNumber()) return false;
            out[i] = operands[first + i].Number();
            if (!std::isfinite(out[i])) return false;
        }
        return true;
    }

    // A point of user space in canvas pixels.
    void Device(double x, double y, float& ox, float& oy) const {
        double px, py, cx, cy;
        gs_.ctm.Apply(x, y, px, py);
        toPixels_.Apply(px, py, cx, cy);
        ox = static_cast<float>(cx);
        oy = static_cast<float>(cy);
    }

    void SetColourSpace(const PdfValue& resources, std::string_view name, bool fill) {
        int components = 1;
        bool separation = false;
        if (name == "DeviceRGB" || name == "RGB" || name == "CalRGB") components = 3;
        else if (name == "DeviceCMYK" || name == "CMYK") components = 4;
        else if (name != "DeviceGray" && name != "G" && name != "CalGray" && name != "Pattern") {
            const PdfValue& space = doc_.Get(doc_.Get(resources, "ColorSpace"), name);
            const PdfSpan<PdfValue> items = space.Array();
            const std::string_view family = items.size() > 0 ? doc_.Resolve(items[0]).Name() : space.Name();
            if (family == "ICCBased" && items.size() > 1) {
                const int64_t n = doc_.Get(items[1], "N").Int(1);
                components = n == 3 || n == 4 ? static_cast<int>(n) : 1;
            } else if (family == "DeviceRGB" || family == "CalRGB" || family == "Lab") {
                components = 3;
            } else if (family == "DeviceCMYK") {
                components = 4;
            } else if (family == "Separation" || family == "DeviceN") {
                separation = true;
            }
        }
        if (fill) {
            gs_.fillComponents = static_cast<uint8_t>(components), gs_.fillSeparation = separation;
            gs_.fill = separation ? 255 : 0;  // initial colour: tint 0, or black
        } else {
            gs_.strokeComponents = static_cast<uint8_t>(components), gs_.strokeSeparation = separation;
            gs_.stroke = separation ? 255 : 0;
        }
    }

    void SetColour(const std::vector<PdfValue>& operands, bool fill) {
        double v[4] = {};
        int n = 0;
        for (const PdfValue& o : operands)
            if (o.IsNumber() && n < 4) v[n++] = o.Number();
        const uint8_t value = fill ? ColourOf(v, n, gs_.fillComponents, gs_.fillSeparation)
                                   : ColourOf(v, n, gs_.strokeComponents, gs_.strokeSeparation);
        (fill ? gs_.fill : gs_.stroke) = n > 0 ? value : 128;  // a pattern: mid gray
    }

    uint8_t Ink(bool fill) const { return mode_ == PaintMode::Glyph ? 0 : fill ? gs_.fill : gs_.stroke; }

    void Fill(Raster::FillRule rule) {
        if (path_.Empty()) return;
        raster_.Fill(path_, Raster::Matrix(), rule, reinterpret_cast<uint8_t*>(&canvas_.pixels[0]), canvas_.width,
                     canvas_.height, Ink(true), mode_ == PaintMode::Glyph);
        stats_.fills++;
        stats_.truncated = stats_.truncated || raster_.Overflowed() || path_.Overflowed();
    }

    // A stroke: one quadrilateral per flattened segment, all of one orientation (non-zero fill).
    void Stroke() {
        if (path_.Empty()) return;
        const double det = std::fabs(gs_.ctm.a * gs_.ctm.d - gs_.ctm.b * gs_.ctm.c);
        double half = 0.5 * gs_.lineWidth * std::sqrt(det) * canvas_.scale;
        if (!(half >= 0.5)) half = 0.5;
        if (half > 64) half = 64;
        struct Quads final : Raster::LineSink {
            Raster::Path out;
            double half = 0.5;
            void Line(double x0, double y0, double x1, double y1) override {
                const double dx = x1 - x0, dy = y1 - y0, length = std::sqrt(dx * dx + dy * dy);
                if (!(length > 1e-9)) return;
                // The normal turned from the direction: every quadrilateral has the same
                // orientation, so overlapping ones add up under the non-zero rule.
                const double nx = -dy / length * half, ny = dx / length * half;
                const double ex = dx / length * half, ey = dy / length * half;  // square caps
                out.MoveTo(float(x0 - ex + nx), float(y0 - ey + ny));
                out.LineTo(float(x0 - ex - nx), float(y0 - ey - ny));
                out.LineTo(float(x1 + ex - nx), float(y1 + ey - ny));
                out.LineTo(float(x1 + ex + nx), float(y1 + ey + ny));
                out.Close();
            }
        } quads;
        quads.half = half;
        // A stroke closes only the subpaths ended by h.
        Raster::Flatten(path_, Raster::Matrix(), Raster::kFlattenTolerance, -1.0, -1.0, canvas_.width + 1.0,
                        canvas_.height + 1.0, quads, false);
        raster_.Fill(quads.out, Raster::Matrix(), Raster::FillRule::NonZero, reinterpret_cast<uint8_t*>(&canvas_.pixels[0]),
                     canvas_.width, canvas_.height, Ink(false), mode_ == PaintMode::Glyph);
        stats_.fills++;
        stats_.truncated = stats_.truncated || raster_.Overflowed() || quads.out.Overflowed();
    }

    void DrawImage(const Image::GrayImage& gray) {
        if (gray.width == 0 || gray.height == 0) return;
        const float m[6] = {float(gs_.ctm.a), float(gs_.ctm.b), float(gs_.ctm.c),
                            float(gs_.ctm.d), float(gs_.ctm.e), float(gs_.ctm.f)};
        DrawGray(canvas_, reinterpret_cast<const uint8_t*>(gray.pixels.data()), gray.width, gray.height, m);
        stats_.images++;
    }

    // The longest side an image placed by the CTM needs at the canvas's resolution.
    uint32_t MaxSide() const {
        const double along = std::sqrt(gs_.ctm.a * gs_.ctm.a + gs_.ctm.b * gs_.ctm.b) * canvas_.scale;
        const double across = std::sqrt(gs_.ctm.c * gs_.ctm.c + gs_.ctm.d * gs_.ctm.d) * canvas_.scale;
        const double side = std::ceil(std::max(along, across)) + 1;
        return side >= 1 && side < 65536 ? static_cast<uint32_t>(side) : 65536;
    }

    void DoXObject(const PdfValue& resources, std::string_view name, int depth) {
        const PdfValue* entry = doc_.Get(resources, "XObject").Find(name);
        if (!entry) return;
        const PdfValue& xobject = doc_.Resolve(*entry);
        if (!xobject.IsStream()) return;
        const PdfValue& subtype = doc_.Get(xobject, "Subtype");
        if (subtype.IsName("Image")) {
            Image::GrayImage gray;
            if (Image::GrayXObject(doc_, xobject, resources, gray, MaxSide()) == Image::Status::Ok) DrawImage(gray);
            return;
        }
        if (!subtype.IsName("Form")) return;
        if (depth + 1 > kMaxPaintFormDepth || std::find(forms_.begin(), forms_.end(), &xobject) != forms_.end()) {
            stats_.truncated = true;
            return;
        }
        std::string data;
        const StreamStatus status = doc_.StreamData(xobject, data);
        if (status == StreamStatus::Corrupt || status == StreamStatus::NotStream || data.empty()) return;
        Raster::Matrix ctm = gs_.ctm;
        double m[6];
        const PdfSpan<PdfValue> matrix = doc_.Get(xobject, "Matrix").Array();
        bool ok = matrix.size() == 6;
        for (size_t i = 0; ok && i < 6; ++i) ok = doc_.Number(matrix[i], m[i]) && std::isfinite(m[i]);
        if (ok) ctm = Raster::Matrix{m[0], m[1], m[2], m[3], m[4], m[5]}.Then(ctm);
        const PdfValue* own = xobject.Find("Resources");
        forms_.push_back(&xobject);
        Run(data, own ? doc_.Resolve(*own) : resources, ctm, depth + 1);
        forms_.pop_back();
    }

    void Execute(std::string_view op, const std::vector<PdfValue>& operands, const PdfValue& resources,
                 ContentLexer& lexer, int depth) {
        double n[6];
        if (op.size() == 1) {
            switch (op[0]) {
            case 'q':
                if (stack_.size() < kMaxPaintStates) stack_.push_back(gs_);
                else stats_.truncated = true;
                return;
            case 'Q':
                if (!stack_.empty()) {
                    gs_ = stack_.back();
                    stack_.pop_back();
                }
                return;
            case 'm':
                if (Numbers(operands, 2, n)) {
                    float x, y;
                    Device(n[0], n[1], x, y);
                    path_.MoveTo(x, y);
                    curX_ = n[0], curY_ = n[1];
                }
                return;
            case 'l':
                if (Numbers(operands, 2, n)) {
                    float x, y;
                    Device(n[0], n[1], x, y);
                    path_.LineTo(x, y);
                    curX_ = n[0], curY_ = n[1];
                }
                return;
            case 'c':
                if (Numbers(operands, 6, n)) Curve(n[0], n[1], n[2], n[3], n[4], n[5]);
                return;
            case 'v':
                if (Numbers(operands, 4, n)) Curve(curX_, curY_, n[0], n[1], n[2], n[3]);
                return;
            case 'y':
                if (Numbers(operands, 4, n)) Curve(n[0], n[1], n[2], n[3], n[2], n[3]);
                return;
            case 'h':
                path_.Close();
                return;
            case 'f':
            case 'F':
                Fill(Raster::FillRule::NonZero);
                path_.Clear();
                return;
            case 'B':
            case 'b':
                if (op[0] == 'b') path_.Close();
                Fill(Raster::FillRule::NonZero);
                Stroke();
                path_.Clear();
                return;
            case 'S':
            case 's':
                if (op[0] == 's') path_.Close();
                Stroke();
                path_.Clear();
                return;
            case 'n':
                path_.Clear();
                return;
            case 'w':
                if (Numbers(operands, 1, n) && n[0] >= 0) gs_.lineWidth = n[0];
                return;
            case 'g':
            case 'G':
                if (op[0] == 'g') gs_.fillComponents = 1, gs_.fillSeparation = false;
                else gs_.strokeComponents = 1, gs_.strokeSeparation = false;
                SetColour(operands, op[0] == 'g');
                return;
            case 'k':
            case 'K':
                if (op[0] == 'k') gs_.fillComponents = 4, gs_.fillSeparation = false;
                else gs_.strokeComponents = 4, gs_.strokeSeparation = false;
                SetColour(operands, op[0] == 'k');
                return;
            default:
                return;
            }
        }
        if (op == "cm") {
            if (Numbers(operands, 6, n)) gs_.ctm = Raster::Matrix{n[0], n[1], n[2], n[3], n[4], n[5]}.Then(gs_.ctm);
        } else if (op == "re") {
            if (Numbers(operands, 4, n)) {
                float x, y;
                Device(n[0], n[1], x, y);
                path_.MoveTo(x, y);
                Device(n[0] + n[2], n[1], x, y);
                path_.LineTo(x, y);
                Device(n[0] + n[2], n[1] + n[3], x, y);
                path_.LineTo(x, y);
                Device(n[0], n[1] + n[3], x, y);
                path_.LineTo(x, y);
                path_.Close();
                curX_ = n[0], curY_ = n[1];
            }
        } else if (op == "f*") {
            Fill(Raster::FillRule::EvenOdd);
            path_.Clear();
        } else if (op == "B*" || op == "b*") {
            if (op[0] == 'b') path_.Close();
            Fill(Raster::FillRule::EvenOdd);
            Stroke();
            path_.Clear();
        } else if (op == "rg" || op == "RG") {
            if (op[0] == 'r') gs_.fillComponents = 3, gs_.fillSeparation = false;
            else gs_.strokeComponents = 3, gs_.strokeSeparation = false;
            SetColour(operands, op[0] == 'r');
        } else if (op == "cs" || op == "CS") {
            if (!operands.empty() && operands.back().IsName()) SetColourSpace(resources, operands.back().Name(), op[0] == 'c');
        } else if (op == "sc" || op == "scn") {
            SetColour(operands, true);
        } else if (op == "SC" || op == "SCN") {
            SetColour(operands, false);
        } else if (op == "Do") {
            if (!operands.empty() && operands.back().IsName()) DoXObject(resources, operands.back().Name(), depth);
        } else if (op == "BI") {
            if (operands.empty()) return;
            Image::GrayImage gray;
            if (Image::GrayInline(doc_, operands.back(), lexer.InlineImageData(), resources, gray, MaxSide()) ==
                Image::Status::Ok)
                DrawImage(gray);
        }
    }

    void Curve(double x1, double y1, double x2, double y2, double x3, double y3) {
        float a, b, c, d, e, f;
        Device(x1, y1, a, b);
        Device(x2, y2, c, d);
        Device(x3, y3, e, f);
        path_.CubicTo(a, b, c, d, e, f);
        curX_ = x3, curY_ = y3;
    }

    const Document& doc_;
    PaintMode mode_;
    OcrCanvas& canvas_;
    Raster::Rasterizer& raster_;
    PaintStats& stats_;
    Raster::Matrix toPixels_;
    State gs_;
    std::vector<State> stack_;
    std::vector<const PdfValue*> forms_;
    Raster::Path path_;
    double curX_ = 0, curY_ = 0;
};

} // namespace

void PaintContent(const Document& document, std::string_view content, const PdfValue& resources,
                  const Raster::Matrix& ctm, PaintMode mode, OcrCanvas& canvas, Raster::Rasterizer& raster,
                  PaintStats& stats) {
    if (canvas.width == 0 || canvas.height == 0) return;
    Painter painter(document, mode, canvas, raster, stats);
    painter.Run(content, resources, ctm, 0);
}

Raster::Matrix PageMatrix(const Document& document, int index) {
    const PdfRect crop = document.CropBox(index);
    const double w = crop.Width(), h = crop.Height();
    Raster::Matrix base{1, 0, 0, 1, -crop.x0, -crop.y0};
    Raster::Matrix turn;
    switch (document.Rotate(index)) {
    case 90: turn = Raster::Matrix{0, -1, 1, 0, 0, w}; break;
    case 180: turn = Raster::Matrix{-1, 0, 0, -1, w, h}; break;
    case 270: turn = Raster::Matrix{0, 1, -1, 0, h, 0}; break;
    default: break;
    }
    return base.Then(turn);
}

} // namespace RayoPdf::Layout
