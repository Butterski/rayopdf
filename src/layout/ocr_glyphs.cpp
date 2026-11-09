// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_glyphs.h"

#include "common/utf8.h"
#include "layout/interpreter.h"
#include "layout/ocr_layer.h"
#include "layout/ocr_paint.h"
#include "layout/ocr_route.h"
#include "raster/raster.h"
#include "text/repair.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>

namespace RayoPdf::Layout {

namespace {

constexpr uint32_t kLinePad = 4;          // pixels above and below the glyphs' extent
constexpr uint32_t kLineMargin = 8;       // pixels left and right of a line's glyphs
constexpr float kGapEm = 0.35f;           // between two glyphs' ink, in em
constexpr uint32_t kMinGap = 6;           // ... in pixels at least
constexpr float kCellEm = 4.0f;           // the canvas one glyph is drawn on, in em
constexpr float kOriginEm = 1.5f;         // ... with the glyph's origin this far in
constexpr double kProbeScale = 64;        // pixels per em of the first drawing
constexpr double kProbeBaselineEm = 2.5;  // its baseline, from the top
constexpr double kMinExtentEm = 0.3;      // the least vertical extent a line is scaled to
constexpr size_t kRobustExtentGlyphs = 20;  // from this many glyphs the extent leaves out 5 % each way
constexpr uint32_t kMaxLineWidth = 2400;  // pixels (the recogniser takes up to 3,200 at its height)
constexpr uint8_t kInk = 200;             // a pixel this dark or darker is ink
constexpr uint16_t kGlyphMinConf = kOcrMinConf;   // a line below this gives no glyph text
constexpr uint16_t kGlyphMinFontConf = 7000;      // the font's lines on average (per glyph)
constexpr uint16_t kGlyphMinWholeConf = 8000;     // ... when every code of the font is replaced
constexpr uint32_t kWholeMinShapes = 4;           // a whole font is replaced from this many shapes read
constexpr uint32_t kScriptMinLetters = 5;         // the one-script check from this many letters
constexpr float kWordGapEm = 0.3f;                 // a glyph this close to a letter forms a word with it
// Words of complex-script fonts (RecogniseWords).
constexpr float kWordJoinEm = 0.25f;               // a glyph starting this close after the last joins its word
constexpr float kWordSpaceEm = 0.6f;               // between two words of a line
constexpr uint32_t kMaxWordGlyphs = 64;
constexpr uint32_t kMaxWords = 8192;               // distinct words drawn for one font
constexpr uint32_t kWordsPerLine = 12;
constexpr uint32_t kWordsPerRetry = 4;
constexpr float kWordRetryGap = 2.5f;

// A font judged as a whole (FontStats) when it has at least this much text on the pages.
constexpr uint32_t kWholeMinGlyphs = 4;
constexpr uint32_t kWholeMinLatin = 20;
constexpr uint32_t kWholeMinWordLetters = 50;

constexpr uint16_t kSkipped = kGlyphDropped | kGlyphClipped | kGlyphImage;

bool LatinLetter(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7);
}

bool Vowel(uint32_t cp) {
    if (cp < 0x80) {
        const uint32_t c = cp | 0x20;
        return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
    }
    if (cp >= 0xE0) cp -= 0x20;
    return (cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xC8 && cp <= 0xCF) || (cp >= 0xD2 && cp <= 0xD6) ||
           (cp >= 0xD8 && cp <= 0xDD) || cp == 0xDF;
}

// The script of a letter (0 Latin, 1 Greek, 2 Cyrillic, 3 CJK, 4 any other), -1 for anything else
// (digits, punctuation, symbols).
constexpr int kScriptCount = 5;
constexpr int kComplexScript = 4;
int ScriptOf(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) ||
        (cp >= 0x1E00 && cp <= 0x1EFF))
        return 0;
    if ((cp >= 0x370 && cp <= 0x3FF) || (cp >= 0x1F00 && cp <= 0x1FFF)) return 1;
    if (cp >= 0x400 && cp <= 0x52F) return 2;
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
        (cp >= 0xF900 && cp <= 0xFAFF))
        return 3;
    if ((cp >= 0x590 && cp <= 0x1DFF) || (cp >= 0xA000 && cp <= 0xABFF)) return 4;  // Hebrew, Arabic, Indic, ...
    return -1;
}

bool SpaceCodePoint(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0 || cp == 0x3000 ||
           (cp >= 0x2000 && cp <= 0x200A);
}

// What a font's own text on the candidate pages looks like (the census checks, per font).
struct FontStats {
    uint32_t glyphs = 0, unreliable = 0, junkGlyphs = 0;
    uint32_t latin = 0, accented = 0, wordLetters = 0, wordVowels = 0;
};

// A use of a glyph to recover: where it is and which drawn glyph it shows.
struct Use {
    uint32_t page = 0;   // index into pages
    uint32_t glyph = 0;  // index into its glyphs
    uint32_t text = 0;   // Glyph::text when it was emitted
    uint32_t slot = 0;   // index into FontWork::glyphIds
    bool weak = false;   // its text is junk, unmapped or guessed
};

struct FontWork {
    const Text::Font* font = nullptr;
    std::unique_ptr<Text::FontGlyphs> glyphs;
    bool whole = false;                         // every code is examined, not only weak ones
    FontStats stats;
    std::vector<uint32_t> glyphIds;             // distinct glyphs, in order of first use
    std::unordered_map<uint32_t, uint32_t> slotOf;
    std::vector<std::string> texts;             // per slot: the recognised text, empty when none
    std::vector<Use> uses;
    std::vector<uint32_t> candidatePages;       // candidate pages (indices into pages) it shows on
    bool plausible = false;                     // RecogniseFont: the recognition looks right
    // RecogniseFont: the line's scale (pixels per em) and baseline (pixels from the top), and the
    // letters read with the script most of them have (ScriptOf).
    double scale = 0, baseline = 0;
    uint32_t letters = 0;
    int script = -1;
    // Words (RecogniseWords): the text of each distinct word, and per use its word (UINT32_MAX:
    // none) and whether it is the word's first glyph, which takes the word's text.
    bool words = false;
    std::vector<std::string> wordTexts;
    std::vector<uint32_t> useWord;
    std::vector<uint8_t> useHead;
};

// Whether a glyph's text is weak: unmapped, guessed or holding junk.
bool Weak(const PageData& page, const Glyph& g) {
    if (g.flags & (kGlyphUnmapped | kGlyphGuessed)) return true;
    const std::string_view text = page.GlyphText(g);
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        if (Text::Repair::IsJunk(cp)) return true;
        at += length;
    }
    return false;
}

// Adds a page's glyphs of each font to its FontStats (words of Latin letters end at a space, a
// character of another kind or a change of font).
void CountFonts(const PageData& page, const std::vector<GlyphUse>& uses,
                std::unordered_map<const Text::Font*, FontStats>& stats) {
    const Text::Font* wordFont = nullptr;
    uint32_t word = 0, vowels = 0;
    const auto endWord = [&] {
        if (wordFont && word >= 3 && word <= 20) {
            FontStats& s = stats[wordFont];
            s.wordLetters += word, s.wordVowels += vowels;
        }
        word = vowels = 0;
    };
    for (const GlyphUse& use : uses) {
        if (use.glyph >= page.glyphs.size()) continue;
        const Glyph& g = page.glyphs[use.glyph];
        if (g.flags & kGlyphSpace) {
            endWord();
            continue;
        }
        if ((g.flags & kSkipped) || g.text != use.text || !(g.size >= 1.0f)) continue;
        if (use.font != wordFont) endWord(), wordFont = use.font;
        FontStats& s = stats[use.font];
        s.glyphs++;
        if (g.flags & (kGlyphUnmapped | kGlyphGuessed)) s.unreliable++;
        bool junk = false;
        const std::string_view text = page.GlyphText(g);
        size_t at = 0, length = 0;
        uint32_t cp = 0;
        while (Utf8::Decode(text, at, cp, length)) {
            at += length;
            junk = junk || Text::Repair::IsJunk(cp);
            if (!LatinLetter(cp)) {
                endWord();
                wordFont = use.font;
                continue;
            }
            s.latin++;
            if (cp >= 0xC0) s.accented++;
            word++;
            if (Vowel(cp)) vowels++;
        }
        if (junk) s.junkGlyphs++;
    }
    endWord();
}

bool WholeFont(const FontStats& s) {
    return (s.glyphs >= kWholeMinGlyphs && s.unreliable * 2 > s.glyphs) ||
           (s.latin >= kWholeMinLatin && s.accented * 2 >= s.latin) ||
           (s.wordLetters >= kWholeMinWordLetters && s.wordVowels * 4 < s.wordLetters);
}

// One glyph drawn on its own canvas, and where its ink is.
struct Cell {
    std::string pixels;           // `height` rows of `width`
    uint32_t width = 0, height = 0;
    uint32_t ink0 = 0, ink1 = 0;  // columns [ink0, ink1) hold ink
    uint32_t top = 0, bottom = 0; // rows [top, bottom) hold ink
};

class GlyphPainter {
public:
    enum class Drawn : uint8_t { Ink, Blank, Failed };

    GlyphPainter(const Document& doc, const Text::Font& font, const Text::FontGlyphs& glyphs)
        : doc_(doc), font_(font), glyphs_(glyphs) {}

    // Draws `glyph` at `scale` pixels per em onto a canvas `width` x `height`, its origin at
    // (`originX`, `baseline`) pixels from the top left: Ink, Blank when it draws nothing (a
    // space), Failed when its outline or procedure cannot be read.
    Drawn Draw(uint32_t glyph, double scale, double originX, double baseline, uint32_t width, uint32_t height,
               Cell& cell) {
        cell.width = width, cell.height = height;
        cell.pixels.assign(size_t(width) * height, static_cast<char>(0xFF));
        cell.ink0 = cell.ink1 = cell.top = cell.bottom = 0;
        if (glyphs_.GetKind() == Text::FontGlyphs::Kind::Type3) {
            const PdfValue* proc = glyphs_.Procedure(glyph);
            if (!proc) return Drawn::Failed;
            const StreamStatus read = doc_.StreamData(*proc, content_);
            if (read == StreamStatus::NotStream || read == StreamStatus::Corrupt) return Drawn::Failed;
            // The canvas's region is its pixels with y up: point (x, y) is pixel (x, height - y).
            OcrCanvas canvas;
            canvas.x0 = 0, canvas.y0 = 0, canvas.x1 = float(width), canvas.y1 = float(height);
            canvas.scale = 1;
            canvas.width = width, canvas.height = height;
            canvas.pixels.swap(cell.pixels);
            const double* fm = font_.Matrix();
            const Raster::Matrix place{scale, 0, 0, scale, originX, double(height) - baseline};
            const Raster::Matrix ctm = Raster::Matrix{fm[0], fm[1], fm[2], fm[3], fm[4], fm[5]}.Then(place);
            PaintStats stats;
            PaintContent(doc_, content_, glyphs_.Resources(), ctm, PaintMode::Glyph, canvas, raster_, stats);
            canvas.pixels.swap(cell.pixels);
        } else {
            path_.Clear();
            if (!glyphs_.Outline(glyph, path_)) return Drawn::Failed;
            const Raster::Matrix toPixels{scale, 0, 0, -scale, originX, baseline};
            raster_.Fill(path_, toPixels, Raster::FillRule::NonZero, reinterpret_cast<uint8_t*>(&cell.pixels[0]),
                         width, height);
        }
        uint32_t x0 = width, x1 = 0, y0 = height, y1 = 0;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = reinterpret_cast<const uint8_t*>(cell.pixels.data()) + size_t(y) * width;
            for (uint32_t x = 0; x < width; ++x) {
                if (row[x] > kInk) continue;
                x0 = std::min(x0, x), x1 = std::max(x1, x + 1);
                y0 = std::min(y0, y), y1 = std::max(y1, y + 1);
            }
        }
        if (x1 <= x0) return Drawn::Blank;
        cell.ink0 = x0, cell.ink1 = x1, cell.top = y0, cell.bottom = y1;
        return Drawn::Ink;
    }

private:
    const Document& doc_;
    const Text::Font& font_;
    const Text::FontGlyphs& glyphs_;
    Raster::Rasterizer raster_;
    Raster::Path path_;
    std::string content_;
};

// The value at `share` of the sorted `values` (0 the smallest, 1 the largest).
double Quantile(std::vector<double>& values, double share) {
    std::sort(values.begin(), values.end());
    const size_t at = static_cast<size_t>(share * double(values.size() - 1) + 0.5);
    return values[std::min(at, values.size() - 1)];
}

// The glyphs of one font drawn as line images and recognised: work.texts filled.
OcrStatus RecogniseFont(const Document& doc, OcrSession& session, FontWork& work, GlyphRecoveryStats& stats) {
    work.texts.assign(work.glyphIds.size(), std::string());
    GlyphPainter painter(doc, *work.font, *work.glyphs);
    Cell cell;

    // A first drawing at kProbeScale finds the glyphs' vertical extent in em (the line's scale
    // makes the extent of nearly all of them, a few very tall ones aside, fill its height) and
    // glyphs of the same shape: each shape is recognised once.
    std::vector<uint32_t> inked;                       // one slot per distinct shape
    std::vector<uint32_t> sameAs(work.glyphIds.size(), UINT32_MAX);  // a duplicate's inked slot
    std::unordered_map<uint64_t, uint32_t> shapes;
    std::vector<double> tops, bottoms;
    {
        const double scale = kProbeScale;
        const uint32_t size = static_cast<uint32_t>(kCellEm * scale);
        const double baseline = kProbeBaselineEm * scale;
        for (uint32_t slot = 0; slot < work.glyphIds.size(); ++slot) {
            const GlyphPainter::Drawn drawn =
                painter.Draw(work.glyphIds[slot], scale, kOriginEm * scale, baseline, size, size, cell);
            if (drawn == GlyphPainter::Drawn::Blank) work.texts[slot] = " ";  // draws nothing: a space
            if (drawn != GlyphPainter::Drawn::Ink) continue;
            uint64_t hash = 14695981039346656037ull;
            const auto mix = [&](uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
            mix(cell.ink1 - cell.ink0), mix(cell.bottom - cell.top), mix(cell.top);
            for (uint32_t y = cell.top; y < cell.bottom; ++y)
                for (uint32_t x = cell.ink0; x < cell.ink1; ++x) mix(static_cast<uint8_t>(cell.pixels[size_t(y) * size + x]));
            const auto known = shapes.emplace(hash, slot);
            if (!known.second) {
                sameAs[slot] = known.first->second;
                continue;
            }
            inked.push_back(slot);
            tops.push_back((baseline - cell.top) / scale);
            bottoms.push_back((baseline - cell.bottom) / scale);
        }
    }
    if (inked.empty()) {
        // Glyphs that draw nothing are spaces, whatever the recogniser would say.
        work.plausible = std::any_of(work.texts.begin(), work.texts.end(), [](const std::string& t) { return !t.empty(); });
        return OcrStatus::Ok;
    }
    const bool many = inked.size() >= kRobustExtentGlyphs;
    double top = Quantile(tops, many ? 0.95 : 1.0), bottom = Quantile(bottoms, many ? 0.05 : 0.0);
    if (!many) {
        // A few glyphs (bullets, symbols) keep their size within the font's em box.
        top = std::max(top, std::min(std::max(work.font->Ascent(), 0.5), 1.2));
        bottom = std::min(bottom, std::max(std::min(work.font->Descent(), 0.0), -0.5));
    }
    const double extent = std::max(top - bottom, kMinExtentEm);
    const double scale = double(kGlyphLineHeight - 2 * kLinePad) / extent;
    const double baseline = kLinePad + top * scale;
    work.scale = scale, work.baseline = baseline;
    const uint32_t width = static_cast<uint32_t>(std::ceil(kCellEm * scale));
    const uint32_t gap = std::max<uint32_t>(kMinGap, static_cast<uint32_t>(std::ceil(kGapEm * scale)));

    // The glyphs at the line's scale, side by side in order of first use with a gap between their
    // ink, as letter-spaced text.
    struct Placed {
        uint32_t slot;
        uint32_t x0, x1;  // the ink's columns in the line
    };
    std::vector<std::vector<Placed>> lines;
    std::vector<std::string> images;
    OcrProtocol::Request request;
    request.op = OcrProtocol::Op::Lines;
    std::vector<Cell> pending;
    std::vector<Placed> placed;
    uint32_t x = kLineMargin;
    const auto flush = [&] {
        if (placed.empty()) return;
        const uint32_t lineWidth = x - gap + kLineMargin;
        std::string image(size_t(lineWidth) * kGlyphLineHeight, static_cast<char>(0xFF));
        for (size_t k = 0; k < placed.size(); ++k) {
            const Cell& c = pending[k];
            for (uint32_t y = 0; y < kGlyphLineHeight; ++y)
                std::copy_n(c.pixels.data() + size_t(y) * c.width + c.ink0, c.ink1 - c.ink0,
                            &image[size_t(y) * lineWidth + placed[k].x0]);
        }
        request.lines.push_back({lineWidth, kGlyphLineHeight});
        images.push_back(std::move(image));
        lines.push_back(std::move(placed));
        placed.clear();
        pending.clear();
        x = kLineMargin;
    };
    for (const uint32_t slot : inked) {
        if (painter.Draw(work.glyphIds[slot], scale, kOriginEm * scale, baseline, width, kGlyphLineHeight, cell) !=
            GlyphPainter::Drawn::Ink)
            continue;
        const uint32_t w = cell.ink1 - cell.ink0;
        if (!placed.empty() && (placed.size() >= kGlyphsPerLine || x + w + kLineMargin > kMaxLineWidth)) flush();
        placed.push_back({slot, x, x + w});
        pending.push_back(cell);
        x += w + gap;
        stats.glyphsSent++;
    }
    flush();
    if (lines.empty()) return OcrStatus::Ok;

    std::string pixels;
    for (const std::string& image : images) pixels += image;
    OcrProtocol::Response response;
    stats.requests++;
    const OcrStatus status = session.Recognize(request, pixels, response);
    if (status != OcrStatus::Ok) return status;
    if (response.lines.size() != lines.size()) return OcrStatus::Ok;  // nothing usable

    // Each character goes to the glyph whose ink centre its span holds (character spans run
    // between the middles of neighbouring characters' time steps, so a character's span holds
    // its own glyph's centre), the one nearest the span's middle when it holds several, else to
    // the glyph nearest the middle (the second character of a ligature).
    uint64_t confSum = 0, cellCount = 0;
    for (size_t l = 0; l < lines.size(); ++l) {
        const OcrProtocol::Line& answer = response.lines[l];
        confSum += uint64_t(answer.conf) * lines[l].size();
        cellCount += lines[l].size();
        if (answer.conf < kGlyphMinConf) continue;
        const std::vector<Placed>& cells = lines[l];
        size_t at = 0, length = 0, k = 0;
        uint32_t cp = 0;
        while (Utf8::Decode(answer.text, at, cp, length) && k < answer.chars.size()) {
            const std::string_view bytes(answer.text.data() + at, length);
            at += length;
            const OcrProtocol::CharSpan span = answer.chars[k++];
            if (SpaceCodePoint(cp) || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) continue;
            const int64_t middle2 = int64_t(span.x0) + span.x1;  // twice the middle
            const Placed* best = nullptr;
            int64_t bestDistance = INT64_MAX;
            bool inside = false;
            for (const Placed& p : cells) {
                const int64_t centre2 = int64_t(p.x0) + p.x1;
                const bool holds = centre2 >= 2 * int64_t(span.x0) && centre2 < 2 * int64_t(span.x1);
                const int64_t distance = centre2 > middle2 ? centre2 - middle2 : middle2 - centre2;
                if ((holds && !inside) || (holds == inside && distance < bestDistance)) {
                    best = &p, bestDistance = distance, inside = holds;
                }
            }
            if (!best) continue;
            std::string& text = work.texts[best->slot];
            if (text.size() < 64) text.append(bytes.data(), bytes.size());
        }
    }
    for (uint32_t slot = 0; slot < sameAs.size(); ++slot)
        if (sameAs[slot] != UINT32_MAX) work.texts[slot] = work.texts[sameAs[slot]];
    // Plausible recognition: confident on the whole, most glyphs read, and the letters of one
    // script (shapes of a script the recogniser lacks come out as a scatter of look-alikes from
    // several, at low confidence, many of them unread). Replacing every code of a font asks more:
    // higher confidence, and several shapes (one symbol of a math font is no evidence).
    uint32_t read = 0, scripts[kScriptCount] = {};
    for (const uint32_t slot : inked) {
        const std::string& text = work.texts[slot];
        if (text.empty()) continue;
        ++read;
        size_t at = 0, length = 0;
        uint32_t cp = 0;
        while (Utf8::Decode(text, at, cp, length)) {
            at += length;
            const int script = ScriptOf(cp);
            if (script >= 0) scripts[script]++;
        }
    }
    uint32_t letters = 0, dominant = 0, dominantScript = 0;
    for (uint32_t s = 0; s < kScriptCount; ++s) {
        letters += scripts[s];
        if (scripts[s] > dominant) dominant = scripts[s], dominantScript = s;
    }
    work.letters = letters;
    work.script = letters > 0 ? int(dominantScript) : -1;
    const uint16_t minConf = work.whole ? kGlyphMinWholeConf : kGlyphMinFontConf;
    work.plausible = cellCount > 0 && confSum >= uint64_t(minConf) * cellCount &&
                     read * 4 >= 3 * uint32_t(inked.size()) && (!work.whole || read >= kWholeMinShapes) &&
                     (letters < kScriptMinLetters || dominant * 10 >= 9 * letters);
    // Full-width forms are a matter of width, not of shape: outside CJK text they are the ASCII
    // characters (the recogniser picks them for letters set apart in a line of single glyphs).
    if (letters == 0 || dominantScript != 3) {
        std::string ascii;
        for (std::string& text : work.texts) {
            ascii.clear();
            size_t at = 0, length = 0;
            uint32_t cp = 0;
            bool changed = false;
            while (Utf8::Decode(text, at, cp, length)) {
                if (cp >= 0xFF01 && cp <= 0xFF5E) {
                    ascii += static_cast<char>(cp - 0xFEE0);
                    changed = true;
                } else {
                    ascii.append(text, at, length);
                }
                at += length;
            }
            if (changed) text.swap(ascii);
        }
    }
    stats.glyphsRead += read;
    return OcrStatus::Ok;
}

// The glyphs of a font of a complex script drawn as the words they form on the pages, and the
// words recognised (a legacy-encoded Devanagari, Kannada or Tamil font draws a syllable from
// several glyphs, a vowel sign before its consonant: a glyph alone reads as nothing or as another
// letter). A word is a run of the font's uses in content order, on one baseline, each glyph the
// one after the last on its page and starting at most kWordJoinEm after its end. Each distinct
// word (its glyphs and their offsets) is drawn once at the font's line scale, glyphs at their
// places, and words go side by side kWordSpaceEm apart into lines; a line whose text splits at its
// spaces into as many words gives each word its text, the others' words go again, fewer to a line
// and further apart, and the rest are recognised alone.
// Plausible as RecogniseFont's: confident, most words read, one script.
OcrStatus RecogniseWords(const Document& doc, OcrSession& session, const std::vector<PageData>& pages, FontWork& work,
                         GlyphRecoveryStats& stats) {
    work.words = true;
    work.plausible = false;
    work.wordTexts.clear();
    work.useWord.assign(work.uses.size(), UINT32_MAX);
    work.useHead.assign(work.uses.size(), 0);
    if (!(work.scale > 0)) return OcrStatus::Ok;
    const auto glyphOf = [&](const Use& use) -> const Glyph& { return pages[use.page].glyphs[use.glyph]; };

    // Words, and the distinct ones (their slots and offsets in 1/16 em as the key).
    struct Word {
        uint32_t first = 0, count = 0;  // uses [first, first + count) of the first occurrence
    };
    std::vector<Word> distinct;
    std::unordered_map<std::string, uint32_t> known;
    std::string key;
    for (uint32_t i = 0; i < work.uses.size();) {
        uint32_t end = i + 1;
        const Glyph& head = glyphOf(work.uses[i]);
        if (!(head.size >= 1.0f)) {
            i = end;
            continue;
        }
        for (; end < work.uses.size() && end - i < kMaxWordGlyphs; ++end) {
            const Use& prev = work.uses[end - 1];
            const Use& next = work.uses[end];
            const Glyph& p = glyphOf(prev);
            const Glyph& n = glyphOf(next);
            if (next.page != prev.page || next.glyph != prev.glyph + 1 || n.dir != head.dir ||
                std::fabs(n.v - head.v) > 0.3f * head.size || n.u0 - p.u1 > kWordJoinEm * head.size ||
                n.u0 < head.u0 - head.size) {
                break;
            }
        }
        key.clear();
        for (uint32_t k = i; k < end; ++k) {
            const Glyph& g = glyphOf(work.uses[k]);
            const int32_t dx = int32_t(std::lround((g.u0 - head.u0) / head.size * 16));
            const int32_t dy = int32_t(std::lround((g.v - head.v) / head.size * 16));
            key.append(reinterpret_cast<const char*>(&work.uses[k].slot), sizeof(uint32_t));
            key.append(reinterpret_cast<const char*>(&dx), sizeof dx);
            key.append(reinterpret_cast<const char*>(&dy), sizeof dy);
        }
        auto it = known.find(key);
        if (it == known.end()) {
            if (distinct.size() >= kMaxWords) break;
            it = known.emplace(key, static_cast<uint32_t>(distinct.size())).first;
            distinct.push_back({i, end - i});
        }
        for (uint32_t k = i; k < end; ++k) work.useWord[k] = it->second;
        work.useHead[i] = 1;
        i = end;
    }
    if (distinct.empty()) return OcrStatus::Ok;
    work.wordTexts.assign(distinct.size(), std::string());

    // Each distinct word drawn: its glyphs at their offsets, the darker pixel winning.
    GlyphPainter painter(doc, *work.font, *work.glyphs);
    Cell cell;
    struct Image {
        std::string pixels;  // kGlyphLineHeight rows
        uint32_t width = 0;
    };
    std::vector<Image> images(distinct.size());
    const double scale = work.scale;
    for (size_t d = 0; d < distinct.size(); ++d) {
        const Word& word = distinct[d];
        const Glyph& head = glyphOf(work.uses[word.first]);
        double right = 0;
        for (uint32_t k = word.first; k < word.first + word.count; ++k) {
            const Glyph& g = glyphOf(work.uses[k]);
            right = std::max(right, double(g.u1 - head.u0) / head.size);
        }
        // The word's em extent with an em either side for marks that overhang their origins.
        const uint32_t width = static_cast<uint32_t>(std::ceil((right + 2.0) * scale));
        if (width > kMaxLineWidth) continue;
        std::string canvas(size_t(width) * kGlyphLineHeight, static_cast<char>(0xFF));
        for (uint32_t k = word.first; k < word.first + word.count; ++k) {
            const Glyph& g = glyphOf(work.uses[k]);
            const double x = (1.0 + double(g.u0 - head.u0) / head.size) * scale;
            const double y = work.baseline - double(g.v - head.v) / head.size * scale;
            if (painter.Draw(work.glyphIds[work.uses[k].slot], scale, x, y, width, kGlyphLineHeight, cell) !=
                GlyphPainter::Drawn::Ink)
                continue;
            for (size_t p = 0; p < canvas.size(); ++p) canvas[p] = std::min(canvas[p], cell.pixels[p], [](char a, char b) {
                return static_cast<uint8_t>(a) < static_cast<uint8_t>(b);
            });
        }
        // Cut to the columns with ink.
        uint32_t x0 = width, x1 = 0;
        for (uint32_t y = 0; y < kGlyphLineHeight; ++y)
            for (uint32_t x = 0; x < width; ++x)
                if (static_cast<uint8_t>(canvas[size_t(y) * width + x]) <= kInk) x0 = std::min(x0, x), x1 = std::max(x1, x + 1);
        if (x1 <= x0) continue;
        Image& image = images[d];
        image.width = x1 - x0;
        image.pixels.resize(size_t(image.width) * kGlyphLineHeight);
        for (uint32_t y = 0; y < kGlyphLineHeight; ++y)
            std::copy_n(canvas.data() + size_t(y) * width + x0, image.width, &image.pixels[size_t(y) * image.width]);
    }

    // Lines of kWordsPerLine words kWordSpaceEm apart; the words of lines that did not split into
    // as many go again kWordsPerRetry to a line, kWordRetryGap times as far apart; the rest alone.
    std::vector<std::vector<uint32_t>> lines;
    std::vector<uint8_t> read(distinct.size(), 0);
    uint64_t confSum = 0, confCount = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const float space = pass == 1 ? kWordRetryGap * kWordSpaceEm : kWordSpaceEm;
        const uint32_t gap = std::max<uint32_t>(kMinGap, static_cast<uint32_t>(std::ceil(space * scale)));
        const size_t perLine = pass == 0 ? kWordsPerLine : pass == 1 ? kWordsPerRetry : 1;
        lines.clear();
        std::vector<uint32_t> line;
        uint32_t x = kLineMargin;
        for (uint32_t d = 0; d < distinct.size(); ++d) {
            if (images[d].width == 0 || read[d]) continue;
            const uint32_t w = images[d].width;
            if (!line.empty() && (line.size() >= perLine || x + w + kLineMargin > kMaxLineWidth)) {
                lines.push_back(line);
                line.clear();
                x = kLineMargin;
            }
            line.push_back(d);
            x += w + gap;
        }
        if (!line.empty()) lines.push_back(line);
        if (lines.empty()) break;
        OcrProtocol::Request request;
        request.op = OcrProtocol::Op::Lines;
        std::string pixels;
        for (const std::vector<uint32_t>& l : lines) {
            uint32_t lineWidth = 2 * kLineMargin;
            for (uint32_t d : l) lineWidth += images[d].width + gap;
            lineWidth -= gap;
            const size_t at = pixels.size();
            pixels.append(size_t(lineWidth) * kGlyphLineHeight, static_cast<char>(0xFF));
            uint32_t x0 = kLineMargin;
            for (uint32_t d : l) {
                for (uint32_t y = 0; y < kGlyphLineHeight; ++y)
                    std::copy_n(images[d].pixels.data() + size_t(y) * images[d].width, images[d].width,
                                &pixels[at + size_t(y) * lineWidth + x0]);
                x0 += images[d].width + gap;
            }
            request.lines.push_back({lineWidth, kGlyphLineHeight});
        }
        OcrProtocol::Response response;
        stats.requests++;
        const OcrStatus status = session.Recognize(request, pixels, response);
        if (status != OcrStatus::Ok) return status;
        if (response.lines.size() != lines.size()) return OcrStatus::Ok;  // nothing usable
        for (size_t l = 0; l < lines.size(); ++l) {
            const OcrProtocol::Line& answer = response.lines[l];
            std::vector<std::string> tokens;
            std::string token;
            for (char c : answer.text + " ") {
                if (c != ' ') {
                    token.push_back(c);
                } else if (!token.empty()) {
                    tokens.push_back(token);
                    token.clear();
                }
            }
            if (answer.conf < kGlyphMinConf) continue;
            // A word alone takes the whole reading, whatever spaces it holds.
            if (lines[l].size() == 1 && !tokens.empty()) {
                std::string joined;
                for (const std::string& t : tokens) joined += t;
                tokens.assign(1, joined);
            }
            if (tokens.size() != lines[l].size()) continue;
            for (size_t k = 0; k < tokens.size(); ++k) {
                const uint32_t d = lines[l][k];
                if (tokens[k].size() < 256) work.wordTexts[d] = tokens[k];
                read[d] = 1;
                confSum += answer.conf;
                confCount++;
            }
        }
    }
    // Plausible: confident, most words read, the letters of one script.
    uint32_t wordsRead = 0, drawn = 0, scripts[kScriptCount] = {};
    for (size_t d = 0; d < distinct.size(); ++d) {
        if (images[d].width == 0) continue;
        ++drawn;
        if (work.wordTexts[d].empty()) continue;
        ++wordsRead;
        size_t at = 0, length = 0;
        uint32_t cp = 0;
        while (Utf8::Decode(work.wordTexts[d], at, cp, length)) {
            at += length;
            const int script = ScriptOf(cp);
            if (script >= 0) scripts[script]++;
        }
    }
    uint32_t letters = 0, dominant = 0;
    for (uint32_t s = 0; s < kScriptCount; ++s) letters += scripts[s], dominant = std::max(dominant, scripts[s]);
    work.plausible = confCount > 0 && confSum >= uint64_t(kGlyphMinFontConf) * confCount && wordsRead * 4 >= 3 * drawn &&
                     letters >= kScriptMinLetters && dominant * 10 >= 9 * letters;
    stats.glyphsSent += drawn;
    stats.glyphsRead += wordsRead;
    return OcrStatus::Ok;
}

// The new texts of `work` written into the glyphs of its uses on `page` (only weak ones unless
// the font is whole); `saved` gets each changed glyph as it was. The unreliable glyphs given text.
uint32_t Apply(const FontWork& work, uint32_t page, std::vector<PageData>& pages,
               std::vector<std::pair<uint32_t, Glyph>>* saved, uint64_t& replaced) {
    PageData& data = pages[page];
    uint32_t unreliable = 0;
    if (work.words) {
        // A word's first glyph takes its text and spans the word; its other glyphs give none.
        for (size_t i = 0; i < work.uses.size(); ++i) {
            const Use& use = work.uses[i];
            if (use.page != page || work.useWord[i] == UINT32_MAX || !work.useHead[i]) continue;
            const std::string& text = work.wordTexts[work.useWord[i]];
            if (text.empty()) continue;
            if (data.text.size() + text.size() >= (size_t(1) << 30)) break;
            size_t end = i + 1;
            while (end < work.uses.size() && work.useWord[end] == work.useWord[i] && !work.useHead[end] &&
                   work.uses[end].page == page)
                ++end;
            bool live = true;
            for (size_t k = i; k < end; ++k) {
                const Glyph& g = data.glyphs[work.uses[k].glyph];
                live = live && !(g.flags & kGlyphDropped) && g.text == work.uses[k].text;
            }
            if (!live) continue;  // covered by ActualText or dropped
            Glyph& head = data.glyphs[use.glyph];
            for (size_t k = i; k < end; ++k) {
                Glyph& g = data.glyphs[work.uses[k].glyph];
                if (saved) saved->push_back({work.uses[k].glyph, g});
                if (g.flags & (kGlyphUnmapped | kGlyphGuessed)) unreliable++;
                g.flags = static_cast<uint16_t>(g.flags & ~(kGlyphUnmapped | kGlyphGuessed | kGlyphSpace));
                if (k == i) continue;
                head.x0 = std::min(head.x0, g.x0), head.y0 = std::min(head.y0, g.y0);
                head.x1 = std::max(head.x1, g.x1), head.y1 = std::max(head.y1, g.y1);
                head.u0 = std::min(head.u0, g.u0), head.u1 = std::max(head.u1, g.u1);
                head.vb = std::min(head.vb, g.vb), head.vt = std::max(head.vt, g.vt);
                g.textLength = 0;
                replaced++;
            }
            head.text = static_cast<uint32_t>(data.text.size());
            head.textLength = static_cast<uint16_t>(text.size());
            data.text += text;
            replaced++;
        }
        return unreliable;
    }
    for (const Use& use : work.uses) {
        if (use.page != page || (!work.whole && !use.weak)) continue;
        const std::string& text = work.texts[use.slot];
        if (text.empty() || use.glyph >= data.glyphs.size()) continue;
        Glyph& g = data.glyphs[use.glyph];
        if ((g.flags & kGlyphDropped) || g.text != use.text) continue;  // covered by ActualText or dropped
        if (data.text.size() + text.size() >= (size_t(1) << 30)) break;
        if (saved) saved->push_back({use.glyph, g});
        if (g.flags & (kGlyphUnmapped | kGlyphGuessed)) unreliable++;
        g.text = static_cast<uint32_t>(data.text.size());
        g.textLength = static_cast<uint16_t>(text.size());
        g.flags = static_cast<uint16_t>(g.flags & ~(kGlyphUnmapped | kGlyphGuessed | kGlyphSpace));
        if (text == " ") g.flags |= kGlyphSpace;
        data.text += text;
        replaced++;
    }
    return unreliable;
}

bool Alphanumeric(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || ScriptOf(cp) >= 0;
}

// The first (`last` false) or last code point of a glyph's text, 0 when it has none.
uint32_t EndOf(std::string_view text, bool last) {
    size_t at = 0, length = 0;
    uint32_t cp = 0, result = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        at += length;
        result = cp;
        if (!last) break;
    }
    return result;
}

// Letters and digits read from a glyph alone mean little when the font is a symbol font or only
// some of its glyphs are examined (bullets, check marks, arrows and circled numbers look like
// "V", "3", "4", "o"): such a glyph keeps its text only where some use of it forms a word with the
// text around it. A neighbour counts when it is the glyph before or after in content order, in the
// same direction, on its baseline, at most kWordGapEm of its size away, and its touching character
// is a letter or digit (a neighbour of the same font read with its recovered text). In a font
// examined only for some codes the glyph must sit inside a word (neighbours on both sides: a list
// marker set against its text has one); in a whole symbol font one side is enough (a word's
// first or last letter).
void DropIsolatedLetters(const std::vector<PageData>& pages, FontWork& w) {
    if (w.whole && !w.font->Style().symbolic) return;
    std::vector<uint8_t> state(w.texts.size(), 0);  // 1 alphanumeric, 2 in a word
    bool any = false;
    for (size_t s = 0; s < w.texts.size(); ++s) {
        const std::string& text = w.texts[s];
        size_t at = 0, length = 0;
        uint32_t cp = 0;
        while (Utf8::Decode(text, at, cp, length)) {
            at += length;
            if (Alphanumeric(cp)) state[s] = 1, any = true;
        }
    }
    if (!any) return;
    std::unordered_map<uint64_t, uint32_t> slotAt;  // (page, glyph) -> slot
    for (const Use& use : w.uses) slotAt[uint64_t(use.page) << 32 | use.glyph] = use.slot;
    const auto textOf = [&](uint32_t page, uint32_t index) -> std::string_view {
        const auto it = slotAt.find(uint64_t(page) << 32 | index);
        if (it != slotAt.end() && !w.texts[it->second].empty()) return w.texts[it->second];
        return pages[page].GlyphText(pages[page].glyphs[index]);
    };
    for (const Use& use : w.uses) {
        if (state[use.slot] != 1) continue;
        const PageData& page = pages[use.page];
        if (use.glyph >= page.glyphs.size()) continue;
        const Glyph& g = page.glyphs[use.glyph];
        const float slack = kWordGapEm * g.size;
        int sides = 0;
        for (const int side : {-1, 1}) {
            if ((side < 0 && use.glyph == 0) || (side > 0 && use.glyph + 1 >= page.glyphs.size())) continue;
            const uint32_t at = use.glyph + side;
            const Glyph& n = page.glyphs[at];
            if ((n.flags & (kGlyphSpace | kGlyphDropped)) || n.dir != g.dir || std::fabs(n.v - g.v) > slack) continue;
            const float gap = side < 0 ? g.u0 - n.u1 : n.u0 - g.u1;
            if (gap > slack || gap < -g.size) continue;
            if (Alphanumeric(EndOf(textOf(use.page, at), side < 0))) ++sides;
        }
        if (sides >= (w.whole ? 1 : 2)) state[use.slot] = 2;
    }
    for (size_t s = 0; s < w.texts.size(); ++s)
        if (state[s] == 1) w.texts[s].clear();
}

uint32_t Popcount(uint8_t v) {
    uint32_t n = 0;
    for (; v; v &= static_cast<uint8_t>(v - 1)) ++n;
    return n;
}

} // namespace

OcrStatus RecoverGlyphShapes(const Document& document, Text::FontCache& fonts, OcrSession& session,
                             const std::vector<uint32_t>& candidates, std::vector<PageData>& pages,
                             std::vector<PageTextQuality>& quality, GlyphRecoveryStats& stats) {
    if (candidates.empty()) return OcrStatus::Ok;
    // The candidate pages interpreted again, with their glyphs' fonts and codes.
    std::vector<std::vector<GlyphUse>> uses(pages.size());
    std::vector<uint8_t> isCandidate(pages.size(), 0);
    PageData scratch;
    const auto trace = [&](uint32_t p) {
        PageStats pageStats;
        pageStats.uses = &uses[p];
        InterpretPage(document, fonts, pages[p].number - 1, false, scratch, pageStats);
        if (scratch.glyphs.size() != pages[p].glyphs.size()) uses[p].clear();  // not the same page: left alone
    };
    std::unordered_map<const Text::Font*, FontStats> fontStats;
    std::vector<const Text::Font*> order;  // fonts in order of first use
    size_t held = 0;
    for (uint32_t p : candidates) {
        if (p >= pages.size() || isCandidate[p]) continue;
        isCandidate[p] = 1;
        trace(p);
        held += uses[p].size();
        CountFonts(pages[p], uses[p], fontStats);
        for (const GlyphUse& use : uses[p])
            if (std::find(order.begin(), order.end(), use.font) == order.end() && order.size() < 4 * kMaxRecoveryFonts)
                order.push_back(use.font);
    }

    // The fonts to examine: those with weak glyphs or judged wrong as a whole, and drawable.
    std::vector<std::unique_ptr<FontWork>> work;
    std::unordered_map<const Text::Font*, FontWork*> workOf;
    for (const Text::Font* font : order) {
        if (work.size() >= kMaxRecoveryFonts) break;
        const FontStats& s = fontStats[font];
        const bool whole = WholeFont(s);
        if (!whole && s.unreliable == 0 && s.junkGlyphs == 0) continue;
        std::unique_ptr<FontWork> w(new FontWork());
        w->font = font;
        w->whole = whole;
        w->stats = s;
        w->glyphs.reset(new Text::FontGlyphs());
        if (w->glyphs->Load(document, *font) == Text::FontGlyphs::Kind::None) continue;
        workOf[font] = w.get();
        work.push_back(std::move(w));
    }
    if (work.empty()) return OcrStatus::Ok;

    // Every page's uses of those fonts: the other pages with glyphs interpreted too (while the
    // uses held stay bounded).
    for (uint32_t p = 0; p < pages.size(); ++p) {
        if (!isCandidate[p] && !pages[p].glyphs.empty() && held < kMaxGlyphUses) {
            trace(p);
            held += uses[p].size();
        }
        for (const GlyphUse& use : uses[p]) {
            const auto it = workOf.find(use.font);
            if (it == workOf.end() || use.glyph >= pages[p].glyphs.size()) continue;
            FontWork& w = *it->second;
            const Glyph& g = pages[p].glyphs[use.glyph];
            if ((g.flags & (kGlyphDropped | kGlyphSpace)) || g.text != use.text) continue;
            const bool weak = Weak(pages[p], g);
            if (!w.whole && !weak) continue;
            uint32_t id = 0;
            if (!w.glyphs->GlyphOf(use.code, use.bytes, id)) continue;
            auto slot = w.slotOf.find(id);
            if (slot == w.slotOf.end()) {
                if (w.glyphIds.size() >= kMaxGlyphsPerFont) continue;
                slot = w.slotOf.emplace(id, static_cast<uint32_t>(w.glyphIds.size())).first;
                w.glyphIds.push_back(id);
            }
            w.uses.push_back({p, use.glyph, use.text, slot->second, weak});
            if (isCandidate[p] && (w.candidatePages.empty() || w.candidatePages.back() != p))
                w.candidatePages.push_back(p);
        }
        if (!isCandidate[p]) {
            held -= uses[p].size();
            std::vector<GlyphUse>().swap(uses[p]);
        }
    }

    for (const std::unique_ptr<FontWork>& wp : work) {
        FontWork& w = *wp;
        if (w.glyphIds.empty() || w.candidatePages.empty()) continue;
        stats.fontsExamined++;
        OcrStatus status = RecogniseFont(document, session, w, stats);
        if (status != OcrStatus::Ok) return status;
        // A whole font read as a complex script (Indic, Arabic, ...): its words, not its glyphs.
        if (w.whole && w.script == kComplexScript && w.letters >= kScriptMinLetters) {
            status = RecogniseWords(document, session, pages, w, stats);
            if (status != OcrStatus::Ok) return status;
        }
        if (!w.plausible) continue;
        if (!w.words) DropIsolatedLetters(pages, w);

        // Acceptance on the candidate pages: fewer failed checks, or as many with less junk or
        // fewer unreliable codes and no new check.
        uint32_t checksBefore = 0, checksAfter = 0, junkBefore = 0, junkAfter = 0;
        uint64_t unreliableBefore = 0, unreliableAfter = 0;
        uint8_t firedBefore = 0, firedAfter = 0;
        std::vector<std::pair<uint32_t, Glyph>> saved;
        std::vector<size_t> textSizes, savedStart;
        std::vector<uint32_t> fixed;
        uint64_t replaced = 0;
        for (uint32_t p : w.candidatePages) {
            const PageTextQuality q = p < quality.size() ? quality[p] : PageTextQuality();
            PageSignals before;
            MeasurePage(pages[p], q.glyphs, q.unreliable, before);
            textSizes.push_back(pages[p].text.size());
            savedStart.push_back(saved.size());
            const uint32_t gained = Apply(w, p, pages, &saved, replaced);
            fixed.push_back(gained);
            PageSignals after;
            MeasurePage(pages[p], q.glyphs, q.unreliable - std::min<uint64_t>(q.unreliable, gained), after);
            checksBefore += Popcount(before.checks), checksAfter += Popcount(after.checks);
            firedBefore |= before.checks, firedAfter |= after.checks;
            junkBefore += before.junk, junkAfter += after.junk;
            unreliableBefore += q.unreliable, unreliableAfter += q.unreliable - std::min<uint64_t>(q.unreliable, gained);
        }
        const bool better = checksAfter < checksBefore ||
                            (checksAfter == checksBefore && (firedAfter & ~firedBefore) == 0 &&
                             (junkAfter < junkBefore || unreliableAfter < unreliableBefore));
        if (!better) {
            // Back as it was, last page first.
            for (size_t i = w.candidatePages.size(); i > 0; --i) {
                const uint32_t p = w.candidatePages[i - 1];
                for (size_t k = saved.size(); k > savedStart[i - 1]; --k) pages[p].glyphs[saved[k - 1].first] = saved[k - 1].second;
                saved.resize(savedStart[i - 1]);
                pages[p].text.resize(textSizes[i - 1]);
            }
            continue;
        }
        stats.fontsAccepted++;
        stats.usesReplaced += replaced;
        for (size_t i = 0; i < w.candidatePages.size(); ++i) {
            const uint32_t p = w.candidatePages[i];
            if (p < quality.size()) quality[p].unreliable -= std::min<uint64_t>(quality[p].unreliable, fixed[i]);
        }
        // The font's other pages.
        std::vector<uint32_t> others;
        for (const Use& use : w.uses)
            if (!isCandidate[use.page] && (others.empty() || others.back() != use.page)) others.push_back(use.page);
        for (uint32_t p : others) {
            uint64_t count = 0;
            const uint32_t gained = Apply(w, p, pages, nullptr, count);
            stats.usesReplaced += count;
            if (p < quality.size()) quality[p].unreliable -= std::min<uint64_t>(quality[p].unreliable, gained);
        }
    }
    return OcrStatus::Ok;
}

OcrStatus RecogniseVectorPage(const Document& document, OcrSession& session, PageData& page, uint32_t& added) {
    added = 0;
    const int index = static_cast<int>(page.number) - 1;
    std::string content;
    const StreamStatus read = document.PageContent(index, content);
    if (read == StreamStatus::NotStream || content.empty()) return OcrStatus::Ok;
    OcrCanvas canvas;
    if (!StartCanvas(0, 0, page.width, page.height, kVectorPageScale, canvas)) return OcrStatus::Ok;
    Raster::Rasterizer raster;
    PaintStats paint;
    PaintContent(document, content, document.Resources(index), PageMatrix(document, index), PaintMode::Page, canvas,
                 raster, paint);
    if (paint.fills == 0) return OcrStatus::Ok;
    OcrProtocol::Request request;
    request.op = OcrProtocol::Op::Page;
    request.width = canvas.width;
    request.height = canvas.height;
    request.dpi = static_cast<uint32_t>(canvas.scale * 72.0f + 0.5f);
    OcrProtocol::Response response;
    const OcrStatus status = session.Recognize(request, canvas.pixels, response);
    if (status == OcrStatus::Ok) added = AddOcrGlyphs(canvas, response, page);
    return status;
}

} // namespace RayoPdf::Layout
