// SPDX-License-Identifier: Apache-2.0
#include "layout/math.h"

#include "common/minmax.h"
#include "common/per_thread.h"
#include "common/utf8.h"
#include "layout/lines.h"
#include "layout/math_tables.h"

#include <algorithm>
#include <cmath>

namespace RayoPdf::Layout {
namespace {

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Whether `name` starts with `prefix` (lowercase), case-insensitively.
bool Starts(std::string_view name, std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (Lower(name[i]) != prefix[i]) return false;
    return true;
}

// Whether `name` holds `part` (lowercase), case-insensitively.
bool Holds(std::string_view name, std::string_view part) {
    for (size_t at = 0; at + part.size() <= name.size(); ++at)
        if (Starts(name.substr(at), part)) return true;
    return false;
}

bool Finite(float x) { return x == x && x - x == 0; }

// ---------------------------------------------------------------------------------------------
// Unicode facts.

bool IsAsciiLetter(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

RAYOPDF_HOT_INLINE bool IsGreek(uint32_t c) {
    if (c < 0x391 || c > 0x3F5) return false;
    return (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) || (c >= 0x3B1 && c <= 0x3C9) || c == 0x3D1 || c == 0x3D5 ||
           c == 0x3D6 || c == 0x3F0 || c == 0x3F1 || c == 0x3F5 || c == 0x3DD;
}

// Operators, relations and arrows: Arrows, Mathematical Operators, Miscellaneous Mathematical
// Symbols-A and -B, Supplemental Arrows, Supplemental Mathematical Operators, and ±, ×, ÷, ¬.
bool IsMathSymbol(uint32_t c) {
    return (c >= 0x2190 && c <= 0x22FF) || (c >= 0x27C0 && c <= 0x27FF) || (c >= 0x2900 && c <= 0x2AFF) ||
           c == 0xB1 || c == 0xD7 || c == 0xF7 || c == 0xAC || c == 0x2032 || c == 0x2033;
}

// Mathematical alphabets.
enum Alphabet : uint8_t {
    kAlphaNone,
    kAlphaBold,        // \mathbf
    kAlphaBoldItalic,  // \boldsymbol
    kAlphaCal,         // \mathcal (Unicode script, CMSY capitals, Euler script)
    kAlphaScr,         // \mathscr (RSFS)
    kAlphaFrak,        // \mathfrak
    kAlphaBb,          // \mathbb
    kAlphaSans,        // \mathsf
    kAlphaMono,        // \mathtt
};

constexpr std::string_view kAlphabetCommands[] = {"",          "\\mathbf",   "\\boldsymbol", "\\mathcal", "\\mathscr",
                                                  "\\mathfrak", "\\mathbb",  "\\mathsf",     "\\mathtt"};

// Letterlike symbols that stand in the holes of the Mathematical Alphanumeric Symbols block.
struct Letterlike {
    uint32_t cp;
    char letter;
    Alphabet alphabet;
};
constexpr Letterlike kLetterlike[] = {
    {0x2102, 'C', kAlphaBb},   {0x210A, 'g', kAlphaCal},  {0x210B, 'H', kAlphaCal},  {0x210C, 'H', kAlphaFrak},
    {0x210D, 'H', kAlphaBb},   {0x210E, 'h', kAlphaNone}, {0x2110, 'I', kAlphaCal},  {0x2111, 'I', kAlphaFrak},
    {0x2112, 'L', kAlphaCal},  {0x2115, 'N', kAlphaBb},   {0x2119, 'P', kAlphaBb},   {0x211A, 'Q', kAlphaBb},
    {0x211B, 'R', kAlphaCal},  {0x211C, 'R', kAlphaFrak}, {0x211D, 'R', kAlphaBb},   {0x2124, 'Z', kAlphaBb},
    {0x2128, 'Z', kAlphaFrak}, {0x212C, 'B', kAlphaCal},  {0x212D, 'C', kAlphaFrak}, {0x212F, 'e', kAlphaCal},
    {0x2130, 'E', kAlphaCal},  {0x2131, 'F', kAlphaCal},  {0x2133, 'M', kAlphaCal},  {0x2134, 'o', kAlphaCal},
};

// The Greek letters of the Mathematical Alphanumeric Symbols' Greek alphabets, in their order.
constexpr uint32_t kMathGreek[58] = {
    0x391, 0x392, 0x393, 0x394, 0x395, 0x396, 0x397, 0x398, 0x399, 0x39A, 0x39B, 0x39C,  0x39D,  0x39E, 0x39F,
    0x3A0, 0x3A1, 0x3F4, 0x3A3, 0x3A4, 0x3A5, 0x3A6, 0x3A7, 0x3A8, 0x3A9, 0x2207, 0x3B1, 0x3B2, 0x3B3, 0x3B4,
    0x3B5, 0x3B6, 0x3B7, 0x3B8, 0x3B9, 0x3BA, 0x3BB, 0x3BC, 0x3BD, 0x3BE, 0x3BF, 0x3C0,  0x3C1,  0x3C2, 0x3C3,
    0x3C4, 0x3C5, 0x3C6, 0x3C7, 0x3C8, 0x3C9, 0x2202, 0x3F5, 0x3D1, 0x3F0, 0x3D5, 0x3F1,  0x3D6,
};

// A mathematical alphanumeric (or letterlike stand-in) as its plain letter, digit or Greek letter
// and its alphabet. False for anything else.
bool MathAlphanumeric(uint32_t cp, uint32_t& base, Alphabet& alphabet) {
    // Neither a letterlike stand-in (U+2102-U+2134) nor in the block: most characters.
    if (cp < 0x2102 || (cp > 0x2134 && cp < 0x1D400) || cp > 0x1D7FF) return false;
    for (const Letterlike& l : kLetterlike) {
        if (l.cp == cp) {
            base = static_cast<unsigned char>(l.letter);
            alphabet = l.alphabet;
            return true;
        }
    }
    if (cp < 0x1D400 || cp > 0x1D7FF) return false;
    if (cp < 0x1D6A4) {
        // Thirteen Latin alphabets of 52 letters: bold, italic, bold italic, script, bold script,
        // fraktur, double-struck, bold fraktur, sans, sans bold, sans italic, sans bold italic, mono.
        static constexpr Alphabet kLatin[13] = {kAlphaBold, kAlphaNone, kAlphaBoldItalic, kAlphaCal,  kAlphaCal,
                                                kAlphaFrak, kAlphaBb,   kAlphaFrak,       kAlphaSans, kAlphaSans,
                                                kAlphaSans, kAlphaSans, kAlphaMono};
        const uint32_t i = cp - 0x1D400;
        const uint32_t letter = i % 52;
        base = letter < 26 ? 'A' + letter : 'a' + (letter - 26);
        alphabet = kLatin[i / 52];
        return true;
    }
    if (cp == 0x1D6A4 || cp == 0x1D6A5) {
        base = cp == 0x1D6A4 ? 0x131 : 0x237;
        alphabet = kAlphaNone;
        return true;
    }
    if (cp >= 0x1D6A8 && cp < 0x1D7CA) {
        // Five Greek alphabets of 58: bold, italic, bold italic, sans bold, sans bold italic.
        static constexpr Alphabet kGreek[5] = {kAlphaBoldItalic, kAlphaNone, kAlphaBoldItalic, kAlphaBoldItalic,
                                               kAlphaBoldItalic};
        const uint32_t i = cp - 0x1D6A8;
        base = kMathGreek[i % 58];
        alphabet = kGreek[i / 58];
        return true;
    }
    if (cp >= 0x1D7CE) {
        static constexpr Alphabet kDigits[5] = {kAlphaBold, kAlphaBb, kAlphaSans, kAlphaSans, kAlphaMono};
        const uint32_t i = cp - 0x1D7CE;
        base = '0' + i % 10;
        alphabet = kDigits[i / 10];
        return true;
    }
    return false;
}

// Greek capitals KaTeX writes as Latin letters.
uint32_t LatinLookalike(uint32_t cp) {
    switch (cp) {
    case 0x391: return 'A';
    case 0x392: return 'B';
    case 0x395: return 'E';
    case 0x396: return 'Z';
    case 0x397: return 'H';
    case 0x399: return 'I';
    case 0x39A: return 'K';
    case 0x39C: return 'M';
    case 0x39D: return 'N';
    case 0x39F: return 'O';
    case 0x3A1: return 'P';
    case 0x3A4: return 'T';
    case 0x3A7: return 'X';
    case 0x3BF: return 'o';
    default: return 0;
    }
}

// Commands chosen over KaTeX's first name, and characters KaTeX names nothing for.
struct Override {
    uint32_t cp;
    std::string_view latex;
};
constexpr Override kOverrides[] = {
    {0x00B7, "\\cdot"},  {0x2016, "\\|"},    {0x2022, "\\bullet"}, {0x2024, "."},      {0x2026, "\\ldots"},
    {0x2032, "'"},       {0x2033, "''"},     {0x2034, "'''"},      {0x2212, "-"},      {0x2215, "/"},
    {0x2223, "|"},       {0x2225, "\\|"},    {0x22C5, "\\cdot"},   {0x22EF, "\\cdots"}, {0x22F1, "\\ddots"},
    {0x2236, ":"},       {0x2044, "/"},      {0x00A0, "~"},        {0x2018, "`"},      {0x2019, "'"},
    {0x201C, "``"},      {0x201D, "''"},     {0x2013, "-"},        {0x2014, "-"},      {0x2010, "-"},
    {0x2011, "-"},       {0x02C6, "\\hat{}"}, {0x02DC, "\\tilde{}"}, {0x2329, "\\langle"}, {0x232A, "\\rangle"},
    {0x3008, "\\langle"}, {0x3009, "\\rangle"}, {0x2307, "\\wr"}, {0x2205, "\\emptyset"}, {0x00F8, "\\emptyset"},
};

// KaTeX's command for `cp`, or empty.
std::string_view CommandOf(uint32_t cp) {
    for (const Override& o : kOverrides)
        if (o.cp == cp) return o.latex;
    size_t lo = 0, hi = MathTables::kCommandCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (MathTables::kCommands[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < MathTables::kCommandCount && MathTables::kCommands[lo].cp == cp)
        return std::string_view(MathTables::kCommandNames + MathTables::kCommands[lo].name, MathTables::kCommands[lo].length);
    return {};
}

// The accent command of an accent character (spacing or combining), or empty.
std::string_view AccentOf(uint32_t cp) {
    switch (cp) {
    case 0x02C6: case 0x0302: case '^': return "\\hat";
    case 0x02DC: case 0x0303: case '~': return "\\tilde";
    case 0x00AF: case 0x02C9: case 0x0304: case 0x0305: return "\\bar";
    case 0x02D9: case 0x0307: return "\\dot";
    case 0x00A8: case 0x0308: return "\\ddot";
    case 0x02C7: case 0x030C: return "\\check";
    case 0x02D8: case 0x0306: return "\\breve";
    case 0x00B4: case 0x02CA: case 0x0301: return "\\acute";
    case 0x02CB: case 0x0300: return "\\grave";
    case 0x20D7: case 0x20D1: return "\\vec";
    case 0x02DA: case 0x030A: return "\\mathring";
    default: return {};
    }
}

bool IsCombining(uint32_t cp) { return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x20D0 && cp <= 0x20FF); }

// n-ary operators that take limits.
bool IsBigOperator(uint32_t cp) {
    switch (cp) {
    case 0x2211: case 0x220F: case 0x2210: case 0x22C3: case 0x22C2: case 0x22C0: case 0x22C1:
    case 0x2A00: case 0x2A01: case 0x2A02: case 0x2A04: case 0x2A06:
    case 0x222B: case 0x222C: case 0x222D: case 0x222E: case 0x222F: case 0x2230: case 0x2A0C:
        return true;
    default:
        return false;
    }
}

bool IsIntegral(uint32_t cp) { return (cp >= 0x222B && cp <= 0x2230) || cp == 0x2A0C; }

bool IsRelation(uint32_t cp) {
    switch (cp) {
    case '=': case '<': case '>': case 0x2264: case 0x2265: case 0x2260: case 0x2261: case 0x2248: case 0x223C:
    case 0x2243: case 0x2245: case 0x2208: case 0x2209: case 0x2282: case 0x2283: case 0x2286: case 0x2287:
    case 0x2192: case 0x2190: case 0x21D2: case 0x21D0: case 0x21D4: case 0x2194: case 0x21A6: case 0x226A:
    case 0x226B: case 0x227A: case 0x227B: case 0x2AAF: case 0x2AB0: case 0x221D: case 0x2254: case 0x2A7D:
    case 0x2A7E: case 0x2266: case 0x2267: case 0x22A2: case 0x2250: case 0x225C: case 0x2223:
        return true;
    default:
        return false;
    }
}

// Delimiters: 1 opening, 2 closing, 3 either (bars).
int DelimiterSide(uint32_t cp) {
    switch (cp) {
    case '(': case '[': case '{': case 0x27E8: case 0x2308: case 0x230A: case 0x2329: case 0x27E6: return 1;
    case ')': case ']': case '}': case 0x27E9: case 0x2309: case 0x230B: case 0x232A: case 0x27E7: return 2;
    case '|': case 0x2223: case 0x2225: case 0x2016: return 3;
    default: return 0;
    }
}

// Delimiter pieces (U+239B-23AD, U+23B7 radical bottom, U+23D0 vertical line extension): which
// stack they belong to, 0 for none: 1 ( 2 ) 3 [ 4 ] 5 { 6 } 7 √ 8 |.
int PieceFamily(uint32_t cp) {
    if (cp >= 0x239B && cp <= 0x239D) return 1;
    if (cp >= 0x239E && cp <= 0x23A0) return 2;
    if (cp >= 0x23A1 && cp <= 0x23A3) return 3;
    if (cp >= 0x23A4 && cp <= 0x23A6) return 4;
    if (cp >= 0x23A7 && cp <= 0x23A9) return 5;
    if (cp >= 0x23AB && cp <= 0x23AD) return 6;
    if (cp == 0x23AA) return 9;  // brace extension: either side
    if (cp == 0x23B7 || cp == 0xE000 || cp == 0xE001) return 7;
    if (cp == 0x23D0) return 8;
    return 0;
}

// KaTeX's function names (\sin), written as commands.
constexpr std::string_view kFunctions[] = {
    "arccos", "arcsin", "arctan", "arg", "cos",  "cosh", "cot", "coth", "csc", "deg",    "det",    "dim", "exp",
    "gcd",    "hom",    "inf",    "ker", "lg",   "lim",  "ln",  "log",  "max", "min",    "Pr",     "sec", "sin",
    "sinh",   "sup",    "tan",    "tanh", "liminf", "limsup",
};

// A word of up to 8 bytes as one integer (its first byte lowest), to compare with a name at once.
constexpr uint64_t Pack(std::string_view word) {
    uint64_t key = 0;
    for (size_t i = 0; i < word.size() && i < 8; ++i) key |= uint64_t(static_cast<uint8_t>(word[i])) << (8 * i);
    return key;
}

constexpr size_t kFunctionCount = sizeof(kFunctions) / sizeof(kFunctions[0]);
// The names' keys with the length in the top byte (every name is shorter than 8), sorted.
struct FunctionKeys {
    uint64_t key[kFunctionCount] = {};
    constexpr FunctionKeys() {
        for (size_t i = 0; i < kFunctionCount; ++i) {
            uint64_t k = Pack(kFunctions[i]) | uint64_t(kFunctions[i].size()) << 56;
            size_t at = i;
            for (; at > 0 && key[at - 1] > k; --at) key[at] = key[at - 1];
            key[at] = k;
        }
    }
};
constexpr FunctionKeys kFunctionKeys;

bool IsFunctionName(std::string_view word) {
    if (word.size() < 2 || word.size() > 6) return false;  // every name has 2 to 6 letters
    const uint64_t key = Pack(word) | uint64_t(word.size()) << 56;
    const uint64_t* const end = kFunctionKeys.key + kFunctionCount;
    const uint64_t* const at = std::lower_bound(kFunctionKeys.key, end, key);
    return at != end && *at == key;
}

// Short English words set upright in formulas ("for", "if", "and"): \text, not \mathrm.
bool IsConnective(std::string_view word) {
    static constexpr std::string_view kWords[] = {"if", "for", "and", "or", "of", "in", "on", "at", "as",
                                                  "is", "to", "all", "any", "but", "not", "so", "s.t", "iff"};
    for (std::string_view w : kWords)
        if (w == word) return true;
    return false;
}

// The WinAnsi code of a character (the font module's last guess reads a code as WinAnsi), or -1.
int WinAnsiCode(uint32_t cp) {
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return static_cast<int>(cp);
    static constexpr uint16_t kHigh[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                           0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
    for (int i = 0; i < 32; ++i)
        if (kHigh[i] == cp) return 0x80 + i;
    return -1;
}

// The symbol of code `code` of a TeX math font, through the font's encoding (KaTeX's tables).
// False when the font has no known encoding or the code means nothing.
bool TexCodeSymbol(MathFont font, int code, uint32_t& cp, uint8_t& family) {
    int table = -1;
    switch (font) {
    case MathFont::Italic: table = MathTables::kOml; break;
    case MathFont::Symbol: table = MathTables::kOms; break;
    case MathFont::Extension: table = MathTables::kOmx; break;
    case MathFont::AmsSymbol: table = MathTables::kMsam; break;
    case MathFont::Blackboard: table = MathTables::kMsbm; break;
    default: return false;
    }
    // Type 1 versions of TeX's fonts repeat codes 0-32 at 0xA1-0xC4 (0xAD skipped).
    if (code >= 0xA1 && code <= 0xAC) code -= 0xA1;
    else if (code >= 0xAE && code <= 0xC4) code = code - 0xAE + 12;
    if (code < 0 || code >= 128) return false;
    const uint32_t entry = MathTables::kTexCodes[table][code];
    if ((entry & 0x1FFFFF) == 0) return false;
    cp = entry & 0x1FFFFF;
    family = static_cast<uint8_t>(entry >> 24);
    return true;
}

// The symbol behind a WinAnsi guess `guess` of a glyph of a TeX math font (the font named the
// glyph nothing the Adobe Glyph List knows): the code back from WinAnsi, then the font's encoding.
bool TexSymbol(MathFont font, uint32_t guess, uint32_t& cp, uint8_t& family) {
    return TexCodeSymbol(font, WinAnsiCode(guess), cp, family);
}

// The ink of a glyph that hangs from its origin: the large glyphs of TeX's extension fonts and
// radical signs of its symbol font, `h` em deep (by cmex10's size family when known, else by the
// glyph's width) under their top at the origin. False for other glyphs.
bool HangingInk(MathFont font, uint32_t cp, uint8_t family, float size, float width, float v, float& bottom, float& top,
                uint8_t& delimSize) {
    delimSize = 0;
    const bool extension = font == MathFont::Extension;
    if (!extension && !(font == MathFont::Symbol && cp == 0x221A)) return false;
    if (AccentOf(cp).size() > 0 || cp == 0x2C6 || cp == 0x2DC) return false;  // wide accents sit above
    const float w = size > 0 ? width / size : 0;
    float h = 1.0f;
    if (family >= MathTables::kFamilySize1 && family <= MathTables::kFamilySize4) {
        delimSize = static_cast<uint8_t>(family - MathTables::kFamilySize1 + 1);
        static constexpr float kHeights[4] = {1.2f, 1.8f, 2.4f, 3.0f};
        h = kHeights[delimSize - 1];
        if (IsBigOperator(cp)) h = IsIntegral(cp) ? (delimSize == 1 ? 1.1f : 2.2f) : (delimSize == 1 ? 1.0f : 1.4f);
    } else if (IsBigOperator(cp)) {
        h = IsIntegral(cp) ? (w > 0.5f ? 2.2f : 1.1f) : (w > 1.25f ? 1.4f : 1.0f);
    } else if (PieceFamily(cp) != 0 && cp != 0x221A) {
        h = 0.9f;
    } else if (extension) {
        // Sized delimiters: cmex10's parentheses are 0.46, 0.6, 0.74 and 0.79 em wide.
        h = w >= 0.77f ? 3.0f : w >= 0.7f ? 2.4f : w >= 0.55f ? 1.8f : 1.2f;
    }
    top = v + 0.04f * size;
    bottom = top - h * size;
    return true;
}

// Characters that stand for others in math: compatibility forms (∆ U+2206, µ U+00B5, Ω U+2126)
// and math fonts' symbols that text fonts read differently (◦ is ∘, \ is ∖, ǫ is ϵ); with
// `named`, a math italic font's Greek through the Adobe Glyph List (φ is TeX's \phi ϕ).
uint32_t MathCharacter(uint32_t cp, MathFont font, bool named) {
    switch (cp) {
    case 0x2206: return 0x394;
    case 0xB5: return 0x3BC;
    case 0x2126: return 0x3A9;
    case 0xE020: return 0x338;  // KaTeX's private-use \not of cmsy10
    default: break;
    }
    if (font != MathFont::None) {
        if (cp == 0x25E6) return 0x2218;
        if (cp == '\\') return 0x2216;
        if (cp == 0x1EB) return 0x3F5;
    }
    if (named && font == MathFont::Italic) {
        if (cp == 0x3C6) return 0x3D5;
        if (cp == 0x3D5) return 0x3C6;
        if (cp == 0x3B5) return 0x3F5;
    }
    return cp;
}

// ---------------------------------------------------------------------------------------------
// Atoms: formula glyphs read into symbols.

enum class Kind : uint8_t {
    Ord,       // letters, digits, symbols
    Relation,  // =, ≤, →
    Binary,    // +, −, ×
    Punct,     // , ;
    BigOp,     // ∑, ∫
    Open,
    Close,
    Bar,       // | or ‖: opening or closing by place
    Accent,
    Radical,
    Piece,     // a delimiter piece
    Prime,
};

struct Atom {
    float u0 = 0, u1 = 0, v = 0, size = 0;
    float bottom = 0, top = 0;  // estimated ink
    uint32_t cp = 0;            // the symbol (0: `latex` holds several characters)
    std::string latex;          // the symbol in LaTeX, with its alphabet
    std::string word;           // letters of an upright letter (for function names)
    Kind kind = Kind::Ord;
    uint8_t delimSize = 0;      // 1-4: \big .. \Bigg (from cmex10's code), 0 unknown
    uint8_t family = 0;         // PieceFamily
    uint8_t font = 0;           // MathFont
    bool upright = false;       // an upright letter of a text font
    bool letter = false;
    bool lineStart = false;
};

// A placed part of a formula: an atom or a built structure.
struct Item {
    float u0 = 0, u1 = 0, v = 0, size = 0, bottom = 0, top = 0;
    std::string latex;
    Kind kind = Kind::Ord;
    int32_t atom = -1;     // its atom, or -1 for a structure
    bool structure = false;
    bool tall = false;     // a delimiter or operator that spans more than a line
    std::string accent;    // an accent command over it
};

void AppendToken(std::string& out, std::string_view token) {
    if (token.empty()) return;
    // A command name ending in a letter runs into a following letter: a space between.
    if (!out.empty()) {
        size_t i = out.size();
        while (i > 0 && IsAsciiLetter(static_cast<unsigned char>(out[i - 1]))) --i;
        const bool command = i > 0 && i < out.size() && out[i - 1] == '\\';
        if (command && IsAsciiLetter(static_cast<unsigned char>(token[0]))) out.push_back(' ');
    }
    out.append(token);
}

// A row's sort order: its items' position and their index.
struct SortKey {
    float u0, v;
    uint32_t index;
};

// The builder's storage, kept by each thread from formula to formula (every use clears it first).
struct BuilderScratch {
    std::vector<Atom> atoms;
    std::vector<uint8_t> ruleUsed;
    std::vector<uint32_t> all, byU, rules, part;  // Run's
    std::vector<const Item*> full;               // FindRows'
    std::vector<SortKey> sortKeys;
    std::vector<Item> sortItems;
    std::vector<float> baseV, baseW;
};

class Builder {
public:
    explicit Builder(const MathInput& in) : in_(in) {}
    MathStatus Run(bool display, std::string& out, std::string& tag);

private:
    bool Resolve();
    bool ResolveAtom(const MathGlyph& g, Atom& a);
    bool Expr(const std::vector<uint32_t>& atoms, const std::vector<uint32_t>& rules, int depth, std::string& out);
    bool Layout(std::vector<Item>& items, int depth, std::string& out);
    bool Row(std::vector<Item>& items, int depth, std::string& out);
    bool Matrix(std::vector<Item>& items, const Item& open, const Item* close, int depth, std::string& out);
    void ItemOf(uint32_t atom, Item& item) const;
    bool Spend(uint64_t n) {
        if (budget_ < n) {
            status_ = MathStatus::TooLarge;
            return false;
        }
        budget_ -= n;
        return true;
    }
    bool Fail(MathStatus s) {
        if (status_ == MathStatus::Ok) status_ = s;
        return false;
    }

    const MathInput& in_;
    BuilderScratch& scratch_ = PerThread<BuilderScratch>();
    std::vector<Atom>& atoms_ = scratch_.atoms;
    std::vector<uint8_t>& ruleUsed_ = scratch_.ruleUsed;
    uint64_t budget_ = uint64_t(1) << 24;
    MathStatus status_ = MathStatus::Ok;
    // Row's scratch, used before it recurses.
    std::vector<SortKey>& sortKeys_ = scratch_.sortKeys;
    std::vector<Item>& sortItems_ = scratch_.sortItems;
    std::vector<float>& baseV_ = scratch_.baseV;
    std::vector<float>& baseW_ = scratch_.baseW;
};

bool Builder::ResolveAtom(const MathGlyph& g, Atom& a) {
    a.u0 = g.u0, a.u1 = g.u1, a.v = g.v, a.size = g.size, a.font = g.font;
    a.lineStart = (g.flags & kMathLineStart) != 0;
    if (!Finite(g.u0) || !Finite(g.u1) || !Finite(g.v) || !Finite(g.size) || g.size <= 0 || g.u1 < g.u0)
        return false;
    if (g.flags & kMathUnmapped) return false;
    if (size_t(g.text) + g.length > in_.text.size()) return false;
    const std::string_view text(in_.text.data() + g.text, g.length);
    const MathFont font = static_cast<MathFont>(g.font);
    uint32_t cps[8];
    size_t count = 0;
    for (size_t at = 0; at < text.size();) {
        uint32_t cp = 0;
        size_t length = 0;
        if (!Utf8::Decode(text, at, cp, length)) break;
        at += length;
        if (cp == Utf8::kReplacement || (cp < 0x20 && cp != '\t') || count == 8) return false;
        if (cp == ' ' || cp == 0xA0 || cp == '\t') continue;
        cps[count++] = cp;
    }
    if (count == 0) return false;
    uint8_t family = 0;
    // A TeX math font's glyph the font gave no Unicode: its code read as WinAnsi; back to the code
    // and through the font's encoding to the symbol.
    if ((g.flags & kMathGuessed) && count == 1) {
        uint32_t symbol = 0;
        if (TexSymbol(font, cps[0], symbol, family)) {
            cps[0] = symbol;
        } else if (font != MathFont::None && font != MathFont::Fraktur && font != MathFont::Script &&
                   font != MathFont::Calligraphic) {
            return false;  // a guess in a math font of no known encoding, or a code meaning nothing
        }
    }
    for (size_t i = 0; i < count; ++i) cps[i] = MathCharacter(cps[i], font, (g.flags & kMathNamed) != 0);
    const float em = g.size;
    a.bottom = g.v - 0.25f * em;
    a.top = g.v + 0.72f * em;
    if (count > 1) {
        // Several characters in one glyph: a ligature, ActualText, a combining accent.
        if (count == 2 && IsCombining(cps[1]) && !AccentOf(cps[1]).empty()) {
            Atom base;
            const std::string_view accent = AccentOf(cps[1]);
            a.cp = cps[0];
            std::string inner;
            uint32_t letter = cps[0];
            Alphabet alpha = kAlphaNone;
            if (MathAlphanumeric(cps[0], letter, alpha)) {}
            std::string_view cmd = IsAsciiLetter(letter) || (letter >= '0' && letter <= '9') ? std::string_view() : CommandOf(letter);
            if (!cmd.empty()) inner.append(cmd);
            else if (letter < 0x80) inner.push_back(static_cast<char>(letter));
            else Utf8::Append(inner, letter);
            a.latex.append(accent);
            a.latex.push_back('{');
            a.latex.append(inner);
            a.latex.push_back('}');
            return true;
        }
        bool letters = true;
        for (size_t i = 0; i < count; ++i) letters = letters && IsAsciiLetter(cps[i]);
        if (letters) {
            for (size_t i = 0; i < count; ++i) a.word.push_back(static_cast<char>(cps[i]));
            a.letter = true;
            a.upright = !(g.flags & kMathItalic) && (font == MathFont::None || font == MathFont::Unicode);
            a.latex = a.word;
            return true;
        }
        for (size_t i = 0; i < count; ++i) {
            std::string_view cmd = CommandOf(cps[i]);
            if (!cmd.empty()) {
                AppendToken(a.latex, cmd);
            } else if (cps[i] < 0x80) {
                a.latex.push_back(static_cast<char>(cps[i]));
            } else {
                Utf8::Append(a.latex, cps[i]);
            }
        }
        return true;
    }
    uint32_t cp = cps[0];
    a.cp = cp;
    // Large glyphs hanging from their origin: their ink as PrepareMathGlyphs placed it (the glyph's
    // baseline moved), else measured from the origin.
    {
        float bottom = 0, top = 0;
        uint8_t delimSize = 0;
        if (HangingInk(font, cp, family, em, g.u1 - g.u0, g.v, bottom, top, delimSize)) {
            if (g.top > g.bottom) bottom = g.bottom, top = g.top;
            a.bottom = bottom, a.top = top, a.delimSize = delimSize;
        }
    }
    // KaTeX's tables give the radical's extension and top private-use code points: only a cmex10
    // code means them.
    if ((cp == 0xE000 || cp == 0xE001) && family < MathTables::kFamilySize1) return false;
    a.family = static_cast<uint8_t>(PieceFamily(cp));
    if (a.family != 0) {
        a.kind = a.family == 7 ? Kind::Radical : Kind::Piece;
        if (a.family == 7) a.latex = "\\surd";
        return true;
    }
    if (cp == 0x221A) {
        a.kind = Kind::Radical;
        a.latex = "\\surd";
        return true;
    }
    const std::string_view accent = AccentOf(cp);
    if (!accent.empty() && cp != '^' && cp != '~') {
        a.kind = Kind::Accent;
        a.latex.assign(accent);
        return true;
    }
    if (cp == 0x2032 || cp == 0x2033 || cp == 0x2034 || ((cp == 0x2019 || cp == '\'') && font != MathFont::None)) {
        a.kind = Kind::Prime;
        a.latex.assign(cp == 0x2033 ? "''" : cp == 0x2034 ? "'''" : "'");
        return true;
    }
    uint32_t letter = cp;
    Alphabet alpha = kAlphaNone;
    const bool alnum = MathAlphanumeric(cp, letter, alpha);
    if (!alnum) {
        // Font alphabets: MSBM capitals are blackboard bold, CMSY capitals calligraphic.
        if (IsAsciiLetter(cp)) {
            if (family == MathTables::kFamilyBlackboard || (font == MathFont::Blackboard && cp >= 'A' && cp <= 'Z'))
                alpha = kAlphaBb;
            else if (family == MathTables::kFamilyCaligraphic || (font == MathFont::Symbol && cp >= 'A' && cp <= 'Z') ||
                     font == MathFont::Calligraphic)
                alpha = kAlphaCal;
            else if (font == MathFont::Fraktur)
                alpha = kAlphaFrak;
            else if (font == MathFont::Script)
                alpha = kAlphaScr;
            else if (g.flags & kMathBold)
                alpha = (g.flags & kMathItalic) || font == MathFont::Italic ? kAlphaBoldItalic : kAlphaBold;
        } else if ((cp >= '0' && cp <= '9') && (g.flags & kMathBold)) {
            alpha = kAlphaBold;
        } else if (IsGreek(cp) && (g.flags & kMathBold)) {
            alpha = kAlphaBoldItalic;
        }
    }
    if (const uint32_t latin = LatinLookalike(letter)) letter = latin;
    std::string symbol;
    if (IsAsciiLetter(letter) || (letter >= '0' && letter <= '9')) {
        symbol.push_back(static_cast<char>(letter));
        if (IsAsciiLetter(letter)) {
            a.letter = true;
            a.upright = alpha == kAlphaNone && !alnum && !(g.flags & kMathItalic) &&
                        (font == MathFont::None || font == MathFont::Unicode);
            if (a.upright) a.word = symbol;
        }
    } else if (letter < 0x80) {
        switch (letter) {
        case '{': symbol = "\\{"; break;
        case '}': symbol = "\\}"; break;
        case '#': symbol = "\\#"; break;
        case '%': symbol = "\\%"; break;
        case '&': symbol = "\\&"; break;
        case '$': symbol = "\\$"; break;
        case '_': symbol = "\\_"; break;
        case '\\': symbol = "\\backslash"; break;
        case '~': symbol = "\\sim"; break;
        case '^': symbol = "\\hat{}"; break;
        case '"': symbol = "''"; break;
        default: symbol.push_back(static_cast<char>(letter)); break;
        }
    } else {
        const std::string_view cmd = CommandOf(letter);
        if (!cmd.empty()) {
            symbol.assign(cmd);
        } else if ((letter >= 0xE000 && letter <= 0xF8FF) || letter >= 0xF0000) {
            return false;  // private use: no symbol known
        } else {
            Utf8::Append(symbol, letter);
        }
    }
    if (alpha != kAlphaNone && !symbol.empty()) {
        a.latex.assign(kAlphabetCommands[alpha]);
        a.latex.push_back('{');
        a.latex.append(symbol);
        a.latex.push_back('}');
    } else {
        a.latex = std::move(symbol);
    }
    // Roles.
    if (IsBigOperator(letter)) {
        a.kind = Kind::BigOp;
    } else if (const int side = DelimiterSide(letter)) {
        a.kind = side == 1 ? Kind::Open : side == 2 ? Kind::Close : Kind::Bar;
    } else if (IsRelation(letter)) {
        a.kind = Kind::Relation;
    } else if (letter == '+' || letter == '-' || letter == 0x2212 || letter == 0xB1 || letter == 0x2213 ||
               letter == 0xD7 || letter == 0x22C5 || letter == 0x2217 || letter == 0x2218 || letter == 0xB7 ||
               letter == 0x2295 || letter == 0x2297 || letter == 0x222A || letter == 0x2229) {
        a.kind = Kind::Binary;
    } else if (letter == ',' || letter == ';') {
        a.kind = Kind::Punct;
    }
    return true;
}

bool Builder::Resolve() {
    atoms_.clear();
    atoms_.resize(in_.glyphs.size());
    for (size_t i = 0; i < in_.glyphs.size(); ++i) {
        if (!ResolveAtom(in_.glyphs[i], atoms_[i])) return Fail(MathStatus::Unreadable);
    }
    for (const MathRule& r : in_.rules)
        if (!Finite(r.u0) || !Finite(r.u1) || !Finite(r.v0) || !Finite(r.v1) || r.u1 < r.u0 || r.v1 < r.v0)
            return Fail(MathStatus::Unreadable);
    return true;
}

void Builder::ItemOf(uint32_t index, Item& item) const {
    const Atom& a = atoms_[index];
    item.u0 = a.u0, item.u1 = a.u1, item.v = a.v, item.size = a.size, item.bottom = a.bottom, item.top = a.top;
    item.latex = a.latex;
    item.kind = a.kind;
    item.atom = static_cast<int32_t>(index);
    item.tall = a.delimSize >= 2 || (a.font == static_cast<uint8_t>(MathFont::Extension) && a.top - a.bottom > 1.5f * a.size);
}

// The size of a set's ordinary glyphs: its largest outside large operators and delimiters.
float EmOf(const std::vector<Item>& items) {
    float em = 0, any = 0;
    for (const Item& it : items) {
        any = Fmax(any, it.size);
        if (it.structure || it.tall || it.kind == Kind::BigOp || it.kind == Kind::Piece || it.kind == Kind::Radical) continue;
        em = Fmax(em, it.size);
    }
    return em > 0 ? em : any;
}

bool Builder::Expr(const std::vector<uint32_t>& atoms, const std::vector<uint32_t>& rules, int depth, std::string& out) {
    if (depth > kMaxFormulaDepth) return Fail(MathStatus::TooLarge);
    if (atoms.empty()) return true;
    if (!Spend(atoms.size() * (rules.size() + 4))) return false;
    float em = 0;
    for (uint32_t a : atoms)
        if (atoms_[a].font != static_cast<uint8_t>(MathFont::Extension)) em = Fmax(em, atoms_[a].size);
    if (em <= 0)
        for (uint32_t a : atoms) em = Fmax(em, atoms_[a].size);
    std::vector<uint8_t> used(atoms.size(), 0);
    std::vector<Item> items;
    items.reserve(atoms.size() + rules.size());  // a fraction per rule at most, else an item per atom

    // Fraction bars (longest first: an outer bar before the bars of its numerator), over- and
    // underlines.
    std::vector<uint32_t> order = rules;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const float wa = in_.rules[a].u1 - in_.rules[a].u0, wb = in_.rules[b].u1 - in_.rules[b].u0;
        return wa != wb ? wa > wb : a < b;
    });
    std::vector<uint32_t> num, den, numRules, denRules;
    // A rule that starts where a radical sign ends is its vinculum, no fraction bar or overline.
    const auto vinculum = [&](const MathRule& rule) {
        for (uint32_t a : atoms) {
            const Atom& sign = atoms_[a];
            if (sign.kind != Kind::Radical) continue;
            if (std::fabs(rule.u0 - sign.u1) <= 0.4f * em && rule.v0 >= sign.bottom - 0.1f * em &&
                rule.v0 <= sign.top + 0.4f * sign.size)
                return true;
        }
        return false;
    };
    for (uint32_t r : order) {
        if (ruleUsed_[r] || vinculum(in_.rules[r])) continue;
        const MathRule& rule = in_.rules[r];
        const float mid = (rule.v0 + rule.v1) / 2;
        if (rule.v1 - rule.v0 > 0.3f * em || rule.u1 - rule.u0 < 0.25f * em) continue;
        num.clear(), den.clear(), numRules.clear(), denRules.clear();
        // The stack's reach above and below, widened by the bars of nested fractions in it.
        float above = 1.6f * em, below = 1.6f * em;
        const float slack = 0.08f * em;
        for (uint32_t s : rules) {
            if (s == r || ruleUsed_[s]) continue;
            const MathRule& o = in_.rules[s];
            if (o.u0 < rule.u0 - slack || o.u1 > rule.u1 + slack) continue;
            const float omid = (o.v0 + o.v1) / 2;
            if (omid > mid && omid - mid < above) above = Fmax(above, omid - mid + 1.6f * em), numRules.push_back(s);
            if (omid < mid && mid - omid < below) below = Fmax(below, mid - omid + 1.6f * em), denRules.push_back(s);
        }
        for (size_t i = 0; i < atoms.size(); ++i) {
            if (used[i]) continue;
            const Atom& a = atoms_[atoms[i]];
            const float cx = (a.u0 + a.u1) / 2;
            if (cx < rule.u0 - slack || cx > rule.u1 + slack) continue;
            if (a.v > mid && a.v - mid <= above)
                num.push_back(static_cast<uint32_t>(i));
            else if (a.v + 0.6f * a.size < mid && mid - a.v <= below)
                den.push_back(static_cast<uint32_t>(i));
        }
        if (num.empty() && den.empty()) continue;
        Item item;
        item.structure = true;
        item.u0 = rule.u0, item.u1 = rule.u1;
        item.top = mid, item.bottom = mid;
        std::vector<uint32_t> part;
        std::string numText, denText;
        float partSize = 0;  // a fraction is as large as its parts (small in an exponent)
        for (const std::vector<uint32_t>* side : {&num, &den}) {
            part.clear();
            for (uint32_t i : *side) {
                part.push_back(atoms[i]);
                item.top = Fmax(item.top, atoms_[atoms[i]].top);
                item.bottom = Fmin(item.bottom, atoms_[atoms[i]].bottom);
                partSize = Fmax(partSize, atoms_[atoms[i]].size);
            }
            std::vector<uint32_t>& sideRules = side == &num ? numRules : denRules;
            if (!Expr(part, sideRules, depth + 1, side == &num ? numText : denText)) return false;
        }
        item.size = partSize > 0 ? partSize : em;
        item.v = mid - 0.25f * item.size;
        if (num.empty() || den.empty()) {
            // A bar over or under text alone: an over- or underline when it hugs it.
            const std::vector<uint32_t>& side = num.empty() ? den : num;
            float nearest = INFINITY;
            for (uint32_t i : side) {
                const Atom& a = atoms_[atoms[i]];
                nearest = Fmin(nearest, num.empty() ? mid - a.top : a.bottom - mid);
            }
            if (nearest > 0.5f * em) continue;
            item.latex = num.empty() ? "\\overline{" + denText + "}" : "\\underline{" + numText + "}";
            item.v = atoms_[atoms[side[0]]].v;
        } else {
            item.latex = "\\frac{" + numText + "}{" + denText + "}";
        }
        ruleUsed_[r] = 1;
        for (uint32_t s : numRules) ruleUsed_[s] = 1;
        for (uint32_t s : denRules) ruleUsed_[s] = 1;
        for (uint32_t i : num) used[i] = 1;
        for (uint32_t i : den) used[i] = 1;
        items.push_back(std::move(item));
    }

    // Radicals: the sign, the rule over the radicand that starts at its right, an index set small
    // above its left.
    std::vector<uint32_t> radicand, index, sideRules;
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (used[i] || atoms_[atoms[i]].kind != Kind::Radical) continue;
        const Atom& sign = atoms_[atoms[i]];
        // Pieces of one built-up sign stand above each other: the lowest is the sign.
        int32_t best = -1;
        float bestGap = INFINITY;
        for (uint32_t r : rules) {
            if (ruleUsed_[r]) continue;
            const MathRule& rule = in_.rules[r];
            const float gap = std::fabs(rule.u0 - sign.u1);
            if (gap > 0.4f * em || rule.v0 < sign.bottom - 0.1f * em || rule.v0 > sign.top + 0.4f * sign.size) continue;
            if (gap < bestGap) bestGap = gap, best = static_cast<int32_t>(r);
        }
        if (best < 0) {
            used[i] = 1;
            ItemOf(atoms[i], items.emplace_back());
            continue;
        }
        const MathRule& rule = in_.rules[static_cast<uint32_t>(best)];
        radicand.clear(), index.clear(), sideRules.clear();
        used[i] = 1;
        float bottom = sign.bottom;
        for (size_t k = 0; k < atoms.size(); ++k) {
            if (used[k]) continue;
            const Atom& a = atoms_[atoms[k]];
            const float cx = (a.u0 + a.u1) / 2;
            if (a.kind == Kind::Radical && cx >= sign.u0 && cx <= sign.u1 && std::fabs(a.u0 - sign.u0) < 0.5f * em) {
                used[k] = 1;  // another piece of the same sign
                bottom = Fmin(bottom, a.bottom);
                continue;
            }
            if (cx > rule.u0 && cx <= rule.u1 + 0.05f * em && a.v < rule.v1 && a.v >= Fmin(bottom, rule.v0 - 3 * em) - 0.2f * em)
                radicand.push_back(static_cast<uint32_t>(k));
            else if (cx < sign.u0 + 0.7f * (sign.u1 - sign.u0) && cx >= sign.u0 - 0.8f * em && a.size < 0.9f * em &&
                     a.v > sign.bottom + 0.3f * em && a.v < rule.v1 + 0.2f * em)
                index.push_back(static_cast<uint32_t>(k));
        }
        for (uint32_t r : rules) {
            if (ruleUsed_[r] || r == static_cast<uint32_t>(best)) continue;
            const MathRule& o = in_.rules[r];
            if (o.u0 >= rule.u0 - 0.05f * em && o.u1 <= rule.u1 + 0.05f * em && o.v1 < rule.v0) sideRules.push_back(r);
        }
        ruleUsed_[static_cast<uint32_t>(best)] = 1;
        Item item;
        item.structure = true;
        item.u0 = Fmin(sign.u0, rule.u0), item.u1 = Fmax(sign.u1, rule.u1);
        item.size = em, item.top = rule.v1, item.bottom = bottom;
        std::vector<uint32_t> part;
        std::string text, indexText;
        for (uint32_t k : radicand) part.push_back(atoms[k]), used[k] = 1;
        float lowest = INFINITY;
        for (uint32_t k : radicand) lowest = Fmin(lowest, atoms_[atoms[k]].v);
        item.v = lowest < INFINITY ? lowest : sign.v;
        if (!Expr(part, sideRules, depth + 1, text)) return false;
        part.clear();
        for (uint32_t k : index) part.push_back(atoms[k]), used[k] = 1;
        if (!Expr(part, {}, depth + 1, indexText)) return false;
        item.latex = indexText.empty() ? "\\sqrt{" + text + "}" : "\\sqrt[" + indexText + "]{" + text + "}";
        items.push_back(std::move(item));
    }

    // Delimiter pieces stacked into one tall delimiter.
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (used[i] || atoms_[atoms[i]].kind != Kind::Piece) continue;
        const Atom& first = atoms_[atoms[i]];
        int family = first.family;
        Item& item = items.emplace_back();
        ItemOf(atoms[i], item);
        used[i] = 1;
        for (size_t k = i + 1; k < atoms.size(); ++k) {
            const Atom& a = atoms_[atoms[k]];
            if (used[k] || a.kind != Kind::Piece) continue;
            if (std::fabs((a.u0 + a.u1) - (first.u0 + first.u1)) > 0.6f * em) continue;
            if (family == 9) family = a.family;
            if (a.family != family && a.family != 9) continue;
            used[k] = 1;
            item.top = Fmax(item.top, a.top), item.bottom = Fmin(item.bottom, a.bottom);
            item.u0 = Fmin(item.u0, a.u0), item.u1 = Fmax(item.u1, a.u1);
        }
        static constexpr std::string_view kStacks[] = {"", "(", ")", "[", "]", "\\{", "\\}", "", "|", "\\{"};
        item.latex.assign(kStacks[family > 9 ? 0 : family]);
        item.kind = family == 1 || family == 3 || family == 5 || family == 9 ? Kind::Open
                    : family == 8                                          ? Kind::Bar
                                                                           : Kind::Close;
        item.tall = true;
        item.v = item.bottom + 0.3f * em;
    }

    for (size_t i = 0; i < atoms.size(); ++i) {
        if (used[i]) continue;
        ItemOf(atoms[i], items.emplace_back());
    }
    return Layout(items, depth, out);
}

// Rows: clusters of the baselines of the set's ordinary-size items.
struct RowBand {
    float v = 0, bottom = 0, top = 0;
    float weight = 0;
};

void FindRows(const std::vector<Item>& items, float em, std::vector<RowBand>& rows) {
    rows.clear();
    std::vector<const Item*>& full = PerThread<BuilderScratch>().full;
    full.clear();
    for (const Item& it : items) {
        if (it.tall || it.kind == Kind::BigOp || it.kind == Kind::Accent || it.kind == Kind::Piece || it.kind == Kind::Radical)
            continue;
        if (it.size < 0.85f * em) continue;
        full.push_back(&it);
    }
    std::sort(full.begin(), full.end(), [](const Item* a, const Item* b) { return a->v > b->v; });
    for (const Item* it : full) {
        if (!rows.empty() && rows.back().v - it->v <= 0.35f * em && it->top >= rows.back().bottom) {
            RowBand& r = rows.back();
            r.bottom = Fmin(r.bottom, it->bottom), r.top = Fmax(r.top, it->top);
            r.weight += it->u1 - it->u0;
            continue;
        }
        RowBand r;
        r.v = it->v, r.bottom = it->bottom, r.top = it->top, r.weight = it->u1 - it->u0;
        rows.push_back(r);
    }
    // Rows whose ink overlaps are one row (a fraction beside a symbol).
    size_t kept = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (kept > 0 && rows[i].top > rows[kept - 1].bottom + 0.1f * em) {
            RowBand& r = rows[kept - 1];
            if (rows[i].weight > r.weight) r.v = rows[i].v;
            r.bottom = Fmin(r.bottom, rows[i].bottom), r.top = Fmax(r.top, rows[i].top), r.weight += rows[i].weight;
            continue;
        }
        rows[kept++] = rows[i];
    }
    rows.resize(kept);
}

bool Builder::Layout(std::vector<Item>& items, int depth, std::string& out) {
    if (depth > kMaxFormulaDepth) return Fail(MathStatus::TooLarge);
    if (items.empty()) return true;
    if (!Spend(items.size() * items.size() + 8)) return false;
    const float em = EmOf(items);
    // Matrices and cases: a tall delimiter with two rows or more between it and its partner.
    bool tall = false;
    for (const Item& it : items) tall = tall || (it.tall && (it.kind == Kind::Open || it.kind == Kind::Bar));
    if (tall) {
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.u0 < b.u0; });
        std::vector<RowBand> bodyRows;
        std::vector<Item> body, rest;
        for (size_t i = 0; i < items.size(); ++i) {
            const Item& open = items[i];
            if (!open.tall || (open.kind != Kind::Open && open.kind != Kind::Bar)) continue;
            // Its partner: the nearest tall closing delimiter to its right of about its height.
            size_t close = items.size();
            for (size_t k = i + 1; k < items.size(); ++k) {
                const Item& c = items[k];
                if (!c.tall || (c.kind != Kind::Close && c.kind != Kind::Bar)) continue;
                if (std::fabs(c.top - open.top) > 0.8f * em || std::fabs(c.bottom - open.bottom) > 0.8f * em) continue;
                close = k;
                break;
            }
            const bool cases = close == items.size() && open.latex == "\\{";
            if (close == items.size() && !cases) continue;
            const float right = close < items.size() ? items[close].u0 : INFINITY;
            const auto inside = [&](const Item& it) {
                const float cy = (it.v + it.top) / 2;
                return it.u0 >= open.u1 - 0.1f * em && it.u1 <= right + 0.1f * em && cy <= open.top + 0.5f * em &&
                       cy >= open.bottom - 0.5f * em;
            };
            body.clear();
            for (size_t k = 0; k < items.size(); ++k)
                if (k != i && k != close && inside(items[k])) body.push_back(items[k]);
            FindRows(body, EmOf(body), bodyRows);
            if (bodyRows.size() < 2) continue;
            Item m;
            m.structure = true;
            m.u0 = open.u0, m.u1 = close < items.size() ? items[close].u1 : open.u1;
            m.top = open.top, m.bottom = open.bottom, m.size = em, m.v = open.bottom + (open.top - open.bottom) / 2 - 0.25f * em;
            for (const Item& b : body) m.u1 = Fmax(m.u1, b.u1);
            std::string text;
            if (!Matrix(body, open, close < items.size() ? &items[close] : nullptr, depth + 1, text)) return false;
            m.latex = std::move(text);
            rest.clear();
            for (size_t k = 0; k < items.size(); ++k)
                if (k != i && k != close && !inside(items[k])) rest.push_back(std::move(items[k]));
            rest.push_back(std::move(m));
            items = std::move(rest);
            return Layout(items, depth + 1, out);
        }
    }
    std::vector<RowBand> rows;
    FindRows(items, em, rows);
    if (rows.size() >= 2) {
        // Rows one under the other: aligned equations.
        std::vector<std::vector<Item>> parts(rows.size());
        for (Item& it : items) {
            size_t best = 0;
            float bestDistance = INFINITY;
            const float cy = (it.bottom + it.top) / 2;
            for (size_t r = 0; r < rows.size(); ++r) {
                const float d = cy > rows[r].top ? cy - rows[r].top : cy < rows[r].bottom ? rows[r].bottom - cy : 0;
                if (d < bestDistance) bestDistance = d, best = r;
            }
            parts[best].push_back(std::move(it));
        }
        // Alignment: each row's first relation, when they line up (or a row starts with it).
        std::vector<float> rel(rows.size(), NAN);
        std::vector<float> start(rows.size(), INFINITY);
        for (size_t r = 0; r < parts.size(); ++r) {
            float first = INFINITY;
            for (const Item& it : parts[r]) {
                start[r] = Fmin(start[r], it.u0);
                if (it.kind == Kind::Relation && it.u0 < first) first = it.u0;
            }
            if (first < INFINITY) rel[r] = first;
        }
        bool align = true;
        float axis = NAN;
        for (size_t r = 0; r < parts.size() && align; ++r) {
            if (rel[r] != rel[r]) {
                align = false;
                break;
            }
            if (axis != axis) axis = rel[r];
            align = std::fabs(rel[r] - axis) <= 0.6f * em;
        }
        out.append("\\begin{aligned}");
        for (size_t r = 0; r < parts.size(); ++r) {
            if (r > 0) out.append("\\\\");
            std::vector<Item> left, right;
            for (Item& it : parts[r]) {
                if (align && it.u0 >= rel[r] - 0.01f) right.push_back(std::move(it));
                else left.push_back(std::move(it));
            }
            std::string text;
            if (!Row(left, depth + 1, text)) return false;
            out.append(text);
            if (align) {
                out.push_back('&');
                text.clear();
                if (!Row(right, depth + 1, text)) return false;
                out.append(text);
            }
        }
        out.append("\\end{aligned}");
        return true;
    }
    return Row(items, depth, out);
}

bool Builder::Matrix(std::vector<Item>& body, const Item& open, const Item* close, int depth, std::string& out) {
    if (depth > kMaxFormulaDepth) return Fail(MathStatus::TooLarge);
    const float em = EmOf(body);
    std::vector<RowBand> rows;
    FindRows(body, em, rows);
    if (rows.empty()) return Fail(MathStatus::Unsure);
    // Columns: the body's items projected on the baseline, merged where they overlap or nearly.
    std::vector<std::pair<float, float>> spans;
    for (const Item& it : body) spans.emplace_back(it.u0, it.u1);
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<float, float>> columns;
    for (const auto& s : spans) {
        if (!columns.empty() && s.first <= columns.back().second + 0.6f * em)
            columns.back().second = Fmax(columns.back().second, s.second);
        else
            columns.push_back(s);
    }
    if (columns.size() > 64 || rows.size() > 64) return Fail(MathStatus::TooLarge);
    std::vector<std::vector<std::vector<Item>>> cells(rows.size(), std::vector<std::vector<Item>>(columns.size()));
    for (Item& it : body) {
        size_t best = 0;
        float bestDistance = INFINITY;
        const float cy = (it.bottom + it.top) / 2;
        for (size_t r = 0; r < rows.size(); ++r) {
            const float d = cy > rows[r].top ? cy - rows[r].top : cy < rows[r].bottom ? rows[r].bottom - cy : 0;
            if (d < bestDistance) bestDistance = d, best = r;
        }
        size_t col = 0;
        for (size_t c = 0; c < columns.size(); ++c)
            if (it.u0 >= columns[c].first - 0.01f) col = c;
        cells[best][col].push_back(std::move(it));
    }
    std::string_view env = "matrix";
    const std::string& o = open.latex;
    const std::string c = close ? close->latex : std::string();
    if (!close) env = "cases";
    else if (o == "(" && c == ")") env = "pmatrix";
    else if (o == "[" && c == "]") env = "bmatrix";
    else if (o == "\\{" && c == "\\}") env = "Bmatrix";
    else if (o == "|" && c == "|") env = "vmatrix";
    else if (o == "\\|" && c == "\\|") env = "Vmatrix";
    else return Fail(MathStatus::Unsure);
    out.append("\\begin{");
    out.append(env);
    out.push_back('}');
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r > 0) out.append("\\\\");
        for (size_t col = 0; col < columns.size(); ++col) {
            if (col > 0) out.push_back('&');
            std::string text;
            if (!Layout(cells[r][col], depth + 1, text)) return false;
            out.append(text);
        }
    }
    out.append("\\end{");
    out.append(env);
    out.push_back('}');
    return true;
}

// Delimiter size commands by height in em.
std::string_view SizePrefix(float height) {
    if (height >= 2.7f) return "\\Bigg";
    if (height >= 2.1f) return "\\bigg";
    if (height >= 1.5f) return "\\Big";
    return "\\big";
}

bool Builder::Row(std::vector<Item>& items, int depth, std::string& out) {
    if (depth > kMaxFormulaDepth) return Fail(MathStatus::TooLarge);
    if (items.empty()) return true;
    if (!Spend(items.size() * items.size() + 8)) return false;
    // Sorted by proxy: the same comparisons on the same order give std::sort's same permutation,
    // without moving the items' strings at each step.
    sortKeys_.clear();
    for (size_t i = 0; i < items.size(); ++i) sortKeys_.push_back({items[i].u0, items[i].v, static_cast<uint32_t>(i)});
    std::sort(sortKeys_.begin(), sortKeys_.end(), [](const SortKey& a, const SortKey& b) {
        return a.u0 != b.u0 ? a.u0 < b.u0 : a.v > b.v;
    });
    sortItems_.clear();
    for (const SortKey& k : sortKeys_) sortItems_.push_back(std::move(items[k.index]));
    items.swap(sortItems_);
    const float em = EmOf(items);
    // The main baseline: the baseline most ordinary-size ink sits on. The ink that counts is listed
    // once (in item order, so each sum adds in the same order); a weight depends only on a.v, so
    // an item on the baseline of the one before it repeats that weight, which never beats it.
    baseV_.clear(), baseW_.clear();
    for (const Item& b : items)
        if (!b.structure && !b.tall && b.size >= 0.85f * em) baseV_.push_back(b.v), baseW_.push_back(b.u1 - b.u0 + 0.01f);
    float mainV = items[0].v, best = -1;
    bool any = false;
    float lastV = 0;
    for (const Item& a : items) {
        if (a.structure || a.tall || a.kind == Kind::BigOp || a.kind == Kind::Accent || a.size < 0.85f * em) continue;
        if (any && a.v == lastV) continue;
        any = true, lastV = a.v;
        float weight = 0;
        for (size_t k = 0; k < baseV_.size(); ++k)
            if (std::fabs(baseV_[k] - a.v) <= 0.1f * em) weight += baseW_[k];
        if (weight > best) best = weight, mainV = a.v;
    }
    if (best < 0) {
        // Only structures and large symbols: their middle.
        for (const Item& a : items)
            if (a.structure || a.tall) {
                mainV = a.v;
                break;
            }
    }
    const size_t n = items.size();
    // 0 base, 1 superscript, 2 subscript, 3 consumed (an accent, a limit).
    // role and wordPart, and the limits and words below, each in one allocation.
    std::vector<uint8_t> marks(2 * n, 0);
    uint8_t* const role = marks.data();
    for (size_t i = 0; i < n; ++i) {
        const Item& it = items[i];
        const bool small = it.size < 0.92f * em;
        // Structures set small (a fraction in an exponent) are scripts like glyphs.
        if ((it.structure && !small) || it.tall || it.kind == Kind::BigOp || it.kind == Kind::Radical || it.kind == Kind::Piece)
            continue;
        const float d = it.v - mainV;
        if (it.kind == Kind::Prime && small) {
            role[i] = 1;
            continue;
        }
        if (it.kind == Kind::Accent) continue;
        if ((small && d > 0.12f * em) || d > 0.3f * em) role[i] = 1;
        else if ((small && d < -0.08f * em) || d < -0.3f * em) role[i] = 2;
    }
    // A small glyph next to a script, on neither side of the baseline (a subscript's own raised
    // script), goes with it.
    for (size_t i = 1; i < n; ++i) {
        if (role[i] != 0 || items[i].size >= 0.92f * em || items[i].kind == Kind::Accent || items[i].tall ||
            items[i].kind == Kind::BigOp)
            continue;
        if ((role[i - 1] == 1 || role[i - 1] == 2) && items[i].u0 - items[i - 1].u1 < 0.3f * em) role[i] = role[i - 1];
    }
    // Accents over a base glyph (or, for wide ones, over several).
    for (size_t i = 0; i < n; ++i) {
        Item& acc = items[i];
        if (acc.kind != Kind::Accent || role[i] == 3) continue;
        const float cx = (acc.u0 + acc.u1) / 2;
        size_t target = n;
        float overlap = 0;
        for (size_t k = 0; k < n; ++k) {
            if (k == i || role[k] == 3 || items[k].kind == Kind::Accent) continue;
            const Item& b = items[k];
            if (cx < b.u0 - 0.05f * em || cx > b.u1 + 0.05f * em || acc.v < b.v - 0.15f * em) continue;
            const float o = Fmin(acc.u1, b.u1) - Fmax(acc.u0, b.u0);
            if (target == n || o > overlap) target = k, overlap = o;
        }
        if (target == n) return Fail(MathStatus::Unsure);
        Item& b = items[target];
        // A wide accent over several glyphs.
        std::vector<size_t> under;
        for (size_t k = 0; k < n; ++k) {
            if (k == i || role[k] != role[target] || items[k].kind == Kind::Accent) continue;
            const float bx = (items[k].u0 + items[k].u1) / 2;
            if (bx >= acc.u0 && bx <= acc.u1 && std::fabs(items[k].v - b.v) < 0.2f * em) under.push_back(k);
        }
        role[i] = 3;
        if (under.size() >= 2) {
            std::string inner;
            for (size_t k : under) {
                AppendToken(inner, items[k].latex);
                if (k != under[0]) role[k] = 3;
            }
            const std::string cmd = acc.latex == "\\hat" ? "\\widehat" : acc.latex == "\\tilde" ? "\\widetilde" : acc.latex;
            Item& head = items[under[0]];
            head.latex = std::string(cmd) + "{" + inner + "}";
            head.u1 = Fmax(head.u1, items[under.back()].u1);
            head.atom = -1;
            continue;
        }
        if (!b.accent.empty()) return Fail(MathStatus::Unsure);
        b.accent = acc.latex;
    }
    // Large operators and function names with limits centred above and below them.
    std::vector<std::string> texts(3 * n);
    std::string* const lower = texts.data();
    std::string* const upper = lower + n;
    for (size_t i = 0; i < n; ++i) {
        const Item& op = items[i];
        const bool word = op.atom >= 0 && atoms_[static_cast<uint32_t>(op.atom)].upright;
        if (role[i] != 0 || (op.kind != Kind::BigOp && !word)) continue;
        std::vector<Item> below, above;
        for (int side = 0; side < 2; ++side) {
            std::vector<Item>& part = side == 0 ? below : above;
            std::vector<size_t> picked;
            for (size_t k = 0; k < n; ++k) {
                if (k == i || role[k] == 0 || role[k] == 3) continue;
                const Item& it = items[k];
                const float cx = (it.u0 + it.u1) / 2;
                if (cx < op.u0 - 0.1f * em || cx > op.u1 + 0.1f * em) continue;
                if (side == 0 ? it.v < Fmin(mainV - 0.45f * em, op.bottom + 0.1f * em) : it.v > Fmax(mainV + 0.65f * em, op.top - 0.2f * em))
                    picked.push_back(k);
            }
            // Grown along the limit's baseline to the glyphs beside it.
            for (size_t grow = 0; grow < picked.size() && grow < n; ++grow) {
                const Item& seed = items[picked[grow]];
                for (size_t k = 0; k < n; ++k) {
                    if (k == i || role[k] == 0 || role[k] == 3) continue;
                    if (std::find(picked.begin(), picked.end(), k) != picked.end()) continue;
                    const Item& it = items[k];
                    if (std::fabs(it.v - seed.v) > 0.5f * seed.size + 0.1f * em) continue;
                    if (it.u0 > seed.u1 + 0.35f * em || it.u1 < seed.u0 - 0.35f * em) continue;
                    picked.push_back(k);
                }
            }
            if (picked.empty()) continue;
            // Limits are centred on the operator; scripts beside it are not.
            float u0 = INFINITY, u1 = -INFINITY;
            for (size_t k : picked) u0 = Fmin(u0, items[k].u0), u1 = Fmax(u1, items[k].u1);
            const float centre = (u0 + u1) / 2, opCentre = (op.u0 + op.u1) / 2;
            if (std::fabs(centre - opCentre) > 0.25f * Fmax(u1 - u0, op.u1 - op.u0) + 0.2f * em) continue;
            for (size_t k : picked) {
                part.push_back(std::move(items[k]));  // consumed: no later step reads its texts
                role[k] = 3;
            }
        }
        if (!below.empty() && !Layout(below, depth + 1, lower[i])) return false;
        if (!above.empty() && !Layout(above, depth + 1, upper[i])) return false;
        if (word && below.empty() && above.empty()) continue;
    }
    // Upright letter runs: function names and text.
    std::string* const words = upper + n;
    uint8_t* const wordPart = role + n;
    for (size_t i = 0; i < n; ++i) {
        if (role[i] != 0 || items[i].atom < 0 || !atoms_[static_cast<uint32_t>(items[i].atom)].upright) continue;
        size_t end = i + 1, prev = i;
        std::string word = atoms_[static_cast<uint32_t>(items[i].atom)].word;
        while (true) {
            size_t next = end;
            while (next < n && role[next] == 3) ++next;  // limits already taken
            if (next >= n || role[next] != 0 || items[next].atom < 0 ||
                !atoms_[static_cast<uint32_t>(items[next].atom)].upright || items[next].u0 - items[prev].u1 >= 0.12f * em ||
                !items[next].accent.empty() || !items[prev].accent.empty())
                break;
            word += atoms_[static_cast<uint32_t>(items[next].atom)].word;
            prev = next;
            end = next + 1;
        }
        if (end - i < 2 && word.size() < 2) continue;
        // Limits found on the run's letters belong to the run.
        for (size_t k = i + 1; k < end; ++k) {
            if (lower[i].empty()) lower[i].swap(lower[k]);
            if (upper[i].empty()) upper[i].swap(upper[k]);
            wordPart[k] = 1;
        }
        if (IsFunctionName(word)) {
            words[i] = "\\" + word;
        } else if (word == "argmax" || word == "argmin") {
            words[i] = "\\operatorname*{" + word + "}";
        } else if (word.size() <= 3 && !IsConnective(word)) {
            words[i] = "\\mathrm{" + word + "}";
        } else {
            // Text, with the space after it kept when the formula goes on after a gap.
            words[i] = "\\text{" + word;
            if (end < n && items[end].u0 - items[end - 1].u1 >= 0.2f * em) words[i].push_back(' ');
            words[i].push_back('}');
        }
        i = end - 1;
    }
    // Three dots in a row, close together on one baseline: \ldots or \cdots.
    for (size_t i = 0; i + 2 < n; ++i) {
        if (role[i] != 0 || wordPart[i] || items[i].atom < 0) continue;
        const uint32_t cp = atoms_[static_cast<uint32_t>(items[i].atom)].cp;
        const bool low = cp == '.';
        if (!low && cp != 0x22C5 && cp != 0xB7 && cp != 0x2219) continue;
        size_t at = i, count = 1;
        for (size_t k = i + 1; k < n && count < 3; ++k) {
            if (role[k] == 3) continue;
            const Item& d = items[k];
            if (role[k] != 0 || d.atom < 0 || atoms_[static_cast<uint32_t>(d.atom)].cp != cp ||
                d.u0 - items[at].u1 > 0.4f * em || std::fabs(d.v - items[i].v) > 0.1f * em)
                break;
            at = k;
            ++count;
        }
        if (count < 3) continue;
        words[i] = low ? "\\ldots" : "\\cdots";
        for (size_t k = i + 1; k <= at; ++k)
            if (role[k] == 0) wordPart[k] = 1;
        items[i].u1 = items[at].u1;
        i = at;
    }
    // A negation slash over the relation after it (≠, ∉), and long arrows set from a bar and an
    // arrow (\longrightarrow is \relbar\joinrel\rightarrow).
    for (size_t i = 0; i + 1 < n; ++i) {
        if (role[i] != 0 || wordPart[i] || items[i].atom < 0 || !words[i].empty()) continue;
        size_t k = i + 1;
        while (k < n && role[k] == 3) ++k;
        if (k >= n || role[k] != 0 || wordPart[k] || items[k].atom < 0 || !words[k].empty()) continue;
        const uint32_t a = atoms_[static_cast<uint32_t>(items[i].atom)].cp, b = atoms_[static_cast<uint32_t>(items[k].atom)].cp;
        const float gap = items[k].u0 - items[i].u1;
        std::string joined;
        if (a == 0x338 && gap < 0.3f * em) {
            joined = b == '=' ? "\\neq" : b == 0x2208 ? "\\notin" : "\\not" + items[k].latex;
        } else if (b == 0x338 && gap < 0) {
            joined = a == '=' ? "\\neq" : a == 0x2208 ? "\\notin" : "\\not" + items[i].latex;
        } else if (gap < 0.15f * em) {
            const bool bar = a == '-' || a == 0x2212;
            if (bar && b == 0x2192) joined = "\\longrightarrow";
            else if (a == 0x2190 && (b == '-' || b == 0x2212)) joined = "\\longleftarrow";
            else if (a == '=' && b == 0x21D2) joined = "\\Longrightarrow";
            else if (a == 0x21D0 && b == '=') joined = "\\Longleftarrow";
            else if (a == 0x2190 && b == 0x2192) joined = "\\longleftrightarrow";
            else if (a == 0x21D0 && b == 0x21D2) joined = "\\Longleftrightarrow";
        }
        if (joined.empty()) continue;
        words[i] = std::move(joined);
        wordPart[k] = 1;
        items[i].u1 = items[k].u1;
        i = k;
    }
    // Emission: each base with the scripts after it (up to the next base); scripts before the
    // first base are prescripts.
    std::vector<Item> sup, sub;
    std::string supText, subText;
    float lastU1 = -INFINITY;
    size_t i = 0;
    const auto scripts = [&](size_t from, size_t to) -> bool {
        sup.clear(), sub.clear(), supText.clear(), subText.clear();
        for (size_t k = from; k < to; ++k) {
            // The scripts' texts are read only here: moved.
            if (role[k] == 1) sup.push_back(std::move(items[k]));
            else if (role[k] == 2) sub.push_back(std::move(items[k]));
        }
        if (!sup.empty()) {
            // Primes alone are written as such.
            bool primes = true;
            for (const Item& s : sup) primes = primes && s.kind == Kind::Prime;
            if (primes) {
                for (const Item& s : sup) supText += s.latex;
            } else if (!Layout(sup, depth + 1, supText)) {
                return false;
            }
        }
        if (!sub.empty() && !Layout(sub, depth + 1, subText)) return false;
        return true;
    };
    const auto writeScripts = [&](bool primeOnly) {
        if (!subText.empty()) {
            out.append("_{");
            out.append(subText);
            out.push_back('}');
        }
        if (!supText.empty()) {
            if (primeOnly) {
                out.append(supText);
            } else {
                out.append("^{");
                out.append(supText);
                out.push_back('}');
            }
        }
    };
    // Prescripts.
    while (i < n && role[i] != 0) ++i;
    if (i > 0) {
        if (!scripts(0, i)) return false;
        const bool primes = !supText.empty() && supText.find_first_not_of('\'') == std::string::npos;
        if (!sup.empty() || !sub.empty()) {
            out.append("{}");
            writeScripts(primes);
        }
    }
    while (i < n) {
        if (role[i] != 0 || wordPart[i]) {
            ++i;
            continue;
        }
        Item& b = items[i];
        size_t next = i + 1;
        while (next < n && (role[next] != 0 || wordPart[next])) ++next;
        // Space: a wide gap in a displayed row.
        if (lastU1 > -INFINITY) {
            const float gap = b.u0 - lastU1;
            if (gap >= 1.8f * em) AppendToken(out, "\\qquad");
            else if (gap >= 0.8f * em) AppendToken(out, "\\quad");
        }
        std::string_view token = !words[i].empty() ? std::string_view(words[i]) : std::string_view(b.latex);
        std::string built;
        if (words[i].empty() && b.tall && (b.kind == Kind::Open || b.kind == Kind::Close || b.kind == Kind::Bar)) {
            built.assign(SizePrefix((b.top - b.bottom) / em));
            built += b.kind == Kind::Open ? "l" : b.kind == Kind::Close ? "r" : "";
            if (b.latex.empty() || IsAsciiLetter(static_cast<unsigned char>(b.latex[0]))) built.push_back(' ');
            built += b.latex;
            token = built;
        }
        if (!b.accent.empty()) {
            std::string accented = b.accent + "{";
            accented.append(token);
            accented.push_back('}');
            built = std::move(accented);
            token = built;
        }
        AppendToken(out, token);
        lastU1 = b.u1;
        for (size_t k = i + 1; k < next; ++k)
            if (role[k] != 3) lastU1 = Fmax(lastU1, items[k].u1);
        if (!lower[i].empty() || !upper[i].empty()) {
            if (b.kind == Kind::BigOp && IsIntegral(b.atom >= 0 ? atoms_[static_cast<uint32_t>(b.atom)].cp : 0))
                out.append("\\limits");
            if (!lower[i].empty()) {
                out.append("_{");
                out.append(lower[i]);
                out.push_back('}');
            }
            if (!upper[i].empty()) {
                out.append("^{");
                out.append(upper[i]);
                out.push_back('}');
            }
        }
        if (!scripts(i + 1, next)) return false;
        if (!sup.empty() || !sub.empty()) {
            const bool primes = !supText.empty() && supText.find_first_not_of('\'') == std::string::npos;
            if ((!lower[i].empty() && !subText.empty()) || (!upper[i].empty() && !supText.empty() && !primes)) out.append("{}");
            writeScripts(primes);
        }
        i = next;
    }
    return true;
}

MathStatus Builder::Run(bool display, std::string& out, std::string& tag) {
    if (in_.glyphs.empty()) return MathStatus::Empty;
    if (in_.glyphs.size() > kMaxFormulaGlyphs || in_.rules.size() > kMaxFormulaRules) return MathStatus::TooLarge;
    if (!Resolve()) return status_;
    ruleUsed_.assign(in_.rules.size(), 0);
    std::vector<uint32_t>& all = scratch_.all;
    all.resize(atoms_.size());
    for (uint32_t i = 0; i < atoms_.size(); ++i) all[i] = i;
    std::string tagText;
    if (display) {
        // An equation number: "(", digits, letters and dots, ")" set apart at the right end.
        std::vector<uint32_t>& byU = scratch_.byU;
        byU.assign(all.begin(), all.end());
        std::sort(byU.begin(), byU.end(), [&](uint32_t a, uint32_t b) { return atoms_[a].u0 < atoms_[b].u0; });
        const size_t m = byU.size();
        if (m >= 4 && atoms_[byU[m - 1]].cp == ')') {
            size_t k = m - 1;
            while (k > 0 && m - k < 10 && atoms_[byU[k - 1]].cp != '(') --k;
            if (k > 0 && atoms_[byU[k - 1]].cp == '(' && k + 1 < m) {
                const size_t openAt = k - 1;
                const Atom& open = atoms_[byU[openAt]];
                bool plain = true;
                for (size_t j = openAt + 1; j + 1 < m && plain; ++j) {
                    const uint32_t cp = atoms_[byU[j]].cp;
                    plain = (cp >= '0' && cp <= '9') || IsAsciiLetter(cp) || cp == '.' || cp == '-' || cp == 0x2032 || cp == '*';
                    plain = plain && std::fabs(atoms_[byU[j]].v - open.v) < 0.2f * open.size;
                }
                float before = -INFINITY;
                for (size_t j = 0; j < openAt; ++j) before = Fmax(before, atoms_[byU[j]].u1);
                for (const MathRule& r : in_.rules) before = Fmax(before, r.u1);
                if (plain && openAt > 0 && open.u0 - before >= 1.5f * open.size) {
                    for (size_t j = openAt + 1; j + 1 < m; ++j) tagText += atoms_[byU[j]].latex;
                    all.clear();
                    for (size_t j = 0; j < openAt; ++j) all.push_back(byU[j]);
                    std::sort(all.begin(), all.end());
                }
            }
        }
    }
    std::vector<uint32_t>& rules = scratch_.rules;
    rules.resize(in_.rules.size());
    for (uint32_t r = 0; r < in_.rules.size(); ++r) rules[r] = r;
    std::string text;
    // An inline formula broken over lines: line by line.
    std::vector<uint32_t>& part = scratch_.part;
    bool ok = true;
    for (size_t at = 0; at < all.size() && ok;) {
        size_t end = at + 1;
        while (end < all.size() && !atoms_[all[end]].lineStart) ++end;
        part.assign(all.begin() + static_cast<std::ptrdiff_t>(at), all.begin() + static_cast<std::ptrdiff_t>(end));
        if (at > 0) text.push_back(' ');
        ok = Expr(part, rules, 0, text);
        at = end;
    }
    if (!ok) return status_ == MathStatus::Ok ? MathStatus::Unsure : status_;
    if (text.empty()) return MathStatus::Unsure;
    out.append(text);
    tag = std::move(tagText);
    return MathStatus::Ok;
}

} // namespace

MathFont MathFontOf(std::string_view name) {
    if (name.size() > 128) return MathFont::None;
    // TeX's own fonts and their clones, by the prefixes their families share.
    struct Prefix {
        std::string_view prefix;
        MathFont font;
    };
    static constexpr Prefix kPrefixes[] = {
        {"cmmi", MathFont::Italic},         {"cmbsy", MathFont::Symbol},       {"cmsy", MathFont::Symbol},
        {"cmex", MathFont::Extension},      {"msbm", MathFont::Blackboard},    {"msam", MathFont::AmsSymbol},
        {"eufm", MathFont::Fraktur},        {"eufb", MathFont::Fraktur},       {"rsfs", MathFont::Script},
        {"eusm", MathFont::Calligraphic},   {"eusb", MathFont::Calligraphic},  {"eurm", MathFont::Italic},
        {"eurb", MathFont::Italic},         {"lmmathitalic", MathFont::Italic}, {"lmmathsymbols", MathFont::Symbol},
        {"lmmathextension", MathFont::Extension}, {"rtxmi", MathFont::Italic}, {"txmi", MathFont::Italic},
        {"ntxmi", MathFont::Italic},        {"newtxmi", MathFont::Italic},     {"rpxmi", MathFont::Italic},
        {"pxmi", MathFont::Italic},         {"txsy", MathFont::Symbol},        {"ntxsy", MathFont::Symbol},
        {"pxsy", MathFont::Symbol},         {"txex", MathFont::Extension},     {"ntxex", MathFont::Extension},
        {"pxex", MathFont::Extension},      {"mnsymbol", MathFont::Symbol},    {"esint", MathFont::Extension},
        {"stmary", MathFont::Symbol},       {"wasy", MathFont::Symbol},        {"tex-matha", MathFont::Symbol},
        {"tex-mathb", MathFont::Symbol},    {"tex-mathx", MathFont::Extension}, {"bbold", MathFont::Blackboard},
        {"dsrom", MathFont::Blackboard},    {"bbm", MathFont::Blackboard},     {"mtextra", MathFont::Symbol},
        {"mt extra", MathFont::Symbol},
    };
    for (const Prefix& p : kPrefixes)
        if (Starts(name, p.prefix)) return p.font;
    // OpenType math fonts ("LatinModernMath-Regular", "STIXTwoMath", "CambriaMath"), MathDesign's
    // "...-MathItalic-10" and other "Math" names.
    if (Holds(name, "mathitalic")) return MathFont::Italic;
    if (Holds(name, "math")) return MathFont::Unicode;
    return MathFont::None;
}

MathStatus BuildLatex(const MathInput& in, bool display, std::string& out, std::string& tag) {
    tag.clear();
    Builder builder(in);
    return builder.Run(display, out, tag);
}

void AddMathGlyph(const PageData& page, const Glyph& g, MathInput& in) {
    // Spaces and what is not drawn are no part of a formula.
    if (g.flags & (kGlyphSpace | kGlyphInvisible | kGlyphDropped)) return;
    MathGlyph m;
    m.u0 = g.u0, m.u1 = g.u1, m.v = g.v, m.size = g.size;
    m.bottom = g.vb, m.top = g.vt;  // read only for glyphs that hang from their origin
    const uint8_t font = g.font < page.fontMath.size() ? page.fontMath[g.font] : 0;
    m.font = font & kMathFontClass;
    m.flags = static_cast<uint8_t>(((g.flags & kGlyphItalic) ? kMathItalic : 0) | ((g.flags & kGlyphBold) ? kMathBold : 0) |
                                   ((g.flags & kGlyphGuessed) ? kMathGuessed : 0) |
                                   ((g.flags & (kGlyphUnmapped | kGlyphImage)) ? kMathUnmapped : 0) |
                                   ((font & kMathFontNamed) ? kMathNamed : 0));
    const std::string_view text = page.GlyphText(g);
    m.text = static_cast<uint32_t>(in.text.size());
    m.length = static_cast<uint16_t>(text.size() < 64 ? text.size() : 64);
    in.text.append(text.substr(0, m.length));
    in.glyphs.push_back(m);
}

void AddMathRules(const PageData& page, float u0, float u1, float vb, float vt, MathInput& in) {
    for (const Ruling& r : page.rulings) {
        if (in.rules.size() > kMaxFormulaRules) return;
        // Pure tests, the vertical band first: it leaves out most of a page's rulings.
        if (r.y0 < vb || r.y1 > vt) continue;
        if (r.x1 - r.x0 <= r.y1 - r.y0) continue;  // horizontal bars only
        if (r.x0 < u0 - 1 || r.x1 > u1 + 1) continue;
        in.rules.push_back(MathRule{r.x0, r.x1, r.y0, r.y1});
    }
    for (const ImageBox& i : page.images) {
        if (in.rules.size() > kMaxFormulaRules) return;
        const float h = i.y1 - i.y0;
        if (!(h >= 0 && h <= 2.0f && i.x1 - i.x0 >= 3 * h && (i.height == 1 || h <= 1.0f))) continue;
        if (i.x0 < u0 - 1 || i.x1 > u1 + 1 || i.y0 < vb || i.y1 > vt) continue;
        in.rules.push_back(MathRule{i.x0, i.x1, i.y0, i.y1});
    }
}

void PrepareMathGlyphs(PageData& page) {
    // Math font glyphs without text whose code TeX's encodings know get its symbol as their text.
    for (const MathCode& c : page.mathCodes) {
        if (c.glyph >= page.glyphs.size() || page.text.size() >= (size_t(1) << 31)) continue;
        Glyph& g = page.glyphs[c.glyph];
        if (!(g.flags & kGlyphUnmapped) || g.textLength != 0 || g.font >= page.fontMath.size()) continue;
        uint32_t cp = 0;
        uint8_t family = 0;
        if (!TexCodeSymbol(static_cast<MathFont>(page.fontMath[g.font] & kMathFontClass), c.code, cp, family)) continue;
        cp = MathCharacter(cp, MathFont::Symbol, false);
        if (cp >= 0xE000 && cp <= 0xF8FF) continue;  // private use: no symbol to write
        g.text = static_cast<uint32_t>(page.text.size());
        Utf8::Append(page.text, cp);
        g.textLength = static_cast<uint16_t>(page.text.size() - g.text);
        g.flags = static_cast<uint16_t>(g.flags & ~kGlyphUnmapped);
    }
    // Glyphs that may be accents, listed in this pass (in order); the accent pass below reads
    // their v after this one moved it. No ASCII character is an accent there (^ and ~ are not
    // taken).
    std::vector<uint32_t> maybeAccents;
    for (size_t gi = 0; gi < page.glyphs.size(); ++gi) {
        Glyph& g = page.glyphs[gi];
        if (g.dir == 0 && g.textLength != 0 && !(g.flags & (kGlyphInvisible | kGlyphSpace))) {
            const std::string_view t = page.GlyphText(g);
            if (!(t.size() == 1 && static_cast<unsigned char>(t[0]) < 0x80)) maybeAccents.push_back(static_cast<uint32_t>(gi));
        }
        if (g.dir != 0 || g.font >= page.fontMath.size() || (g.flags & (kGlyphInvisible | kGlyphUnmapped))) continue;
        const MathFont font = static_cast<MathFont>(page.fontMath[g.font] & kMathFontClass);
        if (font != MathFont::Extension && font != MathFont::Symbol) continue;
        const std::string_view text = page.GlyphText(g);
        uint32_t cp = 0;
        size_t length = 0;
        if (!Utf8::Decode(text, 0, cp, length) || length != text.size()) continue;
        uint8_t family = 0;
        if (g.flags & kGlyphGuessed) {
            uint32_t symbol = 0;
            if (!TexSymbol(font, cp, symbol, family)) continue;
            cp = symbol;
        }
        float bottom = 0, top = 0;
        uint8_t size = 0;
        if (!Finite(g.v) || !Finite(g.size) || !HangingInk(font, cp, family, g.size, g.u1 - g.u0, g.v, bottom, top, size))
            continue;
        g.vb = g.y0 = bottom;
        g.vt = g.y1 = top;
        g.v = (bottom + top) / 2 - 0.25f * g.size;
    }
    // Accents float over the letter they mark, often far enough for a line of their own: each
    // accent glyph goes down onto the baseline of the glyph under it.
    uint64_t budget = uint64_t(1) << 22;
    const size_t n = page.glyphs.size();
    // The glyphs an accent may sit on: visible text glyphs of direction 0 whose text starts with no
    // accent. Nothing below changes which glyphs they are or their boxes (only accents move), so
    // they are listed once, when the first accent needs them, and after 16 accents sorted by the
    // left end of their reach, with the furthest right end up to each. With every box finite the
    // glyph under an accent is the nearest by `d`, the first of equal ones, in any order; otherwise
    // all glyphs are walked in order as they always were.
    struct Reach {
        float lo, hi;  // the glyph's reach: u0 - 0.1 size to u1 + 0.1 size
        uint32_t glyph;
    };
    std::vector<Reach> reach;
    std::vector<float> reachHi;
    bool listed = false, finite = true, sorted = false;
    uint32_t accents = 0;
    for (size_t m = 0; m < maybeAccents.size() && budget > n; ++m) {
        const size_t i = maybeAccents[m];
        Glyph& a = page.glyphs[i];
        if (!Finite(a.v)) continue;
        const std::string_view text = page.GlyphText(a);
        uint32_t cp = 0;
        size_t length = 0;
        if (!Utf8::Decode(text, 0, cp, length) || length != text.size() || AccentOf(cp).empty() || cp == '^' || cp == '~')
            continue;
        budget -= n;
        if (!listed) {
            listed = true;
            for (size_t k = 0; k < n; ++k) {
                const Glyph& b = page.glyphs[k];
                if (b.dir != 0 || b.textLength == 0 || (b.flags & (kGlyphInvisible | kGlyphSpace))) continue;
                const std::string_view bt = page.GlyphText(b);
                const unsigned char lead = bt.empty() ? 0 : static_cast<unsigned char>(bt[0]);
                uint32_t bcp = 0;
                if (lead < 0x80 ? lead == '^' || lead == '~' : Utf8::Decode(bt, 0, bcp, length) && !AccentOf(bcp).empty())
                    continue;
                finite = finite && Finite(b.u0) && Finite(b.u1) && Finite(b.v) && Finite(b.size);
                reach.push_back(Reach{b.u0 - 0.1f * b.size, b.u1 + 0.1f * b.size, static_cast<uint32_t>(k)});
            }
        }
        // Many accents: the list sorted once pays for itself.
        if (finite && !sorted && ++accents > 16) {
            sorted = true;
            std::sort(reach.begin(), reach.end(), [](const Reach& x, const Reach& y) { return x.lo < y.lo; });
            reachHi.resize(reach.size());
            float hi = -INFINITY;
            for (size_t r = 0; r < reach.size(); ++r) {
                hi = Fmax(hi, reach[r].hi);
                reachHi[r] = hi;
            }
        }
        const float cx = (a.u0 + a.u1) / 2;
        float best = INFINITY;
        size_t under = n;
        if (finite) {
            const auto consider = [&](const Reach& r) {
                if (cx < r.lo || cx > r.hi || r.glyph == i) return;
                const Glyph& b = page.glyphs[r.glyph];
                const float d = a.v - b.v;
                if (d < -0.05f * b.size || d > 1.0f * b.size) return;
                if (d < best || (d == best && r.glyph < under)) best = d, under = r.glyph;
            };
            if (!sorted || cx != cx) {
                for (const Reach& r : reach) consider(r);
            } else {
                // Glyphs whose reach starts left of cx, back to where none reaches it any more.
                size_t end = std::upper_bound(reach.begin(), reach.end(), cx, [](float x, const Reach& r) { return x < r.lo; }) -
                             reach.begin();
                while (end > 0 && !(reachHi[end - 1] < cx)) consider(reach[--end]);
            }
        } else {
            for (size_t k = 0; k < n; ++k) {
                const Glyph& b = page.glyphs[k];
                if (k == i || b.dir != 0 || b.textLength == 0 || (b.flags & (kGlyphInvisible | kGlyphSpace))) continue;
                if (cx < b.u0 - 0.1f * b.size || cx > b.u1 + 0.1f * b.size) continue;
                const float d = a.v - b.v;
                if (d < -0.05f * b.size || d > 1.0f * b.size || d >= best) continue;
                const std::string_view bt = page.GlyphText(b);
                uint32_t bcp = 0;
                if (Utf8::Decode(bt, 0, bcp, length) && !AccentOf(bcp).empty()) continue;
                best = d, under = k;
            }
        }
        if (under == n || best <= 0.05f * a.size) continue;
        // Only math: an accent of a math font, or over a glyph of one (text accents are diacritics).
        const auto math = [&](const Glyph& g) { return g.font < page.fontMath.size() && (page.fontMath[g.font] & kMathFontClass) != 0; };
        if (!math(a) && !math(page.glyphs[under])) continue;
        a.v -= best, a.vb -= best, a.vt -= best, a.y0 -= best, a.y1 -= best;
    }
}


namespace {

// What a glyph says about math, for finding formulas.
enum GlyphClass : uint8_t {
    kClassText,     // a letter of a text font, or anything else that is not math
    kClassNeutral,  // digits, brackets, ASCII operators and punctuation: math beside math
    kClassStrong,   // a math font's glyph, a math symbol, a mathematical alphanumeric, an italic letter alone
};

RAYOPDF_HOT_INLINE uint32_t FirstCp(std::string_view t) {
    if (!t.empty() && static_cast<unsigned char>(t[0]) < 0x80) return static_cast<unsigned char>(t[0]);
    uint32_t cp = 0;
    size_t length = 0;
    return Utf8::Decode(t, 0, cp, length) ? cp : 0;
}

// The ASCII characters IsNeutral takes: digits, the accents ^ and ~, operators and punctuation.
struct NeutralAscii {
    bool is[128] = {};
    constexpr NeutralAscii() {
        constexpr char kChars[] = "0123456789^~=+-<>()[]{}/*|!'.,:;_";
        for (size_t i = 0; i + 1 < sizeof(kChars); ++i) is[static_cast<unsigned char>(kChars[i])] = true;
    }
};
constexpr NeutralAscii kNeutralAscii;

bool IsNeutral(uint32_t cp) {
    if (cp < 0x80) return kNeutralAscii.is[cp];
    if (!AccentOf(cp).empty()) return true;  // an accent over a math letter
    switch (cp) {
    case 0x2212: case 0x2032: case 0x2033: case 0x2013: case 0xB7: case 0x2026:
        return true;
    default:
        return false;
    }
}

// A glyph's text appended; most are one byte.
RAYOPDF_HOT_INLINE void AppendText(std::string& out, std::string_view t) {
    if (t.size() == 1)
        out.push_back(t[0]);
    else
        out.append(t.data(), t.size());
}

// Symbols of math fonts that text uses too: footnote and section marks, bullets.
bool IsTextMark(uint32_t cp) {
    return cp == 0x2020 || cp == 0x2021 || cp == 0xA7 || cp == 0xB6 || cp == 0x2022 || cp == 0xA9 || cp == 0x25E6 ||
           cp == 0x2023 || cp == 0x2219 || cp == 0x25A1 || cp == 0x2610 || cp == 0x2611 || cp == 0x2612;
}

RAYOPDF_HOT_INLINE bool IsLetterCp(uint32_t cp) {
    if (cp < 0x80) return IsAsciiLetter(cp);
    return cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x2000 && cp <= 0x2BFF) && !(cp >= 0x3000 && cp <= 0x303F) &&
           !(cp >= 0xE000 && cp <= 0xF8FF) && !(cp >= 0x1D400 && cp <= 0x1D7FF) && cp != 0xFFFD;
}

uint8_t ClassOfOther(uint8_t font, uint16_t flags, std::string_view text, bool alone);

// A glyph's class from its font, text and face; `alone` says it is a word of one letter.
RAYOPDF_HOT_INLINE uint8_t ClassOf(uint8_t font, uint16_t flags, std::string_view text, bool alone) {
    const unsigned char lead = text.empty() ? 0 : static_cast<unsigned char>(text[0]);
    if (lead >= 0x80) return ClassOfOther(font, flags, text, alone);
    // ASCII, as below: no text mark, math symbol, alphanumeric or Greek letter is ASCII, and
    // MathCharacter keeps it.
    if (lead == 0) return kClassText;
    if (font != static_cast<uint8_t>(MathFont::None) && !(flags & kGlyphUnmapped)) return kClassStrong;
    if (kNeutralAscii.is[lead]) return kClassNeutral;
    if (alone && (flags & kGlyphItalic) && IsAsciiLetter(lead) && lead != 'a' && lead != 'A' && lead != 'I')
        return kClassNeutral;
    return kClassText;
}

// ClassOf of a glyph whose text starts with a byte past ASCII.
uint8_t ClassOfOther(uint8_t font, uint16_t flags, std::string_view text, bool alone) {
    const uint32_t cp = MathCharacter(FirstCp(text), MathFont::None, false);  // ∆ is Δ, µ is μ
    if (cp == 0) return kClassText;
    // Footnote marks are text in any font; bullets and the like only in text fonts.
    if (IsTextMark(cp) && (font == static_cast<uint8_t>(MathFont::None) || cp == 0x2020 || cp == 0x2021 || cp == 0xA7 || cp == 0xB6))
        return kClassText;
    uint32_t base = 0;
    Alphabet alphabet = kAlphaNone;
    if (font != static_cast<uint8_t>(MathFont::None) && !(flags & kGlyphUnmapped)) return kClassStrong;
    if (IsMathSymbol(cp) || MathAlphanumeric(cp, base, alphabet)) return kClassStrong;
    if (IsNeutral(cp)) return kClassNeutral;
    // A letter alone in italic (a variable set in a text font) goes with math beside it but
    // starts none: prose names things in italics too ("R and L are").
    if (alone && (flags & kGlyphItalic) && IsAsciiLetter(cp) && cp != 'a' && cp != 'A' && cp != 'I') return kClassNeutral;
    if (IsGreek(cp)) return kClassStrong;  // a Greek word is text: FindInlineFormulas decides
    return kClassText;
}

// The first bytes of the function words: the names' and those of Re, Im, mod, argmax, argmin.
struct FunctionLeads {
    bool is[256] = {};
    constexpr FunctionLeads() {
        for (size_t i = 0; i < kFunctionCount; ++i) is[static_cast<unsigned char>(kFunctions[i][0])] = true;
        is[static_cast<unsigned char>('R')] = is[static_cast<unsigned char>('I')] = true;
        is[static_cast<unsigned char>('m')] = is[static_cast<unsigned char>('a')] = true;
    }
};
constexpr FunctionLeads kFunctionLeads;

bool IsFunctionWord(std::string_view word) {
    if (word.size() < 2 || word.size() > 6) return false;  // every one has 2 to 6 letters
    if (!kFunctionLeads.is[static_cast<unsigned char>(word[0])]) return false;
    if (IsFunctionName(word)) return true;
    const uint64_t key = Pack(word);
    switch (word.size()) {
    case 2: return key == Pack("Re") || key == Pack("Im");
    case 3: return key == Pack("mod");
    case 6: return key == Pack("argmax") || key == Pack("argmin");
    default: return false;
    }
}

} // namespace

bool MayBeFormulaOther(uint8_t font, uint16_t flags, std::string_view text) {
    // ClassOf's strong class does not depend on `alone`; FindInlineFormulas only lowers it.
    return ClassOf(font, flags, text, false) == kClassStrong;
}

void FindInlineFormulas(const std::vector<InlineGlyph>& glyphs, std::vector<uint32_t>& run) {
    const uint32_t n = static_cast<uint32_t>(glyphs.size());
    run.assign(n, 0);
    if (n == 0) return;
    // Words.
    std::vector<uint32_t> wordStart;
    wordStart.reserve(size_t(n) + 1);
    for (uint32_t i = 0; i < n; ++i)
        if (i == 0 || glyphs[i].wordStart) wordStart.push_back(i);
    wordStart.push_back(n);
    const size_t words = wordStart.size() - 1;
    std::vector<uint8_t> cls(n, kClassText);
    bool any = false;
    for (size_t w = 0; w < words; ++w) {
        const uint32_t b = wordStart[w], e = wordStart[w + 1];
        uint32_t letters = 0, greek = 0;
        for (uint32_t i = b; i < e; ++i) {
            const uint32_t c = FirstCp(glyphs[i].text);
            letters += IsLetterCp(c) ? 1 : 0;
            greek += IsGreek(c) ? 1 : 0;
        }
        bool strongWord = false;  // a strong glyph in the word: its class is final once set
        for (uint32_t i = b; i < e; ++i) {
            const InlineGlyph& g = glyphs[i];
            if (g.note || !g.glyph || (g.glyph->flags & (kGlyphImage | kGlyphInvisible))) continue;
            cls[i] = ClassOf(g.font, g.glyph->flags, g.text, letters == 1);
            // Greek in a word of Greek letters (Greek prose) is text.
            if (cls[i] == kClassStrong && g.font == static_cast<uint8_t>(MathFont::None) && IsGreek(FirstCp(g.text)) &&
                letters > 1 && greek == letters)
                cls[i] = kClassText;
            // Raised or lowered glyphs next to math are its scripts.
            if (cls[i] == kClassText && g.script && i > b && cls[i - 1] != kClassText) cls[i] = kClassNeutral;
            any = any || cls[i] == kClassStrong;
            strongWord = strongWord || cls[i] == kClassStrong;
        }
        // Letters of a text font in a word with math: a function name ("max_j", "log x") or up to
        // four letters right before a script or a math glyph ("GoE_m", "h_1") are math too.
        if (!strongWord) continue;
        for (uint32_t s = b; s < e;) {
            if (cls[s] != kClassText || !IsLetterCp(FirstCp(glyphs[s].text))) {
                ++s;
                continue;
            }
            uint32_t t = s;
            std::string word;
            while (t < e && cls[t] == kClassText && IsLetterCp(FirstCp(glyphs[t].text)) && word.size() < 16)
                AppendText(word, glyphs[t].text), ++t;
            // ... or before the bracket of an argument at the word's start ("GELU(x)").
            const bool before = t < e && t - s <= 4 &&
                                (glyphs[t].script || cls[t] == kClassStrong || (s == b && FirstCp(glyphs[t].text) == '('));
            if (before || IsFunctionWord(word))
                for (uint32_t i = s; i < t; ++i) cls[i] = kClassNeutral;
            s = t;
        }
    }
    if (!any) return;
    // Per word: the math segment touching its start and the one touching its end, as runs of
    // glyphs without text letters around a strong glyph.
    const auto wordText = [&](size_t w, std::string& out) {
        out.clear();
        for (uint32_t i = wordStart[w]; i < wordStart[w + 1]; ++i) AppendText(out, glyphs[i].text);
    };
    std::string text;
    uint32_t start = UINT32_MAX;  // the open run's first glyph
    uint32_t last = 0;            // its last glyph so far (exclusive)
    const auto close = [&]() {
        if (start == UINT32_MAX) return;
        uint32_t b = start, e = last;
        // Sentence punctuation and unmatched brackets at the ends stay text.
        for (int pass = 0; pass < 8 && b < e; ++pass) {
            const uint32_t lastCp = FirstCp(glyphs[e - 1].text);
            const uint32_t firstCp = FirstCp(glyphs[b].text);
            int depth = 0, minDepth = 0;
            for (uint32_t i = b; i < e; ++i) {
                const uint32_t c = FirstCp(glyphs[i].text);
                if (c == '(' || c == '[') depth++;
                if (c == ')' || c == ']') depth--, minDepth = std::min(minDepth, depth);
            }
            if (lastCp == '.' || lastCp == ',' || lastCp == ';' || lastCp == ':' || lastCp == '-' || lastCp == 0x2013) {
                --e;
            } else if ((lastCp == ')' || lastCp == ']') && minDepth < 0) {
                --e;
            } else if ((firstCp == '(' || firstCp == '[') && depth > 0) {
                ++b;
            } else {
                break;
            }
        }
        bool strong = false;
        for (uint32_t i = b; i < e; ++i) strong = strong || (cls[i] == kClassStrong && !glyphs[i].script);
        if (strong && e > b) run[b] = e;
        start = UINT32_MAX;
    };
    for (size_t w = 0; w < words; ++w) {
        const uint32_t b = wordStart[w], e = wordStart[w + 1];
        bool strong = false, textLetter = false;
        for (uint32_t i = b; i < e; ++i) {
            strong = strong || cls[i] == kClassStrong;
            textLetter = textLetter || cls[i] == kClassText;
        }
        if (!strong) {
            // A word of neutral glyphs ("=", "2", "(a,"), or a function name, continues an open run
            // when math follows it (through further neutral words) or it is an operator.
            bool neutral = !textLetter;
            // A function word has 2 to 6 bytes: the word's text is put together only then.
            bool function = false;
            if (!neutral) {
                size_t bytes = 0;
                for (uint32_t i = b; i < e; ++i) bytes += glyphs[i].text.size();
                if (bytes >= 2 && bytes <= 6) {
                    wordText(w, text);
                    function = IsFunctionWord(text);
                }
            }
            if (!neutral && !function) {
                close();
                continue;
            }
            size_t ahead = w + 1;
            bool mathAhead = false;
            while (ahead < words && ahead - w < 8) {
                bool s = false, t = false;
                for (uint32_t i = wordStart[ahead]; i < wordStart[ahead + 1]; ++i) {
                    s = s || cls[i] == kClassStrong;
                    t = t || cls[i] == kClassText;
                }
                if (s) {
                    mathAhead = cls[wordStart[ahead]] != kClassText;
                    break;
                }
                if (t) break;
                ++ahead;
            }
            bool op = false;
            for (uint32_t i = b; i < e; ++i) {
                const uint32_t c = FirstCp(glyphs[i].text);
                op = op || c == '=' || c == '+' || c == '<' || c == '>' || c == 0x2212 || c == '/' || c == '*';
            }
            if (mathAhead) {
                if (start == UINT32_MAX) start = b;
                last = e;
            } else if (start != UINT32_MAX && neutral && (op || last == b)) {
                // "x = 2": an operator and the numbers after it; anything without letters after a
                // formula that is not done ("[0," before "1]").
                bool digits = true;
                for (uint32_t i = b; i < e; ++i) {
                    const uint32_t c = FirstCp(glyphs[i].text);
                    digits = digits && ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == ')' || c == '(');
                }
                int depth = 0;
                for (uint32_t i = start; i < last; ++i) {
                    const uint32_t c = FirstCp(glyphs[i].text);
                    depth += c == '(' || c == '[' || c == '{' ? 1 : c == ')' || c == ']' || c == '}' ? -1 : 0;
                }
                const uint32_t end = last > start ? FirstCp(glyphs[last - 1].text) : 0;
                const bool unfinished = depth > 0 || end == ',' || end == '=' || end == '+' || end == '-' || end == 0x2212 ||
                                        end == '(' || end == '[' || end == '<' || end == '>';
                if (op || digits || unfinished) last = e;
                else close();
            } else {
                close();
            }
            continue;
        }
        // A word with math: its segments between text letters.
        uint32_t i = b;
        while (i < e) {
            if (cls[i] == kClassText) {
                close();
                ++i;
                continue;
            }
            uint32_t s = i;
            bool segStrong = false;
            while (i < e && cls[i] != kClassText) segStrong = segStrong || cls[i] == kClassStrong, ++i;
            if (!segStrong) {
                // Neutral glyphs beside text letters ("2nd", "(see"): text, unless they go on a run.
                if (start != UINT32_MAX && s == b && last == b) last = i;
                close();
                continue;
            }
            if (start == UINT32_MAX || !(s == b && last == b)) {
                close();
                start = s;
            }
            last = i;
            if (i < e) close();
        }
        // The run goes on into the next word only if it reached this word's end.
        if (start != UINT32_MAX && last != e) close();
        if (start != UINT32_MAX && w + 1 < words) last = e;
        // `last == wordStart[w + 1]` lets the next word continue the run.
        if (start != UINT32_MAX) last = wordStart[w + 1];
    }
    close();
}

void KeepNumbersText(const std::vector<InlineGlyph>& glyphs, std::vector<uint32_t>& run) {
    const uint32_t n = static_cast<uint32_t>(glyphs.size());
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t to = run[k];
        if (to == 0 || to > n) continue;
        bool number = true, digit = false;
        for (uint32_t i = k; i < to && number; ++i) {
            number = !glyphs[i].script;
            const std::string_view t = glyphs[i].text;
            for (size_t at = 0; at < t.size() && number;) {
                uint32_t cp = static_cast<unsigned char>(t[at]);
                size_t length = 1;
                if (cp >= 0x80 && !Utf8::Decode(t, at, cp, length)) cp = 0, length = 1;
                at += length;
                // Digits as text or as mathematical alphanumerics (bold, sans-serif and the like).
                const bool isDigit = (cp >= '0' && cp <= '9') || (cp >= 0x1D7CE && cp <= 0x1D7FF);
                digit = digit || isDigit;
                number = isDigit || cp == '.' || cp == ',' || cp == '+' || cp == '-' || cp == '%' || cp == ' ' ||
                         cp == 0x2212 || cp == 0xB1 || cp == 0x2213 || cp == 0xA0 || (cp >= 0x2002 && cp <= 0x200A) ||
                         cp == 0x202F;
            }
        }
        if (number && digit) run[k] = 0;
    }
}

namespace {

// Facts of one ordered line for display formulas.
struct LineFacts {
    float u0 = 0, u1 = 0, bottom = 0, top = 0, size = 0;
    uint32_t glyphs = 0, math = 0, words = 0;
    bool usable = false, number = false;
};

void FactsOf(const PageData& page, const Line& line, LineFacts& f) {
    f = LineFacts();
    if (line.dir != 0 || line.count == 0 || (line.flags & (kLineAnchor | kLineTable | kLineTableCell | kLineInvisible | kLineRemoved)))
        return;
    f.usable = true;
    f.u0 = line.u0, f.u1 = line.u1, f.size = line.size;
    f.bottom = INFINITY, f.top = -INFINITY;
    std::string all;
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (g.flags & (kGlyphSpace | kGlyphInvisible | kGlyphDropped)) continue;
        const std::string_view t = page.GlyphText(g);
        const uint8_t font = g.font < page.fontMath.size() ? (page.fontMath[g.font] & kMathFontClass) : 0;
        f.glyphs++;
        if (f.glyphs <= 10) AppendText(all, t);  // the equation number test reads it up to 10 glyphs
        f.bottom = Fmin(f.bottom, g.v - 0.25f * g.size);
        f.top = Fmax(f.top, g.v + 0.72f * g.size);
        if (ClassOf(font, g.flags, t, false) == kClassStrong || (!t.empty() && t[0] == '=')) f.math++;
    }
    // Words of text: MarkDisplayFormulas reads them only of a line with math or of 3 glyphs or
    // fewer (else 0 stays).
    if (f.math > 0 || f.glyphs <= 3) {
        uint32_t wordLetters = 0;
        bool textWord = true;
        // The word's first 8 bytes and its length (counted on to 16 and past by the last glyph, as
        // a string of it would be): a function word has 2 to 6, all in the buffer.
        char word[8] = {};
        size_t wordSize = 0;
        const auto endWord = [&]() {
            if (wordLetters >= 2 && textWord && !(wordSize <= 6 && IsFunctionWord(std::string_view(word, wordSize)))) f.words++;
            wordLetters = 0, textWord = true;
            wordSize = 0;
        };
        for (uint32_t k = 0; k < line.count; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            if (g.flags & (kGlyphSpace | kGlyphInvisible | kGlyphDropped)) continue;
            if (k > 0 && (g.flags & kGlyphSpaceBefore)) endWord();
            const std::string_view t = page.GlyphText(g);
            const uint32_t cp = FirstCp(t);
            const uint8_t font = g.font < page.fontMath.size() ? (page.fontMath[g.font] & kMathFontClass) : 0;
            if (IsLetterCp(cp)) {
                wordLetters++;
                if (font != static_cast<uint8_t>(MathFont::None)) textWord = false;
                if (wordSize < 16)
                    for (const char ch : t) {
                        if (wordSize < sizeof(word)) word[wordSize] = ch;
                        ++wordSize;
                    }
            } else if (cp != '-' && cp != 0x2019 && cp != '\'') {
                endWord();
            }
        }
        endWord();
    }
    if (f.glyphs == 0) {
        f.usable = false;
        return;
    }
    // An equation number: "(12)", "(2.3a)".
    if (f.glyphs >= 3 && f.glyphs <= 10 && all.front() == '(' && all.back() == ')') {
        bool plain = true;
        for (size_t i = 1; i + 1 < all.size() && plain; ++i)
            plain = (all[i] >= '0' && all[i] <= '9') || (all[i] >= 'a' && all[i] <= 'z') || (all[i] >= 'A' && all[i] <= 'Z') || all[i] == '.';
        f.number = plain && all[1] >= '0' && all[1] <= '9';
    }
}

} // namespace

int MarkDisplayFormulas(PageData& page) {
    const size_t n = page.order.size();
    if (n == 0 || n > 20000) return 0;
    std::vector<LineFacts> facts(n);
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        FactsOf(page, page.lines[page.order[i]], facts[i]);
        any = any || (facts[i].usable && facts[i].math > 0);
    }
    if (!any) return 0;
    const auto candidate = [&](size_t i) {
        const LineFacts& f = facts[i];
        return f.usable && f.math >= 1 && (f.words == 0 || (f.words <= 2 && f.math >= 3 * f.words + 2));
    };
    std::vector<int32_t> region(n, -1);
    std::vector<std::vector<uint32_t>> regions;
    uint64_t budget = uint64_t(1) << 22;
    for (size_t seed = 0; seed < n; ++seed) {
        if (region[seed] >= 0 || !candidate(seed) || facts[seed].words > 0) continue;
        std::vector<uint32_t> members{static_cast<uint32_t>(seed)};
        region[seed] = static_cast<int32_t>(regions.size());
        LineFacts box = facts[seed];
        float em = box.size;
        bool grown = true;
        while (grown && members.size() < kMaxFormulaLines && budget > n) {
            grown = false;
            budget -= n;
            for (size_t i = 0; i < n && members.size() < kMaxFormulaLines; ++i) {
                if (region[i] >= 0 || !facts[i].usable) continue;
                const LineFacts& f = facts[i];
                em = Fmax(em, Fmin(f.size, 2 * box.size));
                const bool tiny = f.glyphs <= 3 && f.words == 0;
                const bool beside = f.top > box.bottom && f.bottom < box.top;  // on a row of the formula
                bool join = false;
                if (f.number) {
                    const float cy = (f.bottom + f.top) / 2;
                    join = cy >= box.bottom && cy <= box.top && f.u0 > box.u1;
                } else if (candidate(i) && f.words == 0) {
                    join = f.top > box.bottom - 0.6f * em && f.bottom < box.top + 0.6f * em && f.u1 > box.u0 - 1.5f * em &&
                           f.u0 < box.u1 + 1.5f * em;
                } else if (tiny) {
                    join = f.top > box.bottom - 0.3f * em && f.bottom < box.top + 0.3f * em && f.u1 > box.u0 - 0.5f * em &&
                           f.u0 < box.u1 + 0.5f * em;
                } else if (candidate(i)) {
                    // Words ("for all", "if") only beside a row of the formula.
                    join = beside && f.u1 > box.u0 - 3 * em && f.u0 < box.u1 + 3 * em;
                }
                if (!join) continue;
                region[i] = static_cast<int32_t>(regions.size());
                members.push_back(static_cast<uint32_t>(i));
                box.u0 = Fmin(box.u0, f.u0), box.u1 = Fmax(box.u1, f.u1);
                box.bottom = Fmin(box.bottom, f.bottom), box.top = Fmax(box.top, f.top);
                box.math += f.math, box.glyphs += f.glyphs;
                grown = true;
            }
        }
        // One line alone is a display only when set off from the text around it: indented from
        // the lines before and after it in reading order.
        bool keep = box.math >= 2;
        if (keep && members.size() == 1) {
            const LineFacts& f = facts[seed];
            bool indented = true;
            for (const size_t j : {seed - 1, seed + 1}) {
                if (j >= n || !facts[j].usable) continue;
                if (f.u0 < facts[j].u0 + 1.0f * f.size) indented = false;
            }
            keep = indented && f.glyphs >= 3;
        }
        if (!keep) {
            for (uint32_t m : members) region[m] = -2;  // looked at: not a display formula
            region[seed] = -2;
            continue;
        }
        std::sort(members.begin(), members.end());
        regions.push_back(std::move(members));
    }
    for (int32_t& r : region)
        if (r == -2) r = -1;
    if (regions.empty()) return 0;
    // One line per formula, where its first line was in reading order.
    std::vector<uint32_t> order;
    order.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (region[i] < 0) {
            order.push_back(page.order[i]);
            continue;
        }
        const std::vector<uint32_t>& members = regions[static_cast<size_t>(region[i])];
        if (members[0] != i) continue;
        Line merged;
        merged.first = static_cast<uint32_t>(page.lineGlyphs.size());
        merged.flags = kLineFormula;
        merged.u0 = INFINITY, merged.u1 = -INFINITY, merged.vb = INFINITY, merged.vt = -INFINITY;
        uint32_t most = 0;
        for (uint32_t m : members) {
            const Line& l = page.lines[page.order[m]];
            for (uint32_t k = 0; k < l.count; ++k) {
                const uint32_t gi = page.lineGlyphs[l.first + k];
                if (k == 0 && merged.count > 0) page.glyphs[gi].flags |= kGlyphSpaceBefore;
                page.lineGlyphs.push_back(gi);
            }
            merged.count += l.count;
            merged.chars += l.chars;
            merged.u0 = Fmin(merged.u0, l.u0), merged.u1 = Fmax(merged.u1, l.u1);
            merged.vb = Fmin(merged.vb, l.vb), merged.vt = Fmax(merged.vt, l.vt);
            if (l.count > most) most = l.count, merged.baseline = l.baseline, merged.size = l.size;
        }
        order.push_back(static_cast<uint32_t>(page.lines.size()));
        page.lines.push_back(merged);
    }
    page.order.swap(order);
    return static_cast<int>(regions.size());
}

} // namespace RayoPdf::Layout
