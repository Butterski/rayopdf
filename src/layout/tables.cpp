// SPDX-License-Identifier: Apache-2.0
#include "layout/tables.h"

#include "common/minmax.h"
#include "common/per_thread.h"
#include "layout/figures.h"
#include "layout/lines.h"
#include "layout/word_box.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace RayoPdf::Layout {
namespace {

// Ruling geometry in points (pdfplumber's defaults are 3 pt, algorithms §6.7).
constexpr float kSnap = 2.5f;      // parallel edges this close are one edge
constexpr float kJoin = 3.0f;      // collinear pieces with gaps up to this join
constexpr float kMinEdge = 3.0f;   // shorter pieces are not rulings
constexpr float kMaxThick = 4.0f;  // thicker painted pieces are areas, not rulings
constexpr float kTouch = 3.0f;     // edges touch within this distance
constexpr float kMinRule = 30.0f;  // a horizontal rule of a partially ruled table is at least this long
constexpr int kMaxStacks = 64;     // rule stacks examined per page
constexpr int kMaxBins = 4096;     // whitespace histogram resolution limit

struct Edge {
    float pos = 0, a = 0, b = 0;  // horizontal: y = pos, x in [a, b]; vertical: x = pos, y in [a, b]
    int32_t comp = -1;            // connected component of the lattice pass
    bool area = false;            // only the side of a filled area (a shaded box), no drawn rule
};

// A word: glyphs of one line between word breaks, with its box.
struct TWord {
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    float base = 0, size = 0;
    uint32_t line = 0, k0 = 0, k1 = 0;  // glyphs line.first + [k0, k1)
    bool mono = false;                  // its line is all monospace (code)
    bool removed = false;               // its line was taken for a running header or footer
};

// A row of words (one baseline cluster), words[first, first + count) of a row list, left to right.
struct Row {
    uint32_t first = 0, count = 0;
    float base = 0, top = 0, bottom = 0, x0 = 0, x1 = 0, size = 0;
    float prose = 0;    // the size of its longest word not set in monospace (0: all code): larger
                        // math or a superscript does not set the row's size
    uint32_t proseGlyphs = 0;
    uint32_t mono = 0;  // words set in monospace
};

// A row's text size: of its prose, code cells being set smaller.
float TextSize(const Row& r) { return r.prose > 0 ? r.prose : r.size; }

// A row of the size `em` (a row of code only fits any).
bool SizeFits(const Row& r, float em) { return r.prose == 0 || std::fabs(r.prose - em) <= 0.15f * em; }

// A line in smaller type set right under a row (a standard error under its estimate, a unit under
// its label): the second line of that row's cells.
bool SubLine(const Row& above, const Row& row) {
    const float em = TextSize(above);
    return TextSize(row) < 0.85f * em && above.base - row.base < 0.9f * em;
}

// A whitespace gap that separates columns.
struct Gap {
    float a = 0, b = 0;
};

struct Span {
    uint16_t r = 0, c = 0, rs = 1, cs = 1;
};

// A table before its cells are filled.
struct Grid {
    TableKind kind = TableKind::Lattice;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<float> xs;    // column boundaries, ascending (cols + 1)
    std::vector<float> ys;    // row boundaries, descending (rows + 1)
    std::vector<uint8_t> vb;  // rows x (cols + 1): the vertical boundary at xs[c] exists in row r
    std::vector<uint8_t> hb;  // (rows + 1) x cols: the horizontal boundary at ys[r] exists over column c
    uint16_t headerRows = 0;
    std::vector<int32_t> owner;  // rows x cols: the cell of each minimal cell
    std::vector<Span> cells;     // row-major by top-left corner
    float size = 0;              // median text size
    std::vector<uint8_t> labelRow;  // per row, or empty: the row is one label across the table (the
                                    // name of a section of rows); its text is the first cell's
    int Rows() const { return static_cast<int>(ys.size()) - 1; }
    int Cols() const { return static_cast<int>(xs.size()) - 1; }
};

bool Overlaps(const Grid& g, float x0, float y0, float x1, float y1) {
    return x0 < g.x1 && x1 > g.x0 && y0 < g.y1 && y1 > g.y0;
}

// Joins parallel edges within kSnap and collinear pieces within kJoin; drops short ones.
void MergeEdges(std::vector<Edge>& edges) {
    std::sort(edges.begin(), edges.end(), [](const Edge& p, const Edge& q) { return p.pos < q.pos; });
    std::vector<Edge> out;
    size_t i = 0;
    while (i < edges.size()) {
        size_t j = i + 1;
        while (j < edges.size() && edges[j].pos - edges[i].pos <= kSnap) ++j;
        // The cluster's position: that of its longest piece.
        float pos = edges[i].pos, longest = -1;
        for (size_t k = i; k < j; ++k)
            if (edges[k].b - edges[k].a > longest) longest = edges[k].b - edges[k].a, pos = edges[k].pos;
        std::sort(edges.begin() + static_cast<std::ptrdiff_t>(i), edges.begin() + static_cast<std::ptrdiff_t>(j),
                  [](const Edge& p, const Edge& q) { return p.a < q.a; });
        Edge cur = edges[i];
        cur.pos = pos;
        for (size_t k = i + 1; k < j; ++k) {
            if (edges[k].a <= cur.b + kJoin) {
                cur.b = Fmax(cur.b, edges[k].b);
                cur.area = cur.area && edges[k].area;
            } else {
                if (cur.b - cur.a >= kMinEdge) out.push_back(cur);
                cur = edges[k];
                cur.pos = pos;
            }
        }
        if (cur.b - cur.a >= kMinEdge) out.push_back(cur);
        i = j;
    }
    edges.swap(out);
}

// A box on the upright page in the frame of baseline direction `dir` (page.h): the page turned so
// that text of that direction reads left to right.
Ruling ToFrame(const Ruling& r, uint8_t dir) {
    switch (dir) {
    case 1: return Ruling{r.y0, -r.x1, r.y1, -r.x0, r.flags};
    case 2: return Ruling{-r.x1, -r.y1, -r.x0, -r.y0, r.flags};
    case 3: return Ruling{-r.y1, r.x0, -r.y0, r.x1, r.flags};
    default: return r;
    }
}

// The rulings of the page as horizontal and vertical edges in the frame of direction `dir`.
void CollectEdges(const PageData& page, uint8_t dir, std::vector<Edge>& h, std::vector<Edge>& v) {
    h.clear();
    v.clear();
    const float pageArea = page.width * page.height;
    std::vector<Ruling> areas;
    for (const Ruling& upright : page.rulings) {
        const Ruling r = ToFrame(upright, dir);
        const float w = r.x1 - r.x0, t = r.y1 - r.y0;
        if (!(w >= 0) || !(t >= 0) || !std::isfinite(w) || !std::isfinite(t)) continue;
        if (t <= kMaxThick && w >= kMinEdge && w > t) {
            h.push_back(Edge{(r.y0 + r.y1) / 2, r.x0, r.x1, -1});
        } else if (w <= kMaxThick && t >= kMinEdge && t > w) {
            v.push_back(Edge{(r.x0 + r.x1) / 2, r.y0, r.y1, -1});
        } else if (w > kMaxThick && t > kMaxThick && (r.flags & kRulingFill) && !(r.flags & kRulingStroke) &&
                   w * t < 0.5f * pageArea) {
            areas.push_back(r);
        }
    }
    // Filled areas (cell and row backgrounds): their sides. An area inside another (the shading of
    // each line of text inside a shaded cell) marks no cell boundary, and neither do two that cross
    // (each overlaps the other well inside, neither holds the other): backgrounds of a table's cells
    // tile it, the layers of a drawing overlap.
    const bool nesting = areas.size() <= 2048;
    const auto holds = [](const Ruling* o, const Ruling* r) {
        return o->x0 <= r->x0 + 0.5f && o->x1 >= r->x1 - 0.5f && o->y0 <= r->y0 + 0.5f && o->y1 >= r->y1 - 0.5f;
    };
    // Only an area reaching r's x range (within the half point `holds` allows) can hold r or cross
    // it: tried in the order of their left edges, the areas starting right of r end the search and
    // those ending left of it are passed over. Whether any area makes r inner is the same in any
    // order (the coordinates are finite).
    std::vector<uint32_t> byLeft;
    if (nesting) {
        byLeft.resize(areas.size());
        for (uint32_t i = 0; i < areas.size(); ++i) byLeft[i] = i;
        std::sort(byLeft.begin(), byLeft.end(), [&](uint32_t a, uint32_t b) { return areas[a].x0 < areas[b].x0; });
    }
    for (const Ruling& ar : areas) {
        const Ruling* r = &ar;
        const float right = r->x1 + 0.5f, left = r->x0 - 0.5f;
        bool inner = false;
        for (size_t k = 0; k < byLeft.size() && !inner; ++k) {
            const Ruling* o = &areas[byLeft[k]];
            if (o->x0 > right) break;
            if (o == r || o->x1 < left) continue;
            inner = holds(o, r) && (o->x1 - o->x0) * (o->y1 - o->y0) > 1.2f * (r->x1 - r->x0) * (r->y1 - r->y0);
            inner = inner || (Fmin(r->x1, o->x1) - Fmax(r->x0, o->x0) > kSnap && Fmin(r->y1, o->y1) - Fmax(r->y0, o->y0) > kSnap &&
                              !holds(o, r) && !holds(r, o));
        }
        if (inner) continue;
        h.push_back(Edge{r->y0, r->x0, r->x1, -1, true});
        h.push_back(Edge{r->y1, r->x0, r->x1, -1, true});
        v.push_back(Edge{r->x0, r->y0, r->y1, -1, true});
        v.push_back(Edge{r->x1, r->y0, r->y1, -1, true});
    }
    MergeEdges(h);
    MergeEdges(v);
}

int32_t Find(std::vector<int32_t>& parent, int32_t x) {
    while (parent[static_cast<size_t>(x)] != x) {
        parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
        x = parent[static_cast<size_t>(x)];
    }
    return x;
}

// Positions within kSnap are one (sorted ascending first).
void Cluster(std::vector<float>& p) {
    std::sort(p.begin(), p.end());
    size_t kept = 0;
    for (size_t i = 0; i < p.size(); ++i)
        if (kept == 0 || p[i] - p[kept - 1] > kSnap) p[kept++] = p[i];
    p.resize(kept);
}

// Cells from the boundary maps: a cell grows right while its right boundary is missing, then down
// while the boundary below it is missing over its whole width.
void ComputeSpans(Grid& g) {
    const int rows = g.Rows(), cols = g.Cols();
    g.owner.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), -1);
    g.cells.clear();
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (g.owner[static_cast<size_t>(r * cols + c)] >= 0) continue;
            int cs = 1;
            while (c + cs < cols && !g.vb[static_cast<size_t>(r * (cols + 1) + c + cs)] &&
                   g.owner[static_cast<size_t>(r * cols + c + cs)] < 0)
                ++cs;
            int rs = 1;
            while (r + rs < rows) {
                bool open = true;
                for (int k = c; k < c + cs && open; ++k)
                    open = !g.hb[static_cast<size_t>((r + rs) * cols + k)] && g.owner[static_cast<size_t>((r + rs) * cols + k)] < 0;
                if (!open) break;
                ++rs;
            }
            const int32_t id = static_cast<int32_t>(g.cells.size());
            for (int rr = r; rr < r + rs; ++rr)
                for (int cc = c; cc < c + cs; ++cc) g.owner[static_cast<size_t>(rr * cols + cc)] = id;
            g.cells.push_back(Span{static_cast<uint16_t>(r), static_cast<uint16_t>(c), static_cast<uint16_t>(rs),
                                   static_cast<uint16_t>(cs)});
        }
    }
}

// The minimal cell of point (x, y) in a grid, or false outside it (the last boundary at or before
// the point, clamped to the cells), for the words or glyphs of a grid in reading order: the row and
// the column of the last point are tried before a search where the grid's boundaries are in order
// (no NaN among them), which gives the same cell. The grid must not change while in use.
class Locator {
public:
    explicit Locator(const Grid& g)
        : xs_(g.xs.data()), ys_(g.ys.data()), nx_(g.xs.size()), ny_(g.ys.size()), left_(g.xs.front()),
          right_(g.xs.back()), top_(g.ys.front()), bottom_(g.ys.back()), cols_(g.Cols()), rows_(g.Rows()) {
        for (size_t k = 0; k + 1 < nx_ && ordered_; ++k) ordered_ = xs_[k] <= xs_[k + 1];
        for (size_t k = 0; k + 1 < ny_ && ordered_; ++k) ordered_ = ys_[k] >= ys_[k + 1];
    }

    RAYOPDF_HOT_INLINE bool Find(float x, float y, int& r, int& c) {
        if (x < left_ || x > right_ || y > top_ || y < bottom_) return false;
        size_t k = static_cast<size_t>(column_);
        if (!(ordered_ && column_ >= 0 && xs_[k] <= x && (k + 1 == nx_ || x < xs_[k + 1]))) Column(x);
        if (!(y == y_)) {  // the same y (glyphs of a line) has the same row; NaN is never the same
            y_ = y;
            k = static_cast<size_t>(row_);
            if (!(ordered_ && row_ >= 0 && ys_[k] >= y && (k + 1 == ny_ || y > ys_[k + 1]))) Row(y);
        }
        c = std::min(std::max(column_, 0), cols_ - 1);
        r = std::min(std::max(row_, 0), rows_ - 1);
        return true;
    }

private:
    // The column of x past the last point's: the next one (a line's glyphs left to right), or searched.
    RAYOPDF_NOINLINE void Column(float x) {
        const size_t k = static_cast<size_t>(column_) + 1;
        if (ordered_ && k < nx_ && xs_[k] <= x && (k + 1 == nx_ || x < xs_[k + 1])) {
            column_ = static_cast<int>(k);
            return;
        }
        column_ = static_cast<int>(std::upper_bound(xs_, xs_ + nx_, x) - xs_) - 1;
    }
    RAYOPDF_NOINLINE void Row(float y) {
        row_ = static_cast<int>(std::upper_bound(ys_, ys_ + ny_, y, std::greater<float>()) - ys_) - 1;
    }

    // The grid's boundaries, read once (the grid does not change while in use).
    const float* xs_;
    const float* ys_;
    size_t nx_, ny_;
    float left_, right_, top_, bottom_;  // xs.front(), xs.back(), ys.front(), ys.back()
    int cols_, rows_;
    bool ordered_ = true;  // xs ascend and ys descend
    int column_ = -1;      // the last point's: the last xs at or left of it
    int row_ = -1;         // the last ys at or above it
    float y_ = NAN;        // the last point's y
};

// A page reference of a contents list: a number of four digits at most, or a roman numeral.
bool PageNumber(std::string_view t) {
    if (t.empty() || t.size() > 6) return false;
    bool digits = true, roman = true;
    for (char c : t) {
        digits = digits && c >= '0' && c <= '9';
        roman = roman && std::string_view("ivxlcdmIVXLCDM").find(c) != std::string_view::npos;
    }
    return (digits && t.size() <= 4) || roman;
}

// The value of a page reference (PageNumber), roman numerals (all of one case) by their value; -1
// for anything else. `roman` says which kind of number it is.
int PageValue(std::string_view t, bool& roman) {
    if (!PageNumber(t)) return -1;
    roman = !(t[0] >= '0' && t[0] <= '9');
    int total = 0;
    if (!roman) {
        for (char c : t) total = total * 10 + (c - '0');
        return total;
    }
    const bool upper = t[0] < 'a';
    for (char c : t)
        if ((c < 'a') != upper) return -1;
    const auto digit = [](char c) {
        switch (c | 0x20) {
        case 'i': return 1;
        case 'v': return 5;
        case 'x': return 10;
        case 'l': return 50;
        case 'c': return 100;
        case 'd': return 500;
        default: return 1000;
        }
    };
    for (size_t k = 0; k < t.size(); ++k) {
        const int v = digit(t[k]), next = k + 1 < t.size() ? digit(t[k + 1]) : 0;
        total += v < next ? -v : v;
    }
    return total;
}

// An e-mail address: a name of letters, digits and . _ + - (braces and commas for a group of
// names), an at sign, then a domain of letters, digits, dots and hyphens that ends in a dot and two
// letters at least; a footnote mark (*, a symbol such as ∗ or †) and one closing punctuation mark
// may follow.
bool EmailAddress(std::string_view t) {
    while (!t.empty() && (static_cast<unsigned char>(t.back()) >= 0x80 || t.back() == '*')) t.remove_suffix(1);
    if (!t.empty() && (t.back() == ',' || t.back() == ';' || t.back() == '.' || t.back() == ')')) t.remove_suffix(1);
    const size_t at = t.find('@');
    if (at == 0 || at == std::string_view::npos) return false;
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    for (size_t k = 0; k < at; ++k)
        if (!alnum(t[k]) && std::string_view("._+-{},").find(t[k]) == std::string_view::npos) return false;
    size_t tld = 0;     // characters after the domain's last dot
    bool word = false;  // a dot came and all characters after the last one are letters
    for (size_t k = at + 1; k < t.size(); ++k) {
        const char c = t[k];
        if (c == '.') {
            if (k == at + 1) return false;
            tld = 0, word = true;
        } else if (alnum(c) || c == '-') {
            tld++;
            word = word && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
        } else {
            return false;
        }
    }
    return word && tld >= 2;
}

// An equation number: a label in parentheses of letters, digits and dots with a digit in it
// ("(3)", "(2.4a)", "(A.1)").
bool EquationNumber(std::string_view t) {
    if (t.size() < 3 || t.size() > 10 || t.front() != '(' || t.back() != ')') return false;
    bool digit = false;
    for (size_t k = 1; k + 1 < t.size(); ++k) {
        const char c = t[k];
        digit = digit || (c >= '0' && c <= '9');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '.' && k > 1))) return false;
    }
    return digit;
}

// Text with a relation sign: = < > or ≠ ≡ ≤ ≥ ≈ ∼ (U+2260 U+2261 U+2264 U+2265 U+2248 U+223C).
bool Relation(std::string_view t) {
    for (const std::string_view s : {"=", "<", ">", "\xE2\x89\xA0", "\xE2\x89\xA1", "\xE2\x89\xA4", "\xE2\x89\xA5",
                                     "\xE2\x89\x88", "\xE2\x88\xBC"})
        if (t.find(s) != std::string_view::npos) return true;
    return false;
}

int Bits(uint64_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

// A plain decimal number ("-0.25", "12", with ASCII or U+2212 minus) and its value.
bool ParseNumber(std::string_view t, float& value) {
    bool negative = false;
    if (!t.empty() && t[0] == '-') negative = true, t.remove_prefix(1);
    else if (t.size() > 3 && t.substr(0, 3) == "\xE2\x88\x92") negative = true, t.remove_prefix(3);
    if (t.empty() || t.size() > 12) return false;
    double v = 0, scale = 0;
    int digits = 0;
    for (char c : t) {
        if (c == '.' && scale == 0) {
            scale = 1;
        } else if (c >= '0' && c <= '9') {
            if (scale > 0) scale /= 10, v += (c - '0') * scale;
            else v = 10 * v + (c - '0');
            digits++;
        } else {
            return false;
        }
    }
    value = static_cast<float>(negative ? -v : v);
    return digits > 0;
}

// Numbers read one after another in a cell, evenly stepped (0 0.2 0.4 0.6): a chart's axis.
struct Progression {
    float previous = NAN, step = NAN;
    int run = 0;
    // Adds the next number; true once four in a row keep one step.
    bool Add(float value) {
        const float d = value - previous;
        if (run >= 2 && d != 0 && std::fabs(d - step) <= 1e-3f * Fmax(1.0f, std::fabs(step))) {
            run++;
        } else {
            step = d;
            run = std::isnan(d) || d == 0 ? 1 : 2;
        }
        previous = value;
        return run >= 4;
    }
};

// The bin of x in Columns' whitespace histogram of n bins of width `bin` from xa. Monotone in x:
// a larger x never gets a smaller bin.
inline int BinOf(float x, float xa, float bin, int n) {
    const float t = (x - xa) / bin;  // compared before conversion: x may be far off or not finite
    if (!(t > 0)) return 0;
    if (t >= float(n - 1)) return n - 1;
    return static_cast<int>(t);
}

// The ink of a set of words along x, for Columns: their extents merged where they overlap or touch,
// ascending. Columns' result depends only on which of its bins some word covers; BinOf is monotone,
// so words whose extents overlap or touch cover every bin from the leftmost one's first to the
// rightmost one's last, and the merged extents cover the same bins as the words. Rows added one at
// a time then cost their own words, not all words so far. A word whose x0 exceeds its x1 (glyph
// boxes inverted or infinite) takes from Columns' cover instead: `proper` turns false, and Columns
// counts the rows' words itself.
struct Ink {
    std::vector<Gap> spans;
    bool proper = true;
    void Clear() {
        spans.clear();
        proper = true;
    }
};

// Columns' gaps from the ink of its rows: the same as Columns over those rows when ink.proper.
bool InkColumns(const Ink& ink, float xa, float xb, float minGap, std::vector<Gap>& gaps) {
    gaps.clear();
    if (!(xb > xa) || !std::isfinite(xb - xa)) return false;
    const float bin = Fmax(0.5f, (xb - xa) / kMaxBins);
    const int n = std::min(kMaxBins, static_cast<int>((xb - xa) / bin) + 1);
    // The spans are disjoint and ascending: their bins ascend too, each span's first bin at or
    // after the last bin of the span before. Uncovered bins between two spans are a run of
    // whitespace, measured and placed as Columns does.
    int end = -1;  // the last covered bin so far
    for (const Gap& s : ink.spans) {
        const int i = BinOf(s.a, xa, bin, n);
        const int run = end >= 0 ? i - end - 1 : 0;
        if (run > 0 && float(run) * bin >= minGap) gaps.push_back(Gap{xa + float(i - run) * bin, xa + float(i) * bin});
        end = BinOf(s.b, xa, bin, n);
    }
    return !gaps.empty();
}

// The edges of one connected component of a page's rulings: indices into the horizontal and the
// vertical edges, ascending.
struct Component {
    const uint32_t* h;
    size_t hCount;
    const uint32_t* v;
    size_t vCount;
};

// What the table tests read of a word's text, gathered once a page (Detector::Chars): its length
// in bytes, its first three bytes, its ASCII digits and letters (Numeric adds them up over a
// cell), and whether every byte could belong to a number ParseNumber reads (digits, '.', and an
// ASCII or U+2212 minus): ParseNumber accepts no text that is not.
struct WordChars {
    uint32_t size = 0, digits = 0, letters = 0;
    unsigned char head[3] = {0, 0, 0};
    bool numberish = true;
    bool at = false;  // an '@' among them (an e-mail address has one)
    uint32_t page = 0;  // the page they were gathered for (TableScratch::charsPage)
};

// A numeric cell: digits, and few letters (units, currency, percent and signs aside), counted as
// ASCII bytes of its text (a cell's counts are its words' added up).
bool NumericCounts(uint32_t digits, uint32_t letters) { return digits > 0 && letters * 2 <= digits; }
bool Numeric(const WordChars& c) { return NumericCounts(c.digits, c.letters); }

int ColumnOf(const std::vector<Gap>& gaps, float x);

// A word's sort key (Detector::SortWords).
struct WordKey {
    float base, x0;
    uint32_t word;
};

// The detector's vectors, kept by each thread between pages and directions so that a page grows
// none of them anew: one detector runs at a time (DetectTables runs them one after another), and
// Run() empties what it does not refill.
struct TableScratch {
    std::vector<TWord> words, ordered;
    std::vector<WordKey> keys;
    std::vector<Edge> h, v, rules;
    std::vector<Grid> grids;
    std::vector<uint8_t> wordUsed;
    std::vector<uint32_t> events;
    std::vector<int> bins;
    std::vector<Row> pageRows;
    std::vector<uint32_t> pageRowWords;
    std::vector<float> sizes, spaces, lefts, rights, centres, pitches;
    std::vector<uint8_t> split, label;
    std::vector<Gap> gaps, trial, narrowed;
    Ink ink, grown;
    std::vector<WordChars> chars;  // per word of words; the page's entries bear its charsPage
    uint32_t charsPage = 0;
    // Per word of a row range (from its rows' first word on): its column for a set of gaps
    // (ColumnOf), for Texty, Borderless's cells and TextGrid.
    std::vector<int> textyColumns, cellColumns, gridColumns;
    std::vector<float> widths;
    std::vector<float> middles;  // WordColumns: the gaps' middles (ColumnOf's bounds)
    std::vector<FigureRegion> axes;  // DropChartText: the page's chart axes
};
constexpr size_t kKeptWords = size_t(1) << 16;  // a page with more words releases the scratch

class Detector {
public:
    // Tables of the text of baseline direction `dir`, in that direction's frame (page.h). The
    // vectors come from the thread's scratch and go back to it, their capacity with them.
    Detector(PageData& page, TableContinuation& continuation, uint8_t dir)
        : page_(page), continuation_(continuation), dir_(dir), scratch_(PerThread<TableScratch>()),
          words_(std::move(scratch_.words)),
          h_(std::move(scratch_.h)), v_(std::move(scratch_.v)), rules_(std::move(scratch_.rules)),
          grids_(std::move(scratch_.grids)), wordUsed_(std::move(scratch_.wordUsed)),
          events_(std::move(scratch_.events)), bins_(std::move(scratch_.bins)),
          pageRows_(std::move(scratch_.pageRows)), pageRowWords_(std::move(scratch_.pageRowWords)) {}
    // The same, reporting Borderless's candidate gaps to `features` (upright text only).
    Detector(PageData& page, TableContinuation& continuation, uint8_t dir, const FeatureSink* features)
        : Detector(page, continuation, dir) {
        if (dir == 0 && features && features->tableGap) features_ = features;
    }
    ~Detector() {
        scratch_.words = std::move(words_);
        scratch_.h = std::move(h_);
        scratch_.v = std::move(v_);
        scratch_.rules = std::move(rules_);
        scratch_.grids = std::move(grids_);
        scratch_.wordUsed = std::move(wordUsed_);
        scratch_.events = std::move(events_);
        scratch_.bins = std::move(bins_);
        scratch_.pageRows = std::move(pageRows_);
        scratch_.pageRowWords = std::move(pageRowWords_);
    }
    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;
    int Run();  // the running header and footer lines taken back into tables

private:
    void CollectWords();
    void SortWords();
    void BoxGrids();
    void Lattices();
    void RuleStacks();
    void Continue();
    void Borderless(float gapFactor);
    void ReportGaps(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t begin, size_t end,
                    const std::vector<Gap>& gaps, const std::vector<int>& columns, const std::vector<uint8_t>& label,
                    float em, bool secondPass, bool chosen);
    void BuildRows(float xa, float xb, float ylo, float yhi, std::vector<Row>& rows, std::vector<uint32_t>& rowWords);
    bool Columns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin, size_t rowEnd,
                 float xa, float xb, float minGap, std::vector<Gap>& gaps, const std::vector<uint8_t>* skip = nullptr);
    // `to`: the ink of `from` and of row `row`'s words.
    void AddInk(const Row& row, const std::vector<uint32_t>& rowWords, const Ink& from, Ink& to) const;
    bool Texty(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin, size_t rowEnd,
               const std::vector<Gap>& gaps, float em, bool closed = true);
    bool WordSpaced(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin, size_t rowEnd,
                    const std::vector<Gap>& gaps) const;
    bool ListLike(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin, size_t rowEnd,
                  const std::vector<Gap>& gaps);
    // `partial`: rules among the header lines narrower than the table (a label's \cmidrule).
    // `joints`: x positions where a table's rules drawn cell by cell meet (RuleStacks): no text
    // spans the column boundary of a gap holding one.
    void TextGrid(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin, size_t rowEnd,
                  const std::vector<Gap>& gaps, TableKind kind, uint16_t headerRows, float xa, float xb,
                  const std::vector<uint8_t>* labels = nullptr, const std::vector<uint8_t>* ruled = nullptr,
                  const std::vector<Edge>* partial = nullptr, const std::vector<float>* joints = nullptr);
    bool Free(float x0, float y0, float x1, float y1) const;
    // `figure` (when given) tells a chart's or a diagram's lines apart from other non-tables.
    bool Validate(Grid& g, bool column, bool* figure = nullptr);
    void CollapseEmpty(Grid& g) const;
    void Boundaries(Grid& g, const Component& comp) const;
    void SplitRowsByVerticals(Grid& g, std::vector<Edge>& pieces, bool& read) const;
    void SplitByText(Grid& g) const;
    void SplitRowsByLines(Grid& g) const;
    bool SplitSpanLines(Grid& g) const;
    void SplitColumnsByText(Grid& g) const;
    bool HeaderAbove(Grid& g);
    bool HeaderOverColumns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t begin, size_t end,
                           const std::vector<uint8_t>& label, const Row& header, float minGap, std::vector<Gap>& gaps);
    bool HeaderAtColumns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t begin, size_t end,
                         const std::vector<uint8_t>& label, const Row& header, std::vector<Gap>& gaps);
    uint64_t LineColumns(const Row& r, const std::vector<uint32_t>& rowWords, const Grid& g) const;
    // A word of row r, not in monospace, of the size `em` (SizeFits' tolerance).
    bool WordOfSize(const Row& r, const std::vector<uint32_t>& rowWords, float em) const;
    float LabelAbove(const Grid& g);
    void LatticeHeader(Grid& g) const;
    void RowsBelow(Grid& g);
    void JoinStripes();
    void JoinStacked();
    void RepeatedHeaders();
    void DropChartText();
    void Commit();
    bool Contents(const Grid& g) const;
    void MarkUsed(const Grid& g);
    std::string_view WordText(const TWord& w, std::string& scratch) const;
    const WordChars& Chars(const TWord& w) const;  // w is one of words_
    unsigned char FirstByte(const TWord& w) const;  // of its text, 0 when it has none
    // out[k - rows[rowBegin].first]: ColumnOf(gaps, centre) of each word k of rows [rowBegin, rowEnd).
    void WordColumns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin, size_t rowEnd,
                     const std::vector<Gap>& gaps, std::vector<int>& out) const;
    bool MathWord(const TWord& w) const;
    const std::vector<Ruling>& Figures();
    bool FigureText(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin, size_t rowEnd,
                    float em);

    PageData& page_;
    TableContinuation& continuation_;  // in: from the previous page; out: for the next (upright only)
    uint8_t dir_ = 0;
    TableScratch& scratch_;  // the thread's (PerThread), lent to the vectors below and back
    std::vector<TWord> words_;  // by baseline descending, then x
    std::vector<Edge> h_, v_;
    std::vector<Edge> rules_;     // horizontal edges left for partially ruled tables
    std::vector<Grid> grids_;
    std::vector<uint8_t> wordUsed_;  // inside an accepted grid
    std::vector<uint32_t> events_;   // Columns: bin events, 2 * bin + 1 an ink start, 2 * bin an ink end
    std::vector<int> bins_;          // Columns: ink starts minus ends per bin
    // Borderless: the rows of all words not in a grid, built when marks_ (MarkUsed calls) was
    // pageRowsMarks_.
    std::vector<Row> pageRows_;
    std::vector<uint32_t> pageRowWords_;
    uint32_t marks_ = 0;
    uint32_t pageRowsMarks_ = UINT32_MAX;
    float bodySize_ = 10;
    const FeatureSink* features_ = nullptr;  // Borderless's gaps go there (layout/features.h)
    float wordSpace_ = 0.3f;  // the page's median space between words of a line, in ems
    int restored_ = 0;
    std::vector<Ruling> figures_;  // the page's figure regions in this frame, once figuresKnown_
    bool figuresKnown_ = false;
};

void Detector::CollectWords() {
    words_.clear();
    // Words are at most the lines' glyphs: one allocation on a thread's first page (up to the
    // capacity the scratch keeps).
    words_.reserve(std::min(page_.lineGlyphs.size(), kKeptWords));
    std::vector<float>& sizes = scratch_.sizes;
    std::vector<float>& spaces = scratch_.spaces;
    sizes.clear();
    spaces.clear();
    for (uint32_t li = 0; li < page_.lines.size(); ++li) {
        const Line& line = page_.lines[li];
        // Lines taken for running headers and footers count too: a table's numbers in the page's
        // margin bands look like page numbers (they return to the text inside a table).
        if (line.dir != dir_ || line.count == 0 || (line.flags & (kLineTableCell | kLineTableAnchor))) continue;
        const uint32_t* const order = page_.lineGlyphs.data() + line.first;
        const Glyph* const glyphs = page_.glyphs.data();
        uint32_t k = 0;
        float previousRight = NAN;
        while (k < line.count) {
            TWord w;
            w.line = li;
            w.k0 = k;
            w.base = line.baseline;
            w.size = line.size;
            // Plain comparisons (WordBoxOf), which equal Fmin and Fmax without a NaN; a word with a
            // NaN in its glyphs' boxes takes them again with Fmin and Fmax, which skip it. A word's
            // box is never NaN.
            WordBox box;
            k = WordBoxOf(glyphs, order, k, line.count, box);
            if (box.nan) {
                box.u0 = INFINITY, box.u1 = -INFINITY, box.vb = INFINITY, box.vt = -INFINITY;
                for (uint32_t j = w.k0; j < k; ++j) {
                    const Glyph& g = glyphs[order[j]];
                    box.u0 = Fmin(box.u0, g.u0), box.u1 = Fmax(box.u1, g.u1);
                    box.vb = Fmin(box.vb, g.vb), box.vt = Fmax(box.vt, g.vt);
                }
            }
            w.x0 = box.u0, w.x1 = box.u1, w.y0 = box.vb, w.y1 = box.vt;
            w.k1 = k;
            w.mono = (line.flags & kLineAllMono) != 0;
            w.removed = (line.flags & kLineRemoved) != 0;
            words_.push_back(w);
            if (spaces.size() < 4096 && w.x0 > previousRight && line.size > 0) spaces.push_back((w.x0 - previousRight) / line.size);
            previousRight = w.x1;
        }
        if (sizes.size() < 4096) sizes.push_back(line.size);
    }
    SortWords();
    wordUsed_.assign(words_.size(), 0);
    // A new stamp for the page's word texts (Chars): earlier pages' entries stay, stamped otherwise.
    if (scratch_.chars.size() < words_.size()) scratch_.chars.resize(words_.size());
    if (++scratch_.charsPage == 0) {
        for (WordChars& c : scratch_.chars) c.page = 0;
        scratch_.charsPage = 1;
    }
    if (!sizes.empty()) {
        std::nth_element(sizes.begin(), sizes.begin() + static_cast<std::ptrdiff_t>(sizes.size() / 2), sizes.end());
        bodySize_ = Fmax(1.0f, sizes[sizes.size() / 2]);
    }
    if (spaces.size() >= 20) {
        std::nth_element(spaces.begin(), spaces.begin() + static_cast<std::ptrdiff_t>(spaces.size() / 2), spaces.end());
        wordSpace_ = spaces[spaces.size() / 2];
    }
}

// words_ by baseline descending, then x0, as std::sort with that order leaves them. They come line
// by line in BuildLines' order, nearly sorted already, so an insertion sort of their keys takes few
// moves. When it leaves each key strictly before the next, no two words tie and the sorted order
// is the only one, which std::sort reaches too. Ties (whose order std::sort's steps decide), a NaN
// baseline (no key sorts strictly before or after it) or too many moves leave the words to
// std::sort as before.
void Detector::SortWords() {
    using Key = WordKey;
    const auto before = [](const Key& a, const Key& b) {
        if (a.base != b.base) return a.base > b.base;
        return a.x0 < b.x0;
    };
    const size_t n = words_.size();
    // Words already strictly in order (one line, or lines top to bottom) are std::sort's only order.
    {
        size_t i = 1;
        for (; i < n; ++i) {
            const TWord& a = words_[i - 1];
            const TWord& b = words_[i];
            if (!(a.base != b.base ? a.base > b.base : a.x0 < b.x0)) break;
        }
        if (i >= n) return;
    }
    std::vector<Key>& keys = scratch_.keys;
    keys.clear();
    for (size_t i = 0; i < n; ++i) keys.push_back(Key{words_[i].base, words_[i].x0, static_cast<uint32_t>(i)});
    size_t moves = 8 * n + 256;
    bool sorted = true;
    for (size_t i = 1; i < n && sorted; ++i) {
        const Key k = keys[i];
        size_t j = i;
        for (; j > 0 && before(k, keys[j - 1]); --j) {
            if (moves-- == 0) {
                sorted = false;
                break;
            }
            keys[j] = keys[j - 1];
        }
        keys[j] = k;
    }
    for (size_t i = 1; i < n && sorted; ++i) sorted = before(keys[i - 1], keys[i]);
    if (!sorted) {
        std::sort(words_.begin(), words_.end(), [](const TWord& a, const TWord& b) {
            if (a.base != b.base) return a.base > b.base;
            return a.x0 < b.x0;
        });
        return;
    }
    std::vector<TWord>& ordered = scratch_.ordered;
    ordered.clear();
    for (const Key& k : keys) ordered.push_back(words_[k.word]);
    words_.swap(ordered);
}

std::string_view Detector::WordText(const TWord& w, std::string& scratch) const {
    scratch.clear();
    const Line& line = page_.lines[w.line];
    AppendGlyphTexts(page_, line, w.k0, w.k1, scratch);
    return scratch;
}

unsigned char Detector::FirstByte(const TWord& w) const {
    const Line& line = page_.lines[w.line];
    for (uint32_t k = w.k0; k < w.k1; ++k) {
        const std::string_view t = page_.GlyphText(page_.glyphs[page_.lineGlyphs[line.first + k]]);
        if (!t.empty()) return static_cast<unsigned char>(t[0]);
    }
    return 0;
}

// Chars' byte classes: a digit, an ASCII letter, a byte of a number (a digit, '.', '-' or a byte of
// U+2212's UTF-8), an at sign.
enum : uint8_t { kCharDigit = 1, kCharLetter = 2, kCharNumber = 4, kCharAt = 8 };
struct CharClasses {
    uint8_t of[256] = {};
    constexpr CharClasses() {
        for (int c = '0'; c <= '9'; ++c) of[c] = kCharDigit | kCharNumber;
        for (int c = 'a'; c <= 'z'; ++c) of[c] = kCharLetter;
        for (int c = 'A'; c <= 'Z'; ++c) of[c] = kCharLetter;
        of['.'] = of['-'] = of[0xE2] = of[0x88] = of[0x92] = kCharNumber;
        of['@'] = kCharAt;
    }
};
constexpr CharClasses kCharClasses;

const WordChars& Detector::Chars(const TWord& w) const {
    WordChars& c = scratch_.chars[static_cast<size_t>(&w - words_.data())];
    if (c.page == scratch_.charsPage) return c;
    c = WordChars{};
    c.page = scratch_.charsPage;
    const Line& line = page_.lines[w.line];
    uint8_t all = kCharNumber, any = 0;
    ForGlyphTextRuns(page_, line, w.k0, w.k1, [&](std::string_view text) {
        for (const char ch : text) {
            const unsigned char u = static_cast<unsigned char>(ch);
            const uint8_t kind = kCharClasses.of[u];
            if (c.size < 3) c.head[c.size] = u;
            c.size++;
            c.digits += kind & kCharDigit;
            c.letters += (kind >> 1) & 1;
            all &= kind, any |= kind;
        }
    });
    c.numberish = (all & kCharNumber) != 0;
    c.at = (any & kCharAt) != 0;
    return c;
}

void Detector::WordColumns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin,
                           size_t rowEnd, const std::vector<Gap>& gaps, std::vector<int>& out) const {
    if (rowEnd <= rowBegin) {
        out.clear();
        return;
    }
    const uint32_t first = rows[rowBegin].first, end = rows[rowEnd - 1].first + rows[rowEnd - 1].count;
    std::vector<float>& middles = scratch_.middles;
    middles.resize(gaps.size());
    for (size_t g = 0; g < gaps.size(); ++g) middles[g] = (gaps[g].a + gaps[g].b) / 2;
    const int n = static_cast<int>(middles.size());
    out.resize(end - first);
    for (uint32_t k = first; k < end; ++k) {
        const float x = (words_[rowWords[k]].x0 + words_[rowWords[k]].x1) / 2;
        int c = 0;
        while (c < n && x > middles[static_cast<size_t>(c)]) ++c;
        out[k - first] = c;
    }
}

// A word of displayed math: a few glyphs, each an italic letter, a digit or an operator, with an
// italic letter or an operator among them (digits alone are a table's numbers).
bool Detector::MathWord(const TWord& w) const {
    if (w.k1 - w.k0 > 4) return false;
    // − ± × · ∑ ∫ √ ≤ ≥ → ∞ ∂ ∇ (U+2212 U+00B1 U+00D7 U+00B7 U+2211 U+222B U+221A U+2264 U+2265
    // U+2192 U+221E U+2202 U+2207).
    static const char* const kOperators[] = {"\xE2\x88\x92", "\xC2\xB1", "\xC3\x97", "\xC2\xB7", "\xE2\x88\x91",
                                             "\xE2\x88\xAB", "\xE2\x88\x9A", "\xE2\x89\xA4", "\xE2\x89\xA5",
                                             "\xE2\x86\x92", "\xE2\x88\x9E", "\xE2\x88\x82", "\xE2\x88\x87"};
    const Line& line = page_.lines[w.line];
    bool math = false;
    for (uint32_t k = w.k0; k < w.k1; ++k) {
        const Glyph& g = page_.glyphs[page_.lineGlyphs[line.first + k]];
        const std::string_view t = page_.GlyphText(g);
        if (g.flags & kGlyphItalic) {
            math = true;
            continue;
        }
        if (t.size() == 1 && t[0] >= '0' && t[0] <= '9') continue;
        // Mathematical alphanumeric symbols (U+1D400-U+1D7FF): letters set as math.
        if (t.size() == 4 && t.substr(0, 2) == "\xF0\x9D") {
            math = true;
            continue;
        }
        bool op = t.size() == 1 && std::string_view("=+-/<>*").find(t[0]) != std::string_view::npos;
        for (const char* o : kOperators) op = op || t == o;
        if (!op) return false;
        math = true;
    }
    return math;
}

// The page's figure regions in the detector's frame, found on first use.
const std::vector<Ruling>& Detector::Figures() {
    if (!figuresKnown_) {
        figuresKnown_ = true;
        std::vector<FigureRegion> regions;
        FigureRegions(page_, regions);
        for (const FigureRegion& r : regions) figures_.push_back(ToFrame(Ruling{r.x0, r.y0, r.x1, r.y1, 0}, dir_));
    }
    return figures_;
}

// Rows that are a figure's own text: most of their words stand in a figure region or reach to
// within two ems of it (tick labels along an axis, wider than two ems, a legend, the labels of a
// diagram).
bool Detector::FigureText(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin,
                          size_t rowEnd, float em) {
    if (Figures().empty()) return false;
    const float reach = 2 * em;
    size_t inside = 0, all = 0;
    for (size_t r = rowBegin; r < rowEnd; ++r) {
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
            const TWord& w = words_[rowWords[k]];
            const float cy = (w.y0 + w.y1) / 2;
            all++;
            for (const Ruling& f : figures_)
                if (w.x1 >= f.x0 - reach && w.x0 <= f.x1 + reach && cy >= f.y0 - reach && cy <= f.y1 + reach) {
                    inside++;
                    break;
                }
        }
    }
    return 5 * inside >= 3 * all;
}

bool Detector::Free(float x0, float y0, float x1, float y1) const {
    for (const Grid& g : grids_)
        if (Overlaps(g, x0, y0, x1, y1)) return false;
    return true;
}

// Rows of the words whose centre lies in [xa, xb] with a baseline in [ylo, yhi], top first.
// Words of accepted grids are left out. rowWords holds the rows' words (rows' first and count);
// entries after the last row's are left over.
void Detector::BuildRows(float xa, float xb, float ylo, float yhi, std::vector<Row>& rows, std::vector<uint32_t>& rowWords) {
    rows.clear();
    // words_ is sorted by baseline descending: the first word at or below yhi.
    auto it = std::lower_bound(words_.begin(), words_.end(), yhi,
                               [](const TWord& w, float y) { return w.base > y; });
    // rowWords is written by index, grown to the words from yhi to the end.
    const size_t start = static_cast<size_t>(it - words_.begin());
    if (rowWords.size() < words_.size() - start) rowWords.resize(words_.size() - start);
    size_t count = 0;
    for (size_t i = start; i < words_.size(); ++i) {
        const TWord& w = words_[i];
        if (w.base < ylo) break;
        if (wordUsed_[i]) continue;
        const float cx = (w.x0 + w.x1) / 2;
        if (cx < xa || cx > xb) continue;
        if (!rows.empty() && rows.back().base - w.base <= 0.35f * Fmax(rows.back().size, w.size)) {
            Row& r = rows.back();
            r.count++;
            // Word boxes are never NaN: plain comparisons give what Fmin and Fmax would.
            r.top = r.top > w.y1 ? r.top : w.y1, r.bottom = r.bottom < w.y0 ? r.bottom : w.y0;
            r.x0 = r.x0 < w.x0 ? r.x0 : w.x0, r.x1 = r.x1 > w.x1 ? r.x1 : w.x1;
            r.size = Fmax(r.size, w.size);
            if (w.mono) {
                r.mono++;
            } else if (w.k1 - w.k0 > r.proseGlyphs) {
                r.prose = w.size;
                r.proseGlyphs = w.k1 - w.k0;
            }
        } else {
            Row r;
            r.first = static_cast<uint32_t>(count);
            r.count = 1;
            r.base = w.base, r.top = w.y1, r.bottom = w.y0, r.x0 = w.x0, r.x1 = w.x1, r.size = w.size;
            r.mono = w.mono ? 1 : 0;
            r.prose = w.mono ? 0 : w.size;
            r.proseGlyphs = w.mono ? 0 : w.k1 - w.k0;
            rows.push_back(r);
        }
        rowWords[count++] = static_cast<uint32_t>(i);
    }
    // Words of a row left to right (rows may gather words of several lines). Words already strictly
    // left to right are in the only order std::sort can leave them.
    for (const Row& r : rows) {
        const auto first = rowWords.begin() + r.first, last = first + r.count;
        bool ordered = true;
        for (auto k = first + 1; k < last && ordered; ++k) ordered = words_[*(k - 1)].x0 < words_[*k].x0;
        if (!ordered) std::sort(first, last, [&](uint32_t a, uint32_t b) { return words_[a].x0 < words_[b].x0; });
    }
}

// Columns of rows [rowBegin, rowEnd): gaps at least `minGap` wide where no row has ink, between the
// leftmost and rightmost ink in [xa, xb]. Rows flagged in `skip` are left out. True with at least
// one gap.
bool Detector::Columns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin,
                       size_t rowEnd, float xa, float xb, float minGap, std::vector<Gap>& gaps,
                       const std::vector<uint8_t>* skip) {
    gaps.clear();
    if (rowEnd <= rowBegin || !(xb > xa) || !std::isfinite(xb - xa)) return false;
    const float bin = Fmax(0.5f, (xb - xa) / kMaxBins);
    const int n = std::min(kMaxBins, static_cast<int>((xb - xa) / bin) + 1);
    const auto binOf = [&](float x) { return BinOf(x, xa, bin, n); };
    // Each word covers bins binOf(x0) to binOf(x1): an ink start there and an end after it. A bin
    // is covered while starts outnumber ends; gaps are the uncovered runs between covered bins.
    size_t words = 0;
    for (size_t r = rowBegin; r < rowEnd; ++r)
        if (!(skip && (*skip)[r])) words += rows[r].count;
    int first = -1, run = 0, cover = 0;
    if (words * 16 <= static_cast<size_t>(n)) {
        // Few words for the width: the cover changes only at their bins, so the starts and ends
        // (2 * bin + 1 a start, 2 * bin an end) are sorted and walked one stretch of bins of one
        // cover at a time.
        events_.resize(2 * words);
        size_t at = 0;
        for (size_t r = rowBegin; r < rowEnd; ++r)
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count && !(skip && (*skip)[r]); ++k) {
                const TWord& w = words_[rowWords[k]];
                events_[at++] = static_cast<uint32_t>(binOf(w.x0)) * 2 + 1;
                events_[at++] = static_cast<uint32_t>(binOf(w.x1) + 1) * 2;
            }
        std::sort(events_.begin(), events_.end());
        for (size_t e = 0; e < events_.size();) {
            const int i = static_cast<int>(events_[e] >> 1);
            if (i >= n) break;
            for (; e < events_.size() && static_cast<int>(events_[e] >> 1) == i; ++e) cover += (events_[e] & 1) ? 1 : -1;
            const int next = e < events_.size() ? std::min(n, static_cast<int>(events_[e] >> 1)) : n;
            if (cover > 0) {
                if (first < 0) first = i;
                if (run > 0 && float(run) * bin >= minGap) gaps.push_back(Gap{xa + float(i - run) * bin, xa + float(i) * bin});
                run = 0;
            } else if (first >= 0) {
                run += next - i;
            }
        }
        return !gaps.empty();
    }
    bins_.assign(static_cast<size_t>(n) + 1, 0);
    for (size_t r = rowBegin; r < rowEnd; ++r)
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count && !(skip && (*skip)[r]); ++k) {
            const TWord& w = words_[rowWords[k]];
            bins_[static_cast<size_t>(binOf(w.x0))]++;
            bins_[static_cast<size_t>(binOf(w.x1)) + 1]--;
        }
    for (int i = 0; i < n; ++i) {
        cover += bins_[static_cast<size_t>(i)];
        if (cover > 0) {
            if (first < 0) first = i;
            if (run > 0 && float(run) * bin >= minGap) gaps.push_back(Gap{xa + float(i - run) * bin, xa + float(i) * bin});
            run = 0;
        } else if (first >= 0) {
            ++run;
        }
    }
    return !gaps.empty();
}

void Detector::AddInk(const Row& row, const std::vector<uint32_t>& rowWords, const Ink& from, Ink& to) const {
    to.spans.clear();
    to.proper = from.proper;
    // Both lists ascend by start (a row's words by x0): merged into one, joining what overlaps.
    const std::vector<Gap>& old = from.spans;
    size_t i = 0;
    Gap cur{0, 0};
    bool open = false;
    const auto take = [&](float a, float b) {
        if (open && a <= cur.b) {
            if (b > cur.b) cur.b = b;
            return;
        }
        if (open) to.spans.push_back(cur);
        cur = Gap{a, b};
        open = true;
    };
    float previous = -INFINITY;
    for (uint32_t k = row.first; k < row.first + row.count; ++k) {
        const TWord& w = words_[rowWords[k]];
        if (!(w.x0 <= w.x1) || w.x0 < previous) {
            to.proper = false;
            continue;
        }
        previous = w.x0;
        for (; i < old.size() && old[i].a <= w.x0; ++i) take(old[i].a, old[i].b);
        take(w.x0, w.x1);
    }
    for (; i < old.size(); ++i) take(old[i].a, old[i].b);
    if (open) to.spans.push_back(cur);
}

// The column of x for gaps.
int ColumnOf(const std::vector<Gap>& gaps, float x) {
    int c = 0;
    while (c < static_cast<int>(gaps.size()) && x > (gaps[static_cast<size_t>(c)].a + gaps[static_cast<size_t>(c)].b) / 2) ++c;
    return c;
}

// Every gap of `old` still has a gap of `now` over it: the rows added split the same way.
bool KeepsGaps(const std::vector<Gap>& old, const std::vector<Gap>& now) {
    for (const Gap& o : old) {
        bool kept = false;
        for (const Gap& n : now) kept = kept || (n.a < o.b && n.b > o.a);
        if (!kept) return false;
    }
    return true;
}

// The gaps after rows were added: each gap of `old` as `now` narrows it (the widest gap of `now`
// over it; KeepsGaps holds), and gaps of `now` over no old gap that are `minGap` wide (whitespace
// that the rows so far left open and the new row leaves open too: a header row's). `out` is a
// buffer of the caller's.
void Narrow(std::vector<Gap>& old, const std::vector<Gap>& now, float minGap, std::vector<Gap>& out) {
    out.clear();
    for (size_t i = 0; i < now.size(); ++i) {
        const Gap& n = now[i];
        bool over = false, widest = true;
        for (const Gap& o : old) {
            if (!(n.a < o.b && n.b > o.a)) continue;
            over = true;
            for (const Gap& m : now)
                if (&m != &n && m.a < o.b && m.b > o.a && m.b - m.a > n.b - n.a) widest = false;
        }
        if (over ? widest : n.b - n.a >= minGap) out.push_back(n);
    }
    old.swap(out);
}

// Running text set in columns is not a table (Breuel's gutters, as in reading order). A column is
// running text when its lines mostly fill a wide column with several words each, or when most of
// its lines start in lowercase (sentences continued from the line above; table cells start with a
// capital, a digit or a symbol, wrapped cells aside). Two such columns make prose.
bool Detector::Texty(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin,
                     size_t rowEnd, const std::vector<Gap>& gaps, float em, bool closed) {
    int sentenceColumns = 0;
    const int cols = static_cast<int>(gaps.size()) + 1;
    int texty = 0;
    std::vector<float>& widths = scratch_.widths;
    std::vector<int>& columns = scratch_.textyColumns;
    WordColumns(rows, rowWords, rowBegin, rowEnd, gaps, columns);
    const uint32_t base = rowEnd > rowBegin ? rows[rowBegin].first : 0;
    for (int c = 0; c < cols; ++c) {
        float lo = INFINITY, hi = -INFINITY;
        widths.clear();
        uint32_t wordsIn = 0, lower = 0;
        for (size_t r = rowBegin; r < rowEnd; ++r) {
            float a = INFINITY, b = -INFINITY;
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
                if (columns[k - base] != c) continue;
                const TWord& w = words_[rowWords[k]];
                if (!(b > a)) {
                    const unsigned char h = FirstByte(w);
                    if (h >= 'a' && h <= 'z') lower++;
                }
                a = Fmin(a, w.x0), b = Fmax(b, w.x1);
                wordsIn++;
            }
            if (b > a) {
                widths.push_back(b - a);
                lo = Fmin(lo, a), hi = Fmax(hi, b);
            }
        }
        if (widths.size() < 2 || !(hi > lo)) continue;
        std::nth_element(widths.begin(), widths.begin() + static_cast<std::ptrdiff_t>(widths.size() / 2), widths.end());
        const float median = widths[widths.size() / 2];
        const float wordsPerLine = float(wordsIn) / float(widths.size());
        const bool filling = hi - lo >= 12 * em && median >= 0.65f * (hi - lo) && wordsPerLine >= 5;
        const bool sentences = widths.size() >= 3 && hi - lo >= 6 * em && wordsPerLine >= 2 && 2 * lower >= widths.size();
        if (filling || sentences) texty++;
        // Without rules closing it, one column of sentences (most lines continuing one) already
        // marks text beside prose (headings or a margin to its left); wrapped cells start half
        // their lines in lowercase at most.
        if (sentences && 10 * lower >= 7 * widths.size()) sentenceColumns++;
    }
    return texty >= 2 || (!closed && sentenceColumns >= 1);
}

// Rows of justified prose whose spaces line up (a river): in every row with words in two columns
// or more, the gaps between columns are no wider than a quarter more than the widest space between
// words of one column. Two such rows at least.
bool Detector::WordSpaced(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin,
                          size_t rowEnd, const std::vector<Gap>& gaps) const {
    size_t multi = 0, spaced = 0;
    for (size_t r = rowBegin; r < rowEnd; ++r) {
        float within = 0, across = INFINITY;
        int last = -1;
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
            const TWord& w = words_[rowWords[k]];
            const int c = ColumnOf(gaps, (w.x0 + w.x1) / 2);
            if (k > rows[r].first) {
                const float d = w.x0 - words_[rowWords[k - 1]].x1;
                if (c == last) within = Fmax(within, d);
                else across = Fmin(across, d);
            }
            last = c;
        }
        if (across == INFINITY) continue;
        multi++;
        spaced += within > 0 && across <= 1.25f * within;
    }
    return multi >= 2 && spaced == multi;
}

// The number of a line of a numbered listing ("7:", as LaTeX's algorithmic sets its steps).
bool LineNumber(std::string_view t) {
    if (t.size() < 2 || t.size() > 5 || t.back() != ':') return false;
    for (size_t k = 0; k + 1 < t.size(); ++k)
        if (t[k] < '0' || t[k] > '9') return false;
    return true;
}

// A first column of list markers (bullets, enumerators, the line numbers of a listing) is a list
// or the steps of an algorithm, not a table.
bool Detector::ListLike(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin,
                        size_t rowEnd, const std::vector<Gap>& gaps) {
    int markers = 0, filled = 0;
    std::string scratch;
    for (size_t r = rowBegin; r < rowEnd; ++r) {
        if (rows[r].count == 0) continue;
        const TWord& w = words_[rowWords[rows[r].first]];
        if (ColumnOf(gaps, (w.x0 + w.x1) / 2) != 0) continue;
        filled++;
        uint32_t value = 0;
        const std::string_view text = WordText(w, scratch);
        if (ParseMarker(text, value) != Marker::None || LineNumber(text)) markers++;
    }
    return filled > 0 && markers * 2 > filled;
}

// A grid from text rows and whitespace gaps: row boundaries halfway between rows, column boundaries
// in the middle of the gaps; words crossing a column boundary make that row's cells span it. A row
// whose first cell is empty and whose other cells continue cells above that run to their column's
// right edge is the wrapped text of the row above (multi-line cells). Rows flagged in `labels` are
// labels across the table: each a row of its own, whatever columns its words lie over.
void Detector::TextGrid(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t rowBegin,
                        size_t rowEnd, const std::vector<Gap>& bodyGaps, TableKind kind, uint16_t headerRows, float xa,
                        float xb, const std::vector<uint8_t>* labels, const std::vector<uint8_t>* ruled,
                        const std::vector<Edge>* partial, const std::vector<float>* joints) {
    if (grids_.size() >= static_cast<size_t>(kMaxTablesPerPage)) return;
    const int cols = static_cast<int>(bodyGaps.size()) + 1;
    if (rowEnd <= rowBegin || cols < 2 || cols > kMaxTableCols) return;
    // A header label (words a word space apart) wider than its column's text may reach past the
    // middle of the gap beside it (labels centred over narrow columns of figures; a label centred
    // over the gap spans both columns instead): the gap narrows to its widest stretch clear of the
    // labels of the header lines with such a label, when it has one a point wide.
    std::vector<Gap> gaps(bodyGaps);
    {
        std::vector<Gap> inks;
        const size_t headerEnd = std::min<size_t>(rowBegin + headerRows, rowEnd);
        for (Gap& gap : gaps) {
            const float mid = (gap.a + gap.b) / 2;
            inks.clear();
            for (size_t r = rowBegin; r < headerEnd; ++r) {
                const size_t lineInks = inks.size();
                bool crossed = false;
                for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count;) {
                    float a = words_[rowWords[k]].x0, b = words_[rowWords[k]].x1;
                    for (++k; k < rows[r].first + rows[r].count; ++k) {
                        const TWord& w = words_[rowWords[k]];
                        if (!(w.x0 - b < Fmax(1.0f, Fmin(2 * wordSpace_, 0.45f) * w.size))) break;
                        b = Fmax(b, w.x1);
                    }
                    if (b <= gap.a || a >= gap.b) continue;
                    const float cx = (a + b) / 2;
                    crossed = crossed || (a < mid && b > mid && (cx < gap.a || cx > gap.b));
                    inks.push_back(Gap{a, b});
                }
                if (!crossed) inks.resize(lineInks);
            }
            if (inks.empty()) continue;
            std::sort(inks.begin(), inks.end(), [](const Gap& p, const Gap& q) { return p.a < q.a; });
            float at = gap.a, best = 0;
            Gap clear = gap;
            for (const Gap& ink : inks) {
                if (ink.a - at > best) best = ink.a - at, clear = Gap{at, ink.a};
                at = Fmax(at, ink.b);
            }
            if (gap.b - at > best) best = gap.b - at, clear = Gap{at, gap.b};
            if (best >= 1.0f) gap = clear;
        }
    }
    Grid g;
    g.kind = kind;
    float left = INFINITY, right = -INFINITY;
    for (size_t r = rowBegin; r < rowEnd; ++r) left = Fmin(left, rows[r].x0), right = Fmax(right, rows[r].x1);
    left = Fmax(left, xa) - 1, right = Fmin(Fmax(right, left + 2), xb) + 1;
    g.xs.push_back(left);
    for (const Gap& gap : gaps) g.xs.push_back((gap.a + gap.b) / 2);
    g.xs.push_back(right);

    std::vector<int>& columns = scratch_.gridColumns;
    WordColumns(rows, rowWords, rowBegin, rowEnd, gaps, columns);
    const uint32_t base = rows[rowBegin].first;
    // The columns' extents, from the body's lines under a header set off by a rule (a header label
    // over several columns reaches past the column its centre lies in).
    std::vector<float> colLeft(static_cast<size_t>(cols), INFINITY), colRight(static_cast<size_t>(cols), -INFINITY);
    const size_t bodyBegin = ruled && rowBegin + headerRows < rowEnd ? rowBegin + headerRows : rowBegin;
    for (size_t r = bodyBegin; r < rowEnd; ++r)
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count && !(labels && (*labels)[r]); ++k) {
            const TWord& w = words_[rowWords[k]];
            const size_t c = static_cast<size_t>(columns[k - base]);
            colLeft[c] = Fmin(colLeft[c], w.x0), colRight[c] = Fmax(colRight[c], w.x1);
        }
    // Rows of multi-line cells. Where the whitespace between lines is of two kinds, the wider parts
    // rows and the narrower the lines of one cell (cell padding; cells set to the middle), except
    // that lines between wide gaps that all fill the same cells are rows of their own. Otherwise a
    // line continues the row above when its first cell is empty and its other cells continue cells
    // above that reach their column's right edge (wrapped text in cells set to the top). A label
    // is a row of its own, the line after it starts one, and so does a line flagged in `ruled` (a
    // rule of the table runs above it): the lines between such starts are judged on their own.
    std::vector<size_t> starts{rowBegin};
    const size_t n = rowEnd - rowBegin;
    std::vector<uint64_t> mask(n, 0);
    for (size_t r = rowBegin; r < rowEnd; ++r)
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k)
            mask[r - rowBegin] |= uint64_t(1) << columns[k - base];
    // Rules among the header lines narrower than the table, over a column boundary at least (a
    // \cmidrule under a label over columns): a header row ends above each (`cut`), and the words of
    // the line right above one that lie over it, a label, span the columns under it (`spans`: the
    // line, the rule).
    std::vector<uint8_t> cut(n, 0);
    std::vector<std::pair<size_t, const Edge*>> spans;
    for (size_t e = 0; partial && e < partial->size() && e < 64; ++e) {
        const Edge& rule = (*partial)[e];
        bool across = false;
        for (int c = 1; c < cols; ++c) across = across || (g.xs[static_cast<size_t>(c)] > rule.a + 1 && g.xs[static_cast<size_t>(c)] < rule.b - 1);
        for (size_t r = rowBegin + 1; across && r < rowEnd && r < rowBegin + headerRows; ++r)
            if (rows[r - 1].base > rule.pos && rows[r].base < rule.pos) {
                cut[r - rowBegin] = 1;
                spans.emplace_back(r - 1, &rule);
                break;
            }
    }
    // An equation array set in a grid: the last column of every line empty or an equation number
    // ("(3)", "(2.4a)"), two numbered lines at least, most of them with a relation sign before the
    // number. Displayed formulas, no table.
    {
        // The last column first (a word starting with "(" at most per line), then the relations.
        size_t numbered = 0, related = 0;
        bool equations = true;
        std::string text;
        for (size_t r = rowBegin; r < rowEnd && equations; ++r) {
            bool number = false;
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count && equations; ++k) {
                if (columns[k - base] != cols - 1) continue;
                const TWord& w = words_[rowWords[k]];
                equations = !number && FirstByte(w) == '(' && EquationNumber(WordText(w, text));
                number = true;
            }
            numbered += number;
        }
        for (size_t r = rowBegin; r < rowEnd && equations && numbered >= 2; ++r) {
            if (!(mask[r - rowBegin] >> (cols - 1) & 1)) continue;
            bool relation = false;
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count && !relation; ++k)
                relation = columns[k - base] != cols - 1 && Relation(WordText(words_[rowWords[k]], text));
            related += relation;
        }
        if (equations && related >= 2 && 2 * related >= numbered) return;
    }
    // A label alone in the first column, set halfway between two body lines whose first cells are
    // empty (a \multirow label centred over an even number of rows): it belongs to the row above
    // and parts no rows; the lines around it are judged as if it were not there.
    std::vector<uint8_t> centred(n, 0);
    for (size_t q = rowBegin + 1; q + 1 < rowEnd; ++q) {
        const size_t i = q - rowBegin;
        const uint64_t up = mask[i - 1], down = mask[i + 1];
        if (mask[i] != 1 || !up || !down || (up & 1) || (down & 1) || centred[i - 1] || i - 1 < headerRows) continue;
        if ((labels && ((*labels)[q - 1] || (*labels)[q] || (*labels)[q + 1])) || (ruled && ((*ruled)[q] || (*ruled)[q + 1])) ||
            cut[i] || cut[i + 1])
            continue;
        const float pitch = rows[q - 1].base - rows[q + 1].base, mid = (rows[q - 1].base + rows[q + 1].base) / 2;
        centred[i] = pitch > 0 && pitch <= 3.0f * Fmax(rows[q - 1].size, rows[q + 1].size) &&
                     std::fabs(rows[q].base - mid) <= 0.15f * pitch;
    }
    // The line before line r that the rows are judged from: a centred label is passed over.
    const auto lineBefore = [&](size_t r) { return centred[r - 1 - rowBegin] ? r - 2 : r - 1; };
    // Lines closer than a line pitch are cells of one row set to the middle at different heights.
    const auto offset = [&](size_t r) {
        const size_t p = lineBefore(r);
        return rows[p].base - rows[r].base < 0.85f * Fmax(rows[p].size, rows[r].size);
    };
    const auto gapAbove = [&](size_t r) { return offset(r) ? -INFINITY : rows[lineBefore(r)].bottom - rows[r].top; };
    // Per column of a line: the right edge of its words, whether its first word is a number and
    // whether its last word is one.
    std::vector<float> above(static_cast<size_t>(cols)), here(static_cast<size_t>(cols));
    std::vector<uint8_t> aboveFirst(static_cast<size_t>(cols)), aboveLast(static_cast<size_t>(cols));
    std::vector<uint8_t> hereFirst(static_cast<size_t>(cols)), hereLast(static_cast<size_t>(cols));
    const auto rights = [&](size_t r, std::vector<float>& out, std::vector<uint8_t>& first, std::vector<uint8_t>& last) {
        std::fill(out.begin(), out.end(), -INFINITY);
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
            const size_t c = static_cast<size_t>(columns[k - base]);
            const TWord& w = words_[rowWords[k]];
            const uint8_t number = Numeric(Chars(w)) ? 1 : 0;
            if (out[c] == -INFINITY) first[c] = number;
            last[c] = number;
            out[c] = Fmax(out[c], w.x1);
        }
    };
    // Lines [s, e) as one row of cells set to the middle (HTML's default vertical-align): every
    // filled column one cell (its lines at most 1.6 em apart), one cell of several lines and one
    // of a single line at least, and the middles of all of them (their first and last baselines')
    // within a fifth of an em of each other. A cell of several lines is wrapped text: evenly set,
    // every line but the last reaching the right part of its column, a line continuing the text
    // above (in lower case, or after a hyphen), and not values stacked (every line starting with a
    // number). Sub-rows beside a label spanning them, cells all of one line, or cells centred at
    // different heights (set to the top) are no such row.
    std::vector<float> cellFirst(static_cast<size_t>(cols)), cellLast(static_cast<size_t>(cols));
    std::vector<uint16_t> cellLines(static_cast<size_t>(cols));
    std::vector<uint8_t> cellValues(static_cast<size_t>(cols));  // every line so far starts with a number
    std::vector<float> gapLo(static_cast<size_t>(cols)), gapHi(static_cast<size_t>(cols));  // between its lines
    std::vector<float> lineRight(static_cast<size_t>(cols)), fillLow(static_cast<size_t>(cols));  // its lines' right edges
    std::vector<const TWord*> lastWord(static_cast<size_t>(cols));
    std::vector<uint8_t> continued(static_cast<size_t>(cols));  // a line continues the text of the line above
    std::string wordText;
    // The end e of the longest such run [s, e) of 2 to 16 lines ending at b at most, 0 if none. The
    // cells are read one line at a time and every run from s is judged as it grows, so each line is
    // read once for each s; a line too far below its cell's line above ends every longer run too.
    const auto longestMiddleRow = [&](size_t s, size_t b) {
        const float em = TextSize(rows[s]);
        std::fill(cellLines.begin(), cellLines.end(), uint16_t(0));
        size_t found = 0, stacked = 0;  // stacked: columns of two lines or more (none left for a one-line cell)
        for (size_t r = s; r < b && r < s + 16; ++r) {
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
                const size_t c = static_cast<size_t>(columns[k - base]);
                const TWord& w = words_[rowWords[k]];
                if (cellLines[c] > 0 && cellLast[c] == rows[r].base) {
                    lineRight[c] = Fmax(lineRight[c], w.x1);
                    lastWord[c] = &w;
                    continue;
                }
                const uint8_t value = Numeric(Chars(w)) ? 1 : 0;
                const float gap = cellLast[c] - rows[r].base;
                if (cellLines[c] == 0) {
                    cellFirst[c] = rows[r].base, cellValues[c] = value, gapLo[c] = INFINITY, gapHi[c] = -INFINITY;
                    fillLow[c] = INFINITY, continued[c] = 0;
                } else {
                    if (gap > 1.6f * em) return found;
                    cellValues[c] = cellValues[c] && value;
                    gapLo[c] = Fmin(gapLo[c], gap), gapHi[c] = Fmax(gapHi[c], gap);
                    fillLow[c] = Fmin(fillLow[c], lineRight[c]);
                    // Text continued: a line starting in lower case, or after a hyphen.
                    const unsigned char head = FirstByte(w);
                    const std::string_view before = WordText(*lastWord[c], wordText);
                    continued[c] = continued[c] || (head >= 'a' && head <= 'z') || (!before.empty() && before.back() == '-') ||
                                   (before.size() >= 2 && before.substr(before.size() - 2) == "\xC2\xAD");
                }
                lineRight[c] = w.x1;
                lastWord[c] = &w;
                cellLast[c] = rows[r].base;
                stacked += ++cellLines[c] == 2;
            }
            if (stacked == static_cast<size_t>(cols)) return found;
            const size_t e = r + 1;
            if (e - s < 2) continue;
            int filled = 0, multi = 0, single = 0;
            float lo = INFINITY, hi = -INFINITY, pitch = 0;
            bool cells = true;
            for (size_t c = 0; c < static_cast<size_t>(cols) && cells; ++c) {
                if (cellLines[c] == 0) continue;
                if (cellLines[c] < 2) {
                    single++;
                } else {
                    // Lines unevenly set (a label row over a cell's wrapped text) or a line but the
                    // last ending short of its column's right part (a label): no one cell, in any
                    // longer run either. Stacked values, or no line continuing the text above
                    // (entries of sub-rows beside a label spanning them): no one cell yet.
                    if (gapHi[c] - gapLo[c] > 0.1f * em || fillLow[c] < colLeft[c] + 0.6f * (colRight[c] - colLeft[c]))
                        return found;
                    if (cellValues[c] || !continued[c]) {
                        cells = false;
                        break;
                    }
                    multi++;
                }
                filled++;
                const float middle = (cellFirst[c] + cellLast[c]) / 2;
                lo = Fmin(lo, middle), hi = Fmax(hi, middle);
                if (cellLines[c] >= 2) pitch = Fmax(pitch, gapHi[c]);
            }
            if (!cells || !(filled >= 2 && multi >= 1 && single >= 1 && hi - lo <= 0.2f * em)) continue;
            // A row of its own: the lines of the table next to it stand further off than its
            // cells' lines stand apart (cell padding), else the lines are rows set on one pitch.
            const float apart = pitch + 0.15f * em;
            if ((s == rowBegin || rows[s - 1].base - rows[s].base >= apart) &&
                (e == rowEnd || rows[e - 1].base - rows[e].base >= apart))
                found = e;
        }
        return found;
    };
    std::vector<std::pair<size_t, size_t>> windows;  // part: runs of lines [s, e) that are such rows
    // The rows of lines [a, b).
    const auto part = [&](size_t a, size_t b) {
        const size_t k0 = starts.size() - 1;  // starts[k0] == a
        float narrow = INFINITY;
        size_t judged = 1;  // lines of [a, b) but centred labels
        for (size_t r = a + 1; r < b; ++r)
            if (!centred[r - rowBegin]) {
                judged++;
                if (!offset(r)) narrow = Fmin(narrow, gapAbove(r));
            }
        const float wide = narrow + Fmax(2.0f, 0.25f * rows[rowBegin].size);
        size_t wideGaps = 0;
        for (size_t r = a + 1; r < b; ++r) wideGaps += !centred[r - rowBegin] && gapAbove(r) >= wide;
        if (judged >= 3 && wideGaps >= 2 && wideGaps + 1 < judged) {
            size_t band = a;
            const auto close = [&](size_t bandEnd) {
                bool same = bandEnd - band > 1;
                for (size_t q = band + 1; q < bandEnd && same; ++q)
                    same = centred[q - rowBegin] || mask[q - rowBegin] == mask[band - rowBegin];
                int filled = 0;
                for (uint64_t m = mask[band - rowBegin]; m; m &= m - 1) filled++;
                if (same && filled >= 2) {
                    for (size_t q = band + 1; q < bandEnd; ++q)
                        if (!centred[q - rowBegin]) starts.push_back(q);
                    return;
                }
                // A full line (its first cell and another) on the line grid, clear of the line above,
                // under a line that fills the first cell too, starts a row: sections of one-line rows.
                // A line filling only the first cell is that cell's wrapped text.
                for (size_t q = band + 1; q < bandEnd; ++q) {
                    const size_t p = lineBefore(q);
                    if (!centred[q - rowBegin] && (mask[q - rowBegin] & 1) && (mask[q - rowBegin] & ~uint64_t(1)) &&
                        (mask[p - rowBegin] & 1) && !offset(q) && (q + 1 >= b || !offset(q + 1)) && rows[p].bottom >= rows[q].top)
                        starts.push_back(q);
                }
            };
            for (size_t r = a + 1; r < b; ++r) {
                if (centred[r - rowBegin] || (gapAbove(r) < wide && r - rowBegin != headerRows)) continue;
                close(r);
                starts.push_back(r);
                band = r;
            }
            close(b);
        } else {
            rights(a, above, aboveFirst, aboveLast);
            for (size_t r = a + 1; r < b; ++r) {
                if (centred[r - rowBegin]) continue;
                rights(r, here, hereFirst, hereLast);
                // A number under a number is a value of its own, not the cell's text wrapped, unless
                // set in smaller type right under it (a standard error under its estimate).
                const bool values = !SubLine(rows[lineBefore(r)], rows[r]);
                bool wrapped = here[0] == -INFINITY && r - rowBegin != headerRows;
                for (size_t c = 1; c < static_cast<size_t>(cols) && wrapped; ++c)
                    if (here[c] > -INFINITY &&
                        (!(above[c] >= colLeft[c] + 0.7f * (colRight[c] - colLeft[c])) ||
                         (values && hereFirst[c] && above[c] > -INFINITY && aboveLast[c])))
                        wrapped = false;
                // A header line less than a line below the first line of its row: a cell set to the
                // middle of a header of two lines ("Model" beside "Test" over "clean other").
                const size_t top = starts.back();
                const bool middle = r - rowBegin < headerRows && rows[top].base - rows[r].base < 0.85f * Fmax(rows[top].size, rows[r].size);
                if (!wrapped && !middle) starts.push_back(r);
                above.swap(here);
                aboveFirst.swap(hereFirst);
                aboveLast.swap(hereLast);
            }
        }
        // In a ruled table, body lines that together make one row of cells set to the middle (a
        // one-line cell between or beside the lines of a taller one) are one row, whatever the
        // rules above made of them: from each line, the longest such run of lines is a row, and
        // the search goes on after it. Header lines keep their rows (the header's own tests).
        if (kind != TableKind::Ruled || b - a < 3) return;
        windows.clear();
        for (size_t s = std::max(a, rowBegin + headerRows); s + 2 <= b;) {
            const size_t e = longestMiddleRow(s, b);
            if (e == 0) {
                ++s;
                continue;
            }
            windows.emplace_back(s, e);
            s = e;
        }
        if (windows.empty()) return;
        // The starts after a: none inside a window, one at each window's first line and after
        // its last.
        size_t kept = k0 + 1;
        for (size_t k = k0 + 1; k < starts.size(); ++k) {
            bool inside = false;
            for (const auto& [s, e] : windows) inside = inside || (starts[k] > s && starts[k] < e);
            if (!inside) starts[kept++] = starts[k];
        }
        starts.resize(kept);
        for (const auto& [s, e] : windows) {
            if (s > a) starts.push_back(s);
            if (e < b) starts.push_back(e);
        }
        std::sort(starts.begin() + static_cast<std::ptrdiff_t>(k0), starts.end());
        starts.erase(std::unique(starts.begin() + static_cast<std::ptrdiff_t>(k0), starts.end()), starts.end());
    };
    size_t from = rowBegin;
    for (size_t r = rowBegin + 1; r < rowEnd; ++r) {
        if (!((ruled && (*ruled)[r]) || cut[r - rowBegin] || (labels && ((*labels)[r] || (*labels)[r - 1])))) continue;
        part(from, r);
        starts.push_back(r);
        from = r;
    }
    part(from, rowEnd);
    // Without a header rule: up to four leading lines with no number in any cell, above a body whose
    // rows mostly hold numbers, are the header ("Aug 21," over "2011").
    if (headerRows == 0 && n >= 4) {
        // Numeric() of a cell's text from its words' digits and letters.
        const auto numbers = [&](size_t r) {
            int count = 0, last = -1;
            uint32_t size = 0, digits = 0, letters = 0;
            for (uint32_t k = rows[r].first; k <= rows[r].first + rows[r].count; ++k) {
                const int c = k < rows[r].first + rows[r].count ? columns[k - base] : -2;
                if (c != last && size > 0) {
                    count += NumericCounts(digits, letters);
                    size = digits = letters = 0;
                }
                if (c == -2) break;
                const WordChars& t = Chars(words_[rowWords[k]]);
                size += t.size, digits += t.digits, letters += t.letters;
                last = c;
            }
            return count;
        };
        size_t lead = 0;
        while (lead < 4 && rowBegin + lead < rowEnd && numbers(rowBegin + lead) == 0) ++lead;
        size_t numericRows = 0;
        for (size_t r = rowBegin + lead; r < rowEnd; ++r) numericRows += numbers(r) > 0;
        if (lead >= 2 && lead + 2 <= n && 2 * numericRows >= n - lead) headerRows = static_cast<uint16_t>(lead);
    }
    // Header lines above the header rule are one header row (its cells' text wraps, or sits at
    // different heights), unless a partial rule parts them (`spans`), or a label (a word, or words a
    // word space apart) runs across a column boundary, or stands centred over the gap between two
    // columns that another header line fills both: then the header has levels (a label over
    // several columns) and keeps its rows; so does a header whose lines are set as far apart as the
    // body's rows.
    if (headerRows >= 2) {
        bool levels = !spans.empty();
        const size_t headerEnd = std::min(rowBegin + headerRows, rowEnd);
        // A label [x0, x1] of line r centred over a gap whose columns another header line fills.
        const auto centred = [&](size_t r, float x0, float x1) {
            const float centre = (x0 + x1) / 2;
            for (size_t gi = 0; gi < gaps.size() && gi + 1 < 64; ++gi) {
                if (!(centre > gaps[gi].a && centre < gaps[gi].b)) continue;
                for (size_t q = rowBegin; q < headerEnd; ++q)
                    if (q != r && (mask[q - rowBegin] >> gi & 3) == 3) return true;
            }
            return false;
        };
        for (size_t r = rowBegin; r < headerEnd && !levels; ++r) {
            float x0 = 0, x1 = -INFINITY;  // the label so far
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count && !levels; ++k) {
                const TWord& w = words_[rowWords[k]];
                if (!(w.x0 - x1 < Fmax(1.0f, Fmin(2 * wordSpace_, 0.45f) * w.size))) {
                    if (x1 > -INFINITY) levels = centred(r, x0, x1);
                    x0 = w.x0;
                }
                x1 = w.x1;
                for (const Gap& gap : gaps) levels = levels || (x0 < gap.a && x1 > gap.b);
            }
            if (!levels && x1 > -INFINITY) levels = centred(r, x0, x1);
        }
        // Header lines set as far apart as the body's rows (the median pitch of the body rows' first
        // lines) are rows of their own: a line of units under the labels, as TeX sets two rows. The
        // lines of one wrapped cell sit closer than rows with padding between them.
        if (!levels) {
            std::vector<float> pitches;
            for (size_t s = 1; s < starts.size() && pitches.size() < 256; ++s)
                if (starts[s - 1] >= headerEnd) pitches.push_back(rows[starts[s - 1]].base - rows[starts[s]].base);
            if (!pitches.empty()) {
                std::nth_element(pitches.begin(), pitches.begin() + static_cast<std::ptrdiff_t>(pitches.size() / 2), pitches.end());
                const float pitch = pitches[pitches.size() / 2];
                bool apart = true;
                for (size_t r = rowBegin + 1; r < headerEnd && apart; ++r) apart = rows[r - 1].base - rows[r].base >= 0.9f * pitch;
                if (apart) {
                    size_t kept = 0;
                    for (size_t s : starts)
                        if (s >= headerEnd) starts[kept++] = s;
                    starts.resize(kept);
                    starts.insert(starts.begin(), headerEnd - rowBegin, 0);
                    for (size_t r = rowBegin; r < headerEnd; ++r) starts[r - rowBegin] = r;
                    levels = true;
                }
            }
        }
        if (!levels) {
            size_t kept = 0;
            for (size_t s : starts)
                if (s == rowBegin || s >= rowBegin + headerRows) starts[kept++] = s;
            starts.resize(kept);
            headerRows = 1;
        }
    }
    const int nrows = static_cast<int>(starts.size());
    if (nrows > kMaxTableRows) return;
    // Two rows whose text stands in different columns, fewer than half the columns filled in both:
    // labels of boxes set at two heights (a timing diagram's frames), no table.
    if (nrows == 2) {
        uint64_t top = 0, low = 0;
        for (size_t r = rowBegin; r < rowEnd; ++r) (r < starts[1] ? top : low) |= mask[r - rowBegin];
        if (2 * Bits(top & low) < cols) return;
    }
    g.headerRows = 0;
    for (size_t s : starts)
        if (s < rowBegin + headerRows) g.headerRows++;
    // Boundaries halfway between groups: a group spans its lines' extents (cells set to the middle
    // put lines of a later baseline above an earlier one's bottom).
    const auto extent = [&](size_t s, float& top, float& bottom) {
        const size_t end = s + 1 < starts.size() ? starts[s + 1] : rowEnd;
        top = -INFINITY, bottom = INFINITY;
        for (size_t r = starts[s]; r < end; ++r) top = Fmax(top, rows[r].top), bottom = Fmin(bottom, rows[r].bottom);
    };
    float top = 0, bottom = 0, previousBottom = 0;
    extent(0, top, bottom);
    g.ys.push_back(top + 1);
    previousBottom = bottom;
    for (size_t s = 1; s < starts.size(); ++s) {
        extent(s, top, bottom);
        g.ys.push_back((previousBottom + top) / 2);
        previousBottom = bottom;
    }
    g.ys.push_back(previousBottom - 1);
    for (size_t i = 1; i < g.ys.size(); ++i)
        if (!(g.ys[i] < g.ys[i - 1])) return;  // overlapping rows: not a grid
    g.x0 = g.xs.front(), g.x1 = g.xs.back(), g.y0 = g.ys.back(), g.y1 = g.ys.front();
    g.vb.assign(static_cast<size_t>(nrows) * static_cast<size_t>(cols + 1), 1);
    g.hb.assign(static_cast<size_t>(nrows + 1) * static_cast<size_t>(cols), 1);
    int group = -1;
    std::vector<Progression> numbers(static_cast<size_t>(nrows) * static_cast<size_t>(cols));
    std::string scratch;
    bool numbersOnly = true;  // every word a plain number, no label row
    for (size_t r = rowBegin; r < rowEnd; ++r) {
        if (group + 1 < nrows && starts[static_cast<size_t>(group + 1)] == r) ++group;
        if (labels && (*labels)[r]) {
            if (g.labelRow.empty()) g.labelRow.assign(static_cast<size_t>(nrows), 0);
            g.labelRow[static_cast<size_t>(group)] = 1;
            numbersOnly = false;
            continue;
        }
        uint64_t emails = 0;  // the columns of the line holding an e-mail address
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
            const TWord& w = words_[rowWords[k]];
            // E-mail addresses side by side on a line: the authors' addresses under their names
            // and affiliations (an author block set in columns), no table.
            const WordChars& t = Chars(w);
            if (t.at && EmailAddress(WordText(w, scratch))) emails |= uint64_t(1) << std::min(columns[k - base], 63);
            if (Bits(emails) >= 2) return;
            // A chart's axis in a cell: no table. Pieces of tall brackets (U+239B-U+23AD): a
            // matrix set in display math.
            float value = 0;
            const size_t cell = static_cast<size_t>(group * cols + columns[k - base]);
            const bool number = t.numberish && ParseNumber(WordText(w, scratch), value);
            if (number && numbers[cell].Add(value)) return;
            numbersOnly = numbersOnly && number;
            if (t.size >= 3 && t.head[0] == 0xE2 && t.head[1] == 0x8E && t.head[2] >= 0x9B && t.head[2] <= 0xAD) return;
            // The word before it in the row, when only a word space apart: one cell's text, unless
            // the word starts where its column's text starts (labels set a word space apart, each at
            // its column's left edge) or a joint of the table's rules lies between them.
            const TWord* before = k > rows[r].first ? &words_[rowWords[k - 1]] : nullptr;
            const bool joined = before && w.x0 - before->x1 < Fmax(1.0f, Fmin(2 * wordSpace_, 0.45f) * w.size);
            for (int c = 1; c < cols; ++c) {
                const float x = g.xs[static_cast<size_t>(c)];
                const auto jointBetween = [&]() {
                    for (size_t j = 0; joints && j < joints->size(); ++j)
                        if ((*joints)[j] > before->x1 && (*joints)[j] < w.x0) return true;
                    return false;
                };
                if ((w.x0 < x - 0.5f && w.x1 > x + 0.5f) ||
                    (joined && before->x1 < x && w.x0 > x && !(std::fabs(w.x0 - colLeft[static_cast<size_t>(c)]) <= 0.3f * w.size) &&
                     !jointBetween()))
                    g.vb[static_cast<size_t>(group * (cols + 1) + c)] = 0;
            }
        }
        // The label over a partial rule spans the columns under the rule: its words, a word space
        // apart, and no other words of the line over the rule.
        for (const auto& [line, rule] : spans) {
            if (line != r) continue;
            float right = -INFINITY;
            bool label = true, any = false;
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count && label; ++k) {
                const TWord& w = words_[rowWords[k]];
                if (w.x1 < rule->a || w.x0 > rule->b) continue;
                const float cx = (w.x0 + w.x1) / 2;
                label = cx >= rule->a && cx <= rule->b && (!any || w.x0 - right < Fmax(1.0f, Fmin(2 * wordSpace_, 0.45f) * w.size));
                any = true;
                right = w.x1;
            }
            for (int c = 1; c < cols && label && any; ++c) {
                const float x = g.xs[static_cast<size_t>(c)];
                if (x > rule->a + 1 && x < rule->b - 1) g.vb[static_cast<size_t>(group * (cols + 1) + c)] = 0;
            }
        }
    }
    // In a turned frame, rows of plain numbers alone are the tick labels of a plot's turned axis.
    if (dir_ != 0 && numbersOnly) return;
    MarkUsed(g);
    grids_.push_back(std::move(g));
}

// The boundary maps of a lattice from its component's edges (each edge counts for the nearest
// boundary: one whose sliver of a column was collapsed counts for the boundary that took its place).
void Detector::Boundaries(Grid& g, const Component& comp) const {
    const int rows = g.Rows(), cols = g.Cols();
    const float reach = Fmax(kSnap, 1.2f * bodySize_);
    g.vb.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols + 1), 0);
    g.hb.assign(static_cast<size_t>(rows + 1) * static_cast<size_t>(cols), 0);
    for (size_t m = 0; m < comp.vCount; ++m) {
        const Edge& e = v_[comp.v[m]];
        int c = 0;
        for (int k = 1; k <= cols; ++k)
            if (std::fabs(g.xs[static_cast<size_t>(k)] - e.pos) < std::fabs(g.xs[static_cast<size_t>(c)] - e.pos)) c = k;
        if (std::fabs(g.xs[static_cast<size_t>(c)] - e.pos) > reach) continue;
        for (int r = 0; r < rows; ++r) {
            const float top = g.ys[static_cast<size_t>(r)], bottom = g.ys[static_cast<size_t>(r + 1)];
            const float overlap = Fmin(top, e.b) - Fmax(bottom, e.a);
            if (overlap >= 0.5f * (top - bottom)) g.vb[static_cast<size_t>(r * (cols + 1) + c)] = 1;
        }
    }
    for (size_t m = 0; m < comp.hCount; ++m) {
        const Edge& e = h_[comp.h[m]];
        int r = 0;
        for (int k = 1; k <= rows; ++k)
            if (std::fabs(g.ys[static_cast<size_t>(k)] - e.pos) < std::fabs(g.ys[static_cast<size_t>(r)] - e.pos)) r = k;
        if (std::fabs(g.ys[static_cast<size_t>(r)] - e.pos) > reach) continue;
        for (int c = 0; c < cols; ++c) {
            const float left = g.xs[static_cast<size_t>(c)], right = g.xs[static_cast<size_t>(c + 1)];
            const float overlap = Fmin(right, e.b) - Fmax(left, e.a);
            if (overlap >= 0.5f * (right - left)) g.hb[static_cast<size_t>(r * cols + c)] = 1;
        }
    }
}

// A vertical boundary missing in a row (shaded rows without their rulings) still parts the row
// when the row has text on both sides of it and none across it: a cell spans a column only when
// its text does.
void Detector::SplitByText(Grid& g) const {
    const int rows = g.Rows(), cols = g.Cols();
    const size_t n = static_cast<size_t>(rows) * static_cast<size_t>(cols + 1);
    std::vector<uint8_t> left(n, 0), right(n, 0), across(n, 0);
    auto it = std::lower_bound(words_.begin(), words_.end(), g.y1 + 20, [](const TWord& w, float y) { return w.base > y; });
    Locator locate(g);
    for (; it != words_.end() && it->base >= g.y0 - 20; ++it) {
        const TWord& w = *it;
        int r = 0, c = 0;
        if (!locate.Find((w.x0 + w.x1) / 2, (w.y0 + w.y1) / 2, r, c)) continue;
        const size_t row = static_cast<size_t>(r) * static_cast<size_t>(cols + 1);
        left[row + static_cast<size_t>(c) + 1] = 1;  // ink left of the column's right boundary
        right[row + static_cast<size_t>(c)] = 1;     // ink right of its left boundary
        for (int k = 1; k < cols; ++k) {
            const float x = g.xs[static_cast<size_t>(k)];
            if (w.x0 < x - 1 && w.x1 > x + 1) across[row + static_cast<size_t>(k)] = 1;
        }
    }
    for (int r = 0; r < rows; ++r)
        for (int k = 1; k < cols; ++k) {
            const size_t i = static_cast<size_t>(r) * static_cast<size_t>(cols + 1) + static_cast<size_t>(k);
            if (!g.vb[i] && left[i] && right[i] && !across[i]) g.vb[i] = 1;
        }
}

// Rows of a lattice drawn without rules between them whose vertical rules are drawn row by row
// (TeX draws a row's | rules with the row): a height inside a grid row where the rule of every
// boundary drawn in that row ends and another piece of it starts, and through no word, parts the
// row. Pieces shorter than a line (the dashes of a dashed rule) say nothing. `pieces`: the page's
// vertical rule pieces as drawn (before MergeEdges joins them), by x, read on first use (`read`).
void Detector::SplitRowsByVerticals(Grid& g, std::vector<Edge>& pieces, bool& read) const {
    const int rows = g.Rows(), cols = g.Cols();
    const float minPiece = Fmax(kMinEdge, 0.8f * bodySize_);
    if (!read) {
        read = true;
        if (page_.rulings.size() > 8 * size_t(kMaxTableEdges)) return;
        for (const Ruling& upright : page_.rulings) {
            const Ruling r = ToFrame(upright, dir_);
            const float w = r.x1 - r.x0, t = r.y1 - r.y0;
            if (w >= 0 && w <= kMaxThick && t >= minPiece && t > w) pieces.push_back(Edge{(r.x0 + r.x1) / 2, r.y0, r.y1});
        }
        std::sort(pieces.begin(), pieces.end(), [](const Edge& p, const Edge& q) { return p.pos < q.pos; });
    }
    if (cols < 2 || pieces.size() < 4) return;
    struct Piece {
        int k;       // the grid's vertical boundary it lies on
        float a, b;  // its lower and upper end
    };
    std::vector<Piece> ends, starts;  // by boundary, then lower (upper) end
    for (int k = 0; k <= cols; ++k) {
        const float x = g.xs[static_cast<size_t>(k)];
        auto it = std::lower_bound(pieces.begin(), pieces.end(), x - kSnap, [](const Edge& p, float at) { return p.pos < at; });
        for (; it != pieces.end() && it->pos <= x + kSnap; ++it)
            if (it->b >= g.y0 - kSnap && it->a <= g.y1 + kSnap) ends.push_back(Piece{k, it->a, it->b});
    }
    if (ends.size() < 4) return;
    // A row parts only at a piece's lower end inside it, minPiece clear of its edges: with none
    // inside the grid, no row parts.
    bool inside = false;
    for (const Piece& p : ends) inside = inside || (p.a >= g.ys.back() + minPiece && p.a < g.ys.front() - minPiece);
    if (!inside) return;
    starts = ends;
    std::sort(ends.begin(), ends.end(), [](const Piece& p, const Piece& q) { return p.k != q.k ? p.k < q.k : p.a < q.a; });
    std::sort(starts.begin(), starts.end(), [](const Piece& p, const Piece& q) { return p.k != q.k ? p.k < q.k : p.b < q.b; });
    // A piece of boundary k with its lower (upper) end within kSnap of y.
    const auto has = [](const std::vector<Piece>& v, int k, float y, bool lower) {
        auto it = std::lower_bound(v.begin(), v.end(), y - kSnap, [&](const Piece& p, float at) {
            return p.k != k ? p.k < k : (lower ? p.a : p.b) < at;
        });
        return it != v.end() && it->k == k && (lower ? it->a : it->b) <= y + kSnap;
    };
    std::vector<float> ys{g.ys.front()}, cuts;
    std::vector<uint8_t> vb, hb(g.hb.begin(), g.hb.begin() + cols);
    for (int r = 0; r < rows; ++r) {
        const float top = g.ys[static_cast<size_t>(r)], bottom = g.ys[static_cast<size_t>(r + 1)];
        const size_t row = static_cast<size_t>(r) * static_cast<size_t>(cols + 1);
        cuts.clear();
        int first = -1, drawn = 0;
        for (int k = 0; k <= cols; ++k)
            if (g.vb[row + static_cast<size_t>(k)]) drawn++, first = first < 0 ? k : first;
        auto it = std::lower_bound(ends.begin(), ends.end(), bottom + minPiece, [&](const Piece& p, float at) {
            return p.k != first ? p.k < first : p.a < at;
        });
        for (; drawn >= 2 && it != ends.end() && it->k == first && it->a < top - minPiece; ++it) {
            const float y = it->a;
            if (!cuts.empty() && y - cuts.back() <= kSnap) continue;
            bool all = has(starts, first, y, false);
            for (int k = first + 1; k <= cols && all; ++k)
                if (g.vb[row + static_cast<size_t>(k)]) all = has(ends, k, y, true) && has(starts, k, y, false);
            if (!all) continue;
            auto w = std::lower_bound(words_.begin(), words_.end(), y + 2 * bodySize_, [](const TWord& q, float at) { return q.base > at; });
            for (; w != words_.end() && w->base >= y - 2 * bodySize_ && all; ++w)
                all = !(w->y0 < y - 1 && w->y1 > y + 1 && w->x1 > g.x0 && w->x0 < g.x1);
            if (all) cuts.push_back(y);
        }
        // Ascending from the pieces' order: the grid's rows go top down.
        std::sort(cuts.begin(), cuts.end(), std::greater<float>());
        for (float y : cuts) {
            ys.push_back(y);
            vb.insert(vb.end(), g.vb.begin() + static_cast<std::ptrdiff_t>(row), g.vb.begin() + static_cast<std::ptrdiff_t>(row + cols + 1));
            hb.insert(hb.end(), static_cast<size_t>(cols), 1);
        }
        ys.push_back(bottom);
        vb.insert(vb.end(), g.vb.begin() + static_cast<std::ptrdiff_t>(row), g.vb.begin() + static_cast<std::ptrdiff_t>(row + cols + 1));
        hb.insert(hb.end(), g.hb.begin() + (r + 1) * cols, g.hb.begin() + (r + 2) * cols);
    }
    if (ys.size() == g.ys.size() || static_cast<int>(ys.size()) - 1 > kMaxTableRows) return;
    g.ys.swap(ys);
    g.vb.swap(vb);
    g.hb.swap(hb);
}

// Rows of a lattice drawn without rules between them (rules only under the header and at the
// bottom, or a box around the body): a grid row whose text sits on two baselines or more (three in
// a box of one row) that each hold text in the row's first column and another ("full" lines), with
// a column of numbers on two thirds of them (on both of two), is that many table rows. It parts
// halfway between those lines; a line
// with text in one column only (a wrapped cell) or set less than a line below the one above
// (cells set to the middle of a row) stays with the line above.
void Detector::SplitRowsByLines(Grid& g) const {
    const int rows = g.Rows(), cols = g.Cols();
    if (cols < 2 || cols > 64) return;
    struct Baseline {
        float base, size;
        uint64_t mask, numeric;  // columns with text; columns with a number
    };
    std::vector<Baseline> lines;
    std::vector<float> ys{g.ys.front()}, cuts;
    std::vector<uint8_t> vb, hb(g.hb.begin(), g.hb.begin() + cols);
    uint64_t first = 0;  // the row's first column with text
    const auto full = [&](const Baseline& l) { return (l.mask & first) && Bits(l.mask) >= 2; };
    Locator locate(g);  // g's rows and columns stay as they are until the end
    for (int r = 0; r < rows; ++r) {
        const float top = g.ys[static_cast<size_t>(r)], bottom = g.ys[static_cast<size_t>(r + 1)];
        lines.clear();
        cuts.clear();
        auto it = std::lower_bound(words_.begin(), words_.end(), top + 20, [](const TWord& w, float y) { return w.base > y; });
        for (; it != words_.end() && it->base >= bottom - 20; ++it) {
            const TWord& w = *it;
            int wr = 0, c = 0;
            if (!locate.Find((w.x0 + w.x1) / 2, (w.y0 + w.y1) / 2, wr, c) || wr != r) continue;
            const uint64_t bit = uint64_t(1) << c;
            const uint64_t number = Numeric(Chars(w)) ? bit : 0;
            if (!lines.empty() && lines.back().base - w.base <= 0.35f * w.size) {
                lines.back().mask |= bit, lines.back().numeric |= number;
            } else {
                lines.push_back(Baseline{w.base, w.size, bit, number});
            }
        }
        uint64_t any = 0;
        for (const Baseline& l : lines) any |= l.mask;
        first = any & (~any + 1);
        size_t fullLines = 0;
        for (const Baseline& l : lines) fullLines += full(l);
        bool numbers = false;
        for (int c = 0; c < cols && fullLines >= (rows > 1 ? 2u : 3u) && !numbers; ++c) {
            size_t n = 0;
            for (const Baseline& l : lines) n += full(l) && ((l.numeric >> c) & 1);
            numbers = 3 * n >= 2 * fullLines;
        }
        // Part above each full line but the first that lies a line pitch below the lines before it.
        float above = NAN;  // the lowest baseline so far
        for (const Baseline& l : lines) {
            if (numbers && full(l) && above - l.base >= 0.85f * l.size) {
                const float y = (above + l.base) / 2 + 0.3f * l.size;
                if (y < (cuts.empty() ? top : cuts.back()) && y > bottom) cuts.push_back(y);
            }
            if (!std::isnan(above) || full(l)) above = std::isnan(above) ? l.base : Fmin(above, l.base);
        }
        for (float y : cuts) {
            ys.push_back(y);
            vb.insert(vb.end(), g.vb.begin() + r * (cols + 1), g.vb.begin() + (r + 1) * (cols + 1));
            hb.insert(hb.end(), static_cast<size_t>(cols), 1);
        }
        ys.push_back(bottom);
        vb.insert(vb.end(), g.vb.begin() + r * (cols + 1), g.vb.begin() + (r + 1) * (cols + 1));
        hb.insert(hb.end(), g.hb.begin() + (r + 1) * cols, g.hb.begin() + (r + 2) * cols);
    }
    if (ys.size() == g.ys.size() || static_cast<int>(ys.size()) - 1 > kMaxTableRows) return;
    g.ys.swap(ys);
    g.vb.swap(vb);
    g.hb.swap(hb);
}

// A cell spanning rows whose text is one line in each row it spans, the i-th line from the top
// within the i-th row (a label written over two rows of a LaTeX table, its \cline under the
// other columns only): a cell in each row. True when a grid's cell was split so.
bool Detector::SplitSpanLines(Grid& g) const {
    constexpr int kMaxLines = 32;
    const int cols = g.Cols();
    bool split = false;
    float bases[kMaxLines];
    for (const Span& s : g.cells) {
        if (s.rs < 2 || s.rs > kMaxLines) continue;
        const float left = g.xs[s.c], right = g.xs[s.c + s.cs];
        const float top = g.ys[s.r], bottom = g.ys[s.r + s.rs];
        int lines = 0;
        float lineSize = 0;
        auto it = std::lower_bound(words_.begin(), words_.end(), top + 20, [](const TWord& w, float y) { return w.base > y; });
        for (size_t i = static_cast<size_t>(it - words_.begin()); i < words_.size() && words_[i].base >= bottom - 20; ++i) {
            const TWord& w = words_[i];
            const float cx = (w.x0 + w.x1) / 2, cy = (w.y0 + w.y1) / 2;
            if (!(cx >= left && cx <= right && cy <= top && cy >= bottom)) continue;
            // words_ descend by baseline: a word within 0.3 of the smaller size below the line's
            // first word is on that line.
            if (lines > 0 && bases[lines - 1] - w.base <= 0.3f * Fmin(lineSize, w.size)) continue;
            if (lines == s.rs) {
                lines++;  // more lines than rows
                break;
            }
            bases[lines++] = w.base;
            lineSize = w.size;
        }
        if (lines != s.rs) continue;
        bool fits = true;
        for (int i = 0; i < s.rs && fits; ++i)
            fits = g.ys[static_cast<size_t>(s.r + i)] >= bases[i] && bases[i] >= g.ys[static_cast<size_t>(s.r + i + 1)];
        if (!fits) continue;
        for (int i = 1; i < s.rs; ++i)
            for (int k = s.c; k < s.c + s.cs; ++k) g.hb[static_cast<size_t>((s.r + i) * cols + k)] = 1;
        split = true;
    }
    return split;
}

// Columns of a lattice drawn without rules between them (a rule only around a group of columns
// under a header label): whitespace an em wide inside a grid column, through the text of every row
// below the first (three rows at least; through five, as narrow as two word spaces), with text on
// both sides of it in two thirds of those rows, parts the column; under two rows, whitespace two
// ems wide with text on both sides in both. A word across the new boundary (the group's label)
// spans it; the boundary moves within the whitespace to a stretch half an em wide clear of the
// first row's words when there is one (labels of neighbouring columns set closer than their
// columns).
void Detector::SplitColumnsByText(Grid& g) const {
    const int rows = g.Rows(), cols = g.Cols();
    if (rows < 3) return;
    std::vector<float> xs{g.xs.front()};
    std::vector<int> parent;  // per new column: the old column it lies in
    std::vector<int> oldBoundary{0};  // per new boundary: the old boundary, or -1 for a new one
    struct Ink {
        float a, b;
        int row;
        bool number;
    };
    std::vector<Ink> ink;
    std::vector<uint8_t> left(static_cast<size_t>(rows)), right(static_cast<size_t>(rows)), any(static_cast<size_t>(rows));
    std::vector<const Ink*> nearLeft(static_cast<size_t>(rows)), nearRight(static_cast<size_t>(rows));
    // The words in the grid's band with their cells, in words_' order: Find is a function of the
    // point alone, and g stays as it is until the end.
    struct Located {
        float x0, x1, size;
        int r, c;
        bool number;
    };
    std::vector<Located> located;
    {
        Locator locate(g);
        auto it = std::lower_bound(words_.begin(), words_.end(), g.y1 + 20, [](const TWord& w, float y) { return w.base > y; });
        for (; it != words_.end() && it->base >= g.y0 - 20; ++it) {
            int r = 0, c = 0;
            if (locate.Find((it->x0 + it->x1) / 2, (it->y0 + it->y1) / 2, r, c)) located.push_back(Located{it->x0, it->x1, it->size, r, c, Numeric(Chars(*it))});
        }
    }
    std::vector<Ink> head;  // the first row's words in the column, left to right
    for (int c = 0; c < cols; ++c) {
        ink.clear();
        head.clear();
        float size = 0;
        for (size_t i = 0; i < located.size() && ink.size() < 4096; ++i) {
            const Located& w = located[i];
            if (w.c != c) continue;
            if (w.r == 0) {
                if (head.size() < 4096) head.push_back(Ink{w.x0, w.x1, 0, w.number});
                continue;
            }
            ink.push_back(Ink{w.x0, w.x1, w.r, w.number});
            size = Fmax(size, w.size);
        }
        std::sort(ink.begin(), ink.end(), [](const Ink& p, const Ink& q) { return p.a < q.a; });
        std::sort(head.begin(), head.end(), [](const Ink& p, const Ink& q) { return p.a < q.a; });
        // Through six rows or more, whitespace as narrow as two word spaces (0.4 em at least)
        // parts columns too (tables of many columns set close).
        const float minGap = rows >= 6 ? Fmax(2.5f, Fmin(size, Fmax(0.4f, 2 * wordSpace_) * size))
                                       : rows >= 4 ? Fmax(4.0f, size) : Fmax(8.0f, 2 * size);
        // Through six rows or more, whitespace a quarter wider than a word space parts columns of
        // numbers: every row below the first has a number on both sides of it, next to it.
        const float numberGap = rows >= 6 ? Fmin(minGap, Fmax(1.5f, 1.25f * wordSpace_ * size)) : minGap;
        float reach = -INFINITY;
        for (size_t i = 0; i < ink.size(); ++i) {
            if (i > 0 && ink[i].a - reach >= numberGap && static_cast<int>(xs.size()) <= kMaxTableCols) {
                const bool narrow = ink[i].a - reach < minGap;
                float x = (reach + ink[i].a) / 2;
                // The widest stretch of the gap between two words of the first row that both reach
                // into it.
                bool crossed = false;
                float at = -INFINITY, best = 0, bestX = x;
                for (const Ink& h : head) {
                    crossed = crossed || (h.a < x - 0.5f && h.b > x + 0.5f);
                    if (at > reach && h.a < ink[i].a && h.a - at > best) best = h.a - at, bestX = (at + h.a) / 2;
                    at = Fmax(at, h.b);
                }
                if (crossed && best >= 0.5f * size) {
                    x = bestX;
                } else if (crossed) {
                    // Labels set a word space apart, each where its column's text starts: the
                    // boundary goes between the label that starts at the column on the right and
                    // the one before it.
                    float end = -INFINITY;
                    for (const Ink& h : head) {
                        if (std::fabs(h.a - ink[i].a) <= 0.3f * size && h.a - end >= 1.0f && end > reach - size) {
                            x = (end + h.a) / 2;
                            break;
                        }
                        end = Fmax(end, h.b);
                    }
                }
                std::fill(left.begin(), left.end(), 0), std::fill(right.begin(), right.end(), 0), std::fill(any.begin(), any.end(), 0);
                std::fill(nearLeft.begin(), nearLeft.end(), nullptr), std::fill(nearRight.begin(), nearRight.end(), nullptr);
                for (const Ink& k : ink) {
                    const size_t r = static_cast<size_t>(k.row);
                    any[r] = 1;
                    (k.b < x ? left : right)[r] = 1;
                    if (k.b < x && (!nearLeft[r] || k.b > nearLeft[r]->b)) nearLeft[r] = &k;
                    if (k.b >= x && (!nearRight[r] || k.a < nearRight[r]->a)) nearRight[r] = &k;
                }
                int filled = 0, both = 0, numbers = 0;
                for (int r = 1; r < rows; ++r) {
                    const size_t q = static_cast<size_t>(r);
                    filled += any[q], both += left[q] && right[q];
                    numbers += nearLeft[q] && nearRight[q] && nearLeft[q]->number && nearRight[q]->number;
                }
                if (narrow ? numbers == rows - 1
                           : rows >= 4 ? filled >= 3 && 3 * both >= 2 * filled : filled == 2 && both == 2) {
                    xs.push_back(x);
                    parent.push_back(c);
                    oldBoundary.push_back(-1);
                }
            }
            reach = Fmax(reach, ink[i].b);
        }
        xs.push_back(g.xs[static_cast<size_t>(c + 1)]);
        parent.push_back(c);
        oldBoundary.push_back(c + 1);
    }
    if (xs.size() == g.xs.size()) return;
    const int n = static_cast<int>(xs.size()) - 1;
    std::vector<uint8_t> vb(static_cast<size_t>(rows) * static_cast<size_t>(n + 1), 1), hb(static_cast<size_t>(rows + 1) * static_cast<size_t>(n));
    for (int r = 0; r < rows; ++r)
        for (int k = 0; k <= n; ++k)
            if (oldBoundary[static_cast<size_t>(k)] >= 0)
                vb[static_cast<size_t>(r * (n + 1) + k)] = g.vb[static_cast<size_t>(r * (cols + 1) + oldBoundary[static_cast<size_t>(k)])];
    for (int r = 0; r <= rows; ++r)
        for (int j = 0; j < n; ++j) hb[static_cast<size_t>(r * n + j)] = g.hb[static_cast<size_t>(r * cols + parent[static_cast<size_t>(j)])];
    // A word across a new boundary joins the cells beside it.
    for (const Located& w : located)
        for (int k = 1; k < n; ++k)
            if (oldBoundary[static_cast<size_t>(k)] < 0 && w.x0 < xs[static_cast<size_t>(k)] - 0.5f && w.x1 > xs[static_cast<size_t>(k)] + 0.5f)
                vb[static_cast<size_t>(w.r * (n + 1) + k)] = 0;
    g.xs.swap(xs);
    g.vb.swap(vb);
    g.hb.swap(hb);
}

// The header of a lattice: its leading rows whose first cell is empty (or continues one from
// above) or that are one cell across (labels over the columns, a title inside the frame), and
// under titles alone the row of column labels after them, when two rows follow them at least.
void Detector::LatticeHeader(Grid& g) const {
    const int rows = g.Rows(), cols = g.Cols();
    std::vector<uint8_t> filled(g.cells.size(), 0);
    auto it = std::lower_bound(words_.begin(), words_.end(), g.y1 + 20, [](const TWord& w, float y) { return w.base > y; });
    Locator locate(g);
    for (; it != words_.end() && it->base >= g.y0 - 20; ++it) {
        int r = 0, c = 0;
        if (locate.Find((it->x0 + it->x1) / 2, (it->y0 + it->y1) / 2, r, c))
            filled[static_cast<size_t>(g.owner[static_cast<size_t>(r * cols + c)])] = 1;
    }
    int header = 0;
    bool titles = true;  // the rows so far are each one cell across
    while (header < rows) {
        const size_t cell = static_cast<size_t>(g.owner[static_cast<size_t>(header * cols)]);
        const Span& s = g.cells[cell];
        if (!(s.r < header || !filled[cell] || s.cs == cols)) break;
        titles = titles && s.cs == cols;
        ++header;
    }
    // Under titles alone, the next row holds the column labels: text without numbers.
    if (titles && header > 0 && header < rows) {
        bool numbers = false;
        it = std::lower_bound(words_.begin(), words_.end(), g.y1 + 20, [](const TWord& w, float y) { return w.base > y; });
        for (; it != words_.end() && it->base >= g.y0 - 20 && !numbers; ++it) {
            int r = 0, c = 0;
            numbers = locate.Find((it->x0 + it->x1) / 2, (it->y0 + it->y1) / 2, r, c) && r == header && Numeric(Chars(*it));
        }
        if (!numbers) ++header;
    }
    if (header >= 2 && rows - header >= 2) g.headerRows = static_cast<uint16_t>(header);
}

// The columns of grid g that line r fills (a bit each), or 0 when a word of it lies outside the
// grid or across a column boundary, or two words a word space apart lie on both sides of one (one
// label across the columns, a section's name).
uint64_t Detector::LineColumns(const Row& r, const std::vector<uint32_t>& rowWords, const Grid& g) const {
    const int cols = g.Cols();
    uint64_t mask = 0;
    for (uint32_t k = r.first; k < r.first + r.count; ++k) {
        const TWord& w = words_[rowWords[k]];
        if (w.x0 < g.x0 - 1 || w.x1 > g.x1 + 1) return 0;
        const TWord* before = k > r.first ? &words_[rowWords[k - 1]] : nullptr;
        const bool joined = before && w.x0 - before->x1 < Fmax(1.0f, Fmin(2 * wordSpace_, 0.45f) * w.size);
        for (int c = 1; c < cols; ++c) {
            const float x = g.xs[static_cast<size_t>(c)];
            if ((w.x0 < x - 1 && w.x1 > x + 1) || (joined && before->x0 + before->x1 < 2 * x && w.x0 + w.x1 > 2 * x)) return 0;
        }
        const int c = static_cast<int>(std::upper_bound(g.xs.begin(), g.xs.end(), (w.x0 + w.x1) / 2) - g.xs.begin()) - 1;
        mask |= uint64_t(1) << std::min(std::max(c, 0), 63);
    }
    return mask;
}

bool Detector::WordOfSize(const Row& r, const std::vector<uint32_t>& rowWords, float em) const {
    for (uint32_t k = r.first; k < r.first + r.count; ++k) {
        const TWord& w = words_[rowWords[k]];
        if (!w.mono && std::fabs(w.size - em) <= 0.15f * em) return true;
    }
    return false;
}

// Rows set right below a lattice without rules of their own (the last rows of a table whose
// rules stop short): each line within an em of the grid's bottom that lies in its columns
// (LineColumns), filling half of them and two at least, with nothing else on that line, extends it.
void Detector::RowsBelow(Grid& g) {
    std::vector<Row> rows;
    std::vector<uint32_t> rowWords;
    const int cols = g.Cols();
    BuildRows(-INFINITY, INFINITY, g.y0 - 40 * bodySize_, g.y0, rows, rowWords);
    bool added = false;
    for (const Row& r : rows) {
        if (g.Rows() >= kMaxTableRows || !(r.top < g.y0 + 1) || g.y0 - r.top > r.size || !SizeFits(r, g.size)) break;
        const int filled = Bits(LineColumns(r, rowWords, g));
        if (filled < 2 || 2 * filled < cols || !Free(g.x0, r.bottom - 1, g.x1, g.y0)) break;
        g.ys.push_back(r.bottom - 1);
        g.vb.insert(g.vb.end(), static_cast<size_t>(cols + 1), 1);
        g.hb.insert(g.hb.end(), static_cast<size_t>(cols), 1);
        g.y0 = g.ys.back();
        added = true;
    }
    if (added) ComputeSpans(g);
}

// A header row set right above a lattice (rules drawn around the body only): the line of text
// within an em of its top that lies in its columns (LineColumns), filling half of them and two at
// least, with nothing else on that line, becomes its first row. True when it did.
bool Detector::HeaderAbove(Grid& g) {
    std::vector<Row> rows;
    std::vector<uint32_t> rowWords;
    BuildRows(-INFINITY, INFINITY, g.y1, g.y1 + 4 * bodySize_, rows, rowWords);
    if (rows.empty()) return false;
    const Row& r = rows.back();
    if (!(r.base > g.y1) || r.bottom - g.y1 > r.size || !Free(g.x0, g.y1, g.x1, r.top + 1)) return false;
    const int cols = g.Cols();
    const int filled = Bits(LineColumns(r, rowWords, g));
    if (filled < 2 || 2 * filled < cols) return false;
    g.ys.insert(g.ys.begin(), r.top + 1);
    g.vb.insert(g.vb.begin(), static_cast<size_t>(cols + 1), 1);
    g.hb.insert(g.hb.begin(), static_cast<size_t>(cols), 1);
    g.y1 = g.ys.front();
    ComputeSpans(g);
    return true;
}

// The rule over a lattice's label: a drawn rule of the grid's width less than three lines above
// its top, with one line of text between that does not part into columns (a phrase). NaN when
// there is none.
float Detector::LabelAbove(const Grid& g) {
    float rule = NAN, nearest = 2.5f * bodySize_;
    for (const Edge& e : h_) {
        if (e.area || std::fabs(e.a - g.x0) > 2.5f || std::fabs(e.b - g.x1) > 2.5f) continue;
        const float d = e.pos - g.y1;
        if (d > 0.5f * bodySize_ && d <= nearest) rule = e.pos, nearest = d;
    }
    if (std::isnan(rule)) return NAN;
    std::vector<Row> rows;
    std::vector<uint32_t> rowWords;
    std::vector<Gap> gaps;
    BuildRows(g.x0, g.x1, g.y1, rule, rows, rowWords);
    if (rows.size() != 1 || Columns(rows, rowWords, 0, 1, g.x0, g.x1, Fmax(3.0f, 0.5f * TextSize(rows[0])), gaps)) return NAN;
    return rule;
}

// Cells drawn as separate filled boxes with white gutters between them (a slide's table): no edge
// of one box touches another's, so no lattice forms. Boxes at least two body sizes wide and 0.8
// high, each holding a word's centre and no other box's (a panel behind them is no cell): filled
// rectangles, or filled straight-sided shapes of four sides at most whose top and bottom are edges
// of the page (a box drawn as lines). Those that line up in rows (y-ranges overlapping by half the
// lower box: a row's boxes fit their own text's height) and columns (x-ranges overlapping by 80 %
// of the wider box), three rows and two columns at least with a box in 60 % of the slots, rows and
// columns apart by gutters of 24 points at most, are a grid: its outer box and a rule on each
// gutter's midline replace the boxes' own sides, and Lattices builds the table. Not when a word
// inside the grid lies in a gutter (shading on every other row of a table: the rows between are
// cells too), when the grid is part of a ruled table (shaded header cells: a rule through it runs
// on past it), or when more stroked or curved shapes lie inside it than it has boxes (a figure's
// panels, each a plot under its title).
void Detector::BoxGrids() {
    constexpr size_t kMaxBoxes = 1024;
    constexpr float kMaxGutter = 24.0f;
    struct Box {
        float x0, y0, x1, y1;
    };
    std::vector<Box> boxes;
    const auto edgeAt = [&](float y, float x0, float x1) {
        auto it = std::lower_bound(h_.begin(), h_.end(), y - kSnap, [](const Edge& e, float p) { return e.pos < p; });
        for (; it != h_.end() && it->pos <= y + kSnap; ++it)
            if (it->a <= x0 + kSnap && it->b >= x1 - kSnap) return true;
        return false;
    };
    for (const Shape& s : page_.shapes) {
        if (!s.filled || s.curved || (s.kind != ShapeKind::Rect && (s.kind != ShapeKind::Other || s.segments > 4))) continue;
        const Ruling r = ToFrame(Ruling{s.x0, s.y0, s.x1, s.y1, 0}, dir_);
        const float w = r.x1 - r.x0, h = r.y1 - r.y0;
        if (!(w >= 2 * bodySize_ && h >= 0.8f * bodySize_ && std::isfinite(w) && std::isfinite(h))) continue;
        if (s.kind != ShapeKind::Rect && !(edgeAt(r.y0, r.x0, r.x1) && edgeAt(r.y1, r.x0, r.x1))) continue;
        bool holds = false;
        auto it = std::lower_bound(words_.begin(), words_.end(), r.y1 + 20, [](const TWord& q, float y) { return q.base > y; });
        for (; it != words_.end() && it->base >= r.y0 - 20 && !holds; ++it) {
            const float cx = (it->x0 + it->x1) / 2, cy = (it->y0 + it->y1) / 2;
            holds = cx >= r.x0 && cx <= r.x1 && cy >= r.y0 && cy <= r.y1;
        }
        if (!holds) continue;
        if (boxes.size() >= kMaxBoxes) return;  // fail closed
        boxes.push_back(Box{r.x0, r.y0, r.x1, r.y1});
    }
    const size_t n = boxes.size();
    if (n < 6) return;
    const auto area = [&](size_t i) { return (boxes[i].x1 - boxes[i].x0) * (boxes[i].y1 - boxes[i].y0); };
    // Box i gives way to a smaller box whose centre it holds, and to an earlier box at its place
    // (each side within half a point). The boxes that may make it give way are looked up by their
    // centres (those in i's x-range) and by their left edges (those within a point of i's: a float
    // difference of half a point at most is a real one under a point, and rounding keeps the
    // order), not by trying every other box.
    const auto yields = [&](size_t i, size_t j) {
        const Box &a = boxes[i], &b = boxes[j];
        const float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
        const bool same = std::fabs(a.x0 - b.x0) <= 0.5f && std::fabs(a.x1 - b.x1) <= 0.5f && std::fabs(a.y0 - b.y0) <= 0.5f &&
                          std::fabs(a.y1 - b.y1) <= 0.5f;
        return same ? j < i : cx >= a.x0 && cx <= a.x1 && cy >= a.y0 && cy <= a.y1 && area(i) > area(j);
    };
    std::vector<float> centre(n);
    std::vector<uint32_t> byCentre(n), byLeft(n);
    for (size_t i = 0; i < n; ++i) {
        centre[i] = (boxes[i].x0 + boxes[i].x1) / 2;
        byCentre[i] = byLeft[i] = static_cast<uint32_t>(i);
    }
    std::sort(byCentre.begin(), byCentre.end(), [&](uint32_t p, uint32_t q) { return centre[p] < centre[q]; });
    std::sort(byLeft.begin(), byLeft.end(), [&](uint32_t p, uint32_t q) { return boxes[p].x0 < boxes[q].x0; });
    std::vector<uint8_t> keep(n, 1);
    for (size_t i = 0; i < n; ++i) {
        const Box& a = boxes[i];
        auto c = std::lower_bound(byCentre.begin(), byCentre.end(), a.x0, [&](uint32_t j, float x) { return centre[j] < x; });
        for (; c != byCentre.end() && centre[*c] <= a.x1 && keep[i]; ++c)
            if (*c != i && yields(i, *c)) keep[i] = 0;
        const float from = a.x0 - 1.0f, to = a.x0 + 1.0f;
        auto l = std::lower_bound(byLeft.begin(), byLeft.end(), from, [&](uint32_t j, float x) { return boxes[j].x0 < x; });
        for (; l != byLeft.end() && boxes[*l].x0 <= to && keep[i]; ++l)
            if (*l != i && yields(i, *l)) keep[i] = 0;
    }
    std::vector<int32_t> rowOf(n), colOf(n), comp(n);
    for (size_t i = 0; i < n; ++i) rowOf[i] = colOf[i] = comp[i] = static_cast<int32_t>(i);
    const auto join = [](std::vector<int32_t>& parent, size_t i, size_t j) {
        const int32_t a = Find(parent, static_cast<int32_t>(i)), b = Find(parent, static_cast<int32_t>(j));
        if (a != b) parent[static_cast<size_t>(a)] = b;
    };
    // Rows and columns: boxes whose y-ranges (x-ranges) overlap enough, found by a sweep over the
    // kept boxes in order of their lower (left) edges, each paired with those starting inside it
    // (an overlap is a positive length). Only the sets matter here, not which box roots them.
    std::vector<uint32_t> sweep;
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) sweep.push_back(static_cast<uint32_t>(i));
    std::sort(sweep.begin(), sweep.end(), [&](uint32_t p, uint32_t q) { return boxes[p].y0 < boxes[q].y0; });
    for (size_t k = 0; k < sweep.size(); ++k)
        for (size_t m = k + 1; m < sweep.size() && boxes[sweep[m]].y0 <= boxes[sweep[k]].y1; ++m) {
            const Box &a = boxes[sweep[k]], &b = boxes[sweep[m]];
            if (Fmin(a.y1, b.y1) - Fmax(a.y0, b.y0) >= 0.5f * Fmin(a.y1 - a.y0, b.y1 - b.y0)) join(rowOf, sweep[k], sweep[m]);
        }
    std::sort(sweep.begin(), sweep.end(), [&](uint32_t p, uint32_t q) { return boxes[p].x0 < boxes[q].x0; });
    for (size_t k = 0; k < sweep.size(); ++k)
        for (size_t m = k + 1; m < sweep.size() && boxes[sweep[m]].x0 <= boxes[sweep[k]].x1; ++m) {
            const Box &a = boxes[sweep[k]], &b = boxes[sweep[m]];
            if (Fmin(a.x1, b.x1) - Fmax(a.x0, b.x0) >= 0.8f * Fmax(a.x1 - a.x0, b.x1 - b.x0)) join(colOf, sweep[k], sweep[m]);
        }
    // Groups: box i joins each later box of its row or its column, in the order of their indices
    // (the order fixes which box roots a group, and so the order the groups are tried in). The
    // members of row root r, in order of their indices: rowMembers[rowFrom[r], rowFrom[r + 1]);
    // rowAt[i] is box i's place there (columns likewise).
    for (size_t i = 0; i < n; ++i) {
        rowOf[i] = Find(rowOf, static_cast<int32_t>(i));  // each box points at its root from here on
        colOf[i] = Find(colOf, static_cast<int32_t>(i));
    }
    std::vector<uint32_t> rowFrom(n + 1, 0), colFrom(n + 1, 0), rowMembers(n), colMembers(n), rowAt(n), colAt(n);
    for (size_t i = 0; i < n; ++i)
        if (keep[i]) rowFrom[static_cast<size_t>(rowOf[i]) + 1]++, colFrom[static_cast<size_t>(colOf[i]) + 1]++;
    for (size_t i = 0; i < n; ++i) rowFrom[i + 1] += rowFrom[i], colFrom[i + 1] += colFrom[i];
    {
        std::vector<uint32_t> rowFill(rowFrom.begin(), rowFrom.end() - 1), colFill(colFrom.begin(), colFrom.end() - 1);
        for (size_t i = 0; i < n; ++i)
            if (keep[i]) {
                const size_t r = static_cast<size_t>(rowOf[i]), k = static_cast<size_t>(colOf[i]);
                rowAt[i] = rowFill[r], rowMembers[rowFill[r]++] = static_cast<uint32_t>(i);
                colAt[i] = colFill[k], colMembers[colFill[k]++] = static_cast<uint32_t>(i);
            }
    }
    for (size_t i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        uint32_t p = rowAt[i] + 1, q = colAt[i] + 1;
        const uint32_t pEnd = rowFrom[static_cast<size_t>(rowOf[i]) + 1], qEnd = colFrom[static_cast<size_t>(colOf[i]) + 1];
        while (p < pEnd || q < qEnd) {
            const uint32_t a = p < pEnd ? rowMembers[p] : UINT32_MAX, b = q < qEnd ? colMembers[q] : UINT32_MAX;
            const uint32_t j = a < b ? a : b;
            p += a == j, q += b == j;
            join(comp, i, j);
        }
    }
    // A group of fewer than four boxes fills fewer than 60 % of the six slots of three rows by two
    // columns.
    std::vector<uint32_t> groupSize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        comp[i] = Find(comp, static_cast<int32_t>(i));
        groupSize[static_cast<size_t>(comp[i])] += keep[i];
    }
    struct Band {
        int32_t root;
        float a, b;  // the extent of its boxes
    };
    std::vector<Band> rows, cols;
    std::vector<std::pair<int32_t, int32_t>> slots;
    bool added = false;
    for (size_t c = 0; c < n; ++c) {
        if (!keep[c] || comp[c] != static_cast<int32_t>(c) || groupSize[c] < 4) continue;
        rows.clear(), cols.clear(), slots.clear();
        const auto extend = [](std::vector<Band>& bands, int32_t root, float a, float b) {
            for (Band& band : bands)
                if (band.root == root) {
                    band.a = Fmin(band.a, a), band.b = Fmax(band.b, b);
                    return;
                }
            bands.push_back(Band{root, a, b});
        };
        for (size_t i = 0; i < n; ++i) {
            if (!keep[i] || comp[i] != static_cast<int32_t>(c)) continue;
            const int32_t r = rowOf[i], k = colOf[i];
            extend(rows, r, boxes[i].y0, boxes[i].y1);
            extend(cols, k, boxes[i].x0, boxes[i].x1);
            if (std::find(slots.begin(), slots.end(), std::make_pair(r, k)) == slots.end()) slots.emplace_back(r, k);
        }
        if (rows.size() < 3 || cols.size() < 2 || float(slots.size()) < 0.6f * float(rows.size() * cols.size())) continue;
        std::sort(rows.begin(), rows.end(), [](const Band& p, const Band& q) { return p.b > q.b; });
        std::sort(cols.begin(), cols.end(), [](const Band& p, const Band& q) { return p.a < q.a; });
        bool gutters = true;
        for (size_t k = 0; k + 1 < rows.size() && gutters; ++k) {
            const float gap = rows[k].a - rows[k + 1].b;
            gutters = gap >= 0.5f && gap <= kMaxGutter;
        }
        for (size_t k = 0; k + 1 < cols.size() && gutters; ++k) {
            const float gap = cols[k + 1].a - cols[k].b;
            gutters = gap >= 0.5f && gap <= kMaxGutter;
        }
        if (!gutters) continue;
        const float left = cols.front().a, right = cols.back().b, top = rows.front().b, bottom = rows.back().a;
        bool inSlots = true;  // every word inside the grid lies in a slot, none in a gutter
        auto it = std::lower_bound(words_.begin(), words_.end(), top + 20, [](const TWord& q, float y) { return q.base > y; });
        for (; it != words_.end() && it->base >= bottom - 20 && inSlots; ++it) {
            const float cx = (it->x0 + it->x1) / 2, cy = (it->y0 + it->y1) / 2;
            if (!(cx >= left && cx <= right && cy >= bottom && cy <= top)) continue;
            inSlots = std::any_of(rows.begin(), rows.end(), [&](const Band& b) { return cy >= b.a && cy <= b.b; }) &&
                      std::any_of(cols.begin(), cols.end(), [&](const Band& b) { return cx >= b.a && cx <= b.b; });
        }
        if (!inSlots) continue;
        // A rule through the grid and on past it, a row's rule on past both sides or a column's past
        // either end: the grid is part of a ruled table (a band behind a row may jut out on one side).
        const auto through = [&](const Edge& e, float lo, float hi, float from, float to, bool both) {
            if (!(e.pos > lo + kSnap && e.pos < hi - kSnap && e.a < to && e.b > from)) return false;
            const bool before = e.a < from - 2 * kSnap, after = e.b > to + 2 * kSnap;
            return both ? before && after : before || after;
        };
        if (std::any_of(h_.begin(), h_.end(), [&](const Edge& e) { return through(e, bottom, top, left, right, true); }) ||
            std::any_of(v_.begin(), v_.end(), [&](const Edge& e) { return through(e, left, right, bottom, top, false); }))
            continue;
        const size_t boxCount = groupSize[c];  // the grid's boxes
        size_t drawn = 0;                      // the shapes drawn inside it
        for (const Shape& s : page_.shapes) {
            if (s.filled && !s.curved) continue;
            const Ruling r = ToFrame(Ruling{s.x0, s.y0, s.x1, s.y1, 0}, dir_);
            const float cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
            drawn += cx > left && cx < right && cy > bottom && cy < top;
        }
        if (drawn > boxCount) continue;
        // Every edge inside the grid or across it (its boxes' sides, a band drawn behind a row) gives
        // way to the grid's own.
        const auto across = [&](const Edge& e, bool horizontal) {
            const float lo = horizontal ? bottom : left, hi = horizontal ? top : right;
            const float from = horizontal ? left : bottom, to = horizontal ? right : top;
            return e.pos >= lo - kSnap && e.pos <= hi + kSnap && e.a < to && e.b > from;
        };
        h_.erase(std::remove_if(h_.begin(), h_.end(), [&](const Edge& e) { return across(e, true); }), h_.end());
        v_.erase(std::remove_if(v_.begin(), v_.end(), [&](const Edge& e) { return across(e, false); }), v_.end());
        h_.push_back(Edge{top, left, right});
        h_.push_back(Edge{bottom, left, right});
        for (size_t k = 0; k + 1 < rows.size(); ++k) h_.push_back(Edge{(rows[k].a + rows[k + 1].b) / 2, left, right});
        v_.push_back(Edge{left, bottom, top});
        v_.push_back(Edge{right, bottom, top});
        for (size_t k = 0; k + 1 < cols.size(); ++k) v_.push_back(Edge{(cols[k].b + cols[k + 1].a) / 2, bottom, top});
        added = true;
    }
    if (added) {
        MergeEdges(h_);
        MergeEdges(v_);
    }
}

void Detector::Lattices() {
    if (h_.size() > kMaxTableEdges || v_.size() > kMaxTableEdges) return;
    const size_t nh = h_.size(), nv = v_.size();
    std::vector<int32_t> parent(nh + nv);
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int32_t>(i);
    // v_ is sorted by x: the verticals within a horizontal's reach are a range.
    for (size_t i = 0; i < nh; ++i) {
        const Edge& e = h_[i];
        auto it = std::lower_bound(v_.begin(), v_.end(), e.a - kTouch, [](const Edge& q, float x) { return q.pos < x; });
        for (; it != v_.end() && it->pos <= e.b + kTouch; ++it) {
            if (e.pos < it->a - kTouch || e.pos > it->b + kTouch) continue;
            const int32_t a = Find(parent, static_cast<int32_t>(i));
            const int32_t b = Find(parent, static_cast<int32_t>(nh + static_cast<size_t>(it - v_.begin())));
            if (a != b) parent[static_cast<size_t>(a)] = b;
        }
    }
    // Grids drawn as separate boxes a hair apart (each cell its own rectangle with a white gutter, a
    // band of shaded rows) are one grid: components whose boxes lie within kNear of each other, and
    // overlap along the other axis, join.
    {
        std::vector<int32_t> rootList;
        std::vector<float> bx0(nh + nv, INFINITY), by0(nh + nv, INFINITY), bx1(nh + nv, -INFINITY), by1(nh + nv, -INFINITY);
        for (size_t i = 0; i < nh + nv; ++i) {
            const size_t r = static_cast<size_t>(Find(parent, static_cast<int32_t>(i)));
            const Edge& e = i < nh ? h_[i] : v_[i - nh];
            const float x0 = i < nh ? e.a : e.pos, x1 = i < nh ? e.b : e.pos;
            const float y0 = i < nh ? e.pos : e.a, y1 = i < nh ? e.pos : e.b;
            if (bx0[r] == INFINITY) rootList.push_back(static_cast<int32_t>(r));
            bx0[r] = Fmin(bx0[r], x0), bx1[r] = Fmax(bx1[r], x1);
            by0[r] = Fmin(by0[r], y0), by1[r] = Fmax(by1[r], y1);
        }
        constexpr float kNear = 6.0f;
        if (rootList.size() <= 512) {
            for (size_t a = 0; a < rootList.size(); ++a)
                for (size_t b = a + 1; b < rootList.size(); ++b) {
                    const size_t p = static_cast<size_t>(rootList[a]), q = static_cast<size_t>(rootList[b]);
                    const float gapX = Fmax(bx0[p], bx0[q]) - Fmin(bx1[p], bx1[q]);
                    const float gapY = Fmax(by0[p], by0[q]) - Fmin(by1[p], by1[q]);
                    const float spanX = Fmax(bx1[p] - bx0[p], bx1[q] - bx0[q]);
                    const float spanY = Fmax(by1[p] - by0[p], by1[q] - by0[q]);
                    // Side by side with rows in common, or stacked with columns in common.
                    const bool beside = gapX <= kNear && gapX > -kNear && -gapY >= 0.5f * spanY;
                    const bool stacked = gapY <= kNear && gapY > -kNear && -gapX >= 0.5f * spanX;
                    if (!beside && !stacked) continue;
                    const int32_t ra = Find(parent, rootList[a]), rb = Find(parent, rootList[b]);
                    if (ra != rb) parent[static_cast<size_t>(ra)] = rb;
                }
        }
    }
    std::vector<int32_t> roots;
    for (size_t i = 0; i < nh + nv; ++i) {
        const int32_t r = Find(parent, static_cast<int32_t>(i));
        (i < nh ? h_[i] : v_[i - nh]).comp = r;
        if (r == static_cast<int32_t>(i)) roots.push_back(r);
    }
    // A grid takes two edges of each direction: without them, no component is one.
    if (nh < 2 || nv < 2) roots.clear();
    // Each component's edges, in edge order (a counting sort by root): the loops below over one
    // component's edges visit them as loops over all edges skipping other components did.
    std::vector<uint32_t> hFirst, vFirst, hOf, vOf;
    if (!roots.empty()) {
        hFirst.assign(nh + nv + 1, 0), vFirst.assign(nh + nv + 1, 0), hOf.resize(nh), vOf.resize(nv);
        for (size_t i = 0; i < nh; ++i) hFirst[static_cast<size_t>(h_[i].comp) + 1]++;
        for (size_t i = 0; i < nv; ++i) vFirst[static_cast<size_t>(v_[i].comp) + 1]++;
        for (size_t k = 1; k <= nh + nv; ++k) hFirst[k] += hFirst[k - 1], vFirst[k] += vFirst[k - 1];
        std::vector<uint32_t> hAt(hFirst.begin(), hFirst.end() - 1), vAt(vFirst.begin(), vFirst.end() - 1);
        for (size_t i = 0; i < nh; ++i) hOf[hAt[static_cast<size_t>(h_[i].comp)]++] = static_cast<uint32_t>(i);
        for (size_t i = 0; i < nv; ++i) vOf[vAt[static_cast<size_t>(v_[i].comp)]++] = static_cast<uint32_t>(i);
    }
    std::vector<uint8_t> hUsed(nh, 0);
    std::vector<Edge> pieces;  // SplitRowsByVerticals': the vertical rule pieces as drawn
    bool piecesRead = false;
    for (int32_t root : roots) {
        if (grids_.size() >= static_cast<size_t>(kMaxTablesPerPage)) break;
        const size_t ri = static_cast<size_t>(root);
        const Component comp{hOf.data() + hFirst[ri], hFirst[ri + 1] - hFirst[ri], vOf.data() + vFirst[ri],
                             vFirst[ri + 1] - vFirst[ri]};
        Grid g;
        g.kind = TableKind::Lattice;
        g.x0 = INFINITY, g.y0 = INFINITY, g.x1 = -INFINITY, g.y1 = -INFINITY;
        const int hs = static_cast<int>(comp.hCount), vs = static_cast<int>(comp.vCount);
        if (hs < 2 || vs < 2) continue;
        bool drawn = false;  // drawn vertical rules and a drawn top rule, not only sides of filled areas
        for (size_t m = 0; m < comp.hCount; ++m) {
            const Edge& e = h_[comp.h[m]];
            g.ys.push_back(e.pos);
            g.x0 = Fmin(g.x0, e.a), g.x1 = Fmax(g.x1, e.b);
            g.y0 = Fmin(g.y0, e.pos), g.y1 = Fmax(g.y1, e.pos);
        }
        for (size_t m = 0; m < comp.vCount; ++m) {
            const Edge& e = v_[comp.v[m]];
            drawn = drawn || !e.area;
            g.xs.push_back(e.pos);
            g.x0 = Fmin(g.x0, e.pos), g.x1 = Fmax(g.x1, e.pos);
            g.y0 = Fmin(g.y0, e.a), g.y1 = Fmax(g.y1, e.b);
        }
        bool drawnTop = false;
        for (size_t m = 0; m < comp.hCount; ++m) {
            const Edge& e = h_[comp.h[m]];
            drawnTop = drawnTop || (!e.area && e.pos >= g.y1 - kSnap);
        }
        drawn = drawn && drawnTop;
        g.xs.push_back(g.x0), g.xs.push_back(g.x1);
        g.ys.push_back(g.y0), g.ys.push_back(g.y1);
        Cluster(g.xs);
        Cluster(g.ys);
        std::reverse(g.ys.begin(), g.ys.end());
        CollapseEmpty(g);
        if (g.Rows() < 1 || g.Cols() < 1 || g.Rows() > kMaxTableRows || g.Cols() > kMaxTableCols) continue;
        if (!Free(g.x0, g.y0, g.x1, g.y1)) continue;
        Boundaries(g, comp);
        // A horizontal boundary drawn across fewer than half the columns (the bottom of a short
        // header cell, a line under one value) parts no rows: it goes, and the rows join.
        {
            const int cols = g.Cols();
            bool pruned = false;
            for (int r = g.Rows() - 1; r >= 1; --r) {
                int present = 0;
                for (int c = 0; c < cols; ++c) present += g.hb[static_cast<size_t>(r * cols + c)];
                if (2 * present < cols) {
                    g.ys.erase(g.ys.begin() + r);
                    pruned = true;
                }
            }
            if (pruned) Boundaries(g, comp);
        }
        SplitRowsByVerticals(g, pieces, piecesRead);
        SplitByText(g);
        SplitRowsByLines(g);
        SplitColumnsByText(g);
        const int rows = g.Rows(), cols = g.Cols();
        ComputeSpans(g);
        // Rows that are one cell across, in most of the grid: verticals only frame the table (or
        // a header): its columns come from whitespace instead.
        int wholeRows = 0;
        for (const Span& s : g.cells)
            if (s.cs == cols) wholeRows += s.rs;
        // One column of boxed rows with room around their text: a list set as a table. Rows no
        // taller than a line of text are the shaded lines of a code block or a note.
        bool column = cols == 1 && rows >= 3;
        for (int r = 0; r <= rows && column; ++r) column = g.hb[static_cast<size_t>(r)] != 0;
        for (int r = 0; r < rows && column; ++r) column = g.vb[static_cast<size_t>(r * 2)] && g.vb[static_cast<size_t>(r * 2 + 1)];
        for (int r = 0; r < rows && column; ++r) column = g.ys[static_cast<size_t>(r)] - g.ys[static_cast<size_t>(r + 1)] >= 1.5f * bodySize_;
        if (column) {
            // Columns of whitespace through every row: a table ruled across only (its columns come
            // from the rule stacks), not a list of one column.
            std::vector<Row> textRows;
            std::vector<uint32_t> textWords;
            std::vector<Gap> gaps;
            BuildRows(g.x0, g.x1, g.y0, g.y1, textRows, textWords);
            if (Columns(textRows, textWords, 0, textRows.size(), g.x0, g.x1, Fmax(3.0f, 0.8f * bodySize_), gaps))
                column = false;
        }
        // A box drawn around the body rows alone: the header row stands right above it (a shaded
        // row of a striped table, its sides those of a filled area, is no such box).
        if (!column && drawn && cols >= 2) HeaderAbove(g);
        if (!column && (cols < 2 || g.Rows() < 2 || wholeRows * 2 > g.Rows())) continue;
        bool figure = false;
        // A grid drawn inside a figure region (the boxes of a diagram or a legend; nine tenths of
        // its area at least, titles of the panels above it aside), or inside regions side by side
        // that each span its height (the bars of a chart found as several drawings), is part of the
        // figure.
        bool drawing = false;
        {
            const std::vector<Ruling>& regions = Figures();
            const float near = 2 * bodySize_;
            float reach = g.x0;  // the grid is covered from its left side to here
            bool grown = true;
            for (size_t pass = 0; grown && pass < regions.size(); ++pass) {
                grown = false;
                for (const Ruling& f : regions)
                    if (g.y0 >= f.y0 - near && g.y1 <= f.y1 + near && f.x0 - near <= reach && f.x1 + near > reach)
                        reach = f.x1 + near, grown = true;
            }
            drawing = reach >= g.x1;
            const float area = (g.x1 - g.x0) * (g.y1 - g.y0);
            for (const Ruling& f : regions) {
                const float w = Fmin(g.x1, f.x1 + near) - Fmax(g.x0, f.x0 - near), h = Fmin(g.y1, f.y1 + near) - Fmax(g.y0, f.y0 - near);
                drawing = drawing || (w > 0 && h > 0 && w * h >= 0.9f * area);
            }
        }
        if (drawing || !Validate(g, column, &figure)) {
            // A chart's gridlines or a diagram's boxes: their lines are no rules of a table either.
            for (size_t m = 0; m < comp.hCount && (figure || drawing); ++m) hUsed[comp.h[m]] = 1;
            continue;
        }
        LatticeHeader(g);
        RowsBelow(g);
        for (size_t m = 0; m < comp.hCount; ++m) hUsed[comp.h[m]] = 1;
        MarkUsed(g);
        grids_.push_back(std::move(g));
    }
    for (size_t i = 0; i < nh; ++i)
        if (!hUsed[i] && h_[i].b - h_[i].a >= kMinRule) rules_.push_back(h_[i]);
    JoinStripes();
}

// Tables shaded on every other row draw boxes around the shaded rows only: lattices with the same
// columns stacked a row apart, with a row of text between them that keeps their columns, or
// touching with nothing between (that row taken as a row below the upper one), are one table.
// Tables in sections, each boxed under its label (a line across the columns, set between double
// rules), are one too: a line between two lattices that does not part into columns is a label row,
// and so is the label under a rule of the table's width over the first section.
void Detector::JoinStripes() {
    std::vector<Row> rows;
    std::vector<uint32_t> rowWords;
    std::vector<Gap> gaps;
    for (size_t a = 0; a < grids_.size(); ++a) {
        if (grids_[a].kind != TableKind::Lattice) continue;
        for (size_t b = 0; b < grids_.size(); ++b) {
            if (b == a || grids_[b].kind != TableKind::Lattice) continue;
            Grid& up = grids_[a];
            const Grid& down = grids_[b];
            if (up.xs.size() != down.xs.size() || up.xs.size() < 3) continue;
            bool same = true;
            for (size_t k = 0; k < up.xs.size() && same; ++k) same = std::fabs(up.xs[k] - down.xs[k]) <= 3;
            const float gapTop = up.y0, gapBottom = down.y1;
            if (!same || gapBottom >= gapTop || gapTop - gapBottom > 2.5f * bodySize_) continue;
            // The row between: one line that does not part into columns (a label), or words inside
            // one column each, two columns filled.
            BuildRows(up.xs.front(), up.xs.back(), gapBottom, gapTop, rows, rowWords);
            const bool label = rows.size() == 1 && !Columns(rows, rowWords, 0, 1, up.xs.front(), up.xs.back(),
                                                            Fmax(3.0f, 0.5f * TextSize(rows[0])), gaps);
            uint64_t columns = 0;
            bool crossing = false;
            auto it = std::lower_bound(words_.begin(), words_.end(), gapTop, [](const TWord& w, float y) { return w.base > y; });
            for (; it != words_.end() && it->base >= gapBottom && !label; ++it) {
                const float cx = (it->x0 + it->x1) / 2;
                if (cx < up.xs.front() || cx > up.xs.back()) continue;
                for (size_t k = 1; k + 1 < up.xs.size(); ++k) crossing = crossing || (it->x0 < up.xs[k] - 1 && it->x1 > up.xs[k] + 1);
                const size_t c = static_cast<size_t>(std::upper_bound(up.xs.begin(), up.xs.end(), cx) - up.xs.begin()) - 1;
                columns |= uint64_t(1) << std::min<size_t>(c, 63);
            }
            int filled = 0;
            for (uint64_t m = columns; m; m &= m - 1) filled++;
            // No text between and less than half a line apart: the stripes touch (the rows between
            // went to the upper one as rows below it), and join with no row between.
            const bool touching = rows.empty() && gapTop - gapBottom <= 0.5f * bodySize_;
            if ((!label && !touching && (crossing || filled < 2)) || up.Rows() + down.Rows() + 1 > kMaxTableRows) continue;
            const int cols = up.Cols();
            Grid joined;
            joined.kind = TableKind::Lattice;
            joined.xs = up.xs;
            joined.ys = up.ys;
            if (touching) joined.ys.pop_back();
            joined.ys.insert(joined.ys.end(), down.ys.begin(), down.ys.end());
            joined.vb = up.vb;
            if (!touching) joined.vb.insert(joined.vb.end(), static_cast<size_t>(cols + 1), 1);  // the row between
            joined.vb.insert(joined.vb.end(), down.vb.begin(), down.vb.end());
            joined.hb = up.hb;
            if (touching) joined.hb.resize(joined.hb.size() - static_cast<size_t>(cols));
            joined.hb.insert(joined.hb.end(), down.hb.begin(), down.hb.end());
            joined.x0 = Fmin(up.x0, down.x0), joined.x1 = Fmax(up.x1, down.x1);
            joined.y0 = down.y0, joined.y1 = up.y1;
            joined.size = up.size;
            joined.headerRows = up.headerRows;
            if (label || !up.labelRow.empty() || !down.labelRow.empty()) {
                joined.labelRow = up.labelRow;
                joined.labelRow.resize(static_cast<size_t>(up.Rows()), 0);
                if (!touching) joined.labelRow.push_back(label ? 1 : 0);
                joined.labelRow.insert(joined.labelRow.end(), down.labelRow.begin(), down.labelRow.end());
                joined.labelRow.resize(static_cast<size_t>(joined.Rows()), 0);
            }
            ComputeSpans(joined);
            if (!Validate(joined, false)) continue;
            MarkUsed(joined);
            grids_[a] = std::move(joined);
            grids_.erase(grids_.begin() + static_cast<std::ptrdiff_t>(b));
            if (b < a) --a;
            b = static_cast<size_t>(-1);  // look again: a third stripe may follow
        }
    }
    // The label over the first section: the table's first row.
    for (Grid& g : grids_) {
        if (g.kind != TableKind::Lattice || g.labelRow.empty() || g.Rows() >= kMaxTableRows) continue;
        const float rule = LabelAbove(g);
        if (std::isnan(rule) || !Free(g.x0, g.y1, g.x1, rule)) continue;
        const int cols = g.Cols();
        g.ys.insert(g.ys.begin(), rule - 0.5f);
        g.vb.insert(g.vb.begin(), static_cast<size_t>(cols + 1), 1);
        g.hb.insert(g.hb.begin(), static_cast<size_t>(cols), 1);
        g.labelRow.insert(g.labelRow.begin(), 1);
        g.y1 = g.ys.front();
        g.headerRows = 1;
        ComputeSpans(g);
        MarkUsed(g);
    }
}

// Grid columns and rows no word falls in that are narrower than the text (the space between double
// rulings, a cell background's edge beside a ruling) separate nothing: their boundary goes, and
// they join a neighbour. An empty first or last row (a ruling running on past the table's last
// rule) holds nothing at all: it goes.
void Detector::CollapseEmpty(Grid& g) const {
    if (g.xs.size() < 3 && g.ys.size() < 3) return;
    std::vector<uint8_t> colInk(g.xs.size(), 0), rowInk(g.ys.size(), 0);
    auto it = std::lower_bound(words_.begin(), words_.end(), g.y1 + 20, [](const TWord& w, float y) { return w.base > y; });
    for (; it != words_.end() && it->base >= g.y0 - 20; ++it) {
        const float cx = (it->x0 + it->x1) / 2, cy = (it->y0 + it->y1) / 2;
        if (cx < g.xs.front() || cx > g.xs.back() || cy > g.ys.front() || cy < g.ys.back()) continue;
        const size_t c = static_cast<size_t>(std::upper_bound(g.xs.begin(), g.xs.end(), cx) - g.xs.begin());
        const size_t r = static_cast<size_t>(std::upper_bound(g.ys.begin(), g.ys.end(), cy, std::greater<float>()) - g.ys.begin());
        colInk[std::min(std::max<size_t>(c, 1), g.xs.size() - 1) - 1] = 1;
        rowInk[std::min(std::max<size_t>(r, 1), g.ys.size() - 1) - 1] = 1;
    }
    if (g.ys.size() > 2 && !rowInk[0]) {
        g.ys.erase(g.ys.begin());
        rowInk.erase(rowInk.begin());
    }
    if (g.ys.size() > 2 && !rowInk[g.ys.size() - 2]) {
        rowInk.erase(rowInk.begin() + static_cast<std::ptrdiff_t>(g.ys.size() - 2));
        g.ys.pop_back();
    }
    g.y1 = g.ys.front(), g.y0 = g.ys.back();
    const float narrow = 1.2f * bodySize_;
    // Positions removed from the end backwards keep the indices of the rest.
    for (size_t c = g.xs.size() - 1; c-- > 0 && g.xs.size() > 2;)
        if (!colInk[c] && g.xs[c + 1] - g.xs[c] < narrow) g.xs.erase(g.xs.begin() + static_cast<std::ptrdiff_t>(c + 1 < g.xs.size() - 1 ? c + 1 : c));
    for (size_t r = g.ys.size() - 1; r-- > 0 && g.ys.size() > 2;)
        if (!rowInk[r] && g.ys[r] - g.ys[r + 1] < narrow) g.ys.erase(g.ys.begin() + static_cast<std::ptrdiff_t>(r + 1 < g.ys.size() - 1 ? r + 1 : r));
}

// A lattice holds a table when text fills enough of its cells and does not run across its rulings
// (charts and page frames do not qualify).
bool Detector::Validate(Grid& g, bool column, bool* figure) {
    const auto drawing = [&]() {
        if (figure) *figure = true;
        return false;
    };
    const int cols = g.Cols();
    std::vector<uint32_t> filled(g.cells.size(), 0);  // words per cell
    std::vector<uint8_t> label(g.cells.size(), 0);    // a word with letters and no number
    uint32_t inside = 0, crossing = 0, mono = 0, lowerStarts = 0;
    std::vector<float> sizes;
    std::vector<uint8_t> started(static_cast<size_t>(g.Rows()), 0);
    std::vector<Progression> numbers(g.cells.size());
    bool axis = false;
    bool numbersOnly = true;  // every word a plain number
    std::string scratch;
    auto it = std::lower_bound(words_.begin(), words_.end(), g.y1, [](const TWord& w, float y) { return w.base > y; });
    Locator locate(g);
    for (; it != words_.end() && it->base >= g.y0; ++it) {
        const TWord& w = *it;
        const float cx = (w.x0 + w.x1) / 2, cy = (w.y0 + w.y1) / 2;
        int r = 0, c = 0;
        if (!locate.Find(cx, cy, r, c)) continue;
        inside++;
        mono += w.mono;
        if (sizes.size() < 1024) sizes.push_back(w.size);
        if (column && !started[static_cast<size_t>(r)]) {
            // The first word of the row (words come top to bottom, left to right).
            started[static_cast<size_t>(r)] = 1;
            const WordChars& t = Chars(w);
            if (t.size > 0 && t.head[0] >= 'a' && t.head[0] <= 'z') lowerStarts++;
        }
        const size_t cell = static_cast<size_t>(g.owner[static_cast<size_t>(r * cols + c)]);
        filled[cell]++;
        float value = 0;
        const WordChars& t = Chars(w);
        if (!Numeric(t)) label[cell] = 1;
        const bool number = t.numberish && ParseNumber(WordText(w, scratch), value);
        if (number) axis = numbers[cell].Add(value) || axis;
        numbersOnly = numbersOnly && number;
        for (int k = 1; k < cols; ++k) {
            const float x = g.xs[static_cast<size_t>(k)];
            if (g.vb[static_cast<size_t>(r * (cols + 1) + k)] && w.x0 < x - 1 && w.x1 > x + 1) crossing++;
        }
    }
    // Code set in a box of shaded lines, or a paragraph wrapped over shaded lines (rows that go on
    // in lowercase), is no table of one column.
    if (column && (2 * mono > inside || 3 * lowerStarts >= static_cast<uint32_t>(g.Rows()))) return false;
    // Four numbers in one cell, evenly stepped (0 0.2 0.4 0.6): a chart's axis inside its frame.
    // In a turned frame, cells of plain numbers alone are the tick labels of a plot's turned axis.
    if (axis || (dir_ != 0 && numbersOnly && inside > 0)) return drawing();
    uint32_t nonEmpty = 0, most = 0;
    for (uint32_t f : filled) nonEmpty += f > 0, most = std::max(most, f);
    if (inside == 0 || nonEmpty < 2) return false;
    if (float(nonEmpty) < 0.3f * float(g.cells.size())) return false;
    if (float(crossing) > 0.1f * float(inside)) return false;
    // Boxes of a page layout (a magazine's frames) hold whole articles; table cells hold a few words.
    if (most > 80 || inside > 25 * nonEmpty) return false;
    // Two rows with two filled cells at least (three filled rows of a one-column table): boxes
    // drawn around a quote or a code block are no table.
    std::vector<uint16_t> perRow(static_cast<size_t>(g.Rows()), 0);
    for (size_t c = 0; c < g.cells.size(); ++c)
        if (filled[c]) perRow[g.cells[c].r]++;
    int fullRows = 0;
    for (uint16_t n : perRow) fullRows += column ? n >= 1 : n >= 2;
    // A form to fill in: a header row with text beyond the first column over three rows labelled in
    // the first column and left empty otherwise, in a grid ruled inside both ways (a box around a
    // quote has no inner rules).
    bool form = false;
    bool headed = false;
    for (size_t c = 0; c < g.cells.size(); ++c) headed = headed || (g.cells[c].r == 0 && g.cells[c].c > 0 && filled[c]);
    if (!column && headed && fullRows <= 1 && g.Rows() >= 4 && cols >= 2) {
        int labelled = 0, ruledRows = 0;
        for (int r = 0; r < g.Rows(); ++r) {
            const size_t first = static_cast<size_t>(g.owner[static_cast<size_t>(r * cols)]);
            labelled += g.cells[first].r == r && label[first] && perRow[static_cast<size_t>(r)] == 1;
            bool inner = false;
            for (int k = 1; k < cols; ++k) inner = inner || g.vb[static_cast<size_t>(r * (cols + 1) + k)];
            ruledRows += inner;
        }
        form = labelled >= 3 && 2 * ruledRows >= g.Rows();
    }
    if (fullRows < (column ? 3 : 2) && !form) return false;
    // A diagram's boxes drawn over a grid (a flowchart, a block diagram, a figure's panels): most
    // cells span others in a short grid or one with a row of a single filled cell (a table with
    // stray rulings keeps two filled cells in every row of many), or most cells are empty and the
    // rest hold a word each, unless the first row and the first column are filled in full (the
    // corner aside) and every row and column holds two filled cells (a sparse table: one value per
    // row, on a diagonal).
    if (!column && g.cells.size() >= 6) {
        size_t spans = 0;
        for (const Span& sp : g.cells) spans += sp.rs > 1 || sp.cs > 1;
        if (2 * spans > g.cells.size() && (g.Rows() < 5 || fullRows < g.Rows())) return drawing();
        if (!form && 2 * nonEmpty < g.cells.size() && 2 * inside <= 3 * nonEmpty) {
            bool edges = fullRows == g.Rows() && g.Rows() >= 3 && cols >= 3;
            std::vector<uint16_t> perCol(static_cast<size_t>(cols), 0);
            for (size_t c = 0; c < g.cells.size() && edges; ++c) {
                const Span& sp = g.cells[c];
                if (filled[c]) perCol[sp.c]++;
                else edges = (sp.r > 0 && sp.c > 0) || sp.r + sp.c == 0;
            }
            for (uint16_t n : perCol) edges = edges && n >= 2;
            if (!edges) return drawing();
        }
    }
    std::nth_element(sizes.begin(), sizes.begin() + static_cast<std::ptrdiff_t>(sizes.size() / 2), sizes.end());
    g.size = sizes[sizes.size() / 2];
    return true;
}

// Partially ruled tables: stacks of horizontal rules of one width; rows between and around them
// that split into the same columns. Rules drawn cell by cell (CSS borders, ConTeXt's table frames)
// meet at the column boundaries: where the pieces of every rule of a stack meet at the same x
// positions (joints), a closed run's columns whose text, set without cell margins, stands only a
// word space apart part there.
void Detector::RuleStacks() {
    if (rules_.empty() || rules_.size() > kMaxTableEdges) return;
    std::sort(rules_.begin(), rules_.end(), [](const Edge& a, const Edge& b) { return a.pos > b.pos; });
    struct Stack {
        float a, b;
        std::vector<float> ys;
        bool drawn;  // a drawn rule among them: the top and bottom of a shaded box alone (a quote, a
                     // note, code) make no table
    };
    std::vector<Stack> stacks;
    for (const Edge& e : rules_) {
        // Rules of one table share their ends (booktabs, CSS borders, word processor borders).
        const float tol = 2.5f;
        Stack* into = nullptr;
        for (Stack& s : stacks)
            if (std::fabs(s.a - e.a) <= tol && std::fabs(s.b - e.b) <= tol) {
                into = &s;
                break;
            }
        if (into) {
            into->ys.push_back(e.pos);
            into->drawn = into->drawn || !e.area;
        } else if (stacks.size() < static_cast<size_t>(kMaxStacks)) {
            stacks.push_back(Stack{e.a, e.b, {e.pos}, !e.area});
        }
    }
    // Wider stacks first: a table's rules before the \cmidrule under one of its header labels.
    std::stable_sort(stacks.begin(), stacks.end(), [](const Stack& p, const Stack& q) { return p.b - p.a > q.b - q.a; });
    std::vector<Row> rows;
    std::vector<uint32_t> rowWords;
    std::vector<Gap> gaps, trial, sectionGaps;
    std::vector<size_t> labelBands;  // the rule above each label band of the run
    std::vector<uint8_t> label;      // per row of the run: it lies in a label band
    std::vector<uint8_t> ruled;      // per row of the run: a rule of the run lies between it and the row above
    std::vector<Edge> partial;       // rules among a table's header lines narrower than it
    std::vector<uint8_t> sections;   // per row of the run: label rows, and lines tried as section labels
    // The stacks' horizontal rule pieces as drawn (before MergeEdges joins them), by height, read
    // for the first stack; per stack, where each rule's pieces meet (by rule, then x), and the
    // joints of the rules in use: where every one of them has pieces meeting, ascending.
    std::vector<Edge> pieces, line;
    bool piecesRead = false;
    struct Meeting {
        float x;
        uint32_t rule;
    };
    std::vector<Meeting> meetings;
    std::vector<float> joints;
    for (const Stack& s : stacks) {
        if (!s.drawn) continue;
        const float xa = s.a - 2, xb = s.b + 2;
        joints.clear();
        if (!piecesRead) {
            piecesRead = true;
            if (page_.rulings.size() <= 8 * size_t(kMaxTableEdges)) {
                pieces.reserve(page_.rulings.size());
                for (const Ruling& upright : page_.rulings) {
                    const Ruling r = ToFrame(upright, dir_);
                    const float w = r.x1 - r.x0, t = r.y1 - r.y0;
                    if (t >= 0 && t <= kMaxThick && w >= kMinEdge && w > t) pieces.push_back(Edge{(r.y0 + r.y1) / 2, r.x0, r.x1});
                }
                std::sort(pieces.begin(), pieces.end(), [](const Edge& p, const Edge& q) { return p.pos < q.pos; });
            }
        }
        meetings.clear();
        for (uint32_t k = 0; k < s.ys.size() && pieces.size() >= 2; ++k) {
            // The rule's pieces by x: a piece's end and the next one's start within a point meet.
            line.clear();
            auto it = std::lower_bound(pieces.begin(), pieces.end(), s.ys[k] - kSnap, [](const Edge& p, float y) { return p.pos < y; });
            for (; it != pieces.end() && it->pos <= s.ys[k] + kSnap && line.size() < 256; ++it)
                if (it->a >= xa - kSnap && it->b <= xb + kSnap) line.push_back(*it);
            std::sort(line.begin(), line.end(), [](const Edge& p, const Edge& q) { return p.a < q.a; });
            for (size_t m = 1; m < line.size() && meetings.size() < 256; ++m)
                if (std::fabs(line[m].a - line[m - 1].b) <= 1.0f) meetings.push_back(Meeting{(line[m - 1].b + line[m].a) / 2, k});
        }
        // The joints of rules [from, to] (two at least): the meetings of the first that every
        // other one has within a point. A rule of the stack outside the table (a page header's
        // rule as wide as the table) has none of them.
        const auto jointsOf = [&](size_t from, size_t to) {
            joints.clear();
            for (size_t m = 0; m < meetings.size() && to > from; ++m) {
                if (meetings[m].rule != from) continue;
                bool all = true;
                for (size_t k = from + 1; k <= to && all; ++k) {
                    bool meets = false;
                    for (const Meeting& o : meetings) meets = meets || (o.rule == k && std::fabs(o.x - meetings[m].x) <= 1.0f);
                    all = meets;
                }
                if (all) joints.push_back(meetings[m].x);
            }
        };
        // Rules cut alike: every rule of the stack has as many meetings as the first, all of them
        // joints of the whole stack (a table's rules drawn cell by cell). Rules cut otherwise
        // (under labels spanning columns, a page head's rule as wide as the table) mark none.
        bool alike = s.ys.size() >= 2 && !meetings.empty();
        if (alike) {
            std::vector<size_t> perRule(s.ys.size(), 0);
            for (const Meeting& m : meetings) perRule[m.rule]++;
            for (size_t k = 1; k < perRule.size() && alike; ++k) alike = perRule[k] == perRule[0];
            jointsOf(0, s.ys.size() - 1);
            alike = alike && joints.size() == perRule[0];
        }
        if (!alike) meetings.clear();
        // Columns over rows [from, to) (from `rowsInk` when it is proper): gaps `gapMin` wide, and
        // narrower ones, a point wide at least, around a joint.
        const auto stackColumns = [&](const Ink* rowsInk, size_t from, size_t to, float gapMin, std::vector<Gap>& out,
                                      const std::vector<uint8_t>* skip) {
            const float least = joints.empty() ? gapMin : Fmin(gapMin, 1.0f);
            if (rowsInk && rowsInk->proper) InkColumns(*rowsInk, xa, xb, least, out);
            else Columns(rows, rowWords, from, to, xa, xb, least, out, skip);
            if (least < gapMin) {
                out.erase(std::remove_if(out.begin(), out.end(),
                                         [&](const Gap& g) {
                                             if (g.b - g.a >= gapMin) return false;
                                             for (float x : joints)
                                                 if (x >= g.a - 0.5f && x <= g.b + 0.5f) return false;
                                             return true;
                                         }),
                          out.end());
            }
            return !out.empty();
        };
        // Bands between consecutive rules: tabular ones (rows that split into columns) form tables.
        size_t i = 0;
        while (i < s.ys.size()) {
            // The longest run of rules from i whose bands are tabular, or thin and empty (double
            // rules); a taller empty band holds something else (a heading out of the rules' width).
            size_t j = i;
            float em = bodySize_;
            // A band of one line that does not split into columns, right below a tabular band (or
            // the run's first band: a title row set between the table's top rules) and above another
            // that keeps its columns, is a label across the table (the name of the rows that follow
            // it): the run goes on through it. Two such bands on end are the title of another table.
            labelBands.clear();
            size_t tabularEnd = i;  // the rule under the last tabular band
            bool labelled = false;  // the last band with text was a label
            while (j + 1 < s.ys.size()) {
                BuildRows(xa, xb, s.ys[j + 1], s.ys[j], rows, rowWords);
                if (rows.empty()) {
                    if (s.ys[j] - s.ys[j + 1] > 2 * em) break;
                } else {
                    // Rows of one size, evenly set: a heading or a paragraph gap inside means the
                    // band is no table body. A row with words of that size among smaller ones (a
                    // label of subscripts, in small capitals) is of that size too; when the first
                    // row's prose is smaller than its largest words, the band may be of their size.
                    const auto evenAt = [&](float size) {
                        for (size_t q = 1; q < rows.size(); ++q)
                            if (!((SizeFits(rows[q], size) || (SizeFits(rows[q - 1], size) && SubLine(rows[q - 1], rows[q])) ||
                                   WordOfSize(rows[q], rowWords, size)) &&
                                  rows[q - 1].bottom - rows[q].top < 1.6f * size))
                                return false;
                        return true;
                    };
                    em = TextSize(rows[0]);
                    bool even = evenAt(em);
                    if (!even && rows[0].size > 1.15f * em && evenAt(rows[0].size)) em = rows[0].size, even = true;
                    if (!even) break;
                    jointsOf(j, j + 1);
                    if (stackColumns(nullptr, 0, rows.size(), Fmax(3.0f, 0.5f * em), gaps, nullptr)) {
                        tabularEnd = j + 1;
                        labelled = false;
                    } else if (rows.size() == 1 && (tabularEnd > i || j == i) && !labelled && s.ys[j] - s.ys[j + 1] <= 2.5f * em) {
                        labelBands.push_back(j);
                        labelled = true;
                    } else {
                        break;
                    }
                }
                ++j;
            }
            // A label band with no tabular band below it heads nothing: the run ends above it.
            while (!labelBands.empty() && labelBands.back() >= tabularEnd) {
                j = labelBands.back();
                labelBands.pop_back();
            }
            // Rows from rule i down to rule j. A run of one or two rules (a header rule; top and
            // mid rules of a table that continues on the next page) has no closing rule: rows below
            // it belong while they keep its columns; above a single rule (a header rule, or the
            // closing rule of a continued table), rows above it too.
            const float top = s.ys[i], bottom = s.ys[j];
            const bool single = i == j;
            jointsOf(i, j);
            bool open = j - i <= 1;
            float yhi = top, ylo = bottom;
            // No rule of the stack close above the run's first rule: that rule may be the one under
            // the header, with no top rule; the header lines above it are looked for below.
            const bool headless = !single && (i == 0 || s.ys[i - 1] > top + 6 * em);
            {
                const float below = j + 1 < s.ys.size() ? s.ys[j + 1] : -INFINITY;
                ylo = open ? Fmax(below, bottom - 80 * em) : bottom;
                if (single) yhi = Fmin(i > 0 ? s.ys[i - 1] : INFINITY, top + 80 * em);
                if (headless) yhi = top + 5 * em;
            }
            BuildRows(xa, xb, ylo, yhi, rows, rowWords);
            label.assign(rows.size(), 0);
            for (size_t band : labelBands)
                for (size_t r = 0; r < rows.size(); ++r)
                    if (rows[r].base < s.ys[band] && rows[r].base > s.ys[band + 1]) label[r] = 1;
            const std::vector<uint8_t>* labels = labelBands.empty() ? nullptr : &label;
            ruled.assign(rows.size(), 0);
            for (size_t r = 1, k = i; r < rows.size() && k <= j; ++r) {
                while (k <= j && s.ys[k] >= rows[r - 1].base) ++k;
                if (k <= j && s.ys[k] > rows[r].base) ruled[r] = 1;
            }
            // Core rows: between the rules.
            size_t coreBegin = rows.size(), coreEnd = 0;
            for (size_t r = 0; r < rows.size(); ++r)
                if (rows[r].base < top && rows[r].base > bottom) coreBegin = std::min(coreBegin, r), coreEnd = r + 1;
            // Two rules around three rows or more are a top and a bottom rule, not a top and a header
            // rule: the table is closed.
            if (j - i == 1 && coreEnd >= coreBegin + 3) open = false;
            // Rows taken past the rules (an open run) keep whitespace columns alone: a joint marks
            // the columns of the rows its rules close.
            if (open) joints.clear();
            size_t begin, end;
            if (coreEnd > coreBegin) {
                begin = coreBegin, end = coreEnd;
            } else {
                // No rows between: start at the rows nearest the rule(s).
                size_t firstBelow = 0;
                while (firstBelow < rows.size() && rows[firstBelow].base > bottom) ++firstBelow;
                begin = firstBelow, end = firstBelow;
                if (single && firstBelow > 0 && rows[firstBelow - 1].bottom - top < 2.0f * em) begin = firstBelow - 1;
                if (firstBelow < rows.size() && bottom - rows[firstBelow].top < 2.0f * em) end = firstBelow + 1;
            }
            if (end <= begin) {
                i = j + 1;
                continue;
            }
            // The text size of the rows under the rule; a header row above it shares it.
            em = TextSize(rows[end - 1]);
            if (end - begin >= 2 && SubLine(rows[end - 2], rows[end - 1])) em = TextSize(rows[end - 2]);
            // Rows wholly of running headers or footers (a rule under a page header) join no table
            // from the outside.
            const auto running = [&](size_t q) {
                for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k)
                    if (!words_[rowWords[k]].removed) return false;
                return true;
            };
            if (begin + 1 < end && coreEnd <= coreBegin && (!SizeFits(rows[begin], em) || running(begin))) ++begin;
            // Under a rule, columns may stand as close as half an em (word processors' narrow cell
            // margins): twice a word space, and every row has to agree.
            const float minGap = Fmax(3.0f, 0.5f * em);
            const auto pitchOk = [&](size_t a, size_t b) {
                // Rows a above b are close enough to belong to one table.
                return rows[a].bottom - rows[b].top < 1.6f * Fmax(rows[a].size, rows[b].size);
            };
            // The ink of rows [begin, end), grown with them; Columns over those rows from it.
            Ink& ink = scratch_.ink;
            Ink& grown = scratch_.grown;
            const auto columns = [&](const Ink& rowsInk, size_t from, size_t to, std::vector<Gap>& out) {
                return stackColumns(&rowsInk, from, to, minGap, out, labels);
            };
            const auto extend = [&](size_t& from, size_t& to, bool down) {
                for (;;) {
                    const size_t next = down ? to : from - 1;
                    if (down ? to >= rows.size() : from == 0) return;
                    if (down ? !pitchOk(to - 1, next) : !pitchOk(next, from)) return;
                    if (!SizeFits(rows[next], em) || running(next)) return;
                    const size_t nb = down ? from : next, ne = down ? to + 1 : to;
                    AddInk(rows[next], rowWords, ink, grown);
                    if (!columns(grown, nb, ne, trial)) return;
                    // Every gap still there (gaps of fewer than three rows may be chance word spaces
                    // that close), and the new row in two columns at least; or, below, one cell set
                    // as close as the closest rows so far (a row of empty cells).
                    if (to - from >= 3 && !KeepsGaps(gaps, trial)) return;
                    int occupied = 0, lastCol = -1;
                    for (uint32_t k = rows[next].first; k < rows[next].first + rows[next].count; ++k) {
                        const TWord& w = words_[rowWords[k]];
                        const int c = ColumnOf(trial, (w.x0 + w.x1) / 2);
                        if (c != lastCol) occupied++, lastCol = c;
                    }
                    if (occupied < 2) {
                        if (!down || to - from < 2) return;
                        float closest = INFINITY;
                        for (size_t q = from + 1; q < to; ++q) closest = Fmin(closest, rows[q - 1].bottom - rows[q].top);
                        if (rows[to - 1].bottom - rows[next].top > closest + 0.15f * em) return;
                    }
                    gaps = trial;
                    std::swap(ink, grown);
                    if (down) to = ne;
                    else from = next;
                }
            };
            ink.Clear();
            for (size_t q = begin; q < end; ++q) {
                if (label[q]) continue;  // a label lies across the columns
                AddInk(rows[q], rowWords, ink, grown);
                std::swap(ink, grown);
            }
            columns(ink, begin, end, gaps);
            if (open) extend(begin, end, true);
            if (single) extend(begin, end, false);
            const auto multiCell = [&]() {
                // Rows with two cells filled at least: most rows, two at least. Between a top and a
                // bottom rule (three rules or more), a header row of two cells suffices: the body may
                // leave a column empty (a form to fill in).
                size_t n = 0, cells = 0;
                for (size_t q = begin; q < end; ++q) {
                    int occupied = 0, lastCol = -1;
                    for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k) {
                        const TWord& w = words_[rowWords[k]];
                        const int c = ColumnOf(gaps, (w.x0 + w.x1) / 2);
                        if (c != lastCol) occupied++, lastCol = c;
                    }
                    n += occupied >= 2;
                    cells += static_cast<size_t>(occupied);
                }
                // Most cells empty: labels scattered over a chart's gridlines.
                if (5 * cells < 2 * (end - begin) * (gaps.size() + 1)) return false;
                return open ? n >= 2 && 2 * n >= end - begin : n >= 1;
            };
            const auto code = [&]() {
                // Mostly monospace: aligned code, not a table, unless most lines that start in the
                // first column start with a word in proportional type (labels of rows whose values
                // are code: a table of commands or parameters). Mostly math symbols and italic
                // letters: a fraction bar over displayed math.
                size_t mono = 0, math = 0, all = 0, labelled = 0, starts = 0;
                for (size_t q = begin; q < end; ++q) {
                    mono += rows[q].mono, all += rows[q].count;
                    for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k) math += MathWord(words_[rowWords[k]]);
                    const TWord& first = words_[rowWords[rows[q].first]];
                    if (gaps.empty() || ColumnOf(gaps, (first.x0 + first.x1) / 2) != 0) continue;
                    starts++;
                    labelled += !first.mono;
                }
                return (2 * mono > all && (starts == 0 || 3 * labelled < 2 * starts)) || 10 * math >= 4 * all;
            };
            // A single rule over the fewest rows its table takes, with text beside the rule at its
            // height an em from its ends at most (a displayed formula's name and equals sign, or its
            // next term): the bar of a fraction, whose numerator and denominator are no table.
            const auto fractionBar = [&]() {
                if (!single || end - begin != 3) return false;
                const float reachY = 3 * Fmax(em, bodySize_);
                auto it = std::lower_bound(words_.begin(), words_.end(), top + reachY, [](const TWord& w, float y) { return w.base > y; });
                for (; it != words_.end() && it->base >= top - reachY; ++it) {
                    const float cx = (it->x0 + it->x1) / 2, reach = Fmax(em, it->size);
                    if (it->removed || it->y0 > top || it->y1 < top || (cx >= xa && cx <= xb)) continue;
                    if ((cx < xa && xa - it->x1 <= reach) || (cx > xb && it->x0 - xb <= reach)) return true;
                }
                return false;
            };
            // A single rule closes nothing: its table needs three rows at least.
            if (end - begin >= (single ? 3u : 2u) && columns(ink, begin, end, gaps) &&
                multiCell() && !code() && !fractionBar() &&
                !Texty(rows, rowWords, begin, end, gaps, em, !open) && !ListLike(rows, rowWords, begin, end, gaps) &&
                !(open && WordSpaced(rows, rowWords, begin, end, gaps)) &&
                !FigureText(rows, rowWords, begin, end, em)) {
                // Header: the rows above the second rule of the run; above a single rule, the rows
                // above it when they are few and rows follow below it. More than three lines above
                // the second rule, most of them holding a number, are rows of a section, not a
                // header (a tall header of wrapped labels stays one).
                const auto numbers = [&](size_t q) {
                    for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k)
                        if (Numeric(Chars(words_[rowWords[k]]))) return true;
                    return false;
                };
                uint16_t header = 0;
                if (!single) {
                    size_t numbered = 0;
                    for (size_t r = begin; r < end && rows[r].base > s.ys[i + 1]; ++r) header++, numbered += numbers(r);
                    if (header > 3 && 2 * numbered > header) header = 0;
                } else if (rows[end - 1].base < top) {
                    for (size_t r = begin; r < end && rows[r].base > top; ++r) header++;
                    if (header > 3) header = 0;
                }
                // Header lines above a headless run's first rule (no top rule: the first rule is the
                // one under the header) of three rows at least (two rows boxed by rules are as often
                // a diagram's): up to three lines, each within an em of the rule or of the
                // line under it, set no larger than the body's text, without a number, filling two
                // columns at least (the lowest line half of them); a label across a column gap needs
                // a drawn rule narrower than the table under it (a \cmidrule under a label over
                // columns: it fills the columns of the rule). Text that runs across the columns (a
                // caption, a paragraph) is no header.
                if (headless && !open && begin == coreBegin && end - begin >= 3) {
                    float largest = 0;  // the body's largest text
                    for (size_t q = begin; q < end; ++q) largest = Fmax(largest, TextSize(rows[q]));
                    size_t lifted = 0;
                    for (size_t q = begin; q-- > 0 && lifted < 3;) {
                        const Row& r = rows[q];
                        const float under = lifted == 0 ? top : rows[q + 1].top;
                        if (!(r.base > top) || r.bottom - under > em || TextSize(r) > 1.15f * largest || numbers(q)) break;
                        uint64_t filled = 0;
                        bool crossing = false;
                        for (uint32_t k = r.first; k < r.first + r.count && !crossing;) {
                            // A label: words a word space apart.
                            const float a = words_[rowWords[k]].x0;
                            float b = words_[rowWords[k]].x1;
                            for (++k; k < r.first + r.count; ++k) {
                                const TWord& w = words_[rowWords[k]];
                                if (!(w.x0 - b < Fmax(1.0f, Fmin(2 * wordSpace_, 0.45f) * w.size))) break;
                                b = Fmax(b, w.x1);
                            }
                            // A rule under the label: it fills the columns the rule spans.
                            const Edge* rule = nullptr;
                            auto e = std::lower_bound(h_.begin(), h_.end(), under - 1, [](const Edge& p, float y) { return p.pos < y; });
                            for (; e != h_.end() && e->pos < r.bottom + 1 && !rule; ++e)
                                if (!e->area && e->b - e->a < 0.9f * (xb - xa) && e->a <= a + 0.5f * em && e->b >= b - 0.5f * em) rule = &*e;
                            if (rule) {
                                const int from = ColumnOf(gaps, rule->a + 1), to = ColumnOf(gaps, rule->b - 1);
                                for (int c = from; c <= to && c < 64; ++c) filled |= uint64_t(1) << c;
                                continue;
                            }
                            filled |= uint64_t(1) << std::min(ColumnOf(gaps, (a + b) / 2), 63);
                            for (const Gap& gap : gaps) crossing = crossing || (a < gap.a && b > gap.b);
                        }
                        const int count = Bits(filled);
                        if (crossing || count < 2 || (lifted == 0 && 2 * count < static_cast<int>(gaps.size()) + 1)) break;
                        ++lifted;
                    }
                    if (lifted > 0) begin -= lifted, header = static_cast<uint16_t>(lifted);
                }
                if (header == end - begin) header = 0;
                // No rule under the header (a top and a bottom rule only): the leading rows without
                // a number, three at most, over three rows or more that start with one.
                if (header == 0) {
                    size_t lead = 0;
                    while (lead < 3 && begin + lead < end && !numbers(begin + lead)) ++lead;
                    if (lead > 0 && begin + lead + 3 <= end && numbers(begin + lead)) header = static_cast<uint16_t>(lead);
                }
                // Drawn rules between two header lines narrower than the table (not an underline:
                // a third of a line below the text above, above the middle of the line below).
                partial.clear();
                if (header >= 2) {
                    const float hi = rows[begin].base, lo = rows[begin + header - 1].base;
                    auto e = std::lower_bound(h_.begin(), h_.end(), lo, [](const Edge& q, float y) { return q.pos < y; });
                    for (; e != h_.end() && e->pos < hi && partial.size() < 64; ++e) {
                        if (e->area || e->a < xa - 1 || e->b > xb + 1 || e->b - e->a > 0.9f * (xb - xa) || e->b - e->a < em) continue;
                        size_t r = begin + 1;
                        while (r + 1 < begin + header && rows[r].base > e->pos) ++r;
                        if (rows[r - 1].base - e->pos >= 0.3f * em && e->pos - rows[r].base >= 0.5f * em) partial.push_back(*e);
                    }
                }
                // Body lines that are one label (words a word space apart) across column gaps every
                // other body line leaves open name the sections of rows ("Lower bounds of μ" centred
                // over the columns of figures): each is a label row, the columns come from the
                // other lines. A label heads rows: never the body's last line.
                if (header > 0 && end - begin >= header + 4u) {
                    const size_t body = end - begin - header;
                    sections.assign(label.begin(), label.end());
                    size_t lone = 0;
                    for (size_t q = begin + header; q + 1 < end; ++q) {
                        if (label[q]) continue;
                        bool one = true;
                        for (uint32_t k = rows[q].first + 1; k < rows[q].first + rows[q].count && one; ++k) {
                            const TWord& w = words_[rowWords[k]];
                            one = w.x0 - words_[rowWords[k - 1]].x1 < Fmax(1.0f, Fmin(2 * wordSpace_, 0.45f) * w.size);
                        }
                        if (one) sections[q] = 1, lone++;
                    }
                    if (lone > 0 && body >= lone + 3 && Columns(rows, rowWords, begin + header, end, xa, xb, minGap, trial, labels) &&
                        Columns(rows, rowWords, begin + header, end, xa, xb, minGap, sectionGaps, &sections) &&
                        sectionGaps.size() > trial.size() && KeepsGaps(trial, sectionGaps)) {
                        for (size_t q = begin + header; q + 1 < end; ++q) {
                            if (!sections[q] || label[q]) continue;
                            const float a = words_[rowWords[rows[q].first]].x0;
                            const float b = words_[rowWords[rows[q].first + rows[q].count - 1]].x1;
                            for (const Gap& gap : sectionGaps) label[q] = label[q] || (a < gap.a && b > gap.b);
                            if (label[q]) labels = &label;
                        }
                    }
                }
                // A header label over several columns (a unit over its sub-columns) closes the gaps
                // between them: the body rows, three at least, say where the columns are, and the
                // label spans them.
                const size_t before = grids_.size();
                float y0 = rows[end - 1].bottom, y1 = rows[begin].top;
                if (header > 0 && end - begin >= header + 3u &&
                    Columns(rows, rowWords, begin + header, end, xa, xb, minGap, trial, labels) && trial.size() > gaps.size() &&
                    KeepsGaps(gaps, trial) && Free(xa, y0, xb, y1))
                    TextGrid(rows, rowWords, begin, end, trial, TableKind::Ruled, header, xa, xb, labels, &ruled, &partial, &joints);
                // Without such a label (or when its rows do not make a grid): the columns of all rows.
                if (grids_.size() == before && Free(xa, y0, xb, y1))
                    TextGrid(rows, rowWords, begin, end, gaps, TableKind::Ruled, header, xa, xb, labels, &ruled, &partial, &joints);
            }
            i = j + 1;
        }
    }
}

// A table continued from the previous page: the rows at the top of this page (below running
// headers) that keep its columns, every word inside one column and two columns filled at least.
void Detector::Continue() {
    if (!continuation_.valid || continuation_.xs.size() < 3) return;
    const std::vector<float>& xs = continuation_.xs;
    const float xa = continuation_.x0 - 2, xb = continuation_.x1 + 2;
    std::vector<Row> rows;
    std::vector<uint32_t> rowWords;
    BuildRows(xa, xb, -INFINITY, INFINITY, rows, rowWords);
    const auto removedRow = [&](size_t q) {
        for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k)
            if (!words_[rowWords[k]].removed) return false;
        return true;
    };
    size_t r = 0;
    while (r < rows.size() && removedRow(r)) ++r;
    if (r >= rows.size()) return;
    // Nothing above it anywhere on the page but running headers (a table here took nothing).
    for (size_t i = 0; i < words_.size() && words_[i].base > rows[r].base + 0.35f * rows[r].size; ++i)
        if (!words_[i].removed) return;
    std::vector<Gap> gaps;
    for (size_t c = 1; c + 1 < xs.size(); ++c) gaps.push_back(Gap{xs[c], xs[c]});
    const auto fits = [&](size_t q) {
        // Every word inside one column, two columns filled, and cells apart by more than a word
        // space (a short line of prose can fall into two columns).
        int occupied = 0, last = -1;
        float right = -INFINITY;
        for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k) {
            const TWord& w = words_[rowWords[k]];
            if (w.x0 < xa || w.x1 > xb) return false;
            for (size_t c = 1; c + 1 < xs.size(); ++c)
                if (w.x0 < xs[c] - 1 && w.x1 > xs[c] + 1) return false;
            const int col = ColumnOf(gaps, (w.x0 + w.x1) / 2);
            if (col != last) {
                if (last >= 0 && w.x0 - right < 0.5f * w.size) return false;
                occupied++, last = col;
            }
            right = Fmax(right, w.x1);
        }
        return occupied >= 2;
    };
    size_t e = r;
    while (e < rows.size() && fits(e) &&
           (e == r || rows[e - 1].bottom - rows[e].top < 1.6f * Fmax(rows[e - 1].size, rows[e].size)))
        ++e;
    if (e > r && Free(xa, rows[e - 1].bottom, xb, rows[r].top))
        TextGrid(rows, rowWords, r, e, gaps, TableKind::Ruled, 0, xa, xb);
}

void Detector::MarkUsed(const Grid& g) {
    marks_++;
    auto it = std::lower_bound(words_.begin(), words_.end(), g.y1 + 20, [](const TWord& w, float y) { return w.base > y; });
    for (size_t i = static_cast<size_t>(it - words_.begin()); i < words_.size() && words_[i].base >= g.y0 - 20; ++i) {
        const TWord& w = words_[i];
        const float cx = (w.x0 + w.x1) / 2, cy = (w.y0 + w.y1) / 2;
        if (cx >= g.x0 && cx <= g.x1 && cy >= g.y0 && cy <= g.y1) wordUsed_[i] = 1;
    }
}

// Borderless tables: runs of rows that split into the same columns, kept only when confident.
// Confidence comes from Camelot-style stream evidence (algorithms §6.7): several rows, filled cells,
// column edges aligned left, right or centred, numeric cells, an even row pitch.
void Detector::Borderless(float gapFactor) {
    // The rows of all words left: the second pass reuses the first's while no grid took words.
    if (pageRowsMarks_ != marks_) {
        BuildRows(-INFINITY, INFINITY, -INFINITY, INFINITY, pageRows_, pageRowWords_);
        pageRowsMarks_ = marks_;
    }
    const std::vector<Row>& rows = pageRows_;
    const std::vector<uint32_t>& rowWords = pageRowWords_;
    if (rows.size() < 3 || rows.size() > 4000) return;
    // Rows that split at a column gap (gapFactor ems: wider than any word space) and hold no code
    // are candidates.
    std::vector<uint8_t>& split = scratch_.split;
    split.assign(rows.size(), 0);
    for (size_t r = 0; r < rows.size(); ++r) {
        int segments = 1;
        bool mono = false;
        float right = -INFINITY;
        for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
            const TWord& w = words_[rowWords[k]];
            mono = mono || w.mono;
            if (right > -INFINITY && w.x0 - right >= gapFactor * rows[r].size) segments++;
            right = Fmax(right, w.x1);
        }
        split[r] = segments >= 2 && !mono;
    }
    std::vector<Gap>& gaps = scratch_.gaps;
    std::vector<Gap>& trial = scratch_.trial;
    std::string scratch, cellText;
    std::vector<float>& lefts = scratch_.lefts;
    std::vector<float>& rights = scratch_.rights;
    std::vector<float>& centres = scratch_.centres;
    std::vector<float>& pitches = scratch_.pitches;
    std::vector<uint8_t>& label = scratch_.label;  // a one-cell row of the first column inside a run
    label.assign(rows.size(), 0);
    size_t r = 0;
    while (r < rows.size() && grids_.size() < static_cast<size_t>(kMaxTablesPerPage)) {
        if (!split[r]) {
            ++r;
            continue;
        }
        const float em = rows[r].size;
        const float minGap = Fmax(4.0f, gapFactor * em), keepGap = gapFactor < 1.0f ? minGap : Fmin(minGap, Fmax(4.0f, 0.5f * em));
        float xa = rows[r].x0 - 1, xb = rows[r].x1 + 1;
        Columns(rows, rowWords, r, r + 1, xa, xb, minGap, gaps);
        // The ink of the run's rows but labels, grown a row at a time.
        Ink& ink = scratch_.ink;
        Ink& grown = scratch_.grown;
        ink.Clear();
        if (!label[r]) {
            AddInk(rows[r], rowWords, ink, grown);
            std::swap(ink, grown);
        }
        size_t e = r + 1;
        // A row of one cell that ends before the second column's text, between split rows, is a
        // section label of the table ("Revenue:" over its quarters) or the wrapped text of the cell
        // above; it may reach into the first gap, which the other rows keep.
        const auto isLabel = [&](size_t q) {
            // After two rows of the table at least: a lone line under a header line is a cell.
            return !gaps.empty() && q >= r + 2 && q + 1 < rows.size() && split[q + 1] && rows[q].x1 < gaps[0].b;
        };
        // A line set less than a line below the one above, between split rows: a cell set to the
        // middle of a row whose other cells hold two lines ("Time t" beside two-line labels).
        const auto middle = [&](size_t q) {
            return q + 1 < rows.size() && split[q + 1] && rows[q - 1].base - rows[q].base < 0.85f * Fmax(rows[q - 1].size, rows[q].size);
        };
        while (e < rows.size() && (split[e] || isLabel(e) || middle(e)) && rows[e - 1].bottom - rows[e].top <= 1.0f * em &&
               std::fabs(rows[e].size - em) <= 0.2f * em) {
            label[e] = isLabel(e);
            if (!label[e]) {
                AddInk(rows[e], rowWords, ink, grown);
                std::swap(ink, grown);
            }
            const float na = Fmin(xa, rows[e].x0 - 1), nb = Fmax(xb, rows[e].x1 + 1);
            // Rows after the first keep its gaps half an em wide at least where figures stand on both
            // sides of the gap in that row (close columns of numbers), else as wide as the first; in
            // the second pass, its gaps already that close, as wide as the first.
            bool kept = (ink.proper ? InkColumns(ink, na, nb, keepGap, trial)
                                    : Columns(rows, rowWords, r, e + 1, na, nb, keepGap, trial, &label)) &&
                        KeepsGaps(gaps, trial);
            for (size_t gi = 0; gi < trial.size() && kept; ++gi) {
                const Gap& n = trial[gi];
                if (n.b - n.a >= minGap) continue;
                const TWord *before = nullptr, *after = nullptr;
                for (uint32_t k = rows[e].first; k < rows[e].first + rows[e].count; ++k) {
                    const TWord& w = words_[rowWords[k]];
                    if (w.x1 <= n.a + 0.5f && (!before || w.x1 > before->x1)) before = &w;
                    if (w.x0 >= n.b - 0.5f && (!after || w.x0 < after->x0)) after = &w;
                }
                if (before && after && after->x0 - before->x1 < minGap)
                    kept = Numeric(Chars(*before)) && Numeric(Chars(*after));
            }
            if (!kept) {
                label[e] = 0;
                break;
            }
            Narrow(gaps, trial, minGap, scratch_.narrowed);
            xa = na, xb = nb;
            ++e;
        }
        while (e > r + 1 && label[e - 1]) label[--e] = 0;  // a label ends no table
        const size_t begin = r;
        r = e;
        int labels = 0;
        for (size_t q = begin; q < e; ++q) labels += label[q];
        const int n = static_cast<int>(e - begin), m = static_cast<int>(gaps.size()) + 1;
        const auto clearLabels = [&]() {
            for (size_t q = begin; q < e; ++q) label[q] = 0;
        };
        // Runs of a table's shape the tests reject still have their gaps reported to features_.
        const bool shape = n >= 3 && m >= 2 && m <= kMaxTableCols;
        const bool rejected =
            !shape || Texty(rows, rowWords, begin, e, gaps, em, false) || ListLike(rows, rowWords, begin, e, gaps);
        const bool report = features_ && shape;
        if (rejected && !report) {
            clearLabels();
            continue;
        }
        // Cells: filled, numeric, per row; leading rows without a number over a body with numbers
        // are the header, whose cells may be set otherwise (centred over the column) and further
        // apart.
        std::vector<int>& columns = scratch_.cellColumns;
        WordColumns(rows, rowWords, begin, e, gaps, columns);
        const uint32_t base = rows[begin].first;
        std::vector<uint8_t> numericRow(static_cast<size_t>(n), 0);
        int filled = 0, numeric = 0, cellWords = 0, lastFilled = 0, pageRefs = 0;
        for (size_t q = begin; q < e; ++q) {
            if (label[q]) continue;
            int last = -1;
            // The cell's text by its words' counts; its bytes only in the last column (page numbers).
            uint32_t size = 0, digits = 0, letters = 0;
            cellText.clear();
            for (uint32_t k = rows[q].first; k <= rows[q].first + rows[q].count; ++k) {
                const int c = k < rows[q].first + rows[q].count ? columns[k - base] : -2;
                if (c != last && size > 0) {
                    filled++;
                    if (NumericCounts(digits, letters)) numeric++, numericRow[q - begin] = 1;
                    if (last == m - 1) lastFilled++, pageRefs += PageNumber(cellText);
                    size = digits = letters = 0;
                    cellText.clear();
                }
                if (c == -2) break;
                const TWord& w = words_[rowWords[k]];
                const WordChars& t = Chars(w);
                size += t.size, digits += t.digits, letters += t.letters;
                if (c == m - 1) cellText += WordText(w, scratch);
                cellWords++;
                last = c;
            }
        }
        size_t lead = 0;
        while (lead < 3 && begin + lead < e && !numericRow[lead] && !label[begin + lead]) ++lead;
        if (begin + lead >= e || !numericRow[lead] || n - labels - static_cast<int>(lead) < 3) lead = 0;
        // Column edges of the body rows: aligned left, right or centred.
        int aligned = 0;
        const float tol = Fmax(2.0f, 0.3f * em);
        for (int c = 0; c < m; ++c) {
            lefts.clear(), rights.clear(), centres.clear();
            for (size_t q = begin + lead; q < e; ++q) {
                if (label[q]) continue;
                float a = INFINITY, b = -INFINITY;
                for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k) {
                    const TWord& w = words_[rowWords[k]];
                    if (columns[k - base] == c) a = Fmin(a, w.x0), b = Fmax(b, w.x1);
                }
                if (b > a) lefts.push_back(a), rights.push_back(b), centres.push_back((a + b) / 2);
            }
            const auto agree = [](std::vector<float>& v, float within) {
                if (v.size() < 2) return false;
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
                const float median = v[v.size() / 2];
                size_t near = 0;
                for (float x : v) near += std::fabs(x - median) <= within;
                return near * 5 >= v.size() * 4;
            };
            if (agree(lefts, tol) || agree(rights, tol) || agree(centres, 1.5f * tol)) aligned++;
        }
        // Row pitch of the body (below the header, or, with three pitches left, below the first row:
        // a header row with numbers, years, stands apart as well).
        pitches.clear();
        const size_t from = begin + (lead > 0 ? lead : (n >= 5 ? 1 : 0));
        for (size_t q = from + 1; q < e; ++q) pitches.push_back(rows[q - 1].base - rows[q].base);
        float mean = 0, var = 0;
        for (float p : pitches) mean += p;
        mean /= float(pitches.size());
        for (float p : pitches) var += (p - mean) * (p - mean);
        const float cv = mean > 0 ? std::sqrt(var / float(pitches.size())) / mean : 1;
        const float fill = float(filled) / float((n - labels) * m);
        const float numericShare = filled ? float(numeric) / float(filled) : 0;
        // Short cells: running text has several words to a line.
        const float wordsPerCell = filled ? float(cellWords) / float(filled) : 0;
        // Two columns without numbers: a long run of short, evenly set, fully filled pairs (names and
        // affiliations, terms and values); five of them, unless the second column holds page numbers
        // only (titles over a list of contents).
        const bool contents = m == 2 && lastFilled >= 3 && pageRefs == lastFilled;
        const bool pairs = (n >= 6 || (n == 5 && !contents)) && fill >= 0.9f && wordsPerCell <= 3 && cv <= 0.15f;
        const bool high = fill >= 0.75f && aligned == m && cv <= 0.2f && wordsPerCell <= 4 &&
                          (m >= 3 ? numericShare >= 0.2f || n >= 4 : numericShare >= 0.3f || pairs);
        // A header row set apart above the body (two lines' space at most) whose words keep to its
        // columns, two of them filled at least, none with a number, is the table's first row.
        size_t first = begin;
        if (high && begin > 0) {
            const Row& h = rows[begin - 1];
            int occupied = 0, last = -1;
            bool numbers = false;
            for (uint32_t k = h.first; k < h.first + h.count; ++k) {
                const int c = ColumnOf(gaps, (words_[rowWords[k]].x0 + words_[rowWords[k]].x1) / 2);
                if (c != last) occupied++, last = c;
                numbers = numbers || Numeric(Chars(words_[rowWords[k]]));
            }
            const bool near = h.bottom - rows[begin].top <= 2.0f * em && SizeFits(h, em) && !numbers;
            if (near && h.x0 >= xa - em && h.x1 <= xb + em && occupied >= 2 &&
                Columns(rows, rowWords, begin - 1, e, xa, xb, keepGap, trial, &label) && KeepsGaps(gaps, trial) &&
                trial.size() == gaps.size()) {
                first = begin - 1;
            } else if (near && (HeaderOverColumns(rows, rowWords, begin, e, label, h, minGap, gaps) ||
                                HeaderAtColumns(rows, rowWords, begin, e, label, h, gaps))) {
                // Labels centred over narrow columns of figures, wider than them: each label (a run of
                // words a word space apart) heads the column nearest its centre, and the columns part
                // between labels. Or labels starting where the columns start, a word space apart.
                first = begin - 1;
                xa = Fmin(xa, h.x0 - 1), xb = Fmax(xb, h.x1 + 1);
            }
        }
        const float y0 = rows[e - 1].bottom, y1 = rows[first].top;
        const size_t tables = grids_.size();
        if (!rejected && high && Free(xa, y0, xb, y1) && !FigureText(rows, rowWords, first, e, TextSize(rows[first])))
            TextGrid(rows, rowWords, first, e, gaps, TableKind::Borderless, 0, xa, xb);
        if (report) ReportGaps(rows, rowWords, begin, e, gaps, columns, label, em, gapFactor < 1.0f, grids_.size() > tables);
        if (rejected) {
            clearLabels();
            continue;
        }
        clearLabels();
        // A header row without numbers whose labels, centred over narrow columns of figures, fill
        // the gaps between them sets the columns wrong: the body is tried again without it.
        if (!high && e - begin >= 4 && !numericRow[0] && numericRow[1]) r = begin + 1;
    }
}

// A header row over the columns of body rows [begin, end) (label rows aside): its labels (words
// closer than `minGap`) each go to the column whose text centre is nearest, one label a column and
// two labels at least, and the gaps are moved to lie between each column's text and label and the
// next one's. False, gaps unchanged, when the labels do not part so.
bool Detector::HeaderOverColumns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t begin,
                                 size_t end, const std::vector<uint8_t>& label, const Row& header, float minGap,
                                 std::vector<Gap>& gaps) {
    const size_t cols = gaps.size() + 1;
    if (cols > 64) return false;
    std::vector<float> left(cols, INFINITY), right(cols, -INFINITY);
    for (size_t q = begin; q < end; ++q) {
        if (label[q]) continue;
        for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k) {
            const TWord& w = words_[rowWords[k]];
            const size_t c = static_cast<size_t>(ColumnOf(gaps, (w.x0 + w.x1) / 2));
            left[c] = Fmin(left[c], w.x0), right[c] = Fmax(right[c], w.x1);
        }
    }
    for (size_t c = 0; c < cols; ++c)
        if (!(right[c] > left[c])) return false;
    uint64_t taken = 0;
    int labels = 0;
    uint32_t k = header.first;
    while (k < header.first + header.count) {
        float a = words_[rowWords[k]].x0, b = words_[rowWords[k]].x1;
        for (++k; k < header.first + header.count && words_[rowWords[k]].x0 - b < minGap; ++k) b = Fmax(b, words_[rowWords[k]].x1);
        size_t best = 0;
        for (size_t c = 1; c < cols; ++c)
            if (std::fabs((left[c] + right[c]) / 2 - (a + b) / 2) < std::fabs((left[best] + right[best]) / 2 - (a + b) / 2)) best = c;
        if ((taken >> best) & 1) return false;
        taken |= uint64_t(1) << best;
        labels++;
        left[best] = Fmin(left[best], a), right[best] = Fmax(right[best], b);
    }
    if (labels < 2) return false;
    for (size_t c = 0; c + 1 < cols; ++c)
        if (!(right[c] < left[c + 1])) return false;
    for (size_t c = 0; c + 1 < cols; ++c) gaps[c] = Gap{right[c], left[c + 1]};
    return true;
}

// A header row whose labels start where the body's columns start (labels wider than their
// columns, set a word space apart): a word of it starts within a third of an em of each column's
// left edge, the first column's included, and none starts left of that; each word belongs to the
// column whose left edge is the last at or before it, and the gaps move to lie between each
// column's text and label and the next column's left edge. False, gaps unchanged, when the labels
// do not line up so.
bool Detector::HeaderAtColumns(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t begin,
                               size_t end, const std::vector<uint8_t>& label, const Row& header, std::vector<Gap>& gaps) {
    const size_t cols = gaps.size() + 1;
    if (cols > 64) return false;
    std::vector<float> left(cols, INFINITY), right(cols, -INFINITY);
    for (size_t q = begin; q < end; ++q) {
        if (label[q]) continue;
        for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k) {
            const TWord& w = words_[rowWords[k]];
            const size_t c = static_cast<size_t>(ColumnOf(gaps, (w.x0 + w.x1) / 2));
            left[c] = Fmin(left[c], w.x0), right[c] = Fmax(right[c], w.x1);
        }
    }
    for (size_t c = 0; c < cols; ++c)
        if (!(right[c] > left[c])) return false;
    const float tol = Fmax(1.0f, 0.3f * TextSize(header));
    uint64_t started = 0;
    for (uint32_t k = header.first; k < header.first + header.count; ++k) {
        const TWord& w = words_[rowWords[k]];
        size_t c = cols;
        while (c > 0 && left[c - 1] > w.x0 + tol) --c;
        if (c == 0) return false;
        --c;
        if (std::fabs(w.x0 - left[c]) <= tol) started |= uint64_t(1) << c;
        right[c] = Fmax(right[c], w.x1);
    }
    if (started != (cols == 64 ? ~uint64_t(0) : (uint64_t(1) << cols) - 1)) return false;
    for (size_t c = 0; c + 1 < cols; ++c)
        if (!(right[c] < left[c + 1])) return false;
    for (size_t c = 0; c + 1 < cols; ++c) gaps[c] = Gap{right[c], left[c + 1]};
    return true;
}

// A list of contents set as a table, ruled or not: in three rows at least a page reference (a
// number, or a roman numeral in front matter) alone in the last column, never going down the
// column, with an entry's words before it in four such rows of five; two columns, or three whose
// first holds section numbers only. Ascending numbers beside names are a data table as well (counts,
// ranks), so it also takes a sign of contents: a "Contents" line above it on the page, leader dots
// after an entry, front matter in roman numerals, or five references at least that step unevenly.
bool Detector::Contents(const Grid& g) const {
    const int cols = g.Cols();
    if (cols < 2 || cols > 3) return false;
    std::vector<std::string> last(static_cast<size_t>(g.Rows()));
    std::vector<uint8_t> entry(static_cast<size_t>(g.Rows()), 0);
    std::string scratch;
    bool leaders = false;
    auto it = std::lower_bound(words_.begin(), words_.end(), g.y1, [](const TWord& w, float y) { return w.base > y; });
    const auto above = it;  // the words above the grid
    const auto contentsTitle = [&]() {
        for (auto w = words_.begin(); w != above; ++w) {
            if (w->removed) continue;
            std::string_view t = WordText(*w, scratch);
            if (t.size() != 8) continue;
            std::string folded(t);
            for (char& c : folded) c = static_cast<char>(c | 0x20);
            if (folded == "contents") return true;
        }
        return false;
    };
    Locator locate(g);
    for (; it != words_.end() && it->base >= g.y0; ++it) {
        const TWord& w = *it;
        int r = 0, c = 0;
        if (w.removed || !locate.Find((w.x0 + w.x1) / 2, (w.y0 + w.y1) / 2, r, c)) continue;
        const Span& cell = g.cells[static_cast<size_t>(g.owner[static_cast<size_t>(r * cols + c)])];
        const std::string_view t = WordText(w, scratch);
        if (cell.c + cell.cs != cols) {
            if (cols == 3 && cell.c == 0) {
                for (char ch : t)
                    if (!((ch >= '0' && ch <= '9') || ch == '.')) return false;
                continue;
            }
            entry[static_cast<size_t>(r)] = entry[static_cast<size_t>(r)] || Chars(w).letters > 0;
            leaders = leaders || t.find("....") != std::string_view::npos || t.find("\xE2\x80\xA6\xE2\x80\xA6") != std::string_view::npos;
            continue;
        }
        std::string& text = last[static_cast<size_t>(cell.r)];
        if (text.size() > 8) return false;
        if (!text.empty()) text.push_back(' ');
        text += t;
    }
    int refs = 0, entries = 0, previous = -1, step = -1;
    bool previousRoman = false, roman = false, uneven = false;
    for (size_t r = 0; r < last.size(); ++r) {
        if (last[r].empty()) continue;
        bool isRoman = false;
        const int page = PageValue(last[r], isRoman);
        if (page < 0 || (previous >= 0 && isRoman == previousRoman && page < previous)) return false;
        if (previous >= 0 && isRoman == previousRoman) {
            uneven = uneven || (step >= 0 && page - previous != step);
            step = page - previous;
        }
        previous = page, previousRoman = isRoman, roman = roman || isRoman, refs++;
        entries += entry[r];
    }
    if (refs < 3 || entries * 5 < refs * 4) return false;
    return leaders || roman || (refs >= 5 && uneven) || contentsTitle();
}

// A table of the same columns right under another one, nothing between them but whitespace up to
// one and a half of the upper table's rows (a blank line set between groups of rows, or a run of
// rows broken by a long cell), is the rest of its rows: one table. The lower part has no header of
// its own (its first row holds a number), both parts stand at the same left and right edges (a
// tenth of their width), and in each the body's words of every column keep clear of the next
// column's words of both: the columns part halfway between. Lattices keep their own rules.
void Detector::JoinStacked() {
    std::vector<float> lo, hi;
    // The words of grid g with their row and column in it: f(word, row, column).
    const auto eachWord = [&](const Grid& g, const auto& f) {
        auto it = std::lower_bound(words_.begin(), words_.end(), g.y1, [](const TWord& w, float y) { return w.base > y; });
        for (; it != words_.end() && it->base >= g.y0; ++it) {
            const float cx = (it->x0 + it->x1) / 2, cy = (it->y0 + it->y1) / 2;
            if (!(cx >= g.x0 && cx <= g.x1 && cy >= g.y0 && cy <= g.y1)) continue;
            const int r = static_cast<int>(std::upper_bound(g.ys.begin(), g.ys.end(), cy, std::greater<float>()) - g.ys.begin()) - 1;
            const int c = static_cast<int>(std::upper_bound(g.xs.begin(), g.xs.end(), cx) - g.xs.begin()) - 1;
            f(*it, std::min(std::max(r, 0), g.Rows() - 1), std::min(std::max(c, 0), g.Cols() - 1));
        }
    };
    for (size_t a = 0; a < grids_.size(); ++a) {
        for (size_t b = 0; b < grids_.size(); ++b) {
            if (b == a) continue;
            const Grid& up = grids_[a];
            const Grid& down = grids_[b];
            const int cols = up.Cols();
            if (up.kind == TableKind::Lattice || down.kind == TableKind::Lattice || down.Cols() != cols || cols < 2 ||
                cols > 64 || up.Rows() < 1 || down.Rows() < 2 || down.headerRows > 0 || !down.labelRow.empty() ||
                up.Rows() + down.Rows() > kMaxTableRows)
                continue;
            const float pitch = (up.y1 - up.y0) / float(up.Rows()), width = Fmin(up.x1 - up.x0, down.x1 - down.x0);
            if (!(down.y1 <= up.y0 + 0.3f * pitch && up.y0 - down.y1 <= 1.5f * pitch && down.y0 < up.y0) ||
                !(std::fabs(up.x0 - down.x0) <= 0.1f * width && std::fabs(up.x1 - down.x1) <= 0.1f * width))
                continue;
            // Nothing between: no word outside both.
            const float xa = Fmin(up.x0, down.x0), xb = Fmax(up.x1, down.x1);
            bool free = true;
            auto it = std::lower_bound(words_.begin(), words_.end(), up.y0, [](const TWord& w, float y) { return w.base > y; });
            for (; it != words_.end() && it->base >= down.y1 && free; ++it) {
                const float cx = (it->x0 + it->x1) / 2;
                free = cx < xa || cx > xb || wordUsed_[static_cast<size_t>(it - words_.begin())];
            }
            if (!free) continue;
            bool number = false;
            eachWord(down, [&](const TWord& w, int r, int) { number = number || (r == 0 && Numeric(Chars(w))); });
            if (!number) continue;
            // Per inner boundary: the right edge of the body words before it, the left edge of those after.
            lo.assign(static_cast<size_t>(cols), -INFINITY);
            hi.assign(static_cast<size_t>(cols), INFINITY);
            for (const Grid* g : {&up, &down}) {
                eachWord(*g, [&](const TWord& w, int r, int c) {
                    if (r < g->headerRows || (!g->labelRow.empty() && g->labelRow[static_cast<size_t>(r)])) return;
                    for (int k = 1; k < cols; ++k) {
                        if (c < k) lo[static_cast<size_t>(k)] = Fmax(lo[static_cast<size_t>(k)], w.x1);
                        else hi[static_cast<size_t>(k)] = Fmin(hi[static_cast<size_t>(k)], w.x0);
                    }
                });
            }
            bool clear = true;
            for (int k = 1; k < cols && clear; ++k) clear = hi[static_cast<size_t>(k)] - lo[static_cast<size_t>(k)] >= 1.0f;
            if (!clear) continue;
            Grid joined;
            joined.kind = up.kind;
            joined.xs.push_back(Fmin(up.xs.front(), down.xs.front()));
            for (int k = 1; k < cols; ++k) joined.xs.push_back((lo[static_cast<size_t>(k)] + hi[static_cast<size_t>(k)]) / 2);
            joined.xs.push_back(Fmax(up.xs.back(), down.xs.back()));
            joined.ys = up.ys;
            joined.ys.back() = (up.ys.back() + down.ys.front()) / 2;
            joined.ys.insert(joined.ys.end(), down.ys.begin() + 1, down.ys.end());
            bool ordered = true;
            for (size_t i = 1; i < joined.ys.size() && ordered; ++i) ordered = joined.ys[i] < joined.ys[i - 1];
            for (size_t i = 1; i < joined.xs.size() && ordered; ++i) ordered = joined.xs[i] > joined.xs[i - 1];
            if (!ordered) continue;
            joined.vb = up.vb;
            joined.vb.insert(joined.vb.end(), down.vb.begin(), down.vb.end());
            joined.hb.assign(up.hb.begin(), up.hb.begin() + up.Rows() * cols);
            joined.hb.insert(joined.hb.end(), static_cast<size_t>(cols), 1);
            joined.hb.insert(joined.hb.end(), down.hb.begin() + cols, down.hb.end());
            if (!up.labelRow.empty()) {
                joined.labelRow = up.labelRow;
                joined.labelRow.resize(static_cast<size_t>(joined.Rows()), 0);
            }
            joined.headerRows = up.headerRows;
            joined.size = up.size;
            joined.x0 = xa, joined.x1 = xb, joined.y0 = down.y0, joined.y1 = up.y1;
            grids_[a] = std::move(joined);
            grids_.erase(grids_.begin() + static_cast<std::ptrdiff_t>(b));
            if (b < a) --a;
            b = static_cast<size_t>(-1);  // look again: a third part may follow
        }
    }
}

// A table's header row repeated lower on the page (the same table again after a note, or its next
// part): a free row whose words, in order, are the cell texts of the table's first row (three cells
// with text at least), with a row within two line pitches below it that fills two of its columns,
// starts a table. Its columns are the repeated labels' (a column parts in the space before each
// label after the first: the part may be set closer than the table above), and it runs down through
// each row that fills two columns at least, up to a gap of more than two line pitches.
void Detector::RepeatedHeaders() {
    std::vector<Row> rows;
    std::vector<uint32_t> rowWords;
    std::vector<Gap> gaps;
    std::vector<uint32_t> labelWords;  // per first-row cell with text: its number of words
    std::string scratch, expected, text;
    const size_t count = grids_.size();
    for (size_t gi = 0; gi < count && grids_.size() < static_cast<size_t>(kMaxTablesPerPage); ++gi) {
        if (grids_[gi].cells.empty()) ComputeSpans(grids_[gi]);
        const Grid& g = grids_[gi];
        const int cols = g.Cols();
        if (cols < 3 || cols > kMaxTableCols || g.Rows() < 1) continue;
        // The first row's cell texts, left to right (the cells are in row-major order).
        expected.clear();
        labelWords.clear();
        for (const Span& s : g.cells) {
            if (s.r != 0) continue;
            const float left = g.xs[s.c], right = g.xs[s.c + s.cs], top = g.ys[0], bottom = g.ys[s.rs];
            uint32_t words = 0;
            auto it = std::lower_bound(words_.begin(), words_.end(), top + 20, [](const TWord& w, float y) { return w.base > y; });
            for (; it != words_.end() && it->base >= bottom - 20; ++it) {
                const float cx = (it->x0 + it->x1) / 2, cy = (it->y0 + it->y1) / 2;
                if (!(cx >= left && cx <= right && cy <= top && cy >= bottom)) continue;
                if (!expected.empty()) expected += ' ';
                expected += WordText(*it, scratch);
                words++;
            }
            if (words > 0) labelWords.push_back(words);
        }
        if (labelWords.size() < 3) continue;
        uint32_t total = 0;  // the words of the first row's cells
        for (const uint32_t words : labelWords) total += words;
        const float x0 = g.x0, x1 = g.x1, y0 = g.y0;
        BuildRows(x0, x1, -INFINITY, y0, rows, rowWords);
        for (size_t r = 0; r + 1 < rows.size(); ++r) {
            if (rows[r].count != total) continue;
            text.clear();
            for (uint32_t k = rows[r].first; k < rows[r].first + rows[r].count; ++k) {
                if (!text.empty()) text += ' ';
                text += WordText(words_[rowWords[k]], scratch);
            }
            if (text != expected) continue;
            // The gaps before each label after the first (the words match the labels in order).
            gaps.clear();
            uint32_t k = rows[r].first;
            float right = -INFINITY;
            bool apart = true;
            for (const uint32_t words : labelWords) {
                const TWord& first = words_[rowWords[k]];
                if (right > -INFINITY) {
                    apart = apart && first.x0 > right;
                    gaps.push_back(Gap{right, first.x0});
                }
                k += words;
                right = words_[rowWords[k - 1]].x1;
            }
            if (!apart) break;
            const auto columnsFilled = [&](const Row& row) {
                uint64_t mask = 0;
                for (uint32_t q = row.first; q < row.first + row.count; ++q) {
                    const TWord& w = words_[rowWords[q]];
                    mask |= uint64_t(1) << std::min(ColumnOf(gaps, (w.x0 + w.x1) / 2), 63);
                }
                return Bits(mask);
            };
            const float reach = 2 * 1.2f * TextSize(rows[r]);
            size_t end = r + 1;
            while (end < rows.size() && rows[end - 1].base - rows[end].base <= reach && columnsFilled(rows[end]) >= 2) ++end;
            if (end > r + 1) TextGrid(rows, rowWords, r, end, gaps, TableKind::Borderless, 1, x0, x1);
            break;
        }
    }
}

// A grid standing on a chart's axis (its tick marks within two ems, across the axis or along it)
// holds the chart's text: tick labels, an axis title, a legend, the values on its bars or in the
// cells of a heat map. No table; its words stay lines of text.
void Detector::DropChartText() {
    if (grids_.empty()) return;
    std::vector<FigureRegion>& axes = scratch_.axes;
    ChartAxes(page_, axes);
    if (axes.empty()) return;
    const float reach = 2 * bodySize_;
    for (FigureRegion& a : axes) {
        const Ruling r = ToFrame(Ruling{a.x0 - reach, a.y0 - reach, a.x1 + reach, a.y1 + reach, 0}, dir_);
        a = FigureRegion{r.x0, r.y0, r.x1, r.y1};
    }
    const auto onAxis = [&](const Grid& g) {
        for (const FigureRegion& a : axes)
            if (Overlaps(g, a.x0, a.y0, a.x1, a.y1)) return true;
        return false;
    };
    grids_.erase(std::remove_if(grids_.begin(), grids_.end(), onAxis), grids_.end());
}

void Detector::Commit() {
    for (Grid& g : grids_) {
        if (g.cells.empty()) ComputeSpans(g);
        if (SplitSpanLines(g)) ComputeSpans(g);
    }
    // A list of contents stays lines of text; its words are free again.
    for (size_t gi = grids_.size(); gi-- > 0;) {
        const Grid& g = grids_[gi];
        if (!Contents(g)) continue;
        for (size_t i = 0; i < words_.size(); ++i) {
            const TWord& w = words_[i];
            const float cx = (w.x0 + w.x1) / 2, cy = (w.y0 + w.y1) / 2;
            if (cx >= g.x0 && cx <= g.x1 && cy >= g.y0 && cy <= g.y1) wordUsed_[i] = 0;
        }
        grids_.erase(grids_.begin() + static_cast<std::ptrdiff_t>(gi));
    }
    // The continuation for the next page: the lowest upright table, when no text follows it on
    // this page but running footers.
    if (dir_ == 0) continuation_.valid = false;
    const Grid* lowest = nullptr;
    for (const Grid& g : grids_)
        if (!lowest || g.y0 < lowest->y0) lowest = &g;
    if (lowest && dir_ == 0) {
        bool last = true;
        for (size_t i = 0; i < words_.size() && last; ++i)
            if (!words_[i].removed && !wordUsed_[i] && words_[i].base < lowest->y0) last = false;
        if (last) {
            continuation_.valid = true;
            continuation_.x0 = lowest->x0, continuation_.x1 = lowest->x1;
            continuation_.xs = lowest->xs;
        }
    }
    if (grids_.empty()) return;
    // Which grid and cell each glyph of each line falls in; lines mostly inside a grid move there.
    struct Piece {
        uint32_t grid, cell, line;
        float base, x0;
        uint32_t first, count;  // pieceGlyphs[first, first + count)
    };
    std::vector<Piece> pieces;
    std::vector<uint32_t> pieceGlyphs;
    std::vector<int32_t> glyphCell;  // per glyph of the current line: grid * 65536 + cell, or -1
    std::vector<Locator> locators;
    locators.reserve(grids_.size());
    for (const Grid& g : grids_) locators.emplace_back(g);
    const uint32_t lineCount = static_cast<uint32_t>(page_.lines.size());
    for (uint32_t li = 0; li < lineCount; ++li) {
        const Line& line = page_.lines[li];
        if (line.dir != dir_ || line.count == 0 || (line.flags & (kLineTableCell | kLineTableAnchor))) continue;
        bool near = false;
        for (const Grid& g : grids_)
            if (Overlaps(g, line.u0, line.vb, line.u1, line.vt)) near = true;
        if (!near) continue;
        glyphCell.assign(line.count, -1);
        uint32_t in = 0;
        for (uint32_t k = 0; k < line.count; ++k) {
            const Glyph& gl = page_.glyphs[page_.lineGlyphs[line.first + k]];
            const float cx = (gl.u0 + gl.u1) / 2, cy = (gl.vb + gl.vt) / 2;
            for (uint32_t gi = 0; gi < grids_.size(); ++gi) {
                const Grid& g = grids_[gi];
                int r = 0, c = 0;
                if (!locators[gi].Find(cx, cy, r, c)) continue;
                if (!g.labelRow.empty() && g.labelRow[static_cast<size_t>(r)]) c = 0;
                glyphCell[k] = static_cast<int32_t>(gi * 65536u + static_cast<uint32_t>(g.owner[static_cast<size_t>(r * g.Cols() + c)]));
                in++;
                break;
            }
        }
        if (in * 2 < line.count) continue;
        // Glyphs outside every cell follow their neighbour.
        for (uint32_t k = 1; k < line.count; ++k)
            if (glyphCell[k] < 0) glyphCell[k] = glyphCell[k - 1];
        for (uint32_t k = line.count - 1; k-- > 0;)
            if (glyphCell[k] < 0) glyphCell[k] = glyphCell[k + 1];
        if (line.flags & kLineRemoved) restored_++;
        page_.lines[li].flags = static_cast<uint16_t>((page_.lines[li].flags | kLineTableCell) & ~kLineRemoved);
        uint32_t k = 0;
        while (k < line.count) {
            const int32_t id = glyphCell[k];
            Piece p;
            p.grid = static_cast<uint32_t>(id) / 65536u;
            p.cell = static_cast<uint32_t>(id) % 65536u;
            p.line = li;
            p.base = line.baseline;
            p.first = static_cast<uint32_t>(pieceGlyphs.size());
            p.x0 = page_.glyphs[page_.lineGlyphs[line.first + k]].u0;
            while (k < line.count && glyphCell[k] == id) {
                pieceGlyphs.push_back(page_.lineGlyphs[line.first + k]);
                ++k;
            }
            p.count = static_cast<uint32_t>(pieceGlyphs.size()) - p.first;
            pieces.push_back(p);
        }
    }
    std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
        if (a.grid != b.grid) return a.grid < b.grid;
        if (a.cell != b.cell) return a.cell < b.cell;
        if (a.base != b.base) return a.base > b.base;
        return a.x0 < b.x0;
    });
    size_t at = 0;
    for (uint32_t gi = 0; gi < grids_.size(); ++gi) {
        const Grid& g = grids_[gi];
        Table t;
        t.x0 = g.x0, t.y0 = g.y0, t.x1 = g.x1, t.y1 = g.y1;
        t.rows = static_cast<uint16_t>(std::max(0, g.Rows()));
        t.cols = static_cast<uint16_t>(std::max(0, g.Cols()));
        t.headerRows = g.headerRows;
        t.kind = static_cast<uint8_t>(g.kind);
        t.firstCell = static_cast<uint32_t>(page_.tableCells.size());
        t.cellCount = static_cast<uint32_t>(g.cells.size());
        t.firstLine = static_cast<uint32_t>(page_.lines.size());
        float size = g.size;
        for (uint32_t ci = 0; ci < g.cells.size(); ++ci) {
            TableCell cell;
            cell.row = g.cells[ci].r, cell.col = g.cells[ci].c, cell.rowSpan = g.cells[ci].rs, cell.colSpan = g.cells[ci].cs;
            cell.first = static_cast<uint32_t>(page_.lines.size());
            while (at < pieces.size() && pieces[at].grid == gi && pieces[at].cell == ci) {
                const Piece& p = pieces[at++];
                const Line& src = page_.lines[p.line];
                Line l;
                l.baseline = src.baseline;
                l.size = src.size;
                l.dir = dir_;
                l.first = static_cast<uint32_t>(page_.lineGlyphs.size());
                l.count = p.count;
                l.u0 = INFINITY, l.u1 = -INFINITY, l.vb = INFINITY, l.vt = -INFINITY;
                uint16_t all = kGlyphBold | kGlyphItalic | kGlyphMonospace;
                for (uint32_t k = 0; k < p.count; ++k) {
                    const uint32_t gi2 = pieceGlyphs[p.first + k];
                    const Glyph& gl = page_.glyphs[gi2];
                    page_.lineGlyphs.push_back(gi2);
                    l.u0 = Fmin(l.u0, gl.u0), l.u1 = Fmax(l.u1, gl.u1);
                    l.vb = Fmin(l.vb, gl.vb), l.vt = Fmax(l.vt, gl.vt);
                    all &= gl.flags;
                }
                l.chars = p.count;
                l.flags = static_cast<uint16_t>(kLineTableCell | (src.flags & (kLineArtifact | kLineInvisible)));
                if (all & kGlyphBold) l.flags |= kLineAllBold;
                if (all & kGlyphItalic) l.flags |= kLineAllItalic;
                if (all & kGlyphMonospace) l.flags |= kLineAllMono;
                if (size <= 0) size = l.size;
                page_.lines.push_back(l);
            }
            cell.count = static_cast<uint32_t>(page_.lines.size()) - cell.first;
            page_.tableCells.push_back(cell);
        }
        while (at < pieces.size() && pieces[at].grid == gi) ++at;  // cells past the grid (none)
        t.lineCount = static_cast<uint32_t>(page_.lines.size()) - t.firstLine;
        Line anchor;
        anchor.u0 = t.x0, anchor.u1 = t.x1, anchor.vb = t.y0, anchor.vt = t.y1;
        anchor.size = size > 0 ? size : bodySize_;
        anchor.baseline = t.y1 - anchor.size;
        if (t.lineCount == 0) {
            // No text after all: nothing to write.
            page_.tableCells.resize(t.firstCell);
            continue;
        }
        anchor.first = static_cast<uint32_t>(page_.tables.size());
        anchor.count = 0;
        anchor.flags = kLineTableAnchor;
        anchor.dir = dir_;
        page_.tables.push_back(t);
        page_.lines.push_back(anchor);
    }
}

int Detector::Run() {
    // The scratch vectors keep the previous detector's contents: those not refilled before use
    // are emptied here.
    grids_.clear();
    rules_.clear();
    CollectWords();
    if (words_.empty()) {
        if (dir_ == 0) continuation_.valid = false;
        return 0;
    }
    CollectEdges(page_, dir_, h_, v_);
    BoxGrids();
    Lattices();
    RuleStacks();
    if (dir_ == 0) Continue();
    // Columns an em apart first; then, where the page sets its words close (dense tables of small
    // type), gaps down to 2.5 word spaces (0.6 em at least) for what is left.
    Borderless(1.0f);
    const float close = Fmax(0.6f, 2.5f * wordSpace_);
    if (close < 1.0f) Borderless(close);
    JoinStacked();
    if (dir_ == 0) RepeatedHeaders();
    DropChartText();
    Commit();
    return restored_;
}

void AppendHtmlEscaped(std::string_view t, std::string& out) {
    for (char c : t) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(c); break;
        }
    }
}

// A cell's text without the dot leaders that run its label to the next column ("Short ......"):
// typography, not text. Three dots or more, or ellipses, with spaces, at the end of a cell that has
// text before them.
std::string_view TrimLeaders(std::string_view t) {
    size_t end = t.size(), dots = 0;
    while (end > 0) {
        if (t[end - 1] == '.') {
            dots++, end--;
        } else if (t[end - 1] == ' ') {
            end--;
        } else if (end >= 3 && t.substr(end - 3, 3) == "\xE2\x80\xA6") {  // U+2026
            dots += 3, end -= 3;
        } else {
            break;
        }
    }
    return dots >= 3 && end > 0 ? t.substr(0, end) : t;
}

// A cell's plain text: its lines joined by spaces, a line-end hyphen before a lowercase letter
// joining the halves of the word.
void CellPlain(const PageData& page, const TableCell& c, std::string& out, std::string& scratch) {
    out.clear();
    for (uint32_t l = c.first; l < c.first + c.count; ++l) {
        LineText(page, page.lines[l], scratch);
        if (l > c.first) {
            const unsigned char n0 = scratch.empty() ? 0 : static_cast<unsigned char>(scratch[0]);
            if (out.size() >= 2 && out.back() == '-' && n0 >= 'a' && n0 <= 'z' &&
                static_cast<unsigned char>(out[out.size() - 2]) >= 'a')
                out.pop_back();
            else
                out.push_back(' ');
        }
        out += scratch;
    }
    out.resize(TrimLeaders(out).size());
}

// A cell's Markdown without emphasis that covers the whole cell: a bold header row or first column,
// an italic total, is the table's styling, not emphasis inside text (WriteGfmTable strips it from
// header cells and from cells that share it with another cell of their row or column). Emphasis
// on part of a cell stays, and so do stars that open or close nothing (written raw: "** x", a
// space inside them).
std::string_view CellStyleless(std::string_view t) {
    if (t.size() < 3 || t[0] != '*') return t;  // every delimiter is stars, one at least on each side
    for (const std::string_view d : {"***", "**", "*"}) {
        if (t.size() <= 2 * d.size() || t.substr(0, d.size()) != d || t.substr(t.size() - d.size()) != d) continue;
        const std::string_view inner = t.substr(d.size(), t.size() - 2 * d.size());
        bool other = inner.front() == ' ' || inner.back() == ' ';
        for (size_t i = 0; i < inner.size() && !other; ++i) other = inner[i] == '*' && (i == 0 || inner[i - 1] != '\\');
        return other ? t : inner;
    }
    return t;
}

} // namespace

uint16_t RepeatedHeader(const TablePart& head, const TablePart& part) {
    const uint16_t n = std::max<uint16_t>(1, head.table->headerRows);
    if (part.table->rows <= n || head.table->rows < n || part.table->cols != head.table->cols) return 0;
    std::string a, b, scratch;
    for (uint32_t i = 0; i < head.table->cellCount; ++i) {
        const TableCell& hc = head.page->tableCells[head.table->firstCell + i];
        if (hc.row >= n) break;
        if (i >= part.table->cellCount) return 0;
        const TableCell& pc = part.page->tableCells[part.table->firstCell + i];
        if (pc.row != hc.row || pc.col != hc.col || pc.colSpan != hc.colSpan || pc.rowSpan != hc.rowSpan) return 0;
        CellPlain(*head.page, hc, a, scratch);
        CellPlain(*part.page, pc, b, scratch);
        if (a != b) return 0;
    }
    return n;
}

void Detector::ReportGaps(const std::vector<Row>& rows, const std::vector<uint32_t>& rowWords, size_t begin, size_t end,
                          const std::vector<Gap>& gaps, const std::vector<int>& columns,
                          const std::vector<uint8_t>& label, float em, bool secondPass, bool chosen) {
    const size_t m = gaps.size() + 1;
    const uint32_t base = rows[begin].first;
    // Per column: cells filled and numeric, and the left and right edges of each cell.
    std::vector<int> filled(m, 0), numeric(m, 0);
    std::vector<std::vector<float>> lefts(m), rights(m);
    std::vector<float> a(m), b(m);
    std::vector<uint32_t> digits(m), letters(m);
    int counted = 0;
    for (size_t q = begin; q < end; ++q) {
        if (label[q]) continue;
        counted++;
        std::fill(a.begin(), a.end(), INFINITY);
        std::fill(b.begin(), b.end(), -INFINITY);
        std::fill(digits.begin(), digits.end(), 0u);
        std::fill(letters.begin(), letters.end(), 0u);
        for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count; ++k) {
            const int c = columns[k - base];
            if (c < 0 || static_cast<size_t>(c) >= m) continue;
            const TWord& w = words_[rowWords[k]];
            a[c] = Fmin(a[c], w.x0), b[c] = Fmax(b[c], w.x1);
            const WordChars& t = Chars(w);
            digits[c] += t.digits, letters[c] += t.letters;
        }
        for (size_t c = 0; c < m; ++c) {
            if (!(b[c] > a[c])) continue;
            filled[c]++;
            numeric[c] += NumericCounts(digits[c], letters[c]);
            lefts[c].push_back(a[c]);
            rights[c].push_back(b[c]);
        }
    }
    std::vector<float> spread;
    float f[kTableGapFeatures];
    for (size_t g = 0; g < gaps.size(); ++g) {
        const float width = gaps[g].b - gaps[g].a;
        int rank = 0;
        for (size_t o = 0; o < gaps.size(); ++o) {
            const float other = gaps[o].b - gaps[o].a;
            rank += other > width || (other == width && o < g);
        }
        int inked = 0;
        for (size_t q = begin; q < end; ++q) {
            bool ink = false;
            for (uint32_t k = rows[q].first; k < rows[q].first + rows[q].count && !ink; ++k)
                ink = words_[rowWords[k]].x0 < gaps[g].b && words_[rowWords[k]].x1 > gaps[g].a;
            inked += ink;
        }
        f[0] = width / em;
        f[1] = float(inked) / float(end - begin);
        spread = rights[g];
        f[2] = MedianDeviation(spread) / em;
        spread = lefts[g + 1];
        f[3] = MedianDeviation(spread) / em;
        f[4] = filled[g] ? float(numeric[g]) / float(filled[g]) : 0;
        f[5] = filled[g + 1] ? float(numeric[g + 1]) / float(filled[g + 1]) : 0;
        f[6] = counted ? float(filled[g] + filled[g + 1]) / float(2 * counted) : 0;
        f[7] = float(rank);
        f[8] = float(end - begin);
        f[9] = em / bodySize_;
        f[10] = secondPass ? 1 : 0;
        f[11] = chosen ? 1 : 0;
        features_->tableGap(features_->context, static_cast<uint32_t>(page_.number), f, gaps[g].a, gaps[g].b, chosen);
    }
}

int DetectTables(PageData& page, TableContinuation& continuation, const FeatureSink* features) {
    if (page.lines.empty()) {
        continuation.valid = false;
        return 0;
    }
    int restored = Detector(page, continuation, 0, features).Run();
    // Tables turned on the page (a landscape table on a portrait page): each other direction with
    // lines enough for a table of its own, in its frame.
    uint32_t turned[4] = {0, 0, 0, 0};
    for (const Line& l : page.lines)
        if (l.dir < 4 && l.count > 0 && !(l.flags & (kLineTableCell | kLineTableAnchor))) turned[l.dir]++;
    for (uint8_t dir = 1; dir < 4; ++dir) {
        if (turned[dir] < 6) continue;
        TableContinuation none;
        Detector other(page, none, dir);
        restored += other.Run();
    }
    // A huge page's words leave the thread's scratch with it.
    TableScratch& scratch = PerThread<TableScratch>();
    if (scratch.words.capacity() > kKeptWords) scratch = TableScratch();
    return restored;
}

bool TableContinues(const std::vector<PageData>& pages, const std::vector<BlockLine>& lines, const Block& previous,
                    uint32_t page, uint32_t table) {
    if (page == 0 || page >= pages.size() || table >= pages[page].tables.size()) return false;
    const Table& next = pages[page].tables[table];
    for (uint32_t i = previous.first + previous.count; i-- > previous.first;) {
        const BlockLine& bl = lines[i];
        const Line& line = pages[bl.page].lines[bl.line];
        if (!(line.flags & kLineTableAnchor)) continue;
        const Table& last = pages[bl.page].tables[line.first];
        return bl.page + 1 == page && last.cols == next.cols;
    }
    return false;
}

bool TableNeedsHtml(const PageData& page, const Table& table) {
    // A pipe table writes a spanning cell's text in the first row it spans and, across columns, in
    // each column it spans. That fits labels over sub-columns in the header rows; a header label
    // spanning down beside the sub-labels under its neighbours, which stands in the first header
    // row with the cell below it empty; and a cell of a body row spanning columns (a section's
    // name across the table) under a header of one row. A cell without text spans nothing a
    // reader would miss. HTML keeps what a pipe table would lose: a body cell spanning rows (the
    // label's grouping of the rows) and a body cell spanning columns under a header of several
    // rows (the header's levels go too).
    const uint16_t header = std::max<uint16_t>(1, table.headerRows);
    bool bodySpan = false;
    for (uint32_t i = 0; i < table.cellCount; ++i) {
        const TableCell& c = page.tableCells[table.firstCell + i];
        if (c.count == 0) continue;
        if (c.rowSpan > 1 && c.row >= header) return true;
        bodySpan = bodySpan || (c.colSpan > 1 && c.row >= header);
    }
    return bodySpan && table.headerRows >= 2;
}

void WriteGfmTable(const std::vector<TablePart>& parts, std::string& out, bool repeatColumnSpans) {
    if (parts.empty()) return;
    const uint16_t cols = parts[0].table->cols;
    bool first = true;
    const auto put = [&](std::string_view text) {
        for (char ch : text) {
            if (ch == '|') out.push_back('\\');
            out.push_back(ch == '\n' ? ' ' : ch);
        }
    };
    std::vector<uint8_t> whole;            // per cell of a part: the delimiter length of emphasis over all of it
    std::vector<uint16_t> perRow, perCol;  // per row and column and delimiter length: cells with it
    for (size_t p = 0; p < parts.size(); ++p) {
        const TablePart& part = parts[p];
        const Table& table = *part.table;
        const uint16_t skip = p > 0 ? RepeatedHeader(parts[0], part) : 0;
        // Emphasis over a whole cell is the table's styling in a header row, in a row of one
        // cell (a section's label) and where another cell of its row or column has it too (a bold
        // column of labels, an italic total): it goes. A body cell beside plain cells, alone in
        // its row and column with it (a word set bold in a column of examples), keeps it.
        const uint16_t header = p == 0 ? std::max<uint16_t>(1, table.headerRows) : table.headerRows;
        whole.assign(table.cellCount, 0);
        perRow.assign(size_t(table.rows) * 4, 0);  // body cells; [0]: plain cells with text
        perCol.assign(size_t(cols) * 4, 0);
        for (uint32_t k = 0; k < table.cellCount; ++k) {
            const TableCell& tc = part.page->tableCells[table.firstCell + k];
            const std::string_view t = part.cellText[k];
            whole[k] = static_cast<uint8_t>((t.size() - CellStyleless(t).size()) / 2);
            if ((whole[k] == 0 && TrimLeaders(t).empty()) || tc.row < header || tc.row >= table.rows || tc.col >= cols) continue;
            perRow[size_t(tc.row) * 4 + whole[k]]++;
            perCol[size_t(tc.col) * 4 + whole[k]]++;
        }
        const auto styleless = [&](uint32_t k) {
            const TableCell& tc = part.page->tableCells[table.firstCell + k];
            const std::string_view t = part.cellText[k];
            if (whole[k] == 0 || tc.row < header || tc.row >= table.rows || tc.col >= cols || perRow[size_t(tc.row) * 4] == 0 ||
                perRow[size_t(tc.row) * 4 + whole[k]] >= 2 || perCol[size_t(tc.col) * 4 + whole[k]] >= 2)
                return t.substr(whole[k], t.size() - 2u * whole[k]);  // CellStyleless(t), measured above
            return t;
        };
        uint32_t i = 0;
        for (uint16_t r = 0; r < table.rows; ++r) {
            const bool write = r >= skip;
            if (write) {
                if (!first) out.push_back('\n');
                out += "|";
            }
            std::string_view spanned;  // the text of a cell spanning the columns after it
            uint16_t spanLeft = 0;
            for (uint16_t c = 0; c < cols; ++c) {
                if (write) out.push_back(' ');
                if (i < table.cellCount && part.page->tableCells[table.firstCell + i].row == r &&
                    part.page->tableCells[table.firstCell + i].col == c) {
                    const std::string_view text = TrimLeaders(styleless(i));
                    if (write) put(text);
                    const uint16_t span = part.page->tableCells[table.firstCell + i].colSpan;
                    spanned = text, spanLeft = span > 1 ? static_cast<uint16_t>(span - 1) : 0;
                    ++i;
                } else if (spanLeft > 0) {
                    if (write && repeatColumnSpans) put(spanned);
                    --spanLeft;
                }
                if (write) out += " |";
            }
            if (write && first) {
                out += "\n|";
                for (uint16_t c = 0; c < cols; ++c) out += " --- |";
                first = false;
            }
        }
    }
}

void WriteHtmlTable(const std::vector<TablePart>& parts, std::string& out, bool spaced) {
    if (parts.empty()) return;
    const uint16_t header = std::max<uint16_t>(1, parts[0].table->headerRows);
    std::string text, scratch;
    out += "<table>";
    for (size_t p = 0; p < parts.size(); ++p) {
        const PageData& page = *parts[p].page;
        const Table& table = *parts[p].table;
        const uint16_t skip = p > 0 ? RepeatedHeader(parts[0], parts[p]) : 0;
        uint32_t i = 0;
        for (uint16_t r = 0; r < table.rows; ++r) {
            const bool write = r >= skip;
            const char* tag = p == 0 && r < header ? "th" : "td";
            if (write) out += "\n<tr>";
            while (i < table.cellCount && page.tableCells[table.firstCell + i].row == r) {
                const TableCell& c = page.tableCells[table.firstCell + i++];
                if (!write) continue;
                if (spaced) out.push_back(' ');
                out.push_back('<');
                out += tag;
                if (c.colSpan > 1) out += " colspan=\"" + std::to_string(c.colSpan) + "\"";
                if (c.rowSpan > 1) out += " rowspan=\"" + std::to_string(c.rowSpan) + "\"";
                out.push_back('>');
                CellPlain(page, c, text, scratch);
                if (spaced) out.push_back(' ');
                AppendHtmlEscaped(text, out);
                if (spaced && !text.empty()) out.push_back(' ');
                out += "</";
                out += tag;
                out.push_back('>');
            }
            if (write) out += spaced ? " </tr>" : "</tr>";
        }
    }
    out += "\n</table>";
}

} // namespace RayoPdf::Layout
