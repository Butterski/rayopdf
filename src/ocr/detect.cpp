// SPDX-License-Identifier: Apache-2.0
#include "ocr/detect.h"

#include <algorithm>
#include <cmath>

namespace RayoPdf::Ocr {
namespace {

uint32_t Find(std::vector<uint32_t>& parent, uint32_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

void Union(std::vector<uint32_t>& parent, uint32_t a, uint32_t b) {
    a = Find(parent, a);
    b = Find(parent, b);
    // The smaller index stays the root, so a component's root is its first run in raster order.
    if (a < b) {
        parent[b] = a;
    } else if (b < a) {
        parent[a] = b;
    }
}

float Cross(const float* o, const float* a, const float* b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

// Convex hull (Andrew's monotone chain) of `count` points (x, y pairs) already sorted by (y, x),
// into `hull` as a closed-free polygon; collinear points dropped.
void ConvexHull(const std::vector<float>& points, std::vector<float>& hull) {
    const size_t n = points.size() / 2;
    hull.clear();
    if (n < 3) {
        hull = points;
        return;
    }
    hull.resize(4 * n);
    size_t k = 0;
    for (size_t i = 0; i < n; ++i) {
        while (k >= 2 && Cross(&hull[2 * (k - 2)], &hull[2 * (k - 1)], &points[2 * i]) <= 0) --k;
        hull[2 * k] = points[2 * i];
        hull[2 * k + 1] = points[2 * i + 1];
        ++k;
    }
    const size_t lower = k + 1;
    for (size_t i = n - 1; i-- > 0;) {
        while (k >= lower && Cross(&hull[2 * (k - 2)], &hull[2 * (k - 1)], &points[2 * i]) <= 0) --k;
        hull[2 * k] = points[2 * i];
        hull[2 * k + 1] = points[2 * i + 1];
        ++k;
    }
    hull.resize(2 * (k - 1));
}

// The minimum-area rectangle around a convex polygon (one of its sides lies on a polygon edge).
// False when the polygon has no extent.
bool MinAreaRect(const std::vector<float>& hull, RotatedRect& rect) {
    const size_t n = hull.size() / 2;
    if (n < 2) return false;
    double bestArea = -1;
    for (size_t e = 0; e < n; ++e) {
        const size_t f = e + 1 == n ? 0 : e + 1;
        const double dx = double(hull[2 * f]) - hull[2 * e], dy = double(hull[2 * f + 1]) - hull[2 * e + 1];
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0) continue;
        const double ux = dx / length, uy = dy / length;
        double aMin = 0, aMax = 0, bMin = 0, bMax = 0;
        for (size_t i = 0; i < n; ++i) {
            const double a = hull[2 * i] * ux + hull[2 * i + 1] * uy;
            const double b = -hull[2 * i] * uy + hull[2 * i + 1] * ux;
            if (i == 0 || a < aMin) aMin = a;
            if (i == 0 || a > aMax) aMax = a;
            if (i == 0 || b < bMin) bMin = b;
            if (i == 0 || b > bMax) bMax = b;
        }
        const double area = (aMax - aMin) * (bMax - bMin);
        if (bestArea < 0 || area < bestArea) {
            bestArea = area;
            const double a = (aMin + aMax) / 2, b = (bMin + bMax) / 2;
            rect.cx = float(a * ux - b * uy);
            rect.cy = float(a * uy + b * ux);
            rect.ux = float(ux);
            rect.uy = float(uy);
            rect.width = float(aMax - aMin);
            rect.height = float(bMax - bMin);
        }
    }
    if (bestArea < 0) return false;
    // The width axis is the one nearer horizontal, pointing right (or down when vertical).
    if (std::fabs(rect.ux) < std::fabs(rect.uy)) {
        const float ux = -rect.uy, uy = rect.ux;
        rect.ux = ux;
        rect.uy = uy;
        const float w = rect.width;
        rect.width = rect.height;
        rect.height = w;
    }
    if (rect.ux < 0 || (rect.ux == 0 && rect.uy < 0)) {
        rect.ux = -rect.ux;
        rect.uy = -rect.uy;
    }
    return true;
}

// The x range [lo, hi] of row y inside `rect`; false when the row misses it.
bool RowSpan(const RotatedRect& rect, float y, float& lo, float& hi) {
    lo = -1e30f;
    hi = 1e30f;
    const float dy = y - rect.cy;
    // a = (x - cx) * ux + dy * uy in [-w/2, w/2];  b = -(x - cx) * uy + dy * ux in [-h/2, h/2].
    const float limits[2][3] = {{rect.ux, dy * rect.uy, rect.width / 2}, {-rect.uy, dy * rect.ux, rect.height / 2}};
    for (const auto& l : limits) {
        const float k = l[0], c = l[1], half = l[2];
        if (std::fabs(k) < 1e-6f) {
            if (c < -half || c > half) return false;
            continue;
        }
        float a = (-half - c) / k, b = (half - c) / k;
        if (a > b) std::swap(a, b);
        if (a > lo) lo = a;
        if (b < hi) hi = b;
    }
    lo += rect.cx;
    hi += rect.cx;
    return lo <= hi;
}

} // namespace

void DetectBoxes(const float* prob, uint32_t mapWidth, uint32_t mapHeight, float scaleX, float scaleY,
                 const DetectParams& params, DetectScratch& s, std::vector<TextBox>& boxes) {
    boxes.clear();
    s.runs.clear();
    if (mapWidth == 0 || mapHeight == 0) return;
    const float thresh = params.thresh;
    s.rowStart.assign(size_t(mapHeight) + 1, 0);
    for (uint32_t y = 0; y < mapHeight; ++y) {
        s.rowStart[y] = uint32_t(s.runs.size());
        const float* row = prob + size_t(y) * mapWidth;
        uint32_t x = 0;
        while (x < mapWidth) {
            if (row[x] > thresh) {
                const uint32_t start = x;
                while (x < mapWidth && row[x] > thresh) ++x;
                s.runs.push_back({y, start, x - 1});
            } else {
                ++x;
            }
        }
    }
    s.rowStart[mapHeight] = uint32_t(s.runs.size());
    const uint32_t runCount = uint32_t(s.runs.size());
    if (runCount == 0) return;

    // 8-connected components: runs of neighbouring rows that overlap or touch diagonally.
    s.parent.resize(runCount);
    for (uint32_t i = 0; i < runCount; ++i) s.parent[i] = i;
    for (uint32_t y = 1; y < mapHeight; ++y) {
        uint32_t i = s.rowStart[y - 1], j = s.rowStart[y];
        const uint32_t iEnd = s.rowStart[y], jEnd = s.rowStart[y + 1];
        while (i < iEnd && j < jEnd) {
            const DetectScratch::Run& a = s.runs[i];
            const DetectScratch::Run& b = s.runs[j];
            if (a.x1 + 1 < b.x0) {
                ++i;
            } else if (b.x1 + 1 < a.x0) {
                ++j;
            } else {
                Union(s.parent, i, j);
                if (a.x1 < b.x1) {
                    ++i;
                } else {
                    ++j;
                }
            }
        }
    }
    s.component.resize(runCount);
    uint32_t components = 0;
    for (uint32_t i = 0; i < runCount; ++i) {
        const uint32_t root = Find(s.parent, i);
        s.component[i] = root == i ? components++ : s.component[root];
    }
    // Runs grouped by component, raster order kept (a counting sort).
    s.start.assign(size_t(components) + 1, 0);
    s.pixels.assign(components, 0);
    s.probSum.assign(components, 0.0);
    for (uint32_t i = 0; i < runCount; ++i) {
        const DetectScratch::Run& r = s.runs[i];
        const uint32_t c = s.component[i];
        ++s.start[c + 1];
        s.pixels[c] += r.x1 - r.x0 + 1;
        const float* row = prob + size_t(r.y) * mapWidth;
        double sum = 0;
        for (uint32_t x = r.x0; x <= r.x1; ++x) sum += row[x];
        s.probSum[c] += sum;
    }
    for (uint32_t c = 0; c < components; ++c) s.start[c + 1] += s.start[c];
    s.order.resize(runCount);
    {
        std::vector<uint32_t>& fill = s.parent;  // reused as the write positions
        for (uint32_t c = 0; c < components; ++c) fill[c] = s.start[c];
        for (uint32_t i = 0; i < runCount; ++i) s.order[fill[s.component[i]]++] = i;
    }

    const uint64_t budget = uint64_t(kScoreBudget) * mapWidth * mapHeight;
    uint64_t scored = 0;
    const uint32_t candidates = components < params.maxCandidates ? components : params.maxCandidates;
    for (uint32_t c = 0; c < candidates; ++c) {
        // Hull points: the leftmost and rightmost pixel of every row of the component.
        s.points.clear();
        const uint32_t begin = s.start[c], end = s.start[c + 1];
        for (uint32_t k = begin; k < end;) {
            const DetectScratch::Run& first = s.runs[s.order[k]];
            uint32_t last = k;
            while (last + 1 < end && s.runs[s.order[last + 1]].y == first.y) ++last;
            const uint32_t right = s.runs[s.order[last]].x1;
            s.points.push_back(float(first.x0));
            s.points.push_back(float(first.y));
            if (right != first.x0) {
                s.points.push_back(float(right));
                s.points.push_back(float(first.y));
            }
            k = last + 1;
        }
        ConvexHull(s.points, s.hull);
        RotatedRect rect;
        if (!MinAreaRect(s.hull, rect)) continue;
        if (std::min(rect.width, rect.height) < 3.0f) continue;

        // Mean probability inside the rectangle (the component's own pixels once the budget is spent).
        double score = s.probSum[c] / s.pixels[c];
        const uint64_t area = uint64_t(rect.width + 2) * uint64_t(rect.height + 2);
        if (scored + area <= budget) {
            const float extent = 0.5f * (std::fabs(rect.uy) * rect.width + std::fabs(rect.ux) * rect.height);
            float yLow = std::ceil(rect.cy - extent), yHigh = std::floor(rect.cy + extent);
            if (yLow < 0) yLow = 0;
            if (yHigh > float(mapHeight - 1)) yHigh = float(mapHeight - 1);
            double sum = 0;
            uint64_t count = 0;
            for (float yf = yLow; yf <= yHigh; yf += 1.0f) {
                float lo, hi;
                if (!RowSpan(rect, yf, lo, hi)) continue;
                lo = std::ceil(lo);
                hi = std::floor(hi);
                if (lo < 0) lo = 0;
                if (hi > float(mapWidth - 1)) hi = float(mapWidth - 1);
                if (lo > hi) continue;
                const float* row = prob + size_t(yf) * mapWidth;
                for (uint32_t x = uint32_t(lo); x <= uint32_t(hi); ++x) sum += row[x];
                count += uint32_t(hi) - uint32_t(lo) + 1;
            }
            scored += count;
            if (count > 0) score = sum / double(count);
        }
        if (score < params.boxThresh) continue;

        const float distance = rect.width * rect.height * params.unclipRatio / (2 * (rect.width + rect.height));
        rect.width += 2 * distance;
        rect.height += 2 * distance;
        if (std::min(rect.width, rect.height) < 5.0f) continue;

        TextBox box;
        const float wx = rect.ux * scaleX, wy = rect.uy * scaleY;
        const float hx = -rect.uy * scaleX, hy = rect.ux * scaleY;
        const float wl = std::sqrt(wx * wx + wy * wy), hl = std::sqrt(hx * hx + hy * hy);
        box.rect.cx = rect.cx * scaleX;
        box.rect.cy = rect.cy * scaleY;
        box.rect.ux = wx / wl;
        box.rect.uy = wy / wl;
        box.rect.width = rect.width * wl;
        box.rect.height = rect.height * hl;
        box.score = float(score);
        boxes.push_back(box);
    }
}

PixelBox Bounds(const RotatedRect& rect, uint32_t width, uint32_t height) {
    const float ex = 0.5f * (std::fabs(rect.ux) * rect.width + std::fabs(rect.uy) * rect.height);
    const float ey = 0.5f * (std::fabs(rect.uy) * rect.width + std::fabs(rect.ux) * rect.height);
    float x0 = std::ceil(rect.cx - ex), x1 = std::floor(rect.cx + ex) + 1;
    float y0 = std::ceil(rect.cy - ey), y1 = std::floor(rect.cy + ey) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > float(width)) x1 = float(width);
    if (y1 > float(height)) y1 = float(height);
    PixelBox box;
    box.x0 = int32_t(x0);
    box.y0 = int32_t(y0);
    box.x1 = x1 > x0 ? int32_t(x1) : box.x0;
    box.y1 = y1 > y0 ? int32_t(y1) : box.y0;
    return box;
}

namespace {

struct Item {
    float top, bottom, left;
    uint32_t index, row;
};

// Rows top to bottom (a box joins a row when it overlaps the row's first box vertically by at
// least half the smaller height), left to right in a row.
void OrderRows(Item* items, size_t n) {
    std::sort(items, items + n, [](const Item& a, const Item& b) {
        if (a.top != b.top) return a.top < b.top;
        if (a.left != b.left) return a.left < b.left;
        return a.index < b.index;
    });
    for (size_t i = 0; i < n; ++i) items[i].row = UINT32_MAX;
    uint32_t rows = 0;
    for (size_t i = 0; i < n; ++i) {
        if (items[i].row != UINT32_MAX) continue;
        items[i].row = rows;
        const Item& reference = items[i];
        for (size_t j = i + 1; j < n && items[j].top < reference.bottom; ++j) {
            if (items[j].row != UINT32_MAX) continue;
            const float overlap = std::min(reference.bottom, items[j].bottom) - items[j].top;
            const float smaller = std::min(reference.bottom - reference.top, items[j].bottom - items[j].top);
            if (overlap >= 0.5f * smaller) items[j].row = rows;
        }
        ++rows;
    }
    std::sort(items, items + n, [](const Item& a, const Item& b) {
        if (a.row != b.row) return a.row < b.row;
        if (a.left != b.left) return a.left < b.left;
        return a.index < b.index;
    });
}

} // namespace

void SortIntoLines(std::vector<TextBox>& boxes) {
    const size_t n = boxes.size();
    if (n < 2) return;
    std::vector<Item> items(n);
    for (size_t i = 0; i < n; ++i) {
        const RotatedRect& r = boxes[i].rect;
        const float ex = 0.5f * (std::fabs(r.ux) * r.width + std::fabs(r.uy) * r.height);
        const float ey = 0.5f * (std::fabs(r.uy) * r.width + std::fabs(r.ux) * r.height);
        items[i] = {r.cy - ey, r.cy + ey, r.cx - ex, uint32_t(i), UINT32_MAX};
    }
    OrderRows(items.data(), n);
    std::vector<TextBox> sorted(n);
    for (size_t i = 0; i < n; ++i) sorted[i] = boxes[items[i].index];
    boxes.swap(sorted);
}

} // namespace RayoPdf::Ocr
