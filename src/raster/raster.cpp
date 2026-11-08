// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/gfx/raster.cpp at 9fbbfaf (Apache-2.0, same author): the general
// sweep with a scalar accumulation, writing onto a gray canvas.
//
// Every cell a segment crosses gets the signed height it covers (cover) and that height times
// twice the mean x of its part within the cell (area); a row's coverage at a pixel is then (sum of
// the covers to its left) * 512 - its own area, over 512.
#include "raster/raster.h"

#include <algorithm>
#include <cmath>

namespace RayoPdf::Raster {

namespace {

constexpr int kPixelBits = 8;
constexpr int32_t kOne = 1 << kPixelBits;
constexpr int kBandRows = 16;
constexpr int kBandShift = kPixelBits + 4;  // subpixel y to band

// floor(a / b) for b > 0.
inline int64_t FloorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

inline int32_t WrapAdd(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

// Round half up, for values well inside int64.
inline int64_t RoundToInt(double v) {
    return static_cast<int64_t>(std::floor(v + 0.5));
}

} // namespace

struct Rasterizer::Sink final : LineSink {
    Rasterizer& raster;
    explicit Sink(Rasterizer& r) : raster(r) {}
    void Line(double x0, double y0, double x1, double y1) override { raster.AddLine(x0, y0, x1, y1); }
};

void Rasterizer::Cell(int32_t* row, int local, int32_t c, int32_t cover, int32_t area) {
    int32_t* cell = row + 2 * static_cast<ptrdiff_t>(c);
    cell[0] = WrapAdd(cell[0], cover);
    cell[1] = WrapAdd(cell[1], area);
    minCell_[local] = std::min(minCell_[local], c);
    maxCell_[local] = std::max(maxCell_[local], c);
}

// The part of a segment within one row, from (sx, sy) to (ex, ey) with sy < ey (only their
// difference matters), split at every vertical cell boundary.
void Rasterizer::RowPiece(int32_t* row, int local, int32_t sx, int32_t sy, int32_t ex, int32_t ey, int32_t dir) {
    const int32_t dy = ey - sy;
    if (sx == ex) {
        const int32_t c = sx >> kPixelBits;
        if (c >= width_) return;  // on the canvas's right edge: covers nothing inside
        const int32_t fx = sx - (c << kPixelBits);
        Cell(row, local, c, dir * dy, dir * dy * 2 * fx);
        return;
    }
    int32_t lx, rx;
    int64_t yl, yr;
    if (sx < ex) {
        lx = sx, rx = ex, yl = sy, yr = ey;
    } else {
        lx = ex, rx = sx, yl = ey, yr = sy;
    }
    const int32_t ca = lx >> kPixelBits;
    const int32_t cb = (rx - 1) >> kPixelBits;
    if (ca == cb) {
        Cell(row, local, ca, dir * dy, dir * dy * (lx + rx - 2 * (ca << kPixelBits)));
        return;
    }
    const int64_t dxt = rx - lx;
    const int64_t dyt = yr - yl;
    int64_t yPrev = yl;
    int32_t xPrev = lx;
    for (int32_t c = ca; c <= cb && c < width_; ++c) {
        const int32_t xNext = c == cb ? rx : (c + 1) << kPixelBits;
        const int64_t yNext = c == cb ? yr : yl + FloorDiv(dyt * (xNext - lx), dxt);
        const int32_t dyp = static_cast<int32_t>(yNext > yPrev ? yNext - yPrev : yPrev - yNext);
        const int32_t base = c << kPixelBits;
        Cell(row, local, c, dir * dyp, dir * dyp * ((xPrev - base) + (xNext - base)));
        yPrev = yNext;
        xPrev = xNext;
    }
}

void Rasterizer::RenderBand(const Segment& s, int bandTop) {
    const int32_t top = bandTop << kPixelBits;
    const int32_t bottom = (bandTop + kBandRows) << kPixelBits;
    const int32_t ya = std::max(s.y0, top);
    const int32_t yb = std::min(s.y1, bottom);
    if (ya >= yb) return;
    const int64_t dxs = static_cast<int64_t>(s.x1) - s.x0;
    const int64_t dys = static_cast<int64_t>(s.y1) - s.y0;
    const auto xAt = [&](int32_t y) -> int32_t {
        if (y == s.y0) return s.x0;
        if (y == s.y1) return s.x1;
        return static_cast<int32_t>(s.x0 + FloorDiv(dxs * (y - s.y0), dys));
    };
    const int32_t r0 = ya >> kPixelBits;
    const int32_t r1 = (yb - 1) >> kPixelBits;
    int32_t yPrev = ya;
    int32_t xPrev = xAt(ya);
    for (int32_t r = r0; r <= r1; ++r) {
        const int32_t yNext = r == r1 ? yb : (r + 1) << kPixelBits;
        const int32_t xNext = xAt(yNext);
        const int local = r - bandTop;
        int32_t* row = cells_.data() + 2 * static_cast<ptrdiff_t>(local) * width_;
        RowPiece(row, local, xPrev, yPrev, xNext, yNext, s.dir);
        yPrev = yNext;
        xPrev = xNext;
    }
}

// A segment in canvas pixels, already within its rows and columns, to 24.8.
void Rasterizer::Push(double x0, double y0, double x1, double y1) {
    if (segments_.size() >= kMaxSegments) {
        overflowed_ = true;
        return;
    }
    const int64_t maxXs = static_cast<int64_t>(width_) << kPixelBits;
    const int64_t maxYs = static_cast<int64_t>(height_) << kPixelBits;
    const auto fx = [&](double x) { return static_cast<int32_t>(std::clamp<int64_t>(RoundToInt(x * kOne), 0, maxXs)); };
    const auto fy = [&](double y) { return static_cast<int32_t>(std::clamp<int64_t>(RoundToInt(y * kOne), 0, maxYs)); };
    Segment s{fx(x0), fy(y0), fx(x1), fy(y1), 1};
    if (s.y0 == s.y1) return;
    if (s.y0 > s.y1) {
        std::swap(s.x0, s.x1);
        std::swap(s.y0, s.y1);
        s.dir = -1;
    }
    segments_.push_back(s);
    if (maxY_ < minY_) {
        minY_ = s.y0;
        maxY_ = s.y1;
    } else {
        minY_ = std::min(minY_, s.y0);
        maxY_ = std::max(maxY_, s.y1);
    }
}

void Rasterizer::AddLine(double x0, double y0, double x1, double y1) {
    // NaN and infinities fail these comparisons.
    if (!(x0 > -1e300 && x0 < 1e300 && y0 > -1e300 && y0 < 1e300 && x1 > -1e300 && x1 < 1e300 && y1 > -1e300 &&
          y1 < 1e300))
        return;
    const double cx0 = 0, cy0 = 0, cx1 = width_, cy1 = height_;
    // Rows: only the part within [cy0, cy1] covers anything.
    if (y0 == y1) return;
    if ((y0 <= cy0 && y1 <= cy0) || (y0 >= cy1 && y1 >= cy1)) return;
    const auto xAtY = [&](double y) { return x0 + (x1 - x0) * ((y - y0) / (y1 - y0)); };
    double ax = x0, ay = y0, bx = x1, by = y1;
    if (ay < cy0) ax = xAtY(cy0), ay = cy0;
    else if (ay > cy1) ax = xAtY(cy1), ay = cy1;
    if (by < cy0) bx = xAtY(cy0), by = cy0;
    else if (by > cy1) bx = xAtY(cy1), by = cy1;
    // Columns.
    if (ax >= cx1 && bx >= cx1) return;
    if (ax <= cx0 && bx <= cx0) {
        Push(cx0, ay, cx0, by);
        return;
    }
    const auto yAtX = [&](double x) {
        const double y = ay + (by - ay) * ((x - ax) / (bx - ax));
        const double lo = ay < by ? ay : by, hi = ay < by ? by : ay;
        return y < lo ? lo : (y > hi ? hi : y);
    };
    // Cut at the left edge: the outside part stands on it.
    if (ax < cx0 || bx < cx0) {
        const double ys = yAtX(cx0);
        if (ax < cx0) {
            Push(cx0, ay, cx0, ys);
            ax = cx0, ay = ys;
        } else {
            Push(cx0, ys, cx0, by);
            bx = cx0, by = ys;
        }
    }
    // Cut at the right edge: the outside part is dropped.
    if (ax > cx1 || bx > cx1) {
        const double ys = yAtX(cx1);
        if (ax > cx1)
            ax = cx1, ay = ys;
        else
            bx = cx1, by = ys;
    }
    Push(ax, ay, bx, by);
}

void Rasterizer::Fill(const Path& path, const Matrix& m, FillRule rule, uint8_t* pixels, uint32_t width,
                      uint32_t height, uint8_t ink, bool darkenOnly) {
    overflowed_ = false;
    segments_.clear();
    minY_ = 0, maxY_ = -1;
    if (width == 0 || height == 0 || width > kMaxCanvasSide || height > kMaxCanvasSide || path.Empty()) return;
    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);
    Sink sink(*this);
    Flatten(path, m, kFlattenTolerance, -1.0, -1.0, width_ + 1.0, height_ + 1.0, sink);
    if (segments_.empty()) return;

    const size_t w = width;
    if (cells_.size() < w * 2 * kBandRows) cells_.assign(w * 2 * kBandRows, 0);
    // Bucket the segments by their first band (counting sort).
    const int bandCount = (height_ + kBandRows - 1) / kBandRows;
    bandStart_.assign(static_cast<size_t>(bandCount) + 1, 0);
    for (const Segment& s : segments_) bandStart_[static_cast<size_t>(s.y0 >> kBandShift) + 1]++;
    for (int b = 0; b < bandCount; ++b) bandStart_[b + 1] += bandStart_[b];
    order_.resize(segments_.size());
    for (uint32_t i = 0; i < segments_.size(); ++i) order_[bandStart_[static_cast<size_t>(segments_[i].y0 >> kBandShift)]++] = i;
    for (int b = bandCount; b > 0; --b) bandStart_[b] = bandStart_[b - 1];
    bandStart_[0] = 0;

    const bool evenOdd = rule == FillRule::EvenOdd;
    active_.clear();
    const int firstBand = minY_ >> kBandShift;
    const int lastBand = (maxY_ - 1) >> kBandShift;
    for (int band = firstBand; band <= lastBand && band < bandCount; ++band) {
        for (uint32_t i = bandStart_[band]; i < bandStart_[band + 1]; ++i) active_.push_back(order_[i]);
        if (active_.empty()) continue;
        const int bandTop = band * kBandRows;
        const int rowsInBand = std::min(kBandRows, height_ - bandTop);
        for (int i = 0; i < rowsInBand; ++i) minCell_[i] = INT32_MAX, maxCell_[i] = -1;
        for (size_t i = 0; i < active_.size();) {
            const Segment& s = segments_[active_[i]];
            RenderBand(s, bandTop);
            if (((s.y1 - 1) >> kBandShift) <= band) {
                active_[i] = active_.back();
                active_.pop_back();
            } else {
                ++i;
            }
        }
        for (int i = 0; i < rowsInBand; ++i) {
            if (maxCell_[i] < minCell_[i]) continue;
            int32_t* row = cells_.data() + 2 * static_cast<ptrdiff_t>(i) * width_;
            uint8_t* out = pixels + static_cast<size_t>(bandTop + i) * w;
            int32_t acc = 0;
            // Past the last touched cell the coverage stays at the row's total cover: on to the
            // right edge unless it is zero there.
            int32_t c = minCell_[i];
            for (; c <= maxCell_[i]; ++c) {
                acc = WrapAdd(acc, row[2 * c]);
                const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(acc) << 9) - static_cast<uint32_t>(row[2 * c + 1]));
                row[2 * c] = row[2 * c + 1] = 0;
                int32_t a = v >> 9;
                a = a < 0 ? -a : a;
                if (evenOdd) {
                    a &= 511;
                    if (a > 256) a = 512 - a;
                }
                const int32_t coverage = a > 255 ? 255 : a;
                if (coverage == 0) continue;
                const int32_t old = out[c];
                const int32_t mixed = old + (static_cast<int32_t>(ink) - old) * coverage / 255;
                if (mixed < old || !darkenOnly) out[c] = static_cast<uint8_t>(mixed);
            }
            if (acc != 0) {
                int32_t a = static_cast<int32_t>(static_cast<uint32_t>(acc) << 9) >> 9;
                a = a < 0 ? -a : a;
                if (evenOdd) {
                    a &= 511;
                    if (a > 256) a = 512 - a;
                }
                const int32_t coverage = a > 255 ? 255 : a;
                for (; coverage > 0 && c < width_; ++c) {
                    const int32_t old = out[c];
                    const int32_t mixed = old + (static_cast<int32_t>(ink) - old) * coverage / 255;
                    if (mixed < old || !darkenOnly) out[c] = static_cast<uint8_t>(mixed);
                }
            }
        }
    }
    // Cells of rows never swept stay zero: every row with a touched cell was cleared above.
}

} // namespace RayoPdf::Raster
