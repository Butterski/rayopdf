// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_client.h"

#include <mutex>
#include <vector>

namespace RayoPdf::Layout {

#ifdef _WIN32
const char* const kOcrAddonName = "rayopdf-ocr.exe";
constexpr char kPathSeparator = ';';
#else
const char* const kOcrAddonName = "rayopdf-ocr";
constexpr char kPathSeparator = ':';
#endif

namespace {

constexpr uint32_t kQuitTimeoutMs = 5000;
constexpr size_t kMaxPathEntries = 4096;

struct ReadContext {
    ChildProcess* child;
    OcrClock::time_point deadline;
};

size_t ReadChild(void* context, char* buffer, size_t size) {
    ReadContext* c = static_cast<ReadContext*>(context);
    return c->child->Read(buffer, size, c->deadline);
}

OcrClock::time_point After(uint32_t ms) {
    return OcrClock::now() + std::chrono::milliseconds(ms);
}

std::string Join(const std::string& directory, const char* name) {
    if (directory.empty()) return name;
    const char last = directory.back();
    return directory + (last == '/' || last == '\\' ? "" : "/") + name;
}

} // namespace

std::string FindOcrAddon(const std::string& explicitPath) {
    if (!explicitPath.empty()) return IsExecutableFile(explicitPath) ? explicitPath : std::string();
    const std::string variable = EnvironmentValue("RAYOPDF_OCR");
    if (!variable.empty()) return IsExecutableFile(variable) ? variable : std::string();
    const std::string own = ExecutableDirectory();
    if (!own.empty()) {
        const std::string candidate = Join(own, kOcrAddonName);
        if (IsExecutableFile(candidate)) return candidate;
    }
    const std::string path = EnvironmentValue("PATH");
    size_t at = 0;
    for (size_t entries = 0; at <= path.size() && entries < kMaxPathEntries; ++entries) {
        size_t next = path.find(kPathSeparator, at);
        if (next == std::string::npos) next = path.size();
        if (next > at) {
            const std::string candidate = Join(path.substr(at, next - at), kOcrAddonName);
            if (IsExecutableFile(candidate)) return candidate;
        }
        at = next + 1;
    }
    return std::string();
}

namespace {

// FindOcrAddon once per process and explicit path: the environment is read at the first search.
std::string FindOcrAddonOnce(const std::string& explicitPath) {
    static std::mutex mutex;
    static bool searched = false;
    static std::string key, found;
    std::lock_guard<std::mutex> lock(mutex);
    if (!searched || key != explicitPath) {
        found = FindOcrAddon(explicitPath);
        key = explicitPath;
        searched = true;
    }
    return found;
}

} // namespace

const char* OcrStatusName(OcrStatus status) {
    switch (status) {
        case OcrStatus::Ok: return "ok";
        case OcrStatus::Missing: return "missing";
        case OcrStatus::StartFailed: return "start-failed";
        case OcrStatus::Timeout: return "timeout";
        case OcrStatus::Crashed: return "crashed";
        case OcrStatus::Garbage: return "garbage";
        case OcrStatus::WrongId: return "wrong-id";
        case OcrStatus::Refused: break;
    }
    return "refused";
}

OcrSession::OcrSession(const OcrOptions& options)
    : explicit_(options.addon), requestMs_(options.requestTimeoutMs), handshakeMs_(options.handshakeTimeoutMs) {}

OcrSession::~OcrSession() {
    Quit();
}

OcrStatus OcrSession::Fail(OcrStatus status) {
    child_.Kill();
    return status;
}

OcrStatus OcrSession::Ensure() {
    if (child_.Running()) return OcrStatus::Ok;
    if (!searched_) {
        path_ = FindOcrAddonOnce(explicit_);
        searched_ = true;
    }
    if (path_.empty()) return OcrStatus::Missing;
    if (unusable_) return OcrStatus::StartFailed;
    const bool keepStandardError = EnvironmentValue("RAYOPDF_OCR_STDERR") == "1";
    if (!child_.Start(path_, std::vector<std::string>{"--serve"}, keepStandardError)) {
        unusable_ = true;
        return OcrStatus::StartFailed;
    }
    ReadContext context{&child_, After(handshakeMs_)};
    const OcrProtocol::FrameStatus status = OcrProtocol::ReadFrame(ReadChild, &context, frame_);
    if (status != OcrProtocol::FrameStatus::Ok || !frame_.payload.empty() ||
        !OcrProtocol::ParseHandshake(frame_.header, handshake_, error_) || handshake_.protocol != OcrProtocol::kProtocol ||
        handshake_.version.empty()) {
        unusable_ = true;
        return Fail(OcrStatus::StartFailed);
    }
    nextId_ = 1;
    return OcrStatus::Ok;
}

OcrStatus OcrSession::Recognize(OcrProtocol::Request& request, std::string_view pixels,
                                OcrProtocol::Response& response) {
    response = OcrProtocol::Response();
    const OcrStatus ready = Ensure();
    if (ready != OcrStatus::Ok) return ready;
    request.id = nextId_++;
    OcrProtocol::WriteRequest(request, pixels, out_);
    ReadContext context{&child_, After(requestMs_)};
    if (!child_.Write(out_, context.deadline)) return Fail(child_.TimedOut() ? OcrStatus::Timeout : OcrStatus::Crashed);
    const OcrProtocol::FrameStatus status = OcrProtocol::ReadFrame(ReadChild, &context, frame_);
    if (status != OcrProtocol::FrameStatus::Ok) {
        if (child_.TimedOut()) return Fail(OcrStatus::Timeout);
        const bool ended = status == OcrProtocol::FrameStatus::End || status == OcrProtocol::FrameStatus::Truncated;
        return Fail(ended ? OcrStatus::Crashed : OcrStatus::Garbage);
    }
    if (!frame_.payload.empty() || !OcrProtocol::ParseResponse(frame_.header, response, error_))
        return Fail(OcrStatus::Garbage);
    if (response.id != request.id) return Fail(OcrStatus::WrongId);
    return response.ok ? OcrStatus::Ok : OcrStatus::Refused;
}

void OcrSession::Quit() {
    if (!child_.Running()) return;
    OcrProtocol::Request quit;
    quit.op = OcrProtocol::Op::Quit;
    quit.id = nextId_++;
    OcrProtocol::WriteRequest(quit, std::string_view(), out_);
    ReadContext context{&child_, After(kQuitTimeoutMs)};
    if (child_.Write(out_, context.deadline)) OcrProtocol::ReadFrame(ReadChild, &context, frame_);
    child_.Finish(context.deadline);
}

} // namespace RayoPdf::Layout
