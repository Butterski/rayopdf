// SPDX-License-Identifier: Apache-2.0
#include "ocr/raster.h"

#include <cstring>

namespace RayoPdf::Ocr {
namespace {

// Taps of one axis: output i reads source pixels first[i] .. first[i] + count[i] - 1 with the
// weights from offset[i] on. Shrinking: box filter; growing: bilinear, pixel centres aligned.
void AxisWeights(uint32_t source, uint32_t target, std::vector<uint32_t>& first, std::vector<uint32_t>& count,
                 std::vector<uint32_t>& offset, std::vector<float>& weights) {
    first.resize(target);
    count.resize(target);
    offset.resize(target);
    weights.clear();
    if (target <= source) {
        const double factor = double(source) / double(target);
        for (uint32_t i = 0; i < target; ++i) {
            const double a = i * factor;
            double b = (i + 1) * factor;
            if (b > source) b = source;
            uint32_t j = uint32_t(a);
            if (j >= source) j = source - 1;
            first[i] = j;
            offset[i] = uint32_t(weights.size());
            uint32_t n = 0;
            for (; j < source && double(j) < b; ++j) {
                const double lo = a > j ? a : double(j);
                const double hi = b < j + 1.0 ? b : j + 1.0;
                if (hi > lo) {
                    weights.push_back(float((hi - lo) / factor));
                } else {
                    weights.push_back(0.0f);
                }
                ++n;
            }
            if (n == 0) {  // cannot happen for target <= source; keep one tap so reads stay in range
                weights.push_back(1.0f);
                n = 1;
            }
            count[i] = n;
        }
        return;
    }
    const double factor = double(source) / double(target);
    for (uint32_t i = 0; i < target; ++i) {
        double s = (i + 0.5) * factor - 0.5;
        if (s < 0) s = 0;
        if (s > source - 1.0) s = source - 1.0;
        const uint32_t j = uint32_t(s);
        const float t = float(s - j);
        first[i] = j;
        offset[i] = uint32_t(weights.size());
        if (j + 1 < source && t > 0.0f) {
            weights.push_back(1.0f - t);
            weights.push_back(t);
            count[i] = 2;
        } else {
            weights.push_back(1.0f);
            count[i] = 1;
        }
    }
}

uint8_t ToByte(float value) {
    if (!(value > 0.0f)) return 0;
    if (value >= 255.0f) return 255;
    return uint8_t(value + 0.5f);
}

} // namespace

void Resize(const GrayView& source, uint32_t width, uint32_t height, ResizeScratch& s, std::vector<uint8_t>& out) {
    out.assign(size_t(width) * height, 255);
    if (source.width == 0 || source.height == 0 || width == 0 || height == 0) return;
    if (source.width == width && source.height == height) {
        std::memcpy(out.data(), source.pixels, out.size());
        return;
    }
    AxisWeights(source.width, width, s.first, s.count, s.offset, s.weights);
    AxisWeights(source.height, height, s.firstY, s.countY, s.offsetY, s.weightsY);
    // Rows of the source that some output row reads, resampled horizontally.
    s.rows.resize(size_t(source.height) * width);
    for (uint32_t y = 0; y < source.height; ++y) {
        const uint8_t* row = source.pixels + size_t(y) * source.width;
        float* to = s.rows.data() + size_t(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* from = row + s.first[x];
            const float* w = s.weights.data() + s.offset[x];
            float sum = 0.0f;
            for (uint32_t k = 0; k < s.count[x]; ++k) sum += w[k] * from[k];
            to[x] = sum;
        }
    }
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* to = out.data() + size_t(y) * width;
        const float* w = s.weightsY.data() + s.offsetY[y];
        const uint32_t n = s.countY[y];
        const float* base = s.rows.data() + size_t(s.firstY[y]) * width;
        for (uint32_t x = 0; x < width; ++x) {
            float sum = 0.0f;
            for (uint32_t k = 0; k < n; ++k) sum += w[k] * base[size_t(k) * width + x];
            to[x] = ToByte(sum);
        }
    }
}

void SampleRect(const GrayView& source, const RotatedRect& rect, uint32_t width, uint32_t height,
                std::vector<uint8_t>& out) {
    out.assign(size_t(width) * height, 255);
    if (source.width == 0 || source.height == 0) return;
    const float maxX = float(source.width - 1), maxY = float(source.height - 1);
    const float stepX = rect.width / float(width), stepY = rect.height / float(height);
    for (uint32_t j = 0; j < height; ++j) {
        const float ly = (j + 0.5f) * stepY - rect.height * 0.5f;
        for (uint32_t i = 0; i < width; ++i) {
            const float lx = (i + 0.5f) * stepX - rect.width * 0.5f;
            float px = rect.cx + lx * rect.ux - ly * rect.uy;
            float py = rect.cy + lx * rect.uy + ly * rect.ux;
            if (!(px > 0.0f)) px = 0.0f;
            if (!(py > 0.0f)) py = 0.0f;
            if (px > maxX) px = maxX;
            if (py > maxY) py = maxY;
            const uint32_t x0 = uint32_t(px), y0 = uint32_t(py);
            const uint32_t x1 = x0 + 1 < source.width ? x0 + 1 : x0;
            const uint32_t y1 = y0 + 1 < source.height ? y0 + 1 : y0;
            const float tx = px - x0, ty = py - y0;
            const uint8_t* r0 = source.pixels + size_t(y0) * source.width;
            const uint8_t* r1 = source.pixels + size_t(y1) * source.width;
            const float top = r0[x0] + (r0[x1] - r0[x0]) * tx;
            const float bottom = r1[x0] + (r1[x1] - r1[x0]) * tx;
            out[size_t(j) * width + i] = ToByte(top + (bottom - top) * ty);
        }
    }
}

void RotateCounterClockwise(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                            std::vector<uint8_t>& out) {
    out.resize(size_t(width) * height);
    // out is `height` wide and `width` tall: out(row r, column c) = in(row c, column width-1-r).
    for (uint32_t r = 0; r < width; ++r) {
        for (uint32_t c = 0; c < height; ++c) {
            out[size_t(r) * height + c] = pixels[size_t(c) * width + (width - 1 - r)];
        }
    }
}

void Normalise(const uint8_t* pixels, size_t count, const float mean[3], const float deviation[3],
               std::vector<float>& planes) {
    planes.resize(count * 3);
    for (int c = 0; c < 3; ++c) {
        float table[256];
        for (int v = 0; v < 256; ++v) table[v] = (v / 255.0f - mean[c]) / deviation[c];
        float* plane = planes.data() + count * c;
        for (size_t i = 0; i < count; ++i) plane[i] = table[pixels[i]];
    }
}

} // namespace RayoPdf::Ocr
