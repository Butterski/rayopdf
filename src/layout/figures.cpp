// SPDX-License-Identifier: Apache-2.0
#include "layout/figures.h"

#include "common/minmax.h"
#include "common/per_thread.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace RayoPdf::Layout {
namespace {

constexpr float kCell = 4.0f;           // grid step in points: strokes a cell apart are one drawing
constexpr int kMaxSide = 512;           // grid cells per side at most (a larger page gets larger cells)
constexpr float kMark = 12.0f;          // a mark (a tick, a marker, an arrow head) is no larger
constexpr float kTick = 8.0f;           // an axis-aligned line this short is a tick, not a rule
constexpr float kSlant = 1.5f;          // a line rising or running this far is no axis-aligned rule
constexpr uint16_t kCurveSegments = 8;  // a path of this many segments is a curve or a polyline
constexpr int kCurveWeight = 4;         // marks a text-free curve counts for
constexpr int kMinWeight = 8;           // marks a region needs
constexpr float kMinSide = 30.0f;       // a region is at least this wide and this tall
constexpr float kMaxText = 0.35f;       // the share of a region under lines of text, at most
constexpr float kMaxPage = 0.5f;        // a stroke or an image over more of the page is its background
constexpr int kMaxTests = 256;          // candidate regions measured against the text per page
constexpr float kReach = 2.0f;          // ems around a region its labels stand in (ticks, axis titles)

struct Stroke {
    float x0, y0, x1, y1;
    bool mark;   // no larger than kMark
    bool curve;  // larger both ways, of kCurveSegments segments or more
};

struct Cluster {
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    int weight = 0;
};

// The working storage of FigureRegions, kept by the thread.
struct FigureScratch {
    std::vector<Stroke> strokes;
    std::vector<int32_t> cover;   // (ny + 1) x (nx + 1): strokes over each cell, then its cluster + 1
    std::vector<int32_t> text;    // (ny + 1) x (nx + 1): centres of text lines up to each cell (summed)
    std::vector<int32_t> stack;
    std::vector<Cluster> clusters;
};

// The regions MarkFigureText works with, kept by the thread.
struct FigureMarks {
    std::vector<FigureRegion> regions;
};

// The share of the box under upright lines of text whose centre lies in it.
float TextShare(const PageData& page, float x0, float y0, float x1, float y1) {
    double area = 0;
    for (const Line& l : page.lines) {
        if (l.dir != 0 || l.chars == 0 || (l.flags & kLineTableAnchor)) continue;
        const float cx = (l.u0 + l.u1) / 2, cy = (l.vb + l.vt) / 2;
        if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
        area += double(l.u1 - l.u0) * double(l.vt - l.vb);
    }
    const double box = double(x1 - x0) * double(y1 - y0);
    return box > 0 ? float(area / box) : 1.0f;
}

} // namespace

void FigureRegions(const PageData& page, std::vector<FigureRegion>& regions) {
    regions.clear();
    const float width = page.width, height = page.height;
    if (!(width > 0) || !(height > 0) || !std::isfinite(width) || !std::isfinite(height)) return;
    const float pageArea = width * height;
    FigureScratch& scratch = PerThread<FigureScratch>();
    std::vector<Stroke>& strokes = scratch.strokes;
    strokes.clear();
    int potential = 0;  // the weight all strokes could give, at most
    for (const Shape& s : page.shapes) {
        if (s.kind == ShapeKind::Rect) continue;
        const float x0 = Fmax(s.x0, 0.0f), y0 = Fmax(s.y0, 0.0f), x1 = Fmin(s.x1, width), y1 = Fmin(s.y1, height);
        const float w = x1 - x0, h = y1 - y0;
        if (!(w >= 0) || !(h >= 0) || !std::isfinite(w) || !std::isfinite(h) || w * h > kMaxPage * pageArea) continue;
        const float longer = Fmax(w, h), shorter = Fmin(w, h);
        // A long thin stroke along an axis is a rule (one line, or the pieces of a dashed one).
        if (shorter <= kSlant && longer > kTick) continue;
        const bool mark = longer <= kMark, curve = shorter > kMark && s.segments >= kCurveSegments;
        strokes.push_back(Stroke{x0, y0, x1, y1, mark, curve});
        if (potential < kMinWeight) potential += mark ? 1 : curve ? kCurveWeight : 0;
    }
    bool pictures = false;
    for (const ImageBox& im : page.images) pictures = pictures || (im.x1 - im.x0 >= kMinSide && im.y1 - im.y0 >= kMinSide);
    // No region without a picture or strokes that could weigh enough (the grid below costs a pass
    // over the page per stage).
    if ((strokes.size() < 2 || potential < kMinWeight) && !pictures) return;

    // The grid covers the cells the strokes touch, aligned with a page-wide grid of the same cells
    // (a stroke's cells and the clusters are those of the whole page; text outside counts for none).
    const float cell = Fmax(kCell, Fmax(width, height) / float(kMaxSide));
    const int pageX = std::min(kMaxSide, static_cast<int>(width / cell) + 1);
    const int pageY = std::min(kMaxSide, static_cast<int>(height / cell) + 1);
    const auto pageColumn = [&](float x) { return std::min(pageX - 1, std::max(0, static_cast<int>(x / cell))); };
    const auto pageRow = [&](float y) { return std::min(pageY - 1, std::max(0, static_cast<int>(y / cell))); };
    int c0 = pageX, c1 = 0, r0 = pageY, r1 = 0;
    for (const Stroke& s : strokes) {
        c0 = std::min(c0, pageColumn(s.x0)), c1 = std::max(c1, pageColumn(s.x1));
        r0 = std::min(r0, pageRow(s.y0)), r1 = std::max(r1, pageRow(s.y1));
    }
    if (strokes.empty()) c0 = c1 = r0 = r1 = 0;
    const int nx = c1 - c0 + 1, ny = r1 - r0 + 1;
    const int stride = nx + 1;
    const auto column = [&](float x) { return pageColumn(x) - c0; };
    const auto row = [&](float y) { return pageRow(y) - r0; };
    const size_t cells = static_cast<size_t>(stride) * static_cast<size_t>(ny + 1);

    // Strokes over each cell: corner counts, summed along rows and columns.
    std::vector<int32_t>& cover = scratch.cover;
    cover.assign(cells, 0);
    for (const Stroke& s : strokes) {
        const int a = column(s.x0), b = column(s.x1) + 1, c = row(s.y0), d = row(s.y1) + 1;
        cover[static_cast<size_t>(c * stride + a)]++;
        cover[static_cast<size_t>(c * stride + b)]--;
        cover[static_cast<size_t>(d * stride + a)]--;
        cover[static_cast<size_t>(d * stride + b)]++;
    }
    for (int y = 0; y <= ny; ++y)
        for (int x = 1; x <= nx; ++x) cover[static_cast<size_t>(y * stride + x)] += cover[static_cast<size_t>(y * stride + x - 1)];
    for (int y = 1; y <= ny; ++y)
        for (int x = 0; x <= nx; ++x) cover[static_cast<size_t>(y * stride + x)] += cover[static_cast<size_t>((y - 1) * stride + x)];

    // Centres of upright text lines up to each cell: text[(y + 1) * stride + x + 1] counts those in
    // cells [0, x] x [0, y].
    std::vector<int32_t>& text = scratch.text;
    text.assign(cells, 0);
    for (const Line& l : page.lines) {
        if (l.dir != 0 || l.chars == 0 || (l.flags & kLineTableAnchor)) continue;
        const float cx = (l.u0 + l.u1) / 2, cy = (l.vb + l.vt) / 2;
        if (!(cx >= 0 && cx <= width && cy >= 0 && cy <= height)) continue;
        const int x = pageColumn(cx), y = pageRow(cy);
        if (x < c0 || x > c1 || y < r0 || y > r1) continue;
        text[static_cast<size_t>((y - r0 + 1) * stride + x - c0 + 1)]++;
    }
    for (int y = 1; y <= ny; ++y)
        for (int x = 1; x <= nx; ++x)
            text[static_cast<size_t>(y * stride + x)] += text[static_cast<size_t>(y * stride + x - 1)] +
                                                         text[static_cast<size_t>((y - 1) * stride + x)] -
                                                         text[static_cast<size_t>((y - 1) * stride + x - 1)];
    const auto textFree = [&](const Stroke& s) {
        const int a = column(s.x0), b = column(s.x1) + 1, c = row(s.y0), d = row(s.y1) + 1;
        return text[static_cast<size_t>(d * stride + b)] - text[static_cast<size_t>(c * stride + b)] -
                   text[static_cast<size_t>(d * stride + a)] + text[static_cast<size_t>(c * stride + a)] == 0;
    };

    // Clusters: covered cells that touch (corners too). A covered cell becomes its cluster + 1, an
    // uncovered one stays 0.
    std::vector<int32_t>& stack = scratch.stack;
    std::vector<Cluster>& clusters = scratch.clusters;
    clusters.clear();
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) cover[static_cast<size_t>(y * stride + x)] = cover[static_cast<size_t>(y * stride + x)] > 0 ? -1 : 0;
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            if (cover[static_cast<size_t>(y * stride + x)] != -1) continue;
            clusters.emplace_back();
            const int32_t id = static_cast<int32_t>(clusters.size());
            stack.clear();
            stack.push_back(y * stride + x);
            cover[static_cast<size_t>(y * stride + x)] = id;
            while (!stack.empty()) {
                const int at = stack.back();
                stack.pop_back();
                const int cy = at / stride, cx = at % stride;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int qx = cx + dx, qy = cy + dy;
                        if (qx < 0 || qx >= nx || qy < 0 || qy >= ny) continue;
                        const size_t q = static_cast<size_t>(qy * stride + qx);
                        if (cover[q] != -1) continue;
                        cover[q] = id;
                        stack.push_back(static_cast<int32_t>(q));
                    }
                }
            }
        }
    }
    for (const Stroke& s : strokes) {
        const int32_t id = cover[static_cast<size_t>(row(s.y0) * stride + column(s.x0))];
        if (id <= 0) continue;
        Cluster& c = clusters[static_cast<size_t>(id - 1)];
        c.x0 = Fmin(c.x0, s.x0), c.y0 = Fmin(c.y0, s.y0), c.x1 = Fmax(c.x1, s.x1), c.y1 = Fmax(c.y1, s.y1);
        if (s.mark) c.weight += 1;
        else if (s.curve && textFree(s)) c.weight += kCurveWeight;
    }
    int tests = 0;
    const auto add = [&](float x0, float y0, float x1, float y1) {
        if (x1 - x0 < kMinSide || y1 - y0 < kMinSide || (x1 - x0) * (y1 - y0) > kMaxPage * pageArea) return true;
        if (++tests > kMaxTests) return false;
        if (TextShare(page, x0, y0, x1, y1) > kMaxText) return true;
        if (regions.size() >= kMaxFigureRegions) return false;
        regions.push_back(FigureRegion{x0, y0, x1, y1});
        return true;
    };
    bool bounded = true;
    for (const Cluster& c : clusters)
        if (bounded && c.weight >= kMinWeight) bounded = add(c.x0, c.y0, c.x1, c.y1);
    for (const ImageBox& im : page.images)
        if (bounded && std::isfinite(im.x0) && std::isfinite(im.y0) && std::isfinite(im.x1) && std::isfinite(im.y1))
            bounded = add(Fmax(im.x0, 0.0f), Fmax(im.y0, 0.0f), Fmin(im.x1, width), Fmin(im.y1, height));
    if (!bounded) regions.clear();
}

void MarkFigureText(PageData& page) {
    // Only a page with a figure's caption ("Figure 3", "Fig. 2", "FIG. 1") is searched: its text
    // matters where it stands among prose, and the search costs a pass over the drawing's cells.
    bool captioned = false;
    for (size_t i = 0; i < page.lines.size() && !captioned; ++i) {
        const Line& l = page.lines[i];
        if (l.dir != 0 || l.count < 3) continue;
        captioned = true;
        for (uint32_t k = 0; k < 3 && captioned; ++k) {
            const std::string_view t = page.GlyphText(page.glyphs[page.lineGlyphs[l.first + k]]);
            captioned = t.size() == 1 && (t[0] | 0x20) == "fig"[k];
        }
    }
    if (!captioned) return;
    std::vector<FigureRegion>& regions = PerThread<FigureMarks>().regions;
    FigureRegions(page, regions);
    if (regions.empty()) return;
    for (Line& l : page.lines) {
        if (l.dir != 0 || l.chars == 0 || (l.flags & (kLineRemoved | kLineAnchor | kLineTableCell))) continue;
        const float cx = (l.u0 + l.u1) / 2, cy = (l.vb + l.vt) / 2, reach = kReach * l.size;
        for (const FigureRegion& r : regions)
            if (cx >= r.x0 - reach && cx <= r.x1 + reach && cy >= r.y0 - reach && cy <= r.y1 + reach) {
                l.flags |= kLineFigure;
                break;
            }
    }
}

} // namespace RayoPdf::Layout
