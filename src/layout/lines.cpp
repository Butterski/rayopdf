// SPDX-License-Identifier: Apache-2.0
#include "layout/lines.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RayoPdf::Layout {
namespace {

constexpr uint32_t kEmpty = UINT32_MAX;

uint64_t HashText(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

uint64_t Mix(int64_t x, int64_t y, uint64_t h) {
    uint64_t k = h ^ (static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full);
    k ^= k >> 29;
    k *= 0xBF58476D1CE4E5B9ull;
    k ^= k >> 32;
    return k;
}

// A line fragment: glyphs consecutive in content order on one baseline, `vis[begin, end)`.
struct Fragment {
    uint32_t begin = 0, end = 0;
    float u0 = 0, u1 = 0, vb = 0, vt = 0;
    float baseline = 0, size = 0;
    float lastU1 = 0, lastSize = 0;
    uint8_t dir = 0;
    bool any = false;    // has a glyph that is not a space
    bool marker = false; // holds only a list marker
};

// Folds overprinted duplicates (the same text drawn again within 0.15 em, algorithms §4.2) into one
// glyph marked bold. `vis` keeps the survivors.
void FoldDuplicates(PageData& page, std::vector<uint32_t>& vis) {
    std::vector<Glyph>& glyphs = page.glyphs;
    size_t capacity = 16;
    while (capacity < vis.size() * 2) capacity <<= 1;
    const uint64_t mask = capacity - 1;
    std::vector<uint32_t> table(capacity, kEmpty);
    constexpr float kCell = 2.0f;
    std::vector<uint64_t> hashes(glyphs.size(), 0);
    size_t kept = 0;
    for (size_t k = 0; k < vis.size(); ++k) {
        const uint32_t i = vis[k];
        Glyph& g = glyphs[i];
        if ((g.flags & kGlyphSpace) || g.textLength == 0) {
            vis[kept++] = i;
            continue;
        }
        const std::string_view text = page.GlyphText(g);
        const uint64_t h = HashText(text);
        hashes[i] = h;
        // Coordinates are finite but may be huge: clamped before the integer conversion.
        const int64_t cx = static_cast<int64_t>(std::floor(std::fmin(std::fmax(g.x0 / kCell, -1e15f), 1e15f)));
        const int64_t cy = static_cast<int64_t>(std::floor(std::fmin(std::fmax(g.y0 / kCell, -1e15f), 1e15f)));
        const float tolerance = std::fmax(0.15f * g.size, 0.5f);
        uint32_t found = kEmpty;
        for (int dx = -1; dx <= 1 && found == kEmpty; ++dx) {
            for (int dy = -1; dy <= 1 && found == kEmpty; ++dy) {
                uint64_t slot = Mix(cx + dx, cy + dy, h) & mask;
                for (size_t probes = 0; table[slot] != kEmpty && probes < capacity; ++probes) {
                    const uint32_t j = table[slot];
                    const Glyph& o = glyphs[j];
                    if (hashes[j] == h && o.dir == g.dir && std::fabs(o.x0 - g.x0) < tolerance &&
                        std::fabs(o.y0 - g.y0) < tolerance && std::fabs(o.size - g.size) < tolerance &&
                        page.GlyphText(o) == text) {
                        found = j;
                        break;
                    }
                    slot = (slot + 1) & mask;
                }
            }
        }
        if (found != kEmpty) {
            g.flags |= kGlyphDropped;
            if (!(glyphs[found].flags & kGlyphInvisible)) glyphs[found].flags |= kGlyphBold;
            page.hiddenGlyphs++;
            continue;
        }
        uint64_t slot = Mix(cx, cy, h) & mask;
        while (table[slot] != kEmpty) slot = (slot + 1) & mask;
        table[slot] = i;
        vis[kept++] = i;
    }
    vis.resize(kept);
}

bool StartsWithCombiningMark(std::string_view text) {
    // U+0300-U+036F combining diacritical marks: C3 80..CD AF in UTF-8.
    if (text.size() < 2) return false;
    const unsigned char a = static_cast<unsigned char>(text[0]), b = static_cast<unsigned char>(text[1]);
    return (a == 0xCC && b >= 0x80) || (a == 0xCD && b <= 0xAF);
}

} // namespace

Marker ParseMarker(std::string_view t, uint32_t& value) {
    value = 0;
    if (t.empty() || t.size() > 8) return Marker::None;
    static const char* const kBullets[] = {
        "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA", "\xE2\x96\xAB", "\xE2\x97\x8F", "\xE2\x97\x8B",
        "\xE2\x80\xA3", "\xE2\x81\x83", "\xE2\x80\x93", "-",            "*",            "\xC2\xB7",
        "\xE2\x96\xA0", "\xE2\x96\xA1", "\xE2\x96\xBA", "\xE2\x96\xB6", "\xE2\x9E\xA2", "\xE2\x9E\xA4",
        "\xE2\x9D\x96", "\xE2\x99\xA6", "\xE2\x97\x86", "\xE2\x88\x99", "\xE2\x97\xBE", "\xEF\x82\xB7",
        "\xEF\x82\xA7", "\xEF\x83\x98", "\xEF\x81\xB6", "\xEF\x82\xA8", "\xE2\x80\x94", "\xE2\x9C\x93",
        "\xE2\x9C\x94",
    };
    for (const char* b : kBullets)
        if (t == b) return Marker::Bullet;
    if (t == "\xE2\x98\x90") return Marker::TaskOpen;                            // ☐
    if (t == "\xE2\x98\x91" || t == "\xE2\x98\x92") return Marker::TaskDone;     // ☑ ☒
    // Enumerators: "N." "N)" "(N)" with N digits, one lowercase letter or a lowercase roman numeral.
    std::string_view core = t;
    bool parenthesized = false;
    if (core.size() >= 3 && core.front() == '(' && core.back() == ')') {
        core = core.substr(1, core.size() - 2);
        parenthesized = true;
    } else if (core.size() >= 2 && (core.back() == '.' || core.back() == ')')) {
        core = core.substr(0, core.size() - 1);
    } else {
        return Marker::None;
    }
    (void)parenthesized;
    if (core.empty()) return Marker::None;
    bool digits = true;
    for (char c : core) digits = digits && c >= '0' && c <= '9';
    if (digits) {
        if (core.size() > 3) return Marker::None;
        for (char c : core) value = value * 10 + static_cast<uint32_t>(c - '0');
        return Marker::Number;
    }
    bool roman = core.size() <= 5;
    for (char c : core) roman = roman && (c == 'i' || c == 'v' || c == 'x');
    if (roman) {
        // Values of the first numerals (i to xxxix); a malformed sequence is no marker.
        uint32_t total = 0, previous = 0;
        for (size_t k = core.size(); k-- > 0;) {
            const uint32_t v = core[k] == 'i' ? 1 : core[k] == 'v' ? 5 : 10;
            if (v < previous)
                total -= v;
            else
                total += v, previous = v;
        }
        if (total == 0 || total > 39) return Marker::None;
        value = total;
        return Marker::Roman;
    }
    if (core.size() == 1 && core[0] >= 'a' && core[0] <= 'z') {
        value = static_cast<uint32_t>(core[0] - 'a' + 1);
        return Marker::Letter;
    }
    return Marker::None;
}

void LineText(const PageData& page, const Line& line, std::string& out, uint32_t skip) {
    out.clear();
    for (uint32_t k = skip; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (k > skip && (g.flags & kGlyphSpaceBefore)) out.push_back(' ');
        out.append(page.GlyphText(g));
    }
}

void BuildLines(PageData& page) {
    page.lines.clear();
    page.lineGlyphs.clear();
    std::vector<Glyph>& glyphs = page.glyphs;

    // Hidden text (algorithms §4.3): invisible, clipped or tiny glyphs leave, except that invisible
    // text is the page's text when nothing visible is (an OCR layer).
    bool visibleText = false, invisibleText = false;
    for (const Glyph& g : glyphs) {
        if (g.flags & (kGlyphDropped | kGlyphSpace)) continue;
        if (g.flags & kGlyphClipped || g.size < 1.0f) continue;
        if (g.flags & kGlyphInvisible)
            invisibleText = true;
        else
            visibleText = true;
    }
    const bool keepInvisible = !visibleText && invisibleText;
    std::vector<uint32_t> vis;
    vis.reserve(glyphs.size());
    for (uint32_t i = 0; i < glyphs.size(); ++i) {
        Glyph& g = glyphs[i];
        if (g.flags & kGlyphDropped) continue;
        const bool hidden = (g.flags & kGlyphClipped) || g.size < 1.0f || ((g.flags & kGlyphInvisible) && !keepInvisible);
        if (hidden) {
            g.flags |= kGlyphDropped;
            if (!(g.flags & kGlyphSpace)) page.hiddenGlyphs++;
            continue;
        }
        // Codes without text carry nothing to place.
        if (g.textLength == 0) continue;
        // Typographic signals come from rendered text only: an invisible layer (OCR text over a
        // scan) has no face to be bold, italic or fixed-pitch in, whatever its font says.
        if (g.flags & kGlyphInvisible)
            g.flags = static_cast<uint16_t>(g.flags & ~(kGlyphBold | kGlyphItalic | kGlyphMonospace));
        vis.push_back(i);
    }
    FoldDuplicates(page, vis);

    // Fragments in content order (algorithms §4.4 recommendation: one pass merging into the
    // current line while direction and baseline match).
    std::vector<Fragment> frags;
    for (uint32_t k = 0; k < vis.size(); ++k) {
        const Glyph& g = glyphs[vis[k]];
        const bool space = (g.flags & kGlyphSpace) != 0;
        if (!frags.empty()) {
            Fragment& f = frags.back();
            const float em = std::fmax(f.size, g.size);
            const float gap = g.u0 - f.lastU1;
            const float dv = g.v - f.baseline;
            const bool sameBase = std::fabs(dv) <= 0.25f * em;
            const bool script = !sameBase && std::fabs(dv) <= 0.6f * em &&
                                (g.size < 0.87f * f.size || f.lastSize < 0.87f * g.size);
            const float maxGap = (f.marker ? kMarkerGap : kFragmentGap) * em;
            if (g.dir == f.dir && (sameBase || script || space) && gap >= -0.6f * em && gap <= maxGap &&
                std::fabs(dv) <= 0.6f * em) {
                f.end = k + 1;
                f.lastU1 = std::fmax(f.lastU1, g.u1);
                if (!space) {
                    f.lastSize = g.size;
                    if (!f.any) {
                        f.u0 = g.u0, f.u1 = g.u1, f.vb = g.vb, f.vt = g.vt;
                        f.baseline = g.v, f.size = g.size;
                        f.any = true;
                    } else {
                        f.u0 = std::fmin(f.u0, g.u0), f.u1 = std::fmax(f.u1, g.u1);
                        f.vb = std::fmin(f.vb, g.vb), f.vt = std::fmax(f.vt, g.vt);
                        // A fragment that began with a superscript takes the baseline of the
                        // full-size text that follows.
                        if (g.size > f.size * 1.15f) f.baseline = g.v, f.size = g.size;
                    }
                    f.marker = false;
                }
                continue;
            }
        }
        Fragment f;
        f.begin = k;
        f.end = k + 1;
        f.dir = g.dir;
        f.u0 = g.u0, f.u1 = g.u1, f.vb = g.vb, f.vt = g.vt;
        f.baseline = g.v;
        f.size = g.size;
        f.lastU1 = g.u1;
        f.lastSize = g.size;
        f.any = !space;
        if (f.any) {
            uint32_t value;
            f.marker = ParseMarker(page.GlyphText(g), value) != Marker::None;
        }
        frags.push_back(f);
    }
    // Leading space-only fragments carry nothing.
    // Sort into baseline clusters per direction, then by position along the baseline.
    std::vector<uint32_t> fo;
    fo.reserve(frags.size());
    for (uint32_t i = 0; i < frags.size(); ++i)
        if (frags[i].any) fo.push_back(i);
    std::sort(fo.begin(), fo.end(), [&](uint32_t a, uint32_t b) {
        const Fragment& x = frags[a];
        const Fragment& y = frags[b];
        if (x.dir != y.dir) return x.dir < y.dir;
        if (x.baseline != y.baseline) return x.baseline > y.baseline;
        return x.u0 < y.u0;
    });

    // Line groups: runs of `members` (fragment indices) in u order.
    struct Group {
        uint32_t first = 0, count = 0;
        float u0 = 0, u1 = 0, vb = 0, vt = 0, baseline = 0, size = 0;
        uint8_t dir = 0;
        bool merged = false;   // absorbed into another group
        bool scripts = false;  // received a superscript or subscript fragment: sort glyphs by position
    };
    std::vector<uint32_t> members;
    members.reserve(fo.size());
    std::vector<Group> groups;
    std::vector<uint32_t> cluster;
    for (size_t at = 0; at < fo.size();) {
        const Fragment& head = frags[fo[at]];
        size_t end = at + 1;
        while (end < fo.size()) {
            const Fragment& f = frags[fo[end]];
            if (f.dir != head.dir || head.baseline - f.baseline > 0.25f * std::fmax(head.size, f.size)) break;
            ++end;
        }
        cluster.assign(fo.begin() + static_cast<std::ptrdiff_t>(at), fo.begin() + static_cast<std::ptrdiff_t>(end));
        std::sort(cluster.begin(), cluster.end(), [&](uint32_t a, uint32_t b) { return frags[a].u0 < frags[b].u0; });
        Group g;
        for (size_t c = 0; c < cluster.size(); ++c) {
            const Fragment& f = frags[cluster[c]];
            if (g.count > 0) {
                const float em = std::fmax(g.size, f.size);
                const bool lastMarker = frags[members.back()].marker && g.count == 1;
                const float maxGap = (lastMarker ? kMarkerGap : kJoinGap) * em;
                if (f.u0 - g.u1 <= maxGap) {
                    members.push_back(cluster[c]);
                    g.count++;
                    g.u0 = std::fmin(g.u0, f.u0), g.u1 = std::fmax(g.u1, f.u1);
                    g.vb = std::fmin(g.vb, f.vb), g.vt = std::fmax(g.vt, f.vt);
                    if (f.size > g.size) g.size = f.size, g.baseline = f.baseline;
                    continue;
                }
                groups.push_back(g);
            }
            g = Group();
            g.first = static_cast<uint32_t>(members.size());
            g.count = 1;
            members.push_back(cluster[c]);
            g.u0 = f.u0, g.u1 = f.u1, g.vb = f.vb, g.vt = f.vt;
            g.baseline = f.baseline, g.size = f.size, g.dir = f.dir;
        }
        if (g.count > 0) groups.push_back(g);
        at = end;
    }

    // A small fragment left alone (a superscript or subscript out of content order) joins the
    // full-size line it sits in (algorithms §4.4, MuPDF's 0.8 em baseline tolerance).
    std::vector<std::vector<uint32_t>> extra;  // fragments added to groups, by group, rarely used
    std::vector<uint32_t> extraOf(groups.size(), kEmpty);
    for (size_t s = 0; s < groups.size(); ++s) {
        Group& small = groups[s];
        if (small.count != 1) continue;
        const Fragment& f = frags[members[small.first]];
        if (f.end - f.begin > 12) continue;
        const float center = (small.vb + small.vt) / 2;
        // Groups are sorted by baseline, descending, per direction: look at neighbours only.
        size_t best = kEmpty;
        for (size_t d = 1; d <= 8; ++d) {
            for (int side = 0; side < 2 && best == kEmpty; ++side) {
                if (side == 0 && s < d) continue;
                const size_t o = side == 0 ? s - d : s + d;
                if (o >= groups.size()) continue;
                const Group& line = groups[o];
                if (line.merged || line.dir != small.dir || line.size < small.size / 0.87f) continue;
                if (center < line.vb || center > line.vt) continue;
                if (small.u0 < line.u0 - 0.3f * line.size || small.u0 - line.u1 > 0.6f * line.size) continue;
                best = o;
            }
            if (best != kEmpty) break;
        }
        if (best == kEmpty) continue;
        Group& line = groups[best];
        small.merged = true;
        line.scripts = true;
        line.u0 = std::fmin(line.u0, small.u0), line.u1 = std::fmax(line.u1, small.u1);
        if (extraOf[best] == kEmpty) {
            extraOf[best] = static_cast<uint32_t>(extra.size());
            extra.emplace_back();
        }
        extra[extraOf[best]].push_back(members[small.first]);
    }

    // Proto lines: glyphs in order with their word breaks set, before gutter splitting.
    struct Proto {
        uint32_t first = 0, count = 0;  // in `proto`
        uint32_t segFirst = 0, segCount = 0;
        float baseline = 0, size = 0, u0 = 0, u1 = 0;
        uint8_t dir = 0;
    };
    struct Segment {
        float u0, u1;
        uint32_t start;  // position in `proto` of its first glyph
    };
    std::vector<uint32_t> proto;
    std::vector<Proto> protos;
    std::vector<Segment> segments;
    std::vector<uint32_t> lineVis;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& grp = groups[gi];
        if (grp.merged) continue;
        lineVis.clear();
        for (uint32_t m = 0; m < grp.count; ++m) {
            const Fragment& f = frags[members[grp.first + m]];
            for (uint32_t k = f.begin; k < f.end; ++k) lineVis.push_back(vis[k]);
        }
        if (extraOf[gi] != kEmpty) {
            for (uint32_t fi : extra[extraOf[gi]]) {
                const Fragment& f = frags[fi];
                for (uint32_t k = f.begin; k < f.end; ++k) lineVis.push_back(vis[k]);
            }
        }
        if (grp.scripts) {
            std::stable_sort(lineVis.begin(), lineVis.end(), [&](uint32_t a, uint32_t b) {
                return glyphs[a].u0 + glyphs[a].u1 < glyphs[b].u0 + glyphs[b].u1;
            });
        }
        // Explicit spaces decide the word breaks of lines that have them (algorithms §4.1).
        uint32_t spaces = 0;
        for (uint32_t i : lineVis)
            if (glyphs[i].flags & kGlyphSpace) spaces++;
        const float wordGap = spaces > 0 ? kWordGapWithSpaces : kWordGap;
        Proto p;
        p.first = static_cast<uint32_t>(proto.size());
        p.segFirst = static_cast<uint32_t>(segments.size());
        p.dir = grp.dir;
        p.baseline = grp.baseline;
        p.size = grp.size;
        bool pendingSpace = false;
        float right = 0;
        float previousSize = 0;
        bool first = true;
        for (uint32_t i : lineVis) {
            Glyph& g = glyphs[i];
            if (g.flags & kGlyphSpace) {
                if (!first) pendingSpace = true;
                continue;
            }
            g.flags = static_cast<uint16_t>(g.flags & ~kGlyphSpaceBefore);
            if (!first) {
                const float em = std::fmax(std::fmin(previousSize, g.size), 0.5f * std::fmax(previousSize, g.size));
                const float gap = g.u0 - right;
                const bool combining = StartsWithCombiningMark(page.GlyphText(g));
                if (!combining && (pendingSpace || gap > wordGap * em)) g.flags |= kGlyphSpaceBefore;
                // Ink segments: runs of glyphs without a wide gap, for gutter detection.
                if (gap > kGutterCandidate * grp.size) {
                    segments.push_back(Segment{g.u0, g.u1, static_cast<uint32_t>(proto.size())});
                } else {
                    segments.back().u1 = std::fmax(segments.back().u1, g.u1);
                }
                right = std::fmax(right, g.u1);
                p.u0 = std::fmin(p.u0, g.u0), p.u1 = std::fmax(p.u1, g.u1);
            } else {
                segments.push_back(Segment{g.u0, g.u1, static_cast<uint32_t>(proto.size())});
                right = g.u1;
                p.u0 = g.u0, p.u1 = g.u1;
            }
            pendingSpace = false;
            first = false;
            previousSize = g.size;
            proto.push_back(i);
        }
        p.count = static_cast<uint32_t>(proto.size() - p.first);
        p.segCount = static_cast<uint32_t>(segments.size() - p.segFirst);
        if (p.count > 0) protos.push_back(p);
    }

    // Gutters (Breuel's whitespace columns, algorithms §5.1): a wide gap in a line is a column or
    // cell boundary when the lines around it have no ink at that place either; a wide space of
    // justified text has ink above and below it. The line splits there.
    std::vector<uint8_t> splitBefore(proto.size(), 0);
    {
        std::vector<uint32_t> byBase(protos.size());
        for (uint32_t i = 0; i < protos.size(); ++i) byBase[i] = i;
        std::sort(byBase.begin(), byBase.end(), [&](uint32_t a, uint32_t b) {
            if (protos[a].dir != protos[b].dir) return protos[a].dir < protos[b].dir;
            return protos[a].baseline > protos[b].baseline;
        });
        uint32_t candidates = 0;
        for (size_t r = 0; r < byBase.size(); ++r) {
            const Proto& l = protos[byBase[r]];
            if (l.segCount < 2) continue;
            const float window = 8 * l.size;
            for (uint32_t s = 1; s < l.segCount && candidates < 20000; ++s, ++candidates) {
                const uint32_t start = segments[l.segFirst + s].start;
                // Aligned columns inside code stay one line.
                if ((glyphs[proto[start]].flags & glyphs[proto[start - 1]].flags & kGlyphMonospace) != 0) continue;
                const float a = segments[l.segFirst + s - 1].u1, b = segments[l.segFirst + s].u0;
                const float c = (a + b) / 2;
                const float pad = 0.1f * l.size;
                int blocked = 0, clear = 0, oneSide = 0;
                for (int side = 0; side < 2; ++side) {
                    for (size_t o = r;;) {
                        if (side == 0) {
                            if (o == 0) break;
                            --o;
                        } else {
                            if (++o >= byBase.size()) break;
                        }
                        const Proto& n = protos[byBase[o]];
                        if (n.dir != l.dir || std::fabs(n.baseline - l.baseline) > window) break;
                        if (n.u1 < a - 20 * l.size || n.u0 > b + 20 * l.size) continue;
                        bool ink = false, left = false, right = false;
                        for (uint32_t t = 0; t < n.segCount; ++t) {
                            const Segment& sg = segments[n.segFirst + t];
                            if (sg.u0 - pad <= c && c <= sg.u1 + pad) ink = true;
                            if (sg.u1 < c) left = true;
                            if (sg.u0 > c) right = true;
                        }
                        if (ink)
                            blocked++;
                        else if (left && right)
                            clear++;
                        else
                            oneSide++;
                    }
                }
                const bool gutter = (clear + oneSide >= 2) && blocked * 8 <= clear + oneSide;
                if (gutter) splitBefore[start] = 1;
            }
        }
    }

    // Lines: metrics of each piece.
    struct SizeCount {
        float size;
        uint32_t count;
    };
    std::vector<SizeCount> sizes;
    for (const Proto& p : protos) {
        uint32_t at = p.first;
        const uint32_t end = p.first + p.count;
        while (at < end) {
            uint32_t stop = at + 1;
            while (stop < end && !splitBefore[stop]) ++stop;
            Line line;
            line.first = static_cast<uint32_t>(page.lineGlyphs.size());
            line.dir = p.dir;
            uint16_t allFlags = kGlyphBold | kGlyphItalic | kGlyphMonospace | kGlyphArtifact | kGlyphInvisible;
            sizes.clear();
            for (uint32_t k = at; k < stop; ++k) {
                Glyph& g = glyphs[proto[k]];
                if (k == at) {
                    g.flags = static_cast<uint16_t>(g.flags & ~kGlyphSpaceBefore);
                    line.u0 = g.u0, line.u1 = g.u1, line.vb = g.vb, line.vt = g.vt;
                }
                line.u0 = std::fmin(line.u0, g.u0), line.u1 = std::fmax(line.u1, g.u1);
                line.vb = std::fmin(line.vb, g.vb), line.vt = std::fmax(line.vt, g.vt);
                allFlags &= g.flags;
                line.chars++;
                bool counted = false;
                for (SizeCount& sc : sizes)
                    if (std::fabs(sc.size - g.size) < 0.25f) {
                        sc.count++;
                        counted = true;
                        break;
                    }
                if (!counted && sizes.size() < 32) sizes.push_back({g.size, 1});
                page.lineGlyphs.push_back(proto[k]);
            }
            line.count = stop - at;
            SizeCount best{0, 0};
            for (const SizeCount& sc : sizes)
                if (sc.count > best.count || (sc.count == best.count && sc.size > best.size)) best = sc;
            line.size = best.size;
            line.baseline = p.baseline;
            for (uint32_t k = 0; k < line.count; ++k) {
                const Glyph& g = glyphs[page.lineGlyphs[line.first + k]];
                if (std::fabs(g.size - best.size) < 0.25f) {
                    line.baseline = g.v;
                    break;
                }
            }
            if (allFlags & kGlyphBold) line.flags |= kLineAllBold;
            if (allFlags & kGlyphItalic) line.flags |= kLineAllItalic;
            if (allFlags & kGlyphMonospace) line.flags |= kLineAllMono;
            if (allFlags & kGlyphArtifact) line.flags |= kLineArtifact;
            if (allFlags & kGlyphInvisible) line.flags |= kLineInvisible;
            page.lines.push_back(line);
            at = stop;
        }
    }
}

} // namespace RayoPdf::Layout
