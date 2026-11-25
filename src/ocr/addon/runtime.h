// SPDX-License-Identifier: Apache-2.0
// The inference runtime of the add-on rayopdf-ocr: ncnn (runtime_ncnn.cpp). It loads a model
// folder's networks (models.h) and runs them on one thread. The tests run the engine on a fake
// runtime instead (tests/ocr_fake_runtime.h).
#pragma once

#include "ocr/engine.h"
#include "ocr/models.h"

#include <memory>
#include <string>

namespace RayoPdf::Ocr {

extern const char* const kRuntimeName;     // "ncnn"
extern const char* const kRuntimeVersion;  // the pinned version docker/ocr.Dockerfile builds
// The files of the tier folder this runtime reads (besides config.txt and dict.txt), in the order
// the handshake lists them.
extern const char* const kRuntimeFiles[];
extern const size_t kRuntimeFileCount;

// Which of a folder's networks a runtime loads: the detector and the recogniser (a tier), the
// recogniser alone (a first pass, a recogniser of the script pack), or the orientation classifier
// (cls.*).
enum class Networks : uint8_t { DetectAndRecognize, RecognizeOnly, Classify };

// The runtime with the folder's networks loaded, or null with `detail` saying why. A runtime without
// the detector fails Detect; only one loaded for it classifies.
std::unique_ptr<Runtime> CreateRuntime(const ModelSet& set, Networks networks, std::string& detail);

// Whether this runtime can read `set`: its manifest lists the runtime's file for that network.
bool RuntimeReads(const ModelSet& set, Networks networks);

} // namespace RayoPdf::Ocr
