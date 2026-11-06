// SPDX-License-Identifier: Apache-2.0
// Fonts as text (ISO 32000-1 sections 9.6-9.10, algorithms §3.1-§3.6). Everything a code needs is
// resolved when the font loads: a simple font gets a 256-entry table of advance, Unicode and
// source, so reading a code is one lookup; a Type0 font gets sorted tables (ToUnicode entries,
// CID ranges, /W ranges) searched in O(log n). Unicode text lives in the font's own buffers, so
// Next never allocates.

#include "text/font.h"

#include "common/compiler.h"
#include "common/utf8.h"
#include "text/cmap.h"
#include "text/program.h"
#include "text/tables.h"

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

// The UTF-8 text of every BMP code point, three bytes apart, for the Unicode-based predefined
// CMaps whose codes are the text (built once per process, on first use).
std::string_view BmpText(uint32_t cp) {
    struct Table {
        char bytes[0x10000 * 3];
        uint8_t lengths[0x10000];
        Table() {
            std::string one;
            for (uint32_t c = 0; c < 0x10000; ++c) {
                one.clear();
                Utf8::Append(one, c);
                std::memcpy(bytes + c * 3, one.data(), one.size());
                lengths[c] = static_cast<uint8_t>(one.size());
            }
        }
    };
    static const Table* const table = new Table();
    if (cp >= 0x10000 || (cp >= 0xD800 && cp <= 0xDFFF)) return {};
    return std::string_view(table->bytes + cp * 3, table->lengths[cp]);
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
// "Black", "Heavy"; "Italic", "Oblique", a trailing "-It"/"It"; "Mono", "Courier", "Code",
// "Consol(as)", "Menlo".
void StyleFromName(std::string_view name, FontStyle& style) {
    if (ContainsNoCase(name, "bold") || ContainsNoCase(name, "black") || ContainsNoCase(name, "heavy")) style.bold = true;
    if (ContainsNoCase(name, "italic") || ContainsNoCase(name, "oblique")) style.italic = true;
    // "Minion-It", "Minion-BoldIt", "MinionPro-SemiboldIt".
    const size_t dash = name.find_last_of("-,");
    if (dash != std::string_view::npos) {
        const std::string_view suffix = name.substr(dash + 1);
        if (suffix.size() >= 2 && suffix.substr(suffix.size() - 2) == "It") style.italic = true;
    }
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

struct Slot {
    float width = 0;
    uint32_t offset = 0;
    uint16_t length = 0;
    UnicodeSource source = UnicodeSource::None;
};

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

// How a Type0 font's codes become CIDs and, for the Unicode-based CMaps, text.
enum class CMapKind : uint8_t { Identity, Embedded, Ucs2, Utf16, Utf8, Utf32, Other };

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

    // Simple fonts (Type1, MMType1, TrueType, Type3, and the fallback).
    Slot slots[256];

    // Type0.
    CMapKind cmapKind = CMapKind::Identity;
    EncodingCMap cmap;                 // Embedded: codespace and CID ranges; Ucs2..Other: codespace only
    ToUnicodeMap toUnicode;            // entries filtered to usable text
    std::vector<uint16_t> cidToGid;    // empty: CID = glyph id
    std::vector<TextRef> glyphText;    // by glyph id, into `pool` (FontProgram)
    std::vector<WidthRange> widths;    // sorted by low
    float defaultWidth = 1.0f;
    std::vector<VerticalRange> verticals;
    float defaultVertical = -1.0f;

    bool NextSimple(std::string_view bytes, size_t& at, Code& out) const;
    bool NextComposite(std::string_view bytes, size_t& at, Code& out) const;
    float WidthOf(uint32_t cid) const;
    float VerticalOf(uint32_t cid) const;
    bool CidOf(uint32_t code, uint32_t& cid) const;
    void Composite(uint32_t code, uint8_t length, Code& out) const;
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

bool Font::Impl::CidOf(uint32_t code, uint32_t& cid) const {
    switch (cmapKind) {
    case CMapKind::Identity:
        cid = code;
        return true;
    case CMapKind::Embedded:
        if (cmap.cids.empty() && cmap.notdefs.empty()) {
            cid = code;  // only a codespace (or usecmap of an Identity CMap): codes are CIDs
            return true;
        }
        cmap.Cid(code, cid);
        return true;
    default:
        cid = 0;  // no CMap tables for the predefined CJK CMaps yet: CIDs unknown
        return false;
    }
}

bool Font::Impl::NextSimple(std::string_view bytes, size_t& at, Code& out) const {
    if (at >= bytes.size()) return false;
    const uint8_t c = static_cast<uint8_t>(bytes[at++]);
    const Slot& slot = slots[c];
    out.code = c;
    out.bytes = 1;
    out.width = slot.width;
    out.verticalAdvance = 0;
    out.wordSpace = c == 32;
    out.source = slot.source;
    out.unicode = std::string_view(pool.data() + slot.offset, slot.length);
    return true;
}

void Font::Impl::Composite(uint32_t code, uint8_t length, Code& out) const {
    out.code = code;
    out.bytes = length;
    out.wordSpace = length == 1 && code == 32;
    uint32_t cid = 0;
    const bool known = CidOf(code, cid);
    out.width = known ? WidthOf(cid) : defaultWidth;
    out.verticalAdvance = vertical ? (known ? VerticalOf(cid) : defaultVertical) : 0;
    if (const UnicodeEntry* entry = toUnicode.Find(code)) {
        out.source = UnicodeSource::ToUnicode;
        out.unicode = toUnicode.Text(*entry);
        return;
    }
    uint32_t cp = 0;
    bool text = false;
    switch (cmapKind) {
    case CMapKind::Ucs2:
        cp = code, text = length == 2;
        break;
    case CMapKind::Utf16:
        cp = code, text = length == 2;  // surrogate pairs (4-byte codes) lie outside BmpText
        break;
    case CMapKind::Utf32:
        cp = code, text = length == 4;
        break;
    case CMapKind::Utf8: {
        // The code bytes are UTF-8: re-read them as such.
        char buffer[4];
        for (uint8_t i = 0; i < length; ++i) buffer[i] = static_cast<char>(code >> (8 * (length - 1 - i)));
        size_t used = 0;
        text = Utf8::Decode(std::string_view(buffer, length), 0, cp, used) && used == length;
        break;
    }
    default:
        break;
    }
    if (text) {
        const std::string_view bmp = BmpText(cp);
        if (UsableText(bmp)) {
            out.source = UnicodeSource::Collection;
            out.unicode = bmp;
            return;
        }
    }
    if (known && !glyphText.empty()) {
        const uint32_t glyph = cidToGid.empty() ? cid : (cid < cidToGid.size() ? cidToGid[cid] : 0);
        if (glyph < glyphText.size() && glyphText[glyph].length > 0) {
            out.source = UnicodeSource::FontProgram;
            out.unicode = std::string_view(pool.data() + glyphText[glyph].offset, glyphText[glyph].length);
            return;
        }
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
    }
    at += length;
    Composite(code, length, out);
    return true;
}

Font::Font(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Font::~Font() = default;

bool Font::Next(std::string_view bytes, size_t& at, Code& out) const {
    return impl_->kind == FontKind::Type0 ? impl_->NextComposite(bytes, at, out) : impl_->NextSimple(bytes, at, out);
}
FontKind Font::Kind() const { return impl_->kind; }
std::string_view Font::BaseName() const { return impl_->baseName; }
const FontStyle& Font::Style() const { return impl_->style; }
bool Font::Vertical() const { return impl_->vertical; }
const double* Font::Matrix() const { return impl_->matrix; }
double Font::Ascent() const { return impl_->ascent; }
double Font::Descent() const { return impl_->descent; }
double Font::SpaceWidth() const { return impl_->spaceWidth; }
bool Font::Unmappable() const { return impl_->unmappable; }

namespace {

// Appends `text` to the pool; its reference.
TextRef Pool(std::string& pool, std::string_view text) {
    TextRef ref{static_cast<uint32_t>(pool.size()), static_cast<uint32_t>(text.size())};
    pool.append(text.data(), text.size());
    return ref;
}

void SetSlot(Font::Impl& font, uint8_t code, std::string_view text, UnicodeSource source) {
    const TextRef ref = Pool(font.pool, text);
    font.slots[code].offset = ref.offset;
    font.slots[code].length = static_cast<uint16_t>(text.size() < 0xFFFF ? text.size() : 0xFFFF);
    font.slots[code].source = source;
}

// Only the usable entries of a parsed ToUnicode map stay (algorithms §3.6).
void FilterToUnicode(ToUnicodeMap& map) {
    size_t kept = 0;
    for (const UnicodeEntry& entry : map.entries) {
        if (UsableText(map.Text(entry))) map.entries[kept++] = entry;
    }
    map.entries.resize(kept);
}

bool LoadToUnicode(const Document& doc, const PdfValue& dict, ToUnicodeMap& out) {
    const PdfValue& stream = doc.Get(dict, "ToUnicode");
    if (!stream.IsStream()) return false;
    std::string data;
    const StreamStatus status = doc.StreamData(stream, data, kCMapStreamLimit);
    if (status == StreamStatus::Corrupt || status == StreamStatus::NotStream) return false;
    ParseToUnicode(data, out);
    FilterToUnicode(out);
    return !out.entries.empty();
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
    if (std::string_view(font.pool.data() + space.offset, space.length) == " " && space.width > 0) {
        font.spaceWidth = space.width;
        return;
    }
    for (const Slot& slot : font.slots) {
        if (std::string_view(font.pool.data() + slot.offset, slot.length) == " " && slot.width > 0) {
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

    const int standard = Tables::StandardFontByName(font.baseName);
    const bool symbolName = IsSymbolName(font.baseName) || standard == Tables::kSymbol;
    const bool zapf = standard == Tables::kZapfDingbats || ContainsNoCase(font.baseName, "dingbats");
    const bool symbolic = desc.hasFlags ? (desc.flags & kSymbolic) != 0 && !(desc.flags & kNonsymbolic)
                                        : symbolName || zapf;

    // Glyph names of the 256 codes and where each came from (§9.6.6).
    std::string_view names[256];
    UnicodeSource nameSource[256];
    for (auto& s : nameSource) s = UnicodeSource::Encoding;
    const PdfValue& encoding = doc.Get(dict, "Encoding");
    Tables::Encoding base = Tables::Encoding::Standard;
    bool hasBase = false;
    if (encoding.IsName()) {
        hasBase = EncodingByName(encoding.Name(), base);
    } else if (encoding.IsDict()) {
        hasBase = EncodingByName(doc.Get(encoding, "BaseEncoding").Name(), base);
    }

    std::string program;
    bool programLoaded = false;
    const auto loadProgram = [&]() -> std::string_view {
        if (!programLoaded) {
            programLoaded = true;
            if (!LoadProgram(doc, desc, program)) program.clear();
        }
        return program;
    };

    Program::Type1Encoding type1;
    Program::CffInfo cff;
    bool builtin = false;
    if (!hasBase) {
        if (symbolName) {
            base = Tables::Encoding::Symbol, hasBase = true;
        } else if (zapf) {
            base = Tables::Encoding::ZapfDingbats, hasBase = true;
        } else if ((kind == FontKind::Type1 || kind == FontKind::MMType1) && desc.fontFile &&
                   (symbolic || !encoding.IsName())) {
            // The program's built-in encoding (§9.6.6.2): Type 1 clear text or CFF.
            const std::string_view data = loadProgram();
            if (desc.fontFileKind == 1 && Program::ReadType1Encoding(data, type1)) {
                builtin = !type1.standard;
            } else if (desc.fontFileKind == 3) {
                std::string_view table = data;
                Program::Sfnt sfnt;
                if (Program::ReadSfnt(data, sfnt) && !sfnt.cff.empty()) table = sfnt.cff;
                if (Program::ReadCff(table, cff) && !cff.cidKeyed) {
                    if (cff.encoding == 1) base = Tables::Encoding::CffExpert, hasBase = true;
                    else builtin = cff.encoding == 2;
                }
            }
            if (!builtin && !hasBase) hasBase = true;  // Standard
        } else if (kind == FontKind::TrueType) {
            // pdf.js: a nonsymbolic TrueType font without an encoding reads as WinAnsi; a
            // symbolic one goes through its cmap below.
            if (!symbolic) base = Tables::Encoding::WinAnsi, hasBase = true;
        } else if (kind != FontKind::Type3) {
            hasBase = true;  // Standard
        }
    }
    for (int c = 0; c < 256; ++c) {
        if (builtin) {
            if (desc.fontFileKind == 1) {
                names[c] = type1.codes.Name(static_cast<size_t>(c));
            } else if (cff.codeGlyphs[c] >= 0) {
                names[c] = cff.glyphNames.Name(static_cast<size_t>(cff.codeGlyphs[c]));
            }
            nameSource[c] = UnicodeSource::FontProgram;
        } else if (hasBase) {
            names[c] = EncodingName(base, static_cast<uint32_t>(c));
        }
    }
    // /Differences: [code /name /name ... code /name ...]
    size_t differences = 0;
    size_t resolvedDifferences = 0;
    if (encoding.IsDict()) {
        const PdfSpan<PdfValue> diff = doc.Get(encoding, "Differences").Array();
        int64_t code = -1;
        for (const PdfValue& item : diff) {
            const PdfValue& v = doc.Resolve(item);
            if (v.IsNumber()) {
                code = v.Int(-1);
            } else if (v.IsName() && code >= 0 && code < 256) {
                if (v.Name() != ".notdef") {
                    names[code] = v.Name();
                    nameSource[code] = UnicodeSource::Encoding;
                    ++differences;
                }
                ++code;
            } else if (v.IsName()) {
                ++code;
            }
        }
    }

    ToUnicodeMap toUnicode;
    const bool hasToUnicode = LoadToUnicode(doc, dict, toUnicode);

    // Unicode per code, first source wins (algorithms §3.1).
    bool needProgram = false;
    std::string text;
    for (int c = 0; c < 256; ++c) {
        if (hasToUnicode) {
            if (const UnicodeEntry* entry = toUnicode.Find(static_cast<uint32_t>(c))) {
                SetSlot(font, static_cast<uint8_t>(c), toUnicode.Text(*entry), UnicodeSource::ToUnicode);
                continue;
            }
        }
        text.clear();
        if (!names[c].empty() && GlyphNameToUnicode(names[c], zapf, text) && UsableText(text)) {
            SetSlot(font, static_cast<uint8_t>(c), text, nameSource[c]);
            continue;
        }
        font.slots[c].source = UnicodeSource::None;
        // The program is read for names that mean nothing, or when no encoding names the codes.
        if (!names[c].empty() || (!hasBase && !builtin && !hasToUnicode)) needProgram = true;
    }
    if (encoding.IsDict() && differences > 0) {
        const PdfSpan<PdfValue> diff = doc.Get(encoding, "Differences").Array();
        for (const PdfValue& item : diff) {
            const PdfValue& v = doc.Resolve(item);
            if (v.IsName() && v.Name() != ".notdef") {
                text.clear();
                if (GlyphNameToUnicode(v.Name(), zapf, text) && UsableText(text)) ++resolvedDifferences;
            }
        }
    }

    // TrueType programs: code -> glyph through the (3,0) / (1,0) / (3,1) subtables (§9.6.6.4),
    // glyph -> text through the inverted Unicode cmap and the 'post' names.
    if (needProgram && kind == FontKind::TrueType && desc.fontFileKind == 2) {
        Program::Sfnt sfnt;
        if (Program::ReadSfnt(loadProgram(), sfnt)) {
            const uint32_t glyphCount = SfntGlyphCount(sfnt);
            std::vector<std::string> glyphText;
            GlyphTextFromSfnt(sfnt, glyphCount, glyphText);
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

    // Guess: the code as WinAnsi (Latin-1 for the printable range), never for controls.
    size_t mapped = 0;
    for (int c = 0; c < 256; ++c) {
        if (font.slots[c].source != UnicodeSource::None) {
            ++mapped;
            continue;
        }
        text.clear();
        const std::string_view name = EncodingName(Tables::Encoding::WinAnsi, static_cast<uint32_t>(c));
        if (!name.empty() && GlyphNameToUnicode(name, text) && UsableText(text)) {
            SetSlot(font, static_cast<uint8_t>(c), text, UnicodeSource::Guess);
        }
    }
    // Unmappable (algorithms §3.6): no ToUnicode and nothing better than a guess, or /Differences
    // whose names mostly mean nothing (subset fonts naming glyphs g12, cid33).
    font.unmappable = !hasToUnicode && (mapped == 0 || (differences > 0 && resolvedDifferences * 2 < differences));

    // Widths (§9.6.2, §9.6.5): /FirstChar + /Widths with /MissingWidth beyond; the standard 14
    // metrics when /Widths is absent.
    const double scale = kind == FontKind::Type3 ? font.matrix[0] : 0.001;
    const PdfValue& widthsValue = doc.Get(dict, "Widths");
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
    FinishSpaceWidthSimple(font);
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

// A predefined CMap name (§9.7.5.2): Identity-H/V, the Unicode-based Uni*-UCS2/UTF16/UTF8/UTF32,
// anything else read with 2-byte codes (no CJK CMap tables yet: algorithms §3.4 gap).
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

void LoadType0(const Document& doc, const PdfValue& dict, Font::Impl& font) {
    font.kind = FontKind::Type0;
    font.baseName = BaseNameOf(doc, dict);
    StyleFromName(font.baseName, font.style);

    // The encoding: a predefined CMap name or an embedded CMap stream.
    const PdfValue& encoding = doc.Get(dict, "Encoding");
    bool vertical = false;
    if (encoding.IsName()) {
        font.cmapKind = PredefinedKind(encoding.Name(), vertical);
    } else if (encoding.IsStream()) {
        std::string data;
        const StreamStatus status = doc.StreamData(encoding, data, kCMapStreamLimit);
        font.cmapKind = CMapKind::Embedded;
        if (status != StreamStatus::Corrupt && status != StreamStatus::NotStream) ParseEncodingCMap(data, font.cmap);
        vertical = font.cmap.wmode == 1 || doc.Get(encoding, "WMode").Int(0) == 1;
        if (font.cmap.codespace.Empty()) {
            // A CMap that only names another (usecmap, /UseCMap): that one's reading of codes.
            std::string_view use = font.cmap.useCMap;
            const std::string_view useEntry = doc.Get(encoding, "UseCMap").Name();
            if (use.empty()) use = useEntry;
            bool useVertical = false;
            const CMapKind used = use.empty() ? CMapKind::Identity : PredefinedKind(use, useVertical);
            vertical = vertical || useVertical;
            if (font.cmap.cids.empty() && font.cmap.notdefs.empty()) font.cmapKind = used;
            else if (used != CMapKind::Identity) UnicodeCodespace(used, font.cmap.codespace);
        }
    } else {
        font.cmapKind = CMapKind::Identity;
    }
    if (font.cmapKind != CMapKind::Identity && font.cmapKind != CMapKind::Embedded) {
        UnicodeCodespace(font.cmapKind, font.cmap.codespace);
    }
    font.vertical = vertical;

    // /DescendantFonts is a one-element array; some writers give the dictionary itself.
    const PdfValue& descendants = doc.Get(dict, "DescendantFonts");
    const PdfValue& cidFont = descendants.IsArray() && !descendants.Array().empty()
                                  ? doc.Resolve(descendants.Array()[0])
                                  : descendants;
    const std::string_view cidSubtype = doc.Get(cidFont, "Subtype").Name();
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

    const bool hasToUnicode = LoadToUnicode(doc, dict, font.toUnicode);
    const bool unicodeCMap = font.cmapKind == CMapKind::Ucs2 || font.cmapKind == CMapKind::Utf16 ||
                             font.cmapKind == CMapKind::Utf8 || font.cmapKind == CMapKind::Utf32;

    // The embedded program, only when nothing better maps the codes (algorithms §3.4).
    if (!hasToUnicode && !unicodeCMap && desc.fontFile) {
        std::string program;
        if (LoadProgram(doc, desc, program)) {
            std::vector<std::string> glyphText;
            Program::Sfnt sfnt;
            const bool isSfnt = Program::ReadSfnt(program, sfnt);
            if (cidSubtype == "CIDFontType2" && isSfnt) {
                GlyphTextFromSfnt(sfnt, SfntGlyphCount(sfnt), glyphText);
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
            font.glyphText.resize(glyphText.size());
            bool any = false;
            for (size_t g = 0; g < glyphText.size(); ++g) {
                if (glyphText[g].empty()) continue;
                font.glyphText[g] = Pool(font.pool, glyphText[g]);
                any = true;
            }
            if (!any) font.glyphText.clear();
        }
    }
    font.unmappable = !hasToUnicode && !unicodeCMap && font.glyphText.empty();

    // The space: a code whose text is U+0020.
    font.spaceWidth = 0;
    for (const UnicodeEntry& entry : font.toUnicode.entries) {
        if (font.toUnicode.Text(entry) == " ") {
            uint32_t cid = 0;
            const float w = font.CidOf(entry.code, cid) ? font.WidthOf(cid) : font.defaultWidth;
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
}

// The font FontCache gives for anything it cannot read: single bytes as WinAnsi, width 0.5.
void LoadFallback(Font::Impl& font) {
    font.kind = FontKind::Unknown;
    std::string text;
    for (int c = 0; c < 256; ++c) {
        font.slots[c].width = static_cast<float>(kUnknownWidth);
        text.clear();
        const std::string_view name = EncodingName(Tables::Encoding::WinAnsi, static_cast<uint32_t>(c));
        if (!name.empty() && GlyphNameToUnicode(name, text) && UsableText(text)) {
            SetSlot(font, static_cast<uint8_t>(c), text, UnicodeSource::Guess);
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
    const Font& result = *font;
    fonts_.emplace(key, std::move(font));
    return result;
}

size_t FontCache::Count() const {
    return fonts_.size();
}

} // namespace RayoPdf::Text
