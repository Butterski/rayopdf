// SPDX-License-Identifier: Apache-2.0
// The protocol server of the OCR add-on (`rayopdf-ocr --serve`, protocol 1 of
// docs/development/ocr-protocol.md, frames through common/ocr_protocol.h): it writes the handshake,
// then answers every request frame with one response frame, in order, until "quit" or the end of
// its input.
//
// - A frame whose header is not a valid request gets an error response ("bad-request",
//   "too-large", "bad-pixels" as OcrProtocol::ParseRequest decides) carrying the request's id when
//   it was read, and the server goes on with the next frame.
// - A frame that cannot be read whole (truncated, over a limit, malformed length or payload member)
//   leaves the stream without a trustworthy boundary: the server answers "too-large" or
//   "bad-request" with id 0 and stops.
// - "page" runs Engine::Page on the payload; "lines" runs Engine::Line on each line image, one
//   response line per image in request order; an engine failure answers "engine-failed".
// - "quit" is answered with status ok and no lines, then the server stops.
// The response to a request depends on nothing but that request (the engine keeps no state that
// changes results), so the same request bytes always give the same response bytes.
#pragma once

#include "common/ocr_protocol.h"
#include "ocr/engine.h"

#include <cstdint>
#include <string_view>

namespace RayoPdf::Ocr {

// Writes all of `bytes`; false when the output is gone.
using WriteFunction = bool (*)(void* context, std::string_view bytes);

struct ServerIo {
    OcrProtocol::ReadFunction read = nullptr;
    void* readContext = nullptr;
    WriteFunction write = nullptr;
    void* writeContext = nullptr;
};

enum class ServeStatus : uint8_t {
    Quit,         // answered "quit"
    End,          // the input ended between frames
    BrokenFrame,  // a frame could not be read; answered with an error, then stopped
    WriteFailed,  // the output failed
};

ServeStatus Serve(const ServerIo& io, const OcrProtocol::Handshake& handshake, Engine& engine);

} // namespace RayoPdf::Ocr
