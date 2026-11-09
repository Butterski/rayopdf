// SPDX-License-Identifier: Apache-2.0
#include "layout/lines.h"

#include "common/compiler.h"
#include "common/minmax.h"
#include "common/per_thread.h"
#include "common/utf8.h"
#include "text/bidi.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <type_traits>

namespace RayoPdf::Layout {
namespace {

constexpr uint32_t kEmpty = UINT32_MAX;

uint64_t HashText(std::string_view s) {
    constexpr uint64_t kBasis = 1469598103934665603ull, kPrime = 1099511628211ull;
    // One byte (most glyphs): the loop's single step.
    if (s.size() == 1) return (kBasis ^ static_cast<unsigned char>(s[0])) * kPrime;
    uint64_t h = kBasis;
    for (unsigned char c : s) h = (h ^ c) * kPrime;
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
    bool spaces = false; // has a space glyph
};

// CollectAndFold's working storage, kept by each thread between pages of up to kScratchSlots probe
// slots, so a page fills none of it whole: a filter word empties by clearing the words its page's
// entries set; chain heads and the probe's table hold base + an index, and values below `base` are
// empty, so moving the base past a page's indices at its end empties both.
constexpr size_t kScratchSlots = size_t(1) << 16;
struct FoldWord {
    uint64_t bits;  // a bit per column of a kept glyph here; 0 outside a call
    uint32_t head;  // base + the last entry chained here; below `base` when none
    uint32_t spare;
};
struct FoldEntry {
    uint32_t next;   // base + the entry chained here before, or below `base`
    uint32_t word;   // its filter word
    uint32_t glyph;  // the kept glyph
};
struct FoldScratch {
    std::vector<FoldWord> words;
    std::vector<FoldEntry> entries;
    std::vector<uint32_t> table;  // the probe's table; below `base` outside a call
    uint32_t base = 1;
};

constexpr uint32_t kMaxChainSteps = 256;  // chain entries a glyph looks at before taking the probe
constexpr float kProbeCell = 2.0f;
constexpr float kTestReach = 1e7f;      // glyphs beyond take the probe
constexpr float kKeepReach = 2e7f;      // kept glyphs beyond cannot be within reach + 8
constexpr float kTestTolerance = 8.0f;  // glyphs of larger tolerances take the probe
constexpr float kFloatReach = 2048.0f;  // glyphs nearer the origin have their cells in float
// Columns floor(x0) + kColumnOrigin and rows floor(y0 / 8) + kRowOrigin, modulo 2^32: positive
// near the origin, so that truncation is the floor there.
constexpr int64_t kColumnOrigin = 4096, kRowOrigin = 1024;
// A filter word: the top bits of key + column block * kColumnMix + row * kRowMix.
constexpr uint64_t kColumnMix = 0x9E3779B97F4A7C15ull, kRowMix = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t kKeyMix = 0xBF58476D1CE4E5B9ull;

// The filter's key of a text and direction: equal for equal texts and directions.
RAYOPDF_HOT_INLINE uint64_t KeyOf(const char* text, uint32_t length, uint8_t dir) {
    if (length == 1) return (static_cast<uint64_t>(static_cast<unsigned char>(text[0])) | (static_cast<uint64_t>(dir) << 8)) * kKeyMix;
    return (HashText(std::string_view(text, length)) ^ (static_cast<uint64_t>(dir) << 56)) * kKeyMix;
}

// floor(c) for |c| < 2^62 without a libm call.
int64_t FloorOf(double c) {
    const int64_t t = static_cast<int64_t>(c);
    return t - (c < static_cast<double>(t) ? 1 : 0);
}

// floor(v) of a clamped coordinate (|v| <= 1e15) by one integer conversion: a float of 2^23 or more
// is an integer, so the conversion and the float of its result are exact.
int64_t FloorOf(float v) {
    const int64_t t = static_cast<int64_t>(v);
    return t - (v < static_cast<float>(t) ? 1 : 0);
}

bool Near(const Glyph& g) { return std::fabs(g.x0) < kFloatReach && std::fabs(g.y0) < kFloatReach; }

// A near glyph's column and row.
RAYOPDF_HOT_INLINE void NearCells(const Glyph& g, uint32_t& column, uint32_t& row) {
    column = static_cast<uint32_t>(static_cast<int32_t>(g.x0 + 4096.0f));
    row = static_cast<uint32_t>(static_cast<int32_t>(g.y0 * 0.125f + 1024.0f));
}

// A glyph's column and row; false beyond kKeepReach (or NaN).
bool CellsOf(const Glyph& g, uint32_t& column, uint32_t& row) {
    if (Near(g)) {
        NearCells(g, column, row);
        return true;
    }
    if (!(std::fabs(g.x0) < kKeepReach && std::fabs(g.y0) < kKeepReach)) return false;
    column = static_cast<uint32_t>(FloorOf(static_cast<double>(g.x0)) + kColumnOrigin);
    row = static_cast<uint32_t>(FloorOf(static_cast<double>(g.y0) / 8) + kRowOrigin);
    return true;
}

// The columns [ca, cb] and rows [ra, rb] under a near glyph's window.
RAYOPDF_HOT_INLINE void NearWindow(const Glyph& g, float tolerance, uint32_t& ca, uint32_t& cb, uint32_t& ra, uint32_t& rb) {
    const float rx = tolerance + 1.0f / 512, ry = tolerance * 0.125f + 1.0f / 2048;
    const float qy = g.y0 * 0.125f;
    ca = static_cast<uint32_t>(static_cast<int32_t>(g.x0 + (4096.0f - rx)));
    cb = static_cast<uint32_t>(static_cast<int32_t>(g.x0 + (4096.0f + rx)));
    ra = static_cast<uint32_t>(static_cast<int32_t>(qy + (1024.0f - ry)));
    rb = static_cast<uint32_t>(static_cast<int32_t>(qy + (1024.0f + ry)));
}

// The same for any glyph within kTestReach. Far from the origin the columns and rows may wrap
// around 2^32 inside the window (cb < ca or rb < ra).
void WindowOf(const Glyph& g, float tolerance, uint32_t& ca, uint32_t& cb, uint32_t& ra, uint32_t& rb) {
    if (Near(g)) {
        NearWindow(g, tolerance, ca, cb, ra, rb);
        return;
    }
    const double reach = static_cast<double>(tolerance) + 1.0 / 1024;
    ca = static_cast<uint32_t>(FloorOf(g.x0 - reach) + kColumnOrigin);
    cb = static_cast<uint32_t>(FloorOf(g.x0 + reach) + kColumnOrigin);
    ra = static_cast<uint32_t>(FloorOf((g.y0 - reach) / 8) + kRowOrigin);
    rb = static_cast<uint32_t>(FloorOf((g.y0 + reach) / 8) + kRowOrigin);
}

// The probe's home cell of a glyph. Coordinates are finite but may be huge: clamped before the
// integer conversion.
void ProbeCell(const Glyph& g, int64_t& cx, int64_t& cy) {
    if (Near(g)) {
        cx = FloorOf(g.x0 / kProbeCell);
        cy = FloorOf(g.y0 / kProbeCell);
    } else {
        cx = FloorOf(Fmin(Fmax(g.x0 / kProbeCell, -1e15f), 1e15f));
        cy = FloorOf(Fmin(Fmax(g.y0 / kProbeCell, -1e15f), 1e15f));
    }
}

// Whether BuildLines places glyph g (not yet looked at): not dropped, not hidden, with text.
bool Placed(const Glyph& g, bool keepInvisible) {
    if (g.flags & kGlyphDropped) return false;
    if ((g.flags & kGlyphClipped) || g.size < 1.0f ||
        ((g.flags & (kGlyphInvisible | kGlyphOcr)) == kGlyphInvisible && !keepInvisible))
        return false;
    return g.textLength != 0;
}

// One page's CollectAndFold (below): its storage, and the whole rule for a glyph.
struct Folder {
    PageData& page;
    uint32_t* vis = nullptr;
    bool keepInvisible = false;
    size_t wordMask = 0;
    FoldWord* words = nullptr;
    FoldEntry* entries = nullptr;
    uint32_t* table = nullptr;  // made when the probe is first needed
    uint64_t mask = 0;
    uint32_t base = 1, kept = 0, entryCount = 0;
    uint32_t placed = 0;  // glyphs placed so far, the current one included
    FoldScratch* fold = nullptr;
    std::vector<FoldWord> ownWords;
    std::vector<FoldEntry> ownEntries;
    std::vector<uint32_t> ownTable;

    explicit Folder(PageData& p) : page(p) {}
    size_t WordOf(uint64_t key, uint32_t column, uint32_t row) const {
        return static_cast<size_t>((key + uint64_t(column >> 6) * kColumnMix + uint64_t(row) * kRowMix) >> 32) & wordMask;
    }
    void Chain(size_t word, uint32_t column, uint32_t glyph) {
        FoldWord& w = words[word];
        w.bits |= uint64_t(1) << (column & 63);
        entries[entryCount] = FoldEntry{w.head, static_cast<uint32_t>(word), glyph};
        w.head = base + entryCount++;
    }
    void Insert(uint32_t position, const Glyph& g, uint64_t h) {
        int64_t cx, cy;
        ProbeCell(g, cx, cy);
        uint64_t slot = Mix(cx, cy, h) & mask;
        while (table[slot] >= base) slot = (slot + 1) & mask;
        table[slot] = base + position;
    }
    // A glyph kept outside Place while the table exists joins it as Place would add it.
    RAYOPDF_NOINLINE void InsertKept(uint32_t position, const Glyph& g) { Insert(position, g, HashText(page.GlyphText(g))); }
    // Whether kept glyph `o` is one the probe takes for `g` (whose text, and so its key, it has).
    bool Matches(const Glyph& o, const Glyph& g, float tolerance, std::string_view text) const {
        return o.dir == g.dir && std::fabs(o.x0 - g.x0) < tolerance && std::fabs(o.y0 - g.y0) < tolerance &&
               std::fabs(o.size - g.size) < tolerance && page.GlyphText(o) == text;
    }
    void MakeTable(uint32_t i);
    RAYOPDF_NOINLINE void Place(uint32_t i);
};

// The probe's table, sized as for all the page's placed glyphs (those after glyph i counted
// ahead), filled with the glyphs kept before i in their order.
void Folder::MakeTable(uint32_t i) {
    const std::vector<Glyph>& glyphs = page.glyphs;
    size_t count = placed;
    for (size_t j = size_t(i) + 1; j < glyphs.size(); ++j) count += Placed(glyphs[j], keepInvisible) ? 1 : 0;
    size_t capacity = 16;
    while (capacity < count * 2) capacity <<= 1;
    mask = capacity - 1;
    if (capacity <= kScratchSlots) {
        if (fold->table.size() < capacity) fold->table.assign(capacity, 0);
        table = fold->table.data();
    } else {
        ownTable.assign(capacity, 0);
        table = ownTable.data();
    }
    for (uint32_t p = 0; p < kept; ++p) {
        const Glyph& o = glyphs[vis[p]];
        if (!(o.flags & kGlyphSpace)) Insert(p, o, HashText(page.GlyphText(o)));
    }
}

// The rule for glyph i (placed, no space), kept or dropped.
void Folder::Place(uint32_t i) {
    std::vector<Glyph>& glyphs = page.glyphs;
    Glyph& g = glyphs[i];
    const std::string_view text = page.GlyphText(g);
    const uint64_t key = KeyOf(text.data(), static_cast<uint32_t>(text.size()), g.dir);
    // Fmax against a constant: a NaN size gives 0.5 either way.
    const float scaled = 0.15f * g.size, tolerance = scaled > 0.5f ? scaled : 0.5f;
    bool probe = true;
    uint32_t found = kEmpty;
    if (tolerance <= kTestTolerance && std::fabs(g.x0) < kTestReach && std::fabs(g.y0) < kTestReach) {
        uint32_t ca, cb, ra, rb;
        WindowOf(g, tolerance, ca, cb, ra, rb);
        // The window's words (at most two blocks of columns by three rows), each once, with the
        // bits of its columns.
        size_t at[6];
        uint64_t bits[6];
        uint32_t distinct = 0;
        if (ca <= cb && ra <= rb && cb - ca <= 64 && rb - ra <= 2) {
            for (uint32_t b = 0; b <= (cb >> 6) - (ca >> 6); ++b) {
                const uint32_t block = (ca >> 6) + b;
                const uint32_t lo = std::max(ca, block << 6), hi = std::min(cb, (block << 6) + 63);
                const uint64_t columns = ((uint64_t(2) << (hi - lo)) - 1) << (lo & 63);
                for (uint32_t r = 0; r <= rb - ra; ++r) {
                    const uint32_t row = ra + r;
                    const size_t word = WordOf(key, lo, row);
                    uint32_t d = 0;
                    while (d < distinct && at[d] != word) ++d;
                    if (d == distinct) at[distinct] = word, bits[distinct++] = 0;
                    bits[d] |= columns;
                }
            }
            bool hit = false;
            for (uint32_t d = 0; d < distinct; ++d) hit = hit || (words[at[d]].bits & bits[d]) != 0;
            if (!hit) {
                probe = false;
            } else {
                uint32_t count = 0, steps = 0;
                for (uint32_t d = 0; d < distinct; ++d) {
                    for (uint32_t p = words[at[d]].head; p >= base && steps < kMaxChainSteps; ++steps) {
                        const FoldEntry& e = entries[p - base];
                        if (Matches(glyphs[e.glyph], g, tolerance, text)) count++, found = e.glyph;
                        p = e.next;
                    }
                }
                probe = count > 1 || (count == 1 && tolerance > 2.0f) || steps == kMaxChainSteps;
            }
        }
    }
    if (probe) {
        if (!table) MakeTable(i);
        const uint64_t h = HashText(text);
        int64_t cx, cy;
        ProbeCell(g, cx, cy);
        found = kEmpty;
        for (int dx = -1; dx <= 1 && found == kEmpty; ++dx) {
            for (int dy = -1; dy <= 1 && found == kEmpty; ++dy) {
                uint64_t slot = Mix(cx + dx, cy + dy, h) & mask;
                for (size_t probes = 0; table[slot] >= base && probes <= mask; ++probes) {
                    const uint32_t p = table[slot] - base;
                    if (Matches(glyphs[vis[p]], g, tolerance, text)) {
                        found = vis[p];
                        break;
                    }
                    slot = (slot + 1) & mask;
                }
            }
        }
    }
    if (found != kEmpty) {
        g.flags |= kGlyphDropped;
        if (!(glyphs[found].flags & kGlyphInvisible)) glyphs[found].flags |= kGlyphBold;
        page.hiddenGlyphs++;
        return;
    }
    if (table) Insert(kept, g, HashText(text));
    uint32_t column, row;
    if (CellsOf(g, column, row)) Chain(WordOf(key, column, row), column, i);
    vis[kept++] = i;
}

// Collects the glyphs BuildLines places into `vis`, in content order: hidden text (algorithms
// §4.3: invisible, clipped or tiny glyphs, except invisible text when `keepInvisible`) leaves, and
// overprinted duplicates (the same text drawn again within 0.15 em, algorithms §4.2) fold into
// one glyph marked bold. True when the frame box, baseline and size of every placed glyph are
// finite (when they may not be, false).
//
// The duplicate rule is the probe's: it looks in the nine 2-unit cells around a glyph (a hash
// table filled in content order, linear probing) for a kept glyph of the same direction and text
// (hash, then bytes) whose x0, y0 and size lie within the glyph's tolerance t, and takes the first
// it meets. A kept glyph it could take lies in the window [x0 - t, x0 + t] x [y0 - t, y0 + t]
// (|a - b| < t in float implies it for the real difference too). Most glyphs have none, and exact
// steps find that out before the probe, each looking only at cells under that window: kept glyphs
// are placed by column (1 unit wide) and row (8 units high), and each sets the bit of its column
// in the 64-bit word of (text and direction key, row, its column's block of 64), and is chained
// to that word.
// 1. No bit of the window's columns set in the words of its rows: nothing to find. Most glyphs
//    stop here, in the loop below; the rest take Folder::Place, which runs the whole rule.
// 2. Otherwise the words' chains are walked, each entry tested as the probe tests (equal texts
//    have equal keys):
//    - none matches: the probe would find none either;
//    - one, and t <= 2: the window lies within the probe's nine cells (a match's 2-unit cell is
//      floor((x0 +- t) / 2), within one of the glyph's), and the probe walks the run of slots from
//      each cell's home slot, which reaches every glyph homed there: it would take that one.
// 3. Otherwise (two or more, which the probe orders by its slots; a larger t, where the probe sees
//    only part of the window; a chain too long to walk; a window not tested) the probe decides.
//    Its table is built when first needed, sized for all the page's placed glyphs, from the glyphs
//    kept before, in their order, then grows with each kept glyph: the same slots as a table
//    filled from the page's first glyph.
//
// Cells in float below 2048 units (so that truncation, of sums made positive, is the floor):
// x0 + 4096 and y0 / 8 + 1024 (y0 / 8 exact) round by at most 2^-12 and 2^-14 (half their ulp),
// a kept glyph's cell by as much, and a window's ends add the rounding of r (2^-21) and of the
// origin -+ r (as much again): 3 * 2^-12 + 2^-21 and 3 * 2^-14 + 2^-21, less than the margins of
// 2^-9 and 2^-11 by which the windows widen. Larger coordinates take exact cells in double, and
// windows widened by 1/1024. Rounding is monotonic, so a glyph's own cell lies within its window.
//
// Finiteness: the sum of the six values of each placed glyph, summed over the page, is finite when
// all are (an overflow only sends a page with finite values the checked way).
bool CollectAndFold(PageData& page, std::vector<uint32_t>& vis, bool keepInvisible) {
    std::vector<Glyph>& glyphVector = page.glyphs;
    const size_t n = glyphVector.size();
    if (vis.size() < n) vis.resize(n);
    Folder f(page);
    f.vis = vis.data();
    f.keepInvisible = keepInvisible;
    // The filter: two words per probe slot for all the page's glyphs.
    size_t capacity = 16;
    while (capacity < n * 2) capacity <<= 1;
    const size_t filterWords = capacity * 2;
    f.wordMask = filterWords - 1;
    FoldScratch& fold = PerThread<FoldScratch>();
    f.fold = &fold;
    if (UINT32_MAX - fold.base < n) {
        for (FoldWord& w : fold.words) w.head = 0;
        fold.table.assign(fold.table.size(), 0);
        fold.base = 1;
    }
    f.base = fold.base;
    const bool scratch = capacity <= kScratchSlots;
    if (scratch) {
        if (fold.words.size() < filterWords) fold.words.resize(filterWords, FoldWord{0, 0, 0});
        if (fold.entries.size() < n) fold.entries.resize(n);
        f.words = fold.words.data();
        f.entries = fold.entries.data();
    } else {
        f.ownWords.assign(filterWords, FoldWord{0, 0, 0});
        f.ownEntries.resize(n);
        f.words = f.ownWords.data();
        f.entries = f.ownEntries.data();
    }
    // The common case in locals: a placed glyph near the origin, of a tolerance up to 3.5 (its
    // window has one or two rows: under one row high, rounding included), whose window's columns
    // lie in one block, with no filter bit set there (kept, and added to the probe's table when
    // that exists).
    uint32_t* const order = f.vis;
    Glyph* const glyphs = glyphVector.data();
    const char* const textData = page.text.data();
    const size_t textSize = page.text.size();
    FoldWord* const words = f.words;
    FoldEntry* const entries = f.entries;
    const uint32_t base = f.base;
    const size_t wordMask = f.wordMask;
    // NearWindow's offsets for the size of the glyph before (most glyphs share it): the same
    // values, computed once per size.
    float windowSize = NAN, x0Low = 0, x0High = 0, qyLow = 0, qyHigh = 0;
    bool windowSmall = false;  // tolerance <= 3.5
    bool probing = false;  // the probe's table exists
    uint32_t kept = 0, entryCount = 0, placed = 0;
    float sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        Glyph& g = glyphs[i];
        if (RAYOPDF_UNLIKELY((g.flags & (kGlyphDropped | kGlyphClipped | kGlyphInvisible)) != 0 || g.size < 1.0f ||
                             g.textLength == 0)) {
            const uint16_t flags = g.flags;
            if (flags & kGlyphDropped) continue;
            const bool hidden = (flags & kGlyphClipped) || g.size < 1.0f ||
                                ((flags & (kGlyphInvisible | kGlyphOcr)) == kGlyphInvisible && !keepInvisible);
            if (hidden) {
                g.flags |= kGlyphDropped;
                if (!(flags & kGlyphSpace)) page.hiddenGlyphs++;
                continue;
            }
            // Codes without text carry nothing to place.
            if (g.textLength == 0) continue;
            // Typographic signals come from rendered text only: an invisible layer that came with
            // the PDF (OCR text over a scan) has no face to be bold, italic or fixed-pitch in,
            // whatever its font says. The add-on's OCR layer (kGlyphOcr) carries what the scan's
            // pixels show.
            if ((flags & (kGlyphInvisible | kGlyphOcr)) == kGlyphInvisible)
                g.flags = static_cast<uint16_t>(flags & ~(kGlyphBold | kGlyphItalic | kGlyphMonospace));
        }
        placed++;
        sum += ((g.u0 + g.u1) + (g.vb + g.vt)) + (g.v + g.size);
        if (g.flags & kGlyphSpace) {
            order[kept++] = i;
            continue;
        }
        if (RAYOPDF_LIKELY(Near(g))) {
            if (!(g.size == windowSize)) {
                const float scaled = 0.15f * g.size, tolerance = scaled > 0.5f ? scaled : 0.5f;
                const float rx = tolerance + 1.0f / 512, ry = tolerance * 0.125f + 1.0f / 2048;
                x0Low = 4096.0f - rx, x0High = 4096.0f + rx, qyLow = 1024.0f - ry, qyHigh = 1024.0f + ry;
                windowSmall = tolerance <= 3.5f;
                windowSize = g.size;
            }
            const float qy = g.y0 * 0.125f;
            const uint32_t ca = static_cast<uint32_t>(static_cast<int32_t>(g.x0 + x0Low));
            const uint32_t cb = static_cast<uint32_t>(static_cast<int32_t>(g.x0 + x0High));
            if (windowSmall && ((ca ^ cb) >> 6) == 0 && size_t(g.text) + g.textLength <= textSize) {
                const uint32_t ra = static_cast<uint32_t>(static_cast<int32_t>(qy + qyLow));
                const uint32_t rb = static_cast<uint32_t>(static_cast<int32_t>(qy + qyHigh));
                const uint64_t key = KeyOf(textData + g.text, g.textLength, g.dir);
                const uint64_t columns = ((uint64_t(2) << (cb - ca)) - 1) << (ca & 63);
                const uint64_t block = key + uint64_t(ca >> 6) * kColumnMix;
                const size_t wa = static_cast<size_t>((block + uint64_t(ra) * kRowMix) >> 32) & wordMask;
                const size_t wb = static_cast<size_t>((block + uint64_t(rb) * kRowMix) >> 32) & wordMask;
                if (((words[wa].bits | words[wb].bits) & columns) == 0) {
                    // Kept: its column and row lie in the window, so its word is wa or wb.
                    uint32_t column, row;
                    NearCells(g, column, row);
                    const size_t word = row == ra ? wa : wb;
                    FoldWord& w = words[word];
                    w.bits |= uint64_t(1) << (column & 63);
                    entries[entryCount] = FoldEntry{w.head, static_cast<uint32_t>(word), i};
                    w.head = base + entryCount++;
                    if (RAYOPDF_UNLIKELY(probing)) f.InsertKept(kept, g);
                    order[kept++] = i;
                    continue;
                }
            }
        }
        f.kept = kept;
        f.entryCount = entryCount;
        f.placed = placed;
        f.Place(i);
        kept = f.kept;
        entryCount = f.entryCount;
        probing = f.table != nullptr;
    }
    vis.resize(kept);
    fold.base = base + kept;
    if (scratch)
        for (uint32_t e = 0; e < entryCount; ++e) words[entries[e].word].bits = 0;
    return sum - sum == 0;
}

// An opening quote or bracket: ( [ “ ‘ „ «.
bool OpeningMark(const PageData& page, const Glyph& g) {
    if (g.textLength == 0) return false;
    const unsigned char c = static_cast<unsigned char>(page.text[g.text]);
    if (c == '(' || c == '[') return g.textLength == 1;
    if (c != 0xE2 && c != 0xC2) return false;
    const std::string_view t = page.GlyphText(g);
    return t == "\xE2\x80\x9C" || t == "\xE2\x80\x98" || t == "\xE2\x80\x9E" || t == "\xC2\xAB";
}

bool StartsWithCombiningMark(std::string_view text) {
    // U+0300-U+036F combining diacritical marks: C3 80..CD AF in UTF-8.
    if (text.size() < 2) return false;
    const unsigned char a = static_cast<unsigned char>(text[0]), b = static_cast<unsigned char>(text[1]);
    return (a == 0xCC && b >= 0x80) || (a == 0xCD && b <= 0xAF);
}

// Lead bytes that can start a character from U+0590 up that may be right-to-left: D6-DF
// (U+0590-U+07FF), E0 (U+0800-U+0FFF), EF (presentation forms) and F0 (supplementary).
bool RightToLeftLead(unsigned char b) { return (b >= 0xD6 && b <= 0xDF) || b == 0xE0 || b == 0xEF || b == 0xF0; }

// Whether the line holds a strong right-to-left character (bidi class R or AL), which only a
// RightToLeftLead byte can start.
bool HoldsRightToLeft(const PageData& page, const Line& line) {
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (g.textLength == 0) continue;
        if (!RightToLeftLead(static_cast<unsigned char>(page.text[g.text]))) continue;
        uint32_t cp = 0;
        size_t length = 0;
        if (!Utf8::Decode(page.GlyphText(g), 0, cp, length)) continue;
        const Text::Bidi::Class c = Text::Bidi::ClassOf(cp);
        if (c == Text::Bidi::Class::R || c == Text::Bidi::Class::AL) return true;
    }
    return false;
}

struct BidiScratch {
    std::vector<Text::Bidi::Class> classes;
    std::vector<uint32_t> codePoints;  // each visual item's first code point (paired brackets, N0)
    std::vector<uint32_t> visual;  // the line's glyphs in visual order
    std::vector<uint8_t> breaks;   // word break before visual item k
    std::vector<uint32_t> order;
    std::vector<uint8_t> levels;
    std::vector<uint32_t> runs;    // level run of visual item k
};

// Right-to-left text (algorithms §4.4, UAX #9): a PDF places the glyphs of a line in visual order,
// Markdown needs them in logical order. The line's glyphs are reordered, word breaks found
// between visual neighbours are carried over, and characters at odd levels are mirrored ("(" for
// ")"). Logical neighbours that were no visual neighbours meet at the boundary of two level runs:
// the break there is the one where the two runs touch on the line ("اليسار." in a left-to-right
// paragraph: the full stop is set right of the run's visual right end, its logical start). Only
// runs that do not touch make a break.
// `direction` -1 lets the characters decide the paragraph direction, 0 forces left-to-right.
// True when the line's paragraph direction is right-to-left.
bool ToLogicalOrder(PageData& page, const Line& line, BidiScratch& s, int direction) {
    if (line.dir != 0) return false;
    // Only lines with a character that starts with a RightToLeftLead byte can hold right-to-left
    // text.
    bool candidate = false;
    for (uint32_t k = 0; k < line.count && !candidate; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (g.textLength == 0) continue;
        candidate = RightToLeftLead(static_cast<unsigned char>(page.text[g.text]));
    }
    if (!candidate) return false;
    s.classes.resize(line.count);
    s.codePoints.resize(line.count);
    s.visual.assign(page.lineGlyphs.begin() + line.first, page.lineGlyphs.begin() + line.first + line.count);
    s.breaks.resize(line.count);
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[s.visual[k]];
        uint32_t cp = 0;
        size_t length = 0;
        if (!Utf8::Decode(page.GlyphText(g), 0, cp, length)) cp = ' ';
        s.classes[k] = Text::Bidi::ClassOf(cp);
        s.codePoints[k] = cp;
        s.breaks[k] = (g.flags & kGlyphSpaceBefore) != 0;
    }
    const bool rtl = Text::Bidi::VisualToLogical(s.classes.data(), s.codePoints.data(), line.count, s.order, &s.levels,
                                                 direction);
    if (s.order.size() != line.count || s.levels.size() != line.count) return false;
    s.runs.resize(line.count);
    for (uint32_t k = 0; k < line.count; ++k)
        s.runs[k] = k == 0 ? 0 : s.levels[k] == s.levels[k - 1] ? s.runs[k - 1] : s.runs[k - 1] + 1;
    for (uint32_t j = 0; j < line.count; ++j) {
        const uint32_t v = s.order[j];
        page.lineGlyphs[line.first + j] = s.visual[v];
        Glyph& g = page.glyphs[s.visual[v]];
        bool space = false;
        if (j > 0) {
            const uint32_t a = s.order[j - 1];
            if (v == a + 1) {
                space = s.breaks[v] != 0;
            } else if (a == v + 1) {
                space = s.breaks[a] != 0;
            } else if (v > 0 && s.runs[v - 1] == s.runs[a]) {
                space = s.breaks[v] != 0;  // a's run ends visually just left of v
            } else if (v + 1 < line.count && s.runs[v + 1] == s.runs[a]) {
                space = s.breaks[v + 1] != 0;
            } else if (a > 0 && s.runs[a - 1] == s.runs[v]) {
                space = s.breaks[a] != 0;
            } else if (a + 1 < line.count && s.runs[a + 1] == s.runs[v]) {
                space = s.breaks[a + 1] != 0;
            } else {
                space = true;
            }
        }
        g.flags = static_cast<uint16_t>(space ? (g.flags | kGlyphSpaceBefore) : (g.flags & ~kGlyphSpaceBefore));
        if (!(s.levels[v] & 1) || g.textLength == 0) continue;
        uint32_t cp = 0;
        size_t length = 0;
        const std::string_view text = page.GlyphText(g);
        if (!Utf8::Decode(text, 0, cp, length)) continue;
        const uint32_t mirror = Text::Bidi::Mirror(cp);
        if (mirror == cp || page.text.size() > (size_t(1) << 30)) continue;
        const std::string rest(text.substr(length));
        const uint32_t at = static_cast<uint32_t>(page.text.size());
        Utf8::Append(page.text, mirror);
        page.text += rest;
        g.text = at;
        g.textLength = static_cast<uint16_t>(page.text.size() - at);
    }
    return rtl;
}

// Whether at least 80 % of the glyphs [from, count) of `line` are monospace (and there are some).
bool MostlyMono(const PageData& page, const Line& line, uint32_t from) {
    if (from >= line.count) return false;
    uint32_t mono = 0;
    for (uint32_t k = from; k < line.count; ++k)
        if (page.glyphs[page.lineGlyphs[line.first + k]].flags & kGlyphMonospace) mono++;
    return mono * 5 >= (line.count - from) * 4;
}

// A listing's line number: a line that starts with a word of 1 to 5 ASCII digits.
struct NumberCandidate {
    uint32_t line = 0;
    uint32_t value = 0;
    uint32_t digits = 0;  // glyphs of the number
    float right = 0, baseline = 0, size = 0;
    bool code = false;  // monospace text follows it on its baseline
    uint32_t codeLine = UINT32_MAX;  // the line holding that text (its own line, or the one beside it)
};

constexpr uint32_t kMaxNumberCandidates = 4096;

// Line numbers of code listings (algorithms §6.4): a column of at least three consecutive integers,
// right-aligned at a steady pitch, mostly beside monospace text (or a listing of one or two lines
// numbered from 1, code on each). They are not part of the code: their glyphs are dropped, lines
// holding only a number go, and lines that start with one start after it, so that the listing
// keeps its indentation. A numbered table column (prose beside the numbers) or an ordered list
// ("1.") is no such column.
void DropLineNumbers(PageData& page) {
    std::vector<NumberCandidate> cands;
    for (uint32_t li = 0; li < page.lines.size() && cands.size() < kMaxNumberCandidates; ++li) {
        const Line& line = page.lines[li];
        if (line.dir != 0 || line.count == 0) continue;
        uint32_t k = 0, value = 0;
        for (; k < line.count && k < 6; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            const std::string_view t = page.GlyphText(g);
            if (t.size() != 1 || t[0] < '0' || t[0] > '9' || (k > 0 && (g.flags & kGlyphSpaceBefore))) break;
            value = value * 10 + static_cast<uint32_t>(t[0] - '0');
        }
        if (k == 0 || k > 5) continue;
        if (k < line.count && !(page.glyphs[page.lineGlyphs[line.first + k]].flags & kGlyphSpaceBefore)) continue;
        NumberCandidate c;
        c.line = li, c.value = value, c.digits = k;
        c.right = page.glyphs[page.lineGlyphs[line.first + k - 1]].u1;
        c.baseline = line.baseline, c.size = line.size;
        c.code = MostlyMono(page, line, k);
        if (c.code) c.codeLine = li;
        cands.push_back(c);
    }
    if (cands.empty()) return;

    // Number-only lines: the nearest line to the right on their baseline is monospace (deeply
    // indented code starts far from its number).
    std::vector<uint32_t> byBase;
    for (uint32_t li = 0; li < page.lines.size(); ++li)
        if (page.lines[li].dir == 0 && page.lines[li].count > 0) byBase.push_back(li);
    std::sort(byBase.begin(), byBase.end(),
              [&](uint32_t a, uint32_t b) { return page.lines[a].baseline < page.lines[b].baseline; });
    for (NumberCandidate& c : cands) {
        if (c.code || c.digits != page.lines[c.line].count) continue;
        auto it = std::lower_bound(byBase.begin(), byBase.end(), c.baseline - 0.3f * c.size,
                                   [&](uint32_t li, float v) { return page.lines[li].baseline < v; });
        const Line* nearest = nullptr;
        uint32_t nearestIndex = UINT32_MAX;
        for (int scanned = 0; it != byBase.end() && scanned < 64; ++it, ++scanned) {
            const Line& o = page.lines[*it];
            if (o.baseline > c.baseline + 0.3f * c.size) break;
            if (o.u0 > c.right && (!nearest || o.u0 < nearest->u0)) nearest = &o, nearestIndex = *it;
        }
        c.code = nearest && nearest->u0 - c.right <= 30 * c.size && MostlyMono(page, *nearest, 0);
        if (c.code) c.codeLine = nearestIndex;
    }

    // Runs, top to bottom.
    std::sort(cands.begin(), cands.end(), [](const NumberCandidate& a, const NumberCandidate& b) {
        return a.baseline != b.baseline ? a.baseline > b.baseline : a.right < b.right;
    });
    std::vector<uint8_t> drop(cands.size(), 0);
    size_t begin = 0;
    while (begin < cands.size()) {
        size_t end = begin + 1;
        float pitch = 0;
        // Steps are whole multiples of the line pitch: a wrapped source line has no number.
        while (end < cands.size()) {
            const NumberCandidate& a = cands[end - 1];
            const NumberCandidate& b = cands[end];
            const float step = a.baseline - b.baseline;
            if (b.value != a.value + 1 || std::fabs(b.right - a.right) > 0.3f * a.size || step <= 0.5f * a.size ||
                step > 5 * a.size)
                break;
            if (pitch > 0) {
                const float ratio = Fmax(step, pitch) / Fmin(step, pitch);
                const float whole = std::round(ratio);
                if (whole > 4 || std::fabs(ratio - whole) > 0.25f) break;
            }
            pitch = pitch > 0 ? Fmin(pitch, step) : step;
            if (pitch > 2.5f * a.size) break;
            ++end;
        }
        uint32_t code = 0;
        for (size_t k = begin; k < end; ++k) code += cands[k].code ? 1 : 0;
        // A short listing (one or two lines) counts from 1 and has code on every line.
        const size_t n = end - begin;
        if ((n >= 3 && code >= 2 && 2 * code >= n) || (n <= 2 && cands[begin].value == 1 && code == n))
            for (size_t k = begin; k < end; ++k) drop[k] = 1;
        begin = end;
    }

    bool any = false;
    for (size_t k = 0; k < cands.size(); ++k) {
        if (!drop[k]) continue;
        any = true;
        Line& line = page.lines[cands[k].line];
        for (uint32_t d = 0; d < cands[k].digits; ++d) page.glyphs[page.lineGlyphs[line.first + d]].flags |= kGlyphDropped;
        line.first += cands[k].digits;
        line.count -= cands[k].digits;
        line.chars = line.chars > cands[k].digits ? line.chars - cands[k].digits : 0;
        if (line.count == 0) continue;
        // The rest of the line: its extent and common style again.
        uint16_t all = kGlyphBold | kGlyphItalic | kGlyphMonospace;
        for (uint32_t i = 0; i < line.count; ++i) {
            Glyph& g = page.glyphs[page.lineGlyphs[line.first + i]];
            if (i == 0) {
                g.flags = static_cast<uint16_t>(g.flags & ~kGlyphSpaceBefore);
                line.u0 = g.u0, line.vb = g.vb, line.vt = g.vt;
            }
            line.u0 = Fmin(line.u0, g.u0);
            line.vb = Fmin(line.vb, g.vb), line.vt = Fmax(line.vt, g.vt);
            all &= g.flags;
        }
        line.flags = static_cast<uint16_t>(line.flags & ~(kLineAllBold | kLineAllItalic | kLineAllMono));
        if (all & kGlyphBold) line.flags |= kLineAllBold;
        if (all & kGlyphItalic) line.flags |= kLineAllItalic;
        if (all & kGlyphMonospace) line.flags |= kLineAllMono;
    }
    if (!any) return;

    // Wrapped source lines: a listing breaks a line too long for its frame and sets the rest on
    // the next line, indented and without a number. Such a line (monospace, just below a numbered
    // line that runs to the text's right edge, indented past its start, itself neither numbered
    // nor beside a number) joins the line it continues, as one source line again.
    std::vector<uint8_t> numbered(page.lines.size(), 0);
    float textRight = -INFINITY;
    for (const Line& l : page.lines)
        if (l.dir == 0 && l.count > 0) textRight = Fmax(textRight, l.u1);
    for (const NumberCandidate& c : cands) {
        numbered[c.line] = 1;
        if (c.codeLine != UINT32_MAX) numbered[c.codeLine] = 1;
    }
    for (size_t k = 0; k < cands.size(); ++k) {
        if (!drop[k] || cands[k].codeLine == UINT32_MAX) continue;
        Line& code = page.lines[cands[k].codeLine];
        const float size = cands[k].size;
        if (code.count == 0 || code.u1 < textRight - 2 * size) continue;
        auto it = std::lower_bound(byBase.begin(), byBase.end(), cands[k].baseline - 1.6f * size,
                                   [&](uint32_t li, float v) { return page.lines[li].baseline < v; });
        for (int scanned = 0; it != byBase.end() && scanned < 64; ++it, ++scanned) {
            Line& o = page.lines[*it];
            if (o.baseline > cands[k].baseline - 0.5f * size) break;
            if (numbered[*it] || o.count == 0 || o.u0 <= code.u0 + 0.5f * size || o.u0 >= code.u1 ||
                !MostlyMono(page, o, 0))
                continue;
            const uint32_t first = static_cast<uint32_t>(page.lineGlyphs.size());
            for (uint32_t i = 0; i < code.count; ++i) page.lineGlyphs.push_back(page.lineGlyphs[code.first + i]);
            for (uint32_t i = 0; i < o.count; ++i) page.lineGlyphs.push_back(page.lineGlyphs[o.first + i]);
            code.first = first;
            code.count += o.count;
            code.chars += o.chars;
            if (!(o.flags & kLineAllMono)) code.flags = static_cast<uint16_t>(code.flags & ~kLineAllMono);
            o.count = 0;
            numbered[*it] = 1;
            break;
        }
    }
    page.lines.erase(std::remove_if(page.lines.begin(), page.lines.end(), [](const Line& l) { return l.count == 0; }),
                     page.lines.end());
}

constexpr size_t kMaxStrikeRules = 1024;
constexpr uint64_t kStrikeChecks = uint64_t(1) << 22;  // rule and glyph comparisons per page

// Strike-through (algorithms §6.6): a thin horizontal rule across the middle of glyphs on the
// line's baseline, between a sixth and half an em above it, no longer than the line by more than
// half an em on either side. An underline sits below the baseline, a fraction bar between a
// numerator and a denominator off the baseline, and a table rule runs on past the text it
// crosses. Fails closed (fewer marks) once the comparison budget runs out.
void MarkStruck(PageData& page) {
    std::vector<const Shape*> rules;
    for (const Shape& s : page.shapes) {
        const float w = s.x1 - s.x0, h = s.y1 - s.y0;
        if (s.kind == ShapeKind::Other || !(h >= 0 && h <= 2) || !(w >= 2 && w >= 4 * h)) continue;
        if (rules.size() >= kMaxStrikeRules) break;
        rules.push_back(&s);
    }
    if (rules.empty()) return;
    uint64_t budget = kStrikeChecks;
    for (const Line& line : page.lines) {
        if (line.dir != 0 || line.count == 0) continue;
        if (budget < rules.size()) return;
        budget -= rules.size();
        const float em = line.size;
        for (const Shape* s : rules) {
            const float y = (s->y0 + s->y1) / 2;
            if (y <= line.vb || y >= line.vt || s->y1 - s->y0 > 0.15f * em) continue;
            if (s->x0 < line.u0 - 0.5f * em || s->x1 > line.u1 + 0.5f * em || s->x1 <= line.u0 || s->x0 >= line.u1)
                continue;
            if (budget < line.count) return;
            budget -= line.count;
            for (uint32_t k = 0; k < line.count; ++k) {
                Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
                const float cx = (g.u0 + g.u1) / 2, lift = y - g.v;
                if (cx < s->x0 || cx > s->x1 || std::fabs(g.v - line.baseline) > 0.1f * em) continue;
                if (lift >= 0.16f * g.size && lift <= 0.5f * g.size) g.flags |= kGlyphStrike;
            }
        }
    }
}

} // namespace

Marker ParseMarker(std::string_view t, uint32_t& value) {
    value = 0;
    if (t.empty() || t.size() > 8) return Marker::None;
    static constexpr std::string_view kBullets[] = {
        "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA", "\xE2\x96\xAB", "\xE2\x97\x8F", "\xE2\x97\x8B",
        "\xE2\x80\xA3", "\xE2\x81\x83", "\xE2\x80\x93", "-",            "*",            "\xC2\xB7",
        "\xE2\x96\xA0", "\xE2\x96\xA1", "\xE2\x96\xBA", "\xE2\x96\xB6", "\xE2\x9E\xA2", "\xE2\x9E\xA4",
        "\xE2\x9D\x96", "\xE2\x99\xA6", "\xE2\x97\x86", "\xE2\x88\x99", "\xE2\x97\xBE", "\xEF\x82\xB7",
        "\xEF\x82\xA7", "\xEF\x83\x98", "\xEF\x81\xB6", "\xEF\x82\xA8", "\xE2\x80\x94", "\xE2\x9C\x93",
        "\xE2\x9C\x94", "\xE2\x88\x97",  // ✔ ∗ (LaTeX's third itemize level)
    };
    // Bullets and task boxes are at most three bytes and start with one of these.
    const unsigned char lead = static_cast<unsigned char>(t[0]);
    if (t.size() <= 3 && (lead == '-' || lead == '*' || lead == 0xC2 || lead == 0xE2 || lead == 0xEF)) {
        for (const std::string_view b : kBullets)
            if (t == b) return Marker::Bullet;
        if (t == "\xE2\x98\x90") return Marker::TaskOpen;                            // ☐
        if (t == "\xE2\x98\x91" || t == "\xE2\x98\x92" || t == "\xE2\x8A\xA0") return Marker::TaskDone;  // ☑ ☒ ⊠
    }
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

namespace {

// Fmin and Fmax (common/minmax.h) without their NaN tests, for pages whose placed glyphs all
// have a finite frame box, baseline and size: of finite values both keep the smaller (larger)
// one, and the later of two equal ones, so the lines are the same as with the tests.
struct FiniteMinMax {
    static float Min(float a, float b) { return a < b ? a : b; }
    static float Max(float a, float b) { return a > b ? a : b; }
};
struct CheckedMinMax {
    static float Min(float a, float b) { return Fmin(a, b); }
    static float Max(float a, float b) { return Fmax(a, b); }
};

// Line groups: runs of `members` (fragment indices) in u order.
struct Group {
    uint32_t first = 0, count = 0;
    float u0 = 0, u1 = 0, vb = 0, vt = 0, baseline = 0, size = 0;
    uint8_t dir = 0;
    bool merged = false;   // absorbed into another group
    bool scripts = false;  // received a superscript or subscript fragment: sort glyphs by position
};

// Proto lines: glyphs in order with their word breaks set, before gutter splitting.
struct Proto {
    uint32_t first = 0, count = 0;  // in `proto`
    uint32_t segFirst = 0, segCount = 0;
    float baseline = 0, size = 0, u0 = 0, u1 = 0;
    // As its line when it does not split (PlaceGlyphs with finite values).
    float vb = 0, vt = 0, lineSize = 0;
    uint16_t allFlags = 0;
    uint8_t dir = 0;
    bool split = false;  // a gutter splits it
};

struct Segment {
    float u0, u1;
    uint32_t start;  // position in `proto` of its first glyph
};

// BuildLines' working vectors, kept by each thread between pages so that a page grows none of
// them anew; released after a page of more than kKeptGlyphs glyphs.
constexpr size_t kKeptGlyphs = size_t(1) << 18;
struct PlaceScratch {
    std::vector<uint32_t> vis, fo, members, cluster, extraOf, lineVis, byBase;
    std::vector<Fragment> frags;
    std::vector<Group> groups;
    std::vector<Proto> protos;
    std::vector<Segment> segments;
    std::vector<uint8_t> splitBefore;
};

// A line's sizes: buckets of sizes within 0.25 of the first in each, the first kLineSizes kept.
struct SizeCount {
    float size;
    uint32_t count;
};
constexpr uint32_t kLineSizes = 32, kProtoSizes = 16;

// The glyph flags a line has when all its glyphs have them.
constexpr uint16_t kLineFlagGlyphs = kGlyphBold | kGlyphItalic | kGlyphMonospace | kGlyphArtifact | kGlyphInvisible | kGlyphOcr;

// The size of the bucket with the most glyphs, of equal ones the larger size.
float MostSize(const SizeCount* sizes, uint32_t count) {
    SizeCount best{0, 0};
    for (uint32_t s = 0; s < count; ++s)
        if (sizes[s].count > best.count || (sizes[s].count == best.count && sizes[s].size > best.size)) best = sizes[s];
    return best.size;
}

// Sets a measured line's size, baseline (that of its first glyph of that size, else the proto's)
// and flags from its glyphs' common ones, and appends it.
void FinishLine(PageData& page, Line& line, float size, float baseline, uint16_t allFlags) {
    line.size = size;
    line.baseline = baseline;
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (std::fabs(g.size - size) < 0.25f) {
            line.baseline = g.v;
            break;
        }
    }
    if (allFlags & kGlyphBold) line.flags |= kLineAllBold;
    if (allFlags & kGlyphItalic) line.flags |= kLineAllItalic;
    if (allFlags & kGlyphMonospace) line.flags |= kLineAllMono;
    if (allFlags & kGlyphArtifact) line.flags |= kLineArtifact;
    if ((allFlags & (kGlyphInvisible | kGlyphOcr)) == kGlyphInvisible) line.flags |= kLineInvisible;
    page.lines.push_back(line);
}

// Lines from the placed glyphs `vis` (content order), M the page's min and max.
template <typename M>
void PlaceGlyphs(PageData& page, std::vector<uint32_t>& vis) {
    std::vector<Glyph>& glyphs = page.glyphs;
    PlaceScratch& place = PerThread<PlaceScratch>();

    // Fragments in content order (algorithms §4.4 recommendation: one pass merging into the
    // current line while direction and baseline match).
    std::vector<Fragment>& frags = place.frags;
    frags.clear();
    // The fragment being extended stays in `f` until the next one starts, out of memory that the
    // glyphs' fields might alias.
    Fragment f;
    bool open = false;
    for (uint32_t k = 0; k < vis.size(); ++k) {
        const Glyph& g = glyphs[vis[k]];
        const bool space = (g.flags & kGlyphSpace) != 0;
        if (open) {
            const float em = M::Max(f.size, g.size);
            const float gap = g.u0 - f.lastU1;
            const float dv = g.v - f.baseline;
            const bool sameBase = std::fabs(dv) <= 0.25f * em;
            const bool script = !sameBase && std::fabs(dv) <= 0.6f * em &&
                                (g.size < 0.87f * f.size || f.lastSize < 0.87f * g.size);
            const float maxGap = (f.marker ? kMarkerGap : kFragmentGap) * em;
            if (g.dir == f.dir && (sameBase || script || space) && gap >= -0.6f * em && gap <= maxGap &&
                std::fabs(dv) <= 0.6f * em) {
                f.end = k + 1;
                f.lastU1 = M::Max(f.lastU1, g.u1);
                if (!space) {
                    f.lastSize = g.size;
                    if (!f.any) {
                        f.u0 = g.u0, f.u1 = g.u1, f.vb = g.vb, f.vt = g.vt;
                        f.baseline = g.v, f.size = g.size;
                        f.any = true;
                    } else {
                        f.u0 = M::Min(f.u0, g.u0), f.u1 = M::Max(f.u1, g.u1);
                        f.vb = M::Min(f.vb, g.vb), f.vt = M::Max(f.vt, g.vt);
                        // A fragment that began with a superscript takes the baseline of the
                        // full-size text that follows.
                        if (g.size > f.size * 1.15f) f.baseline = g.v, f.size = g.size;
                    }
                    f.marker = false;
                } else {
                    f.spaces = true;
                }
                continue;
            }
            frags.push_back(f);
        }
        f = Fragment();
        f.begin = k;
        f.end = k + 1;
        f.dir = g.dir;
        f.u0 = g.u0, f.u1 = g.u1, f.vb = g.vb, f.vt = g.vt;
        f.baseline = g.v;
        f.size = g.size;
        f.lastU1 = g.u1;
        f.lastSize = g.size;
        f.any = !space;
        f.spaces = space;
        if (f.any) {
            uint32_t value;
            f.marker = ParseMarker(page.GlyphText(g), value) != Marker::None;
        }
        open = true;
    }
    if (open) frags.push_back(f);
    // Leading space-only fragments carry nothing.
    // Sort into baseline clusters per direction, then by position along the baseline.
    std::vector<uint32_t>& fo = place.fo;
    fo.clear();
    for (uint32_t i = 0; i < frags.size(); ++i)
        if (frags[i].any) fo.push_back(i);
    std::sort(fo.begin(), fo.end(), [&](uint32_t a, uint32_t b) {
        const Fragment& x = frags[a];
        const Fragment& y = frags[b];
        if (x.dir != y.dir) return x.dir < y.dir;
        if (x.baseline != y.baseline) return x.baseline > y.baseline;
        return x.u0 < y.u0;
    });

    std::vector<uint32_t>& members = place.members;
    members.clear();
    std::vector<Group>& groups = place.groups;
    groups.clear();
    std::vector<uint32_t>& cluster = place.cluster;
    for (size_t at = 0; at < fo.size();) {
        const Fragment& head = frags[fo[at]];
        size_t end = at + 1;
        while (end < fo.size()) {
            const Fragment& f = frags[fo[end]];
            if (f.dir != head.dir || head.baseline - f.baseline > 0.25f * M::Max(head.size, f.size)) break;
            ++end;
        }
        cluster.assign(fo.begin() + static_cast<std::ptrdiff_t>(at), fo.begin() + static_cast<std::ptrdiff_t>(end));
        std::sort(cluster.begin(), cluster.end(), [&](uint32_t a, uint32_t b) { return frags[a].u0 < frags[b].u0; });
        Group g;
        for (size_t c = 0; c < cluster.size(); ++c) {
            const Fragment& f = frags[cluster[c]];
            if (g.count > 0) {
                const float em = M::Max(g.size, f.size);
                const bool lastMarker = frags[members.back()].marker && g.count == 1;
                const float maxGap = (lastMarker ? kMarkerGap : kJoinGap) * em;
                if (f.u0 - g.u1 <= maxGap) {
                    members.push_back(cluster[c]);
                    g.count++;
                    g.u0 = M::Min(g.u0, f.u0), g.u1 = M::Max(g.u1, f.u1);
                    g.vb = M::Min(g.vb, f.vb), g.vt = M::Max(g.vt, f.vt);
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
    std::vector<uint32_t>& extraOf = place.extraOf;
    extraOf.assign(groups.size(), kEmpty);
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
        line.u0 = M::Min(line.u0, small.u0), line.u1 = M::Max(line.u1, small.u1);
        if (extraOf[best] == kEmpty) {
            extraOf[best] = static_cast<uint32_t>(extra.size());
            extra.emplace_back();
        }
        extra[extraOf[best]].push_back(members[small.first]);
    }

    // Proto lines in `proto`, which becomes the page's lineGlyphs: the lines split from them below
    // take their glyphs in that order. Glyphs and segments are written into storage sized for all
    // placed glyphs: a glyph enters one proto at most and starts one segment at most.
    //
    // With finite values (FiniteMinMax) the pass also measures each proto as the lines below
    // measure a line, for the protos that do not split: Min and Max then give the same values
    // whether or not the first glyph is applied twice, and both size rules find a glyph's bucket
    // by the same first match (|a - b| >= 0.25 is !(|a - b| < 0.25) without NaN).
    constexpr bool kMeasured = std::is_same<M, FiniteMinMax>::value;
    std::vector<uint32_t>& proto = page.lineGlyphs;
    proto.resize(vis.size());
    uint32_t* const protoAt = proto.data();
    uint32_t protoCount = 0;
    std::vector<Proto>& protos = place.protos;
    protos.clear();
    std::vector<Segment>& segmentVector = place.segments;
    if (segmentVector.size() < vis.size()) segmentVector.resize(vis.size());
    Segment* const segments = segmentVector.data();
    uint32_t segmentCount = 0;
    std::vector<uint32_t>& lineVis = place.lineVis;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& grp = groups[gi];
        if (grp.merged) continue;
        // The group's glyphs: a lone fragment's are a run of `vis` already. Explicit spaces decide
        // the word breaks of lines that have them (algorithms §4.1).
        const uint32_t *lineBegin, *lineEnd;
        bool spaces = false;
        if (grp.count == 1 && extraOf[gi] == kEmpty) {
            const Fragment& f = frags[members[grp.first]];
            lineBegin = vis.data() + f.begin, lineEnd = vis.data() + f.end;
            spaces = f.spaces;
        } else {
            lineVis.clear();
            for (uint32_t m = 0; m < grp.count; ++m) {
                const Fragment& f = frags[members[grp.first + m]];
                lineVis.insert(lineVis.end(), vis.data() + f.begin, vis.data() + f.end);
                spaces = spaces || f.spaces;
            }
            if (extraOf[gi] != kEmpty) {
                for (uint32_t fi : extra[extraOf[gi]]) {
                    const Fragment& f = frags[fi];
                    lineVis.insert(lineVis.end(), vis.data() + f.begin, vis.data() + f.end);
                    spaces = spaces || f.spaces;
                }
            }
            if (grp.scripts) {
                std::stable_sort(lineVis.begin(), lineVis.end(), [&](uint32_t a, uint32_t b) {
                    return glyphs[a].u0 + glyphs[a].u1 < glyphs[b].u0 + glyphs[b].u1;
                });
            }
            lineBegin = lineVis.data(), lineEnd = lineVis.data() + lineVis.size();
        }
        const float wordGap = spaces ? kWordGapWithSpaces : kWordGap;
        Proto p;
        p.first = protoCount;
        p.segFirst = segmentCount;
        p.dir = grp.dir;
        p.baseline = grp.baseline;
        p.size = grp.size;
        bool pendingSpace = false;
        float right = 0;
        float previousSize = 0;
        bool first = true;
        Segment seg{0, 0, 0};  // the ink segment being extended, appended when the next starts
        // A glyph set tight after the one before it (no space, no wide gap) may be an opening
        // quote or bracket hanging into a column gutter (LaTeX's protrusion): the gap is then also
        // measured past it, from the ink before it to the glyph after it.
        bool prevTight = false;   // the previous glyph was set tight
        float beforePrevious = 0; // the ink's right edge before the previous glyph
        // Sizes (kMeasured): the proto's is the size of most of its glyphs (not a drop cap's:
        // gutter windows and gaps scale with it), the first of the first 16 sizes to lead; the
        // line's is MostSize of the first 32.
        SizeCount sizes[kLineSizes];
        uint32_t sizeCount = 0, sizeBest = 0;
        float pu0 = 0, pu1 = 0, vb = 0, vt = 0;
        uint16_t allFlags = kLineFlagGlyphs;
        for (const uint32_t* at = lineBegin; at != lineEnd; ++at) {
            const uint32_t i = *at;
            Glyph& g = glyphs[i];
            if (g.flags & kGlyphSpace) {
                if (!first) pendingSpace = true;
                continue;
            }
            g.flags = static_cast<uint16_t>(g.flags & ~kGlyphSpaceBefore);
            if (!first) {
                const float em = M::Max(M::Min(previousSize, g.size), 0.5f * M::Max(previousSize, g.size));
                const float gap = g.u0 - right;
                // A combining mark takes two bytes at least.
                const bool combining = g.textLength >= 2 && StartsWithCombiningMark(page.GlyphText(g));
                if (!combining && (pendingSpace || gap > wordGap * em)) g.flags |= kGlyphSpaceBefore;
                // Ink segments: runs of glyphs without a wide gap, for gutter detection. The gap is
                // measured in the size of the glyphs beside it: a drop cap must not widen the
                // threshold for the whole line.
                if (gap > kGutterCandidate * em) {
                    segments[segmentCount++] = seg;
                    seg = Segment{g.u0, g.u1, protoCount};
                    prevTight = false;
                } else if (prevTight && g.u0 - beforePrevious > kGutterCandidate * em &&
                           OpeningMark(page, glyphs[protoAt[protoCount - 1]])) {
                    // The previous glyph, a mark set tight after the text before it, hangs into
                    // the gap: the gap's middle is found from the text after it.
                    seg.u1 = beforePrevious;
                    segments[segmentCount++] = seg;
                    seg = Segment{g.u0, g.u1, protoCount - 1};
                    prevTight = false;
                } else {
                    seg.u1 = M::Max(seg.u1, g.u1);
                    prevTight = !pendingSpace;
                }
                beforePrevious = right;
                right = M::Max(right, g.u1);
                pu0 = M::Min(pu0, g.u0), pu1 = M::Max(pu1, g.u1);
                if (kMeasured) vb = M::Min(vb, g.vb), vt = M::Max(vt, g.vt);
            } else {
                seg = Segment{g.u0, g.u1, protoCount};
                right = g.u1;
                pu0 = g.u0, pu1 = g.u1;
                vb = g.vb, vt = g.vt;
            }
            if (kMeasured) {
                allFlags &= g.flags;
                uint32_t d = 0;
                while (d < sizeCount && !(std::fabs(sizes[d].size - g.size) < 0.25f)) ++d;
                if (d < kLineSizes) {
                    if (d == sizeCount) sizes[sizeCount++] = SizeCount{g.size, 0};
                    if (++sizes[d].count > sizes[sizeBest].count && d < kProtoSizes) sizeBest = d;
                }
            }
            pendingSpace = false;
            first = false;
            previousSize = g.size;
            protoAt[protoCount++] = i;
        }
        if (!first) segments[segmentCount++] = seg;
        p.u0 = pu0, p.u1 = pu1;
        p.count = protoCount - p.first;
        p.segCount = segmentCount - p.segFirst;
        if (kMeasured) {
            if (sizeCount > 0) p.size = sizes[sizeBest].size;
            p.vb = vb, p.vt = vt;
            p.allFlags = allFlags;
            p.lineSize = MostSize(sizes, sizeCount);
        } else {
            float sizes16[kProtoSizes];
            uint32_t counts[kProtoSizes] = {}, distinct = 0, best = 0;
            for (uint32_t k = p.first; k < p.first + p.count; ++k) {
                const float s = glyphs[protoAt[k]].size;
                uint32_t d = 0;
                while (d < distinct && std::fabs(sizes16[d] - s) >= 0.25f) ++d;
                if (d == distinct) {
                    if (distinct == kProtoSizes) continue;
                    sizes16[distinct++] = s;
                }
                if (++counts[d] > counts[best]) best = d;
            }
            if (distinct > 0) p.size = sizes16[best];
        }
        if (p.count > 0) protos.push_back(p);
    }
    proto.resize(protoCount);

    // Gutters (Breuel's whitespace columns, algorithms §5.1): a wide gap in a line is a column or
    // cell boundary when the lines around it have no ink at that place either, and text bounds it
    // on both sides (a line with text left and right of it, or lines left of it and lines right of
    // it); a wide space of justified text has ink above and below it, or only short lines on one
    // side. The line splits there.
    std::vector<uint8_t>& splitBefore = place.splitBefore;
    splitBefore.assign(proto.size(), 0);
    uint32_t splits = 0;
    {
        std::vector<uint32_t>& byBase = place.byBase;
        byBase.resize(protos.size());
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
                bool leftSide = false, rightSide = false;  // some neighbour has text only left / right of it
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
                        else {
                            oneSide++;
                            leftSide = leftSide || left;
                            rightSide = rightSide || right;
                        }
                    }
                }
                const bool bounded = clear >= 1 || (leftSide && rightSide);
                const bool gutter = bounded && (clear + oneSide >= 2) && blocked * 8 <= clear + oneSide;
                if (gutter) splitBefore[start] = 1, splits++, protos[byBase[r]].split = true;
            }
        }
    }

    // Lines: metrics of each piece; a piece's glyphs stay where they are in `proto`.
    page.lines.reserve(protos.size() + splits);
    for (const Proto& p : protos) {
        if (kMeasured && !p.split) {
            // The whole proto, measured above (its first glyph has no word break before it).
            Line line;
            line.first = p.first;
            line.dir = p.dir;
            line.u0 = p.u0, line.u1 = p.u1, line.vb = p.vb, line.vt = p.vt;
            line.chars = p.count;
            line.count = p.count;
            FinishLine(page, line, p.lineSize, p.baseline, p.allFlags);
            continue;
        }
        uint32_t at = p.first;
        const uint32_t end = p.first + p.count;
        while (at < end) {
            uint32_t stop = at + 1;
            while (stop < end && !splitBefore[stop]) ++stop;
            Line line;
            line.first = at;
            line.dir = p.dir;
            uint16_t allFlags = kLineFlagGlyphs;
            SizeCount sizes[kLineSizes];
            uint32_t sizeCount = 0;
            for (uint32_t k = at; k < stop; ++k) {
                Glyph& g = glyphs[proto[k]];
                if (k == at) {
                    g.flags = static_cast<uint16_t>(g.flags & ~kGlyphSpaceBefore);
                    line.u0 = g.u0, line.u1 = g.u1, line.vb = g.vb, line.vt = g.vt;
                }
                line.u0 = M::Min(line.u0, g.u0), line.u1 = M::Max(line.u1, g.u1);
                line.vb = M::Min(line.vb, g.vb), line.vt = M::Max(line.vt, g.vt);
                allFlags &= g.flags;
                line.chars++;
                bool counted = false;
                for (uint32_t s = 0; s < sizeCount; ++s)
                    if (std::fabs(sizes[s].size - g.size) < 0.25f) {
                        sizes[s].count++;
                        counted = true;
                        break;
                    }
                if (!counted && sizeCount < kLineSizes) sizes[sizeCount++] = SizeCount{g.size, 1};
            }
            line.count = stop - at;
            FinishLine(page, line, MostSize(sizes, sizeCount), p.baseline, allFlags);
            at = stop;
        }
    }
}

} // namespace

void BuildLines(PageData& page) {
    page.lines.clear();
    page.lineGlyphs.clear();
    std::vector<Glyph>& glyphs = page.glyphs;

    // Hidden text (algorithms §4.3): invisible, clipped or tiny glyphs leave (CollectAndFold),
    // except that invisible text is the page's text when nothing visible is (an OCR layer). The
    // add-on's OCR layer (kGlyphOcr) stays whatever else the page has.
    bool visibleText = false, invisibleText = false;
    for (const Glyph& g : glyphs) {
        if (g.flags & (kGlyphDropped | kGlyphSpace | kGlyphOcr)) continue;
        if (g.flags & kGlyphClipped || g.size < 1.0f) continue;
        if (!(g.flags & kGlyphInvisible)) {
            visibleText = true;
            break;
        }
        invisibleText = true;
    }
    const bool keepInvisible = !visibleText && invisibleText;
    PlaceScratch& place = PerThread<PlaceScratch>();
    std::vector<uint32_t>& vis = place.vis;
    if (CollectAndFold(page, vis, keepInvisible))
        PlaceGlyphs<FiniteMinMax>(page, vis);
    else
        PlaceGlyphs<CheckedMinMax>(page, vis);
    if (glyphs.size() > kKeptGlyphs) place = PlaceScratch();

    DropLineNumbers(page);
    MarkStruck(page);

    // Right-to-left text, once the page's text extent is known: a short line flush with the left
    // edge belongs to a left-to-right paragraph (an RTL sentence in an LTR document keeps its
    // final punctuation on the right), whatever most of its characters are. When the page sets
    // right-to-left text that way only (short lines flush left, none flush right), its full lines
    // with right-to-left text are left-to-right paragraphs too: they wrap into those short lines.
    // A page whose text has none of the lead bytes such a character starts with has no line to
    // reorder.
    if (!std::any_of(page.text.begin(), page.text.end(), [](char c) { return RightToLeftLead(static_cast<unsigned char>(c)); }))
        return;
    BidiScratch bidi;
    float left = INFINITY, right = -INFINITY;
    for (const Line& line : page.lines)
        if (line.dir == 0) left = Fmin(left, line.u0), right = Fmax(right, line.u1);
    uint32_t flushLeft = 0, flushRight = 0;
    for (const Line& line : page.lines) {
        const float em = Fmax(line.size, 1.0f);
        const bool atLeft = line.u0 - left <= em, atRight = right - line.u1 <= em;
        if (atLeft == atRight || line.dir != 0 || !HoldsRightToLeft(page, line)) continue;
        if (atLeft && right - line.u1 >= 3 * em) flushLeft++;
        if (atRight && line.u0 - left >= 3 * em) flushRight++;
    }
    const bool leftAligned = flushLeft > 0 && flushRight == 0;
    for (Line& line : page.lines) {
        const float em = Fmax(line.size, 1.0f);
        const bool atLeft = line.u0 - left <= em;
        const int direction = atLeft && (right - line.u1 >= 3 * em || leftAligned) ? 0 : -1;
        if (ToLogicalOrder(page, line, bidi, direction)) line.flags |= kLineRtl;
    }
}

namespace {

// Extent, character count and common style of `line` from its glyphs.
void Measure(const PageData& page, Line& line) {
    uint16_t all = kGlyphBold | kGlyphItalic | kGlyphMonospace | kGlyphArtifact | kGlyphInvisible | kGlyphOcr;
    line.chars = line.count;
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (k == 0) line.u0 = g.u0, line.u1 = g.u1, line.vb = g.vb, line.vt = g.vt;
        line.u0 = Fmin(line.u0, g.u0), line.u1 = Fmax(line.u1, g.u1);
        line.vb = Fmin(line.vb, g.vb), line.vt = Fmax(line.vt, g.vt);
        all &= g.flags;
    }
    line.flags = static_cast<uint16_t>(line.flags & ~(kLineAllBold | kLineAllItalic | kLineAllMono | kLineArtifact |
                                                     kLineInvisible));
    if (all & kGlyphBold) line.flags |= kLineAllBold;
    if (all & kGlyphItalic) line.flags |= kLineAllItalic;
    if (all & kGlyphMonospace) line.flags |= kLineAllMono;
    if (all & kGlyphArtifact) line.flags |= kLineArtifact;
    if ((all & (kGlyphInvisible | kGlyphOcr)) == kGlyphInvisible) line.flags |= kLineInvisible;
}

} // namespace

void SplitRunInHeadings(PageData& page) {
    const size_t ordered = page.order.size();
    for (size_t at = 0; at < ordered && page.order.size() < (size_t(1) << 24); ++at) {
        // Indices into page.order shift as rests are inserted: `at` walks the original entries.
        const uint32_t li = page.order[at + (page.order.size() - ordered)];
        Line& line = page.lines[li];
        if (line.dir != 0 || line.count < 3 || (line.flags & (kLineAnchor | kLineTableCell | kLineTable))) continue;
        uint32_t k = 0, letters = 0;
        while (k < line.count && k <= 80) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            if (!(g.flags & kGlyphBold)) break;
            const std::string_view t = page.GlyphText(g);
            if (!t.empty() && ((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z') ||
                               static_cast<unsigned char>(t[0]) >= 0xC0))
                letters++;
            ++k;
        }
        if (k < 2 || k >= line.count || k > 80 || letters < 2) continue;
        const Glyph& last = page.glyphs[page.lineGlyphs[line.first + k - 1]];
        const Glyph& next = page.glyphs[page.lineGlyphs[line.first + k]];
        if (next.u0 - last.u1 < 0.6f * line.size) continue;
        const std::string_view lastText = page.GlyphText(last);
        // Ending in punctuation it leads into its sentence; ending in a number it labels a caption
        // ("Table 4", "Fig. 2").
        if (lastText.empty() || std::strchr(".:;,!?0123456789", lastText.back()) != nullptr) continue;
        // The body text after it is set in another face (not one bold cell beside another).
        if (page.glyphs[page.lineGlyphs[line.first + line.count - 1]].flags & kGlyphBold) continue;
        Line rest = line;
        rest.first = line.first + k;
        rest.count = line.count - k;
        line.count = k;
        page.glyphs[page.lineGlyphs[rest.first]].flags &= static_cast<uint16_t>(~kGlyphSpaceBefore);
        Measure(page, line);
        Measure(page, rest);
        const size_t position = at + (page.order.size() - ordered) + 1;
        page.order.insert(page.order.begin() + static_cast<std::ptrdiff_t>(position),
                          static_cast<uint32_t>(page.lines.size()));
        page.lines.push_back(rest);
    }
}

} // namespace RayoPdf::Layout
