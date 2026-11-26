// SPDX-License-Identifier: Apache-2.0
#include "layout/markdown_writer.h"

#include "common/batch.h"
#include "common/minmax.h"
#include "common/per_thread.h"
#include "common/utf8.h"
#include "layout/lines.h"
#include "layout/tables.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// A bold, italic or strike-through delimiter SetStyle wrote: where in out_, the glyph it opens at
// or closes after, its style, whether it opens.
struct Delimiter {
    size_t at;
    uint32_t glyph;
    uint8_t style;
    bool open;
};

// The delimiters of the block being written, kept by the thread (no allocation per document).
struct DelimiterScratch {
    std::vector<Delimiter> list;
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
RAYOPDF_HOT_INLINE bool IsLetterOrDigit(uint32_t cp) {
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

// What dehyphenation needs to know of the part of a word before a hyphen ending its line: its
// code points (counted up to 2) and whether it holds a hyphen of its own. Takes the part's text
// whole or glyph by glyph.
void AddFragment(std::string_view t, uint32_t& codePoints, bool& hyphen) {
    const size_t size = t.size();
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(t[i]);
        codePoints += codePoints < 2 && (c & 0xC0) != 0x80;
        hyphen = hyphen || c == '-' || (c == 0xE2 && i + 2 < size && t[i + 1] == '\x80' && t[i + 2] == '\x90');
    }
}

// A hard hyphen ending a line between letters stays when the line goes on in a capital ("anti-" /
// "American"), or in lowercase after a part of one letter ("N-" / "fertilization") or a part that
// holds a hyphen already ("state-of-the-" / "art"): hyphenation breaks neither a single letter off
// nor a word that has a hyphen of its own. The halves join without a space.
bool CompoundBreak(bool upperNext, uint32_t codePoints, bool hyphen) { return upperNext || codePoints == 1 || hyphen; }

// Whether the line that glyph `from` of `line` starts goes on in a function word and a hyphen
// before a letter ("of-" of "state-" / "of-the-art", "to-" of "signal-" / "to-noise"): a compound
// broken at its own hyphen, which stays. Hyphenation leaves no such fragment before a hyphen,
// while other fragments may stand there ("sin-" / "gle-nucleotide", "Twen-" / "ty-two").
bool FunctionWordCompound(const PageData& page, const Line& line, uint32_t from) {
    static constexpr std::string_view kWords[] = {"of", "to", "and", "in", "on", "by", "for", "or", "the"};
    char word[3];
    uint32_t n = 0;
    for (uint32_t k = from; k + 1 < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (k > from && (g.flags & kGlyphSpaceBefore)) return false;
        const std::string_view t = page.GlyphText(g);
        if (t.size() != 1) return false;
        if (t[0] == '-') {
            const Glyph& after = page.glyphs[page.lineGlyphs[line.first + k + 1]];
            const std::string_view a = page.GlyphText(after);
            const unsigned char a0 = a.empty() ? 0 : static_cast<unsigned char>(a[0] | 0x20);
            if ((after.flags & kGlyphSpaceBefore) || a0 < 'a' || a0 > 'z') return false;
            for (const std::string_view w : kWords)
                if (w == std::string_view(word, n)) return true;
            return false;
        }
        if (t[0] < 'a' || t[0] > 'z' || n == 3) return false;
        word[n++] = t[0];
    }
    return false;
}

// An en or em dash: TeX may break a line after one set without spaces ("291–" / "302",
// "Hirzebruch–" / "Riemann"); the words on both sides join without a space, the dash between them.
bool JoiningDash(std::string_view t) { return t == "\xE2\x80\x93" || t == "\xE2\x80\x94"; }

// A letter or digit a joining dash stands between: ASCII, or Latin-1 and Latin Extended-A letters.
bool DashNeighbour(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 0xC3 && c <= 0xC5);
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
    // Inline's words after StyleWords (and FindFormulas): runs of one style and script, links,
    // formulas, note references and images appended to out_, every style closed at the end.
    void WriteWords(const Block& block);
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
    // Dehyphenation's document lexicon: whether the letters before glyph `hyphen` of `line` (a
    // hyphen ending it) and those `next` starts with (from glyph `from`) make a pair that the
    // document writes with a hyphen inside a line somewhere ("cross-" / "correlation", "signal-" /
    // "to" of "signal-to-noise"): a compound, whose hyphen stays.
    bool KnownCompound(const PageData& page, const Line& line, uint32_t hyphen, const PageData& nextPage,
                       const Line& next, uint32_t from);
    void StyleWords(bool styles);
    // Opens and closes delimiters so that exactly the styles `target` are open, nested properly;
    // `at` is the glyph they apply from, the one before it the last glyph a closing style had
    // (styles that run longer open first, outside). Most calls change nothing (no style open and
    // none wanted, or the one open style wanted): those return here, the rest go to ChangeStyle.
    RAYOPDF_HOT_INLINE void SetStyle(uint8_t target, uint32_t at) {
        if (depth_ == 0 ? target == 0 : depth_ == 1 && stack_[0] == target) return;
        ChangeStyle(target, at);
    }
    void ChangeStyle(uint8_t target, uint32_t at);
    // CommonMark reads a run of `*` or `~` as emphasis or strike-through only when it flanks the
    // text the right way (0.31 §6.2): an opening run left-flanking, a closing one right-flanking.
    // Checks the runs SetStyle wrote in out_ from `from` (the block's inline text); where one does
    // not flank, takes the style off the glyphs at that edge of its run (TakeStyle). True when
    // styles changed and the block is written again; from `attempt` kFlankAttempts on, every
    // emphasis and strike-through of a block that still fails goes.
    bool RepairFlanking(size_t from, int attempt);
    // Takes style `bit` off the edge of its run that a delimiter opening at glyph `glyph` (`open`)
    // or closing after it leaves unflanked: a punctuation mark or space alone ("**3:**309" becomes
    // "**3**:309"), with the spaces inside the run beside it; a script in tags or a code span whole
    // (their markup is punctuation); else the whole run. False when no glyph had the style.
    bool TakeStyle(uint32_t glyph, uint8_t bit, bool open);
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
    void FindFormulas(const Block& block);
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
    // The delimiters SetStyle wrote since the block's inline text began.
    std::vector<Delimiter>& delimiters_ = PerThread<DelimiterScratch>().list;
    std::string plain_;
    std::string other_;
    std::vector<EscapedRun> runs_;   // the runs AppendEscaped wrote since the block's inline text began
    std::string forms_;  // ScriptForms' forms
    // KnownCompound's lexicon, built from every page when first asked: each "-" inside a line
    // between two letters (a candidate pair), sorted by the two bytes beside it; a pair's runs are
    // read and hashed (PairHash) only when a line-end hyphen asks for those bytes. A document with
    // more candidates than kMaxCompounds keeps the sorted hashes of its first kMaxCompounds pairs
    // instead (compounds_).
    struct Candidate {
        uint64_t hash = 0;     // the pair's hash once read and taken
        uint32_t page = 0, glyph = 0;
        uint16_t key = 0;      // the bytes beside the hyphen (CompoundKey)
        uint8_t state = 0;     // 0 unread, 1 a pair (hash), 2 no pair
    };
    std::vector<Candidate> candidates_;
    std::vector<uint64_t> compounds_;
    bool compoundsBuilt_ = false;
    bool compoundsHashed_ = false;  // compounds_ holds the lexicon (too many candidates)
    // LaTeX mode.
    std::vector<InlineGlyph> inline_;
    std::vector<uint32_t> mathRun_;  // per GlyphRef: the end of the formula starting there, or 0
    MathInput mathIn_;
    std::string latex_, tag_;
};

constexpr size_t kMaxCompounds = size_t(1) << 16;  // pairs a document's lexicon keeps at most

// A glyph whose text is a letter (ASCII, or a letter or digit from U+00C0 up).
bool WordLetter(std::string_view t) {
    const uint32_t cp = FirstOf(t);
    return cp < 0x80 ? ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') : IsLetterOrDigit(cp);
}

// WordLetter of glyph `g`'s text, an ASCII first byte read in place.
inline bool GlyphLetter(const PageData& page, const Glyph& g) {
    if (g.textLength != 0 && g.text < page.text.size()) {
        const unsigned char c = static_cast<unsigned char>(page.text[g.text]);
        if (c < 0x80) return static_cast<unsigned>((c | 0x20) - 'a') < 26u;
    }
    return WordLetter(page.GlyphText(g));
}

bool HyphenText(std::string_view t) { return t == "-" || t == "\xE2\x80\x90"; }

inline uint64_t FoldCapital(unsigned char u) { return u >= 'A' && u <= 'Z' ? u + 0x20 : u; }

// FNV-1a over glyph `g`'s text, ASCII capitals folded, on from `h`; a one-byte text read in place.
inline uint64_t HashGlyph(const PageData& page, const Glyph& g, uint64_t h) {
    if (g.textLength == 1 && g.text < page.text.size())
        return (h ^ FoldCapital(static_cast<unsigned char>(page.text[g.text]))) * 0x100000001B3ull;
    for (const char c : page.GlyphText(g)) h = (h ^ FoldCapital(static_cast<unsigned char>(c))) * 0x100000001B3ull;
    return h;
}

// A pair's key: the folded bytes on either side of its hyphen, the last of the left run's text
// and the first of the right run's (both runs are letters, so neither text is empty). Two pairs
// whose folded texts are the same, each with no "-" but its hyphen, have the same key.
inline uint16_t CompoundKey(std::string_view left, std::string_view right) {
    return static_cast<uint16_t>(FoldCapital(static_cast<unsigned char>(left.back())) << 8 |
                                 FoldCapital(static_cast<unsigned char>(right.front())));
}

// A line-end hyphen's pair of letter runs: the run of one word's letters of `left` that ends
// before glyph `end`, and the run of `right` from glyph `begin`. False when either is empty or the
// pair is part of a longer compound (a hyphen right before or after it: "to-day" of "day-to-day"
// is no compound of its own); else the runs' ends, the pair's key and whether a glyph text of
// either run holds a "-" of its own.
struct LineEndPair {
    uint32_t a = 0, b = 0;
    uint16_t key = 0;
    bool dash = false;
};
bool ReadLineEndPair(const PageData& leftPage, const Line& left, uint32_t end, const PageData& rightPage,
                     const Line& right, uint32_t begin, LineEndPair& pair) {
    const uint32_t* const leftIds = leftPage.lineGlyphs.data() + left.first;
    const uint32_t* const rightIds = rightPage.lineGlyphs.data() + right.first;
    const Glyph* const leftGlyphs = leftPage.glyphs.data();
    const Glyph* const rightGlyphs = rightPage.glyphs.data();
    // Whether a glyph of either run has a text of more than one byte (only such a text can hold
    // a "-" besides its letter).
    bool multi = false;
    uint32_t a = end;
    while (a > 0 && GlyphLetter(leftPage, leftGlyphs[leftIds[a - 1]])) {
        --a;
        multi = multi || leftGlyphs[leftIds[a]].textLength > 1;
        if (leftGlyphs[leftIds[a]].flags & kGlyphSpaceBefore) break;
    }
    if (a == end) return false;
    uint32_t b = begin;
    while (b < right.count && GlyphLetter(rightPage, rightGlyphs[rightIds[b]]) &&
           (b == begin || !(rightGlyphs[rightIds[b]].flags & kGlyphSpaceBefore))) {
        multi = multi || rightGlyphs[rightIds[b]].textLength > 1;
        ++b;
    }
    if (b == begin) return false;
    if ((a > 0 && !(leftGlyphs[leftIds[a]].flags & kGlyphSpaceBefore) &&
         HyphenText(leftPage.GlyphText(leftGlyphs[leftIds[a - 1]]))) ||
        (b < right.count && !(rightGlyphs[rightIds[b]].flags & kGlyphSpaceBefore) &&
         HyphenText(rightPage.GlyphText(rightGlyphs[rightIds[b]]))))
        return false;
    bool dash = false;
    for (uint32_t k = a; multi && k < end && !dash; ++k)
        dash = leftPage.GlyphText(leftGlyphs[leftIds[k]]).find('-') != std::string_view::npos;
    for (uint32_t k = begin; multi && k < b && !dash; ++k)
        dash = rightPage.GlyphText(rightGlyphs[rightIds[k]]).find('-') != std::string_view::npos;
    pair.a = a, pair.b = b, pair.dash = dash;
    pair.key = CompoundKey(leftPage.GlyphText(leftGlyphs[leftIds[end - 1]]), rightPage.GlyphText(rightGlyphs[rightIds[begin]]));
    return true;
}

// The hash of a line-end pair: FNV-1a over the left run's texts, "-", the right run's texts.
uint64_t LineEndPairHash(const PageData& leftPage, const Line& left, uint32_t end, const PageData& rightPage,
                         const Line& right, uint32_t begin, const LineEndPair& pair) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t k = pair.a; k < end; ++k) h = HashGlyph(leftPage, leftPage.glyphs[leftPage.lineGlyphs[left.first + k]], h);
    h = (h ^ '-') * 0x100000001B3ull;
    for (uint32_t k = begin; k < pair.b; ++k)
        h = HashGlyph(rightPage, rightPage.glyphs[rightPage.lineGlyphs[right.first + k]], h);
    return h;
}

// The glyph of `page` whose text is the "-" at `offset` (glyphs in the order of their texts),
// when it is drawn with no word break before or after it between two glyphs; else SIZE_MAX.
size_t InnerHyphen(const PageData& page, size_t offset) {
    const Glyph* const glyphs = page.glyphs.data();
    const size_t n = page.glyphs.size();
    const Glyph* g = std::lower_bound(glyphs, glyphs + n, offset, [](const Glyph& a, size_t o) { return a.text < o; });
    if (g == glyphs + n || g->text != offset || g->textLength != 1 || (g->flags & (kGlyphDropped | kGlyphSpaceBefore)))
        return SIZE_MAX;
    const size_t h = static_cast<size_t>(g - glyphs);
    if (h == 0 || h + 1 >= n || (glyphs[h + 1].flags & kGlyphSpaceBefore)) return SIZE_MAX;
    return h;
}

// A glyph of a run beside a hyphen drawn at baseline `v` in a size of tolerance / 0.3: a visible
// letter on that baseline.
inline bool RunLetter(const PageData& page, const Glyph& x, float v, float tolerance) {
    return !(x.flags & (kGlyphDropped | kGlyphSpace)) && GlyphLetter(page, x) && std::fabs(x.v - v) <= tolerance;
}

// The pair of letter runs beside InnerHyphen `h`, read in content order: a run of one word's
// letters on each side, and no hyphen beyond either run (a two-part compound). False when there
// is none, else its hash (FNV-1a over the left run's texts, "-", the right run's).
bool PairHash(const PageData& page, size_t h, uint64_t& hash) {
    const Glyph* const glyphs = page.glyphs.data();
    const size_t n = page.glyphs.size();
    const float v = glyphs[h].v, tolerance = 0.3f * glyphs[h].size;
    size_t a = h;
    while (a > 0 && RunLetter(page, glyphs[a - 1], v, tolerance)) {
        --a;
        if (glyphs[a].flags & kGlyphSpaceBefore) break;
    }
    if (a == h) return false;
    uint64_t sum = 0xCBF29CE484222325ull;
    for (size_t i = a; i < h; ++i) sum = HashGlyph(page, glyphs[i], sum);
    sum = (sum ^ '-') * 0x100000001B3ull;
    size_t b = h + 1;
    while (b < n && RunLetter(page, glyphs[b], v, tolerance) && (b == h + 1 || !(glyphs[b].flags & kGlyphSpaceBefore))) {
        sum = HashGlyph(page, glyphs[b], sum);
        ++b;
    }
    if (b == h + 1) return false;
    if ((a > 0 && !(glyphs[a].flags & kGlyphSpaceBefore) && HyphenText(page.GlyphText(glyphs[a - 1]))) ||
        (b < n && !(glyphs[b].flags & kGlyphSpaceBefore) && HyphenText(page.GlyphText(glyphs[b]))))
        return false;
    hash = sum;
    return true;
}

// The hashes of `page`'s pairs, in text order, into `out` while it holds under kMaxCompounds.
void AddCompounds(const PageData& page, std::vector<uint64_t>& out) {
    const std::string& text = page.text;
    for (size_t at = 0; at < text.size() && out.size() < kMaxCompounds;) {
        const void* found = std::memchr(text.data() + at, '-', text.size() - at);
        if (!found) break;
        const size_t offset = static_cast<size_t>(static_cast<const char*>(found) - text.data());
        at = offset + 1;
        const size_t h = InnerHyphen(page, offset);
        uint64_t hash;
        if (h != SIZE_MAX && PairHash(page, h, hash)) out.push_back(hash);
    }
}

bool Writer::KnownCompound(const PageData& page, const Line& line, uint32_t hyphen, const PageData& nextPage,
                           const Line& next, uint32_t from) {
    if (!compoundsBuilt_) {
        // The candidates: every "-" of the pages' texts (memchr, the glyph by its text offset)
        // between two letters on its baseline (the first glyph of either run). Pages without a
        // hyphen cost one scan of their text.
        compoundsBuilt_ = true;
        for (uint32_t p = 0; p < pages_.size() && !compoundsHashed_; ++p) {
            const PageData& pd = pages_[p];
            const std::string& text = pd.text;
            for (size_t at = 0; at < text.size();) {
                const void* found = std::memchr(text.data() + at, '-', text.size() - at);
                if (!found) break;
                const size_t offset = static_cast<size_t>(static_cast<const char*>(found) - text.data());
                at = offset + 1;
                const size_t h = InnerHyphen(pd, offset);
                if (h == SIZE_MAX) continue;
                const Glyph* const glyphs = pd.glyphs.data();
                const float v = glyphs[h].v, tolerance = 0.3f * glyphs[h].size;
                if (!RunLetter(pd, glyphs[h - 1], v, tolerance) || !RunLetter(pd, glyphs[h + 1], v, tolerance)) continue;
                if (candidates_.size() == kMaxCompounds) {
                    compoundsHashed_ = true;
                    break;
                }
                Candidate c;
                c.page = p;
                c.glyph = static_cast<uint32_t>(h);
                c.key = CompoundKey(pd.GlyphText(glyphs[h - 1]), pd.GlyphText(glyphs[h + 1]));
                candidates_.push_back(c);
            }
        }
        if (compoundsHashed_) {
            candidates_.clear();
            for (const PageData& p : pages_) AddCompounds(p, compounds_);
            std::sort(compounds_.begin(), compounds_.end());
            compounds_.erase(std::unique(compounds_.begin(), compounds_.end()), compounds_.end());
        } else {
            std::sort(candidates_.begin(), candidates_.end(),
                      [](const Candidate& x, const Candidate& y) { return x.key < y.key; });
        }
    }
    if (candidates_.empty() && compounds_.empty()) return false;
    LineEndPair pair;
    if (!ReadLineEndPair(page, line, hyphen, nextPage, next, from, pair)) return false;
    // A pair the document writes has the same folded text: with no "-" but the hyphen, the same
    // key; runs whose glyph texts hold a "-" are compared with every candidate.
    Candidate* first = candidates_.data();
    Candidate* last = first + candidates_.size();
    if (!compoundsHashed_ && !pair.dash) {
        first = std::lower_bound(first, last, pair.key, [](const Candidate& x, uint16_t k) { return x.key < k; });
        last = std::upper_bound(first, last, pair.key, [](uint16_t k, const Candidate& x) { return k < x.key; });
        if (first == last) return false;
    }
    const uint64_t h = LineEndPairHash(page, line, hyphen, nextPage, next, from, pair);
    if (h == 0) return false;
    if (compoundsHashed_) return std::binary_search(compounds_.begin(), compounds_.end(), h);
    for (Candidate* c = first; c != last; ++c) {
        if (c->state == 0) c->state = PairHash(pages_[c->page], c->glyph, c->hash) ? 1 : 2;
        if (c->state == 1 && c->hash == h) return true;
    }
    return false;
}

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
    // A local: the refs' byte stores would make the compiler read the member again at each glyph.
    const bool monoIsBody = monoIsBody_;
    for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
        const BlockLine& ref = lines_[bl];
        const PageData& page = pages_[ref.page];
        const Line& line = page.lines[ref.line];
        const uint32_t lineFirst = n;
        bool carriedSpace = false;  // a soft hyphen left out had the word break before it
        // IsSuperscript's and IsSubscript's bounds and the line's glyphs, worked out once a line
        // (locals the refs' byte stores leave in registers).
        const float small = 0.85f * line.size, raised = 0.2f * line.size, lowered = -0.1f * line.size;
        const float baseline = line.baseline;
        const uint32_t* const lineGlyphs = page.lineGlyphs.data() + line.first;
        const Glyph* const glyphs = page.glyphs.data();
        const std::string_view pageText = page.text;
        const uint32_t count = line.count, pageIndex = ref.page;
        for (uint32_t k = ref.skip; k < count; ++k) {
            const uint32_t gi = lineGlyphs[k];
            const Glyph& g = glyphs[gi];
            const std::string_view text = pageText.substr(g.text, g.textLength);  // PageData::GlyphText
            // Soft hyphens are invisible except at a line break; a word break before one stays.
            if (text.size() == 2 && text[0] == '\xC2' && text[1] == '\xAD' && k + 1 < count) {
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
            if (g.size < small && !(f & kGlyphInvisible)) {
                const float rise = g.v - baseline;
                script = rise > raised ? kScriptSup : rise < lowered ? kScriptSub : kScriptNone;
            }
            refs[n] = GlyphRef{text.data(), pageIndex, gi, static_cast<uint16_t>(text.size()), f, script,
                               static_cast<uint8_t>(n == lineFirst)};
            wordOf[n] = word;
            ++n;
            if (!(f & kGlyphImage)) {
                faces |= f;
                if ((f & kGlyphMonospace) && !monoIsBody && !code) {
                    const uint32_t cp = FirstOf(text);
                    code = !(cp >= 0x1D400 && cp <= 0x1D7FF);
                }
            }
        }
        joinNext = false;
        // Dehyphenation (algorithms §4.4): a hyphen ending a line, after a letter, before a line
        // that starts in lowercase, joins the halves of the word; a compound's hyphen
        // (CompoundBreak, FunctionWordCompound) joins them and stays. A dash between a letter or
        // digit and the next line's (JoiningDash) joins the words and stays.
        if (bl + 1 >= block.first + block.count || n == 0) continue;
        const uint32_t wordFirst = words_.back().first;
        if (n - wordFirst < 2) continue;
        const std::string_view last = Text(refs[n - 1]);
        const bool dash = JoiningDash(last);
        if (!dash && last != "-" && last != "\xC2\xAD" && last != "\xE2\x80\x90") continue;
        const std::string_view before = Text(refs[n - 2]);
        const unsigned char b0 = before.empty() ? 0 : static_cast<unsigned char>(before[0]);
        const bool letterBefore = (b0 >= 'a' && b0 <= 'z') || (b0 >= 'A' && b0 <= 'Z') || b0 >= 0xC3;
        const BlockLine& nextRef = lines_[bl + 1];
        const PageData& nextPage = pages_[nextRef.page];
        const Line& nextLine = nextPage.lines[nextRef.line];
        if (nextRef.skip >= nextLine.count) continue;
        const std::string_view next = nextPage.GlyphText(nextPage.glyphs[nextPage.lineGlyphs[nextLine.first + nextRef.skip]]);
        const unsigned char n0 = next.empty() ? 0 : static_cast<unsigned char>(next[0]);
        if (dash) {
            joinNext = DashNeighbour(b0) && DashNeighbour(n0);
            continue;
        }
        const bool lowerNext = (n0 >= 'a' && n0 <= 'z') || n0 == 0xC3 || n0 == 0xC4 || n0 == 0xC5;
        const bool upperNext = n0 >= 'A' && n0 <= 'Z';
        if (!letterBefore || !(lowerNext || (upperNext && last != "\xC2\xAD"))) {
            // A soft hyphen that does not join still leaves no character.
            if (last == "\xC2\xAD") --n;
            continue;
        }
        joinNext = true;
        if (last != "\xC2\xAD") {
            uint32_t codePoints = 0;
            bool hyphen = false;
            for (uint32_t i = wordFirst; i + 1 < n; ++i) AddFragment(Text(refs[i]), codePoints, hyphen);
            if (CompoundBreak(upperNext, codePoints, hyphen) ||
                FunctionWordCompound(nextPage, nextLine, nextRef.skip) ||
                (ref.skip < line.count && KnownCompound(page, line, line.count - 1, nextPage, nextLine, nextRef.skip)))
                continue;
        }
        --n;
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
    // The arrays and the body faces in locals: the byte stores into styles_ (and kinds_) would make
    // the compiler read every member again after each one. No pass below resizes refs_, styles_,
    // words_ or wordOf_.
    const GlyphRef* const refs = refs_.data();
    uint8_t* const styleOf = styles_.data();
    const Word* const words = words_.data();
    const size_t wordCount = words_.size();
    const uint32_t* const wordOf = wordOf_.data();
    const bool monoIsBody = monoIsBody_, boldIsBody = boldIsBody_, italicIsBody = italicIsBody_;
    for (uint32_t k = 0; k < n; ++k) {
        const uint16_t f = refs[k].flags;
        if (f & kGlyphImage) continue;
        const uint32_t cp = FirstOf(Text(refs[k]));
        uint8_t s = IsLetterOrDigit(cp) ? kLetter : 0;
        // Mathematical alphanumerics (𝑥, 𝐸) are math whatever the font's pitch: no code.
        if ((f & kGlyphMonospace) && !monoIsBody && !(cp >= 0x1D400 && cp <= 0x1D7FF)) s |= kStyleCode;
        if ((f & kGlyphBold) && !boldIsBody) s |= kStyleBold;
        if ((f & kGlyphItalic) && !italicIsBody) s |= kStyleItalic;
        if (f & kGlyphStrike) s |= kStyleStrike;
        styleOf[k] = s;
    }
    // Math set in an italic text font is no emphasis: an italic word of one letter (but the
    // pronoun "I"), of up to three letters with a bracket ("T(n)") or a digit ("4ac", "πr²"), with
    // an operator ("x+y", "z=x−iy") or with a raised or lowered letter ("P<sub>x</sub>"), or whose
    // italic letters stand one by one between brackets and scripts ("(x₀,y₀,x₁,y₁)"); a word
    // mixing italic and upright letters with a bracket, an operator or a script ("O(nlogn)"), or
    // whose italic runs are a letter or two ("n-th", "cosθ"); an italic word of up to three
    // letters beside an operator or beside math ("ax + by"). Unless it belongs to a longer italic
    // phrase.
    constexpr uint8_t kUpright = 0, kPhrase = 1, kMath = 2, kOperator = 3, kShort = 0x10;
    const auto kindOf = [&](size_t i) -> uint8_t {
        const Word& w = words[i];
        // A word of letters only, none italic, is upright: no glyph needs decoding.
        bool plain = true;
        for (uint32_t k = w.first; k < w.first + w.count && plain; ++k)
            plain = (styleOf[k] & (kLetter | kStyleItalic)) == kLetter;
        if (plain) return kUpright;
        uint32_t letters = 0, italic = 0, brackets = 0, ops = 0, scripts = 0, digits = 0, run = 0, longest = 0;
        bool pronoun = false;
        for (uint32_t k = w.first; k < w.first + w.count; ++k) {
            const std::string_view t = Text(refs[k]);
            const uint32_t cp = FirstOf(t);
            const bool letter = (styleOf[k] & kLetter) && !(cp >= '0' && cp <= '9');
            run = letter && (styleOf[k] & kStyleItalic) ? run + 1 : 0;
            longest = std::max(longest, run);
            if (refs[k].script != kScriptNone) scripts++;
            if (letter) {
                letters++;
                if (styleOf[k] & kStyleItalic) italic++;
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
        const bool math = (letters == 1 && !pronoun) || ops > 0 || (small && (brackets + scripts + digits > 0)) ||
                          (longest == 1 && brackets + scripts > 0);
        return math ? kMath : small ? uint8_t(kPhrase | kShort) : kPhrase;
    };
    if (set & kStyleItalic) {
        kinds_.resize(wordCount);
        for (size_t i = 0; i < wordCount; ++i) kinds_[i] = kindOf(i);
        // Short italic words beside operators or math are math too, in both directions.
        const auto mathLike = [&](size_t j) { return kinds_[j] == kMath || kinds_[j] == kOperator; };
        for (size_t i = 0; i < wordCount; ++i)
            if (kinds_[i] == (kPhrase | kShort) && i > 0 && mathLike(i - 1)) kinds_[i] = kMath;
        for (size_t i = wordCount; i-- > 0;)
            if (kinds_[i] == (kPhrase | kShort) && i + 1 < wordCount && mathLike(i + 1)) kinds_[i] = kMath;
        for (size_t i = 0; i < wordCount; ++i) {
            const bool phraseBefore = i > 0 && (kinds_[i - 1] & ~kShort) == kPhrase;
            const bool phraseAfter = i + 1 < wordCount && (kinds_[i + 1] & ~kShort) == kPhrase;
            if (kinds_[i] == kMath && !phraseBefore && !phraseAfter)
                for (uint32_t k = words[i].first; k < words[i].first + words[i].count; ++k)
                    styleOf[k] = static_cast<uint8_t>(styleOf[k] & ~kStyleItalic);
        }
    }
    // A bold letter alone in math (a matrix "A𝑖", a vector "∇ ⋅ E = …") is no strong emphasis: one
    // bold letter in a word with a mathematical alphanumeric, or beside a word that is an operator
    // or holds one.
    if (set & kStyleBold) {
        const auto mathWord = [&](size_t i) {
            for (uint32_t k = words[i].first; k < words[i].first + words[i].count; ++k) {
                const uint32_t cp = FirstOf(Text(refs[k]));
                if ((cp >= 0x1D400 && cp <= 0x1D7FF) || (cp >= 0x2200 && cp <= 0x22FF) || cp == '=') return true;
            }
            return false;
        };
        for (size_t i = 0; i < wordCount; ++i) {
            // Only a word with one bold letter is a candidate; its code points are read then.
            uint32_t bold = 0;
            for (uint32_t k = words[i].first; k < words[i].first + words[i].count && bold <= 1; ++k)
                if ((styleOf[k] & (kLetter | kStyleBold)) == (kLetter | kStyleBold)) bold++;
            if (bold != 1) continue;
            uint32_t letters = 0;
            for (uint32_t k = words[i].first; k < words[i].first + words[i].count; ++k) {
                if (!(styleOf[k] & kLetter)) continue;
                const uint32_t cp = FirstOf(Text(refs[k]));
                if (cp < 0x1D400 || cp > 0x1D7FF) letters++;
            }
            if (bold != 1 || letters != 1) continue;
            if (!mathWord(i) && !(i > 0 && mathWord(i - 1)) && !(i + 1 < wordCount && mathWord(i + 1))) continue;
            for (uint32_t k = words[i].first; k < words[i].first + words[i].count; ++k)
                styleOf[k] = static_cast<uint8_t>(styleOf[k] & ~kStyleBold);
        }
    }
    // The nearest letters before each glyph: any letter, and letters outside code ("firm" ones).
    if (before_.size() < 2 * size_t(n)) before_.resize(2 * size_t(n));
    int32_t* const before = before_.data();
    {
        int32_t any = -1, firm = -1;
        for (uint32_t k = 0; k < n; ++k) {
            before[k] = any, before[n + k] = firm;
            if (styleOf[k] & kLetter) any = static_cast<int32_t>(k);
            if ((styleOf[k] & (kLetter | kStyleCode)) == kLetter) firm = static_cast<int32_t>(k);
        }
    }
    // Backwards, with the nearest letters after each glyph: the styles each glyph is written in. A
    // glyph's styles are worked out from letters' code bits, which no glyph's result changes, and
    // from firm letters' styles, which keep theirs: the order the glyphs are taken in makes no
    // difference.
    const auto at = [&](int32_t k) { return k >= 0 ? static_cast<uint8_t>(styleOf[static_cast<uint32_t>(k)]) : uint8_t(0); };
    int32_t anyAfter = -1, firmAfter = -1;
    for (uint32_t k = n; k-- > 0;) {
        const uint8_t s = styleOf[k];
        uint8_t written = s;  // firm letters keep their own
        if ((s & (kLetter | kStyleCode)) != kLetter) {
            uint8_t result = 0;
            bool code = (s & kStyleCode) != 0;
            if (code && !(s & kLetter)) {
                // A monospace mark is code beside code: in a code word or between code words.
                const int32_t b = before[k], a = anyAfter;
                const bool word = (b >= 0 && wordOf[uint32_t(b)] == wordOf[k] && (at(b) & kStyleCode)) ||
                                  (a >= 0 && wordOf[uint32_t(a)] == wordOf[k] && (at(a) & kStyleCode));
                code = word || (at(b) & at(a) & kStyleCode) != 0;
            }
            if (code) result |= kStyleCode;
            const int32_t fb = before[n + k], fa = firmAfter;
            const uint8_t enclosed = at(fb) & at(fa) & (kStyleBold | kStyleItalic | kStyleStrike);
            if (code) {
                result |= enclosed | (s & (kStyleBold | kStyleItalic | kStyleStrike));
            } else {
                // Inside a word ("Alice’s") a mark between two letters of a style takes it whatever
                // its own face; elsewhere it keeps a style it is set in next to letters that have it.
                const bool sameB = fb >= 0 && wordOf[uint32_t(fb)] == wordOf[k];
                const bool sameA = fa >= 0 && wordOf[uint32_t(fa)] == wordOf[k];
                const uint8_t attached = static_cast<uint8_t>((sameB ? at(fb) : 0) | (sameA ? at(fa) : 0));
                result |= ((sameB && sameA ? enclosed : 0) | (s & (enclosed | attached))) &
                          (kStyleBold | kStyleItalic | kStyleStrike);
            }
            written = result;
        }
        styleOf[k] = static_cast<uint8_t>(written & kStyles);
        if (s & kLetter) anyAfter = static_cast<int32_t>(k);
        if ((s & (kLetter | kStyleCode)) == kLetter) firmAfter = static_cast<int32_t>(k);
    }
}

void Writer::ChangeStyle(uint8_t target, uint32_t at) {
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
        if (bit != kStyleCode) delimiters_.push_back(Delimiter{out_.size(), at > 0 ? at - 1 : 0, bit, false});
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
        if (best != 2) delimiters_.push_back(Delimiter{out_.size(), at, static_cast<uint8_t>(1u << best), true});
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

// CommonMark's class of a character beside a delimiter run (0.31 §2.1) as bits: Unicode
// whitespace (Zs, tab, line ends), Unicode punctuation (the P and S categories) or any other.
// Both of the latter where this table does not tell them apart (letterlike symbols, enclosed
// alphanumerics, bytes that are no UTF-8).
constexpr uint8_t kFlankSpace = 1, kFlankPunct = 2, kFlankOther = 4;

uint8_t FlankBits(uint32_t cp) {
    if (cp < 0x80) {
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f') return kFlankSpace;
        return cp < 0x20 || cp == 0x7F || IsAsciiAlnum(static_cast<char>(cp)) ? kFlankOther : kFlankPunct;
    }
    if (cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000)
        return kFlankSpace;
    if (cp < 0xC0)
        return cp < 0xA1 || cp == 0xAA || cp == 0xAD || cp == 0xB2 || cp == 0xB3 || cp == 0xB5 || cp == 0xB9 || cp == 0xBA ||
                       (cp >= 0xBC && cp <= 0xBE)
                   ? kFlankOther
                   : kFlankPunct;
    if (cp < 0x2000) {
        // Multiplication and division signs, the spacing modifiers that are symbols (˜ ˆ ˙), and
        // the punctuation of Greek, Armenian, Hebrew, Arabic, Devanagari and Thai.
        const bool punct = cp == 0xD7 || cp == 0xF7 || (cp >= 0x2C2 && cp <= 0x2C5) || (cp >= 0x2D2 && cp <= 0x2DF) ||
                           (cp >= 0x2E5 && cp <= 0x2EB) || cp == 0x2ED || (cp >= 0x2EF && cp <= 0x2FF) || cp == 0x375 ||
                           cp == 0x37E || cp == 0x384 || cp == 0x385 || cp == 0x387 || cp == 0x3F6 ||
                           (cp >= 0x55A && cp <= 0x55F) || cp == 0x589 || cp == 0x58A || cp == 0x5BE || cp == 0x5C0 ||
                           cp == 0x5C3 || cp == 0x5C6 || cp == 0x5F3 || cp == 0x5F4 || cp == 0x60C || cp == 0x60D ||
                           cp == 0x61B || cp == 0x61E || cp == 0x61F || (cp >= 0x66A && cp <= 0x66D) || cp == 0x6D4 ||
                           cp == 0x964 || cp == 0x965 || cp == 0x970 || cp == 0xE3F || cp == 0xE4F || cp == 0xE5A ||
                           cp == 0xE5B;
        return punct ? kFlankPunct : kFlankOther;
    }
    if (cp <= 0x2BFF) {
        // Format characters and the line and paragraph separators are no punctuation; nor are the
        // superscript and subscript digits and letters, or the number forms.
        if (cp <= 0x200F || (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F)) return kFlankOther;
        if (cp <= 0x205E) return kFlankPunct;
        if (cp <= 0x209F) return (cp >= 0x207A && cp <= 0x207E) || (cp >= 0x208A && cp <= 0x208E) ? kFlankPunct : kFlankOther;
        if (cp <= 0x20CF) return kFlankPunct;
        if (cp <= 0x20FF) return kFlankOther;
        if (cp <= 0x214F || (cp >= 0x2460 && cp <= 0x24FF) || (cp >= 0x2776 && cp <= 0x2793)) return kFlankPunct | kFlankOther;
        if (cp <= 0x218F) return kFlankOther;
        return kFlankPunct;
    }
    if ((cp >= 0x2E00 && cp <= 0x2FFF) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF01 && cp <= 0xFF0F) ||
        (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65) || (cp >= 0xFFE0 && cp <= 0xFFEE) || cp == 0xFFFC ||
        cp == 0xFFFD || (cp >= 0x1F000 && cp <= 0x1F0FF) || (cp >= 0x1F200 && cp <= 0x1FAFF))
        return kFlankPunct;
    if (cp >= 0x3001 && cp <= 0x303F)
        return (cp >= 0x3005 && cp <= 0x3007) || (cp >= 0x3021 && cp <= 0x302F) || (cp >= 0x3031 && cp <= 0x3035) ||
                       (cp >= 0x3038 && cp <= 0x303C)
                   ? kFlankOther
                   : kFlankPunct;
    if (cp >= 0x1F100 && cp <= 0x1F1FF) return kFlankPunct | kFlankOther;
    return kFlankOther;
}

// Whether a delimiter run between characters of the classes `before` and `after` (FlankBits) is
// left-flanking whichever class each is; right-flanking is the same with the two swapped.
bool LeftFlanking(uint8_t before, uint8_t after) {
    return !(after & kFlankSpace) && (!(after & kFlankPunct) || !(before & kFlankOther));
}

// RepairFlanking's rewrites of one block before every emphasis of a block still unflanked goes.
constexpr int kFlankAttempts = 4;

bool Writer::RepairFlanking(size_t from, int attempt) {
    if (delimiters_.empty()) return false;
    const size_t size = out_.size(), count = delimiters_.size();
    // The class of the character at out_[at]; in LaTeX mode a dollar is either class: a reader of
    // formulas takes `$…$` as one piece. The text is as AppendEscaped wrote it: an escaped
    // character is punctuation as it is unescaped, and stays escaped beside a delimiter.
    const auto bits = [&](size_t at) -> uint8_t {
        uint32_t cp = 0;
        size_t length = 0;
        if (!Utf8::Decode(out_, at, cp, length)) return kFlankPunct | kFlankOther;
        if (cp == '$' && math_ == MathMode::Latex) return kFlankPunct | kFlankOther;
        return FlankBits(cp);
    };
    const auto length = [&](size_t k) { return delimiters_[k].style == kStyleItalic ? size_t(1) : size_t(2); };
    bool failed = false, changed = false;
    for (size_t i = 0; i < count;) {
        // A run: the delimiters of one character written one after another.
        const bool tilde = delimiters_[i].style == kStyleStrike;
        const size_t start = delimiters_[i].at;
        size_t j = i + 1, end = start + length(i);
        while (j < count && delimiters_[j].at == end && (delimiters_[j].style == kStyleStrike) == tilde) end += length(j++);
        size_t back = start;
        while (back > from && (static_cast<unsigned char>(out_[back - 1]) & 0xC0) == 0x80) --back;
        const uint8_t before = start > from ? bits(back - 1) : kFlankSpace;
        const uint8_t after = end < size ? bits(end) : kFlankSpace;
        const bool left = LeftFlanking(before, after), right = LeftFlanking(after, before);
        // GFM's strike-through pairs runs of two tildes: none joins a tilde of the text.
        const bool tildes = tilde && (end - start != 2 || (start > from && out_[start - 1] == '~') || (end < size && out_[end] == '~'));
        for (; i < j; ++i) {
            const Delimiter& d = delimiters_[i];
            if (!tildes && (d.open ? left : right)) continue;
            failed = true;
            if (attempt < kFlankAttempts) changed = TakeStyle(d.glyph, d.style, d.open) || changed;
        }
    }
    if (!failed) return false;
    if (!changed)
        for (uint8_t& s : styles_) s = static_cast<uint8_t>(s & kStyleCode);
    return true;
}

bool Writer::TakeStyle(uint32_t glyph, uint8_t bit, bool open) {
    const uint32_t n = static_cast<uint32_t>(refs_.size());
    if (glyph >= n) return false;
    uint32_t j = glyph;
    // A delimiter kept open over glyphs without the style (a footnote reference) leaves the
    // nearest glyph of the run unflanked: the run goes.
    bool whole = false;
    if (!(styles_[j] & bit)) {
        if (open) {
            while (j < n && !(styles_[j] & bit)) ++j;
        } else {
            while (j > 0 && !(styles_[j] & bit)) --j;
        }
        if (j >= n || !(styles_[j] & bit)) return false;
        whole = true;
    }
    const auto space = [&](uint32_t k) { return (styles_[k] & bit) && (FlankBits(FirstOf(Text(refs_[k]))) & kFlankSpace); };
    uint32_t a = j, b = j + 1;  // the glyphs [a, b) lose the style
    if (!whole && (refs_[j].script != kScriptNone || (styles_[j] & kStyleCode))) {
        const auto same = [&](uint32_t k) {
            return wordOf_[k] == wordOf_[j] && refs_[k].script == refs_[j].script && ((styles_[k] ^ styles_[j]) & kStyleCode) == 0;
        };
        while (a > 0 && same(a - 1)) --a;
        while (b < n && same(b)) ++b;
    } else if (!whole && !IsLetterOrDigit(FirstOf(Text(refs_[j])))) {
        if (open) {
            while (b < n && space(b)) ++b;
        } else {
            while (a > 0 && space(a - 1)) --a;
        }
    } else {
        while (a > 0 && (styles_[a - 1] & bit)) --a;
        while (b < n && (styles_[b] & bit)) ++b;
    }
    bool changed = false;
    for (uint32_t k = a; k < b; ++k) {
        changed = changed || (styles_[k] & bit) != 0;
        styles_[k] = static_cast<uint8_t>(styles_[k] & ~bit);
    }
    return changed;
}

template <bool kLatex>
bool Writer::InlinePlain(const Block& block, bool styles) {
    uint16_t bail = kGlyphImage;
    if (styles)
        bail = static_cast<uint16_t>(bail | kGlyphStrike | (boldIsBody_ ? 0 : kGlyphBold) | (italicIsBody_ ? 0 : kGlyphItalic) |
                                     (monoIsBody_ ? 0 : kGlyphMonospace));
    // LaTeX mode: a bold letter alone may be a formula (FindInlineFormulas).
    if (kLatex && !boldIsBody_) bail = static_cast<uint16_t>(bail | kGlyphBold);
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
        // The line's glyphs and IsSuperscript's and IsSubscript's bounds in locals: out_'s byte
        // writes would make the compiler read them again at each glyph.
        const uint32_t* const lineGlyphs = page.lineGlyphs.data() + line.first;
        const Glyph* const glyphs = page.glyphs.data();
        const std::string_view pageText = page.text;
        const uint32_t count = line.count;
        const float small = 0.85f * line.size, raised = 0.2f * line.size, lowered = -0.1f * line.size;
        const float baseline = line.baseline;
        for (uint32_t k = ref.skip; k < count; ++k) {
            const Glyph& g = glyphs[lineGlyphs[k]];
            const std::string_view text = pageText.substr(g.text, g.textLength);  // PageData::GlyphText
            if (text.size() == 2 && text[0] == '\xC2' && text[1] == '\xAD' && k + 1 < count) {
                carriedSpace = carriedSpace || (g.flags & kGlyphSpaceBefore) != 0;
                continue;
            }
            if (g.size < small && !(g.flags & kGlyphInvisible)) {
                const float rise = g.v - baseline;
                if (rise > raised || rise < lowered) return abandon();  // a superscript or subscript
            }
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
        const bool dash = JoiningDash(last);
        if (!dash && last != "-" && last != "\xC2\xAD" && last != "\xE2\x80\x90") continue;
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
        if (dash) {
            joinNext = DashNeighbour(b0) && DashNeighbour(n0);
            continue;
        }
        const bool lowerNext = (n0 >= 'a' && n0 <= 'z') || n0 == 0xC3 || n0 == 0xC4 || n0 == 0xC5;
        const bool upperNext = n0 >= 'A' && n0 <= 'Z';
        const bool hard = last != "\xC2\xAD";
        if (letterBefore && hard && (lowerNext || upperNext)) {
            uint32_t codePoints = 0;
            bool hyphen = false;
            AddFragment(std::string_view(out_).substr(wordAt, out_.size() - wordAt - last.size()), codePoints, hyphen);
            if (CompoundBreak(upperNext, codePoints, hyphen) ||
                FunctionWordCompound(nextPage, nextLine, nextRef.skip) ||
                KnownCompound(page, line, line.count - 1, nextPage, nextLine, nextRef.skip)) {
                joinNext = true;
                continue;
            }
        }
        const bool join = letterBefore && lowerNext;
        if (!join && hard) continue;
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
    // LaTeX mode: formulas found among the glyphs are written as $…$ in their place; a table
    // cell's number stays text.
    if (math_ == MathMode::Latex) {
        FindFormulas(block);
        if (block.kind == BlockKind::Table) KeepNumbersText(inline_, mathRun_);
    }
    // A block whose delimiters do not all flank is written again with fewer styles.
    for (int attempt = 0;; ++attempt) {
        WriteWords(block);
        if (!RepairFlanking(from, attempt)) break;
        out_.resize(from);
        runs_.clear();
    }
    // No trailing spaces.
    while (!out_.empty() && out_.back() == ' ') out_.pop_back();
    DropNeedlessEscapes(from);
    JoinLeaderDots(from);
}

void Writer::WriteWords(const Block& block) {
    depth_ = 0;
    for (int s = 0; s < kStyleCount; ++s) runFrom_[s] = runTo_[s] = 0;
    delimiters_.clear();
    const bool latex = math_ == MathMode::Latex;
    // Unraised "(a)" references may be among the glyphs; the arrays read (none changes here). In
    // locals: out_'s byte writes would make the compiler read the members again after each one.
    const bool letterRefs = letterRefs_;
    const GlyphRef* const refs = refs_.data();
    const uint8_t* const styles = styles_.data();
    const uint32_t* const mathRun = mathRun_.data();
    const Word* const words = words_.data();
    const size_t wordCount = words_.size();
    uint32_t skipTo = 0;  // the glyphs before it went into a formula
    int32_t openLink = -1;
    // Heading and table cell text is inline: "## 1. Installation" opens no list.
    bool blockStart = block.kind != BlockKind::Heading && block.kind != BlockKind::Table;
    std::string_view openUri;
    // Styles close at glyph `at`, the link after them.
    const auto closeLink = [&](uint32_t at) {
        SetStyle(0, at);
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
    for (size_t i = 0; i < wordCount; ++i) {
        const Word& w = words[i];
        if (latex && skipTo >= w.first + w.count) continue;
        // A formula that ended inside the word: the rest of the word follows it directly.
        const bool resumed = latex && skipTo > w.first;
        if (!resumed) {
            const bool linkChanges = openLink >= 0 ? (i == 0 || !SameLink(words[i - 1], w)) : w.link >= 0;
            if (linkChanges) {
                closeLink(w.first);
            } else if (w.spaceBefore) {
                // Styles that do not go on past the space close before it, so that delimiters stay
                // flanking; all of them before a formula.
                const uint8_t keep = latex && mathRun[w.first] ? 0 : static_cast<uint8_t>(open() & styles[w.first]);
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
            if (k == trimAt && openLink >= 0) closeLink(k);
            uint32_t stop = k < trimAt ? trimAt : end;
            if (latex) {
                if (const uint32_t to = mathRun[k]) {
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
                    if (mathRun[j]) {
                        stop = j;
                        break;
                    }
            }
            const uint8_t script = refs[k].script;
            // A footnote reference [k, refEnd): a raised run, or an unraised "(a)" (semantics.h's
            // bold letter references).
            uint32_t refEnd = 0;
            if (script == kScriptSup && !noteRefs_.empty() && (k == w.first || refs[k - 1].script != kScriptSup)) {
                refEnd = k + 1;
                while (refEnd < end && refs[refEnd].script == kScriptSup) ++refEnd;
            } else if (letterRefs && script == kScriptNone && k + 3 <= end && Text(refs[k]) == "(") {
                refEnd = k + 3;
            }
            if (refEnd != 0) {
                if (const uint32_t number = NoteNumber(refs[k], refEnd - k)) {
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
            if (refs[k].flags & kGlyphImage) {
                SetStyle(static_cast<uint8_t>(open() & ~kStyleCode), k);
                out_.append(Text(refs[k]));
                blockStart = false;
                ++k;
                continue;
            }
            // Unicode mode: a formula's script in Unicode forms, in the style of its first glyph.
            if (script != kScriptNone && !latex && !(styles[k] & kStyleCode) && (k == w.first || refs[k - 1].script != script)) {
                if (const uint32_t to = ScriptForms(w, k, stop)) {
                    SetStyle(styles[k], k);
                    out_.append(forms_);
                    blockStart = false;
                    k = to;
                    continue;
                }
            }
            uint32_t runEnd = k + 1;
            while (runEnd < stop && refs[runEnd].script == script && styles[runEnd] == styles[k] &&
                   !(refs[runEnd].flags & kGlyphImage))
                ++runEnd;
            // An unraised "(a)" reference inside the run ends it.
            if (letterRefs && script == kScriptNone)
                for (uint32_t j = k + 1; j < runEnd; ++j)
                    if (j + 3 <= end && Text(refs[j]) == "(" && NoteNumber(refs[j], 3)) {
                        runEnd = j;
                        break;
                    }
            SetStyle(styles[k], k);
            if (depth_ > 0) blockStart = false;
            if (styles[k] & kStyleCode) {
                // Code spans hold their text as is; a backtick inside cannot be written in a
                // single-backtick span, so it becomes an apostrophe.
                for (uint32_t j = k; j < runEnd; ++j)
                    for (char c : Text(refs[j])) out_.push_back(c == '`' ? '\'' : c);
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
                    const std::string_view t = Text(refs[j]);
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
    closeLink(static_cast<uint32_t>(refs_.size()));
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

void Writer::FindFormulas(const Block& block) {
    // A bold glyph in a heading is the heading's face.
    const bool boldMath = !boldIsBody_ && block.kind != BlockKind::Heading;
    const uint32_t n = static_cast<uint32_t>(refs_.size());
    inline_.resize(n);
    // The arrays in locals, the page's read again only when the page changes: the bool stores
    // into inline_ would make the compiler read every member again at each glyph.
    const GlyphRef* const refs = refs_.data();
    InlineGlyph* const out = inline_.data();
    uint32_t pageIndex = UINT32_MAX;
    const Glyph* glyphs = nullptr;
    const uint8_t* fontMath = nullptr;
    size_t fonts = 0;
    bool tagged = false;  // a page of the block has tagged formulas (PageData::glyphFormula)
    for (uint32_t k = 0; k < n; ++k) {
        const GlyphRef& r = refs[k];
        if (r.page != pageIndex) {
            const PageData& page = pages_[r.page];
            pageIndex = r.page;
            glyphs = page.glyphs.data();
            fontMath = page.fontMath.data();
            fonts = page.fontMath.size();
            tagged = tagged || !page.glyphFormula.empty();
        }
        const Glyph& g = glyphs[r.glyph];
        InlineGlyph& ig = out[k];
        ig.glyph = &g;
        ig.font = g.font < fonts ? (fontMath[g.font] & kMathFontClass) : 0;
        ig.wordStart = false;
        ig.lineStart = r.lineStart != 0;
        ig.script = r.script != kScriptNone;
        ig.note = false;
        ig.bold = boldMath && (g.flags & kGlyphBold) != 0;
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
    for (uint32_t k = 0; tagged && k < n;) {
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
            // only), its cells escaped for a LaTeX reader with PipeLatex. A table broken across
            // pages is one table from its parts, each its anchor line then its cell lines. GFM
            // cells are inline Markdown, cut back out of `out` after the writer appends them.
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
            WriteGfmTable(parts, out, settings.tables != TableFormat::Pipe && settings.tables != TableFormat::PipeLatex,
                          settings.tables == TableFormat::PipeLatex);
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

// A coordinate as `%.2f` writes it (0 for a value that is not finite: JSON has no NaN). Apple's
// libc++ has no floating-point std::to_chars before macOS 13.3; its snprintf rounds as correctly.
void AppendCoordinate(std::string& out, float value) {
    char buf[64];
    const double number = std::isfinite(value) ? static_cast<double>(value) : 0.0;
#ifdef __APPLE__
    out.append(buf, static_cast<size_t>(std::snprintf(buf, sizeof buf, "%.2f", number)));
#else
    out.append(buf, std::to_chars(buf, buf + sizeof buf, number, std::chars_format::fixed, 2).ptr);
#endif
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

// `"box":[x0,y0,x1,y1]` of a box of the frame of `dir`, on the upright page.
void AppendFrameBox(std::string& out, uint8_t dir, float x0, float y0, float x1, float y1) {
    RectFromFrame(dir, x0, y0, x1, y1);
    out += "\"box\":[";
    AppendCoordinate(out, x0);
    out.push_back(',');
    AppendCoordinate(out, y0);
    out.push_back(',');
    AppendCoordinate(out, x1);
    out.push_back(',');
    AppendCoordinate(out, y1);
    out.push_back(']');
}

// Glyphs [k0, k1) of `line` as JSON's "lines" give a line: {"page","box","text"}, with `words`
// also "words":[{"box","text"}...]. The text joins the glyphs' texts with one space at each word
// break; inline images (kGlyphImage) are left out, and a word of nothing else too. `text` is
// scratch.
void AppendDetailLine(std::string& out, const PageData& page, const Line& line, uint32_t k0, uint32_t k1, bool words,
                      std::string& text) {
    const auto glyph = [&](uint32_t k) -> const Glyph& { return page.glyphs[page.lineGlyphs[line.first + k]]; };
    text.clear();
    bool space = false;
    for (uint32_t k = k0; k < k1; ++k) {
        const Glyph& g = glyph(k);
        space = space || (k > k0 && (g.flags & kGlyphSpaceBefore));
        if (g.flags & kGlyphImage) continue;
        if (space && !text.empty()) text.push_back(' ');
        space = false;
        text += page.GlyphText(g);
    }
    out += "{\"page\":";
    out += std::to_string(page.number);
    out.push_back(',');
    AppendFrameBox(out, line.dir, line.u0, line.vb, line.u1, line.vt);
    AppendKey(out, "text");
    Batch::AppendJsonString(out, text);
    if (words) {
        out += ",\"words\":[";
        bool first = true;
        for (uint32_t k = k0; k < k1;) {
            float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            bool any = false;
            text.clear();
            do {
                const Glyph& g = glyph(k);
                if (!(g.flags & kGlyphImage)) {
                    x0 = Fmin(x0, g.u0), y0 = Fmin(y0, g.vb), x1 = Fmax(x1, g.u1), y1 = Fmax(y1, g.vt);
                    text += page.GlyphText(g);
                    any = true;
                }
            } while (++k < k1 && !(glyph(k).flags & kGlyphSpaceBefore));
            if (!any) continue;
            out += first ? "{" : ",{";
            first = false;
            AppendFrameBox(out, line.dir, x0, y0, x1, y1);
            AppendKey(out, "text");
            Batch::AppendJsonString(out, text);
            out.push_back('}');
        }
        out.push_back(']');
    }
    out.push_back('}');
}

// `,"lines":[...]` of `block` (WriteJson with JsonDetail Lines or Words): its lines in order, a
// table's cells' lines cell by cell, none for an image or a comment.
void AppendBlockLines(std::string& out, const std::vector<PageData>& pages, const BlockList& list, const Block& block,
                      bool words, std::string& text) {
    out += ",\"lines\":[";
    const size_t start = out.size();
    const auto add = [&](const PageData& page, const Line& line, uint32_t k0, uint32_t k1) {
        if (k0 >= k1) return;
        if (out.size() > start) out.push_back(',');
        AppendDetailLine(out, page, line, k0, k1, words, text);
    };
    if (block.kind != BlockKind::Comment && block.kind != BlockKind::Image) {
        for (uint32_t bl = block.first; bl < block.first + block.count; ++bl) {
            const BlockLine& ref = list.lines[bl];
            const PageData& page = pages[ref.page];
            const Line& line = page.lines[ref.line];
            if (block.kind == BlockKind::Table) {
                if (!(line.flags & kLineTableAnchor)) continue;
                const Table& table = page.tables[line.first];
                for (uint32_t c = table.firstCell; c < table.firstCell + table.cellCount; ++c) {
                    const TableCell& cell = page.tableCells[c];
                    for (uint32_t l = cell.first; l < cell.first + cell.count; ++l)
                        add(page, page.lines[l], 0, page.lines[l].count);
                }
                continue;
            }
            // A heading run in with its paragraph is the first `number` glyphs of its line.
            const uint32_t end =
                block.kind == BlockKind::Heading && block.number != 0 ? std::min(block.number, line.count) : line.count;
            add(page, line, ref.skip, end);
        }
    }
    out.push_back(']');
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
               const DocumentMetadata* metadata, const std::vector<OutlineEntry>* outline, std::string& out,
               WriteCounts& counts, MathMode math, const WriteSettings& settings) {
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
    out.push_back(']');
    if (outline && !outline->empty()) {
        out += ",\"outline\":[";
        for (size_t i = 0; i < outline->size(); ++i) {
            const OutlineEntry& entry = (*outline)[i];
            out += i > 0 ? ",{\"level\":" : "{\"level\":";
            out += std::to_string(entry.level);
            AppendKey(out, "title");
            Batch::AppendJsonString(out, entry.title);
            AppendKey(out, "page");
            out += entry.page > 0 ? std::to_string(entry.page) : "null";
            out.push_back('}');
        }
        out.push_back(']');
    }
    out += ",\"blocks\":[";
    std::vector<PageBox> boxes;
    std::string text;
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
        if (settings.jsonDetail != JsonDetail::Blocks)
            AppendBlockLines(out, pages, list, block, settings.jsonDetail == JsonDetail::Words, text);
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
