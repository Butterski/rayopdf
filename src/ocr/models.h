// SPDX-License-Identifier: Apache-2.0
// The model folder of the OCR add-on: <models>/<tier>/ as docker/ocr_models.py writes it.
//
//   models.txt   "<sha256> <bytes> <file>" a line: every file of the tier, checked by size when the
//                add-on starts (by SHA-256 with `rayopdf-ocr --verify`); the hashes go into the
//                handshake
//   config.txt   "<key> <value>" a line: model, det_thresh, det_box_thresh, det_unclip_ratio,
//                rec_height (unknown keys are ignored)
//   dict.txt     the recogniser's character list (recognize.h)
//   det.*, rec.* the networks, in each runtime's format (det.ncnn.param/.bin, det.ort, ...)
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
    std::string directory;  // <models>/<tier>, no trailing separator
    std::string model;      // config "model", e.g. PP-OCRv6_small
    DetectParams detect;
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

// Reads models.txt, config.txt and dict.txt of `directory` and checks that every file models.txt
// lists has its size. `detail` names the file at fault.
ModelStatus LoadModelSet(const std::string& directory, ModelSet& set, std::string& detail);

// Hashes every file of the manifest (slow: the networks are megabytes).
ModelStatus VerifyModelSet(const ModelSet& set, std::string& detail);

// The SHA-256 the manifest records for `name`, or empty.
std::string_view ManifestHash(const ModelSet& set, std::string_view name);

} // namespace RayoPdf::Ocr
