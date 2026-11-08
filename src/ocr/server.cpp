// SPDX-License-Identifier: Apache-2.0
#include "ocr/server.h"

namespace RayoPdf::Ocr {
namespace {

// A response whose header would pass the frame limit (a page of thousands of long lines) cannot be
// read by the client: it is answered "too-large" instead.
bool Answer(const ServerIo& io, const OcrProtocol::Response& response, std::string& frame) {
    OcrProtocol::WriteResponse(response, frame);
    if (frame.size() - 4 > OcrProtocol::kMaxHeaderBytes) {
        OcrProtocol::Response refusal;
        refusal.id = response.id;
        refusal.ok = false;
        refusal.error = OcrProtocol::kErrorTooLarge;
        OcrProtocol::WriteResponse(refusal, frame);
    }
    return io.write(io.writeContext, frame);
}

bool AnswerError(const ServerIo& io, uint64_t id, const char* error, OcrProtocol::Response& response,
                 std::string& frame) {
    response.id = id;
    response.ok = false;
    response.error = error;
    response.lines.clear();
    return Answer(io, response, frame);
}

} // namespace

ServeStatus Serve(const ServerIo& io, const OcrProtocol::Handshake& handshake, Engine& engine) {
    std::string out;
    OcrProtocol::WriteHandshake(handshake, out);
    if (!io.write(io.writeContext, out)) return ServeStatus::WriteFailed;

    OcrProtocol::Frame frame;
    OcrProtocol::Request request;
    OcrProtocol::Response response;
    std::string error;
    for (;;) {
        const OcrProtocol::FrameStatus status = OcrProtocol::ReadFrame(io.read, io.readContext, frame);
        if (status == OcrProtocol::FrameStatus::End) return ServeStatus::End;
        if (status != OcrProtocol::FrameStatus::Ok) {
            const char* code = status == OcrProtocol::FrameStatus::TooLarge ? OcrProtocol::kErrorTooLarge
                                                                            : OcrProtocol::kErrorBadRequest;
            if (!AnswerError(io, 0, code, response, out)) return ServeStatus::WriteFailed;
            return ServeStatus::BrokenFrame;
        }
        request = OcrProtocol::Request();
        error.clear();
        if (!OcrProtocol::ParseRequest(frame.header, frame.payload.size(), request, error)) {
            const char* code = error == OcrProtocol::kErrorTooLarge   ? OcrProtocol::kErrorTooLarge
                               : error == OcrProtocol::kErrorBadPixels ? OcrProtocol::kErrorBadPixels
                                                                       : OcrProtocol::kErrorBadRequest;
            if (!AnswerError(io, request.id, code, response, out)) return ServeStatus::WriteFailed;
            continue;
        }
        response.id = request.id;
        response.ok = true;
        response.error.clear();
        response.lines.clear();
        const uint8_t* pixels = reinterpret_cast<const uint8_t*>(frame.payload.data());
        bool failed = false;
        switch (request.op) {
        case OcrProtocol::Op::Quit:
            if (!Answer(io, response, out)) return ServeStatus::WriteFailed;
            return ServeStatus::Quit;
        case OcrProtocol::Op::Page:
            failed = engine.Page(GrayView{pixels, request.width, request.height}, request.dpi, response.lines) !=
                     EngineStatus::Ok;
            break;
        case OcrProtocol::Op::Lines: {
            size_t at = 0;
            response.lines.resize(request.lines.size());
            for (size_t i = 0; i < request.lines.size() && !failed; ++i) {
                const OcrProtocol::LineSize& size = request.lines[i];
                failed = engine.Line(GrayView{pixels + at, size.width, size.height}, response.lines[i]) !=
                         EngineStatus::Ok;
                at += size_t(size.width) * size.height;
            }
            break;
        }
        }
        if (failed) {
            if (!AnswerError(io, request.id, OcrProtocol::kErrorEngineFailed, response, out)) {
                return ServeStatus::WriteFailed;
            }
            continue;
        }
        if (!Answer(io, response, out)) return ServeStatus::WriteFailed;
    }
}

} // namespace RayoPdf::Ocr
