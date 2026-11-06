// SPDX-License-Identifier: Apache-2.0
// The general-purpose stream filters of ISO 32000-1 section 7.4 and the predictors of 7.4.4.4
// (algorithms §1.3). The PNG row filters are forked from RayoMD src/core/png_filter.cpp at ef106ca
// (UnfilterBytes, Paeth; Apache-2.0, same author), generalized to any bytes per pixel and to
// unfiltering in place.
#include "pdf/filters.h"

#include "pdf/inflate.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace RayoPdf::Filters {
namespace {

// A Flate stream inflating more than this many times its size is a bomb; past it the output is
// cut as at the limit (algorithms §1.3, §1.5).
constexpr size_t kMaxFlateRatio = 1000;
constexpr size_t kFlateAllowance = size_t(64) << 10;
// Predictor rows longer than this are no image a PDF holds.
constexpr size_t kMaxRowBytes = size_t(1) << 28;

bool IsWhite(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }

int HexDigit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t SaturatingAdd(size_t a, size_t b) { return a > SIZE_MAX - b ? SIZE_MAX : a + b; }
size_t SaturatingMul(size_t a, size_t b) { return b != 0 && a > SIZE_MAX / b ? SIZE_MAX : a * b; }

// The status of a decoder that stopped at bad data: what came before is kept when there is any.
StreamStatus Stopped(const std::string& out) { return out.empty() ? StreamStatus::Corrupt : StreamStatus::Salvaged; }

StreamStatus Ascii85(std::string_view in, std::string& out, size_t limit) {
    out.clear();
    size_t i = 0;
    // A leading "<~" (the PostScript form) is no data.
    while (i < in.size() && IsWhite(static_cast<unsigned char>(in[i]))) i++;
    if (in.size() - i >= 2 && in[i] == '<' && in[i + 1] == '~') i += 2;
    uint32_t group = 0;
    int count = 0;
    bool bad = false;
    bool more = false;  // output past the limit
    for (; i < in.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (IsWhite(c)) continue;
        if (c == '~') break;  // "~>", or a lone '~' at the end
        if (c == 'z' && count == 0) {
            if (out.size() + 4 > limit) {
                more = true;
                break;
            }
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') {
            bad = true;
            break;
        }
        const uint64_t next = uint64_t(group) * 85 + (c - '!');
        if (next > UINT32_MAX) {
            bad = true;
            break;
        }
        group = static_cast<uint32_t>(next);
        if (++count == 5) {
            if (out.size() + 4 > limit) {
                more = true;
                break;
            }
            const char bytes[4] = {static_cast<char>(group >> 24), static_cast<char>(group >> 16),
                                   static_cast<char>(group >> 8), static_cast<char>(group)};
            out.append(bytes, 4);
            group = 0;
            count = 0;
        }
    }
    if (more) {
        out.resize(std::min(out.size(), limit));
        return StreamStatus::TooLarge;
    }
    if (!bad && count == 1) bad = true;  // one character cannot end a group
    if (!bad && count > 1) {
        // A final partial group of n characters is padded with 'u' and gives n - 1 bytes.
        uint64_t value = group;
        for (int k = count; k < 5; k++) value = value * 85 + 84;
        if (value > UINT32_MAX) {
            bad = true;
        } else {
            for (int k = 0; k < count - 1; k++) {
                if (out.size() >= limit) return StreamStatus::TooLarge;
                out.push_back(static_cast<char>(value >> (24 - 8 * k)));
            }
        }
    }
    return bad ? Stopped(out) : StreamStatus::Ok;
}

StreamStatus AsciiHex(std::string_view in, std::string& out, size_t limit) {
    out.clear();
    out.reserve(std::min(limit, in.size() / 2 + 1));
    int high = -1;
    for (size_t i = 0; i < in.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '>') break;
        if (IsWhite(c)) continue;
        const int d = HexDigit(c);
        if (d < 0) return Stopped(out);
        if (high < 0) {
            high = d;
            continue;
        }
        if (out.size() >= limit) return StreamStatus::TooLarge;
        out.push_back(static_cast<char>(high << 4 | d));
        high = -1;
    }
    if (high >= 0) {
        // An odd final digit is followed by 0.
        if (out.size() >= limit) return StreamStatus::TooLarge;
        out.push_back(static_cast<char>(high << 4));
    }
    return StreamStatus::Ok;
}

StreamStatus RunLength(std::string_view in, std::string& out, size_t limit) {
    out.clear();
    size_t i = 0;
    while (i < in.size()) {
        const unsigned length = static_cast<unsigned char>(in[i++]);
        if (length == 128) return StreamStatus::Ok;
        if (length < 128) {
            size_t n = length + 1;
            const bool truncated = n > in.size() - i;
            if (truncated) n = in.size() - i;
            if (out.size() + n > limit) {
                out.append(in.data() + i, limit - out.size());
                return StreamStatus::TooLarge;
            }
            out.append(in.data() + i, n);
            i += n;
            if (truncated) return Stopped(out);
        } else {
            if (i >= in.size()) return Stopped(out);
            const size_t n = 257 - length;
            if (out.size() + n > limit) {
                out.append(limit - out.size(), in[i]);
                return StreamStatus::TooLarge;
            }
            out.append(n, in[i++]);
        }
    }
    return StreamStatus::Ok;  // no end-of-data byte: the data ends with the stream
}

// LZW with 9 to 12-bit codes (ISO 32000-1 section 7.4.4.2): 256 clears the table, 257 ends the
// data; with EarlyChange 1 the code width grows one code early.
StreamStatus Lzw(std::string_view in, std::string& out, size_t limit, int earlyChange) {
    out.clear();
    struct Code {
        uint16_t prefix;
        uint16_t length;
        uint8_t last;
        uint8_t first;
    };
    std::vector<Code> table(4096);
    for (unsigned c = 0; c < 256; c++) table[c] = {0, 1, static_cast<uint8_t>(c), static_cast<uint8_t>(c)};
    const unsigned early = earlyChange == 0 ? 0 : 1;
    unsigned next = 258;
    unsigned width = 9;
    int previous = -1;
    uint32_t bits = 0;
    unsigned have = 0;
    size_t i = 0;
    for (;;) {
        while (have < width && i < in.size()) {
            bits = (bits << 8) | static_cast<unsigned char>(in[i++]);
            have += 8;
        }
        if (have < width) break;  // no end-of-data code: the data ends with the stream
        const unsigned code = (bits >> (have - width)) & ((1u << width) - 1);
        have -= width;
        if (code == 256) {
            next = 258;
            width = 9;
            previous = -1;
            continue;
        }
        if (code == 257) break;
        if (code > next || (code == next && previous < 0)) {
            return Stopped(out);
        }
        // The string of `code`; for the code being defined, the previous string plus its first byte.
        const bool defining = code == next;
        const unsigned source = defining ? static_cast<unsigned>(previous) : code;
        const size_t length = table[source].length + (defining ? 1 : 0);
        const size_t at = out.size();
        out.resize(at + length);
        char* const p = &out[0] + at;
        size_t k = table[source].length;
        if (defining) p[k] = static_cast<char>(table[source].first);
        for (unsigned c = source; k > 0; c = table[c].prefix) p[--k] = static_cast<char>(table[c].last);
        if (out.size() > limit) {
            out.resize(limit);
            return StreamStatus::TooLarge;
        }
        if (previous >= 0 && next < 4096) {
            const uint8_t first = static_cast<uint8_t>(p[0]);
            table[next] = {static_cast<uint16_t>(previous), static_cast<uint16_t>(table[previous].length + 1), first,
                           table[previous].first};
            next++;
        }
        previous = static_cast<int>(code);
        if (next + early >= (1u << width) && width < 12) width++;
    }
    return StreamStatus::Ok;
}

// The Paeth predictor (whichever of a, b, c is nearest to p = a + b - c, in that order on ties).
inline int Paeth(int a, int b, int c) {
    const int pa = std::abs(b - c);
    const int pb = std::abs(a - c);
    const int pc = std::abs(a + b - 2 * c);
    const int bc = pb <= pc ? b : c;
    return (pa <= pb && pa <= pc) ? a : bc;
}

// One PNG row (filter types 0-4) into `row` from `raw`, which may start at or after `row` in the
// same buffer: each byte is read before the byte under it is written. False for an unknown type.
bool UnfilterBytes(uint8_t filter, const uint8_t* raw, const uint8_t* up, size_t rowBytes, size_t bpp, uint8_t* row) {
    const size_t lead = std::min(bpp, rowBytes);
    switch (filter) {
        case 0:
            std::memmove(row, raw, rowBytes);
            return true;
        case 1:
            std::memmove(row, raw, lead);
            for (size_t x = lead; x < rowBytes; x++) row[x] = static_cast<uint8_t>(raw[x] + row[x - bpp]);
            return true;
        case 2:
            for (size_t x = 0; x < rowBytes; x++) row[x] = static_cast<uint8_t>(raw[x] + up[x]);
            return true;
        case 3:
            for (size_t x = 0; x < lead; x++) row[x] = static_cast<uint8_t>(raw[x] + (up[x] >> 1));
            for (size_t x = lead; x < rowBytes; x++) {
                row[x] = static_cast<uint8_t>(raw[x] + ((row[x - bpp] + up[x]) >> 1));
            }
            return true;
        case 4:
            for (size_t x = 0; x < lead; x++) row[x] = static_cast<uint8_t>(raw[x] + up[x]);
            for (size_t x = lead; x < rowBytes; x++) {
                row[x] = static_cast<uint8_t>(raw[x] + Paeth(row[x - bpp], up[x], up[x - bpp]));
            }
            return true;
        default:
            return false;
    }
}

// PNG predictors 10-15: every row has its own filter type byte. In place.
StreamStatus UnpredictPng(std::string& data, size_t rowBytes, size_t bpp) {
    uint8_t* const base = reinterpret_cast<uint8_t*>(&data[0]);
    const size_t size = data.size();
    std::vector<uint8_t> zeros(rowBytes, 0);
    const uint8_t* up = zeros.data();
    size_t in = 0;
    size_t out = 0;
    StreamStatus status = StreamStatus::Ok;
    while (in < size) {
        const uint8_t filter = base[in];
        size_t n = rowBytes;
        if (size - in - 1 < rowBytes) {
            // A short last row is kept as far as it goes.
            n = size - in - 1;
            status = StreamStatus::Salvaged;
            if (n == 0) break;
        }
        uint8_t* const row = base + out;
        if (!UnfilterBytes(filter, base + in + 1, up, n, bpp, row)) {
            // An unknown filter type: the row as it stands.
            std::memmove(row, base + in + 1, n);
            status = StreamStatus::Salvaged;
        }
        up = row;
        in += 1 + n;
        out += n;
        if (n < rowBytes) break;
    }
    data.resize(out);
    return status;
}

// TIFF predictor 2: every sample is the difference to the same component of the pixel before.
StreamStatus UnpredictTiff(std::string& data, size_t rowBytes, int colors, int bpc, int columns) {
    uint8_t* const base = reinterpret_cast<uint8_t*>(&data[0]);
    const size_t size = data.size();
    const StreamStatus status = size % rowBytes == 0 ? StreamStatus::Ok : StreamStatus::Salvaged;
    for (size_t start = 0; start < size; start += rowBytes) {
        uint8_t* const row = base + start;
        const size_t n = std::min(rowBytes, size - start);
        if (bpc == 8) {
            for (size_t x = static_cast<size_t>(colors); x < n; x++) {
                row[x] = static_cast<uint8_t>(row[x] + row[x - colors]);
            }
        } else if (bpc == 16) {
            const size_t step = static_cast<size_t>(colors) * 2;
            for (size_t x = step; x + 1 < n; x += 2) {
                const unsigned sum = ((row[x] << 8) | row[x + 1]) + ((row[x - step] << 8) | row[x - step + 1]);
                row[x] = static_cast<uint8_t>(sum >> 8);
                row[x + 1] = static_cast<uint8_t>(sum);
            }
        } else {
            // 1, 2 or 4 bits a sample, most significant first.
            const size_t samples = std::min(static_cast<size_t>(colors) * static_cast<size_t>(columns), n * 8 / bpc);
            const unsigned mask = (1u << bpc) - 1;
            const auto get = [&](size_t s) {
                const size_t bit = s * bpc;
                return (row[bit >> 3] >> (8 - bpc - (bit & 7))) & mask;
            };
            for (size_t s = static_cast<size_t>(colors); s < samples; s++) {
                const unsigned value = (get(s) + get(s - colors)) & mask;
                const size_t bit = s * bpc;
                const unsigned shift = 8 - bpc - (bit & 7);
                row[bit >> 3] = static_cast<uint8_t>((row[bit >> 3] & ~(mask << shift)) | (value << shift));
            }
        }
    }
    return status;
}

// The bytes of one row (without a PNG filter byte) and of one pixel; false for a shape no image has.
bool RowShape(const Params& params, size_t& rowBytes, size_t& bpp) {
    const int bpc = params.bitsPerComponent;
    if (params.colors < 1 || params.colors > 32 || params.columns < 1 ||
        (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)) {
        return false;
    }
    const size_t bitsPerPixel = static_cast<size_t>(params.colors) * static_cast<size_t>(bpc);
    const size_t bits = SaturatingMul(bitsPerPixel, static_cast<size_t>(params.columns));
    if (bits / 8 > kMaxRowBytes) return false;
    rowBytes = (bits + 7) / 8;
    bpp = std::max<size_t>(1, (bitsPerPixel + 7) / 8);
    return true;
}

StreamStatus Worse(StreamStatus a, StreamStatus b) {
    const auto rank = [](StreamStatus s) {
        switch (s) {
            case StreamStatus::Ok: return 0;
            case StreamStatus::Salvaged: return 1;
            case StreamStatus::Unsupported: return 2;
            case StreamStatus::TooLarge: return 3;
            case StreamStatus::Corrupt: return 4;
            case StreamStatus::NotStream: return 5;
        }
        return 5;
    };
    return rank(a) >= rank(b) ? a : b;
}

} // namespace

Kind KindOf(std::string_view name) {
    if (name == "FlateDecode" || name == "Fl") return Kind::Flate;
    if (name == "LZWDecode" || name == "LZW") return Kind::Lzw;
    if (name == "ASCII85Decode" || name == "A85") return Kind::Ascii85;
    if (name == "ASCIIHexDecode" || name == "AHx") return Kind::AsciiHex;
    if (name == "RunLengthDecode" || name == "RL") return Kind::RunLength;
    if (name == "Crypt") return Kind::Crypt;
    if (name == "DCTDecode" || name == "DCT") return Kind::Dct;
    if (name == "JPXDecode") return Kind::Jpx;
    if (name == "JBIG2Decode") return Kind::Jbig2;
    if (name == "CCITTFaxDecode" || name == "CCF") return Kind::Ccitt;
    if (name == "BrotliDecode") return Kind::Brotli;
    return Kind::Unknown;
}

bool IsImageFilter(Kind kind) {
    return kind == Kind::Dct || kind == Kind::Jpx || kind == Kind::Jbig2 || kind == Kind::Ccitt;
}

StreamStatus Decode(Kind kind, const Params& params, std::string_view in, std::string& out, size_t limit) {
    switch (kind) {
        case Kind::Ascii85: return Ascii85(in, out, limit);
        case Kind::AsciiHex: return AsciiHex(in, out, limit);
        case Kind::RunLength: return RunLength(in, out, limit);
        case Kind::Flate:
        case Kind::Lzw: break;
        default:
            out.assign(in.data(), in.size());
            return StreamStatus::Unsupported;
    }
    // Predictor 2 is TIFF, 10 and above PNG; anything else none.
    const int predictor = params.predictor;
    const bool png = predictor >= 10;
    const bool tiff = predictor == 2;
    size_t rowBytes = 0;
    size_t bpp = 1;
    if ((png || tiff) && !RowShape(params, rowBytes, bpp)) {
        out.clear();
        return StreamStatus::Corrupt;
    }
    // PNG rows carry a filter byte each: the data before the predictor may be that much longer.
    size_t decodeLimit = limit;
    if (png) decodeLimit = SaturatingAdd(SaturatingAdd(limit, limit / rowBytes), rowBytes + 1);
    StreamStatus status = StreamStatus::Ok;
    if (kind == Kind::Flate) {
        const size_t bomb = SaturatingAdd(SaturatingMul(in.size(), kMaxFlateRatio), kFlateAllowance);
        switch (Inflate::Inflate(in, out, std::min(decodeLimit, bomb))) {
            case Inflate::Result::Ok: break;
            case Inflate::Result::Salvaged: status = StreamStatus::Salvaged; break;
            case Inflate::Result::TooLarge: status = StreamStatus::TooLarge; break;
            case Inflate::Result::Corrupt: return StreamStatus::Corrupt;
        }
    } else {
        status = Lzw(in, out, decodeLimit, params.earlyChange);
        if (status == StreamStatus::Corrupt) return status;
    }
    if (png) status = Worse(status, UnpredictPng(out, rowBytes, bpp));
    if (tiff && !out.empty()) {
        status = Worse(status, UnpredictTiff(out, rowBytes, params.colors, params.bitsPerComponent, params.columns));
    }
    if (out.size() > limit) {
        out.resize(limit);
        status = StreamStatus::TooLarge;
    }
    return status;
}

} // namespace RayoPdf::Filters
