// SPDX-License-Identifier: Apache-2.0
// layout/ocr_process.h where no process can be started (the WebAssembly module, CMake's
// EMSCRIPTEN builds, in place of ocr_process.cpp): no child starts and no add-on is found, so a
// conversion that asks for OCR reports the add-on as missing, as when none is installed.
#include "layout/ocr_process.h"

namespace RayoPdf::Layout {

ChildProcess::~ChildProcess() {
    CloseHandles();
}

bool ChildProcess::Start(const std::string&, const std::vector<std::string>&, bool) {
    return false;
}

bool ChildProcess::Running() const {
    return false;
}

bool ChildProcess::Write(std::string_view, OcrClock::time_point) {
    return false;
}

size_t ChildProcess::Read(char*, size_t, OcrClock::time_point) {
    return 0;
}

void ChildProcess::Finish(OcrClock::time_point) {
    CloseHandles();
}

void ChildProcess::Kill() {
    CloseHandles();
}

void ChildProcess::CloseHandles() {
    pid_ = -1;
    socket_ = -1;
}

std::string ExecutableDirectory() {
    return std::string();
}

std::string EnvironmentValue(const char*) {
    return std::string();
}

bool IsExecutableFile(const std::string&) {
    return false;
}

} // namespace RayoPdf::Layout
