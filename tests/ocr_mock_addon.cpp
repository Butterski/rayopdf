// SPDX-License-Identifier: Apache-2.0
// A stand-in for the OCR add-on `rayopdf-ocr --serve` (docs/development/ocr-protocol.md) for
// tests/test_layout_ocr.cpp: it speaks protocol 1 through common/ocr_protocol.h and "recognises"
// the bands of dark rows of a page image as lines "Mock line K" (K counted from the top), answered
// bottom line first so that the core's ordering is tested. The environment variable
// RAYOPDF_OCR_MOCK_MODE makes it misbehave:
//
//   ok (default)  as above; op lines answers "word" for each line image
//   glyphs        op lines reads glyphs drawn as vertical bars: ink columns closer than 8 pixels
//                 form one glyph, read by its bars as "a" (1), "b" (2), "c" (3), "fi" (4, two
//                 characters, a ligature) or nothing (5 or more); conf 0.95
//   crash         exits without answering the first page request
//   crash-start   exits before the handshake
//   hang          never answers a page request
//   hang-start    never sends the handshake
//   garbage       answers a page request with bytes that are no frame
//   bad-json      answers with a frame whose header is not JSON
//   wrong-id      answers with the id of another request
//   error         answers every page request with status "error" (engine-failed)
//   bad-handshake announces protocol 2
//   low-conf      answers lines with confidence 0.3 (op lines read as in mode glyphs)
//
// The answer depends only on the request's pixels (never on its id or on earlier requests).
#include "common/ocr_protocol.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

using namespace RayoPdf::OcrProtocol;

namespace {

size_t ReadInput(void*, char* buffer, size_t size) {
#ifdef _WIN32
    const int n = _read(0, buffer, static_cast<unsigned>(size > (1u << 20) ? (1u << 20) : size));
#else
    const ssize_t n = read(0, buffer, size);
#endif
    return n > 0 ? static_cast<size_t>(n) : 0;
}

void WriteOutput(std::string_view data) {
    while (!data.empty()) {
#ifdef _WIN32
        const int n = _write(1, data.data(), static_cast<unsigned>(data.size() > (1u << 20) ? (1u << 20) : data.size()));
#else
        const ssize_t n = write(1, data.data(), data.size());
#endif
        if (n <= 0) std::exit(4);
        data.remove_prefix(static_cast<size_t>(n));
    }
}

void SleepSeconds(int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
#ifdef _WIN32
        Sleep(100);
#else
        const timespec step{0, 100000000};
        nanosleep(&step, nullptr);
#endif
    }
}

// Bands of rows holding a pixel darker than 128, each a line spanning its dark columns.
void Recognise(const Request& request, std::string_view pixels, uint16_t conf, Response& response) {
    const uint32_t w = request.width, h = request.height;
    uint32_t start = 0, x0 = 0, x1 = 0;
    bool inBand = false;
    int count = 0;
    Response top;  // lines from the top
    for (uint32_t y = 0; y <= h; ++y) {
        uint32_t first = w, last = 0;
        if (y < h) {
            for (uint32_t x = 0; x < w; ++x) {
                if (static_cast<unsigned char>(pixels[size_t(y) * w + x]) < 128) {
                    if (first == w) first = x;
                    last = x + 1;
                }
            }
        }
        if (first < w) {
            if (!inBand) inBand = true, start = y, x0 = first, x1 = last;
            x0 = first < x0 ? first : x0;
            x1 = last > x1 ? last : x1;
            continue;
        }
        if (!inBand) continue;
        inBand = false;
        Line line;
        line.x0 = static_cast<int32_t>(x0), line.y0 = static_cast<int32_t>(start);
        line.x1 = static_cast<int32_t>(x1), line.y1 = static_cast<int32_t>(y);
        // Texts of different lengths: the even character spans of one box then differ from line
        // to line, as a real page's words do (no columns that look like a table).
        ++count;
        line.text = "Mock line " + std::to_string(count) + (count % 2 ? " of the scanned page" : " reads on");
        line.conf = conf;
        const size_t n = CodePoints(line.text);
        for (size_t k = 0; k < n; ++k) {
            CharSpan span;
            span.x0 = static_cast<int32_t>(x0 + (x1 - x0) * k / n);
            span.x1 = static_cast<int32_t>(x0 + (x1 - x0) * (k + 1) / n);
            line.chars.push_back(span);
        }
        top.lines.push_back(line);
    }
    for (size_t i = top.lines.size(); i > 0; --i) response.lines.push_back(top.lines[i - 1]);
}

// A line image of bar glyphs (mode glyphs): each group of ink columns read by its bars.
void ReadBars(const LineSize& size, std::string_view pixels, uint16_t conf, Line& line) {
    const uint32_t w = size.width, h = size.height;
    std::vector<bool> ink(w, false);
    for (uint32_t x = 0; x < w; ++x)
        for (uint32_t y = 0; y < h && !ink[x]; ++y) ink[x] = static_cast<unsigned char>(pixels[size_t(y) * w + x]) < 128;
    line.x1 = static_cast<int32_t>(w), line.y1 = static_cast<int32_t>(h);
    line.conf = conf;
    uint32_t x = 0;
    while (x < w) {
        if (!ink[x]) {
            ++x;
            continue;
        }
        const uint32_t start = x;
        uint32_t bars = 0, end = x, blank = 0;
        for (; x < w && blank < 8; ++x) {
            if (ink[x]) {
                if (x == start || !ink[x - 1]) ++bars;
                end = x + 1, blank = 0;
            } else {
                ++blank;
            }
        }
        x = end;
        const char* text = bars == 1 ? "a" : bars == 2 ? "b" : bars == 3 ? "c" : bars == 4 ? "fi" : "";
        const int32_t n = static_cast<int32_t>(std::strlen(text));
        for (int32_t k = 0; k < n; ++k) {
            line.text += text[k];
            line.chars.push_back({int32_t(start) + int32_t(end - start) * k / n, int32_t(start) + int32_t(end - start) * (k + 1) / n});
        }
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
#endif
    if (argc < 2 || std::strcmp(argv[1], "--serve") != 0) return 2;
    const char* value = std::getenv("RAYOPDF_OCR_MOCK_MODE");
    const std::string mode = value ? value : "ok";
    if (mode == "crash-start") return 3;
    if (mode == "hang-start") SleepSeconds(600);

    Handshake hello;
    hello.version = "0.0.0-mock";
    hello.protocol = mode == "bad-handshake" ? 2 : kProtocol;
    hello.engine = "mock";
    hello.tier = "none";
    hello.runtime = "none";
    hello.models.emplace_back("det", std::string(64, '0'));
    hello.scripts.push_back("latin");
    std::string out;
    WriteHandshake(hello, out);
    WriteOutput(out);

    Frame frame;
    Request request;
    std::string error;
    for (;;) {
        if (ReadFrame(ReadInput, nullptr, frame) != FrameStatus::Ok) return 0;
        Response response;
        if (!ParseRequest(frame.header, frame.payload.size(), request, error)) {
            response.id = request.id;
            response.ok = false;
            response.error = error;
            WriteResponse(response, out);
            WriteOutput(out);
            continue;
        }
        response.id = request.id;
        if (request.op == Op::Quit) {
            WriteResponse(response, out);
            WriteOutput(out);
            return 0;
        }
        if (request.op == Op::Lines) {
            size_t at = 0;
            for (const LineSize& size : request.lines) {
                Line line;
                if (mode == "glyphs" || mode == "low-conf") {
                    ReadBars(size, std::string_view(frame.payload).substr(at, size_t(size.width) * size.height),
                             mode == "glyphs" ? 9500 : 3000, line);
                } else {
                    line.x1 = static_cast<int32_t>(size.width), line.y1 = static_cast<int32_t>(size.height);
                    line.text = "word";
                    line.conf = kConfScale;
                    for (int32_t k = 0; k < 4; ++k) line.chars.push_back({line.x1 * k / 4, line.x1 * (k + 1) / 4});
                }
                at += size_t(size.width) * size.height;
                response.lines.push_back(line);
            }
        } else if (mode == "crash") {
            return 5;
        } else if (mode == "hang") {
            SleepSeconds(600);
            return 0;
        } else if (mode == "garbage") {
            WriteOutput(std::string_view("\x07\x00\x00\x00garbage!!", 13));
            continue;
        } else if (mode == "bad-json") {
            WriteOutput(std::string_view("\x06\x00\x00\x00{nope\n", 10));
            continue;
        } else if (mode == "error") {
            response.ok = false;
            response.error = kErrorEngineFailed;
        } else {
            if (mode == "wrong-id") response.id = request.id + 7;
            Recognise(request, frame.payload, mode == "low-conf" ? 3000 : 9000, response);
        }
        WriteResponse(response, out);
        WriteOutput(out);
    }
}
