// SPDX-License-Identifier: Apache-2.0
#include "layout/interpreter.h"

#include "common/minmax.h"
#include "common/utf8.h"
#include "layout/glyph_box.h"
#include "layout/math.h"
#include "layout/path_box.h"
#include "pdf/content_lexer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_set>

namespace RayoPdf::Layout {
namespace {

// The most glyphs (and text bytes) reserved for a page from its content's length (Run).
constexpr size_t kGlyphReserveMax = size_t(1) << 16;

// An affine matrix in PDF's row-vector convention: [x y 1] × M (ISO 32000-1 §8.3.4).
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// `m` then `n`.
Matrix Multiply(const Matrix& m, const Matrix& n) {
    Matrix r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

bool Finite(const Matrix& m) {
    return std::isfinite(m.a) && std::isfinite(m.b) && std::isfinite(m.c) && std::isfinite(m.d) &&
           std::isfinite(m.e) && std::isfinite(m.f);
}

struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Empty() const { return !(x1 > x0) || !(y1 > y0); }
};

Box Intersect(const Box& a, const Box& b) {
    Box r;
    r.x0 = Fmax(a.x0, b.x0);
    r.y0 = Fmax(a.y0, b.y0);
    r.x1 = Fmin(a.x1, b.x1);
    r.y1 = Fmin(a.y1, b.y1);
    return r;
}

// An image dimension in pixels from /Width or /Height: 0 when missing or out of range.
uint32_t Dimension(int64_t value) {
    return value > 0 && value <= INT32_MAX ? static_cast<uint32_t>(value) : 0;
}

// The device box of the unit square under `m` (images, §8.9.4).
Box UnitSquare(const Matrix& m) {
    const double xs[4] = {m.e, m.a + m.e, m.c + m.e, m.a + m.c + m.e};
    const double ys[4] = {m.f, m.b + m.f, m.d + m.f, m.b + m.d + m.f};
    Box r{xs[0], ys[0], xs[0], ys[0]};
    for (int i = 1; i < 4; ++i) {
        r.x0 = Fmin(r.x0, xs[i]);
        r.x1 = Fmax(r.x1, xs[i]);
        r.y0 = Fmin(r.y0, ys[i]);
        r.y1 = Fmax(r.y1, ys[i]);
    }
    return r;
}

// (x, y) on the upright page into the frame of baseline direction `dir`.
inline void ToFrame(int dir, double x, double y, double& u, double& v) {
    switch (dir) {
    case 1: u = y, v = -x; break;
    case 2: u = -x, v = -y; break;
    case 3: u = -y, v = x; break;
    default: u = x, v = y; break;
    }
}

struct GState {
    Matrix ctm;
    Box clip;
    double charSpace = 0;
    double wordSpace = 0;
    double hScale = 1;
    double leading = 0;
    double fontSize = 0;
    double rise = 0;
    double lineWidth = 1;
    int render = 0;
    const Text::Font* font = nullptr;
    uint16_t fontId = 0;
    uint8_t fontMath = 0;  // PageData::fontMath class of the font (layout/math.h), 0 for text fonts
    bool transparent = false;  // fill alpha 0 (/ca 0)
};

// Execute's operators first, then those RunStream runs itself: each switch's jump table starts at 0.
enum class Op : uint8_t {
    Unknown, BT, ET, Tc, Tw, Tz, TL, Tf, Tr, Ts, Td, TD, Tm, TStar, Tj, TJ, Quote, DoubleQuote, Do, BI, w, gs, BDC,
    BMC, EMC, q, Q, cm, re, m, l, c, v, y, h, S, s, f, F, fStar, B, BStar, b, bStar, n, W, WStar,
};

// The one-byte operators by their byte, and the two-byte ones starting with 'T' by their second.
struct OpTables {
    Op one[256] = {};
    Op text[256] = {};
    constexpr OpTables() {
        one['q'] = Op::q, one['Q'] = Op::Q, one['\''] = Op::Quote, one['"'] = Op::DoubleQuote;
        one['m'] = Op::m, one['l'] = Op::l, one['c'] = Op::c, one['v'] = Op::v, one['y'] = Op::y;
        one['h'] = Op::h, one['S'] = Op::S, one['s'] = Op::s, one['f'] = Op::f, one['F'] = Op::F;
        one['B'] = Op::B, one['b'] = Op::b, one['n'] = Op::n, one['W'] = Op::W, one['w'] = Op::w;
        text['c'] = Op::Tc, text['w'] = Op::Tw, text['z'] = Op::Tz, text['L'] = Op::TL, text['f'] = Op::Tf;
        text['r'] = Op::Tr, text['s'] = Op::Ts, text['d'] = Op::Td, text['D'] = Op::TD, text['m'] = Op::Tm;
        text['*'] = Op::TStar, text['j'] = Op::Tj, text['J'] = Op::TJ;
    }
};
constexpr OpTables kOpTables;

// The bytes of a two- or three-byte operator in one integer.
constexpr uint32_t OpKey(char a, char b, char c = 0) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
           static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16;
}

Op OpOf(std::string_view op) {
    switch (op.size()) {
    case 1:
        return kOpTables.one[static_cast<unsigned char>(op[0])];
    case 2:
        if (op[0] == 'T') return kOpTables.text[static_cast<unsigned char>(op[1])];
        switch (OpKey(op[0], op[1])) {
        case OpKey('c', 'm'): return Op::cm;
        case OpKey('B', 'T'): return Op::BT;
        case OpKey('E', 'T'): return Op::ET;
        case OpKey('D', 'o'): return Op::Do;
        case OpKey('B', 'I'): return Op::BI;
        case OpKey('r', 'e'): return Op::re;
        case OpKey('f', '*'): return Op::fStar;
        case OpKey('B', '*'): return Op::BStar;
        case OpKey('b', '*'): return Op::bStar;
        case OpKey('W', '*'): return Op::WStar;
        case OpKey('g', 's'): return Op::gs;
        default: return Op::Unknown;
        }
    case 3:
        switch (OpKey(op[0], op[1], op[2])) {
        case OpKey('B', 'D', 'C'): return Op::BDC;
        case OpKey('B', 'M', 'C'): return Op::BMC;
        case OpKey('E', 'M', 'C'): return Op::EMC;
        default: return Op::Unknown;
        }
    default:
        return Op::Unknown;
    }
}

// PDFDocEncoding bytes 0x18-0x1F and 0x80-0xA0 (ISO 32000-1 Annex D.2); the rest are Latin-1.
const uint16_t kPdfDocLow[8] = {0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC};
const uint16_t kPdfDocHigh[33] = {
    0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203A, 0x2212,
    0x2030, 0x201E, 0x201C, 0x201D, 0x2018, 0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141,
    0x0152, 0x0160, 0x0178, 0x017D, 0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0xFFFD, 0x20AC,
};

// One marked-content sequence of the stack.
struct MarkedContent {
    uint32_t firstGlyph = 0;
    uint32_t actualText = 0;     // offset in the page text
    uint32_t actualLength = 0;
    bool hasActualText = false;
    bool artifact = false;
    int32_t previousMcid = -1;
    uint8_t previousStream = 0;
};

// What every glyph of a shown string shares: the font's extents (defaults when it has none),
// writing mode and style, and what the text rendering matrix's linear part gives, which stays the
// same along the string (only its translation moves): the glyphs' size, baseline direction and
// state flags, the terms of the box corners that do not depend on the glyph, and the clip test's
// bounds. A run is made from its font, Trm's linear part, the render mode, alpha, artifact state
// and clip; the next string reuses it when all of these have the same bits (the strings of a TJ
// array, a line's Tj after Td).
struct GlyphRun {
    const Text::Font* font = nullptr;
    double a = 0, b = 0, c = 0, d = 0;  // Trm without its translation
    int render = 0;
    bool transparent = false, artifact = false;
    Box clip;

    double ascent = 0, descent = 0;
    bool vertical = false;
    bool usable = false;                 // size and run length finite
    uint8_t dir = 0;
    uint16_t flags = 0;                  // style, render mode, shear and artifact flags
    float size = 0;
    // x·a and x·b of the box's left and right edges, y·c and y·d of its bottom and top: the
    // glyph's own edge (its width, or its vertical advance) is filled in per glyph.
    double xa[2] = {}, xb[2] = {}, yc[2] = {}, yd[2] = {};
    // The clip widened by one unit, and whether it is empty.
    double clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
    bool clipEmpty = false;
    // All eight terms above are finite: with a finite glyph edge and a finite origin no corner
    // sum meets two opposite infinities, so no corner is NaN (PlaceGlyphs skips its NaN test).
    bool termsFinite = false;
    // HorizontalBox applies (horizontal, all terms finite), and its terms.
    bool boxTerms = false;
    BoxTerms box = {};
};

// The glyph flags a code's Unicode source implies, by Text::UnicodeSource.
constexpr uint16_t kSourceFlags[] = {0, 0, 0, 0, kGlyphGuessed, kGlyphUnmapped};
static_assert(static_cast<int>(Text::UnicodeSource::Guess) == 4 && static_cast<int>(Text::UnicodeSource::None) == 5,
              "kSourceFlags follows Text::UnicodeSource");

// The same value bit for bit (a -0 is no 0).
inline bool SameBits(double x, double y) {
    uint64_t u, v;
    std::memcpy(&u, &x, sizeof u);
    std::memcpy(&v, &y, sizeof v);
    return u == v;
}

// The slots of a font that has none (not simple): no code is in a pool, so each takes Next.
struct NoSlots {
    Text::FontSlot of[256];
    constexpr NoSlots() : of() {
        for (Text::FontSlot& slot : of) slot.state = Text::FontSlotState::Pending;
    }
};
constexpr NoSlots kNoSlots;

// The glyph counts of ShowText's common case, packed in one word: shown codes in bits 0-20,
// unmapped ones (UnicodeSource::None) in bits 21-41, guessed ones (Guess) in bits 42-62, added
// per code by its source. A cursor moves them into its separate counts after a string that
// leaves kCountFlush shown or more: a string has at most kMaxGlyphsPerPage codes, so no field
// passes 21 bits.
constexpr int kCountBits = 21;
constexpr uint64_t kCountMask = (uint64_t(1) << kCountBits) - 1;
constexpr uint64_t kCountFlush = uint64_t(1) << 20;
static_assert(kCountFlush + kMaxGlyphsPerPage <= kCountMask, "packed glyph counts fit 21 bits");
constexpr uint64_t kSourceCounts[] = {1, 1, 1, 1, 1 | uint64_t(1) << (2 * kCountBits), 1 | uint64_t(1) << kCountBits};

// Whether a TJ adjustment, Multiply([1 0 0 1 tx ty], Tm), leaves Tm's linear part with its bits:
// each term plus a zero product keeps its bits unless it is -0 or the product is not 0.
inline bool KeepsLinear(double ta, double tb, double tc, double td) {
    return SameBits(ta + 0.0 * tc, ta) && SameBits(tb + 0.0 * td, tb) && SameBits(0.0 * ta + tc, tc) &&
           SameBits(0.0 * tb + td, td);
}

// What placing a text operator's glyphs needs (Interpreter::ShowText), in one local: the
// graphics state's text parameters, Tm × CTM's linear part and what the glyph loop reads of the
// run made for it. Only `ready` is initialised: Read sets the rest.
struct Placement {
    bool ready = false;
    const GlyphRun* run;          // the rest of the run (run_, unchanged while ready)
    double ta, tb, tc, td;        // the text matrix's linear part it was read for
    double tma, tmb, tmc, tmd;    // Tm × CTM's linear part
    double ctmA, ctmB, ctmC, ctmD, ctmE, ctmF;
    const Text::Font* font;
    const Text::FontSlot* slots;  // a simple font's slots (Font::SimpleSlots), else kNoSlots.of
    const char* pool;
    double tfs, th, charSpace, wordSpace;
    double riseC, riseD;          // Trise × (Tm × CTM)'s c and d: Trm's translation less Tm × CTM's
    bool vertical, usable;
    bool boxTerms;                // HorizontalBox applies: horizontal, all run terms finite
    uint16_t runFlags;
    const BoxTerms* box;          // the run's
    // What every glyph record of the string has besides its box, text and flags: size,
    // direction, page-local font and marked content.
    float size;
    uint8_t dir, mcStream;
    uint16_t fontId;
    int32_t mcid;
    uint8_t fontMath;
    std::vector<GlyphUse>* uses;
    // KeepsLinear of the text matrix's linear part, found at the first adjustment: -1 until then.
    int8_t keepsLinear;

    void Read(const GState& gs, const Matrix& textMatrix, const Matrix& tm, const GlyphRun& r) {
        ready = true;
        run = &r;
        ta = textMatrix.a, tb = textMatrix.b, tc = textMatrix.c, td = textMatrix.d;
        keepsLinear = -1;
        tma = tm.a, tmb = tm.b, tmc = tm.c, tmd = tm.d;
        ctmA = gs.ctm.a, ctmB = gs.ctm.b, ctmC = gs.ctm.c, ctmD = gs.ctm.d, ctmE = gs.ctm.e, ctmF = gs.ctm.f;
        font = gs.font;
        slots = gs.font->SimpleSlots() != nullptr ? gs.font->SimpleSlots() : kNoSlots.of;
        pool = gs.font->SimplePool();
        tfs = gs.fontSize, th = gs.hScale, charSpace = gs.charSpace, wordSpace = gs.wordSpace;
        riseC = gs.rise * tm.c, riseD = gs.rise * tm.d;
        vertical = r.vertical, usable = r.usable;
        boxTerms = r.boxTerms;
        runFlags = r.flags;
        box = &r.box;
        size = r.size;
        dir = r.dir;
        fontId = gs.fontId;
        fontMath = gs.fontMath;
    }
};

// Where glyph placement is along the strings of a text operator: the text matrix's translation
// and Tm × CTM's (which advances alike), the glyph counts for PageStats (packed in `counts` by
// the common case, see kSourceCounts) and the records written.
struct GlyphCursor {
    double te, tf;
    double tme, tmf;
    uint64_t counts = 0;
    uint64_t shown = 0, unmapped = 0, guessed = 0;
    size_t placed = 0;
};

// Moves a cursor's packed counts into its separate ones.
inline void FlushCounts(GlyphCursor& cur) {
    cur.shown += cur.counts & kCountMask;
    cur.unmapped += (cur.counts >> kCountBits) & kCountMask;
    cur.guessed += cur.counts >> (2 * kCountBits);
    cur.counts = 0;
}

// A glyph's box outside HorizontalBox's case (a vertical run, a term or the width not finite): the
// corners [x y] × Trm with the glyph's own edge, its width (the right edge) or its vertical advance
// (the bottom edge); their extremes by Fmin and Fmax where a corner is NaN. Returns whether the box
// is clipped away.
RAYOPDF_NOINLINE bool GeneralGlyphBox(const Placement& p, double width, double verticalAdvance, double e, double f,
                                      Glyph& g) {
    const GlyphRun& run = *p.run;
    double xa1 = run.xa[1], xb1 = run.xb[1], yc0 = run.yc[0], yd0 = run.yd[0];
    bool finite = run.termsFinite;
    if (p.vertical) {
        const double y0 = verticalAdvance < 0 ? verticalAdvance : -1;
        yc0 = y0 * run.c, yd0 = y0 * run.d;
        finite = finite && std::isfinite(y0);
    } else {
        xa1 = width * run.a, xb1 = width * run.b;
        finite = finite && std::isfinite(width);
    }
    const double xa0 = run.xa[0], xb0 = run.xb[0], yc1 = run.yc[1], yd1 = run.yd[1];
    const double dx[4] = {xa0 + yc0 + e, xa1 + yc0 + e, xa0 + yc1 + e, xa1 + yc1 + e};
    const double dy[4] = {xb0 + yd0 + f, xb1 + yd0 + f, xb0 + yd1 + f, xb1 + yd1 + f};
    double bx0, bx1, by0, by1;
    // Without NaN, Fmin and Fmax are plain comparisons.
    if (finite || (dx[0] == dx[0] && dx[1] == dx[1] && dx[2] == dx[2] && dx[3] == dx[3] && dy[0] == dy[0] &&
                   dy[1] == dy[1] && dy[2] == dy[2] && dy[3] == dy[3])) {
        BoxExtremes(dx[0], dx[1], dx[2], dx[3], bx0, bx1);
        BoxExtremes(dy[0], dy[1], dy[2], dy[3], by0, by1);
        WriteBox(bx0, by0, bx1, by1, p.dir, g);
    } else {
        double fu0 = 0, fu1 = 0, fv0 = 0, fv1 = 0;
        bx0 = bx1 = dx[0], by0 = by1 = dy[0];
        for (int i = 0; i < 4; ++i) {
            bx0 = Fmin(bx0, dx[i]), bx1 = Fmax(bx1, dx[i]);
            by0 = Fmin(by0, dy[i]), by1 = Fmax(by1, dy[i]);
            double u, v;
            ToFrame(p.dir, dx[i], dy[i], u, v);
            if (i == 0) fu0 = fu1 = u, fv0 = fv1 = v;
            fu0 = Fmin(fu0, u), fu1 = Fmax(fu1, u);
            fv0 = Fmin(fv0, v), fv1 = Fmax(fv1, v);
        }
        g.u0 = float(fu0), g.u1 = float(fu1), g.vb = float(fv0), g.vt = float(fv1);
    }
    // Clipped away: the glyph's centre is outside the clip (which lies within the page).
    const double mx = (bx0 + bx1) / 2, my = (by0 + by1) / 2;
    return mx < run.clipX0 || mx > run.clipX1 || my < run.clipY0 || my > run.clipY1 || run.clipEmpty;
}

class Interpreter {
public:
    Interpreter(const Document& document, Text::FontCache& fonts, PageData& page, PageStats& stats)
        : doc_(document), fonts_(fonts), page_(page), stats_(stats) {}

    void Run(int index, bool withLinks);

private:
    void RunStream(std::string_view content, const PdfValue& resources, int depth);
    // Out of line: RunStream's loop around it keeps its registers.
    RAYOPDF_NOINLINE bool Execute(Op op, const ContentLexer& lexer, const PdfValue& resources, int depth);
    bool ShowString(std::string_view bytes);
    bool ShowArray(const PdfValue& array);
    bool ShowText(const PdfValue* items, size_t count, std::string_view single);
    bool ShowItems(const PdfValue* items, size_t count, std::string_view single, size_t& placed);
    // kFast: ShowText's common case, where the tests PlaceGlyphs drops cannot change anything.
    template <bool kFast>
    bool PlaceGlyphs(std::string_view bytes, const Placement& p, GlyphCursor& cur);
    void EndCursor(const GlyphCursor& cur);
    bool PlaceGlyphsGeneral(std::string_view bytes, const Placement& p, double tme, double tmf);
    const GlyphRun& RunFor(const Text::Font& font, const Matrix& tm);
    bool SetFont(const PdfValue& resources, std::string_view name, double size);
    void SetFontEntry(const PdfValue& entry, double size);
    void ApplyExtGState(const PdfValue& resources, std::string_view name);
    void DoXObject(const PdfValue& resources, std::string_view name, int depth);
    const PdfValue* FindXObject(const PdfValue& xobjects, std::string_view name);
    void BeginMarked(std::string_view tag, const PdfValue* properties, const PdfValue& resources);
    void EndMarked();
    uint8_t MarkStream();
    void Paint(bool fill, bool stroke);
    template <int kCount>
    RAYOPDF_HOT_INLINE void PathPoints(const double* xy, bool curve);
    void AddPiece(double x0, double y0, double x1, double y1, bool rect);
    RAYOPDF_NOINLINE void InlineImage(const PdfValue& dictionary, const PdfValue& resources, std::string_view data);
    void PaintedSample(const PdfValue& dictionary, std::string_view data);
    void ReadLinks(int index);
    void NextLine(double tx, double ty) {
        Matrix t;
        t.e = tx;
        t.f = ty;
        lineMatrix_ = Multiply(t, lineMatrix_);
        textMatrix_ = lineMatrix_;
    }

    const Document& doc_;
    Text::FontCache& fonts_;
    PageData& page_;
    PageStats& stats_;

    GState gs_;
    std::vector<GState> stack_;
    Matrix textMatrix_;
    Matrix lineMatrix_;
    Box pageBox_;
    uint32_t operators_ = 0;
    bool budgetHit_ = false;
    Matrix pageMatrix_;  // default user space to the upright page

    std::vector<const Text::Font*> fontIds_;
    // Tf's last fonts by resource dictionary and name: the dictionary by its entries (arena storage
    // stays put and unchanged while the page is read), the font and its id as SetFont found them.
    // Kept only where no lookup on the way could count a limit hit (no reference chains).
    struct FontMemo {
        const void* entries = nullptr;
        uint32_t count = 0;
        uint8_t nameSize = 0;
        char name[19] = {};
        const Text::Font* font = nullptr;
        uint16_t id = 0;
    };
    static constexpr size_t kFontMemos = 16;
    FontMemo fontMemo_[kFontMemos];
    bool OneHop(const PdfValue* value) const { return value && (!value->IsRef() || !doc_.Object(value->refNum).IsRef()); }
    // XObject dictionaries of kIndexedDictionary entries or more looked up more than kIndexAfter
    // times on the page: their keys sorted with their entry index (FindXObject).
    struct NameIndex {
        const void* entries = nullptr;
        uint32_t count = 0;
        uint32_t lookups = 0;
        std::vector<std::pair<std::string_view, uint32_t>> sorted;
    };
    static constexpr uint32_t kIndexedDictionary = 64, kIndexAfter = 16;
    static constexpr size_t kNameIndexes = 4;
    NameIndex nameIndexes_[kNameIndexes];
    size_t nextNameIndex_ = 0;
    std::vector<MarkedContent> marked_;
    uint32_t markedOverflow_ = 0;
    int artifactDepth_ = 0;
    int32_t mcid_ = -1;
    uint8_t mcStream_ = 0;  // Glyph::mcStream of mcid_
    GlyphRun run_;          // the last shown string's

    // Current path, in device space.
    Box pathBox_;
    bool pathEmpty_ = true;
    bool pathCurved_ = false;
    uint16_t pathSegments_ = 0;
    uint16_t pathSubpaths_ = 0;
    bool pathIsRect_ = false;
    double startX_ = 0, startY_ = 0;  // current subpath start (user space)
    double curX_ = 0, curY_ = 0;      // current point (user space)
    int pendingClip_ = 0;             // W or W* seen: the path clips at the next painting operator
    // Straight axis-aligned pieces of the current path in device space (table rulings, §6.7).
    struct PathPiece {
        double x0, y0, x1, y1;
        bool rect;  // a whole `re` rectangle
    };
    std::vector<PathPiece> pieces_;

    std::vector<uint32_t> formStack_;          // object numbers of the forms being run
    // Decoded form content, one per depth. A deque: a deeper form's buffer added while an outer
    // form's lexer reads its own must not move it (a short one lives inside the string object).
    std::deque<std::string> formBuffers_;
    std::string_view opSource_;                // the source of the operator being executed
};

void Interpreter::Run(int index, bool withLinks) {
    const PdfRect crop = doc_.CropBox(index);
    const int rotate = doc_.Rotate(index);
    const double w = crop.Width(), h = crop.Height();
    // Device space is the upright page: the CropBox origin moved to (0, 0), then turned clockwise
    // by /Rotate as a viewer shows it (§7.7.3.3).
    Matrix base;
    base.e = -crop.x0;
    base.f = -crop.y0;
    Matrix turn;
    switch (rotate) {
    case 90: turn = Matrix{0, -1, 1, 0, 0, w}; break;
    case 180: turn = Matrix{-1, 0, 0, -1, w, h}; break;
    case 270: turn = Matrix{0, 1, -1, 0, h, 0}; break;
    default: break;
    }
    base = Multiply(base, turn);
    pageMatrix_ = base;
    const bool sideways = rotate == 90 || rotate == 270;
    page_.width = static_cast<float>(sideways ? h : w);
    page_.height = static_cast<float>(sideways ? w : h);
    pageBox_ = Box{0, 0, page_.width, page_.height};
    gs_ = GState();
    gs_.ctm = base;
    gs_.clip = pageBox_;

    std::string_view content;
    const StreamStatus status = doc_.PageContent(index, content);
    if (status == StreamStatus::Corrupt || status == StreamStatus::Salvaged || status == StreamStatus::TooLarge)
        page_.contentErrors++;
    if (stats_.contentRead) stats_.contentRead(stats_.contentContext, content.size());
    // The glyph vector and the page text sized once from the content, not grown by doubling:
    // each doubling copies every glyph so far into new memory, which a fresh process first-touches
    // a page at a time (111 page faults of odl 192's 4,277 glyphs). A shown code takes at least
    // one content byte and a glyph about 7-11 (median over odl, olmOCR and SafeDocs; 1.6-3.3 at
    // the 1st percentile): half a glyph a byte covers nearly every page, and memory reserved but
    // never written costs no page. Glyphs of forms grow it as before.
    const size_t expected = content.size() / 2 < kGlyphReserveMax ? content.size() / 2 : kGlyphReserveMax;
    page_.glyphs.reserve(expected);
    page_.text.reserve(expected);
    if (!content.empty()) RunStream(content, doc_.Resources(index), 0);
    // Marked content left open at the end still applies its ActualText.
    while (!marked_.empty()) EndMarked();
    if (withLinks) ReadLinks(index);
    page_.limitHit = budgetHit_;
}

void Interpreter::RunStream(std::string_view content, const PdfValue& resources, int depth) {
    ContentLexer lexer(content);
    const size_t stackBase = stack_.size();
    bool error = false;
    size_t next = lexer.Offset();  // where the operator being read starts
    lexer.Run([&](std::string_view op) RAYOPDF_HOT_LAMBDA {
        // The lexer's offsets only grow and stay within the content: no substr checks.
        const size_t start = next;
        next = lexer.Offset();
        if (RAYOPDF_UNLIKELY(++operators_ > kMaxOperatorsPerPage)) {
            budgetHit_ = true;
            return false;
        }
        // The path, painting and graphics-state operators, most of a drawing's, here; the others
        // by Execute.
        const Op code = OpOf(op);
        const double* n;
        switch (code) {
        case Op::Unknown:
            return true;
        case Op::m:
            if (!(n = lexer.Numbers(2))) break;
            startX_ = curX_ = n[0];
            startY_ = curY_ = n[1];
            pathSubpaths_++;
            if (!pathEmpty_) pathIsRect_ = false;
            PathPoints<1>(n, false);
            return true;
        case Op::l:
            if (!(n = lexer.Numbers(2)) || pathEmpty_) break;
            AddPiece(curX_, curY_, n[0], n[1], false);
            curX_ = n[0];
            curY_ = n[1];
            PathPoints<1>(n, false);
            if (pathSegments_ < 60000) pathSegments_++;
            return true;
        case Op::c:
            if (!(n = lexer.Numbers(6)) || pathEmpty_) break;
            PathPoints<3>(n, true);
            curX_ = n[4];
            curY_ = n[5];
            if (pathSegments_ < 60000) pathSegments_++;
            return true;
        case Op::v:
        case Op::y:
            if (!(n = lexer.Numbers(4)) || pathEmpty_) break;
            PathPoints<2>(n, true);
            curX_ = n[2];
            curY_ = n[3];
            if (pathSegments_ < 60000) pathSegments_++;
            return true;
        case Op::h:
            if (!pathEmpty_ && (curX_ != startX_ || curY_ != startY_)) AddPiece(curX_, curY_, startX_, startY_, false);
            curX_ = startX_;
            curY_ = startY_;
            return true;
        case Op::re: {
            if (!(n = lexer.Numbers(4))) break;
            const bool wasEmpty = pathEmpty_;
            const double corners[8] = {n[0], n[1], n[0] + n[2], n[1] + n[3], n[0] + n[2], n[1], n[0], n[1] + n[3]};
            PathPoints<4>(corners, false);
            // Four segments of one subpath; an axis-aligned rectangle when the CTM keeps axes.
            pathSegments_ = static_cast<uint16_t>(pathSegments_ + 4 < 60000 ? pathSegments_ + 4 : 60000);
            pathSubpaths_++;
            const bool axisAligned = (std::fabs(gs_.ctm.b) < 1e-9 && std::fabs(gs_.ctm.c) < 1e-9) ||
                                     (std::fabs(gs_.ctm.a) < 1e-9 && std::fabs(gs_.ctm.d) < 1e-9);
            pathIsRect_ = wasEmpty && axisAligned;
            if (axisAligned) AddPiece(n[0], n[1], n[0] + n[2], n[1] + n[3], true);
            curX_ = startX_ = n[0];
            curY_ = startY_ = n[1];
            return true;
        }
        case Op::q:
            if (stack_.size() >= static_cast<size_t>(kMaxStateDepth)) break;
            stack_.push_back(gs_);
            return true;
        case Op::Q:
            if (stack_.empty()) return true;
            gs_ = stack_.back();
            stack_.pop_back();
            return true;
        case Op::cm: {
            if (!(n = lexer.Numbers(6))) break;
            const Matrix m = Multiply(Matrix{n[0], n[1], n[2], n[3], n[4], n[5]}, gs_.ctm);
            if (!Finite(m)) break;
            gs_.ctm = m;
            return true;
        }
        case Op::S:
        case Op::s:
            Paint(false, true);
            return true;
        case Op::f:
        case Op::F:
        case Op::fStar:
            Paint(true, false);
            return true;
        case Op::B:
        case Op::BStar:
        case Op::b:
        case Op::bStar:
            Paint(true, true);
            return true;
        case Op::n:
            Paint(false, false);
            return true;
        case Op::W:
        case Op::WStar:
            pendingClip_ = 1;
            return true;
        default:
            if (code == Op::BI) opSource_ = std::string_view(content.data() + start, next - start);
            if (!Execute(code, lexer, resources, depth)) error = true;
            return !budgetHit_;
        }
        error = true;  // an operator without its operands, with no current point, or failing
        return true;
    });
    // A stream restores the state stack it started with (forms run inside q/Q, §8.10.1).
    while (stack_.size() > stackBase) {
        gs_ = stack_.back();
        stack_.pop_back();
    }
    if (error) page_.contentErrors++;
}

// The last `n` operands as doubles; false when there are fewer than `n` or one is not a finite
// number.
bool Numbers(const ContentLexer& lexer, size_t n, const double*& out) {
    out = lexer.Numbers(n);
    return out != nullptr;
}

bool Interpreter::Execute(Op op, const ContentLexer& lexer, const PdfValue& resources, int depth) {
    const PdfSpan<PdfValue> operands = lexer.Operands();
    const double* n = nullptr;
    switch (op) {
    case Op::BT:
        textMatrix_ = lineMatrix_ = Matrix();
        return true;
    case Op::ET:
        return true;
    case Op::Tc:
        if (!Numbers(lexer, 1, n)) return false;
        gs_.charSpace = n[0];
        return true;
    case Op::Tw:
        if (!Numbers(lexer, 1, n)) return false;
        gs_.wordSpace = n[0];
        return true;
    case Op::Tz:
        if (!Numbers(lexer, 1, n)) return false;
        gs_.hScale = n[0] / 100;
        return true;
    case Op::TL:
        if (!Numbers(lexer, 1, n)) return false;
        gs_.leading = n[0];
        return true;
    case Op::Tf:
        if (operands.size() < 2 || !operands[operands.size() - 2].IsName() || !operands.back().IsNumber())
            return false;
        return SetFont(resources, operands[operands.size() - 2].Name(), operands.back().Number());
    case Op::Tr:
        if (!Numbers(lexer, 1, n)) return false;
        // Modes are 0-7; anything else (out of int range included) is malformed.
        if (!(n[0] >= 0 && n[0] <= 7)) return false;
        gs_.render = static_cast<int>(n[0]);
        return true;
    case Op::Ts:
        if (!Numbers(lexer, 1, n)) return false;
        gs_.rise = n[0];
        return true;
    case Op::Td:
        if (!Numbers(lexer, 2, n)) return false;
        NextLine(n[0], n[1]);
        return true;
    case Op::TD:
        if (!Numbers(lexer, 2, n)) return false;
        gs_.leading = -n[1];
        NextLine(n[0], n[1]);
        return true;
    case Op::Tm: {
        if (!Numbers(lexer, 6, n)) return false;
        const Matrix m{n[0], n[1], n[2], n[3], n[4], n[5]};
        textMatrix_ = lineMatrix_ = m;
        return true;
    }
    case Op::TStar:
        NextLine(0, -gs_.leading);
        return true;
    case Op::Tj:
        if (operands.empty() || !operands.back().IsString()) return false;
        return ShowString(operands.back().String());
    case Op::TJ:
        if (operands.empty() || !operands.back().IsArray()) return false;
        return ShowArray(operands.back());
    case Op::Quote:
        NextLine(0, -gs_.leading);
        if (operands.empty() || !operands.back().IsString()) return false;
        return ShowString(operands.back().String());
    case Op::DoubleQuote: {
        const size_t count = operands.size();
        if (count < 3 || !operands[count - 1].IsString() || !operands[count - 3].IsNumber() ||
            !operands[count - 2].IsNumber())
            return false;
        gs_.wordSpace = operands[count - 3].Number();
        gs_.charSpace = operands[count - 2].Number();
        NextLine(0, -gs_.leading);
        return ShowString(operands.back().String());
    }
    case Op::Do:
        if (operands.empty() || !operands.back().IsName()) return false;
        DoXObject(resources, operands.back().Name(), depth);
        return true;
    case Op::BI:
        if (!operands.empty()) InlineImage(operands.back(), resources, lexer.InlineImageData());
        return true;
    case Op::w:
        if (!Numbers(lexer, 1, n)) return false;
        gs_.lineWidth = n[0];
        return true;
    case Op::gs:
        if (operands.empty() || !operands.back().IsName()) return false;
        ApplyExtGState(resources, operands.back().Name());
        return true;
    case Op::BDC:
        if (operands.size() < 2 || !operands[operands.size() - 2].IsName()) {
            BeginMarked({}, nullptr, resources);
            return false;
        }
        BeginMarked(operands[operands.size() - 2].Name(), &operands.back(), resources);
        return true;
    case Op::BMC:
        BeginMarked(operands.empty() ? std::string_view() : operands.back().Name(), nullptr, resources);
        return true;
    case Op::EMC:
        EndMarked();
        return true;
    default:  // Op::Unknown and the operators RunStream runs itself
        return true;
    }
}

// A stencil mask of one sample that paints (BI /IM true /W 1 /H 1, its sample 0 unless /D [1 0]) is
// its unit square filled with the fill colour: dvips with Ghostscript writes each rule of TeX so
// (the rules of a table, a fraction bar). Axis-aligned on the page, it is recorded as the filled
// rectangle it paints: a shape and a ruling, as `re f` leaves them.
void Interpreter::PaintedSample(const PdfValue& dictionary, std::string_view data) {
    const auto entry = [&](const char* brief, const char* full) {
        const PdfValue* v = dictionary.Find(brief);
        return v ? v : dictionary.Find(full);
    };
    const PdfValue* mask = entry("IM", "ImageMask");
    const PdfValue* w = entry("W", "Width");
    const PdfValue* h = entry("H", "Height");
    if (!mask || !mask->Bool() || !w || !h || w->Int() != 1 || h->Int() != 1 || data.empty()) return;
    if (entry("F", "Filter")) return;  // encoded data: an image like any other
    bool inverted = false;
    if (const PdfValue* decode = entry("D", "Decode")) {
        double first = 0;
        inverted = decode->IsArray() && !decode->Array().empty() && doc_.Number(decode->Array()[0], first) && first == 1;
    }
    const bool sample = (static_cast<unsigned char>(data[0]) & 0x80) != 0;
    if (sample != inverted) return;  // the sample is masked out: nothing is painted
    const Matrix& m = gs_.ctm;
    const bool axisAligned = (std::fabs(m.b) < 1e-9 && std::fabs(m.c) < 1e-9) || (std::fabs(m.a) < 1e-9 && std::fabs(m.d) < 1e-9);
    if (!axisAligned) return;
    const Box square = UnitSquare(m);
    if (!std::isfinite(square.x0) || !std::isfinite(square.y0) || !std::isfinite(square.x1) || !std::isfinite(square.y1)) return;
    const Box visible = Intersect(square, gs_.clip);
    if (!(visible.x1 >= visible.x0 && visible.y1 >= visible.y0)) return;
    if (page_.shapes.size() < kMaxShapesPerPage) {
        Shape s;
        s.x0 = float(square.x0), s.y0 = float(square.y0), s.x1 = float(square.x1), s.y1 = float(square.y1);
        s.filled = true;
        s.segments = 4;
        s.kind = ShapeKind::Rect;
        page_.shapes.push_back(s);
    }
    if (page_.rulings.size() < kMaxRulingsPerPage)
        page_.rulings.push_back(Ruling{float(visible.x0), float(visible.y0), float(visible.x1), float(visible.y1), kRulingFill});
}

// The inline image BI just read (its dictionary; opSource_ is its source, BI to EI; `data` its
// samples) as an image of the page. The source is kept: Layout re-reads it when it extracts the
// image (the lexer's dictionary does not outlive the next operator). Out of line: Execute keeps no
// image record.
void Interpreter::InlineImage(const PdfValue& dictionary, const PdfValue& resources, std::string_view data) {
    PaintedSample(dictionary, data);
    const Box box = Intersect(UnitSquare(gs_.ctm), pageBox_);
    if (box.Empty() || page_.images.size() >= kMaxShapesPerPage ||
        page_.inlineImages.size() + opSource_.size() > kMaxInlineImageBytes)
        return;
    ImageBox image;
    image.x0 = float(box.x0), image.y0 = float(box.y0), image.x1 = float(box.x1), image.y1 = float(box.y1);
    image.matrix[0] = float(gs_.ctm.a), image.matrix[1] = float(gs_.ctm.b), image.matrix[2] = float(gs_.ctm.c);
    image.matrix[3] = float(gs_.ctm.d), image.matrix[4] = float(gs_.ctm.e), image.matrix[5] = float(gs_.ctm.f);
    const PdfValue* w = dictionary.Find("W");
    const PdfValue* h = dictionary.Find("H");
    if (!w) w = dictionary.Find("Width");
    if (!h) h = dictionary.Find("Height");
    image.width = w ? Dimension(w->Int()) : 0;
    image.height = h ? Dimension(h->Int()) : 0;
    image.resources = resources;
    image.mcid = mcid_;
    image.mcStream = mcStream_;
    image.inlineData = static_cast<uint32_t>(page_.inlineImages.size());
    image.inlineLength = static_cast<uint32_t>(opSource_.size());
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : opSource_) hash = (hash ^ c) * 1099511628211ull;
    image.identity = hash | 1;
    page_.inlineImages.append(opSource_);
    page_.images.push_back(image);
}

void Interpreter::AddPiece(double x0, double y0, double x1, double y1, bool rect) {
    if (pieces_.size() >= kMaxPiecesPerPath) return;
    const Matrix& m = gs_.ctm;
    const double ax = x0 * m.a + y0 * m.c + m.e, ay = x0 * m.b + y0 * m.d + m.f;
    const double bx = x1 * m.a + y1 * m.c + m.e, by = x1 * m.b + y1 * m.d + m.f;
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by)) return;
    // Straight pieces count only when axis-aligned on the page (within a hundredth of a point).
    if (!rect && std::fabs(ax - bx) > 0.01 && std::fabs(ay - by) > 0.01) return;
    // Both ends finite: Fmin and Fmax are the plain comparisons.
    pieces_.push_back(PathPiece{ax < bx ? ax : bx, ay < by ? ay : by, ax > bx ? ax : bx, ay > by ? ay : by, rect});
}

// The `kCount` points (x, y) of `xy` added to the path's box in device space (layout/path_box.h).
// The box holds finite points only; an empty path's box starts at infinities, which the first
// point replaces whole.
template <int kCount>
RAYOPDF_HOT_INLINE void Interpreter::PathPoints(const double* xy, bool curve) {
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    static_assert(sizeof(Matrix) == 6 * sizeof(double) && sizeof(Box) == 4 * sizeof(double),
                  "Matrix holds a b c d e f and Box x0 y0 x1 y1, as PathBounds reads them");
    double m[6];
    std::memcpy(m, &gs_.ctm, sizeof m);
    double box[4] = {kInfinity, kInfinity, -kInfinity, -kInfinity};
    if (!pathEmpty_) std::memcpy(box, &pathBox_, sizeof box);
    if (!PathBounds(m, xy, kCount, box)) return;
    std::memcpy(&pathBox_, box, sizeof box);
    pathEmpty_ = false;
    if (curve) pathCurved_ = true;
}

void Interpreter::Paint(bool fill, bool stroke) {
    if (!pathEmpty_) {
        if (pendingClip_) gs_.clip = Intersect(gs_.clip, pathBox_);
        if ((fill || stroke) && page_.shapes.size() < kMaxShapesPerPage) {
            Box box = pathBox_;
            if (stroke) {
                // Half the line width around the path; the line width scales with the CTM.
                const double scale = std::sqrt(std::fabs(gs_.ctm.a * gs_.ctm.d - gs_.ctm.b * gs_.ctm.c));
                const double half = Fmax(gs_.lineWidth * scale, 0.1) / 2;
                box.x0 -= half, box.y0 -= half, box.x1 += half, box.y1 += half;
            }
            const Box visible = Intersect(box, gs_.clip);
            if (visible.x1 >= visible.x0 && visible.y1 >= visible.y0) {
                Shape s;
                s.x0 = float(box.x0), s.y0 = float(box.y0), s.x1 = float(box.x1), s.y1 = float(box.y1);
                s.filled = fill;
                s.stroked = stroke;
                s.curved = pathCurved_;
                s.segments = pathSegments_;
                if (pathIsRect_ && pathSubpaths_ == 1 && !pathCurved_)
                    s.kind = ShapeKind::Rect;
                else if (pathSegments_ == 1 && pathSubpaths_ == 1 && !pathCurved_)
                    s.kind = ShapeKind::Line;
                page_.shapes.push_back(s);
            }
        }
        if ((fill || stroke) && !pieces_.empty()) {
            const double scale = std::sqrt(std::fabs(gs_.ctm.a * gs_.ctm.d - gs_.ctm.b * gs_.ctm.c));
            const double half = stroke ? Fmax(gs_.lineWidth * scale, 0.1) / 2 : 0;
            const uint8_t flags = static_cast<uint8_t>((stroke ? kRulingStroke : 0) | (fill ? kRulingFill : 0));
            // A piece under half a point both ways (a plot's dot or marker stroke) is no rule of a
            // table and no bar of a formula: it does not count against the page's rulings, which a
            // plot of many points would otherwise fill before the rules of a table after it.
            const auto add = [&](double x0, double y0, double x1, double y1) {
                Box b{x0 - half, y0 - half, x1 + half, y1 + half};
                b = Intersect(b, gs_.clip);
                if (!(b.x1 >= b.x0 && b.y1 >= b.y0) || (b.x1 - b.x0 < 0.5 && b.y1 - b.y0 < 0.5) ||
                    page_.rulings.size() >= kMaxRulingsPerPage)
                    return;
                page_.rulings.push_back(Ruling{float(b.x0), float(b.y0), float(b.x1), float(b.y1), flags});
            };
            for (const PathPiece& p : pieces_) {
                if (p.rect && stroke) {
                    add(p.x0, p.y0, p.x1, p.y0);
                    add(p.x0, p.y1, p.x1, p.y1);
                    add(p.x0, p.y0, p.x0, p.y1);
                    add(p.x1, p.y0, p.x1, p.y1);
                } else {
                    add(p.x0, p.y0, p.x1, p.y1);
                }
            }
        }
    } else if (pendingClip_) {
        // Clipping to an empty path hides everything after it.
        gs_.clip = Box{0, 0, 0, 0};
    }
    pieces_.clear();
    pathEmpty_ = true;
    pathCurved_ = false;
    pathSegments_ = 0;
    pathSubpaths_ = 0;
    pathIsRect_ = false;
    pendingClip_ = 0;
}

bool Interpreter::SetFont(const PdfValue& resources, std::string_view name, double size) {
    gs_.fontSize = std::isfinite(size) ? size : 0;
    const PdfValue& own = doc_.Resolve(resources);
    FontMemo* memo = nullptr;
    if (own.IsDict() && !name.empty() && name.size() <= sizeof(FontMemo::name)) {
        memo = &fontMemo_[(static_cast<unsigned char>(name.back()) + name.size()) % kFontMemos];
        if (memo->entries == own.data && memo->count == own.count && memo->nameSize == name.size() &&
            std::memcmp(memo->name, name.data(), name.size()) == 0) {
            gs_.font = memo->font;
            gs_.fontId = memo->id;
            gs_.fontMath = memo->id < page_.fontMath.size() ? static_cast<uint8_t>(page_.fontMath[memo->id] & kMathFontClass) : 0;
            return true;
        }
    }
    // As doc_.Get(resources, "Font"): `own` is resolved already.
    const PdfValue* fonts = own.Find("Font");
    const PdfValue& fontsDict = fonts ? doc_.Resolve(*fonts) : doc_.Get(own, "Font");
    const PdfValue* entry = fontsDict.Find(name);
    if (!entry) {
        gs_.font = nullptr;
        return false;
    }
    SetFontEntry(*entry, size);
    if (memo && gs_.fontId < fontIds_.size() && OneHop(fonts) && OneHop(entry)) {
        memo->entries = own.data;
        memo->count = own.count;
        memo->nameSize = static_cast<uint8_t>(name.size());
        std::memcpy(memo->name, name.data(), name.size());
        memo->font = gs_.font;
        memo->id = gs_.fontId;
    }
    return true;
}

void Interpreter::SetFontEntry(const PdfValue& entry, double size) {
    gs_.fontSize = std::isfinite(size) ? size : 0;
    const Text::Font& font = fonts_.Get(entry);
    gs_.font = &font;
    size_t id = 0;
    while (id < fontIds_.size() && fontIds_[id] != &font) ++id;
    if (id == fontIds_.size() && fontIds_.size() < 65535) {
        fontIds_.push_back(&font);
        page_.fontMath.push_back(MathFontByte(font.BaseName()));
    }
    gs_.fontId = static_cast<uint16_t>(id < 65535 ? id : 65534);
    gs_.fontMath = id < page_.fontMath.size() ? static_cast<uint8_t>(page_.fontMath[id] & kMathFontClass) : 0;
}

void Interpreter::ApplyExtGState(const PdfValue& resources, std::string_view name) {
    const PdfValue& states = doc_.Get(resources, "ExtGState");
    const PdfValue* entry = states.Find(name);
    if (!entry) return;
    const PdfValue& state = doc_.Resolve(*entry);
    if (!state.IsDict()) return;
    const PdfValue& font = doc_.Get(state, "Font");
    const PdfValue fontArray = font;
    if (fontArray.IsArray() && fontArray.Array().size() >= 2) {
        double size = 0;
        if (doc_.Number(fontArray.Array()[1], size)) SetFontEntry(fontArray.Array()[0], size);
    }
    double alpha = 1;
    if (doc_.Number(doc_.Get(state, "ca"), alpha)) gs_.transparent = alpha <= 0.0;
    double width = 0;
    if (doc_.Number(doc_.Get(state, "LW"), width)) gs_.lineWidth = width;
}

const PdfValue* Interpreter::FindXObject(const PdfValue& xobjects, std::string_view name) {
    // Small dictionaries, and the first lookups in a large one, read it in order; a large one
    // looked up again and again gets its keys sorted (with their entry order, so the first of
    // repeated keys is found, as Find finds it).
    if (!xobjects.IsDict() || xobjects.count < kIndexedDictionary) return xobjects.Find(name);
    NameIndex* index = nullptr;
    for (NameIndex& slot : nameIndexes_)
        if (slot.entries == xobjects.data && slot.count == xobjects.count) index = &slot;
    if (!index) {
        index = &nameIndexes_[nextNameIndex_++ % kNameIndexes];
        index->entries = xobjects.data, index->count = xobjects.count, index->lookups = 0;
        index->sorted.clear();
    }
    if (++index->lookups <= kIndexAfter) return xobjects.Find(name);
    const PdfSpan<PdfDictEntry> entries = xobjects.Entries();
    if (index->sorted.empty()) {
        index->sorted.reserve(entries.size());
        for (uint32_t i = 0; i < entries.size(); ++i) index->sorted.emplace_back(entries[i].key, i);
        std::sort(index->sorted.begin(), index->sorted.end());
    }
    const auto it = std::lower_bound(index->sorted.begin(), index->sorted.end(), std::make_pair(name, uint32_t(0)));
    return it != index->sorted.end() && it->first == name ? &entries[it->second].value : nullptr;
}

void Interpreter::DoXObject(const PdfValue& resources, std::string_view name, int depth) {
    const PdfValue& xobjects = doc_.Get(resources, "XObject");
    const PdfValue* entry = FindXObject(xobjects, name);
    if (!entry) return;
    const PdfValue& xobject = doc_.Resolve(*entry);
    if (!xobject.IsStream()) return;
    const PdfValue& subtype = doc_.Get(xobject, "Subtype");
    if (subtype.IsName("Image")) {
        const Box box = Intersect(UnitSquare(gs_.ctm), gs_.clip);
        if (box.Empty() || page_.images.size() >= kMaxShapesPerPage) return;
        ImageBox image;
        image.x0 = float(box.x0), image.y0 = float(box.y0), image.x1 = float(box.x1), image.y1 = float(box.y1);
        image.matrix[0] = float(gs_.ctm.a), image.matrix[1] = float(gs_.ctm.b), image.matrix[2] = float(gs_.ctm.c);
        image.matrix[3] = float(gs_.ctm.d), image.matrix[4] = float(gs_.ctm.e), image.matrix[5] = float(gs_.ctm.f);
        image.xobject = xobject;
        image.resources = resources;
        image.mcid = mcid_;
        image.mcStream = mcStream_;
        image.width = Dimension(doc_.Get(xobject, "Width").Int());
        image.height = Dimension(doc_.Get(xobject, "Height").Int());
        // A stream's data offset names it in the file: every placement of one XObject shares it.
        image.identity = (static_cast<uint64_t>(xobject.streamOffset) << 1) | 1;
        page_.images.push_back(image);
        return;
    }
    if (!subtype.IsName("Form")) return;
    if (depth + 1 > kMaxFormDepth) {
        budgetHit_ = true;
        return;
    }
    const uint32_t num = entry->IsRef() ? entry->refNum : 0;
    if (num != 0) {
        for (uint32_t active : formStack_)
            if (active == num) {
                page_.contentErrors++;
                return;
            }
    }
    if (formBuffers_.size() <= static_cast<size_t>(depth)) formBuffers_.resize(static_cast<size_t>(depth) + 1);
    std::string& data = formBuffers_[static_cast<size_t>(depth)];
    const StreamStatus status = doc_.StreamData(xobject, data);
    if (status == StreamStatus::Corrupt || status == StreamStatus::NotStream || status == StreamStatus::Salvaged ||
        status == StreamStatus::TooLarge)
        page_.contentErrors++;
    if (data.empty()) return;

    const GState saved = gs_;
    const size_t savedStack = stack_.size();
    const PdfValue& matrix = doc_.Get(xobject, "Matrix");
    if (matrix.IsArray() && matrix.Array().size() == 6) {
        double m[6];
        bool ok = true;
        for (int i = 0; i < 6; ++i) ok = ok && doc_.Number(matrix.Array()[static_cast<size_t>(i)], m[i]);
        if (ok) {
            const Matrix product = Multiply(Matrix{m[0], m[1], m[2], m[3], m[4], m[5]}, gs_.ctm);
            if (Finite(product)) gs_.ctm = product;
        }
    }
    PdfRect bbox;
    if (doc_.Rect(doc_.Get(xobject, "BBox"), bbox)) {
        const Matrix& c = gs_.ctm;
        const Matrix unit{bbox.x1 - bbox.x0, 0, 0, bbox.y1 - bbox.y0, bbox.x0, bbox.y0};
        gs_.clip = Intersect(gs_.clip, UnitSquare(Multiply(unit, c)));
    }
    const PdfValue& own = doc_.Get(xobject, "Resources");
    formStack_.push_back(num);
    // `data` must stay unchanged while the lexer reads it: deeper forms use deeper buffers.
    const std::string_view content(data);
    RunStream(content, own.IsDict() ? own : resources, depth + 1);
    formStack_.pop_back();
    while (stack_.size() > savedStack) stack_.pop_back();
    gs_ = saved;
}

void Interpreter::BeginMarked(std::string_view tag, const PdfValue* properties, const PdfValue& resources) {
    if (marked_.size() >= static_cast<size_t>(kMaxStateDepth)) {
        markedOverflow_++;
        return;
    }
    MarkedContent mc;
    mc.firstGlyph = static_cast<uint32_t>(page_.glyphs.size());
    mc.previousMcid = mcid_;
    mc.previousStream = mcStream_;
    mc.artifact = tag == "Artifact";
    if (mc.artifact) artifactDepth_++;
    if (properties) {
        const PdfValue* dict = properties;
        if (properties->IsName()) dict = &doc_.Get(doc_.Get(resources, "Properties"), properties->Name());
        if (dict->IsDict()) {
            const PdfValue& actual = doc_.Get(*dict, "ActualText");
            if (actual.IsString() && page_.text.size() < (size_t(1) << 30)) {
                mc.actualText = static_cast<uint32_t>(page_.text.size());
                AppendTextString(actual.String().substr(0, 4096), page_.text);
                // Control characters are no text (as font text, text/font.cpp UsableText): C0 and
                // C1 controls leave, whitespace controls become spaces.
                size_t kept = mc.actualText;
                for (size_t i = mc.actualText; i < page_.text.size(); ++i) {
                    const unsigned char c = static_cast<unsigned char>(page_.text[i]);
                    if (c == 0xC2 && i + 1 < page_.text.size() && static_cast<unsigned char>(page_.text[i + 1]) < 0xA0) {
                        ++i;  // U+0080..U+009F
                        continue;
                    }
                    if (c < 0x20 || c == 0x7F) {
                        if (c >= 0x09 && c <= 0x0D) page_.text[kept++] = ' ';
                        continue;
                    }
                    page_.text[kept++] = page_.text[i];
                }
                page_.text.resize(kept);
                mc.actualLength = static_cast<uint32_t>(page_.text.size() - mc.actualText);
                mc.hasActualText = true;
            }
            // An MCID counts in the stream that holds it (§14.7.4.2): the page's content, or a Form
            // XObject's (a marked-content reference with /Stm names it).
            const PdfValue& mcid = doc_.Get(*dict, "MCID");
            if (mcid.type == PdfValue::Type::Int && mcid.integer >= 0 && mcid.integer < INT32_MAX) {
                const uint8_t stream = MarkStream();
                mcid_ = stream == kNoMarkStream ? -1 : static_cast<int32_t>(mcid.integer);
                mcStream_ = stream == kNoMarkStream ? 0 : stream;
            }
        }
    }
    marked_.push_back(mc);
}

uint8_t Interpreter::MarkStream() {
    if (formStack_.empty()) return 0;
    // A form that is no indirect object cannot be named by a marked-content reference.
    const uint32_t num = formStack_.back();
    if (num == 0) return kNoMarkStream;
    for (size_t i = 0; i < page_.markStreams.size(); ++i)
        if (page_.markStreams[i] == num) return static_cast<uint8_t>(i + 1);
    if (page_.markStreams.size() >= kMaxMarkStreams) return kNoMarkStream;
    page_.markStreams.push_back(num);
    return static_cast<uint8_t>(page_.markStreams.size());
}

void Interpreter::EndMarked() {
    if (markedOverflow_ > 0) {
        markedOverflow_--;
        return;
    }
    if (marked_.empty()) return;
    const MarkedContent mc = marked_.back();
    marked_.pop_back();
    mcid_ = mc.previousMcid;
    mcStream_ = mc.previousStream;
    if (mc.artifact && artifactDepth_ > 0) artifactDepth_--;
    if (!mc.hasActualText) return;
    // ActualText replaces the enclosed glyphs (§14.9.4): the first visible one carries the text and
    // the union of their boxes; the others leave the text.
    GlyphVector& glyphs = page_.glyphs;
    uint32_t carrier = UINT32_MAX;
    for (uint32_t i = mc.firstGlyph; i < glyphs.size(); ++i) {
        Glyph& g = glyphs[i];
        if (g.flags & kGlyphDropped) continue;
        if (carrier == UINT32_MAX) {
            carrier = i;
            continue;
        }
        Glyph& c = glyphs[carrier];
        if (g.dir == c.dir) {
            c.u0 = Fmin(c.u0, g.u0), c.u1 = Fmax(c.u1, g.u1);
            c.vb = Fmin(c.vb, g.vb), c.vt = Fmax(c.vt, g.vt);
        }
        g.flags |= kGlyphDropped;
    }
    if (carrier == UINT32_MAX) return;
    Glyph& c = glyphs[carrier];
    std::string_view text = std::string_view(page_.text).substr(mc.actualText, mc.actualLength);
    // A space-only replacement is a word break: keep a space glyph.
    bool blank = true;
    for (char ch : text) blank = blank && (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
    c.text = mc.actualText;
    c.textLength = static_cast<uint16_t>(mc.actualLength < 65535 ? mc.actualLength : 65535);
    c.flags = static_cast<uint16_t>(c.flags & ~(kGlyphUnmapped | kGlyphGuessed | kGlyphSpace));
    if (blank) c.flags |= kGlyphSpace;
    if (mc.actualLength == 0) c.flags |= kGlyphDropped;
}

bool Interpreter::ShowString(std::string_view bytes) {
    return ShowText(nullptr, 0, bytes);
}

bool Interpreter::ShowArray(const PdfValue& array) {
    const PdfSpan<PdfValue> items = array.Array();
    return ShowText(items.begin(), items.size(), std::string_view());
}

// AppendGlyphText past its one-ASCII-character case.
RAYOPDF_NOINLINE uint16_t AppendGlyphTextGeneral(std::string_view unicode, std::string& out) {
    uint16_t flags = 0;
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    bool any = false, allSpace = true;
    while (Utf8::Decode(unicode, at, cp, length)) {
        at += length;
        any = true;
        switch (cp) {
        case 0xFB00: out += "ff"; allSpace = false; continue;
        case 0xFB01: out += "fi"; allSpace = false; continue;
        case 0xFB02: out += "fl"; allSpace = false; continue;
        case 0xFB03: out += "ffi"; allSpace = false; continue;
        case 0xFB04: out += "ffl"; allSpace = false; continue;
        case 0xFB05:
        case 0xFB06: out += "st"; allSpace = false; continue;
        default: break;
        }
        const bool space = cp == 0x20 || cp == 0xA0 || cp == 0x09 || cp == 0x0A || cp == 0x0D ||
                           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
        if (space) {
            out.push_back(' ');
            continue;
        }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || cp == 0x200B || cp == 0xFEFF) continue;
        allSpace = false;
        Utf8::Append(out, cp);
    }
    if (any && allSpace) flags |= kGlyphSpace;
    return flags;
}

// Appends the UTF-8 text of one code to the page text, normalized: presentation-form ligatures
// expanded (algorithms §4.4). Returns the flags the text implies. One ASCII character, most
// glyphs, inline: what AppendGlyphTextGeneral's loop does with it.
RAYOPDF_HOT_INLINE uint16_t AppendGlyphText(std::string_view unicode, std::string& out) {
    if (unicode.size() == 1 && static_cast<unsigned char>(unicode[0]) < 0x80) {
        const char c = unicode[0];
        if (c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D) {
            out.push_back(' ');
            return kGlyphSpace;
        }
        if (c < 0x20 || c == 0x7F) return kGlyphSpace;
        out.push_back(c);
        return 0;
    }
    return AppendGlyphTextGeneral(unicode, out);
}

const GlyphRun& Interpreter::RunFor(const Text::Font& font, const Matrix& tm) {
    // Trm = [Tfs·Th 0 0 Tfs 0 Trise] × Tm × CTM (§9.4.2), without its translation; the y axis in
    // ems of the font (Text::Font::EmScale).
    const double tfs = gs_.fontSize;
    const double th = gs_.hScale;
    const double em = font.EmScale();
    const double a = tfs * th * tm.a, b = tfs * th * tm.b, c = tfs * em * tm.c, d = tfs * em * tm.d;
    const bool artifact = artifactDepth_ > 0;
    GlyphRun& run = run_;
    if (run.font == &font && SameBits(run.a, a) && SameBits(run.b, b) && SameBits(run.c, c) && SameBits(run.d, d) &&
        run.render == gs_.render && run.transparent == gs_.transparent && run.artifact == artifact &&
        SameBits(run.clip.x0, gs_.clip.x0) && SameBits(run.clip.y0, gs_.clip.y0) &&
        SameBits(run.clip.x1, gs_.clip.x1) && SameBits(run.clip.y1, gs_.clip.y1))
        return run;
    run.font = &font;
    run.a = a, run.b = b, run.c = c, run.d = d;
    run.render = gs_.render;
    run.transparent = gs_.transparent;
    run.artifact = artifact;
    run.clip = gs_.clip;
    run.vertical = font.Vertical();
    run.ascent = font.Ascent();
    run.descent = font.Descent();
    if (!(run.ascent > run.descent)) run.ascent = 0.8, run.descent = -0.2;
    const Text::FontStyle& style = font.Style();
    uint16_t flags = 0;
    if (style.bold) flags |= kGlyphBold;
    if (style.italic) flags |= kGlyphItalic;
    if (style.monospace) flags |= kGlyphMonospace;
    if (style.symbolic) flags |= kGlyphSymbolic;
    const double size = std::sqrt(run.c * run.c + run.d * run.d);
    const double runLength = std::sqrt(run.a * run.a + run.b * run.b);
    run.usable = std::isfinite(size) && std::isfinite(runLength);
    run.size = float(size);
    // Baseline direction from the text matrix's x axis, or the y axis turned when x is degenerate.
    double bx = run.a, by = run.b;
    if (runLength < 1e-9) bx = run.d, by = -run.c;
    if (std::fabs(bx) >= std::fabs(by))
        run.dir = bx >= 0 ? 0 : 2;
    else
        run.dir = by > 0 ? 1 : 3;
    if (gs_.render == 2) flags |= kGlyphBold;
    // A sheared text matrix is a synthetic oblique (algorithms §6.6).
    if (runLength > 1e-9 && size > 1e-9) {
        const double cosine = (run.a * run.c + run.b * run.d) / (runLength * size);
        if (std::fabs(cosine) > 0.1) flags |= kGlyphItalic;
    }
    const int mode = gs_.render & 7;
    if (mode == 3 || mode == 7 || gs_.transparent) flags |= kGlyphInvisible;
    if (mode == 1 || mode == 5) flags |= kGlyphStrokeOnly;
    if (artifact) flags |= kGlyphArtifact;
    run.flags = flags;
    // The box: x from 0 to the width, y from descent to ascent; vertically x from -0.5 to 0.5, y
    // from the vertical advance (or -1) to 0.
    const double x0 = run.vertical ? -0.5 : 0, x1 = 0.5, y1 = run.vertical ? 0 : run.ascent, y0 = run.descent;
    run.xa[0] = x0 * run.a, run.xb[0] = x0 * run.b;
    run.xa[1] = x1 * run.a, run.xb[1] = x1 * run.b;
    run.yc[0] = y0 * run.c, run.yd[0] = y0 * run.d;
    run.yc[1] = y1 * run.c, run.yd[1] = y1 * run.d;
    const Box& clip = gs_.clip;
    run.clipX0 = clip.x0 - 1, run.clipY0 = clip.y0 - 1, run.clipX1 = clip.x1 + 1, run.clipY1 = clip.y1 + 1;
    run.clipEmpty = clip.Empty();
    bool finite = true;
    for (int i = 0; i < 2; ++i)
        finite = finite && std::isfinite(run.xa[i]) && std::isfinite(run.xb[i]) && std::isfinite(run.yc[i]) &&
                 std::isfinite(run.yd[i]);
    run.termsFinite = finite;
    run.boxTerms = !run.vertical && finite;
    BoxTerms& box = run.box;
    box.r[0] = run.a, box.r[1] = run.b;
    box.s0[0] = run.xa[0] + run.yc[0], box.s0[1] = run.xb[0] + run.yd[0];
    box.s1[0] = run.xa[0] + run.yc[1], box.s1[1] = run.xb[0] + run.yd[1];
    box.y0[0] = run.yc[0], box.y0[1] = run.yd[0];
    box.y1[0] = run.yc[1], box.y1[1] = run.yd[1];
    box.clipLo[0] = run.clipX0, box.clipLo[1] = run.clipY0;
    box.clipHi[0] = run.clipX1, box.clipHi[1] = run.clipY1;
    box.clipEmpty = run.clipEmpty;
    return run;
}

// Shows the string `single` (Tj, ', "; `items` null) or the strings and position adjustments of a
// TJ array (`items`, `count`). Glyph placement:
// - What placing glyphs needs from the graphics state and the run is read once into a local
//   (Placement), for a TJ array once for all its strings while the text matrix's linear part keeps
//   its bits: an adjustment moves only the translation, so Trm's linear part and the run stay, and
//   Trm's translation is Multiply's own expression. Locals, not members: a glyph's text is stored
//   as chars, which may alias any member, so a member read in the glyph loop would be loaded again
//   after every glyph.
// - A simple font's code is read from its slot inline (as Font::Next does); anything else goes
//   through Next.
// - Each code's record is written in place in the page's glyph vector: for the common case
//   (PlaceGlyphs<true>) the vector is grown once per operator by a record per byte of its strings,
//   and cut back to the records written when the operator ends (ShowText) or before the general
//   case appends its own.
bool Interpreter::ShowText(const PdfValue* items, size_t count, std::string_view single) {
    size_t placed = page_.glyphs.size();
    const bool shown = ShowItems(items, count, single, placed);
    page_.glyphs.resize(placed);
    return shown;
}

// ShowText's strings and adjustments; `placed` counts the records written, which the vector may
// exceed.
RAYOPDF_HOT_INLINE bool Interpreter::ShowItems(const PdfValue* items, size_t count, std::string_view single,
                                               size_t& placed) {
    GlyphVector& glyphs = page_.glyphs;
    Placement p;
    int vertical = -1;  // the font's writing mode, read at the first adjustment
    const size_t strings = items != nullptr ? count : 1;
    // The bytes of this and the later strings: the records the vector is grown by.
    size_t remaining = single.size();
    for (size_t k = 0; items != nullptr && k < count; ++k)
        if (items[k].IsString()) remaining += items[k].String().size();
    for (size_t k = 0; k < strings; ++k) {
        std::string_view bytes = single;
        if (items != nullptr) {
            const PdfValue& item = items[k];
            if (budgetHit_) return true;
            if (item.IsNumber()) {
                const double adjust = item.Number();
                if (!std::isfinite(adjust)) continue;
                const double shift = -adjust / 1000 * gs_.fontSize;
                if (vertical < 0) vertical = gs_.font && gs_.font->Vertical() ? 1 : 0;
                // Multiply(t, textMatrix_) for t = [1 0 0 1 tx ty], term by term: the same
                // products and sums (1·x is x), inline.
                const double tx = vertical ? 0 : shift * gs_.hScale, ty = vertical ? shift : 0;
                const Matrix m = textMatrix_;
                textMatrix_.a = m.a + 0.0 * m.c;
                textMatrix_.b = m.b + 0.0 * m.d;
                textMatrix_.c = 0.0 * m.a + m.c;
                textMatrix_.d = 0.0 * m.b + m.d;
                textMatrix_.e = tx * m.a + ty * m.c + m.e;
                textMatrix_.f = tx * m.b + ty * m.d + m.f;
                continue;
            }
            if (!item.IsString()) continue;
            bytes = item.String();
        }
        remaining -= bytes.size();
        const Text::Font* font = gs_.font;
        if (!font) return false;
        // Tm × CTM: its linear part and the run kept from the string before when the text
        // matrix's linear part has the same bits (the graphics state does not change inside TJ).
        double tme, tmf;
        if (p.ready && SameBits(textMatrix_.a, p.ta) && SameBits(textMatrix_.b, p.tb) &&
            SameBits(textMatrix_.c, p.tc) && SameBits(textMatrix_.d, p.td)) {
            tme = textMatrix_.e * p.ctmA + textMatrix_.f * p.ctmC + p.ctmE;
            tmf = textMatrix_.e * p.ctmB + textMatrix_.f * p.ctmD + p.ctmF;
        } else {
            const Matrix tm = Multiply(textMatrix_, gs_.ctm);
            tme = tm.e, tmf = tm.f;
            p.Read(gs_, textMatrix_, tm, RunFor(*font, tm));
            p.mcid = mcid_;
            p.mcStream = mcStream_;
            p.uses = stats_.uses;
        }
        // The common case: a horizontal run with finite terms and size, no math font, no glyph
        // uses kept, and room for a record per byte, so the glyph budget cannot run out.
        if (p.boxTerms && p.usable && p.fontMath == 0 && p.uses == nullptr &&
            placed + bytes.size() <= kMaxGlyphsPerPage) {
            if (glyphs.size() < placed + bytes.size()) {
                const size_t want = placed + bytes.size() + remaining;
                glyphs.resize(want < kMaxGlyphsPerPage ? want : kMaxGlyphsPerPage);
            }
            // The string, then the array's next strings while the case holds, in one cursor:
            // an adjustment that leaves the text matrix's linear part with its bits moves only
            // the cursor's translation (the outer loop's expressions), and the next string's Trm
            // translation is the one the outer loop would compute.
            GlyphCursor cur{textMatrix_.e, textMatrix_.f, tme, tmf};
            cur.placed = placed;
            bool open = true;
            for (;;) {
                if (!PlaceGlyphs<true>(bytes, p, cur)) {
                    open = false;
                    break;
                }
                if (items == nullptr) break;
                if ((cur.counts & kCountMask) >= kCountFlush) FlushCounts(cur);
                size_t next = k + 1;
                bool adjusted = false;  // the linear part lost its bits: the outer loop goes on
                for (; next < count; ++next) {
                    const PdfValue& item = items[next];
                    if (item.IsString()) break;
                    if (!item.IsNumber()) continue;
                    const double adjust = item.Number();
                    if (!std::isfinite(adjust)) continue;
                    const double tx = -adjust / 1000 * p.tfs * p.th, ty = 0;
                    cur.te = tx * p.ta + ty * p.tc + cur.te;
                    cur.tf = tx * p.tb + ty * p.td + cur.tf;
                    if (p.keepsLinear < 0) p.keepsLinear = KeepsLinear(p.ta, p.tb, p.tc, p.td) ? 1 : 0;
                    if (!p.keepsLinear) {
                        textMatrix_.a = p.ta + 0.0 * p.tc, textMatrix_.b = p.tb + 0.0 * p.td;
                        textMatrix_.c = 0.0 * p.ta + p.tc, textMatrix_.d = 0.0 * p.tb + p.td;
                        adjusted = true;
                        break;
                    }
                }
                k = next;
                if (adjusted || next >= count) break;
                bytes = items[next].String();
                if (budgetHit_ || cur.placed + bytes.size() > kMaxGlyphsPerPage ||
                    glyphs.size() < cur.placed + bytes.size()) {
                    k = next - 1;  // the outer loop takes this string
                    break;
                }
                remaining -= bytes.size();
                cur.tme = cur.te * p.ctmA + cur.tf * p.ctmC + p.ctmE;
                cur.tmf = cur.te * p.ctmB + cur.tf * p.ctmD + p.ctmF;
            }
            placed = cur.placed;
            EndCursor(cur);
            if (!open) return true;
        } else {
            glyphs.resize(placed);
            const bool open = PlaceGlyphsGeneral(bytes, p, tme, tmf);
            placed = glyphs.size();
            if (!open) return true;
        }
    }
    return true;
}

// The glyphs of one string, from the cursor's translations on. False when the glyph budget ran out.
// With kFast (ShowText's common case) the run is horizontal with finite terms and usable, no math
// font or glyph uses apply, and the glyph vector holds a record past `cur.placed` for every byte of
// the string: each is written there and counted. Otherwise records are appended.
template <bool kFast>
RAYOPDF_HOT_INLINE bool Interpreter::PlaceGlyphs(std::string_view bytes, const Placement& p, GlyphCursor& cur) {
    const bool vertical = kFast ? false : p.vertical;
    const Text::Font* font = p.font;
    // The text matrix moves by each advance; its linear part stays.
    double &te = cur.te, &tf = cur.tf, &tme = cur.tme, &tmf = cur.tmf;
    uint64_t &counts = cur.counts, &shown = cur.shown, &unmapped = cur.unmapped, &guessed = cur.guessed;
    size_t& placed = cur.placed;
    GlyphVector& glyphs = page_.glyphs;
    std::string& text = page_.text;
    Glyph* const records = glyphs.data();
    Glyph* out = records + placed;  // kFast: the next record
    bool open = true;
    size_t at = 0;
    while (at < bytes.size()) {
        // The next code: a simple font's slot read inline, as Font::Next does; anything else
        // through Next.
        uint32_t codeValue;
        uint8_t codeBytes;
        double width, verticalAdvance;
        bool wordSpaceCode;
        Text::UnicodeSource source;
        std::string_view unicode;
        const uint8_t c = static_cast<uint8_t>(bytes[at]);
        if (p.slots[c].state == Text::FontSlotState::Pool) {
            const Text::FontSlot& slot = p.slots[c];
            ++at;
            codeValue = c;
            codeBytes = 1;
            width = slot.width;
            verticalAdvance = 0;
            wordSpaceCode = c == 32;
            source = slot.source;
            unicode = std::string_view(p.pool + slot.offset, slot.length);
        } else {
            Text::Code code;
            size_t after = at;  // Next takes the offset by reference: `at` itself stays a register
            if (!font->Next(bytes, after, code)) break;
            at = after;
            codeValue = code.code;
            codeBytes = code.bytes;
            width = code.width;
            verticalAdvance = code.verticalAdvance;
            wordSpaceCode = code.wordSpace;
            source = code.source;
            unicode = code.unicode;
        }
        // Trm's translation; its linear part is the run's.
        const double e = p.riseC + tme;
        const double f = p.riseD + tmf;
        double advance;
        if (vertical) {
            advance = verticalAdvance * p.tfs + p.charSpace + (wordSpaceCode ? p.wordSpace : 0);
        } else {
            advance = (width * p.tfs + p.charSpace + (wordSpaceCode ? p.wordSpace : 0)) * p.th;
        }
        if (kFast) {
            counts += kSourceCounts[static_cast<uint8_t>(source)];
        } else {
            shown++;
            if (source == Text::UnicodeSource::None) unmapped++;
            if (source == Text::UnicodeSource::Guess) guessed++;
        }
        if ((!kFast && glyphs.size() >= kMaxGlyphsPerPage) || text.size() >= (size_t(1) << 30)) {
            // The budget ends the page; this code does not move the text matrix.
            budgetHit_ = true;
            open = false;
            break;
        }
        if ((kFast || p.usable) && std::isfinite(e) && std::isfinite(f)) {
            // The text first: the box below then keeps no value across a call.
            const uint32_t textAt = static_cast<uint32_t>(text.size());
            uint16_t flags;
            size_t length;
            if (unicode.size() == 1 && static_cast<unsigned char>(unicode[0]) - 0x21u < 0x5Eu) {
                // A printable ASCII character other than the space: AppendGlyphText's first case.
                text.push_back(unicode[0]);
                flags = 0, length = 1;
            } else {
                flags = AppendGlyphText(unicode, text);
                length = text.size() - textAt;
            }
            flags |= p.runFlags | kSourceFlags[static_cast<uint8_t>(source)];
            if (length == 0 && source != Text::UnicodeSource::None) flags |= kGlyphDropped;  // control characters
            if (!kFast && RAYOPDF_UNLIKELY(p.fontMath != 0)) {
                // Math fonts (layout/math.h): glyphs left without text, every glyph of a mathabx
                // or kMathFontCoded font, a code WinAnsi guessed as a space (cmex10's \Bigg( at
                // 0x20), and the φ, ϕ,
                // ε and ϵ a math italic font's ToUnicode gives keep their code for --math=latex,
                // and math italic fonts read through glyph names are marked (TeX's \phi is AGL's
                // "phi1").
                if (codeBytes == 1 && page_.mathCodes.size() < kMaxGlyphsPerPage &&
                    (source == Text::UnicodeSource::None ||
                     (source == Text::UnicodeSource::Guess && (flags & kGlyphSpace) != 0) ||
                     (p.fontId < page_.fontMath.size() && (page_.fontMath[p.fontId] & (kMathFontAbx | kMathFontCoded)) != 0) ||
                     (source == Text::UnicodeSource::ToUnicode && p.fontMath == static_cast<uint8_t>(MathFont::Italic) &&
                      IsGreekVariantText(unicode))))
                    page_.mathCodes.push_back(MathCode{static_cast<uint32_t>(glyphs.size()), static_cast<uint8_t>(codeValue)});
                if (p.fontMath == static_cast<uint8_t>(MathFont::Italic) &&
                    (source == Text::UnicodeSource::Encoding || source == Text::UnicodeSource::FontProgram) &&
                    p.fontId < page_.fontMath.size())
                    page_.fontMath[p.fontId] |= kMathFontNamed;
            }

            // The record: in kFast a default one the vector was grown by, else a default one
            // appended; given every field: the string's, then the box's and the glyph's own. Its
            // box: the corners [x y] × Trm, the run's terms with the glyph's own edge, its width
            // (the right edge) or its vertical advance (the bottom edge).
            Glyph& g = kFast ? *out++ : glyphs.emplace_back();
            g.size = p.size;
            g.dir = p.dir;
            g.mcStream = p.mcStream;
            g.font = p.fontId;
            g.mcid = p.mcid;
            const bool clipped = (kFast || p.boxTerms) && std::isfinite(width)
                                     ? HorizontalBox(*p.box, width, e, f, p.dir, g)  // no NaN corner (termsFinite)
                                     : GeneralGlyphBox(p, width, verticalAdvance, e, f, g);
            double ou, ov = f;
            if (RAYOPDF_UNLIKELY(p.dir != 0)) ToFrame(p.dir, e, f, ou, ov);
            g.v = float(ov);
            g.text = textAt;
            g.textLength = static_cast<uint16_t>(length < 65535 ? length : 65535);
            g.flags = clipped ? static_cast<uint16_t>(flags | kGlyphClipped) : flags;
            if (!kFast && RAYOPDF_UNLIKELY(p.uses != nullptr)) {
                GlyphUse use;
                use.glyph = static_cast<uint32_t>(glyphs.size() - 1);
                use.text = textAt;
                use.font = font;
                use.code = codeValue;
                use.bytes = codeBytes;
                const double m[6] = {p.run->a, p.run->b, p.run->c, p.run->d, e, f};
                for (int i = 0; i < 6; ++i) use.matrix[i] = static_cast<float>(m[i]);
                p.uses->push_back(use);
            }
        }
        if (!std::isfinite(advance)) advance = 0;
        if (vertical) {
            tme += advance * p.tmc;
            tmf += advance * p.tmd;
            te += advance * p.tc;
            tf += advance * p.td;
        } else {
            tme += advance * p.tma;
            tmf += advance * p.tmb;
            te += advance * p.ta;
            tf += advance * p.tb;
        }
    }
    if (kFast) placed = static_cast<size_t>(out - records);
    return open;
}

// Ends a run of strings placed from `cur`: the text matrix's translation and the counts.
RAYOPDF_HOT_INLINE void Interpreter::EndCursor(const GlyphCursor& cur) {
    textMatrix_.e = cur.te;
    textMatrix_.f = cur.tf;
    stats_.glyphs += cur.shown + (cur.counts & kCountMask);
    stats_.unmapped += cur.unmapped + ((cur.counts >> kCountBits) & kCountMask);
    stats_.guessed += cur.guessed + (cur.counts >> (2 * kCountBits));
}

// PlaceGlyphs outside its common case, out of line: ShowText's glyph loop keeps one inline copy.
RAYOPDF_NOINLINE bool Interpreter::PlaceGlyphsGeneral(std::string_view bytes, const Placement& p, double tme, double tmf) {
    GlyphCursor cur{textMatrix_.e, textMatrix_.f, tme, tmf};
    const bool open = PlaceGlyphs<false>(bytes, p, cur);
    EndCursor(cur);
    return open;
}

void Interpreter::ReadLinks(int index) {
    const PdfValue& annots = doc_.Get(doc_.PageDict(index), "Annots");
    if (!annots.IsArray()) return;
    for (const PdfValue& item : annots.Array()) {
        if (page_.links.size() >= kMaxLinksPerPage) break;
        const PdfValue& annot = doc_.Resolve(item);
        if (!doc_.Get(annot, "Subtype").IsName("Link")) continue;
        const PdfValue& action = doc_.Get(annot, "A");
        if (!doc_.Get(action, "S").IsName("URI")) continue;
        const PdfValue& uri = doc_.Get(action, "URI");
        if (!uri.IsString() || uri.String().empty()) continue;
        PdfRect rect;
        if (!doc_.Rect(doc_.Get(annot, "Rect"), rect)) continue;
        Box box = UnitSquare(Multiply(Matrix{rect.Width(), 0, 0, rect.Height(), rect.x0, rect.y0}, pageMatrix_));
        LinkArea link;
        link.x0 = float(box.x0), link.y0 = float(box.y0), link.x1 = float(box.x1), link.y1 = float(box.y1);
        link.uri = static_cast<uint32_t>(page_.text.size());
        std::string_view bytes = uri.String().substr(0, 8192);
        // URIs are 7-bit ASCII (§12.6.4.7); spaces and controls are percent-encoded.
        for (unsigned char ch : bytes) {
            if (ch <= 0x20 || ch >= 0x7F) {
                static const char kHex[] = "0123456789ABCDEF";
                page_.text.push_back('%');
                page_.text.push_back(kHex[ch >> 4]);
                page_.text.push_back(kHex[ch & 15]);
            } else {
                page_.text.push_back(static_cast<char>(ch));
            }
        }
        link.uriLength = static_cast<uint32_t>(page_.text.size() - link.uri);
        page_.links.push_back(link);
    }
}

} // namespace

void AppendTextString(std::string_view bytes, std::string& out) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        size_t i = 2;
        while (i + 1 < bytes.size()) {
            uint32_t unit = (uint32_t(static_cast<unsigned char>(bytes[i])) << 8) | static_cast<unsigned char>(bytes[i + 1]);
            i += 2;
            if (unit >= 0xD800 && unit < 0xDC00 && i + 1 < bytes.size()) {
                const uint32_t low =
                    (uint32_t(static_cast<unsigned char>(bytes[i])) << 8) | static_cast<unsigned char>(bytes[i + 1]);
                if (low >= 0xDC00 && low < 0xE000) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            // A language escape (U+001B ... U+001B) is skipped (§7.9.2.2.1).
            if (unit == 0x1B) {
                while (i + 1 < bytes.size()) {
                    const uint32_t next = (uint32_t(static_cast<unsigned char>(bytes[i])) << 8) |
                                          static_cast<unsigned char>(bytes[i + 1]);
                    i += 2;
                    if (next == 0x1B) break;
                }
                continue;
            }
            Utf8::Append(out, unit);
        }
        return;
    }
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") {
        const std::string_view rest = bytes.substr(3);
        size_t at = 0;
        uint32_t cp = 0;
        size_t length = 0;
        while (Utf8::Decode(rest, at, cp, length)) {
            Utf8::Append(out, cp);
            at += length;
        }
        return;
    }
    for (unsigned char ch : bytes) {
        uint32_t cp = ch;
        if (ch >= 0x18 && ch <= 0x1F)
            cp = kPdfDocLow[ch - 0x18];
        else if (ch >= 0x80 && ch <= 0xA0)
            cp = kPdfDocHigh[ch - 0x80];
        Utf8::Append(out, cp);
    }
}

void InterpretPage(const Document& document, Text::FontCache& fonts, int index, bool withLinks, PageData& page,
                   PageStats& stats) {
    page.Clear();
    page.number = index + 1;
    Interpreter interpreter(document, fonts, page, stats);
    interpreter.Run(index, withLinks);
}

void ReadOutline(const Document& document, std::vector<OutlineEntry>& out) {
    out.clear();
    const PdfValue& outlines = document.Get(document.Catalog(), "Outlines");
    if (!outlines.IsDict()) return;
    struct Pending {
        const PdfValue* node;
        int level;
    };
    // Depth-first preorder: a node, its children, then its next sibling. Object numbers are
    // visited once, so a cyclic /Next or /First chain ends.
    std::vector<Pending> work;
    std::unordered_set<uint32_t> seen;
    if (const PdfValue* first = outlines.Find("First")) work.push_back({first, 1});
    int visits = 0;
    while (!work.empty() && static_cast<int>(out.size()) < kMaxOutlineEntries && visits++ < 4 * kMaxOutlineEntries) {
        const Pending p = work.back();
        work.pop_back();
        if (p.node->IsRef() && !seen.insert(p.node->refNum).second) continue;
        const PdfValue& node = document.Resolve(*p.node);
        if (!node.IsDict()) continue;
        OutlineEntry entry;
        entry.level = p.level;
        AppendTextString(document.Get(node, "Title").String(), entry.title);
        out.push_back(std::move(entry));
        if (const PdfValue* next = node.Find("Next")) work.push_back({next, p.level});
        if (p.level < 64)
            if (const PdfValue* child = node.Find("First")) work.push_back({child, p.level + 1});
    }
}

} // namespace RayoPdf::Layout
