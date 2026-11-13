// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_route.h"

#include "text/repair.h"

#include <algorithm>
#include <cmath>

namespace RayoPdf::Layout {

namespace {

// The census's thresholds (eval/census/census.py THRESHOLDS, docling's checks).
constexpr float kJunkShare = 0.03f;
constexpr uint32_t kAccentedMinLetters = 50;
constexpr float kAccentedShare = 0.5f;
constexpr uint32_t kVowelMinLetters = 200;
constexpr float kVowelShareMax = 0.25f;
constexpr uint32_t kWordMin = 3, kWordMax = 20;
constexpr uint64_t kUnreliableMinGlyphs = 20;  // pipeline.cpp: the "mostly unmapped" comment

// Text-likeness (LooksLikeText).
constexpr uint32_t kMinWidth = 32, kMinHeight = 16;
constexpr float kMinContrast = 64.0f;
constexpr float kMinPaper = 160.0f;
constexpr float kMinInk = 0.005f, kMaxInk = 0.35f;
constexpr float kMidtoneMargin = 0.3f;
constexpr float kMaxMidtones = 0.15f;
constexpr uint32_t kMinBandHeight = 4;
constexpr float kMinStrokes = 3.0f;          // ink runs a row of a text band crosses, at least
constexpr float kStrokesPerHeight = 0.8f;    // ... and per band height of ink span
constexpr float kMinTextRows = 0.15f;

// Vector-outline text: glyph-sized filled curves.
constexpr float kGlyphShapeMin = 2.0f, kGlyphShapeMax = 40.0f;  // points high

bool Counted(const Glyph& g) {
    return !(g.flags & (kGlyphDropped | kGlyphSpace | kGlyphClipped | kGlyphImage)) && g.size >= 1.0f &&
           g.textLength > 0;
}

bool LatinLetter(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7);
}

// Vowels after removing accents (Latin-1 only), as census.py's NFD base letter in "aeiouy".
bool Vowel(uint32_t cp) {
    if (cp < 0x80) {
        const uint32_t c = cp | 0x20;
        return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
    }
    if (cp >= 0xE0) cp -= 0x20;
    return (cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xC8 && cp <= 0xCF) || (cp >= 0xD2 && cp <= 0xD6) ||
           (cp >= 0xD8 && cp <= 0xDD) || cp == 0xDF;  // 0xFF (ÿ) lands on 0xDF
}

// The next code point of UTF-8 `text` at `at` (advanced); U+FFFD for a bad byte.
uint32_t NextCodePoint(std::string_view text, size_t& at) {
    const unsigned char b = static_cast<unsigned char>(text[at++]);
    if (b < 0x80) return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t cp = b & (0x3F >> extra);
    for (; extra > 0; --extra) {
        if (at >= text.size() || (static_cast<unsigned char>(text[at]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(text[at++]) & 0x3F);
    }
    return cp;
}

bool Space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0 || cp == 0x3000 ||
           (cp >= 0x2000 && cp <= 0x200A);
}

float Area(const ImageBox& image) {
    return std::max(0.0f, image.x1 - image.x0) * std::max(0.0f, image.y1 - image.y0);
}

// What the broken-text checks need of an ASCII byte, looked up once (Space, IsJunk, LatinLetter
// and Vowel above, for code points below 0x80).
enum : uint8_t { kAsciiSpace = 1, kAsciiJunk = 2, kAsciiLetter = 4, kAsciiVowel = 8 };
struct AsciiClasses {
    uint8_t of[128];
    constexpr AsciiClasses() : of() {
        for (int c = 0; c < 128; ++c) {
            uint8_t k = 0;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') k |= kAsciiSpace;
            if ((c < 0x20 && c != '\t' && c != '\n' && c != '\f') || c == 0x7F) k |= kAsciiJunk;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) k |= kAsciiLetter;
            const int lower = c | 0x20;
            if ((k & kAsciiLetter) &&
                (lower == 'a' || lower == 'e' || lower == 'i' || lower == 'o' || lower == 'u' || lower == 'y'))
                k |= kAsciiVowel;
            of[c] = k;
        }
    }
};
constexpr AsciiClasses kAscii;

// MeasurePage's count of Latin letters and their vowels, in one word: letters in the low 32 bits,
// vowels in the high ones (kLetter, plus kVowel for a vowel).
constexpr uint64_t kLetter = 1, kVowel = uint64_t(1) << 32;
struct AsciiLetters {
    uint64_t of[128];
    constexpr AsciiLetters() : of() {
        for (int c = 0; c < 128; ++c)
            of[c] = (kAscii.of[c] & kAsciiLetter) ? kLetter | ((kAscii.of[c] & kAsciiVowel) ? kVowel : 0) : 0;
    }
};
constexpr AsciiLetters kAsciiLetters;

} // namespace

void MeasurePage(const PageData& page, uint64_t glyphs, uint64_t unreliable, PageSignals& signals) {
    signals = PageSignals();
    // One pass over the counted glyphs' text bytes; ASCII through a table, words of Latin letters
    // for the vowel check (a word ends at a space glyph or any other character). Latin letters
    // and their vowels are counted together (kLetter, kVowel), a word's as the count since its
    // start; a character is a letter or one of `others`; a counted glyph is visible unless
    // invisible.
    uint32_t counted = 0, invisible = 0;
    uint32_t others = 0, junk = 0, accented = 0, wordLetters = 0, wordVowels = 0;
    uint64_t letters = 0, wordStart = 0;
    const auto endWord = [&] {
        const uint64_t word = letters - wordStart;
        const uint32_t length = static_cast<uint32_t>(word);
        if (length >= kWordMin && length <= kWordMax) wordLetters += length, wordVowels += static_cast<uint32_t>(word >> 32);
        wordStart = letters;
    };
    const unsigned char* const base = reinterpret_cast<const unsigned char*>(page.text.data());
    const size_t textSize = page.text.size();
    for (const Glyph& g : page.glyphs) {
        const uint16_t flags = g.flags;
        if (flags & kGlyphSpace) {
            endWord();
            continue;
        }
        if ((flags & (kGlyphDropped | kGlyphClipped | kGlyphImage)) || !(g.size >= 1.0f) || g.textLength == 0 ||
            size_t(g.text) + g.textLength > textSize)
            continue;
        counted++;
        invisible += (flags & kGlyphInvisible) ? 1 : 0;
        const unsigned char* p = base + g.text;
        if (g.textLength == 1 && *p < 0x80) {  // most glyphs: one ASCII character
            const uint64_t letter = kAsciiLetters.of[*p];
            if (letter != 0) {
                letters += letter;
                continue;
            }
            const uint8_t k = kAscii.of[*p];
            if (!(k & kAsciiSpace)) others++;
            junk += (k & kAsciiJunk) ? 1 : 0;
            endWord();
            continue;
        }
        const unsigned char* const end = p + g.textLength;
        while (p < end) {
            if (*p < 0x80) {  // most text: one table look-up
                const uint64_t letter = kAsciiLetters.of[*p];
                const uint8_t k = kAscii.of[*p++];
                if (letter != 0) {
                    letters += letter;
                    continue;
                }
                if (!(k & kAsciiSpace)) others++;
                junk += (k & kAsciiJunk) ? 1 : 0;
                endWord();
                continue;
            }
            size_t at = size_t(p - base);
            const uint32_t cp = NextCodePoint(std::string_view(page.text.data(), size_t(end - base)), at);
            p = base + at;
            if (Space(cp)) {
                endWord();
                continue;
            }
            if (Text::Repair::IsJunk(cp)) junk++;
            if (!LatinLetter(cp)) {
                others++;
                endWord();
                continue;
            }
            if (cp >= 0xC0) accented++;
            letters += Vowel(cp) ? kLetter | kVowel : kLetter;
        }
    }
    endWord();
    const uint32_t latin = static_cast<uint32_t>(letters);
    const uint32_t chars = latin + others;
    signals.visibleChars = counted - invisible;
    signals.invisibleChars = invisible;
    signals.chars = chars, signals.junk = junk, signals.latin = latin, signals.accented = accented;
    signals.wordLetters = wordLetters, signals.wordVowels = wordVowels;
    uint8_t checks = 0;
    if (chars > 0 && junk >= kJunkShare * chars) checks |= kCheckJunk;
    if (latin >= kAccentedMinLetters && accented >= kAccentedShare * latin) checks |= kCheckAccented;
    if (wordLetters >= kVowelMinLetters && wordVowels < kVowelShareMax * wordLetters) checks |= kCheckVowels;
    if (glyphs >= kUnreliableMinGlyphs && unreliable * 2 > glyphs) checks |= kCheckUnreliable;
    signals.checks = checks;
    signals.broken = checks != 0;

    const float pageArea = page.width * page.height;
    if (pageArea > 0) {
        float covered = 0;
        for (const ImageBox& image : page.images) covered += Area(image);
        signals.coverage = std::min(1.0f, covered / pageArea);
    }
    if (signals.visibleChars + signals.invisibleChars < kPageMinChars) {
        uint32_t shapes = 0;
        for (const Shape& s : page.shapes) {
            const float h = s.y1 - s.y0, w = s.x1 - s.x0;
            if (s.kind == ShapeKind::Other && s.filled && s.curved && h >= kGlyphShapeMin && h <= kGlyphShapeMax &&
                w <= 4 * h)
                shapes++;
        }
        signals.vectorText = shapes >= kVectorTextMinShapes;
    }
}

void PlanPage(const PageData& page, const PageSignals& signals, OcrMode mode, OcrPlan& plan) {
    plan.route = OcrRoute::None;
    plan.images.clear();
    plan.checkText = false;
    plan.replaceInvisible = false;
    plan.textLayer = false;
    if (mode == OcrMode::Off || page.images.empty()) return;
    const uint32_t chars = signals.visibleChars + signals.invisibleChars;
    plan.textLayer = chars >= kPageMinChars;
    const bool force = mode == OcrMode::Force;
    bool whole = chars < kPageMinChars && signals.coverage >= kPageMinCoverage;
    if (force) {
        // An invisible layer over a page image (an earlier OCR) is read again; so is any page with
        // images and no text.
        whole = chars < kPageMinChars || (signals.visibleChars < kPageMinChars && signals.coverage >= kPageMinCoverage);
        plan.replaceInvisible = whole && signals.invisibleChars > 0;
    }
    if (whole) {
        plan.route = OcrRoute::Page;
        for (uint32_t i = 0; i < page.images.size(); ++i)
            if (Area(page.images[i]) > 0) plan.images.push_back(i);
        if (plan.images.empty()) plan.route = OcrRoute::None;
        return;
    }
    // Regions: large images with no text over them.
    const float pageArea = page.width * page.height;
    for (uint32_t i = 0; i < page.images.size(); ++i) {
        const ImageBox& image = page.images[i];
        if (Area(image) < kRegionMinShare * pageArea) continue;
        if ((image.width > 0 && image.width < kRegionMinPixels) || (image.height > 0 && image.height < kRegionMinPixels))
            continue;
        bool repeated = false;
        for (uint32_t j : plan.images) {
            const ImageBox& o = page.images[j];
            repeated = repeated || (o.identity == image.identity && o.x0 == image.x0 && o.y0 == image.y0 &&
                                    o.x1 == image.x1 && o.y1 == image.y1);
        }
        if (repeated) continue;
        uint32_t covering = 0;
        for (const Glyph& g : page.glyphs) {
            if (!Counted(g)) continue;
            const UprightBox box = GlyphPageBox(g);
            const float cx = 0.5f * (box.x0 + box.x1), cy = 0.5f * (box.y0 + box.y1);
            if (cx >= image.x0 && cx <= image.x1 && cy >= image.y0 && cy <= image.y1 && ++covering > kRegionMaxCoveringChars)
                break;
        }
        if (covering > kRegionMaxCoveringChars) continue;
        plan.images.push_back(i);
    }
    if (!plan.images.empty()) {
        plan.route = OcrRoute::Regions;
        plan.checkText = !force;
    }
}

bool LooksLikeText(const uint8_t* pixels, uint32_t width, uint32_t height, TextEvidence* evidence) {
    TextEvidence local;
    TextEvidence& e = evidence ? *evidence : local;
    e = TextEvidence();
    if (width < kMinWidth || height < kMinHeight) return false;
    const uint64_t n = uint64_t(width) * height;
    uint64_t histogram[256] = {};
    for (uint64_t i = 0; i < n; ++i) histogram[pixels[i]]++;

    // Otsu's threshold: ink is at most `threshold`.
    double total = 0;
    for (int v = 0; v < 256; ++v) total += double(v) * double(histogram[v]);
    double below = 0, belowSum = 0, best = -1;
    int threshold = 0;
    for (int t = 0; t < 255; ++t) {
        below += double(histogram[t]);
        belowSum += double(t) * double(histogram[t]);
        const double above = double(n) - below;
        if (below == 0 || above == 0) continue;
        const double d = belowSum / below - (total - belowSum) / above;
        const double between = below * above * d * d;
        if (between > best) best = between, threshold = t;
    }
    uint64_t inkCount = 0;
    double inkSum = 0;
    for (int v = 0; v <= threshold; ++v) inkCount += histogram[v], inkSum += double(v) * double(histogram[v]);
    if (inkCount == 0 || inkCount == n) return false;
    const double inkMean = inkSum / double(inkCount);
    const double paperMean = (total - inkSum) / double(n - inkCount);
    e.contrast = float(paperMean - inkMean);
    e.ink = float(double(inkCount) / double(n));
    const double lo = inkMean + kMidtoneMargin * e.contrast, hi = paperMean - kMidtoneMargin * e.contrast;
    uint64_t mid = 0;
    for (int v = 0; v < 256; ++v)
        if (v > lo && v < hi) mid += histogram[v];
    e.midtones = float(double(mid) / double(n));
    if (e.contrast < kMinContrast || paperMean < kMinPaper || e.ink < kMinInk || e.ink > kMaxInk ||
        e.midtones > kMaxMidtones)
        return false;

    // Rows: bands of rows holding ink, and how many ink runs cross each.
    const uint32_t inked = std::max<uint32_t>(2, width / 333);
    std::vector<uint32_t> heights;
    uint32_t bandStart = 0, bandRuns = 0, bandMin = 0, bandMax = 0;
    bool inBand = false;
    const auto closeBand = [&](uint32_t end) {
        const uint32_t h = end - bandStart;
        const float span = float(bandMax - bandMin + 1);
        const float strokes = float(bandRuns) / float(h);
        e.bands++;
        if (h >= kMinBandHeight && h <= height / 3 && strokes >= kMinStrokes && strokes >= kStrokesPerHeight * span / float(h))
            heights.push_back(h);
    };
    const uint8_t t = static_cast<uint8_t>(threshold);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = pixels + uint64_t(y) * width;
        uint32_t count = 0, runs = 0, first = width, last = 0;
        bool previous = false;
        for (uint32_t x = 0; x < width; ++x) {
            const bool ink = row[x] <= t;
            if (ink) {
                count++;
                if (!previous) runs++;
                if (first == width) first = x;
                last = x;
            }
            previous = ink;
        }
        if (count >= inked) {
            if (!inBand) {
                inBand = true;
                bandStart = y, bandRuns = 0, bandMin = first, bandMax = last;
            }
            bandRuns += runs;
            bandMin = std::min(bandMin, first), bandMax = std::max(bandMax, last);
        } else if (inBand) {
            inBand = false;
            closeBand(y);
        }
    }
    if (inBand) closeBand(height);
    if (heights.empty()) return false;

    // Lines of one text size: bands within a factor of two of the median height.
    std::vector<uint32_t> sorted = heights;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const uint32_t median = sorted[sorted.size() / 2];
    uint64_t rows = 0;
    for (uint32_t h : heights) {
        if (2 * h >= median && h <= 2 * median) {
            e.textBands++;
            rows += h;
        }
    }
    e.textRows = float(double(rows) / double(height));
    return e.textBands >= kMinTextBands && e.textRows >= kMinTextRows;
}

} // namespace RayoPdf::Layout
