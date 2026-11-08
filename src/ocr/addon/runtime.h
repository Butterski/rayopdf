// SPDX-License-Identifier: Apache-2.0
// The inference runtime the add-on rayopdf-ocr is built with: exactly one of runtime_ncnn.cpp and
// runtime_onnxruntime.cpp is compiled in (CMake RAYOPDF_OCR_RUNTIME). Both load the tier's
// detection and recognition networks from its model folder (models.h) and run them on one thread.
#pragma once

#include "ocr/engine.h"
#include "ocr/models.h"

#include <memory>
#include <string>

namespace RayoPdf::Ocr {

extern const char* const kRuntimeName;     // "ncnn" or "onnxruntime"
extern const char* const kRuntimeVersion;  // the pinned version docker/ocr.Dockerfile builds
// The files of the tier folder this runtime reads (besides config.txt and dict.txt), in the order
// the handshake lists them.
extern const char* const kRuntimeFiles[];
extern const size_t kRuntimeFileCount;

// The runtime with the tier's networks loaded, or null with `detail` saying why.
std::unique_ptr<Runtime> CreateRuntime(const ModelSet& set, std::string& detail);

} // namespace RayoPdf::Ocr
