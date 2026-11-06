// SPDX-License-Identifier: Apache-2.0
#include "layout/semantics.h"

#include "layout/lines.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>

namespace RayoPdf::Layout {
namespace {

constexpr float kMonoShare = 0.8f;   // a code line: at least this share of monospace characters
constexpr float kHeadingSize = 1.12f; // a heading candidate: at least this times the body size

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
    bool numbering = false;  // only a section or item number ("1.5.", "IV", "(3)")
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

void LineInfo(const PageData& page, uint32_t pi, uint32_t li, const Marks& marks, Info& info, std::string& scratch) {
    const Line& line = page.lines[li];
    info = Info();
    info.page = pi;
    info.line = li;
    info.u0 = line.u0, info.u1 = line.u1, info.vb = line.vb, info.vt = line.vt;
    info.baseline = line.baseline, info.size = line.size, info.dir = line.dir;
    info.flags = line.flags;
    info.chars = line.chars;
    info.markerU0 = info.textU0 = line.u0;
    uint32_t mono = 0;
    bool letter = false;
    bool numbering = line.chars <= 8;
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (g.flags & kGlyphMonospace) mono++;
        const uint32_t cp = FirstCodePoint(page.GlyphText(g));
        if (!letter) letter = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp >= 0xC0;
        if (numbering && !((cp >= '0' && cp <= '9') || cp == '.' || cp == ')' || cp == '(' || cp == 'I' ||
                           cp == 'V' || cp == 'X' || (k == 0 && cp >= 'A' && cp <= 'Z')))
            numbering = false;
    }
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
            info.marker = Marker::Bullet;
            info.markerU0 = bullet->x0;
        }
    }
    // Quote bars: thin vertical rules to the left that cover the line (algorithms §6.5). A rule on
    // the right as well makes the line a table cell, not a quote.
    float barX[4];
    int bars = 0;
    bool rightRule = false;
    for (const Shape* s : marks.bars) {
        if (s->y0 > line.vb + 0.2f * line.size || s->y1 < line.vt - 0.2f * line.size) continue;
        if (s->x0 >= line.u1 - 0.5f && s->x0 - line.u1 <= 40 * line.size) rightRule = true;
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
};

bool Continues(const RawBlock& b, const Info& first, const Info& last, const Info& next) {
    if (next.page != last.page || next.dir != last.dir) return false;
    // A section number set apart from its title ("1.5.   Migrant Workers") joins it.
    if (b.count == 1 && last.numbering && std::fabs(last.baseline - next.baseline) <= 0.3f * last.size &&
        next.u0 > last.u1 && next.u0 - last.u1 <= 4 * last.size && std::fabs(next.size - last.size) <= 0.15f * last.size)
        return true;
    if (next.listStart()) return false;
    if ((next.flags ^ last.flags) & kLineTable) return false;
    if (next.quoteBars != last.quoteBars) return false;
    const float em = std::fmax(last.size, next.size);
    // A size change starts a block, except in an OCR layer whose sizes are fitted word boxes.
    const bool ocr = (last.flags & next.flags & kLineInvisible) != 0;
    if (!ocr && std::fabs(next.size - last.size) > 0.15f * em) return false;
    const float pitch = last.baseline - next.baseline;
    if (pitch <= 0.3f * em) return false;
    const float limit = b.pitch > 0 ? std::fmax(b.pitch * 1.3f, b.pitch + 0.25f * em) : 1.6f * em;
    if (pitch > limit) return false;
    if (next.u0 > last.u1 || next.u1 < last.u0) return false;
    const bool monoNext = next.mono >= kMonoShare;
    if (b.mono != monoNext) return false;
    if (b.mono) return true;
    if (b.bold != ((next.flags & kLineAllBold) != 0)) return false;
    if (first.listStart()) {
        if (next.u0 >= first.markerU0 + 0.5f * em) return true;
        // Without a hanging indent the item still continues where its long last line broke
        // mid-sentence.
        return next.u0 >= first.markerU0 - 0.5f * em && !last.endsTerminal && last.u1 - last.u0 >= 25 * em;
    }
    const float right = std::fmax(b.right, next.u1);
    if (b.count >= 2) {
        // A first-line indent starts a paragraph.
        if (next.u0 > b.bodyU0 + 0.8f * em && std::fabs(last.u0 - b.bodyU0) < 0.5f * em) return false;
    } else if (next.u0 > last.u0 + 0.8f * em && last.endsTerminal) {
        return false;
    }
    // A short line ending a sentence ends the paragraph.
    const float left = std::fmin(b.bodyU0, next.u0);
    if (last.endsTerminal && last.u1 < right - std::fmax(2.5f * em, 0.12f * (right - left))) return false;
    return true;
}

} // namespace

void BuildBlocks(const std::vector<PageData>& pages, const std::vector<OutlineEntry>& outline,
                 const std::vector<uint8_t>& unmappedPages, std::vector<Block>& blocks, std::vector<BlockLine>& lines) {
    blocks.clear();
    lines.clear();

    // Line facts in reading order.
    std::vector<Info> infos;
    Marks marks;
    std::string scratch;
    std::vector<uint32_t> byBase;
    for (uint32_t p = 0; p < pages.size(); ++p) {
        const PageData& page = pages[p];
        CollectMarks(page, marks);
        const size_t pageFirst = infos.size();
        for (uint32_t li : page.order) {
            infos.emplace_back();
            LineInfo(page, p, li, marks, infos.back(), scratch);
        }
        // Row peers: short lines of the same style side by side on one baseline are cells of a row
        // (a table header), not headings; a heading beside the other column's text has no peer.
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
                if (a.dir != b.dir || std::fabs(a.size - b.size) > 0.03f * a.size) continue;
                if (a.chars > 60 || b.chars > 60 || a.numbering || b.numbering) continue;
                if (((a.flags ^ b.flags) & kLineAllBold) || !(b.u0 > a.u1 || b.u1 < a.u0)) continue;
                a.rowPeer = b.rowPeer = true;
            }
        }
    }

    // Body size: the most common size by characters (algorithms §6.1), of prose lines: code blocks
    // set in a smaller monospace size do not define it. Document-wide styles.
    std::unordered_map<int, uint64_t> sizeChars, proseChars;
    uint64_t totalChars = 0, monoChars = 0, boldChars = 0;
    for (const Info& in : infos) {
        const int key = static_cast<int>(std::lround(in.size * 2));
        sizeChars[key] += in.chars;
        if (in.mono < 0.5f) proseChars[key] += in.chars;
        totalChars += in.chars;
        monoChars += static_cast<uint64_t>(in.mono * float(in.chars));
        if (in.flags & kLineAllBold) boldChars += in.chars;
    }
    int bodyKey = 0;
    uint64_t bodyCount = 0;
    for (const auto& kv : proseChars.empty() ? sizeChars : proseChars)
        if (kv.second > bodyCount || (kv.second == bodyCount && kv.first < bodyKey)) bodyKey = kv.first, bodyCount = kv.second;
    const float body = float(bodyKey) / 2;
    const bool monoDocument = totalChars > 0 && monoChars * 10 > totalChars * 6;
    const bool boldDocument = totalChars > 0 && boldChars * 10 > totalChars * 6;

    // Raw blocks: lines grouped by the paragraph rules (§6.2).
    std::vector<RawBlock> raws;
    for (uint32_t i = 0; i < infos.size(); ++i) {
        const Info& in = infos[i];
        if (!raws.empty()) {
            RawBlock& b = raws.back();
            const Info& first = infos[b.first];
            const Info& last = infos[b.first + b.count - 1];
            if (b.first + b.count == i && Continues(b, first, last, in)) {
                if (b.count == 1) {
                    b.pitch = last.baseline - in.baseline;
                    b.bodyU0 = in.u0;
                } else {
                    b.bodyU0 = std::fmin(b.bodyU0, in.u0);
                }
                b.right = std::fmax(b.right, in.u1);
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
        int styleKey = -1;  // heading style: size key * 2 + bold
        bool boldHeading = false;
        uint32_t chars = 0;
    };
    std::vector<Classified> kinds(raws.size());
    std::string text, line;
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
        // Cells of a ruled table stay text (tables are written as such in a later milestone).
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
        const bool shortBlock = b.count <= 3 && chars <= 160 && letter && !first.rowPeer;
        const Info& last = infos[b.first + b.count - 1];
        const bool unpunctuated = !last.endsTerminal && last.lastCp != ',';
        // Slightly larger text (from 7 % up) is a heading only as one or two short unpunctuated lines.
        const bool larger = size >= body * kHeadingSize ||
                            (size >= body * 1.07f && b.count <= 2 && chars <= 80 && unpunctuated);
        if (shortBlock && larger) {
            k.kind = BlockKind::Heading;
            k.styleKey = static_cast<int>(std::lround(size * 2)) * 2 + (b.bold ? 1 : 0);
            continue;
        }
        if (k.kind == BlockKind::Heading) continue;
        // A bold line of body size: a heading, numbered ("5. Natural dispersal") or not.
        const bool enumerated =
            first.marker == Marker::Number || first.marker == Marker::Roman || first.marker == Marker::Letter;
        if (shortBlock && b.bold && !boldDocument && b.count <= 2 && chars <= 100 && size >= body * 0.95f &&
            unpunctuated && (!first.listStart() || enumerated)) {
            k.kind = BlockKind::Heading;
            k.boldHeading = true;
            continue;
        }
        if (first.listStart()) k.kind = BlockKind::ListItem;
    }

    // A heading style that holds a large share of the text is the body of a document whose most
    // common size is something else (footnotes, captions): its blocks are not headings.
    {
        std::unordered_map<int, uint64_t> styleChars;
        for (const Classified& k : kinds)
            if (k.kind == BlockKind::Heading && k.level == 0) styleChars[k.styleKey] += k.chars;
        for (size_t r = 0; r < raws.size(); ++r) {
            Classified& k = kinds[r];
            if (k.kind != BlockKind::Heading || k.level > 0) continue;
            if (styleChars[k.styleKey] * 10 > totalChars * 3)
                k.kind = infos[raws[r].first].listStart() ? BlockKind::ListItem : BlockKind::Paragraph;
        }
    }

    // Heading levels: style keys ranked by size (bold first), below them bold body-size lines;
    // outline levels where the outline matched (§6.1).
    std::vector<int> styleKeys;
    for (const Classified& k : kinds)
        if (k.kind == BlockKind::Heading && k.styleKey >= 0 && k.level == 0) styleKeys.push_back(k.styleKey);
    std::sort(styleKeys.begin(), styleKeys.end(), std::greater<int>());
    styleKeys.erase(std::unique(styleKeys.begin(), styleKeys.end()), styleKeys.end());
    std::vector<int> levelOf(raws.size(), 0);
    for (size_t r = 0; r < raws.size(); ++r) {
        const Classified& k = kinds[r];
        if (k.kind != BlockKind::Heading) continue;
        if (k.level > 0) {
            levelOf[r] = k.level;
        } else if (k.styleKey >= 0) {
            const int rank = static_cast<int>(std::find(styleKeys.begin(), styleKeys.end(), k.styleKey) - styleKeys.begin());
            levelOf[r] = std::min(rank + 1, 6);
        } else {
            levelOf[r] = std::min(static_cast<int>(styleKeys.size()) + 1, 6);
        }
    }
    // Never skip a level: the levels in use renumbered 1..n.
    {
        bool used[8] = {};
        for (size_t r = 0; r < raws.size(); ++r)
            if (levelOf[r] > 0) used[levelOf[r]] = true;
        int map[8] = {};
        int next = 0;
        for (int l = 1; l <= 6; ++l)
            if (used[l]) map[l] = ++next;
        for (size_t r = 0; r < raws.size(); ++r)
            if (levelOf[r] > 0) levelOf[r] = map[levelOf[r]];
    }

    // Blocks in order, with list nesting, paragraphs inside list items, comments per page.
    std::vector<float> markerStack;
    float itemTextU0 = 0;
    uint32_t itemPage = UINT32_MAX;
    int itemDepth = -1;
    size_t r = 0;
    for (uint32_t p = 0; p <= pages.size(); ++p) {
        // Blocks of pages before p.
        while (r < raws.size() && infos[raws[r].first].page < p) {
            const RawBlock& b = raws[r];
            const Info& first = infos[b.first];
            Block block;
            block.kind = kinds[r].kind;
            block.level = static_cast<uint8_t>(levelOf[r]);
            block.quote = first.quoteBars;
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
                block.ordered = first.marker == Marker::Number || first.marker == Marker::Letter ||
                                first.marker == Marker::Roman;
                block.number = first.value;
                block.task = first.task;
                lines[block.first].skip = first.skip;
                itemTextU0 = first.textU0;
                itemPage = first.page;
                itemDepth = block.depth;
            } else if (block.kind == BlockKind::Paragraph && itemDepth >= 0 && first.page == itemPage &&
                       std::fabs(first.u0 - itemTextU0) <= 0.8f * em && !blocks.empty() &&
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
                // unless a comment intervened.
                if (b.first > 0) prevLast = &infos[b.first - 1];
                if ((prev.kind == BlockKind::Paragraph || prev.kind == BlockKind::ListItem) && prevLast &&
                    prevLast->page == pl.page && prevLast->line == pl.line && !prevLast->endsTerminal &&
                    first.startsLower && std::fabs(prevLast->size - first.size) <= 0.1f * em &&
                    prev.quote == block.quote &&
                    (prevLast->page != first.page || first.baseline > prevLast->baseline)) {
                    prev.count += block.count;
                    ++r;
                    continue;
                }
            }
            // Code blocks split by blank lines join again.
            if (block.kind == BlockKind::Code && !blocks.empty() && blocks.back().kind == BlockKind::Code &&
                blocks.back().first + blocks.back().count == block.first) {
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
        if (page.order.empty() || (p < unmappedPages.size() && unmappedPages[p])) {
            Block c;
            c.kind = BlockKind::Comment;
            c.level = static_cast<uint8_t>(page.order.empty() ? CommentKind::NoText : CommentKind::Unmapped);
            c.number = static_cast<uint32_t>(page.number);
            c.first = static_cast<uint32_t>(lines.size());
            blocks.push_back(c);
        }
    }
}

} // namespace RayoPdf::Layout
