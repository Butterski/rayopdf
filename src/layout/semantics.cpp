// SPDX-License-Identifier: Apache-2.0
#include "layout/semantics.h"

#include "common/minmax.h"
#include "layout/lines.h"
#include "layout/tables.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>

namespace RayoPdf::Layout {
namespace {

constexpr float kMonoShare = 0.8f;   // a code line: at least this share of monospace characters
constexpr float kHeadingSize = 1.12f; // a heading candidate: at least this times the body size
// The top heading level is a title (H1) from this times the body size up. Measured first levels:
// LaTeX \section 1.44 (1.31 at 11 pt), typst 1.40, LibreOffice (pandoc's reference.docx) 1.67,
// browsers 2.0; second levels: LaTeX \subsection and typst 1.20 (1.10 at 11 pt), LibreOffice 1.33.
constexpr float kTitleSize = 1.38f;
// A top level used once or twice above deeper levels is a title from this times the body size up.
constexpr float kSmallTitleSize = 1.25f;

struct Info {
    uint32_t page = 0, line = 0;
    float u0 = 0, u1 = 0, vb = 0, vt = 0, baseline = 0, size = 0;
    uint8_t dir = 0;
    uint16_t flags = 0;
    uint32_t chars = 0;
    float mono = 0;          // share of monospace characters
    Marker marker = Marker::None;
    uint32_t value = 0;      // enumerator value
    uint32_t skip = 0;       // marker glyphs
    float markerU0 = 0;      // where the marker starts (a vector bullet's left edge)
    float textU0 = 0;        // where the text after the marker starts
    uint8_t task = 0;        // 1 open, 2 done
    uint8_t quoteBars = 0;
    bool endsTerminal = false;
    uint32_t lastCp = 0;     // the last character
    bool startsLower = false;
    bool hasLetter = false;
    bool rowPeer = false;    // another line of its style shares its baseline (a table row)
    bool pageRef = false;    // a lone page number sits far to its right (a table-of-contents entry)
    bool verbatimMarker = false;  // a list marker Markdown cannot write ("(2)", "(iv)"): kept as text
    bool numbering = false;  // only a section or item number ("1.5.", "IV", "(3)")
    bool topBand = false;    // the page's topmost line, alone in its top 6 %
    float gapBelow = 0;      // a top-band line's gap to the next line below
    uint32_t note = 0;       // the number of the footnote the line starts or continues
    bool noteStart = false;
    uint32_t noteSkip = 0;   // marker glyphs of a note's first line
    bool listStart() const { return marker != Marker::None; }
};

uint32_t FirstCodePoint(std::string_view s) {
    if (s.empty()) return 0;
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) return c;
    uint32_t cp = 0;
    size_t n = 0;
    if ((c & 0xE0) == 0xC0) cp = c & 0x1F, n = 1;
    else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, n = 2;
    else if ((c & 0xF8) == 0xF0) cp = c & 0x07, n = 3;
    if (s.size() <= n) return 0xFFFD;
    for (size_t i = 1; i <= n; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    return cp;
}

// Lowercase letters: ASCII, Latin-1 and Latin Extended-A/B lowercase forms, Greek and Cyrillic
// lowercase blocks. Enough for continuation decisions.
bool IsLower(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 0xDF && cp <= 0xFF && cp != 0xF7) return true;
    if (cp >= 0x100 && cp <= 0x17F) return (cp & 1) == 1;
    if (cp >= 0x3B1 && cp <= 0x3C9) return true;
    if (cp >= 0x430 && cp <= 0x45F) return true;
    return false;
}

// The last code point of `s`.
uint32_t LastCodePoint(std::string_view s) {
    size_t i = s.size();
    while (i > 0) {
        --i;
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return FirstCodePoint(s.substr(i));
    }
    return 0;
}

bool IsTerminal(uint32_t cp) {
    return cp == '.' || cp == '!' || cp == '?' || cp == ':' || cp == ';' || cp == 0x2026 || cp == 0x3002 ||
           cp == 0xFF01 || cp == 0xFF1F;
}

// A caption: a label word and its number ("Figure 6.1.1: …", "Table 13.2. …", "Fig. A1",
// "TABLE IV"), in any letter case.
bool IsCaption(std::string_view t) {
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

// A GitHub alert's title: one alert word alone ("Note", "WARNING", "Tip:"), in any letter case.
uint8_t AlertOf(std::string_view t) {
    if (!t.empty() && t.back() == ':') t.remove_suffix(1);
    static const std::string_view kWords[] = {"note", "tip", "important", "warning", "caution"};
    for (size_t w = 0; w < 5; ++w) {
        const std::string_view word = kWords[w];
        if (t.size() != word.size()) continue;
        bool same = true;
        for (size_t i = 0; i < word.size() && same; ++i) {
            char c = t[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            same = c == word[i];
        }
        if (same) return static_cast<uint8_t>(w + 1);
    }
    return 0;
}

// A section of a paper's front matter by its first word ("Abstract", "ABSTRACT—", "Contents",
// "Keywords:"; "1 Introduction" is told apart by its number).
bool FrontMatter(std::string_view t) {
    static const std::string_view kWords[] = {"abstract", "contents", "introduction", "keywords", "summary",
                                              "preface",  "foreword", "acknowledgements", "acknowledgments",
                                              "references", "index"};
    char word[20];
    size_t n = 0;
    for (size_t i = 0; i < t.size() && n < sizeof(word); ++i) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c < 'a' || c > 'z') break;
        word[n++] = c;
    }
    const std::string_view first(word, n);
    for (const std::string_view w : kWords)
        if (first == w) return true;
    return false;
}

// The depth of a section number opening a heading: "4 Results" 1, "4.3. Ablation" 2,
// "A.1 Proofs" 2 (an appendix letter only before ".digit"); 0 when none ("2025 Report", "3D").
int SectionDepth(std::string_view t) {
    size_t i = 0;
    int depth = 0;
    if (t.size() >= 3 && t[0] >= 'A' && t[0] <= 'Z' && t[1] == '.' && t[2] >= '0' && t[2] <= '9') i = 2, depth = 1;
    while (depth < 8) {
        size_t digits = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9' && digits < 4) ++i, ++digits;
        if (digits == 0 || digits > 3) return 0;
        depth++;
        if (i + 1 < t.size() && t[i] == '.' && t[i + 1] >= '0' && t[i + 1] <= '9') {
            ++i;
            continue;
        }
        break;
    }
    if (i < t.size() && t[i] == '.') ++i;
    if (i >= t.size() || t[i] != ' ') return 0;
    while (i < t.size() && t[i] == ' ') ++i;
    return i < t.size() && ((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= 'a' && t[i] <= 'z') ||
                            static_cast<unsigned char>(t[i]) >= 0xC0)
               ? depth
               : 0;
}

// Text that titles nothing: display math (a math symbol from Mathematical Alphanumeric Symbols,
// Mathematical Operators, Supplemental Mathematical Operators or "=", and fewer than three letters
// of ordinary text, or more math symbols than letters: "𝑇(𝑛) = 𝑂(𝑛 log 𝑛)"; or four words and more
// of a letter or so each: a matrix row "h1 hn · · · h2") or fewer than two letters at all ("20%↑",
// "1.7X"). "Heading with ∇ ⋅ E" has words around its math and stays a possible heading. Letters
// are ASCII ones and every code point from U+00C0 up outside the symbol, punctuation, private-use
// and math blocks.
bool NoTitle(std::string_view t) {
    uint32_t math = 0, letters = 0, words = 0;
    size_t at = 0;
    bool space = true;
    while (at < t.size()) {
        const uint32_t cp = FirstCodePoint(t.substr(at));
        const unsigned char c = static_cast<unsigned char>(t[at]);
        at += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (space && cp != ' ') words++;
        space = cp == ' ';
        if ((cp >= 0x1D400 && cp <= 0x1D7FF) || (cp >= 0x2200 && cp <= 0x22FF) || (cp >= 0x2A00 && cp <= 0x2AFF) ||
            cp == '=')
            math++;
        else if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                 (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x2000 && cp <= 0x2BFF) &&
                  !(cp >= 0x3000 && cp <= 0x303F) && !(cp >= 0xE000 && cp <= 0xF8FF) && !(cp >= 0xFE00 && cp <= 0xFE0F) &&
                  !(cp >= 0xFF00 && cp <= 0xFF20) && cp != 0xFFFD))
            letters++;
    }
    return letters < 2 || (math > 0 && letters < 3) || math > letters || (words >= 4 && 2 * letters < 3 * words);
}

// Lowercase ASCII letters and digits plus every non-ASCII byte: the outline matching key.
void MatchKey(std::string_view text, std::string& key, bool stripNumbering) {
    key.clear();
    size_t i = 0;
    if (stripNumbering) {
        while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.' || text[i] == ' ')) ++i;
    }
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= 'A' && c <= 'Z')
            key.push_back(static_cast<char>(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || static_cast<unsigned char>(c) >= 0x80)
            key.push_back(c);
    }
}

// Small painted shapes near the left of lines: vector bullets, task boxes, quote bars.
struct Marks {
    std::vector<const Shape*> small;  // bullets and boxes
    std::vector<const Shape*> bars;   // thin vertical rules
};

void CollectMarks(const PageData& page, Marks& marks) {
    marks.small.clear();
    marks.bars.clear();
    for (const Shape& s : page.shapes) {
        const float w = s.x1 - s.x0, h = s.y1 - s.y0;
        if (w > 0.3f && h > 0.3f && w <= 14 && h <= 14 && w <= 2.5f * h && h <= 2.5f * w) {
            if (marks.small.size() < 4096) marks.small.push_back(&s);
        } else if (w <= 6 && h >= 6 && h >= 3 * w && h <= 0.5f * page.height && s.kind != ShapeKind::Other) {
            // A rule taller than half the page divides columns; it marks no quote.
            if (marks.bars.size() < 4096) marks.bars.push_back(&s);
        }
    }
}

// `bold` and `italic` go up by the line's bold and italic glyphs (BlockList::boldBody).
void LineInfo(const PageData& page, uint32_t pi, uint32_t li, const Marks& marks, Info& info, std::string& scratch,
              uint64_t& bold, uint64_t& italic) {
    const Line& line = page.lines[li];
    info = Info();
    info.page = pi;
    info.line = li;
    info.u0 = line.u0, info.u1 = line.u1, info.vb = line.vb, info.vt = line.vt;
    info.baseline = line.baseline, info.size = line.size, info.dir = line.dir;
    info.flags = line.flags;
    info.chars = line.chars;
    info.markerU0 = info.textU0 = line.u0;
    uint32_t mono = 0, boldHere = 0, italicHere = 0;
    bool letter = false;
    bool numbering = line.chars <= 8;
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        boldHere += (g.flags & kGlyphBold) != 0;
        italicHere += (g.flags & kGlyphItalic) != 0;
        // Once a letter is seen and the line is no numbering, only monospace glyphs need theirs.
        if (letter && !numbering && !(g.flags & kGlyphMonospace)) continue;
        const uint32_t cp = FirstCodePoint(page.GlyphText(g));
        // Mathematical alphanumerics (𝑥, 𝐸) are math whatever the font's pitch: no code.
        if ((g.flags & kGlyphMonospace) && !(cp >= 0x1D400 && cp <= 0x1D7FF)) mono++;
        if (!letter) letter = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp >= 0xC0;
        if (numbering && !((cp >= '0' && cp <= '9') || cp == '.' || cp == ')' || cp == '(' || cp == 'I' ||
                           cp == 'V' || cp == 'X' || (k == 0 && cp >= 'A' && cp <= 'Z')))
            numbering = false;
    }
    bold += boldHere;
    italic += italicHere;
    info.hasLetter = letter;
    info.numbering = numbering;
    info.mono = line.count ? float(mono) / float(line.count) : 0;
    const Glyph& firstGlyph = page.glyphs[page.lineGlyphs[line.first]];
    const Glyph& lastGlyph = page.glyphs[page.lineGlyphs[line.first + line.count - 1]];
    std::string_view lastText = page.GlyphText(lastGlyph);
    uint32_t last = LastCodePoint(lastText);
    // A closing quote or bracket after the punctuation still ends the sentence.
    if ((last == '"' || last == ')' || last == 0x201D || last == 0x2019 || last == '\'') && line.count >= 2) {
        const Glyph& before = page.glyphs[page.lineGlyphs[line.first + line.count - 2]];
        if (lastText.size() == 1 || last >= 0x80) last = LastCodePoint(page.GlyphText(before));
    }
    info.endsTerminal = IsTerminal(last);
    info.lastCp = last;

    // The first word: a marker?
    uint32_t wordEnd = 1;
    while (wordEnd < line.count &&
           !(page.glyphs[page.lineGlyphs[line.first + wordEnd]].flags & kGlyphSpaceBefore))
        ++wordEnd;
    scratch.clear();
    for (uint32_t k = 0; k < wordEnd; ++k) scratch.append(page.GlyphText(page.glyphs[page.lineGlyphs[line.first + k]]));
    uint32_t value = 0;
    Marker marker = ParseMarker(scratch, value);
    uint32_t skip = wordEnd;
    if (marker == Marker::None && wordEnd > 1) {
        // A bullet set tight against its text.
        const Marker single = ParseMarker(page.GlyphText(firstGlyph), value);
        if (single == Marker::Bullet || single == Marker::TaskOpen || single == Marker::TaskDone) {
            const std::string_view t = page.GlyphText(firstGlyph);
            if (t != "-" && t != "*" && t != "\xE2\x80\x93" && t != "\xE2\x80\x94") marker = single, skip = 1;
        }
    }
    // A marker needs text after it on this line, unless the marker stands alone.
    if (marker != Marker::None && (skip < line.count || line.count == skip)) {
        // "-" and "*" and dashes are markers only when followed by text.
        if ((marker == Marker::Bullet) && skip == line.count && (scratch == "-" || scratch == "*")) marker = Marker::None;
    }
    if (marker != Marker::None) {
        info.marker = marker;
        info.value = value;
        info.skip = skip;
        // "(2)" and "(iv)" have no Markdown form: the item keeps them as its text. ("(a)", LaTeX's
        // second enumerate level, stays an ordered item like "1." and "a.".)
        info.verbatimMarker = !scratch.empty() && scratch[0] == '(' && (marker == Marker::Number || marker == Marker::Roman);
        info.task = marker == Marker::TaskOpen ? 1 : marker == Marker::TaskDone ? 2 : 0;
        info.textU0 = skip < line.count ? page.glyphs[page.lineGlyphs[line.first + skip]].u0 : line.u1;
        // A task box right after a bullet: "• ☐ text".
        if (skip < line.count && marker == Marker::Bullet) {
            uint32_t v2 = 0;
            const Marker next = ParseMarker(page.GlyphText(page.glyphs[page.lineGlyphs[line.first + skip]]), v2);
            if (next == Marker::TaskOpen || next == Marker::TaskDone) {
                info.task = next == Marker::TaskOpen ? 1 : 2;
                info.skip = skip + 1;
                info.textU0 = info.skip < line.count ? page.glyphs[page.lineGlyphs[line.first + info.skip]].u0 : line.u1;
            }
        }
    }
    {
        const uint32_t cpFirst = FirstCodePoint(info.skip < line.count
                                                    ? page.GlyphText(page.glyphs[page.lineGlyphs[line.first + info.skip]])
                                                    : std::string_view());
        info.startsLower = IsLower(cpFirst);
    }
    if (line.flags & kLineRtl) {
        // Right-to-left paragraphs align, indent and hang their markers on the right: the
        // paragraph and list rules read them in a mirrored frame.
        info.u0 = -line.u1, info.u1 = -line.u0;
        info.markerU0 = -line.u1;
        info.textU0 = info.skip < line.count ? -page.glyphs[page.lineGlyphs[line.first + info.skip]].u1 : -line.u0;
        return;
    }
    if (line.dir != 0) return;
    // Vector marks left of the line (algorithms §6.3: small filled shapes are bullets; RayoMD draws
    // bullets and task boxes as paths).
    if (info.marker == Marker::None) {
        const Shape* box = nullptr;
        const Shape* bullet = nullptr;
        for (const Shape* s : marks.small) {
            const float w = s->x1 - s->x0, h = s->y1 - s->y0;
            const float cy = (s->y0 + s->y1) / 2;
            if (cy < line.vb || cy > line.vt) continue;
            if (s->x1 > line.u0 + 0.5f || line.u0 - s->x1 > 2.5f * line.size) continue;
            if (w > 0.9f * line.size || h > 0.9f * line.size) continue;
            // Filled, or a stroked circle (a hollow bullet ◦): a bullet. A larger stroked square: a
            // task box.
            const bool hollowDot = s->stroked && !s->filled && s->curved && w < 0.45f * line.size;
            if (((s->filled && !s->stroked) || hollowDot) && w <= 0.6f * line.size && w >= 0.12f * line.size) {
                if (!bullet || s->x0 > bullet->x0) bullet = s;
            } else if (s->stroked && w >= 0.4f * line.size) {
                if (!box) box = s;
            }
        }
        if (box) {
            info.marker = Marker::Bullet;
            info.task = 1;
            for (const Shape* s : marks.small) {
                if (s == box) continue;
                const float cx = (s->x0 + s->x1) / 2, cy = (s->y0 + s->y1) / 2;
                if (cx > box->x0 && cx < box->x1 && cy > box->y0 && cy < box->y1) info.task = 2;
            }
            info.markerU0 = box->x0;
        } else if (bullet) {
            // A dot of a dotted rule (a column separator drawn dot by dot) has another dot right
            // above or below it, closer than a line: no bullet.
            const float cx = (bullet->x0 + bullet->x1) / 2, cy = (bullet->y0 + bullet->y1) / 2;
            bool dotted = false;
            for (const Shape* s : marks.small) {
                if (s == bullet) continue;
                const float dy = std::fabs((s->y0 + s->y1) / 2 - cy);
                if (std::fabs((s->x0 + s->x1) / 2 - cx) < 0.5f && dy > 0 && dy < 0.8f * line.size) dotted = true;
            }
            if (!dotted) {
                info.marker = Marker::Bullet;
                info.markerU0 = bullet->x0;
            }
        }
    }
    // Quote bars: thin vertical rules to the left that cover the line (algorithms §6.5). A rule on
    // the right as well, however far, makes the line a table cell or the inside of a framed box (a
    // code listing's frame), not a quote.
    float barX[4];
    int bars = 0;
    bool rightRule = false;
    for (const Shape* s : marks.bars) {
        if (s->y0 > line.vb + 0.2f * line.size || s->y1 < line.vt - 0.2f * line.size) continue;
        if (s->x0 >= line.u1 - 0.5f) rightRule = true;
        if (s->x1 > line.u0 + 0.5f || line.u0 - s->x1 > 3 * line.size) continue;
        bool distinct = true;
        for (int b = 0; b < bars; ++b)
            if (std::fabs(barX[b] - s->x0) < 2) distinct = false;
        if (distinct && bars < 4) barX[bars++] = s->x0;
    }
    info.quoteBars = static_cast<uint8_t>(rightRule ? 0 : bars);
}

struct RawBlock {
    uint32_t first = 0, count = 0;  // infos
    float pitch = 0;
    float bodyU0 = 0;
    float right = 0;
    bool mono = false;
    bool bold = false;
    uint32_t note = 0;  // a footnote's lines
};

// `monoDocument`: the document is set in a monospace font, whose lines are prose, not code.
bool Continues(const RawBlock& b, const Info& first, const Info& last, const Info& next, bool monoDocument) {
    if (next.page != last.page || next.dir != last.dir) return false;
    // A section number set apart from its title ("1.5.   Migrant Workers") joins it.
    if (b.count == 1 && last.numbering && std::fabs(last.baseline - next.baseline) <= 0.3f * last.size &&
        next.u0 > last.u1 && next.u0 - last.u1 <= 4 * last.size && std::fabs(next.size - last.size) <= 0.15f * last.size)
        return true;
    if (next.listStart()) return false;
    if ((next.flags ^ last.flags) & kLineTable) return false;
    if ((next.flags | last.flags) & kLineAnchor) return false;
    if (next.quoteBars != last.quoteBars) return false;
    const float em = Fmax(last.size, next.size);
    // A size change starts a block, except in an OCR layer whose sizes are fitted word boxes.
    const bool ocr = (last.flags & next.flags & kLineInvisible) != 0;
    if (!ocr && std::fabs(next.size - last.size) > 0.15f * em) return false;
    const float pitch = last.baseline - next.baseline;
    if (pitch <= 0.3f * em) return false;
    const float limit = b.pitch > 0 ? Fmax(b.pitch * 1.3f, b.pitch + 0.25f * em) : 1.6f * em;
    if (pitch > limit) return false;
    if (next.u0 > last.u1 || next.u1 < last.u0) return false;
    const bool monoNext = next.mono >= kMonoShare && !monoDocument;
    if (b.mono != monoNext) return false;
    if (b.mono) return true;
    if (b.bold != ((next.flags & kLineAllBold) != 0)) return false;
    if (first.listStart()) {
        if (next.u0 >= first.markerU0 + 0.5f * em) return true;
        // Without a hanging indent the item still continues where its long last line broke
        // mid-sentence.
        return next.u0 >= first.markerU0 - 0.5f * em && !last.endsTerminal && last.u1 - last.u0 >= 25 * em;
    }
    const float right = Fmax(b.right, next.u1);
    // Lines centred on one axis (a title over two or three lines) are no indented paragraph start.
    const bool centred = std::fabs((next.u0 + next.u1) - (last.u0 + last.u1)) <= 0.5f * em &&
                         std::fabs(next.u0 - last.u0) >= 0.8f * em;
    if (!centred) {
        if (b.count >= 2) {
            // A first-line indent starts a paragraph.
            if (next.u0 > b.bodyU0 + 0.8f * em && std::fabs(last.u0 - b.bodyU0) < 0.5f * em) return false;
        } else if (next.u0 > last.u0 + 0.8f * em && last.endsTerminal) {
            return false;
        }
    }
    // A short line ending a sentence ends the paragraph.
    const float left = Fmin(b.bodyU0, next.u0);
    if (last.endsTerminal && last.u1 < right - Fmax(2.5f * em, 0.12f * (right - left))) return false;
    return true;
}

// The key of a footnote marker: its number (1-999), or 1000 + 8 * symbol + repeats for * † ‡ § ¶
// written up to three times; 0 for anything else.
uint32_t NoteKey(std::string_view t) {
    if (t.empty() || t.size() > 9) return 0;
    if (t[0] >= '1' && t[0] <= '9') {
        if (t.size() > 3) return 0;
        uint32_t value = 0;
        for (char c : t) {
            if (c < '0' || c > '9') return 0;
            value = value * 10 + static_cast<uint32_t>(c - '0');
        }
        return value;
    }
    static const std::string_view kSymbols[] = {"*", "\xE2\x80\xA0", "\xE2\x80\xA1", "\xC2\xA7", "\xC2\xB6"};
    for (uint32_t s = 0; s < 5; ++s) {
        const std::string_view symbol = kSymbols[s];
        uint32_t repeats = 0;
        size_t at = 0;
        while (at < t.size() && t.substr(at, symbol.size()) == symbol) at += symbol.size(), ++repeats;
        if (repeats > 0 && repeats <= 3 && at == t.size()) return 1000 + 8 * s + repeats;
    }
    return 0;
}

// Superscript glyphs [k, end) of `line` from `k` (at most 8), their text in `text`.
uint32_t SuperscriptRun(const PageData& page, const Line& line, uint32_t k, std::string& text) {
    text.clear();
    uint32_t end = k;
    while (end < line.count && end - k < 8) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + end]];
        if (!IsSuperscript(g, line) || (end > k && (g.flags & kGlyphSpaceBefore))) break;
        text.append(page.GlyphText(g));
        ++end;
    }
    return end;
}

// The line ends in a back link to its reference (↩, as HTML endnotes carry).
bool EndsInBackLink(const PageData& page, const Line& line) {
    for (uint32_t k = line.count; k-- > 0 && k + 3 > line.count;) {
        const std::string_view t = page.GlyphText(page.glyphs[page.lineGlyphs[line.first + k]]);
        if (t.substr(0, 3) == "\xE2\x86\xA9") return true;  // U+21A9
    }
    return false;
}

constexpr size_t kMaxNoteRefs = 1u << 14;
constexpr size_t kNoteScan = 64;  // references of one key looked at per note
constexpr uint64_t kBottomChecks = uint64_t(1) << 22;  // line comparisons AtBottom may make per document

// A page footnote sits below the text of its column: no line under it that it overlaps along the
// baseline is set larger, except in the page's last row (a running footer or page number left on
// a single page). Superscript-marked lines higher up are author affiliations or table notes, and
// stay where they are. Fails closed (no note) once `budget` runs out.
bool AtBottom(const std::vector<Info>& infos, uint32_t begin, uint32_t end, uint32_t at, uint64_t& budget) {
    const Info& note = infos[at];
    if (2 * uint64_t(end - begin) > budget) return false;
    budget -= 2 * uint64_t(end - begin);
    const auto under = [&](const Info& o) {
        return o.dir == note.dir && o.baseline < note.baseline && o.chars > 0 && o.u0 < note.u1 && o.u1 > note.u0;
    };
    float lowest = INFINITY;
    for (uint32_t j = begin; j < end; ++j)
        if (under(infos[j])) lowest = Fmin(lowest, infos[j].baseline);
    for (uint32_t j = begin; j < end; ++j) {
        const Info& o = infos[j];
        if (under(o) && o.size > 1.1f * note.size && o.baseline > lowest + 0.5f * o.size) return false;
    }
    return true;
}

constexpr uint64_t kRuleChecks = uint64_t(1) << 22;  // shapes RuleBetween may look at per document

// A horizontal rule (at most 3 points high) between two lines of one page that runs under both
// of them: the edge of a frame or a separator. Shapes looked at count against `budget`; once it
// runs out the answer is no.
bool RuleBetween(const std::vector<PageData>& pages, const BlockLine& a, const BlockLine& b, uint64_t& budget) {
    if (a.page != b.page) return false;
    const PageData& page = pages[a.page];
    if (page.shapes.size() > budget) return false;
    budget -= page.shapes.size();
    const Line& upper = page.lines[a.line];
    const Line& lower = page.lines[b.line];
    if (upper.dir != 0 || lower.dir != 0) return false;
    const float left = Fmin(upper.u0, lower.u0), right = Fmax(upper.u1, lower.u1);
    for (const Shape& s : page.shapes) {
        if (s.y1 - s.y0 > 3 || s.x0 > left + 0.5f || s.x1 < right - 0.5f) continue;
        const float y = (s.y0 + s.y1) / 2;
        if (y < upper.vb && y > lower.vt) return true;
    }
    return false;
}

// The baseline of a line's last glyph: a wrapped source line joined again in lines.cpp ends a row
// lower than it starts.
float LastRow(const PageData& page, const Line& line) {
    return line.count ? page.glyphs[page.lineGlyphs[line.first + line.count - 1]].v : line.baseline;
}

// Two code blocks of one page, `a` right above `b`, set apart by space that is no whole number of
// their line pitch (the smallest baseline step inside either): paragraph spacing between two
// listings, not blank lines inside one. At most 64 lines of each are looked at.
bool OffGrid(const std::vector<PageData>& pages, const std::vector<BlockLine>& lines, const Block& a, const Block& b) {
    if (a.count == 0 || b.count == 0) return false;
    const BlockLine& upper = lines[a.first + a.count - 1];
    const BlockLine& lower = lines[b.first];
    if (upper.page != lower.page) return false;
    const PageData& page = pages[upper.page];
    float pitch = INFINITY;
    for (const Block* blk : {&a, &b}) {
        const uint32_t from = blk->count > 64 ? blk->first + blk->count - 64 : blk->first;
        for (uint32_t l = from + 1; l < blk->first + blk->count; ++l) {
            if (lines[l - 1].page != upper.page || lines[l].page != upper.page) continue;
            const float step = LastRow(page, page.lines[lines[l - 1].line]) - page.lines[lines[l].line].baseline;
            if (step > 0) pitch = Fmin(pitch, step);
        }
    }
    if (!(pitch < INFINITY)) return false;
    const float steps = (LastRow(page, page.lines[upper.line]) - page.lines[lower.line].baseline) / pitch;
    if (!(steps > 0 && steps < 64)) return false;
    const float fraction = steps - std::floor(steps);
    return fraction > 0.25f && fraction < 0.75f;
}

// Footnotes (algorithms §6.9). References are superscript runs inside lines whose text is a note
// key. A note starts a line with a marker whose key matches an unused reference: a superscript
// marker any size (LibreOffice sets notes in the body size), or an enumerator ("1.") in a smaller
// size or before a back link (HTML endnotes), so that body-size numbered lists stay lists. A page
// footnote's reference sits on its page above it and the note below the text (AtBottom); an
// enumerated endnote's reference on an earlier page, and the line before the endnotes is no
// heading ("References" sections keep their list). Lines of the note's size right below it
// continue it.
void FindNotes(const std::vector<PageData>& pages, std::vector<Info>& infos, float body, std::vector<NoteRef>& out) {
    struct Ref {
        uint32_t info, page, glyph, count, key;
        float baseline, size;  // of the reference's line
        bool used;
    };
    std::vector<Ref> refs;
    std::unordered_map<uint32_t, std::vector<uint32_t>> byKey;
    std::string text;
    for (uint32_t i = 0; i < infos.size() && refs.size() < kMaxNoteRefs; ++i) {
        const PageData& page = pages[infos[i].page];
        const Line& line = page.lines[infos[i].line];
        uint32_t k = 0;
        while (k < line.count && IsSuperscript(page.glyphs[page.lineGlyphs[line.first + k]], line)) ++k;
        while (k < line.count && refs.size() < kMaxNoteRefs) {
            if (!IsSuperscript(page.glyphs[page.lineGlyphs[line.first + k]], line)) {
                ++k;
                continue;
            }
            const uint32_t end = SuperscriptRun(page, line, k, text);
            // A raised number after a math letter (𝑏², 𝑥ⁱ) is an exponent, not a reference.
            const uint32_t before =
                FirstCodePoint(page.GlyphText(page.glyphs[page.lineGlyphs[line.first + k - 1]]));
            const uint32_t key = before >= 0x1D400 && before <= 0x1D7FF ? 0 : NoteKey(text);
            if (key) {
                byKey[key].push_back(static_cast<uint32_t>(refs.size()));
                refs.push_back(Ref{i, infos[i].page, page.lineGlyphs[line.first + k], end - k, key, line.baseline,
                                   line.size, false});
            }
            k = end;
            while (k < line.count && IsSuperscript(page.glyphs[page.lineGlyphs[line.first + k]], line)) ++k;
        }
    }
    if (refs.empty()) return;

    uint32_t number = 0;
    uint32_t current = 0;  // the note being continued
    float noteSize = 0, lastBaseline = 0;
    uint32_t notePage = 0;
    uint32_t pageBegin = 0, pageEnd = 0;  // infos of the current page
    uint64_t budget = kBottomChecks;
    for (uint32_t i = 0; i < infos.size(); ++i) {
        Info& in = infos[i];
        const PageData& page = pages[in.page];
        const Line& line = page.lines[in.line];
        if (i >= pageEnd) {
            pageBegin = i;
            pageEnd = i + 1;
            while (pageEnd < infos.size() && infos[pageEnd].page == in.page) ++pageEnd;
        }
        // A marker? Sizes compare with the reference's line: notes may hold most of a short
        // document's characters, and so define its body size.
        uint32_t key = 0, skip = 0;
        bool enumerated = false, backLink = false;
        if (line.count >= 2) {
            const uint32_t end = SuperscriptRun(page, line, 0, text);
            if (end > 0 && end < line.count) {
                key = NoteKey(text);
                skip = end;
            } else if (in.marker == Marker::Number && in.skip < line.count && in.value >= 1 && in.value <= 999) {
                key = in.value;
                skip = in.skip;
                enumerated = true;
                // The back link ends the note's last line.
                for (uint32_t j = i; j < infos.size() && j < i + 8 && !backLink; ++j) {
                    if (j > i && (infos[j].listStart() || infos[j].page != in.page)) break;
                    backLink = EndsInBackLink(pages[infos[j].page], pages[infos[j].page].lines[infos[j].line]);
                }
            }
        }
        uint32_t match = UINT32_MAX;
        if (key) {
            auto it = byKey.find(key);
            if (it != byKey.end()) {
                // Endnotes need a non-heading line before them.
                bool endnoteContext = false;
                if (enumerated && i > 0) {
                    const Info& before = infos[i - 1];
                    endnoteContext = before.note != 0 ||
                                     (before.size < kHeadingSize * body &&
                                      !((before.flags & kLineAllBold) && before.chars <= 60 && !before.endsTerminal));
                }
                size_t scanned = 0;
                int bottom = -1;  // AtBottom, worked out once
                for (uint32_t r : it->second) {
                    if (refs[r].used) continue;
                    if (++scanned > kNoteScan || refs[r].info >= i) break;
                    const Ref& ref = refs[r];
                    if (in.size > 1.05f * ref.size || (enumerated && !backLink && in.size > 0.92f * ref.size)) continue;
                    bool samePage = ref.page == in.page && ref.baseline > in.baseline;
                    if (samePage) {
                        if (bottom < 0) bottom = AtBottom(infos, pageBegin, pageEnd, i, budget) ? 1 : 0;
                        samePage = bottom == 1;
                    }
                    const bool endnote = enumerated && ref.page < in.page && endnoteContext;
                    if (samePage || endnote) {
                        match = r;
                        break;
                    }
                }
            }
        }
        if (match != UINT32_MAX) {
            Ref& r = refs[match];
            r.used = true;
            out.push_back(NoteRef{r.page, r.glyph, r.count, ++number});
            in.note = number;
            in.noteStart = true;
            in.noteSkip = skip;
            current = number;
            noteSize = in.size;
            lastBaseline = in.baseline;
            notePage = in.page;
            continue;
        }
        if (current && line.count > 0 && in.page == notePage && !in.listStart() &&
            std::fabs(in.size - noteSize) <= 0.15f * noteSize &&
            in.baseline < lastBaseline && lastBaseline - in.baseline <= 2.4f * noteSize) {
            in.note = current;
            lastBaseline = in.baseline;
            continue;
        }
        current = 0;
    }
    std::sort(out.begin(), out.end(), [](const NoteRef& a, const NoteRef& b) {
        return a.page != b.page ? a.page < b.page : a.glyph < b.glyph;
    });
}
} // namespace

void BuildBlocks(const std::vector<PageData>& pages, const std::vector<OutlineEntry>& outline,
                 const std::vector<uint8_t>& unmappedPages, BlockList& out) {
    std::vector<Block>& blocks = out.blocks;
    std::vector<BlockLine>& lines = out.lines;
    blocks.clear();
    lines.clear();
    out.noteRefs.clear();

    // Line facts in reading order.
    std::vector<Info> infos;
    Marks marks;
    std::string scratch;
    std::vector<uint32_t> byBase;
    // Glyphs of the ordered lines, and those of them bold and italic (BlockList::boldBody).
    uint64_t faceGlyphs = 0, boldGlyphs = 0, italicGlyphs = 0;
    for (uint32_t p = 0; p < pages.size(); ++p) {
        const PageData& page = pages[p];
        CollectMarks(page, marks);
        const size_t pageFirst = infos.size();
        for (uint32_t li : page.order) {
            infos.emplace_back();
            const Line& tl = page.lines[li];
            faceGlyphs += tl.count;
            if (tl.flags & kLineAnchor) {
                // A table (layout/tables.h) or an image (layout/images.h): no glyphs, only its
                // place and box.
                Info& t = infos.back();
                t.page = p, t.line = li, t.dir = tl.dir, t.flags = tl.flags;
                t.u0 = tl.u0, t.u1 = tl.u1, t.vb = tl.vb, t.vt = tl.vt, t.baseline = tl.baseline, t.size = tl.size;
                t.markerU0 = t.textU0 = tl.u0;
                t.endsTerminal = true;
                // Its glyphs, should it have any, count for the faces like any line's.
                for (uint32_t k = 0; k < tl.count; ++k) {
                    const uint16_t f = page.glyphs[page.lineGlyphs[tl.first + k]].flags;
                    boldGlyphs += (f & kGlyphBold) != 0;
                    italicGlyphs += (f & kGlyphItalic) != 0;
                }
                continue;
            }
            LineInfo(page, p, li, marks, infos.back(), scratch, boldGlyphs, italicGlyphs);
        }
        // Row peers: short lines of the same style side by side on one baseline are cells of a row
        // (a table header), not headings; a heading beside the other column's text has no peer. A
        // line with a lone number far to its right on its baseline is a table-of-contents entry
        // and its page, not a heading either.
        byBase.clear();
        for (size_t i = pageFirst; i < infos.size(); ++i) byBase.push_back(static_cast<uint32_t>(i));
        std::sort(byBase.begin(), byBase.end(), [&](uint32_t a, uint32_t b) {
            return infos[a].baseline > infos[b].baseline;
        });
        for (size_t k = 0; k < byBase.size(); ++k) {
            Info& a = infos[byBase[k]];
            for (size_t o = k + 1; o < byBase.size(); ++o) {
                Info& b = infos[byBase[o]];
                if (a.baseline - b.baseline > 0.3f * a.size) break;
                if (a.dir == b.dir && a.numbering != b.numbering) {
                    Info& entry = a.numbering ? b : a;
                    const Info& number = a.numbering ? a : b;
                    if (number.chars <= 4 && number.u0 - entry.u1 >= 3 * entry.size) entry.pageRef = true;
                }
                if (a.dir != b.dir || std::fabs(a.size - b.size) > 0.03f * a.size) continue;
                if (a.chars > 60 || b.chars > 60 || a.numbering || b.numbering) continue;
                if (((a.flags ^ b.flags) & kLineAllBold) || !(b.u0 > a.u1 || b.u1 < a.u0)) continue;
                if ((a.flags | b.flags) & kLineAnchor) continue;
                a.rowPeer = b.rowPeer = true;
            }
        }
        // The page's topmost line, alone in the top 6 % of the page: a running head when set in
        // the body face with a wide gap below it (decided once the body size is known), even on a
        // page of its own, where no repeat on other pages removed it.
        if (!byBase.empty()) {
            Info& top = infos[byBase[0]];
            const Info* below = byBase.size() > 1 ? &infos[byBase[1]] : nullptr;
            if (!(top.flags & kLineAnchor) && top.dir == 0 && page.height - top.vt <= 0.06f * page.height &&
                (!below || top.baseline - below->baseline > 0.5f * top.size)) {
                top.topBand = true;
                top.gapBelow = below ? top.vb - below->vt : INFINITY;
            }
        }
    }

    // Body size: the most common size by characters (algorithms §6.1), of prose lines: code blocks
    // set in a smaller monospace size do not define it. Table cells vote too: on a page that is
    // mostly a table, the few lines outside it are its title and labels, not the body.
    // Document-wide styles.
    std::unordered_map<int, uint64_t> sizeChars, proseChars;
    uint64_t totalChars = 0, monoChars = 0, boldChars = 0;
    uint64_t proseLineChars = 0;  // characters of lines in a proportional font, outside tables
    uint64_t cellChars = 0;       // characters of table cells
    for (const Info& in : infos) {
        const int key = static_cast<int>(std::lround(in.size * 2));
        sizeChars[key] += in.chars;
        if (in.mono < 0.5f) proseChars[key] += in.chars, proseLineChars += in.chars;
        totalChars += in.chars;
        monoChars += static_cast<uint64_t>(in.mono * float(in.chars));
        if (in.flags & kLineAllBold) boldChars += in.chars;
    }
    for (const PageData& page : pages)
        for (const Line& l : page.lines)
            if ((l.flags & (kLineTableCell | kLineRemoved)) == kLineTableCell && !(l.flags & kLineAllMono)) {
                const int key = static_cast<int>(std::lround(l.size * 2));
                sizeChars[key] += l.chars;
                proseChars[key] += l.chars;
                cellChars += l.chars;
            }
    int bodyKey = 0;
    uint64_t bodyCount = 0;
    for (const auto& kv : proseChars.empty() ? sizeChars : proseChars)
        if (kv.second > bodyCount || (kv.second == bodyCount && kv.first < bodyKey)) bodyKey = kv.first, bodyCount = kv.second;
    const float body = float(bodyKey) / 2;
    // A document typeset in a monospace font: most characters monospace and next to no lines in
    // another font (code-heavy documents keep their prose in a proportional font). Its lines make
    // paragraphs, and the writer marks no code spans in them.
    const bool monoDocument = totalChars > 0 && monoChars * 10 > totalChars * 6 && proseLineChars * 10 < totalChars;
    out.monoDocument = monoDocument;
    out.boldBody = faceGlyphs > 0 && boldGlyphs * 10 > faceGlyphs * 6;
    out.italicBody = faceGlyphs > 0 && italicGlyphs * 10 > faceGlyphs * 6;
    const bool boldDocument = totalChars > 0 && boldChars * 10 > totalChars * 6;
    FindNotes(pages, infos, body, out.noteRefs);
    // A bar beside text set at heading size is an accent rule (slide titles, call-outs), not a
    // quote, unless the quote goes on in body text next to it ("> ## Title" then its paragraph).
    for (size_t i = 0; i < infos.size();) {
        const auto large = [&](size_t k) { return infos[k].size >= kHeadingSize * body; };
        if (!large(i) || infos[i].quoteBars == 0) {
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < infos.size() && large(end) && infos[end].page == infos[i].page) ++end;
        const bool before = i > 0 && infos[i - 1].page == infos[i].page && infos[i - 1].quoteBars > 0;
        const bool after = end < infos.size() && infos[end].page == infos[i].page && infos[end].quoteBars > 0;
        if (!before && !after)
            for (size_t k = i; k < end; ++k) infos[k].quoteBars = 0;
        i = end;
    }

    // Raw blocks: lines grouped by the paragraph rules (§6.2); a footnote's lines make one block.
    std::vector<RawBlock> raws;
    for (uint32_t i = 0; i < infos.size(); ++i) {
        const Info& in = infos[i];
        if (!raws.empty()) {
            RawBlock& b = raws.back();
            const Info& first = infos[b.first];
            const Info& last = infos[b.first + b.count - 1];
            if (b.note != 0 || in.note != 0) {
                if (b.note == in.note && !in.noteStart && b.first + b.count == i) {
                    b.count++;
                    continue;
                }
            } else if (b.first + b.count == i && b.count == 1 && last.numbering && in.page == last.page &&
                       in.dir == last.dir && !in.numbering && last.size >= kHeadingSize * body &&
                       in.size >= kHeadingSize * body && std::fabs(in.u0 - last.u0) <= last.size &&
                       last.baseline - in.baseline > 0 &&
                       last.baseline - in.baseline <= 2.5f * Fmax(last.size, in.size)) {
                // A chapter or section number set large on a line of its own above its title
                // ("4" over "Basis Fields") heads it with the title.
                b.count++;
                b.right = Fmax(b.right, in.u1);
                b.bold = b.bold && (in.flags & kLineAllBold) != 0;
                continue;
            } else if (b.first + b.count == i && Continues(b, first, last, in, monoDocument)) {
                if (b.count == 1) {
                    b.pitch = last.baseline - in.baseline;
                    b.bodyU0 = in.u0;
                } else {
                    b.bodyU0 = Fmin(b.bodyU0, in.u0);
                }
                b.right = Fmax(b.right, in.u1);
                b.count++;
                continue;
            }
        }
        RawBlock b;
        b.first = i;
        b.count = 1;
        b.bodyU0 = in.u0;
        b.right = in.u1;
        b.mono = in.mono >= kMonoShare && !monoDocument;
        b.bold = (in.flags & kLineAllBold) != 0;
        b.note = in.note;
        raws.push_back(b);
    }

    // Outline titles (D5: outline > style > numbering).
    std::unordered_map<std::string, std::vector<uint32_t>> titles;
    std::vector<uint8_t> titleUsed(outline.size(), 0);
    for (uint32_t t = 0; t < outline.size(); ++t) {
        MatchKey(outline[t].title, scratch, false);
        if (!scratch.empty()) titles[scratch].push_back(t);
        MatchKey(outline[t].title, scratch, true);
        if (!scratch.empty()) titles[scratch].push_back(t);
    }

    // Classification.
    struct Classified {
        BlockKind kind = BlockKind::Paragraph;
        int level = 0;      // outline level, or 0
        int styleKey = -1;  // heading style above body size: size key * 2 + bold
        bool boldHeading = false;
        bool runIn = false;  // a bold heading its paragraph runs on from, on the same baseline
        bool bold = false;
        float size = 0;     // the size of most of its characters
        uint32_t chars = 0;
    };
    std::vector<Classified> kinds(raws.size());
    std::string text, line;
    // GitHub alerts: a block quote whose first line is an alert word alone ("Note", "Warning")
    // and whose text goes on below it in the same quote. The title is no text of its own: the
    // writer writes `> [!NOTE]` before the alert's first block.
    std::vector<uint8_t> alertTitle(raws.size(), 0);
    for (size_t r = 0; r + 1 < raws.size(); ++r) {
        const RawBlock& b = raws[r];
        const Info& in = infos[b.first];
        if (b.count != 1 || b.note != 0 || in.quoteBars == 0 || in.chars > 10 || (in.flags & kLineAnchor)) continue;
        const Info& next = infos[raws[r + 1].first];
        if (next.page != in.page || next.quoteBars != in.quoteBars || raws[r + 1].note != 0) continue;
        LineText(pages[in.page], pages[in.page].lines[in.line], line);
        alertTitle[r] = AlertOf(line);
    }
    for (size_t r = 0; r < raws.size(); ++r) {
        const RawBlock& b = raws[r];
        const Info& first = infos[b.first];
        Classified& k = kinds[r];
        uint32_t chars = 0;
        bool letter = false;
        float size = 0;
        uint32_t sizeVotes = 0;
        for (uint32_t i = b.first; i < b.first + b.count; ++i) {
            chars += infos[i].chars;
            letter = letter || infos[i].hasLetter;
            if (infos[i].chars > sizeVotes) size = infos[i].size, sizeVotes = infos[i].chars;
        }
        k.chars = chars;
        k.size = size;
        k.bold = b.bold;
        if (b.note != 0) {
            k.kind = BlockKind::Footnote;
            continue;
        }
        if (alertTitle[r]) continue;
        if (first.flags & kLineImage) {
            k.kind = BlockKind::Image;
            continue;
        }
        // Lines of a ruled grid read row by row stay text; a detected table is a block of its own.
        if (first.flags & kLineTableAnchor) {
            k.kind = BlockKind::Table;
            continue;
        }
        if (first.flags & kLineTable) continue;
        if (b.mono && !monoDocument) {
            k.kind = BlockKind::Code;
            continue;
        }
        // The outline.
        if (!titles.empty() && b.count <= 4 && chars <= 300 && letter) {
            text.clear();
            for (uint32_t i = b.first; i < b.first + b.count; ++i) {
                const PageData& page = pages[infos[i].page];
                LineText(page, page.lines[infos[i].line], line);
                if (!text.empty()) text.push_back(' ');
                text += line;
            }
            for (int strip = 0; strip < 2 && k.level == 0; ++strip) {
                MatchKey(text, scratch, strip == 1);
                auto it = titles.find(scratch);
                if (it == titles.end()) continue;
                for (uint32_t t : it->second) {
                    if (titleUsed[t]) continue;
                    titleUsed[t] = 1;
                    k.kind = BlockKind::Heading;
                    k.level = std::min(outline[t].level, 6);
                    break;
                }
            }
        }
        // Invisible text (an OCR layer) carries no typographic signal: its sizes are fitted word
        // boxes, so it makes paragraphs and lists only.
        if (first.flags & kLineInvisible) {
            if (k.kind != BlockKind::Heading && first.listStart()) k.kind = BlockKind::ListItem;
            continue;
        }
        // Text rotated to read upwards (an arXiv stamp in the margin) or upside down heads nothing.
        const bool shortBlock = b.count <= 3 && chars <= 160 && letter && !first.rowPeer && !first.pageRef &&
                                first.dir != 1 && first.dir != 2;
        const Info& last = infos[b.first + b.count - 1];
        const bool unpunctuated = !last.endsTerminal && last.lastCp != ',';
        // Captions are set apart like headings but title no section; display math set large (an
        // integral sign, a fraction's numerator), a figure set large ("20%↑") and an author list
        // (names with raised affiliation marks: after a word of two letters or more, not after a
        // symbol as in "L²(Rᵈ)") title nothing either.
        const auto caption = [&]() {
            const PageData& page = pages[first.page];
            LineText(page, page.lines[first.line], line);
            if (IsCaption(line)) return true;
            text = line;
            uint32_t marks = 0;
            const auto letter = [](const PageData& p, uint32_t gi) {
                const uint32_t cp = FirstCodePoint(p.GlyphText(p.glyphs[gi]));
                return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp < 0x2000);
            };
            for (uint32_t i = b.first; i < b.first + b.count; ++i) {
                const PageData& p = pages[infos[i].page];
                const Line& l = p.lines[infos[i].line];
                bool raised = false;
                for (uint32_t g = 0; g < l.count; ++g) {
                    const bool sup = IsSuperscript(p.glyphs[p.lineGlyphs[l.first + g]], l);
                    if (sup && !raised && g >= 2 && letter(p, p.lineGlyphs[l.first + g - 1]) &&
                        letter(p, p.lineGlyphs[l.first + g - 2]))
                        marks++;
                    raised = sup;
                }
                if (i == b.first) continue;
                LineText(p, l, line);
                text += ' ';
                text += line;
            }
            return marks >= 2 || NoTitle(text);
        };
        // Slightly larger text (from 7 % up) is a heading only as one or two short unpunctuated lines.
        const bool larger = size >= body * kHeadingSize ||
                            (size >= body * 1.07f && b.count <= 2 && chars <= 80 && unpunctuated);
        // A running head left on a page (the top line alone above a wide gap, in the body face
        // and not much larger) titles nothing.
        const bool runningHead = first.topBand && b.count == 1 && !(first.flags & kLineAllBold) &&
                                 size <= 1.2f * body && first.gapBelow >= 2 * body;
        if (shortBlock && larger && !runningHead && !caption()) {
            k.kind = BlockKind::Heading;
            k.styleKey = static_cast<int>(std::lround(size * 2)) * 2 + (b.bold ? 1 : 0);
            continue;
        }
        if (k.kind == BlockKind::Heading) continue;
        // A bold line of body size: a heading, numbered ("5. Natural dispersal") or not. It may
        // ask or introduce ("Procedure:"), but a bold sentence is emphasis. "Body size" reaches a
        // little below the body (11 pt headings over 12 pt table text); a table's bold header row
        // in its 90 % size does not. In a document set bold throughout, bold says nothing, but a
        // "Contents" or "Table of Contents" line still titles its contents.
        const bool enumerated =
            first.marker == Marker::Number || first.marker == Marker::Roman || first.marker == Marker::Letter;
        const bool titleEnd = unpunctuated || last.lastCp == ':' || last.lastCp == '?';
        bool contents = false;
        if (boldDocument && b.count == 1 && first.chars <= 17) {
            const PageData& page = pages[first.page];
            LineText(page, page.lines[first.line], line);
            MatchKey(line, scratch, false);
            contents = scratch == "contents" || scratch == "tableofcontents";
        }
        if (shortBlock && b.bold && (!boldDocument || contents) && b.count <= 2 && chars <= 100 &&
            size >= body * 0.915f &&
            titleEnd && (!first.listStart() || enumerated) && !caption()) {
            k.kind = BlockKind::Heading;
            k.boldHeading = true;
            // Run in: its paragraph's text goes on beside it on its baseline (lines.cpp split the
            // line after the heading's bold run).
            if (b.count == 1 && b.first + 1 < infos.size()) {
                const Info& rest = infos[b.first + 1];
                k.runIn = rest.page == first.page && rest.dir == first.dir && !(rest.flags & kLineAllBold) &&
                          std::fabs(rest.baseline - first.baseline) <= 0.3f * first.size && rest.u0 >= first.u1 - 0.5f;
            }
            continue;
        }
        if (first.listStart()) k.kind = BlockKind::ListItem;
    }

    // A heading style that holds a large share of the text is the body of a document whose most
    // common size is something else (footnotes, captions): its blocks are not headings. A style
    // of a single block is still a heading when the text of the tables beside it outweighs it (the
    // title over a page that is all table). Scanned text (the add-on's OCR layer) has no outline to
    // name the section titles of a document that is mostly tables: there a style of title size
    // (kTitleSize) is weighed against the tables' text too, whatever its number of blocks.
    {
        std::unordered_map<int, uint64_t> styleChars;
        std::unordered_map<int, uint32_t> styleBlocks;
        for (const Classified& k : kinds)
            if (k.kind == BlockKind::Heading && k.level == 0) styleChars[k.styleKey] += k.chars, styleBlocks[k.styleKey]++;
        const auto scanned = [&](size_t r) {
            const Info& in = infos[raws[r].first];
            const Line& l = pages[in.page].lines[in.line];
            return l.count > 0 && (pages[in.page].glyphs[pages[in.page].lineGlyphs[l.first]].flags & kGlyphOcr) != 0;
        };
        for (size_t r = 0; r < raws.size(); ++r) {
            Classified& k = kinds[r];
            if (k.kind != BlockKind::Heading || k.level > 0) continue;
            const uint64_t chars = styleChars[k.styleKey];
            if (chars * 10 <= totalChars * 3) continue;
            const bool weighed = styleBlocks[k.styleKey] == 1 || (k.size >= kTitleSize * body && scanned(r));
            if (weighed && chars * 10 <= (totalChars + cellChars) * 3) continue;
            k.kind = infos[raws[r].first].listStart() ? BlockKind::ListItem : BlockKind::Paragraph;
        }
    }

    // A title block (§6.1): under the title on the first page, blocks centred under it in a
    // smaller size name the authors, their affiliations and the date, up to the first section
    // ("Abstract", "1 Introduction") or the first body paragraph. They head nothing.
    if (!raws.empty()) {
        const uint32_t page = infos[raws[0].first].page;
        const auto extent = [&](size_t r, float& u0, float& u1) {
            u0 = INFINITY, u1 = -INFINITY;
            for (uint32_t i = raws[r].first; i < raws[r].first + raws[r].count; ++i)
                u0 = Fmin(u0, infos[i].u0), u1 = Fmax(u1, infos[i].u1);
        };
        size_t title = raws.size();
        float titleSize = 0;
        for (size_t r = 0; r < raws.size() && infos[raws[r].first].page == page; ++r) {
            if (kinds[r].kind != BlockKind::Heading || kinds[r].level > 0) continue;
            if (title == raws.size()) title = r;
            titleSize = Fmax(titleSize, kinds[r].size);
        }
        if (title < raws.size() && kinds[title].size >= titleSize && infos[raws[title].first].dir == 0) {
            float t0 = 0, t1 = 0;
            extent(title, t0, t1);
            // The text's width on the page: a body paragraph runs across it, a centred block of
            // the title's does not.
            float left = INFINITY, right = -INFINITY;
            for (const Info& in : infos)
                if (in.page == page && in.dir == 0) left = Fmin(left, in.u0), right = Fmax(right, in.u1);
            for (size_t r = title + 1; r < raws.size() && infos[raws[r].first].page == page; ++r) {
                const Info& first = infos[raws[r].first];
                float u0 = 0, u1 = 0;
                extent(r, u0, u1);
                const bool centred = std::fabs((u0 + u1) - (t0 + t1)) <= 4 * body && u0 > left + body && first.dir == 0;
                if (kinds[r].kind != BlockKind::Heading) {
                    if (raws[r].count >= 3 && (!centred || u1 - u0 >= right - left - 4 * body)) break;
                    continue;
                }
                // A section starts with its number ("1 Introduction"); a date ends with its year.
                LineText(pages[first.page], pages[first.page].lines[first.line], line);
                const bool numbered = !line.empty() && line[0] >= '0' && line[0] <= '9' &&
                                      !(line.back() >= '0' && line.back() <= '9');
                // Set like the title or larger, it is a title line of its own.
                const bool titleStyle = kinds[r].size > kinds[title].size + 0.25f ||
                                        (kinds[r].size >= kinds[title].size - 0.25f && kinds[r].bold == kinds[title].bold);
                if (kinds[r].level > 0 || !centred || titleStyle || first.listStart() || first.numbering || numbered ||
                    FrontMatter(line))
                    break;
                kinds[r].kind = BlockKind::Paragraph;
            }
        }
    }

    // Heading levels (§6.1). Outline levels where the outline matched; a heading the outline
    // missed takes the level of the last matched heading of its style, else sits one below the
    // last matched heading. Without the outline: style keys ranked by size (bold first), below
    // them bold body-size lines. A run-in heading the outline missed sits one below the last
    // heading set on a line of its own (below the deepest one before any), whatever its style.
    std::vector<int> styleKeys;
    for (const Classified& k : kinds)
        if (k.kind == BlockKind::Heading && k.styleKey >= 0 && k.level == 0) styleKeys.push_back(k.styleKey);
    std::sort(styleKeys.begin(), styleKeys.end(), std::greater<int>());
    styleKeys.erase(std::unique(styleKeys.begin(), styleKeys.end()), styleKeys.end());
    std::vector<int> levelOf(raws.size(), 0);
    {
        const auto anyStyle = [](const Classified& k) {
            return static_cast<int>(std::lround(k.size * 2)) * 2 + (k.bold ? 1 : 0);
        };
        std::unordered_map<int, int> matchedLevel;  // style -> level of its last outline-matched heading
        int lastLevel = 0;
        float lastSize = 0;
        for (size_t r = 0; r < raws.size(); ++r) {
            const Classified& k = kinds[r];
            if (k.kind != BlockKind::Heading || (k.runIn && k.level == 0)) continue;
            if (k.level > 0) {
                levelOf[r] = k.level;
                matchedLevel[anyStyle(k)] = k.level;
                lastLevel = k.level;
                lastSize = k.size;
                continue;
            }
            if (lastLevel > 0) {
                const auto it = matchedLevel.find(anyStyle(k));
                if (it != matchedLevel.end()) {
                    levelOf[r] = it->second;
                    continue;
                }
                if (k.size <= lastSize * 1.02f) {
                    levelOf[r] = std::min(lastLevel + 1, 6);
                    continue;
                }
            }
            if (k.styleKey >= 0) {
                const int rank =
                    static_cast<int>(std::find(styleKeys.begin(), styleKeys.end(), k.styleKey) - styleKeys.begin());
                levelOf[r] = std::min(rank + 1, 6);
            } else {
                levelOf[r] = std::min(static_cast<int>(styleKeys.size()) + 1, 6);
            }
        }
        // Section numbers refine the style ranks (D5: style > numbering): within one style,
        // "4.3.1" sits below "4.3" (some templates set both levels alike). The front matter and
        // back matter of a numbered paper ("Abstract", "References") sit with its top sections.
        {
            std::vector<uint8_t> depth(raws.size(), 0);
            std::unordered_map<int, std::pair<int, int>> depths;  // style -> min and max depth
            int sections = 0, top = 7;
            for (size_t r = 0; r < raws.size(); ++r) {
                const Classified& k = kinds[r];
                if (k.kind != BlockKind::Heading || k.level > 0 || k.runIn || levelOf[r] <= 0) continue;
                const Info& first = infos[raws[r].first];
                LineText(pages[first.page], pages[first.page].lines[first.line], line);
                depth[r] = static_cast<uint8_t>(SectionDepth(line));
                if (depth[r] == 0) continue;
                auto it = depths.find(anyStyle(k));
                if (it == depths.end())
                    depths[anyStyle(k)] = {depth[r], depth[r]};
                else
                    it->second = {std::min<int>(it->second.first, depth[r]), std::max<int>(it->second.second, depth[r])};
            }
            for (size_t r = 0; r < raws.size(); ++r) {
                if (depth[r] == 0) continue;
                const std::pair<int, int>& d = depths[anyStyle(kinds[r])];
                if (d.second > d.first) levelOf[r] = std::min(levelOf[r] + depth[r] - d.first, 6);
                if (depth[r] == 1) sections++, top = std::min(top, levelOf[r]);
            }
            if (sections >= 2)
                for (size_t r = 0; r < raws.size(); ++r) {
                    const Classified& k = kinds[r];
                    if (k.kind != BlockKind::Heading || k.level > 0 || k.runIn || depth[r] != 0 || levelOf[r] <= top)
                        continue;
                    const Info& first = infos[raws[r].first];
                    LineText(pages[first.page], pages[first.page].lines[first.line], line);
                    if (FrontMatter(line) && line.size() <= 20) levelOf[r] = top;
                }
        }
        int deepest = 0, last = 0;
        for (size_t r = 0; r < raws.size(); ++r)
            if (kinds[r].kind == BlockKind::Heading && levelOf[r] > 0) deepest = std::max(deepest, levelOf[r]);
        for (size_t r = 0; r < raws.size(); ++r) {
            const Classified& k = kinds[r];
            if (k.kind != BlockKind::Heading) continue;
            if (k.runIn && k.level == 0)
                levelOf[r] = std::min((last > 0 ? last : deepest) + 1, 6);
            else
                last = levelOf[r];
        }
    }
    // Never skip a level: the levels in use renumbered 1..n. Then the top level is made absolute:
    // it is a title (H1) when set clearly larger than the body, or when it is used once or twice
    // above deeper levels and still set well above the body (LaTeX at 11 pt sets \section at 1.31
    // times the body); it is the sections of a title-less document (H2) when smaller, or when one
    // style alone heads several sections (Markdown documents carry one H1). The outline gives
    // depths, not levels: a title-less document's sections are its top bookmarks too.
    {
        bool used[8] = {};
        for (size_t r = 0; r < raws.size(); ++r)
            if (levelOf[r] > 0) used[levelOf[r]] = true;
        int map[8] = {};
        int next = 0;
        for (int l = 1; l <= 6; ++l)
            if (used[l]) map[l] = ++next;
        float topSize = 0;
        int topCount = 0;
        for (size_t r = 0; r < raws.size(); ++r) {
            if (levelOf[r] <= 0) continue;
            levelOf[r] = map[levelOf[r]];
            if (levelOf[r] == 1) topSize = Fmax(topSize, kinds[r].size), topCount++;
        }
        const bool title = next >= 2 && topCount <= 2 && topSize >= kSmallTitleSize * body;
        const bool sections =
            topCount > 0 && !title && (topSize < kTitleSize * body || (next == 1 && topCount >= 2));
        if (sections)
            for (size_t r = 0; r < raws.size(); ++r)
                if (levelOf[r] > 0) levelOf[r] = std::min(levelOf[r] + 1, 6);
    }

    // Blocks in order, with list nesting, paragraphs inside list items, comments per page; footnote
    // definitions after the last block.
    std::vector<float> markerStack;
    float itemTextU0 = 0, itemMarkerU0 = 0;  // where the last list item's text and marker start
    uint32_t itemPage = UINT32_MAX;
    int itemDepth = -1;
    std::vector<Block> notes;
    std::vector<BlockLine> noteLines;
    uint64_t ruleBudget = kRuleChecks;
    uint8_t alert = 0;  // the alert whose title was the last block
    size_t r = 0;
    for (uint32_t p = 0; p <= pages.size(); ++p) {
        // Blocks of pages before p.
        while (r < raws.size() && infos[raws[r].first].page < p) {
            const RawBlock& b = raws[r];
            const Info& first = infos[b.first];
            if (kinds[r].kind == BlockKind::Footnote) {
                Block note;
                note.kind = BlockKind::Footnote;
                note.number = first.note;
                note.first = static_cast<uint32_t>(noteLines.size());
                note.count = b.count;
                note.size = first.size;
                for (uint32_t i = b.first; i < b.first + b.count; ++i)
                    noteLines.push_back(BlockLine{infos[i].page, infos[i].line, 0});
                noteLines[note.first].skip = first.noteSkip;
                notes.push_back(note);
                ++r;
                continue;
            }
            if (alertTitle[r]) {
                alert = alertTitle[r];
                ++r;
                continue;
            }
            Block block;
            block.kind = kinds[r].kind;
            block.level = static_cast<uint8_t>(levelOf[r]);
            block.quote = first.quoteBars;
            block.alert = alert;
            alert = 0;
            block.first = static_cast<uint32_t>(lines.size());
            block.count = b.count;
            block.size = first.size;
            for (uint32_t i = b.first; i < b.first + b.count; ++i) {
                BlockLine bl;
                bl.page = infos[i].page;
                bl.line = infos[i].line;
                bl.skip = 0;
                lines.push_back(bl);
            }
            if (block.kind == BlockKind::Image) block.number = static_cast<uint32_t>(pages[first.page].number);
            if (block.kind == BlockKind::Table) {
                // After the anchor line, the table's cell lines (cell order).
                const PageData& tp = pages[first.page];
                block.number = tp.lines[first.line].first;
                const Table& t = tp.tables[block.number];
                for (uint32_t l = t.firstLine; l < t.firstLine + t.lineCount; ++l)
                    lines.push_back(BlockLine{first.page, l, 0});
                block.count += t.lineCount;
            }
            const float em = first.size;
            if (block.kind == BlockKind::ListItem) {
                const float mu = first.markerU0;
                while (!markerStack.empty() && mu < markerStack.back() - 0.8f * em) markerStack.pop_back();
                if (markerStack.empty() || mu > markerStack.back() + 0.8f * em) {
                    if (markerStack.size() < 8) markerStack.push_back(mu);
                } else {
                    markerStack.back() = mu;
                }
                block.depth = static_cast<uint8_t>(markerStack.size() - 1);
                block.ordered = !first.verbatimMarker && (first.marker == Marker::Number ||
                                                          first.marker == Marker::Letter || first.marker == Marker::Roman);
                block.number = first.value;
                block.task = first.task;
                if (!first.verbatimMarker) lines[block.first].skip = first.skip;
                itemTextU0 = first.textU0;
                itemMarkerU0 = first.markerU0;
                itemPage = first.page;
                itemDepth = block.depth;
            } else if (block.kind == BlockKind::Paragraph && itemDepth >= 0 && first.page == itemPage &&
                       std::fabs(first.u0 - itemTextU0) <= 0.8f * em &&
                       std::fabs(first.u0 - itemTextU0) < std::fabs(first.u0 - itemMarkerU0) && !blocks.empty() &&
                       (blocks.back().kind == BlockKind::ListItem || blocks.back().listParagraph)) {
                block.listParagraph = true;
                block.depth = static_cast<uint8_t>(itemDepth);
            } else if (block.kind != BlockKind::Comment) {
                markerStack.clear();
                itemDepth = -1;
            }

            // Continuation across columns and pages (§6.2): the previous paragraph ends without
            // terminal punctuation and this one starts in lowercase.
            if (block.kind == BlockKind::Paragraph && !block.listParagraph && !blocks.empty() &&
                !(first.flags & kLineTable)) {
                Block& prev = blocks.back();
                const BlockLine& pl = lines[prev.first + prev.count - 1];
                const Info* prevLast = nullptr;
                // Find the Info of the previous block's last line: it is the one just before b.first
                // unless a comment or footnotes intervened.
                uint32_t before = b.first;
                while (before > 0 && infos[before - 1].note != 0) --before;
                if (before > 0) prevLast = &infos[before - 1];
                if ((prev.kind == BlockKind::Paragraph || prev.kind == BlockKind::ListItem) && prevLast &&
                    prevLast->page == pl.page && prevLast->line == pl.line && !prevLast->endsTerminal &&
                    first.startsLower && std::fabs(prevLast->size - first.size) <= 0.1f * em &&
                    prev.quote == block.quote && block.alert == 0 &&
                    (prevLast->page != first.page || first.baseline > prevLast->baseline)) {
                    prev.count += block.count;
                    ++r;
                    continue;
                }
            }
            // Code blocks split by blank lines join again, unless a rule between them ends one
            // framed listing and starts the next, or the space between them is off the listings'
            // line grid (blank lines inside a listing are whole lines; the space between two
            // listings is paragraph spacing).
            if (block.kind == BlockKind::Code && !blocks.empty() && blocks.back().kind == BlockKind::Code &&
                blocks.back().first + blocks.back().count == block.first && block.alert == 0 &&
                !RuleBetween(pages, lines[block.first - 1], lines[block.first], ruleBudget) &&
                !OffGrid(pages, lines, blocks.back(), block)) {
                blocks.back().count += block.count;
                ++r;
                continue;
            }
            // A table broken across pages: its part here continues the table ending the page before.
            if (block.kind == BlockKind::Table && !blocks.empty() && blocks.back().kind == BlockKind::Table &&
                blocks.back().first + blocks.back().count == block.first &&
                TableContinues(pages, lines, blocks.back(), first.page, block.number)) {
                blocks.back().count += block.count;
                ++r;
                continue;
            }
            blocks.push_back(block);
            ++r;
        }
        if (p == pages.size()) break;
        // Comments open the page they are about.
        const PageData& page = pages[p];
        const bool noText = !page.HasText();
        const uint8_t flags = p < unmappedPages.size() ? unmappedPages[p] : 0;
        if (noText || (flags & 1)) {
            Block c;
            c.kind = BlockKind::Comment;
            c.level = static_cast<uint8_t>(noText ? ((flags & 2) ? CommentKind::OcrFailed : CommentKind::NoText)
                                                  : CommentKind::Unmapped);
            c.number = static_cast<uint32_t>(page.number);
            c.first = static_cast<uint32_t>(lines.size());
            blocks.push_back(c);
        }
    }
    const uint32_t base = static_cast<uint32_t>(lines.size());
    lines.insert(lines.end(), noteLines.begin(), noteLines.end());
    for (Block& note : notes) {
        note.first += base;
        blocks.push_back(note);
    }
}

} // namespace RayoPdf::Layout
