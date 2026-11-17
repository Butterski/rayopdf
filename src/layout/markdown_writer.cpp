// SPDX-License-Identifier: Apache-2.0
#include "layout/markdown_writer.h"

#include "common/batch.h"
#include "common/minmax.h"
#include "common/utf8.h"
#include "layout/lines.h"
#include "layout/tables.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

#ifndef RAYOPDF_VERSION
#define RAYOPDF_VERSION "0.0.0"
#endif

namespace RayoPdf::Layout {
namespace {

// Inline styles, each written with its delimiter. Code spans hold no other markup, so a code span
// is always the innermost one open.
enum : uint8_t {
    kStyleBold = 1,
    kStyleItalic = 2,
    kStyleCode = 4,
    kStyleStrike = 8,
};
constexpr int kStyleCount = 4;
constexpr std::string_view kDelimiters[kStyleCount] = {"**", "*", "`", "~~"};

enum : uint8_t { kScriptNone = 0, kScriptSup = 1, kScriptSub = 2 };

// A glyph of the block being written, with its text (in its page's text) at hand.
struct GlyphRef {
    const char* text;
    uint32_t page;
    uint32_t glyph;
    uint16_t length;
    uint16_t flags;  // the glyph's flags
    uint8_t script;
    uint8_t lineStart;  // the first glyph of one of the block's lines
};

struct Word {
    uint32_t first = 0, count = 0;  // GlyphRefs
    bool spaceBefore = false;
    int32_t linkPage = -1;          // the link annotation over the word
    int32_t link = -1;
    uint32_t linkTrim = 0;          // glyphs ending the word that follow its link
};

// A run of `*` or `_` AppendEscaped wrote escaped: from out_[at], `length` characters (2 x length
// bytes).
struct EscapedRun {
    size_t at;
    size_t length;
};

bool IsAsciiAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

RAYOPDF_HOT_INLINE uint32_t FirstOf(std::string_view t) {
    if (!t.empty() && static_cast<unsigned char>(t[0]) < 0x80) return static_cast<unsigned char>(t[0]);
    uint32_t cp = 0;
    size_t length = 0;
    return Utf8::Decode(t, 0, cp, length) ? cp : 0;
}

// A letter or digit: ASCII ones and every code point from U+00C0 up outside the symbol,
// punctuation, private-use and fullwidth-punctuation blocks ("…" and "—" are punctuation).
bool IsLetterOrDigit(uint32_t cp) {
    if (cp < 0x80) return IsAsciiAlnum(static_cast<char>(cp));
    return cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x2000 && cp <= 0x2BFF) && !(cp >= 0x3000 && cp <= 0x303F) &&
           !(cp >= 0xE000 && cp <= 0xF8FF) && !(cp >= 0xFE00 && cp <= 0xFE0F) && !(cp >= 0xFF00 && cp <= 0xFF20) &&
           cp != 0xFFFD;
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

// The Unicode superscript (`sup`) or subscript form of `cp`, 0 when Unicode has none. Each form's
// compatibility decomposition (NFKC) is `cp`, or for a mathematical bold, italic or bold italic
// letter (𝑥) its letter: the text stays the same.
uint32_t ScriptForm(uint32_t cp, bool sup) {
    if (cp >= 0x1D400 && cp <= 0x1D49B) {
        const uint32_t i = (cp - 0x1D400) % 52;
        cp = i < 26 ? 'A' + i : 'a' + (i - 26);
    } else if (cp == 0x210E) {
        cp = 'h';  // the mathematical italic h
    }
    static constexpr uint16_t kSupDigits[10] = {0x2070, 0xB9, 0xB2, 0xB3, 0x2074, 0x2075, 0x2076, 0x2077, 0x2078, 0x2079};
    static constexpr uint16_t kSupSmall[26] = {0x1D43, 0x1D47, 0x1D9C, 0x1D48, 0x1D49, 0x1DA0, 0x1D4D, 0x2B0, 0x2071,
                                               0x2B2,  0x1D4F, 0x2E1,  0x1D50, 0x207F, 0x1D52, 0x1D56, 0,     0x2B3,
                                               0x2E2,  0x1D57, 0x1D58, 0x1D5B, 0x2B7,  0x2E3,  0x2B8,  0x1DBB};
    static constexpr uint16_t kSupCapital[26] = {0x1D2C, 0x1D2E, 0,      0x1D30, 0x1D31, 0,      0x1D33, 0x1D34, 0x1D35,
                                                 0x1D36, 0x1D37, 0x1D38, 0x1D39, 0x1D3A, 0x1D3C, 0x1D3E, 0,      0x1D3F,
                                                 0,      0x1D40, 0x1D41, 0x2C7D, 0x1D42, 0,      0,      0};
    static constexpr uint16_t kSubSmall[26] = {0x2090, 0,      0,      0,      0x2091, 0,      0,      0x2095, 0x1D62,
                                               0x2C7C, 0x2096, 0x2097, 0x2098, 0x2099, 0x2092, 0x209A, 0,      0x1D63,
                                               0x209B, 0x209C, 0x1D64, 0x1D65, 0,      0x2093, 0,      0};
    if (cp >= '0' && cp <= '9') return sup ? kSupDigits[cp - '0'] : 0x2080 + (cp - '0');
    if (cp >= 'a' && cp <= 'z') return sup ? kSupSmall[cp - 'a'] : kSubSmall[cp - 'a'];
    if (cp >= 'A' && cp <= 'Z') return sup ? kSupCapital[cp - 'A'] : 0;
    const uint32_t offset = sup ? 0 : 0x10;
    switch (cp) {
    case '+': return 0x207A + offset;
    case 0x2212: return 0x207B + offset;  // minus sign; a hyphen has no form
    case '=': return 0x207C + offset;
    case '(': return 0x207D + offset;
    case ')': return 0x207E + offset;
    case 0x3B2: return sup ? 0x1D5D : 0x1D66;  // β
    case 0x3B3: return sup ? 0x1D5E : 0x1D67;  // γ
    case 0x3B4: return sup ? 0x1D5F : 0;       // δ
    case 0x3B8: return sup ? 0x1DBF : 0;       // θ
    case 0x3C1: return sup ? 0 : 0x1D68;       // ρ
    case 0x3C6: return sup ? 0x1D60 : 0x1D69;  // φ
    case 0x3C7: return sup ? 0x1D61 : 0x1D6A;  // χ
    default: return 0;
    }
}

class Writer {
public:
    Writer(const std::vector<PageData>& pages, const BlockList& blocks, std::string& out, WriteCounts& counts, MathMode math,
           bool plain, bool smallCapsLower)
        : pages_(pages), lines_(blocks.lines), noteRefs_(blocks.noteRefs), out_(out), counts_(counts),
          boldIsBody_(blocks.boldBody), italicIsBody_(blocks.italicBody), monoIsBody_(blocks.monoDocument), math_(math),
          plainOutput_(plain), smallCapsLower_(smallCapsLower) {
        for (const NoteRef& r : noteRefs_) letterRefs_ = letterRefs_ || r.count == 3;
    }

    // The block's text as one Markdown line; `styles` writes emphasis, code spans and links.
    void Inline(const Block& block, bool styles);
    // A heading's text glyph by glyph, without styles, as one Markdown line: a heading in small
    // capitals (Block::smallCaps) in the source's case, its small capitals written as lower-case
    // letters (as printed with SmallCaps::Keep); a label run in with its paragraph (Block::number),
    // its glyphs alone. False (nothing written) when a glyph of a small-capitals heading may be
    // math in LaTeX mode: Inline writes it then.
    bool PlainHeading(const Block& block);
    // A fenced code block; `prefix` starts every line (block quotes).
    void Code(const Block& block, const std::string& prefix);
    // A display formula's block (one kLineFormula line) as $$…$$ LaTeX; false (nothing written)
    // when the formula cannot be rebuilt.
    bool Display(const Block& block);

private:
    // Inline's path for a block that needs none of its passes: no glyph in a style it writes, no
    // script, no inline image, no link annotation on its pages (with styles) and, in LaTeX mode
    // (kLatex), no glyph that may be math. Its words go to out_ as they are collected, by
    // CollectWords' word, soft hyphen and dehyphenation rules and Inline's escaping. False, with
    // out_ as it was, when the block needs the full path.
    template <bool kLatex>
    bool InlinePlain(const Block& block, bool styles);
    void CollectWords(const Block& block);
    void StyleWords(bool styles);
    // Opens and closes delimiters so that exactly the styles `target` are open, nested properly;
    // `at` is the glyph they apply from (styles that run longer open first, outside).
    void SetStyle(uint8_t target, uint32_t at);
    void AppendEscaped(std::string_view text, bool blockStart);
    // Unescapes the runs AppendEscaped recorded in out_ from `from` (the block's inline text) that
    // CommonMark reads as no emphasis delimiter as they are: one that cannot open, and can close
    // nothing because no `*` or `_` of its kind stands raw before it. Literal stars ("0.05**")
    // stay as they are.
    void DropNeedlessEscapes(size_t from);
    // Leader dots set one by one (". . . . 17" after an entry of a list of contents) fill space and
    // make no words: in out_ from `from`, four dots and more a space apart become one run of dots.
    void JoinLeaderDots(size_t from);
    // LaTeX mode: the inline formulas of the collected glyphs (mathRun_).
    void FindFormulas();
    // The LaTeX of the glyphs refs_[from, to) as `$…$` appended to out_; false when not rebuilt.
    bool WriteFormula(uint32_t from, uint32_t to);
    // A tagged formula's LaTeX (PageData::formulaAlts) appended to out_: its ends trimmed, line
    // breaks and other control characters as spaces.
    void AppendFormulaAlt(std::string_view alt);
    // The tagged formula (index + 1 into its page's formulaAlts) that is a block of its own and
    // holds every glyph of `block`, or 0.
    uint32_t TaggedDisplay(const Block& block) const;
    // Unicode mode (D6): a formula's script written in Unicode superscript or subscript forms
    // ("mc²", "x₀"). The glyphs refs_[k, …) of word `w`, before `stop`, raised or lowered alike
    // from `k` on are a formula's script when one of them, or their base (the glyph before them
    // and the scripts between), is set in a math font or is a mathematical alphanumeric, a Greek
    // letter or a math symbol, or when the base ends a variable: one to three italic letters in a
    // text font whose body is upright. Scripts of text (H<sub>2</sub>O, 2<sup>10</sup>,
    // "1<sup>st</sup>"), reference marks and scripts with a character that has no form stay
    // <sup>/<sub>. Returns the script's end with its forms in forms_; 0 when it stays a tag.
    uint32_t ScriptForms(const Word& w, uint32_t k, uint32_t stop);
    // The footnote number of the superscript run starting at `r`, `count` glyphs long; 0 when none.
    uint32_t NoteNumber(const GlyphRef& r, uint32_t count) const {
        const auto it = std::lower_bound(noteRefs_.begin(), noteRefs_.end(), r, [](const NoteRef& n, const GlyphRef& g) {
            return n.page != g.page ? n.page < g.page : n.glyph < g.glyph;
        });
        return it != noteRefs_.end() && it->page == r.page && it->glyph == r.glyph && it->count == count ? it->number : 0;
    }
    static std::string_view Text(const GlyphRef& r) { return std::string_view(r.text, r.length); }
    std::string_view Uri(const Word& w) const {
        const PageData& page = pages_[static_cast<size_t>(w.linkPage)];
        return page.LinkUri(page.links[static_cast<size_t>(w.link)]);
    }
    // Words of one link: under the same annotation, or under annotations with the same target,
    // unless a comma or semicolon ends the first word: two links to one place ("[a](u),
    // [a](u)"), not a link drawn word by word or wrapped over lines.
    bool SameLink(const Word& a, const Word& b) const {
        if (a.link < 0 || b.link < 0) return a.link == b.link;
        if (a.linkPage == b.linkPage && a.link == b.link) return true;
        if (Uri(a) != Uri(b)) return false;
        const std::string_view end = Text(refs_[a.first + a.count - 1]);
        return !(end.size() == 1 && (end[0] == ',' || end[0] == ';'));
    }

    const std::vector<PageData>& pages_;
    const std::vector<BlockLine>& lines_;
    const std::vector<NoteRef>& noteRefs_;
    std::string& out_;
    WriteCounts& counts_;
    bool boldIsBody_ = false;
    bool italicIsBody_ = false;
    bool monoIsBody_ = false;  // the document is typeset in a monospace font (BlockList::monoDocument)
    MathMode math_ = MathMode::Unicode;
    // WriteSettings::plain: scripts without <sup>/<sub> tags, footnote references as their number.
    bool plainOutput_ = false;
    // WriteSettings::smallCaps Lower: a small-capitals heading's small capitals as lower-case letters.
    bool smallCapsLower_ = true;
    // Some reference spans three glyphs: unraised "(a)" references (semantics.h) may be among them.
    bool letterRefs_ = false;
    std::vector<GlyphRef> refs_;
    std::vector<Word> words_;
    std::vector<uint8_t> styles_;    // per GlyphRef: the styles it is written in
    std::vector<uint8_t> kinds_;     // per Word: upright, italic phrase, math or operator (StyleWords)
    std::vector<uint32_t> wordOf_;   // per GlyphRef: its word (CollectWords)
    size_t runFrom_[kStyleCount] = {}, runTo_[kStyleCount] = {};  // per style: the last run SetStyle walked
    std::vector<int32_t> before_;    // per GlyphRef: the nearest letter glyph before it, and firm letter
    uint8_t set_ = 0;                // the styles some glyph of the block is set in (CollectWords)
    uint8_t stack_[kStyleCount] = {};  // open styles, outermost first
    int depth_ = 0;
    std::string plain_;
    std::string other_;
    std::vector<EscapedRun> runs_;   // the runs AppendEscaped wrote since the block's inline text began
    std::string forms_;  // ScriptForms' forms
    // LaTeX mode.
    std::vector<InlineGlyph> inline_;
    std::vector<uint32_t> mathRun_;  // per GlyphRef: the end of the formula starting there, or 0
    MathInput mathIn_;
    std::string latex_, tag_;
};

void Writer::CollectWords(const Block& block) {
    words_.clear();
    // refs_ (and wordOf_) are sized once for every glyph of the block's lines and cut to the
    // glyphs kept.
    size_t bound = 0;
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const BlockLine& ref = lines_[bl];
        const Line& line = pages_[ref.page].lines[ref.line];
        if (ref.skip < line.count) bound += line.count - ref.skip;
    }
    refs_.resize(bound);
    if (wordOf_.size() < bound) wordOf_.resize(bound);
    GlyphRef* const refs = refs_.data();
    uint32_t* const wordOf = wordOf_.data();
    uint32_t n = 0;
    uint32_t word = 0;  // the word being collected: words_.size() - 1 once there is one
    // Words' counts follow from where the next one starts (only the last word's last glyph is ever
    // taken out again); the styles some glyph is set in (StyleWords) from the faces of the glyphs
    // that are no image. A hyphen taken out again may add to them, which only makes StyleWords
    // look for styles no glyph has.
    uint16_t faces = 0;
    bool code = false;
    bool joinNext = false;  // the previous line ended in a hyphen that joins its word to this line
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const BlockLine& ref = lines_[bl];
        const PageData& page = pages_[ref.page];
        const Line& line = page.lines[ref.line];
        const uint32_t lineFirst = n;
        bool carriedSpace = false;  // a soft hyphen left out had the word break before it
        for (uint32_t k = ref.skip; k < line.count; ++k) {
            const uint32_t gi = page.lineGlyphs[line.first + k];
            const Glyph& g = page.glyphs[gi];
            const std::string_view text = page.GlyphText(g);
            // Soft hyphens are invisible except at a line break; a word break before one stays.
            if (text.size() == 2 && text[0] == '\xC2' && text[1] == '\xAD' && k + 1 < line.count) {
                carriedSpace = carriedSpace || (g.flags & kGlyphSpaceBefore) != 0;
                continue;
            }
            const uint16_t f = g.flags;
            const bool lineStart = n == lineFirst;  // the line's first glyph written
            bool newWord = n == 0 || (f & kGlyphSpaceBefore) || carriedSpace || lineStart;
            carriedSpace = false;
            if (lineStart && joinNext) newWord = false;
            if (newWord) {
                Word w;
                w.first = n;
                w.spaceBefore = n != 0;
                // CJK text has no word spaces: a line break between two CJK characters joins them.
                if (w.spaceBefore && lineStart && IsCjk(FirstOf(text)) && IsCjk(LastOf(Text(refs[n - 1]))))
                    w.spaceBefore = false;
                word = static_cast<uint32_t>(words_.size());
                words_.push_back(w);
            }
            uint8_t script = kScriptNone;
            if (IsSuperscript(g, line))
                script = kScriptSup;
            else if (IsSubscript(g, line))
                script = kScriptSub;
            refs[n] = GlyphRef{text.data(), ref.page, gi, static_cast<uint16_t>(text.size()), f, script,
                               static_cast<uint8_t>(n == lineFirst)};
            wordOf[n] = word;
            ++n;
            if (!(f & kGlyphImage)) {
                faces |= f;
                if ((f & kGlyphMonospace) && !monoIsBody_ && !code) {
                    const uint32_t cp = FirstOf(text);
                    code = !(cp >= 0x1D400 && cp <= 0x1D7FF);
                }
            }
        }
        joinNext = false;
        // Dehyphenation (algorithms §4.4): a hyphen ending a line, after a letter, before a line
        // that starts in lowercase, joins the halves of the word.
        if (bl + 1 >= block.first + block.count || n == 0) continue;
        const uint32_t wordFirst = words_.back().first;
        if (n - wordFirst < 2) continue;
        const std::string_view last = Text(refs[n - 1]);
        if (last != "-" && last != "\xC2\xAD" && last != "\xE2\x80\x90") continue;
        const std::string_view before = Text(refs[n - 2]);
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
            if (last == "\xC2\xAD") --n;
            continue;
        }
        --n;
        joinNext = true;
    }
    refs_.resize(n);
    for (size_t i = 0; i < words_.size(); ++i)
        words_[i].count = (i + 1 < words_.size() ? words_[i + 1].first : n) - words_[i].first;
    uint8_t set = code ? kStyleCode : 0;
    if ((faces & kGlyphBold) && !boldIsBody_) set |= kStyleBold;
    if ((faces & kGlyphItalic) && !italicIsBody_) set |= kStyleItalic;
    if (faces & kGlyphStrike) set |= kStyleStrike;
    set_ = set;
    // Empty words (a lone soft hyphen) leave.
    size_t kept = 0;
    while (kept < words_.size() && words_[kept].count > 0) ++kept;
    if (kept < words_.size()) {
        for (size_t i = kept; i < words_.size(); ++i)
            if (words_[i].count > 0) words_[kept++] = words_[i];
        words_.resize(kept);
        for (uint32_t i = 0; i < kept; ++i)
            for (uint32_t k = words_[i].first; k < words_[i].first + words_[i].count; ++k) wordOf[k] = i;
    }
    if (!words_.empty()) words_[0].spaceBefore = false;
}

void Writer::StyleWords(bool styles) {
    for (Word& w : words_) {
        w.link = -1;
        w.linkPage = -1;
        w.linkTrim = 0;
        if (!styles) continue;
        // A link annotation over the word's centre (algorithms §6.8).
        const uint32_t page = refs_[w.first].page;
        const std::vector<LinkArea>& links = pages_[page].links;
        if (links.empty()) continue;
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        for (uint32_t k = w.first; k < w.first + w.count; ++k) {
            const UprightBox g = GlyphPageBox(pages_[refs_[k].page], refs_[k].glyph);
            x0 = Fmin(x0, g.x0), y0 = Fmin(y0, g.y0), x1 = Fmax(x1, g.x1), y1 = Fmax(y1, g.y1);
        }
        const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
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
        while (end < words_.size() && SameLink(words_[end - 1], words_[end])) ++end;
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
            // A URL broken over lines is one word again when its pieces spell the link's target.
            if (url && plain_.find(' ') != std::string::npos) {
                other_.clear();
                for (char c : plain_)
                    if (c != ' ') other_.push_back(c);
                std::string_view target = uri;
                if (!target.empty() && target.back() == '/' && (other_.empty() || other_.back() != '/')) target.remove_suffix(1);
                if (other_ == target)
                    for (size_t j = i + 1; j < end; ++j) words_[j].spaceBefore = false;
            }
            for (size_t j = i; j < end; ++j) words_[j].link = -1;
        } else {
            counts_.links++;
            // Clause punctuation ending the link's last word follows the link: an annotation drawn
            // over whole words takes in "reference," with its comma. A full stop stays: a linked
            // reference entry ends with its own.
            Word& last = words_[end - 1];
            const bool opened = text.find('(') != std::string_view::npos;
            while (last.linkTrim + 1 < last.count) {
                const std::string_view t = Text(refs_[last.first + last.count - 1 - last.linkTrim]);
                if (t.size() != 1 || std::string_view(",;:").find(t[0]) == std::string_view::npos) {
                    if (!(t == ")" && !opened)) break;
                }
                last.linkTrim++;
            }
        }
        i = end;
    }
    // Styles per glyph (algorithms §6.6). Letters and digits carry their own; a code span is
    // monospace text, and bold, italic and strike-through in it follow the text around it too (a
    // code font may have no bold face). Punctuation and symbols set in a style keep it only next
    // to letters that have it: between two such letters, or attached to one ("**Note:**", but
    // "**bold**," and "*a*, ***b***").
    const uint32_t n = static_cast<uint32_t>(refs_.size());
    styles_.assign(n, 0);
    if (!styles || n == 0) return;
    constexpr uint8_t kLetter = 0x80;  // a letter or digit, while the styles are worked out
    constexpr uint8_t kStyles = kStyleBold | kStyleItalic | kStyleCode | kStyleStrike;
    // A glyph is written in a style only when some glyph of the block is set in it (set_): a block
    // set in none (most body text) has every glyph's styles 0 and needs none of the passes below.
    const uint8_t set = set_;
    if (set == 0) return;
    for (uint32_t k = 0; k < n; ++k) {
        const uint16_t f = refs_[k].flags;
        if (f & kGlyphImage) continue;
        const uint32_t cp = FirstOf(Text(refs_[k]));
        uint8_t s = IsLetterOrDigit(cp) ? kLetter : 0;
        // Mathematical alphanumerics (𝑥, 𝐸) are math whatever the font's pitch: no code.
        if ((f & kGlyphMonospace) && !monoIsBody_ && !(cp >= 0x1D400 && cp <= 0x1D7FF)) s |= kStyleCode;
        if ((f & kGlyphBold) && !boldIsBody_) s |= kStyleBold;
        if ((f & kGlyphItalic) && !italicIsBody_) s |= kStyleItalic;
        if (f & kGlyphStrike) s |= kStyleStrike;
        styles_[k] = s;
    }
    // Math set in an italic text font is no emphasis: an italic word of one letter (but the
    // pronoun "I"), of up to three letters with a bracket ("T(n)") or a digit ("4ac", "πr²"), with
    // an operator ("x+y", "z=x−iy") or with a raised or lowered letter ("P<sub>x</sub>"); a word
    // mixing italic and upright letters with a bracket, an operator or a script ("O(nlogn)"), or
    // whose italic runs are a letter or two ("n-th", "cosθ"); an italic word of up to three
    // letters beside an operator or beside math ("ax + by"). Unless it belongs to a longer italic
    // phrase.
    constexpr uint8_t kUpright = 0, kPhrase = 1, kMath = 2, kOperator = 3, kShort = 0x10;
    const auto kindOf = [&](size_t i) -> uint8_t {
        const Word& w = words_[i];
        // A word of letters only, none italic, is upright: no glyph needs decoding.
        bool plain = true;
        for (uint32_t k = w.first; k < w.first + w.count && plain; ++k)
            plain = (styles_[k] & (kLetter | kStyleItalic)) == kLetter;
        if (plain) return kUpright;
        uint32_t letters = 0, italic = 0, brackets = 0, ops = 0, scripts = 0, digits = 0, run = 0, longest = 0;
        bool pronoun = false;
        for (uint32_t k = w.first; k < w.first + w.count; ++k) {
            const std::string_view t = Text(refs_[k]);
            const uint32_t cp = FirstOf(t);
            const bool letter = (styles_[k] & kLetter) && !(cp >= '0' && cp <= '9');
            run = letter && (styles_[k] & kStyleItalic) ? run + 1 : 0;
            longest = std::max(longest, run);
            if (refs_[k].script != kScriptNone) scripts++;
            if (letter) {
                letters++;
                if (styles_[k] & kStyleItalic) italic++;
                pronoun = t == "I";
            } else if (cp >= '0' && cp <= '9') {
                digits++;
            } else if (cp == '(' || cp == ')') {
                brackets++;
            } else if (cp == '=' || cp == '+' || cp == '<' || cp == '>' || cp == '^' || cp == '|' || cp == 0x2212 ||
                       cp == 0xB1 || (cp >= 0x2200 && cp <= 0x22FF)) {
                ops++;
            }
        }
        if (letters == 0) return ops > 0 && digits == 0 ? kOperator : kUpright;
        if (italic == 0) return kUpright;
        if (italic < letters) return brackets + ops > 0 || scripts > 0 || longest <= 2 ? kMath : kUpright;
        const bool small = letters <= 3;
        const bool math = (letters == 1 && !pronoun) || ops > 0 || (small && (brackets + scripts + digits > 0));
        return math ? kMath : small ? uint8_t(kPhrase | kShort) : kPhrase;
    };
    if (set & kStyleItalic) {
        kinds_.resize(words_.size());
        for (size_t i = 0; i < words_.size(); ++i) kinds_[i] = kindOf(i);
        // Short italic words beside operators or math are math too, in both directions.
        const auto mathLike = [&](size_t j) { return kinds_[j] == kMath || kinds_[j] == kOperator; };
        for (size_t i = 0; i < words_.size(); ++i)
            if (kinds_[i] == (kPhrase | kShort) && i > 0 && mathLike(i - 1)) kinds_[i] = kMath;
        for (size_t i = words_.size(); i-- > 0;)
            if (kinds_[i] == (kPhrase | kShort) && i + 1 < words_.size() && mathLike(i + 1)) kinds_[i] = kMath;
        for (size_t i = 0; i < words_.size(); ++i) {
            const bool phraseBefore = i > 0 && (kinds_[i - 1] & ~kShort) == kPhrase;
            const bool phraseAfter = i + 1 < words_.size() && (kinds_[i + 1] & ~kShort) == kPhrase;
            if (kinds_[i] == kMath && !phraseBefore && !phraseAfter)
                for (uint32_t k = words_[i].first; k < words_[i].first + words_[i].count; ++k)
                    styles_[k] = static_cast<uint8_t>(styles_[k] & ~kStyleItalic);
        }
    }
    // A bold letter alone in math (a matrix "A𝑖", a vector "∇ ⋅ E = …") is no strong emphasis: one
    // bold letter in a word with a mathematical alphanumeric, or beside a word that is an operator
    // or holds one.
    if (set & kStyleBold) {
        const auto mathWord = [&](size_t i) {
            for (uint32_t k = words_[i].first; k < words_[i].first + words_[i].count; ++k) {
                const uint32_t cp = FirstOf(Text(refs_[k]));
                if ((cp >= 0x1D400 && cp <= 0x1D7FF) || (cp >= 0x2200 && cp <= 0x22FF) || cp == '=') return true;
            }
            return false;
        };
        for (size_t i = 0; i < words_.size(); ++i) {
            // Only a word with one bold letter is a candidate; its code points are read then.
            uint32_t bold = 0;
            for (uint32_t k = words_[i].first; k < words_[i].first + words_[i].count && bold <= 1; ++k)
                if ((styles_[k] & (kLetter | kStyleBold)) == (kLetter | kStyleBold)) bold++;
            if (bold != 1) continue;
            uint32_t letters = 0;
            for (uint32_t k = words_[i].first; k < words_[i].first + words_[i].count; ++k) {
                if (!(styles_[k] & kLetter)) continue;
                const uint32_t cp = FirstOf(Text(refs_[k]));
                if (cp < 0x1D400 || cp > 0x1D7FF) letters++;
            }
            if (bold != 1 || letters != 1) continue;
            if (!mathWord(i) && !(i > 0 && mathWord(i - 1)) && !(i + 1 < words_.size() && mathWord(i + 1))) continue;
            for (uint32_t k = words_[i].first; k < words_[i].first + words_[i].count; ++k)
                styles_[k] = static_cast<uint8_t>(styles_[k] & ~kStyleBold);
        }
    }
    // The nearest letters before each glyph: any letter, and letters outside code ("firm" ones).
    if (before_.size() < 2 * size_t(n)) before_.resize(2 * size_t(n));
    {
        int32_t any = -1, firm = -1;
        for (uint32_t k = 0; k < n; ++k) {
            before_[k] = any, before_[n + k] = firm;
            if (styles_[k] & kLetter) any = static_cast<int32_t>(k);
            if ((styles_[k] & (kLetter | kStyleCode)) == kLetter) firm = static_cast<int32_t>(k);
        }
    }
    // Backwards, with the nearest letters after each glyph: the styles each glyph is written in. A
    // glyph's styles are worked out from letters' code bits, which no glyph's result changes, and
    // from firm letters' styles, which keep theirs: the order the glyphs are taken in makes no
    // difference.
    const auto at = [&](int32_t k) { return k >= 0 ? static_cast<uint8_t>(styles_[static_cast<uint32_t>(k)]) : uint8_t(0); };
    int32_t anyAfter = -1, firmAfter = -1;
    for (uint32_t k = n; k-- > 0;) {
        const uint8_t s = styles_[k];
        uint8_t written = s;  // firm letters keep their own
        if ((s & (kLetter | kStyleCode)) != kLetter) {
            uint8_t result = 0;
            bool code = (s & kStyleCode) != 0;
            if (code && !(s & kLetter)) {
                // A monospace mark is code beside code: in a code word or between code words.
                const int32_t b = before_[k], a = anyAfter;
                const bool word = (b >= 0 && wordOf_[uint32_t(b)] == wordOf_[k] && (at(b) & kStyleCode)) ||
                                  (a >= 0 && wordOf_[uint32_t(a)] == wordOf_[k] && (at(a) & kStyleCode));
                code = word || (at(b) & at(a) & kStyleCode) != 0;
            }
            if (code) result |= kStyleCode;
            const int32_t fb = before_[n + k], fa = firmAfter;
            const uint8_t enclosed = at(fb) & at(fa) & (kStyleBold | kStyleItalic | kStyleStrike);
            if (code) {
                result |= enclosed | (s & (kStyleBold | kStyleItalic | kStyleStrike));
            } else {
                // Inside a word ("Alice’s") a mark between two letters of a style takes it whatever
                // its own face; elsewhere it keeps a style it is set in next to letters that have it.
                const bool sameB = fb >= 0 && wordOf_[uint32_t(fb)] == wordOf_[k];
                const bool sameA = fa >= 0 && wordOf_[uint32_t(fa)] == wordOf_[k];
                const uint8_t attached = static_cast<uint8_t>((sameB ? at(fb) : 0) | (sameA ? at(fa) : 0));
                result |= ((sameB && sameA ? enclosed : 0) | (s & (enclosed | attached))) &
                          (kStyleBold | kStyleItalic | kStyleStrike);
            }
            written = result;
        }
        styles_[k] = static_cast<uint8_t>(written & kStyles);
        if (s & kLetter) anyAfter = static_cast<int32_t>(k);
        if ((s & (kLetter | kStyleCode)) == kLetter) firmAfter = static_cast<int32_t>(k);
    }
}

void Writer::SetStyle(uint8_t target, uint32_t at) {
    int keep = 0;
    uint8_t kept = 0;
    while (keep < depth_ && (target & stack_[keep])) kept |= stack_[keep++];
    // A code span holds no markup: a style to open inside it closes it first.
    if ((target & ~kept) != 0 && keep > 0 && stack_[keep - 1] == kStyleCode) {
        --keep;
        kept = static_cast<uint8_t>(kept & ~kStyleCode);
    }
    while (depth_ > keep) {
        const uint8_t bit = stack_[--depth_];
        out_.append(kDelimiters[bit == kStyleBold ? 0 : bit == kStyleItalic ? 1 : bit == kStyleCode ? 2 : 3]);
    }
    uint8_t missing = static_cast<uint8_t>(target & ~kept);
    const size_t n = refs_.size();
    while (missing != 0 && depth_ < kStyleCount) {
        // Code opens last; of the others the one whose run from `at` goes on longest opens first
        // (outside), the first of equal ones.
        int best = 2;
        const uint8_t others = static_cast<uint8_t>(missing & ~kStyleCode);
        if ((others & (others - 1)) == 0) {
            if (others != 0) best = others == kStyleBold ? 0 : others == kStyleItalic ? 1 : 3;
        } else {
            size_t bestEnd = 0;
            for (int s = 0; s < kStyleCount; ++s) {
                if (s == 2 || !(missing & (1u << s))) continue;
                // Where the style's run from `at` ends; `at` never goes back within a block, so
                // each run is walked once.
                size_t end = at;
                if (at >= runFrom_[s] && at < runTo_[s]) {
                    end = runTo_[s];
                } else {
                    while (end < n && (styles_[end] & (1u << s))) ++end;
                    runFrom_[s] = at, runTo_[s] = end;
                }
                if (best == 2 || end > bestEnd) best = s, bestEnd = end;
            }
        }
        out_.append(kDelimiters[best]);
        stack_[depth_++] = static_cast<uint8_t>(1u << best);
        missing = static_cast<uint8_t>(missing & ~(1u << best));
    }
}

// The characters AppendEscaped may escape, by byte value; text without them it writes as it is.
// In LaTeX mode a dollar too.
struct EscapeTable {
    bool may[256] = {};
    constexpr explicit EscapeTable(bool dollar) {
        constexpr char kChars[] = "\\*`_<]~&";
        for (size_t i = 0; i + 1 < sizeof(kChars); ++i) may[static_cast<unsigned char>(kChars[i])] = true;
        may[static_cast<unsigned char>('$')] = dollar;
    }
};
constexpr EscapeTable kEscape(false);
constexpr EscapeTable kEscapeLatex(true);

bool MayEscape(char c) { return kEscape.may[static_cast<unsigned char>(c)]; }

// Escapes only what would change the structure (D6): emphasis delimiters (with
// DropNeedlessEscapes), code spans, HTML and autolink openers, entity references, and block
// markers at the start of the block.
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
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        const char prev = i > 0 ? t[i - 1] : ' ';
        const char next = i + 1 < t.size() ? t[i + 1] : ' ';
        switch (c) {
        case '\\':
        case '`':
            out_.push_back('\\');
            break;
        case '*':
        case '_': {
            // A run of one of them is written escaped; DropNeedlessEscapes unescapes it where it
            // can be neither an opener nor a closer. One that starts the block (a list marker, a
            // thematic break) stays escaped.
            size_t end = i + 1;
            while (end < t.size() && t[end] == c) ++end;
            if (!(blockStart && i == 0)) runs_.push_back(EscapedRun{out_.size(), end - i});
            for (size_t k = i; k < end; ++k) {
                out_.push_back('\\');
                out_.push_back(c);
            }
            i = end - 1;
            continue;
        }
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
        case '$':
            // LaTeX mode: a dollar of the text opens no formula. One before a digit or a space
            // ("$8,000") opens none anyway and stays as it is.
            if (math_ == MathMode::Latex && !(next >= '0' && next <= '9') && next != ' ') out_.push_back('\\');
            break;
        default:
            break;
        }
        out_.push_back(c);
    }
}

// A neighbour of a delimiter run by CommonMark's classes (6.2): whitespace, punctuation, other.
// Only ASCII is classed; any other byte (UTF-8, a line break) is unknown.
enum class Flank : uint8_t { Space, Punct, Other, Unknown };

Flank FlankClass(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == ' ') return Flank::Space;
    if (u < 0x20 || u >= 0x7F) return Flank::Unknown;
    return IsAsciiAlnum(c) ? Flank::Other : Flank::Punct;
}

// Whether a run of `c` (`*` or `_`) between the bytes `before` and `after` stays escaped: when it
// could open emphasis, or close it after a `c` written raw (`opener`, maybe an opener), and when a
// neighbour is unknown or the same character (the runs would join).
bool RunNeedsEscape(char before, char after, char c, bool opener) {
    const Flank b = FlankClass(before), a = FlankClass(after);
    if (b == Flank::Unknown || a == Flank::Unknown || before == c || after == c) return true;
    const bool left = a != Flank::Space && (a != Flank::Punct || b != Flank::Other);
    const bool right = b != Flank::Space && (b != Flank::Punct || a != Flank::Other);
    const bool opens = c == '*' ? left : left && (!right || b == Flank::Punct);
    const bool closes = c == '*' ? right : right && (!left || a == Flank::Punct);
    return opens || (closes && opener);
}

void Writer::DropNeedlessEscapes(size_t from) {
    if (runs_.empty()) return;
    const size_t n = out_.size();
    size_t w = from, r = from;  // out_ is compacted in place: written up to w, read from r
    bool star = false, under = false;  // a raw `*` / `_` before r: written by the writer (emphasis, a formula)
    for (const EscapedRun& run : runs_) {
        bool escaped = false;
        for (; r < run.at; ++r) {
            const char b = out_[r];
            if (escaped) {
                escaped = false;
            } else if (b == '\\') {
                escaped = true;
            } else {
                star = star || b == '*';
                under = under || b == '_';
            }
            out_[w++] = b;
        }
        // The run's bounds are those of the block (whitespace for CommonMark) at its ends.
        const char c = out_[run.at + 1];
        const size_t end = run.at + 2 * run.length;
        const char before = w > from ? out_[w - 1] : ' ';
        const char after = end < n ? out_[end] : ' ';
        if (RunNeedsEscape(before, after, c, c == '*' ? star : under)) {
            for (; r < end; ++r) out_[w++] = out_[r];
        } else {
            for (size_t k = 0; k < run.length; ++k) out_[w++] = c;
            r = end;
        }
    }
    for (; r < n; ++r) out_[w++] = out_[r];
    out_.resize(w);
    runs_.clear();
}

template <bool kLatex>
bool Writer::InlinePlain(const Block& block, bool styles) {
    uint16_t bail = kGlyphImage;
    if (styles)
        bail = static_cast<uint16_t>(bail | kGlyphStrike | (boldIsBody_ ? 0 : kGlyphBold) | (italicIsBody_ ? 0 : kGlyphItalic) |
                                     (monoIsBody_ ? 0 : kGlyphMonospace));
    const uint32_t blockEnd = block.first + block.count;
    // A glyph in a style written, an inline image, or (with styles) a page with link annotations:
    // the full path, found before anything is written. In LaTeX mode also a glyph that may be
    // math or belongs to a tagged formula with LaTeX: without one the block holds no formula, and
    // the full path writes what this one does (a dollar escaped as AppendEscaped does it).
    for (uint32_t bl = block.first; bl < blockEnd; ++bl) {
        const BlockLine& ref = lines_[bl];
        const PageData& page = pages_[ref.page];
        if (styles && !page.links.empty()) return false;
        const Line& line = page.lines[ref.line];
        uint16_t faces = 0;
        for (uint32_t k = ref.skip; k < line.count; ++k) faces |= page.glyphs[page.lineGlyphs[line.first + k]].flags;
        if (faces & bail) return false;
        if constexpr (kLatex) {
            for (uint32_t k = ref.skip; k < line.count; ++k) {
                const uint32_t gi = page.lineGlyphs[line.first + k];
                const Glyph& g = page.glyphs[gi];
                const uint8_t font = g.font < page.fontMath.size() ? (page.fontMath[g.font] & kMathFontClass) : 0;
                if (MayBeFormula(font, g.flags, page.GlyphText(g)) || page.FormulaOf(gi) != 0) return false;
            }
        }
    }
    const bool* const may = kLatex ? kEscapeLatex.may : kEscape.may;
    const size_t mark = out_.size();
    // Heading and table cell text is inline: "## 1. Installation" opens no list.
    bool blockStart = block.kind != BlockKind::Heading && block.kind != BlockKind::Table;
    // The word being written: its text from out_[wordAt], its glyph count and whether it holds a
    // byte AppendEscaped may escape; `last` is the text of the last glyph written (worked out at the
    // end of each line: a line's last glyph is always written).
    bool any = false;
    size_t wordAt = 0;
    uint32_t wordCount = 0;
    bool escapes = false;
    std::string_view last;
    const auto finishWord = [&]() {
        if (wordCount != 0 && (blockStart || escapes)) {
            other_.assign(out_, wordAt, std::string::npos);
            out_.resize(wordAt);
            AppendEscaped(other_, blockStart);
        }
        blockStart = false;
    };
    const auto abandon = [&]() {
        out_.resize(mark);
        return false;
    };
    bool joinNext = false;
    for (uint32_t bl = block.first; bl < blockEnd; ++bl) {
        const BlockLine& ref = lines_[bl];
        const PageData& page = pages_[ref.page];
        const Line& line = page.lines[ref.line];
        bool lineStart = true;      // no glyph of this line written yet
        bool carriedSpace = false;  // a soft hyphen left out had the word break before it
        for (uint32_t k = ref.skip; k < line.count; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            const std::string_view text = page.GlyphText(g);
            if (text.size() == 2 && text[0] == '\xC2' && text[1] == '\xAD' && k + 1 < line.count) {
                carriedSpace = carriedSpace || (g.flags & kGlyphSpaceBefore) != 0;
                continue;
            }
            if (IsSuperscript(g, line) || IsSubscript(g, line)) return abandon();
            const bool spaced = (g.flags & kGlyphSpaceBefore) || carriedSpace;
            carriedSpace = false;
            const bool starts = lineStart;
            lineStart = false;
            if (starts ? !joinNext : (!any || spaced)) {
                bool spaceBefore = any;
                if (spaceBefore && starts && IsCjk(FirstOf(text)) && IsCjk(LastOf(last))) spaceBefore = false;
                if (any) finishWord();
                if (spaceBefore) out_.push_back(' ');
                any = true;
                wordAt = out_.size();
                wordCount = 0;
                escapes = false;
            }
            if (text.size() == 1) {
                out_.push_back(text[0]);
                escapes |= may[static_cast<unsigned char>(text[0])];
            } else {
                out_.append(text.data(), text.size());
                for (const char c : text) escapes |= may[static_cast<unsigned char>(c)];
            }
            wordCount++;
        }
        joinNext = false;
        const std::string_view previous = last;  // the glyph written before this line's
        const bool wrote = ref.skip < line.count;
        if (wrote) last = page.GlyphText(page.glyphs[page.lineGlyphs[line.first + line.count - 1]]);
        // Dehyphenation, as CollectWords does it.
        if (bl + 1 >= blockEnd || !any || wordCount < 2) continue;
        if (last != "-" && last != "\xC2\xAD" && last != "\xE2\x80\x90") continue;
        if (!wrote) return abandon();
        // The glyph written before the hyphen: in this line past the soft hyphens left out, or the
        // one before the line (the word has both, so both are its glyphs).
        std::string_view beforeLast = previous;
        for (uint32_t j = line.count - 1; j-- > ref.skip;) {
            const std::string_view t = page.GlyphText(page.glyphs[page.lineGlyphs[line.first + j]]);
            if (t != "\xC2\xAD") {
                beforeLast = t;
                break;
            }
        }
        const unsigned char b0 = beforeLast.empty() ? 0 : static_cast<unsigned char>(beforeLast[0]);
        const bool letterBefore = (b0 >= 'a' && b0 <= 'z') || (b0 >= 'A' && b0 <= 'Z') || b0 >= 0xC3;
        const BlockLine& nextRef = lines_[bl + 1];
        const PageData& nextPage = pages_[nextRef.page];
        const Line& nextLine = nextPage.lines[nextRef.line];
        if (nextRef.skip >= nextLine.count) continue;
        const std::string_view next = nextPage.GlyphText(nextPage.glyphs[nextPage.lineGlyphs[nextLine.first + nextRef.skip]]);
        const unsigned char n0 = next.empty() ? 0 : static_cast<unsigned char>(next[0]);
        const bool lowerNext = (n0 >= 'a' && n0 <= 'z') || n0 == 0xC3 || n0 == 0xC4 || n0 == 0xC5;
        const bool join = letterBefore && lowerNext;
        if (!join && last != "\xC2\xAD") continue;
        // The hyphen leaves (a soft hyphen that does not join leaves no character either).
        out_.resize(out_.size() - last.size());
        wordCount--;
        last = beforeLast;
        joinNext = join;
    }
    if (any) finishWord();
    // No trailing spaces.
    while (!out_.empty() && out_.back() == ' ') out_.pop_back();
    return true;
}

bool Writer::PlainHeading(const Block& block) {
    other_.clear();
    const uint32_t blockEnd = block.first + block.count;
    bool joinNext = false;
    for (uint32_t bl = block.first; bl < blockEnd; ++bl) {
        const BlockLine& ref = lines_[bl];
        const PageData& page = pages_[ref.page];
        const Line& line = page.lines[ref.line];
        size_t wordAt = other_.size();
        const uint32_t end = block.number != 0 ? std::min(block.number, line.count) : line.count;
        bool lineStart = true;      // no glyph of this line written yet
        bool carriedSpace = false;  // a soft hyphen left out had the word break before it
        for (uint32_t k = ref.skip; k < end; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            std::string_view text = page.GlyphText(g);
            if (math_ == MathMode::Latex && block.number == 0) {
                const uint8_t font = g.font < page.fontMath.size() ? (page.fontMath[g.font] & kMathFontClass) : 0;
                if (MayBeFormula(font, g.flags, text) || page.FormulaOf(page.lineGlyphs[line.first + k]) != 0) return false;
            }
            // A soft hyphen shows only at a line's end, as a hyphen; a word break before it stays.
            if (text == "\xC2\xAD") {
                if (k + 1 < line.count) {
                    carriedSpace = carriedSpace || (g.flags & kGlyphSpaceBefore) != 0;
                    continue;
                }
                text = "-";
            }
            const bool spaced = (g.flags & kGlyphSpaceBefore) || carriedSpace;
            carriedSpace = false;
            const bool starts = lineStart;
            lineStart = false;
            if (starts ? (!joinNext && !other_.empty()) : spaced) {
                other_.push_back(' ');
                wordAt = other_.size();
            }
            const size_t at = other_.size();
            other_.append(text.data(), text.size());
            if (g.size >= block.smallCaps || !smallCapsLower_) continue;
            // ASCII and Latin-1 capitals to their lower-case letters (U+00C0-00DE but U+00D7).
            for (size_t i = at; i < other_.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(other_[i]);
                if (c >= 'A' && c <= 'Z') {
                    other_[i] = static_cast<char>(c + 0x20);
                } else if (c == 0xC3 && i + 1 < other_.size()) {
                    const unsigned char d = static_cast<unsigned char>(other_[i + 1]);
                    if (d >= 0x80 && d <= 0x9E && d != 0x97) other_[i + 1] = static_cast<char>(d + 0x20);
                    ++i;
                }
            }
        }
        // Dehyphenation, as InlinePlain does it: a word of two letters or more broken at a hyphen
        // goes on in a lower-case letter, here a small capital.
        joinNext = false;
        if (bl + 1 >= blockEnd || other_.size() < wordAt + 3 || other_.back() != '-') continue;
        const unsigned char before = static_cast<unsigned char>(other_[other_.size() - 2]);
        const BlockLine& nextRef = lines_[bl + 1];
        const PageData& nextPage = pages_[nextRef.page];
        const Line& nextLine = nextPage.lines[nextRef.line];
        if (nextRef.skip >= nextLine.count) continue;
        const Glyph& next = nextPage.glyphs[nextPage.lineGlyphs[nextLine.first + nextRef.skip]];
        const std::string_view nextText = nextPage.GlyphText(next);
        const bool letterBefore = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') || before >= 0x80;
        const bool lowerNext = next.size < block.smallCaps && !nextText.empty() &&
                               ((nextText[0] >= 'A' && nextText[0] <= 'Z') || nextText[0] == '\xC3');
        if (!letterBefore || !lowerNext) continue;
        other_.pop_back();
        joinNext = true;
    }
    while (!other_.empty() && other_.back() == ' ') other_.pop_back();
    const size_t from = out_.size();
    runs_.clear();
    AppendEscaped(other_, false);
    DropNeedlessEscapes(from);
    return true;
}

void Writer::Inline(const Block& block, bool styles) {
    const size_t from = out_.size();
    runs_.clear();
    if (math_ == MathMode::Latex ? InlinePlain<true>(block, styles) : InlinePlain<false>(block, styles)) {
        DropNeedlessEscapes(from);
        JoinLeaderDots(from);
        return;
    }
    runs_.clear();  // InlinePlain's, out_ cut back
    CollectWords(block);
    StyleWords(styles);
    depth_ = 0;
    for (int s = 0; s < kStyleCount; ++s) runFrom_[s] = runTo_[s] = 0;
    // LaTeX mode: formulas found among the glyphs are written as $…$ in their place; a table
    // cell's number stays text.
    const bool latex = math_ == MathMode::Latex;
    // Unraised "(a)" references may be among the glyphs (a local: out_'s writes would reload it).
    const bool letterRefs = letterRefs_;
    if (latex) {
        FindFormulas();
        if (block.kind == BlockKind::Table) KeepNumbersText(inline_, mathRun_);
    }
    uint32_t skipTo = 0;  // the glyphs before it went into a formula
    int32_t openLink = -1;
    // Heading and table cell text is inline: "## 1. Installation" opens no list.
    bool blockStart = block.kind != BlockKind::Heading && block.kind != BlockKind::Table;
    std::string_view openUri;
    const auto closeLink = [&]() {
        SetStyle(0, 0);
        if (openLink < 0) return;
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
    };
    const auto open = [&]() {
        uint8_t set = 0;
        for (int d = 0; d < depth_; ++d) set |= stack_[d];
        return set;
    };
    for (size_t i = 0; i < words_.size(); ++i) {
        const Word& w = words_[i];
        if (latex && skipTo >= w.first + w.count) continue;
        // A formula that ended inside the word: the rest of the word follows it directly.
        const bool resumed = latex && skipTo > w.first;
        if (!resumed) {
            const bool linkChanges = openLink >= 0 ? (i == 0 || !SameLink(words_[i - 1], w)) : w.link >= 0;
            if (linkChanges) {
                closeLink();
            } else if (w.spaceBefore) {
                // Styles that do not go on past the space close before it, so that delimiters stay
                // flanking; all of them before a formula.
                const uint8_t keep = latex && mathRun_[w.first] ? 0 : static_cast<uint8_t>(open() & styles_[w.first]);
                SetStyle(keep, w.first);
            }
            if (w.spaceBefore) out_.push_back(' ');
            if (w.link >= 0 && openLink < 0) {
                out_.push_back('[');
                openLink = w.link;
                openUri = Uri(w);
                blockStart = false;
            }
        }
        // The word in runs of one script and one style; the link closes before punctuation that
        // follows it.
        uint32_t k = resumed ? skipTo : w.first;
        const uint32_t end = w.first + w.count;
        const uint32_t trimAt = openLink >= 0 && w.linkTrim > 0 ? end - w.linkTrim : end;
        while (k < end) {
            if (k == trimAt && openLink >= 0) closeLink();
            uint32_t stop = k < trimAt ? trimAt : end;
            if (latex) {
                if (const uint32_t to = mathRun_[k]) {
                    SetStyle(0, k);
                    if (WriteFormula(k, to)) {
                        blockStart = false;
                        if (to >= end) {
                            skipTo = to;
                            break;
                        }
                        k = to;
                        continue;
                    }
                }
                for (uint32_t j = k + 1; j < stop; ++j)
                    if (mathRun_[j]) {
                        stop = j;
                        break;
                    }
            }
            const uint8_t script = refs_[k].script;
            // A footnote reference [k, refEnd): a raised run, or an unraised "(a)" (semantics.h's
            // bold letter references).
            uint32_t refEnd = 0;
            if (script == kScriptSup && !noteRefs_.empty() && (k == w.first || refs_[k - 1].script != kScriptSup)) {
                refEnd = k + 1;
                while (refEnd < end && refs_[refEnd].script == kScriptSup) ++refEnd;
            } else if (letterRefs && script == kScriptNone && k + 3 <= end && Text(refs_[k]) == "(") {
                refEnd = k + 3;
            }
            if (refEnd != 0) {
                if (const uint32_t number = NoteNumber(refs_[k], refEnd - k)) {
                    SetStyle(static_cast<uint8_t>(open() & ~kStyleCode), k);
                    if (plainOutput_) {
                        out_.append(std::to_string(number));
                    } else {
                        out_.append("[^");
                        out_.append(std::to_string(number));
                        out_.push_back(']');
                    }
                    blockStart = false;
                    k = refEnd;
                    continue;
                }
            }
            // An inline image glyph holds its Markdown link.
            if (refs_[k].flags & kGlyphImage) {
                SetStyle(static_cast<uint8_t>(open() & ~kStyleCode), k);
                out_.append(Text(refs_[k]));
                blockStart = false;
                ++k;
                continue;
            }
            // Unicode mode: a formula's script in Unicode forms, in the style of its first glyph.
            if (script != kScriptNone && !latex && !(styles_[k] & kStyleCode) && (k == w.first || refs_[k - 1].script != script)) {
                if (const uint32_t to = ScriptForms(w, k, stop)) {
                    SetStyle(styles_[k], k);
                    out_.append(forms_);
                    blockStart = false;
                    k = to;
                    continue;
                }
            }
            uint32_t runEnd = k + 1;
            while (runEnd < stop && refs_[runEnd].script == script && styles_[runEnd] == styles_[k] &&
                   !(refs_[runEnd].flags & kGlyphImage))
                ++runEnd;
            // An unraised "(a)" reference inside the run ends it.
            if (letterRefs && script == kScriptNone)
                for (uint32_t j = k + 1; j < runEnd; ++j)
                    if (j + 3 <= end && Text(refs_[j]) == "(" && NoteNumber(refs_[j], 3)) {
                        runEnd = j;
                        break;
                    }
            SetStyle(styles_[k], k);
            if (depth_ > 0) blockStart = false;
            if (styles_[k] & kStyleCode) {
                // Code spans hold their text as is; a backtick inside cannot be written in a
                // single-backtick span, so it becomes an apostrophe.
                for (uint32_t j = k; j < runEnd; ++j)
                    for (char c : Text(refs_[j])) out_.push_back(c == '`' ? '\'' : c);
            } else {
                // Plain text: a script's text without its tag, so a run at the block's start is
                // escaped as one.
                const bool tags = !plainOutput_;
                const bool start = blockStart && (script == kScriptNone || !tags);
                if (tags && script == kScriptSup) out_.append("<sup>");
                if (tags && script == kScriptSub) out_.append("<sub>");
                // The run's text goes straight into out_. AppendEscaped writes text without the
                // characters it escapes as it is, so only a run with one of them, or one at the
                // start of the block, is taken out again and escaped.
                const size_t mark = out_.size();
                bool escapes = false;
                for (uint32_t j = k; j < runEnd; ++j) {
                    const std::string_view t = Text(refs_[j]);
                    if (t.size() == 1) {
                        out_.push_back(t[0]);
                        escapes |= MayEscape(t[0]);
                    } else {
                        out_.append(t.data(), t.size());
                        for (const char c : t) escapes |= MayEscape(c);
                    }
                }
                bool plain = !start && !escapes;
                if (latex && plain) plain = std::string_view(out_).substr(mark).find('$') == std::string_view::npos;
                if (!plain) {
                    other_.assign(out_, mark, std::string::npos);
                    out_.resize(mark);
                    AppendEscaped(other_, start);
                }
                if (tags && script == kScriptSup) out_.append("</sup>");
                if (tags && script == kScriptSub) out_.append("</sub>");
            }
            blockStart = false;
            k = runEnd;
        }
    }
    closeLink();
    // No trailing spaces.
    while (!out_.empty() && out_.back() == ' ') out_.pop_back();
    DropNeedlessEscapes(from);
    JoinLeaderDots(from);
}

void Writer::JoinLeaderDots(size_t from) {
    // Most text holds no run: nothing moves before the first.
    size_t r = out_.find(". . . .", from);
    if (r == std::string::npos) return;
    const size_t n = out_.size();
    size_t w = r;
    while (r < n) {
        if (out_[r] == '.') {
            size_t e = r + 1, dots = 1;
            while (e + 1 < n && out_[e] == ' ' && out_[e + 1] == '.') e += 2, dots++;
            if (dots >= 4) {
                for (size_t d = 0; d < dots; ++d) out_[w++] = '.';
                r = e;
                continue;
            }
        }
        out_[w++] = out_[r++];
    }
    out_.resize(w);
}

uint32_t Writer::ScriptForms(const Word& w, uint32_t k, uint32_t stop) {
    const uint8_t script = refs_[k].script;
    const bool sup = script == kScriptSup;
    const uint32_t wordEnd = w.first + w.count;
    // The script's forms; every character needs one.
    forms_.clear();
    uint32_t end = k;
    for (; end < wordEnd && refs_[end].script == script; ++end) {
        if (end >= stop || (refs_[end].flags & kGlyphImage)) return 0;
        const std::string_view t = Text(refs_[end]);
        for (size_t at = 0; at < t.size();) {
            uint32_t cp = 0;
            size_t length = 0;
            if (!Utf8::Decode(t, at, cp, length)) return 0;
            const uint32_t form = ScriptForm(cp, sup);
            if (form == 0) return 0;
            Utf8::Append(forms_, form);
            at += length;
        }
    }
    const auto math = [&](uint32_t i) {
        const GlyphRef& r = refs_[i];
        const PageData& page = pages_[r.page];
        const Glyph& g = page.glyphs[r.glyph];
        if (g.font < page.fontMath.size() && (page.fontMath[g.font] & kMathFontClass) != 0 && !(r.flags & kGlyphUnmapped))
            return true;
        const uint32_t cp = FirstOf(Text(r));
        return (cp >= 0x1D400 && cp <= 0x1D7FF) || cp == 0x210E || (cp >= 0x391 && cp <= 0x3C9) || cp == 0x3D1 ||
               cp == 0x3D5 || cp == 0x3F5 || (cp >= 0x2200 && cp <= 0x22FF);
    };
    for (uint32_t i = k; i < end; ++i)
        if (math(i)) return end;
    // The base: the glyph before the scripts in the word.
    uint32_t base = k;
    while (base > w.first && refs_[base - 1].script != kScriptNone) --base;
    if (base == w.first) return 0;
    --base;
    if (math(base)) return end;
    if (italicIsBody_) return 0;
    // A variable: one to three italic letters ending at the base, after no other letter.
    uint32_t letters = 0;
    for (uint32_t i = base + 1; i > w.first; --i) {
        const uint32_t cp = FirstOf(Text(refs_[i - 1]));
        if (!((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))) break;
        if (!(refs_[i - 1].flags & kGlyphItalic) || ++letters > 3) return 0;
    }
    return letters > 0 ? end : 0;
}

void Writer::FindFormulas() {
    const uint32_t n = static_cast<uint32_t>(refs_.size());
    inline_.resize(n);
    for (uint32_t k = 0; k < n; ++k) {
        const GlyphRef& r = refs_[k];
        const PageData& page = pages_[r.page];
        const Glyph& g = page.glyphs[r.glyph];
        InlineGlyph& ig = inline_[k];
        ig.glyph = &g;
        ig.font = g.font < page.fontMath.size() ? (page.fontMath[g.font] & kMathFontClass) : 0;
        ig.wordStart = false;
        ig.lineStart = r.lineStart != 0;
        ig.script = r.script != kScriptNone;
        ig.note = false;
        ig.text = Text(r);
    }
    for (const Word& w : words_) inline_[w.first].wordStart = true;
    // Footnote references are never math.
    if (!noteRefs_.empty()) {
        for (uint32_t k = 0; k < n;) {
            if (letterRefs_ && refs_[k].script == kScriptNone && Text(refs_[k]) == "(" && k + 3 <= n &&
                NoteNumber(refs_[k], 3)) {
                for (uint32_t j = k; j < k + 3; ++j) inline_[j].note = true;
                k += 3;
                continue;
            }
            if (refs_[k].script != kScriptSup || (k > 0 && refs_[k - 1].script == kScriptSup)) {
                ++k;
                continue;
            }
            uint32_t end = k + 1;
            while (end < n && refs_[end].script == kScriptSup && !inline_[end].wordStart) ++end;
            if (NoteNumber(refs_[k], end - k))
                for (uint32_t j = k; j < end; ++j) inline_[j].note = true;
            k = end;
        }
    }
    FindInlineFormulas(inline_, mathRun_);
    // Tagged formulas with LaTeX: each run of a formula's glyphs is one formula, in place of the
    // runs found among them.
    for (uint32_t k = 0; k < n;) {
        const uint32_t f = pages_[refs_[k].page].FormulaOf(refs_[k].glyph);
        if (f == 0) {
            ++k;
            continue;
        }
        uint32_t end = k + 1;
        while (end < n && refs_[end].page == refs_[k].page && pages_[refs_[end].page].FormulaOf(refs_[end].glyph) == f) ++end;
        for (uint32_t j = 0; j < k; ++j)
            if (mathRun_[j] > k) mathRun_[j] = 0;  // a run found that reaches into the formula
        for (uint32_t j = k; j < end; ++j) mathRun_[j] = 0;
        mathRun_[k] = end;
        k = end;
    }
}

void Writer::AppendFormulaAlt(std::string_view alt) {
    size_t b = 0, e = alt.size();
    while (b < e && static_cast<unsigned char>(alt[b]) <= ' ') ++b;
    while (e > b && static_cast<unsigned char>(alt[e - 1]) <= ' ') --e;
    for (size_t i = b; i < e; ++i) out_.push_back(static_cast<unsigned char>(alt[i]) < ' ' ? ' ' : alt[i]);
}

uint32_t Writer::TaggedDisplay(const Block& block) const {
    uint32_t formula = 0;
    const uint32_t page = lines_[block.first].page;
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const BlockLine& ref = lines_[bl];
        if (ref.page != page) return 0;
        const PageData& p = pages_[ref.page];
        if (p.glyphFormula.empty()) return 0;
        const Line& line = p.lines[ref.line];
        for (uint32_t k = ref.skip; k < line.count; ++k) {
            const uint32_t gi = p.lineGlyphs[line.first + k];
            if (p.glyphs[gi].flags & (kGlyphSpace | kGlyphDropped)) continue;
            const uint32_t f = p.FormulaOf(gi);
            if (f == 0 || (formula != 0 && f != formula)) return 0;
            formula = f;
        }
    }
    return formula != 0 && pages_[page].formulaAlts[formula - 1].display ? formula : 0;
}

bool Writer::WriteFormula(uint32_t from, uint32_t to) {
    // A tagged formula's LaTeX (FindFormulas made the run its glyphs).
    if (const uint32_t f = pages_[refs_[from].page].FormulaOf(refs_[from].glyph)) {
        out_.push_back('$');
        AppendFormulaAlt(pages_[refs_[from].page].FormulaText(f));
        out_.push_back('$');
        return true;
    }
    mathIn_.Clear();
    const uint32_t pageIndex = refs_[from].page;
    const PageData& page = pages_[pageIndex];
    float u0 = INFINITY, u1 = -INFINITY, vb = INFINITY, vt = -INFINITY;
    for (uint32_t k = from; k < to; ++k) {
        if (refs_[k].page != pageIndex) return false;
        const Glyph& g = page.glyphs[refs_[k].glyph];
        if (g.dir != 0) return false;
        const size_t before = mathIn_.glyphs.size();
        AddMathGlyph(page, g, mathIn_);
        if (mathIn_.glyphs.size() > before && k > from && refs_[k].lineStart) mathIn_.glyphs.back().flags |= kMathLineStart;
        u0 = Fmin(u0, g.u0), u1 = Fmax(u1, g.u1);
        vb = Fmin(vb, g.v - 0.4f * g.size), vt = Fmax(vt, g.v + 0.9f * g.size);
    }
    AddMathRules(page, u0 - 1, u1 + 1, vb, vt, mathIn_);
    latex_.clear();
    // A run that is the block's whole text is a display formula the lines did not mark as one:
    // the arborescence's LaTeX would set it inline where the source sets it apart (READoc reads it
    // as a wrong inline formula), so it keeps the greedy rebuild's result.
    const bool alone = from == 0 && to == refs_.size();
    if (BuildLatex(mathIn_, false, latex_, tag_, !alone) != MathStatus::Ok) return false;
    out_.push_back('$');
    out_.append(latex_);
    out_.push_back('$');
    return true;
}

bool Writer::Display(const Block& block) {
    if (const uint32_t f = TaggedDisplay(block)) {
        out_.append("$$");
        AppendFormulaAlt(pages_[lines_[block.first].page].FormulaText(f));
        out_.append("$$");
        return true;
    }
    if (block.count != 1) return false;
    const BlockLine& ref = lines_[block.first];
    const PageData& page = pages_[ref.page];
    const Line& line = page.lines[ref.line];
    if (!(line.flags & kLineFormula) || line.dir != 0) return false;
    mathIn_.Clear();
    for (uint32_t k = 0; k < line.count; ++k) AddMathGlyph(page, page.glyphs[page.lineGlyphs[line.first + k]], mathIn_);
    AddMathRules(page, line.u0 - 1, line.u1 + 1, line.vb - line.size, line.vt + line.size, mathIn_);
    latex_.clear();
    if (BuildLatex(mathIn_, true, latex_, tag_) != MathStatus::Ok) return false;
    out_.append("$$");
    out_.append(latex_);
    out_.append("$$");
    // The equation number stays outside the formula: as \tag it makes olmOCR's KaTeX comparison
    // fail more often (2026-11-09, arxiv_math 0.648 with \tag, 0.664 without).
    if (!tag_.empty()) {
        out_.append(" (");
        out_.append(tag_);
        out_.push_back(')');
    }
    return true;
}

void Writer::Code(const Block& block, const std::string& prefix) {
    // A line's last row: the baseline of its last glyph (a wrapped source line joined again in
    // lines.cpp ends a row lower than it starts).
    const auto bottom = [&](const BlockLine& ref) {
        const PageData& page = pages_[ref.page];
        const Line& line = page.lines[ref.line];
        return line.count ? page.glyphs[page.lineGlyphs[line.first + line.count - 1]].v : line.baseline;
    };
    // The character grid: the median advance of the block's glyphs (algorithms §6.4).
    std::vector<float> widths;
    float left = INFINITY;
    std::vector<float> pitches;
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const PageData& page = pages_[lines_[bl].page];
        const Line& line = page.lines[lines_[bl].line];
        left = Fmin(left, line.u0);
        for (uint32_t k = 0; k < line.count && widths.size() < 4096; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            if (g.u1 > g.u0) widths.push_back(g.u1 - g.u0);
        }
        if (bl > block.first && lines_[bl - 1].page == lines_[bl].page) {
            const float pitch = bottom(lines_[bl - 1]) - line.baseline;
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
                const float gap = bottom(lines_[bl - 1]) - line.baseline;
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
                   WriteCounts& counts, MathMode math, const WriteSettings& settings, std::vector<WrittenBlock>* written) {
    Writer writer(pages, list, out, counts, math, settings.plain, settings.smallCaps == SmallCaps::Lower);
    const bool styles = !settings.plain;
    if (repaired) out += "<!-- rayopdf: the file was damaged and has been repaired; text may be missing -->\n";
    int contentColumn[9] = {};
    const Block* previous = nullptr;
    std::string prefix;
    std::vector<std::string> cellText;
    std::vector<TablePart> parts;
    std::vector<uint32_t> partFirst;
    size_t marked = 0;  // pages[0, marked) have their marker
    const auto markPagesTo = [&](int number) {
        for (; marked < pages.size() && pages[marked].number <= number; ++marked) {
            if (!out.empty()) out += "\n\n";
            out += "<!-- page ";
            out += std::to_string(pages[marked].number);
            out += " -->";
            previous = nullptr;
        }
    };
    for (const Block& block : list.blocks) {
        if (block.kind != BlockKind::Comment && block.count == 0) continue;
        if (settings.pageMarkers)
            markPagesTo(block.kind == BlockKind::Comment ? static_cast<int>(block.number)
                                                         : pages[list.lines[block.first].page].number);
        if (!out.empty()) {
            const bool tight = previous && previous->kind == BlockKind::ListItem && block.kind == BlockKind::ListItem &&
                               previous->quote == block.quote;
            // Blocks of one block quote stay in it: the blank line between them is quoted too. An
            // alert starts a quote of its own.
            const bool quoted = [&]() {
                for (const Block* b : {previous, &block})
                    if (!b || b->quote == 0 || b->kind == BlockKind::Table || b->kind == BlockKind::Comment ||
                        b->kind == BlockKind::Footnote)
                        return false;
                return block.alert == 0;
            }();
            if (tight) {
                out += "\n";
            } else if (quoted) {
                out += "\n>";
                for (int q = 1; q < std::min(previous->quote, block.quote); ++q) out += " >";
                out += "\n";
            } else {
                out += "\n\n";
            }
        }
        previous = &block;
        if (written) {
            written->emplace_back();
            written->back().block = static_cast<uint32_t>(&block - list.blocks.data());
            written->back().begin = out.size();
        }
        prefix.clear();
        for (int q = 0; q < block.quote; ++q) prefix += "> ";
        if (block.alert != 0 && block.quote > 0) {
            static const char* const kAlerts[] = {"NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION"};
            out += prefix;
            out += "[!";
            out += kAlerts[std::min<int>(block.alert, 5) - 1];
            out += "]\n";
        }
        switch (block.kind) {
        case BlockKind::Comment:
            out += "<!-- rayopdf: page ";
            out += std::to_string(block.number);
            out += block.level == static_cast<uint8_t>(CommentKind::NoText)      ? " has no extractable text -->"
                   : block.level == static_cast<uint8_t>(CommentKind::OcrFailed) ? " has no extractable text; OCR failed -->"
                                                                                  : " text is mostly unmapped glyphs -->";
            break;
        case BlockKind::Heading:
            counts.headings++;
            out += prefix;
            out.append(std::max<int>(1, std::min<int>(6, block.level)), '#');
            out.push_back(' ');
            if ((block.smallCaps > 0 || block.number != 0) && writer.PlainHeading(block)) break;
            writer.Inline(block, false);
            break;
        case BlockKind::Paragraph: {
            counts.paragraphs++;
            out += prefix;
            if (block.listParagraph) out.append(static_cast<size_t>(contentColumn[std::min<int>(block.depth, 8)]), ' ');
            if (math == MathMode::Latex && writer.Display(block)) break;
            writer.Inline(block, styles);
            break;
        }
        case BlockKind::ListItem: {
            counts.listItems++;
            out += prefix;
            const int depth = std::min<int>(block.depth, 8);
            const int indent = depth == 0 ? 0 : contentColumn[depth - 1];
            out.append(static_cast<size_t>(indent), ' ');
            std::string marker =
                block.ordered ? std::to_string(block.number == 0 ? 1 : block.number) + (block.closeParen ? ")" : ".") : "-";
            out += marker;
            out.push_back(' ');
            contentColumn[depth] = indent + static_cast<int>(marker.size()) + 1;
            for (int d = depth + 1; d < 9; ++d) contentColumn[d] = contentColumn[depth] + 2 * (d - depth);
            if (block.task) out += block.task == 2 ? "[x] " : "[ ] ";
            writer.Inline(block, styles);
            break;
        }
        case BlockKind::Code:
            counts.codeBlocks++;
            writer.Code(block, prefix);
            break;
        case BlockKind::Footnote: {
            counts.footnotes++;
            // Plain text: "N text" without the reference's brackets and colon.
            if (styles) out += "[^";
            out += std::to_string(block.number);
            out += styles ? "]: " : " ";
            writer.Inline(block, styles);
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
            // D6: GFM unless a cell spans rows, or columns below the header rows, then HTML
            // (layout/tables.h); every table HTML, its cells spaced, with TableFormat::Html; every
            // table GFM with TableFormat::Pipe (a spanning cell's text in its first row and column
            // only). A table broken across pages is one table from its parts, each its anchor line
            // then its cell lines. GFM cells are inline Markdown, cut back out of `out` after the
            // writer appends them.
            parts.clear();
            partFirst.clear();
            bool html = false;
            for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
                const PageData& page = pages[list.lines[bl].page];
                const Line& line = page.lines[list.lines[bl].line];
                if (!(line.flags & kLineTableAnchor)) continue;
                parts.push_back(TablePart{&page, &page.tables[line.first], nullptr});
                partFirst.push_back(bl);
                html = html || (settings.tables == TableFormat::Auto && TableNeedsHtml(page, page.tables[line.first]));
            }
            if (parts.empty()) break;
            html = html || settings.tables == TableFormat::Html;
            counts.tables++;
            if (html && !written) {
                WriteHtmlTable(parts, out, settings.tables == TableFormat::Html);
                break;
            }
            // The JSON output (`written`) takes every cell's inline Markdown, an HTML table's too;
            // the links of the cells of an HTML table are no links written.
            const int links = counts.links;
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
                    writer.Inline(sub, styles);
                    text.assign(out, mark, std::string::npos);
                    out.resize(mark);
                }
                parts[p].cellText = cellText.data() + at;
                at += table.cellCount;
            }
            if (written) {
                WrittenBlock& entry = written->back();
                entry.cols = parts[0].table->cols;
                entry.headerRows = std::max<uint16_t>(1, parts[0].table->headerRows);
                for (size_t p = 0; p < parts.size(); ++p) {
                    const Table& table = *parts[p].table;
                    const uint16_t skip = p > 0 ? RepeatedHeader(parts[0], parts[p]) : 0;
                    for (uint32_t c = 0; c < table.cellCount; ++c) {
                        const TableCell& cell = parts[p].page->tableCells[table.firstCell + c];
                        if (cell.row < skip) continue;
                        entry.cells.push_back(WrittenCell{entry.rows + (cell.row - skip), cell.col, cell.rowSpan,
                                                          cell.colSpan, parts[p].cellText[c]});
                    }
                    entry.rows += table.rows > skip ? table.rows - skip : 0;
                }
            }
            if (html) {
                counts.links = links;
                WriteHtmlTable(parts, out, settings.tables == TableFormat::Html);
                break;
            }
            WriteGfmTable(parts, out, settings.tables != TableFormat::Pipe);
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
            out += "![";
            // Alternate text (a tagged figure's /Alt, else the text OCR read in the image) on one
            // line, with what would end the link text or start other markup escaped.
            const std::string_view alt = image.altLength > 0 ? page.ImageAlt(image) : page.ImageOcrText(image);
            for (const char c : alt) {
                if (c == '\\' || c == '[' || c == ']' || c == '*' || c == '_' || c == '`' || c == '<' || c == '&')
                    out.push_back('\\');
                out.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
            }
            out += "](";
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
        if (written) {
            if (out.size() > written->back().begin)
                written->back().end = out.size();
            else
                written->pop_back();
        }
    }
    if (settings.pageMarkers) markPagesTo(pages.empty() ? 0 : pages.back().number);
    if (!out.empty()) out.push_back('\n');
}

namespace {

// A coordinate as `%.2f` (0 for a value that is not finite: JSON has no NaN).
void AppendCoordinate(std::string& out, float value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", std::isfinite(value) ? static_cast<double>(value) : 0.0);
    out += buf;
}

void AppendKey(std::string& out, const char* key) {
    out += ",\"";
    out += key;
    out += "\":";
}

void AppendInteger(std::string& out, const char* key, uint64_t value) {
    AppendKey(out, key);
    out += std::to_string(value);
}

// Adds a box of the frame of `dir` on page `page` to `boxes`: joined with the box it has there.
void AddBox(std::vector<PageBox>& boxes, uint32_t page, uint8_t dir, float x0, float y0, float x1, float y1) {
    RectFromFrame(dir, x0, y0, x1, y1);
    for (PageBox& box : boxes) {
        if (box.page != page) continue;
        box.x0 = Fmin(box.x0, x0), box.y0 = Fmin(box.y0, y0), box.x1 = Fmax(box.x1, x1), box.y1 = Fmax(box.y1, y1);
        return;
    }
    boxes.push_back(PageBox{page, x0, y0, x1, y1});
}

} // namespace

const char* BlockTypeName(BlockKind kind) {
    switch (kind) {
    case BlockKind::Paragraph: return "paragraph";
    case BlockKind::Heading: return "heading";
    case BlockKind::ListItem: return "list_item";
    case BlockKind::Code: return "code";
    case BlockKind::Comment: return "comment";
    case BlockKind::Footnote: return "footnote";
    case BlockKind::Table: return "table";
    case BlockKind::Image: return "image";
    }
    return "paragraph";
}

void BlockBoxes(const std::vector<PageData>& pages, const BlockList& list, const Block& block,
                std::vector<PageBox>& boxes) {
    boxes.clear();
    if (block.kind == BlockKind::Comment) return;
    // An image's box, a table's parts' (their anchor lines' tables, in the frame of their
    // direction), any other block's lines'.
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const PageData& page = pages[list.lines[bl].page];
        const Line& line = page.lines[list.lines[bl].line];
        if (block.kind == BlockKind::Image) {
            const ImageBox& image = page.images[line.first];
            AddBox(boxes, list.lines[bl].page, 0, image.x0, image.y0, image.x1, image.y1);
            break;
        }
        if (block.kind == BlockKind::Table) {
            if (!(line.flags & kLineTableAnchor)) continue;
            const Table& table = page.tables[line.first];
            AddBox(boxes, list.lines[bl].page, line.dir, table.x0, table.y0, table.x1, table.y1);
            continue;
        }
        if (line.count == 0 && !(line.flags & kLineAnchor)) continue;
        AddBox(boxes, list.lines[bl].page, line.dir, line.u0, line.vb, line.u1, line.vt);
    }
}

void AppendBoxes(std::string& out, const std::vector<PageData>& pages, const PageBox* boxes, size_t count) {
    out.push_back('[');
    for (size_t b = 0; b < count; ++b) {
        if (b > 0) out.push_back(',');
        out += "{\"page\":";
        out += std::to_string(pages[boxes[b].page].number);
        out += ",\"bbox\":[";
        AppendCoordinate(out, boxes[b].x0);
        out.push_back(',');
        AppendCoordinate(out, boxes[b].y0);
        out.push_back(',');
        AppendCoordinate(out, boxes[b].x1);
        out.push_back(',');
        AppendCoordinate(out, boxes[b].y1);
        out += "]}";
    }
    out.push_back(']');
}

void WriteJson(const std::vector<PageData>& pages, const BlockList& list, bool repaired,
               const DocumentMetadata* metadata, std::string& out, WriteCounts& counts, MathMode math,
               const WriteSettings& settings) {
    std::string markdown;
    std::vector<WrittenBlock> written;
    WriteSettings blocks = settings;
    blocks.pageMarkers = false;
    WriteMarkdown(pages, list, false, markdown, counts, math, blocks, &written);
    out += "{\"rayopdf\":\"" RAYOPDF_VERSION "\",\"schema\":1,\"repaired\":";
    out += repaired ? "true" : "false";
    out += ",\"metadata\":{";
    if (metadata) {
        bool first = true;
        for (const MetadataField& field : kMetadataFields) {
            const std::string& value = metadata->*field.member;
            if (value.empty()) continue;
            if (!first) out.push_back(',');
            first = false;
            Batch::AppendJsonString(out, field.name);
            out.push_back(':');
            Batch::AppendJsonString(out, value);
        }
    }
    out += "},\"pages\":[";
    for (size_t p = 0; p < pages.size(); ++p) {
        if (p > 0) out.push_back(',');
        out += "{\"page\":";
        out += std::to_string(pages[p].number);
        out += ",\"width\":";
        AppendCoordinate(out, pages[p].width);
        out += ",\"height\":";
        AppendCoordinate(out, pages[p].height);
        out.push_back('}');
    }
    out += "],\"blocks\":[";
    std::vector<PageBox> boxes;
    for (size_t w = 0; w < written.size(); ++w) {
        const WrittenBlock& entry = written[w];
        const Block& block = list.blocks[entry.block];
        if (w > 0) out.push_back(',');
        out += "{\"type\":\"";
        out += BlockTypeName(block.kind);
        out.push_back('"');
        const BlockLine* anchor = block.kind == BlockKind::Comment ? nullptr : &list.lines[block.first];
        AppendInteger(out, "page", anchor ? static_cast<uint64_t>(pages[anchor->page].number) : block.number);
        BlockBoxes(pages, list, block, boxes);
        out += ",\"boxes\":";
        AppendBoxes(out, pages, boxes.data(), boxes.size());
        switch (block.kind) {
        case BlockKind::Heading:
            AppendInteger(out, "level", static_cast<uint64_t>(std::max(1, std::min<int>(6, block.level))));
            break;
        case BlockKind::ListItem:
            AppendInteger(out, "depth", std::min<uint64_t>(block.depth, 8));
            AppendKey(out, "ordered");
            out += block.ordered ? "true" : "false";
            if (block.ordered) AppendInteger(out, "number", block.number == 0 ? 1 : block.number);
            if (block.task != 0) {
                AppendKey(out, "task");
                out += block.task == 2 ? "\"done\"" : "\"open\"";
            }
            break;
        case BlockKind::Table:
            AppendInteger(out, "rows", entry.rows);
            AppendInteger(out, "cols", entry.cols);
            AppendInteger(out, "header_rows", entry.headerRows);
            out += ",\"cells\":[";
            for (size_t c = 0; c < entry.cells.size(); ++c) {
                const WrittenCell& cell = entry.cells[c];
                out += c > 0 ? ",{\"row\":" : "{\"row\":";
                out += std::to_string(cell.row);
                AppendInteger(out, "col", cell.col);
                AppendInteger(out, "row_span", cell.rowSpan);
                AppendInteger(out, "col_span", cell.colSpan);
                AppendKey(out, "text");
                Batch::AppendJsonString(out, cell.text);
                out.push_back('}');
            }
            out.push_back(']');
            break;
        case BlockKind::Image: {
            const PageData& page = pages[anchor->page];
            const ImageBox& image = page.images[page.lines[anchor->line].first];
            AppendKey(out, "file");
            if (image.state == ImageState::Written)
                Batch::AppendJsonString(out, page.ImageName(image));
            else
                out += "null";
            AppendKey(out, "alt");
            Batch::AppendJsonString(out, image.altLength > 0 ? page.ImageAlt(image) : page.ImageOcrText(image));
            break;
        }
        case BlockKind::Footnote: AppendInteger(out, "number", block.number); break;
        case BlockKind::Paragraph:
        case BlockKind::Code:
        case BlockKind::Comment: break;
        }
        if (block.quote > 0) AppendInteger(out, "quote", block.quote);
        if (block.alert != 0 && block.quote > 0) {
            static const char* const kAlerts[] = {"\"note\"", "\"tip\"", "\"important\"", "\"warning\"", "\"caution\""};
            AppendKey(out, "alert");
            out += kAlerts[std::min<int>(block.alert, 5) - 1];
        }
        AppendKey(out, "markdown");
        Batch::AppendJsonString(out, std::string_view(markdown).substr(entry.begin, entry.end - entry.begin));
        out.push_back('}');
    }
    out += "]}\n";
}

namespace {

bool IsAsciiPunct(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

// The text of a heading's Markdown into `text`: without quote marks, its "#" marker and backslash
// escapes.
void HeadingText(std::string_view md, std::string& text) {
    while (md.size() >= 2 && md[0] == '>' && md[1] == ' ') md.remove_prefix(2);
    size_t at = 0;
    while (at < md.size() && md[at] == '#') ++at;
    if (at < md.size() && md[at] == ' ') ++at;
    text.clear();
    for (; at < md.size(); ++at) {
        if (md[at] == '\\' && at + 1 < md.size() && IsAsciiPunct(md[at + 1])) ++at;
        text.push_back(md[at]);
    }
}

// Where the first piece of `text` (longer than `limit`) ends: `cut` its length (at most `limit`),
// `next` where the rest starts. At the last sentence end (". ", "? ", "! " before a capital or a
// digit) within the limit, else at the last space, else at the last character boundary.
void TextCut(std::string_view text, size_t limit, size_t& cut, size_t& next) {
    size_t sentence = 0, space = 0;
    const size_t last = std::min(limit, text.size() - 1);
    for (size_t i = 1; i <= last; ++i) {
        if (text[i] != ' ') continue;
        space = i;
        const char before = text[i - 1];
        const char after = i + 1 < text.size() ? text[i + 1] : ' ';
        if ((before == '.' || before == '?' || before == '!') &&
            ((after >= 'A' && after <= 'Z') || (after >= '0' && after <= '9')))
            sentence = i;
    }
    if (sentence != 0 || space != 0) {
        cut = sentence != 0 ? sentence : space;
        next = cut + 1;
        return;
    }
    cut = limit;
    while (cut > 1 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    next = cut;
}

// The open chunk of WriteChunks and the heading path, written as JSON Lines to `out`.
class ChunkWriter {
public:
    ChunkWriter(const std::vector<PageData>& pages, std::string& out) : pages_(pages), out_(out) {}

    bool Empty() const { return blocks_.empty(); }
    bool OnlyHeadings() const { return onlyHeadings_; }
    size_t Size() const { return markdown_.size(); }

    // A heading of `level` with `text`: the path's entries of that level or deeper end.
    void Heading(int level, const std::string& text) {
        while (!levels_.empty() && levels_.back() >= level) {
            levels_.pop_back();
            path_.pop_back();
        }
        levels_.push_back(level);
        path_.push_back(text);
    }

    // Adds block `index` (its Markdown, boxes and first page number) to the open chunk.
    void Add(uint32_t index, std::string_view md, bool heading, const std::vector<PageBox>& boxes, int page) {
        if (!markdown_.empty()) markdown_ += "\n\n";
        markdown_ += md;
        Take(index, boxes, page);
        onlyHeadings_ = onlyHeadings_ && heading;
    }

    // Writes the open chunk, if any.
    void Flush() {
        if (blocks_.empty()) return;
        Write(markdown_);
    }

    // Writes `md`, a piece of block `index`, as a chunk of its own (the open chunk must be empty).
    void Piece(uint32_t index, std::string_view md, const std::vector<PageBox>& boxes, int page) {
        Take(index, boxes, page);
        Write(md);
    }

private:
    void Take(uint32_t index, const std::vector<PageBox>& boxes, int page) {
        blocks_.push_back(index);
        boxes_.insert(boxes_.end(), boxes.begin(), boxes.end());
        const bool first = blocks_.size() == 1;
        first_ = first ? page : std::min(first_, page);
        last_ = first ? page : std::max(last_, page);
        for (const PageBox& box : boxes) {
            first_ = std::min(first_, pages_[box.page].number);
            last_ = std::max(last_, pages_[box.page].number);
        }
    }

    void Write(std::string_view md) {
        out_ += "{\"chunk\":";
        out_ += std::to_string(number_++);
        out_ += ",\"heading_path\":[";
        for (size_t h = 0; h < path_.size(); ++h) {
            if (h > 0) out_.push_back(',');
            Batch::AppendJsonString(out_, path_[h]);
        }
        out_ += "],\"pages\":[";
        out_ += std::to_string(first_);
        out_.push_back(',');
        out_ += std::to_string(last_);
        out_ += "],\"boxes\":";
        AppendBoxes(out_, pages_, boxes_.data(), boxes_.size());
        out_ += ",\"blocks\":[";
        for (size_t b = 0; b < blocks_.size(); ++b) {
            if (b > 0) out_.push_back(',');
            out_ += std::to_string(blocks_[b]);
        }
        out_ += "],\"markdown\":";
        Batch::AppendJsonString(out_, md);
        out_ += "}\n";
        markdown_.clear();
        blocks_.clear();
        boxes_.clear();
        onlyHeadings_ = true;
    }

    const std::vector<PageData>& pages_;
    std::string& out_;
    std::string markdown_;
    std::vector<uint32_t> blocks_;
    std::vector<PageBox> boxes_;
    std::vector<int> levels_;
    std::vector<std::string> path_;
    int first_ = 0, last_ = 0;
    bool onlyHeadings_ = true;
    uint64_t number_ = 0;
};

// The lines of a table or code block's Markdown that open (`head`) and close (`tail`) each piece
// of it; false when it has no line between them (or is not as WriteMarkdown writes it). `starts`
// holds where each line starts.
bool LineFrame(BlockKind kind, std::string_view md, const std::vector<size_t>& starts, size_t& head, size_t& tail) {
    const size_t n = starts.size();
    const auto line = [&](size_t l) {
        const size_t end = l + 1 < n ? starts[l + 1] - 1 : md.size();
        return md.substr(starts[l], end - starts[l]);
    };
    head = tail = 0;
    if (kind == BlockKind::Code) {
        head = tail = 1;
    } else if (line(0).size() >= 7 && line(0).compare(line(0).size() - 7, 7, "<table>") == 0) {
        // HTML: "<table>", the header rows (<th> cells), the rows, "</table>".
        head = 1;
        while (head < n && line(head).find("<th") != std::string_view::npos) ++head;
        tail = line(n - 1) == "</table>" ? 1 : 0;
    } else {
        // GFM: the header row and the separator row "| --- |...", then the rows.
        for (size_t l = 0; l < n && head == 0; ++l) {
            const std::string_view text = line(l);
            if (text.find("---") != std::string_view::npos &&
                text.find_first_not_of("|-: ") == std::string_view::npos)
                head = l + 1;
        }
    }
    return head > 0 && n > head + tail;
}

} // namespace

void WriteChunks(const std::vector<PageData>& pages, const BlockList& list, std::string& out, WriteCounts& counts,
                 MathMode math, const WriteSettings& settings) {
    std::string markdown;
    std::vector<WrittenBlock> written;
    WriteSettings blocks = settings;
    blocks.pageMarkers = false;
    WriteMarkdown(pages, list, false, markdown, counts, math, blocks, &written);
    const size_t limit = std::min(std::max(settings.chunkChars, kMinChunkChars), kMaxChunkChars);
    ChunkWriter chunks(pages, out);
    std::vector<PageBox> boxes;
    std::vector<size_t> starts;
    std::string heading, piece;
    for (size_t w = 0; w < written.size(); ++w) {
        const WrittenBlock& entry = written[w];
        const Block& block = list.blocks[entry.block];
        const std::string_view md = std::string_view(markdown).substr(entry.begin, entry.end - entry.begin);
        const uint32_t index = static_cast<uint32_t>(w);
        const bool isHeading = block.kind == BlockKind::Heading;
        BlockBoxes(pages, list, block, boxes);
        const int page = block.kind == BlockKind::Comment ? static_cast<int>(block.number)
                                                          : pages[list.lines[block.first].page].number;
        if (isHeading && !chunks.Empty() && !chunks.OnlyHeadings()) chunks.Flush();
        const bool alone = md.size() > limit;
        if (alone || (!chunks.Empty() && chunks.Size() + 2 + md.size() > limit)) chunks.Flush();
        if (isHeading) {
            HeadingText(md, heading);
            chunks.Heading(std::max(1, std::min<int>(6, block.level)), heading);
        }
        if (!alone) {
            chunks.Add(index, md, isHeading, boxes, page);
            continue;
        }
        // Longer than a chunk: tables by rows and code by lines, each piece opened (and closed) as
        // the block is; other blocks as text.
        size_t head = 0, tail = 0;
        bool lines = false;
        if (block.kind == BlockKind::Table || block.kind == BlockKind::Code) {
            starts.assign(1, 0);
            for (size_t i = 0; i < md.size(); ++i)
                if (md[i] == '\n') starts.push_back(i + 1);
            lines = LineFrame(block.kind, md, starts, head, tail);
        }
        if (lines) {
            const size_t n = starts.size();
            const std::string_view open = md.substr(0, starts[head]);  // with its line break
            const std::string_view close = tail > 0 ? md.substr(starts[n - tail] - 1) : std::string_view();
            piece.assign(open.data(), open.size());
            bool body = false;
            for (size_t l = head; l < n - tail; ++l) {
                const size_t end = l + 1 < n ? starts[l + 1] - 1 : md.size();
                const std::string_view line = md.substr(starts[l], end - starts[l]);
                if (body && piece.size() + 1 + line.size() + close.size() > limit) {
                    piece.append(close.data(), close.size());
                    chunks.Piece(index, piece, boxes, page);
                    piece.assign(open.data(), open.size());
                    body = false;
                }
                if (body) piece.push_back('\n');
                piece.append(line.data(), line.size());
                body = true;
            }
            piece.append(close.data(), close.size());
            chunks.Piece(index, piece, boxes, page);
            continue;
        }
        for (size_t at = 0; at < md.size();) {
            const std::string_view rest = md.substr(at);
            if (rest.size() <= limit) {
                chunks.Piece(index, rest, boxes, page);
                break;
            }
            size_t cut = 0, next = 0;
            TextCut(rest, limit, cut, next);
            chunks.Piece(index, rest.substr(0, cut), boxes, page);
            at += next;
        }
    }
    chunks.Flush();
}

} // namespace RayoPdf::Layout
