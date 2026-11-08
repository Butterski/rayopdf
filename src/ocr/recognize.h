// SPDX-License-Identifier: Apache-2.0
// Text recognition post-processing of the OCR add-on: the recogniser's character list and greedy
// CTC decoding (Graves et al. 2006) of its per-timestep class probabilities, with the timesteps of
// every character kept so that each gets an x span.
//
// Classes: 0 is the CTC blank, 1..N the dictionary's entries in file order, N+1 a space (PaddleOCR's
// use_space_char). Decoding takes the most probable class of each timestep (the lowest index on a
// tie), merges repeats and drops blanks. A line's confidence is the mean of the kept characters'
// probabilities, as PaddleOCR's CTCLabelDecode reports it.
#pragma once

#include "common/ocr_protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Ocr {

// Entries per dictionary at most, and bytes per entry.
constexpr uint32_t kMaxDictionaryEntries = 1u << 20;
constexpr uint32_t kMaxEntryBytes = 64;

class Dictionary {
public:
    // Reads entries one a line ('\n', a '\r' before it dropped; a last line without one counts).
    // False when an entry is empty, too long or not valid UTF-8, or there are none or too many.
    bool Load(std::string_view text);
    // Blank + entries + space.
    uint32_t Classes() const { return uint32_t(offsets_.size()) + 1; }
    // Class 1..Classes()-1; the last one is " ".
    std::string_view Entry(uint32_t cls) const;

private:
    std::string bytes_;
    std::vector<uint32_t> offsets_;  // start in bytes_ of class 1, 2, ..., the space last
    uint32_t end_ = 0;               // end of the last entry
};

struct DecodedChar {
    uint32_t cls = 0;
    uint32_t firstStep = 0, lastStep = 0;  // the timesteps whose maximum it is
    float prob = 0;                        // its highest probability over those timesteps
    bool inserted = false;                 // a space added by InsertSpaces (not in the confidence)
};

// Greedy CTC decoding of `steps` x `classes` probabilities (row-major), into `out` (replaced).
void DecodeCtc(const float* probs, uint32_t steps, uint32_t classes, std::vector<DecodedChar>& out);

// Space recovery. Greedy decoding drops a space whose probability is spread over several blank
// timesteps (PP-OCRv6 does this often in English text, e.g. "andit"). Between two decoded
// characters that are not spaces, the gap's timesteps give the probability that some step emits a
// space, 1 - prod(1 - p_space(t)); at `threshold` or above a space is inserted at the step where
// p_space is highest. A threshold above 1 turns it off. `spaceClass` is Dictionary::Classes() - 1.
void InsertSpaces(const float* probs, uint32_t classes, uint32_t spaceClass, float threshold,
                  std::vector<DecodedChar>& chars);
constexpr float kDefaultSpaceThreshold = 0.25f;

// A recognised line in the coordinates of its crop: text, confidence, and per code point the span
// [x0, x1) along the crop's width (contiguous: each boundary is midway between the centres of two
// neighbouring characters' timesteps). `cropWidth` is the crop's width in the pixels the spans are
// wanted in; `steps` covers it evenly. Leading and trailing spaces are dropped.
struct CropText {
    std::string text;
    uint16_t conf = 0;              // per myriad
    std::vector<float> x0, x1;      // one per code point of text
};
void AssembleText(const std::vector<DecodedChar>& chars, const Dictionary& dictionary, uint32_t steps,
                  float cropWidth, CropText& out);

// Confidence 0..1 as per-myriad integer, rounded.
uint16_t ToConf(double value);

} // namespace RayoPdf::Ocr
