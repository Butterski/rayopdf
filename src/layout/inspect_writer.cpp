// SPDX-License-Identifier: Apache-2.0
#include "layout/inspect_writer.h"

#include "common/minmax.h"
#include "common/utf8.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#ifndef RAYOPDF_VERSION
#define RAYOPDF_VERSION "0.0.0"
#endif

namespace RayoPdf::Layout {
namespace {

constexpr const char* kStyle =
    "body{margin:16px;background:#e8e8e8;font:14px sans-serif;color:#222}\n"
    "section{margin:0 0 32px}\n"
    "h2{font-size:16px;margin:0 0 6px}\n"
    "svg{display:block;background:#fff;box-shadow:0 0 4px #888;max-width:100%;height:auto}\n"
    "svg text{font-family:sans-serif;fill:#222;white-space:pre}\n"
    "svg text.removed{fill:#999;text-decoration:line-through}\n"
    "svg text.num{fill:#000;font-weight:bold}\n"
    "svg text.name{fill:#555}\n"
    "rect{fill:none;stroke-width:0.8}\n"
    "rect.cell{stroke:#aaa;stroke-dasharray:2 1.5;stroke-width:0.5}\n"
    "rect.image{stroke:#777;fill:rgba(120,120,120,0.12)}\n"
    "rect.t-heading{stroke:#c62828;fill:rgba(198,40,40,0.08)}\n"
    "rect.t-paragraph{stroke:#1565c0;fill:rgba(21,101,192,0.06)}\n"
    "rect.t-list_item{stroke:#2e7d32;fill:rgba(46,125,50,0.08)}\n"
    "rect.t-code{stroke:#6a1b9a;fill:rgba(106,27,154,0.08)}\n"
    "rect.t-table{stroke:#ef6c00;fill:rgba(239,108,0,0.06)}\n"
    "rect.t-image{stroke:#00838f;fill:rgba(0,131,143,0.08)}\n"
    "rect.t-footnote{stroke:#795548;fill:rgba(121,85,72,0.08)}\n"
    "rect.t-comment{stroke:#999;fill:rgba(153,153,153,0.08)}\n"
    "polyline.order{fill:none;stroke:#0d47a1;stroke-width:0.7;stroke-opacity:0.6}\n"
    "p.note{color:#a00}\n"
    ".legend span{display:inline-block;margin-right:12px;padding:0 4px;border:1px solid}\n";

// A number as `%.2f` writes it (0 for a value that is not finite; -0 as 0).
void AppendNumber(std::string& out, float value) {
    char buf[48];
    const double number = std::isfinite(value) ? static_cast<double>(value) + 0.0 : 0.0;
    out.append(buf, std::to_chars(buf, buf + sizeof buf, number, std::chars_format::fixed, 2).ptr);
}

// Appends `text` escaped for HTML text and attribute values, at most `limit` characters (then
// "…"): control characters as spaces, malformed UTF-8 as U+FFFD.
void AppendEscaped(std::string& out, std::string_view text, size_t limit = SIZE_MAX) {
    size_t at = 0, length = 0, count = 0;
    uint32_t cp = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        if (count++ == limit) {
            out += "\xE2\x80\xA6";
            break;
        }
        switch (cp) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default:
            if (cp < 0x20 || cp == 0x7F)
                out.push_back(' ');
            else
                Utf8::Append(out, cp);
        }
        at += length;
    }
}

// x, y, width and height attributes of a box.
void AppendBox(std::string& out, float x0, float y0, float x1, float y1) {
    out += " x=\"";
    AppendNumber(out, x0);
    out += "\" y=\"";
    AppendNumber(out, y0);
    out += "\" width=\"";
    AppendNumber(out, Fmax(0.0f, x1 - x0));
    out += "\" height=\"";
    AppendNumber(out, Fmax(0.0f, y1 - y0));
    out.push_back('"');
}

// The start of a <text> element whose text runs from (x, y) along the frame of `dir`, upright on
// the screen inside the flipped page (its local y axis points down the frame's v axis).
void OpenText(std::string& out, const char* cls, uint8_t dir, float x, float y, float size) {
    float ux, uy, vx, vy;
    FromFrame(dir, 1, 0, ux, uy);
    FromFrame(dir, 0, 1, vx, vy);
    out += "<text";
    if (cls) {
        out += " class=\"";
        out += cls;
        out.push_back('"');
    }
    out += " transform=\"matrix(";
    const float matrix[6] = {ux, uy, -vx, -vy, x, y};
    for (int i = 0; i < 6; ++i) {
        AppendNumber(out, matrix[i]);
        out.push_back(i < 5 ? ' ' : ')');
    }
    out += "\" font-size=\"";
    AppendNumber(out, size);
    out.push_back('"');
}

// A block box of the inspector: block `index` (from 0, output order), on page `page`.
struct DrawnBox {
    uint32_t page;
    uint32_t index;
    float x0, y0, x1, y1;
};

// One page: its lines, table cells, images, block boxes and the reading-order polyline.
void WritePage(const PageData& page, const DrawnBox* boxes, size_t boxCount, const BlockList& list,
               std::string_view markdown, const std::vector<WrittenBlock>& written, std::string& text,
               std::string& out) {
    const std::string number = std::to_string(page.number);
    out += "<section id=\"page-";
    out += number;
    out += "\">\n<h2>Page ";
    out += number;
    out += "</h2>\n<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ";
    AppendNumber(out, page.width);
    out.push_back(' ');
    AppendNumber(out, page.height);
    out += "\" width=\"";
    AppendNumber(out, page.width);
    out += "\" height=\"";
    AppendNumber(out, page.height);
    out += "\">\n<g transform=\"matrix(1 0 0 -1 0 ";
    AppendNumber(out, page.height);
    out += ")\">\n";

    // Table cells: the union of their lines' boxes.
    for (const Table& table : page.tables) {
        for (uint32_t c = 0; c < table.cellCount && table.firstCell + c < page.tableCells.size(); ++c) {
            const TableCell& cell = page.tableCells[table.firstCell + c];
            bool any = false;
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            for (uint32_t l = cell.first; l < cell.first + cell.count && l < page.lines.size(); ++l) {
                const Line& line = page.lines[l];
                float a0 = line.u0, b0 = line.vb, a1 = line.u1, b1 = line.vt;
                RectFromFrame(line.dir, a0, b0, a1, b1);
                x0 = any ? Fmin(x0, a0) : a0, y0 = any ? Fmin(y0, b0) : b0;
                x1 = any ? Fmax(x1, a1) : a1, y1 = any ? Fmax(y1, b1) : b1;
                any = true;
            }
            if (!any) continue;
            out += "<rect class=\"cell\"";
            AppendBox(out, x0, y0, x1, y1);
            out += "/>\n";
        }
    }

    // Images, with their file names.
    for (const ImageBox& image : page.images) {
        out += "<rect class=\"image\"";
        AppendBox(out, image.x0, image.y0, image.x1, image.y1);
        out += "/>\n";
        if (image.nameLength == 0) continue;
        OpenText(out, "name", 0, image.x0 + 2, image.y1 - 9, 7);
        out.push_back('>');
        AppendEscaped(out, page.ImageName(image));
        out += "</text>\n";
    }

    // Lines: their text at their baseline, as wide as their box.
    uint32_t drawn = 0, skipped = 0;
    for (const Line& line : page.lines) {
        if (line.count == 0) continue;
        if (drawn == kInspectMaxLines) {
            ++skipped;
            continue;
        }
        ++drawn;
        text.clear();
        const Glyph* previous = nullptr;
        for (uint32_t k = 0; k < line.count && line.first + k < page.lineGlyphs.size(); ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            if (g.flags & kGlyphImage) continue;
            // A space where the glyphs leave a gap (lines may hold no space glyphs).
            if (previous && !((previous->flags | g.flags) & kGlyphSpace) &&
                Fmax(g.u0 - previous->u1, previous->u0 - g.u1) > 0.15f * line.size)
                text.push_back(' ');
            text += page.GlyphText(g);
            previous = &g;
        }
        float x, y;
        FromFrame(line.dir, line.u0, line.baseline, x, y);
        OpenText(out, (line.flags & kLineRemoved) ? "removed" : nullptr, line.dir, x, y, Fmax(1.0f, line.size));
        out += " textLength=\"";
        AppendNumber(out, Fmax(0.0f, line.u1 - line.u0));
        out += "\" lengthAdjust=\"spacingAndGlyphs\">";
        AppendEscaped(out, text);
        out += "</text>\n";
    }

    // Blocks: boxes by type, numbered in reading order, and the polyline through their centres.
    for (size_t b = 0; b < boxCount; ++b) {
        const DrawnBox& box = boxes[b];
        const WrittenBlock& entry = written[box.index];
        const Block& block = list.blocks[entry.block];
        const char* type = BlockTypeName(block.kind);
        const std::string k = std::to_string(box.index + 1);
        out += "<rect class=\"t-";
        out += type;
        out.push_back('"');
        AppendBox(out, box.x0, box.y0, box.x1, box.y1);
        out += "><title>#";
        out += k;
        out.push_back(' ');
        out += type;
        if (block.kind == BlockKind::Heading) {
            out.push_back(' ');
            out += std::to_string(std::max(1, std::min<int>(6, block.level)));
        }
        out += "&#10;";
        AppendEscaped(out, markdown.substr(entry.begin, entry.end - entry.begin), 120);
        out += "</title></rect>\n";
        OpenText(out, "num", 0, box.x0 + 1, box.y1 - 7, 7);
        out.push_back('>');
        out += k;
        out += "</text>\n";
    }
    if (boxCount > 1) {
        out += "<polyline class=\"order\" points=\"";
        for (size_t b = 0; b < boxCount; ++b) {
            if (b > 0) out.push_back(' ');
            AppendNumber(out, 0.5f * (boxes[b].x0 + boxes[b].x1));
            out.push_back(',');
            AppendNumber(out, 0.5f * (boxes[b].y0 + boxes[b].y1));
        }
        out += "\"/>\n";
    }
    out += "</g>\n</svg>\n";
    if (skipped > 0) {
        out += "<p class=\"note\">";
        out += std::to_string(skipped);
        out += " more lines not drawn (at most ";
        out += std::to_string(kInspectMaxLines);
        out += " a page).</p>\n";
    }
    out += "</section>\n";
}

} // namespace

void WriteInspect(const std::vector<PageData>& pages, const BlockList& list, std::string_view markdown,
                  const std::vector<WrittenBlock>& written, std::string& out) {
    // Every block's boxes, grouped by page in output order.
    std::vector<DrawnBox> drawn;
    std::vector<PageBox> boxes;
    for (size_t w = 0; w < written.size(); ++w) {
        BlockBoxes(pages, list, list.blocks[written[w].block], boxes);
        for (const PageBox& box : boxes)
            drawn.push_back(DrawnBox{box.page, static_cast<uint32_t>(w), box.x0, box.y0, box.x1, box.y1});
    }
    std::stable_sort(drawn.begin(), drawn.end(),
                     [](const DrawnBox& a, const DrawnBox& b) { return a.page < b.page; });
    out += "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<title>RayoPDF layout inspector</title>\n<style>\n";
    out += kStyle;
    out += "</style>\n</head>\n<body>\n<h1>RayoPDF layout inspector</h1>\n<p>rayopdf " RAYOPDF_VERSION ", ";
    out += std::to_string(pages.size());
    out += pages.size() == 1 ? " page, " : " pages, ";
    out += std::to_string(written.size());
    out += written.size() == 1 ? " block" : " blocks";
    out += " numbered in reading order (hover a box for its type and Markdown).</p>\n<p class=\"legend\">"
           "<span style=\"border-color:#c62828\">heading</span><span style=\"border-color:#1565c0\">paragraph</span>"
           "<span style=\"border-color:#2e7d32\">list item</span><span style=\"border-color:#6a1b9a\">code</span>"
           "<span style=\"border-color:#ef6c00\">table</span><span style=\"border-color:#00838f\">image</span>"
           "<span style=\"border-color:#795548\">footnote</span><span style=\"border-color:#999\">removed line"
           "</span></p>\n";
    std::string text;
    size_t at = 0;
    for (size_t p = 0; p < pages.size(); ++p) {
        const size_t from = at;
        while (at < drawn.size() && drawn[at].page == p) ++at;
        WritePage(pages[p], drawn.data() + from, at - from, list, markdown, written, text, out);
    }
    out += "</body>\n</html>\n";
}

} // namespace RayoPdf::Layout
