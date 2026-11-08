// SPDX-License-Identifier: Apache-2.0
// The core's side of the OCR add-on (docs/development/ocr-protocol.md): finding `rayopdf-ocr`,
// starting it with --serve, the handshake, one request at a time with a deadline, and killing it
// when it crashes, hangs or answers anything but a well-formed response to the request it was sent.
// A session is used by one thread; the batch modes give each worker its own.
#pragma once

#include "common/ocr_protocol.h"
#include "layout/ocr_options.h"
#include "layout/ocr_process.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::Layout {

// The add-on's file name: rayopdf-ocr, rayopdf-ocr.exe on Windows.
extern const char* const kOcrAddonName;

// The add-on to start, in the contract's order: `explicitPath` (--ocr-addon=PATH) when given, else
// the environment variable RAYOPDF_OCR when set, else rayopdf-ocr in this executable's directory,
// else the first one on PATH. A path given (flag or variable) that names no executable file is not
// replaced by a later step. Empty when there is none.
std::string FindOcrAddon(const std::string& explicitPath);

enum class OcrStatus : uint8_t {
    Ok,
    Missing,      // no add-on found
    StartFailed,  // it could not be started, or its handshake was missing, malformed or not protocol 1
    Timeout,      // no complete answer within the request timeout: killed
    Crashed,      // its output ended (it exited or closed it): killed
    Garbage,      // an answer that is no well-formed response frame: killed
    WrongId,      // a response to another request: killed
    Refused,      // a well-formed error response (status "error"); the add-on keeps running
};

// "ok", "missing", "start-failed", "timeout", "crashed", "garbage", "wrong-id", "refused".
const char* OcrStatusName(OcrStatus status);

class OcrSession {
public:
    explicit OcrSession(const OcrOptions& options);
    ~OcrSession();  // Quit()
    OcrSession(const OcrSession&) = delete;
    OcrSession& operator=(const OcrSession&) = delete;

    // Finds and starts the add-on unless it runs. The search runs once per process and add-on
    // path (the environment as it was then). An add-on that is missing or fails to start stays
    // so for the session's life (no start is repeated); one killed after a request starts again on
    // the next call.
    OcrStatus Ensure();
    // Sends `request` (its id is set here) with `pixels` and reads the answer into `response`.
    // Starts the add-on first when needed.
    OcrStatus Recognize(OcrProtocol::Request& request, std::string_view pixels, OcrProtocol::Response& response);
    // Ends the add-on: a quit request, then its exit awaited for a moment before it is killed.
    void Quit();

    bool Running() const { return child_.Running(); }
    const OcrProtocol::Handshake& Handshake() const { return handshake_; }
    const std::string& Path() const { return path_; }

private:
    OcrStatus Fail(OcrStatus status);

    std::string explicit_;
    uint32_t requestMs_;
    uint32_t handshakeMs_;
    bool searched_ = false;
    bool unusable_ = false;
    std::string path_;
    ChildProcess child_;
    OcrProtocol::Handshake handshake_;
    OcrProtocol::Frame frame_;
    std::string out_;
    std::string error_;
    uint64_t nextId_ = 1;
};

} // namespace RayoPdf::Layout
