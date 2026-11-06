// SPDX-License-Identifier: Apache-2.0
#include "layout/markdown_writer.h"

#include "common/utf8.h"
#include "layout/lines.h"
#include "layout/tables.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace RayoPdf::Layout {
namespace {

enum : uint8_t {
    kStyleBold = 1,
    kStyleItalic = 2,
    kStyleCode = 4,
};

enum : uint8_t { kScriptNone = 0, kScriptSup = 1, kScriptSub = 2 };

struct GlyphRef {
    uint32_t page;
    uint32_t glyph;
    uint8_t script;
};

struct Word {
    uint32_t first = 0, count = 0;  // GlyphRefs
    bool spaceBefore = false;
    uint8_t style = 0;
    int32_t linkPage = -1;          // the link annotation over the word
    int32_t link = -1;
};

bool IsAsciiAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool StartsLetterOrDigit(std::string_view t) {
    if (t.empty()) return false;
    const unsigned char c = static_cast<unsigned char>(t[0]);
    return IsAsciiAlnum(static_cast<char>(c)) || c >= 0xC0;
}

uint32_t FirstOf(std::string_view t) {
    uint32_t cp = 0;
    size_t length = 0;
    return Utf8::Decode(t, 0, cp, length) ? cp : 0;
}

uint32_t LastOf(std::string_view t) {
    size_t i = t.size();
    while (i > 0 && (static_cast<unsigned char>(t[i - 1]) & 0xC0) == 0x80) --i;
    return i > 0 ? FirstOf(t.substr(i - 1)) : 0;
}

// CJK ideographs, kana, Hangul, CJK punctuation and fullwidth forms.
bool IsCjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

class Writer {
public:
    Writer(const std::vector<PageData>& pages, const BlockList& blocks, std::string& out, WriteCounts& counts)
        : pages_(pages), lines_(blocks.lines), noteRefs_(blocks.noteRefs), out_(out), counts_(counts) {
        // Bold or italic that covers most text is the document's normal face, not emphasis.
        uint64_t total = 0, bold = 0, italic = 0;
        for (const PageData& page : pages) {
            for (uint32_t li : page.order) {
                const Line& line = page.lines[li];
                for (uint32_t k = 0; k < line.count; ++k) {
                    const uint16_t f = page.glyphs[page.lineGlyphs[line.first + k]].flags;
                    total++;
                    if (f & kGlyphBold) bold++;
                    if (f & kGlyphItalic) italic++;
                }
            }
        }
        boldIsBody_ = total > 0 && bold * 10 > total * 6;
        italicIsBody_ = total > 0 && italic * 10 > total * 6;
    }

    // The block's text as one Markdown line; `styles` writes emphasis, code spans and links.
    void Inline(const Block& block, bool styles);
    // A fenced code block; `prefix` starts every line (block quotes).
    void Code(const Block& block, const std::string& prefix);

private:
    void CollectWords(const Block& block);
    void StyleWords(bool styles);
    void AppendEscaped(std::string_view text, bool blockStart);
    // The footnote number of the superscript run starting at `r`, `count` glyphs long; 0 when none.
    uint32_t NoteNumber(const GlyphRef& r, uint32_t count) const {
        const auto it = std::lower_bound(noteRefs_.begin(), noteRefs_.end(), r, [](const NoteRef& n, const GlyphRef& g) {
            return n.page != g.page ? n.page < g.page : n.glyph < g.glyph;
        });
        return it != noteRefs_.end() && it->page == r.page && it->glyph == r.glyph && it->count == count ? it->number : 0;
    }
    std::string_view Text(const GlyphRef& r) const { return pages_[r.page].GlyphText(pages_[r.page].glyphs[r.glyph]); }
    std::string_view Uri(const Word& w) const {
        const PageData& page = pages_[static_cast<size_t>(w.linkPage)];
        return page.LinkUri(page.links[static_cast<size_t>(w.link)]);
    }
    bool SameLink(const Word& a, const Word& b) const {
        if (a.link < 0 || b.link < 0) return a.link == b.link;
        return Uri(a) == Uri(b);
    }

    const std::vector<PageData>& pages_;
    const std::vector<BlockLine>& lines_;
    const std::vector<NoteRef>& noteRefs_;
    std::string& out_;
    WriteCounts& counts_;
    bool boldIsBody_ = false;
    bool italicIsBody_ = false;
    std::vector<GlyphRef> refs_;
    std::vector<Word> words_;
    std::string plain_;
    std::string other_;
};

void Writer::CollectWords(const Block& block) {
    refs_.clear();
    words_.clear();
    bool joinNext = false;  // the previous line ended in a hyphen that joins its word to this line
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const BlockLine& ref = lines_[bl];
        const PageData& page = pages_[ref.page];
        const Line& line = page.lines[ref.line];
        for (uint32_t k = ref.skip; k < line.count; ++k) {
            const uint32_t gi = page.lineGlyphs[line.first + k];
            const Glyph& g = page.glyphs[gi];
            const std::string_view text = page.GlyphText(g);
            // Soft hyphens are invisible except at a line break.
            if (text == "\xC2\xAD" && k + 1 < line.count) continue;
            bool newWord = words_.empty() || (g.flags & kGlyphSpaceBefore) || k == ref.skip;
            if (k == ref.skip && joinNext) newWord = false;
            if (newWord) {
                Word w;
                w.first = static_cast<uint32_t>(refs_.size());
                w.spaceBefore = !words_.empty();
                // CJK text has no word spaces: a line break between two CJK characters joins them.
                if (w.spaceBefore && k == ref.skip && IsCjk(FirstOf(text)) && IsCjk(LastOf(Text(refs_.back()))))
                    w.spaceBefore = false;
                words_.push_back(w);
            }
            uint8_t script = kScriptNone;
            if (IsSuperscript(g, line))
                script = kScriptSup;
            else if (IsSubscript(g, line))
                script = kScriptSub;
            refs_.push_back(GlyphRef{ref.page, gi, script});
            words_.back().count++;
        }
        joinNext = false;
        // Dehyphenation (algorithms §4.4): a hyphen ending a line, after a letter, before a line
        // that starts in lowercase, joins the halves of the word.
        if (bl + 1 >= block.first + block.count || words_.empty()) continue;
        Word& w = words_.back();
        if (w.count < 2) continue;
        const std::string_view last = Text(refs_[w.first + w.count - 1]);
        if (last != "-" && last != "\xC2\xAD" && last != "\xE2\x80\x90") continue;
        const std::string_view before = Text(refs_[w.first + w.count - 2]);
        const unsigned char b0 = before.empty() ? 0 : static_cast<unsigned char>(before[0]);
        const bool letterBefore = (b0 >= 'a' && b0 <= 'z') || (b0 >= 'A' && b0 <= 'Z') || b0 >= 0xC3;
        const BlockLine& nextRef = lines_[bl + 1];
        const PageData& nextPage = pages_[nextRef.page];
        const Line& nextLine = nextPage.lines[nextRef.line];
        if (nextRef.skip >= nextLine.count) continue;
        const std::string_view next = nextPage.GlyphText(nextPage.glyphs[nextPage.lineGlyphs[nextLine.first + nextRef.skip]]);
        const unsigned char n0 = next.empty() ? 0 : static_cast<unsigned char>(next[0]);
        const bool lowerNext = (n0 >= 'a' && n0 <= 'z') || n0 == 0xC3 || n0 == 0xC4 || n0 == 0xC5;
        if (!letterBefore || !lowerNext) {
            // A soft hyphen that does not join still leaves no character.
            if (last == "\xC2\xAD") {
                w.count--;
                refs_.pop_back();
            }
            continue;
        }
        w.count--;
        refs_.pop_back();
        joinNext = true;
    }
    // Empty words (a lone soft hyphen) leave.
    size_t kept = 0;
    for (size_t i = 0; i < words_.size(); ++i)
        if (words_[i].count > 0) words_[kept++] = words_[i];
    words_.resize(kept);
    if (!words_.empty()) words_[0].spaceBefore = false;
}

void Writer::StyleWords(bool styles) {
    for (Word& w : words_) {
        w.style = 0;
        w.link = -1;
        w.linkPage = -1;
        if (!styles) continue;
        uint32_t considered = 0, bold = 0, italic = 0, mono = 0;
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (uint32_t k = w.first; k < w.first + w.count; ++k) {
            const Glyph& g = pages_[refs_[k].page].glyphs[refs_[k].glyph];
            x0 = std::fmin(x0, g.x0), y0 = std::fmin(y0, g.y0), x1 = std::fmax(x1, g.x1), y1 = std::fmax(y1, g.y1);
            if (!StartsLetterOrDigit(Text(refs_[k]))) continue;
            considered++;
            if (g.flags & kGlyphBold) bold++;
            if (g.flags & kGlyphItalic) italic++;
            if (g.flags & kGlyphMonospace) mono++;
        }
        if (considered > 0) {
            if (mono == considered) {
                w.style = kStyleCode;
            } else {
                if (bold == considered && !boldIsBody_) w.style |= kStyleBold;
                if (italic == considered && !italicIsBody_) w.style |= kStyleItalic;
            }
        }
        // A link annotation over the word's centre (algorithms §6.8).
        const uint32_t page = refs_[w.first].page;
        const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        const std::vector<LinkArea>& links = pages_[page].links;
        for (size_t l = 0; l < links.size(); ++l) {
            const LinkArea& a = links[l];
            if (cx >= a.x0 - 1 && cx <= a.x1 + 1 && cy >= a.y0 - 1 && cy <= a.y1 + 1) {
                w.link = static_cast<int32_t>(l);
                w.linkPage = static_cast<int32_t>(page);
                break;
            }
        }
    }
    // Links whose text is their URL stay text: GFM makes them autolinks.
    for (size_t i = 0; i < words_.size();) {
        if (words_[i].link < 0) {
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < words_.size() && SameLink(words_[i], words_[end])) ++end;
        plain_.clear();
        for (size_t j = i; j < end; ++j) {
            if (j > i && words_[j].spaceBefore) plain_.push_back(' ');
            for (uint32_t k = words_[j].first; k < words_[j].first + words_[j].count; ++k) plain_.append(Text(refs_[k]));
        }
        std::string_view uri = Uri(words_[i]);
        std::string_view text = plain_;
        bool bare = text == uri;
        if (!bare && uri.substr(0, 7) == "mailto:") bare = text == uri.substr(7);
        if (!bare && (uri.substr(0, 7) == "http://" || uri.substr(0, 8) == "https://")) {
            std::string_view rest = uri.substr(uri[4] == 's' ? 8 : 7);
            if (!rest.empty() && rest.back() == '/' && (text.empty() || text.back() != '/')) rest.remove_suffix(1);
            bare = text == rest && text.substr(0, 4) == "www.";
            if (!bare && !text.empty() && text.back() == '/') bare = false;
        }
        // Link text that shows a URL stays text: GFM autolinks it, and the visible URL, broken
        // over lines or cut short, is what the page says.
        const bool url = text.substr(0, 7) == "http://" || text.substr(0, 8) == "https://" || text.substr(0, 4) == "www.";
        const bool autolinkable = url || text.find('@') != std::string_view::npos;
        if ((bare && autolinkable) || url) {
            for (size_t j = i; j < end; ++j) words_[j].link = -1;
        } else {
            counts_.links++;
        }
        i = end;
    }
}

// Escapes only what would change the structure (D6): emphasis delimiters, code spans, HTML and
// autolink openers, entity references, and block markers at the start of the block.
void Writer::AppendEscaped(std::string_view t, bool blockStart) {
    if (blockStart && !t.empty()) {
        const char c = t[0];
        bool allHashes = t.size() <= 6;
        for (char h : t) allHashes = allHashes && h == '#';
        if (allHashes || c == '>' || ((c == '-' || c == '+' || c == '=') && t.find_first_not_of(c) == std::string_view::npos) ||
            (c == '~' && t.size() >= 3 && t.substr(0, 3) == "~~~") || (c == '[' && t.find("]:") != std::string_view::npos)) {
            out_.push_back('\\');
        } else if (c >= '0' && c <= '9') {
            size_t d = 0;
            while (d < t.size() && t[d] >= '0' && t[d] <= '9') ++d;
            if (d <= 9 && d + 1 == t.size() && (t[d] == '.' || t[d] == ')')) {
                out_.append(t.substr(0, d));
                out_.push_back('\\');
                out_.push_back(t[d]);
                return;
            }
        }
    }
    // A word of underscores only (a form's blank) is neither left- nor right-flanking between
    // spaces: it opens or closes no emphasis and needs no escapes.
    if (!blockStart && t.size() >= 2 && t.find_first_not_of('_') == std::string_view::npos) {
        out_.append(t);
        return;
    }
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        const char prev = i > 0 ? t[i - 1] : ' ';
        const char next = i + 1 < t.size() ? t[i + 1] : ' ';
        switch (c) {
        case '\\':
        case '*':
        case '`':
            out_.push_back('\\');
            break;
        case '_':
            if (!IsAsciiAlnum(prev) || !IsAsciiAlnum(next)) out_.push_back('\\');
            break;
        case '<':
            if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || next == '/' || next == '!' || next == '?')
                out_.push_back('\\');
            break;
        case ']':
            if (next == '(' || next == '[') out_.push_back('\\');
            break;
        case '~':
            if (next == '~' || prev == '~') out_.push_back('\\');
            break;
        case '&': {
            size_t j = i + 1;
            while (j < t.size() && (IsAsciiAlnum(t[j]) || t[j] == '#')) ++j;
            if (j > i + 1 && j < t.size() && t[j] == ';') out_.push_back('\\');
            break;
        }
        default:
            break;
        }
        out_.push_back(c);
    }
}

void Writer::Inline(const Block& block, bool styles) {
    CollectWords(block);
    StyleWords(styles);
    uint8_t open = 0;
    int32_t openLinkPage = -1, openLink = -1;
    // Heading and table cell text is inline: "## 1. Installation" opens no list.
    bool blockStart = block.kind != BlockKind::Heading && block.kind != BlockKind::Table;
    std::string_view openUri;
    const auto closeAll = [&]() {
        if (open & kStyleItalic) out_.push_back('*');
        if (open & kStyleBold) out_.append("**");
        if (open & kStyleCode) out_.push_back('`');
        open = 0;
        if (openLink >= 0) {
            out_.append("](");
            const bool angle = openUri.find_first_of(" ()<>") != std::string_view::npos;
            if (angle) out_.push_back('<');
            for (char c : openUri) {
                if (angle && (c == '<' || c == '>'))
                    out_.append(c == '<' ? "%3C" : "%3E");
                else
                    out_.push_back(c);
            }
            if (angle) out_.push_back('>');
            out_.push_back(')');
            openLink = -1;
            openLinkPage = -1;
        }
    };
    for (size_t i = 0; i < words_.size(); ++i) {
        const Word& w = words_[i];
        const bool linkChanges = openLink >= 0 ? (w.link < 0 || Uri(w) != openUri) : w.link >= 0;
        if (w.style != open || linkChanges) {
            // Emphasis closes before the space so that its delimiters stay flanking.
            if (linkChanges) {
                closeAll();
            } else {
                if (open & kStyleItalic) out_.push_back('*');
                if (open & kStyleBold) out_.append("**");
                if (open & kStyleCode) out_.push_back('`');
                open = 0;
            }
        }
        if (w.spaceBefore) out_.push_back(' ');
        if (w.link >= 0 && openLink < 0) {
            out_.push_back('[');
            openLink = w.link;
            openLinkPage = w.linkPage;
            openUri = Uri(w);
            blockStart = false;
        }
        if (w.style != open) {
            if (w.style & kStyleCode) out_.push_back('`');
            if (w.style & kStyleBold) out_.append("**");
            if (w.style & kStyleItalic) out_.push_back('*');
            open = w.style;
            blockStart = false;
        }
        if (open & kStyleCode) {
            // Code spans hold their text as is; a backtick inside cannot be written in a single-backtick
            // span, so it becomes an apostrophe-like grave accent outside ASCII.
            for (uint32_t k = w.first; k < w.first + w.count; ++k) {
                for (char c : Text(refs_[k])) out_.push_back(c == '`' ? '\'' : c);
            }
            continue;
        }
        // The word in script runs.
        uint32_t k = w.first;
        const uint32_t end = w.first + w.count;
        while (k < end) {
            const uint8_t script = refs_[k].script;
            uint32_t runEnd = k + 1;
            while (runEnd < end && refs_[runEnd].script == script) ++runEnd;
            if (script == kScriptSup && !noteRefs_.empty()) {
                if (const uint32_t number = NoteNumber(refs_[k], runEnd - k)) {
                    out_.append("[^");
                    out_.append(std::to_string(number));
                    out_.push_back(']');
                    blockStart = false;
                    k = runEnd;
                    continue;
                }
            }
            // An inline image glyph holds its Markdown link.
            if (pages_[refs_[k].page].glyphs[refs_[k].glyph].flags & kGlyphImage) {
                out_.append(Text(refs_[k]));
                blockStart = false;
                ++k;
                continue;
            }
            other_.clear();
            for (uint32_t j = k; j < runEnd; ++j) other_.append(Text(refs_[j]));
            if (script == kScriptSup) out_.append("<sup>");
            if (script == kScriptSub) out_.append("<sub>");
            AppendEscaped(other_, blockStart && script == kScriptNone);
            if (script == kScriptSup) out_.append("</sup>");
            if (script == kScriptSub) out_.append("</sub>");
            blockStart = false;
            k = runEnd;
        }
    }
    closeAll();
    (void)openLinkPage;
    // No trailing spaces.
    while (!out_.empty() && out_.back() == ' ') out_.pop_back();
}

void Writer::Code(const Block& block, const std::string& prefix) {
    // The character grid: the median advance of the block's glyphs (algorithms §6.4).
    std::vector<float> widths;
    float left = INFINITY;
    std::vector<float> pitches;
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const PageData& page = pages_[lines_[bl].page];
        const Line& line = page.lines[lines_[bl].line];
        left = std::fmin(left, line.u0);
        for (uint32_t k = 0; k < line.count && widths.size() < 4096; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            if (g.u1 > g.u0) widths.push_back(g.u1 - g.u0);
        }
        if (bl > block.first && lines_[bl - 1].page == lines_[bl].page) {
            const float pitch = page.lines[lines_[bl - 1].line].baseline - line.baseline;
            if (pitch > 0) pitches.push_back(pitch);
        }
    }
    float cell = 0;
    if (!widths.empty()) {
        std::nth_element(widths.begin(), widths.begin() + static_cast<std::ptrdiff_t>(widths.size() / 2), widths.end());
        cell = widths[widths.size() / 2];
    }
    float pitch = 0;
    if (!pitches.empty()) pitch = *std::min_element(pitches.begin(), pitches.end());

    // The body first, to choose a fence longer than any backtick run inside.
    other_.clear();
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const PageData& page = pages_[lines_[bl].page];
        const Line& line = page.lines[lines_[bl].line];
        if (bl > block.first) {
            other_.push_back('\n');
            if (pitch > 0 && lines_[bl - 1].page == lines_[bl].page) {
                const float gap = page.lines[lines_[bl - 1].line].baseline - line.baseline;
                // Decided on the float: a huge or non-finite gap must not reach the integer
                // conversion (fuzz_layout_tables found -2147483648 - 1 here); NaN gives none.
                const float lines = gap / pitch;
                const int blanks = lines >= 3.5f ? 3 : lines >= 1.5f ? static_cast<int>(std::lround(lines)) - 1 : 0;
                for (int b = 0; b < blanks; ++b) other_.push_back('\n');
            }
        }
        int column = 0;
        for (uint32_t k = 0; k < line.count; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            int target = column;
            if (cell > 0) {
                // Compared as a float before converting, for the same reason as the blank lines.
                const float at = (g.u0 - left) / cell;
                if (at >= static_cast<float>(column + 200)) {
                    target = column + 200;
                } else if (at > static_cast<float>(column)) {
                    target = static_cast<int>(std::lround(at));
                }
            }
            if (target > column) {
                other_.append(static_cast<size_t>(target - column), ' ');
                column = target;
            } else if (k > 0 && (g.flags & kGlyphSpaceBefore)) {
                other_.push_back(' ');
                column++;
            }
            const std::string_view t = page.GlyphText(g);
            other_.append(t);
            for (unsigned char c : t)
                if ((c & 0xC0) != 0x80) column++;
        }
        while (!other_.empty() && other_.back() == ' ') other_.pop_back();
    }
    size_t run = 0, longest = 0;
    for (char c : other_) {
        run = c == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string fence(std::max<size_t>(3, longest + 1), '`');
    out_ += prefix;
    out_ += fence;
    out_.push_back('\n');
    out_ += prefix;
    for (char c : other_) {
        out_.push_back(c);
        if (c == '\n') out_ += prefix;
    }
    out_.push_back('\n');
    out_ += prefix;
    out_ += fence;
}

} // namespace

void WriteMarkdown(const std::vector<PageData>& pages, const BlockList& list, bool repaired, std::string& out,
                   WriteCounts& counts) {
    Writer writer(pages, list, out, counts);
    if (repaired) out += "<!-- rayopdf: the file was damaged and has been repaired; text may be missing -->\n";
    int contentColumn[9] = {};
    const Block* previous = nullptr;
    std::string prefix;
    std::vector<std::string> cellText;
    std::vector<TablePart> parts;
    std::vector<uint32_t> partFirst;
    for (const Block& block : list.blocks) {
        if (block.kind != BlockKind::Comment && block.count == 0) continue;
        if (!out.empty()) {
            const bool tight = previous && previous->kind == BlockKind::ListItem && block.kind == BlockKind::ListItem &&
                               previous->quote == block.quote;
            out += tight ? "\n" : "\n\n";
        }
        previous = &block;
        prefix.clear();
        for (int q = 0; q < block.quote; ++q) prefix += "> ";
        switch (block.kind) {
        case BlockKind::Comment:
            out += "<!-- rayopdf: page ";
            out += std::to_string(block.number);
            out += block.level == static_cast<uint8_t>(CommentKind::NoText) ? " has no extractable text -->"
                                                                             : " text is mostly unmapped glyphs -->";
            break;
        case BlockKind::Heading:
            counts.headings++;
            out += prefix;
            out.append(std::max<int>(1, std::min<int>(6, block.level)), '#');
            out.push_back(' ');
            writer.Inline(block, false);
            break;
        case BlockKind::Paragraph: {
            counts.paragraphs++;
            out += prefix;
            if (block.listParagraph) out.append(static_cast<size_t>(contentColumn[std::min<int>(block.depth, 8)]), ' ');
            writer.Inline(block, true);
            break;
        }
        case BlockKind::ListItem: {
            counts.listItems++;
            out += prefix;
            const int depth = std::min<int>(block.depth, 8);
            const int indent = depth == 0 ? 0 : contentColumn[depth - 1];
            out.append(static_cast<size_t>(indent), ' ');
            std::string marker = block.ordered ? std::to_string(block.number == 0 ? 1 : block.number) + "." : "-";
            out += marker;
            out.push_back(' ');
            contentColumn[depth] = indent + static_cast<int>(marker.size()) + 1;
            for (int d = depth + 1; d < 9; ++d) contentColumn[d] = contentColumn[depth] + 2 * (d - depth);
            if (block.task) out += block.task == 2 ? "[x] " : "[ ] ";
            writer.Inline(block, true);
            break;
        }
        case BlockKind::Code:
            counts.codeBlocks++;
            writer.Code(block, prefix);
            break;
        case BlockKind::Footnote: {
            counts.footnotes++;
            out += "[^";
            out += std::to_string(block.number);
            out += "]: ";
            writer.Inline(block, true);
            // An HTML endnote's back link (↩, maybe with the text-style selector U+FE0E) is no text.
            for (const std::string_view link : {"\xE2\x86\xA9\xEF\xB8\x8E", "\xE2\x86\xA9"}) {
                if (out.size() >= link.size() && out.compare(out.size() - link.size(), link.size(), link) == 0) {
                    out.resize(out.size() - link.size());
                    break;
                }
            }
            while (!out.empty() && out.back() == ' ') out.pop_back();
            break;
        }
        case BlockKind::Table: {
            // D6: GFM when no cell spans, else HTML (layout/tables.h). A table broken across pages is
            // one table from its parts, each its anchor line then its cell lines. GFM cells are inline
            // Markdown, cut back out of `out` after the writer appends them.
            parts.clear();
            partFirst.clear();
            bool html = false;
            for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
                const PageData& page = pages[list.lines[bl].page];
                const Line& line = page.lines[list.lines[bl].line];
                if (!(line.flags & kLineTableAnchor)) continue;
                parts.push_back(TablePart{&page, &page.tables[line.first], nullptr});
                partFirst.push_back(bl);
                html = html || TableNeedsHtml(page, page.tables[line.first]);
            }
            if (parts.empty()) break;
            counts.tables++;
            if (html) {
                WriteHtmlTable(parts, out);
                break;
            }
            size_t total = 0;
            for (const TablePart& part : parts) total += part.table->cellCount;
            cellText.resize(total);
            size_t at = 0;
            for (size_t p = 0; p < parts.size(); ++p) {
                const Table& table = *parts[p].table;
                for (uint32_t c = 0; c < table.cellCount; ++c) {
                    const TableCell& cell = parts[p].page->tableCells[table.firstCell + c];
                    std::string& text = cellText[at + c];
                    text.clear();
                    if (cell.count == 0) continue;
                    Block sub;
                    sub.kind = BlockKind::Table;
                    sub.first = partFirst[p] + 1 + (cell.first - table.firstLine);
                    sub.count = cell.count;
                    const size_t mark = out.size();
                    writer.Inline(sub, true);
                    text.assign(out, mark, std::string::npos);
                    out.resize(mark);
                }
                parts[p].cellText = cellText.data() + at;
                at += table.cellCount;
            }
            WriteGfmTable(parts, out);
            break;
        }
        case BlockKind::Image: {
            const BlockLine& anchor = list.lines[block.first];
            const PageData& page = pages[anchor.page];
            const ImageBox& image = page.images[page.lines[anchor.line].first];
            const std::string_view name = page.ImageName(image);
            if (image.state != ImageState::Written) {
                out += "<!-- rayopdf: image on page ";
                out += std::to_string(block.number);
                out += " not extracted (";
                out += name;
                out += ") -->";
                break;
            }
            out += prefix;
            out += "![](";
            const bool angle = name.find_first_of(" ()<>") != std::string_view::npos;
            if (angle) out.push_back('<');
            for (char c : name) {
                if (angle && (c == '<' || c == '>'))
                    out.append(c == '<' ? "%3C" : "%3E");
                else
                    out.push_back(c);
            }
            if (angle) out.push_back('>');
            out.push_back(')');
            break;
        }
        }
    }
    if (!out.empty()) out.push_back('\n');
}

} // namespace RayoPdf::Layout
