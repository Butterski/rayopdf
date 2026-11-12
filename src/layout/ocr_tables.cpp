// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_tables.h"

#include <algorithm>
#include <climits>

namespace RayoPdf::Layout {

namespace {

// Long runs a canvas may hold before it is taken for something else than a table (a photo, a
// hatched area): beyond it no rule is kept.
constexpr size_t kMaxImageRuns = 65536;

// A run of dark pixels: `a0..a1` (exclusive) along a row (a column), in row (column) `c`.
struct Run {
    int32_t c = 0, a0 = 0, a1 = 0;
};

// A stack of runs in consecutive rows (columns) that overlap.
struct Segment {
    int32_t c0 = 0, c1 = 0, a0 = 0, a1 = 0;  // c1 inclusive, a1 exclusive
};

// Merges `runs` (ascending by c, then a0) into segments at most kImageRuleThick thick, appended to
// `out`; false when there are more than kMaxImageRulings.
bool MergeRuns(const std::vector<Run>& runs, std::vector<Segment>& out) {
    std::vector<Segment> open, next;
    const auto close = [&](const Segment& s) {
        if (uint32_t(s.c1 - s.c0 + 1) > kImageRuleThick) return true;
        if (out.size() >= kMaxImageRulings) return false;
        out.push_back(s);
        return true;
    };
    size_t i = 0;
    while (i < runs.size()) {
        const int32_t c = runs[i].c;
        next.clear();
        for (; i < runs.size() && runs[i].c == c; ++i) {
            const Run& r = runs[i];
            Segment merged{c, c, r.a0, r.a1};
            // Every open segment it overlaps joins it.
            for (Segment& s : open) {
                if (s.c1 != c - 1 || s.a1 <= r.a0 || s.a0 >= r.a1) continue;
                merged.c0 = std::min(merged.c0, s.c0);
                merged.a0 = std::min(merged.a0, s.a0), merged.a1 = std::max(merged.a1, s.a1);
                s.c1 = INT32_MIN;  // taken
            }
            // The run, with the segments it joined, stays open for the next row.
            next.push_back(merged);
        }
        for (const Segment& s : open)
            if (s.c1 != INT32_MIN && !close(s)) return false;
        open.swap(next);
    }
    for (const Segment& s : open)
        if (!close(s)) return false;
    return true;
}

} // namespace

void ImageRulings(const OcrCanvas& canvas, std::vector<Ruling>& out) {
    const uint32_t w = canvas.width, h = canvas.height;
    if (w == 0 || h == 0 || canvas.pixels.size() < uint64_t(w) * h || !(canvas.scale > 0)) return;
    const uint8_t* px = reinterpret_cast<const uint8_t*>(canvas.pixels.data());
    const int32_t longH = static_cast<int32_t>(std::max(kImageRuleMin, w / 10));
    const int32_t longV = static_cast<int32_t>(std::max(kImageRuleMin, h / 10));
    std::vector<Run> runs;
    std::vector<Segment> horizontal, vertical;

    // Horizontal: runs along each row, rows in order.
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = px + uint64_t(y) * w;
        for (uint32_t x = 0; x < w;) {
            if (row[x] >= kImageRuleDark) {
                ++x;
                continue;
            }
            uint32_t end = x + 1;
            while (end < w && row[end] < kImageRuleDark) ++end;
            if (int32_t(end - x) >= longH) {
                if (runs.size() >= kMaxImageRuns) return;
                runs.push_back(Run{int32_t(y), int32_t(x), int32_t(end)});
            }
            x = end;
        }
    }
    if (!MergeRuns(runs, horizontal)) return;

    // Vertical: runs down each column (found row by row), then in column order.
    runs.clear();
    std::vector<int32_t> start(w, -1);
    for (uint32_t y = 0; y <= h; ++y) {
        const uint8_t* row = y < h ? px + uint64_t(y) * w : nullptr;
        for (uint32_t x = 0; x < w; ++x) {
            const bool dark = row && row[x] < kImageRuleDark;
            if (dark) {
                if (start[x] < 0) start[x] = int32_t(y);
                continue;
            }
            if (start[x] < 0) continue;
            if (int32_t(y) - start[x] >= longV) {
                if (runs.size() >= kMaxImageRuns) return;
                runs.push_back(Run{int32_t(x), start[x], int32_t(y)});
            }
            start[x] = -1;
        }
    }
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.c != b.c ? a.c < b.c : a.a0 < b.a0; });
    if (!MergeRuns(runs, vertical)) return;
    if (horizontal.size() + vertical.size() > kMaxImageRulings) return;

    const double step = 1.0 / canvas.scale;
    for (const Segment& s : horizontal) {
        Ruling r;
        r.x0 = float(canvas.x0 + s.a0 * step), r.x1 = float(canvas.x0 + s.a1 * step);
        r.y0 = float(canvas.y1 - (s.c1 + 1) * step), r.y1 = float(canvas.y1 - s.c0 * step);
        r.flags = kRulingFill;
        out.push_back(r);
    }
    for (const Segment& s : vertical) {
        Ruling r;
        r.x0 = float(canvas.x0 + s.c0 * step), r.x1 = float(canvas.x0 + (s.c1 + 1) * step);
        r.y0 = float(canvas.y1 - s.a1 * step), r.y1 = float(canvas.y1 - s.a0 * step);
        r.flags = kRulingFill;
        out.push_back(r);
    }
}

} // namespace RayoPdf::Layout
