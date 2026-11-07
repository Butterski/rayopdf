// SPDX-License-Identifier: Apache-2.0
// Derived from RayoMD src/cli/serve_request.h at ef106ca (Apache-2.0, same author).
// A request of `rayopdf --serve --json`, protocol 1 (RayoMD's protocol 1 for PDF to Markdown):
// one JSON object a line, of the fields below and no other,
//
//   {"id":"1","input":"a.pdf","output":"a.md","args":["--pages=1-3"]}
//
// "input" is required; "output" may be left out, and the answer then carries the Markdown itself.
// The reader takes exactly that shape (strings, an integer id and an array of strings as values; no
// nesting, no null) and rejects anything else, reading each byte once. The fuzz target
// fuzz_common_serve reads untrusted lines through it.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Serve {

constexpr int kProtocol = 1;
// The longest request line, without its line break: a request holds paths and flags only.
constexpr size_t kMaxRequestBytes = size_t(1) << 20;
// The most `args` a request may have.
constexpr size_t kMaxArgs = 64;

struct Request {
    // The id as it was sent, as JSON text (a string with its quotes, or an integer); empty for none.
    // The answer carries it back unchanged.
    std::string id;
    std::string input;              // UTF-8, never empty
    std::string output;             // UTF-8; empty when `hasOutput` is false
    std::vector<std::string> args;  // flags of --convert, applied over the session's
    bool hasOutput = false;
};

// Reads `line` into `request`, whose buffers are reused from request to request. False, with
// `error`, when it is not a request of protocol 1; `request.id` is kept once it was read, so the
// answer to a request with a bad field still carries it.
bool ParseRequest(std::string_view line, Request& request, std::string& error);

// The lines of an input, through a buffer of its own: `read` fills it with what the input has (a
// read(2) or ReadFile, never a stdio call that waits for a full buffer) and returns 0 at the end or
// on an error.
class LineReader {
public:
    using ReadFunction = size_t (*)(void* context, char* buffer, size_t size);
    enum class Status { Line, TooLong, End };

    LineReader(ReadFunction read, void* context) : read_(read), context_(context) {}

    // The next line, without its line break and a '\r' before it. A line longer than `limit` bytes
    // is read to its end without being kept and is TooLong; End when the input is over.
    Status Next(std::string& line, size_t limit);

private:
    ReadFunction read_;
    void* context_;
    size_t begin_ = 0;
    size_t end_ = 0;
    char buffer_[64 * 1024];
};

} // namespace RayoPdf::Serve
