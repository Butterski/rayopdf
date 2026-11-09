// SPDX-License-Identifier: Apache-2.0
#include "ocr/engine.h"

#include "common/utf8.h"

#include <algorithm>
#include <cmath>

namespace RayoPdf::Ocr {
namespace {

constexpr float kDetMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kDetDeviation[3] = {0.229f, 0.224f, 0.225f};
constexpr float kRecMean[3] = {0.5f, 0.5f, 0.5f};
constexpr float kRecDeviation[3] = {0.5f, 0.5f, 0.5f};
// PaddleOCR pads every recognition input to at least this width (with 0, mid-gray once normalised).
constexpr uint32_t kMinRecWidth = 320;
// A crop this much taller than wide holds vertical text.
constexpr float kVerticalRatio = 1.5f;
// Largest crop sampled for one line: across and along its text.
constexpr float kMaxCropShort = 4.0f * kRecHeight;
constexpr float kMaxCropLong = 4.0f * kMaxRecWidth;
// A box is line-like (counts for orientation, can be probed for its script) when its long side is
// at least this many times its short side.
constexpr float kLineLike = 2.0f;

uint32_t RoundTo32(double value) {
    uint32_t v = uint32_t(value / 32 + 0.5) * 32;
    return v < 32 ? 32 : v;
}

int32_t ToPixel(float value) {
    return int32_t(std::floor(value + 0.5f));
}

// The size a rectangle is sampled at: at most kMaxCropShort pixels across and kMaxCropLong along
// (the recogniser reads 48 rows and kMaxRecWidth columns), so a huge box costs no more than a
// normal line.
void CropSize(const RotatedRect& rect, uint32_t& width, uint32_t& height) {
    const float shorter = rect.width < rect.height ? rect.width : rect.height;
    const float longer = rect.width < rect.height ? rect.height : rect.width;
    float factor = 1.0f;
    if (shorter > kMaxCropShort) factor = kMaxCropShort / shorter;
    if (longer * factor > kMaxCropLong) factor = kMaxCropLong / longer;
    const float w = rect.width * factor, h = rect.height * factor;
    width = w < 1 ? 1 : uint32_t(w + 0.5f);
    height = h < 1 ? 1 : uint32_t(h + 0.5f);
}

bool LineLike(const RotatedRect& rect) {
    return rect.width >= kLineLike * rect.height || rect.height >= kLineLike * rect.width;
}

// The baseline direction (OcrProtocol::Line::dir) of text read from a crop turned `quarters`
// clockwise: unturned it runs right (0), turned clockwise it ran up (1), turned half it runs left
// (2), turned counter-clockwise it ran down (3).
uint8_t DirOfQuarters(uint32_t quarters) {
    return uint8_t(quarters & 3);
}

// A coordinate `s` along the x axis (or the y axis when `alongY`) of an image that is a W x H page
// turned `turn` quarters clockwise, on the page: along y for x after a quarter turn, and so on.
float BackAlong(uint32_t turn, bool alongY, float s, float W, float H) {
    switch (turn) {
    case 1: return alongY ? s : H - s;
    case 2: return alongY ? H - s : W - s;
    default: return alongY ? W - s : s;
    }
}

// The share of the letters of `text` (ASCII letters and code points from U+00C0 on, punctuation and
// symbol blocks aside) that lie in [first, last]: 1 when the range is unset or there are none.
float OwnShare(std::string_view text, uint32_t first, uint32_t last) {
    if (last == 0) return 1.0f;
    uint32_t letters = 0, own = 0;
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    while (Utf8::Decode(text, at, cp, length) && length > 0) {
        at += length;
        const bool letter = (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                            (cp >= 0xC0 && cp != 0xD7 && cp != 0xF7 && !(cp >= 0x2000 && cp <= 0x2BFF) &&
                             !(cp >= 0x3000 && cp <= 0x303F) && !(cp >= 0xFF00 && cp <= 0xFF20));
        if (!letter) continue;
        ++letters;
        if (cp >= first && cp <= last) ++own;
    }
    return letters == 0 ? 1.0f : float(own) / float(letters);
}

// Bidi classes of ToLogicalOrder.
enum class Strong : uint8_t { Rtl, Ltr, Number, Neutral };

Strong ClassOf(uint32_t cp) {
    if ((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
        if ((cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9)) return Strong::Number;
        return Strong::Rtl;
    }
    if (cp >= '0' && cp <= '9') return Strong::Number;
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7) ||
        (cp >= 0x370 && cp <= 0x58F)) {
        return Strong::Ltr;
    }
    return Strong::Neutral;
}

uint32_t Mirror(uint32_t cp) {
    switch (cp) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0xAB: return 0xBB;
    case 0xBB: return 0xAB;
    default: return cp;
    }
}

} // namespace

bool HasCjk(std::string_view text) {
    // Every code point at or above U+2E80 takes 3 or 4 UTF-8 bytes, led by 0xE2 0xBA or higher.
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        const uint8_t b0 = uint8_t(text[i]);
        if (b0 < 0xE2 || b0 > 0xEF) continue;
        const uint32_t cp = (uint32_t(b0 & 0x0F) << 12) | (uint32_t(uint8_t(text[i + 1]) & 0x3F) << 6) |
                            (uint32_t(uint8_t(text[i + 2]) & 0x3F));
        if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF)) {
            return true;
        }
        i += 2;
    }
    return false;
}

uint32_t Characters(const CropText& text) {
    uint32_t characters = 0;
    for (char c : text.text) {
        // Lead bytes of non-space code points.
        if ((uint8_t(c) & 0xC0) != 0x80 && c != ' ') ++characters;
    }
    return characters;
}

float Density(const CropText& text, uint32_t width, uint32_t height) {
    const uint32_t characters = Characters(text);
    if (width == 0) return characters ? 1e9f : 0.0f;
    return float(characters) * float(height) / float(width);
}

bool Unread(const CropText& text, uint32_t width, uint32_t height) {
    return text.conf < kUnreadConf || Density(text, width, height) < kUnreadDensity;
}

void ToLogicalOrder(CropText& text) {
    struct Item {
        uint32_t cp;
        float x0, x1;
        Strong strong;
    };
    std::vector<Item> items;
    for (size_t at = 0, k = 0; at < text.text.size() && k < text.x0.size(); ++k) {
        uint32_t cp = 0;
        size_t length = 0;
        if (!Utf8::Decode(text.text, at, cp, length) || length == 0) return;  // AssembleText wrote valid UTF-8
        items.push_back({cp, text.x0[k], text.x1[k], ClassOf(cp)});
        at += length;
    }
    // Resolve: a number takes the left-to-right level 2 in a right-to-left paragraph; neutrals take
    // the direction of their strong neighbours when both agree (numbers count as left-to-right
    // here), else the paragraph's (right to left).
    const size_t n = items.size();
    std::vector<uint8_t> ltr(n, 0);
    for (size_t i = 0; i < n; ++i) ltr[i] = items[i].strong == Strong::Ltr || items[i].strong == Strong::Number;
    for (size_t i = 0; i < n;) {
        if (items[i].strong != Strong::Neutral) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && items[j].strong == Strong::Neutral) ++j;
        const bool before = i > 0 && ltr[i - 1], after = j < n && ltr[j];
        for (size_t k = i; k < j; ++k) ltr[k] = before && after;
        i = j;
    }
    // Visual to logical in a right-to-left paragraph: the whole line reversed, left-to-right runs
    // reversed back.
    std::vector<Item> logical(items.rbegin(), items.rend());
    std::vector<uint8_t> runs(ltr.rbegin(), ltr.rend());
    for (size_t i = 0; i < n;) {
        if (!runs[i]) {
            logical[i].cp = Mirror(logical[i].cp);
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && runs[j]) ++j;
        std::reverse(logical.begin() + std::ptrdiff_t(i), logical.begin() + std::ptrdiff_t(j));
        i = j;
    }
    text.text.clear();
    text.x0.clear();
    text.x1.clear();
    for (const Item& item : logical) {
        Utf8::Append(text.text, item.cp);
        text.x0.push_back(item.x0);
        text.x1.push_back(item.x1);
    }
}

EngineStatus PpOcrEngine::RecognizeCrop(const Recognizer& recognizer, const std::vector<uint8_t>& crop,
                                        uint32_t cropWidth, uint32_t cropHeight, CropText& text) {
    text.text.clear();
    text.x0.clear();
    text.x1.clear();
    text.conf = 0;
    if (cropWidth == 0 || cropHeight == 0) return EngineStatus::Ok;
    double scaled = std::ceil(double(kRecHeight) * cropWidth / cropHeight);
    if (scaled < 1) scaled = 1;
    if (scaled > kMaxRecWidth) scaled = kMaxRecWidth;
    const uint32_t width = uint32_t(scaled);
    Resize(GrayView{crop.data(), cropWidth, cropHeight}, width, kRecHeight, resize_, line_);
    const uint32_t padded = width < kMinRecWidth ? kMinRecWidth : width;
    Normalise(line_.data(), line_.size(), kRecMean, kRecDeviation, prob_);  // prob_ as scratch for the planes
    planes_.assign(size_t(3) * kRecHeight * padded, 0.0f);
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t y = 0; y < kRecHeight; ++y) {
            const float* from = prob_.data() + (size_t(c) * kRecHeight + y) * width;
            float* to = planes_.data() + (size_t(c) * kRecHeight + y) * padded;
            for (uint32_t x = 0; x < width; ++x) to[x] = from[x];
        }
    }
    uint32_t steps = 0, classes = 0;
    stats.recognitions++;
    if (&recognizer != &recognizer_ && &recognizer != &first_) stats.scriptRecognitions++;
    if (recognizer.runtime->Recognize(planes_.data(), padded, probs_, steps, classes) != RuntimeStatus::Ok) {
        return EngineStatus::Failed;
    }
    const Dictionary& dictionary = *recognizer.dictionary;
    if (classes != dictionary.Classes() || steps == 0 || probs_.size() != size_t(steps) * classes) {
        return EngineStatus::Failed;
    }
    DecodeCtc(probs_.data(), steps, classes, decoded_);
    InsertSpaces(probs_.data(), classes, classes - 1, settings_.spaceThreshold, decoded_);
    if (recognizer.spaceGap > 0) {
        InsertInkSpaces(line_.data(), width, kRecHeight, padded, steps, classes - 1, recognizer.spaceGap * kRecHeight,
                        decoded_);
    }
    // The timesteps cover the padded input; one input pixel is cropWidth / width crop pixels.
    AssembleText(decoded_, dictionary, steps, float(padded) * float(cropWidth) / float(width), text);
    if (recognizer.visualOrder) ToLogicalOrder(text);
    return EngineStatus::Ok;
}

EngineStatus PpOcrEngine::DetectPage(const GrayView& page) {
    boxes_.clear();
    const uint32_t longer = page.width > page.height ? page.width : page.height;
    uint32_t side = settings_.detMaxSide;
    if (side == 0 || side > kMaxDetSide) side = kMaxDetSide;
    const double ratio = longer > side ? double(side) / longer : 1.0;
    const uint32_t width = RoundTo32(page.width * ratio), height = RoundTo32(page.height * ratio);
    Resize(page, width, height, resize_, small_);
    Normalise(small_.data(), small_.size(), kDetMean, kDetDeviation, planes_);
    if (recognizer_.runtime->Detect(planes_.data(), width, height, prob_) != RuntimeStatus::Ok ||
        prob_.size() != size_t(width) * height) {
        return EngineStatus::Failed;
    }
    DetectBoxes(prob_.data(), width, height, float(page.width) / width, float(page.height) / height, settings_.detect,
                detect_, boxes_);
    SortIntoLines(boxes_);
    if (boxes_.size() > kMaxPageLines) boxes_.resize(kMaxPageLines);
    return EngineStatus::Ok;
}

void PpOcrEngine::CutRead(const GrayView& page, const Read& read, uint32_t& width, uint32_t& height) {
    SampleRect(page, read.rect, read.cropWidth, read.cropHeight, crop_);
    width = read.cropWidth;
    height = read.cropHeight;
    if (read.quarters & 3) {
        Turn(GrayView{crop_.data(), width, height}, read.quarters, turned_);
        crop_.swap(turned_);
        if (read.quarters & 1) std::swap(width, height);
    }
}

bool PpOcrEngine::UpsideDown(uint32_t width, uint32_t height, float& probability) {
    if (!orient_ || width == 0 || height == 0) return false;
    Resize(GrayView{crop_.data(), width, height}, kClsWidth, kClsHeight, resize_, line_);
    Normalise(line_.data(), line_.size(), kDetMean, kDetDeviation, planes_);
    stats.classifications++;
    if (orient_->Classify(planes_.data(), probs_) != RuntimeStatus::Ok || probs_.size() != 2) return false;
    probability = probs_[1];
    return true;
}

EngineStatus PpOcrEngine::PageTurn(const GrayView& page, uint32_t& quarters) {
    quarters = 0;
    flip_ = -1;
    if (!orient_) return EngineStatus::Ok;
    uint32_t standing = 0, lying = 0;
    for (const TextBox& box : boxes_) {
        if (!LineLike(box.rect)) continue;
        if (box.rect.height >= kVerticalRatio * box.rect.width) {
            ++standing;
        } else {
            ++lying;
        }
    }
    const bool turned = standing >= kTurnMinLines && float(standing) >= kTurnShare * float(standing + lying);
    if (!turned && lying == 0) return EngineStatus::Ok;
    // The longest boxes of the page's orientation.
    order_.clear();
    for (uint32_t i = 0; i < boxes_.size(); ++i) {
        const RotatedRect& r = boxes_[i].rect;
        if (LineLike(r) && (r.height >= kVerticalRatio * r.width) == turned) order_.push_back(i);
    }
    std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const RotatedRect& p = boxes_[a].rect;
        const RotatedRect& q = boxes_[b].rect;
        return std::max(p.width, p.height) > std::max(q.width, q.height);
    });
    if (order_.size() > kOrientSample) order_.resize(kOrientSample);
    Read read;
    float sum = 0;
    uint32_t counted = 0;
    for (uint32_t i : order_) {
        read.rect = boxes_[i].rect;
        CropSize(read.rect, read.cropWidth, read.cropHeight);
        read.quarters = turned ? 3 : 0;
        uint32_t w = 0, h = 0;
        CutRead(page, read, w, h);
        float p = 0;
        if (!UpsideDown(w, h, p)) return EngineStatus::Ok;  // no classifier answer: as it stands
        sum += p;
        ++counted;
    }
    if (counted == 0) return EngineStatus::Ok;
    const bool flipped = sum >= kTurnProb * float(counted);
    if (!turned) flip_ = sum / float(counted);
    // The readings decide where they differ (the classifier knows Latin and CJK lines best): the
    // way whose samples read more, more confidently, wins; the classifier breaks a tie.
    uint64_t one = 0, other = 0;
    if (turned) {
        // Read counter-clockwise the text stands upright when it runs down: the page turns
        // counter-clockwise (three quarters clockwise); upside down so, it runs up: one quarter.
        if (ReadingScore(page, 3, false, one) != EngineStatus::Ok || ReadingScore(page, 1, true, other) != EngineStatus::Ok) {
            return EngineStatus::Failed;
        }
        quarters = other > one || (other == one && flipped) ? 1 : 3;
        return EngineStatus::Ok;
    }
    if (!flipped) return EngineStatus::Ok;
    // Upside down only if the readings agree: the turned ones read more, more confidently.
    if (ReadingScore(page, 0, false, one) != EngineStatus::Ok || ReadingScore(page, 2, true, other) != EngineStatus::Ok) {
        return EngineStatus::Failed;
    }
    if (other > one) quarters = 2;
    return EngineStatus::Ok;
}

EngineStatus PpOcrEngine::ReadingScore(const GrayView& page, uint32_t quarters, bool second, uint64_t& score) {
    score = 0;
    Read read;
    size_t sampled = 0;
    // The second way tries only the script that read the first way, when one did.
    if (!second) scoreScript_ = -1;
    size_t hint = scoreScript_ >= 0 ? size_t(scoreScript_) : 0;
    for (uint32_t i : order_) {
        if (sampled++ == kScoreSample) break;
        read.rect = boxes_[i].rect;
        CropSize(read.rect, read.cropWidth, read.cropHeight);
        read.quarters = quarters;
        uint32_t w = 0, h = 0;
        CutRead(page, read, w, h);
        if (RecognizeCrop(first_, crop_, w, h, text_) != EngineStatus::Ok) return EngineStatus::Failed;
        uint16_t best = Unread(text_, w, h) ? 0 : text_.conf;
        // A line the first pass cannot read may be of a script of the pack: the recogniser that read
        // the last such line first, and no more once one reads it surely.
        size_t count = best == 0 && scripts_ ? scripts_->Count() : 0;
        if (second && scoreScript_ >= 0) count = std::min<size_t>(count, 1);
        for (size_t k = 0; k < count && best < kScriptSure; ++k) {
            const size_t s = (hint + k) % count;
            const Recognizer* recognizer = scripts_->Get(s);
            if (!recognizer) continue;
            if (RecognizeCrop(*recognizer, crop_, w, h, other_) != EngineStatus::Ok) return EngineStatus::Failed;
            if (!Unread(other_, w, h) && other_.conf > best &&
                OwnShare(other_.text, recognizer->lettersFirst, recognizer->lettersLast) >= kScriptOwnShare) {
                best = other_.conf;
                hint = s;
                if (!second) scoreScript_ = int32_t(s);
            }
        }
        score += best;
    }
    return EngineStatus::Ok;
}

bool PpOcrEngine::ReadByScript(uint32_t width, uint32_t height, CropText& text) {
    if (script_ < 0) return false;
    const Recognizer* recognizer = scripts_->Get(size_t(script_));
    if (!recognizer || RecognizeCrop(*recognizer, crop_, width, height, other_) != EngineStatus::Ok) return false;
    if (other_.conf < kScriptConf || Unread(other_, width, height) ||
        OwnShare(other_.text, recognizer->lettersFirst, recognizer->lettersLast) < kScriptOwnShare) {
        return false;
    }
    text.text.swap(other_.text);
    text.x0.swap(other_.x0);
    text.x1.swap(other_.x1);
    text.conf = other_.conf;
    return true;
}

template <typename Cut> EngineStatus PpOcrEngine::Shortcut(Cut image, bool& tried) {
    if (tried || !scripts_ || scripts_->Count() == 0 || reads_.size() < kShortcutLines) return EngineStatus::Ok;
    tried = true;
    uint32_t empty = 0;
    for (const Read& r : reads_) empty += r.unread && r.density < kProbeDensity;
    if (3 * empty < 2 * uint32_t(reads_.size())) return EngineStatus::Ok;
    return RouteScripts(image);
}

EngineStatus PpOcrEngine::ReadBoxes(const GrayView& page) {
    reads_.clear();
    script_ = -1;
    bool tried = false;
    const auto cut = [&](uint32_t i, uint32_t& w, uint32_t& h) { CutRead(page, reads_[i], w, h); };
    // First pass: lines read so far, and how many of them went to the recogniser again.
    uint32_t read = 0, reread = 0;
    bool recognizerOnly = !firstPass_;
    for (const TextBox& box : boxes_) {
        Read r;
        r.rect = box.rect;
        r.bounds = Bounds(r.rect, page.width, page.height);
        if (r.bounds.x1 <= r.bounds.x0 || r.bounds.y1 <= r.bounds.y0) continue;
        CropSize(r.rect, r.cropWidth, r.cropHeight);
        const bool vertical = float(r.cropHeight) >= kVerticalRatio * float(r.cropWidth);
        r.quarters = vertical ? 3 : 0;
        uint32_t w = 0, h = 0;
        CutRead(page, r, w, h);
        // A short box only a little taller than wide (one or two characters, a digit in a narrow
        // table cell) is read turned, as PaddleOCR does, but stays a line of the page's direction:
        // whether its text runs at all is not to be told from it.
        r.lying = vertical && !LineLike(r.rect);
        float p = 0;
        if (vertical && !r.lying && UpsideDown(w, h, p) && p >= kTurnProb) {
            // Upside down when turned counter-clockwise: the text runs up; turned clockwise instead.
            r.quarters = 1;
            Turn(GrayView{crop_.data(), w, h}, 2, turned_);
            crop_.swap(turned_);
        }
        // A page whose first lines are of a script of the pack reads its further lines by that
        // script's recogniser first; a line it does not read goes the usual way.
        if (ReadByScript(w, h, r.text)) {
            r.density = Density(r.text, w, h);
            r.lying = r.lying || (vertical && Characters(r.text) < kMinTurnedCharacters);
            reads_.push_back(std::move(r));
            continue;
        }
        EngineStatus status;
        if (recognizerOnly) {
            status = RecognizeCrop(recognizer_, crop_, w, h, r.text);
        } else {
            status = RecognizeCrop(first_, crop_, w, h, r.text);
            if (status != EngineStatus::Ok) return status;
            ++read;
            if (r.text.conf < settings_.rereadConf || HasCjk(r.text.text)) {
                ++reread;
                status = RecognizeCrop(recognizer_, crop_, w, h, r.text);
            }
            if (read >= settings_.switchAfter && double(reread) > double(settings_.switchShare) * read) {
                recognizerOnly = true;
            }
        }
        if (status != EngineStatus::Ok) return status;
        r.unread = Unread(r.text, w, h);
        r.density = Density(r.text, w, h);
        // A standing box that reads as a few characters (an integral sign, a line number) is no
        // turned line either.
        r.lying = r.lying || (vertical && Characters(r.text) < kMinTurnedCharacters);
        reads_.push_back(std::move(r));
        if (Shortcut(cut, tried) != EngineStatus::Ok) return EngineStatus::Failed;
    }
    return EngineStatus::Ok;
}

template <typename Cut> EngineStatus PpOcrEngine::RouteScripts(Cut image) {
    const size_t count = scripts_ ? scripts_->Count() : 0;
    if (count == 0) return EngineStatus::Ok;
    order_.clear();
    for (uint32_t i = 0; i < reads_.size(); ++i) {
        if (reads_[i].unread) order_.push_back(i);
    }
    if (order_.empty()) return EngineStatus::Ok;
    // The longest lines the tier leaves nearly empty are probed (a degraded Latin line reads at a
    // low confidence, but with its characters); every recogniser of the pack reads them.
    std::vector<uint32_t> probes;
    for (uint32_t i : order_) {
        const Read& r = reads_[i];
        if ((r.cropWidth >= kLineLike * r.cropHeight || r.cropHeight >= kLineLike * r.cropWidth) &&
            r.density < kProbeDensity) {
            probes.push_back(i);
        }
    }
    std::stable_sort(probes.begin(), probes.end(), [&](uint32_t a, uint32_t b) {
        return std::max(reads_[a].cropWidth, reads_[a].cropHeight) > std::max(reads_[b].cropWidth, reads_[b].cropHeight);
    });
    if (probes.size() > kProbeLines) probes.resize(kProbeLines);
    // A reading replaces the line's when it reads the line (not unread itself) and is confident:
    // above the line's reading by kScriptMargin unless that one is unread.
    const auto better = [](const Recognizer& recognizer, const CropText& candidate, const CropText& current,
                           bool currentUnread, uint32_t w, uint32_t h) {
        if (candidate.conf < kScriptConf || Unread(candidate, w, h)) return false;
        if (OwnShare(candidate.text, recognizer.lettersFirst, recognizer.lettersLast) < kScriptOwnShare) return false;
        return currentUnread || candidate.conf >= current.conf + kScriptMargin;
    };
    std::vector<uint8_t> winner(count, 0), probed(reads_.size(), 0);
    for (uint32_t i : probes) {
        Read& r = reads_[i];
        uint32_t w = 0, h = 0;
        image(i, w, h);
        probed[i] = 1;
        // The winners so far first: a line one of them reads is not probed further.
        for (int round = 0; round < 2 && r.unread; ++round) {
            for (size_t s = 0; s < count; ++s) {
                if ((winner[s] != 0) != (round == 0)) continue;
                const Recognizer* recognizer = scripts_->Get(s);
                if (!recognizer) continue;
                if (RecognizeCrop(*recognizer, crop_, w, h, other_) != EngineStatus::Ok) return EngineStatus::Failed;
                if (!better(*recognizer, other_, r.text, r.unread, w, h)) continue;
                winner[s] = 1;
                r.text.text.swap(other_.text);
                r.text.x0.swap(other_.x0);
                r.text.x1.swap(other_.x1);
                r.text.conf = other_.conf;
                r.unread = false;
            }
        }
    }
    if (std::find(winner.begin(), winner.end(), 1) == winner.end()) return EngineStatus::Ok;
    if (script_ < 0) script_ = int32_t(std::find(winner.begin(), winner.end(), 1) - winner.begin());
    for (uint32_t i : order_) {
        if (probed[i]) continue;
        Read& r = reads_[i];
        uint32_t w = 0, h = 0;
        image(i, w, h);
        for (size_t s = 0; s < count; ++s) {
            if (!winner[s]) continue;
            const Recognizer* recognizer = scripts_->Get(s);
            if (!recognizer) continue;
            if (RecognizeCrop(*recognizer, crop_, w, h, other_) != EngineStatus::Ok) return EngineStatus::Failed;
            if (!better(*recognizer, other_, r.text, r.unread, w, h)) continue;
            r.text.text.swap(other_.text);
            r.text.x0.swap(other_.x0);
            r.text.x1.swap(other_.x1);
            r.text.conf = other_.conf;
            r.unread = false;
        }
    }
    return EngineStatus::Ok;
}

EngineStatus PpOcrEngine::Page(const GrayView& page, uint32_t, std::vector<OcrProtocol::Line>& lines) {
    lines.clear();
    if (page.width == 0 || page.height == 0) return EngineStatus::Ok;
    stats.pages++;
    if (DetectPage(page) != EngineStatus::Ok) return EngineStatus::Failed;
    uint32_t turn = 0;
    if (PageTurn(page, turn) != EngineStatus::Ok) return EngineStatus::Failed;
    GrayView view = page;
    uint32_t firstUnread = 0;
    for (int look = 0; look < 2; ++look) {
        if (turn) {
            Turn(page, turn, upright_);
            view = GrayView{upright_.data(), (turn & 1) ? page.height : page.width, (turn & 1) ? page.width : page.height};
            if (DetectPage(view) != EngineStatus::Ok) return EngineStatus::Failed;
        }
        if (ReadBoxes(view) != EngineStatus::Ok) return EngineStatus::Failed;
        if (RouteScripts([&](uint32_t i, uint32_t& w, uint32_t& h) { CutRead(view, reads_[i], w, h); }) !=
            EngineStatus::Ok) {
            return EngineStatus::Failed;
        }
        // A second look at a page left standing whose lines mostly stay unread: upside down text of
        // a script the classifier does not know (Cyrillic, Hangul) reads so. The page is read
        // turned and kept so only when that leaves under half as many lines unread.
        uint32_t lineLike = 0, unread = 0, empty = 0;
        for (const Read& r : reads_) {
            if (!LineLike(r.rect)) continue;
            ++lineLike;
            unread += r.unread;
            empty += r.unread && r.density < kProbeDensity;
        }
        if (look == 1) {
            if (2 * unread >= firstUnread) {
                reads_.swap(firstReads_);
                view = page;
                turn = 0;
            }
            break;
        }
        // Pages the classifier finds plainly upright and whose lines read with characters (Latin
        // scans read badly) are not read again.
        if (turn || !orient_ || lineLike < kTurnMinLines || float(unread) < kSecondLookShare * float(lineLike) ||
            (flip_ < kSecondLookFlip && float(empty) < kSecondLookShare * float(lineLike))) {
            break;
        }
        stats.secondLooks++;
        firstReads_.swap(reads_);
        firstUnread = unread;
        turn = 2;
    }

    if (turn) stats.turnedPages++;
    stats.lastFlip = flip_;

    // Lines in the pixels of `view`, then of the page.
    const float W = float(page.width), H = float(page.height);
    for (const Read& r : reads_) {
        if (r.text.text.empty()) continue;
        const RotatedRect& rect = r.rect;
        OcrProtocol::Line line;
        line.x0 = r.bounds.x0;
        line.y0 = r.bounds.y0;
        line.x1 = r.bounds.x1;
        line.y1 = r.bounds.y1;
        line.text = r.text.text;
        line.conf = r.text.conf;
        line.dir = r.lying ? 0 : DirOfQuarters(r.quarters);
        line.chars.resize(r.text.x0.size());
        // A position t along the read image (0 at its left edge) lies on the page at centre +
        // (t - length / 2) * scale * axis: the crop's width axis (ux, uy), or its height axis
        // (-uy, ux) for a crop turned a quarter, negated when the turn reverses it.
        const bool across = (r.quarters & 1) != 0;
        float ax = across ? -rect.uy : rect.ux, ay = across ? rect.ux : rect.uy;
        if (r.quarters == 1 || r.quarters == 2) ax = -ax, ay = -ay;
        const float length = across ? float(r.cropHeight) : float(r.cropWidth);
        const float scale = (across ? rect.height : rect.width) / length;
        const bool alongY = (line.dir & 1) != 0;
        const float centre = alongY ? rect.cy : rect.cx;
        const float axis = alongY ? ay : ax;
        const int32_t lo = alongY ? line.y0 : line.x0, hi = alongY ? line.y1 : line.x1;
        for (size_t i = 0; i < r.text.x0.size(); ++i) {
            float a = centre + (r.text.x0[i] - length * 0.5f) * scale * axis;
            float b = centre + (r.text.x1[i] - length * 0.5f) * scale * axis;
            if (a > b) std::swap(a, b);
            int32_t x0 = ToPixel(a), x1 = ToPixel(b);
            if (x0 < lo) x0 = lo;
            if (x1 > hi) x1 = hi;
            if (x0 > hi) x0 = hi;
            if (x1 < x0) x1 = x0;
            line.chars[i] = {x0, x1};
        }
        if (turn) {
            // `view` is the page turned `turn` quarters clockwise; back into the page's pixels.
            const auto back = [&](float x, float y, float& px, float& py) {
                switch (turn) {
                case 1: px = y, py = H - x; break;
                case 2: px = W - x, py = H - y; break;
                default: px = W - y, py = x; break;
                }
            };
            float ax0, ay0, ax1, ay1;
            back(float(line.x0), float(line.y0), ax0, ay0);
            back(float(line.x1), float(line.y1), ax1, ay1);
            line.x0 = ToPixel(std::min(ax0, ax1)), line.x1 = ToPixel(std::max(ax0, ax1));
            line.y0 = ToPixel(std::min(ay0, ay1)), line.y1 = ToPixel(std::max(ay0, ay1));
            line.dir = uint8_t((line.dir + turn) & 3);
            const bool nowY = (line.dir & 1) != 0;
            const int32_t l0 = nowY ? line.y0 : line.x0, l1 = nowY ? line.y1 : line.x1;
            for (OcrProtocol::CharSpan& span : line.chars) {
                const float a = BackAlong(turn, alongY, float(span.x0), W, H);
                const float b = BackAlong(turn, alongY, float(span.x1), W, H);
                int32_t x0 = ToPixel(std::min(a, b)), x1 = ToPixel(std::max(a, b));
                x0 = std::min(std::max(x0, l0), l1);
                x1 = std::min(std::max(x1, x0), l1);
                span = {x0, x1};
            }
        }
        lines.push_back(std::move(line));
    }
    return EngineStatus::Ok;
}

EngineStatus PpOcrEngine::Lines(const std::vector<GrayView>& images, std::vector<OcrProtocol::Line>& lines) {
    lines.assign(images.size(), OcrProtocol::Line());
    // The image of read i as the recogniser reads it: turned counter-clockwise when vertical.
    const auto image = [&](uint32_t i, uint32_t& w, uint32_t& h) {
        const GrayView& view = images[i];
        w = view.width;
        h = view.height;
        if (reads_[i].quarters) {
            Turn(view, reads_[i].quarters, crop_);
            std::swap(w, h);
        } else {
            crop_.assign(view.pixels, view.pixels + size_t(view.width) * view.height);
        }
    };
    // Read in order; reads_ grows as they are read, so that the shortcut sees the lines so far.
    reads_.clear();
    script_ = -1;
    bool tried = false;
    for (uint32_t i = 0; i < images.size(); ++i) {
        const GrayView& view = images[i];
        reads_.push_back(Read());
        Read& r = reads_.back();
        r.cropWidth = view.width;
        r.cropHeight = view.height;
        if (view.width == 0 || view.height == 0) continue;
        r.quarters = float(view.height) >= kVerticalRatio * float(view.width) ? 3 : 0;
        uint32_t w = 0, h = 0;
        image(i, w, h);
        if (ReadByScript(w, h, r.text)) {
            r.density = Density(r.text, w, h);
            continue;
        }
        if (RecognizeCrop(recognizer_, crop_, w, h, r.text) != EngineStatus::Ok) return EngineStatus::Failed;
        r.unread = Unread(r.text, w, h);
        r.density = Density(r.text, w, h);
        if (Shortcut(image, tried) != EngineStatus::Ok) return EngineStatus::Failed;
    }
    if (RouteScripts(image) != EngineStatus::Ok) return EngineStatus::Failed;
    for (uint32_t i = 0; i < images.size(); ++i) {
        const Read& r = reads_[i];
        OcrProtocol::Line& line = lines[i];
        line.x1 = int32_t(images[i].width);
        line.y1 = int32_t(images[i].height);
        line.text = r.text.text;
        line.conf = r.text.conf;
        line.chars.resize(r.text.x0.size());
        const bool vertical = r.quarters != 0;
        for (size_t k = 0; k < r.text.x0.size(); ++k) {
            int32_t x0 = vertical ? 0 : ToPixel(r.text.x0[k]);
            int32_t x1 = vertical ? line.x1 : ToPixel(r.text.x1[k]);
            if (x0 < 0) x0 = 0;
            if (x1 > line.x1) x1 = line.x1;
            if (x0 > line.x1) x0 = line.x1;
            if (x1 < x0) x1 = x0;
            line.chars[k] = {x0, x1};
        }
    }
    return EngineStatus::Ok;
}

EngineStatus PpOcrEngine::Line(const GrayView& image, OcrProtocol::Line& line) {
    single_.assign(1, image);
    if (Lines(single_, lines_) != EngineStatus::Ok) return EngineStatus::Failed;
    line = std::move(lines_[0]);
    return EngineStatus::Ok;
}

} // namespace RayoPdf::Ocr
