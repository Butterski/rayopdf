// SPDX-License-Identifier: Apache-2.0
#include "layout/reading_order.h"

#include "layout/lines.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace RayoPdf::Layout {
namespace {

// The repetition key of a header or footer line (§5.2): ASCII lowercased, digit runs and lowercase
// roman-numeral words as '#', white space collapsed.
void RunningKey(const std::string& text, std::string& key) {
    key.clear();
    size_t i = 0;
    bool space = false;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ') {
            space = !key.empty();
            ++i;
            continue;
        }
        if (space) key.push_back(' '), space = false;
        if (c >= '0' && c <= '9') {
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
            key.push_back('#');
            continue;
        }
        // A word of roman numerals only.
        if ((key.empty() || key.back() == ' ') && (c == 'i' || c == 'v' || c == 'x' || c == 'I' || c == 'V' || c == 'X')) {
            size_t j = i;
            while (j < text.size() && std::string_view("ivxlcIVXLC").find(text[j]) != std::string_view::npos) ++j;
            if (j - i <= 6 && (j == text.size() || text[j] == ' ')) {
                key.push_back('#');
                i = j;
                continue;
            }
        }
        key.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
        ++i;
    }
}

// A page number line: "#", "- # -", "page #", "# of #", "# / #", "[#]" ...
bool IsPageNumberKey(const std::string& key) {
    bool number = false;
    size_t i = 0;
    while (i < key.size()) {
        const char c = key[i];
        if (c == '#') {
            number = true;
            ++i;
            continue;
        }
        if (key.compare(i, 4, "page") == 0) {
            i += 4;
            continue;
        }
        if (key.compare(i, 2, "of") == 0) {
            i += 2;
            continue;
        }
        if (key.compare(i, 1, "p") == 0 && (i + 1 == key.size() || key[i + 1] == '.' || key[i + 1] == ' ')) {
            ++i;
            continue;
        }
        if (std::string_view(" -/|.,:()[]").find(c) != std::string_view::npos) {
            ++i;
            continue;
        }
        // En and em dashes.
        if (key.compare(i, 3, "\xE2\x80\x93") == 0 || key.compare(i, 3, "\xE2\x80\x94") == 0) {
            i += 3;
            continue;
        }
        return false;
    }
    return number;
}

struct KeyCount {
    int total = 0;
    int odd = 0;
    int even = 0;
    int lastPage = -1;
};

// A horizontal rule of the page: a thin filled rectangle or a stroked horizontal line.
struct Rule {
    float y, x0, x1;
};

struct Orderer {
    PageData& page;
    std::vector<uint32_t>& out;
    std::vector<Rule> rules;
    std::vector<uint32_t> scratch;
    std::vector<float> gaps_;

    const Line& L(uint32_t i) const { return page.lines[i]; }

    void CollectRules() {
        rules.clear();
        for (const Shape& s : page.shapes) {
            if (s.kind == ShapeKind::Other || s.y1 - s.y0 > 3 || s.x1 - s.x0 < 20) continue;
            if (rules.size() >= 4096) break;
            rules.push_back(Rule{(s.y0 + s.y1) / 2, s.x0, s.x1});
        }
    }

    // Horizontal rules running across a region that also has column gaps make it a ruled table
    // (algorithms §6.7): it is read row by row, cutting at the rules, and its lines are marked.
    bool SplitAtRules(std::vector<uint32_t>& items, std::vector<size_t>& cuts) {
        cuts.clear();
        if (rules.empty() || L(items[0]).dir != 0) return false;
        float u0 = INFINITY, u1 = -INFINITY, vb = INFINITY, vt = -INFINITY;
        for (uint32_t i : items) {
            u0 = std::fmin(u0, L(i).u0), u1 = std::fmax(u1, L(i).u1);
            vb = std::fmin(vb, L(i).vb), vt = std::fmax(vt, L(i).vt);
        }
        const float width = u1 - u0;
        std::sort(items.begin(), items.end(), [&](uint32_t a, uint32_t b) {
            if (L(a).vt != L(b).vt) return L(a).vt > L(b).vt;
            return L(a).u0 < L(b).u0;
        });
        float bottom = L(items[0]).vb;
        for (size_t k = 1; k < items.size(); ++k) {
            const float top = L(items[k]).vt;
            for (const Rule& r : rules) {
                if (r.y <= vb || r.y >= vt) continue;
                if (std::fmin(r.x1, u1) - std::fmax(r.x0, u0) < 0.6f * width) continue;
                if (r.y >= top - 1 && r.y <= bottom + 1) {
                    cuts.push_back(k);
                    break;
                }
            }
            bottom = std::fmin(bottom, L(items[k]).vb);
        }
        if (cuts.empty()) return false;
        for (uint32_t i : items) page.lines[i].flags |= kLineTable;
        return true;
    }

    float MedianSize(const std::vector<uint32_t>& items) {
        scratch.assign(items.begin(), items.end());
        auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
        std::nth_element(scratch.begin(), mid, scratch.end(),
                         [&](uint32_t a, uint32_t b) { return L(a).size < L(b).size; });
        return L(*mid).size;
    }

    // Rows top to bottom (baselines within 0.4 em), each left to right.
    void Fallback(std::vector<uint32_t>& items) {
        std::sort(items.begin(), items.end(), [&](uint32_t a, uint32_t b) {
            if (L(a).baseline != L(b).baseline) return L(a).baseline > L(b).baseline;
            return L(a).u0 < L(b).u0;
        });
        size_t at = 0;
        while (at < items.size()) {
            const Line& head = L(items[at]);
            size_t end = at + 1;
            while (end < items.size() && head.baseline - L(items[end]).baseline <= 0.4f * head.size) ++end;
            std::sort(items.begin() + static_cast<std::ptrdiff_t>(at), items.begin() + static_cast<std::ptrdiff_t>(end),
                      [&](uint32_t a, uint32_t b) { return L(a).u0 < L(b).u0; });
            at = end;
        }
        out.insert(out.end(), items.begin(), items.end());
    }

    // Groups of `items` split at vertical whitespace gaps of at least `minGap` (x projection).
    void SplitX(std::vector<uint32_t>& items, float minGap, std::vector<size_t>& cuts) {
        cuts.clear();
        std::sort(items.begin(), items.end(), [&](uint32_t a, uint32_t b) { return L(a).u0 < L(b).u0; });
        float right = L(items[0]).u1;
        for (size_t k = 1; k < items.size(); ++k) {
            if (L(items[k]).u0 - right >= minGap) cuts.push_back(k);
            right = std::fmax(right, L(items[k]).u1);
        }
    }

    // Like SplitX, on extents shrunk by up to 0.7 em at both ends: punctuation hanging into a
    // narrow gutter (LaTeX protrudes quotes and hyphens) closes the exact gap.
    void SplitXSoft(std::vector<uint32_t>& items, float size, std::vector<size_t>& cuts) {
        cuts.clear();
        const auto shrink = [&](uint32_t i) { return std::fmin(0.7f * size, 0.25f * (L(i).u1 - L(i).u0)); };
        std::sort(items.begin(), items.end(),
                  [&](uint32_t a, uint32_t b) { return L(a).u0 + shrink(a) < L(b).u0 + shrink(b); });
        float right = L(items[0]).u1 - shrink(items[0]);
        for (size_t k = 1; k < items.size(); ++k) {
            if (L(items[k]).u0 + shrink(items[k]) - right >= 1.0f) cuts.push_back(k);
            right = std::fmax(right, L(items[k]).u1 - shrink(items[k]));
        }
    }

    // Groups split at horizontal whitespace gaps (y projection), top first. Only the widest gaps
    // (at least half the widest) cut at one level, as XY-cut cuts at the widest valley: cutting at
    // every narrow gap between aligned rows would slice through columns that the next level
    // separates.
    void SplitY(std::vector<uint32_t>& items, std::vector<size_t>& cuts) {
        cuts.clear();
        std::sort(items.begin(), items.end(), [&](uint32_t a, uint32_t b) {
            if (L(a).vt != L(b).vt) return L(a).vt > L(b).vt;
            return L(a).u0 < L(b).u0;
        });
        gaps_.clear();
        float bottom = L(items[0]).vb;
        float widest = 0;
        for (size_t k = 1; k < items.size(); ++k) {
            const float gap = bottom - L(items[k]).vt;
            if (gap > 0) {
                cuts.push_back(k);
                gaps_.push_back(gap);
                widest = std::fmax(widest, gap);
            }
            bottom = std::fmin(bottom, L(items[k]).vb);
        }
        size_t kept = 0;
        for (size_t c = 0; c < cuts.size(); ++c)
            if (gaps_[c] >= 0.5f * widest) cuts[kept++] = cuts[c];
        cuts.resize(kept);
    }

    // Columns of running text: at least two groups of several lines that mostly fill their width
    // (Breuel's gutters are bounded by text on both sides); table columns of short cells are not.
    bool ColumnsLike(const std::vector<uint32_t>& items, const std::vector<size_t>& cuts, float size) {
        int texty = 0;
        size_t begin = 0;
        std::vector<float> widths;
        for (size_t c = 0; c <= cuts.size(); ++c) {
            const size_t end = c < cuts.size() ? cuts[c] : items.size();
            if (end - begin >= 3) {
                float u0 = L(items[begin]).u0, u1 = L(items[begin]).u1;
                widths.clear();
                for (size_t k = begin; k < end; ++k) {
                    u0 = std::fmin(u0, L(items[k]).u0);
                    u1 = std::fmax(u1, L(items[k]).u1);
                    widths.push_back(L(items[k]).u1 - L(items[k]).u0);
                }
                std::nth_element(widths.begin(), widths.begin() + static_cast<std::ptrdiff_t>(widths.size() / 2), widths.end());
                const float median = widths[widths.size() / 2];
                if (u1 - u0 >= 8 * size && median >= 0.6f * (u1 - u0)) texty++;
            }
            begin = end;
        }
        return texty >= 2;
    }

    void Cut(std::vector<uint32_t>& items, int depth) {
        if (items.size() <= 1 || depth > 48) {
            Fallback(items);
            return;
        }
        const float size = MedianSize(items);
        std::vector<size_t> xCuts, yCuts;
        std::vector<uint32_t> byY(items);
        SplitY(byY, yCuts);
        // LaTeX's 10 pt gutter narrows to 7 pt where hyphens protrude: half an em suffices, since
        // word gaps never reach this stage (lines are whole runs of words).
        SplitX(items, std::fmax(3.0f, 0.5f * size), xCuts);
        if (!xCuts.empty() && (yCuts.empty() || ColumnsLike(items, xCuts, size))) {
            std::vector<size_t> ruleCuts;
            std::vector<uint32_t> byRule(items);
            if (SplitAtRules(byRule, ruleCuts)) {
                Recurse(byRule, ruleCuts, depth);
                return;
            }
            Recurse(items, xCuts, depth);
            return;
        }
        if (xCuts.empty() && items.size() >= 6) {
            std::vector<uint32_t> soft(items);
            SplitXSoft(soft, size, xCuts);
            if (!xCuts.empty() && ColumnsLike(soft, xCuts, size)) {
                Recurse(soft, xCuts, depth);
                return;
            }
        }
        if (!yCuts.empty()) {
            Recurse(byY, yCuts, depth);
        } else {
            Fallback(items);
        }
    }

    void Recurse(const std::vector<uint32_t>& items, const std::vector<size_t>& cuts, int depth) {
        size_t begin = 0;
        std::vector<uint32_t> part;
        for (size_t c = 0; c <= cuts.size(); ++c) {
            const size_t end = c < cuts.size() ? cuts[c] : items.size();
            part.assign(items.begin() + static_cast<std::ptrdiff_t>(begin), items.begin() + static_cast<std::ptrdiff_t>(end));
            Cut(part, depth + 1);
            begin = end;
        }
    }

    // Cross-layout lines (XY-Cut++ §4 step 2): a line overlapping, along the baseline, two nearby
    // lines that do not overlap each other and are each at least a quarter of the content wide
    // spans a column gutter. Such lines cut the page into bands read one after another.
    void Order(std::vector<uint32_t>& items) {
        if (items.size() <= 1 || items.size() > 4000) {
            Cut(items, 0);
            return;
        }
        float left = L(items[0]).u0, right = L(items[0]).u1;
        for (uint32_t i : items) left = std::fmin(left, L(i).u0), right = std::fmax(right, L(i).u1);
        const float wide = 0.25f * (right - left);
        std::vector<uint32_t> separators, rest;
        for (uint32_t f : items) {
            const Line& lf = L(f);
            const float window = 6 * lf.size;
            float minRight = INFINITY, maxLeft = -INFINITY;
            for (uint32_t g : items) {
                if (g == f) continue;
                const Line& lg = L(g);
                if (lg.u1 - lg.u0 < wide || lg.u1 <= lf.u0 || lg.u0 >= lf.u1) continue;
                if (std::fabs(lg.baseline - lf.baseline) > window) continue;
                minRight = std::fmin(minRight, lg.u1);
                maxLeft = std::fmax(maxLeft, lg.u0);
            }
            if (maxLeft > minRight)
                separators.push_back(f);
            else
                rest.push_back(f);
        }
        if (separators.empty()) {
            Cut(items, 0);
            return;
        }
        std::sort(separators.begin(), separators.end(), [&](uint32_t a, uint32_t b) {
            if (L(a).baseline != L(b).baseline) return L(a).baseline > L(b).baseline;
            return L(a).u0 < L(b).u0;
        });
        // Band k holds the lines above separator k and below separator k - 1; a line on a
        // separator's baseline follows it.
        std::vector<std::vector<uint32_t>> bands(separators.size() + 1);
        for (uint32_t r : rest) {
            const Line& lr = L(r);
            size_t k = 0;
            while (k < separators.size() && lr.baseline < L(separators[k]).baseline + 0.3f * lr.size) ++k;
            bands[k].push_back(r);
        }
        for (size_t k = 0; k < bands.size(); ++k) {
            Cut(bands[k], 0);
            if (k < separators.size()) out.push_back(separators[k]);
        }
    }
};

// Single-page evidence (§5.2), for documents of any length: the outermost row of text at the top
// or the bottom of a page is a running header, footer or page number when it is set apart from
// the rest by at least an em of the body size, is no larger than the body, is short, starts with
// no footnote marker, and either sits in the page's outer margin (kMarginBand) or reads as a page
// number inside the running band. A second row behind a removed one gets the same test. Pages
// with fewer than kEdgeMinLines other lines keep everything.
// The outer 6 % of the page height: Word's headers and footers at 0.5 in sit at about 5 %, LaTeX's
// page number at 5 %. Body text can start that high too (RayoMD's compact layout at 5 %), hence
// the gap, size and heading conditions.
constexpr float kMarginBand = 0.06f;
constexpr uint32_t kEdgeMinLines = 3;
constexpr uint32_t kEdgeMaxChars = 150;

int RemoveEdgeRows(PageData& page, float body, std::string& text, std::string& key) {
    std::vector<uint32_t> lines;
    for (uint32_t i = 0; i < page.lines.size(); ++i) {
        const Line& l = page.lines[i];
        if (l.dir == 0 && l.count > 0 && !(l.flags & kLineRemoved)) lines.push_back(i);
    }
    if (lines.size() < kEdgeMinLines + 1 || body <= 0) return 0;
    std::sort(lines.begin(), lines.end(), [&](uint32_t a, uint32_t b) {
        return page.lines[a].vb + page.lines[a].vt > page.lines[b].vb + page.lines[b].vt;
    });
    const float h = page.height;
    int removed = 0;
    size_t first = 0, last = lines.size();  // the lines not removed: lines[first, last)
    for (int side = 0; side < 2; ++side) {
        for (int row = 0; row < 2 && last - first > kEdgeMinLines; ++row) {
            const Line& edge = page.lines[lines[side == 0 ? first : last - 1]];
            // The row: lines on the edge line's baseline.
            size_t count = 0;
            float inner = side == 0 ? INFINITY : -INFINITY;  // the row's inner edge
            uint32_t chars = 0;
            float largest = 0;
            bool superscript = false;
            for (size_t k = 0; k < last - first; ++k) {
                const Line& l = page.lines[lines[side == 0 ? first + k : last - 1 - k]];
                if (std::fabs(l.baseline - edge.baseline) > 0.3f * std::fmax(l.size, edge.size)) break;
                count++;
                inner = side == 0 ? std::fmin(inner, l.vb) : std::fmax(inner, l.vt);
                chars += l.chars;
                largest = std::fmax(largest, l.size);
                superscript = superscript || IsSuperscript(page.glyphs[page.lineGlyphs[l.first]], l);
                // A row starting with a list marker or enumerator ("5. The last note") is content.
                uint32_t end = 1, value = 0;
                while (end < l.count && end < 8 && !(page.glyphs[page.lineGlyphs[l.first + end]].flags & kGlyphSpaceBefore))
                    ++end;
                if (end < l.count) {
                    text.clear();
                    for (uint32_t g = 0; g < end; ++g) text.append(page.GlyphText(page.glyphs[page.lineGlyphs[l.first + g]]));
                    superscript = superscript || ParseMarker(text, value) != Marker::None;
                }
            }
            if (last - first - count < kEdgeMinLines) break;
            // The rest's outer edge toward this side.
            float outer = side == 0 ? -INFINITY : INFINITY;
            float nextSize = 0;  // the size of the line the gap separates the row from
            for (size_t k = count; k < last - first; ++k) {
                const Line& l = page.lines[lines[side == 0 ? first + k : last - 1 - k]];
                const float e = side == 0 ? l.vt : l.vb;
                if (side == 0 ? e > outer : e < outer) outer = e, nextSize = l.size;
            }
            const float gap = side == 0 ? inner - outer : outer - inner;
            const float center = (edge.vb + edge.vt) / 2;
            const bool margin = side == 0 ? center > h * (1 - kMarginBand) : center < h * kMarginBand;
            const bool band = side == 0 ? center > h * (1 - kRunningBand) : center < h * kRunningBand;
            bool number = false;
            if (count == 1 && band) {
                LineText(page, edge, text);
                RunningKey(text, key);
                number = IsPageNumberKey(key);
            }
            // A body-size row above a heading is the document's first paragraph, set high on a page
            // with narrow margins: a header must be smaller than the body to be told from it.
            const bool beforeHeading = side == 0 && nextSize >= 1.12f * body && largest > 0.95f * body;
            const bool qualifies = gap >= body && !superscript && chars <= kEdgeMaxChars &&
                                   ((number && largest <= 1.3f * body) ||
                                    (margin && largest <= 1.02f * body && !beforeHeading));
            if (!qualifies) break;
            for (size_t k = 0; k < count; ++k) {
                page.lines[lines[side == 0 ? first + k : last - 1 - k]].flags |= kLineRemoved;
                removed++;
            }
            if (side == 0)
                first += count;
            else
                last -= count;
        }
    }
    return removed;
}

} // namespace

int RemoveRunningLines(std::vector<PageData>& pages) {
    const int n = static_cast<int>(pages.size());
    std::unordered_map<std::string, KeyCount> counts;
    std::string text, key;
    // Keys of the band lines, per page, to apply the decision afterwards.
    struct Candidate {
        uint32_t page;
        uint32_t line;
        std::string key;
        bool pageNumber;
        bool artifact;
    };
    std::vector<Candidate> candidates;
    for (int p = 0; p < n; ++p) {
        PageData& page = pages[static_cast<size_t>(p)];
        const float h = page.height;
        // A lone number is a page number only as the page's outermost line.
        float highest = -INFINITY, lowest = INFINITY;
        for (const Line& line : page.lines) {
            if (line.dir != 0) continue;
            highest = std::fmax(highest, (line.vb + line.vt) / 2);
            lowest = std::fmin(lowest, (line.vb + line.vt) / 2);
        }
        for (uint32_t li = 0; li < page.lines.size(); ++li) {
            const Line& line = page.lines[li];
            if (line.dir != 0) continue;
            const float center = (line.vb + line.vt) / 2;
            const bool top = center > h * (1 - kRunningBand);
            const bool bottom = center < h * kRunningBand;
            if (!top && !bottom) continue;
            LineText(page, line, text);
            RunningKey(text, key);
            if (key.empty()) continue;
            const bool outermost = top ? highest - center <= 1.5f * line.size : center - lowest <= 1.5f * line.size;
            Candidate c{static_cast<uint32_t>(p), li, (top ? "T" : "B") + key, outermost && IsPageNumberKey(key),
                        (line.flags & kLineArtifact) != 0};
            KeyCount& kc = counts[c.key];
            if (kc.lastPage != p) {
                kc.lastPage = p;
                kc.total++;
                (p % 2 == 0 ? kc.odd : kc.even)++;
            }
            candidates.push_back(std::move(c));
        }
    }
    const int odd = (n + 1) / 2, even = n / 2;
    int removed = 0;
    for (const Candidate& c : candidates) {
        const KeyCount& kc = counts[c.key];
        // A single page gives no evidence that a number or an artifact at its edge is a running
        // header, footer or page number.
        bool running = (c.pageNumber || c.artifact) && n >= 2;
        if (n >= 2 && kc.total >= std::max(2, (n + 1) / 2)) running = true;
        if (n >= 4 && kc.odd >= std::max(2, (odd + 1) / 2) && kc.odd * 2 > kc.total) running = true;
        if (n >= 4 && kc.even >= std::max(2, (even + 1) / 2) && kc.even * 2 > kc.total) running = true;
        if (!running) continue;
        Line& line = pages[c.page].lines[c.line];
        if (!(line.flags & kLineRemoved)) {
            line.flags |= kLineRemoved;
            removed++;
        }
    }
    // The body size (algorithms §6.1: the most common size by characters) for the edge test.
    std::unordered_map<int, uint64_t> sizeChars;
    for (const PageData& page : pages)
        for (const Line& line : page.lines)
            if (line.dir == 0) sizeChars[static_cast<int>(std::lround(line.size * 2))] += line.chars;
    int bodyKey = 0;
    uint64_t bodyCount = 0;
    for (const auto& kv : sizeChars)
        if (kv.second > bodyCount || (kv.second == bodyCount && kv.first < bodyKey))
            bodyKey = kv.first, bodyCount = kv.second;
    for (PageData& page : pages) removed += RemoveEdgeRows(page, float(bodyKey) / 2, text, key);
    return removed;
}

void OrderLines(PageData& page) {
    page.order.clear();
    Orderer orderer{page, page.order, {}, {}, {}};
    orderer.CollectRules();
    std::vector<uint32_t> items;
    for (uint8_t dir = 0; dir < 4; ++dir) {
        items.clear();
        for (uint32_t i = 0; i < page.lines.size(); ++i)
            if (page.lines[i].dir == dir && !(page.lines[i].flags & (kLineRemoved | kLineTableCell | kLineImage)))
                items.push_back(i);
        if (!items.empty()) orderer.Order(items);
    }
    // Images join the text's order without changing it: before the first line read below their
    // top edge that shares their horizontal extent (the text after them in their column, or after
    // a full-width figure), else before the first line read below their top edge anywhere, else
    // at the end.
    for (uint32_t i = 0; i < page.lines.size(); ++i) {
        const Line& image = page.lines[i];
        if (!(image.flags & kLineImage)) continue;
        size_t at = SIZE_MAX, below = SIZE_MAX;
        for (size_t k = 0; k < page.order.size(); ++k) {
            const Line& l = page.lines[page.order[k]];
            if (l.dir != 0 || (l.flags & kLineImage) || l.vt > image.vt) continue;
            if (below == SIZE_MAX) below = k;
            if (l.u1 > image.u0 && l.u0 < image.u1) {
                at = k;
                break;
            }
        }
        if (at == SIZE_MAX) at = below != SIZE_MAX ? below : page.order.size();
        // Images placed before the same line stay top to bottom.
        while (at > 0 && (page.lines[page.order[at - 1]].flags & kLineImage) &&
               page.lines[page.order[at - 1]].vt < image.vt)
            --at;
        while (at < page.order.size() && (page.lines[page.order[at]].flags & kLineImage) &&
               page.lines[page.order[at]].vt >= image.vt)
            ++at;
        page.order.insert(page.order.begin() + static_cast<std::ptrdiff_t>(at), i);
    }
}

} // namespace RayoPdf::Layout
