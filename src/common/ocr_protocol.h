// SPDX-License-Identifier: Apache-2.0
// Protocol 1 of the OCR add-on `rayopdf-ocr --serve` (docs/development/ocr-protocol.md): the frame
// codec that the core (the client, layout/ocr_client.h) and the add-on (the server, src/ocr) share.
// Depends on the C++ standard library only, so the add-on links it without the rest of the core.
//
// A frame on a pipe is a little-endian u32 N, then a JSON header of N bytes whose last byte, and
// only line break, is '\n', then a binary payload of as many bytes as the header's member "payload"
// says (0 when it has none). N is 2 to kMaxHeaderBytes; header and payload together are at most
// kMaxFrameBytes. Three kinds of header travel:
//
//   handshake (add-on, first frame): {"rayopdf-ocr":"0.0.9","protocol":1,"engine":"pp-ocrv6",
//       "tier":"small","runtime":"ncnn","models":{"det":"<sha256>",...},"scripts":["latin",...]}
//   request (core):  {"id":1,"op":"page","width":W,"height":H,"channels":1,"dpi":300,"payload":W*H}
//                    {"id":2,"op":"lines","width":0,"height":0,"channels":1,"dpi":300,
//                     "lines":[{"width":w,"height":h},...],"payload":sum of w*h}
//                    {"id":3,"op":"quit","width":0,"height":0,"channels":1,"dpi":0,"payload":0}
//   response (add-on): {"id":1,"status":"ok","lines":[{"box":[x0,y0,x1,y1],"text":"...",
//                       "conf":0.9731,"chars":[[x0,x1],...]},...]}
//                      {"id":1,"status":"error","error":"bad-pixels"}
//
// Pixels are 8-bit gray, rows top to bottom with no padding. Boxes and character spans are integer
// pixels of the request's image, origin top-left, y down, x1 and y1 exclusive. "chars" has exactly
// one span per Unicode code point of "text", spaces included. "conf" is 0 to 1 with at most four
// decimals; the structures hold it as a per-myriad integer (kConfScale), so no floating point is
// parsed or printed. The writers emit members in the order above and nothing else (same structure,
// same bytes); the parsers take members in any order, skip unknown ones (nesting at most kMaxDepth),
// and reject anything malformed or out of bounds, reading each byte once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RayoPdf::OcrProtocol {

constexpr int kProtocol = 1;
constexpr size_t kMaxHeaderBytes = size_t(1) << 20;
constexpr uint64_t kMaxFrameBytes = uint64_t(256) << 20;  // header + payload
constexpr uint64_t kMaxPixels = uint64_t(1) << 28;        // pixels of one request, all line images together
constexpr size_t kMaxLines = size_t(1) << 16;             // line images of a request, lines of a response
constexpr uint32_t kMaxSide = uint32_t(1) << 20;          // width or height of one image
constexpr uint32_t kMaxDpi = 10000;
constexpr int kMaxDepth = 16;                             // JSON nesting of a header
constexpr uint16_t kConfScale = 10000;                    // conf 1.0
constexpr int32_t kMaxCoordinate = int32_t(1) << 30;      // |box and span coordinates|

// Error codes of a response with status "error".
constexpr const char* kErrorTooLarge = "too-large";
constexpr const char* kErrorBadRequest = "bad-request";
constexpr const char* kErrorBadPixels = "bad-pixels";
constexpr const char* kErrorEngineFailed = "engine-failed";

// ---- Frames ----

enum class FrameStatus : uint8_t {
    Ok,
    End,        // the input ended (or failed) before the first byte of a frame
    Truncated,  // the input ended (or failed) inside a frame
    TooLarge,   // header or frame over its limit
    Malformed,  // header length below 2, a line break missing or misplaced, a bad "payload" member
};

// Reads up to `size` bytes into `buffer`: returns how many (at least 1), or 0 at the end of the
// input, on an error or a timeout.
using ReadFunction = size_t (*)(void* context, char* buffer, size_t size);

struct Frame {
    std::string header;   // the JSON header, its '\n' included
    std::string payload;
};

// Reads one frame. The payload buffer grows with the bytes that arrive, never ahead of them.
// `frame`'s buffers are reused from frame to frame.
FrameStatus ReadFrame(ReadFunction read, void* context, Frame& frame);

// ---- Headers ----

enum class Op : uint8_t { Page, Lines, Quit };

struct LineSize {
    uint32_t width = 0, height = 0;
};

struct Request {
    uint64_t id = 0;
    Op op = Op::Page;
    uint32_t width = 0, height = 0;  // op page: the image; 0 otherwise
    uint32_t channels = 1;           // always 1 (gray)
    uint32_t dpi = 0;                // resolution of the pixels, 0 when unknown
    std::vector<LineSize> lines;     // op lines: the line images, concatenated in the payload
};

// One character's extent along its line's baseline: x for lines of dir 0 and 2, y for dir 1 and 3
// (pixels of the request's image, y down).
struct CharSpan {
    int32_t x0 = 0, x1 = 0;
};

struct Line {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::string text;               // UTF-8, logical order
    uint16_t conf = 0;              // 0 to kConfScale
    std::vector<CharSpan> chars;    // one a code point of `text`
    // The baseline direction of the text in the image (Layout::Glyph::dir): 0 left to right, 1
    // bottom to top, 2 upside down (right to left), 3 top to bottom. Written only when not 0.
    uint8_t dir = 0;
};

struct Response {
    uint64_t id = 0;
    bool ok = true;
    std::string error;              // a kError* code when !ok
    std::vector<Line> lines;
};

struct Handshake {
    std::string version;            // "rayopdf-ocr": the add-on's version
    int protocol = 0;
    std::string engine, tier, runtime;
    std::vector<std::pair<std::string, std::string>> models;  // name, SHA-256 (hex), in order
    std::vector<std::string> scripts;
};

// Whole frames (u32, header, payload) appended to `frame` (replaced). WriteRequest takes the
// payload; responses and the handshake carry none.
void WriteRequest(const Request& request, std::string_view payload, std::string& frame);
void WriteResponse(const Response& response, std::string& frame);
void WriteHandshake(const Handshake& handshake, std::string& frame);

// Pixel bytes `request` needs in its payload (0 for quit), or UINT64_MAX when its sizes overflow.
uint64_t RequestPixels(const Request& request);

// Parse a header (with or without its '\n'). False, with `error` set to a kError* code, when it is
// not a header of that kind. ParseRequest checks the payload size (`payloadBytes`) against the
// sizes and keeps `request.id` once read, so that an error answer can carry it.
bool ParseRequest(std::string_view header, uint64_t payloadBytes, Request& request, std::string& error);
bool ParseResponse(std::string_view header, Response& response, std::string& error);
bool ParseHandshake(std::string_view header, Handshake& handshake, std::string& error);

// The number of Unicode code points of valid UTF-8 `text`.
size_t CodePoints(std::string_view text);

} // namespace RayoPdf::OcrProtocol
