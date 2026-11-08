// SPDX-License-Identifier: Apache-2.0
#include "layout/reading_order.h"

#include "common/minmax.h"
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

// A folio at either end of a running head or foot: "38 Chapter title", "Journal title 39". A
// leading number must have two digits or be roman, since "3 Methods" is a numbered heading.
bool HasFolio(const std::string& key, const std::string& text) {
    if (key.size() < 3) return false;
    if (key.compare(key.size() - 2, 2, " #") == 0) return true;
    if (key.compare(0, 2, "# ") != 0) return false;
    size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') ++digits;
    return digits != 1;
}

// Publication metadata in a running head or foot: a URL, a DOI, a copyright or licence notice, a
// download stamp, an ISSN. ASCII case is folded; the copyright sign is matched as UTF-8.
bool IsMetadata(const std::string& text) {
    static const char* const kWords[] = {"http", "www.", "doi", "copyright", "issn", "downloaded", "rights reserved",
                                         "license", "licence", "\xC2\xA9"};
    std::string lower(text.size(), ' ');
    for (size_t i = 0; i < text.size(); ++i)
        lower[i] = text[i] >= 'A' && text[i] <= 'Z' ? static_cast<char>(text[i] - 'A' + 'a') : text[i];
    for (const char* word : kWords)
        if (lower.find(word) != std::string::npos) return true;
    return false;
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

// The largest region ordered by Breuel's partial order, whose cost grows with the cube of its
// lines: larger regions are cut row by row instead.
constexpr size_t kTopologicalMax = 200;

struct Orderer {
    PageData& page;
    std::vector<uint32_t>& out;
    uint32_t obstacleFrom;  // lines from this index on are figures added as obstacles
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
            u0 = Fmin(u0, L(i).u0), u1 = Fmax(u1, L(i).u1);
            vb = Fmin(vb, L(i).vb), vt = Fmax(vt, L(i).vt);
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
                if (Fmin(r.x1, u1) - Fmax(r.x0, u0) < 0.6f * width) continue;
                if (r.y >= top - 1 && r.y <= bottom + 1) {
                    cuts.push_back(k);
                    break;
                }
            }
            bottom = Fmin(bottom, L(items[k]).vb);
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
            right = Fmax(right, L(items[k]).u1);
        }
    }

    // Like SplitX, on extents shrunk by up to 0.7 em at both ends: punctuation hanging into a
    // narrow gutter (LaTeX protrudes quotes and hyphens) closes the exact gap.
    void SplitXSoft(std::vector<uint32_t>& items, float size, std::vector<size_t>& cuts) {
        cuts.clear();
        const auto shrink = [&](uint32_t i) { return Fmin(0.7f * size, 0.25f * (L(i).u1 - L(i).u0)); };
        std::sort(items.begin(), items.end(),
                  [&](uint32_t a, uint32_t b) { return L(a).u0 + shrink(a) < L(b).u0 + shrink(b); });
        float right = L(items[0]).u1 - shrink(items[0]);
        for (size_t k = 1; k < items.size(); ++k) {
            if (L(items[k]).u0 + shrink(items[k]) - right >= 1.0f) cuts.push_back(k);
            right = Fmax(right, L(items[k]).u1 - shrink(items[k]));
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
                widest = Fmax(widest, gap);
            }
            bottom = Fmin(bottom, L(items[k]).vb);
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
        bool previousTexty = false;
        for (size_t c = 0; c <= cuts.size(); ++c) {
            const size_t end = c < cuts.size() ? cuts[c] : items.size();
            // A tall gutter beside a column of text: a column of short lines (a list, a column
            // of headings and items) next to it is a column too, unless its lines sit on the
            // text's baselines like the cells of a table row.
            const bool tall = c > 0 && TallGutter(items, c == 1 ? 0 : cuts[c - 2], cuts[c - 1], end, size);
            bool isTexty = false;
            if (end - begin >= 3) {
                float u0 = L(items[begin]).u0, u1 = L(items[begin]).u1;
                for (size_t k = begin; k < end; ++k) u0 = Fmin(u0, L(items[k]).u0), u1 = Fmax(u1, L(items[k]).u1);
                // Weighted by characters: display formulas and figure labels inside a column are
                // many short lines with few characters.
                // A group of long lines (prose averages 25 characters a line or more, table cells
                // far fewer) is text even when its lines have different widths, as when it holds
                // columns of its own.
                uint32_t chars = 0, full = 0, lines = 0;
                for (size_t k = begin; k < end; ++k) {
                    chars += L(items[k]).chars;
                    lines += L(items[k]).chars > 0;
                    if (L(items[k]).u1 - L(items[k]).u0 >= 0.5f * (u1 - u0)) full += L(items[k]).chars;
                }
                isTexty = u1 - u0 >= 8 * size && chars > 0 && (full * 2 >= chars || chars >= 25 * lines);
                texty += isTexty;
            }
            if (tall && (isTexty || previousTexty)) return true;
            previousTexty = isTexty;
            begin = end;
        }
        return texty >= 2;
    }

    // Two neighbouring groups items[a, b) and items[b, c), each of several lines, side by side
    // over most of the shorter one's height, with few lines of one on a baseline of the other.
    bool TallGutter(const std::vector<uint32_t>& items, size_t a, size_t b, size_t c, float size) {
        if (b - a < 6 || c - b < 6 || b - a > 512 || c - b > 512) return false;
        float lb = INFINITY, lt = -INFINITY, rb = INFINITY, rt = -INFINITY;
        for (size_t k = a; k < b; ++k) lb = Fmin(lb, L(items[k]).vb), lt = Fmax(lt, L(items[k]).vt);
        for (size_t k = b; k < c; ++k) rb = Fmin(rb, L(items[k]).vb), rt = Fmax(rt, L(items[k]).vt);
        if (Fmin(lt, rt) - Fmax(lb, rb) < 0.5f * Fmin(lt - lb, rt - rb)) return false;
        size_t mates = 0;
        for (size_t k = a; k < b; ++k)
            for (size_t j = b; j < c; ++j)
                if (std::fabs(L(items[k]).baseline - L(items[j]).baseline) <= 0.3f * size) {
                    mates++;
                    break;
                }
        return mates * 2 < b - a;
    }


    // A narrow group of short items, each on the baseline of a line of the group to its right,
    // is a column of labels (reference numbers "[12]", line numbers, enumerators set apart by a
    // tab), not a column of text: it is read row by row with the text it labels, so the cut
    // between them goes.
    void DropLabelCuts(const std::vector<uint32_t>& items, std::vector<size_t>& cuts, float size) {
        size_t kept = 0;
        for (size_t c = 0; c < cuts.size(); ++c) {
            const size_t begin = c == 0 ? 0 : cuts[c - 1];
            const size_t end = cuts[c];
            const size_t next = c + 1 < cuts.size() ? cuts[c + 1] : items.size();
            float u0 = INFINITY, u1 = -INFINITY;
            for (size_t k = begin; k < end; ++k) u0 = Fmin(u0, L(items[k]).u0), u1 = Fmax(u1, L(items[k]).u1);
            bool labels = u1 - u0 <= 4 * size && end - begin <= 128 && next - end <= 2048;
            for (size_t k = begin; k < end && labels; ++k) {
                const Line& label = L(items[k]);
                bool mate = false;
                for (size_t j = end; j < next && !mate; ++j) {
                    const Line& l = L(items[j]);
                    mate = std::fabs(l.baseline - label.baseline) <= 0.3f * label.size && l.u0 - label.u1 <= 3 * size;
                }
                labels = mate;
            }
            if (!labels) cuts[kept++] = cuts[c];
        }
        cuts.resize(kept);
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
        SplitX(items, Fmax(3.0f, 0.5f * size), xCuts);
        DropLabelCuts(items, xCuts, size);
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
            DropLabelCuts(soft, xCuts, size);
            if (!xCuts.empty() && ColumnsLike(soft, xCuts, size)) {
                Recurse(soft, xCuts, depth);
                return;
            }
        }
        // Whitespace no wider than the space between lines is no paragraph or section break: a
        // y cut there would slice columns into rows, so columns found with their bridging lines
        // set aside come first.
        float widest = 0;
        for (float g : gaps_) widest = Fmax(widest, g);
        if ((yCuts.empty() || widest < size) && MaskedColumns(items, size, depth)) return;
        if (!yCuts.empty() && widest < size && Topological(items)) return;
        if (!yCuts.empty()) {
            Recurse(byY, yCuts, depth);
            return;
        }
        // Figures keep a column's whitespace from reading as a page-wide gap; a region they make
        // uncuttable (text beside and between figures) is cut again without them.
        size_t text = 0;
        for (uint32_t i : items) text += i < obstacleFrom;
        if (text < items.size() && text >= 2) {
            items.erase(std::remove_if(items.begin(), items.end(), [&](uint32_t i) { return i >= obstacleFrom; }),
                        items.end());
            Cut(items, depth + 1);
            return;
        }
        Fallback(items);
    }

    // Breuel's partial order (algorithms §5.1), for a region whose columns no cut separates (text
    // flowing around a figure set into both columns, a box straddling a gutter): a comes before b
    // when they overlap horizontally and a is above b, or when a lies wholly left of b and no line
    // between them vertically overlaps both. Lines are taken in that order, topmost first among
    // those free. Regions without two lines of text side by side (labels beside their entries do not
    // count), or too large for the cubic order, are left to the y cuts.
    bool Topological(const std::vector<uint32_t>& items) {
        const size_t n = items.size();
        if (n < 3 || n > kTopologicalMax) return false;
        bool side = false;
        for (size_t a = 0; a < n && !side; ++a)
            for (size_t b = a + 1; b < n && !side; ++b) {
                const Line& la = L(items[a]);
                const Line& lb = L(items[b]);
                side = la.vb < lb.vt && lb.vb < la.vt && (la.u1 <= lb.u0 || lb.u1 <= la.u0) &&
                       la.u1 - la.u0 >= 6 * la.size && lb.u1 - lb.u0 >= 6 * lb.size;
            }
        if (!side) return false;
        std::vector<uint32_t> v(items);
        std::sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) {
            if (L(a).baseline != L(b).baseline) return L(a).baseline > L(b).baseline;
            return L(a).u0 < L(b).u0;
        });
        std::vector<uint8_t> edge(n * n, 0);
        std::vector<uint32_t> indegree(n, 0);
        for (size_t a = 0; a < n; ++a) {
            const Line& la = L(v[a]);
            for (size_t b = 0; b < n; ++b) {
                if (a == b) continue;
                const Line& lb = L(v[b]);
                const float tolerance = 0.3f * Fmin(la.size, lb.size);
                bool before = la.u0 < lb.u1 && lb.u0 < la.u1 && la.baseline > lb.baseline + tolerance;
                if (!before && la.u1 <= lb.u0) {
                    before = true;
                    const float high = Fmax(la.baseline, lb.baseline) - tolerance;
                    const float low = Fmin(la.baseline, lb.baseline) + tolerance;
                    for (size_t c = std::min(a, b) + 1; c < std::max(a, b) && before; ++c) {
                        const Line& lc = L(v[c]);
                        if (lc.baseline >= high || lc.baseline <= low) continue;
                        before = !(lc.u0 < la.u1 && lc.u1 > la.u0 && lc.u0 < lb.u1 && lc.u1 > lb.u0);
                    }
                }
                if (before) {
                    edge[a * n + b] = 1;
                    indegree[b]++;
                }
            }
        }
        // v is sorted top first: the first free line is the topmost. A cycle (possible with
        // overlapping boxes) is broken by taking the topmost line left.
        std::vector<uint8_t> done(n, 0);
        for (size_t step = 0; step < n; ++step) {
            size_t pick = n;
            for (size_t k = 0; k < n && pick == n; ++k)
                if (!done[k] && indegree[k] == 0) pick = k;
            for (size_t k = 0; k < n && pick == n; ++k)
                if (!done[k]) pick = k;
            done[pick] = 1;
            out.push_back(v[pick]);
            for (size_t b = 0; b < n; ++b)
                if (edge[pick * n + b] && indegree[b] > 0) indegree[b]--;
        }
        return true;
    }

    // Lines that bridge a gutter inside a region (XY-Cut++'s cross-layout lines: a line
    // overlapping, near its baseline, two substantial lines that do not overlap each other):
    // headlines over some columns of a newspaper page, a line run across a gutter. Fills `cross`
    // and `rest`.
    void CrossLines(const std::vector<uint32_t>& items, std::vector<uint32_t>& cross, std::vector<uint32_t>& rest) {
        cross.clear();
        rest.clear();
        float left = INFINITY, right = -INFINITY;
        scratch.clear();
        for (uint32_t i : items) {
            left = Fmin(left, L(i).u0), right = Fmax(right, L(i).u1);
            if (L(i).count > 0) scratch.push_back(i);
        }
        float median = 0;
        if (!scratch.empty()) {
            auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
            std::nth_element(scratch.begin(), mid, scratch.end(),
                             [&](uint32_t a, uint32_t b) { return L(a).u1 - L(a).u0 < L(b).u1 - L(b).u0; });
            median = L(*mid).u1 - L(*mid).u0;
        }
        // Substantial: a quarter of the region, or most of a typical line when the region holds
        // many narrow columns.
        const float wide = Fmin(0.25f * (right - left), 0.6f * median);
        scratch.assign(items.begin(), items.end());
        std::sort(scratch.begin(), scratch.end(), [&](uint32_t a, uint32_t b) { return L(a).baseline > L(b).baseline; });
        size_t from = 0;
        for (size_t k = 0; k < scratch.size(); ++k) {
            const Line& lf = L(scratch[k]);
            const float window = 6 * lf.size;
            while (from < k && L(scratch[from]).baseline - lf.baseline > window) ++from;
            float minRight = INFINITY, maxLeft = -INFINITY;
            for (size_t j = from; j < scratch.size(); ++j) {
                const Line& lg = L(scratch[j]);
                if (lf.baseline - lg.baseline > window) break;
                if (j == k || lg.u1 - lg.u0 < wide || lg.u1 <= lf.u0 || lg.u0 >= lf.u1) continue;
                if (std::fabs(lg.baseline - lf.baseline) > window) continue;
                minRight = Fmin(minRight, lg.u1);
                maxLeft = Fmax(maxLeft, lg.u0);
            }
            (maxLeft > minRight ? cross : rest).push_back(scratch[k]);
        }
    }

    // A region no gutter crosses cleanly: its columns found with the bridging lines set aside,
    // each bridging line then read with the column it starts in. Returns whether it cut.
    bool MaskedColumns(const std::vector<uint32_t>& items, float size, int depth) {
        if (items.size() < 6 || items.size() > 2000) return false;
        std::vector<uint32_t> cross, rest;
        CrossLines(items, cross, rest);
        if (cross.empty() || rest.size() < 6) return false;
        std::vector<size_t> cuts;
        SplitX(rest, Fmax(3.0f, 0.5f * size), cuts);
        DropLabelCuts(rest, cuts, size);
        if (cuts.empty() || !ColumnsLike(rest, cuts, size)) return false;
        // The columns' left edges, to place the bridging lines.
        std::vector<std::vector<uint32_t>> groups(cuts.size() + 1);
        std::vector<float> starts;
        size_t begin = 0;
        for (size_t c = 0; c <= cuts.size(); ++c) {
            const size_t end = c < cuts.size() ? cuts[c] : rest.size();
            groups[c].assign(rest.begin() + static_cast<std::ptrdiff_t>(begin), rest.begin() + static_cast<std::ptrdiff_t>(end));
            starts.push_back(L(rest[begin]).u0);
            begin = end;
        }
        for (uint32_t f : cross) {
            size_t g = 0;
            while (g + 1 < starts.size() && L(f).u0 >= starts[g + 1]) ++g;
            groups[g].push_back(f);
        }
        for (std::vector<uint32_t>& group : groups) Cut(group, depth + 1);
        return true;
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
        for (uint32_t i : items) left = Fmin(left, L(i).u0), right = Fmax(right, L(i).u1);
        const float wide = 0.25f * (right - left);
        // XY-Cut++'s length condition: a cross-layout line is longer than β = 1.3 times the
        // median line, so a line of text wrapped around a pull quote or a figure is not one.
        scratch.clear();
        for (uint32_t i : items)
            if (L(i).count > 0) scratch.push_back(i);
        float median = 0;
        if (!scratch.empty()) {
            auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
            std::nth_element(scratch.begin(), mid, scratch.end(),
                             [&](uint32_t a, uint32_t b) { return L(a).u1 - L(a).u0 < L(b).u1 - L(b).u0; });
            median = L(*mid).u1 - L(*mid).u0;
        }
        std::vector<uint32_t> separators, rest;
        for (uint32_t f : items) {
            const Line& lf = L(f);
            if (lf.u1 - lf.u0 < 1.3f * median) {
                rest.push_back(f);
                continue;
            }
            const float window = 6 * lf.size;
            float minRight = INFINITY, maxLeft = -INFINITY;
            for (uint32_t g : items) {
                if (g == f) continue;
                const Line& lg = L(g);
                if (lg.u1 - lg.u0 < wide || lg.u1 <= lf.u0 || lg.u0 >= lf.u1) continue;
                if (std::fabs(lg.baseline - lf.baseline) > window) continue;
                minRight = Fmin(minRight, lg.u1);
                maxLeft = Fmax(maxLeft, lg.u0);
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
constexpr int kEdgeMaxRows = 4;

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
    // The horizontal extent of the page's text, against which a row's spread is measured.
    float textLeft = INFINITY, textRight = -INFINITY;
    for (uint32_t i : lines) textLeft = Fmin(textLeft, page.lines[i].u0), textRight = Fmax(textRight, page.lines[i].u1);
    // The text's own line spacing: the median gap from a line to the next line below it in its
    // column. A header or footer is set apart by more than that, so double-spaced text keeps its
    // first and last lines.
    std::vector<float> gaps;
    for (size_t k = 0; k + 1 < lines.size(); ++k) {
        const Line& a = page.lines[lines[k]];
        for (size_t j = k + 1; j < lines.size() && j <= k + 8; ++j) {
            const Line& b = page.lines[lines[j]];
            if (b.u1 <= a.u0 || b.u0 >= a.u1) continue;
            const float g = a.vb - b.vt;
            if (g > -0.5f * body && g < 3 * body) gaps.push_back(g);
            break;
        }
    }
    float spacing = 0;
    if (!gaps.empty()) {
        std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
        spacing = gaps[gaps.size() / 2];
    }
    const float minGap = Fmax(body, 1.5f * spacing);
    int removed = 0;
    std::vector<uint32_t> rowLines;
    std::string piece;
    size_t first = 0, last = lines.size();  // the lines not removed: lines[first, last)
    for (int side = 0; side < 2; ++side) {
        // Rows set close together at the edge (a two- or three-line journal or licence block)
        // are taken together when each carries furniture evidence and the innermost is set
        // apart: `pending` rows wait for that.
        size_t start = side == 0 ? first : last;
        int pending = 0;
        for (int row = 0; row < kEdgeMaxRows && last - first > kEdgeMinLines; ++row) {
            const Line& edge = page.lines[lines[side == 0 ? first : last - 1]];
            // The row: lines on the edge line's baseline.
            size_t count = 0;
            float inner = side == 0 ? INFINITY : -INFINITY;  // the row's inner edge
            float rowLeft = INFINITY, rowRight = -INFINITY;
            uint32_t chars = 0;
            float largest = 0;
            bool superscript = false, bold = true, mono = true;
            for (size_t k = 0; k < last - first; ++k) {
                const Line& l = page.lines[lines[side == 0 ? first + k : last - 1 - k]];
                if (std::fabs(l.baseline - edge.baseline) > 0.3f * Fmax(l.size, edge.size)) break;
                count++;
                inner = side == 0 ? Fmin(inner, l.vb) : Fmax(inner, l.vt);
                chars += l.chars;
                largest = Fmax(largest, l.size);
                rowLeft = Fmin(rowLeft, l.u0), rowRight = Fmax(rowRight, l.u1);
                bold = bold && (l.flags & kLineAllBold);
                mono = mono && (l.flags & kLineAllMono);
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
            bool number = false, folio = false, note = false, metadata = false;
            if (band) {
                // The row's text, left to right.
                rowLines.assign(lines.begin() + static_cast<std::ptrdiff_t>(side == 0 ? first : last - count),
                           lines.begin() + static_cast<std::ptrdiff_t>(side == 0 ? first + count : last));
                std::sort(rowLines.begin(), rowLines.end(),
                          [&](uint32_t a, uint32_t b) { return page.lines[a].u0 < page.lines[b].u0; });
                text.clear();
                for (uint32_t li : rowLines) {
                    if (!text.empty()) text.push_back(' ');
                    LineText(page, page.lines[li], piece);
                    text += piece;
                }
                RunningKey(text, key);
                number = count == 1 && IsPageNumberKey(key);
                folio = !bold && HasFolio(key, text);
                metadata = IsMetadata(text);
                // A foot row that starts with a number or a reference mark is a footnote.
                note = side == 1 && (key.compare(0, 2, "# ") == 0 || key.compare(0, 3, "#. ") == 0 ||
                                     key.compare(0, 3, "#) ") == 0 || text.compare(0, 1, "*") == 0 ||
                                     text.compare(0, 3, "\xE2\x80\xA0") == 0 || text.compare(0, 3, "\xE2\x80\xA1") == 0 ||
                                     text.compare(0, 2, "\xC2\xA7") == 0);
            }
            // A body-size row above a heading is the document's first paragraph, set high on a page
            // with narrow margins: a header must be smaller than the body to be told from it.
            const bool beforeHeading = side == 0 && nextSize >= 1.12f * body && largest > 0.95f * body;
            // Furniture evidence inside the running band: pieces spread across the text width
            // ("Journal 12(3) ... Author et al."), a folio at an end of the row ("38 Chapter
            // title"), or print smaller than the body. Figures between the row and the text count
            // as content: a caption or credit under a figure is not set apart.
            float figureGap = gap;
            for (const ImageBox& image : page.images) {
                if ((image.x1 - image.x0) * (image.y1 - image.y0) < 0.01f * page.width * h) continue;
                if (side == 0 && image.y1 <= inner + 1) figureGap = Fmin(figureGap, inner - image.y1);
                if (side == 1 && image.y0 >= inner - 1) figureGap = Fmin(figureGap, image.y0 - inner);
            }
            // A spread row reaches both edges of the text: a row with a piece in each column of a
            // two-column page does not.
            const bool spread = count >= 2 && rowLeft <= textLeft + 2 * body && rowRight >= textRight - 2 * body;
            const bool evidence = spread || (!note && (folio || (largest <= 0.9f * body && !mono)));
            const bool qualifies = gap >= minGap && !superscript && chars <= kEdgeMaxChars &&
                                   ((number && largest <= 1.3f * body) ||
                                    (margin && largest <= 1.02f * body && !beforeHeading) ||
                                    (band && evidence && figureGap >= minGap && largest <= 1.35f * body));
            // Publication metadata (a URL, a DOI, a copyright or licence line, a download stamp,
            // an ISSN), a row spread across the text, or small print in the head band: furniture
            // even above a figure or a heading. Small print at the foot is a footnote or a caption.
            const bool strong = band && !note && !superscript && chars <= kEdgeMaxChars && largest <= 1.35f * body &&
                                (metadata || spread || (side == 0 && largest <= 0.9f * body && !mono));
            const bool apart = (qualifies && (row < 2 || strong)) || (strong && gap >= minGap);
            if (!apart && !(strong && gap > -0.5f * body && pending + 1 < kEdgeMaxRows)) {
                // The block does not end set apart: the pending rows stay.
                if (side == 0)
                    first = start;
                else
                    last = start;
                break;
            }
            if (side == 0)
                first += count;
            else
                last -= count;
            if (!apart) {
                pending++;
                continue;
            }
            for (size_t k = side == 0 ? start : last; k < (side == 0 ? first : start); ++k) {
                page.lines[lines[k]].flags |= kLineRemoved;
                removed++;
            }
            pending = 0;
            start = side == 0 ? first : last;
        }
        if (pending > 0) {
            if (side == 0)
                first = start;
            else
                last = start;
        }
    }
    return removed;
}

// Page furniture set sideways in a side margin (§5.2): download stamps ("Downloaded from ... on
// <date>"), repository and licence notes, arXiv identifiers. A line of another direction lying
// wholly left or right of the upright text, on a page with upright text, is removed. Rotated text
// inside the text's width (table headers, figure labels) stays.
int RemoveMarginLines(PageData& page) {
    float left = INFINITY, right = -INFINITY;
    uint32_t upright = 0;
    for (const Line& l : page.lines) {
        if (l.dir != 0 || l.count == 0 || (l.flags & kLineRemoved)) continue;
        left = Fmin(left, l.u0), right = Fmax(right, l.u1);
        upright++;
    }
    if (upright < kEdgeMinLines) return 0;
    int removed = 0;
    for (Line& l : page.lines) {
        if (l.dir == 0 || l.count == 0 || (l.flags & kLineRemoved)) continue;
        float x0 = INFINITY, x1 = -INFINITY;
        for (uint32_t k = 0; k < l.count; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[l.first + k]];
            x0 = Fmin(x0, g.x0), x1 = Fmax(x1, g.x1);
        }
        if (x1 <= left || x0 >= right) {
            l.flags |= kLineRemoved;
            removed++;
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
    // One page gives no evidence of running lines (every test below needs two pages at least).
    for (int p = 0; p < n && n >= 2; ++p) {
        PageData& page = pages[static_cast<size_t>(p)];
        const float h = page.height;
        // A lone number is a page number only as the page's outermost line.
        float highest = -INFINITY, lowest = INFINITY;
        for (const Line& line : page.lines) {
            if (line.dir != 0) continue;
            highest = Fmax(highest, (line.vb + line.vt) / 2);
            lowest = Fmin(lowest, (line.vb + line.vt) / 2);
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
    // The body size (algorithms §6.1: the most common size by characters) for the edge test: the
    // size key with the most characters, the smallest of equal counts.
    std::vector<std::pair<int, uint64_t>> sizeChars;
    for (const PageData& page : pages)
        for (const Line& line : page.lines)
            if (line.dir == 0) sizeChars.emplace_back(static_cast<int>(std::lround(line.size * 2)), line.chars);
    std::sort(sizeChars.begin(), sizeChars.end(),
              [](const std::pair<int, uint64_t>& a, const std::pair<int, uint64_t>& b) { return a.first < b.first; });
    int bodyKey = 0;
    uint64_t bodyCount = 0;
    for (size_t i = 0; i < sizeChars.size();) {
        const int size = sizeChars[i].first;
        uint64_t chars = 0;
        for (; i < sizeChars.size() && sizeChars[i].first == size; ++i) chars += sizeChars[i].second;
        if (chars > bodyCount || (chars == bodyCount && size < bodyKey)) bodyKey = size, bodyCount = chars;
    }
    for (PageData& page : pages) {
        removed += RemoveEdgeRows(page, float(bodyKey) / 2, text, key);
        removed += RemoveMarginLines(page);
    }
    return removed;
}

namespace {

// Figures as obstacles (algorithms §5.1, XY-Cut++ pre-masks figures): an image beside a column
// keeps the whitespace above and below it from reading as a page-wide gap, so a column wrapped
// around a figure and its caption is not cut in two. Only images that could be content count,
// judged on the page alone so that the order is the same whether images are written or not: at
// least 1 % and under 90 % of the page, with no text over them (text on an image makes it a
// background).
constexpr float kObstacleMinShare = 0.01f;
constexpr float kObstacleMaxShare = 0.9f;
constexpr uint32_t kMaxObstacles = 64;

void AddObstacles(PageData& page, std::vector<uint32_t>& items) {
    const float pageArea = page.width * page.height;
    std::vector<float> sizes;
    for (uint32_t i : items)
        if (page.lines[i].count > 0) sizes.push_back(page.lines[i].size);
    if (sizes.empty()) return;
    std::nth_element(sizes.begin(), sizes.begin() + static_cast<std::ptrdiff_t>(sizes.size() / 2), sizes.end());
    const float size = sizes[sizes.size() / 2];
    uint32_t added = 0;
    for (const ImageBox& image : page.images) {
        if (added >= kMaxObstacles) break;
        const float area = (image.x1 - image.x0) * (image.y1 - image.y0);
        if (!(area >= kObstacleMinShare * pageArea) || area >= kObstacleMaxShare * pageArea) continue;
        bool covered = false;
        for (size_t k = 0; k < items.size() && !covered; ++k) {
            const Line& l = page.lines[items[k]];
            const float cu = (l.u0 + l.u1) / 2, cv = (l.vb + l.vt) / 2;
            covered = l.count > 0 && cu > image.x0 + 1 && cu < image.x1 - 1 && cv > image.y0 + 1 && cv < image.y1 - 1;
        }
        if (covered) continue;
        Line obstacle;
        obstacle.u0 = image.x0, obstacle.u1 = image.x1, obstacle.vb = image.y0, obstacle.vt = image.y1;
        obstacle.baseline = image.y0;
        obstacle.size = size;
        items.push_back(static_cast<uint32_t>(page.lines.size()));
        page.lines.push_back(obstacle);
        added++;
    }
}

} // namespace

void OrderLines(PageData& page) {
    page.order.clear();
    Orderer orderer{page, page.order, static_cast<uint32_t>(page.lines.size()), {}, {}, {}};
    orderer.CollectRules();
    std::vector<uint32_t> items;
    const size_t lineCount = page.lines.size();
    for (uint8_t dir = 0; dir < 4; ++dir) {
        items.clear();
        for (uint32_t i = 0; i < lineCount; ++i)
            if (page.lines[i].dir == dir && !(page.lines[i].flags & (kLineRemoved | kLineTableCell | kLineImage)))
                items.push_back(i);
        if (dir == 0) AddObstacles(page, items);
        if (!items.empty()) orderer.Order(items);
    }
    // The obstacles leave the order and the page again.
    page.order.erase(std::remove_if(page.order.begin(), page.order.end(), [&](uint32_t i) { return i >= lineCount; }),
                     page.order.end());
    page.lines.resize(lineCount);
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
        // Not inside a paragraph: a line that runs on from the one before it (the next line down
        // in its column, at the paragraph's line spacing) takes the image after the paragraph.
        const auto continues = [&](const Line& a, const Line& b) {
            if (a.count == 0 || b.count == 0 || a.dir != 0 || b.dir != 0) return false;
            const float gap = a.vb - b.vt;
            return gap > -0.3f * a.size && gap < 0.8f * a.size && b.u0 < a.u1 && a.u0 < b.u1 &&
                   std::fabs(a.size - b.size) <= 0.15f * a.size;
        };
        while (at > 0 && at < page.order.size() &&
               continues(page.lines[page.order[at - 1]], page.lines[page.order[at]]))
            ++at;
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
