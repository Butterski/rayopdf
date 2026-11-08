// SPDX-License-Identifier: Apache-2.0
// Fonts as text (ISO 32000-1 sections 9.6-9.10, algorithms §3.1-§3.6). Everything a code needs is
// resolved when the font loads: a simple font gets a 256-entry table of advance, Unicode and
// source, so reading a code is one lookup; a Type0 font gets sorted tables (ToUnicode entries,
// CID ranges, /W ranges) searched in O(log n). Unicode text lives in the font's own buffers, so
// Next never allocates. One exception: a font whose trusted ToUnicode leaves codes to its embedded
// program reads the program (Font::Impl::Complete) only when a shown code needs it, so most such
// fonts never inflate it; that first Next allocates.

#include "text/font.h"

#include "common/compiler.h"
#include "common/utf8.h"
#include "text/cjk.h"
#include "text/cmap.h"
#include "text/outline.h"
#include "text/program.h"
#include "text/repair.h"
#include "text/tables.h"
#include "text/unicode_text.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RayoPdf::Text {
namespace {

constexpr size_t kCMapStreamLimit = size_t(16) << 20;
constexpr size_t kProgramStreamLimit = size_t(64) << 20;
constexpr size_t kCidToGidLimit = size_t(2) << 16;
// /W and /W2 entries read from one font: CIDs listed in arrays plus ranges.
constexpr size_t kMaxWidthEntries = size_t(1) << 20;
constexpr double kDefaultAscent = 0.8;
constexpr double kDefaultDescent = -0.2;
// Advance of a glyph nothing gives a width for, in text space (FontCache's fallback too).
constexpr double kUnknownWidth = 0.5;
// Acrobat's width of .notdef in the standard 14 fonts (PDFBox, algorithms §3.5).
constexpr double kNotdefWidth = 0.25;

// Unicode text a converter can use: not empty, no C0 control and no U+FFFD (a ToUnicode or
// glyph-name result with them counts as missing, algorithms §3.6).
bool UsableText(std::string_view text) {
    if (text.empty()) return false;
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        if (cp < 0x20 || cp == 0x7F || cp == Utf8::kReplacement) return false;
        at += length;
    }
    return true;
}

// "ABCDEF+Name" -> "Name" (a subset tag is six uppercase letters and '+').
std::string_view StripSubsetTag(std::string_view name) {
    if (name.size() > 7 && name[6] == '+') {
        bool tag = true;
        for (size_t i = 0; i < 6; ++i) tag = tag && name[i] >= 'A' && name[i] <= 'Z';
        if (tag) return name.substr(7);
    }
    return name;
}

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t j = 0;
        while (j < needle.size()) {
            char a = haystack[i + j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != needle[j]) break;
            ++j;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

// Style from a font name (algorithms §6.4, §6.6): "-Bold", ",Bold", "BoldItalic", "Semibold",
// "Black", "Heavy"; URW's "-Medi" and "-Demi" weights (NimbusRomNo9L-Medi is the Times-Bold clone
// LaTeX's times packages embed; "Medium" elsewhere is weight 500, not bold); TeX's bold extended
// "CMBX12", "SFBX1000" and "CMB10"; "Italic", "Oblique", a trailing "-It"/"It"; "Mono", "Courier",
// "Code", "Consol(as)", "Menlo".
void StyleFromName(std::string_view name, FontStyle& style) {
    if (ContainsNoCase(name, "bold") || ContainsNoCase(name, "black") || ContainsNoCase(name, "heavy")) style.bold = true;
    if (ContainsNoCase(name, "italic") || ContainsNoCase(name, "oblique")) style.italic = true;
    // "Minion-It", "Minion-BoldIt", "MinionPro-SemiboldIt".
    const size_t dash = name.find_last_of("-,");
    if (dash != std::string_view::npos) {
        const std::string_view suffix = name.substr(dash + 1);
        if (suffix.size() >= 2 && suffix.substr(suffix.size() - 2) == "It") style.italic = true;
        // "NimbusRomNo9L-Medi", "-MediItal", "URWBookmanL-DemiBold", "AvantGarde-Demi"; never
        // "NotoSansCJK-DemiLight".
        if (suffix == "Medi" || suffix.substr(0, 8) == "MediItal" ||
            (suffix.substr(0, 4) == "Demi" && !ContainsNoCase(suffix, "light")))
            style.bold = true;
    }
    if (name.size() >= 4 && (name.substr(0, 4) == "CMBX" || name.substr(0, 4) == "SFBX" ||
                             (name.substr(0, 3) == "CMB" && name[3] >= '0' && name[3] <= '9')))
        style.bold = true;
    if (ContainsNoCase(name, "mono") || ContainsNoCase(name, "courier") || ContainsNoCase(name, "code") ||
        ContainsNoCase(name, "consol") || ContainsNoCase(name, "menlo")) {
        style.monospace = true;
    }
    if (ContainsNoCase(name, "symbol") || ContainsNoCase(name, "dingbat") || ContainsNoCase(name, "wingding")) {
        style.symbolic = true;
    }
}

// Descriptor flags (Table 123).
constexpr int64_t kFixedPitch = 1;
constexpr int64_t kSerif = 2;
constexpr int64_t kSymbolic = 4;
constexpr int64_t kNonsymbolic = 32;
constexpr int64_t kItalic = 64;
constexpr int64_t kForceBold = int64_t(1) << 18;

struct Descriptor {
    int64_t flags = 0;
    bool hasFlags = false;
    double ascent = 0;
    double descent = 0;
    double missingWidth = 0;
    bool hasBBox = false;
    PdfRect bbox;
    const PdfValue* fontFile = nullptr;   // the program stream and its kind
    int fontFileKind = 0;                 // 1 FontFile, 2 FontFile2, 3 FontFile3
    std::string_view fontFile3Subtype;
};

Descriptor ReadDescriptor(const Document& doc, const PdfValue& dict, FontStyle& style) {
    Descriptor d;
    if (!dict.IsDict()) return d;
    const PdfValue& flags = doc.Get(dict, "Flags");
    if (flags.IsNumber()) {
        d.flags = flags.Int();
        d.hasFlags = true;
        if (d.flags & kFixedPitch) style.monospace = true;
        if (d.flags & kSerif) style.serif = true;
        if ((d.flags & kSymbolic) && !(d.flags & kNonsymbolic)) style.symbolic = true;
        if (d.flags & kItalic) style.italic = true;
        if (d.flags & kForceBold) style.bold = true;
    }
    double value = 0;
    if (doc.Number(doc.Get(dict, "FontWeight"), value) && value >= 600) style.bold = true;
    if (doc.Number(doc.Get(dict, "ItalicAngle"), value) && std::fabs(value) >= 1) style.italic = true;
    if (doc.Number(doc.Get(dict, "Ascent"), value)) d.ascent = value;
    if (doc.Number(doc.Get(dict, "Descent"), value)) d.descent = value;
    if (doc.Number(doc.Get(dict, "MissingWidth"), value)) d.missingWidth = value;
    d.hasBBox = doc.Rect(doc.Get(dict, "FontBBox"), d.bbox);
    static const char* const kFileKeys[3] = {"FontFile", "FontFile2", "FontFile3"};
    for (int i = 0; i < 3; ++i) {
        const PdfValue& file = doc.Get(dict, kFileKeys[i]);
        if (file.IsStream()) {
            d.fontFile = &file;
            d.fontFileKind = i + 1;
            if (i == 2) d.fontFile3Subtype = doc.Get(file, "Subtype").Name();
            break;
        }
    }
    return d;
}

// Ascent and descent at size 1: the descriptor's values, else its bounding box, else `fallback`.
void Extents(const Descriptor& d, double ascentFallback, double descentFallback, double& ascent, double& descent) {
    ascent = ascentFallback;
    descent = descentFallback;
    if (d.ascent > 0 && d.ascent < 3000) ascent = d.ascent / 1000;
    else if (d.hasBBox && d.bbox.y1 > 0 && d.bbox.y1 < 3000) ascent = d.bbox.y1 / 1000;
    if (d.descent < 0 && d.descent > -3000) descent = d.descent / 1000;
    else if (d.hasBBox && d.bbox.y0 < 0 && d.bbox.y0 > -3000) descent = d.bbox.y0 / 1000;
}

bool EncodingByName(std::string_view name, Tables::Encoding& out) {
    if (name == "WinAnsiEncoding") out = Tables::Encoding::WinAnsi;
    else if (name == "MacRomanEncoding") out = Tables::Encoding::MacRoman;
    else if (name == "StandardEncoding") out = Tables::Encoding::Standard;
    else if (name == "MacExpertEncoding") out = Tables::Encoding::MacExpert;
    else return false;
    return true;
}

std::string_view EncodingName(Tables::Encoding encoding, uint32_t code) {
    if (code > 255) return {};
    const uint16_t index = Tables::kEncodings[static_cast<int>(encoding)][code];
    return index == Tables::kNoName ? std::string_view() : Tables::GlyphName(index);
}

// The glyph-name text (GlyphNameToUnicode) of each code of a base encoding, and whether it is
// usable: the same for every font of that encoding, so it is made once per process (per encoding
// and name rule, on first use) instead of resolving the 256 names again for every font.
struct EncodingTexts {
    std::string pool;
    uint32_t offset[256] = {};
    uint16_t length[256] = {};
    bool named[256] = {};   // the code has a name and GlyphNameToUnicode gave it text
    bool usable[256] = {};  // and UsableText accepts that text
    std::string_view Text(int code) const { return std::string_view(pool.data() + offset[code], length[code]); }
};

EncodingTexts MakeEncodingTexts(Tables::Encoding encoding, bool zapf) {
    EncodingTexts out;
    std::string text;
    for (int c = 0; c < 256; ++c) {
        const std::string_view name = EncodingName(encoding, static_cast<uint32_t>(c));
        text.clear();
        if (name.empty() || !GlyphNameToUnicode(name, zapf, text)) continue;
        out.named[c] = true;
        out.usable[c] = UsableText(text);
        out.offset[c] = static_cast<uint32_t>(out.pool.size());
        out.length[c] = static_cast<uint16_t>(text.size());
        out.pool += text;
    }
    return out;
}

template <int kEncoding, bool kZapf>
const EncodingTexts& CachedEncodingTexts() {
    static const EncodingTexts texts = MakeEncodingTexts(static_cast<Tables::Encoding>(kEncoding), kZapf);
    return texts;
}

template <bool kZapf>
const EncodingTexts& EncodingTextsFor(Tables::Encoding encoding) {
    static_assert(Tables::kEncodingCount == 7, "one case per encoding");
    switch (encoding) {
    case Tables::Encoding::Standard: return CachedEncodingTexts<0, kZapf>();
    case Tables::Encoding::MacRoman: return CachedEncodingTexts<1, kZapf>();
    case Tables::Encoding::WinAnsi: return CachedEncodingTexts<2, kZapf>();
    case Tables::Encoding::MacExpert: return CachedEncodingTexts<3, kZapf>();
    case Tables::Encoding::Symbol: return CachedEncodingTexts<4, kZapf>();
    case Tables::Encoding::ZapfDingbats: return CachedEncodingTexts<5, kZapf>();
    case Tables::Encoding::CffExpert: break;
    }
    return CachedEncodingTexts<6, kZapf>();
}

const EncodingTexts& EncodingTextsFor(Tables::Encoding encoding, bool zapf) {
    return zapf ? EncodingTextsFor<true>(encoding) : EncodingTextsFor<false>(encoding);
}

// The standard 14 font whose metrics stand in for a font without /Widths: by name or alias, else
// by family words in the name (Courier/Mono, Times/Serif, Symbol, Dingbats, else Helvetica) with
// the style's bold/italic variant.
int StandardFontFor(std::string_view name, const FontStyle& style) {
    const int exact = Tables::StandardFontByName(name);
    if (exact >= 0) return exact;
    if (ContainsNoCase(name, "dingbat")) return Tables::kZapfDingbats;
    if (ContainsNoCase(name, "symbol")) return Tables::kSymbol;
    int family = Tables::kHelvetica;
    if (style.monospace) family = Tables::kCourier;
    else if (style.serif || ContainsNoCase(name, "times")) family = Tables::kTimesRoman;
    if (family == Tables::kTimesRoman) {
        // Times-Roman, -Bold, -Italic, -BoldItalic.
        return family + (style.bold ? 1 : 0) + (style.italic ? 2 : 0);
    }
    // Courier / Helvetica: plain, -Bold, -Oblique, -BoldOblique.
    return family + (style.bold && style.italic ? 3 : style.italic ? 2 : style.bold ? 1 : 0);
}

// The name of a standard 14 font as BaseName gives it.
bool IsSymbolName(std::string_view name) {
    return name.substr(0, 6) == "Symbol";
}

// A simple font's code (font.h): Pool texts are in Font::Impl::pool, LatePool ones in latePool
// (set by Font::Impl::Complete), Pending ones wait for Complete.
using SlotState = FontSlotState;
using Slot = FontSlot;

struct TextRef {
    uint32_t offset = 0;
    uint32_t length = 0;
};

struct WidthRange {
    uint32_t low = 0;
    uint32_t high = 0;
    float width = 0;
};

struct VerticalRange {
    uint32_t low = 0;
    uint32_t high = 0;
    float advance = 0;  // w1y
};

// What completing a font after loading needs (Font::Impl::Complete), defined with the loaders.
struct SimpleLoad;
struct Type0Load;

// How a Type0 font's codes become CIDs and, for the Unicode-based CMaps, text. Predefined: a
// legacy predefined CMap of the generated CJK tables (90ms-RKSJ-H, GBK-EUC-H, ...); Other: a CMap
// name nothing knows, read with 2-byte codes and no CIDs.
enum class CMapKind : uint8_t { Identity, Embedded, Predefined, Ucs2, Utf16, Utf8, Utf32, Other };

constexpr uint32_t kNoCodePoint = UINT32_MAX;

// The code point a code of a Unicode-based CMap stands for (kNoCodePoint when the code is not a
// whole, valid one). `length` 0 means unknown (a ToUnicode entry): taken from the code's value.
uint32_t UnicodeCode(CMapKind kind, uint32_t code, uint8_t length) {
    switch (kind) {
    case CMapKind::Ucs2:
        if (length == 2 || (length == 0 && code <= 0xFFFF)) return code;
        [[fallthrough]];  // a surrogate pair written into a UCS-2 string (NextComposite joins them)
    case CMapKind::Utf16:
        if (length == 2 || (length == 0 && code <= 0xFFFF))
            return code >= 0xD800 && code <= 0xDFFF ? kNoCodePoint : code;
        if (length == 4 || length == 0) {
            const uint32_t high = code >> 16, low = code & 0xFFFF;
            if (high < 0xD800 || high > 0xDBFF || low < 0xDC00 || low > 0xDFFF) return kNoCodePoint;
            return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
        }
        return kNoCodePoint;
    case CMapKind::Utf32:
        return (length == 4 || length == 0) && code <= 0x10FFFF && !(code >= 0xD800 && code <= 0xDFFF) ? code
                                                                                                       : kNoCodePoint;
    case CMapKind::Utf8: {
        // The code bytes are UTF-8: re-read them as such.
        if (length == 0) length = code > 0xFFFFFF ? 4 : code > 0xFFFF ? 3 : code > 0xFF ? 2 : 1;
        if (length > 4) return kNoCodePoint;
        char buffer[4];
        for (uint8_t i = 0; i < length; ++i) buffer[i] = static_cast<char>(code >> (8 * (length - 1 - i)));
        size_t used = 0;
        uint32_t cp = 0;
        return Utf8::Decode(std::string_view(buffer, length), 0, cp, used) && used == length ? cp : kNoCodePoint;
    }
    default:
        return kNoCodePoint;
    }
}

CMapKind KindOfEncoding(Cjk::CMapEncoding encoding) {
    switch (encoding) {
    case Cjk::CMapEncoding::Ucs2: return CMapKind::Ucs2;
    case Cjk::CMapEncoding::Utf16: return CMapKind::Utf16;
    case Cjk::CMapEncoding::Utf32: return CMapKind::Utf32;
    case Cjk::CMapEncoding::Utf8: return CMapKind::Utf8;
    default: return CMapKind::Predefined;
    }
}

} // namespace

struct Font::Impl {
    FontKind kind = FontKind::Unknown;
    std::string baseName;
    FontStyle style;
    bool vertical = false;
    double matrix[6] = {0.001, 0, 0, 0.001, 0, 0};
    double ascent = kDefaultAscent;
    double descent = kDefaultDescent;
    double spaceWidth = 0;
    bool unmappable = false;
    std::string pool;  // UTF-8 text the tables point into
    const PdfValue* dict = nullptr;  // the font dictionary (FontGlyphs)

    // Simple fonts (Type1, MMType1, TrueType, Type3, and the fallback).
    Slot slots[256];

    // Type0.
    CMapKind cmapKind = CMapKind::Identity;
    EncodingCMap cmap;                 // Embedded: codespace and CID ranges; Predefined..Other: codespace only
    int predefined = -1;               // the generated predefined CMap (Cjk::FindPredefined): the
                                       // encoding itself, or the usecmap of an embedded CMap
    Cjk::Collection collection = Cjk::Collection::None;  // whose CID -> Unicode applies
    bool toUnicodeUntrusted = false;   // the ToUnicode looks wrong: it comes after collection and program
    bool glyphTextGuessed = false;     // glyphText is a heuristic (Macintosh glyph order): Guess source
    ToUnicodeMap toUnicode;            // entries filtered to usable text
    std::vector<uint16_t> cidToGid;    // empty: CID = glyph id
    std::vector<TextRef> glyphText;    // by glyph id, into `pool` (FontProgram)
    std::vector<WidthRange> widths;    // sorted by low
    float defaultWidth = 1.0f;
    std::vector<VerticalRange> verticals;
    float defaultVertical = -1.0f;

    // A font with a trusted ToUnicode reads its embedded program only when a shown code needs it
    // (one the ToUnicode lacks; SpaceWidth): until then the loader's state waits here, and a
    // simple font's slots that the ToUnicode does not fill are Pending.
    std::unique_ptr<SimpleLoad> pendingSimple;
    std::unique_ptr<Type0Load> pendingType0;
    std::string latePool;  // simple fonts: texts set by Complete (views into `pool` stay valid)
    bool late = false;     // SetSlot writes into latePool

    bool NextSimple(std::string_view bytes, size_t& at, Code& out) const;
    bool NextComposite(std::string_view bytes, size_t& at, Code& out) const;
    float WidthOf(uint32_t cid) const;
    float VerticalOf(uint32_t cid) const;
    bool CidOf(uint32_t code, uint8_t length, uint32_t& cid) const;
    bool PredefinedCid(uint32_t code, uint8_t length, uint32_t& cid) const;
    void Composite(uint32_t code, uint8_t length, Code& out) const;
    std::string_view SlotText(const Slot& slot) const {
        return std::string_view((slot.state == SlotState::LatePool ? latePool : pool).data() + slot.offset, slot.length);
    }
    // Reads what a pending font waits for; a Font is used by one thread, so a const reader may
    // complete it (the Impl itself is never const).
    void Complete();
    void CompleteIfPending() const {
        if (RAYOPDF_UNLIKELY(pendingSimple || pendingType0)) const_cast<Impl*>(this)->Complete();
    }
};

namespace {

template <class Range>
const Range* FindByCid(const std::vector<Range>& ranges, uint32_t cid) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), cid, [](uint32_t c, const Range& r) { return c < r.low; });
    if (it == ranges.begin()) return nullptr;
    --it;
    return cid <= it->high ? &*it : nullptr;
}

} // namespace

float Font::Impl::WidthOf(uint32_t cid) const {
    const WidthRange* range = FindByCid(widths, cid);
    return range ? range->width : defaultWidth;
}

float Font::Impl::VerticalOf(uint32_t cid) const {
    const VerticalRange* range = FindByCid(verticals, cid);
    return range ? range->advance : defaultVertical;
}

bool Font::Impl::PredefinedCid(uint32_t code, uint8_t length, uint32_t& cid) const {
    if (predefined < 0) return false;
    // A Unicode-based font CMap reads its own codes (its predefined CMap may be a borrowed UTF32
    // one); an embedded CMap's usecmap reads codes its own way.
    const bool unicodeFont = cmapKind == CMapKind::Ucs2 || cmapKind == CMapKind::Utf16 || cmapKind == CMapKind::Utf8 ||
                             cmapKind == CMapKind::Utf32;
    const CMapKind kind = unicodeFont ? cmapKind : KindOfEncoding(Cjk::EncodingOf(predefined));
    const uint32_t key = kind == CMapKind::Predefined ? code : UnicodeCode(kind, code, length);
    return key != kNoCodePoint && Cjk::Cid(predefined, key, cid);
}

bool Font::Impl::CidOf(uint32_t code, uint8_t length, uint32_t& cid) const {
    cid = 0;
    switch (cmapKind) {
    case CMapKind::Identity:
        cid = code;
        return true;
    case CMapKind::Embedded:
        if (cmap.cids.empty() && cmap.notdefs.empty()) {
            cid = code;  // only a codespace (or usecmap of an Identity CMap): codes are CIDs
            return true;
        }
        if (cmap.Cid(code, cid)) return true;
        if (PredefinedCid(code, length, cid)) return true;  // the usecmap's mapping
        cid = 0;
        return true;
    case CMapKind::Predefined:
    case CMapKind::Ucs2:
    case CMapKind::Utf16:
    case CMapKind::Utf8:
    case CMapKind::Utf32:
        return PredefinedCid(code, length, cid);
    default:
        return false;  // an unknown CMap: CIDs unknown
    }
}

bool Font::Impl::NextSimple(std::string_view bytes, size_t& at, Code& out) const {
    if (at >= bytes.size()) return false;
    const uint8_t c = static_cast<uint8_t>(bytes[at++]);
    if (RAYOPDF_UNLIKELY(slots[c].state == SlotState::Pending)) const_cast<Impl*>(this)->Complete();
    const Slot& slot = slots[c];
    out.code = c;
    out.bytes = 1;
    out.width = slot.width;
    out.verticalAdvance = 0;
    out.wordSpace = c == 32;
    out.source = slot.source;
    out.unicode = SlotText(slot);
    return true;
}

void Font::Impl::Composite(uint32_t code, uint8_t length, Code& out) const {
    out.code = code;
    out.bytes = length;
    out.wordSpace = length == 1 && code == 32;
    uint32_t cid = 0;
    const bool known = CidOf(code, length, cid);
    out.width = known ? WidthOf(cid) : defaultWidth;
    out.verticalAdvance = vertical ? (known ? VerticalOf(cid) : defaultVertical) : 0;
    if (cmapKind == CMapKind::Ucs2 && length == 4) {
        // A joined surrogate pair advances as the two UCS-2 codes the font shows.
        uint32_t half = 0;
        const float high = CidOf(code >> 16, 2, half) ? WidthOf(half) : defaultWidth;
        const float low = CidOf(code & 0xFFFF, 2, half) ? WidthOf(half) : defaultWidth;
        out.width = high + low;
        if (vertical) out.verticalAdvance = 2 * defaultVertical;
    }
    const UnicodeEntry* entry = toUnicode.Find(code);
    if (entry && !toUnicodeUntrusted) {
        out.source = UnicodeSource::ToUnicode;
        out.unicode = toUnicode.Text(*entry);
        return;
    }
    // A Unicode-based CMap: the code is the text (beyond planes 0-2 through the collection, whose
    // table has storage for it).
    const uint32_t cp = UnicodeCode(cmapKind, code, length);
    if (cp != kNoCodePoint) {
        std::string_view text = CodePointText(cp);
        if (text.empty() && known && Cjk::CidToUnicode(collection, cid) == cp) text = Cjk::CidText(collection, cid);
        if (UsableText(text)) {
            out.source = UnicodeSource::Collection;
            out.unicode = text;
            return;
        }
    }
    // The character collection of the CID (algorithms §3.1 step 3).
    if (known && collection != Cjk::Collection::None) {
        const std::string_view text = Cjk::CidText(collection, cid);
        if (UsableText(text)) {
            out.source = UnicodeSource::Collection;
            out.unicode = text;
            return;
        }
    }
    // The program's glyph texts come after the ToUnicode and the collection: read now if pending.
    if (known && RAYOPDF_UNLIKELY(pendingType0 != nullptr)) const_cast<Impl*>(this)->Complete();
    if (known && !glyphText.empty()) {
        const uint32_t glyph = cidToGid.empty() ? cid : (cid < cidToGid.size() ? cidToGid[cid] : 0);
        if (glyph < glyphText.size() && glyphText[glyph].length > 0) {
            out.source = glyphTextGuessed ? UnicodeSource::Guess : UnicodeSource::FontProgram;
            out.unicode = std::string_view(pool.data() + glyphText[glyph].offset, glyphText[glyph].length);
            return;
        }
    }
    if (entry) {
        // An untrustworthy ToUnicode, with nothing better: kept, as a guess.
        out.source = UnicodeSource::Guess;
        out.unicode = toUnicode.Text(*entry);
        return;
    }
    out.source = UnicodeSource::None;
    out.unicode = {};
}

bool Font::Impl::NextComposite(std::string_view bytes, size_t& at, Code& out) const {
    if (at >= bytes.size()) return false;
    uint32_t code = 0;
    uint8_t length = 0;
    if (cmapKind == CMapKind::Identity) {
        length = bytes.size() - at >= 2 ? 2 : 1;
        code = static_cast<uint8_t>(bytes[at]);
        if (length == 2) code = code << 8 | static_cast<uint8_t>(bytes[at + 1]);
    } else {
        length = cmap.codespace.Read(bytes, at, code);
        if (length == 0) length = 1;
        // UCS-2 strings with UTF-16 surrogate pairs (Acrobat writes emoji so): one code of 4 bytes.
        if (cmapKind == CMapKind::Ucs2 && length == 2 && code >= 0xD800 && code <= 0xDBFF && at + 4 <= bytes.size()) {
            const uint32_t low = static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 2])) << 8 |
                                 static_cast<uint8_t>(bytes[at + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = code << 16 | low;
                length = 4;
            }
        }
    }
    at += length;
    Composite(code, length, out);
    return true;
}

bool Font::NextSlow(std::string_view bytes, size_t& at, Code& out) const {
    return impl_->kind == FontKind::Type0 ? impl_->NextComposite(bytes, at, out) : impl_->NextSimple(bytes, at, out);
}
FontKind Font::Kind() const { return impl_->kind; }
std::string_view Font::BaseName() const { return impl_->baseName; }
const FontStyle& Font::Style() const { return impl_->style; }
bool Font::Vertical() const { return impl_->vertical; }
const double* Font::Matrix() const { return impl_->matrix; }
double Font::Ascent() const { return impl_->ascent; }
double Font::Descent() const { return impl_->descent; }
double Font::SpaceWidth() const {
    impl_->CompleteIfPending();
    return impl_->spaceWidth;
}
// A pending font has a trusted ToUnicode, so it is never unmappable: nothing to complete.
bool Font::Unmappable() const { return impl_->unmappable; }

const PdfValue& Font::Dictionary() const {
    static const PdfValue kNull{};
    return impl_->dict ? *impl_->dict : kNull;
}

bool Font::Cid(uint32_t code, uint8_t bytes, uint32_t& cid) const {
    return impl_->kind == FontKind::Type0 && impl_->CidOf(code, bytes, cid);
}

namespace {

// Appends `text` to the pool; its reference.
TextRef Pool(std::string& pool, std::string_view text) {
    TextRef ref{static_cast<uint32_t>(pool.size()), static_cast<uint32_t>(text.size())};
    pool.append(text.data(), text.size());
    return ref;
}

void SetSlot(Font::Impl& font, uint8_t code, std::string_view text, UnicodeSource source) {
    const TextRef ref = Pool(font.late ? font.latePool : font.pool, text);
    Slot& slot = font.slots[code];
    slot.offset = ref.offset;
    slot.length = static_cast<uint16_t>(text.size() < 0xFFFF ? text.size() : 0xFFFF);
    slot.source = source;
    slot.state = font.late ? SlotState::LatePool : SlotState::Pool;
}

// Only the usable entries of a parsed ToUnicode map stay (algorithms §3.6).
void FilterToUnicode(ToUnicodeMap& map) {
    size_t kept = 0;
    for (const UnicodeEntry& entry : map.entries) {
        if (UsableText(map.Text(entry))) map.entries[kept++] = entry;
    }
    map.entries.resize(kept);
}

// Repairs every entry's text (Repair::RepairText; Thai forms when the map has Thai) and gathers
// the statistics that judge the map, before unusable entries are dropped.
void RepairToUnicode(ToUnicodeMap& map, Repair::Context context, Repair::MapStats& stats) {
    Repair::MapStats first;
    for (const UnicodeEntry& entry : map.entries) Repair::Count(map.Text(entry), first);
    context.thai = first.thai;
    std::string repaired;
    bool repairedAny = false;
    for (UnicodeEntry& entry : map.entries) {
        if (!Repair::RepairText(map.Text(entry), context, repaired)) continue;
        if (map.text.size() + repaired.size() > UINT32_MAX) break;
        entry.offset = static_cast<uint32_t>(map.text.size());
        entry.length = static_cast<uint32_t>(repaired.size());
        map.text += repaired;
        repairedAny = true;
    }
    // The texts as counted first when none was repaired (Count only adds up).
    if (!repairedAny) {
        stats.entries += first.entries;
        stats.junk += first.junk;
        stats.latinLetters += first.latinLetters;
        stats.accented += first.accented;
        stats.thai = stats.thai || first.thai;
    }
    // The most codes one text has (a map of many codes onto one character): equal texts counted
    // in an open-addressing table, so a CID font's map of tens of thousands of codes costs one
    // pass instead of a sort.
    size_t capacity = 16;
    while (capacity < map.entries.size() * 2) capacity <<= 1;
    const size_t mask = capacity - 1;
    constexpr size_t kFree = SIZE_MAX;
    std::vector<size_t> owners(capacity, kFree);  // the first entry with the slot's text
    std::vector<uint32_t> counts(map.entries.size(), 0);
    for (size_t e = 0; e < map.entries.size(); ++e) {
        const std::string_view text = map.Text(map.entries[e]);
        if (repairedAny) Repair::Count(text, stats);
        if (text == " ") continue;
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
        size_t slot = static_cast<size_t>(hash) & mask;
        while (owners[slot] != kFree && map.Text(map.entries[owners[slot]]) != text) slot = (slot + 1) & mask;
        if (owners[slot] == kFree) owners[slot] = e;
        stats.largestShare = std::max(stats.largestShare, ++counts[owners[slot]]);
    }
}

// Reads, repairs and judges a font's ToUnicode. `dropped` (when not null) receives the number of
// entries dropped as unusable, `untrusted` whether the map as a whole looks wrong.
bool LoadToUnicode(const Document& doc, const PdfValue& dict, const Repair::Context& context, ToUnicodeMap& out,
                   size_t* dropped, bool& untrusted) {
    untrusted = false;
    const PdfValue& stream = doc.Get(dict, "ToUnicode");
    if (!stream.IsStream()) return false;
    std::string data;
    const StreamStatus status = doc.StreamData(stream, data, kCMapStreamLimit);
    if (status == StreamStatus::Corrupt || status == StreamStatus::NotStream) return false;
    ParseToUnicode(data, out);
    Repair::MapStats stats;
    RepairToUnicode(out, context, stats);
    untrusted = Repair::Untrusted(stats);
    const size_t parsed = out.entries.size();
    FilterToUnicode(out);
    if (dropped) *dropped = parsed - out.entries.size();
    return !out.entries.empty();
}

// Repairs program-derived glyph texts in place (Thai forms when some glyph is Thai).
void RepairGlyphTexts(std::vector<std::string>& texts, Repair::Context context) {
    Repair::MapStats stats;
    for (const std::string& t : texts) {
        if (!t.empty()) Repair::Count(t, stats);
    }
    context.thai = stats.thai;
    std::string repaired;
    for (std::string& t : texts) {
        if (!t.empty() && Repair::RepairText(t, context, repaired)) t = repaired;
    }
}

bool LoadProgram(const Document& doc, const Descriptor& d, std::string& data) {
    if (!d.fontFile) return false;
    const StreamStatus status = doc.StreamData(*d.fontFile, data, kProgramStreamLimit);
    return status == StreamStatus::Ok || status == StreamStatus::Salvaged || status == StreamStatus::TooLarge;
}

// The base font name of a font dictionary, subset tag removed.
std::string BaseNameOf(const Document& doc, const PdfValue& dict) {
    std::string_view name = doc.Get(dict, "BaseFont").Name();
    if (name.empty()) name = doc.Get(dict, "BaseFont").String();
    name = StripSubsetTag(name);
    return std::string(name.data(), name.size());
}

void FinishSpaceWidthSimple(Font::Impl& font) {
    const Slot& space = font.slots[32];
    if (font.SlotText(space) == " " && space.width > 0) {
        font.spaceWidth = space.width;
        return;
    }
    for (const Slot& slot : font.slots) {
        if (font.SlotText(slot) == " " && slot.width > 0) {
            font.spaceWidth = slot.width;
            return;
        }
    }
    font.spaceWidth = 0;
}

// The glyph text of a TrueType font program: inverted Unicode cmap, then 'post' names.
void GlyphTextFromSfnt(const Program::Sfnt& sfnt, uint32_t glyphCount, std::vector<std::string>& out) {
    out.assign(glyphCount, std::string());
    std::vector<uint32_t> unicode;
    Program::InvertUnicodeCmap(sfnt, glyphCount, unicode);
    for (uint32_t g = 0; g < glyphCount; ++g) {
        if (unicode[g] != 0 && !(unicode[g] >= 0xD800 && unicode[g] <= 0xDFFF)) Utf8::Append(out[g], unicode[g]);
        if (!UsableText(out[g])) out[g].clear();
    }
    Program::Names post;
    if (Program::ReadPostNames(sfnt.post, glyphCount, post)) {
        for (uint32_t g = 1; g < glyphCount && g < post.Count(); ++g) {
            if (!out[g].empty()) continue;
            std::string text;
            if (GlyphNameToUnicode(post.Name(g), text) && UsableText(text)) out[g] = std::move(text);
        }
    }
}

uint32_t SfntGlyphCount(const Program::Sfnt& sfnt) {
    uint32_t count = sfnt.glyphCount;
    if (count == 0) count = 0x10000;  // no 'maxp': every glyph id a cmap can name
    return count;
}

// ---- Simple fonts -----------------------------------------------------------------------------

// The value of a numeric glyph name in one of pdf.js's forms (algorithms §3.1): "65" (decimal),
// "C65"/"c65" (decimal), "G41"/"g41" (two hex digits); -1 for any other name.
int NumericNameValue(std::string_view name) {
    int base = 10;
    if (name.size() >= 2 && (name[0] == 'C' || name[0] == 'c')) {
        name.remove_prefix(1);
    } else if (name.size() == 3 && (name[0] == 'G' || name[0] == 'g')) {
        name.remove_prefix(1);
        base = 16;
    }
    if (name.empty() || name.size() > 3) return -1;
    int value = 0;
    for (const char ch : name) {
        int digit = -1;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (base == 16 && ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else if (base == 16 && ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        if (digit < 0) return -1;
        value = value * base + digit;
    }
    return value;
}

// Glyph names that mean nothing in the Adobe Glyph List but are numbers ("/45 /50 /103", a Type 3
// font's names as character codes; pdf.js's Gxx and Cdd forms): read as Latin-1 when nearly all of
// the font's unresolved names are numbers and every number is a printable Latin-1 code, as guesses
// (algorithms §3.1; Adobe Reader does not do this).
void NumericGlyphNames(const std::string_view (&names)[256], Font::Impl& font) {
    int values[256];
    size_t unresolved = 0, numeric = 0, letters = 0;
    for (int c = 0; c < 256; ++c) {
        values[c] = -1;
        if (names[c].empty() || font.slots[c].source != UnicodeSource::None) continue;
        ++unresolved;
        const int v = NumericNameValue(names[c]);
        if (v < 0) continue;
        if (v < 0x20 || (v >= 0x7F && v < 0xA0) || v > 0xFF) return;  // codes, not characters
        values[c] = v;
        ++numeric;
        if ((v | 0x20) >= 'a' && (v | 0x20) <= 'z') ++letters;
    }
    if (numeric < 4 || numeric * 10 < unresolved * 9 || letters * 4 < numeric) return;
    std::string text;
    for (int c = 0; c < 256; ++c) {
        if (values[c] < 0) continue;
        text.clear();
        Utf8::Append(text, static_cast<uint32_t>(values[c]));
        SetSlot(font, static_cast<uint8_t>(c), text, UnicodeSource::Guess);
    }
}

// The glyph names of a simple font's 256 codes and where each came from (§9.6.6): the base
// encoding (named, or the program's built-in one, or the default for the font's kind), then
// /Differences. LoadSimple reads Unicode from them; FontGlyphs finds glyphs by them.
struct SimpleNames {
    int standard = -1;              // Tables::StandardFontByName of the base name
    bool symbolName = false, zapf = false, symbolic = false;
    std::string_view names[256];
    UnicodeSource nameSource[256];
    bool fromBase[256] = {};        // names[c] is the base encoding's
    const PdfValue* encoding = nullptr;  // the /Encoding entry
    Tables::Encoding base = Tables::Encoding::Standard;
    bool hasBase = false;
    bool builtin = false;           // the names are the program's built-in encoding
    bool programNames = false;      // the base names come from the program (ReadBaseNames)
    size_t differences = 0;         // /Differences entries naming a glyph
    bool differenced[256] = {};     // names[c] is a /Differences name
    Program::Type1Encoding type1;   // storage of built-in names
    Program::CffInfo cff;
    std::string program;            // the font program, once read
    bool programLoaded = false;

    std::string_view ProgramData(const Document& doc, const Descriptor& desc) {
        if (!programLoaded) {
            programLoaded = true;
            if (!LoadProgram(doc, desc, program)) program.clear();
        }
        return program;
    }
};

// The names /Encoding declares: which base encoding applies (or that the program's built-in one
// does) and the /Differences names, which override the base names. Reads no font program.
void ReadDeclaredNames(const Document& doc, const PdfValue& dict, FontKind kind, const std::string& baseName,
                       const Descriptor& desc, SimpleNames& out) {
    out.standard = Tables::StandardFontByName(baseName);
    out.symbolName = IsSymbolName(baseName) || out.standard == Tables::kSymbol;
    out.zapf = out.standard == Tables::kZapfDingbats || ContainsNoCase(baseName, "dingbats");
    out.symbolic = desc.hasFlags ? (desc.flags & kSymbolic) != 0 && !(desc.flags & kNonsymbolic)
                                 : out.symbolName || out.zapf;
    const bool symbolName = out.symbolName, zapf = out.zapf, symbolic = out.symbolic;
    for (auto& s : out.nameSource) s = UnicodeSource::Encoding;
    const PdfValue& encoding = doc.Get(dict, "Encoding");
    out.encoding = &encoding;
    Tables::Encoding& base = out.base;
    bool& hasBase = out.hasBase;
    if (encoding.IsName()) {
        hasBase = EncodingByName(encoding.Name(), base);
    } else if (encoding.IsDict()) {
        hasBase = EncodingByName(doc.Get(encoding, "BaseEncoding").Name(), base);
    }
    if (!hasBase) {
        if (symbolName) {
            base = Tables::Encoding::Symbol, hasBase = true;
        } else if (zapf) {
            base = Tables::Encoding::ZapfDingbats, hasBase = true;
        } else if ((kind == FontKind::Type1 || kind == FontKind::MMType1) && desc.fontFile &&
                   (symbolic || !encoding.IsName())) {
            out.programNames = true;  // the program's built-in encoding (§9.6.6.2)
        } else if (kind == FontKind::TrueType) {
            // pdf.js: a nonsymbolic TrueType font without an encoding reads as WinAnsi; a
            // symbolic one goes through its cmap below.
            if (!symbolic) base = Tables::Encoding::WinAnsi, hasBase = true;
        } else if (kind != FontKind::Type3) {
            hasBase = true;  // Standard
        }
    }
    // /Differences: [code /name /name ... code /name ...]
    if (encoding.IsDict()) {
        const PdfSpan<PdfValue> diff = doc.Get(encoding, "Differences").Array();
        int64_t code = -1;
        for (const PdfValue& item : diff) {
            const PdfValue& v = doc.Resolve(item);
            if (v.IsNumber()) {
                code = v.Int(-1);
            } else if (v.IsName() && code >= 0 && code < 256) {
                if (v.Name() != ".notdef") {
                    out.names[code] = v.Name();
                    out.differenced[code] = true;
                    ++out.differences;
                }
                ++code;
            } else if (v.IsName()) {
                ++code;
            }
        }
    }
}

// The base names of the codes /Differences leaves: the program's built-in encoding (Type 1 clear
// text or CFF; the program is read here) or the base encoding.
void ReadBaseNames(const Document& doc, const Descriptor& desc, SimpleNames& out) {
    Program::Type1Encoding& type1 = out.type1;
    Program::CffInfo& cff = out.cff;
    bool& builtin = out.builtin;
    if (out.programNames) {
        const std::string_view data = out.ProgramData(doc, desc);
        if (desc.fontFileKind == 1 && Program::ReadType1Encoding(data, type1)) {
            builtin = !type1.standard;
        } else if (desc.fontFileKind == 3) {
            std::string_view table = data;
            Program::Sfnt sfnt;
            if (Program::ReadSfnt(data, sfnt) && !sfnt.cff.empty()) table = sfnt.cff;
            if (Program::ReadCff(table, cff) && !cff.cidKeyed) {
                if (cff.encoding == 1) out.base = Tables::Encoding::CffExpert, out.hasBase = true;
                else builtin = cff.encoding == 2;
            }
        }
        if (!builtin && !out.hasBase) out.hasBase = true;  // Standard
    }
    for (int c = 0; c < 256; ++c) {
        if (out.differenced[c]) continue;
        if (builtin) {
            if (desc.fontFileKind == 1) {
                out.names[c] = type1.codes.Name(static_cast<size_t>(c));
            } else if (cff.codeGlyphs[c] >= 0) {
                out.names[c] = cff.glyphNames.Name(static_cast<size_t>(cff.codeGlyphs[c]));
            }
            out.nameSource[c] = UnicodeSource::FontProgram;
        } else if (out.hasBase) {
            out.names[c] = EncodingName(out.base, static_cast<uint32_t>(c));
            out.fromBase[c] = true;
        }
    }
}

void ReadSimpleNames(const Document& doc, const PdfValue& dict, FontKind kind, const std::string& baseName,
                     const Descriptor& desc, SimpleNames& out) {
    ReadDeclaredNames(doc, dict, kind, baseName, desc, out);
    ReadBaseNames(doc, desc, out);
}

// A simple font's loader state that the codes without a trusted ToUnicode text need: kept by a
// pending font until a shown code needs it.
struct SimpleLoad {
    const Document* doc = nullptr;
    FontKind kind = FontKind::Unknown;
    Descriptor desc;
    Repair::Context repairContext;
    ToUnicodeMap toUnicode;
    bool hasToUnicode = false;
    bool toUnicodeUntrusted = false;
    std::vector<std::string_view> differenceNames;  // the /Differences names other than .notdef
    SimpleNames names;               // the declared names; CompleteSimpleTexts adds the base names
};

// The texts of the codes a trusted ToUnicode does not give (every code without one): the base
// names (the program's built-in encoding is read here), /Differences, the TrueType program, the
// Symbol positions, an untrusted ToUnicode, the WinAnsi guess; and whether the font is unmappable.
void CompleteSimpleTexts(Font::Impl& font, SimpleLoad& load) {
    const Document& doc = *load.doc;
    const Descriptor& desc = load.desc;
    SimpleNames& simple = load.names;
    ReadBaseNames(doc, desc, simple);
    const bool zapf = simple.zapf;
    const std::string_view (&names)[256] = simple.names;
    const UnicodeSource (&nameSource)[256] = simple.nameSource;
    const bool (&fromBase)[256] = simple.fromBase;
    const Tables::Encoding base = simple.base;
    const bool hasBase = simple.hasBase, builtin = simple.builtin;
    const size_t differences = simple.differences;
    const Repair::Context& repairContext = load.repairContext;
    const ToUnicodeMap& toUnicode = load.toUnicode;
    const bool hasToUnicode = load.hasToUnicode, toUnicodeUntrusted = load.toUnicodeUntrusted;
    std::string text, repaired;
    // The /Differences names with a usable text: only the numeric-name test (four names or more)
    // and, without a ToUnicode, the unmappable test ask.
    size_t resolvedDifferences = 0;
    if (differences >= 4 || (!hasToUnicode && differences > 0)) {
        for (const std::string_view name : load.differenceNames) {
            text.clear();
            if (GlyphNameToUnicode(name, zapf, text) && UsableText(text)) ++resolvedDifferences;
        }
    }

    bool needProgram = false;
    const EncodingTexts* baseTexts = hasBase && !builtin ? &EncodingTextsFor(base, zapf) : nullptr;
    for (int c = 0; c < 256; ++c) {
        if (font.slots[c].source == UnicodeSource::ToUnicode) continue;  // set by LoadSimple
        if (fromBase[c]) {
            // The base encoding's name: its text and usability made once per process.
            if (baseTexts->named[c]) {
                std::string_view named = baseTexts->Text(c);
                bool usable = baseTexts->usable[c];
                if (Repair::RepairText(named, repairContext, repaired)) named = repaired, usable = UsableText(named);
                if (usable) {
                    SetSlot(font, static_cast<uint8_t>(c), named, nameSource[c]);
                    continue;
                }
            }
        } else {
            text.clear();
            if (!names[c].empty() && GlyphNameToUnicode(names[c], zapf, text)) {
                if (Repair::RepairText(text, repairContext, repaired)) text = repaired;
                if (UsableText(text)) {
                    SetSlot(font, static_cast<uint8_t>(c), text, nameSource[c]);
                    continue;
                }
            }
        }
        font.slots[c].source = UnicodeSource::None;
        // The program is read for names that mean nothing, or when no encoding names the codes.
        if (!names[c].empty() || (!hasBase && !builtin && !hasToUnicode)) needProgram = true;
    }
    if (differences >= 4 && resolvedDifferences * 2 < differences) NumericGlyphNames(names, font);

    // TrueType programs: code -> glyph through the (3,0) / (1,0) / (3,1) subtables (§9.6.6.4),
    // glyph -> text through the inverted Unicode cmap and the 'post' names.
    if (needProgram && load.kind == FontKind::TrueType && desc.fontFileKind == 2) {
        Program::Sfnt sfnt;
        if (Program::ReadSfnt(simple.ProgramData(doc, desc), sfnt)) {
            const uint32_t glyphCount = SfntGlyphCount(sfnt);
            std::vector<std::string> glyphText;
            GlyphTextFromSfnt(sfnt, glyphCount, glyphText);
            RepairGlyphTexts(glyphText, repairContext);
            const Program::CmapSubtable* symbolCmap = sfnt.Cmap(3, 0);
            const Program::CmapSubtable* macCmap = sfnt.Cmap(1, 0);
            const Program::CmapSubtable* unicodeCmap = sfnt.Cmap(3, 1);
            for (int c = 0; c < 256; ++c) {
                if (font.slots[c].source != UnicodeSource::None) continue;
                uint32_t glyph = 0;
                if (symbolCmap) {
                    for (uint32_t prefix : {0u, 0xF000u, 0xF100u, 0xF200u}) {
                        glyph = symbolCmap->Glyph(prefix + static_cast<uint32_t>(c));
                        if (glyph) break;
                    }
                }
                if (!glyph && macCmap) glyph = macCmap->Glyph(static_cast<uint32_t>(c));
                if (!glyph && unicodeCmap) glyph = unicodeCmap->Glyph(static_cast<uint32_t>(c));
                if (glyph && glyph < glyphText.size() && !glyphText[glyph].empty()) {
                    SetSlot(font, static_cast<uint8_t>(c), glyphText[glyph], UnicodeSource::FontProgram);
                }
            }
        }
    }

    // Symbol and Wingdings fonts without text for a code: their known symbol positions.
    if (repairContext.symbol || repairContext.wingdings) {
        for (int c = 0x20; c < 256; ++c) {
            if (font.slots[c].source != UnicodeSource::None) continue;
            const uint32_t cp = Repair::SymbolCodePoint(0xF000u + static_cast<uint32_t>(c), repairContext);
            if (cp == 0) continue;
            text.clear();
            Utf8::Append(text, cp);
            SetSlot(font, static_cast<uint8_t>(c), text, UnicodeSource::Encoding);
        }
    }
    // An untrustworthy ToUnicode still gives the codes nothing else maps, as guesses.
    if (toUnicodeUntrusted) {
        for (int c = 0; c < 256; ++c) {
            if (font.slots[c].source != UnicodeSource::None) continue;
            if (const UnicodeEntry* entry = toUnicode.Find(static_cast<uint32_t>(c))) {
                SetSlot(font, static_cast<uint8_t>(c), toUnicode.Text(*entry), UnicodeSource::Guess);
            }
        }
    }

    // Guess: the code as WinAnsi (Latin-1 for the printable range), never for controls.
    const EncodingTexts& winAnsi = EncodingTextsFor(Tables::Encoding::WinAnsi, false);
    size_t mapped = 0;
    for (int c = 0; c < 256; ++c) {
        if (font.slots[c].source != UnicodeSource::None) {
            ++mapped;
            continue;
        }
        if (winAnsi.named[c] && winAnsi.usable[c]) {
            SetSlot(font, static_cast<uint8_t>(c), winAnsi.Text(c), UnicodeSource::Guess);
        }
    }
    // Unmappable (algorithms §3.6): no ToUnicode and nothing better than a guess, or /Differences
    // whose names mostly mean nothing (subset fonts naming glyphs g12, cid33).
    font.unmappable = !hasToUnicode && (mapped == 0 || (differences > 0 && resolvedDifferences * 2 < differences));
}

void LoadSimple(const Document& doc, const PdfValue& dict, FontKind kind, Font::Impl& font) {
    font.kind = kind;
    font.baseName = BaseNameOf(doc, dict);
    if (kind == FontKind::Type3 && font.baseName.empty()) {
        const std::string_view name = doc.Get(dict, "Name").Name();
        font.baseName.assign(name.data(), name.size());
    }
    StyleFromName(font.baseName, font.style);
    const Descriptor desc = ReadDescriptor(doc, doc.Get(dict, "FontDescriptor"), font.style);

    if (kind == FontKind::Type3) {
        const PdfSpan<PdfValue> m = doc.Get(dict, "FontMatrix").Array();
        double values[6];
        bool ok = m.size() == 6;
        for (size_t i = 0; ok && i < 6; ++i) ok = doc.Number(m[i], values[i]);
        if (ok && values[0] != 0) std::memcpy(font.matrix, values, sizeof values);
    }

    std::unique_ptr<SimpleLoad> load(new SimpleLoad());
    load->doc = &doc;
    load->kind = kind;
    load->desc = desc;
    load->repairContext = Repair::ContextForName(font.baseName);
    SimpleNames& simple = load->names;
    ReadDeclaredNames(doc, dict, kind, font.baseName, desc, simple);
    const int standard = simple.standard;
    // The /Differences names (counted in CompleteSimpleTexts).
    const PdfValue& encoding = *simple.encoding;
    if (encoding.IsDict() && simple.differences > 0) {
        const PdfSpan<PdfValue> diff = doc.Get(encoding, "Differences").Array();
        for (const PdfValue& item : diff) {
            const PdfValue& v = doc.Resolve(item);
            if (v.IsName() && v.Name() != ".notdef") load->differenceNames.push_back(v.Name());
        }
    }

    load->hasToUnicode =
        LoadToUnicode(doc, dict, load->repairContext, load->toUnicode, nullptr, load->toUnicodeUntrusted);
    // Unicode per code, first source wins (algorithms §3.1): a trusted ToUnicode's codes first (its
    // entries are sorted by code, one per code), the others in CompleteSimpleTexts.
    const bool trusted = load->hasToUnicode && !load->toUnicodeUntrusted;
    font.pool.reserve(512);
    if (trusted) {
        const UnicodeEntry* unicodeAt = load->toUnicode.entries.data();
        const UnicodeEntry* const unicodeEnd = unicodeAt + load->toUnicode.entries.size();
        for (int c = 0; c < 256 && unicodeAt != unicodeEnd; ++c) {
            while (unicodeAt != unicodeEnd && unicodeAt->code < static_cast<uint32_t>(c)) ++unicodeAt;
            if (unicodeAt != unicodeEnd && unicodeAt->code == static_cast<uint32_t>(c)) {
                SetSlot(font, static_cast<uint8_t>(c), load->toUnicode.Text(*unicodeAt), UnicodeSource::ToUnicode);
            }
        }
    }
    // The other codes' texts need the base names and may need the program. With a trusted
    // ToUnicode and /Widths (no width needs a name) they wait until a shown code lacks a text.
    const PdfValue& widthsValue = doc.Get(dict, "Widths");
    const bool pending = trusted && widthsValue.IsArray();
    if (!pending) CompleteSimpleTexts(font, *load);

    // Widths (§9.6.2, §9.6.5): /FirstChar + /Widths with /MissingWidth beyond; the standard 14
    // metrics when /Widths is absent.
    const double scale = kind == FontKind::Type3 ? font.matrix[0] : 0.001;
    if (widthsValue.IsArray()) {
        const PdfSpan<PdfValue> widths = widthsValue.Array();
        const int64_t first = doc.Get(dict, "FirstChar").Int(0);
        for (int c = 0; c < 256; ++c) {
            const int64_t i = c - first;
            double w = desc.missingWidth;
            if (i >= 0 && i < static_cast<int64_t>(widths.size())) {
                if (!doc.Number(widths[static_cast<size_t>(i)], w)) w = desc.missingWidth;
            }
            font.slots[c].width = static_cast<float>(w * scale);
        }
    } else if (kind != FontKind::Type3) {
        const std::string_view (&names)[256] = simple.names;
        const int metrics = StandardFontFor(font.baseName, font.style);
        for (int c = 0; c < 256; ++c) {
            uint16_t w = 0;
            double width = kNotdefWidth;
            if (!names[c].empty()) {
                const uint16_t index = Tables::FindGlyphName(names[c]);
                if (Tables::StandardWidth(metrics, index, w)) width = w / 1000.0;
                else if (desc.missingWidth > 0) width = desc.missingWidth / 1000.0;
            }
            font.slots[c].width = static_cast<float>(width);
        }
    } else {
        for (Slot& slot : font.slots) slot.width = static_cast<float>(kUnknownWidth);
    }

    // Extents.
    if (kind == FontKind::Type3) {
        PdfRect box;
        font.ascent = kDefaultAscent;
        font.descent = kDefaultDescent;
        if (doc.Rect(doc.Get(dict, "FontBBox"), box) && font.matrix[3] != 0) {
            const double top = box.y1 * font.matrix[3], bottom = box.y0 * font.matrix[3];
            const double hi = top > bottom ? top : bottom, lo = top > bottom ? bottom : top;
            if (hi > 0 && hi < 3) font.ascent = hi;
            if (lo < 0 && lo > -3) font.descent = lo;
        }
    } else {
        double ascentFallback = kDefaultAscent, descentFallback = kDefaultDescent;
        if (standard >= 0 && Tables::kStandardFonts[standard].ascent != 0) {
            ascentFallback = Tables::kStandardFonts[standard].ascent / 1000.0;
            descentFallback = Tables::kStandardFonts[standard].descent / 1000.0;
        }
        Extents(desc, ascentFallback, descentFallback, font.ascent, font.descent);
    }
    if (standard >= 0 && standard < 4) font.style.monospace = true;
    if (pending) {
        for (Slot& slot : font.slots) {
            if (slot.source != UnicodeSource::ToUnicode) slot.state = SlotState::Pending;
        }
        font.pendingSimple = std::move(load);
    } else {
        FinishSpaceWidthSimple(font);
    }
}

// ---- Type0 ------------------------------------------------------------------------------------

// The codespace of a Unicode-based predefined CMap.
void UnicodeCodespace(CMapKind kind, Codespace& codespace) {
    const auto add = [&](std::string_view low, std::string_view high) { codespace.Add(low, high); };
    using namespace std::string_view_literals;
    switch (kind) {
    case CMapKind::Ucs2:
        add("\x00\x00"sv, "\xFF\xFF"sv);
        break;
    case CMapKind::Utf16:
        add("\x00\x00"sv, "\xD7\xFF"sv);
        add("\xD8\x00\xDC\x00"sv, "\xDB\xFF\xDF\xFF"sv);
        add("\xE0\x00"sv, "\xFF\xFF"sv);
        break;
    case CMapKind::Utf8:
        add("\x00"sv, "\x7F"sv);
        add("\xC2\x80"sv, "\xDF\xBF"sv);
        add("\xE0\x80\x80"sv, "\xEF\xBF\xBF"sv);
        add("\xF0\x80\x80\x80"sv, "\xF4\xBF\xBF\xBF"sv);
        break;
    case CMapKind::Utf32:
        add("\x00\x00\x00\x00"sv, "\x00\x10\xFF\xFF"sv);
        break;
    default:
        add("\x00\x00"sv, "\xFF\xFF"sv);
        break;
    }
}

// A predefined CMap name (§9.7.5.2) by its form: Identity-H/V, the Unicode-based
// Uni*-UCS2/UTF16/UTF8/UTF32; anything else is Other until the generated tables know it.
CMapKind PredefinedKind(std::string_view name, bool& vertical) {
    vertical = name.size() >= 2 && name.substr(name.size() - 2) == "-V";
    if (name == "Identity-H" || name == "Identity-V") return CMapKind::Identity;
    if (name.substr(0, 3) == "Uni") {
        if (name.find("-UCS2-") != std::string_view::npos) return CMapKind::Ucs2;
        if (name.find("-UTF16-") != std::string_view::npos) return CMapKind::Utf16;
        if (name.find("-UTF8-") != std::string_view::npos) return CMapKind::Utf8;
        if (name.find("-UTF32-") != std::string_view::npos) return CMapKind::Utf32;
    }
    return CMapKind::Other;
}

// A predefined CMap name: its kind and, when the generated tables hold it, its index. A
// Unicode-based CMap the tables lack (UniJIS2004-UTF16-H, UniJISPro-UCS2-V, ...) borrows its
// collection's UTF32 CMap for CIDs.
CMapKind NamedCMap(std::string_view name, int& predefined, bool& vertical) {
    CMapKind kind = PredefinedKind(name, vertical);
    predefined = Cjk::FindPredefined(name);
    if (predefined >= 0) {
        vertical = Cjk::IsVertical(predefined);
        return KindOfEncoding(Cjk::EncodingOf(predefined));
    }
    if (kind == CMapKind::Ucs2 || kind == CMapKind::Utf16 || kind == CMapKind::Utf8 || kind == CMapKind::Utf32) {
        static const char* const kPrefixes[5] = {"UniJIS", "UniGB", "UniCNS", "UniKS", "UniAKR"};
        for (const char* prefix : kPrefixes) {
            const std::string_view p(prefix);
            if (name.substr(0, p.size()) != p) continue;
            std::string utf32(prefix);
            utf32 += vertical ? "-UTF32-V" : "-UTF32-H";
            predefined = Cjk::FindPredefined(utf32);
            break;
        }
    }
    return kind;
}

// /W: [c [w1 w2 ...] cfirst clast w ...] in glyph space (§9.7.4.3).
void ReadWidths(const Document& doc, const PdfValue& w, std::vector<WidthRange>& out) {
    const PdfSpan<PdfValue> items = doc.Resolve(w).Array();
    size_t budget = kMaxWidthEntries;
    for (size_t i = 0; i < items.size() && budget > 0;) {
        double first = 0;
        if (!doc.Number(items[i], first) || first < 0 || first > 0xFFFFFFFFu) {
            ++i;
            continue;
        }
        if (i + 1 >= items.size()) break;
        const PdfValue& next = doc.Resolve(items[i + 1]);
        if (next.IsArray()) {
            const PdfSpan<PdfValue> list = next.Array();
            uint32_t cid = static_cast<uint32_t>(first);
            for (size_t k = 0; k < list.size() && budget > 0; ++k, --budget) {
                double width = 0;
                if (doc.Number(list[k], width)) out.push_back(WidthRange{cid, cid, static_cast<float>(width / 1000)});
                if (cid == UINT32_MAX) break;
                ++cid;
            }
            i += 2;
            continue;
        }
        double last = 0, width = 0;
        if (i + 2 < items.size() && doc.Number(next, last) && doc.Number(items[i + 2], width) && last >= first &&
            last <= 0xFFFFFFFFu) {
            out.push_back(WidthRange{static_cast<uint32_t>(first), static_cast<uint32_t>(last), static_cast<float>(width / 1000)});
            --budget;
        }
        i += 3;
    }
    std::stable_sort(out.begin(), out.end(), [](const WidthRange& a, const WidthRange& b) { return a.low < b.low; });
}

// /W2: [c [w1y vx vy ...] cfirst clast w1y vx vy ...]; only w1y (the vertical advance) is kept.
void ReadVerticalWidths(const Document& doc, const PdfValue& w2, std::vector<VerticalRange>& out) {
    const PdfSpan<PdfValue> items = doc.Resolve(w2).Array();
    size_t budget = kMaxWidthEntries;
    for (size_t i = 0; i < items.size() && budget > 0;) {
        double first = 0;
        if (!doc.Number(items[i], first) || first < 0 || first > 0xFFFFFFFFu) {
            ++i;
            continue;
        }
        if (i + 1 >= items.size()) break;
        const PdfValue& next = doc.Resolve(items[i + 1]);
        if (next.IsArray()) {
            const PdfSpan<PdfValue> list = next.Array();
            uint32_t cid = static_cast<uint32_t>(first);
            for (size_t k = 0; k + 2 < list.size() && budget > 0; k += 3, --budget) {
                double advance = 0;
                if (doc.Number(list[k], advance)) out.push_back(VerticalRange{cid, cid, static_cast<float>(advance / 1000)});
                if (cid == UINT32_MAX) break;
                ++cid;
            }
            i += 2;
            continue;
        }
        double last = 0, advance = 0;
        if (i + 4 < items.size() && doc.Number(next, last) && doc.Number(items[i + 2], advance) && last >= first &&
            last <= 0xFFFFFFFFu) {
            out.push_back(VerticalRange{static_cast<uint32_t>(first), static_cast<uint32_t>(last), static_cast<float>(advance / 1000)});
            --budget;
        }
        i += 5;
    }
    std::stable_sort(out.begin(), out.end(), [](const VerticalRange& a, const VerticalRange& b) { return a.low < b.low; });
}

// A subset TrueType program without 'cmap' or 'post' gives no glyph text; many such subsets keep
// their font's glyph ids, and the common text fonts (Arial, Helvetica, Times New Roman, Courier
// New) put their first 258 glyphs in the Macintosh standard order. That order is taken when the
// /W widths confirm it: of the CIDs (= glyph ids) below 258 with a width of their own, at least 8
// and 80 % match the standard 14 width of the Macintosh name in the font's family (within 3 %).
// The texts are then guesses (glyphTextGuessed).
bool MacOrderGlyphText(const Font::Impl& font, int metrics, uint32_t glyphCount, std::vector<std::string>& out) {
    if (glyphCount < 258 || !font.cidToGid.empty() || font.cmapKind != CMapKind::Identity) return false;
    size_t compared = 0, agree = 0;
    for (const WidthRange& range : font.widths) {
        if (range.low != range.high || range.low == 0 || range.low >= 258) continue;
        uint16_t standard = 0;
        if (!Tables::StandardWidth(metrics, Tables::kMacGlyphNames[range.low], standard) || standard == 0) continue;
        ++compared;
        const double w = range.width * 1000.0;
        if (std::fabs(w - standard) <= 0.03 * standard + 1) ++agree;
    }
    if (compared < 8 || agree * 5 < compared * 4) return false;
    out.assign(258, std::string());
    for (uint32_t g = 1; g < 258; ++g) {
        const std::string_view name = Tables::GlyphName(Tables::kMacGlyphNames[g]);
        if (!GlyphNameToUnicode(name, out[g]) || !UsableText(out[g])) out[g].clear();
    }
    return true;
}

// A Type0 font's loader state that reading its embedded program needs: kept by a pending font.
struct Type0Load {
    const Document* doc = nullptr;
    Descriptor desc;
    const PdfValue* cidFont = nullptr;
    std::string_view cidSubtype;
    Repair::Context repairContext;
    bool hasToUnicode = false;
    bool collectionMaps = false;
};

// The glyph texts of a Type0 font's embedded program (and the CIDToGIDMap they need).
void ReadType0Program(Font::Impl& font, const Type0Load& load) {
    const Document& doc = *load.doc;
    const Descriptor& desc = load.desc;
    const PdfValue& cidFont = *load.cidFont;
    const std::string_view cidSubtype = load.cidSubtype;
    const Repair::Context& repairContext = load.repairContext;
    const bool hasToUnicode = load.hasToUnicode;
    std::string program;
    if (!LoadProgram(doc, desc, program)) return;
    std::vector<std::string> glyphText;
    Program::Sfnt sfnt;
    const bool isSfnt = Program::ReadSfnt(program, sfnt);
    if (cidSubtype == "CIDFontType2" && isSfnt) {
        GlyphTextFromSfnt(sfnt, SfntGlyphCount(sfnt), glyphText);
        bool anyText = false;
        for (const std::string& t : glyphText) anyText = anyText || !t.empty();
        const PdfValue& map = doc.Get(cidFont, "CIDToGIDMap");
        if (map.IsStream()) {
            std::string gids;
            const StreamStatus status = doc.StreamData(map, gids, kCidToGidLimit);
            if (status != StreamStatus::Corrupt && status != StreamStatus::NotStream) {
                font.cidToGid.resize(gids.size() / 2);
                for (size_t i = 0; i < font.cidToGid.size(); ++i) {
                    font.cidToGid[i] = static_cast<uint16_t>(static_cast<uint8_t>(gids[2 * i]) << 8 |
                                                             static_cast<uint8_t>(gids[2 * i + 1]));
                }
            }
        }
        if (!anyText && !hasToUnicode) {
            font.glyphTextGuessed =
                MacOrderGlyphText(font, StandardFontFor(font.baseName, font.style), sfnt.glyphCount, glyphText);
        }
    } else if (cidSubtype == "CIDFontType0") {
        Program::CffInfo cff;
        const std::string_view table = isSfnt && !sfnt.cff.empty() ? std::string_view(sfnt.cff) : std::string_view(program);
        if (Program::ReadCff(table, cff)) {
            if (!cff.cidKeyed) {
                // A name-keyed program: CIDs are glyph ids, glyph names give the text.
                glyphText.assign(cff.glyphCount, std::string());
                for (uint32_t g = 1; g < cff.glyphCount; ++g) {
                    if (!GlyphNameToUnicode(cff.glyphNames.Name(g), glyphText[g]) || !UsableText(glyphText[g])) {
                        glyphText[g].clear();
                    }
                }
            } else if (isSfnt && !sfnt.cmaps.empty()) {
                // CID-keyed inside OpenType: CID -> glyph (charset), glyph -> text (cmap).
                GlyphTextFromSfnt(sfnt, cff.glyphCount, glyphText);
                uint32_t maxCid = 0;
                for (uint16_t cid : cff.glyphCids) maxCid = cid > maxCid ? cid : maxCid;
                font.cidToGid.assign(static_cast<size_t>(maxCid) + 1, 0);
                for (uint32_t g = 0; g < cff.glyphCids.size(); ++g) font.cidToGid[cff.glyphCids[g]] = static_cast<uint16_t>(g);
            }
        }
    }
    RepairGlyphTexts(glyphText, repairContext);
    font.glyphText.resize(glyphText.size());
    bool any = false;
    for (size_t g = 0; g < glyphText.size(); ++g) {
        if (glyphText[g].empty()) continue;
        font.glyphText[g] = Pool(font.pool, glyphText[g]);
        any = true;
    }
    if (!any) font.glyphText.clear();
}

// The space: a code whose text is U+0020.
void FinishSpaceWidthType0(Font::Impl& font, bool collectionMaps) {
    font.spaceWidth = 0;
    for (const UnicodeEntry& entry : font.toUnicode.entries) {
        if (font.toUnicode.Text(entry) == " ") {
            uint32_t cid = 0;
            const float w = font.CidOf(entry.code, 0, cid) ? font.WidthOf(cid) : font.defaultWidth;
            if (w > 0) {
                font.spaceWidth = w;
                break;
            }
        }
    }
    for (size_t g = 0; font.spaceWidth == 0 && g < font.glyphText.size(); ++g) {
        const TextRef& ref = font.glyphText[g];
        if (std::string_view(font.pool).substr(ref.offset, ref.length) != " ") continue;
        uint32_t cid = static_cast<uint32_t>(g);
        if (!font.cidToGid.empty()) {
            cid = UINT32_MAX;
            for (size_t c = 0; c < font.cidToGid.size(); ++c) {
                if (font.cidToGid[c] == g) {
                    cid = static_cast<uint32_t>(c);
                    break;
                }
            }
            if (cid == UINT32_MAX) break;
        }
        font.spaceWidth = font.WidthOf(cid);
    }
    // CID 1 is the proportional space of all four Adobe CJK collections.
    if (font.spaceWidth == 0 && collectionMaps && FindByCid(font.widths, 1u)) font.spaceWidth = font.WidthOf(1);
}

void LoadType0(const Document& doc, const PdfValue& dict, Font::Impl& font) {
    font.kind = FontKind::Type0;
    font.baseName = BaseNameOf(doc, dict);
    StyleFromName(font.baseName, font.style);

    // The encoding: a predefined CMap name or an embedded CMap stream.
    const PdfValue& encoding = doc.Get(dict, "Encoding");
    bool vertical = false;
    if (encoding.IsName()) {
        font.cmapKind = NamedCMap(encoding.Name(), font.predefined, vertical);
    } else if (encoding.IsStream()) {
        std::string data;
        const StreamStatus status = doc.StreamData(encoding, data, kCMapStreamLimit);
        font.cmapKind = CMapKind::Embedded;
        if (status != StreamStatus::Corrupt && status != StreamStatus::NotStream) ParseEncodingCMap(data, font.cmap);
        vertical = font.cmap.wmode == 1 || doc.Get(encoding, "WMode").Int(0) == 1;
        // A CMap based on another (usecmap, /UseCMap): a CMap with no mappings of its own reads
        // codes as that one does; one with mappings falls back to it for the codes it lacks.
        std::string_view use = font.cmap.useCMap;
        if (use.empty()) use = doc.Get(encoding, "UseCMap").Name();
        if (!use.empty()) {
            bool useVertical = false;
            int usePredefined = -1;
            const CMapKind used = NamedCMap(use, usePredefined, useVertical);
            vertical = vertical || useVertical;
            if (font.cmap.cids.empty() && font.cmap.notdefs.empty()) {
                font.cmapKind = used;
                font.predefined = usePredefined;
            } else if (font.cmap.codespace.Empty() && used != CMapKind::Identity) {
                font.predefined = usePredefined;
                if (used == CMapKind::Predefined) Cjk::AddCodespace(usePredefined, font.cmap.codespace);
                else UnicodeCodespace(used, font.cmap.codespace);
            } else {
                font.predefined = usePredefined;
            }
        }
    } else {
        font.cmapKind = CMapKind::Identity;
    }
    if (font.cmap.codespace.Empty()) {
        if (font.cmapKind == CMapKind::Predefined) Cjk::AddCodespace(font.predefined, font.cmap.codespace);
        else if (font.cmapKind != CMapKind::Identity && font.cmapKind != CMapKind::Embedded)
            UnicodeCodespace(font.cmapKind, font.cmap.codespace);
    }
    font.vertical = vertical;

    // /DescendantFonts is a one-element array; some writers give the dictionary itself.
    const PdfValue& descendants = doc.Get(dict, "DescendantFonts");
    const PdfValue& cidFont = descendants.IsArray() && !descendants.Array().empty()
                                  ? doc.Resolve(descendants.Array()[0])
                                  : descendants;
    const std::string_view cidSubtype = doc.Get(cidFont, "Subtype").Name();
    // The character collection: the predefined CMap's (its CIDs are that collection's), else the
    // descendant's /CIDSystemInfo.
    font.collection = Cjk::CollectionOf(font.predefined);
    if (font.collection == Cjk::Collection::None) {
        const PdfValue& info = doc.Get(cidFont, "CIDSystemInfo");
        font.collection = Cjk::CollectionByName(doc.Get(info, "Registry").String(), doc.Get(info, "Ordering").String());
    }
    const Descriptor desc = ReadDescriptor(doc, doc.Get(cidFont, "FontDescriptor"), font.style);
    if (font.baseName.empty()) font.baseName = BaseNameOf(doc, cidFont);
    Extents(desc, kDefaultAscent, kDefaultDescent, font.ascent, font.descent);

    // Metrics (§9.7.4.3).
    double value = 0;
    font.defaultWidth = doc.Number(doc.Get(cidFont, "DW"), value) ? static_cast<float>(value / 1000) : 1.0f;
    ReadWidths(doc, doc.Get(cidFont, "W"), font.widths);
    if (font.vertical) {
        const PdfSpan<PdfValue> dw2 = doc.Get(cidFont, "DW2").Array();
        double advance = -1000;
        if (dw2.size() == 2) doc.Number(dw2[1], advance);
        font.defaultVertical = static_cast<float>(advance / 1000);
        ReadVerticalWidths(doc, doc.Get(cidFont, "W2"), font.verticals);
    }

    size_t droppedToUnicode = 0;
    const Repair::Context repairContext = Repair::ContextForName(font.baseName);
    const bool hasToUnicode =
        LoadToUnicode(doc, dict, repairContext, font.toUnicode, &droppedToUnicode, font.toUnicodeUntrusted);
    const bool unicodeCMap = font.cmapKind == CMapKind::Ucs2 || font.cmapKind == CMapKind::Utf16 ||
                             font.cmapKind == CMapKind::Utf8 || font.cmapKind == CMapKind::Utf32;
    // The collection maps codes when their CIDs can be known.
    const bool cidsKnown = font.cmapKind == CMapKind::Identity || font.cmapKind == CMapKind::Embedded ||
                           font.cmapKind == CMapKind::Predefined || (unicodeCMap && font.predefined >= 0);
    const bool collectionMaps = font.collection != Cjk::Collection::None && cidsKnown;

    // The embedded program, only when nothing better maps the codes (algorithms §3.4), or as the
    // fallback for codes an incomplete ToUnicode lacks (§3.1, §3.6): entries dropped as unusable,
    // a truncated map, or an Identity font whose /W lists CIDs (the glyphs of a subset) that the
    // ToUnicode does not map.
    bool incomplete = hasToUnicode && (droppedToUnicode > 0 || font.toUnicode.truncated || font.toUnicodeUntrusted);
    if (hasToUnicode && !incomplete && font.cmapKind == CMapKind::Identity) {
        for (const WidthRange& range : font.widths) {
            if (range.low == range.high && range.low != 0 && !font.toUnicode.Find(range.low)) {
                incomplete = true;
                break;
            }
        }
    }
    if (desc.fontFile && ((!hasToUnicode && !unicodeCMap && !collectionMaps) || incomplete)) {
        std::unique_ptr<Type0Load> load(new Type0Load());
        load->doc = &doc;
        load->desc = desc;
        load->cidFont = &cidFont;
        load->cidSubtype = cidSubtype;
        load->repairContext = repairContext;
        load->hasToUnicode = hasToUnicode;
        load->collectionMaps = collectionMaps;
        // An incomplete ToUnicode still maps most codes: the program waits for a code it lacks.
        if (incomplete) font.pendingType0 = std::move(load);
        else ReadType0Program(font, *load);
    }
    // Pending (incomplete, so a ToUnicode) or not: glyphText decides only without a ToUnicode.
    font.unmappable = !hasToUnicode && !unicodeCMap && !collectionMaps && font.glyphText.empty();
    if (!font.pendingType0) FinishSpaceWidthType0(font, collectionMaps);
}

// The font FontCache gives for anything it cannot read: single bytes as WinAnsi, width 0.5.
void LoadFallback(Font::Impl& font) {
    font.kind = FontKind::Unknown;
    const EncodingTexts& winAnsi = EncodingTextsFor(Tables::Encoding::WinAnsi, false);
    for (int c = 0; c < 256; ++c) {
        font.slots[c].width = static_cast<float>(kUnknownWidth);
        if (winAnsi.named[c] && winAnsi.usable[c]) {
            SetSlot(font, static_cast<uint8_t>(c), winAnsi.Text(c), UnicodeSource::Guess);
        }
    }
    font.unmappable = true;
    font.spaceWidth = kUnknownWidth;
}

FontKind KindOf(std::string_view subtype) {
    if (subtype == "Type1") return FontKind::Type1;
    if (subtype == "MMType1") return FontKind::MMType1;
    if (subtype == "TrueType") return FontKind::TrueType;
    if (subtype == "Type3") return FontKind::Type3;
    if (subtype == "Type0") return FontKind::Type0;
    return FontKind::Unknown;
}

} // namespace

void Font::Impl::Complete() {
    if (std::unique_ptr<SimpleLoad> load = std::move(pendingSimple)) {
        for (Slot& slot : slots) {
            if (slot.state == SlotState::Pending) slot.state = SlotState::Pool;
        }
        late = true;
        CompleteSimpleTexts(*this, *load);
        late = false;
        FinishSpaceWidthSimple(*this);
    }
    if (std::unique_ptr<Type0Load> load = std::move(pendingType0)) {
        // Composite has handed out no view into `pool` yet: the glyph texts may extend it.
        ReadType0Program(*this, *load);
        FinishSpaceWidthType0(*this, load->collectionMaps);
    }
}

Font::Font(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Font::~Font() = default;

FontCache::FontCache(const Document& document) : document_(document) {}
FontCache::~FontCache() = default;

const Font& FontCache::Get(const PdfValue& fontEntry) {
    const PdfValue& dict = document_.Resolve(fontEntry);
    uintptr_t key = 0;
    if (dict.IsDict()) {
        // Referenced fonts by object number (odd keys), direct ones by their entries' address
        // (arena storage is 8-byte aligned: even keys).
        key = fontEntry.IsRef() ? (static_cast<uintptr_t>(fontEntry.refNum) << 1 | 1)
                                : reinterpret_cast<uintptr_t>(dict.data);
    }
    auto found = fonts_.find(key);
    if (found != fonts_.end()) return *found->second;
    std::unique_ptr<Font::Impl> impl(new Font::Impl());
    impl->dict = &dict;
    const FontKind kind = dict.IsDict() ? KindOf(document_.Get(dict, "Subtype").Name()) : FontKind::Unknown;
    switch (kind) {
    case FontKind::Type0:
        LoadType0(document_, dict, *impl);
        break;
    case FontKind::Unknown:
        if (dict.IsDict() && !document_.Get(dict, "BaseFont").IsNull()) {
            // A font dictionary without a usable /Subtype: read it as Type 1.
            LoadSimple(document_, dict, FontKind::Type1, *impl);
            impl->kind = FontKind::Unknown;
        } else {
            LoadFallback(*impl);
        }
        break;
    default:
        LoadSimple(document_, dict, kind, *impl);
        break;
    }
    std::unique_ptr<Font> font(new Font(std::move(impl)));
    // A simple font's slots for the inline Next: its pool is final (Complete writes latePool).
    if (font->impl_->kind != FontKind::Type0) {
        font->simple_ = font->impl_->slots;
        font->simplePool_ = font->impl_->pool.data();
    }
    const Font& result = *font;
    fonts_.emplace(key, std::move(font));
    return result;
}

size_t FontCache::Count() const {
    return fonts_.size();
}

// ---- FontGlyphs -------------------------------------------------------------------------------

struct FontGlyphs::Impl {
    Kind kind = Kind::None;
    const Font* font = nullptr;
    bool composite = false;
    std::string program;  // the bytes the outline tables view
    Program::Sfnt sfnt;
    Program::GlyfOutlines glyf;
    Program::CffOutlines cff;
    Program::Type1Outlines type1;
    Raster::Matrix toText;               // TrueType and Type 1: glyph space to text space at size 1
    int32_t codeGlyph[256];              // simple fonts: the glyph of each code, -1 for none
    std::vector<uint16_t> cidToGid;      // Type0 TrueType: /CIDToGIDMap (empty: Identity)
    std::vector<const PdfValue*> procs;  // Type 3: the procedure of each code
    const PdfValue* resources = nullptr;
    Program::Names post;                 // TrueType: the 'post' glyph names

    // The glyph of a simple TrueType font's code `c` named `name` (§9.6.6.4 and pdf.js's order).
    int32_t TrueTypeGlyph(int c, std::string_view name, bool symbolic) const {
        const Program::CmapSubtable* symbolCmap = sfnt.Cmap(3, 0);
        const Program::CmapSubtable* macCmap = sfnt.Cmap(1, 0);
        const Program::CmapSubtable* unicodeCmap = sfnt.Cmap(3, 1);
        uint32_t glyph = 0;
        const auto bySymbol = [&] {
            if (symbolCmap)
                for (uint32_t prefix : {0u, 0xF000u, 0xF100u, 0xF200u})
                    if ((glyph = symbolCmap->Glyph(prefix + static_cast<uint32_t>(c))) != 0) return;
        };
        const auto byName = [&] {
            std::string text;
            if (unicodeCmap && !name.empty() && GlyphNameToUnicode(name, text)) {
                size_t at = 0, length = 0;
                uint32_t cp = 0;
                if (Utf8::Decode(text, at, cp, length) && length == text.size()) glyph = unicodeCmap->Glyph(cp);
            }
            if (!glyph && !name.empty())
                for (uint32_t g = 1; g < post.Count() && !glyph; ++g)
                    if (post.Name(g) == name) glyph = g;
        };
        if (symbolic) {
            bySymbol();
            if (!glyph && macCmap) glyph = macCmap->Glyph(static_cast<uint32_t>(c));
            if (!glyph) byName();
        } else {
            byName();
            if (!glyph && macCmap) glyph = macCmap->Glyph(static_cast<uint32_t>(c));
            if (!glyph) bySymbol();
        }
        if (!glyph && unicodeCmap) glyph = unicodeCmap->Glyph(static_cast<uint32_t>(c));
        if (!glyph && sfnt.cmaps.empty()) glyph = static_cast<uint32_t>(c);  // a subset without cmap keeps codes
        return glyph > 0 && glyph < glyf.glyphCount ? static_cast<int32_t>(glyph) : -1;
    }
};

FontGlyphs::FontGlyphs() : impl_(new Impl()) {}
FontGlyphs::~FontGlyphs() = default;

FontGlyphs::Kind FontGlyphs::GetKind() const { return impl_->kind; }

FontGlyphs::Kind FontGlyphs::Load(const Document& doc, const Font& font) {
    impl_.reset(new Impl());
    Impl& g = *impl_;
    g.font = &font;
    for (int32_t& glyph : g.codeGlyph) glyph = -1;
    const PdfValue& dict = font.Dictionary();
    if (!dict.IsDict()) return Kind::None;
    FontKind kind = font.Kind();
    if (kind == FontKind::Unknown) {
        if (doc.Get(dict, "BaseFont").IsNull()) return Kind::None;
        kind = FontKind::Type1;  // as FontCache reads it
    }
    FontStyle style;
    if (kind == FontKind::Type3) {
        const PdfValue& procs = doc.Get(dict, "CharProcs");
        if (!procs.IsDict()) return Kind::None;
        g.resources = &doc.Get(dict, "Resources");
        SimpleNames simple;
        ReadSimpleNames(doc, dict, kind, std::string(font.BaseName()), Descriptor(), simple);
        g.procs.assign(256, nullptr);
        bool any = false;
        for (int c = 0; c < 256; ++c) {
            if (simple.names[c].empty()) continue;
            const PdfValue& proc = doc.Get(procs, simple.names[c]);
            if (!proc.IsStream()) continue;
            g.procs[c] = &proc;
            g.codeGlyph[c] = c;
            any = true;
        }
        return g.kind = any ? Kind::Type3 : Kind::None;
    }

    g.composite = kind == FontKind::Type0;
    const PdfValue* cidFont = nullptr;
    if (g.composite) {
        const PdfValue& descendants = doc.Get(dict, "DescendantFonts");
        cidFont = descendants.IsArray() && !descendants.Array().empty() ? &doc.Resolve(descendants.Array()[0])
                                                                        : &descendants;
    }
    const Descriptor desc = ReadDescriptor(doc, doc.Get(g.composite ? *cidFont : dict, "FontDescriptor"), style);
    if (!desc.fontFile || !LoadProgram(doc, desc, g.program)) return Kind::None;

    // The program's outlines: sfnt (glyf, or an OpenType 'CFF '), bare CFF, Type 1.
    const bool isSfnt = Program::ReadSfnt(g.program, g.sfnt);
    if (isSfnt && Program::ReadGlyfOutlines(g.sfnt, g.glyf)) {
        g.kind = Kind::TrueType;
        const double scale = 1.0 / g.glyf.unitsPerEm;
        g.toText = Raster::Matrix{scale, 0, 0, scale, 0, 0};
        Program::ReadPostNames(g.sfnt.post, g.glyf.glyphCount, g.post);
    } else if (Program::ReadCffOutlines(isSfnt ? g.sfnt.cff : std::string_view(g.program), g.cff)) {
        g.kind = Kind::Cff;
    } else if (desc.fontFileKind == 1 && Program::ReadType1Outlines(g.program, g.type1)) {
        g.kind = Kind::Type1;
        const double* m = g.type1.matrix;
        g.toText = Raster::Matrix{m[0], m[1], m[2], m[3], m[4], m[5]};
    } else {
        return Kind::None;
    }

    if (g.composite) {
        if (g.kind == Kind::TrueType) {
            const PdfValue& map = doc.Get(*cidFont, "CIDToGIDMap");
            if (map.IsStream()) {
                std::string gids;
                const StreamStatus status = doc.StreamData(map, gids, kCidToGidLimit);
                if (status != StreamStatus::Corrupt && status != StreamStatus::NotStream) {
                    g.cidToGid.resize(gids.size() / 2);
                    for (size_t i = 0; i < g.cidToGid.size(); ++i)
                        g.cidToGid[i] = static_cast<uint16_t>(static_cast<uint8_t>(gids[2 * i]) << 8 |
                                                              static_cast<uint8_t>(gids[2 * i + 1]));
                }
            }
        } else if (g.kind == Kind::Type1) {
            g.kind = Kind::None;  // a Type0 font has no Type 1 descendant
        }
        return g.kind;
    }

    SimpleNames simple;
    ReadSimpleNames(doc, dict, kind, std::string(font.BaseName()), desc, simple);
    for (int c = 0; c < 256; ++c) {
        const std::string_view name = simple.names[c];
        int32_t glyph = -1;
        switch (g.kind) {
        case Kind::TrueType:
            glyph = g.TrueTypeGlyph(c, name, simple.symbolic);
            break;
        case Kind::Cff:
            if (g.cff.info.cidKeyed) {
                glyph = Program::CffGlyphByCid(g.cff, static_cast<uint32_t>(c));
            } else if (simple.builtin && desc.fontFileKind == 3 && g.cff.info.encoding == 2 && simple.cff.codeGlyphs[c] >= 0) {
                glyph = simple.cff.codeGlyphs[c];
            } else {
                glyph = Program::CffGlyphByName(g.cff, name);
            }
            break;
        case Kind::Type1: {
            if (name.empty()) break;
            const auto& strings = g.type1.charStrings;
            const auto it = std::lower_bound(strings.begin(), strings.end(), name,
                                             [](const std::pair<std::string, std::string>& e, std::string_view n) {
                                                 return std::string_view(e.first) < n;
                                             });
            if (it != strings.end() && it->first == name) glyph = static_cast<int32_t>(it - strings.begin());
            break;
        }
        default:
            break;
        }
        g.codeGlyph[c] = glyph > 0 || (glyph == 0 && g.kind == Kind::Type1) ? glyph : -1;  // glyph 0 is .notdef
    }
    return g.kind;
}

bool FontGlyphs::GlyphOf(uint32_t code, uint8_t bytes, uint32_t& glyph) const {
    const Impl& g = *impl_;
    if (g.kind == Kind::None) return false;
    if (!g.composite) {
        if (code > 255 || g.codeGlyph[code] < 0) return false;
        glyph = static_cast<uint32_t>(g.codeGlyph[code]);
        return true;
    }
    uint32_t cid = 0;
    if (!g.font->Cid(code, bytes, cid)) return false;
    int64_t gid = -1;
    if (g.kind == Kind::TrueType) {
        gid = g.cidToGid.empty() ? int64_t(cid) : (cid < g.cidToGid.size() ? int64_t(g.cidToGid[cid]) : -1);
        if (gid >= int64_t(g.glyf.glyphCount)) gid = -1;
    } else if (g.kind == Kind::Cff) {
        gid = Program::CffGlyphByCid(g.cff, cid);
    }
    if (gid <= 0) return false;
    glyph = static_cast<uint32_t>(gid);
    return true;
}

bool FontGlyphs::Outline(uint32_t glyph, Raster::Path& out) const {
    const Impl& g = *impl_;
    switch (g.kind) {
    case Kind::TrueType:
        return Program::GlyfOutline(g.glyf, glyph, g.toText, out);
    case Kind::Cff:
        return Program::CffGlyphOutline(g.cff, glyph, Program::CffGlyphMatrix(g.cff, glyph), out);
    case Kind::Type1:
        return glyph < g.type1.charStrings.size() &&
               Program::Type1GlyphOutline(g.type1, g.type1.charStrings[glyph].first, g.toText, out);
    default:
        return false;
    }
}

const PdfValue* FontGlyphs::Procedure(uint32_t glyph) const {
    const Impl& g = *impl_;
    return g.kind == Kind::Type3 && glyph < g.procs.size() ? g.procs[glyph] : nullptr;
}

const PdfValue& FontGlyphs::Resources() const {
    static const PdfValue kNull{};
    return impl_->resources ? *impl_->resources : kNull;
}

} // namespace RayoPdf::Text
