// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_style.h"

#include "common/minmax.h"
#include "common/utf8.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace RayoPdf::Layout {

namespace {

constexpr float kXHeight = 0.5f;        // x-height over the font size
constexpr float kCapRatio = 1.42f;      // cap height over x-height when the page shows none
constexpr float kBodySnap = 0.08f;      // sizes this close to the body size are the body size
constexpr float kSizeCluster = 0.06f;   // other sizes this close to each other are one size
constexpr float kBoldStroke = 1.3f;     // bold: strokes at least this times as wide as the body's
constexpr float kBoldLine = 0.6f;       // a line with this share of its letters in bold words is bold
constexpr float kBoldBetween = 1.15f;   // a short word between bold words: bold from this times the body's stroke
constexpr float kStemMax = 0.3f;        // runs of ink longer than this, in em, are no stems
constexpr uint32_t kMinSizeLetters = 3; // letters a line needs to be sized on its own
constexpr float kSameRow = 0.3f;        // lines on one row: baselines this close, in em
constexpr size_t kRowCandidates = 32;   // sized lines looked at on either side for a row neighbour
constexpr uint32_t kStyleMinLetters = 3; // a word alone needs this many letters to be bold or italic
constexpr float kSlantMin = -0.15f;     // shears tried, x shift per pixel up
constexpr float kSlantMax = 0.45f;
constexpr int kSlantSteps = 12;
constexpr float kItalicSlant = 0.12f;   // italic: this much more shear than the body words
constexpr float kMonoResidual = 0.12f;  // fixed pitch: character centres this close to a grid, over its pitch
constexpr float kMonoPitchMin = 0.47f;  // a fixed-pitch cell, in em (proportional text averages below)
constexpr float kMonoPitchMax = 0.75f;
constexpr float kMonoOutlier = 0.5f;    // centres this far off the grid, in cells, are recogniser slips
constexpr float kMonoOnGrid = 0.3f;     // a short line on a neighbour's grid: centres this close, in cells
constexpr uint32_t kMonoMinChars = 16;
constexpr float kMonoGridShare = 0.85f; // a longer line on a neighbour's grid: this share of its centres
constexpr float kMonoNeighbour = 2.5f;  // fixed-pitch neighbours: baselines this close, in em
constexpr uint32_t kMonoCandidates = 8; // lines looked at on either side
constexpr int kStrip = 16;               // pixels of a strip in which rule pieces are looked for
constexpr float kRuleCover = 0.85f;      // share of a strip a rule piece covers
constexpr float kRuleThick = 0.3f;       // the thickest rule, in em
constexpr float kRuleLongH = 3.0f;       // the shortest horizontal rule, in em
constexpr float kRuleLongV = 1.3f;       // the shortest vertical rule (a one-line quote bar), in em
constexpr size_t kMaxRules = 4096;

// Where a letter's ink sits between baseline, x-height and cap height.
enum class Zone : uint8_t {
    None,       // anything else (accents, dots, punctuation, other scripts): not measured
    X,          // from the baseline to the x-height: a c e m n o r s u v w x z
    Descender,  // from below the baseline to the x-height: g p q y
    Ascender,   // from the baseline to above the x-height: b d h k l
    Cap,        // from the baseline to the cap height: capitals and digits
};

Zone ZoneOf(uint32_t cp) {
    switch (cp) {
    case 'a': case 'c': case 'e': case 'm': case 'n': case 'o': case 'r': case 's': case 'u': case 'v':
    case 'w': case 'x': case 'z':
        return Zone::X;
    case 'g': case 'p': case 'q': case 'y':
        return Zone::Descender;
    case 'b': case 'd': case 'h': case 'k': case 'l':
        return Zone::Ascender;
    default:
        break;
    }
    if ((cp >= 'A' && cp <= 'Z' && cp != 'J' && cp != 'Q') || (cp >= '0' && cp <= '9')) return Zone::Cap;
    // Greek and Cyrillic lowercase without accents.
    switch (cp) {
    case 0x3B1: case 0x3B5: case 0x3B9: case 0x3BA: case 0x3BD: case 0x3BF: case 0x3C0: case 0x3C3: case 0x3C4:
    case 0x3C5: case 0x3C9:
    case 0x430: case 0x432: case 0x433: case 0x435: case 0x436: case 0x437: case 0x438: case 0x43A: case 0x43B:
    case 0x43C: case 0x43D: case 0x43E: case 0x43F: case 0x441: case 0x442: case 0x445: case 0x447: case 0x448:
    case 0x44A: case 0x44B: case 0x44C: case 0x44D: case 0x44E: case 0x44F:
        return Zone::X;
    case 0x3B3: case 0x3B7: case 0x3BC: case 0x3C1: case 0x3C7: case 0x440: case 0x443:
        return Zone::Descender;
    default:
        break;
    }
    if ((cp >= 0x391 && cp <= 0x3A9) || (cp >= 0x410 && cp <= 0x42F && cp != 0x414 && cp != 0x419 && cp != 0x426 &&
                                         cp != 0x429))
        return Zone::Cap;
    return Zone::None;
}

// Letters standing on upright stems, whose slant is the face's: b d h l m n p q r u and B D E F
// H I L P R T U.
bool IsStem(uint32_t cp) {
    switch (cp) {
    case 'b': case 'd': case 'h': case 'l': case 'm': case 'n': case 'p': case 'q': case 'r': case 'u':
    case 'B': case 'D': case 'E': case 'F': case 'H': case 'I': case 'L': case 'P': case 'R': case 'T': case 'U':
        return true;
    default:
        return false;
    }
}

bool IsSpace(uint32_t cp) {
    return cp == ' ' || cp == 0xA0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200A);
}

float Median(std::vector<float>& v) {
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

struct Weighted {
    float value = 0;
    float weight = 0;
};

// The value at half the total weight.
float WeightedMedian(std::vector<Weighted>& v) {
    std::sort(v.begin(), v.end(), [](const Weighted& a, const Weighted& b) { return a.value < b.value; });
    float total = 0;
    for (const Weighted& w : v) total += w.weight;
    float run = 0;
    for (const Weighted& w : v) {
        run += w.weight;
        if (run >= total / 2) return w.value;
    }
    return v.back().value;
}

// Otsu's threshold over a histogram: gray levels below it are ink. Sets the classes' means.
int Otsu(const uint64_t hist[256], int& paper, int& ink) {
    uint64_t total = 0;
    double sum = 0;
    for (int i = 0; i < 256; ++i) total += hist[i], sum += double(i) * double(hist[i]);
    paper = 255, ink = 0;
    if (total == 0) return 0;
    uint64_t below = 0;
    double sumBelow = 0, best = -1;
    int threshold = 0;
    for (int t = 1; t < 256; ++t) {
        below += hist[t - 1];
        sumBelow += double(t - 1) * double(hist[t - 1]);
        if (below == 0 || below == total) continue;
        const double m0 = sumBelow / double(below), m1 = (sum - sumBelow) / double(total - below);
        const double between = double(below) * double(total - below) * (m1 - m0) * (m1 - m0);
        if (between > best) best = between, threshold = t, ink = int(m0 + 0.5), paper = int(m1 + 0.5);
    }
    return threshold;
}

struct Box {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// A word of a line: code points [first, first + count) of the line, spanning columns x0..x1.
struct Word {
    uint32_t line = 0, first = 0, count = 0;
    int32_t x0 = 0, x1 = 0;
    float stroke = 0;  // stroke width over the line's em, 0 when not measured
    float slant = 0;   // the shear that sets its strokes upright (x shift per pixel up)
    bool slanted = false;  // `slant` was measured
    uint64_t ink = 0, runs = 0;  // ink pixels in stem-wide runs, and those runs
};

// The grid of a line's character centres (`pitch` apart from `origin`) by least squares, fitted
// again without the centres more than kMonoOutlier cells off it when they are at most 15 %; `rms`
// is the root mean square distance of the centres kept. False when no grid fits.
bool FitGrid(const OcrProtocol::Line& line, double& pitch, double& origin, double& rms) {
    const size_t n = line.chars.size();
    const auto centre = [&](size_t k) { return 0.5 * (double(line.chars[k].x0) + double(line.chars[k].x1)); };
    const auto fit = [&](bool trim) {
        double sk = 0, sc = 0, skk = 0, skc = 0, m = 0;
        for (size_t k = 0; k < n; ++k) {
            const double c = centre(k);
            if (trim && std::fabs(c - (origin + pitch * double(k))) > kMonoOutlier * pitch) continue;
            sk += double(k), sc += c, skk += double(k) * double(k), skc += double(k) * c, m += 1;
        }
        const double den = m * skk - sk * sk;
        if (!(den > 0)) return false;
        pitch = (m * skc - sk * sc) / den;
        origin = (sc - pitch * sk) / m;
        return pitch > 0;
    };
    if (n < 2 || !fit(false)) return false;
    size_t off = 0;
    for (size_t k = 0; k < n; ++k) off += std::fabs(centre(k) - (origin + pitch * double(k))) > kMonoOutlier * pitch;
    if (off > 0 && off * 100 <= n * 15 && !fit(true)) return false;
    double sum = 0, kept = 0;
    for (size_t k = 0; k < n; ++k) {
        const double d = centre(k) - (origin + pitch * double(k));
        if (off * 100 <= n * 15 && std::fabs(d) > kMonoOutlier * pitch) continue;
        sum += d * d, kept += 1;
    }
    rms = kept > 0 ? std::sqrt(sum / kept) : 1e30;
    return true;
}

} // namespace

void MeasureOcrStyle(const OcrCanvas& canvas, const OcrProtocol::Response& response, OcrStyle& style) {
    style.lines.assign(response.lines.size(), OcrLineStyle());
    style.flags.clear();
    style.bodyEm = 0;
    style.paper = 255, style.ink = 0;
    const int32_t cw = static_cast<int32_t>(canvas.width), ch = static_cast<int32_t>(canvas.height);
    const uint8_t* px = reinterpret_cast<const uint8_t*>(canvas.pixels.data());
    std::vector<Box> boxes(response.lines.size());
    uint64_t hist[256] = {};
    for (size_t i = 0; i < response.lines.size(); ++i) {
        const OcrProtocol::Line& line = response.lines[i];
        style.lines[i].first = static_cast<uint32_t>(style.flags.size());
        style.flags.resize(style.flags.size() + line.chars.size(), 0);
        Box& b = boxes[i];
        b.x0 = std::min(std::max(line.x0, 0), cw), b.x1 = std::min(std::max(line.x1, b.x0), cw);
        b.y0 = std::min(std::max(line.y0, 0), ch), b.y1 = std::min(std::max(line.y1, b.y0), ch);
        if (line.conf < kOcrMinConf) continue;
        for (int32_t y = b.y0; y < b.y1; ++y)
            for (int32_t x = b.x0; x < b.x1; ++x) hist[px[size_t(y) * canvas.width + size_t(x)]]++;
    }
    const int threshold = Otsu(hist, style.paper, style.ink);
    if (style.paper - style.ink < kOcrMinContrast) return;
    const auto inkAt = [&](int32_t x, int32_t y) { return px[size_t(y) * canvas.width + size_t(x)] < threshold; };

    // Per letter, the ink in its columns inside the line's box: the vertical run of ink rows with
    // the most ink (a neighbouring line's descenders and ascenders reach in with less, and are cut
    // off by blank rows). A letter's height is skew-proof: x-height letters give the x-height,
    // capitals and digits the cap height. The baseline is the median foot of the letters that
    // stand on it.
    std::vector<int32_t> charTop(style.flags.size(), -1), charBottom(style.flags.size(), -1);
    std::vector<uint8_t> charZone(style.flags.size(), 0), charStem(style.flags.size(), 0);
    std::vector<uint32_t> rowInk;
    std::vector<float> bottoms, xSizes, capSizes, ratios;
    std::vector<float> xHeights(response.lines.size(), 0), capHeights(response.lines.size(), 0);
    std::vector<uint32_t> letters(response.lines.size(), 0);
    std::vector<uint8_t> based(response.lines.size(), 0);  // a baseline, but too few letters for a size
    for (size_t i = 0; i < response.lines.size(); ++i) {
        const OcrProtocol::Line& line = response.lines[i];
        const Box& b = boxes[i];
        if (b.x1 - b.x0 < 2 || b.y1 - b.y0 < 4 || line.conf < kOcrMinConf) continue;
        bottoms.clear(), xSizes.clear(), capSizes.clear();
        const uint32_t first = style.lines[i].first;
        size_t at = 0;
        for (size_t k = 0; at < line.text.size() && k < line.chars.size(); ++k) {
            uint32_t cp = 0;
            size_t length = 0;
            if (!Utf8::Decode(line.text, at, cp, length)) break;
            at += length;
            const Zone zone = ZoneOf(cp);
            charStem[style.lines[i].first + k] = IsStem(cp) ? 1 : 0;
            if (!IsSpace(cp)) letters[i]++;
            if (zone == Zone::None) continue;
            int32_t sx0 = std::min(std::max(line.chars[k].x0, b.x0), b.x1);
            int32_t sx1 = std::min(std::max(line.chars[k].x1, sx0), b.x1);
            if (sx1 <= sx0) continue;
            rowInk.assign(size_t(b.y1 - b.y0), 0);
            for (int32_t y = b.y0; y < b.y1; ++y)
                for (int32_t x = sx0; x < sx1; ++x) rowInk[size_t(y - b.y0)] += inkAt(x, y) ? 1u : 0u;
            int32_t t = -1, u = -1, runTop = -1, runBottom = -1, blank = 0;
            uint32_t best = 0, sum = 0;
            for (int32_t y = b.y0; y <= b.y1; ++y) {
                const uint32_t n = y < b.y1 ? rowInk[size_t(y - b.y0)] : 0;
                if (n > 0) {
                    if (runTop < 0) runTop = y, sum = 0;
                    runBottom = y, sum += n, blank = 0;
                    continue;
                }
                // A run ends at two blank rows (one may be a thin stroke broken by noise).
                if (runTop >= 0 && (++blank >= 2 || y == b.y1)) {
                    if (sum > best) best = sum, t = runTop, u = runBottom;
                    runTop = -1;
                }
            }
            if (t < 0) continue;
            charTop[first + k] = t, charBottom[first + k] = u, charZone[first + k] = static_cast<uint8_t>(zone);
            if (zone != Zone::Descender) bottoms.push_back(float(u));
            if (zone == Zone::X) xSizes.push_back(float(u - t + 1));
            if (zone == Zone::Cap) capSizes.push_back(float(u - t + 1));
        }
        if (bottoms.empty()) continue;
        OcrLineStyle& s = style.lines[i];
        s.baseline = Median(bottoms) + 1;  // under the bottom row of ink
        based[i] = 1;
        if (bottoms.size() < kMinSizeLetters) continue;
        if (xSizes.size() >= 2) {
            const float h = Median(xSizes);
            if (h >= 3) xHeights[i] = h;
        }
        if (capSizes.size() >= 2) {
            const float h = Median(capSizes);
            if (h >= 4) capHeights[i] = h;
        }
        if (xHeights[i] > 0 && capHeights[i] > 0) ratios.push_back(capHeights[i] / xHeights[i]);
        if (xHeights[i] <= 0 && capHeights[i] <= 0) continue;
        s.measured = true;
        based[i] = 0;
    }
    // Sizes: from the x-height, or from the cap height through the page's own ratio between them.
    float ratio = kCapRatio;
    if (ratios.size() >= 3) ratio = std::min(std::max(Median(ratios), 1.2f), 1.7f);
    std::vector<Weighted> sizes;
    for (size_t i = 0; i < response.lines.size(); ++i) {
        OcrLineStyle& s = style.lines[i];
        if (!s.measured) continue;
        s.em = (xHeights[i] > 0 ? xHeights[i] : capHeights[i] / ratio) / kXHeight;
        sizes.push_back({s.em, float(std::max<uint32_t>(letters[i], 1))});
    }
    if (sizes.empty()) return;
    // The body size and the sizes near it are one; other sizes close to each other cluster.
    const float body = WeightedMedian(sizes);
    style.bodyEm = body;
    sizes.clear();
    for (size_t i = 0; i < response.lines.size(); ++i) {
        OcrLineStyle& s = style.lines[i];
        if (!s.measured) continue;
        if (std::fabs(s.em - body) <= kBodySnap * body)
            s.em = body;
        else
            sizes.push_back({s.em, float(i)});
    }
    std::sort(sizes.begin(), sizes.end(), [](const Weighted& a, const Weighted& b) { return a.value < b.value; });
    std::vector<Weighted> group;
    for (size_t g = 0; g < sizes.size();) {
        size_t e = g + 1;
        while (e < sizes.size() && sizes[e].value <= sizes[e - 1].value * (1 + kSizeCluster)) ++e;
        group.clear();
        for (size_t k = g; k < e; ++k)
            group.push_back({sizes[k].value, float(std::max<uint32_t>(letters[size_t(sizes[k].weight)], 1))});
        const float value = WeightedMedian(group);
        for (size_t k = g; k < e; ++k) style.lines[size_t(sizes[k].weight)].em = value;
        g = e;
    }
    // Lines with too few letters to size (a number, a short word) stand on their measured baseline
    // in the size of the nearest sized line on that baseline (the other cells of a table row,
    // kRowCandidates looked at on either side), or else in the body size.
    std::vector<uint32_t> sized;
    for (uint32_t i = 0; i < response.lines.size(); ++i)
        if (style.lines[i].measured) sized.push_back(i);
    std::sort(sized.begin(), sized.end(), [&](uint32_t a, uint32_t b) {
        return style.lines[a].baseline != style.lines[b].baseline ? style.lines[a].baseline < style.lines[b].baseline : a < b;
    });
    for (size_t i = 0; i < response.lines.size(); ++i) {
        if (!based[i]) continue;
        OcrLineStyle& s = style.lines[i];
        float em = body, nearest = INFINITY;
        const float centre = 0.5f * float(response.lines[i].x0 + response.lines[i].x1);
        const auto from = std::lower_bound(sized.begin(), sized.end(), s.baseline,
                                           [&](uint32_t a, float v) { return style.lines[a].baseline < v; });
        const size_t at = static_cast<size_t>(from - sized.begin());
        for (size_t k = at > kRowCandidates ? at - kRowCandidates : 0; k < std::min(sized.size(), at + kRowCandidates); ++k) {
            const OcrLineStyle& t = style.lines[sized[k]];
            if (std::fabs(t.baseline - s.baseline) > kSameRow * t.em) continue;
            const float d = std::fabs(0.5f * float(response.lines[sized[k]].x0 + response.lines[sized[k]].x1) - centre);
            if (d < nearest) nearest = d, em = t.em;
        }
        s.measured = true;
        s.em = em;
    }

    // Stroke widths per word: the mean run of ink across the rows of its x-height letters (or its
    // capitals and digits), between their median top and foot, over the font size; runs longer
    // than a stem (a bar, a rule striking the word through) left out. Bold is a stroke well above
    // the body text's. Text larger than the body (headings) is bold or not per size, all its lines
    // together: one heading level has one style. Body text is bold per word; a line mostly bold
    // is bold throughout, and a short word between bold words that measures a little heavier than
    // the body goes with them.
    std::vector<Word> words;
    std::vector<Weighted> strokes;
    std::vector<float> tops, feet;
    std::vector<uint32_t> columns;
    for (size_t i = 0; i < response.lines.size(); ++i) {
        const OcrProtocol::Line& line = response.lines[i];
        const OcrLineStyle& s = style.lines[i];
        if (!s.measured) continue;
        const Box& b = boxes[i];
        const uint32_t first = s.first;
        const uint32_t longest = static_cast<uint32_t>(kStemMax * s.em);
        size_t at = 0;
        Word word;
        word.line = static_cast<uint32_t>(i);
        bool open = false;
        const auto close = [&]() {
            if (!open) return;
            open = false;
            for (const Zone want : {Zone::X, Zone::Cap}) {
                tops.clear(), feet.clear();
                for (uint32_t k = word.first; k < word.first + word.count; ++k)
                    if (charZone[first + k] == static_cast<uint8_t>(want))
                        tops.push_back(float(charTop[first + k])), feet.push_back(float(charBottom[first + k]));
                if (!tops.empty()) break;
            }
            if (!tops.empty()) {
                const float top = Median(tops), foot = Median(feet);
                const int32_t inset = std::max<int32_t>(1, static_cast<int32_t>((foot - top + 1) / 8));
                const int32_t x0 = std::max(word.x0, b.x0), x1 = std::min(word.x1, b.x1);
                const int32_t y0 = std::max(static_cast<int32_t>(top) + inset, 0);
                const int32_t y1 = std::min(static_cast<int32_t>(foot) - inset, ch - 1);
                for (int32_t y = y0; y <= y1; ++y) {
                    uint32_t run = 0;
                    for (int32_t x = x0; x <= x1; ++x) {
                        if (x < x1 && inkAt(x, y)) {
                            run++;
                            continue;
                        }
                        if (run > 0 && run <= longest) word.ink += run, word.runs++;
                        run = 0;
                    }
                }
                // Slant: the shear that stands the word's strokes upright, where its ink piles
                // into the fewest columns (the sum of squared column counts is largest).
                const int32_t h = static_cast<int32_t>(foot - top) + 1;
                // Only letters built on upright stems count: diagonals (v, w, y) and bowls lean
                // whatever the face. Digits are left out too (tables set them in italics rarely).
                uint32_t stems = 0;
                for (uint32_t k = word.first; k < word.first + word.count; ++k) stems += charStem[first + k];
                if (h >= 6 && stems >= 2 && x1 > x0) {
                    const int32_t reach = static_cast<int32_t>(std::ceil(kSlantMax * float(h))) + 1;
                    double best = -1;
                    for (int step = 0; step <= kSlantSteps; ++step) {
                        const float shear = kSlantMin + (kSlantMax - kSlantMin) * float(step) / float(kSlantSteps);
                        columns.assign(size_t(x1 - x0 + 2 * reach + 1), 0);
                        for (uint32_t k = word.first; k < word.first + word.count; ++k) {
                            if (!charStem[first + k]) continue;
                            const int32_t cx0 = std::max(line.chars[k].x0, x0), cx1 = std::min(line.chars[k].x1, x1);
                            for (int32_t y = std::max(static_cast<int32_t>(top), 0); y <= std::min(static_cast<int32_t>(foot), ch - 1); ++y) {
                                const int32_t shift = static_cast<int32_t>(std::lround(shear * (foot - float(y))));
                                for (int32_t x = cx0; x < cx1; ++x)
                                    if (inkAt(x, y)) columns[size_t(x - x0 - shift + reach)]++;
                            }
                        }
                        double sharp = 0;
                        for (uint32_t c : columns) sharp += double(c) * double(c);
                        if (sharp > best) best = sharp, word.slant = shear;
                    }
                    word.slanted = best > 0;
                }
            }
            if (word.runs >= 4) {
                word.stroke = float(double(word.ink) / double(word.runs)) / s.em;
                if (word.count >= 2 && s.em == body) strokes.push_back({word.stroke, float(word.count)});
            }
            words.push_back(word);
        };
        for (uint32_t k = 0; at < line.text.size() && k < line.chars.size(); ++k) {
            uint32_t cp = 0;
            size_t length = 0;
            if (!Utf8::Decode(line.text, at, cp, length)) break;
            at += length;
            if (IsSpace(cp)) {
                close();
                continue;
            }
            if (!open) {
                open = true;
                word.first = k, word.count = 0, word.stroke = 0, word.ink = 0, word.runs = 0;
                word.slant = 0, word.slanted = false;
                word.x0 = line.chars[k].x0;
            }
            word.count++;
            word.x1 = line.chars[k].x1;
        }
        close();
    }
    if (!strokes.empty()) {
        const float bold = kBoldStroke * WeightedMedian(strokes);
        const auto mark = [&](const Word& w) {
            uint16_t* flags = style.flags.data() + style.lines[w.line].first;
            for (uint32_t c = w.first; c < w.first + w.count; ++c) flags[c] |= kGlyphBold;
        };
        // Larger sizes, pooled.
        std::vector<uint8_t> done(words.size(), 0);
        for (size_t w = 0; w < words.size(); ++w) {
            const float em = style.lines[words[w].line].em;
            if (done[w] || !(em > body)) continue;
            uint64_t ink = 0, runs = 0;
            for (size_t k = w; k < words.size(); ++k)
                if (style.lines[words[k].line].em == em) ink += words[k].ink, runs += words[k].runs;
            const bool heavy = runs > 0 && float(double(ink) / double(runs)) / em >= bold;
            for (size_t k = w; k < words.size(); ++k) {
                if (style.lines[words[k].line].em != em) continue;
                done[k] = 1;
                if (heavy) mark(words[k]);
            }
        }
        // Body and smaller sizes, per word.
        for (size_t w = 0; w < words.size();) {
            size_t e = w;
            uint32_t heavy = 0, measured = 0;
            for (; e < words.size() && words[e].line == words[w].line; ++e) {
                if (!(words[e].stroke > 0)) continue;
                measured += words[e].count;
                if (words[e].stroke >= bold) heavy += words[e].count;
            }
            if (done[w]) {
                w = e;
                continue;
            }
            const bool line = measured >= 4 && float(heavy) >= kBoldLine * float(measured);
            for (size_t k = w; k < e; ++k) {
                const bool between = k > w && k + 1 < e && words[k].count <= 3 && words[k - 1].stroke >= bold &&
                                     words[k + 1].stroke >= bold && words[k].stroke >= kBoldBetween / kBoldStroke * bold;
                if (line || (words[k].count >= kStyleMinLetters && words[k].stroke >= bold) || between) mark(words[k]);
            }
            w = e;
        }
    }
    // Italic: a word slanted well beyond the page's body words (whatever slant a skewed scan gives
    // them all), with the same line and between-word rules as bold.
    {
        std::vector<Weighted> slants;
        for (const Word& w : words)
            if (w.slanted && w.count >= kStyleMinLetters && style.lines[w.line].em == body)
                slants.push_back({w.slant, float(w.count)});
        if (!slants.empty()) {
            const float upright = WeightedMedian(slants);
            const auto leans = [&](const Word& w, float by) { return w.slanted && w.slant - upright >= by; };
            for (size_t w = 0; w < words.size();) {
                size_t e = w;
                uint32_t leaning = 0, measured = 0;
                for (; e < words.size() && words[e].line == words[w].line; ++e) {
                    if (!words[e].slanted) continue;
                    measured += words[e].count;
                    if (leans(words[e], kItalicSlant)) leaning += words[e].count;
                }
                const bool line = measured >= 4 && float(leaning) >= kBoldLine * float(measured);
                uint16_t* flags = style.flags.data() + style.lines[words[w].line].first;
                for (size_t k = w; k < e; ++k) {
                    const bool between = k > w && k + 1 < e && words[k].count <= 3 && leans(words[k - 1], kItalicSlant) &&
                                         leans(words[k + 1], kItalicSlant) && leans(words[k], kItalicSlant / 2);
                    if (!line && !(words[k].count >= kStyleMinLetters && leans(words[k], kItalicSlant)) && !between)
                        continue;
                    for (uint32_t c = words[k].first; c < words[k].first + words[k].count; ++c) flags[c] |= kGlyphItalic;
                }
                w = e;
            }
        }
    }

    // Fixed pitch: the centres of a line's characters, spaces included, on a regular grid one
    // character cell apart, the cell about half an em wide (proportional text drifts off any grid).
    // A few centres far off the grid are characters the recogniser inserted or split. A line too
    // short to tell (a closing brace) is fixed-pitch when its centres sit on the grid of a
    // fixed-pitch line just above or below it. Lines set together on one pitch (a code block)
    // share the grid of the longest of them, so that their columns line up.
    std::vector<double> pitches(response.lines.size(), 0), origins(response.lines.size(), 0);
    std::vector<uint32_t> grid(response.lines.size(), UINT32_MAX);  // the line whose grid a line is on
    for (size_t i = 0; i < response.lines.size(); ++i) {
        const OcrProtocol::Line& line = response.lines[i];
        const OcrLineStyle& s = style.lines[i];
        const size_t n = line.chars.size();
        if (!s.measured || n < kMonoMinChars) continue;
        double pitch = 0, origin = 0, rms = 0;
        if (!FitGrid(line, pitch, origin, rms)) continue;
        if (!(pitch >= kMonoPitchMin * s.em && pitch <= kMonoPitchMax * s.em) || rms > kMonoResidual * pitch) continue;
        pitches[i] = pitch, origins[i] = origin;
        grid[i] = static_cast<uint32_t>(i);
    }
    // Neighbours: measured lines by baseline, each line looking at most kMonoCandidates lines on
    // either side within kMonoNeighbour em of its baseline, of its size.
    std::vector<uint32_t> byBase;
    for (size_t i = 0; i < response.lines.size(); ++i)
        if (style.lines[i].measured) byBase.push_back(static_cast<uint32_t>(i));
    std::sort(byBase.begin(), byBase.end(), [&](uint32_t a, uint32_t b) {
        return style.lines[a].baseline != style.lines[b].baseline ? style.lines[a].baseline < style.lines[b].baseline : a < b;
    });
    std::vector<uint32_t> rank(response.lines.size(), 0);
    for (uint32_t r = 0; r < byBase.size(); ++r) rank[byBase[r]] = r;
    const auto neighbours = [&](uint32_t i, auto&& visit) {
        const OcrLineStyle& a = style.lines[i];
        const uint32_t r = rank[i];
        for (int side = 0; side < 2; ++side) {
            for (uint32_t step = 1; step <= kMonoCandidates; ++step) {
                if (side == 0 ? step > r : r + step >= byBase.size()) break;
                const uint32_t j = byBase[side == 0 ? r - step : r + step];
                const OcrLineStyle& b = style.lines[j];
                if (std::fabs(a.baseline - b.baseline) > kMonoNeighbour * a.em) break;
                if (std::fabs(a.em - b.em) <= 0.1f * a.em && visit(j)) return;
            }
        }
    };
    for (uint32_t i : byBase) {
        const OcrProtocol::Line& line = response.lines[i];
        if (grid[i] != UINT32_MAX || line.chars.empty()) continue;
        neighbours(i, [&](uint32_t j) {
            if (!(pitches[j] > 0)) return false;
            size_t on = 0;
            for (const OcrProtocol::CharSpan& span : line.chars) {
                const double cell = (0.5 * (double(span.x0) + double(span.x1)) - origins[j]) / pitches[j];
                if (std::fabs(cell - std::round(cell)) <= kMonoOnGrid) on++;
            }
            const size_t n = line.chars.size();
            if (n >= kMonoMinChars ? float(on) >= kMonoGridShare * float(n) : on == n) {
                grid[i] = j;
                return true;
            }
            return false;
        });
    }
    // Blocks: fixed-pitch neighbours of one pitch, joined (union-find); a block takes the grid of
    // its longest fitted line.
    std::vector<uint32_t> parent(response.lines.size());
    for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto root = [&](uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (uint32_t i : byBase) {
        if (grid[i] == UINT32_MAX) continue;
        neighbours(i, [&](uint32_t j) {
            if (grid[j] == UINT32_MAX) return false;
            const double p = pitches[grid[i]], q = pitches[grid[j]];
            if (std::fabs(p - q) > 0.05 * p) return false;
            const uint32_t a = root(i), b = root(j);
            if (a == b) return false;
            // The root keeps the longer grid.
            const bool keepA = response.lines[grid[a]].chars.size() >= response.lines[grid[b]].chars.size();
            parent[keepA ? b : a] = keepA ? a : b;
            return false;
        });
    }
    for (size_t i = 0; i < response.lines.size(); ++i) {
        if (grid[i] == UINT32_MAX) continue;
        const uint32_t g = grid[root(static_cast<uint32_t>(i))];
        style.lines[i].pitch = float(pitches[g]);
        style.lines[i].origin = float(origins[g]);
        uint16_t* flags = style.flags.data() + style.lines[i].first;
        for (size_t k = 0; k < response.lines[i].chars.size(); ++k) flags[k] |= kGlyphMonospace;
    }
}

uint32_t AddOcrRulings(const OcrCanvas& canvas, const OcrStyle& style, PageData& page) {
    if (!(style.bodyEm > 0) || canvas.width == 0 || canvas.height == 0) return 0;
    for (const Ruling& r : page.rulings)
        if (r.x1 > canvas.x0 && r.x0 < canvas.x1 && r.y1 > canvas.y0 && r.y0 < canvas.y1) return 0;
    const int dark = style.paper - std::max(24, (style.paper - style.ink) / 5);
    const uint8_t* px = reinterpret_cast<const uint8_t*>(canvas.pixels.data());
    const int32_t cw = static_cast<int32_t>(canvas.width), ch = static_cast<int32_t>(canvas.height);
    const float em = style.bodyEm;
    const int32_t thick = std::max<int32_t>(3, static_cast<int32_t>(kRuleThick * em));
    const double step = 1.0 / canvas.scale;
    uint32_t added = 0;

    // A piece of a rule inside one strip: rows (or columns) c0..c1 across it.
    struct Piece {
        int32_t strip = 0, c0 = 0, c1 = 0;
    };
    struct Chain {
        int32_t first = 0, last = 0;  // strips
        int32_t c0 = 0, c1 = 0;       // the last piece
        int32_t f0 = 0, f1 = 0;       // the first piece
        float centres = 0, widths = 0;  // sums over its pieces
        uint32_t pieces = 0;
    };
    std::vector<uint32_t> count;
    std::vector<Piece> pieces;
    std::vector<Chain> chains;
    std::vector<uint32_t> open, next;  // chains whose last piece is in the strip before, and in this one
    for (int dir = 0; dir < 2; ++dir) {
        // Horizontal rules (dir 0): `along` runs over columns, `across` over rows; vertical the other way.
        const int32_t along = dir == 0 ? cw : ch, across = dir == 0 ? ch : cw;
        const auto darkAt = [&](int32_t a, int32_t c) {
            const size_t at = dir == 0 ? size_t(c) * canvas.width + size_t(a) : size_t(a) * canvas.width + size_t(c);
            return px[at] <= dark;
        };
        const float shortest = (dir == 0 ? kRuleLongH : kRuleLongV) * em;
        chains.clear();
        open.clear();
        count.assign(size_t(across), 0);
        for (int32_t s = 0; s * kStrip < along; ++s) {
            const int32_t a0 = s * kStrip, a1 = std::min(along, a0 + kStrip);
            if (a1 - a0 < kStrip / 2) break;
            std::fill(count.begin(), count.end(), 0u);
            if (dir == 0) {
                for (int32_t c = 0; c < across; ++c) {
                    const uint8_t* row = px + size_t(c) * canvas.width;
                    uint32_t n = 0;
                    for (int32_t a = a0; a < a1; ++a) n += row[a] <= dark ? 1u : 0u;
                    count[size_t(c)] = n;
                }
            } else {
                for (int32_t a = a0; a < a1; ++a) {
                    const uint8_t* row = px + size_t(a) * canvas.width;
                    for (int32_t c = 0; c < across; ++c) count[size_t(c)] += row[c] <= dark ? 1u : 0u;
                }
            }
            pieces.clear();
            const uint32_t least = static_cast<uint32_t>(a1 - a0) / 4;
            for (int32_t c = 0; c < across;) {
                if (count[size_t(c)] < least || count[size_t(c)] == 0) {
                    ++c;
                    continue;
                }
                int32_t e = c;
                while (e + 1 < across && count[size_t(e + 1)] >= least && count[size_t(e + 1)] > 0) ++e;
                if (e - c + 1 <= thick) {
                    int32_t covered = 0;
                    for (int32_t a = a0; a < a1; ++a)
                        for (int32_t k = c; k <= e; ++k)
                            if (darkAt(a, k)) {
                                covered++;
                                break;
                            }
                    if (covered >= kRuleCover * float(a1 - a0)) pieces.push_back({s, c, e});
                }
                c = e + 1;
            }
            // Pieces continue the chains of the strip before whose last piece they meet.
            next.clear();
            for (const Piece& p : pieces) {
                uint32_t into = UINT32_MAX;
                for (uint32_t k : open) {
                    const Chain& chain = chains[k];
                    if (chain.last == s - 1 && p.c0 <= chain.c1 + 1 && p.c1 >= chain.c0 - 1) {
                        into = k;
                        break;
                    }
                }
                if (into == UINT32_MAX) {
                    if (chains.size() >= kMaxRules) continue;
                    into = static_cast<uint32_t>(chains.size());
                    chains.emplace_back();
                    chains.back().first = s;
                    chains.back().f0 = p.c0, chains.back().f1 = p.c1;
                }
                Chain& chain = chains[into];
                chain.last = s;
                chain.c0 = p.c0, chain.c1 = p.c1;
                chain.centres += 0.5f * float(p.c0 + p.c1 + 1);
                chain.widths += float(p.c1 - p.c0 + 1);
                chain.pieces++;
                next.push_back(into);
            }
            open.swap(next);
        }
        for (const Chain& chain : chains) {
            if (added >= kMaxRules) break;
            // The ends: on from the end pieces, pixel by pixel, over one missing pixel at most.
            const auto reach = [&](int32_t from, int32_t stepBy, int32_t c0, int32_t c1) {
                int32_t end = from, gap = 0;
                for (int32_t a = from + stepBy; a >= 0 && a < along; a += stepBy) {
                    bool on = false;
                    for (int32_t k = std::max(c0 - 1, 0); k <= std::min(c1 + 1, across - 1) && !on; ++k) on = darkAt(a, k);
                    if (on) {
                        end = a, gap = 0;
                    } else if (++gap > 1) {
                        break;
                    }
                }
                return end;
            };
            const int32_t start = reach(chain.first * kStrip, -1, chain.f0, chain.f1);
            const int32_t stop = reach(std::min(along, (chain.last + 1) * kStrip) - 1, 1, chain.c0, chain.c1) + 1;
            if (float(stop - start) < shortest) continue;
            const float centre = chain.centres / float(chain.pieces), width = chain.widths / float(chain.pieces);
            Ruling r;
            if (dir == 0) {
                r.x0 = float(canvas.x0 + start * step), r.x1 = float(canvas.x0 + stop * step);
                r.y0 = float(canvas.y1 - (centre + width / 2) * step), r.y1 = float(canvas.y1 - (centre - width / 2) * step);
            } else {
                r.x0 = float(canvas.x0 + (centre - width / 2) * step), r.x1 = float(canvas.x0 + (centre + width / 2) * step);
                r.y0 = float(canvas.y1 - stop * step), r.y1 = float(canvas.y1 - start * step);
            }
            r.flags = kRulingFill;
            page.rulings.push_back(r);
            Shape shape;
            shape.x0 = r.x0, shape.y0 = r.y0, shape.x1 = r.x1, shape.y1 = r.y1;
            shape.kind = ShapeKind::Rect;
            shape.filled = true;
            shape.segments = 4;
            page.shapes.push_back(shape);
            added++;
        }
    }
    return added;
}

} // namespace RayoPdf::Layout
