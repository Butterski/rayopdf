// SPDX-License-Identifier: Apache-2.0
#include "common/ocr_protocol.h"

#include <cstring>

namespace RayoPdf::OcrProtocol {

namespace {

// ---- Reading JSON ----

// A cursor over one header. Every function consumes what it reads and returns false on anything
// malformed; nesting is counted in `depth`.
struct Cursor {
    const char* p;
    const char* end;
    int depth = 0;

    void SkipSpace() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool Take(char c) {
        SkipSpace();
        if (p == end || *p != c) return false;
        ++p;
        return true;
    }
    bool Peek(char c) {
        SkipSpace();
        return p < end && *p == c;
    }
};

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool Hex4(Cursor& c, uint32_t& out) {
    if (c.end - c.p < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
        const int d = HexDigit(c.p[i]);
        if (d < 0) return false;
        out = (out << 4) | static_cast<uint32_t>(d);
    }
    c.p += 4;
    return true;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The length of the valid UTF-8 sequence at `p` (1 to 4), or 0 when it is not one (overlong forms,
// surrogates and code points past U+10FFFF are not).
size_t Utf8Sequence(const unsigned char* p, const unsigned char* end) {
    const unsigned char b = p[0];
    if (b < 0x80) return 1;
    size_t n;
    uint32_t cp;
    if (b >= 0xC2 && b <= 0xDF) {
        n = 2, cp = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
        n = 3, cp = b & 0x0F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        n = 4, cp = b & 0x07;
    } else {
        return 0;
    }
    if (static_cast<size_t>(end - p) < n) return 0;
    for (size_t i = 1; i < n; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

// A JSON string into `out` (UTF-8; escapes decoded; raw bytes must be valid UTF-8).
bool ReadString(Cursor& c, std::string& out) {
    out.clear();
    if (!c.Take('"')) return false;
    while (c.p < c.end) {
        const unsigned char b = static_cast<unsigned char>(*c.p);
        if (b == '"') {
            ++c.p;
            return true;
        }
        if (b < 0x20) return false;
        if (b != '\\') {
            const size_t n = Utf8Sequence(reinterpret_cast<const unsigned char*>(c.p),
                                          reinterpret_cast<const unsigned char*>(c.end));
            if (n == 0) return false;
            out.append(c.p, n);
            c.p += n;
            continue;
        }
        if (++c.p == c.end) return false;
        const char e = *c.p++;
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(c, cp)) return false;
                if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low = 0;
                    if (c.end - c.p < 2 || c.p[0] != '\\' || c.p[1] != 'u') return false;
                    c.p += 2;
                    if (!Hex4(c, low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

// A JSON number's text: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
bool SkipNumber(Cursor& c) {
    c.SkipSpace();
    const char* p = c.p;
    if (p < c.end && *p == '-') ++p;
    if (p == c.end) return false;
    if (*p == '0') {
        ++p;
    } else if (*p >= '1' && *p <= '9') {
        while (p < c.end && *p >= '0' && *p <= '9') ++p;
    } else {
        return false;
    }
    if (p < c.end && *p == '.') {
        ++p;
        const char* digits = p;
        while (p < c.end && *p >= '0' && *p <= '9') ++p;
        if (p == digits) return false;
    }
    if (p < c.end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < c.end && (*p == '+' || *p == '-')) ++p;
        const char* digits = p;
        while (p < c.end && *p >= '0' && *p <= '9') ++p;
        if (p == digits) return false;
    }
    c.p = p;
    return true;
}

// An integer with no fraction or exponent and a magnitude of at most `max`: its magnitude and sign.
bool ReadMagnitude(Cursor& c, bool allowNegative, uint64_t max, uint64_t& magnitude, bool& negative) {
    c.SkipSpace();
    negative = false;
    if (c.p < c.end && *c.p == '-') {
        if (!allowNegative) return false;
        negative = true;
        ++c.p;
    }
    if (c.p == c.end || *c.p < '0' || *c.p > '9') return false;
    if (*c.p == '0' && c.end - c.p > 1 && c.p[1] >= '0' && c.p[1] <= '9') return false;
    uint64_t value = 0;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        const uint64_t digit = static_cast<uint64_t>(*c.p - '0');
        if (value > (max - digit) / 10) return false;
        value = value * 10 + digit;
        ++c.p;
    }
    if (c.p < c.end && (*c.p == '.' || *c.p == 'e' || *c.p == 'E')) return false;
    magnitude = value;
    return true;
}

bool ReadUnsigned(Cursor& c, uint64_t max, uint64_t& out) {
    bool negative = false;
    return ReadMagnitude(c, false, max, out, negative);
}

bool ReadU32(Cursor& c, uint32_t max, uint32_t& out) {
    uint64_t v = 0;
    if (!ReadUnsigned(c, max, v)) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool ReadCoordinate(Cursor& c, int32_t& out) {
    uint64_t v = 0;
    bool negative = false;
    if (!ReadMagnitude(c, true, static_cast<uint64_t>(kMaxCoordinate), v, negative)) return false;
    out = negative ? -static_cast<int32_t>(v) : static_cast<int32_t>(v);
    return true;
}

// 0 to 1 with any number of decimals, rounded half up to four; no sign, no exponent.
bool ReadConf(Cursor& c, uint16_t& out) {
    c.SkipSpace();
    if (c.p == c.end || (*c.p != '0' && *c.p != '1')) return false;
    const bool one = *c.p++ == '1';
    uint32_t value = one ? kConfScale : 0;
    if (c.p < c.end && *c.p == '.') {
        ++c.p;
        const char* digits = c.p;
        uint32_t fraction = 0;
        int count = 0;
        bool roundUp = false;
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
            const uint32_t digit = static_cast<uint32_t>(*c.p - '0');
            if (one && digit != 0) return false;
            if (count < 4) fraction = fraction * 10 + digit;
            if (count == 4) roundUp = digit >= 5;
            ++count;
            ++c.p;
        }
        if (c.p == digits) return false;
        for (int i = count; i < 4; ++i) fraction *= 10;
        value += fraction + (roundUp ? 1 : 0);
    } else if (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        return false;
    }
    if (c.p < c.end && (*c.p == 'e' || *c.p == 'E')) return false;
    if (value > kConfScale) return false;
    out = static_cast<uint16_t>(value);
    return true;
}

bool SkipValue(Cursor& c);

// The members of an object: `member(key)` reads each value. An empty key is allowed.
template <class Member>
bool ReadObject(Cursor& c, Member&& member) {
    if (!c.Take('{')) return false;
    if (++c.depth > kMaxDepth) return false;
    std::string key;
    if (!c.Take('}')) {
        for (;;) {
            if (!ReadString(c, key) || !c.Take(':') || !member(key)) return false;
            if (c.Take(',')) continue;
            if (!c.Take('}')) return false;
            break;
        }
    }
    --c.depth;
    return true;
}

// The elements of an array: `element(index)` reads each.
template <class Element>
bool ReadArray(Cursor& c, Element&& element) {
    if (!c.Take('[')) return false;
    if (++c.depth > kMaxDepth) return false;
    if (!c.Take(']')) {
        for (size_t index = 0;; ++index) {
            if (!element(index)) return false;
            if (c.Take(',')) continue;
            if (!c.Take(']')) return false;
            break;
        }
    }
    --c.depth;
    return true;
}

bool SkipLiteral(Cursor& c, const char* word) {
    const size_t n = std::strlen(word);
    if (static_cast<size_t>(c.end - c.p) < n || std::memcmp(c.p, word, n) != 0) return false;
    c.p += n;
    return true;
}

bool SkipValue(Cursor& c) {
    c.SkipSpace();
    if (c.p == c.end) return false;
    std::string scratch;
    switch (*c.p) {
        case '{': return ReadObject(c, [&](const std::string&) { return SkipValue(c); });
        case '[': return ReadArray(c, [&](size_t) { return SkipValue(c); });
        case '"': return ReadString(c, scratch);
        case 't': return SkipLiteral(c, "true");
        case 'f': return SkipLiteral(c, "false");
        case 'n': return SkipLiteral(c, "null");
        default: return SkipNumber(c);
    }
}

// A header's cursor: without its line break; after the object only white space may follow.
Cursor HeaderCursor(std::string_view header) {
    if (!header.empty() && header.back() == '\n') header.remove_suffix(1);
    return Cursor{header.data(), header.data() + header.size()};
}

bool AtEnd(Cursor& c) {
    c.SkipSpace();
    return c.p == c.end;
}

// Members seen once: a second one with the same name is malformed.
bool Once(uint32_t& seen, int bit) {
    if (seen & (1u << bit)) return false;
    seen |= 1u << bit;
    return true;
}

// ---- Writing JSON ----

void AppendUnsigned(std::string& out, uint64_t value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0) out.push_back(digits[--n]);
}

void AppendSigned(std::string& out, int64_t value) {
    if (value < 0) {
        out.push_back('-');
        AppendUnsigned(out, static_cast<uint64_t>(-(value + 1)) + 1);
    } else {
        AppendUnsigned(out, static_cast<uint64_t>(value));
    }
}

// `text` as a JSON string: quotes, backslashes and control characters escaped, bytes that are not
// UTF-8 as U+FFFD.
void AppendString(std::string& out, std::string_view text) {
    static const char kHex[] = "0123456789abcdef";
    out.push_back('"');
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
    const unsigned char* end = p + text.size();
    while (p < end) {
        const unsigned char b = *p;
        if (b == '"' || b == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(b));
            ++p;
        } else if (b < 0x20 || b == 0x7F) {
            out += "\\u00";
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 15]);
            ++p;
        } else {
            const size_t n = Utf8Sequence(p, end);
            if (n == 0) {
                out += "\xEF\xBF\xBD";
                ++p;
            } else {
                out.append(reinterpret_cast<const char*>(p), n);
                p += n;
            }
        }
    }
    out.push_back('"');
}

void AppendConf(std::string& out, uint16_t conf) {
    if (conf >= kConfScale) {
        out.push_back('1');
        return;
    }
    out.push_back('0');
    if (conf == 0) return;
    char digits[4];
    uint32_t v = conf;
    for (int i = 3; i >= 0; --i) {
        digits[i] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    int n = 4;
    while (digits[n - 1] == '0') --n;
    out.push_back('.');
    out.append(digits, static_cast<size_t>(n));
}

void Key(std::string& out, const char* key) {
    if (out.size() > 5) out.push_back(',');  // after the u32 and '{'
    out.push_back('"');
    out += key;
    out += "\":";
}

void BeginFrame(std::string& frame) {
    frame.assign(4, '\0');
    frame.push_back('{');
}

// Closes the header and fills in its length.
void EndHeader(std::string& frame) {
    frame += "}\n";
    const uint32_t n = static_cast<uint32_t>(frame.size() - 4);
    frame[0] = static_cast<char>(n & 0xFF);
    frame[1] = static_cast<char>((n >> 8) & 0xFF);
    frame[2] = static_cast<char>((n >> 16) & 0xFF);
    frame[3] = static_cast<char>((n >> 24) & 0xFF);
}

const char* OpName(Op op) {
    switch (op) {
        case Op::Page: return "page";
        case Op::Lines: return "lines";
        case Op::Quit: break;
    }
    return "quit";
}

// Reads exactly `size` bytes; how many arrived before the input ended.
size_t ReadExactly(ReadFunction read, void* context, char* buffer, size_t size) {
    size_t got = 0;
    while (got < size) {
        const size_t n = read(context, buffer + got, size - got);
        if (n == 0 || n > size - got) break;
        got += n;
    }
    return got;
}

} // namespace

// ---- Frames ----

FrameStatus ReadFrame(ReadFunction read, void* context, Frame& frame) {
    frame.header.clear();
    frame.payload.clear();
    unsigned char length[4];
    const size_t got = ReadExactly(read, context, reinterpret_cast<char*>(length), 4);
    if (got == 0) return FrameStatus::End;
    if (got < 4) return FrameStatus::Truncated;
    const uint32_t n = uint32_t(length[0]) | uint32_t(length[1]) << 8 | uint32_t(length[2]) << 16 |
                       uint32_t(length[3]) << 24;
    if (n > kMaxHeaderBytes) return FrameStatus::TooLarge;
    if (n < 2) return FrameStatus::Malformed;
    frame.header.resize(n);
    if (ReadExactly(read, context, &frame.header[0], n) < n) return FrameStatus::Truncated;
    if (frame.header.back() != '\n' || frame.header.find('\n') != n - 1) return FrameStatus::Malformed;

    // The payload's size: the top-level member "payload" of a well-formed object.
    Cursor c = HeaderCursor(frame.header);
    uint64_t payload = 0;
    uint32_t seen = 0;
    const bool ok = ReadObject(c, [&](const std::string& key) {
        if (key == "payload" && c.depth == 1) return Once(seen, 0) && ReadUnsigned(c, kMaxFrameBytes, payload);
        return SkipValue(c);
    });
    if (!ok || !AtEnd(c)) return FrameStatus::Malformed;
    if (payload > kMaxFrameBytes - n) return FrameStatus::TooLarge;

    // Grown with what arrives: a header cannot make the reader allocate ahead of the data.
    constexpr size_t kChunk = size_t(1) << 20;
    while (frame.payload.size() < payload) {
        const size_t want = static_cast<size_t>(
            payload - frame.payload.size() < kChunk ? payload - frame.payload.size() : kChunk);
        const size_t at = frame.payload.size();
        frame.payload.resize(at + want);
        const size_t arrived = ReadExactly(read, context, &frame.payload[at], want);
        if (arrived < want) {
            frame.payload.resize(at + arrived);
            return FrameStatus::Truncated;
        }
    }
    return FrameStatus::Ok;
}

// ---- Writers ----

void WriteRequest(const Request& request, std::string_view payload, std::string& frame) {
    BeginFrame(frame);
    Key(frame, "id");
    AppendUnsigned(frame, request.id);
    Key(frame, "op");
    AppendString(frame, OpName(request.op));
    Key(frame, "width");
    AppendUnsigned(frame, request.width);
    Key(frame, "height");
    AppendUnsigned(frame, request.height);
    Key(frame, "channels");
    AppendUnsigned(frame, request.channels);
    Key(frame, "dpi");
    AppendUnsigned(frame, request.dpi);
    if (request.op == Op::Lines) {
        Key(frame, "lines");
        frame.push_back('[');
        for (size_t i = 0; i < request.lines.size(); ++i) {
            if (i) frame.push_back(',');
            frame += "{\"width\":";
            AppendUnsigned(frame, request.lines[i].width);
            frame += ",\"height\":";
            AppendUnsigned(frame, request.lines[i].height);
            frame.push_back('}');
        }
        frame.push_back(']');
    }
    Key(frame, "payload");
    AppendUnsigned(frame, payload.size());
    EndHeader(frame);
    frame.append(payload.data(), payload.size());
}

void WriteResponse(const Response& response, std::string& frame) {
    BeginFrame(frame);
    Key(frame, "id");
    AppendUnsigned(frame, response.id);
    Key(frame, "status");
    AppendString(frame, response.ok ? "ok" : "error");
    if (!response.ok) {
        Key(frame, "error");
        AppendString(frame, response.error);
        EndHeader(frame);
        return;
    }
    Key(frame, "lines");
    frame.push_back('[');
    for (size_t i = 0; i < response.lines.size(); ++i) {
        const Line& line = response.lines[i];
        if (i) frame.push_back(',');
        frame += "{\"box\":[";
        AppendSigned(frame, line.x0);
        frame.push_back(',');
        AppendSigned(frame, line.y0);
        frame.push_back(',');
        AppendSigned(frame, line.x1);
        frame.push_back(',');
        AppendSigned(frame, line.y1);
        frame += "],\"text\":";
        AppendString(frame, line.text);
        frame += ",\"conf\":";
        AppendConf(frame, line.conf);
        frame += ",\"chars\":[";
        for (size_t k = 0; k < line.chars.size(); ++k) {
            if (k) frame.push_back(',');
            frame.push_back('[');
            AppendSigned(frame, line.chars[k].x0);
            frame.push_back(',');
            AppendSigned(frame, line.chars[k].x1);
            frame.push_back(']');
        }
        frame += "]}";
    }
    frame.push_back(']');
    EndHeader(frame);
}

void WriteHandshake(const Handshake& handshake, std::string& frame) {
    BeginFrame(frame);
    Key(frame, "rayopdf-ocr");
    AppendString(frame, handshake.version);
    Key(frame, "protocol");
    AppendSigned(frame, handshake.protocol);
    Key(frame, "engine");
    AppendString(frame, handshake.engine);
    Key(frame, "tier");
    AppendString(frame, handshake.tier);
    Key(frame, "runtime");
    AppendString(frame, handshake.runtime);
    Key(frame, "models");
    frame.push_back('{');
    for (size_t i = 0; i < handshake.models.size(); ++i) {
        if (i) frame.push_back(',');
        AppendString(frame, handshake.models[i].first);
        frame.push_back(':');
        AppendString(frame, handshake.models[i].second);
    }
    frame.push_back('}');
    Key(frame, "scripts");
    frame.push_back('[');
    for (size_t i = 0; i < handshake.scripts.size(); ++i) {
        if (i) frame.push_back(',');
        AppendString(frame, handshake.scripts[i]);
    }
    frame.push_back(']');
    EndHeader(frame);
}

// ---- Parsers ----

uint64_t RequestPixels(const Request& request) {
    switch (request.op) {
        case Op::Page: return uint64_t(request.width) * request.height;
        case Op::Lines: {
            uint64_t total = 0;
            for (const LineSize& l : request.lines) {
                total += uint64_t(l.width) * l.height;
                if (total > kMaxPixels) return UINT64_MAX;
            }
            return total;
        }
        case Op::Quit: break;
    }
    return 0;
}

bool ParseRequest(std::string_view header, uint64_t payloadBytes, Request& request, std::string& error) {
    request.id = 0;
    request.op = Op::Page;
    request.width = request.height = 0;
    request.channels = 1;
    request.dpi = 0;
    request.lines.clear();
    error = kErrorBadRequest;
    Cursor c = HeaderCursor(header);
    uint32_t seen = 0;
    uint64_t payload = 0;
    bool tooMany = false;
    std::string op;
    const bool ok = ReadObject(c, [&](const std::string& key) {
        if (key == "id") return Once(seen, 0) && ReadUnsigned(c, UINT64_MAX, request.id);
        if (key == "op") {
            if (!Once(seen, 1) || !ReadString(c, op)) return false;
            if (op == "page") {
                request.op = Op::Page;
            } else if (op == "lines") {
                request.op = Op::Lines;
            } else if (op == "quit") {
                request.op = Op::Quit;
            } else {
                return false;
            }
            return true;
        }
        if (key == "width") return Once(seen, 2) && ReadU32(c, kMaxSide, request.width);
        if (key == "height") return Once(seen, 3) && ReadU32(c, kMaxSide, request.height);
        if (key == "channels") return Once(seen, 4) && ReadU32(c, 1, request.channels) && request.channels == 1;
        if (key == "dpi") return Once(seen, 5) && ReadU32(c, kMaxDpi, request.dpi);
        if (key == "payload") return Once(seen, 6) && ReadUnsigned(c, kMaxFrameBytes, payload);
        if (key == "lines") {
            if (!Once(seen, 7)) return false;
            return ReadArray(c, [&](size_t index) {
                if (index >= kMaxLines) {
                    tooMany = true;
                    return false;
                }
                LineSize size;
                uint32_t has = 0;
                const bool read = ReadObject(c, [&](const std::string& k) {
                    if (k == "width") return Once(has, 0) && ReadU32(c, kMaxSide, size.width);
                    if (k == "height") return Once(has, 1) && ReadU32(c, kMaxSide, size.height);
                    return SkipValue(c);
                });
                if (!read || has != 3 || size.width == 0 || size.height == 0) return false;
                request.lines.push_back(size);
                return true;
            });
        }
        return SkipValue(c);
    });
    if (tooMany) error = kErrorTooLarge;
    if (!ok || !AtEnd(c) || !(seen & 1) || !(seen & 2)) return false;
    switch (request.op) {
        case Op::Page:
            if (request.width == 0 || request.height == 0 || !request.lines.empty()) return false;
            break;
        case Op::Lines:
            if (request.lines.empty() || request.width != 0 || request.height != 0) return false;
            break;
        case Op::Quit:
            if (request.width != 0 || request.height != 0 || !request.lines.empty()) return false;
            break;
    }
    const uint64_t pixels = RequestPixels(request);
    if (pixels > kMaxPixels) {
        error = kErrorTooLarge;
        return false;
    }
    if (payload != pixels || payloadBytes != pixels) {
        error = kErrorBadPixels;
        return false;
    }
    error.clear();
    return true;
}

bool ParseResponse(std::string_view header, Response& response, std::string& error) {
    response.id = 0;
    response.ok = true;
    response.error.clear();
    response.lines.clear();
    error = kErrorBadRequest;
    Cursor c = HeaderCursor(header);
    uint32_t seen = 0;
    bool tooMany = false;
    std::string status;
    const bool ok = ReadObject(c, [&](const std::string& key) {
        if (key == "id") return Once(seen, 0) && ReadUnsigned(c, UINT64_MAX, response.id);
        if (key == "status") {
            if (!Once(seen, 1) || !ReadString(c, status)) return false;
            if (status != "ok" && status != "error") return false;
            response.ok = status == "ok";
            return true;
        }
        if (key == "error") return Once(seen, 2) && ReadString(c, response.error);
        if (key == "lines") {
            if (!Once(seen, 3)) return false;
            return ReadArray(c, [&](size_t index) {
                if (index >= kMaxLines) {
                    tooMany = true;
                    return false;
                }
                response.lines.emplace_back();
                Line& line = response.lines.back();
                uint32_t has = 0;
                const bool read = ReadObject(c, [&](const std::string& k) {
                    if (k == "box") {
                        if (!Once(has, 0)) return false;
                        int32_t box[4];
                        size_t count = 0;
                        const bool b = ReadArray(c, [&](size_t i) { return i < 4 && ReadCoordinate(c, box[count++]); });
                        if (!b || count != 4 || box[0] > box[2] || box[1] > box[3]) return false;
                        line.x0 = box[0], line.y0 = box[1], line.x1 = box[2], line.y1 = box[3];
                        return true;
                    }
                    if (k == "text") return Once(has, 1) && ReadString(c, line.text);
                    if (k == "conf") return Once(has, 2) && ReadConf(c, line.conf);
                    if (k == "chars") {
                        if (!Once(has, 3)) return false;
                        return ReadArray(c, [&](size_t) {
                            CharSpan span;
                            size_t count = 0;
                            const bool b = ReadArray(c, [&](size_t i) {
                                if (i >= 2) return false;
                                ++count;
                                return ReadCoordinate(c, i == 0 ? span.x0 : span.x1);
                            });
                            if (!b || count != 2 || span.x0 > span.x1) return false;
                            line.chars.push_back(span);
                            return true;
                        });
                    }
                    return SkipValue(c);
                });
                return read && has == 15 && line.chars.size() == CodePoints(line.text);
            });
        }
        return SkipValue(c);
    });
    if (tooMany) error = kErrorTooLarge;
    if (!ok || !AtEnd(c) || (seen & 3) != 3) return false;
    if (response.ok ? (seen & 4) != 0 : (seen & 4) == 0 || (seen & 8) != 0) return false;
    error.clear();
    return true;
}

bool ParseHandshake(std::string_view header, Handshake& handshake, std::string& error) {
    handshake = Handshake();
    error = kErrorBadRequest;
    Cursor c = HeaderCursor(header);
    uint32_t seen = 0;
    constexpr size_t kMaxEntries = 64;
    const bool ok = ReadObject(c, [&](const std::string& key) {
        if (key == "rayopdf-ocr") return Once(seen, 0) && ReadString(c, handshake.version);
        if (key == "protocol") {
            uint64_t v = 0;
            if (!Once(seen, 1) || !ReadUnsigned(c, 1000000, v)) return false;
            handshake.protocol = static_cast<int>(v);
            return true;
        }
        if (key == "engine") return Once(seen, 2) && ReadString(c, handshake.engine);
        if (key == "tier") return Once(seen, 3) && ReadString(c, handshake.tier);
        if (key == "runtime") return Once(seen, 4) && ReadString(c, handshake.runtime);
        if (key == "models") {
            if (!Once(seen, 5)) return false;
            return ReadObject(c, [&](const std::string& name) {
                if (handshake.models.size() >= kMaxEntries) return false;
                std::string value;
                if (!ReadString(c, value)) return false;
                handshake.models.emplace_back(name, std::move(value));
                return true;
            });
        }
        if (key == "scripts") {
            if (!Once(seen, 6)) return false;
            return ReadArray(c, [&](size_t index) {
                if (index >= kMaxEntries) return false;
                handshake.scripts.emplace_back();
                return ReadString(c, handshake.scripts.back());
            });
        }
        return SkipValue(c);
    });
    if (!ok || !AtEnd(c) || (seen & 3) != 3) return false;
    error.clear();
    return true;
}

size_t CodePoints(std::string_view text) {
    size_t count = 0;
    for (unsigned char b : text) count += (b & 0xC0) != 0x80;
    return count;
}

} // namespace RayoPdf::OcrProtocol
