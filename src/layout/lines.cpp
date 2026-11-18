// SPDX-License-Identifier: Apache-2.0
#include "layout/lines.h"

#include "common/compiler.h"
#include "common/minmax.h"
#include "common/per_thread.h"
#include "common/utf8.h"
#include "layout/math.h"
#include "text/bidi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
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

// Storage from calloc: the kernel maps a fresh allocation's pages zeroed on first touch, so a page
// faults in only what it writes (vector's value-initialization writes every element).
struct FreeDeleter {
    void operator()(void* p) const { std::free(p); }
};
template <typename T>
using Buffer = std::unique_ptr<T[], FreeDeleter>;
template <typename T>
Buffer<T> Zeroed(size_t count) {
    T* p = static_cast<T*>(std::calloc(count > 0 ? count : 1, sizeof(T)));
    if (!p) std::abort();
    return Buffer<T>(p);
}

// CollectAndFold's working storage, kept by each thread between pages of up to kScratchSlots probe
// slots, so a page fills none of it whole: a filter word empties by clearing the words its page's
// entries set; chain heads and the probe's table hold base + an index, and values below `base` are
// empty, so moving the base past a page's indices at its end empties both.
constexpr size_t kScratchSlots = size_t(1) << 16;
struct FoldEntry {
    uint32_t next;   // base + the entry chained here before, or below `base`
    uint32_t word;   // its filter word
    uint32_t glyph;  // the kept glyph
};
struct FoldScratch {
    Buffer<uint64_t> bits;   // per filter word, a bit per column of a kept glyph; 0 outside a call
    Buffer<uint32_t> heads;  // per filter word, base + its last entry; below `base` when none
    size_t words = 0;
    Buffer<FoldEntry> entries;
    size_t entryCount = 0;
    Buffer<uint32_t> table;  // the probe's table; below `base` outside a call
    size_t tableSize = 0;
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

// The lower-left corner of a glyph's box on the upright page, by which the fold places it.
struct Corner {
    float x, y;
};

RAYOPDF_HOT_INLINE Corner CornerOf(const Glyph& g) {
    if (RAYOPDF_LIKELY(g.dir == 0)) return Corner{g.u0, g.vb};
    const UprightBox box = GlyphPageBox(g);
    return Corner{box.x0, box.y0};
}

// |x| < kFloatReach and |y| < kFloatReach (false for NaN), both at once on their bits: a float's
// magnitude bits order as its magnitude, NaN's above infinity's, and 2048 is 0x45000000; adding
// 0x80000000 - 0x45000000 to each 31-bit magnitude sets its bit 31 exactly when it is 2048 or more,
// without a carry into the other half.
static_assert(kFloatReach == 2048.0f, "Near's constant is 2048's bits");
static_assert(sizeof(Corner) == 8, "Corner is two adjacent floats");
RAYOPDF_HOT_INLINE bool Near(Corner c) {
    uint64_t xy;
    std::memcpy(&xy, &c, sizeof xy);
    constexpr uint64_t kMagnitude = 0x7FFFFFFF7FFFFFFFull, kBias = 0x3B0000003B000000ull, kHigh = 0x8000000080000000ull;
    return (((xy & kMagnitude) + kBias) & kHigh) == 0;
}

// A near corner's column and row.
RAYOPDF_HOT_INLINE void NearCells(Corner c, uint32_t& column, uint32_t& row) {
    column = static_cast<uint32_t>(static_cast<int32_t>(c.x + 4096.0f));
    row = static_cast<uint32_t>(static_cast<int32_t>(c.y * 0.125f + 1024.0f));
}

// A corner's column and row; false beyond kKeepReach (or NaN).
bool CellsOf(Corner c, uint32_t& column, uint32_t& row) {
    if (Near(c)) {
        NearCells(c, column, row);
        return true;
    }
    if (!(std::fabs(c.x) < kKeepReach && std::fabs(c.y) < kKeepReach)) return false;
    column = static_cast<uint32_t>(FloorOf(static_cast<double>(c.x)) + kColumnOrigin);
    row = static_cast<uint32_t>(FloorOf(static_cast<double>(c.y) / 8) + kRowOrigin);
    return true;
}

// The columns [ca, cb] and rows [ra, rb] under a near corner's window.
RAYOPDF_HOT_INLINE void NearWindow(Corner c, float tolerance, uint32_t& ca, uint32_t& cb, uint32_t& ra, uint32_t& rb) {
    const float rx = tolerance + 1.0f / 512, ry = tolerance * 0.125f + 1.0f / 2048;
    const float qy = c.y * 0.125f;
    ca = static_cast<uint32_t>(static_cast<int32_t>(c.x + (4096.0f - rx)));
    cb = static_cast<uint32_t>(static_cast<int32_t>(c.x + (4096.0f + rx)));
    ra = static_cast<uint32_t>(static_cast<int32_t>(qy + (1024.0f - ry)));
    rb = static_cast<uint32_t>(static_cast<int32_t>(qy + (1024.0f + ry)));
}

// The same for any corner within kTestReach. Far from the origin the columns and rows may wrap
// around 2^32 inside the window (cb < ca or rb < ra).
void WindowOf(Corner c, float tolerance, uint32_t& ca, uint32_t& cb, uint32_t& ra, uint32_t& rb) {
    if (Near(c)) {
        NearWindow(c, tolerance, ca, cb, ra, rb);
        return;
    }
    const double reach = static_cast<double>(tolerance) + 1.0 / 1024;
    ca = static_cast<uint32_t>(FloorOf(c.x - reach) + kColumnOrigin);
    cb = static_cast<uint32_t>(FloorOf(c.x + reach) + kColumnOrigin);
    ra = static_cast<uint32_t>(FloorOf((c.y - reach) / 8) + kRowOrigin);
    rb = static_cast<uint32_t>(FloorOf((c.y + reach) / 8) + kRowOrigin);
}

// The probe's home cell of a corner. Coordinates are finite but may be huge: clamped before the
// integer conversion.
void ProbeCell(Corner c, int64_t& cx, int64_t& cy) {
    if (Near(c)) {
        cx = FloorOf(c.x / kProbeCell);
        cy = FloorOf(c.y / kProbeCell);
    } else {
        cx = FloorOf(Fmin(Fmax(c.x / kProbeCell, -1e15f), 1e15f));
        cy = FloorOf(Fmin(Fmax(c.y / kProbeCell, -1e15f), 1e15f));
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
    uint64_t* bits = nullptr;
    uint32_t* heads = nullptr;
    FoldEntry* entries = nullptr;
    uint32_t* table = nullptr;  // made when the probe is first needed
    uint64_t mask = 0;
    uint32_t base = 1, kept = 0, entryCount = 0;
    uint32_t placed = 0;  // glyphs placed so far, the current one included
    FoldScratch* fold = nullptr;
    Buffer<uint64_t> ownBits;
    Buffer<uint32_t> ownHeads;
    Buffer<FoldEntry> ownEntries;
    Buffer<uint32_t> ownTable;

    explicit Folder(PageData& p) : page(p) {}
    size_t WordOf(uint64_t key, uint32_t column, uint32_t row) const {
        return static_cast<size_t>((key + uint64_t(column >> 6) * kColumnMix + uint64_t(row) * kRowMix) >> 32) & wordMask;
    }
    void Chain(size_t word, uint32_t column, uint32_t glyph) {
        bits[word] |= uint64_t(1) << (column & 63);
        entries[entryCount] = FoldEntry{heads[word], static_cast<uint32_t>(word), glyph};
        heads[word] = base + entryCount++;
    }
    void Insert(uint32_t position, const Glyph& g, uint64_t h) {
        int64_t cx, cy;
        ProbeCell(CornerOf(g), cx, cy);
        uint64_t slot = Mix(cx, cy, h) & mask;
        while (table[slot] >= base) slot = (slot + 1) & mask;
        table[slot] = base + position;
    }
    // A glyph kept outside Place while the table exists joins it as Place would add it.
    RAYOPDF_NOINLINE void InsertKept(uint32_t position, const Glyph& g) { Insert(position, g, HashText(page.GlyphText(g))); }
    // Whether kept glyph `o` is one the probe takes for `g` (whose text, and so its key, it has,
    // and whose corner is `c`).
    bool Matches(const Glyph& o, const Glyph& g, Corner c, float tolerance, std::string_view text) const {
        if (o.dir != g.dir) return false;
        const Corner oc = CornerOf(o);
        return std::fabs(oc.x - c.x) < tolerance && std::fabs(oc.y - c.y) < tolerance &&
               std::fabs(o.size - g.size) < tolerance && page.GlyphText(o) == text;
    }
    void MakeTable(uint32_t i);
    RAYOPDF_NOINLINE void Place(uint32_t i);
};

// The probe's table, sized as for all the page's placed glyphs (those after glyph i counted
// ahead), filled with the glyphs kept before i in their order.
void Folder::MakeTable(uint32_t i) {
    const GlyphVector& glyphs = page.glyphs;
    size_t count = placed;
    for (size_t j = size_t(i) + 1; j < glyphs.size(); ++j) count += Placed(glyphs[j], keepInvisible) ? 1 : 0;
    size_t capacity = 16;
    while (capacity < count * 2) capacity <<= 1;
    mask = capacity - 1;
    if (capacity <= kScratchSlots) {
        if (fold->tableSize < capacity) fold->table = Zeroed<uint32_t>(capacity), fold->tableSize = capacity;
        table = fold->table.get();
    } else {
        ownTable = Zeroed<uint32_t>(capacity);
        table = ownTable.get();
    }
    for (uint32_t p = 0; p < kept; ++p) {
        const Glyph& o = glyphs[vis[p]];
        if (!(o.flags & kGlyphSpace)) Insert(p, o, HashText(page.GlyphText(o)));
    }
}

// The rule for glyph i (placed, no space), kept or dropped.
void Folder::Place(uint32_t i) {
    GlyphVector& glyphs = page.glyphs;
    Glyph& g = glyphs[i];
    const std::string_view text = page.GlyphText(g);
    const Corner c = CornerOf(g);
    const uint64_t key = KeyOf(text.data(), static_cast<uint32_t>(text.size()), g.dir);
    // Fmax against a constant: a NaN size gives 0.5 either way.
    const float scaled = 0.15f * g.size, tolerance = scaled > 0.5f ? scaled : 0.5f;
    bool probe = true;
    uint32_t found = kEmpty;
    if (tolerance <= kTestTolerance && std::fabs(c.x) < kTestReach && std::fabs(c.y) < kTestReach) {
        uint32_t ca, cb, ra, rb;
        WindowOf(c, tolerance, ca, cb, ra, rb);
        // The window's words (at most two blocks of columns by three rows), each once, with the
        // bits of its columns.
        size_t at[6];
        uint64_t wanted[6];
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
                    if (d == distinct) at[distinct] = word, wanted[distinct++] = 0;
                    wanted[d] |= columns;
                }
            }
            bool hit = false;
            for (uint32_t d = 0; d < distinct; ++d) hit = hit || (bits[at[d]] & wanted[d]) != 0;
            if (!hit) {
                probe = false;
            } else {
                uint32_t count = 0, steps = 0;
                for (uint32_t d = 0; d < distinct; ++d) {
                    for (uint32_t p = heads[at[d]]; p >= base && steps < kMaxChainSteps; ++steps) {
                        const FoldEntry& e = entries[p - base];
                        if (Matches(glyphs[e.glyph], g, c, tolerance, text)) count++, found = e.glyph;
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
        ProbeCell(c, cx, cy);
        found = kEmpty;
        for (int dx = -1; dx <= 1 && found == kEmpty; ++dx) {
            for (int dy = -1; dy <= 1 && found == kEmpty; ++dy) {
                uint64_t slot = Mix(cx + dx, cy + dy, h) & mask;
                for (size_t probes = 0; table[slot] >= base && probes <= mask; ++probes) {
                    const uint32_t p = table[slot] - base;
                    if (Matches(glyphs[vis[p]], g, c, tolerance, text)) {
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
    if (CellsOf(c, column, row)) Chain(WordOf(key, column, row), column, i);
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
// (hash, then bytes) whose box's lower-left corner (x0, y0 on the upright page) and size lie
// within the glyph's tolerance t, and takes the first it meets. A kept glyph it could take lies in
// the window [x0 - t, x0 + t] x [y0 - t, y0 + t]
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
// Finiteness: each of the six values summed over the page's placed glyphs, and the six sums
// added, is finite when all are (an overflow only sends a page with finite values the checked way).
bool CollectAndFold(PageData& page, std::vector<uint32_t>& vis, bool keepInvisible) {
    GlyphVector& glyphVector = page.glyphs;
    const size_t n = glyphVector.size();
    if (vis.size() < n) vis.resize(n);
    Folder f(page);
    f.vis = vis.data();
    f.keepInvisible = keepInvisible;
    // The filter: two words per probe slot for all the page's glyphs.
    size_t capacity = 16;
    while (capacity < n * 2) capacity <<= 1;
    const size_t filterWords = capacity;
    f.wordMask = filterWords - 1;
    FoldScratch& fold = PerThread<FoldScratch>();
    f.fold = &fold;
    if (UINT32_MAX - fold.base < n) {
        if (fold.words > 0) std::memset(fold.heads.get(), 0, fold.words * sizeof(uint32_t));
        if (fold.tableSize > 0) std::memset(fold.table.get(), 0, fold.tableSize * sizeof(uint32_t));
        fold.base = 1;
    }
    f.base = fold.base;
    const bool scratch = capacity <= kScratchSlots;
    if (scratch) {
        if (fold.words < filterWords) {
            fold.bits = Zeroed<uint64_t>(filterWords), fold.heads = Zeroed<uint32_t>(filterWords);
            fold.words = filterWords;
        }
        if (fold.entryCount < n) fold.entries = Zeroed<FoldEntry>(n), fold.entryCount = n;
        f.bits = fold.bits.get(), f.heads = fold.heads.get();
        f.entries = fold.entries.get();
    } else {
        f.ownBits = Zeroed<uint64_t>(filterWords), f.ownHeads = Zeroed<uint32_t>(filterWords);
        f.ownEntries = Zeroed<FoldEntry>(n);
        f.bits = f.ownBits.get(), f.heads = f.ownHeads.get();
        f.entries = f.ownEntries.get();
    }
    // The common case in locals: a placed glyph near the origin, of a tolerance up to 3.5 (its
    // window has one or two rows: under one row high, rounding included), whose window's columns
    // lie in one block, with no filter bit set there (kept, and added to the probe's table when
    // that exists).
    uint32_t* const order = f.vis;
    Glyph* const glyphs = glyphVector.data();
    const char* const textData = page.text.data();
    const size_t textSize = page.text.size();
    uint64_t* const bits = f.bits;
    uint32_t* const heads = f.heads;
    FoldEntry* const entries = f.entries;
    const uint32_t base = f.base;
    const size_t wordMask = f.wordMask;
    // NearWindow's offsets for the size of the glyph before (most glyphs share it): the same
    // values, computed once per size.
    float windowSize = NAN, x0Low = 0, x0High = 0, qyLow = 0, qyHigh = 0;
    bool windowSmall = false;  // tolerance <= 3.5
    bool probing = false;  // the probe's table exists
    uint32_t kept = 0, entryCount = 0, placed = 0;
    // The finiteness sums: one per field, each added from memory without a load of its own.
    float sumU0 = 0, sumU1 = 0, sumVb = 0, sumVt = 0, sumV = 0, sumSize = 0;
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
        sumU0 += g.u0, sumU1 += g.u1, sumVb += g.vb, sumVt += g.vt, sumV += g.v, sumSize += g.size;
        if (g.flags & kGlyphSpace) {
            order[kept++] = i;
            continue;
        }
        const Corner c = CornerOf(g);
        if (RAYOPDF_LIKELY(Near(c))) {
            if (!(g.size == windowSize)) {
                const float scaled = 0.15f * g.size, tolerance = scaled > 0.5f ? scaled : 0.5f;
                const float rx = tolerance + 1.0f / 512, ry = tolerance * 0.125f + 1.0f / 2048;
                x0Low = 4096.0f - rx, x0High = 4096.0f + rx, qyLow = 1024.0f - ry, qyHigh = 1024.0f + ry;
                windowSmall = tolerance <= 3.5f;
                windowSize = g.size;
            }
            const float qy = c.y * 0.125f;
            const uint32_t ca = static_cast<uint32_t>(static_cast<int32_t>(c.x + x0Low));
            const uint32_t cb = static_cast<uint32_t>(static_cast<int32_t>(c.x + x0High));
            if (windowSmall && ((ca ^ cb) >> 6) == 0 && size_t(g.text) + g.textLength <= textSize) {
                const uint32_t ra = static_cast<uint32_t>(static_cast<int32_t>(qy + qyLow));
                const uint32_t rb = static_cast<uint32_t>(static_cast<int32_t>(qy + qyHigh));
                const uint64_t key = KeyOf(textData + g.text, g.textLength, g.dir);
                const uint64_t columns = ((uint64_t(2) << (cb - ca)) - 1) << (ca & 63);
                const uint64_t block = key + uint64_t(ca >> 6) * kColumnMix;
                const size_t wa = static_cast<size_t>((block + uint64_t(ra) * kRowMix) >> 32) & wordMask;
                const size_t wb = static_cast<size_t>((block + uint64_t(rb) * kRowMix) >> 32) & wordMask;
                if (((bits[wa] | bits[wb]) & columns) == 0) {
                    // Kept: its column and row lie in the window, so its word is wa or wb.
                    uint32_t column, row;
                    NearCells(c, column, row);
                    const size_t word = row == ra ? wa : wb;
                    bits[word] |= uint64_t(1) << (column & 63);
                    entries[entryCount] = FoldEntry{heads[word], static_cast<uint32_t>(word), i};
                    heads[word] = base + entryCount++;
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
        for (uint32_t e = 0; e < entryCount; ++e) bits[entries[e].word] = 0;
    const float sum = ((sumU0 + sumU1) + (sumVb + sumVt)) + (sumV + sumSize);
    return sum - sum == 0;
}

// The opening quotes among OpeningMark's: “ ‘ „ «.
bool OpeningQuote(std::string_view t) {
    return t == "\xE2\x80\x9C" || t == "\xE2\x80\x98" || t == "\xE2\x80\x9E" || t == "\xC2\xAB";
}

// An opening quote or bracket: ( [ “ ‘ „ «.
RAYOPDF_HOT_INLINE bool OpeningMark(const PageData& page, const Glyph& g) {
    if (g.textLength == 0) return false;
    const unsigned char c = static_cast<unsigned char>(page.text[g.text]);
    if (c == '(' || c == '[') return g.textLength == 1;
    return (c == 0xE2 || c == 0xC2) && OpeningQuote(page.GlyphText(g));
}

// Glyphs `at[0, count)` (1-3) are a note's raised mark before `next`: digits or * † ‡, smaller than
// `next` and raised above it, as word processors set a note's mark at the margin a tab before its
// text.
bool RaisedNoteMark(const PageData& page, const uint32_t* at, uint32_t count, const Glyph& next) {
    float size = 0, v = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const Glyph& m = page.glyphs[at[k]];
        const std::string_view t = page.GlyphText(m);
        if (!((t.size() == 1 && t[0] >= '0' && t[0] <= '9') || t == "*" || t == "\xE2\x80\xA0" || t == "\xE2\x80\xA1"))
            return false;
        size = Fmax(size, m.size);
        v = k == 0 ? m.v : Fmin(v, m.v);
    }
    return size < 0.87f * next.size && v - next.v > 0.2f * next.size;
}

// Punctuation that closes what is before it: , . ; : ! ? % ) ] } ” ’ ».
bool ClosingMark(const PageData& page, const Glyph& g) {
    if (g.textLength == 0) return false;
    const std::string_view t = page.GlyphText(g);
    if (t.size() == 1) {
        const char c = t[0];
        return c == ',' || c == '.' || c == ';' || c == ':' || c == '!' || c == '?' || c == '%' || c == ')' || c == ']' ||
               c == '}';
    }
    return t == "\xE2\x80\x9D" || t == "\xE2\x80\x99" || t == "\xC2\xBB";
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

// Whether `text` has a RightToLeftLead byte. Eight bytes at a time first: a word none of whose
// bytes is 0xD6 or above has none (a byte's low seven bits plus 0x2A reach its bit 7, without a
// carry into the next byte, exactly when they are 0x56 or above).
bool HasRightToLeftLead(std::string_view text) {
    const unsigned char* const p = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, sizeof w);
        if ((((w & 0x7F7F7F7F7F7F7F7Full) + 0x2A2A2A2A2A2A2A2Aull) & w & 0x8080808080808080ull) == 0) continue;
        for (size_t k = i; k < i + 8; ++k)
            if (RightToLeftLead(p[k])) return true;
    }
    for (; i < n; ++i)
        if (RightToLeftLead(p[i])) return true;
    return false;
}

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
    AppendLineText(page, line, out, skip);
}

void AppendLineText(const PageData& page, const Line& line, std::string& out, uint32_t skip) {
    // Glyphs whose texts follow each other in the page text, with no word break between them
    // (most of a word), are appended as one piece: the same bytes as glyph by glyph.
    const size_t textSize = page.text.size();
    uint32_t k = skip;
    while (k < line.count) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (k > skip && (g.flags & kGlyphSpaceBefore)) out.push_back(' ');
        const std::string_view first = page.GlyphText(g);
        size_t end = size_t(g.text) + first.size();
        for (++k; k < line.count; ++k) {
            const Glyph& n = page.glyphs[page.lineGlyphs[line.first + k]];
            if ((n.flags & kGlyphSpaceBefore) || n.text != end || textSize - end < n.textLength) break;
            end += n.textLength;
        }
        out.append(first.data(), end - g.text);
    }
}

// A caption: a label word and its number ("Figure 6.1.1: …", "Table 13.2. …", "Fig. A1",
// "TABLE IV"), in any letter case.
bool IsCaption(std::string_view t) {
    // Most texts start with none of the labels' first letters (A-Z | 0x20 is a-z).
    if (t.empty()) return false;
    const char c0 = static_cast<char>(t[0] | 0x20);
    if (c0 != 'f' && c0 != 't' && c0 != 'c' && c0 != 'e' && c0 != 'g' && c0 != 'd') return false;
    static const std::string_view kLabels[] = {"figure ", "fig. ", "table ", "chart ", "exhibit ", "graph ", "diagram "};
    for (const std::string_view label : kLabels) {
        if (t.size() < label.size() + 1) continue;
        bool same = true;
        for (size_t i = 0; i < label.size() && same; ++i) {
            char c = t[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            same = c == label[i];
        }
        if (!same) continue;
        const char c = t[label.size()];
        const char d = t.size() > label.size() + 1 ? t[label.size() + 1] : ' ';
        if (c >= '0' && c <= '9') return true;
        // "A1", "A.2", "IV", "S3"
        return c >= 'A' && c <= 'Z' && ((d >= '0' && d <= '9') || d == '.' || d == ':' || d == ' ' || d == 'V' || d == 'I');
    }
    return false;
}

void AppendGlyphTexts(const PageData& page, const Line& line, uint32_t k0, uint32_t k1, std::string& out) {
    ForGlyphTextRuns(page, line, k0, k1, [&](std::string_view text) { out.append(text); });
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
    bool accents = false;  // received a fragment of accents alone: SeatAccents seats them
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
    bool split = false;    // a gutter splits it
    bool markTab = false;  // a note's raised mark, then a tab, leads it: no gutter after the mark
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
    std::vector<uint32_t> stray;  // positions in `vis` of the spacing accents ComposeAccents left
    std::vector<Fragment> frags;
    std::vector<Group> groups;
    std::vector<Proto> protos;
    Buffer<Segment> segments;  // Zeroed: a page faults in only the segments it writes
    size_t segmentCapacity = 0;
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

// Lines that draw their own spaces may still leave one out where a run of text ends (an Office
// export starts the next run where the space would have ended): on such a line a gap of
// kSpaceShare of the width of its spaces in the font before it at least is a word break too, when
// the fonts on both sides draw spaces in the line and the gaps beside it are under kTightShare of
// it. In letter-spaced text every gap is as wide: its words stay whole. A formula's fonts draw no
// spaces: the room set around its operators breaks nothing, nor does the room after it before the
// punctuation that follows (no such break comes before a closing mark or after an opening one).
// Right-to-left text is shaped (joined forms, kashidas) and may leave such room inside a word: the
// rule keeps to other text.
constexpr float kSpaceShare = 0.85f, kTightShare = 0.6f;

// The fonts of a line's space glyphs, the first kSpaceFonts of them, with their widths' sum in
// units of their size.
struct SpaceFont {
    uint16_t font;
    uint32_t count;
    float sum;
};
constexpr uint32_t kSpaceFonts = 8;

uint32_t SpaceFonts(const Glyph* glyphs, const uint32_t* begin, const uint32_t* end, SpaceFont* fonts) {
    uint32_t count = 0;
    for (const uint32_t* at = begin; at != end; ++at) {
        const Glyph& s = glyphs[*at];
        if (!(s.flags & kGlyphSpace) || !(s.size > 0)) continue;
        const float width = (s.u1 - s.u0) / s.size;
        if (!(width > 0.05f && width < 1.5f)) continue;
        uint32_t f = 0;
        while (f < count && fonts[f].font != s.font) ++f;
        if (f == count) {
            if (count == kSpaceFonts) continue;
            fonts[count++] = SpaceFont{s.font, 0, 0};
        }
        fonts[f].count++;
        fonts[f].sum += width;
    }
    return count;
}

// The mean width of `font`'s spaces among `fonts`; 0 when it draws none.
float SpaceWidth(const SpaceFont* fonts, uint32_t count, uint16_t font) {
    for (uint32_t f = 0; f < count; ++f)
        if (fonts[f].font == font) return fonts[f].sum / float(fonts[f].count);
    return 0;
}

// Whether a gap of `gap` points between `before` and `g` (glyphs of size about `em`) on a line that
// draws its own spaces is a space the line left out (kSpaceShare). `previous` and `next` are the
// gaps beside it (-1 for none); `fonts` and `count` keep SpaceFonts of the line's glyphs
// [begin, end), found at the first call that needs them (`count` -1 until then).
RAYOPDF_NOINLINE bool DroppedSpace(const PageData& page, const Glyph& before, const Glyph& g, float gap, float em,
                                   float previous, float next, const uint32_t* begin, const uint32_t* end,
                                   SpaceFont* fonts, int32_t& count) {
    if (!(previous < kTightShare * gap && next < kTightShare * gap)) return false;
    const auto mayBeRtl = [&](const Glyph& x) {
        return x.textLength != 0 && RightToLeftLead(static_cast<unsigned char>(page.text[x.text]));
    };
    if (mayBeRtl(before) || mayBeRtl(g) || OpeningMark(page, before) || ClosingMark(page, g)) return false;
    if (count < 0) count = static_cast<int32_t>(SpaceFonts(page.glyphs.data(), begin, end, fonts));
    const float width = SpaceWidth(fonts, static_cast<uint32_t>(count), before.font);
    return width > 0 && SpaceWidth(fonts, static_cast<uint32_t>(count), g.font) > 0 && gap >= kSpaceShare * width * em;
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

// Spacing accents drawn as glyphs of their own over (or, for the cedilla and the ogonek, under) a
// letter: TeX's \accent with OT1 fonts, and some word processors, which draw the accent before or
// after its letter. Their combining marks, in the order of kSpacingAccents' rows.
constexpr uint16_t kAccentMarks[13] = {0x300, 0x301, 0x302, 0x303, 0x304, 0x306, 0x307,
                                       0x308, 0x30A, 0x30B, 0x30C, 0x327, 0x328};
constexpr int kAccentDotAbove = 6, kAccentFirstBelow = 11;  // the dot; the cedilla and the ogonek
// The NFC composition of A-Z then a-z with each accent's mark, 0 where Unicode has none.
constexpr uint16_t kAccentComposed[13][52] = {
    // ` grave: U+0300
    {0x00C0, 0, 0, 0, 0x00C8, 0, 0, 0, 0x00CC, 0, 0, 0, 0, 0x01F8, 0x00D2, 0, 0, 0, 0, 0, 0x00D9, 0, 0x1E80, 0, 0x1EF2, 0,
     0x00E0, 0, 0, 0, 0x00E8, 0, 0, 0, 0x00EC, 0, 0, 0, 0, 0x01F9, 0x00F2, 0, 0, 0, 0, 0, 0x00F9, 0, 0x1E81, 0, 0x1EF3, 0},
    // ´ acute: U+0301
    {0x00C1, 0, 0x0106, 0, 0x00C9, 0, 0x01F4, 0, 0x00CD, 0, 0x1E30, 0x0139, 0x1E3E, 0x0143, 0x00D3, 0x1E54, 0, 0x0154, 0x015A, 0, 0x00DA, 0, 0x1E82, 0, 0x00DD, 0x0179,
     0x00E1, 0, 0x0107, 0, 0x00E9, 0, 0x01F5, 0, 0x00ED, 0, 0x1E31, 0x013A, 0x1E3F, 0x0144, 0x00F3, 0x1E55, 0, 0x0155, 0x015B, 0, 0x00FA, 0, 0x1E83, 0, 0x00FD, 0x017A},
    // ˆ circumflex: U+0302
    {0x00C2, 0, 0x0108, 0, 0x00CA, 0, 0x011C, 0x0124, 0x00CE, 0x0134, 0, 0, 0, 0, 0x00D4, 0, 0, 0, 0x015C, 0, 0x00DB, 0, 0x0174, 0, 0x0176, 0x1E90,
     0x00E2, 0, 0x0109, 0, 0x00EA, 0, 0x011D, 0x0125, 0x00EE, 0x0135, 0, 0, 0, 0, 0x00F4, 0, 0, 0, 0x015D, 0, 0x00FB, 0, 0x0175, 0, 0x0177, 0x1E91},
    // ˜ tilde: U+0303
    {0x00C3, 0, 0, 0, 0x1EBC, 0, 0, 0, 0x0128, 0, 0, 0, 0, 0x00D1, 0x00D5, 0, 0, 0, 0, 0, 0x0168, 0x1E7C, 0, 0, 0x1EF8, 0,
     0x00E3, 0, 0, 0, 0x1EBD, 0, 0, 0, 0x0129, 0, 0, 0, 0, 0x00F1, 0x00F5, 0, 0, 0, 0, 0, 0x0169, 0x1E7D, 0, 0, 0x1EF9, 0},
    // ¯ macron: U+0304
    {0x0100, 0, 0, 0, 0x0112, 0, 0x1E20, 0, 0x012A, 0, 0, 0, 0, 0, 0x014C, 0, 0, 0, 0, 0, 0x016A, 0, 0, 0, 0x0232, 0,
     0x0101, 0, 0, 0, 0x0113, 0, 0x1E21, 0, 0x012B, 0, 0, 0, 0, 0, 0x014D, 0, 0, 0, 0, 0, 0x016B, 0, 0, 0, 0x0233, 0},
    // ˘ breve: U+0306
    {0x0102, 0, 0, 0, 0x0114, 0, 0x011E, 0, 0x012C, 0, 0, 0, 0, 0, 0x014E, 0, 0, 0, 0, 0, 0x016C, 0, 0, 0, 0, 0,
     0x0103, 0, 0, 0, 0x0115, 0, 0x011F, 0, 0x012D, 0, 0, 0, 0, 0, 0x014F, 0, 0, 0, 0, 0, 0x016D, 0, 0, 0, 0, 0},
    // ˙ dot above: U+0307
    {0x0226, 0x1E02, 0x010A, 0x1E0A, 0x0116, 0x1E1E, 0x0120, 0x1E22, 0x0130, 0, 0, 0, 0x1E40, 0x1E44, 0x022E, 0x1E56, 0, 0x1E58, 0x1E60, 0x1E6A, 0, 0, 0x1E86, 0x1E8A, 0x1E8E, 0x017B,
     0x0227, 0x1E03, 0x010B, 0x1E0B, 0x0117, 0x1E1F, 0x0121, 0x1E23, 0, 0, 0, 0, 0x1E41, 0x1E45, 0x022F, 0x1E57, 0, 0x1E59, 0x1E61, 0x1E6B, 0, 0, 0x1E87, 0x1E8B, 0x1E8F, 0x017C},
    // ¨ diaeresis: U+0308
    {0x00C4, 0, 0, 0, 0x00CB, 0, 0, 0x1E26, 0x00CF, 0, 0, 0, 0, 0, 0x00D6, 0, 0, 0, 0, 0, 0x00DC, 0, 0x1E84, 0x1E8C, 0x0178, 0,
     0x00E4, 0, 0, 0, 0x00EB, 0, 0, 0x1E27, 0x00EF, 0, 0, 0, 0, 0, 0x00F6, 0, 0, 0, 0, 0x1E97, 0x00FC, 0, 0x1E85, 0x1E8D, 0x00FF, 0},
    // ˚ ring above: U+030A
    {0x00C5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x016E, 0, 0, 0, 0, 0,
     0x00E5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x016F, 0, 0x1E98, 0, 0x1E99, 0},
    // ˝ double acute: U+030B
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0150, 0, 0, 0, 0, 0, 0x0170, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0151, 0, 0, 0, 0, 0, 0x0171, 0, 0, 0, 0, 0},
    // ˇ caron: U+030C
    {0x01CD, 0, 0x010C, 0x010E, 0x011A, 0, 0x01E6, 0x021E, 0x01CF, 0, 0x01E8, 0x013D, 0, 0x0147, 0x01D1, 0, 0, 0x0158, 0x0160, 0x0164, 0x01D3, 0, 0, 0, 0, 0x017D,
     0x01CE, 0, 0x010D, 0x010F, 0x011B, 0, 0x01E7, 0x021F, 0x01D0, 0x01F0, 0x01E9, 0x013E, 0, 0x0148, 0x01D2, 0, 0, 0x0159, 0x0161, 0x0165, 0x01D4, 0, 0, 0, 0, 0x017E},
    // ¸ cedilla: U+0327
    {0, 0, 0x00C7, 0x1E10, 0x0228, 0, 0x0122, 0x1E28, 0, 0, 0x0136, 0x013B, 0, 0x0145, 0, 0, 0, 0x0156, 0x015E, 0x0162, 0, 0, 0, 0, 0, 0,
     0, 0, 0x00E7, 0x1E11, 0x0229, 0, 0x0123, 0x1E29, 0, 0, 0x0137, 0x013C, 0, 0x0146, 0, 0, 0, 0x0157, 0x015F, 0x0163, 0, 0, 0, 0, 0, 0},
    // ˛ ogonek: U+0328
    {0x0104, 0, 0, 0, 0x0118, 0, 0, 0, 0x012E, 0, 0, 0, 0, 0, 0x01EA, 0, 0, 0, 0, 0, 0x0172, 0, 0, 0, 0, 0,
     0x0105, 0, 0, 0, 0x0119, 0, 0, 0, 0x012F, 0, 0, 0, 0, 0, 0x01EB, 0, 0, 0, 0, 0, 0x0173, 0, 0, 0, 0, 0},
};

// The row of kAccentMarks of a glyph text that is one spacing accent, else -1:
// ` ´ ˆ ˜ ¯ ˘ ˙ ¨ ˚ ˝ ˇ ¸ ˛.
int SpacingAccent(std::string_view t) {
    if (t.size() == 1) return t[0] == '`' ? 0 : -1;
    if (t.size() != 2) return -1;
    const unsigned char a = static_cast<unsigned char>(t[0]), b = static_cast<unsigned char>(t[1]);
    if (a == 0xC2) return b == 0xB4 ? 1 : b == 0xAF ? 4 : b == 0xA8 ? 7 : b == 0xB8 ? 11 : -1;
    if (a != 0xCB) return -1;
    switch (b) {
    case 0x86: return 2;
    case 0x9C: return 3;
    case 0x98: return 5;
    case 0x99: return 6;
    case 0x9A: return 8;
    case 0x9D: return 9;
    case 0x87: return 10;
    case 0x9B: return 12;
    default: return -1;
    }
}

// A letter an accent may sit on: Latin, dotless ı and ȷ included.
bool AccentBase(uint32_t cp) {
    if (cp < 0x80) return (cp | 0x20) >= 'a' && (cp | 0x20) <= 'z';
    return (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) || (cp >= 0x1E00 && cp <= 0x1EFF);
}

// Whether the page text may hold a spacing accent: a ` byte, a lead byte CB, or C2 before B4, AF,
// A8 or B8 (´ ¯ ¨ ¸; C2 also leads the no-break space and other common Latin-1 signs).
bool MayHoldAccent(std::string_view text) {
    const char* const begin = text.data();
    const size_t n = text.size();
    if (std::memchr(begin, 0xCB, n) || std::memchr(begin, '`', n)) return true;
    const char* const end = begin + n;
    for (const char* at = begin; (at = static_cast<const char*>(std::memchr(at, 0xC2, size_t(end - at)))) != nullptr;) {
        if (++at == end) break;
        const unsigned char b = static_cast<unsigned char>(*at);
        if (b == 0xB4 || b == 0xAF || b == 0xA8 || b == 0xB8) return true;
    }
    return false;
}

bool MathFontGlyph(const PageData& page, const Glyph& g) {
    return g.font < page.fontMath.size() && (page.fontMath[g.font] & kMathFontClass) != 0;
}

// Accents composed with their letters (ComposeAccents): their geometry, in the size of the letter.
constexpr float kAccentReach = 0.08f;     // the accent's centre within the letter's advance, or this close
constexpr float kAccentRaise = 0.5f;      // the most an accent above sits above the letter's baseline
constexpr float kAccentDrop = 0.05f;      // ... and below it; an accent below, both ways
constexpr float kAccentSizeRatio = 1.3f;  // sizes differ by at most this ratio
// An accent set apart from its glyph (SeatAccents) may sit higher: TeX raises a formula's accent
// over a capital by the capital's height above the x-height.
constexpr float kSeatRaise = 0.7f;

// Whether spacing accent `a` (row `accent` of kAccentMarks) is set over (or under) glyph `b` by
// their geometry alone, whatever their fonts and b's character, an accent above at most `raise`
// above b's baseline; `distance` is how far its centre is from b's.
bool AccentSits(const Glyph& a, const Glyph& b, int accent, float raise, float& distance) {
    if (b.dir != a.dir || (b.flags & kGlyphSpace) || ((a.flags ^ b.flags) & kGlyphInvisible)) return false;
    if (!(a.size <= kAccentSizeRatio * b.size && b.size <= kAccentSizeRatio * a.size)) return false;
    const float cx = (a.u0 + a.u1) / 2, reach = kAccentReach * b.size;
    if (!(cx >= b.u0 - reach && cx <= b.u1 + reach)) return false;
    const float dv = a.v - b.v;
    const bool above = accent < kAccentFirstBelow;
    if (!(dv >= -kAccentDrop * b.size && dv <= (above ? raise : kAccentDrop) * b.size)) return false;
    distance = std::fabs(cx - (b.u0 + b.u1) / 2);
    return true;
}

// Whether spacing accent `a` (row `accent` of kAccentMarks) sits on letter `b` of its font;
// `distance` is how far its centre is from the letter's.
bool AccentOver(const PageData& page, const Glyph& a, const Glyph& b, int accent, float& distance) {
    float d = 0;
    if (b.font != a.font || !AccentSits(a, b, accent, kAccentRaise, d)) return false;
    uint32_t cp = 0;
    size_t length = 0;
    const std::string_view t = page.GlyphText(b);
    if (!Utf8::Decode(t, 0, cp, length) || length != t.size() || !AccentBase(cp)) return false;
    distance = d;
    return true;
}

// Whether glyph `n` is a letter of `b`'s font set beside it in one word (or the spacing accent of
// such a letter): on its baseline, at most a word gap (kWordGap) away.
bool LetterBeside(const PageData& page, const Glyph& b, const Glyph& n) {
    if (n.font != b.font || n.dir != b.dir || (n.flags & kGlyphSpace) || !(std::fabs(n.v - b.v) <= 0.25f * b.size)) return false;
    if (!(Fmax(n.u0 - b.u1, b.u0 - n.u1) <= kWordGap * b.size)) return false;
    uint32_t cp = 0;
    size_t length = 0;
    const std::string_view t = page.GlyphText(n);
    return Utf8::Decode(t, 0, cp, length) && (AccentBase(cp) || SpacingAccent(t) >= 0);
}

// A glyph whose text may be a spacing accent: one byte `, or two from C2 or CB.
bool AccentCandidate(const PageData& page, const Glyph& g) {
    const uint32_t length = g.textLength;
    if (length - 1u > 1u || (g.flags & kGlyphSpace) || size_t(g.text) + length > page.text.size()) return false;
    const unsigned char lead = static_cast<unsigned char>(page.text[g.text]);
    return length == 1 ? lead == '`' : lead == 0xC2 || lead == 0xCB;
}

// The row of kAccentMarks of glyph `g` when it is a spacing accent, else -1.
int GlyphAccent(const PageData& page, const Glyph& g) {
    return AccentCandidate(page, g) ? SpacingAccent(page.GlyphText(g)) : -1;
}

// Composes spacing accent `a` (row `accent` of kAccentMarks) with letter `b` it sits on: the
// letter's text becomes the NFC precomposed character, else the letter and the combining mark (a
// dotless ı or ȷ under an accent above becomes i or j); the letter's box grows to the accent's
// height (not its width), and the accent is dropped.
void ComposeAccent(PageData& page, Glyph& a, Glyph& b, int accent) {
    uint32_t cp = 0;
    size_t length = 0;
    Utf8::Decode(page.GlyphText(b), 0, cp, length);
    const bool dotless = cp == 0x131 || cp == 0x237;
    if (dotless && accent < kAccentFirstBelow) cp = cp == 0x131 ? 'i' : 'j';
    const size_t letter = cp < 0x80 ? (cp >= 'a' ? 26 + cp - 'a' : cp - 'A') : 52;
    const uint32_t composed = letter < 52 ? kAccentComposed[accent][letter] : 0;
    const size_t at = page.text.size();
    if (composed != 0) {
        Utf8::Append(page.text, composed);
    } else {
        Utf8::Append(page.text, cp);
        if (!(dotless && accent == kAccentDotAbove)) Utf8::Append(page.text, kAccentMarks[accent]);
    }
    b.text = static_cast<uint32_t>(at);
    b.textLength = static_cast<uint16_t>(page.text.size() - at);
    // The letter keeps its advance: an accent wider than it (an acute over a dotless ı) must not
    // narrow the word gap after it ("Jiří Sgall").
    b.vb = Fmin(b.vb, a.vb), b.vt = Fmax(b.vt, a.vt);
    a.flags |= kGlyphDropped;
}

// Composes each spacing accent of `vis` (content order) with the letter next to it in that order
// that it is drawn over (or under) in the same font (ComposeAccent), and the accent leaves `vis`.
// Math's accents stay apart (layout/math.h): those of math fonts, and TeX's \hat or \bar, drawn in
// a text font over a letter of a math or another font. With `mathScripts` (--math=latex), whose
// formulas read an accent over a lone letter themselves, a bold letter takes its accent only with
// another letter of its font beside it, in a word: \tilde{\mathbf{u}} sets accent and letter in one
// bold font. `stray` receives the positions in `vis` of the spacing accents that stay, ascending.
void ComposeAccents(PageData& page, std::vector<uint32_t>& vis, bool mathScripts, std::vector<uint32_t>& stray) {
    stray.clear();
    if (!MayHoldAccent(page.text)) return;
    GlyphVector& glyphs = page.glyphs;
    const size_t n = vis.size();
    size_t k = 0;
    while (k < n && !AccentCandidate(page, glyphs[vis[k]])) ++k;
    size_t out = k;
    for (; k < n; ++k) {
        Glyph& a = glyphs[vis[k]];
        const int accent = GlyphAccent(page, a);
        if (accent < 0 || MathFontGlyph(page, a)) {
            if (accent >= 0) stray.push_back(static_cast<uint32_t>(out));
            vis[out++] = vis[k];
            continue;
        }
        // The letter before it (already in place at out - 1) or after it, the nearer of two.
        float before = 0, after = 0;
        const bool b0 = out > 0 && AccentOver(page, a, glyphs[vis[out - 1]], accent, before);
        const bool b1 = k + 1 < vis.size() && AccentOver(page, a, glyphs[vis[k + 1]], accent, after);
        if ((!b0 && !b1) || page.text.size() >= (size_t(1) << 30)) {
            stray.push_back(static_cast<uint32_t>(out));
            vis[out++] = vis[k];
            continue;
        }
        const bool next = b1 && (!b0 || after < before);
        Glyph& b = glyphs[next ? vis[k + 1] : vis[out - 1]];
        if (mathScripts && (b.flags & kGlyphBold)) {
            // The glyphs before and after the letter, the accent left out.
            const size_t left = next ? out : out - 1, right = next ? k + 2 : k + 1;
            if (!((left > 0 && LetterBeside(page, b, glyphs[vis[left - 1]])) ||
                  (right < vis.size() && LetterBeside(page, b, glyphs[vis[right]])))) {
                stray.push_back(static_cast<uint32_t>(out));
                vis[out++] = vis[k];
                continue;
            }
        }
        ComposeAccent(page, a, b, accent);
    }
    vis.resize(out);
}

// Accents set apart from their letter (SeatAccents): the glyphs of the line searched on each side
// of an accent for it, the accents examined per page, how far beyond the accent and its letter (in
// the letter's size) a glyph between them is drawn elsewhere, and the most fragments of a group of
// accents alone that joins the line of their letter (PlaceGlyphs).
constexpr size_t kSeatWindow = 256;
constexpr uint32_t kSeatBudget = 4096;
constexpr float kSeatApart = 0.3f;
constexpr uint32_t kAccentFragments = 4;

// The glyph of `line` (count `n`) that spacing accent `a` (row `accent`) sits over (AccentSits), the
// nearest of them, within kSeatWindow of position `p`; `n` for none.
size_t AccentSeat(const PageData& page, const uint32_t* line, size_t n, size_t p, const Glyph& a, int accent) {
    const size_t lo = p > kSeatWindow ? p - kSeatWindow : 0, hi = n - p > kSeatWindow ? p + kSeatWindow + 1 : n;
    size_t q = n;
    float best = 0;
    for (size_t i = lo; i < hi; ++i) {
        const Glyph& b = page.glyphs[line[i]];
        float d = 0;
        if (i == p || !AccentSits(a, b, accent, kSeatRaise, d) || GlyphAccent(page, b) >= 0) continue;
        if (q == n || d < best) q = i, best = d;
    }
    return q;
}

// The spacing accents of a line (`begin`, `end`) that content order set apart from the glyph they
// sit over, glyphs drawn elsewhere between them: TeX draws a formula's \hat over the first glyph of a
// line before that line (at the end of the line above) and some producers draw an accent after its
// line. Each goes just before its glyph, where TeX draws an accent, or is composed with it where
// ComposeAccents would compose it beside it (a letter of its font). The reordered line is written to
// `seq`, which `begin` points into when `inSeq`; returns whether `seq` holds the line. `budget`
// bounds the accents examined on the page.
bool SeatAccents(PageData& page, const uint32_t* begin, const uint32_t* end, bool inSeq, std::vector<uint32_t>& seq,
                 bool mathScripts, uint32_t& budget) {
    GlyphVector& glyphs = page.glyphs;
    bool copied = inSeq;
    size_t n = static_cast<size_t>(end - begin);
    for (size_t p = 0; p < n && budget > 0;) {
        const uint32_t* line = copied ? seq.data() : begin;
        Glyph& a = glyphs[line[p]];
        const int accent = GlyphAccent(page, a);
        if (accent < 0) {
            ++p;
            continue;
        }
        // In place: beside a glyph it sits over, or in the room between the glyphs beside it.
        const float cx = (a.u0 + a.u1) / 2;
        const Glyph* const before = p > 0 ? &glyphs[line[p - 1]] : nullptr;
        const Glyph* const after = p + 1 < n ? &glyphs[line[p + 1]] : nullptr;
        float d = 0;
        if ((before != nullptr && AccentSits(a, *before, accent, kSeatRaise, d) && GlyphAccent(page, *before) < 0) ||
            (after != nullptr && AccentSits(a, *after, accent, kSeatRaise, d) && GlyphAccent(page, *after) < 0) ||
            ((before == nullptr || before->u1 <= cx) && (after == nullptr || after->u0 >= cx))) {
            ++p;
            continue;
        }
        --budget;
        const size_t q = AccentSeat(page, line, n, p, a, accent);
        if (q == n || q + 1 == p || p + 1 == q) {
            ++p;
            continue;
        }
        const Glyph& base = glyphs[line[q]];
        const float lo = Fmin(a.u0, base.u0) - kSeatApart * base.size, hi = Fmax(a.u1, base.u1) + kSeatApart * base.size;
        bool apart = false;
        for (size_t i = (p < q ? p : q) + 1, last = p < q ? q : p; i < last && !apart; ++i) {
            const Glyph& g = glyphs[line[i]];
            const float c = (g.u0 + g.u1) / 2;
            apart = !(g.flags & kGlyphSpace) && !(c >= lo && c <= hi);
        }
        if (!apart) {
            ++p;
            continue;
        }
        if (!copied) seq.assign(begin, end), copied = true;
        uint32_t* const s = seq.data();
        Glyph& b = glyphs[s[q]];
        float distance = 0;
        bool compose = !MathFontGlyph(page, a) && AccentOver(page, a, b, accent, distance) &&
                       page.text.size() < (size_t(1) << 30);
        if (compose && mathScripts && (b.flags & kGlyphBold))
            compose = (q > 0 && LetterBeside(page, b, glyphs[s[q - 1]])) || (q + 1 < n && LetterBeside(page, b, glyphs[s[q + 1]]));
        if (compose) {
            ComposeAccent(page, a, b, accent);
            seq.erase(seq.begin() + static_cast<std::ptrdiff_t>(p));
            --n;
        } else if (p > q) {
            std::rotate(s + q, s + p, s + p + 1);
            ++p;
        } else {
            std::rotate(s + p, s + p + 1, s + q);
        }
    }
    return copied;
}

// Lines from the placed glyphs `vis` (content order), M the page's min and max. `stray`: the
// positions in `vis` of the spacing accents ComposeAccents left there, ascending.
template <typename M>
void PlaceGlyphs(PageData& page, std::vector<uint32_t>& vis, bool mathScripts, const std::vector<uint32_t>& stray) {
    GlyphVector& glyphs = page.glyphs;
    PlaceScratch& place = PerThread<PlaceScratch>();
    // Whether fragment `f` holds one of those accents.
    const auto holdsAccent = [&](const Fragment& f) {
        const auto at = std::lower_bound(stray.begin(), stray.end(), f.begin);
        return at != stray.end() && *at < f.end;
    };

    // Fragments in content order (algorithms §4.4 recommendation: one pass merging into the
    // current line while direction and baseline match).
    std::vector<Fragment>& frags = place.frags;
    frags.clear();
    // The fragment being extended stays in `f` until the next one starts, out of memory that the
    // glyphs' fields might alias. Every glyph after its first joins it until one starts the next:
    // its end is set as it closes.
    Fragment f;
    bool open = false;
    // In locals: the vector's own fields are reloaded after each store the loop makes.
    const uint32_t* const order = vis.data();
    const uint32_t visCount = static_cast<uint32_t>(vis.size());
    const Glyph* const records = glyphs.data();
    for (uint32_t k = 0; k < visCount; ++k) {
        const Glyph& g = records[order[k]];
        const bool space = (g.flags & kGlyphSpace) != 0;
        if (open) {
            const float em = M::Max(f.size, g.size);
            const float gap = g.u0 - f.lastU1;
            const float dv = std::fabs(g.v - f.baseline);
            const float maxGap = (f.marker ? kMarkerGap : kFragmentGap) * em;
            // On its baseline (within 0.25 em), a space, or a script: smaller than the glyph
            // before it or than the fragment, within 0.6 em of the baseline as all are.
            if (g.dir == f.dir && gap >= -0.6f * em && gap <= maxGap && dv <= 0.6f * em &&
                (dv <= 0.25f * em || space || g.size < 0.87f * f.size || f.lastSize < 0.87f * g.size)) {
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
            f.end = k;
            frags.emplace_back() = f;  // f's address never escapes: it stays in registers
        }
        f = Fragment();
        f.begin = k;
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
    if (open) {
        f.end = visCount;
        frags.emplace_back() = f;
    }
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
    // full-size line it sits in (algorithms §4.4, MuPDF's 0.8 em baseline tolerance). With
    // `mathScripts`, a lone fragment of math-font glyphs (the 2 of e^{x^2}, set above the line's
    // ink) joins a line holding a script glyph it is 0.87 of the size of or smaller, its baseline
    // from 0.6 em below to 0.9 em above the line's, and a lone ∼ joins the line of the = it is
    // stacked over (TeX's \cong: centred on it, about as wide, up to 0.6 em above it). A lone
    // fragment of spacing accents joins the line of the glyph its first accent sits over
    // (AccentSits: TeX raises a formula's accent over a capital past the baseline tolerance), where
    // SeatAccents sets it before that glyph.
    std::vector<std::vector<uint32_t>> extra;  // fragments added to groups, by group, rarely used
    std::vector<uint32_t>& extraOf = place.extraOf;
    extraOf.assign(groups.size(), kEmpty);
    // The size of the largest script glyph of fragment `f` in a line of size `lineSize` (0 for none).
    const auto scriptSize = [&](const Fragment& f, float lineSize) {
        float most = 0;
        for (uint32_t k = f.begin; k < f.end; ++k) {
            const Glyph& g = glyphs[vis[k]];
            if (!(g.flags & kGlyphSpace) && g.size < 0.87f * lineSize) most = M::Max(most, g.size);
        }
        return most;
    };
    // Whether fragment `f` is a ∼ alone stacked over an = of group `line` (TeX's \cong).
    const auto stackedOver = [&](const Fragment& f, const Group& line, size_t o) {
        const Glyph* top = nullptr;
        for (uint32_t k = f.begin; k < f.end; ++k) {
            const Glyph& g = glyphs[vis[k]];
            if (g.flags & kGlyphSpace) continue;
            if (top != nullptr) return false;
            top = &g;
        }
        if (top == nullptr || page.GlyphText(*top) != "\xE2\x88\xBC") return false;
        const float dv = top->v - line.baseline;
        const float width = top->u1 - top->u0, centre = (top->u0 + top->u1) / 2;
        if (!(dv >= 0.05f * line.size && dv <= 0.6f * line.size) || !(width > 0.2f * line.size)) return false;
        const auto over = [&](const Fragment& h) {
            for (uint32_t k = h.begin; k < h.end; ++k) {
                const Glyph& g = glyphs[vis[k]];
                if ((g.flags & kGlyphSpace) || std::fabs(g.v - line.baseline) > 0.1f * line.size || page.GlyphText(g) != "=")
                    continue;
                const float w = g.u1 - g.u0;
                if (std::fabs((g.u0 + g.u1) / 2 - centre) <= 0.15f * line.size && w >= 0.7f * width && w <= 1.4f * width)
                    return true;
            }
            return false;
        };
        for (uint32_t m = line.first; m < line.first + line.count; ++m)
            if (over(frags[members[m]])) return true;
        if (extraOf[o] != kEmpty)
            for (uint32_t m : extra[extraOf[o]])
                if (over(frags[m])) return true;
        return false;
    };
    // Whether a glyph of fragment `h` that is no accent is under accent `a` (row `accent`).
    const auto holdsBase = [&](const Fragment& h, const Glyph& a, int accent) {
        for (uint32_t k = h.begin; k < h.end; ++k) {
            const Glyph& b = glyphs[vis[k]];
            float d = 0;
            if (AccentSits(a, b, accent, kSeatRaise, d) && GlyphAccent(page, b) < 0) return true;
        }
        return false;
    };
    for (size_t s = 0; s < groups.size(); ++s) {
        Group& small = groups[s];
        // Groups are sorted by baseline, descending, per direction: look at neighbours only.
        size_t best = kEmpty;
        bool seat = false;
        if (!stray.empty() && small.count <= kAccentFragments) {
            // The group's first glyph that is no space, when all of them are accents.
            const Glyph* first = nullptr;
            int accent = -1;
            bool accents = holdsAccent(frags[members[small.first]]);
            for (uint32_t m = small.first; m < small.first + small.count && accents; ++m) {
                const Fragment& h = frags[members[m]];
                accents = h.end - h.begin <= 12;
                for (uint32_t k = h.begin; k < h.end && accents; ++k) {
                    const Glyph& g = glyphs[vis[k]];
                    if (g.flags & kGlyphSpace) continue;
                    const int row = GlyphAccent(page, g);
                    accents = row >= 0;
                    if (accents && first == nullptr) first = &g, accent = row;
                }
            }
            for (size_t d = 1; d <= 8 && accents && first != nullptr && best == kEmpty; ++d) {
                for (int side = 0; side < 2 && best == kEmpty; ++side) {
                    if (side == 0 && s < d) continue;
                    const size_t o = side == 0 ? s - d : s + d;
                    if (o >= groups.size() || groups[o].merged || groups[o].dir != small.dir) continue;
                    const Group& line = groups[o];
                    bool holds = false;
                    for (uint32_t m = line.first; m < line.first + line.count && !holds; ++m)
                        holds = holdsBase(frags[members[m]], *first, accent);
                    if (extraOf[o] != kEmpty)
                        for (size_t m = 0; m < extra[extraOf[o]].size() && !holds; ++m)
                            holds = holdsBase(frags[extra[extraOf[o]][m]], *first, accent);
                    if (holds) best = o, seat = true;
                }
            }
        }
        if (best == kEmpty) {
            if (small.count != 1) continue;
            const Fragment& f = frags[members[small.first]];
            if (f.end - f.begin > 12) continue;
            const float center = (small.vb + small.vt) / 2;
            bool mathFragment = mathScripts;
            for (uint32_t k = f.begin; k < f.end && mathFragment; ++k) {
                const Glyph& g = glyphs[vis[k]];
                mathFragment = (g.flags & kGlyphSpace) || (g.font < page.fontMath.size() && (page.fontMath[g.font] & kMathFontClass) != 0);
            }
            for (size_t d = 1; d <= 8; ++d) {
                for (int side = 0; side < 2 && best == kEmpty; ++side) {
                    if (side == 0 && s < d) continue;
                    const size_t o = side == 0 ? s - d : s + d;
                    if (o >= groups.size()) continue;
                    const Group& line = groups[o];
                    if (line.merged || line.dir != small.dir) continue;
                    if (line.size < small.size / 0.87f) {
                        if (!mathFragment || !stackedOver(f, line, o)) continue;
                        best = o;
                        break;
                    }
                    if (small.u0 < line.u0 - 0.3f * line.size || small.u0 - line.u1 > 0.6f * line.size) continue;
                    if (center < line.vb || center > line.vt) {
                        const float dv = small.baseline - line.baseline;
                        if (!mathFragment || dv > 0.9f * line.size || dv < -0.6f * line.size) continue;
                        float script = 0;
                        for (uint32_t m = line.first; m < line.first + line.count; ++m)
                            script = M::Max(script, scriptSize(frags[members[m]], line.size));
                        if (extraOf[o] != kEmpty)
                            for (uint32_t m : extra[extraOf[o]]) script = M::Max(script, scriptSize(frags[m], line.size));
                        if (!(small.size <= 0.87f * script)) continue;
                    }
                    best = o;
                }
                if (best != kEmpty) break;
            }
            if (best == kEmpty) continue;
        }
        Group& line = groups[best];
        small.merged = true;
        if (seat)
            line.accents = true;
        else
            line.scripts = true;
        line.u0 = M::Min(line.u0, small.u0), line.u1 = M::Max(line.u1, small.u1);
        if (extraOf[best] == kEmpty) {
            extraOf[best] = static_cast<uint32_t>(extra.size());
            extra.emplace_back();
        }
        for (uint32_t m = small.first; m < small.first + small.count; ++m) extra[extraOf[best]].push_back(members[m]);
        // With `mathScripts`, fragments that joined this one before it joined a line go with it
        // (a merged group is written with no line of its own): the l of Z^{W^{l}}. The default
        // output keeps its lines as they were, but for the accents seated in it.
        if ((mathScripts || small.accents) && extraOf[s] != kEmpty) {
            std::vector<uint32_t>& from = extra[extraOf[s]];
            std::vector<uint32_t>& to = extra[extraOf[best]];
            to.insert(to.end(), from.begin(), from.end());
            from.clear();
            line.accents = line.accents || small.accents;
        }
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
    if (place.segmentCapacity < vis.size()) place.segments = Zeroed<Segment>(vis.size()), place.segmentCapacity = vis.size();
    Segment* const segments = place.segments.get();
    uint32_t segmentCount = 0;
    std::vector<uint32_t>& lineVis = place.lineVis;
    uint32_t seatBudget = kSeatBudget;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& grp = groups[gi];
        if (grp.merged) continue;
        // The group's glyphs: a lone fragment's are a run of `vis` already. Explicit spaces decide
        // the word breaks of lines that have them (algorithms §4.1).
        const uint32_t *lineBegin, *lineEnd;
        bool spaces = false, seatable = false;
        if (grp.count == 1 && extraOf[gi] == kEmpty) {
            const Fragment& f = frags[members[grp.first]];
            lineBegin = vis.data() + f.begin, lineEnd = vis.data() + f.end;
            spaces = f.spaces;
            seatable = !stray.empty() && holdsAccent(f);
        } else {
            lineVis.clear();
            for (uint32_t m = 0; m < grp.count; ++m) {
                const Fragment& f = frags[members[grp.first + m]];
                lineVis.insert(lineVis.end(), vis.data() + f.begin, vis.data() + f.end);
                spaces = spaces || f.spaces;
                seatable = seatable || (!stray.empty() && holdsAccent(f));
            }
            if (extraOf[gi] != kEmpty) {
                for (uint32_t fi : extra[extraOf[gi]]) {
                    const Fragment& f = frags[fi];
                    lineVis.insert(lineVis.end(), vis.data() + f.begin, vis.data() + f.end);
                    spaces = spaces || f.spaces;
                    seatable = seatable || (!stray.empty() && holdsAccent(f));
                }
            }
            if (grp.scripts) {
                std::stable_sort(lineVis.begin(), lineVis.end(), [&](uint32_t a, uint32_t b) {
                    return glyphs[a].u0 + glyphs[a].u1 < glyphs[b].u0 + glyphs[b].u1;
                });
            }
            lineBegin = lineVis.data(), lineEnd = lineVis.data() + lineVis.size();
        }
        if (seatable && seatBudget > 0) {
            const bool inSeq = lineBegin == lineVis.data();
            if (SeatAccents(page, lineBegin, lineEnd, inSeq, lineVis, mathScripts, seatBudget))
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
        SpaceFont spaceFonts[kSpaceFonts];
        int32_t spaceFontCount = -1;  // SpaceFonts of the line, found at its first gap that needs them
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
        uint32_t sizeCount = 0, sizeBest = 0, sizeAt = 0;  // sizeAt: the bucket of the glyph before
        // The glyphs of bucket sizeAt not yet counted: a run of one bucket is counted at its end.
        // Within the run only its own count grows, so it leads at its end exactly when it would
        // have taken the lead at one of its glyphs.
        uint32_t run = 0;
        const auto countRun = [&] {
            if (run == 0 || sizeAt >= kLineSizes) return;
            sizes[sizeAt].count += run;
            if (sizes[sizeAt].count > sizes[sizeBest].count && sizeAt < kProtoSizes) sizeBest = sizeAt;
        };
        float pu0 = 0, pu1 = 0, vb = 0, vt = 0;
        uint16_t allFlags = kLineFlagGlyphs;
        // The line's first wide gap when no segment ends before it: the glyph after it (0: none)
        // and its width, tried for a note's mark before a tab once the line is read.
        uint32_t markAt = 0;
        float markGap = 0;
        // Leading spaces carry nothing; the first glyph opens the line's segment, extent and
        // size bucket, and each glyph after it is measured against the ones before.
        const uint32_t* at = lineBegin;
        while (at != lineEnd && (glyphs[*at].flags & kGlyphSpace)) ++at;
        const bool any = at != lineEnd;
        if (any) {
            const uint32_t i = *at;
            Glyph& g = glyphs[i];
            g.flags = static_cast<uint16_t>(g.flags & ~kGlyphSpaceBefore);
            seg = Segment{g.u0, g.u1, protoCount};
            right = g.u1;
            pu0 = g.u0, pu1 = g.u1;
            vb = g.vb, vt = g.vt;
            if (kMeasured) {
                allFlags &= g.flags;
                sizes[sizeCount++] = SizeCount{g.size, 0};
                run = 1;
            }
            previousSize = g.size;
            protoAt[protoCount++] = i;
            ++at;
        }
        for (; at != lineEnd; ++at) {
            const uint32_t i = *at;
            Glyph& g = glyphs[i];
            if (g.flags & kGlyphSpace) {
                pendingSpace = true;
                continue;
            }
            g.flags = static_cast<uint16_t>(g.flags & ~kGlyphSpaceBefore);
            const float em = M::Max(M::Min(previousSize, g.size), 0.5f * M::Max(previousSize, g.size));
            const float gap = g.u0 - right;
            // A gap of kWordGap at least may break words: most gaps inside words are narrower.
            bool wordBreak = pendingSpace;
            if (!wordBreak && gap > kWordGap * em) {
                wordBreak = gap > wordGap * em;
                if (!wordBreak && spaces) {
                    // The gaps beside it: before the glyph before (when no word break is there)
                    // and after this one (when no space follows).
                    const Glyph& before = glyphs[protoAt[protoCount - 1]];
                    const float previous = protoCount - 1 > p.first && !(before.flags & kGlyphSpaceBefore)
                                               ? before.u0 - beforePrevious
                                               : -1;
                    const float next =
                        at + 1 != lineEnd && !(glyphs[at[1]].flags & kGlyphSpace) ? glyphs[at[1]].u0 - g.u1 : -1;
                    wordBreak = DroppedSpace(page, before, g, gap, em, previous, next, lineBegin, lineEnd, spaceFonts,
                                             spaceFontCount);
                }
            }
            // A combining mark (two bytes at least) takes no space before it.
            if (wordBreak && !(g.textLength >= 2 && StartsWithCombiningMark(page.GlyphText(g))))
                g.flags |= kGlyphSpaceBefore;
            // Ink segments: runs of glyphs without a wide gap, for gutter detection. The gap is
            // measured in the size of the glyphs beside it: a drop cap must not widen the
            // threshold for the whole line.
            if (gap > kGutterCandidate * em) {
                if (segmentCount == p.segFirst) markAt = protoCount, markGap = gap;
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
            if (kMeasured) {
                allFlags &= g.flags;
                // A size equal to the glyph before's has its bucket: buckets are only added, and
                // the earlier ones did not match it then either.
                if (!(g.size == previousSize)) {
                    uint32_t d = 0;
                    while (d < sizeCount && !(std::fabs(sizes[d].size - g.size) < 0.25f)) ++d;
                    if (d != sizeAt) {
                        countRun();
                        run = 0;
                        sizeAt = d;
                    }
                    if (d == sizeCount && d < kLineSizes) sizes[sizeCount++] = SizeCount{g.size, 0};
                }
                ++run;
            }
            pendingSpace = false;
            previousSize = g.size;
            protoAt[protoCount++] = i;
        }
        if (any) segments[segmentCount++] = seg;
        if (kMeasured) countRun();
        // The first wide gap after a note's raised mark is a tab, no gutter.
        if (markAt != 0 && markAt - p.first <= 3 && markGap <= 2.5f * glyphs[protoAt[markAt]].size &&
            RaisedNoteMark(page, protoAt + p.first, markAt - p.first, glyphs[protoAt[markAt]]))
            p.markTab = true;
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
                // A note's mark before a tab stays with its text when no line around has ink left
                // of it, at the margin (a patent's line numbers stand in the gutter between two
                // columns). A line's segments run left to right.
                const bool markTab = s == 1 && l.markTab;
                bool inkLeft = false;
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
                        inkLeft = inkLeft || (markTab && n.segCount > 0 && segments[n.segFirst].u1 < l.u0);
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
                if (markTab && !inkLeft) continue;
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

void BuildLines(PageData& page, bool mathScripts) {
    page.lines.clear();
    page.lineGlyphs.clear();
    GlyphVector& glyphs = page.glyphs;

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
    const bool finite = CollectAndFold(page, vis, keepInvisible);
    ComposeAccents(page, vis, mathScripts, place.stray);
    if (finite)
        PlaceGlyphs<FiniteMinMax>(page, vis, mathScripts, place.stray);
    else
        PlaceGlyphs<CheckedMinMax>(page, vis, mathScripts, place.stray);
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
    if (!HasRightToLeftLead(page.text)) return;
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

// Splits the line at `position` of page.order before its glyph `k` (0 < k < count): the rest
// becomes a line of its own, next in the order.
void SplitAt(PageData& page, size_t position, uint32_t k) {
    Line& line = page.lines[page.order[position]];
    Line rest = line;
    rest.first = line.first + k;
    rest.count = line.count - k;
    line.count = k;
    page.glyphs[page.lineGlyphs[rest.first]].flags &= static_cast<uint16_t>(~kGlyphSpaceBefore);
    Measure(page, line);
    Measure(page, rest);
    page.order.insert(page.order.begin() + static_cast<std::ptrdiff_t>(position + 1),
                      static_cast<uint32_t>(page.lines.size()));
    page.lines.push_back(rest);
}

// Where the equation number at the end of `line` starts, or 0: "(", up to six digits, letters,
// dots and hyphens with a digit among them, ")" on the line's baseline, set apart from all before
// it by nearly half an em or more (TeX keeps a quad between a formula and its number, amsmath half
// of one) and by more than one and a half times any gap before it (the last word of a justified
// line is no number).
uint32_t EquationNumberAt(const PageData& page, const Line& line) {
    if (line.count < 5) return 0;
    const auto glyph = [&](uint32_t k) -> const Glyph& { return page.glyphs[page.lineGlyphs[line.first + k]]; };
    if (page.GlyphText(glyph(line.count - 1)) != ")") return 0;
    uint32_t open = line.count - 1;
    bool digit = false;
    while (open > 0 && line.count - open <= 7) {
        --open;
        const std::string_view t = page.GlyphText(glyph(open));
        if (t == "(") break;
        if (t.size() != 1) return 0;
        const char c = t[0];
        if (c >= '0' && c <= '9') digit = true;
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return 0;
    }
    if (open == 0 || !digit || page.GlyphText(glyph(open)) != "(") return 0;
    const Glyph& first = glyph(open);
    for (uint32_t k = open + 1; k < line.count; ++k)
        if (std::fabs(glyph(k).v - first.v) > 0.2f * line.size) return 0;
    float before = glyph(0).u1, widest = 0;
    bool relation = false;
    for (uint32_t k = 1; k < open; ++k) {
        const Glyph& g = glyph(k);
        widest = Fmax(widest, g.u0 - before);
        before = Fmax(before, g.u1);
        const std::string_view t = page.GlyphText(g);
        relation = relation || t == "=" || t == "\xE2\x89\xA4" || t == "\xE2\x89\xA5" || t == "\xE2\x88\x9D" ||
                   t == "\xE2\x89\x88" || t == "\xE2\x89\xA1";  // = ≤ ≥ ∝ ≈ ≡
    }
    const float gap = first.u0 - before;
    if (gap < 0.45f * line.size) return 0;
    // A display formula has wide spaces of its own (a quad between two equations, before "s.t."):
    // after a relation sign, a gap of an em or more and nearly as wide as the widest of them sets
    // the number apart too.
    return gap > 1.5f * widest || (relation && gap >= line.size && gap >= 0.8f * widest) ? open : 0;
}

} // namespace

void SplitRunInHeadings(PageData& page) {
    const size_t ordered = page.order.size();
    for (size_t at = 0; at < ordered && page.order.size() < (size_t(1) << 24); ++at) {
        // Indices into page.order shift as rests are inserted: `at` walks the original entries.
        const size_t position = at + (page.order.size() - ordered);
        const uint32_t li = page.order[position];
        const Line& line = page.lines[li];
        if (line.dir != 0 || line.count < 3 || (line.flags & (kLineAnchor | kLineTableCell | kLineTable))) continue;
        // An equation's number at the end of its formula's line is a line of its own, as it is
        // where the formula leaves room for it.
        if (!(line.flags & kLineRtl)) {
            if (const uint32_t number = EquationNumberAt(page, line)) {
                SplitAt(page, position, number);
                continue;
            }
        }
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
        SplitAt(page, position, k);
    }
}

} // namespace RayoPdf::Layout
