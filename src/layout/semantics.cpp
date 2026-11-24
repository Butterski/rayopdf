// SPDX-License-Identifier: Apache-2.0
#include "layout/semantics.h"

#include "common/minmax.h"
#include "common/robust.h"
#include "common/utf8.h"
#include "layout/lines.h"
#include "layout/tables.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace RayoPdf::Layout {
namespace {

constexpr float kMonoShare = 0.8f;   // a code line: at least this share of monospace characters
constexpr float kHeadingSize = 1.12f; // a heading candidate: at least this times the body size
constexpr float kOcrIndent = 1.5f;    // an OCR layer's centred heading: clear of its column's edges by this, in ems
constexpr float kOcrAxis = 1.0f;      // ... its lines' centres this close to the column's, in ems
constexpr size_t kOcrNeighbours = 2;  // ... the column: a block this many blocks away at most
constexpr size_t kOcrRowWindow = 8;   // ... lines read on either side for another line on its baseline
constexpr float kOcrSizedShare = 0.8f; // an OCR layer set in its type's sizes: this share of its lines in one size each
constexpr float kOcrSizeSpread = 0.1f; // ... their glyphs this close to the line's size
// The top heading level is a title (H1) from this times the body size up. Measured first levels:
// LaTeX \section 1.44 (1.31 at 11 pt), typst 1.40, LibreOffice (pandoc's reference.docx) 1.67,
// browsers 2.0; second levels: LaTeX \subsection and typst 1.20 (1.10 at 11 pt), LibreOffice 1.33.
constexpr float kTitleSize = 1.38f;
// A top level used once or twice above deeper levels is a title from this times the body size up.
constexpr float kSmallTitleSize = 1.25f;
// A heading line over a title of at least this times its size, the page's topmost text, is a
// kicker (BuildBlocks).
constexpr float kKickerSize = 1.3f;
// A numbered section title set bold below the body size (REVTeX's at 0.9) is one from this
// times the body size up (BuildBlocks).
constexpr float kSmallHeadingSize = 0.8f;

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
    // The line ends in a word broken at a hyphen: a letter, then "-", U+2010 or a soft hyphen.
    bool hyphenBreak = false;
    bool hasLetter = false;
    bool rowPeer = false;    // another line of its style shares its baseline (a table row)
    bool pageRef = false;    // a lone page number sits far to its right (a table-of-contents entry)
    // A list marker Markdown cannot write: a letter or roman numeral ("a.", "(iv)"), a number in
    // parentheses or brackets ("(2)", "[12]"). The item keeps it as its text.
    bool verbatimMarker = false;
    bool closeParen = false;   // an enumerator ending in ")" ("4)"), which Markdown writes so
    bool markerAlone = false;  // the marker is the whole line: its text is the next line on its baseline
    bool bracketed = false;    // a reference entry's number in brackets ("[12]")
    bool numbering = false;  // only a section or item number ("1.5.", "IV", "(3)")
    uint8_t sectionDepth = 0;  // a section number of two levels or more opens the line ("2.1.", "4.1.2"): its depth
    bool hollowBox = false;  // its marker is "□": an open task box beside done boxes (⊠, ☒)
    float stripX0 = 0, stripX1 = 0;  // a band of shading behind the line alone (stripX1 > stripX0)
    bool topBand = false;    // the page's topmost line, alone in its top 6 %
    float gapBelow = 0;      // a top-band line's gap to the next line below
    uint32_t note = 0;       // the number of the footnote the line starts or continues
    bool noteStart = false;
    uint32_t noteSkip = 0;   // marker glyphs of a note's first line
    bool raised = false;     // a raised glyph after those it starts with (LineInfo; FindNotes reads it)
    // A parenthesized number or letter alone on its line ("(13)", "(2.4a)", "(b)"). BuildBlocks
    // keeps it only where no text follows it on its baseline: an equation's number or a subfigure's
    // label, a paragraph of its own and never a list item waiting for its text.
    bool label = false;
    // The line opens with a run-in label in bold or italic ("Proof.", "Lemma 2.", "Remark.") that
    // ends in a period, and goes on in another style.
    bool runInLabel = false;
    bool listStart() const { return marker != Marker::None; }
    // No list item after all: the marker is the line's text.
    void ClearMarker() {
        marker = Marker::None;
        skip = 0;
        task = 0;
        verbatimMarker = closeParen = markerAlone = bracketed = false;
    }
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

// A float's caption: a caption whose label and number no lowercase word follows ("Table 1 shows
// …" opens a sentence of the body).
bool IsFloatCaption(std::string_view t) {
    if (!IsCaption(t)) return false;
    size_t i = t.find(' ');  // after the label word
    while (i < t.size() && t[i] == ' ') ++i;
    while (i < t.size() && t[i] != ' ') ++i;  // the number
    while (i < t.size() && t[i] == ' ') ++i;
    return i >= t.size() || t[i] < 'a' || t[i] > 'z';
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

// Whether the first word of `t` (its leading letters, any case) is `lower`.
bool FirstWordIs(std::string_view t, std::string_view lower) {
    size_t n = 0;
    while (n < t.size() && ((t[n] >= 'a' && t[n] <= 'z') || (t[n] >= 'A' && t[n] <= 'Z'))) ++n;
    if (n != lower.size()) return false;
    for (size_t i = 0; i < n; ++i)
        if ((t[i] | 0x20) != lower[i]) return false;
    return true;
}

// A division's label alone: a word naming a division of a book or a course ("Chapter", "Part",
// "Appendix", "Unit", "Lecture", any case) and its number, Arabic (one to three digits), Roman
// (up to seven numerals) or one letter, with at most a full stop or a colon after it ("CHAPTER
// 1.", "Part IV", "Appendix B:"). Nothing else is on its line.
bool DivisionLabel(std::string_view t) {
    static const std::string_view kWords[] = {"chapter", "part", "appendix", "annex", "unit", "lesson", "lecture",
                                              "module", "book"};
    size_t n = 0;
    while (n < t.size() && ((t[n] >= 'a' && t[n] <= 'z') || (t[n] >= 'A' && t[n] <= 'Z'))) ++n;
    bool named = false;
    for (const std::string_view w : kWords) named = named || FirstWordIs(t, w);
    if (!named || n + 1 >= t.size() || t[n] != ' ') return false;
    std::string_view number = t.substr(n + 1);
    if (!number.empty() && (number.back() == '.' || number.back() == ':')) number.remove_suffix(1);
    if (number.empty() || number.size() > 7) return false;
    bool digits = true, roman = true;
    for (const char c : number) {
        digits = digits && c >= '0' && c <= '9';
        const char lower = static_cast<char>(c | 0x20);
        roman = roman && (lower == 'i' || lower == 'v' || lower == 'x' || lower == 'l' || lower == 'c');
    }
    const bool letter = number.size() == 1 && ((number[0] >= 'a' && number[0] <= 'z') || (number[0] >= 'A' && number[0] <= 'Z'));
    return (digits && number.size() <= 3) || roman || letter;
}

// The first byte of LineText(page, line) without building it (0 for none): a space put before a
// later glyph, or the first byte of the first glyph text.
unsigned char FirstTextByte(const PageData& page, const Line& line) {
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (k > 0 && (g.flags & kGlyphSpaceBefore)) return ' ';
        const std::string_view t = page.GlyphText(g);
        if (!t.empty()) return static_cast<unsigned char>(t[0]);
    }
    return 0;
}

// Whether `t` holds a web address ("http://", "https://", "www.") or an e-mail address (a word
// character before an '@' and a '.' after it in the same word).
bool HoldsAddress(std::string_view t) {
    if (t.find("http://") != std::string_view::npos || t.find("https://") != std::string_view::npos ||
        t.find("www.") != std::string_view::npos)
        return true;
    const auto word = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == '}';
    };
    for (size_t at = t.find('@'); at != std::string_view::npos; at = t.find('@', at + 1)) {
        if (at == 0 || !word(t[at - 1])) continue;
        for (size_t i = at + 1; i < t.size() && t[i] != ' '; ++i)
            if (t[i] == '.' && i > at + 1 && i + 1 < t.size() && t[i + 1] != ' ') return true;
    }
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

// Whether the text holds a math symbol: a Mathematical Alphanumeric Symbol, a Mathematical
// Operator, a Supplemental Mathematical Operator or "=".
bool MathSymbol(std::string_view t) {
    for (size_t at = 0; at < t.size(); ++at) {
        const unsigned char c = static_cast<unsigned char>(t[at]);
        if (c == '=') return true;
        if (c != 0xE2 && c != 0xF0) continue;
        const uint32_t cp = FirstCodePoint(t.substr(at));
        if ((cp >= 0x1D400 && cp <= 0x1D7FF) || (cp >= 0x2200 && cp <= 0x22FF) || (cp >= 0x2A00 && cp <= 0x2AFF))
            return true;
    }
    return false;
}

// A date alone on its line ("June 2023", "12 March 2021", "March 12, 2021", "Mon, 3 Jan 2022",
// "2023-06-01", "06/01/2023"): English month and weekday names (whole or cut to three letters,
// with an optional "."), day numbers 1-31, years of four digits; at least a month and a number, or
// a numeric date of three parts. A date dates a document and titles nothing.
bool DateLine(std::string_view t) {
    static const char* const kMonths[] = {"january", "february", "march", "april", "may", "june", "july",
                                          "august", "september", "october", "november", "december"};
    static const char* const kDays[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
    const auto named = [](std::string_view w, const char* const* names, size_t n) {
        if (!w.empty() && w.back() == '.') w.remove_suffix(1);
        if (w.size() < 3) return false;
        for (size_t i = 0; i < n; ++i) {
            const std::string_view name = names[i];
            if (w.size() != 3 && w.size() != name.size() && !(w.size() == 4 && name.size() > 4)) continue;
            if (w.size() > name.size()) continue;
            bool same = true;
            for (size_t k = 0; k < w.size() && same; ++k) same = (static_cast<unsigned char>(w[k]) | 0x20) == name[k];
            if (same) return true;
        }
        return false;
    };
    uint32_t months = 0, numbers = 0, words = 0, numeric = 0;
    size_t at = 0;
    while (at < t.size() && words < 8) {
        while (at < t.size() && (t[at] == ' ' || t[at] == ',')) ++at;
        size_t end = at;
        while (end < t.size() && t[end] != ' ' && t[end] != ',') ++end;
        if (end == at) break;
        const std::string_view w = t.substr(at, end - at);
        at = end;
        words++;
        bool digits = true;
        uint32_t parts = 1, run = 0, value = 0;
        for (const char c : w) {
            if (c >= '0' && c <= '9') {
                run++, value = value * 10 + static_cast<uint32_t>(c - '0');
            } else if ((c == '-' || c == '/' || c == '.') && run > 0) {
                parts++, run = 0;
            } else {
                digits = false;
            }
        }
        if (digits && parts == 3 && run > 0) {
            numeric++;
        } else if (digits && parts == 1 && ((w.size() <= 2 && value >= 1 && value <= 31) || (w.size() == 4 && value >= 1000))) {
            numbers++;
        } else if (named(w, kMonths, 12)) {
            months++;
        } else if (!named(w, kDays, 7)) {
            return false;
        }
    }
    return at >= t.size() && ((months == 1 && numbers >= 1 && numbers <= 2) || (numeric == 1 && months == 0 && numbers == 0));
}

// A copyright or licence notice ("© 2021 Elsevier B.V.", "Copyright 2019 IEEE", "CC0, 2022",
// "CC BY 4.0, 2023"): a "©" anywhere, or the word "copyright", the mark "(c)" or a Creative
// Commons licence ("CC0", "CC BY", "CC-BY") with a year from 1900 to 2099 or "rights reserved"
// beside it, in any letter case. It states the document's rights and titles nothing; a title
// about copyright ("Copyright Protects Creative Work") holds no year.
bool RightsLine(std::string_view t) {
    const auto alnum = [](char c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'); };
    // `word` (lowercase ASCII) at `at` in any letter case, not inside a longer word of letters
    // and digits.
    const auto wordAt = [&](size_t at, std::string_view word) {
        if (at + word.size() > t.size() || (at > 0 && alnum(t[at - 1]) && alnum(word.front()))) return false;
        for (size_t k = 0; k < word.size(); ++k)
            if ((word[k] >= 'a' && word[k] <= 'z' ? t[at + k] | 0x20 : t[at + k]) != word[k]) return false;
        const size_t end = at + word.size();
        return end == t.size() || !alnum(t[end]) || !alnum(word.back());
    };
    bool mark = false, dated = false;
    for (size_t at = 0; at < t.size(); ++at) {
        const char c = t[at];
        if (c == '\xC2' && at + 1 < t.size() && t[at + 1] == '\xA9') return true;
        if ((c | 0x20) == 'c') {
            mark = mark || wordAt(at, "copyright") || wordAt(at, "cc0") || wordAt(at, "cc by") || wordAt(at, "cc-by");
        } else if (c == '(') {
            mark = mark || wordAt(at, "(c)");
        } else if ((c | 0x20) == 'r') {
            dated = dated || wordAt(at, "rights reserved");
        } else if (c >= '0' && c <= '9' && (at == 0 || !alnum(t[at - 1]))) {
            size_t end = at;
            while (end < t.size() && t[end] >= '0' && t[end] <= '9') ++end;
            dated = dated || (end - at == 4 && (end == t.size() || !alnum(t[end])) &&
                              (t.substr(at, 2) == "19" || t.substr(at, 2) == "20"));
            at = end - 1;
        }
    }
    return mark && dated;
}

// Text that titles nothing: display math (an equation, with "=" and fewer than two words of three
// letters; a math symbol from Mathematical Alphanumeric Symbols, Mathematical Operators or
// Supplemental Mathematical Operators and fewer than three letters of ordinary text, or more math
// symbols than letters: "𝑇(𝑛) ≤ 𝑂(𝑛 log 𝑛)"; or four words and more of a letter or so each: a
// matrix row "h1 hn · · · h2") or fewer than two letters at all ("20%↑", "1.7X"). "Heading with
// ∇ ⋅ E = ρ" has words around its math and stays a possible heading. Letters are ASCII ones and
// every code point from U+00C0 up outside the symbol, punctuation, private-use and math blocks.
bool NoTitle(std::string_view t) {
    uint32_t math = 0, letters = 0, words = 0, longWords = 0, run = 0;
    bool equation = false;
    size_t at = 0;
    bool space = true;
    while (at < t.size()) {
        const uint32_t cp = FirstCodePoint(t.substr(at));
        const unsigned char c = static_cast<unsigned char>(t[at]);
        at += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (space && cp != ' ') words++;
        space = cp == ' ';
        equation = equation || cp == '=';
        bool letter = false;
        if ((cp >= 0x1D400 && cp <= 0x1D7FF) || (cp >= 0x2200 && cp <= 0x22FF) || (cp >= 0x2A00 && cp <= 0x2AFF) ||
            cp == '=')
            math++;
        else if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                 (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x2000 && cp <= 0x2BFF) &&
                  !(cp >= 0x3000 && cp <= 0x303F) && !(cp >= 0xE000 && cp <= 0xF8FF) && !(cp >= 0xFE00 && cp <= 0xFE0F) &&
                  !(cp >= 0xFF00 && cp <= 0xFF20) && cp != 0xFFFD))
            letters++, letter = true;
        run = letter ? run + 1 : 0;
        if (run == 3) longWords++;
    }
    return (equation && longWords < 2) || letters < 2 || (math > 0 && letters < 3) || math > letters ||
           (words >= 4 && 2 * letters < 3 * words);
}

// The length of the section numbering that opens a title, with the spaces after it: "4.3. ",
// "III-C ", "A. ", "B.2 ", "II-A1 ", "Appendix A "; 0 when there is none ("3D Printing"). Its
// parts are joined by "." or "-": up to three digits, a roman numeral of I, V and X, or one
// capital letter with up to two digits; "." ":" or ")" may end it. One letter alone numbers a
// title only with such an end or after "Appendix" ("A Survey", "I Think" start with words).
size_t NumberingPrefix(std::string_view t) {
    size_t i = 0;
    bool appendix = false;
    if (t.size() > 9 && t[8] == ' ') {
        static const char kWord[] = "appendix";
        appendix = true;
        for (size_t k = 0; k < 8 && appendix; ++k) appendix = (t[k] | 0x20) == kWord[k];
        if (appendix) i = 9;
    }
    const auto digit = [&](size_t k) { return k < t.size() && t[k] >= '0' && t[k] <= '9'; };
    bool lone = false;
    for (int parts = 0; parts < 8; ++parts) {
        const size_t start = i;
        if (digit(i)) {
            while (digit(i) && i - start < 4) ++i;
            if (i - start > 3) return 0;
            lone = false;
        } else {
            while (i < t.size() && (t[i] == 'I' || t[i] == 'V' || t[i] == 'X') && i - start < 8) ++i;
            if (i == start) {
                if (i >= t.size() || t[i] < 'A' || t[i] > 'Z') return 0;
                ++i;
            }
            lone = parts == 0 && i - start == 1;
            if (i - start == 1)
                for (int d = 0; d < 2 && digit(i); ++d) ++i, lone = false;
        }
        if (i + 1 < t.size() && (t[i] == '.' || t[i] == '-') && t[i + 1] != ' ') {
            ++i;
            continue;
        }
        break;
    }
    bool ended = false;
    if (i < t.size() && (t[i] == '.' || t[i] == ':' || t[i] == ')')) ++i, ended = true;
    if (i >= t.size() || t[i] != ' ' || (lone && !ended && !appendix)) return 0;
    while (i < t.size() && t[i] == ' ') ++i;
    // A title's word follows the numbering ("Z0 1" has none).
    const unsigned char next = i < t.size() ? static_cast<unsigned char>(t[i]) : 0;
    return ((next | 0x20) >= 'a' && (next | 0x20) <= 'z') || next >= 0xC3 ? i : 0;
}

// The outline matching key: lowercase ASCII letters and digits plus every non-ASCII byte but
// those of hyphens (U+00AD, U+2010, U+2011: a bookmark and its line may differ in them; a dash
// may be a list's bullet), written over `key` from byte `at` on. The key of texts joined by
// spaces is their keys joined: a space adds nothing, and no hyphen's bytes span a space.
void WriteMatchKey(std::string_view text, std::string& key, size_t at) {
    // A key is never longer than its text: written in place, then cut to its length.
    key.resize(at + text.size());
    char* const out = &key[0];
    size_t n = at;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const unsigned char u = static_cast<unsigned char>(c);
        if (c >= 'A' && c <= 'Z') {
            out[n++] = static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = c;
        } else if (u == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xAD) {
            ++i;
        } else if (u == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(text[i + 2]) >= 0x90 && static_cast<unsigned char>(text[i + 2]) <= 0x91) {
            i += 2;
        } else if (u >= 0x80) {
            out[n++] = c;
        }
    }
    key.resize(n);
}

// The outline matching key of `text` (WriteMatchKey); `stripNumbering` leaves the section
// numbering out (NumberingPrefix).
void MatchKey(std::string_view text, std::string& key, bool stripNumbering) {
    WriteMatchKey(stripNumbering ? text.substr(NumberingPrefix(text)) : text, key, 0);
}

// Key bytes a section numbering can hold (NumberingPrefix): "appendix" and eight parts of up to
// eight letters or digits.
constexpr size_t kMaxNumberingKey = 72;

// The bytes of MatchKey(t, key, false) that MatchKey(t, key, true) leaves out: the numbering's
// letters and digits. The numbering is ASCII, so the stripped key is the rest of the whole one.
size_t NumberingKeyBytes(std::string_view t) {
    const size_t p = NumberingPrefix(t);
    size_t n = 0;
    for (size_t i = 0; i < p; ++i) {
        const char c = t[i];
        n += (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    }
    return n;
}

// A table's or figure's label as an OCR layer spells it, by its match key (MatchKey): "table",
// "figure" or "fig" and then a digit or a letter recognised for a roman numeral or a digit (i v x
// l), the space before it kept or lost ("table28", "tableio", "figure3a").
bool OcrLabel(std::string_view key) {
    for (const std::string_view label : {std::string_view("table"), std::string_view("figure"), std::string_view("fig")}) {
        if (key.size() <= label.size() || key.substr(0, label.size()) != label) continue;
        const char c = key[label.size()];
        return (c >= '0' && c <= '9') || c == 'i' || c == 'v' || c == 'x' || c == 'l';
    }
    return false;
}

// The text ends in a row of leader dots (four or more, spaced or not), with or without the page
// number after them ("2.1 Methods . . . . . 17"): a table-of-contents entry.
bool EndsInLeader(std::string_view t) {
    size_t i = t.size();
    while (i > 0 && (t[i - 1] == ' ' || (t[i - 1] >= '0' && t[i - 1] <= '9'))) --i;
    uint32_t dots = 0;
    while (i > 0 && (t[i - 1] == '.' || t[i - 1] == ' ')) dots += t[i - 1] == '.' ? 1 : 0, --i;
    return dots >= 4;
}

// The text opens with a section number of one level and a title after it: an Arabic number of
// one or two digits or a roman one in capitals, a period and a space ("5. Conclusion", "IV.
// SURVEY"). Not "(2)", "i." or "a.": a list's or a note's markers.
bool OpensWithSectionNumber(std::string_view t) {
    size_t n = 0;
    if (!t.empty() && t[0] >= '0' && t[0] <= '9') {
        while (n < t.size() && n < 3 && t[n] >= '0' && t[n] <= '9') ++n;
        if (n > 2) return false;
    } else {
        while (n < t.size() && n < 7 && (t[n] == 'I' || t[n] == 'V' || t[n] == 'X')) ++n;
        if (n == 0 || n > 6) return false;
    }
    return n + 2 < t.size() && t[n] == '.' && t[n + 1] == ' ' && t[n + 2] != ' ';
}

// The text holds an ASCII lower-case letter.
bool HasLowerAscii(std::string_view t) {
    for (const char c : t)
        if (c >= 'a' && c <= 'z') return true;
    return false;
}

// The name of a standard unnumbered section of a paper, with or without a number before it
// ("Abstract", "REFERENCES", "7. Acknowledgments"). `key` is scratch.
bool StandardSection(std::string_view t, std::string& key) {
    static const std::string_view kNames[] = {"abstract",       "references",      "bibliography",    "acknowledgments",
                                              "acknowledgements", "acknowledgment", "acknowledgement", "appendix"};
    MatchKey(t, key, true);
    for (const std::string_view name : kNames)
        if (key == name) return true;
    return false;
}

// Glyphs [k, k + 3) of `line` are "(", a bold lowercase letter and ")" with the parentheses in
// another face: the letter's ordinal (1-26), else 0.
uint32_t BoldLetterMark(const PageData& page, const Line& line, uint32_t k) {
    if (k + 3 > line.count) return 0;
    const Glyph& open = page.glyphs[page.lineGlyphs[line.first + k]];
    const Glyph& letter = page.glyphs[page.lineGlyphs[line.first + k + 1]];
    const Glyph& close = page.glyphs[page.lineGlyphs[line.first + k + 2]];
    if (!(letter.flags & kGlyphBold) || ((open.flags | close.flags) & kGlyphBold)) return 0;
    const std::string_view t = page.GlyphText(letter);
    if (t.size() != 1 || t[0] < 'a' || t[0] > 'z' || page.GlyphText(open) != "(" || page.GlyphText(close) != ")") return 0;
    return static_cast<uint32_t>(t[0] - 'a' + 1);
}

// Small painted shapes near the left of lines: vector bullets, task boxes, quote bars.
struct Marks {
    std::vector<const Shape*> small;  // bullets and boxes
    std::vector<const Shape*> bars;   // thin vertical rules
    std::vector<const Shape*> strips; // filled bands a line high: shading behind a line
    // With many small marks and many lines to test, their centres' heights and indexes in `small`,
    // by height then index (built by LineInfo once `scans` lines tested every mark).
    struct Height {
        float cy;
        uint32_t index;
    };
    std::vector<Height> byHeight;
    uint32_t scans = 0;
    // Document-wide (CollectMarks keeps it): some line starts with a bold letter note's mark,
    // "(a)" (FindNotes).
    bool letterNotes = false;
    // Document-wide too: some line starts with a raised glyph (a note's mark): FindNotes then
    // looks for references in tables' cells.
    bool raisedStarts = false;
};

// Below this many small marks a line tests each; from it on, after kScansBeforeIndex lines did,
// those at its height (Marks::byHeight): sorting them costs about as much as that many scans.
constexpr size_t kIndexedMarks = 16;
constexpr uint32_t kScansBeforeIndex = 16;

// A small mark's centre height, as LineInfo tests it.
inline float CentreY(const Shape& s) { return (s.y0 + s.y1) / 2; }

void CollectMarks(const PageData& page, Marks& marks) {
    marks.small.clear();
    marks.bars.clear();
    marks.strips.clear();
    marks.byHeight.clear();
    marks.scans = 0;
    for (const Shape& s : page.shapes) {
        const float w = s.x1 - s.x0, h = s.y1 - s.y0;
        if (s.kind == ShapeKind::Rect && s.filled && !s.stroked && h >= 6 && h <= 40 && w >= 4 * h &&
            marks.strips.size() < 1024)
            marks.strips.push_back(&s);
        if (w > 0.3f && h > 0.3f && w <= 14 && h <= 14 && w <= 2.5f * h && h <= 2.5f * w) {
            if (marks.small.size() < 4096) marks.small.push_back(&s);
        } else if (w <= 6 && h >= 6 && h >= 3 * w && h <= 0.5f * page.height && s.kind != ShapeKind::Other) {
            // A rule taller than half the page divides columns; it marks no quote.
            if (marks.bars.size() < 4096) marks.bars.push_back(&s);
        }
    }
}

// A small mark's height is finite and positive, so neither of its y is NaN: no centre is NaN.
void IndexMarks(Marks& marks) {
    for (uint32_t i = 0; i < marks.small.size(); ++i) marks.byHeight.push_back(Marks::Height{CentreY(*marks.small[i]), i});
    std::sort(marks.byHeight.begin(), marks.byHeight.end(), [](const Marks::Height& a, const Marks::Height& b) {
        return a.cy != b.cy ? a.cy < b.cy : a.index < b.index;
    });
}

// `bold` and `italic` go up by the line's bold and italic glyphs (BlockList::boldBody).
void LineInfo(const PageData& page, uint32_t pi, uint32_t li, Marks& marks, Info& info, std::string& scratch,
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
    uint32_t mono = 0;
    // The line's bold and italic glyphs, counted at once: bold in the low half, italic in the high.
    static_assert(kGlyphBold == 1 && kGlyphItalic == 2, "kFaces is indexed by the two face bits");
    static constexpr uint64_t kFaces[4] = {0, 1, uint64_t(1) << 32, 1 + (uint64_t(1) << 32)};
    uint64_t faces = 0;
    const uint32_t* const ids = page.lineGlyphs.data() + line.first;
    const Glyph* const glyphs = page.glyphs.data();
    bool letter = false;
    bool numbering = line.chars <= 8;
    // A raised glyph after those the line starts with: a footnote reference FindNotes looks for.
    bool lead = true, raised = false, leadRaised = false;
    uint32_t k = 0;
    for (; k < line.count && !(letter && !numbering); ++k) {
        const Glyph& g = glyphs[ids[k]];
        faces += kFaces[g.flags & (kGlyphBold | kGlyphItalic)];
        if (!IsSuperscript(g, line))
            lead = false;
        else if (!lead)
            raised = true;
        else
            leadRaised = true;
        const uint32_t cp = FirstCodePoint(page.GlyphText(g));
        // Mathematical alphanumerics (𝑥, 𝐸) are math whatever the font's pitch: no code.
        if ((g.flags & kGlyphMonospace) && !(cp >= 0x1D400 && cp <= 0x1D7FF)) mono++;
        if (!letter) letter = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp >= 0xC0;
        if (numbering && !((cp >= '0' && cp <= '9') || cp == '.' || cp == ')' || cp == '(' || cp == 'I' ||
                           cp == 'V' || cp == 'X' || (k == 0 && cp >= 'A' && cp <= 'Z')))
            numbering = false;
    }
    // Once a letter is seen and the line is no numbering, only monospace glyphs need theirs. The
    // line's leading raised glyphs are followed in a loop of their own; past them any raised
    // glyph sets `raised`.
    for (; k < line.count && lead; ++k) {
        const Glyph& g = glyphs[ids[k]];
        const uint16_t f = g.flags;
        faces += kFaces[f & (kGlyphBold | kGlyphItalic)];
        lead = IsSuperscript(g, line);
        if (f & kGlyphMonospace) {
            const uint32_t cp = FirstCodePoint(page.GlyphText(g));
            if (!(cp >= 0x1D400 && cp <= 0x1D7FF)) mono++;
        }
    }
    for (; k < line.count; ++k) {
        const Glyph& g = glyphs[ids[k]];
        const uint16_t f = g.flags;
        faces += kFaces[f & (kGlyphBold | kGlyphItalic)];
        if (RAYOPDF_UNLIKELY(f & kGlyphMonospace)) {
            const uint32_t cp = FirstCodePoint(page.GlyphText(g));
            if (!(cp >= 0x1D400 && cp <= 0x1D7FF)) mono++;
        }
        if (RAYOPDF_UNLIKELY(IsSuperscript(g, line))) raised = true;
    }
    bold += static_cast<uint32_t>(faces);
    italic += faces >> 32;
    info.raised = raised;
    if (leadRaised) marks.raisedStarts = true;
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
    if ((lastText == "-" || lastText == "\xE2\x80\x90" || lastText == "\xC2\xAD") && line.count >= 2 &&
        !(lastGlyph.flags & kGlyphSpaceBefore)) {
        const uint32_t b = LastCodePoint(page.GlyphText(page.glyphs[page.lineGlyphs[line.first + line.count - 2]]));
        info.hyphenBreak = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
                           (b >= 0xC0 && b < 0x2000 && b != 0xD7 && b != 0xF7);
    }

    // The first word: a marker? Its text is read in place when its glyphs' texts follow each other
    // in the page text (ForGlyphTextRuns: the same bytes), else copied into `scratch`.
    uint32_t wordEnd = 1;
    const size_t textSize = page.text.size();
    std::string_view word = page.GlyphText(firstGlyph);
    size_t wordTextEnd = size_t(firstGlyph.text) + word.size();
    bool inPlace = true;
    for (; wordEnd < line.count; ++wordEnd) {
        const Glyph& n = glyphs[ids[wordEnd]];
        if (n.flags & kGlyphSpaceBefore) break;
        if (inPlace && (n.text != wordTextEnd || textSize - wordTextEnd < n.textLength))
            inPlace = false;
        else
            wordTextEnd += n.textLength;
    }
    if (inPlace) {
        word = std::string_view(word.data(), wordTextEnd - firstGlyph.text);
    } else {
        scratch.clear();
        AppendGlyphTexts(page, line, 0, wordEnd, scratch);
        word = scratch;
    }
    if (wordEnd == line.count && word.size() >= 3 && word.size() <= 8 && word.front() == '(' && word.back() == ')' &&
        line.dir == 0 && !(line.flags & kLineRtl)) {
        bool plain = true;
        for (size_t c = 1; c + 1 < word.size(); ++c) {
            const char ch = word[c];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-'))
                plain = false;
        }
        info.label = plain;
    }
    if (wordEnd < line.count) {
        // "2.1", "2.1." or "4.1.2." with text after it: numbers of one to three digits joined by dots.
        size_t at = 0;
        uint8_t depth = 0;
        while (at < word.size() && depth < 8) {
            size_t digits = 0;
            while (at < word.size() && word[at] >= '0' && word[at] <= '9') ++at, ++digits;
            if (digits == 0 || digits > 3) break;
            ++depth;
            if (at < word.size() && word[at] == '.') ++at;
        }
        if (at == word.size() && depth >= 2) info.sectionDepth = depth;
    }
    uint32_t value = 0;
    Marker marker = ParseMarker(word, value);
    uint32_t skip = wordEnd;
    if (marker == Marker::None && wordEnd > 1) {
        // A bullet set tight against its text. ParseMarker finds a bullet or a task box only in a
        // text of three bytes at most starting with one of these (value stays 0 without the call:
        // only an enumerator sets it, and an enumerator leaves the line without a marker here).
        const std::string_view t = page.GlyphText(firstGlyph);
        const unsigned char head = t.empty() ? 0 : static_cast<unsigned char>(t[0]);
        if (t.size() <= 3 && (head == '-' || head == '*' || head == 0xC2 || head == 0xE2 || head == 0xEF)) {
            const Marker single = ParseMarker(t, value);
            if ((single == Marker::Bullet || single == Marker::TaskOpen || single == Marker::TaskDone) && t != "-" &&
                t != "*" && t != "\xE2\x80\x93" && t != "\xE2\x80\x94")
                marker = single, skip = 1;
        }
    }
    // A reference entry's number in brackets ("[12]"): an unordered item that keeps it as text.
    // BuildBlocks keeps it only beside the entry before or after it.
    bool bracketed = false;
    if (marker == Marker::None && word.size() >= 3 && word.size() <= 5 && word.front() == '[' && word.back() == ']' &&
        word[1] != '0') {
        bracketed = true;
        for (size_t c = 1; c + 1 < word.size(); ++c) bracketed = bracketed && word[c] >= '0' && word[c] <= '9';
        if (bracketed) {
            marker = Marker::Bullet;
            for (size_t c = 1; c + 1 < word.size(); ++c) value = value * 10 + static_cast<uint32_t>(word[c] - '0');
        }
    }
    // A marker needs text after it on this line, unless the marker stands alone.
    if (marker != Marker::None && (skip < line.count || line.count == skip)) {
        // "-" and "*" and dashes are markers only when followed by text.
        if ((marker == Marker::Bullet) && skip == line.count && (word == "-" || word == "*")) marker = Marker::None;
    }
    if (marker != Marker::None) {
        info.marker = marker;
        info.value = value;
        info.skip = skip;
        // Markdown writes "1." and "1)"; any other enumerator, a letter or roman numeral ("a.",
        // "(iv)") or a number in parentheses ("(2)"), the item keeps as its text, unordered.
        info.verbatimMarker = bracketed || marker == Marker::Letter || marker == Marker::Roman ||
                              (marker == Marker::Number && word[0] == '(');
        info.closeParen = marker == Marker::Number && !info.verbatimMarker && word.back() == ')';
        info.bracketed = bracketed;
        info.markerAlone = skip == line.count;
        if ((marker == Marker::Letter || marker == Marker::Roman) && skip == 3 && !marks.letterNotes)
            marks.letterNotes = BoldLetterMark(page, line, 0) != 0;
        info.task = marker == Marker::TaskOpen ? 1 : marker == Marker::TaskDone ? 2 : 0;
        info.hollowBox = marker == Marker::Bullet && page.GlyphText(firstGlyph) == "\xE2\x96\xA1";  // □
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
    // A run-in label: up to four words of one emphasis (bold, italic or both) from the line's
    // first glyph, a capital letter, to a period (spaces after it aside), then text in another style.
    if (const uint16_t emph = firstGlyph.flags & (kGlyphBold | kGlyphItalic); emph != 0 && line.count >= 3) {
        const uint32_t cp0 = FirstCodePoint(page.GlyphText(firstGlyph));
        if ((cp0 >= 'A' && cp0 <= 'Z') || (cp0 >= 0xC0 && cp0 <= 0xDE && cp0 != 0xD7)) {
            constexpr uint32_t kMaxLabelGlyphs = 40;
            uint32_t words = 1, e = 1;
            bool restyled = false;  // a glyph in another style follows the label
            for (; e < line.count && e <= kMaxLabelGlyphs && words <= 4; ++e) {
                const uint16_t f = page.glyphs[page.lineGlyphs[line.first + e]].flags;
                if ((f & (kGlyphBold | kGlyphItalic)) != emph) {
                    restyled = true;
                    break;
                }
                words += (f & kGlyphSpaceBefore) != 0;
            }
            uint32_t end = e;
            while (end > 1 && (page.glyphs[page.lineGlyphs[line.first + end - 1]].flags & kGlyphSpace)) --end;
            info.runInLabel = restyled && words <= 4 &&
                              LastCodePoint(page.GlyphText(page.glyphs[page.lineGlyphs[line.first + end - 1]])) == '.';
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
        const auto consider = [&](const Shape* s) {
            const float w = s->x1 - s->x0, h = s->y1 - s->y0;
            const float cy = CentreY(*s);
            if (cy < line.vb || cy > line.vt) return;
            if (s->x1 > line.u0 + 0.5f || line.u0 - s->x1 > 2.5f * line.size) return;
            if (w > 0.9f * line.size || h > 0.9f * line.size) return;
            // Filled, or a stroked circle (a hollow bullet ◦): a bullet. A larger stroked square: a
            // task box.
            const bool hollowDot = s->stroked && !s->filled && s->curved && w < 0.45f * line.size;
            if (((s->filled && !s->stroked) || hollowDot) && w <= 0.6f * line.size && w >= 0.12f * line.size) {
                if (!bullet || s->x0 > bullet->x0) bullet = s;
            } else if (s->stroked && w >= 0.4f * line.size) {
                if (!box) box = s;
            }
        };
        // The marks whose centre lies within [vb, vt], taken in their order in `small` as the full
        // scan takes them (the first of equals wins); the full scan where they are few or many at
        // the line's height, until enough lines scanned, or where the line's heights are not
        // ordered numbers.
        constexpr size_t kMaxAtHeight = 32;
        uint32_t atHeight[kMaxAtHeight];
        size_t found = 0;
        bool scan = marks.small.size() < kIndexedMarks || marks.scans++ < kScansBeforeIndex || !(line.vb <= line.vt);
        if (!scan && marks.byHeight.empty()) IndexMarks(marks);
        if (!scan) {
            auto it = std::lower_bound(marks.byHeight.begin(), marks.byHeight.end(), line.vb,
                                       [](const Marks::Height& m, float y) { return m.cy < y; });
            for (; it != marks.byHeight.end() && it->cy <= line.vt; ++it) {
                if (found == kMaxAtHeight) {
                    scan = true;
                    break;
                }
                atHeight[found++] = it->index;
            }
        }
        if (scan) {
            for (const Shape* s : marks.small) consider(s);
        } else {
            std::sort(atHeight, atHeight + found);
            for (size_t i = 0; i < found; ++i) consider(marks.small[atHeight[i]]);
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
    // Shading behind the line alone: a band about the line's height, centred on it, reaching
    // past both its ends (a listing's background, drawn line by line).
    for (const Shape* s : marks.strips) {
        const float h = s->y1 - s->y0, cy = (s->y0 + s->y1) / 2;
        if (h > 2.2f * line.size || std::fabs(cy - (line.vb + line.vt) / 2) > 0.5f * line.size) continue;
        if (s->x0 > line.u0 - 1 || s->x1 < line.u1 + 1) continue;
        info.stripX0 = s->x0, info.stripX1 = s->x1;
        break;
    }
}

struct RawBlock {
    uint32_t first = 0, count = 0;  // infos
    float pitch = 0;
    float bodyU0 = 0;
    float right = 0;
    bool mono = false;
    bool bold = false;
    uint32_t note = 0;  // a footnote's lines
    // A label run in with its paragraph ("Abstract—"): the label's block is the first `take`
    // glyphs of its line; the paragraph's block, on the same line, starts `labelSkip` glyphs in.
    uint32_t take = 0, labelSkip = 0;
    bool lead = false;  // the first line is a list marker alone, the second its text on its baseline
    bool italicTitle = false;  // an italic title line its paragraph would have run on from (ItalicTitle)
};

// A short line in italics over the upright text it titles ("Directors' appointment and selection"
// over its paragraph): `line` upright in direction, all italic and not bold, not opening in lower
// case or with a list's marker, of four characters or more, ending without a sentence's or a
// clause's end, neither a row's cell nor a contents entry, short of the `next` line's right edge
// by more than 15 % of their width; `next` not italic and not going on in lower case.
bool ItalicTitle(const Info& line, const Info& next) {
    if (!(line.flags & kLineAllItalic) || (line.flags & kLineAllBold) || (next.flags & kLineAllItalic)) return false;
    if (line.dir != 0 || line.rowPeer || line.pageRef || !line.hasLetter || line.startsLower || next.startsLower ||
        line.listStart() || line.endsTerminal || line.chars < 4)
        return false;
    if (line.lastCp == ',' || line.lastCp == '-') return false;
    const float left = Fmin(line.u0, next.u0), right = Fmax(line.u1, next.u1);
    return line.u1 < right - 0.15f * (right - left);
}

// A line that starts with the label "Abstract" run in with the abstract's text: the word and a
// mark ("Abstract—We show", "ABSTRACT. In 1954", "Abstract: …"), or the word in bold before text
// that is not bold. `take` is the label's glyphs, `skip` those before the text (the label and its
// mark, which the heading stands for).
bool AbstractLabel(const PageData& page, const Line& line, uint32_t& take, uint32_t& skip) {
    static const char kWord[] = "abstract";
    if (line.count < 12) return false;
    bool bold = true;
    for (uint32_t k = 0; k < 8; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        const std::string_view t = page.GlyphText(g);
        if (t.size() != 1 || (t[0] | 0x20) != kWord[k] || (k > 0 && (g.flags & kGlyphSpaceBefore))) return false;
        bold = bold && (g.flags & kGlyphBold) != 0;
    }
    const Glyph& mark = page.glyphs[page.lineGlyphs[line.first + 8]];
    const std::string_view t = page.GlyphText(mark);
    take = 8;
    if (mark.flags & kGlyphSpaceBefore) {
        skip = 8;
        return bold && !(mark.flags & kGlyphBold);
    }
    skip = 9;
    return t == ":" || t == "\xE2\x80\x94" || t == "\xE2\x80\x93";
}

// Small capitals (§6.1): a block whose letters are all capitals in two sizes on one baseline, the
// larger where the source has a capital and the smaller where it has a lower-case letter (a
// font's small capitals made by scaling its capitals, as LaTeX's Times and word processors do).
// Both sizes stand on one line: lines of capitals each in a size of its own (a display title's
// "SEAGRASS" over "IN SOUTH FLORIDA") are no small capitals. Returns the size below which its
// capitals are lower-case letters; 0 when it is not set so: a lower-case letter, a script or an
// image in it, fewer than three small capitals, small capitals of several sizes, or of less than
// 60 % or more than 92 % of the large ones.
float SmallCapsSize(const std::vector<PageData>& pages, const std::vector<Info>& infos, const RawBlock& b) {
    float large = 0, smallMin = INFINITY, smallMax = 0;
    uint32_t smalls = 0;
    for (uint32_t i = b.first; i < b.first + b.count; ++i) {
        const PageData& page = pages[infos[i].page];
        const Line& line = page.lines[infos[i].line];
        float lineLarge = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32_t k = 0; k < line.count; ++k) {
                const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
                const std::string_view t = page.GlyphText(g);
                const uint32_t cp = FirstCodePoint(t);
                const bool capital = (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7);
                if (pass == 0) {
                    if (IsLower(cp) || (g.flags & kGlyphImage) || IsSuperscript(g, line) || IsSubscript(g, line)) return 0;
                    for (const char c : t)
                        if (c >= 'a' && c <= 'z') return 0;
                    if (capital) lineLarge = Fmax(lineLarge, g.size);
                } else if (capital && g.size <= 0.92f * lineLarge) {
                    if (std::fabs(g.v - line.baseline) > 0.05f * lineLarge) return 0;
                    smallMin = Fmin(smallMin, g.size), smallMax = Fmax(smallMax, g.size);
                    smalls++;
                    large = Fmax(large, lineLarge);
                }
            }
        }
    }
    if (smalls < 3 || smallMax > 1.03f * smallMin || smallMin < 0.6f * large) return 0;
    return 0.5f * (smallMax + large);
}

// Wide leading (a double-spaced manuscript): the pitch most pairs of neighbouring body lines
// have is more than the 1.6 em that the first two lines of a paragraph may be apart, and at that
// pitch the text runs on more often than not (the upper line ends without terminal punctuation
// and the lower one starts in lowercase), which lines of a list or a table set that far apart do
// not. The leading is set for the whole document (titles and abstracts in other sizes are spaced
// in proportion, LaTeX's sizes within 6 % of the body's ratio): returns the pitch, in ems of a
// line's size, up to which two lines are then neighbours (12 % above the common one), or 0.
float WideLeading(const std::vector<Info>& infos, float body) {
    constexpr int kSteps = 20;           // pitch in twentieths of the body size
    constexpr int kBins = 3 * kSteps + 2;  // up to three body sizes
    uint32_t pairs[kBins] = {}, running[kBins] = {};
    uint32_t total = 0;
    if (!(body > 0)) return 0;
    for (size_t i = 0; i + 1 < infos.size(); ++i) {
        const Info& a = infos[i];
        const Info& b = infos[i + 1];
        if (a.page != b.page || a.dir != 0 || b.dir != 0 || ((a.flags | b.flags) & (kLineAnchor | kLineTable | kLineFormula)))
            continue;
        if (std::fabs(a.size - body) > 0.05f * body || std::fabs(b.size - body) > 0.05f * body) continue;
        if (b.u0 > a.u1 || b.u1 < a.u0) continue;
        const float pitch = (a.baseline - b.baseline) / body;
        if (!(pitch > 0.8f && pitch <= 3.0f)) continue;
        const int bin = static_cast<int>(pitch * kSteps + 0.5f);
        pairs[bin]++;
        running[bin] += !a.endsTerminal && b.startsLower;
        total++;
    }
    int mode = 0;
    for (int bin = 1; bin < kBins; ++bin)
        if (pairs[bin] > pairs[mode]) mode = bin;
    if (mode <= static_cast<int>(1.6f * kSteps) + 1) return 0;
    const uint32_t count = pairs[mode - 1] + pairs[mode] + pairs[mode + 1];
    const uint32_t runs = running[mode - 1] + running[mode] + running[mode + 1];
    if (count < 8 || 2 * count <= total || 2 * runs <= count) return 0;
    return 1.12f * float(mode) / kSteps;
}

// `monoDocument`: the document is set in a monospace font, whose lines are prose, not code.
// `firstPitch`: how far apart, in ems, the first two lines of a paragraph may be (1.6, or more
// in a document set with wide leading).
bool Continues(const RawBlock& b, const Info& first, const Info& last, const Info& next, bool monoDocument,
               float firstPitch) {
    if (next.page != last.page || next.dir != last.dir) return false;
    if (next.label || last.label) return false;
    if ((next.flags ^ last.flags) & kLineFurniture) return false;  // kept furniture and the text
    // A section number set apart from its title ("1.5.   Migrant Workers") joins it.
    if (b.count == 1 && last.numbering && std::fabs(last.baseline - next.baseline) <= 0.3f * last.size &&
        next.u0 > last.u1 && next.u0 - last.u1 <= 4 * last.size && std::fabs(next.size - last.size) <= 0.15f * last.size)
        return true;
    if (next.listStart()) return false;
    if ((next.flags ^ last.flags) & kLineTable) return false;
    if ((next.flags | last.flags) & (kLineAnchor | kLineFormula)) return false;
    if (next.quoteBars != last.quoteBars) return false;
    // A section title of two levels or more set in italics on a line of its own ("2.1. Spatial
    // discretization", elsarticle's subsections) ends where its section's text goes on upright.
    if (b.count == 1 && last.sectionDepth >= 2 && (last.flags & kLineAllItalic) && !(next.flags & kLineAllItalic) &&
        !last.endsTerminal)
        return false;
    const float em = Fmax(last.size, next.size);
    // A size change starts a block, except in an OCR layer whose sizes are fitted word boxes.
    const bool ocr = (last.flags & next.flags & kLineInvisible) != 0;
    if (!ocr && std::fabs(next.size - last.size) > 0.15f * em) return false;
    // Nor does it have faces to set a heading off from its paragraph: there a line centred over a
    // wider next line, clear of both its ends, ends its block (ocrCentred's measures).
    if (ocr && next.u0 <= last.u0 - kOcrIndent * em && next.u1 >= last.u1 + kOcrIndent * em &&
        std::fabs((next.u0 + next.u1) - (last.u0 + last.u1)) <= 2 * kOcrAxis * em)
        return false;
    const float pitch = last.baseline - next.baseline;
    if (pitch <= 0.3f * em) return false;
    // No paragraph ends inside a word: a line ending in a word broken at a hyphen goes on in the
    // lowercase line below it, whatever the leading. (A list item keeps its own rules below.)
    if (!first.listStart() && last.hyphenBreak && next.startsLower && pitch <= 2.5f * em && next.u0 <= last.u1 &&
        next.u1 >= last.u0 &&
        b.mono == (next.mono >= kMonoShare && !monoDocument) && b.bold == ((next.flags & kLineAllBold) != 0))
        return true;
    const float limit = b.pitch > 0 ? Fmax(b.pitch * 1.3f, b.pitch + 0.25f * em) : firstPitch * em;
    if (pitch > limit) return false;
    // A proof, a lemma or a remark opening with its emphasised run-in label after a sentence's end,
    // set off by more space than the paragraph's own pitch, starts a paragraph.
    if (next.runInLabel && last.endsTerminal && b.pitch > 0 && pitch >= b.pitch + 0.25f * em) return false;
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
        // A line set out to the left of the paragraph's lines ends it (the text after a quote),
        // but for the margin below a drop cap, beside which the lines after the first are indented.
        const bool belowDropCap = (first.flags & kLineDropCap) && next.u0 >= first.u0 - 0.5f * em;
        if (b.count >= 2 && next.u0 < b.bodyU0 - 1.5f * em && !belowDropCap) return false;
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

constexpr uint32_t kLetterKey = 3000;  // a bold letter note's key: kLetterKey + the letter's ordinal

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
constexpr size_t kMaxFloatRun = 8;  // floats (captions, tables, images) a paragraph runs on around
constexpr float kProseWidth = 15.0f;  // ems a column of prose is wide at least

// A figure's own text, left out with FigureText::Drop: a block every line of which stands in or
// beside a figure region (kLineFigure: tick labels, legends, axis titles, the boxes of a diagram),
// unless it reads as prose or is furniture kept as text. Prose near a figure stays: a caption
// ("Figure 3: …"); a subfigure's caption ("(b) Trained on …"); a sentence's end at the body size
// ("Result:", the last words of a paragraph run on below the figure); or text set at 0.8 of the
// body size or larger whose widest line runs 12 ems or more and holds four words with a lowercase
// letter and no digit, half of its words at least (ten letters of a script without case, as Chinese
// or Japanese, count as such a sentence). Tick numbers, short labels and legends fail all of these.
bool ChartText(const std::vector<PageData>& pages, const std::vector<Info>& infos, const RawBlock& b, float body,
               std::string& line) {
    float size = 0, wide = 0;
    for (uint32_t i = b.first; i < b.first + b.count; ++i) {
        if (infos[i].flags & kLineFurniture) return false;
        size = Fmax(size, infos[i].size);
        if (infos[i].size > 0) wide = Fmax(wide, (infos[i].u1 - infos[i].u0) / infos[i].size);
    }
    const Info& first = infos[b.first];
    LineText(pages[first.page], pages[first.page].lines[first.line], line);
    if (IsCaption(line)) return false;
    if (line.size() >= 5 && line[0] == '(' && line[1] >= 'a' && line[1] <= 'z' && line[2] == ')' && line[3] == ' ' &&
        line[4] != ' ')
        return false;
    const bool sentenceEnd = infos[b.first + b.count - 1].endsTerminal && size >= 0.95f * body;
    const bool wideProse = size >= 0.8f * body && wide >= 12.0f;
    if (!sentenceEnd && !wideProse) return true;
    uint32_t words = 0, lower = 0, caseless = 0;
    for (uint32_t i = b.first; i < b.first + b.count; ++i) {
        if (i != b.first) LineText(pages[infos[i].page], pages[infos[i].page].lines[infos[i].line], line);
        bool inWord = false, hasLower = false, hasDigit = false;
        uint32_t letters = 0;
        const auto endWord = [&] {
            if (!inWord) return;
            ++words;
            lower += hasLower && !hasDigit && letters >= 2;
            inWord = hasLower = hasDigit = false;
            letters = 0;
        };
        uint32_t cp = 0;
        size_t length = 0;
        for (size_t k = 0; Utf8::Decode(line, k, cp, length); k += length) {
            if (cp == ' ') {
                endWord();
                continue;
            }
            inWord = true;
            if (cp >= '0' && cp <= '9') hasDigit = true;
            else if (IsLower(cp)) hasLower = true, ++letters;
            else if ((cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) ||
                     (cp >= 0x391 && cp <= 0x3A9) || (cp >= 0x400 && cp <= 0x42F))
                ++letters;
            else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF))
                ++caseless;
        }
        endWord();
    }
    if (sentenceEnd && (lower >= 1 || caseless >= 1)) return false;
    return !(wideProse && (caseless >= 10 || (lower >= 4 && lower * 2 >= words)));
}

// A page footnote sits below the text of its column: in the lower half of the page's text, and no
// line under it that it overlaps along the baseline is set larger, except in the page's last row
// (a running footer or page number left on a single page). Superscript-marked lines higher up are
// author affiliations (set in the title block at the size of the text below them) or table notes,
// and stay where they are. Fails closed (no note) once `budget` runs out.
bool AtBottom(const std::vector<Info>& infos, uint32_t begin, uint32_t end, uint32_t at, uint64_t& budget) {
    const Info& note = infos[at];
    if (2 * uint64_t(end - begin) > budget) return false;
    budget -= 2 * uint64_t(end - begin);
    const auto under = [&](const Info& o) {
        return o.dir == note.dir && o.baseline < note.baseline && o.chars > 0 && o.u0 < note.u1 && o.u1 > note.u0;
    };
    float lowest = INFINITY, top = -INFINITY, bottom = INFINITY;
    for (uint32_t j = begin; j < end; ++j) {
        if (under(infos[j])) lowest = Fmin(lowest, infos[j].baseline);
        if (infos[j].dir == note.dir && infos[j].chars > 0)
            top = Fmax(top, infos[j].baseline), bottom = Fmin(bottom, infos[j].baseline);
    }
    if (note.baseline > (top + bottom) / 2) return false;
    for (uint32_t j = begin; j < end; ++j) {
        const Info& o = infos[j];
        if (under(o) && o.size > 1.1f * note.size && o.baseline > lowest + 0.5f * o.size) return false;
    }
    return true;
}

constexpr uint64_t kRuleChecks = uint64_t(1) << 22;  // shapes RuleBetween may look at per document
constexpr uint64_t kCardChecks = uint64_t(1) << 22;  // lines and blocks card titles may look at per document

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

// Footnotes (algorithms §6.9). References are superscript runs inside lines, or inside a table's
// cells, whose text is a note key. A note starts a line with a marker whose key matches an unused
// reference: a superscript marker any size (LibreOffice sets notes in the body size), or an
// enumerator ("1.") in a smaller size or before a back link (HTML endnotes), so that body-size
// numbered lists stay lists; an enumerator alone on its line takes the text a tab to its right
// (page footnotes only).
// A page footnote's reference sits on its page above it and the note below the text (AtBottom);
// an enumerated endnote's reference on an earlier page, and the line before the endnotes is no
// heading ("References" sections keep their list). Lines of the note's size right below it
// continue it. `marks`: whether a line starts with a raised glyph or a bold letter note's mark.
void FindNotes(const std::vector<PageData>& pages, std::vector<Info>& infos, float body, const Marks& marks,
               std::vector<NoteRef>& out) {
    const bool letterNotes = marks.letterNotes;
    struct Ref {
        uint32_t info, page, glyph, count, key;
        float baseline, size;  // of the reference's line
        bool used;
    };
    std::vector<Ref> refs;
    std::unordered_map<uint32_t, std::vector<uint32_t>> byKey;
    std::string text;
    // The references of line `line` of info i's page.
    const auto collect = [&](uint32_t i, const Line& line) {
        const PageData& page = pages[infos[i].page];
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
    };
    for (uint32_t i = 0; i < infos.size() && refs.size() < kMaxNoteRefs; ++i) {
        const PageData& page = pages[infos[i].page];
        const Line& line = page.lines[infos[i].line];
        // A display formula's raised glyphs are its scripts and limits (--math=latex).
        if (line.flags & kLineFormula) continue;
        if (!infos[i].raised) continue;  // no superscript run after the line's first glyphs
        // A table's cells hold references too ("Net income⁽²⁾" in a row label), in cell order,
        // when a line starts with a raised mark to answer them.
        if (line.flags & kLineTableAnchor) {
            if (!marks.raisedStarts || line.first >= page.tables.size()) continue;
            const Table& table = page.tables[line.first];
            for (uint32_t l = table.firstLine; l < table.firstLine + table.lineCount && l < page.lines.size(); ++l)
                collect(i, page.lines[l]);
            continue;
        }
        collect(i, line);
    }
    // Notes keyed by a bold letter in parentheses ("(a)", as United Kingdom statutory instruments
    // print them): a note line starting with one in a document (`letterNotes`), then references
    // in the text that follow a word without a space ("Act 1965(a)"), unraised in the body size.
    for (uint32_t i = 0; i < infos.size() && letterNotes && refs.size() < kMaxNoteRefs; ++i) {
        const PageData& page = pages[infos[i].page];
        const Line& line = page.lines[infos[i].line];
        if (line.flags & (kLineFormula | kLineAnchor)) continue;
        for (uint32_t k = 1; k + 2 < line.count && refs.size() < kMaxNoteRefs; ++k) {
            const uint32_t value = BoldLetterMark(page, line, k);
            if (value == 0 || (page.glyphs[page.lineGlyphs[line.first + k]].flags & kGlyphSpaceBefore)) continue;
            byKey[kLetterKey + value].push_back(static_cast<uint32_t>(refs.size()));
            refs.push_back(Ref{i, infos[i].page, page.lineGlyphs[line.first + k], 3, kLetterKey + value, line.baseline,
                               line.size, false});
            k += 2;
        }
    }
    if (refs.empty()) return;

    uint32_t number = 0;
    uint32_t current = 0;  // the note being continued
    float noteSize = 0, lastBaseline = 0;
    uint32_t notePage = 0;
    bool markerAlone = false;  // the note's first line holds only its marker: its text is on its row
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
        // A marker alone on its line takes the text a tab to its right on its row.
        const bool rowText = i + 1 < infos.size() && infos[i + 1].page == in.page && infos[i + 1].chars > 0 &&
                             std::fabs(infos[i + 1].baseline - in.baseline) <= 0.3f * in.size &&
                             infos[i + 1].u0 > in.u1 && infos[i + 1].u0 - in.u1 <= 4 * in.size;
        if (line.count >= 2 && !(line.flags & kLineFormula)) {
            const uint32_t end = SuperscriptRun(page, line, 0, text);
            if (end > 0 && end < line.count) {
                key = NoteKey(text);
                skip = end;
            } else if (letterNotes && (in.marker == Marker::Letter || in.marker == Marker::Roman) && in.skip == 3 &&
                       (in.skip < line.count || rowText) && BoldLetterMark(page, line, 0) != 0) {
                // "(i)" parses as a roman numeral: the letter keys it.
                key = kLetterKey + BoldLetterMark(page, line, 0);
                skip = in.skip;
                enumerated = true;
            } else if (in.marker == Marker::Number && (in.skip < line.count || rowText) && in.value >= 1 &&
                       in.value <= 999) {
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
                        // A raised mark that starts a line higher up on the reference's page answers
                        // it (an affiliation, a table's note): the mark is no note's.
                        if (!samePage && !enumerated) {
                            refs[r].used = true;
                            break;
                        }
                    }
                    // A mark in larger print (an author list's) refers to no endnote, nor does a
                    // number alone on its line (a reference list's, a plot's tick label).
                    const bool endnote = enumerated && skip < line.count && ref.page < in.page && endnoteContext &&
                                         ref.size <= 1.05f * body;
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
            markerAlone = skip >= line.count;
            continue;
        }
        if (current && line.count > 0 && in.page == notePage && !in.listStart() &&
            std::fabs(in.size - noteSize) <= 0.15f * noteSize &&
            ((in.baseline < lastBaseline && lastBaseline - in.baseline <= 2.4f * noteSize) ||
             (markerAlone && std::fabs(in.baseline - lastBaseline) <= 0.3f * noteSize))) {
            in.note = current;
            lastBaseline = in.baseline;
            markerAlone = false;
            continue;
        }
        current = 0;
    }
    std::sort(out.begin(), out.end(), [](const NoteRef& a, const NoteRef& b) {
        return a.page != b.page ? a.page < b.page : a.glyph < b.glyph;
    });
}

constexpr size_t kTitleBlocks = 12;     // blocks after the title an abstract or heading is looked for in
constexpr size_t kMaxAddressBlocks = 16; // paragraphs an address run at the document's end holds at most
constexpr uint32_t kAddressLine = 120;   // characters of an address line without an e-mail or label
constexpr size_t kMaxAuthorText = 2048;  // bytes of the title block's author lines searched for names
constexpr size_t kMaxName = 80;          // bytes of a name looked up in them

// A block's text: its lines' text joined by spaces.
void BlockText(const std::vector<PageData>& pages, const std::vector<BlockLine>& lines, const Block& b,
               std::string& out, std::string& line) {
    out.clear();
    for (uint32_t i = b.first; i < b.first + b.count; ++i) {
        const PageData& page = pages[lines[i].page];
        LineText(page, page.lines[lines[i].line], line, lines[i].skip);
        if (!out.empty()) out.push_back(' ');
        out += line;
    }
}

// The lower case of a Latin letter (ASCII, Latin-1, Latin Extended-A), other code points as they
// are: "JÓZSEF" in an author line set in capitals is "József" in an address.
uint32_t FoldLatin(uint32_t cp) {
    if (cp < 0x80) return cp >= 'A' && cp <= 'Z' ? cp | 0x20 : cp;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
    if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) return cp | 1;
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) return cp + (cp & 1);
    return cp == 0x178 ? 0xFF : cp;
}

// `t` starts with `prefix`, Latin letters case folded (FoldLatin).
bool StartsFolded(std::string_view t, std::string_view prefix) {
    size_t i = 0, k = 0;
    uint32_t a = 0, b = 0;
    size_t la = 0, lb = 0;
    while (Utf8::Decode(prefix, k, b, lb)) {
        if (!Utf8::Decode(t, i, a, la) || FoldLatin(a) != FoldLatin(b)) return false;
        i += la, k += lb;
    }
    return true;
}

// `hay` holds `needle`, Latin letters case folded.
bool ContainsFolded(std::string_view hay, std::string_view needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (size_t at = 0; at + needle.size() <= hay.size(); ++at)
        if (StartsFolded(hay.substr(at), needle)) return true;
    return false;
}

bool EmailChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
           c == '_' || c == '+';
}

// An e-mail address in `t`: a name, "@" and a domain with a dot.
bool HasEmail(std::string_view t) {
    for (size_t at = t.find('@'); at != std::string_view::npos; at = t.find('@', at + 1)) {
        if (at == 0 || !EmailChar(t[at - 1])) continue;
        size_t end = at + 1;
        bool dot = false;
        while (end < t.size() && EmailChar(t[end])) {
            if (t[end] == '.' && end > at + 1 && end + 1 < t.size() && EmailChar(t[end + 1]) && t[end + 1] != '.')
                dot = true;
            ++end;
        }
        if (dot) return true;
    }
    return false;
}

// The labels amsart sets before an author's addresses.
bool AddressLabel(std::string_view t) {
    return StartsFolded(t, "e-mail address") || StartsFolded(t, "email address") ||
           StartsFolded(t, "current address") || StartsFolded(t, "present address") || StartsFolded(t, "url:");
}

// All letters capitals, four or more of them (an author line set in capitals).
bool InCapitals(std::string_view t) {
    uint32_t capitals = 0;
    for (const char c : t) {
        if (c >= 'a' && c <= 'z') return false;
        capitals += c >= 'A' && c <= 'Z';
    }
    for (size_t k = 0; k < t.size();) {
        const size_t start = k;
        ++k;
        while (k < t.size() && (static_cast<unsigned char>(t[k]) & 0xC0) == 0x80) ++k;
        if (IsLower(FirstCodePoint(t.substr(start, k - start)))) return false;
    }
    return capitals >= 4;
}

// Authors' addresses printed after the bibliography (amsart and the classes following it), which
// the source, and a reader, has under the author line: when the title's author line is set in
// capitals, the run of paragraphs that ends the document, each short and unpunctuated, an
// e-mail address, an address label ("E-mail address:") or starting with an author's name (or an
// author's surname in parentheses, amsart's label of each address of several authors), with at
// least one of the last two kinds among them, moves to before the abstract (or the first heading
// after the title). A code block of a line or two holding a web or e-mail address (amsart's
// \urladdr, set in typewriter type) is an address too. `blocks` and `lines` hold the body only
// (no notes yet).
void MoveAddresses(const std::vector<PageData>& pages, std::vector<Block>& blocks, std::vector<BlockLine>& lines) {
    if (blocks.size() < 3 || blocks[0].kind != BlockKind::Heading || lines[blocks[0].first].page != 0) return;
    std::string text, line, authors;
    // The abstract (or the first heading) first, from each block's first line where that is long
    // enough to tell: a block's text starts with its first line's.
    size_t abstract = 0;
    for (size_t i = 1; i < blocks.size() && i <= kTitleBlocks; ++i) {
        const Block& b = blocks[i];
        if (b.count == 0 || lines[b.first].page != 0) return;
        bool found = b.kind == BlockKind::Heading;
        if (!found) {
            const PageData& page = pages[lines[b.first].page];
            LineText(page, page.lines[lines[b.first].line], line, lines[b.first].skip);
            const bool shortLine = line.size() < 8;
            if (shortLine) BlockText(pages, lines, b, text, line);
            found = StartsFolded(shortLine ? text : line, "abstract");
        }
        if (found) {
            abstract = i;
            break;
        }
        if (b.kind != BlockKind::Paragraph) return;
    }
    if (abstract < 2) return;
    bool capitals = false;
    for (size_t i = 1; i < abstract; ++i) {
        BlockText(pages, lines, blocks[i], text, line);
        capitals = capitals || InCapitals(text);
        authors += text;
        authors.push_back('\n');
        if (authors.size() > kMaxAuthorText) return;
    }
    if (!capitals) return;
    size_t start = blocks.size();
    bool named = false;
    while (start - 1 > abstract && blocks.size() - start < kMaxAddressBlocks) {
        const Block& b = blocks[start - 1];
        // A code block of a line or two: an address set in typewriter type, else the run's end.
        const bool code = b.kind == BlockKind::Code && b.count <= 2;
        if ((b.kind != BlockKind::Paragraph && !code) || b.count == 0) break;
        BlockText(pages, lines, b, text, line);
        std::string_view t = text;
        const bool parenthesized = !t.empty() && t[0] == '(';
        while (!t.empty() && (t[0] == '(' || t[0] == ' ')) t.remove_prefix(1);
        const size_t nameEnd = t.find_first_of(",)");
        const std::string_view name = t.substr(0, nameEnd);
        const bool surname = parenthesized && nameEnd != std::string_view::npos && t[nameEnd] == ')';
        const bool author = !code && name.size() > 3 && name.size() <= kMaxName &&
                            (surname || name.find(' ') != std::string_view::npos) && ContainsFolded(authors, name);
        const bool address = HasEmail(t) || AddressLabel(t) || (code && HoldsAddress(t));
        const uint32_t cp = LastCodePoint(t);
        const bool shortLine =
            !code && t.size() <= kAddressLine && cp != '.' && cp != ';' && cp != ':' && !t.empty() && t[0] != '[';
        if (!author && !address && !shortLine) break;
        named = named || author || (address && !code);
        --start;
    }
    if (!named || start == blocks.size()) return;
    // Lines and blocks [start, end) go before blocks[abstract], in the same order.
    const uint32_t to = blocks[abstract].first, from = blocks[start].first;
    const uint32_t movedLines = static_cast<uint32_t>(lines.size()) - from, shift = from - to;
    std::rotate(lines.begin() + to, lines.begin() + from, lines.end());
    for (size_t i = abstract; i < start; ++i) blocks[i].first += movedLines;
    for (size_t i = start; i < blocks.size(); ++i) {
        Block& b = blocks[i];
        b.first -= shift;
        b.listParagraph = false;
        b.depth = 0;
        b.quote = 0;
    }
    std::rotate(blocks.begin() + static_cast<std::ptrdiff_t>(abstract), blocks.begin() + static_cast<std::ptrdiff_t>(start),
                blocks.end());
}

constexpr uint32_t kTitleBlockLines = 4;  // lines of a title block's paragraph (an author list, an affiliation) at most
constexpr uint64_t kMaxTitleGlyphs = uint64_t(1) << 16;  // glyphs of the title block searched for note marks

// Notes referenced from the title block (an author's affiliation or address, "Corresponding
// author", a \thanks of the title) are front matter, not footnotes of the text: their definitions
// are written as paragraphs under the title block, before the abstract (or the first heading),
// each with its mark first, and their marks stay raised text. The title block is the document's
// first blocks up to that point, all on its first page. The other notes are numbered anew in
// their order. The block must open with the title (a heading, not a numbered section's: a page
// that opens at "2.2 Structure of the tender" is inside a document) and hold short paragraphs
// only: a page that starts with body text (an excerpt) has no title block. `blocks` and `lines`
// hold the body only; `notes` index `noteLines`.
void TitleNotes(const std::vector<PageData>& pages, std::vector<Block>& blocks, std::vector<BlockLine>& lines,
                std::vector<Block>& notes, const std::vector<BlockLine>& noteLines, std::vector<NoteRef>& refs) {
    if (notes.empty() || refs.empty() || blocks.size() < 2 || blocks[0].count == 0 ||
        blocks[0].kind != BlockKind::Heading)
        return;
    const uint32_t page = lines[blocks[0].first].page;
    std::string line;
    {
        const BlockLine& bl = lines[blocks[0].first];
        LineText(pages[bl.page], pages[bl.page].lines[bl.line], line, bl.skip);
        if (NumberingPrefix(line) > 0) return;
    }
    size_t abstract = 0;
    for (size_t i = 1; i < blocks.size() && i <= kTitleBlocks; ++i) {
        const Block& b = blocks[i];
        if (b.count == 0 || lines[b.first].page != page) return;
        const BlockLine& bl = lines[b.first];
        LineText(pages[bl.page], pages[bl.page].lines[bl.line], line, bl.skip);
        if (b.kind == BlockKind::Heading || StartsFolded(line, "abstract")) {
            abstract = i;
            break;
        }
        if (b.count > kTitleBlockLines) return;
    }
    if (abstract == 0) return;
    // The note numbers whose marks stand in the title block.
    std::vector<uint32_t> moved;
    uint64_t budget = kMaxTitleGlyphs;
    for (size_t i = 0; i < abstract; ++i) {
        for (uint32_t l = blocks[i].first; l < blocks[i].first + blocks[i].count; ++l) {
            const PageData& pg = pages[lines[l].page];
            const Line& tl = pg.lines[lines[l].line];
            if (tl.count > budget) return;
            budget -= tl.count;
            for (uint32_t k = 0; k < tl.count; ++k) {
                const uint32_t glyph = pg.lineGlyphs[tl.first + k];
                const auto it = std::lower_bound(refs.begin(), refs.end(), std::make_pair(lines[l].page, glyph),
                                                 [](const NoteRef& n, const std::pair<uint32_t, uint32_t>& g) {
                                                     return n.page != g.first ? n.page < g.first : n.glyph < g.second;
                                                 });
                if (it != refs.end() && it->page == lines[l].page && it->glyph == glyph) moved.push_back(it->number);
            }
        }
    }
    if (moved.empty()) return;
    std::sort(moved.begin(), moved.end());
    moved.erase(std::unique(moved.begin(), moved.end()), moved.end());
    const auto isMoved = [&](uint32_t number) { return std::binary_search(moved.begin(), moved.end(), number); };
    // Their definitions become paragraphs before blocks[abstract], in note order, marks kept.
    std::vector<Block> front;
    std::vector<BlockLine> frontLines;
    const uint32_t to = blocks[abstract].first;
    for (const Block& note : notes) {
        if (!isMoved(note.number)) continue;
        Block b;
        b.kind = BlockKind::Paragraph;
        b.size = note.size;
        b.first = to + static_cast<uint32_t>(frontLines.size());
        b.count = note.count;
        for (uint32_t l = note.first; l < note.first + note.count; ++l) frontLines.push_back(noteLines[l]);
        frontLines[b.first - to].skip = 0;
        front.push_back(b);
    }
    if (front.empty()) return;
    lines.insert(lines.begin() + to, frontLines.begin(), frontLines.end());
    for (size_t i = abstract; i < blocks.size(); ++i) blocks[i].first += static_cast<uint32_t>(frontLines.size());
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(abstract), front.begin(), front.end());
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Block& n) { return isMoved(n.number); }), notes.end());
    refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const NoteRef& r) { return isMoved(r.number); }), refs.end());
    // The rest numbered 1, 2, … in their order.
    const auto renumber = [&](uint32_t number) {
        return number - static_cast<uint32_t>(std::lower_bound(moved.begin(), moved.end(), number) - moved.begin());
    };
    for (Block& note : notes) note.number = renumber(note.number);
    for (NoteRef& ref : refs) ref.number = renumber(ref.number);
}

constexpr uint64_t kMaxNoteGlyphs = uint64_t(1) << 22;  // glyphs of body lines searched for note references

// Footnote definitions stand in the reading flow, where a paper's source has them: each after the
// body block that holds its reference (the paragraph it annotates, on whatever page that ends) or,
// when no body block holds it, after the last block that starts on or before the note's page.
// Notes printed on the last page (every note of a one-page document, endnotes) stay at the end.
// `blocks` hold the body, `notes` the definitions in reading order, `refs` are sorted by page,
// then glyph.
void PlaceNotes(const std::vector<PageData>& pages, std::vector<Block>& blocks, const std::vector<BlockLine>& lines,
                const std::vector<Block>& notes, const std::vector<NoteRef>& refs) {
    if (notes.empty()) return;
    // The body block each note number's reference stands in.
    uint32_t maxNumber = 0;
    for (const Block& note : notes) maxNumber = std::max(maxNumber, note.number);
    std::vector<size_t> refBlock(size_t(maxNumber) + 1, SIZE_MAX);
    uint64_t budget = kMaxNoteGlyphs;
    for (size_t i = 0; i < blocks.size() && budget > 0; ++i) {
        for (uint32_t l = blocks[i].first; l < blocks[i].first + blocks[i].count; ++l) {
            const BlockLine& bl = lines[l];
            const auto lo = std::lower_bound(refs.begin(), refs.end(), bl.page,
                                             [](const NoteRef& r, uint32_t p) { return r.page < p; });
            if (lo == refs.end() || lo->page != bl.page) continue;
            const auto hi =
                std::upper_bound(lo, refs.end(), bl.page, [](uint32_t p, const NoteRef& r) { return p < r.page; });
            const PageData& pg = pages[bl.page];
            const Line& tl = pg.lines[bl.line];
            if (tl.count > budget) {
                budget = 0;
                break;
            }
            budget -= tl.count;
            for (uint32_t k = 0; k < tl.count; ++k) {
                const uint32_t glyph = pg.lineGlyphs[tl.first + k];
                const auto it = std::lower_bound(lo, hi, glyph, [](const NoteRef& r, uint32_t g) { return r.glyph < g; });
                if (it != hi && it->glyph == glyph && it->number <= maxNumber && refBlock[it->number] == SIZE_MAX)
                    refBlock[it->number] = i;
            }
        }
    }
    // before[n]: the body block note n goes before (blocks.size(): the end). The page foot is the
    // first block from which on every block starts on a later page (a block moved up, an author's
    // address, does not count); a note with none is on the last page.
    std::vector<size_t> before(notes.size(), blocks.size());
    std::vector<uint32_t> from(blocks.size() + 1, UINT32_MAX);
    for (size_t i = blocks.size(); i-- > 0;)
        from[i] = blocks[i].count > 0 ? std::min(from[i + 1], lines[blocks[i].first].page) : from[i + 1];
    size_t n = 0;
    for (size_t i = 0; i < blocks.size() && n < notes.size(); ++i)
        while (n < notes.size() && lines[notes[n].first].page < from[i]) before[n++] = i;
    // Kept furniture at the foot of a page (Furniture::Keep: a footer, a folio) is printed under
    // the page's notes: a note that ends its page's blocks goes before the furniture blocks there.
    const auto footFurniture = [&](const Block& b, uint32_t page) {
        if (b.count == 0) return false;
        for (uint32_t l = b.first; l < b.first + b.count; ++l) {
            const PageData& pg = pages[lines[l].page];
            const Line& tl = pg.lines[lines[l].line];
            if (lines[l].page != page || !(tl.flags & kLineFurniture) || tl.dir != 0 || tl.vb + tl.vt >= pg.height)
                return false;
        }
        return true;
    };
    for (n = 0; n < notes.size(); ++n) {
        const size_t ref = refBlock[notes[n].number];
        if (before[n] < blocks.size() && ref != SIZE_MAX) {
            before[n] = ref + 1;
            continue;
        }
        const uint32_t page = lines[notes[n].first].page;
        while (before[n] > 0 && footFurniture(blocks[before[n] - 1], page)) --before[n];
    }
    std::vector<uint32_t> order(notes.size());
    for (uint32_t k = 0; k < order.size(); ++k) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return before[a] < before[b]; });
    std::vector<Block> merged;
    merged.reserve(blocks.size() + notes.size());
    size_t k = 0;
    for (size_t i = 0; i <= blocks.size(); ++i) {
        while (k < order.size() && before[order[k]] <= i) merged.push_back(notes[order[k++]]);
        if (i < blocks.size()) merged.push_back(blocks[i]);
    }
    blocks.swap(merged);
}
} // namespace

void BuildBlocks(const std::vector<PageData>& pages, const std::vector<OutlineEntry>& outline,
                 const std::vector<uint8_t>& unmappedPages, bool dropFigureText, BlockList& out) {
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
    std::vector<uint32_t> inSentence;  // lines whose marker in parentheses refers to an equation
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
                t.raised = true;  // its glyphs, should it have any, are looked at by FindNotes
                // Its glyphs, should it have any, count for the faces like any line's.
                for (uint32_t k = 0; k < tl.count; ++k) {
                    const uint16_t f = page.glyphs[page.lineGlyphs[tl.first + k]].flags;
                    boldGlyphs += (f & kGlyphBold) != 0;
                    italicGlyphs += (f & kGlyphItalic) != 0;
                }
                continue;
            }
            LineInfo(page, p, li, marks, infos.back(), scratch, boldGlyphs, italicGlyphs);
            // A display formula (--math=latex) is no list item, heading number or task.
            if (tl.flags & kLineFormula) {
                Info& f = infos.back();
                f.ClearMarker();
                f.numbering = false;
            }
        }
        // Row peers: short lines of the same style side by side on one baseline are cells of a row
        // (a table header), not headings; a heading beside the other column's text has no peer. A
        // line with a lone number far to its right on its baseline is a table-of-contents entry
        // and its page, not a heading either; so is the entry's own number set apart before it
        // ("3   Results   8"), a line of its own.
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
                    const uint32_t at = a.numbering ? byBase[o] : byBase[k];
                    Info& entry = infos[at];
                    const Info& number = a.numbering ? a : b;
                    if (number.chars <= 4 && number.u0 - entry.u1 >= 3 * entry.size) {
                        entry.pageRef = true;
                        if (at > pageFirst) {
                            Info& before = infos[at - 1];
                            if (before.numbering && before.dir == entry.dir && before.u1 <= entry.u0 &&
                                std::fabs(before.baseline - entry.baseline) <= 0.3f * entry.size)
                                before.pageRef = true;
                        }
                    }
                }
                if (a.dir != b.dir || std::fabs(a.size - b.size) > 0.03f * a.size) continue;
                if (a.chars > 60 || b.chars > 60 || a.numbering || b.numbering) continue;
                if (((a.flags ^ b.flags) & kLineAllBold) || !(b.u0 > a.u1 || b.u1 < a.u0)) continue;
                if ((a.flags | b.flags) & kLineAnchor) continue;
                a.rowPeer = b.rowPeer = true;
            }
        }
        // The text of a list item whose marker stands apart follows it in the order on its
        // baseline; a parenthesized number without it labels an equation or a subfigure (the other
        // column's line beside an equation number is not next in the order). So does one with a
        // display formula two ems or more to its right: an equation numbered on the left.
        for (size_t i = pageFirst; i < infos.size(); ++i) {
            Info& in = infos[i];
            if (!in.label) continue;
            if (i + 1 < infos.size()) {
                const Info& next = infos[i + 1];
                if (next.dir == in.dir && std::fabs(next.baseline - in.baseline) <= 0.3f * in.size && next.u0 >= in.u1 &&
                    !(next.flags & kLineAnchor)) {
                    bool formula = false;
                    if (next.u0 - in.u1 >= 2 * in.size && next.chars <= 200) {
                        LineText(page, page.lines[next.line], scratch);
                        formula = NoTitle(scratch) && MathSymbol(scratch);
                    }
                    if (!formula) {
                        in.label = false;
                        continue;
                    }
                }
            }
            in.ClearMarker();
            in.textU0 = in.markerU0 = in.u0;
        }
        // A number in brackets starts a reference entry only next to another entry in the page's
        // order, numbered one less or one more, its marker within an em of this one's; a citation
        // that happens to start a line of a paragraph ("[12] showed that") stays text.
        {
            size_t prev = SIZE_MAX;
            bool prevKept = false;
            const auto drop = [&](Info& in) {
                in.ClearMarker();
                in.textU0 = in.markerU0 = in.u0;
                in.startsLower = false;
            };
            for (size_t i = pageFirst; i < infos.size(); ++i) {
                Info& in = infos[i];
                if (!in.bracketed) continue;
                bool kept = false;
                if (prev != SIZE_MAX) {
                    const Info& p = infos[prev];
                    kept = in.value == p.value + 1 && in.dir == p.dir && std::fabs(in.markerU0 - p.markerU0) <= in.size;
                    if (!kept && !prevKept) drop(infos[prev]);
                }
                prev = i;
                prevKept = kept;
            }
            if (prev != SIZE_MAX && !prevKept) drop(infos[prev]);
        }
        // A lowercase letter enumerator opening the line right below a word broken at a hyphen is
        // that word's end ("objec-" over "t. Examples"): no list item, a lowercase continuation.
        for (size_t i = pageFirst + 1; i < infos.size(); ++i) {
            Info& in = infos[i];
            const Info& above = infos[i - 1];
            if (in.marker != Marker::Letter || !above.hyphenBreak || above.dir != in.dir || (in.flags & kLineAnchor))
                continue;
            const float pitch = above.baseline - in.baseline;
            if (!(pitch > 0.3f * in.size && pitch <= 2.5f * Fmax(above.size, in.size)) || in.u0 > above.u1 || in.u1 < above.u0)
                continue;
            const Line& line = page.lines[in.line];
            if (!IsLower(FirstCodePoint(page.GlyphText(page.glyphs[page.lineGlyphs[line.first]])))) continue;
            in.ClearMarker();
            in.textU0 = in.markerU0 = in.u0;
            in.startsLower = true;
        }
        // A number or letter in parentheses that starts a line inside a sentence refers to an
        // equation or a part ("we solve\n(3) and characterize ..."): the line before it in the order,
        // directly above and not set in left of it, ends without a sentence end, and the text after
        // the marker starts in lower case. An item of a run stays: another item in parentheses within
        // 24 lines before or after it, its value one apart (or one a letter and one a roman numeral).
        inSentence.clear();
        for (size_t i = pageFirst + 1; i < infos.size(); ++i) {
            const Info& in = infos[i];
            if (!in.listStart() || !in.verbatimMarker || in.bracketed || !in.startsLower || in.markerAlone ||
                in.marker == Marker::Bullet || page.GlyphText(page.glyphs[page.lineGlyphs[page.lines[in.line].first]]) != "(")
                continue;
            const Info& prev = infos[i - 1];
            if ((prev.flags & kLineAnchor) || prev.dir != in.dir || !prev.hasLetter || prev.endsTerminal) continue;
            const float pitch = prev.baseline - in.baseline;
            if (!(pitch > 0.5f * in.size && pitch <= 2 * in.size) || in.u0 > prev.u0 + in.size || prev.u1 < in.u0) continue;
            const auto lettered = [](Marker m) { return m == Marker::Letter || m == Marker::Roman; };
            bool run = false;
            for (size_t j = i > pageFirst + 24 ? i - 24 : pageFirst; j < infos.size() && j <= i + 24 && !run; ++j) {
                const Info& o = infos[j];
                run = j != i && o.listStart() && o.verbatimMarker && !o.bracketed &&
                      ((o.marker == in.marker && (o.value + 1 == in.value || in.value + 1 == o.value)) ||
                       (o.marker != in.marker && lettered(o.marker) && lettered(in.marker)));
            }
            if (run) continue;
            // "...; and" or "...; or" before it: the last item of an enumeration in a sentence.
            LineText(page, page.lines[prev.line], scratch);
            std::string_view t = scratch;
            while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
            const bool joined = (t.size() >= 4 && t.substr(t.size() - 4) == " and") || (t.size() >= 3 && t.substr(t.size() - 3) == " or");
            if (!(joined && t.find(';') != std::string_view::npos)) inSentence.push_back(static_cast<uint32_t>(i));
        }
        for (uint32_t i : inSentence) {
            infos[i].ClearMarker();
            infos[i].textU0 = infos[i].markerU0 = infos[i].u0;
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
        // Lines shaded one by one with bands of one width, one under the next: a listing set in a
        // proportional font (its shading is what marks it as code).
        for (size_t i = pageFirst; i + 1 < infos.size(); ++i) {
            Info& a = infos[i];
            Info& b = infos[i + 1];
            if (a.stripX1 <= a.stripX0 || b.stripX1 <= b.stripX0 || a.quoteBars || b.quoteBars) continue;
            if (std::fabs(a.stripX0 - b.stripX0) > 1 || std::fabs(a.stripX1 - b.stripX1) > 1) continue;
            a.mono = b.mono = 1;
        }
        // A "□" bullet in a list of done boxes ("⊠", LaTeX's task lists) is an open task box: one
        // set on the left edge of a done box's marker within an em.
        for (size_t i = pageFirst; i < infos.size(); ++i) {
            if (!infos[i].hollowBox) continue;
            for (size_t j = pageFirst; j < infos.size(); ++j)
                if (infos[j].task == 2 && std::fabs(infos[j].markerU0 - infos[i].markerU0) <= infos[i].size) {
                    infos[i].task = 1;
                    break;
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
    const float firstPitch = Fmax(1.6f, WideLeading(infos, body));
    FindNotes(pages, infos, body, marks, out.noteRefs);
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
            } else if (b.first + b.count == i && b.count == 1 && first.markerAlone && in.page == first.page &&
                       in.dir == first.dir && std::fabs(in.baseline - first.baseline) <= 0.3f * first.size &&
                       in.u0 >= first.u1 - 0.1f * first.size && in.u0 - first.u1 <= 4 * Fmax(first.size, in.size) &&
                       in.hasLetter && !in.listStart() && !in.label && in.quoteBars == first.quoteBars &&
                       !((in.flags | first.flags) & (kLineAnchor | kLineTable | kLineFormula | kLineFurniture))) {
                // A list marker set apart from its text ("•", a tab, then the text) is the item's
                // first line; its text, next on its baseline, takes the block's measures. A number
                // alone beside a dash (a table's zero) is no item's text.
                b.count++;
                b.lead = true;
                b.bodyU0 = in.u0;
                b.right = Fmax(b.right, in.u1);
                b.mono = in.mono >= kMonoShare && !monoDocument;
                b.bold = (in.flags & kLineAllBold) != 0;
                continue;
            } else if (b.first + b.count == i && Continues(b, first, last, in, monoDocument, firstPitch)) {
                // An italic title line its paragraph of two lines or more would run on from ends
                // its block there (a lone italic word over one line, "Required" over "Type:
                // string", is the paragraph's own text).
                if (b.count == 1 && b.note == 0 && !out.italicBody && last.size >= 0.915f * body && ItalicTitle(last, in) &&
                    i + 1 < infos.size() && infos[i + 1].note == 0) {
                    RawBlock para;
                    para.first = i;
                    para.count = 1;
                    para.bodyU0 = in.u0;
                    para.right = in.u1;
                    para.mono = in.mono >= kMonoShare && !monoDocument;
                    para.bold = (in.flags & kLineAllBold) != 0;
                    b.italicTitle = Continues(para, in, in, infos[i + 1], monoDocument, firstPitch);
                }
                if (!b.italicTitle) {
                    if (b.count == 1 || (b.lead && b.count == 2)) {
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

    // A display formula set in pieces (a fraction's numerator and denominator, an integral's
    // limits, a matrix's entries each on a baseline of its own): single short lines of formula
    // text, each within an em of the box of the pieces before it (beside or below them), with a
    // math symbol among them, make one paragraph in reading order.
    {
        // 0: no piece, 1: a piece, 2: a piece with a math symbol; worked out once per block. A
        // block that may be a piece is tested only beside another that may: alone it joins nothing.
        constexpr uint8_t kMayBePiece = 3;
        std::vector<uint8_t> pieceOf(raws.size(), 0);
        for (size_t r = 0; r < raws.size(); ++r) {
            const RawBlock& b = raws[r];
            const Info& in = infos[b.first];
            if (b.count != 1 || b.note != 0 || b.mono || in.chars == 0 || in.chars > 40 || in.dir != 0 || in.label ||
                in.listStart() || in.quoteBars != 0 || (in.flags & (kLineAnchor | kLineTable | kLineFormula)))
                continue;
            pieceOf[r] = kMayBePiece;
        }
        for (size_t r = 0; r < raws.size(); ++r) {
            if (pieceOf[r] != kMayBePiece) continue;
            if (!((r > 0 && pieceOf[r - 1] != 0) || (r + 1 < raws.size() && pieceOf[r + 1] != 0))) {
                pieceOf[r] = 0;
                continue;
            }
            const Info& in = infos[raws[r].first];
            LineText(pages[in.page], pages[in.page].lines[in.line], scratch);
            pieceOf[r] = NoTitle(scratch) ? (MathSymbol(scratch) ? 2 : 1) : 0;
        }
        size_t kept = 0;
        for (size_t r = 0; r < raws.size();) {
            bool math = pieceOf[r] == 2;
            size_t end = r + 1;
            if (pieceOf[r] != 0) {
                const Info& head = infos[raws[r].first];
                float u0 = head.u0, u1 = head.u1, vb = head.vb, vt = head.vt;
                while (end < raws.size() && end - r < 16 && pieceOf[end] != 0) {
                    const Info& in = infos[raws[end].first];
                    const float em = Fmax(head.size, in.size);
                    if (in.page != head.page || in.vt < vb - em || in.vb > vt + em || in.u0 > u1 + 4 * em ||
                        in.u1 < u0 - 4 * em)
                        break;
                    math = math || pieceOf[end] == 2;
                    u0 = Fmin(u0, in.u0), u1 = Fmax(u1, in.u1), vb = Fmin(vb, in.vb), vt = Fmax(vt, in.vt);
                    ++end;
                }
            }
            raws[kept] = raws[r];
            if (end > r + 1 && math) {
                raws[kept].count = raws[end - 1].first + raws[end - 1].count - raws[r].first;
                r = end;
            } else {
                ++r;
            }
            ++kept;
        }
        raws.resize(kept);
    }

    // The abstract's label run in with its text ("Abstract—We show …"): the label heads the
    // paragraph as a block of its own on the paragraph's first line, whose text starts after it.
    for (size_t r = 0; r < raws.size(); ++r) {
        const RawBlock& b = raws[r];
        const Info& first = infos[b.first];
        if (b.count < 2 || b.note != 0 || b.mono || first.dir != 0 ||
            (first.flags & (kLineTable | kLineFormula | kLineTableAnchor | kLineImage | kLineInvisible)))
            continue;
        uint32_t take = 0, skip = 0;
        if (!AbstractLabel(pages[first.page], pages[first.page].lines[first.line], take, skip)) continue;
        RawBlock label = b;
        label.count = 1;
        label.take = take;
        raws[r].labelSkip = skip;
        raws.insert(raws.begin() + static_cast<std::ptrdiff_t>(r), label);
        break;  // a document has one abstract
    }

    // Outline titles (D5: outline > style > numbering). A text whose key outgrows the longest
    // title key by more than a numbering can hold (`keyLimit`) matches none, stripped or not.
    std::unordered_map<std::string, std::vector<uint32_t>> titles;
    std::vector<uint8_t> titleUsed(outline.size(), 0);
    size_t maxTitleKey = 0;
    for (uint32_t t = 0; t < outline.size(); ++t) {
        MatchKey(outline[t].title, scratch, false);
        maxTitleKey = std::max(maxTitleKey, scratch.size());
        if (!scratch.empty()) titles[scratch].push_back(t);
        scratch.erase(0, NumberingKeyBytes(outline[t].title));
        if (!scratch.empty()) titles[scratch].push_back(t);
    }
    const size_t keyLimit = maxTitleKey + kMaxNumberingKey;
    std::string key;
    // A heading set like the text under it (IEEE's italic "B. Problem statement" in the body's
    // size and pitch) was grouped with that paragraph: where the first line of a block of body
    // text alone is an outline title and the block as a whole is none, the line makes a block of
    // its own. A title's first line may be a section's title too: what follows a title's line is
    // no paragraph (two lines or more, or the end of a sentence), or is set large.
    if (!titles.empty()) {
        std::string joined;
        // The text of the line a candidate block starts with, kept for its second test.
        std::string firstText;
        uint32_t firstLine = UINT32_MAX;
        const auto titled = [&](uint32_t from, uint32_t count) {
            joined.clear();
            key.clear();
            for (uint32_t i = from; i < from + count; ++i) {
                if (i == firstLine) {
                    scratch = firstText;
                } else {
                    const PageData& page = pages[infos[i].page];
                    LineText(page, page.lines[infos[i].line], scratch);
                    if (i == from) firstText = scratch, firstLine = i;
                }
                if (!joined.empty()) joined.push_back(' ');
                joined += scratch;
                WriteMatchKey(scratch, key, key.size());
                if (key.size() > keyLimit) return false;
            }
            if (key.size() <= maxTitleKey && titles.find(key) != titles.end()) return true;
            key.erase(0, NumberingKeyBytes(joined));
            return key.size() <= maxTitleKey && titles.find(key) != titles.end();
        };
        const auto block = [&](uint32_t from, uint32_t count, const RawBlock& whole) {
            RawBlock b = whole;
            b.first = from;
            b.count = count;
            b.bold = (infos[from].flags & kLineAllBold) != 0;
            b.bodyU0 = infos[count > 1 ? from + 1 : from].u0;
            b.right = infos[from].u1;
            for (uint32_t i = from + 1; i < from + count; ++i)
                b.bodyU0 = Fmin(b.bodyU0, infos[i].u0), b.right = Fmax(b.right, infos[i].u1);
            if (count > 1) b.pitch = infos[from].baseline - infos[from + 1].baseline;
            return b;
        };
        std::vector<RawBlock> split;
        for (size_t r = 0; r < raws.size(); ++r) {
            const RawBlock b = raws[r];
            const Info& first = infos[b.first];
            const bool candidate =
                b.count >= 2 && b.note == 0 && !b.mono && first.hasLetter && first.chars <= 120 && !first.pageRef &&
                !first.startsLower && first.size < kHeadingSize * body &&
                (b.count >= 3 || infos[b.first + 1].endsTerminal) &&
                !(first.flags & (kLineTable | kLineFormula | kLineTableAnchor | kLineImage | kLineInvisible));
            if (candidate && !(b.count <= 4 && titled(b.first, b.count)) && titled(b.first, 1)) {
                if (split.empty()) split.assign(raws.begin(), raws.begin() + static_cast<std::ptrdiff_t>(r));
                split.push_back(block(b.first, 1, b));
                split.push_back(block(b.first + 1, b.count - 1, b));
            } else if (!split.empty()) {
                split.push_back(b);
            }
        }
        if (!split.empty()) raws.swap(split);
    }

    // Classification.
    struct Classified {
        BlockKind kind = BlockKind::Paragraph;
        int level = 0;      // outline level, or 0
        int styleKey = -1;  // heading style above body size: size key * 2 + bold
        bool boldHeading = false;
        bool runIn = false;  // a bold heading its paragraph runs on from, on the same baseline
        bool divisionLabel = false;  // a division's label made a heading over its title
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
    // Card titles (a slide's or a brochure's boxes): short blocks of one or two lines set larger or
    // in bold side by side on one baseline, most of them right over a column of text, title their
    // columns, where a table's header row over its cells (rowPeer) heads nothing. A block stands
    // over a column when the nearest block below it on its page whose left edge is within one
    // body size of its own starts within 2.5 times its size below its first line and holds two
    // lines or 40 characters, in any size. Fails closed (no title) once kCardChecks runs out.
    std::vector<uint8_t> cardTitle(raws.size(), 0);
    {
        // The raws and the infos of page p: [rawFrom[p], rawFrom[p + 1]), [infoFrom[p], infoFrom[p + 1]).
        std::vector<uint32_t> rawFrom(pages.size() + 1, 0), infoFrom(pages.size() + 1, 0);
        for (const RawBlock& b : raws) rawFrom[infos[b.first].page + 1]++;
        for (const Info& in : infos) infoFrom[in.page + 1]++;
        for (size_t p = 0; p < pages.size(); ++p) rawFrom[p + 1] += rawFrom[p], infoFrom[p + 1] += infoFrom[p];
        // The raw block each info starts, or kNoRaw.
        constexpr uint32_t kNoRaw = UINT32_MAX;
        std::vector<uint32_t> rawOf(infos.size(), kNoRaw);
        for (size_t r = 0; r < raws.size(); ++r) rawOf[raws[r].first] = static_cast<uint32_t>(r);
        uint64_t budget = kCardChecks;
        // A look over t's page for the column below it takes the page's raw blocks from the budget:
        // false once the budget has run out (no column then).
        const auto takeLook = [&](const Info& t) {
            const uint32_t from = rawFrom[t.page], to = rawFrom[t.page + 1];
            if (to - from > budget) return false;
            budget -= to - from;
            return true;
        };
        // What overColumn reads of each raw block's first line, packed, made at the first look: its
        // baseline, top, left edge (or its body's), direction, and whether it may be a column at all
        // (no anchor, no note).
        struct Top {
            float baseline, vt, left;
            uint8_t dir;
            bool usable;
        };
        std::vector<Top> tops;
        const auto overColumn = [&](const Info& t) {
            if (tops.empty()) {
                tops.resize(raws.size());
                for (size_t q = 0; q < raws.size(); ++q) {
                    const Info& top = infos[raws[q].first];
                    tops[q] = Top{top.baseline, top.vt, Fmin(top.u0, raws[q].bodyU0), top.dir,
                                  !(top.flags & kLineAnchor) && raws[q].note == 0};
                }
            }
            const uint32_t from = rawFrom[t.page], to = rawFrom[t.page + 1];
            size_t below = to;
            for (size_t q = from; q < to; ++q) {
                const Top& top = tops[q];
                if (top.dir != t.dir || !top.usable || !(top.baseline < t.vb)) continue;
                if (std::fabs(top.left - t.u0) > body) continue;
                if (below == to || top.vt > tops[below].vt) below = q;
            }
            if (below == to || t.vb - tops[below].vt > 2.5f * t.size) return false;
            const RawBlock& c = raws[below];
            uint32_t lines = 0, chars = 0;
            for (uint32_t i = c.first; i < c.first + c.count && i < c.first + 64; ++i) lines++, chars += infos[i].chars;
            return lines >= 2 || chars >= 40;
        };
        const auto titleLike = [&](uint32_t r) {
            const RawBlock& b = raws[r];
            const Info& a = infos[b.first];
            if (b.count > 2 || !a.rowPeer || a.chars > 40 || (a.flags & kLineAnchor) ||
                !(a.size >= 1.1f * body || (a.flags & kLineAllBold)))
                return false;
            uint32_t chars = 0;
            for (uint32_t i = b.first; i < b.first + b.count; ++i) chars += infos[i].chars;
            return chars <= 80;
        };
        // The infos of a block's titles whose look fitted the budget, looked at only when the block
        // has a peer.
        std::vector<uint32_t> looks;
        for (size_t r = 0; r < raws.size(); ++r) {
            const RawBlock& b = raws[r];
            const Info& a = infos[b.first];
            if (!titleLike(static_cast<uint32_t>(r))) continue;
            const uint32_t from = infoFrom[a.page], to = infoFrom[a.page + 1];
            if (to - from > budget) continue;
            budget -= to - from;
            uint32_t titles = 1;
            looks.clear();
            if (takeLook(a)) looks.push_back(b.first);
            // Its peers: the lines rowPeer paired it with, each starting a title-like block.
            for (uint32_t i = from; i < to; ++i) {
                const Info& o = infos[i];
                if (i == b.first || !o.rowPeer || o.dir != a.dir || rawOf[i] == kNoRaw ||
                    std::fabs(o.baseline - a.baseline) > 0.3f * Fmax(a.size, o.size) ||
                    std::fabs(o.size - a.size) > 0.03f * Fmax(a.size, o.size) || o.chars > 60 || o.numbering ||
                    ((o.flags ^ a.flags) & kLineAllBold) || (o.flags & kLineAnchor) || !(o.u0 > a.u1 || o.u1 < a.u0) ||
                    !titleLike(rawOf[i]))
                    continue;
                titles++;
                if (takeLook(o)) looks.push_back(i);
            }
            if (titles < 2) continue;
            uint32_t over = 0;
            for (const uint32_t i : looks) over += overColumn(infos[i]) ? 1 : 0;
            cardTitle[r] = over >= 2 && 2 * over >= titles;
        }
    }
    // Block r (upright) set centred in its column: the column is the extent of the nearest block
    // of two lines or more before or after it on its page (within kOcrNeighbours blocks) whose
    // extent holds it; every line of r is clear of both of the column's edges by kOcrIndent ems,
    // centred on the column within kOcrAxis ems, and alone on its baseline in the column (a line
    // of the other column may share it; a table row's other cells may not). The lines looked at
    // for that are the kOcrRowWindow before and after the block in reading order, where a row's
    // other lines stand.
    const auto ocrCentred = [&](size_t r) {
        const RawBlock& b = raws[r];
        const Info& first = infos[b.first];
        if (first.dir != 0) return false;
        const float em = body;
        float u0 = INFINITY, u1 = -INFINITY;
        for (uint32_t i = b.first; i < b.first + b.count; ++i) u0 = Fmin(u0, infos[i].u0), u1 = Fmax(u1, infos[i].u1);
        const auto centredIn = [&](size_t q) {
            const RawBlock& c = raws[q];
            const Info& top = infos[c.first];
            if (c.count < 2 || top.page != first.page || top.dir != 0) return false;
            float c0 = INFINITY, c1 = -INFINITY;
            for (uint32_t i = c.first; i < c.first + c.count; ++i) c0 = Fmin(c0, infos[i].u0), c1 = Fmax(c1, infos[i].u1);
            if (u0 < c0 + kOcrIndent * em || u1 > c1 - kOcrIndent * em) return false;
            for (uint32_t i = b.first; i < b.first + b.count; ++i)
                if (std::fabs((infos[i].u0 + infos[i].u1) - (c0 + c1)) > 2 * kOcrAxis * em) return false;
            const size_t from = b.first > kOcrRowWindow ? b.first - kOcrRowWindow : 0;
            const size_t to = std::min(infos.size(), size_t(b.first) + b.count + kOcrRowWindow);
            for (size_t o = from; o < to; ++o) {
                const Info& in = infos[o];
                if ((o >= b.first && o < b.first + b.count) || in.page != first.page || in.dir != 0 || in.u1 <= c0 ||
                    in.u0 >= c1 || (in.numbering && in.u1 <= u0))
                    continue;
                for (uint32_t i = b.first; i < b.first + b.count; ++i)
                    if (std::fabs(in.baseline - infos[i].baseline) <= 0.3f * infos[i].size) return false;
            }
            return true;
        };
        for (size_t d = 1; d <= kOcrNeighbours; ++d) {
            if (r >= d && centredIn(r - d)) return true;
            if (r + d < raws.size() && centredIn(r + d)) return true;
        }
        return false;
    };
    // Whether the sizes of page p's OCR layer are its type's, so that a heading set larger reads
    // larger: on at least kOcrSizedShare of its upright invisible lines of four glyphs or more,
    // every glyph within kOcrSizeSpread of its line's size. A layer whose sizes are fitted to its
    // word boxes one by one fails. Measured once a page.
    std::vector<uint8_t> ocrSized(pages.size(), 0);  // 0 not measured, 1 fitted, 2 typeset
    const auto ocrSizes = [&](uint32_t p) {
        if (ocrSized[p] == 0) {
            const PageData& page = pages[p];
            uint32_t lines = 0, uniform = 0;
            for (const Line& l : page.lines) {
                if (!(l.flags & kLineInvisible) || l.dir != 0 || l.count < 4) continue;
                lines++;
                bool same = true;
                for (uint32_t k = 0; k < l.count && same; ++k) {
                    const Glyph& g = page.glyphs[page.lineGlyphs[l.first + k]];
                    same = (g.flags & kGlyphSpace) || std::fabs(g.size - l.size) <= kOcrSizeSpread * l.size;
                }
                uniform += same ? 1 : 0;
            }
            ocrSized[p] = lines > 0 && float(uniform) >= kOcrSizedShare * float(lines) ? 2 : 1;
        }
        return ocrSized[p] == 2;
    };
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
        if (b.take != 0) {
            k.kind = BlockKind::Heading;
            k.boldHeading = true;
            k.chars = b.take;
            continue;
        }
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
        // Kept page furniture (kLineFurniture) stays a paragraph: a running head titles nothing.
        if (first.flags & (kLineTable | kLineFormula | kLineFurniture)) continue;
        if (b.mono && !monoDocument) {
            k.kind = BlockKind::Code;
            continue;
        }
        // The outline. A table-of-contents entry repeats a title and heads nothing: a line with
        // its page number far to its right next to another such line set alone (or after its own
        // number set apart; without one it is a heading beside a number in the other column), or
        // one ending in a row of leader dots.
        const auto contentsEntry = [&]() {
            if (!first.pageRef) return false;
            for (size_t q = r >= 2 ? r - 2 : 0; q < raws.size() && q <= r + 2; ++q) {
                const Info& near = infos[raws[q].first];
                const bool alone = raws[q].count == 1 || (raws[q].count == 2 && near.numbering);
                if (q != r && alone && near.pageRef && !near.endsTerminal && near.page == first.page)
                    return true;
            }
            return false;
        };
        if (!titles.empty() && b.count <= 4 && chars <= 300 && letter && !contentsEntry()) {
            text.clear();
            key.clear();
            bool fits = true;
            for (uint32_t i = b.first; i < b.first + b.count && fits; ++i) {
                const PageData& page = pages[infos[i].page];
                LineText(page, page.lines[infos[i].line], line);
                if (!text.empty()) text.push_back(' ');
                text += line;
                WriteMatchKey(line, key, key.size());
                fits = key.size() <= keyLimit;
            }
            for (int strip = !fits || EndsInLeader(text) ? 2 : 0; strip < 2 && k.level == 0; ++strip) {
                if (strip == 1) key.erase(0, NumberingKeyBytes(text));
                if (key.size() > maxTitleKey) continue;
                auto it = titles.find(key);
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
        // Text rotated to read upwards (an arXiv stamp in the margin) or upside down heads nothing,
        // nor does text that goes on a sentence in lowercase ("and", "yields", "with" between two
        // formulas), unless its first word is a hyphenated or numbered term ("p-adic Bounds").
        bool lowerLead = first.startsLower;
        if (lowerLead) {
            const PageData& page = pages[first.page];
            const Line& l = page.lines[first.line];
            for (uint32_t k = first.skip; k < l.count && k < first.skip + 24; ++k) {
                const Glyph& g = page.glyphs[page.lineGlyphs[l.first + k]];
                if (k > first.skip && (g.flags & kGlyphSpaceBefore)) break;
                const std::string_view t = page.GlyphText(g);
                if (!t.empty() && (t[0] == '-' || (t[0] >= '0' && t[0] <= '9'))) lowerLead = false;
            }
        }
        // A part title among the entries of a list of contents (an entry within two blocks before it
        // and after it on its page) is an entry too, not a heading.
        const auto amongContents = [&]() {
            bool before = false, after = false;
            for (size_t q = r >= 2 ? r - 2 : 0; q < r; ++q)
                before = before || (infos[raws[q].first].pageRef && infos[raws[q].first].page == first.page);
            for (size_t q = r + 1; q < raws.size() && q <= r + 2; ++q)
                after = after || (infos[raws[q].first].pageRef && infos[raws[q].first].page == first.page);
            return before && after;
        };
        const bool shortBlock = b.count <= 3 && chars <= 160 && letter && (!first.rowPeer || cardTitle[r]) &&
                                !first.pageRef && first.dir != 1 && first.dir != 2 && !lowerLead && !amongContents();
        const Info& last = infos[b.first + b.count - 1];
        const bool unpunctuated = !last.endsTerminal && last.lastCp != ',';
        // Captions are set apart like headings but title no section; display math set large (an
        // integral sign, a fraction's numerator), a figure set large ("20%↑"), an author list
        // (names with raised affiliation marks: after a word of two letters or more, not after a
        // symbol as in "L²(Rᵈ)"), an affiliation (its raised mark first, "¹University of …"), a
        // date and a copyright or licence notice ("CC0, 2022": RightsLine) title nothing either.
        const auto caption = [&]() {
            const PageData& page = pages[first.page];
            LineText(page, page.lines[first.line], line);
            if (IsCaption(line)) return true;
            text = line;
            {
                const Line& l = page.lines[first.line];
                if (l.count >= 2 && IsSuperscript(page.glyphs[page.lineGlyphs[l.first]], l) &&
                    !IsSuperscript(page.glyphs[page.lineGlyphs[l.first + l.count - 1]], l))
                    return true;
            }
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
            return marks >= 2 || NoTitle(text) || DateLine(text) || RightsLine(text);
        };
        // Slightly larger text (from 7 % up) is a heading only as one or two short unpunctuated lines.
        const bool larger = size >= body * kHeadingSize ||
                            (size >= body * 1.07f && b.count <= 2 && chars <= 80 && unpunctuated);
        // A running head left on a page (the top line alone above a wide gap, in the body face
        // and not much larger) titles nothing.
        const bool runningHead = first.topBand && b.count == 1 && !(first.flags & kLineAllBold) &&
                                 size <= 1.2f * body && first.gapBelow >= 2 * body;
        // Invisible text (an OCR layer) has no faces: no bold, italic or fixed pitch tells its
        // headings. What its geometry still shows is a block set centred in its column, clear of
        // both of the column's edges: a heading, unless it only says that a table goes on
        // ("-continued", "(Continued)", "52 -continued"), labels a table or figure as recognised
        // text spells it ("TABLE28", "TABLE IO-continued"), or titles the table or figure whose
        // label stands right above it. Where the layer is set in its type's sizes (ocrSizes, not
        // fitted to its word boxes one by one), a short block set larger is a heading as in
        // visible text.
        if (first.flags & kLineInvisible) {
            if (k.kind != BlockKind::Heading && b.count <= 6 && chars <= 300 && letter &&
                !lowerLead && unpunctuated && !runningHead && !amongContents() && ocrCentred(r) && !caption()) {
                MatchKey(text, scratch, false);
                size_t digits = 0;
                while (digits < scratch.size() && scratch[digits] >= '0' && scratch[digits] <= '9') ++digits;
                bool heading = std::string_view(scratch).substr(digits) != "continued" && !OcrLabel(scratch);
                if (heading && r > 0 && infos[raws[r - 1].first].page == first.page) {
                    const Info& above = infos[raws[r - 1].first];
                    LineText(pages[above.page], pages[above.page].lines[above.line], line);
                    MatchKey(line, scratch, false);
                    heading = !OcrLabel(scratch);
                }
                if (heading) {
                    k.kind = BlockKind::Heading;
                    k.boldHeading = true;
                    continue;
                }
            }
            if (k.kind != BlockKind::Heading && shortBlock && larger && !runningHead && last.lastCp != ',' &&
                ocrSizes(first.page) && !caption()) {
                k.kind = BlockKind::Heading;
                k.styleKey = static_cast<int>(std::lround(size * 2)) * 2;
                continue;
            }
            if (k.kind != BlockKind::Heading && first.listStart()) k.kind = BlockKind::ListItem;
            continue;
        }
        if (shortBlock && larger && !runningHead && last.lastCp != ',' && !caption()) {
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
        // A numbered section title set bold a size below the text (REVTeX's "I. INTRODUCTION" and
        // "A. Model Hamiltonian" at nine tenths of the body, a tender's "1.2 SCOPE"): one or two
        // bold lines from kSmallHeadingSize times the body size up, opening with a section number
        // (NumberingPrefix), without a sentence's or a clause's end, over text on its page that is
        // neither bold, nor set as large as a heading, nor going on in lower case. A table's bold
        // header row in that size names no section ("2019 2018", "31 December").
        if (shortBlock && b.bold && !boldDocument && b.count <= 2 && chars <= 100 && size >= body * kSmallHeadingSize &&
            unpunctuated && (!first.listStart() || enumerated) && r + 1 < raws.size()) {
            const RawBlock& n = raws[r + 1];
            const Info& below = infos[n.first];
            if (below.page == first.page && below.dir == first.dir && !n.bold && !n.mono && n.note == 0 &&
                !below.startsLower && below.size < kHeadingSize * body &&
                !(below.flags & (kLineTable | kLineFormula | kLineAnchor | kLineFurniture | kLineInvisible))) {
                const PageData& page = pages[first.page];
                LineText(page, page.lines[first.line], line);
                if (NumberingPrefix(line) > 0 && !caption()) {
                    k.kind = BlockKind::Heading;
                    k.boldHeading = true;
                    continue;
                }
            }
        }
        // Its italic kin, numbered: one line of body size in italics opening with a section number
        // of two levels or more ("2.1. Spatial discretization"), without a sentence's end, over
        // upright text on its page, in lower case after its capitals: a line in italic capitals is
        // a running head's ("70.5. EXAMINING THE DUMP FUNCTIONS CHAPTER 70. PERLGUTS").
        const auto lowerCase = [&]() {
            const PageData& page = pages[first.page];
            LineText(page, page.lines[first.line], line);
            return HasLowerAscii(line);
        };
        if (shortBlock && b.count == 1 && first.sectionDepth >= 2 && (first.flags & kLineAllItalic) && !out.italicBody &&
            size >= body * 0.915f && titleEnd && r + 1 < raws.size() && infos[raws[r + 1].first].page == first.page &&
            !(infos[raws[r + 1].first].flags & kLineAllItalic) && !caption() && lowerCase()) {
            k.kind = BlockKind::Heading;
            k.boldHeading = true;
            continue;
        }
        // An italic title line its paragraph would have run on from (RawBlock::italicTitle) heads
        // it, about the body's size, in a document whose text is not set in italics.
        if (b.italicTitle && shortBlock && b.count == 1 && !out.italicBody && size >= body * 0.915f && !caption()) {
            k.kind = BlockKind::Heading;
            k.boldHeading = true;
            continue;
        }
        // A paper's standard unnumbered section named alone on its line ("Abstract", "References",
        // "7. ACKNOWLEDGMENTS") heads it in any style and size: no sentence is set so.
        if (shortBlock && b.count == 1 && chars <= 32 && first.dir == 0) {
            const PageData& page = pages[first.page];
            LineText(page, page.lines[first.line], line);
            if (StandardSection(line, scratch)) {
                k.kind = BlockKind::Heading;
                k.boldHeading = true;
                continue;
            }
        }
        // A numbered section title set centred over the paragraph under it, about the body's size
        // (IEEE's "II. SYSTEM MODEL" in small capitals, amsart's "5. Conclusion"): one line opening
        // with a section number ("5. ", "IV. ": OpensWithSectionNumber) and a title without a
        // sentence's end or leader dots, its middle within an em of the middle of the paragraph of
        // two lines or more below it on its page, half an em clear of both its edges; that
        // paragraph does not go on in lower case (a sentence's first line set apart: a patent's
        // claim, an indented item). A list's items stand at its left edge. The other column's text
        // beside it (a row peer) does not make it a table's cell.
        if (b.count == 1 && chars <= 160 && letter && !first.pageRef && first.dir != 1 && first.dir != 2 && !lowerLead &&
            !b.bold && !first.markerAlone && unpunctuated && size >= 0.75f * body && size <= 1.1f * body &&
            r + 1 < raws.size()) {
            const RawBlock& n = raws[r + 1];
            const Info& below = infos[n.first];
            if (below.page == first.page && below.dir == first.dir && n.count >= 2 && !below.listStart() &&
                !below.startsLower && std::fabs((first.u0 + first.u1) - (n.bodyU0 + n.right)) <= 2 * body &&
                first.u0 >= n.bodyU0 + 0.5f * body && first.u1 <= n.right - 0.5f * body && !amongContents()) {
                const PageData& page = pages[first.page];
                LineText(page, page.lines[first.line], line);
                if (OpensWithSectionNumber(line) && !EndsInLeader(line) && !caption()) {
                    k.kind = BlockKind::Heading;
                    k.boldHeading = true;
                    continue;
                }
            }
        }
        if (first.listStart()) k.kind = BlockKind::ListItem;
    }

    // A heading style that holds a large share of the text is the body of a document whose most
    // common size is something else (footnotes, captions): its blocks are not headings. A style of
    // size counts its paragraphs too: running text set larger than a document's most common size
    // (a long list of authors or the figures' labels in a smaller one) leaves its short blocks
    // (references, a lone line) in that style, and they head nothing. A style of a single block is
    // still a heading when the text of the tables beside it outweighs it (the title over a page
    // that is all table). Scanned text (the add-on's OCR layer) has no outline to name the section
    // titles of a document that is mostly tables: there a style of title size (kTitleSize) is
    // weighed against the tables' text too, whatever its number of blocks.
    {
        std::unordered_map<int, uint64_t> styleChars, textChars;
        std::unordered_map<int, uint32_t> styleBlocks;
        const auto sizeStyle = [](const Classified& k) {
            return static_cast<int>(std::lround(k.size * 2)) * 2 + (k.bold ? 1 : 0);
        };
        for (const Classified& k : kinds) {
            if (k.kind == BlockKind::Heading && k.level == 0) styleChars[k.styleKey] += k.chars, styleBlocks[k.styleKey]++;
            if (k.kind == BlockKind::Paragraph || k.kind == BlockKind::ListItem || (k.kind == BlockKind::Heading && k.level == 0))
                textChars[sizeStyle(k)] += k.chars;
        }
        const auto scanned = [&](size_t r) {
            const Info& in = infos[raws[r].first];
            const Line& l = pages[in.page].lines[in.line];
            return l.count > 0 && (pages[in.page].glyphs[pages[in.page].lineGlyphs[l.first]].flags & kGlyphOcr) != 0;
        };
        for (size_t r = 0; r < raws.size(); ++r) {
            Classified& k = kinds[r];
            if (k.kind != BlockKind::Heading || k.level > 0) continue;
            uint64_t chars = styleChars[k.styleKey];
            if (k.styleKey >= 0) chars = std::max(chars, textChars[sizeStyle(k)]);
            if (chars * 10 <= totalChars * 3) continue;
            // A cover page's title: the only block of its style, set at title size on the first
            // page. A document of a few lines (a cover, a flyer, a slide) gives the share no meaning.
            if (styleBlocks[k.styleKey] == 1 && k.size >= kTitleSize * body && infos[raws[r].first].page == 0) continue;
            const bool weighed = styleBlocks[k.styleKey] == 1 || (k.size >= kTitleSize * body && scanned(r));
            if (weighed && chars * 10 <= (totalChars + cellChars) * 3) continue;
            k.kind = infos[raws[r].first].listStart() ? BlockKind::ListItem : BlockKind::Paragraph;
        }
    }

    // A line that holds an e-mail address or a web address (an author's address under a title, a
    // repository's address set on every page) says where to write or to look, and heads nothing.
    for (size_t r = 0; r < raws.size(); ++r) {
        Classified& k = kinds[r];
        if (k.kind != BlockKind::Heading || k.level > 0 || raws[r].take != 0) continue;
        bool address = false;
        for (uint32_t i = raws[r].first; i < raws[r].first + raws[r].count && !address; ++i) {
            const PageData& page = pages[infos[i].page];
            LineText(page, page.lines[infos[i].line], line);
            address = HoldsAddress(line);
        }
        if (address) k.kind = infos[raws[r].first].listStart() ? BlockKind::ListItem : BlockKind::Paragraph;
    }

    // A lead-in ending in a colon, set no larger than the text, inside a numbered list whose numbers
    // run on across it ("19." above it, "20." below: a lab protocol's "Digest Prep (switch to the
    // 20-μL pipette):") introduces the next step of that list and opens no section. A title without
    // the colon there (a statute's "Planning permission" between its articles 6 and 7) stays a
    // heading. The items are numbered ones (Marker::Number) on its page, the one above within three
    // blocks before it, the one below within two after it.
    for (size_t r = 1; r + 1 < raws.size(); ++r) {
        Classified& k = kinds[r];
        const Info& h = infos[raws[r].first];
        if (k.kind != BlockKind::Heading || k.level > 0 || raws[r].take != 0 || h.listStart() ||
            !(k.size < kHeadingSize * body) || infos[raws[r].first + raws[r].count - 1].lastCp != ':')
            continue;
        const auto item = [&](size_t q) {
            const Info& in = infos[raws[q].first];
            return in.page == h.page && kinds[q].kind == BlockKind::ListItem && in.marker == Marker::Number;
        };
        size_t above = r, below = r;
        for (size_t q = r; q-- > 0 && q + 3 >= r && above == r;)
            if (item(q)) above = q;
        for (size_t q = r + 1; q < raws.size() && q <= r + 2 && below == r; ++q)
            if (item(q)) below = q;
        if (above != r && below != r && infos[raws[below].first].value == infos[raws[above].first].value + 1)
            k.kind = BlockKind::Paragraph;
    }

    // A kicker: a short line set above a page's title in a smaller size ("AI Pack" over a slide's
    // title) names what the title belongs to and heads nothing. It is a heading of one line, the
    // page's topmost text (an image may stand above it), directly over the next block, a heading
    // of kKickerSize times its size at least within three of its sizes below it, the two
    // overlapping across half the narrower. (kordoc's heading-demote.ts demotes the same.)
    for (size_t r = 0; r + 1 < raws.size(); ++r) {
        Classified& k = kinds[r];
        const Classified& n = kinds[r + 1];
        if (k.kind != BlockKind::Heading || k.level > 0 || n.kind != BlockKind::Heading || raws[r].count != 1) continue;
        const Info& a = infos[raws[r].first];
        const Info& b = infos[raws[r + 1].first];
        if (b.page != a.page || a.dir != 0 || b.dir != 0 || !(n.size >= kKickerSize * k.size)) continue;
        if (!(a.baseline > b.baseline) || a.vb - b.vt > 3 * k.size) continue;
        float u0 = INFINITY, u1 = -INFINITY;
        for (uint32_t i = raws[r + 1].first; i < raws[r + 1].first + raws[r + 1].count; ++i)
            u0 = Fmin(u0, infos[i].u0), u1 = Fmax(u1, infos[i].u1);
        if (Fmin(a.u1, u1) - Fmax(a.u0, u0) < 0.5f * Fmin(a.u1 - a.u0, u1 - u0)) continue;
        // The page's lines are the infos around the kicker's of its page.
        uint32_t from = raws[r].first, to = raws[r].first;
        while (from > 0 && infos[from - 1].page == a.page) --from;
        while (to < infos.size() && infos[to].page == a.page) ++to;
        bool topmost = true;
        for (uint32_t i = from; i < to && topmost; ++i)
            topmost = infos[i].dir != 0 || (infos[i].flags & kLineImage) || !(infos[i].vb > a.vt);
        if (topmost) k.kind = a.listStart() ? BlockKind::ListItem : BlockKind::Paragraph;
    }

    // A page's lead line: a line in the body's size and face that opens its page, set apart from
    // the paragraph under it by an em and by more than that paragraph's own leading, heads the
    // page when nothing else on it does ("Print vs. Digital" over its text). It starts with a
    // capital or a digit and ends without a sentence's or a clause's punctuation; the paragraph
    // under it has two lines or more and starts on its left edge.
    for (size_t r = 0; r + 1 < raws.size(); ++r) {
        const RawBlock& b = raws[r];
        const Info& a = infos[b.first];
        if (r > 0 && infos[raws[r - 1].first].page == a.page) continue;  // not the page's first block
        if (kinds[r].kind != BlockKind::Paragraph || b.count != 1 || b.note != 0 || b.mono || a.dir != 0 ||
            (a.flags & (kLineAnchor | kLineTable | kLineFormula | kLineInvisible | kLineAllBold | kLineFurniture)) ||
            a.chars < 5 || a.chars > 80 || std::fabs(a.size - body) > 0.05f * body || a.lastCp == '.' || a.lastCp == ',' ||
            a.lastCp == ';' || a.lastCp == ':')
            continue;
        const RawBlock& c = raws[r + 1];
        const Info& n = infos[c.first];
        if (n.page != a.page || c.count < 2 || n.dir != 0 || (n.flags & kLineAnchor) ||
            std::fabs(Fmin(n.u0, c.bodyU0) - a.u0) > body || a.vb - n.vt < body ||
            a.baseline - n.baseline < 1.8f * (n.baseline - infos[c.first + 1].baseline))
            continue;
        bool headed = false;
        for (size_t q = r + 1; q < raws.size() && infos[raws[q].first].page == a.page && !headed; ++q)
            headed = kinds[q].kind == BlockKind::Heading;
        if (headed) continue;
        const PageData& page = pages[a.page];
        LineText(page, page.lines[a.line], line);
        const uint32_t cp = FirstCodePoint(line);
        if (!((cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7))) continue;
        kinds[r].kind = BlockKind::Heading;
    }

    // A title block (§6.1): under the title on the first page, blocks centred under it in a
    // smaller size name the authors, their affiliations and the date, up to the first section
    // ("Abstract", "1 Introduction") or the first body paragraph. They head nothing. In a paper's
    // front matter (an abstract, its heading or its label, follows the title on its page) the
    // block may also be set flush left (JMLR, PoS). A body paragraph is justified across its
    // width, or (outside a paper's front matter) not centred under the title: a centred block of
    // affiliations as wide as the text, its lines of many widths, is none.
    if (!raws.empty()) {
        const uint32_t page = infos[raws[0].first].page;
        const auto extent = [&](size_t r, float& u0, float& u1) {
            u0 = INFINITY, u1 = -INFINITY;
            for (uint32_t i = raws[r].first; i < raws[r].first + raws[r].count; ++i)
                u0 = Fmin(u0, infos[i].u0), u1 = Fmax(u1, infos[i].u1);
        };
        // The title: the page's first heading, set at least as large as every heading before the
        // first section ("Abstract", "1 Introduction": a paper may set its sections larger).
        size_t title = raws.size();
        float titleSize = 0;
        for (size_t r = 0; r < raws.size() && infos[raws[r].first].page == page; ++r) {
            if (kinds[r].kind != BlockKind::Heading || kinds[r].level > 0) continue;
            if (title != raws.size()) {
                const Info& in = infos[raws[r].first];
                LineText(pages[in.page], pages[in.page].lines[in.line], line);
                if (FrontMatter(line) || SectionDepth(line) > 0) break;
            }
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
            bool abstractAfter = false;
            for (size_t r = title + 1; r < raws.size() && infos[raws[r].first].page == page && !abstractAfter; ++r) {
                const Info& in = infos[raws[r].first];
                abstractAfter = raws[r].take != 0;
                // Only a line whose text starts with an 'a' is read whole.
                if (!abstractAfter && (FirstTextByte(pages[in.page], pages[in.page].lines[in.line]) | 0x20) == 'a') {
                    LineText(pages[in.page], pages[in.page].lines[in.line], line);
                    abstractAfter = FirstWordIs(line, "abstract");
                }
            }
            // Every line of block r but its last runs across the block's width [u0, u1].
            const auto justified = [&](size_t r, float u0, float u1) {
                for (uint32_t i = raws[r].first; i + 1 < raws[r].first + raws[r].count; ++i)
                    if (infos[i].u0 > u0 + 0.5f * body || infos[i].u1 < u1 - 0.5f * body) return false;
                return true;
            };
            size_t titleEnd = title;  // the title's last block (a title may be set in several)
            for (size_t r = title + 1; r < raws.size() && infos[raws[r].first].page == page; ++r) {
                const Info& first = infos[raws[r].first];
                float u0 = 0, u1 = 0;
                extent(r, u0, u1);
                const bool axis = std::fabs((u0 + u1) - (t0 + t1)) <= 4 * body && first.dir == 0;
                const bool centred = axis && u0 > left + body;
                if (kinds[r].kind != BlockKind::Heading) {
                    if (raws[r].count >= 3 && (justified(r, u0, u1) || (!axis && !abstractAfter))) break;
                    continue;
                }
                // A section starts with its number ("1 Introduction"); a date ends with its year.
                LineText(pages[first.page], pages[first.page].lines[first.line], line);
                const bool numbered = !line.empty() && line[0] >= '0' && line[0] <= '9' &&
                                      !(line.back() >= '0' && line.back() <= '9');
                // Set like the title or larger, it is a title line of its own: the title's next
                // block when it follows it on its axis or its left edge, else a title of its own.
                const bool titleStyle = kinds[r].size > kinds[title].size + 0.25f ||
                                        (kinds[r].size >= kinds[title].size - 0.25f && kinds[r].bold == kinds[title].bold);
                if (titleStyle && r == titleEnd + 1 && (axis || std::fabs(u0 - t0) <= body) && !numbered &&
                    !FrontMatter(line)) {
                    titleEnd = r;
                    continue;
                }
                if (kinds[r].level > 0 || (!centred && !abstractAfter) || titleStyle || first.listStart() ||
                    first.numbering || numbered || FrontMatter(line))
                    break;
                kinds[r].kind = BlockKind::Paragraph;
            }
        }
    }

    // A chapter's number set alone above its title ("2" over "The Lost Homeland"): one or two
    // digits on a line of their own at 2.5 times the body size or more, right over a heading of
    // title size (kTitleSize times the body) on its page, the two within twice the number's size,
    // is a heading of its own (kordoc's isChapterNumber takes the same).
    for (size_t r = 0; r + 1 < raws.size(); ++r) {
        Classified& k = kinds[r];
        const Classified& n = kinds[r + 1];
        if (k.kind != BlockKind::Paragraph || raws[r].count != 1 || n.kind != BlockKind::Heading) continue;
        const Info& a = infos[raws[r].first];
        const Info& b = infos[raws[r + 1].first];
        if (b.page != a.page || a.dir != 0 || b.dir != 0 || (a.flags & kLineFurniture) || !(a.size >= 2.5f * body) ||
            !(n.size >= kTitleSize * body) || !(a.baseline > b.baseline) || a.vb - b.vt > 2 * a.size)
            continue;
        const PageData& page = pages[a.page];
        LineText(page, page.lines[a.line], line);
        size_t digits = 0;
        while (digits < line.size() && line[digits] >= '0' && line[digits] <= '9') ++digits;
        if (digits < 1 || digits > 2 || digits != line.size()) continue;
        k.kind = BlockKind::Heading;
        k.styleKey = static_cast<int>(std::lround(k.size * 2)) * 2 + (k.bold ? 1 : 0);
    }

    // A division's label set alone above its title ("CHAPTER 1." over "CALIFORNIA", "Part II"
    // over "Methods"): a DivisionLabel line of its own right over a heading on its page, the two
    // within 2.5 times the label's size, is a heading of its own in any size and face, at its
    // title's level (set with the levels below). It names the division its title heads, so it is
    // no kicker either.
    for (size_t r = 0; r + 1 < raws.size(); ++r) {
        Classified& k = kinds[r];
        const Classified& n = kinds[r + 1];
        if (k.kind != BlockKind::Paragraph || raws[r].count != 1 || n.kind != BlockKind::Heading) continue;
        const Info& a = infos[raws[r].first];
        const Info& b = infos[raws[r + 1].first];
        if (b.page != a.page || a.dir != 0 || b.dir != 0 || a.chars > 16 ||
            (a.flags & (kLineFurniture | kLineAnchor | kLineTable | kLineFormula)) || !(a.baseline > b.baseline) ||
            a.vb - b.vt > 2.5f * a.size)
            continue;
        const PageData& page = pages[a.page];
        LineText(page, page.lines[a.line], line);
        if (!DivisionLabel(line)) continue;
        k.kind = BlockKind::Heading;
        k.styleKey = -1;
        k.divisionLabel = true;
    }

    // An italic subtitle under a heading ("1. Shipping as a vector" over "List of ports in
    // Appendix 3" in italics) completes it: a heading right under another on its page, within 1.5
    // times the upper one's size, its lines all italic where the upper one's are not, set no larger
    // (a quarter point aside) and neither numbered nor enumerated, joins it as one heading at the
    // upper one's level. So do the stacked lines of a display title ("SEAGRASS" over "IN SOUTH
    // FLORIDA"): two headings of one line each, at least 1.25 times the body size, centred on each
    // other within half the smaller size, touching within 0.3 of it, neither more than 1.45 times
    // the other's size and neither numbered (kordoc's mergeStackedHeadingLines joins the same).
    {
        size_t kept = 0;
        for (size_t r = 0; r < raws.size(); ++r) {
            if (kept > 0) {
                RawBlock& h = raws[kept - 1];
                Classified& hk = kinds[kept - 1];
                const RawBlock& s = raws[r];
                const Classified& sk = kinds[r];
                const Info& above = infos[h.first + h.count - 1];
                const Info& n = infos[s.first];
                bool join = hk.kind == BlockKind::Heading && sk.kind == BlockKind::Heading && !hk.runIn && !sk.runIn &&
                            h.take == 0 && s.take == 0 && h.first + h.count == s.first && n.page == above.page &&
                            n.dir == above.dir && n.baseline < above.baseline && above.vb - n.vt <= 1.5f * hk.size &&
                            sk.size <= hk.size + 0.25f && !n.numbering && n.marker != Marker::Number &&
                            n.marker != Marker::Roman && n.marker != Marker::Letter;
                bool upperItalic = true;
                for (uint32_t i = h.first; i < h.first + h.count && join && upperItalic; ++i)
                    upperItalic = (infos[i].flags & kLineAllItalic) != 0;
                join = join && !upperItalic;
                for (uint32_t i = s.first; i < s.first + s.count && join; ++i) join = (infos[i].flags & kLineAllItalic) != 0;
                const float small = Fmin(hk.size, sk.size), large = Fmax(hk.size, sk.size);
                const bool stack = !join && hk.kind == BlockKind::Heading && sk.kind == BlockKind::Heading && !hk.runIn &&
                                   !sk.runIn && h.take == 0 && s.take == 0 && h.count == 1 && s.count == 1 &&
                                   h.first + 1 == s.first && n.page == above.page && n.dir == above.dir &&
                                   n.baseline < above.baseline && above.vb - n.vt <= 0.3f * small &&
                                   std::fabs((above.u0 + above.u1) - (n.u0 + n.u1)) <= small && large <= 1.45f * small &&
                                   small >= 1.25f * body && !above.numbering && !n.numbering;
                join = (join || stack) && !hk.divisionLabel && !sk.divisionLabel;
                if (join) {
                    LineText(pages[n.page], pages[n.page].lines[n.line], line);
                    join = SectionDepth(line) == 0;
                }
                if (join) {
                    h.count += s.count;
                    h.right = Fmax(h.right, s.right);
                    h.bodyU0 = Fmin(h.bodyU0, s.bodyU0);
                    hk.chars += sk.chars;
                    continue;
                }
            }
            raws[kept] = raws[r];
            kinds[kept] = kinds[r];
            alertTitle[kept] = alertTitle[r];
            ++kept;
        }
        raws.resize(kept);
        kinds.resize(kept);
        alertTitle.resize(kept);
    }

    // Block quotes set by indentation alone (LaTeX's quote and quotation, algorithms §6.5): a
    // body-size paragraph inset by two em or more on both sides of the body paragraph nearest to
    // it on its page, its lines (after the first) flush on one left edge. One line alone counts
    // only when it is not centred (a display line, a short title). Captions and formulas are none.
    // Inset by four em or more it is a quote in a quote.
    std::vector<uint8_t> indentQuote(raws.size(), 0);
    {
        const auto bodyPara = [&](size_t q) {
            const RawBlock& c = raws[q];
            const Info& in = infos[c.first];
            return kinds[q].kind == BlockKind::Paragraph && !c.mono && in.quoteBars == 0 &&
                   in.dir == 0 && std::fabs(kinds[q].size - body) <= 0.1f * body && c.note == 0;
        };
        for (size_t r = 0; r < raws.size(); ++r) {
            const RawBlock& b = raws[r];
            const Info& first = infos[b.first];
            if (kinds[r].kind != BlockKind::Paragraph || b.mono || b.note != 0 || first.quoteBars != 0 ||
                first.dir != 0 || (first.flags & (kLineTable | kLineInvisible | kLineAnchor | kLineFormula)) ||
                std::fabs(kinds[r].size - body) > 0.1f * body)
                continue;
            const float em = body;
            float left = INFINITY, u1 = -INFINITY;
            for (uint32_t i = b.first; i < b.first + b.count; ++i) {
                if (i > b.first || b.count == 1) left = Fmin(left, infos[i].u0);
                u1 = Fmax(u1, infos[i].u1);
            }
            bool flush = first.u0 >= left - em && first.u0 <= left + 2 * em;  // a quote mark may hang
            for (uint32_t i = b.first + 1; i < b.first + b.count && flush; ++i) flush = infos[i].u0 <= left + 0.5f * em;
            if (!flush) continue;
            // The frame: the body paragraph set farthest left among the 16 blocks on either side on
            // its page that are left of it, in its column (a nested quote is inset from the text,
            // not from the quote around it).
            size_t frame = raws.size();
            for (size_t d = 1; d <= 16; ++d) {
                for (int side = 0; side < 2; ++side) {
                    if (side == 0 && d > r) continue;
                    const size_t q = side == 0 ? r - d : r + d;
                    if (q >= raws.size() || infos[raws[q].first].page != first.page || !bodyPara(q)) continue;
                    // Near it on the page: a frame far above is another text (a newspaper's lead
                    // above its columns).
                    const float top = infos[raws[q].first].vt, bottom = infos[raws[q].first + raws[q].count - 1].vb;
                    const float candTop = first.vt, candBottom = infos[b.first + b.count - 1].vb;
                    if (Fmax(bottom - candTop, candBottom - top) > 10 * em) continue;
                    if (raws[q].bodyU0 <= left - 2 * em && raws[q].right > left && raws[q].bodyU0 < u1 &&
                        (frame == raws.size() || raws[q].bodyU0 < raws[frame].bodyU0 - 0.5f * em))
                        frame = q;
                }
            }
            if (frame == raws.size()) continue;
            // A one-line frame ends where it ends: the text's right edge then mirrors its left
            // edge on the page.
            const float frameLeft = raws[frame].bodyU0;
            const float frameRight = raws[frame].count >= 2
                                         ? raws[frame].right
                                         : Fmax(raws[frame].right, pages[first.page].width - frameLeft);
            if (u1 > frameRight - 1.5f * em) continue;
            // Its insets: equal on both sides when its lines are justified (two or more lines,
            // the last aside, end on one right edge), the right one no smaller than the left one
            // when they are ragged (verse). A column of text beside a wider paragraph is no quote.
            uint32_t flushRight = 0;
            for (uint32_t i = b.first; i + 1 < b.first + b.count; ++i) flushRight += infos[i].u1 >= u1 - 1 ? 1 : 0;
            const float leftInset = left - frameLeft, rightInset = frameRight - u1;
            if (flushRight >= 2 ? std::fabs(leftInset - rightInset) > 1.5f * em : rightInset < leftInset - 1.5f * em)
                continue;
            // Lines centred one under the other, none flush with the next (a cover's lines, an
            // affiliation), bold lines (a title), labels (a chart's) and table rows are none: a
            // quote is prose that ends a sentence, forty characters at least over several lines.
            uint32_t chars = 0;
            bool centred = b.count >= 2, flushLeft = false;
            for (uint32_t i = b.first; i < b.first + b.count; ++i) {
                chars += infos[i].chars;
                centred = centred && std::fabs((infos[i].u0 + infos[i].u1) - (first.u0 + first.u1)) <= 2 * em;
                if (i > b.first) flushLeft = flushLeft || std::fabs(infos[i].u0 - infos[i - 1].u0) <= 0.3f * em;
            }
            if ((centred && !flushLeft) || b.bold || (b.count >= 2 && chars < 40) ||
                !infos[b.first + b.count - 1].endsTerminal)
                continue;
            // One line alone is a quote only as a sentence (a label, a formula piece or a slide's
            // text line is set apart the same way), when it is not centred, and when it is next
            // to a quote of more lines (below).
            if (b.count == 1 && (!first.endsTerminal || first.chars < 20 ||
                                 std::fabs((left - frameLeft) - (frameRight - u1)) <= 2 * em))
                continue;
            LineText(pages[first.page], pages[first.page].lines[first.line], line);
            // Nor is an abstract or a keyword list ("Abstract. …", or under an "Abstract" heading)
            // or a formula.
            if (IsCaption(line) || NoTitle(line) || FrontMatter(line) || MathSymbol(line)) continue;
            if (r > 0 && kinds[r - 1].kind == BlockKind::Heading) {
                const Info& h = infos[raws[r - 1].first];
                LineText(pages[h.page], pages[h.page].lines[h.line], line);
                if (FrontMatter(line)) continue;
            }
            indentQuote[r] = static_cast<uint8_t>((left - frameLeft >= 4 * em ? 2 : 1) | (b.count == 1 ? 0x80 : 0));
        }
        // One-line quotes join a run of quotes next to them on their page, in either direction;
        // two of them one after the other, flush on one left edge, make a run of their own.
        const auto settled = [&](size_t q, size_t r) {
            return indentQuote[q] != 0 && !(indentQuote[q] & 0x80) && infos[raws[q].first].page == infos[raws[r].first].page;
        };
        for (size_t r = 1; r < raws.size(); ++r) {
            const Info& a = infos[raws[r - 1].first];
            const Info& c = infos[raws[r].first];
            if ((indentQuote[r - 1] & 0x80) && (indentQuote[r] & 0x80) && a.page == c.page &&
                std::fabs(a.u0 - c.u0) <= 0.5f * body) {
                indentQuote[r - 1] = static_cast<uint8_t>(indentQuote[r - 1] | 0x40);
                indentQuote[r] = static_cast<uint8_t>(indentQuote[r] | 0x40);
            }
        }
        for (uint8_t& q : indentQuote)
            if ((q & 0xC0) == 0xC0) q = static_cast<uint8_t>(q & 0x3F);
        for (size_t r = 1; r < raws.size(); ++r)
            if ((indentQuote[r] & 0x80) && settled(r - 1, r)) indentQuote[r] = static_cast<uint8_t>(indentQuote[r] & 0x7F);
        for (size_t r = raws.size(); r-- > 1;)
            if ((indentQuote[r - 1] & 0x80) && settled(r, r - 1))
                indentQuote[r - 1] = static_cast<uint8_t>(indentQuote[r - 1] & 0x7F);
        for (uint8_t& q : indentQuote)
            if (q & 0x80) q = 0;
    }

    // Heading levels (§6.1). Outline levels where the outline matched; a heading the outline
    // missed takes the level of the last matched heading of its style, else sits one below the
    // last matched heading. Without the outline: styles ranked by size (bold first), below them
    // bold body-size lines. A run-in heading the outline missed sits one below the last heading
    // set on a line of its own (below the deepest one before any), whatever its style.
    //
    // A style's size for its rank is the cluster of its size among the distinct sizes of the
    // headings ranked (docs/research/layout-math.md §2): in log space, weighted by characters,
    // clustered by Fisher's exact 1-D k-means with k chosen by BIC (at most 6 clusters, a
    // cluster's variance at least 0.0004, so that sizes within about 2 % of each other fall
    // together). More distinct sizes than the clustering takes keep the half-point size keys.
    std::vector<int> rankKey(raws.size(), -1);
    {
        std::vector<std::pair<float, uint32_t>> sizes;  // the distinct sizes ranked, with their characters
        for (const Classified& k : kinds)
            if (k.kind == BlockKind::Heading && k.styleKey >= 0 && k.level == 0) sizes.emplace_back(k.size, k.chars);
        std::sort(sizes.begin(), sizes.end());
        size_t distinct = 0;
        for (size_t i = 0; i < sizes.size(); ++i) {
            if (distinct > 0 && sizes[distinct - 1].first == sizes[i].first)
                sizes[distinct - 1].second += sizes[i].second;
            else
                sizes[distinct++] = sizes[i];
        }
        sizes.resize(distinct);
        float logSize[Robust::kMaxClusterPoints];
        uint32_t weight[Robust::kMaxClusterPoints];
        uint8_t cluster[Robust::kMaxClusterPoints];
        int clusters = 0;
        if (!sizes.empty() && sizes.size() <= Robust::kMaxClusterPoints && sizes.front().first > 0) {
            for (size_t i = 0; i < sizes.size(); ++i)
                logSize[i] = std::log(sizes[i].first), weight[i] = std::max<uint32_t>(1, sizes[i].second);
            clusters = Robust::FisherClusters(logSize, weight, sizes.size(), 6, 0.0004f, cluster);
        }
        for (size_t r = 0; r < raws.size(); ++r) {
            const Classified& k = kinds[r];
            if (k.kind != BlockKind::Heading || k.styleKey < 0) continue;
            rankKey[r] = k.styleKey;
            if (clusters == 0 || k.level != 0) continue;
            const size_t at = static_cast<size_t>(
                std::lower_bound(sizes.begin(), sizes.end(), std::make_pair(k.size, uint32_t(0))) - sizes.begin());
            if (at < sizes.size() && sizes[at].first == k.size) rankKey[r] = cluster[at] * 2 + (k.bold ? 1 : 0);
        }
    }
    std::vector<int> styleKeys;
    for (size_t r = 0; r < raws.size(); ++r) {
        const Classified& k = kinds[r];
        if (k.kind == BlockKind::Heading && k.styleKey >= 0 && k.level == 0) styleKeys.push_back(rankKey[r]);
    }
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
            if (k.kind != BlockKind::Heading || (k.runIn && k.level == 0) || k.divisionLabel) continue;
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
                    static_cast<int>(std::find(styleKeys.begin(), styleKeys.end(), rankKey[r]) - styleKeys.begin());
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
    // The title of a document whose outline starts at its sections (LaTeX's bookmarks name the
    // sections, never the title): the first heading, on the first page, above every heading the
    // outline matched, set no smaller than any of them, in a style none of them has (in their
    // style it is one of them, its bookmark written another way) and no front-matter word
    // ("Abstract"). It sits alone on the top level, with the title lines of its style right
    // after it; every other heading goes one level down.
    bool documentTitle = false;
    {
        const auto style = [](const Classified& k) {
            return static_cast<int>(std::lround(k.size * 2)) * 2 + (k.bold ? 1 : 0);
        };
        size_t matched = raws.size(), first = raws.size();
        float matchedSize = 0;
        bool ownStyle = true;
        for (size_t r = 0; r < raws.size(); ++r) {
            if (kinds[r].kind != BlockKind::Heading || levelOf[r] <= 0) continue;
            if (kinds[r].level > 0) {
                if (matched == raws.size()) matched = r;
                matchedSize = Fmax(matchedSize, kinds[r].size);
                ownStyle = ownStyle && (first == raws.size() || style(kinds[r]) != style(kinds[first]));
            } else if (first == raws.size() && matched == raws.size()) {
                first = r;
            }
        }
        if (first < matched && matched < raws.size() && !kinds[first].runIn && ownStyle &&
            infos[raws[first].first].page == infos[raws[0].first].page && kinds[first].size >= matchedSize - 0.25f) {
            const Info& in = infos[raws[first].first];
            LineText(pages[in.page], pages[in.page].lines[in.line], line);
            if (!FrontMatter(line)) {
                documentTitle = true;
                size_t end = first + 1;
                while (end < matched && kinds[end].kind == BlockKind::Heading && levelOf[end] > 0 &&
                       kinds[first].styleKey >= 0 && kinds[end].styleKey == kinds[first].styleKey)
                    ++end;
                for (size_t r = 0; r < raws.size(); ++r)
                    if (levelOf[r] > 0) levelOf[r] = r >= first && r < end ? 1 : levelOf[r] + 1;
            }
        }
    }
    // A division's label sits at its title's level; one whose title no longer heads (a later
    // pass made it text) is text too.
    for (size_t r = 0; r < raws.size(); ++r) {
        if (!kinds[r].divisionLabel || kinds[r].kind != BlockKind::Heading) continue;
        if (r + 1 < raws.size() && kinds[r + 1].kind == BlockKind::Heading && levelOf[r + 1] > 0)
            levelOf[r] = levelOf[r + 1];
        else
            kinds[r].kind = BlockKind::Paragraph;
    }
    // Never skip a level: the levels in use renumbered 1..n. Then the top level is made absolute:
    // it is a title (H1) when it is the document's title above its outline, when set clearly
    // larger than the body, or when it is used once or twice above deeper levels and still set
    // well above the body (LaTeX at 11 pt sets \section at 1.31 times the body); it is the
    // sections of a title-less document (H2) when smaller, or when one style alone heads several
    // sections (Markdown documents carry one H1). The outline gives depths, not levels: a
    // title-less document's sections are its top bookmarks too.
    {
        bool used[8] = {};
        for (size_t r = 0; r < raws.size(); ++r)
            if (levelOf[r] > 0) used[levelOf[r]] = true;
        int map[8] = {};
        int next = 0;
        for (int l = 1; l <= 7; ++l)
            if (used[l]) map[l] = std::min(++next, 6);
        float topSize = 0;
        int topCount = 0;
        for (size_t r = 0; r < raws.size(); ++r) {
            if (levelOf[r] <= 0) continue;
            levelOf[r] = map[levelOf[r]];
            if (levelOf[r] == 1) topSize = Fmax(topSize, kinds[r].size), topCount++;
        }
        const bool title = documentTitle || (next >= 2 && topCount <= 2 && topSize >= kSmallTitleSize * body);
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
    // Per block in `blocks`: the info of its last line (UINT32_MAX for a comment), and whether it
    // is a float (a caption, a table, an image) that a paragraph may run on around.
    std::vector<uint32_t> blockLast;
    std::vector<uint8_t> floating;
    std::string capText;
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
            // FigureText::Drop: a figure's own text (every line of it in or beside a figure region)
            // that does not read as prose is left out (ChartText).
            if (dropFigureText && (kinds[r].kind == BlockKind::Paragraph || kinds[r].kind == BlockKind::ListItem)) {
                bool inFigure = true;
                for (uint32_t i = b.first; i < b.first + b.count && inFigure; ++i)
                    inFigure = (infos[i].flags & kLineFigure) != 0;
                if (inFigure && ChartText(pages, infos, b, body, capText)) {
                    ++r;
                    continue;
                }
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
            if (block.kind == BlockKind::Heading) {
                if (b.take != 0)
                    block.number = b.take;
                else
                    block.smallCaps = SmallCapsSize(pages, infos, b);
            }
            for (uint32_t i = b.first; i < b.first + b.count; ++i) {
                BlockLine bl;
                bl.page = infos[i].page;
                bl.line = infos[i].line;
                bl.skip = 0;
                lines.push_back(bl);
            }
            lines[block.first].skip = b.labelSkip;
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
                block.ordered = first.marker == Marker::Number && !first.verbatimMarker;
                block.number = first.value;
                block.closeParen = first.closeParen;
                block.task = first.task;
                if (!first.verbatimMarker) lines[block.first].skip = first.skip;
                itemTextU0 = b.lead ? infos[b.first + 1].u0 : first.textU0;
                itemMarkerU0 = first.markerU0;
                itemPage = first.page;
                itemDepth = block.depth;
            } else if (block.kind == BlockKind::Paragraph && itemDepth >= 0 && first.page == itemPage &&
                       !(indentQuote[r] && b.count >= 2) &&
                       std::fabs(first.u0 - itemTextU0) <= 0.8f * em &&
                       std::fabs(first.u0 - itemTextU0) < std::fabs(first.u0 - itemMarkerU0) && !blocks.empty() &&
                       (blocks.back().kind == BlockKind::ListItem || blocks.back().listParagraph)) {
                block.listParagraph = true;
                block.depth = static_cast<uint8_t>(itemDepth);
            } else if (block.kind != BlockKind::Comment) {
                markerStack.clear();
                itemDepth = -1;
            }
            if (!block.listParagraph && block.quote < indentQuote[r]) block.quote = indentQuote[r];
            // A figure's own text: every line of it in or beside a figure region.
            bool figureText = block.kind != BlockKind::Heading && block.kind != BlockKind::Code;
            for (uint32_t i = b.first; i < b.first + b.count && figureText; ++i)
                figureText = (infos[i].flags & kLineFigure) != 0;

            // Continuation across columns and pages (§6.2): the previous paragraph ends without
            // terminal punctuation and this one starts in lowercase. Floats set between the two
            // parts (a caption, a table, an image, a figure's own text, at the foot of a column or
            // the top of the next) move after the joined paragraph, where its source put them;
            // around floats the paragraph may go on below them in the same column.
            if (block.kind == BlockKind::Paragraph && !block.listParagraph && !blocks.empty() && !figureText &&
                first.startsLower && !(first.flags & (kLineTable | kLineFormula))) {
                size_t target = blocks.size() - 1;
                const auto floats = [&](size_t f) {
                    if (floating[f] == 2) {
                        // IsCaption takes only a text whose first byte starts a label (the line's
                        // text, as FirstTextByte reads it).
                        const BlockLine& bl = lines[blocks[f].first];
                        const Line& l = pages[bl.page].lines[bl.line];
                        const unsigned char c0 = FirstTextByte(pages[bl.page], l) | 0x20;
                        floating[f] = 0;
                        if (c0 == 'f' || c0 == 't' || c0 == 'c' || c0 == 'e' || c0 == 'g' || c0 == 'd') {
                            LineText(pages[bl.page], l, capText);
                            floating[f] = IsFloatCaption(capText) ? 1 : 0;
                        }
                    }
                    return floating[f] == 1;
                };
                while (target > 0 && floats(target) && blocks.size() - target <= kMaxFloatRun) --target;
                const bool around = target + 1 < blocks.size();
                // Around floats both parts are prose: the line before the break runs as wide as the
                // widest line after it, a column's width (a label of a figure, a lone equation
                // number ends no prose).
                const auto prose = [&](const Info* last, const RawBlock& next) {
                    float wide = 0;
                    for (uint32_t i = next.first; i < next.first + next.count; ++i)
                        wide = Fmax(wide, infos[i].u1 - infos[i].u0);
                    return wide >= kProseWidth * em && last->u1 - last->u0 >= 0.85f * wide;
                };
                Block& prev = blocks[target];
                const BlockLine& pl = lines[prev.first + prev.count - 1];
                const Info* prevLast = blockLast[target] != UINT32_MAX ? &infos[blockLast[target]] : nullptr;
                if (!floats(target) && (prev.kind == BlockKind::Paragraph || prev.kind == BlockKind::ListItem) &&
                    prevLast && prevLast->page == pl.page && prevLast->line == pl.line && !prevLast->endsTerminal &&
                    !(prevLast->flags & kLineFormula) &&
                    first.startsLower && std::fabs(prevLast->size - first.size) <= 0.1f * em &&
                    prev.quote == block.quote && block.alert == 0 &&
                    (around ? prose(prevLast, b) && first.page <= prevLast->page + 1 &&
                                  blocks[target + 1].first == prev.first + prev.count &&
                                  (prevLast->page != first.page || first.baseline > prevLast->baseline ||
                                   (first.u0 < prevLast->u1 && prevLast->u0 < first.u1))
                            : prevLast->page != first.page || first.baseline > prevLast->baseline)) {
                    size_t joined = target;
                    if (around) {
                        // The floats go before the paragraph's lines.
                        const uint32_t floatsFirst = prev.first + prev.count, moved = floatsFirst - prev.first;
                        std::rotate(lines.begin() + prev.first, lines.begin() + floatsFirst, lines.begin() + block.first);
                        for (size_t f = target + 1; f < blocks.size(); ++f) blocks[f].first -= moved;
                        prev.first = block.first - moved;
                        std::rotate(blocks.begin() + target, blocks.begin() + target + 1, blocks.end());
                        std::rotate(blockLast.begin() + target, blockLast.begin() + target + 1, blockLast.end());
                        std::rotate(floating.begin() + target, floating.begin() + target + 1, floating.end());
                        joined = blocks.size() - 1;
                    }
                    blocks[joined].count += block.count;
                    blockLast[joined] = b.first + b.count - 1;
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
            // A float: an image, a table, a caption, or a figure's own text (every line of it in or
            // beside a figure region). Whether a paragraph is a caption is read when a paragraph
            // after it may run on around it (2: not read yet).
            uint8_t isFloat = block.kind == BlockKind::Image || block.kind == BlockKind::Table || figureText ? 1 : 0;
            if (!isFloat && block.kind == BlockKind::Paragraph) isFloat = 2;
            blocks.push_back(block);
            blockLast.push_back(b.first + b.count - 1);
            floating.push_back(isFloat);
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
            blockLast.push_back(UINT32_MAX);
            floating.push_back(0);
        }
    }
    TitleNotes(pages, blocks, lines, notes, noteLines, out.noteRefs);
    MoveAddresses(pages, blocks, lines);
    const uint32_t base = static_cast<uint32_t>(lines.size());
    lines.insert(lines.end(), noteLines.begin(), noteLines.end());
    for (Block& note : notes) note.first += base;
    PlaceNotes(pages, blocks, lines, notes, out.noteRefs);
}

} // namespace RayoPdf::Layout
