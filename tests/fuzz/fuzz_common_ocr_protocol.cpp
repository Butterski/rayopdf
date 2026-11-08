// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the OCR add-on protocol's frame codec (common/ocr_protocol.h), which reads
// what the add-on sends the core and what the core sends the add-on: the bytes as a stream of
// frames (in pieces of varying size, as a pipe hands them over), each header read as a handshake,
// a request and a response, and also the whole input as one header. Whatever parses must be
// written back and read again to the same structure, and the second writing must give the same
// bytes as the first.
#include "common/ocr_protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

using namespace RayoPdf::OcrProtocol;

struct Input {
    const uint8_t* data;
    size_t size;
    size_t at;
};

// Pieces of 1 to 64 bytes, sized by the bytes themselves.
size_t Read(void* context, char* buffer, size_t size) {
    Input& input = *static_cast<Input*>(context);
    if (input.at >= input.size) return 0;
    size_t take = 1 + input.data[input.at] % 64;
    if (take > input.size - input.at) take = input.size - input.at;
    if (take > size) take = size;
    std::memcpy(buffer, input.data + input.at, take);
    input.at += take;
    return take;
}

size_t ReadAll(void* context, char* buffer, size_t size) {
    std::string_view& rest = *static_cast<std::string_view*>(context);
    const size_t take = size < rest.size() ? size : rest.size();
    std::memcpy(buffer, rest.data(), take);
    rest.remove_prefix(take);
    return take;
}

// One frame of `bytes`, which must be whole.
void ReadBack(const std::string& bytes, Frame& frame) {
    std::string_view rest = bytes;
    if (ReadFrame(ReadAll, &rest, frame) != FrameStatus::Ok || !rest.empty()) __builtin_trap();
}

bool Same(const Response& a, const Response& b) {
    if (a.id != b.id || a.ok != b.ok || a.error != b.error || a.lines.size() != b.lines.size()) return false;
    for (size_t i = 0; i < a.lines.size(); ++i) {
        const Line &x = a.lines[i], &y = b.lines[i];
        if (x.x0 != y.x0 || x.y0 != y.y0 || x.x1 != y.x1 || x.y1 != y.y1 || x.text != y.text || x.conf != y.conf ||
            x.chars.size() != y.chars.size())
            return false;
        for (size_t k = 0; k < x.chars.size(); ++k)
            if (x.chars[k].x0 != y.chars[k].x0 || x.chars[k].x1 != y.chars[k].x1) return false;
    }
    return true;
}

void CheckHeader(std::string_view header, std::string_view payload) {
    std::string error, first, second;
    Frame frame;

    Request request, again;
    if (ParseRequest(header, payload.size(), request, error)) {
        if (!error.empty() || RequestPixels(request) != payload.size()) __builtin_trap();
        WriteRequest(request, payload, first);
        ReadBack(first, frame);
        if (frame.payload != payload || !ParseRequest(frame.header, frame.payload.size(), again, error)) __builtin_trap();
        if (again.id != request.id || again.op != request.op || again.width != request.width ||
            again.height != request.height || again.dpi != request.dpi || again.lines.size() != request.lines.size())
            __builtin_trap();
        WriteRequest(again, payload, second);
        if (first != second) __builtin_trap();
    } else if (error != kErrorBadRequest && error != kErrorTooLarge && error != kErrorBadPixels) {
        __builtin_trap();
    }

    Response response, back;
    if (ParseResponse(header, response, error)) {
        for (const Line& line : response.lines)
            if (line.conf > kConfScale || line.chars.size() != CodePoints(line.text) || line.x0 > line.x1 ||
                line.y0 > line.y1)
                __builtin_trap();
        WriteResponse(response, first);
        ReadBack(first, frame);
        if (!frame.payload.empty() || !ParseResponse(frame.header, back, error) || !Same(response, back)) __builtin_trap();
        WriteResponse(back, second);
        if (first != second) __builtin_trap();
    }

    Handshake hello, hello2;
    if (ParseHandshake(header, hello, error)) {
        WriteHandshake(hello, first);
        ReadBack(first, frame);
        if (!ParseHandshake(frame.header, hello2, error) || hello2.version != hello.version ||
            hello2.protocol != hello.protocol || hello2.engine != hello.engine || hello2.models != hello.models ||
            hello2.scripts != hello.scripts)
            __builtin_trap();
        WriteHandshake(hello2, second);
        if (first != second) __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    CheckHeader(std::string_view(reinterpret_cast<const char*>(data), size), std::string_view());
    Input input{data, size, 0};
    Frame frame;
    for (int frames = 0; frames < 1024; ++frames) {
        const FrameStatus status = ReadFrame(Read, &input, frame);
        if (status != FrameStatus::Ok) break;
        if (frame.header.empty() || frame.header.back() != '\n') __builtin_trap();
        CheckHeader(frame.header, frame.payload);
    }
    return 0;
}
