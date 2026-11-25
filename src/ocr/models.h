// SPDX-License-Identifier: Apache-2.0
// The model folders of the OCR add-on, as docker/ocr_models.py writes them: a tier
// (<models>/<tier>/), a recogniser of the script pack (<models>/scripts/<script>/) and the
// text-line orientation classifier (<models>/orient/).
//
//   models.txt   "<sha256> <bytes> <file>" a line: every file of the folder, checked by size when
//                the add-on loads it (by SHA-256 with `rayopdf-ocr --verify`); the hashes go into the
//                handshake
//   config.txt   "<key> <value>" a line: model; det_thresh, det_box_thresh, det_unclip_ratio (a
//                tier: all three, else none); rec_height; script (a script pack folder: its script);
//                order (logical, or visual for a recogniser that reads right-to-left text left to
//                right); space_gap (InsertInkSpaces, in line heights); letters ("XXXX-YYYY",
//                the script's code points); unknown keys are ignored
//   dict.txt     the recogniser's character list (recognize.h), or the classifier's labels
//   det.*, rec.*, cls.*   the networks, in the runtime's format (det.ncnn.param, det.ncnn.bin, ...)
#pragma once

#include "ocr/detect.h"
#include "ocr/recognize.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Ocr {

struct ModelFile {
    std::string name;
    std::string sha256;  // 64 lowercase hex digits
    uint64_t bytes = 0;
};

struct ModelSet {
    std::string directory;     // no trailing separator
    std::string model;         // config "model", e.g. PP-OCRv6_small
    bool detectConfig = false; // config has the detection thresholds (a tier)
    DetectParams detect;
    std::string script;        // config "script", empty outside the script pack
    bool visualOrder = false;  // config "order visual"
    float spaceGap = 0;        // config "space_gap": InsertInkSpaces' gap in line heights, 0 off
    uint32_t lettersFirst = 0, lettersLast = 0;  // config "letters": the script's code points
    Dictionary dictionary;
    std::vector<ModelFile> files;
};

enum class ModelStatus : uint8_t {
    Ok,
    Missing,         // a file of the folder is absent or unreadable
    BadManifest,     // models.txt malformed
    BadConfig,       // config.txt malformed or out of range
    BadDictionary,   // dict.txt not a dictionary
    SizeMismatch,    // a file's size differs from models.txt
    HashMismatch,    // (VerifyModelSet) a file's SHA-256 differs
};

const char* ModelStatusText(ModelStatus status);

// Parsers of the two text files (exposed for the tests).
bool ParseManifest(std::string_view text, std::vector<ModelFile>& files);
bool ParseConfig(std::string_view text, ModelSet& set);
// A decimal number such as "0.45" or "2" (no sign, no exponent), in 0 to 1000.
bool ParseDecimal(std::string_view text, float& value);
// A code point in hexadecimal, 1 to 6 digits.
bool ParseHex(std::string_view text, uint32_t& value);

// Reads models.txt, config.txt and dict.txt of `directory` and checks that every file models.txt
// lists has its size. `detail` names the file at fault.
ModelStatus LoadModelSet(const std::string& directory, ModelSet& set, std::string& detail);

// Hashes every file of the manifest (slow: the networks are megabytes).
ModelStatus VerifyModelSet(const ModelSet& set, std::string& detail);

// The SHA-256 the manifest records for `name`, or empty.
std::string_view ManifestHash(const ModelSet& set, std::string_view name);

} // namespace RayoPdf::Ocr
