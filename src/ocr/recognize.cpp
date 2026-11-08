// SPDX-License-Identifier: Apache-2.0
#include "ocr/recognize.h"

#include "common/utf8.h"

#include <cstddef>

namespace RayoPdf::Ocr {

bool Dictionary::Load(std::string_view text) {
    bytes_.clear();
    offsets_.clear();
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view entry = text.substr(at, end - at);
        if (!entry.empty() && entry.back() == '\r') entry.remove_suffix(1);
        at = end + 1;
        if (entry.empty() || entry.size() > kMaxEntryBytes) return false;
        for (size_t i = 0; i < entry.size();) {
            uint32_t codePoint;
            size_t length;
            if (!Utf8::Decode(entry, i, codePoint, length)) return false;
            if (codePoint == Utf8::kReplacement && length == 1) return false;  // malformed
            i += length;
        }
        if (offsets_.size() >= kMaxDictionaryEntries) return false;
        offsets_.push_back(uint32_t(bytes_.size()));
        bytes_.append(entry);
    }
    if (offsets_.empty()) return false;
    offsets_.push_back(uint32_t(bytes_.size()));  // the space, the last class
    bytes_.push_back(' ');
    end_ = uint32_t(bytes_.size());
    return true;
}

std::string_view Dictionary::Entry(uint32_t cls) const {
    if (cls == 0 || cls > offsets_.size()) return {};
    const uint32_t begin = offsets_[cls - 1];
    const uint32_t end = cls < offsets_.size() ? offsets_[cls] : end_;
    return std::string_view(bytes_).substr(begin, end - begin);
}

void DecodeCtc(const float* probs, uint32_t steps, uint32_t classes, std::vector<DecodedChar>& out) {
    out.clear();
    if (classes == 0) return;
    uint32_t previous = 0;
    for (uint32_t t = 0; t < steps; ++t) {
        const float* row = probs + size_t(t) * classes;
        uint32_t best = 0;
        float bestProb = row[0];
        for (uint32_t c = 1; c < classes; ++c) {
            if (row[c] > bestProb) {
                bestProb = row[c];
                best = c;
            }
        }
        if (best != 0 && best == previous && !out.empty()) {
            out.back().lastStep = t;
            if (bestProb > out.back().prob) out.back().prob = bestProb;
        } else if (best != 0) {
            DecodedChar decoded;
            decoded.cls = best;
            decoded.firstStep = decoded.lastStep = t;
            decoded.prob = bestProb;
            out.push_back(decoded);
        }
        previous = best;
    }
}

void InsertSpaces(const float* probs, uint32_t classes, uint32_t spaceClass, float threshold,
                  std::vector<DecodedChar>& chars) {
    if (threshold > 1.0f || chars.size() < 2 || spaceClass == 0 || spaceClass >= classes) return;
    // One pass from the end, so that insertions do not move the characters still to be visited.
    for (size_t i = chars.size() - 1; i > 0; --i) {
        const DecodedChar& left = chars[i - 1];
        const DecodedChar& right = chars[i];
        if (left.cls == spaceClass || right.cls == spaceClass || right.firstStep <= left.lastStep + 1) continue;
        double none = 1.0;
        float best = -1.0f;
        uint32_t at = left.lastStep + 1;
        for (uint32_t t = left.lastStep + 1; t < right.firstStep; ++t) {
            const float p = probs[size_t(t) * classes + spaceClass];
            none *= 1.0 - double(p);
            if (p > best) {
                best = p;
                at = t;
            }
        }
        const double some = 1.0 - none;
        if (some < threshold) continue;
        DecodedChar space;
        space.cls = spaceClass;
        space.firstStep = space.lastStep = at;
        space.prob = float(some);
        space.inserted = true;
        chars.insert(chars.begin() + std::ptrdiff_t(i), space);
    }
}

uint16_t ToConf(double value) {
    if (!(value > 0)) return 0;
    if (value >= 1) return OcrProtocol::kConfScale;
    return uint16_t(value * OcrProtocol::kConfScale + 0.5);
}

void AssembleText(const std::vector<DecodedChar>& chars, const Dictionary& dictionary, uint32_t steps,
                  float cropWidth, CropText& out) {
    out.text.clear();
    out.x0.clear();
    out.x1.clear();
    out.conf = 0;
    if (chars.empty() || steps == 0) return;
    const uint32_t space = dictionary.Classes() - 1;
    size_t first = 0, last = chars.size();
    while (first < last && chars[first].cls == space) ++first;
    while (last > first && chars[last - 1].cls == space) --last;
    if (first == last) return;

    // The mean over the decoder's own characters (inserted spaces are a correction, not a reading).
    double sum = 0;
    size_t counted = 0;
    for (const DecodedChar& c : chars) {
        if (c.inserted) continue;
        sum += c.prob;
        ++counted;
    }
    out.conf = counted ? ToConf(sum / double(counted)) : 0;

    const float step = cropWidth / float(steps);
    // Centre of every kept character, in crop pixels.
    auto centre = [&](size_t i) { return (chars[i].firstStep + chars[i].lastStep + 1) * 0.5f * step; };
    for (size_t i = first; i < last; ++i) {
        float lo, hi;
        if (i == first) {
            lo = chars[i].firstStep * step;
        } else {
            lo = 0.5f * (centre(i - 1) + centre(i));
        }
        if (i + 1 == last) {
            hi = (chars[i].lastStep + 1) * step;
        } else {
            hi = 0.5f * (centre(i) + centre(i + 1));
        }
        const std::string_view entry = dictionary.Entry(chars[i].cls);
        // An entry of several code points shares the character's span evenly.
        const size_t points = OcrProtocol::CodePoints(entry);
        out.text.append(entry);
        for (size_t k = 0; k < points; ++k) {
            out.x0.push_back(lo + (hi - lo) * float(k) / float(points));
            out.x1.push_back(lo + (hi - lo) * float(k + 1) / float(points));
        }
    }
}

} // namespace RayoPdf::Ocr
