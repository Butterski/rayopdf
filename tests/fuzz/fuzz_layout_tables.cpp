// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: table detection (layout/tables.h), figure regions (layout/figures.h) and the
// table writers. The input is decoded into up to three pages of glyphs, rulings, drawn shapes and
// image boxes (positions on a coarse grid so that rows, columns and rulings line up often, plus raw
// floats including NaN and infinities), then the stages after interpretation run on them: lines,
// running-line removal, DetectTables page after page with its continuation, figure text, reading
// order, semantics and the Markdown writer (GFM and HTML tables).
#include "layout/convert.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace RayoPdf::Layout;

namespace {

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t at = 0;
    bool Empty() const { return at >= size; }
    uint8_t Byte() { return at < size ? data[at++] : 0; }
    // A coordinate: mostly on a 4 pt grid over the page, sometimes any float bit pattern.
    float Coord() {
        const uint8_t mode = Byte();
        if (mode < 240) return float(Byte()) * 4.0f + float(mode % 4);
        uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) bits = bits << 8 | Byte();
        float f;
        std::memcpy(&f, &bits, sizeof f);
        return f;
    }
};

void AddGlyph(PageData& page, char c, float x, float y, float size, uint16_t flags) {
    Glyph g;
    const float w = 0.5f * size;
    g.x0 = x, g.x1 = x + w, g.y0 = y - 0.2f * size, g.y1 = y + 0.8f * size;
    g.u0 = g.x0, g.u1 = g.x1, g.vb = g.y0, g.vt = g.y1, g.v = y;
    g.size = size;
    g.text = static_cast<uint32_t>(page.text.size());
    page.text.push_back(c);
    g.textLength = 1;
    g.flags = flags;
    if (c == ' ') g.flags |= kGlyphSpace;
    page.glyphs.push_back(g);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1 << 16) return 0;
    Reader in{data, size};
    std::vector<PageData> pages(1 + in.Byte() % 3);
    for (size_t p = 0; p < pages.size(); ++p) {
        pages[p].number = static_cast<int>(p + 1);
        pages[p].width = 612;
        pages[p].height = 792;
    }
    static const char kChars[] = "Aa1.-|*  xyz09%$:()Fig";  // "Fig" starts a caption (MarkFigureText)
    size_t page = 0;
    while (!in.Empty()) {
        const uint8_t op = in.Byte();
        PageData& pd = pages[page];
        switch (op % 7) {
        case 0: {  // a word of glyphs
            float x = in.Coord();
            const float y = in.Coord();
            const float size = 4.0f + float(in.Byte() % 16);
            const uint16_t flags = static_cast<uint16_t>(in.Byte() & (kGlyphBold | kGlyphItalic | kGlyphMonospace));
            const int n = 1 + in.Byte() % 12;
            for (int i = 0; i < n && pd.glyphs.size() < 4096; ++i) {
                AddGlyph(pd, kChars[in.Byte() % (sizeof kChars - 1)], x, y, size, flags);
                x += 0.5f * size;
            }
            break;
        }
        case 1:    // a horizontal ruling
        case 2: {  // a vertical ruling
            const float a = in.Coord(), b = in.Coord(), at = in.Coord(), w = float(in.Byte() % 8) * 0.25f;
            Ruling r;
            r.flags = static_cast<uint8_t>(1 + in.Byte() % 3);
            if (op % 6 == 1)
                r.x0 = a, r.x1 = b, r.y0 = at - w, r.y1 = at + w;
            else
                r.y0 = a, r.y1 = b, r.x0 = at - w, r.x1 = at + w;
            if (pd.rulings.size() < 8192) pd.rulings.push_back(r);
            break;
        }
        case 3: {  // a box: a filled area or a stroked frame
            Ruling r{in.Coord(), in.Coord(), in.Coord(), in.Coord(), static_cast<uint8_t>(1 + in.Byte() % 3)};
            if (pd.rulings.size() < 8192) pd.rulings.push_back(r);
            break;
        }
        case 4: {  // a grid of rulings and a row of cells in it
            const float x = float(in.Byte()) * 2, y = float(in.Byte()) * 3;
            const int rows = 1 + in.Byte() % 8, cols = 1 + in.Byte() % 6;
            const float cw = 20.0f + float(in.Byte() % 60), rh = 10.0f + float(in.Byte() % 20);
            const uint8_t skip = in.Byte();
            for (int r = 0; r <= rows && pd.rulings.size() < 8192; ++r)
                pd.rulings.push_back(Ruling{x, y - float(r) * rh, x + float(cols) * cw, y - float(r) * rh + 0.5f, kRulingStroke});
            for (int c = 0; c <= cols && pd.rulings.size() < 8192; ++c)
                if (!(skip >> (c % 8) & 1))
                    pd.rulings.push_back(Ruling{x + float(c) * cw, y - float(rows) * rh, x + float(c) * cw + 0.5f, y, kRulingStroke});
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols && pd.glyphs.size() < 4096; ++c)
                    AddGlyph(pd, kChars[in.Byte() % (sizeof kChars - 1)], x + float(c) * cw + 3, y - float(r + 1) * rh + 3, 9, 0);
            break;
        }
        case 5: {  // a drawn shape (a figure's stroke, curve or mark), or a placed image
            const uint8_t kind = in.Byte();
            const float x0 = in.Coord(), y0 = in.Coord(), x1 = in.Coord(), y1 = in.Coord();
            if (kind & 0x80) {
                ImageBox im;
                im.x0 = x0, im.y0 = y0, im.x1 = x1, im.y1 = y1;
                if (pd.images.size() < 256) pd.images.push_back(im);
                break;
            }
            Shape s;
            s.x0 = x0, s.y0 = y0, s.x1 = x1, s.y1 = y1;
            s.kind = static_cast<ShapeKind>(kind % 3);
            s.stroked = true, s.curved = (kind & 4) != 0;
            s.segments = static_cast<uint16_t>(1 + in.Byte() % 64);
            const int copies = 1 + (kind >> 3) % 8 * 32;  // up to 225 at once: overlapping strokes
            for (int k = 0; k < copies && pd.shapes.size() < 8192; ++k) pd.shapes.push_back(s);
            break;
        }
        default:  // the next page
            page = (page + 1) % pages.size();
            break;
        }
    }
    std::string markdown;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, markdown, report);
    return 0;
}
