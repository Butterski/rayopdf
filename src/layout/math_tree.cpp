// SPDX-License-Identifier: Apache-2.0
#include "layout/math_tree.h"

#include "common/minmax.h"
#include "common/per_thread.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace RayoPdf::Layout {
namespace {

enum Relation : uint8_t { kRight, kSup, kSub, kAbove, kBelow, kInside, kRelationCount };
enum NodeKind : uint8_t { kGlyph, kBar, kRadical };

constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kPerSector = 6;
// Contractions of Chu-Liu/Edmonds at most (each keeps a copy of the edges): more leave the formula
// Unsure. Geometric edges point right or up and down, so cycles are rare.
constexpr size_t kMaxContractions = 16;
// tan 22.5° and tan 67.5°: the sectors' borders.
constexpr float kTan22 = 0.41421356f, kTan67 = 2.41421356f;

// A glyph with the accents set over it: what the tree places.
struct Unit {
    float u0 = 0, u1 = 0, v = 0, size = 0, bottom = 0, top = 0;
    uint32_t text = 0, length = 0;  // its LaTeX in TreeScratch::pool
    uint32_t glyph = kNone;         // its glyph while it carries no accent
    bool dead = false;              // merged into another unit
};

struct Node {
    float u0 = 0, u1 = 0, bottom = 0, top = 0;  // its box
    float size = 0;                              // h
    float c = 0;                                 // baseline + 0.25 size; a bar's middle
    // Where edges leave and enter it: a glyph's centre, a bar's or radical's right and left end.
    float outU = 0, inU = 0;
    // Its core for the line of sight: a glyph's middle 80 % of its advance from 0.1 to 0.5 size
    // above its baseline (its ink when it is tall), a bar's or radical's box.
    float coreU0 = 0, coreU1 = 0, coreV0 = 0, coreV1 = 0;
    float barU0 = 0, barU1 = 0;  // a bar's extent; a radical's vinculum
    uint32_t unit = kNone;       // a glyph node's unit
    NodeKind kind = kGlyph;
    bool tall = false;  // a glyph taller than 1.5 times its size: a big delimiter or operator
};

struct Edge {
    uint32_t from = 0, to = 0;
    uint32_t key = 0;  // cost * 4096 + distance: the cheaper, then the nearer
    uint32_t origin = 0;  // in a contraction: the edge of the level before it came from
    uint8_t cost = 0;
    Relation relation = kRight;
};

// One contraction of Chu-Liu/Edmonds: the graph before it.
struct Level {
    std::vector<Edge> edges;
    std::vector<uint32_t> in;     // per node: its cheapest incoming edge
    std::vector<uint32_t> cycle;  // per node: its cycle, or kNone
    uint32_t nodes = 0, root = 0;
};

struct TreeScratch {
    std::vector<MathSymbol> symbols;
    std::vector<uint32_t> glyphs, part, under;
    std::vector<Unit> units;
    std::vector<uint32_t> accents;
    std::string pool, scratchText;
    std::vector<Node> nodes;
    std::vector<uint8_t> ruleUsed;
    std::vector<std::pair<float, uint32_t>> sector[5];
    std::vector<Edge> edges;
    std::vector<Level> levels;
    std::vector<uint32_t> chosen, nextChosen, id, mark, entry;
    std::vector<uint32_t> parent, chain, order;
    std::vector<uint32_t> childStart, childList, stack, enter, leave;
    std::vector<uint32_t> row;
    std::vector<uint8_t> top;
};

// The relation and cost of a geometric edge a -> b (Right, Sup, Sub), false when it is none.
bool Classify(const Node& a, const Node& b, Relation& relation, uint8_t& cost) {
    const float H = 1000 * b.size / a.size, D = 1000 * (a.c - b.c) / a.size;
    if (std::fabs(D) <= 150 && H >= 700 && H <= 1400) {
        relation = kRight, cost = 30;
    } else if (D >= -700 && D <= -150 && H >= 450 && H <= 900) {
        relation = kSup, cost = 30;
    } else if (D >= 100 && D <= 600 && H >= 450 && H <= 900) {
        relation = kSub, cost = 30;
    } else if (H < 350 || H > 1600) {
        return false;
    } else if (std::fabs(D) <= 250) {
        relation = kRight, cost = 70;
    } else if (D >= -900 && D <= -100 && H <= 1000) {
        relation = kSup, cost = 70;
    } else if (D >= 50 && D <= 800 && H <= 1000) {
        relation = kSub, cost = 70;
    } else {
        return false;
    }
    // A big delimiter or operator neither takes nor is a script: rows beside it are a matrix's or
    // limits.
    return relation == kRight || !((a.kind == kGlyph && a.tall) || (b.kind == kGlyph && b.tall));
}

// Whether the segment (x0, y0) - (x1, y1) crosses the core of node `n` (Liang-Barsky clipping).
bool Crosses(const Node& n, float x0, float y0, float x1, float y1) {
    float t0 = 0, t1 = 1;
    const float dx = x1 - x0, dy = y1 - y0;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {x0 - n.coreU0, n.coreU1 - x0, y0 - n.coreV0, n.coreV1 - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0) t0 = Fmax(t0, t);
        else t1 = Fmin(t1, t);
        if (t0 > t1) return false;
    }
    return true;
}

// Chu-Liu/Edmonds: the arborescence of least key rooted at `root` over `edges`; per node its
// edge (an index into `edges`, kNone for the root) in `parent`. False when a node is unreachable
// or past kMaxContractions.
bool Arborescence(uint32_t nodes, uint32_t root, const std::vector<Edge>& edges, TreeScratch& s,
                  std::vector<uint32_t>& parent) {
    size_t depth = 0;
    if (s.levels.size() < 1) s.levels.resize(1);
    Level* level = &s.levels[0];
    level->edges = edges;
    for (uint32_t k = 0; k < level->edges.size(); ++k) level->edges[k].origin = k;
    level->nodes = nodes, level->root = root;
    while (true) {
        Level& L = *level;
        const uint32_t n = L.nodes;
        L.in.assign(n, kNone);
        for (uint32_t k = 0; k < L.edges.size(); ++k) {
            const Edge& e = L.edges[k];
            if (e.to == L.root || e.from == e.to) continue;
            if (L.in[e.to] == kNone || e.key < L.edges[L.in[e.to]].key) L.in[e.to] = k;
        }
        for (uint32_t v = 0; v < n; ++v)
            if (v != L.root && L.in[v] == kNone) return false;
        // Cycles of the cheapest incoming edges: walk up from each node, marking the walk.
        L.cycle.assign(n, kNone);
        s.mark.assign(n, kNone);
        uint32_t cycles = 0;
        for (uint32_t v = 0; v < n; ++v) {
            uint32_t at = v;
            while (at != L.root && s.mark[at] == kNone && L.cycle[at] == kNone) {
                s.mark[at] = v;
                at = L.edges[L.in[at]].from;
            }
            if (at != L.root && s.mark[at] == v && L.cycle[at] == kNone) {
                // A new cycle through `at`.
                uint32_t c = at;
                do {
                    L.cycle[c] = cycles;
                    c = L.edges[L.in[c]].from;
                } while (c != at);
                ++cycles;
            }
        }
        if (cycles == 0) break;
        if (++depth > kMaxContractions) return false;
        // Contract each cycle into one node.
        s.id.assign(n, kNone);
        uint32_t next = cycles;
        for (uint32_t v = 0; v < n; ++v) s.id[v] = L.cycle[v] != kNone ? L.cycle[v] : next++;
        if (s.levels.size() < depth + 1) s.levels.resize(depth + 1);
        Level& M = s.levels[depth];
        Level& prev = s.levels[depth - 1];  // `L` again: resize may have moved it
        M.edges.clear();
        for (uint32_t k = 0; k < prev.edges.size(); ++k) {
            const Edge& e = prev.edges[k];
            const uint32_t u = s.id[e.from], w = s.id[e.to];
            if (u == w || e.to == prev.root) continue;
            Edge c = e;
            c.from = u, c.to = w, c.origin = k;
            c.key = e.key - prev.edges[prev.in[e.to]].key;
            M.edges.push_back(c);
        }
        M.nodes = next, M.root = s.id[prev.root];
        level = &M;
    }
    // Expansion: the chosen edges of each level back to the level before it.
    std::vector<uint32_t>& chosen = s.chosen;
    chosen.clear();
    {
        const Level& F = s.levels[depth];
        for (uint32_t v = 0; v < F.nodes; ++v)
            if (v != F.root) chosen.push_back(F.in[v]);
    }
    for (size_t d = depth; d-- > 0;) {
        const Level& L = s.levels[d];
        const Level& M = s.levels[d + 1];
        std::vector<uint32_t>& next = s.nextChosen;
        next.clear();
        // Per cycle, the node the chosen edge enters it at.
        s.entry.assign(L.nodes, kNone);
        for (uint32_t k : chosen) {
            const uint32_t e = M.edges[k].origin;
            next.push_back(e);
            const uint32_t to = L.edges[e].to;
            if (L.cycle[to] != kNone) s.entry[L.cycle[to]] = to;
        }
        for (uint32_t v = 0; v < L.nodes; ++v)
            if (L.cycle[v] != kNone && s.entry[L.cycle[v]] != v) next.push_back(L.in[v]);
        chosen.swap(next);
    }
    parent.assign(nodes, kNone);
    for (uint32_t k : chosen) parent[edges[k].to] = k;
    for (uint32_t v = 0; v < nodes; ++v)
        if (v != root && parent[v] == kNone) return false;
    return true;
}

class Tree {
public:
    Tree(const MathInput& in, TreeScratch& s) : in_(in), s_(s) {}
    // The LaTeX of the glyphs s_.part.
    MathStatus Part(std::string& out);

private:
    bool Units();
    bool Nodes();
    void Edges();
    bool Inside(const Node& structure, const Node& n) const;
    bool EmitRow(const uint32_t* heads, size_t count, int depth, std::string& out);
    bool EmitGroup(uint32_t node, Relation relation, int depth, std::string& out);
    const uint32_t* Children(uint32_t node, Relation relation, size_t& count) const {
        const size_t at = size_t(node) * kRelationCount + relation;
        count = s_.childStart[at + 1] - s_.childStart[at];
        return s_.childList.data() + s_.childStart[at];
    }
    std::string_view UnitText(const Unit& u) const { return std::string_view(s_.pool).substr(u.text, u.length); }

    const MathInput& in_;
    TreeScratch& s_;
    float em_ = 0;
    uint32_t emitted_ = 0;
    float reach_ = 0;  // EmitRow's: how far right what it wrote of the current node reaches
};

// The part's glyphs as units: accents set over the glyph (or the run of glyphs, for a wide one)
// under them, the lowest first, so that stacked accents nest (\dot{\bar{A}}).
bool Tree::Units() {
    std::vector<Unit>& units = s_.units;
    units.clear();
    s_.pool.clear();
    s_.accents.clear();
    em_ = 0;
    for (uint32_t g : s_.part) {
        const MathGlyph& glyph = in_.glyphs[g];
        const MathSymbol& sym = s_.symbols[g];
        if (glyph.font != static_cast<uint8_t>(MathFont::Extension)) em_ = Fmax(em_, glyph.size);
        // Its ink as ReadMathSymbols estimated it (from the glyph's own when it hangs): finite.
        if (!(sym.bottom - sym.bottom == 0 && sym.top - sym.top == 0)) return false;
        if (sym.role & kSymbolAccent) {
            s_.accents.push_back(g);
            continue;
        }
        if (sym.role & kSymbolStructure) return false;
        Unit u;
        u.u0 = glyph.u0, u.u1 = glyph.u1, u.v = glyph.v, u.size = glyph.size, u.bottom = sym.bottom, u.top = sym.top;
        u.text = static_cast<uint32_t>(s_.pool.size()), u.length = static_cast<uint32_t>(sym.latex.size());
        s_.pool.append(sym.latex);
        u.glyph = g;
        units.push_back(u);
    }
    if (em_ <= 0)
        for (uint32_t g : s_.part) em_ = Fmax(em_, in_.glyphs[g].size);
    if (!(em_ > 0)) return false;
    // The innermost accent first: the lower one, and of accents set on one baseline (stacked
    // accents keep their base's baseline) the one drawn later: TeX draws the outer accent first.
    std::sort(s_.accents.begin(), s_.accents.end(), [&](uint32_t a, uint32_t b) {
        const float va = in_.glyphs[a].v, vb = in_.glyphs[b].v;
        return va != vb ? va < vb : a > b;
    });
    for (uint32_t a : s_.accents) {
        const MathGlyph& acc = in_.glyphs[a];
        const float cx = (acc.u0 + acc.u1) / 2;
        // Its base: the unit under it that its centre stands over, with the widest overlap.
        uint32_t target = kNone;
        float overlap = 0;
        for (uint32_t k = 0; k < units.size(); ++k) {
            const Unit& b = units[k];
            if (b.dead || cx < b.u0 - 0.05f * em_ || cx > b.u1 + 0.05f * em_ || acc.v < b.v - 0.15f * em_) continue;
            const float o = Fmin(acc.u1, b.u1) - Fmax(acc.u0, b.u0);
            if (target == kNone || o > overlap) target = k, overlap = o;
        }
        if (target == kNone) return false;
        // A wide accent: over every unit of its base's size and baseline whose middle it spans
        // (not the base's scripts).
        const float baseV = units[target].v, baseSize = units[target].size;
        std::vector<uint32_t>& under = s_.under;
        under.clear();
        for (uint32_t k = 0; k < units.size(); ++k) {
            const Unit& b = units[k];
            const float bx = (b.u0 + b.u1) / 2;
            if (!b.dead && bx >= acc.u0 && bx <= acc.u1 && std::fabs(b.v - baseV) < 0.2f * em_ &&
                std::fabs(b.size - baseSize) <= 0.08f * baseSize)
                under.push_back(k);
        }
        std::string_view cmd = s_.symbols[a].latex;
        std::string& text = s_.scratchText;
        text.clear();
        float u0 = units[target].u0, u1 = units[target].u1, bottom = units[target].bottom, top = units[target].top;
        if (under.size() >= 2) {
            if (cmd == "\\hat") cmd = "\\widehat";
            else if (cmd == "\\tilde") cmd = "\\widetilde";
            text.append(cmd);
            text.push_back('{');
            std::sort(under.begin(), under.end(), [&](uint32_t x, uint32_t y) {
                return units[x].u0 != units[y].u0 ? units[x].u0 < units[y].u0 : x < y;
            });
            for (uint32_t k : under) {
                AppendLatexToken(text, UnitText(units[k]));
                u0 = Fmin(u0, units[k].u0), u1 = Fmax(u1, units[k].u1);
                bottom = Fmin(bottom, units[k].bottom), top = Fmax(top, units[k].top);
                units[k].dead = k != under[0];
            }
            target = under[0];
        } else {
            text.append(cmd);
            text.push_back('{');
            text.append(UnitText(units[target]));
        }
        text.push_back('}');
        Unit& t = units[target];
        t.u0 = u0, t.u1 = u1, t.bottom = bottom, t.top = Fmax(top, s_.symbols[a].top);
        t.text = static_cast<uint32_t>(s_.pool.size()), t.length = static_cast<uint32_t>(text.size());
        t.glyph = kNone;
        s_.pool.append(text);
    }
    // Compound symbols as the greedy rebuild joins them on a baseline: three dots (\ldots,
    // \cdots), a negation slash and its relation (\neq), a bar and an arrow (\longrightarrow).
    std::vector<uint32_t>& byU = s_.under;
    byU.clear();
    for (uint32_t k = 0; k < units.size(); ++k)
        if (!units[k].dead && units[k].glyph != kNone) byU.push_back(k);
    std::sort(byU.begin(), byU.end(), [&](uint32_t x, uint32_t y) {
        return units[x].u0 != units[y].u0 ? units[x].u0 < units[y].u0 : x < y;
    });
    const auto join = [&](uint32_t first, uint32_t last, std::string_view latex) {
        Unit& t = units[byU[first]];
        for (uint32_t k = first + 1; k <= last; ++k) {
            const Unit& o = units[byU[k]];
            t.u1 = Fmax(t.u1, o.u1), t.bottom = Fmin(t.bottom, o.bottom), t.top = Fmax(t.top, o.top);
            units[byU[k]].dead = true;
        }
        t.text = static_cast<uint32_t>(s_.pool.size()), t.length = static_cast<uint32_t>(latex.size());
        t.glyph = kNone;
        s_.pool.append(latex);
    };
    for (uint32_t i = 0; i + 1 < byU.size(); ++i) {
        const Unit& a = units[byU[i]];
        const MathSymbol& sa = s_.symbols[a.glyph];
        const auto beside = [&](uint32_t k, float gap) {
            const Unit& b = units[byU[k]];
            return b.u0 - units[byU[k - 1]].u1 <= gap && std::fabs(b.v - a.v) <= 0.1f * em_;
        };
        const std::string_view dots = DotsOf(sa.cp);
        if (!dots.empty() && i + 2 < byU.size() && s_.symbols[units[byU[i + 1]].glyph].cp == sa.cp &&
            s_.symbols[units[byU[i + 2]].glyph].cp == sa.cp && beside(i + 1, 0.4f * em_) && beside(i + 2, 0.4f * em_)) {
            join(i, i + 2, dots);
            i += 2;
            continue;
        }
        const Unit& b = units[byU[i + 1]];
        std::string& joined = s_.scratchText;
        joined.clear();
        if (beside(i + 1, INFINITY) &&
            JoinMathSymbols(sa.cp, s_.symbols[b.glyph].cp, sa.latex, s_.symbols[b.glyph].latex, b.u0 - a.u1, em_, joined)) {
            join(i, i + 1, joined);
            ++i;
        }
    }
    return true;
}

bool Tree::Nodes() {
    std::vector<Node>& nodes = s_.nodes;
    nodes.clear();
    std::vector<uint8_t>& ruleUsed = s_.ruleUsed;
    ruleUsed.assign(in_.rules.size(), 0);
    for (uint32_t i = 0; i < s_.units.size(); ++i) {
        const Unit& u = s_.units[i];
        if (u.dead) continue;
        Node n;
        n.unit = i;
        n.u0 = u.u0, n.u1 = u.u1, n.bottom = u.bottom, n.top = u.top, n.size = u.size;
        n.c = u.v + 0.25f * u.size;
        n.outU = n.inU = (u.u0 + u.u1) / 2;
        const float w = u.u1 - u.u0;
        n.coreU0 = u.u0 + 0.1f * w, n.coreU1 = u.u1 - 0.1f * w;
        n.tall = u.top - u.bottom > 1.5f * u.size;
        if (n.tall) n.coreV0 = u.bottom, n.coreV1 = u.top;
        else n.coreV0 = u.v + 0.1f * u.size, n.coreV1 = u.v + 0.5f * u.size;
        if (u.glyph != kNone && (s_.symbols[u.glyph].role & kSymbolRadical)) {
            // Its vinculum: the rule that starts at its right, at most 0.4 em away, near its top.
            uint32_t best = kNone;
            float bestGap = INFINITY;
            for (uint32_t r = 0; r < in_.rules.size(); ++r) {
                const MathRule& rule = in_.rules[r];
                const float gap = std::fabs(rule.u0 - u.u1);
                if (ruleUsed[r] || gap > 0.4f * em_ || rule.v0 < u.bottom || rule.v0 > u.top + 0.4f * u.size) continue;
                if (gap < bestGap) bestGap = gap, best = r;
            }
            if (best == kNone) return false;  // a sign alone
            ruleUsed[best] = 1;
            const MathRule& rule = in_.rules[best];
            n.kind = kRadical;
            n.barU0 = u.u1, n.barU1 = rule.u1;
            n.u1 = Fmax(u.u1, rule.u1), n.top = Fmax(n.top, rule.v1);
            n.size = em_;
            n.c = (u.bottom + u.top) / 2;
            n.inU = u.u0, n.outU = n.u1;
            n.coreU0 = u.u0, n.coreU1 = n.u1, n.coreV0 = n.bottom, n.coreV1 = rule.v1;
        }
        nodes.push_back(n);
    }
    // Fraction bars: the other rules, flat and a quarter em wide or more. Any other rule (a thick
    // or narrow one) leaves the formula Unsure, as does a bar without a numerator or denominator.
    for (uint32_t r = 0; r < in_.rules.size(); ++r) {
        const MathRule& rule = in_.rules[r];
        if (ruleUsed[r]) continue;
        if (rule.v1 - rule.v0 > 0.3f * em_ || rule.u1 - rule.u0 < 0.25f * em_) return false;
        Node n;
        n.kind = kBar;
        n.u0 = n.barU0 = rule.u0, n.u1 = n.barU1 = rule.u1;
        n.bottom = rule.v0, n.top = rule.v1;
        n.size = em_;
        n.c = (rule.v0 + rule.v1) / 2;
        n.inU = rule.u0, n.outU = rule.u1;
        n.coreU0 = rule.u0, n.coreU1 = rule.u1, n.coreV0 = rule.v0, n.coreV1 = rule.v1;
        nodes.push_back(n);
    }
    return nodes.size() <= kMaxTreeGlyphs + kMaxFormulaRules;
}

// Whether node `n` stands where a bar's numerator or denominator, or a radical's radicand, is.
bool Tree::Inside(const Node& structure, const Node& n) const {
    const float cx = (n.u0 + n.u1) / 2;
    if (cx < structure.barU0 || cx > structure.barU1) return false;
    if (structure.kind == kBar)
        return (n.bottom < structure.top + 1.2f * em_ && n.c > structure.c) ||
               (n.top > structure.bottom - 1.2f * em_ && n.c < structure.c);
    return n.c < structure.coreV1 && n.top > structure.bottom;
}

void Tree::Edges() {
    const std::vector<Node>& nodes = s_.nodes;
    std::vector<Edge>& edges = s_.edges;
    edges.clear();
    const uint32_t n = static_cast<uint32_t>(nodes.size());
    for (uint32_t a = 0; a < n; ++a) {
        const Node& from = nodes[a];
        // A bar's numerator and denominator, a radical's radicand: an edge to the leftmost node of
        // each (the others follow it), the one nearer the bar where several start alike.
        if (from.kind != kGlyph) {
            uint32_t first[2] = {kNone, kNone};
            for (uint32_t b = 0; b < n; ++b) {
                const Node& to = nodes[b];
                if (b == a || !Inside(from, to)) continue;
                const int part = from.kind == kBar && to.c < from.c ? 1 : 0;
                const uint32_t f = first[part];
                if (f == kNone || to.u0 < nodes[f].u0 ||
                    (to.u0 == nodes[f].u0 && std::fabs(to.c - from.c) < std::fabs(nodes[f].c - from.c)))
                    first[part] = b;
            }
            for (int part = 0; part < 2; ++part) {
                if (first[part] == kNone) continue;
                Edge e;
                e.from = a, e.to = first[part];
                e.relation = from.kind == kRadical ? kInside : part == 0 ? kAbove : kBelow;
                e.cost = from.kind == kRadical ? 10 : 20;
                const float d = std::fabs(nodes[first[part]].c - from.c) / from.size * 256;
                e.key = e.cost * 4096u + static_cast<uint32_t>(Fmin(4095.0f, d));
                edges.push_back(e);
            }
        }
        // The nearest nodes in each sector: right, upper right, lower right, above, below.
        for (std::vector<std::pair<float, uint32_t>>& sector : s_.sector) sector.clear();
        for (uint32_t b = 0; b < n; ++b) {
            const Node& to = nodes[b];
            if (b == a) continue;
            if (from.kind != kGlyph && Inside(from, to)) continue;  // its parts: the edges above
            const float dx = to.inU - from.outU, dy = to.c - from.c;
            const float ady = std::fabs(dy);
            int sector;
            if (dx > 0 && ady <= kTan22 * dx) sector = 0;
            else if (dx > 0 && ady <= kTan67 * dx) sector = dy > 0 ? 1 : 2;
            else if (ady > 0 && dx >= -kTan22 * ady) sector = dy > 0 ? 3 : 4;
            else continue;
            s_.sector[sector].emplace_back(dx * dx + dy * dy, b);
        }
        for (std::vector<std::pair<float, uint32_t>>& sector : s_.sector) {
            const size_t keep = std::min(kPerSector, sector.size());
            std::partial_sort(sector.begin(), sector.begin() + static_cast<std::ptrdiff_t>(keep), sector.end());
            for (size_t k = 0; k < keep; ++k) {
                const uint32_t b = sector[k].second;
                const Node& to = nodes[b];
                Edge e;
                e.from = a, e.to = b;
                if (!Classify(from, to, e.relation, e.cost)) continue;
                bool seen = true;
                for (uint32_t o = 0; o < n && seen; ++o)
                    if (o != a && o != b) seen = !Crosses(nodes[o], from.outU, from.c, to.inU, to.c);
                if (!seen) continue;
                e.key = e.cost * 4096u + static_cast<uint32_t>(Fmin(4095.0f, std::sqrt(sector[k].first) / from.size * 256));
                edges.push_back(e);
            }
        }
    }
}

bool Tree::EmitGroup(uint32_t node, Relation relation, int depth, std::string& out) {
    size_t count = 0;
    const uint32_t* heads = Children(node, relation, count);
    return EmitRow(heads, count, depth + 1, out);
}

// The nodes the heads' Right chains reach, left to right, each with its parts and scripts.
bool Tree::EmitRow(const uint32_t* heads, size_t count, int depth, std::string& out) {
    if (depth > kMaxFormulaDepth) return false;
    const std::vector<Node>& nodes = s_.nodes;
    // The row lives at the end of s_.row while the nested rows push theirs after it.
    std::vector<uint32_t>& row = s_.row;
    const size_t base = row.size();
    for (size_t h = 0; h < count; ++h) row.push_back(heads[h]);
    for (size_t at = base; at < row.size(); ++at) {
        size_t m = 0;
        const uint32_t* right = Children(row[at], kRight, m);
        for (size_t k = 0; k < m; ++k) row.push_back(right[k]);
    }
    // In x order, as the greedy rebuild sorts a row.
    std::sort(row.begin() + static_cast<std::ptrdiff_t>(base), row.end(), [&](uint32_t a, uint32_t b) {
        return nodes[a].u0 != nodes[b].u0 ? nodes[a].u0 < nodes[b].u0 : a < b;
    });
    const size_t end = row.size();
    float lastU1 = -INFINITY;
    bool ok = true;
    for (size_t i = base; i < end && ok; ++i) {
        const uint32_t id = row[i];
        const Node& v = nodes[id];
        if (lastU1 > -INFINITY) {
            const float gap = v.u0 - lastU1;
            if (gap >= 1.8f * em_) AppendLatexToken(out, "\\qquad");
            else if (gap >= 0.8f * em_) AppendLatexToken(out, "\\quad");
        }
        ++emitted_;
        // How far right the node, its parts and its scripts reach: where a gap after it starts.
        const float saved = reach_;
        reach_ = v.u1;
        uint32_t last = id;
        size_t subs = 0, sups = 0;
        Children(id, kSub, subs), Children(id, kSup, sups);
        const Unit* unit = v.kind == kGlyph ? &s_.units[v.unit] : nullptr;
        if (v.kind == kBar) {
            size_t above = 0, below = 0;
            Children(id, kAbove, above), Children(id, kBelow, below);
            if (above == 0 || below == 0) return false;
            out.append("\\frac{");
            ok = EmitGroup(id, kAbove, depth, out);
            out.append("}{");
            ok = ok && EmitGroup(id, kBelow, depth, out);
            out.push_back('}');
        } else if (v.kind == kRadical) {
            size_t inside = 0;
            Children(id, kInside, inside);
            if (inside == 0) return false;
            out.append("\\sqrt{");
            ok = EmitGroup(id, kInside, depth, out);
            out.push_back('}');
        } else if (unit->glyph != kNone && (s_.symbols[unit->glyph].role & kSymbolUpright)) {
            // An upright letter run: a function name or text. Only its last letter takes scripts:
            // scripts inside a run (limits under \lim) are a structure the tree does not build.
            std::string& word = s_.scratchText;
            word = s_.symbols[unit->glyph].word;
            size_t j = i;
            while (j + 1 < end) {
                const Node& next = nodes[row[j + 1]];
                if (next.kind != kGlyph || next.u0 - nodes[row[j]].u1 >= 0.12f * em_) break;
                const Unit& nu = s_.units[next.unit];
                if (nu.glyph == kNone || !(s_.symbols[nu.glyph].role & kSymbolUpright)) break;
                if (subs != 0 || sups != 0) return false;
                ++j;
                word += s_.symbols[nu.glyph].word;
                ++emitted_;
                Children(row[j], kSub, subs), Children(row[j], kSup, sups);
            }
            if (j == i && word.size() < 2) {
                AppendLatexToken(out, UnitText(*unit));
            } else {
                const bool spaceAfter = subs == 0 && sups == 0 && j + 1 < end &&
                                        nodes[row[j + 1]].u0 - nodes[row[j]].u1 >= 0.2f * em_;
                AppendUprightWord(word, spaceAfter, out);
            }
            last = row[j];
            i = j;
        } else {
            AppendLatexToken(out, UnitText(*unit));
        }
        reach_ = Fmax(reach_, nodes[last].u1);
        if (ok && subs > 0) {
            out.append("_{");
            ok = EmitGroup(last, kSub, depth, out);
            out.push_back('}');
        }
        if (ok && sups > 0) {
            out.append("^{");
            ok = EmitGroup(last, kSup, depth, out);
            out.push_back('}');
        }
        lastU1 = reach_;
        reach_ = Fmax(saved, reach_);
    }
    row.resize(base);
    return ok;
}

MathStatus Tree::Part(std::string& out) {
    if (!Units() || !Nodes()) return MathStatus::Unsure;
    std::vector<Node>& nodes = s_.nodes;
    const uint32_t n = static_cast<uint32_t>(nodes.size());
    // A bar or radical has the size of what it holds: a fraction set in a script is a script.
    for (uint32_t a = 0; a < n; ++a) {
        if (nodes[a].kind == kGlyph) continue;
        float size = 0;
        for (uint32_t b = 0; b < n; ++b)
            if (nodes[b].kind == kGlyph && Inside(nodes[a], nodes[b])) size = Fmax(size, nodes[b].size);
        if (size > 0) nodes[a].size = size;
    }
    // Structures the tree does not build: a radical's index (over its sign), a big operator set
    // tall, anything standing over or under a tall glyph (a matrix's rows), and limits over or
    // under an operator or a word.
    for (uint32_t a = 0; a < n; ++a) {
        if (nodes[a].kind == kRadical) {
            for (uint32_t b = 0; b < n; ++b) {
                const float cx = (nodes[b].u0 + nodes[b].u1) / 2;
                if (b != a && cx >= nodes[a].inU && cx < nodes[a].barU0) return MathStatus::Unsure;
            }
        }
        if (nodes[a].kind != kGlyph) continue;
        const uint32_t glyph = s_.units[nodes[a].unit].glyph;
        const uint8_t role = glyph != kNone ? s_.symbols[glyph].role : 0;
        if (nodes[a].tall && (role & kSymbolBigOp)) return MathStatus::Unsure;
        if (!nodes[a].tall && !(role & (kSymbolBigOp | kSymbolUpright))) continue;
        for (uint32_t b = 0; b < n; ++b) {
            const float cx = (nodes[b].u0 + nodes[b].u1) / 2;
            if (b == a || cx < nodes[a].u0 || cx > nodes[a].u1) continue;
            if (nodes[a].tall || std::fabs(nodes[b].c - nodes[a].c) >= 0.5f * nodes[a].size) return MathStatus::Unsure;
        }
    }
    Edges();
    const std::vector<Edge>& edges = s_.edges;
    if (n == 0) return MathStatus::Unsure;
    // Nodes outside every bar's and radical's parts: the root's row.
    std::vector<uint8_t>& top = s_.top;
    top.assign(n, 1);
    for (uint32_t a = 0; a < n; ++a)
        if (nodes[a].kind != kGlyph)
            for (uint32_t b = 0; b < n; ++b)
                if (b != a && Inside(nodes[a], nodes[b])) top[b] = 0;
    // The root: the leftmost node of the longest chain of Right edges between such nodes. Right
    // edges point right, so nodes taken right to left see the chains after them settled.
    std::vector<uint32_t>& order = s_.order;
    order.resize(n);
    for (uint32_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return nodes[a].inU != nodes[b].inU ? nodes[a].inU > nodes[b].inU : a > b;
    });
    std::vector<uint32_t>& chain = s_.chain;
    chain.assign(n, 1);
    // Right edges grouped by source (edges come in source order).
    std::vector<uint32_t>& start = s_.childStart;
    start.assign(n + 1, 0);
    for (const Edge& e : edges) ++start[e.from + 1];
    for (uint32_t i = 0; i < n; ++i) start[i + 1] += start[i];
    for (uint32_t a : order) {
        if (!top[a]) continue;
        for (uint32_t k = start[a]; k < start[a + 1]; ++k) {
            const Edge& e = edges[k];
            if (e.relation == kRight && top[e.to]) chain[a] = std::max(chain[a], chain[e.to] + 1);
        }
    }
    uint32_t root = kNone;
    for (uint32_t i = n; i-- > 0;) {
        const uint32_t a = order[i];  // left to right: the first of the longest wins
        if (top[a] && (root == kNone || chain[a] > chain[root])) root = a;
    }
    if (root == kNone) return MathStatus::Unsure;
    std::vector<uint32_t>& parent = s_.parent;
    if (!Arborescence(n, root, edges, s_, parent)) return MathStatus::Unsure;
    uint32_t total = 0;
    for (uint32_t v = 0; v < n; ++v)
        if (v != root) total += edges[parent[v]].cost;
    if (n > 1 && total > 50u * (n - 1)) return MathStatus::Unsure;
    // Children per node and relation, left to right.
    std::vector<uint32_t>& childStart = s_.childStart;
    std::vector<uint32_t>& childList = s_.childList;
    const size_t slots = size_t(n) * kRelationCount;
    childStart.assign(slots + 1, 0);
    for (uint32_t v = 0; v < n; ++v)
        if (v != root) ++childStart[size_t(edges[parent[v]].from) * kRelationCount + edges[parent[v]].relation + 1];
    for (size_t i = 0; i < slots; ++i) childStart[i + 1] += childStart[i];
    childList.assign(n, 0);
    std::vector<uint32_t>& fill = s_.chain;  // reused: the next free place per slot
    fill.assign(childStart.begin(), childStart.end() - 1);
    for (uint32_t v = 0; v < n; ++v)
        if (v != root) childList[fill[size_t(edges[parent[v]].from) * kRelationCount + edges[parent[v]].relation]++] = v;
    for (size_t i = 0; i < slots; ++i)
        std::sort(childList.begin() + childStart[i], childList.begin() + childStart[i + 1], [&](uint32_t a, uint32_t b) {
            return nodes[a].u0 != nodes[b].u0 ? nodes[a].u0 < nodes[b].u0 : a < b;
        });
    // Every node standing in a bar's or radical's parts must hang below it in the tree: order of
    // a depth-first walk, a node's subtree being the walk between its entry and its exit.
    std::vector<uint32_t>& enter = s_.enter;
    std::vector<uint32_t>& leave = s_.leave;
    std::vector<uint32_t>& stack = s_.stack;
    enter.assign(n, 0), leave.assign(n, 0);
    stack.clear();
    stack.push_back(root);
    uint32_t clock = 0;
    std::vector<uint32_t>& next = s_.order;  // reused: the next child slot per node
    next.assign(n, 0);
    enter[root] = clock++;
    while (!stack.empty()) {
        const uint32_t v = stack.back();
        const uint32_t from = childStart[size_t(v) * kRelationCount], to = childStart[size_t(v) * kRelationCount + kRelationCount];
        if (from + next[v] < to) {
            const uint32_t c = childList[from + next[v]++];
            enter[c] = clock++;
            stack.push_back(c);
        } else {
            leave[v] = clock++;
            stack.pop_back();
        }
    }
    for (uint32_t a = 0; a < n; ++a) {
        if (nodes[a].kind == kGlyph) continue;
        for (uint32_t b = 0; b < n; ++b)
            if (b != a && Inside(nodes[a], nodes[b]) && !(enter[b] > enter[a] && leave[b] < leave[a])) return MathStatus::Unsure;
    }
    std::string latex;
    emitted_ = 0, reach_ = 0;
    s_.row.clear();
    const uint32_t heads[1] = {root};
    if (!EmitRow(heads, 1, 0, latex) || emitted_ != n || latex.empty()) return MathStatus::Unsure;
    out.append(latex);
    return MathStatus::Ok;
}

} // namespace

MathStatus BuildLatexTree(const MathInput& in, bool display, std::string& out, std::string& tag, bool resolved) {
    tag.clear();
    if (in.glyphs.empty()) return MathStatus::Empty;
    if (in.glyphs.size() > kMaxTreeGlyphs || in.rules.size() > kMaxFormulaRules) return MathStatus::TooLarge;
    TreeScratch& s = PerThread<TreeScratch>();
    std::string tagText;
    if (!ReadMathSymbols(in, display, s.symbols, s.glyphs, tagText, resolved)) return MathStatus::Unreadable;
    // An inline formula broken over lines: line by line. An accent is no line of its own (it may
    // have been set apart from its base's line): a line its glyph starts begins at the next glyph.
    std::string text;
    s.part.clear();
    bool pending = false;
    for (size_t k = 0; k <= s.glyphs.size(); ++k) {
        bool start = k == s.glyphs.size();
        if (!start) {
            const bool lineStart = (in.glyphs[s.glyphs[k]].flags & kMathLineStart) != 0;
            if (s.symbols[s.glyphs[k]].role & kSymbolAccent) {
                pending = pending || lineStart;
            } else {
                start = lineStart || pending;
                pending = false;
            }
        }
        if (start && !s.part.empty()) {
            if (!text.empty()) text.push_back(' ');
            Tree tree(in, s);
            const MathStatus status = tree.Part(text);
            if (status != MathStatus::Ok) return status;
            s.part.clear();
        }
        if (k < s.glyphs.size()) s.part.push_back(s.glyphs[k]);
    }
    if (text.empty()) return MathStatus::Unsure;
    out.append(text);
    tag = std::move(tagText);
    return MathStatus::Ok;
}

} // namespace RayoPdf::Layout
