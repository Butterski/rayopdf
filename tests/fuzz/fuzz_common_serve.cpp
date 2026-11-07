// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the --serve --json input: the bytes as standard input, split into lines by
// Serve::LineReader (in pieces of varying size, with a small line limit so long lines are met),
// each line read by Serve::ParseRequest, and every answer's record written by Batch::AppendRecord.
#include "common/batch.h"
#include "common/serve.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Input input{data, size, 0};
    RayoPdf::Serve::LineReader reader(Read, &input);
    RayoPdf::Serve::Request request;
    std::string line, error, answer;
    for (int lines = 0; lines < 4096; ++lines) {
        const RayoPdf::Serve::LineReader::Status status = reader.Next(line, 4096);
        if (status == RayoPdf::Serve::LineReader::Status::End) break;
        RayoPdf::Batch::Record record;
        if (status == RayoPdf::Serve::LineReader::Status::Line) {
            if (RayoPdf::Serve::ParseRequest(line, request, error)) {
                if (request.input.empty() || (request.hasOutput && request.output.empty())) __builtin_trap();
                record.input = request.input;
                record.output = request.hasOutput ? request.output : std::string("-");
                for (const std::string& arg : request.args) record.error += arg;
            } else {
                record.status = "error";
                record.error = error;
            }
            record.id = request.id;
        }
        answer.clear();
        RayoPdf::Batch::AppendRecord(answer, record);
        if (answer.empty() || answer.back() != '\n') __builtin_trap();
    }
    return 0;
}
