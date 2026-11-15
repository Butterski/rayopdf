// SPDX-License-Identifier: Apache-2.0
// The page model shared by the layout stages (roadmap D4 steps 2-5): what the interpreter
// records of one page (glyphs, painted shapes, link areas) and the lines built from it.
//
// Coordinates are points on the upright page: the CropBox's lower-left corner is the origin, y
// grows upwards, and the page /Rotate is already applied. Each glyph also carries its box in the
// frame of its baseline direction (u along the baseline, v perpendicular to it, upwards for the
// reader), so that lines and reading order work the same way for rotated text (algorithms §4.4).
// A page owns everything it refers to and depends on no other page: pages can be built in parallel.
#pragma once

#include "common/compiler.h"
#include "pdf/value.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RayoPdf::Layout {

// Glyph flags.
enum : uint16_t {
    kGlyphBold = 1u << 0,        // bold font, Tr 2, or an overprinted duplicate (algorithms §4.2)
    kGlyphItalic = 1u << 1,      // italic font or a sheared text matrix
    kGlyphMonospace = 1u << 2,   // fixed-pitch font
    kGlyphInvisible = 1u << 3,   // Tr 3 or 7, or fill alpha 0
    kGlyphStrokeOnly = 1u << 4,  // Tr 1
    kGlyphClipped = 1u << 5,     // outside the CropBox or the clip
    kGlyphUnmapped = 1u << 6,    // no Unicode text
    kGlyphGuessed = 1u << 7,     // Unicode text is a guess
    kGlyphArtifact = 1u << 8,    // inside /Artifact marked content
    kGlyphSpace = 1u << 9,       // a space character (U+0020, NBSP and other spaces)
    kGlyphDropped = 1u << 10,    // not part of the text: a duplicate, hidden, or covered by ActualText
    kGlyphSpaceBefore = 1u << 11, // set by BuildLines: a word break precedes this glyph in its line
    kGlyphSymbolic = 1u << 12,   // symbolic font (Symbol, ZapfDingbats, Wingdings)
    kGlyphImage = 1u << 13,      // no glyph: an image set in the line; its text is the Markdown image link
    kGlyphStrike = 1u << 14,     // struck through: set by BuildLines from a rule across its middle, or by tags
    // Recognised by the OCR add-on (layout/ocr.h), always with kGlyphInvisible: an OCR text layer
    // that BuildLines keeps even beside visible text (an image region's text on a digital page).
    kGlyphOcr = 1u << 15,
};

// One glyph: 40 bytes, so a page's glyphs take as little memory as they can. Its box on the
// upright page is not stored: it is the frame box turned back (PageBox below).
struct Glyph {
    // Box in the direction frame: u0..u1 along the baseline (origin to origin + advance), vb..vt
    // from descent to ascent; `v` is the baseline. For direction 0 it is the box on the page.
    float u0 = 0, u1 = 0, vb = 0, vt = 0, v = 0;
    float size = 0;          // effective font size in points
    uint32_t text = 0;       // offset of its UTF-8 text in PageData::text
    uint16_t textLength = 0;
    uint16_t flags = 0;
    uint8_t dir = 0;         // baseline direction: 0 left-to-right, 1 upwards, 2 right-to-left, 3 downwards
    uint8_t mcStream = 0;    // where `mcid` counts: 0 the page's content, k PageData::markStreams[k - 1]
    uint16_t font = 0;       // page-local font index, for style comparisons
    int32_t mcid = -1;       // marked-content id, -1 when none
};
static_assert(sizeof(Glyph) == 40, "Glyph is a 40-byte record");

// A painted path, reduced to what layout needs: its box, how it was painted and its shape.
enum class ShapeKind : uint8_t {
    Rect,   // a single axis-aligned rectangle (re, or four axis-aligned lines closing)
    Line,   // a single straight segment
    Other,  // anything else (curves, polygons, several subpaths)
};

struct Shape {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ShapeKind kind = ShapeKind::Other;
    bool filled = false;
    bool stroked = false;
    bool curved = false;
    uint16_t segments = 0;
};

// What the search for a page's figure regions (layout/figures.h) finds from its shapes and images
// alone, kept from its first search: whether they can make a region at all, the clusters of its
// drawing strokes with the weight of their marks, and the curves of the clusters whose marks alone
// weigh too little, on the grid the strokes lie on.
struct FigureStrokes {
    struct Cluster {
        float x0, y0, x1, y1;
        int32_t weight;  // of its marks
    };
    struct Curve {
        int32_t a, b, c, d;  // columns a to b and rows c to d of the grid, from (c0, r0)
        uint32_t cluster;
    };
    bool known = false;
    bool possible = false;  // a picture, or strokes that could weigh enough
    float cell = 0;         // the grid's step: the page's grid is pageX x pageY cells
    int32_t pageX = 0, pageY = 0, c0 = 0, c1 = 0, r0 = 0, r1 = 0;  // and the cells the strokes touch
    std::vector<Cluster> clusters;
    std::vector<Curve> curves;

    void Clear() {
        known = false;
        clusters.clear();
        curves.clear();
    }
};

// An image placed on the page (an image XObject or an inline image), with what extracting it
// takes (Image::EncodeXObject / EncodeInline): an XObject's stream and the resources that named
// it, views valid while the Document lives; an inline image's source from BI to EI, copied into
// PageData::inlineImages because the content it came from does not outlive interpretation.
enum class ImageState : uint8_t {
    Found,    // recorded by the interpreter
    Skipped,  // decoration: too small, a page background, or repeated on many pages
    Written,  // `name` holds its Markdown link target
    Failed,   // `name` holds why it could not be extracted
};

struct ImageBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // The CTM of the placement: image space (the unit square; the image's first row at its top,
    // y = 1) to the space of x0..y1, which is the placement's bounding box, clipped.
    float matrix[6] = {1, 0, 0, 1, 0, 0};
    PdfValue xobject;     // the image XObject stream; Null for an inline image
    PdfValue resources;   // the resources that named it (Null when none)
    uint32_t inlineData = 0, inlineLength = 0;  // an inline image's source in PageData::inlineImages
    uint32_t width = 0, height = 0;             // pixels, from /Width and /Height (0 when unknown)
    uint64_t identity = 0;  // the same for every placement of one XObject, or of equal inline data
    uint32_t name = 0, nameLength = 0;  // in PageData::text, set by PlaceImages
    ImageState state = ImageState::Found;
    int32_t line = -1;  // the text line it sits in (an inline image), or -1
    uint32_t glyph = UINT32_MAX;  // its glyph in that line (kGlyphImage), set by PlaceImages
    int32_t mcid = -1;  // marked-content id, -1 when none (counted as Glyph::mcid is)
    uint8_t mcStream = 0;
    uint32_t alt = 0, altLength = 0;  // alternate text in PageData::text (a tagged /Figure's /Alt), empty when none
    // Text the OCR add-on read in the image (region OCR, layout/ocr.h) when the image is written:
    // its alternate text where it has no /Alt. In PageData::text, empty when none.
    uint32_t ocr = 0, ocrLength = 0;
    // A table drawn as an image whose OCR text was read as a table (layout/ocr.h): its text is the
    // page's and it is no figure region (layout/figures.h). Such an image is also Skipped.
    bool ocrTable = false;
};

// A link annotation with a URI action.
struct LinkArea {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t uri = 0;  // offset in PageData::text
    uint32_t uriLength = 0;
};

// A straight axis-aligned piece of a painted path, as painted (a stroke widened by its line width,
// a filled rectangle as its box, a side of a filled polygon with no thickness): the raw material of
// table rulings (algorithms §6.7). A stroked rectangle is recorded as its four sides; pieces under
// half a point both ways (dots) are not recorded.
enum : uint8_t {
    kRulingStroke = 1u << 0,
    kRulingFill = 1u << 1,
};

struct Ruling {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t flags = 0;
};

// A table found by DetectTables (layout/tables.h): its grid and its cells. Cell text lives in lines
// of their own, PageData::lines[firstLine, firstLine + lineCount), grouped by cell in cell order.
struct TableCell {
    uint16_t row = 0, col = 0;
    uint16_t rowSpan = 1, colSpan = 1;
    uint32_t first = 0, count = 0;  // its lines: PageData::lines[first, first + count)
};

struct Table {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint16_t rows = 0, cols = 0;
    uint16_t headerRows = 0;        // rows above the header rule (0: the first row is taken as header)
    uint8_t kind = 0;               // TableKind (layout/tables.h)
    uint32_t firstCell = 0, cellCount = 0;  // PageData::tableCells[firstCell, firstCell + cellCount), row-major
    uint32_t firstLine = 0, lineCount = 0;
};

// Line flags.
enum : uint16_t {
    kLineRemoved = 1u << 0,   // a running header, footer or page number
    kLineAllBold = 1u << 1,
    kLineAllItalic = 1u << 2,
    kLineAllMono = 1u << 3,
    kLineArtifact = 1u << 4,  // every glyph inside /Artifact marked content
    kLineTable = 1u << 5,     // inside a ruled grid read row by row: no headings or lists
    kLineInvisible = 1u << 6, // every glyph drawn invisibly, not by the add-on (a PDF's own OCR layer): no typographic signals
    kLineTableCell = 1u << 7, // text of a table cell: written by its table, never ordered on its own
    // Stands for table `first` (PageData::tables) in reading order: no glyphs (count 0), its box is
    // the table's box. Code walking page.order meets lines with count 0.
    kLineTableAnchor = 1u << 8,
    // Stands for image `first` (PageData::images) in reading order: no glyphs (count 0), its box is
    // the image's box.
    kLineImage = 1u << 9,
    kLineRtl = 1u << 10,      // reordered to logical order; its paragraph direction is right-to-left
    // A display formula's lines merged into one (--math=latex only, layout/math.h): its glyphs are
    // those of the formula's lines in reading order; it makes a block of its own.
    kLineFormula = 1u << 11,
    kLineFigure = 1u << 12,   // stands in or beside a figure region (layout/figures.h): a figure's own text
    // Page furniture kept by Furniture::Keep (layout/convert.h): a line RemoveRunningLines marks,
    // kept as text. It heads nothing and makes no block with lines that are not furniture.
    kLineFurniture = 1u << 13,
    // A row the edge-row model takes for page furniture (layout/model.h, RemoveRunningLines): read
    // as text by every stage, so that no other line's role changes; ConvertPages leaves out the
    // paragraphs and headings it alone makes.
    kLineEdgeModel = 1u << 14,
};
constexpr uint16_t kLineAnchor = kLineTableAnchor | kLineImage;  // lines with no glyphs

// A line: glyphs of one direction on one baseline, in reading order along the baseline.
struct Line {
    float u0 = 0, u1 = 0, vb = 0, vt = 0;  // box in the direction frame
    float baseline = 0;
    float size = 0;          // the size of most of its characters
    uint32_t first = 0;      // its glyphs: PageData::lineGlyphs[first, first + count)
    uint32_t count = 0;
    uint32_t chars = 0;      // visible non-space characters
    uint16_t flags = 0;
    uint8_t dir = 0;
    uint8_t reserved = 0;
};

// The allocator of PageData::glyphs: std::allocator's, except that a record the vector grows by
// without a value (resize, emplace_back()) is left unset, for its writer to give every field: the
// interpreter grows the vector once per text operator by a record per byte and writes each record
// it keeps (a record's default fields would be written twice). Glyph is trivially copyable and
// destructible, so an unset record is storage the writer fills.
template <typename T>
struct UnsetAllocator : std::allocator<T> {
    template <typename U>
    struct rebind {
        using other = UnsetAllocator<U>;
    };
    UnsetAllocator() = default;
    template <typename U>
    UnsetAllocator(const UnsetAllocator<U>&) noexcept {}
    template <typename U>
    void construct(U*) noexcept {}
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
};
using GlyphVector = std::vector<Glyph, UnsetAllocator<Glyph>>;

// A math font's glyph without text, or any glyph of a mathabx font, and its code (PageData::mathCodes).
struct MathCode {
    uint32_t glyph = 0;  // index into PageData::glyphs
    uint8_t code = 0;
};

// A tagged Formula's LaTeX (layout/structure.h): its /Alt, else its /ActualText, when it reads as
// LaTeX (layout/math.h LatexAlt). --math=latex writes it in place of the formula's glyphs.
struct FormulaAlt {
    uint32_t alt = 0, altLength = 0;  // in PageData::text
    bool display = false;             // the element is a block of its own: $$…$$
};

struct PageData {
    int number = 0;              // 1-based page number
    float width = 0, height = 0; // upright page size
    GlyphVector glyphs;          // in content order
    std::string text;            // glyph texts and link URIs
    std::vector<Shape> shapes;
    std::vector<ImageBox> images;
    std::vector<LinkArea> links;
    std::vector<Ruling> rulings;
    std::string inlineImages;    // inline images' sources (ImageBox::inlineData)
    // Object numbers of the Form XObjects whose own marked-content ids glyphs and images carry
    // (Glyph::mcStream), at most kMaxMarkStreams.
    std::vector<uint32_t> markStreams;
    // Per page-local font (Glyph::font): its MathFont class (layout/math.h), from its name, with
    // the bits kMathFontNamed and kMathFontAbx; fonts past the end are text fonts.
    std::vector<uint8_t> fontMath;
    // Glyphs of math fonts left without text (kGlyphUnmapped) and every glyph of a mathabx font,
    // with their one-byte code: --math=latex reads them through TeX's font encodings (layout/math.h
    // PrepareMathGlyphs).
    std::vector<MathCode> mathCodes;
    // Tagged formulas with LaTeX alternate text (the tagged path's Prepare), and per glyph the
    // formula it belongs to: index + 1 into formulaAlts, 0 for none; empty when the page has none
    // (glyphs past its end belong to none).
    std::vector<FormulaAlt> formulaAlts;
    std::vector<uint32_t> glyphFormula;
    int contentErrors = 0;       // content streams not fully read or interpreted
    bool limitHit = false;       // the operator or glyph budget ran out

    // Built by BuildLines and OrderLines.
    std::vector<Line> lines;
    std::vector<uint32_t> lineGlyphs;  // glyph indices of the lines
    std::vector<uint32_t> order;       // indices of the lines left after header/footer removal, in reading order
    uint32_t hiddenGlyphs = 0;         // glyphs dropped as hidden, clipped or duplicate
    std::vector<Table> tables;          // built by DetectTables
    std::vector<TableCell> tableCells;
    FigureStrokes figureStrokes;        // kept by the first search for figure regions

    std::string_view GlyphText(const Glyph& g) const { return std::string_view(text).substr(g.text, g.textLength); }
    std::string_view LinkUri(const LinkArea& l) const { return std::string_view(text).substr(l.uri, l.uriLength); }
    std::string_view ImageName(const ImageBox& i) const { return std::string_view(text).substr(i.name, i.nameLength); }
    std::string_view ImageAlt(const ImageBox& i) const { return std::string_view(text).substr(i.alt, i.altLength); }
    std::string_view ImageOcrText(const ImageBox& i) const { return std::string_view(text).substr(i.ocr, i.ocrLength); }
    // The tagged formula (index + 1 into formulaAlts) glyph `glyph` belongs to, 0 for none.
    uint32_t FormulaOf(uint32_t glyph) const { return glyph < glyphFormula.size() ? glyphFormula[glyph] : 0; }
    std::string_view FormulaText(uint32_t formula) const {
        const FormulaAlt& f = formulaAlts[formula - 1];
        return std::string_view(text).substr(f.alt, f.altLength);
    }
    // Whether the ordered lines hold text (image anchors hold none).
    bool HasText() const {
        for (uint32_t li : order)
            if (lines[li].count > 0 || (lines[li].flags & kLineTableAnchor)) return true;
        return false;
    }

    void Clear() {
        glyphs.clear();
        text.clear();
        shapes.clear();
        images.clear();
        links.clear();
        rulings.clear();
        inlineImages.clear();
        markStreams.clear();
        fontMath.clear();
        mathCodes.clear();
        formulaAlts.clear();
        glyphFormula.clear();
        lines.clear();
        lineGlyphs.clear();
        order.clear();
        tables.clear();
        tableCells.clear();
        figureStrokes.Clear();
        contentErrors = 0;
        limitHit = false;
        hiddenGlyphs = 0;
    }
};

// A bookmark of the document outline (§12.3.3): its title as UTF-8 and its depth (1 = top level).
struct OutlineEntry {
    std::string title;
    int level = 1;
};

// A point of the frame of baseline direction `dir` (interpreter.cpp ToFrame) back on the upright
// page.
inline void FromFrame(uint8_t dir, float u, float v, float& x, float& y) {
    switch (dir) {
    case 1: x = -v, y = u; break;
    case 2: x = -u, y = -v; break;
    case 3: x = v, y = -u; break;
    default: x = u, y = v; break;
    }
}

// A rectangle of the frame of `dir` as the rectangle it covers on the upright page.
inline void RectFromFrame(uint8_t dir, float& x0, float& y0, float& x1, float& y1) {
    float ax, ay, bx, by;
    FromFrame(dir, x0, y0, ax, ay);
    FromFrame(dir, x1, y1, bx, by);
    x0 = std::min(ax, bx), x1 = std::max(ax, bx), y0 = std::min(ay, by), y1 = std::max(ay, by);
}

// A box on the upright page.
struct UprightBox {
    float x0, y0, x1, y1;
};

// A glyph's box on the upright page: its frame box, turned back for directions other than 0.
inline UprightBox GlyphPageBox(const Glyph& g) {
    UprightBox box{g.u0, g.vb, g.u1, g.vt};
    if (g.dir != 0) RectFromFrame(g.dir, box.x0, box.y0, box.x1, box.y1);
    return box;
}

// The box of an inline image's glyph `glyph` (kGlyphImage): its image's, else its own.
RAYOPDF_NOINLINE inline UprightBox ImageGlyphPageBox(const PageData& page, uint32_t glyph) {
    for (const ImageBox& image : page.images)
        if (image.glyph == glyph) return UprightBox{image.x0, image.y0, image.x1, image.y1};
    return GlyphPageBox(page.glyphs[glyph]);
}

// The box on the upright page of glyph `glyph` of `page`: an inline image's glyph (kGlyphImage)
// stands in its line with the line's height, but covers its image's box.
RAYOPDF_HOT_INLINE UprightBox GlyphPageBox(const PageData& page, uint32_t glyph) {
    const Glyph& g = page.glyphs[glyph];
    if (RAYOPDF_UNLIKELY(g.flags & kGlyphImage)) return ImageGlyphPageBox(page, glyph);
    return GlyphPageBox(g);
}

} // namespace RayoPdf::Layout
