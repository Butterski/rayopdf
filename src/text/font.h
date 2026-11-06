// SPDX-License-Identifier: Apache-2.0
// PDF fonts as text: from a font dictionary to character codes, their advances and their Unicode
// text (ISO 32000-1 sections 9.6-9.10, algorithms §3). Unicode comes, first match wins, from
// /ToUnicode, the predefined CMap's character collection, the simple font's encoding and
// /Differences glyph names (Adobe Glyph List and its uniXXXX/uXXXX[XX] forms), the embedded
// font's own tables (TrueType cmap, CFF charset and encoding), and finally a guess.
// Advances come from /Widths, /W, the standard 14 fonts' metrics, or /MissingWidth.
#pragma once

#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RayoPdf::Text {

enum class FontKind : uint8_t { Type1, MMType1, TrueType, Type3, Type0, Unknown };

struct FontStyle {
    bool bold = false;       // /FontWeight >= 600, ForceBold, or "Bold"/"Black"/"Heavy"/"Semibold" in the name
    bool italic = false;     // the Italic flag, a nonzero /ItalicAngle, or "Italic"/"Oblique" in the name
    bool monospace = false;  // the FixedPitch flag, or a Courier/Mono/Code/Consol name
    bool serif = false;      // the Serif flag
    bool symbolic = false;   // the Symbolic flag, or Symbol/ZapfDingbats/Wingdings
};

// How a code's Unicode text was found.
enum class UnicodeSource : uint8_t {
    ToUnicode,     // the font's /ToUnicode CMap
    Collection,    // a predefined CMap's character collection (Adobe-Japan1, -GB1, -CNS1, -Korea1)
    Encoding,      // a simple font's encoding or /Differences glyph name
    FontProgram,   // the embedded font's cmap, charset or glyph names
    Guess,         // the code itself read as Latin-1 / WinAnsi, or as Unicode for Identity CIDs
    None,          // nothing: `unicode` is empty
};

// One character code of a shown string.
struct Code {
    uint32_t code = 0;
    uint8_t bytes = 1;          // its length in the string: 1 for simple fonts, 1-4 for Type0
    double width = 0;           // horizontal advance in text space at font size 1 (glyph-space
                                // width / 1000, or through the FontMatrix for Type 3)
    double verticalAdvance = 0; // vertical fonts: advance at size 1 (negative is downwards)
    bool wordSpace = false;     // a single-byte code 32: the word spacing Tw applies
    UnicodeSource source = UnicodeSource::None;
    std::string_view unicode;   // UTF-8 (one or more code points; ligatures stay as mapped),
                                // valid while the Font lives
};

class Font {
public:
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Reads the next code of `bytes` from `at`, which moves past it. False at the end; a trailing
    // partial code of a multi-byte font is read as one code of the remaining bytes.
    bool Next(std::string_view bytes, size_t& at, Code& out) const;

    FontKind Kind() const;
    // /BaseFont without a subset tag ("ABCDEF+"); "" for Type 3 fonts without a name.
    std::string_view BaseName() const;
    const FontStyle& Style() const;
    // Writing mode 1 (a Type0 font with a -V CMap).
    bool Vertical() const;
    // The /FontMatrix of a Type 3 font; {0.001, 0, 0, 0.001, 0, 0} for all others.
    const double* Matrix() const;
    // Text-space extents at size 1 for glyph boxes: the descriptor's /Ascent and /Descent (or
    // the font bounding box) divided by 1000; 0.8 and -0.2 when unknown.
    double Ascent() const;
    double Descent() const;
    // The advance of the space character when the font has one (code 32 of a simple font, or
    // the code that maps to U+0020); 0 when unknown. Word-gap detection uses it.
    double SpaceWidth() const;
    // True when the font has no ToUnicode map and its codes cannot be mapped by name or by
    // collection: its text is unreliable and the converter marks it.
    bool Unmappable() const;

    // The loaded tables, defined in font.cpp (public only so its loaders can name them).
    struct Impl;

private:
    friend class FontCache;
    explicit Font(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// The fonts of one document, each loaded once: keyed by the object number of the font
// dictionary (or the address of a direct dictionary). Get never fails: a font that cannot be read
// becomes a fallback that reads single bytes as WinAnsi with a width of 0.5.
class FontCache {
public:
    explicit FontCache(const Document& document);
    ~FontCache();
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;

    // The font of a /Font resource entry (a reference to a font dictionary, or the dictionary).
    const Font& Get(const PdfValue& fontEntry);
    // Fonts loaded so far.
    size_t Count() const;

private:
    const Document& document_;
    std::unordered_map<uintptr_t, std::unique_ptr<Font>> fonts_;
};

// The Unicode text of an Adobe glyph name (AGL plus the uniXXXX, uXXXX[XX] and "f_f_i" ligature
// forms; "a.sc" suffixes stripped), appended to `out` as UTF-8. False when the name means nothing.
bool GlyphNameToUnicode(std::string_view name, std::string& out);

} // namespace RayoPdf::Text
