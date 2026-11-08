// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the OCR add-on's input: the bytes as the add-on's standard input, read in
// pieces of varying size by the protocol server (src/ocr/server.h) with the PP-OCR engine's whole
// pre- and post-processing (resampling, DB boxes, crops, CTC decoding) on the model-free stand-in
// runtime of tests/ocr_fake_runtime.h, alone or behind a first pass (an odd first input byte; its
// dictionary has a CJK entry, so lines go to the recogniser for both reasons). Every answer must be a
// frame the client's parser accepts, and the same input must give the same output.
#include "common/ocr_protocol.h"
#include "ocr/engine.h"
#include "ocr/recognize.h"
#include "ocr/server.h"

#include "../ocr_fake_runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

using namespace RayoPdf;

struct Input {
    const uint8_t* data;
    size_t size;
    size_t at;
};

// Pieces of 1 to 64 bytes, sized by the bytes themselves, as a pipe hands them over.
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

bool Write(void* context, std::string_view bytes) {
    static_cast<std::string*>(context)->append(bytes);
    return true;
}

size_t ReadOutput(void* context, char* buffer, size_t size) {
    Input& input = *static_cast<Input*>(context);
    if (input.at >= input.size) return 0;
    size_t take = input.size - input.at < size ? input.size - input.at : size;
    std::memcpy(buffer, input.data + input.at, take);
    input.at += take;
    return take;
}

std::string RunOnce(const uint8_t* data, size_t size, const Ocr::Dictionary& dictionary,
                    const Ocr::Dictionary& firstDictionary) {
    Ocr::Testing::FakeRuntime runtime(dictionary.Classes()), firstRuntime(firstDictionary.Classes());
    Ocr::PpOcrSettings settings;
    settings.detMaxSide = 256;  // keeps a large page cheap; the post-processing is the same
    settings.switchAfter = 2;
    Ocr::PpOcrEngine single(runtime, dictionary, settings);
    Ocr::PpOcrEngine cascade(runtime, dictionary, firstRuntime, firstDictionary, settings);
    Ocr::Engine& engine = size && (data[0] & 1) ? static_cast<Ocr::Engine&>(cascade) : single;
    Input input{data, size, 0};
    std::string output;
    Ocr::ServerIo io;
    io.read = Read;
    io.readContext = &input;
    io.write = Write;
    io.writeContext = &output;
    OcrProtocol::Handshake handshake;
    handshake.version = "0";
    handshake.protocol = 1;
    handshake.engine = "pp-ocrv6";
    handshake.tier = "fuzz";
    handshake.runtime = "fake";
    Ocr::Serve(io, handshake, engine);
    return output;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static Ocr::Dictionary dictionary;
    static Ocr::Dictionary firstDictionary;
    static const bool loaded =
        dictionary.Load("a\nb\nc\n\xc3\xa9\n") && firstDictionary.Load("\xe4\xb8\xad\nb\nc\nd\n");
    if (!loaded) __builtin_trap();
    const std::string output = RunOnce(data, size, dictionary, firstDictionary);
    // Every frame written is one the client reads: the handshake, then responses.
    Input reader{reinterpret_cast<const uint8_t*>(output.data()), output.size(), 0};
    OcrProtocol::Frame frame;
    std::string error;
    if (OcrProtocol::ReadFrame(ReadOutput, &reader, frame) != OcrProtocol::FrameStatus::Ok) __builtin_trap();
    OcrProtocol::Handshake handshake;
    if (!OcrProtocol::ParseHandshake(frame.header, handshake, error)) __builtin_trap();
    for (;;) {
        const OcrProtocol::FrameStatus status = OcrProtocol::ReadFrame(ReadOutput, &reader, frame);
        if (status == OcrProtocol::FrameStatus::End) break;
        OcrProtocol::Response response;
        if (status != OcrProtocol::FrameStatus::Ok || !OcrProtocol::ParseResponse(frame.header, response, error)) {
            __builtin_trap();
        }
        for (const OcrProtocol::Line& line : response.lines) {
            if (line.chars.size() != OcrProtocol::CodePoints(line.text)) __builtin_trap();
        }
    }
    if (RunOnce(data, size, dictionary, firstDictionary) != output) __builtin_trap();
    return 0;
}
