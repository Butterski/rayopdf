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

#include <cstdint>
#include <string>
#include <string_view>
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
};

struct Glyph {
    // Box on the upright page.
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // Box in the direction frame: u0..u1 along the baseline (origin to origin + advance), vb..vt
    // from descent to ascent; `v` is the baseline.
    float u0 = 0, u1 = 0, vb = 0, vt = 0, v = 0;
    float size = 0;          // effective font size in points
    uint32_t text = 0;       // offset of its UTF-8 text in PageData::text
    uint16_t textLength = 0;
    uint16_t flags = 0;
    uint8_t dir = 0;         // baseline direction: 0 left-to-right, 1 upwards, 2 right-to-left, 3 downwards
    uint8_t reserved = 0;
    uint16_t font = 0;       // page-local font index, for style comparisons
    int32_t mcid = -1;       // marked-content id, -1 when none
};

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

// An image placed on the page (an image XObject or an inline image).
struct ImageBox {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// A link annotation with a URI action.
struct LinkArea {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t uri = 0;  // offset in PageData::text
    uint32_t uriLength = 0;
};

// Line flags.
enum : uint16_t {
    kLineRemoved = 1u << 0,   // a running header, footer or page number
    kLineAllBold = 1u << 1,
    kLineAllItalic = 1u << 2,
    kLineAllMono = 1u << 3,
    kLineArtifact = 1u << 4,  // every glyph inside /Artifact marked content
    kLineTable = 1u << 5,     // inside a ruled grid read row by row: no headings or lists
    kLineInvisible = 1u << 6, // every glyph drawn invisibly (an OCR layer): no typographic signals
};

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

struct PageData {
    int number = 0;              // 1-based page number
    float width = 0, height = 0; // upright page size
    std::vector<Glyph> glyphs;   // in content order
    std::string text;            // glyph texts and link URIs
    std::vector<Shape> shapes;
    std::vector<ImageBox> images;
    std::vector<LinkArea> links;
    int contentErrors = 0;       // content streams not fully read or interpreted
    bool limitHit = false;       // the operator or glyph budget ran out

    // Built by BuildLines and OrderLines.
    std::vector<Line> lines;
    std::vector<uint32_t> lineGlyphs;  // glyph indices of the lines
    std::vector<uint32_t> order;       // indices of the lines left after header/footer removal, in reading order
    uint32_t hiddenGlyphs = 0;         // glyphs dropped as hidden, clipped or duplicate

    std::string_view GlyphText(const Glyph& g) const { return std::string_view(text).substr(g.text, g.textLength); }
    std::string_view LinkUri(const LinkArea& l) const { return std::string_view(text).substr(l.uri, l.uriLength); }

    void Clear() {
        glyphs.clear();
        text.clear();
        shapes.clear();
        images.clear();
        links.clear();
        lines.clear();
        lineGlyphs.clear();
        order.clear();
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

} // namespace RayoPdf::Layout
