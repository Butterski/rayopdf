// SPDX-License-Identifier: Apache-2.0
#include "layout/figures.h"

#include "common/minmax.h"
#include "common/per_thread.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

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
    int32_t a, b, c, d;  // the grid cells it lies over: columns a to b, rows c to d
    int32_t cluster;
    bool mark;   // no larger than kMark
    bool curve;  // larger both ways, of kCurveSegments segments or more
};

// Covered cells x0 to x1 of a grid row, and the run it joins (union-find over runs).
struct Run {
    int32_t x0, x1, parent, cluster;
};

// The working storage of FigureRegions, kept by the thread.
struct FigureScratch {
    std::vector<Stroke> strokes;
    std::vector<uint64_t> covered;  // ny rows of `words` words: the cells under a stroke
    std::vector<int32_t> cover;     // (ny + 1) x (nx + 1): strokes over each cell (corner counts summed)
    std::vector<int32_t> text;      // (ny + 1) x (nx + 1): centres of text lines up to each cell (summed)
    std::vector<Run> runs;          // row by row, left to right
    std::vector<uint32_t> rowRuns;  // ny + 1: the first run of each row
    std::vector<uint32_t> centres;  // the cells of text lines' centres: row << 16 | column, from (c0, r0)
    std::vector<int32_t> weights;   // per cluster
};

// The regions MarkFigureText works with, kept by the thread.
struct FigureMarks {
    std::vector<FigureRegion> regions;
};

// Chart axes (ChartAxes).
constexpr float kTickMin = 1.0f;         // a tick is a stroke this long at least,
constexpr float kTickMax = 8.0f;         // this long at most (kTick, as long as a mark's side),
constexpr float kTickThick = 1.0f;       // this thick at most,
constexpr float kTickAspect = 3.0f;      // and this many times longer than thick
constexpr float kTickRow = 0.6f;         // the ticks of an axis start within this of the one before
constexpr float kTickTouch = 0.75f;      // a shape this close to a tick's free end holds it
constexpr float kTickStroke = 1.5f;      // a shape this thin is a stroke, not an area
constexpr float kTickMinStep = 3.0f;     // ticks this close are no axis's
constexpr float kTickEven = 0.6f;        // the share of steps that equal the median step, at least
constexpr float kAxisCell = 8.0f;          // cells of the shape index (larger on a larger page)
constexpr int kAxisSide = 512;             // index cells per side at most
constexpr size_t kMaxShapeCells = 256;     // a shape over more cells is looked at by every search
constexpr size_t kMaxWideShapes = 1024;    // such shapes per page at most (more: no axes)
constexpr size_t kMaxAxisEntries = size_t(1) << 20;  // index entries per page at most (more: no axes)
constexpr size_t kMaxTicks = size_t(1) << 15;        // candidate ticks per page at most (more: no axes)
constexpr size_t kMaxAxes = 256;                     // axes per page at most
constexpr uint32_t kMaxAxisWork = uint32_t(1) << 23;  // shapes and lines looked at per page at most

// A tick: across `pos` (the x of an upright tick, the y of a level one), along [a, b].
struct Tick {
    float pos, a, b;
    uint32_t shape;
};

// The working storage of ChartAxes, kept by the thread.
struct AxisScratch {
    std::vector<Tick> ticks[2];           // [0] upright (an x axis's), [1] level (a y axis's)
    std::vector<uint32_t> order;          // ticks by the end they start from
    std::vector<Tick> free;               // a row's ticks that end free
    std::vector<float> positions, steps;  // their positions, and the steps between them
    std::vector<uint64_t> cells;          // the shape index: cell << 32 | shape, ascending
    std::vector<uint32_t> wide;           // shapes over more than kMaxShapeCells cells
    std::vector<FigureRegion> lines;      // the lines of text on the upright page
};

// The index of the lowest set bit of m (m != 0).
int LowestBit(uint64_t m) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(m);
#else
    int n = 0;
    for (; !(m & 1); m >>= 1) n++;
    return n;
#endif
}

// The first cell at or after x of the row (`words` words) that is covered (`set`) or not; the
// row's end (64 x words) when there is none.
int NextCell(const uint64_t* row, int words, int x, bool set) {
    int w = x >> 6;
    if (w >= words) return words * 64;
    uint64_t v = (set ? row[w] : ~row[w]) & (~uint64_t(0) << (x & 63));
    while (v == 0) {
        if (++w >= words) return words * 64;
        v = set ? row[w] : ~row[w];
    }
    return w * 64 + LowestBit(v);
}

int32_t FindRun(std::vector<Run>& runs, int32_t i) {
    while (runs[static_cast<size_t>(i)].parent != i) {
        Run& r = runs[static_cast<size_t>(i)];
        r.parent = runs[static_cast<size_t>(r.parent)].parent;
        i = r.parent;
    }
    return i;
}

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

// The page's FigureStrokes from its shapes and images.
void FindStrokes(PageData& page) {
    FigureStrokes& found = page.figureStrokes;
    found.Clear();
    found.known = true;
    found.possible = false;
    const float width = page.width, height = page.height;
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
        strokes.push_back(Stroke{x0, y0, x1, y1, 0, 0, 0, 0, 0, mark, curve});
        if (potential < kMinWeight) potential += mark ? 1 : curve ? kCurveWeight : 0;
    }
    bool pictures = false;
    for (const ImageBox& im : page.images) pictures = pictures || (im.x1 - im.x0 >= kMinSide && im.y1 - im.y0 >= kMinSide);
    // No region without a picture or strokes that could weigh enough; no cluster without such
    // strokes.
    found.possible = potential >= kMinWeight || pictures;
    if (potential < kMinWeight) return;

    // The grid covers the cells the strokes touch, aligned with a page-wide grid of the same cells
    // (a stroke's cells and the clusters are those of the whole page; text outside counts for none).
    const float cell = Fmax(kCell, Fmax(width, height) / float(kMaxSide));
    const int pageX = std::min(kMaxSide, static_cast<int>(width / cell) + 1);
    const int pageY = std::min(kMaxSide, static_cast<int>(height / cell) + 1);
    const auto pageColumn = [&](float x) { return std::min(pageX - 1, std::max(0, static_cast<int>(x / cell))); };
    const auto pageRow = [&](float y) { return std::min(pageY - 1, std::max(0, static_cast<int>(y / cell))); };
    int c0 = pageX, c1 = 0, r0 = pageY, r1 = 0;
    for (Stroke& s : strokes) {
        s.a = pageColumn(s.x0), s.b = pageColumn(s.x1), s.c = pageRow(s.y0), s.d = pageRow(s.y1);
        c0 = std::min(c0, s.a), c1 = std::max(c1, s.b);
        r0 = std::min(r0, s.c), r1 = std::max(r1, s.d);
    }
    found.cell = cell, found.pageX = pageX, found.pageY = pageY;
    found.c0 = c0, found.c1 = c1, found.r0 = r0, found.r1 = r1;
    const int nx = c1 - c0 + 1, ny = r1 - r0 + 1;
    const int stride = nx + 1;
    const size_t cells = static_cast<size_t>(stride) * static_cast<size_t>(ny + 1);
    for (Stroke& s : strokes) s.a -= c0, s.b -= c0, s.c -= r0, s.d -= r0;

    // The cells under a stroke, a bit each: painted stroke by stroke, or, when the strokes overlap
    // so much that painting costs more than a pass over the grid, from strokes over each cell
    // (corner counts summed along rows and columns).
    const int words = (nx + 63) / 64;
    std::vector<uint64_t>& covered = scratch.covered;
    covered.assign(static_cast<size_t>(ny) * static_cast<size_t>(words), 0);
    size_t paint = 0;
    for (const Stroke& s : strokes) paint += static_cast<size_t>(s.d - s.c + 1) * static_cast<size_t>((s.b >> 6) - (s.a >> 6) + 1);
    if (paint <= cells) {
        for (const Stroke& s : strokes) {
            const int wa = s.a >> 6, wb = s.b >> 6;
            const uint64_t first = ~uint64_t(0) << (s.a & 63), last = ~uint64_t(0) >> (63 - (s.b & 63));
            for (int y = s.c; y <= s.d; ++y) {
                uint64_t* row = covered.data() + static_cast<size_t>(y) * static_cast<size_t>(words);
                if (wa == wb) {
                    row[wa] |= first & last;
                    continue;
                }
                row[wa] |= first;
                for (int w = wa + 1; w < wb; ++w) row[w] = ~uint64_t(0);
                row[wb] |= last;
            }
        }
    } else {
        std::vector<int32_t>& cover = scratch.cover;
        cover.assign(cells, 0);
        for (const Stroke& s : strokes) {
            const int a = s.a, b = s.b + 1, c = s.c, d = s.d + 1;
            cover[static_cast<size_t>(c * stride + a)]++;
            cover[static_cast<size_t>(c * stride + b)]--;
            cover[static_cast<size_t>(d * stride + a)]--;
            cover[static_cast<size_t>(d * stride + b)]++;
        }
        for (int y = 0; y <= ny; ++y)
            for (int x = 1; x <= nx; ++x) cover[static_cast<size_t>(y * stride + x)] += cover[static_cast<size_t>(y * stride + x - 1)];
        for (int y = 1; y <= ny; ++y)
            for (int x = 0; x <= nx; ++x) cover[static_cast<size_t>(y * stride + x)] += cover[static_cast<size_t>((y - 1) * stride + x)];
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                if (cover[static_cast<size_t>(y * stride + x)] > 0)
                    covered[static_cast<size_t>(y) * static_cast<size_t>(words) + static_cast<size_t>(x >> 6)] |= uint64_t(1) << (x & 63);
    }

    // Clusters: covered cells that touch (corners too). Each row's covered cells are runs; a run
    // joins the runs of the row above that reach its cells or their corners. Clusters are numbered
    // in the order of their first cell (row by row, left to right).
    std::vector<Run>& runs = scratch.runs;
    std::vector<uint32_t>& rowRuns = scratch.rowRuns;
    runs.clear();
    rowRuns.resize(static_cast<size_t>(ny) + 1);
    for (int y = 0; y < ny; ++y) {
        rowRuns[static_cast<size_t>(y)] = static_cast<uint32_t>(runs.size());
        const uint64_t* row = covered.data() + static_cast<size_t>(y) * static_cast<size_t>(words);
        for (int x = NextCell(row, words, 0, true); x < nx;) {
            const int end = NextCell(row, words, x, false);
            const int32_t id = static_cast<int32_t>(runs.size());
            runs.push_back(Run{x, end - 1, id, -1});
            x = end < nx ? NextCell(row, words, end, true) : nx;
        }
        if (y == 0) continue;
        // Two pointers over the row above's runs and this row's, both left to right.
        size_t i = rowRuns[static_cast<size_t>(y) - 1], j = rowRuns[static_cast<size_t>(y)];
        const size_t aboveEnd = rowRuns[static_cast<size_t>(y)];
        while (i < aboveEnd && j < runs.size()) {
            const Run p = runs[i], q = runs[j];
            if (p.x1 + 1 >= q.x0 && q.x1 + 1 >= p.x0) {
                const int32_t ra = FindRun(runs, static_cast<int32_t>(i)), rb = FindRun(runs, static_cast<int32_t>(j));
                if (ra != rb) runs[static_cast<size_t>(std::max(ra, rb))].parent = std::min(ra, rb);
            }
            if (p.x1 < q.x1) ++i;
            else ++j;
        }
    }
    rowRuns[static_cast<size_t>(ny)] = static_cast<uint32_t>(runs.size());
    std::vector<FigureStrokes::Cluster>& clusters = found.clusters;
    for (size_t i = 0; i < runs.size(); ++i) {
        Run& root = runs[static_cast<size_t>(FindRun(runs, static_cast<int32_t>(i)))];
        if (root.cluster < 0) {
            root.cluster = static_cast<int32_t>(clusters.size());
            clusters.push_back(FigureStrokes::Cluster{INFINITY, INFINITY, -INFINITY, -INFINITY, 0});
        }
        runs[i].cluster = root.cluster;
    }
    // Each stroke's cluster: that of the run holding its first cell. Marks weigh here; the curves
    // of clusters whose marks weigh too little are kept, to be weighed against the text.
    bool curves = false;
    for (Stroke& s : strokes) {
        const auto first = runs.begin() + rowRuns[static_cast<size_t>(s.c)], last = runs.begin() + rowRuns[static_cast<size_t>(s.c) + 1];
        const auto at = std::upper_bound(first, last, s.a, [](int32_t x, const Run& r) { return x < r.x0; }) - 1;
        s.cluster = at->cluster;
        FigureStrokes::Cluster& c = clusters[static_cast<size_t>(s.cluster)];
        c.x0 = Fmin(c.x0, s.x0), c.y0 = Fmin(c.y0, s.y0), c.x1 = Fmax(c.x1, s.x1), c.y1 = Fmax(c.y1, s.y1);
        if (s.mark) c.weight += 1;
        curves = curves || s.curve;
    }
    if (!curves) return;
    for (const Stroke& s : strokes)
        if (s.curve && clusters[static_cast<size_t>(s.cluster)].weight < kMinWeight)
            found.curves.push_back(FigureStrokes::Curve{s.a, s.b, s.c, s.d, static_cast<uint32_t>(s.cluster)});
}

// Adds kCurveWeight to the weight of a cluster for each of its curves (found.curves) that holds no
// text line's centre in its cells, until the cluster weighs enough.
void WeighCurves(const PageData& page, const FigureStrokes& found, FigureScratch& scratch) {
    const float width = page.width, height = page.height;
    const float cell = found.cell;
    const int pageX = found.pageX, pageY = found.pageY, c0 = found.c0, c1 = found.c1, r0 = found.r0, r1 = found.r1;
    const auto pageColumn = [&](float x) { return std::min(pageX - 1, std::max(0, static_cast<int>(x / cell))); };
    const auto pageRow = [&](float y) { return std::min(pageY - 1, std::max(0, static_cast<int>(y / cell))); };
    const int nx = c1 - c0 + 1, ny = r1 - r0 + 1;
    const int stride = nx + 1;
    const size_t cells = static_cast<size_t>(stride) * static_cast<size_t>(ny + 1);
    // The cells of upright text lines' centres, within the grid.
    std::vector<uint32_t>& centres = scratch.centres;
    centres.clear();
    for (const Line& l : page.lines) {
        if (l.dir != 0 || l.chars == 0 || (l.flags & kLineTableAnchor)) continue;
        const float cx = (l.u0 + l.u1) / 2, cy = (l.vb + l.vt) / 2;
        if (!(cx >= 0 && cx <= width && cy >= 0 && cy <= height)) continue;
        const int x = pageColumn(cx), y = pageRow(cy);
        if (x < c0 || x > c1 || y < r0 || y > r1) continue;
        centres.push_back(static_cast<uint32_t>(y - r0) << 16 | static_cast<uint32_t>(x - c0));
    }
    std::vector<int32_t>& weights = scratch.weights;
    // Each curve against each centre, or, when that costs more than a pass over the grid, through
    // the centres up to each cell: text[(y + 1) * stride + x + 1] counts those in [0, x] x [0, y].
    if (found.curves.size() * centres.size() <= cells) {
        for (const FigureStrokes::Curve& k : found.curves) {
            if (weights[k.cluster] >= kMinWeight) continue;
            bool free = true;
            for (size_t i = 0; i < centres.size() && free; ++i) {
                const int x = static_cast<int>(centres[i] & 0xFFFF), y = static_cast<int>(centres[i] >> 16);
                free = x < k.a || x > k.b || y < k.c || y > k.d;
            }
            if (free) weights[k.cluster] += kCurveWeight;
        }
        return;
    }
    std::vector<int32_t>& text = scratch.text;
    text.assign(cells, 0);
    for (uint32_t centre : centres)
        text[static_cast<size_t>((static_cast<int>(centre >> 16) + 1) * stride + static_cast<int>(centre & 0xFFFF) + 1)]++;
    for (int y = 1; y <= ny; ++y)
        for (int x = 1; x <= nx; ++x)
            text[static_cast<size_t>(y * stride + x)] += text[static_cast<size_t>(y * stride + x - 1)] +
                                                         text[static_cast<size_t>((y - 1) * stride + x)] -
                                                         text[static_cast<size_t>((y - 1) * stride + x - 1)];
    for (const FigureStrokes::Curve& k : found.curves) {
        if (weights[k.cluster] >= kMinWeight) continue;
        const int a = k.a, b = k.b + 1, c = k.c, d = k.d + 1;
        if (text[static_cast<size_t>(d * stride + b)] - text[static_cast<size_t>(c * stride + b)] -
                text[static_cast<size_t>(d * stride + a)] + text[static_cast<size_t>(c * stride + a)] ==
            0)
            weights[k.cluster] += kCurveWeight;
    }
}

} // namespace

void FigureRegions(PageData& page, std::vector<FigureRegion>& regions) {
    regions.clear();
    const float width = page.width, height = page.height;
    if (!(width > 0) || !(height > 0) || !std::isfinite(width) || !std::isfinite(height)) return;
    if (!page.figureStrokes.known) FindStrokes(page);
    const FigureStrokes& found = page.figureStrokes;
    if (!found.possible) return;
    FigureScratch& scratch = PerThread<FigureScratch>();
    std::vector<int32_t>& weights = scratch.weights;
    weights.resize(found.clusters.size());
    for (size_t c = 0; c < found.clusters.size(); ++c) weights[c] = found.clusters[c].weight;
    if (!found.curves.empty()) WeighCurves(page, found, scratch);
    const float pageArea = width * height;
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
    for (size_t c = 0; c < found.clusters.size(); ++c)
        if (bounded && weights[c] >= kMinWeight) {
            const FigureStrokes::Cluster& k = found.clusters[c];
            bounded = add(k.x0, k.y0, k.x1, k.y1);
        }
    for (const ImageBox& im : page.images)
        if (bounded && !im.ocrTable && std::isfinite(im.x0) && std::isfinite(im.y0) && std::isfinite(im.x1) &&
            std::isfinite(im.y1))
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

void ChartAxes(const PageData& page, std::vector<FigureRegion>& axes) {
    axes.clear();
    AxisScratch& scratch = PerThread<AxisScratch>();
    const std::vector<Shape>& shapes = page.shapes;
    const auto finite = [](const Shape& s) {
        return std::isfinite(s.x0) && std::isfinite(s.y0) && std::isfinite(s.x1) && std::isfinite(s.y1) && s.x1 >= s.x0 &&
               s.y1 >= s.y0;
    };
    for (std::vector<Tick>& ticks : scratch.ticks) ticks.clear();
    size_t count = 0;
    for (size_t i = 0; i < shapes.size(); ++i) {
        // A tick is stroked: a thin filled box is a rule as TeX sets it, or a chart's bar.
        const Shape& s = shapes[i];
        if (!s.stroked || !finite(s)) continue;
        const float w = s.x1 - s.x0, h = s.y1 - s.y0, length = Fmax(w, h), thick = Fmin(w, h);
        if (thick > kTickThick || length < kTickMin || length > kTickMax || length < kTickAspect * thick) continue;
        if (++count > kMaxTicks) return;
        if (h > w) scratch.ticks[0].push_back(Tick{(s.x0 + s.x1) / 2, s.y0, s.y1, static_cast<uint32_t>(i)});
        else scratch.ticks[1].push_back(Tick{(s.y0 + s.y1) / 2, s.x0, s.x1, static_cast<uint32_t>(i)});
    }
    if (count < 3) return;
    uint32_t work = 0;
    // No tick inside a line of text, from a little under its baseline (an underscore drawn as a
    // rule) to an em above it.
    scratch.lines.clear();
    for (const Line& l : page.lines) {
        if (l.chars == 0) continue;
        float x0 = l.u0, y0 = l.baseline - 0.35f * l.size, x1 = l.u1, y1 = l.baseline + l.size;
        RectFromFrame(l.dir, x0, y0, x1, y1);
        scratch.lines.push_back(FigureRegion{x0, y0, x1, y1});
    }
    for (std::vector<Tick>& ticks : scratch.ticks) {
        size_t kept = 0;
        for (const Tick& t : ticks) {
            const Shape& s = shapes[t.shape];
            bool text = false;
            for (size_t k = 0; k < scratch.lines.size() && !text; ++k) {
                const FigureRegion& l = scratch.lines[k];
                text = s.x1 >= l.x0 && s.x0 <= l.x1 && s.y1 >= l.y0 && s.y0 <= l.y1;
            }
            work += static_cast<uint32_t>(std::min<size_t>(scratch.lines.size(), kMaxAxisWork));
            if (work > kMaxAxisWork) return;
            if (!text) ticks[kept++] = t;
        }
        ticks.resize(kept);
    }
    if (scratch.ticks[0].size() < 3 && scratch.ticks[1].size() < 3) return;
    // The shapes a tick's end may touch, by the index cells their boxes (grown by kTickTouch) lie
    // over; the few over many cells (frames, backgrounds) apart.
    const float width = page.width, height = page.height;
    if (!(width > 0) || !(height > 0) || !std::isfinite(width) || !std::isfinite(height)) return;
    const float cell = Fmax(kAxisCell, Fmax(width, height) / float(kAxisSide));
    const int nx = std::min(kAxisSide, static_cast<int>(width / cell) + 1);
    const int ny = std::min(kAxisSide, static_cast<int>(height / cell) + 1);
    const auto cellOf = [&](float v, int n) {
        const float c = v / cell;
        return c <= 0 ? 0 : c >= float(n - 1) ? n - 1 : static_cast<int>(c);
    };
    scratch.cells.clear(), scratch.wide.clear();
    for (uint32_t i = 0; i < shapes.size(); ++i) {
        const Shape& s = shapes[i];
        if (!finite(s)) continue;
        const int c0 = cellOf(s.x0 - kTickTouch, nx), c1 = cellOf(s.x1 + kTickTouch, nx);
        const int r0 = cellOf(s.y0 - kTickTouch, ny), r1 = cellOf(s.y1 + kTickTouch, ny);
        if (static_cast<size_t>(c1 - c0 + 1) * static_cast<size_t>(r1 - r0 + 1) > kMaxShapeCells) {
            if (scratch.wide.size() >= kMaxWideShapes) return;
            scratch.wide.push_back(i);
            continue;
        }
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                if (scratch.cells.size() >= kMaxAxisEntries) return;
                scratch.cells.push_back(uint64_t(static_cast<uint32_t>(r * nx + c)) << 32 | i);
            }
    }
    std::sort(scratch.cells.begin(), scratch.cells.end());
    // Shape j holds the end (x, y) of a tick (upright or level): a stroke across the tick there, a
    // stroke running on from it (the next piece of a rule), or an area whose side across the tick
    // passes there. A stroke along the tick past its end (a plot's frame under its first tick)
    // holds nothing.
    const auto holds = [&](uint32_t j, uint32_t self, float x, float y, bool upright) {
        if (j == self) return false;
        const Shape& s = shapes[j];
        if (x < s.x0 - kTickTouch || x > s.x1 + kTickTouch || y < s.y0 - kTickTouch || y > s.y1 + kTickTouch) return false;
        const float w = s.x1 - s.x0, h = s.y1 - s.y0;
        if (Fmin(w, h) <= kTickStroke) {
            if (upright) return w > h || std::fabs(s.y0 - y) <= kTickTouch || std::fabs(s.y1 - y) <= kTickTouch;
            return h >= w || std::fabs(s.x0 - x) <= kTickTouch || std::fabs(s.x1 - x) <= kTickTouch;
        }
        return upright ? std::fabs(y - s.y0) <= kTickTouch || std::fabs(y - s.y1) <= kTickTouch
                       : std::fabs(x - s.x0) <= kTickTouch || std::fabs(x - s.x1) <= kTickTouch;
    };
    bool exhausted = false;
    const auto held = [&](float x, float y, bool upright, uint32_t self) {
        const uint64_t key = uint64_t(static_cast<uint32_t>(cellOf(y, ny) * nx + cellOf(x, nx))) << 32;
        for (auto it = std::lower_bound(scratch.cells.begin(), scratch.cells.end(), key);
             it != scratch.cells.end() && (*it & ~uint64_t(0xFFFFFFFF)) == key; ++it) {
            if (++work > kMaxAxisWork) return exhausted = true;
            if (holds(static_cast<uint32_t>(*it), self, x, y, upright)) return true;
        }
        for (uint32_t j : scratch.wide) {
            if (++work > kMaxAxisWork) return exhausted = true;
            if (holds(j, self, x, y, upright)) return true;
        }
        return false;
    };
    for (int o = 0; o < 2; ++o) {
        const std::vector<Tick>& ticks = scratch.ticks[o];
        if (ticks.size() < 3) continue;
        for (int end = 0; end < 2; ++end) {
            const auto key = [&](uint32_t k) { return end == 0 ? ticks[k].a : ticks[k].b; };
            scratch.order.resize(ticks.size());
            for (uint32_t k = 0; k < ticks.size(); ++k) scratch.order[k] = k;
            std::sort(scratch.order.begin(), scratch.order.end(), [&](uint32_t p, uint32_t q) { return key(p) < key(q); });
            for (size_t i = 0; i < scratch.order.size();) {
                size_t j = i + 1;
                while (j < scratch.order.size() && key(scratch.order[j]) - key(scratch.order[j - 1]) <= kTickRow) ++j;
                const size_t first = i;
                i = j;
                if (j - first < 3) continue;
                // The row's ticks that end free, at the end away from the axis.
                scratch.free.clear();
                for (size_t k = first; k < j; ++k) {
                    const Tick& t = ticks[scratch.order[k]];
                    const float other = end == 0 ? t.b : t.a;
                    const bool hold = o == 0 ? held(t.pos, other, true, t.shape) : held(other, t.pos, false, t.shape);
                    if (exhausted) {
                        axes.clear();
                        return;
                    }
                    if (!hold) scratch.free.push_back(t);
                }
                if (scratch.free.size() < 3) continue;
                // Three positions at least, most steps between them alike.
                std::vector<float>& positions = scratch.positions;
                positions.clear();
                for (const Tick& t : scratch.free) positions.push_back(t.pos);
                std::sort(positions.begin(), positions.end());
                size_t distinct = 1;
                for (size_t k = 1; k < positions.size(); ++k)
                    if (positions[k] - positions[distinct - 1] >= 0.1f) positions[distinct++] = positions[k];
                positions.resize(distinct);
                if (positions.size() < 3) continue;
                std::vector<float>& steps = scratch.steps;
                steps.clear();
                for (size_t k = 1; k < positions.size(); ++k) steps.push_back(positions[k] - positions[k - 1]);
                std::nth_element(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(steps.size() / 2), steps.end());
                const float median = steps[steps.size() / 2];
                if (median < kTickMinStep) continue;
                const float within = Fmax(0.5f, 0.03f * median);
                size_t even = 0;
                for (float s : steps) even += std::fabs(s - median) <= within;
                if (even < 2 || float(even) < kTickEven * float(steps.size())) continue;
                float lo = INFINITY, hi = -INFINITY;
                for (const Tick& t : scratch.free) lo = Fmin(lo, t.a), hi = Fmax(hi, t.b);
                if (o == 0) axes.push_back(FigureRegion{positions.front(), lo, positions.back(), hi});
                else axes.push_back(FigureRegion{lo, positions.front(), hi, positions.back()});
                if (axes.size() >= kMaxAxes) return;
            }
        }
    }
}

} // namespace RayoPdf::Layout
