// SPDX-License-Identifier: Apache-2.0
// Derived from RayoMD src/cli/serve_request.cpp at ef106ca (Apache-2.0, same author).
#include "common/serve.h"

#include "common/utf8.h"

#include <cstdint>
#include <cstring>

namespace RayoPdf::Serve {

namespace {

enum Field : unsigned { kId, kInput, kOutput, kArgs, kFieldCount };

constexpr const char* kFieldNames[kFieldCount] = {"id", "input", "output", "args"};

// A forward cursor over the line: every byte is read once, and no value nests.
struct Reader {
    std::string_view text;
    size_t at;
    std::string& error;

    bool Fail(const char* what, std::string_view detail = {}) {
        error = what;
        if (!detail.empty()) {
            error += " \"";
            error.append(detail.data(), detail.size() > 64 ? 64 : detail.size());
            error += '"';
        }
        error += " at byte " + std::to_string(at);
        return false;
    }
    void SkipSpace() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
    }
    // Skips white space, then takes `c` when it comes next.
    bool Take(char c) {
        SkipSpace();
        if (at < text.size() && text[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    bool Next(char c) const { return at < text.size() && text[at] == c; }

    bool Hex4(uint32_t& value) {
        if (text.size() - at < 4) return false;
        uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[at + static_cast<size_t>(i)];
            result <<= 4;
            if (c >= '0' && c <= '9') result |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') result |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') result |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        at += 4;
        value = result;
        return true;
    }

    // The string after its opening quote into `out`: escapes decoded, surrogate pairs joined, raw
    // bytes kept only when they are UTF-8.
    bool String(std::string& out) {
        out.clear();
        for (;;) {
            const size_t run = at;
            while (at < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[at]);
                if (c == '"' || c == '\\' || c < 0x20) break;
                if (c < 0x80) {
                    ++at;
                    continue;
                }
                uint32_t codePoint = 0;
                size_t length = 0;
                Utf8::Decode(text, at, codePoint, length);
                if (length == 1) return Fail("invalid UTF-8");
                at += length;
            }
            out.append(text.data() + run, at - run);
            if (at == text.size()) return Fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(text[at]);
            if (c == '"') {
                ++at;
                return true;
            }
            if (c < 0x20) return Fail("control character in a string");
            if (++at == text.size()) return Fail("unterminated string");
            const char escape = text[at++];
            switch (escape) {
                case '"': case '\\': case '/': out.push_back(escape); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t codePoint = 0;
                    if (!Hex4(codePoint)) return Fail("bad \\u escape");
                    if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) return Fail("unpaired surrogate");
                    if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                        uint32_t low = 0;
                        if (text.size() - at < 2 || text[at] != '\\' || text[at + 1] != 'u') {
                            return Fail("unpaired surrogate");
                        }
                        at += 2;
                        if (!Hex4(low)) return Fail("bad \\u escape");
                        if (low < 0xDC00 || low > 0xDFFF) return Fail("unpaired surrogate");
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    Utf8::Append(out, codePoint);
                    break;
                }
                default:
                    --at;
                    return Fail("bad escape");
            }
        }
    }

    // An integer as JSON writes it (-?(0|[1-9][0-9]*)), at most 18 digits; false for a fraction or
    // an exponent too.
    bool Integer() {
        if (Next('-')) ++at;
        const size_t digits = at;
        if (Next('0')) {
            ++at;
        } else {
            if (at == text.size() || text[at] < '1' || text[at] > '9') return false;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
        }
        if (at - digits > 18) return false;
        return !(Next('.') || Next('e') || Next('E') || (at < text.size() && text[at] >= '0' && text[at] <= '9'));
    }
};

bool HasNul(const std::string& text) {
    return text.find('\0') != std::string::npos;
}

} // namespace

bool ParseRequest(std::string_view line, Request& request, std::string& error) {
    request.id.clear();
    request.input.clear();
    request.output.clear();
    request.args.clear();
    request.hasOutput = false;

    Reader reader{line, 0, error};
    if (!reader.Take('{')) return reader.Fail("expected a JSON object");
    unsigned seen = 0;
    std::string key;
    if (!reader.Take('}')) {
        for (;;) {
            if (!reader.Take('"')) return reader.Fail("expected a field name");
            if (!reader.String(key)) return false;
            if (!reader.Take(':')) return reader.Fail("expected ':'");
            unsigned field = 0;
            while (field < kFieldCount && key != kFieldNames[field]) ++field;
            if (field == kFieldCount) return reader.Fail("unknown field", key);
            if (seen & (1u << field)) return reader.Fail("duplicate field", key);
            seen |= 1u << field;
            reader.SkipSpace();
            const size_t valueStart = reader.at;
            if (field == kId) {
                if (reader.Next('"')) {
                    ++reader.at;
                    if (!reader.String(key)) return false;
                } else if (!reader.Integer()) {
                    reader.at = valueStart;
                    return reader.Fail("id must be a string or an integer");
                }
                // Valid JSON as it was read: the answer carries it back as it came.
                request.id.assign(line.data() + valueStart, reader.at - valueStart);
            } else if (field == kArgs) {
                if (!reader.Take('[')) return reader.Fail("args must be an array of strings");
                if (!reader.Take(']')) {
                    for (;;) {
                        if (request.args.size() == kMaxArgs) return reader.Fail("too many args");
                        if (!reader.Take('"')) return reader.Fail("args must be an array of strings");
                        request.args.emplace_back();
                        if (!reader.String(request.args.back())) return false;
                        if (reader.Take(',')) continue;
                        if (reader.Take(']')) break;
                        return reader.Fail("expected ',' or ']'");
                    }
                }
            } else {
                if (!reader.Next('"')) {
                    return reader.Fail(field == kInput ? "input must be a string" : "output must be a string");
                }
                ++reader.at;
                if (!reader.String(field == kInput ? request.input : request.output)) return false;
            }
            if (reader.Take(',')) continue;
            if (reader.Take('}')) break;
            return reader.Fail("expected ',' or '}'");
        }
    }
    reader.SkipSpace();
    if (reader.at != line.size()) return reader.Fail("text after the request");

    request.hasOutput = (seen & (1u << kOutput)) != 0;
    if (!(seen & (1u << kInput))) {
        error = "a request needs input";
        return false;
    }
    if (request.input.empty()) {
        error = "input is empty";
        return false;
    }
    if (request.hasOutput && request.output.empty()) {
        error = "output is empty";
        return false;
    }
    if (HasNul(request.input) || HasNul(request.output)) {
        error = "a path contains a NUL character";
        return false;
    }
    for (const std::string& arg : request.args) {
        if (HasNul(arg)) {
            error = "an argument contains a NUL character";
            return false;
        }
    }
    return true;
}

LineReader::Status LineReader::Next(std::string& line, size_t limit) {
    line.clear();
    bool any = false;
    bool tooLong = false;
    for (;;) {
        if (begin_ == end_) {
            const size_t got = read_(context_, buffer_, sizeof(buffer_));
            if (got == 0) break;
            begin_ = 0;
            end_ = got > sizeof(buffer_) ? sizeof(buffer_) : got;
        }
        any = true;
        const char* start = buffer_ + begin_;
        const size_t available = end_ - begin_;
        const void* found = std::memchr(start, '\n', available);
        const size_t take = found ? static_cast<size_t>(static_cast<const char*>(found) - start) : available;
        if (!tooLong) {
            // One byte more than the limit: the '\r' of a CRLF line.
            if (line.size() + take > limit + 1) {
                tooLong = true;
                line.clear();
            } else {
                line.append(start, take);
            }
        }
        begin_ += take;
        if (found) {
            ++begin_;
            break;
        }
    }
    if (!any) return Status::End;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (tooLong || line.size() > limit) {
        line.clear();
        return Status::TooLong;
    }
    return Status::Line;
}

} // namespace RayoPdf::Serve
