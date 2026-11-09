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

// RunLength (ISO 32000-1 section 7.4.5), resumable: Run appends output a piece at a time.
class RunLengthReader {
public:
    void Start(std::string_view in, size_t limit) {
        in_ = in;
        limit_ = limit;
        i_ = 0;
        total_ = 0;
        ended_ = false;
        status_ = StreamStatus::Ok;
    }

    // Appends output to `out` until it has grown by at least `chunk` bytes or the data ends;
    // false once it has ended (Status then set).
    bool Run(std::string& out, size_t chunk) {
        const size_t start = out.size();
        while (!ended_ && out.size() - start < chunk) Step(out);
        return !ended_;
    }

    StreamStatus Status() const { return status_; }

private:
    void End(StreamStatus status) {
        ended_ = true;
        status_ = status;
    }

    // One run.
    void Step(std::string& out) {
        if (i_ >= in_.size()) return End(StreamStatus::Ok);  // no end-of-data byte: the data ends with the stream
        const unsigned length = static_cast<unsigned char>(in_[i_++]);
        if (length == 128) return End(StreamStatus::Ok);
        if (length < 128) {
            size_t n = length + 1;
            const bool truncated = n > in_.size() - i_;
            if (truncated) n = in_.size() - i_;
            if (total_ + n > limit_) {
                out.append(in_.data() + i_, limit_ - total_);
                return End(StreamStatus::TooLarge);
            }
            out.append(in_.data() + i_, n);
            total_ += n;
            i_ += n;
            if (truncated) End(Stopped());
            return;
        }
        if (i_ >= in_.size()) return End(Stopped());
        const size_t n = 257 - length;
        if (total_ + n > limit_) {
            out.append(limit_ - total_, in_[i_]);
            return End(StreamStatus::TooLarge);
        }
        out.append(n, in_[i_++]);
        total_ += n;
    }

    // The status of a decoder that stopped at bad data: what came before is kept when there is any.
    StreamStatus Stopped() const { return total_ == 0 ? StreamStatus::Corrupt : StreamStatus::Salvaged; }

    std::string_view in_;
    size_t limit_ = 0;
    size_t i_ = 0;
    size_t total_ = 0;
    bool ended_ = false;
    StreamStatus status_ = StreamStatus::Ok;
};

StreamStatus RunLength(std::string_view in, std::string& out, size_t limit) {
    out.clear();
    RunLengthReader reader;
    reader.Start(in, limit);
    reader.Run(out, SIZE_MAX);
    return reader.Status();
}

// LZW with 9 to 12-bit codes (ISO 32000-1 section 7.4.4.2): 256 clears the table, 257 ends the
// data; with EarlyChange 1 the code width grows one code early. Resumable, as RunLengthReader.
class LzwReader {
public:
    void Start(std::string_view in, size_t limit, int earlyChange) {
        in_ = in;
        limit_ = limit;
        early_ = earlyChange == 0 ? 0 : 1;
        table_.resize(4096);
        for (unsigned c = 0; c < 256; c++) table_[c] = {0, 1, static_cast<uint8_t>(c), static_cast<uint8_t>(c)};
        next_ = 258;
        width_ = 9;
        previous_ = -1;
        bits_ = 0;
        have_ = 0;
        i_ = 0;
        total_ = 0;
        ended_ = false;
        status_ = StreamStatus::Ok;
    }

    bool Run(std::string& out, size_t chunk) {
        const size_t start = out.size();
        while (!ended_ && out.size() - start < chunk) Step(out);
        return !ended_;
    }

    StreamStatus Status() const { return status_; }

private:
    struct Code {
        uint16_t prefix;
        uint16_t length;
        uint8_t last;
        uint8_t first;
    };

    void End(StreamStatus status) {
        ended_ = true;
        status_ = status;
    }

    // One code.
    void Step(std::string& out) {
        while (have_ < width_ && i_ < in_.size()) {
            bits_ = (bits_ << 8) | static_cast<unsigned char>(in_[i_++]);
            have_ += 8;
        }
        if (have_ < width_) return End(StreamStatus::Ok);  // no end-of-data code: the data ends with the stream
        const unsigned code = (bits_ >> (have_ - width_)) & ((1u << width_) - 1);
        have_ -= width_;
        if (code == 256) {
            next_ = 258;
            width_ = 9;
            previous_ = -1;
            return;
        }
        if (code == 257) return End(StreamStatus::Ok);
        if (code > next_ || (code == next_ && previous_ < 0)) {
            return End(total_ == 0 ? StreamStatus::Corrupt : StreamStatus::Salvaged);
        }
        // The string of `code`; for the code being defined, the previous string plus its first byte.
        const bool defining = code == next_;
        const unsigned source = defining ? static_cast<unsigned>(previous_) : code;
        const size_t length = table_[source].length + (defining ? 1 : 0);
        const size_t at = out.size();
        out.resize(at + length);
        char* const p = &out[0] + at;
        size_t k = table_[source].length;
        if (defining) p[k] = static_cast<char>(table_[source].first);
        for (unsigned c = source; k > 0; c = table_[c].prefix) p[--k] = static_cast<char>(table_[c].last);
        if (total_ + length > limit_) {
            out.resize(at + (limit_ - total_));
            return End(StreamStatus::TooLarge);
        }
        total_ += length;
        if (previous_ >= 0 && next_ < 4096) {
            const uint8_t first = static_cast<uint8_t>(p[0]);
            table_[next_] = {static_cast<uint16_t>(previous_), static_cast<uint16_t>(table_[previous_].length + 1),
                             first, table_[previous_].first};
            next_++;
        }
        previous_ = static_cast<int>(code);
        if (next_ + early_ >= (1u << width_) && width_ < 12) width_++;
    }

    std::string_view in_;
    size_t limit_ = 0;
    unsigned early_ = 1;
    std::vector<Code> table_;
    unsigned next_ = 258;
    unsigned width_ = 9;
    int previous_ = -1;
    uint32_t bits_ = 0;
    unsigned have_ = 0;
    size_t i_ = 0;
    size_t total_ = 0;
    bool ended_ = false;
    StreamStatus status_ = StreamStatus::Ok;
};

StreamStatus Lzw(std::string_view in, std::string& out, size_t limit, int earlyChange) {
    out.clear();
    LzwReader reader;
    reader.Start(in, limit, earlyChange);
    reader.Run(out, SIZE_MAX);
    return reader.Status();
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

// TIFF predictor 2 on one row of `n` bytes (a last row may be short): every sample is the
// difference to the same component of the pixel before.
void UnpredictTiffRow(uint8_t* row, size_t n, int colors, int bpc, int columns) {
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

StreamStatus UnpredictTiff(std::string& data, size_t rowBytes, int colors, int bpc, int columns) {
    uint8_t* const base = reinterpret_cast<uint8_t*>(&data[0]);
    const size_t size = data.size();
    const StreamStatus status = size % rowBytes == 0 ? StreamStatus::Ok : StreamStatus::Salvaged;
    for (size_t start = 0; start < size; start += rowBytes) {
        UnpredictTiffRow(base + start, std::min(rowBytes, size - start), colors, bpc, columns);
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

StreamStatus DecodeFlateInto(std::string_view in, Inflate::Output& out, size_t at, size_t limit, size_t& length) {
    const size_t bomb = SaturatingAdd(SaturatingMul(in.size(), kMaxFlateRatio), kFlateAllowance);
    switch (Inflate::InflateInto(in, out, at, std::min(limit, bomb), length)) {
        case Inflate::Result::Ok: return StreamStatus::Ok;
        case Inflate::Result::Salvaged: return StreamStatus::Salvaged;
        case Inflate::Result::TooLarge: return StreamStatus::TooLarge;
        case Inflate::Result::Corrupt: break;
    }
    length = 0;
    return StreamStatus::Corrupt;
}

// ---------------------------------------------------------------- a piece at a time

struct Decoder::State {
    // LZW and RunLength pieces of about this many bytes.
    static constexpr size_t kPiece = size_t(64) << 10;

    Kind kind = Kind::Unknown;
    Params params;
    std::string_view in;
    size_t limit = 0;

    // The filter's own output (before a predictor).
    Inflate::Stream inflate;
    LzwReader lzw;
    RunLengthReader runLength;
    std::string buffer;  // an LZW or RunLength piece; the whole output of ASCII85 and ASCIIHex
    bool wholeGiven = false;
    StreamStatus rawStatus = StreamStatus::Ok;

    // The predictor: 0 none, 2 TIFF, 10 PNG.
    int predictor = 0;
    size_t rowBytes = 0;
    size_t bpp = 1;
    std::vector<uint8_t> raw;      // a row as it arrives (PNG: its filter byte first)
    size_t rawFill = 0;
    std::vector<uint8_t> rows[2];  // PNG: the row above and the row being made
    int current = 0;
    std::string_view pending;      // filter output not yet taken into a row
    StreamStatus predictorStatus = StreamStatus::Ok;

    uint64_t given = 0;
    bool ended = false;
    StreamStatus status = StreamStatus::Ok;

    // The filter's next piece; false at its end, rawStatus then set.
    bool RawNext(std::string_view& piece) {
        switch (kind) {
        case Kind::Flate: {
            const uint8_t* data = nullptr;
            size_t size = 0;
            if (inflate.Next(data, size)) {
                piece = std::string_view(reinterpret_cast<const char*>(data), size);
                return true;
            }
            switch (inflate.Finish()) {
            case Inflate::Result::Ok: rawStatus = StreamStatus::Ok; break;
            case Inflate::Result::Salvaged: rawStatus = StreamStatus::Salvaged; break;
            case Inflate::Result::TooLarge: rawStatus = StreamStatus::TooLarge; break;
            case Inflate::Result::Corrupt: rawStatus = StreamStatus::Corrupt; break;
            }
            return false;
        }
        case Kind::Lzw:
        case Kind::RunLength: {
            buffer.clear();
            if (kind == Kind::Lzw) {
                lzw.Run(buffer, kPiece);
                rawStatus = lzw.Status();
            } else {
                runLength.Run(buffer, kPiece);
                rawStatus = runLength.Status();
            }
            piece = buffer;
            return !buffer.empty();
        }
        default:
            // ASCII85, ASCIIHex and the filters not decoded: the whole output, once.
            if (wholeGiven) return false;
            wholeGiven = true;
            piece = kind == Kind::Ascii85 || kind == Kind::AsciiHex ? std::string_view(buffer) : in;
            return !piece.empty();
        }
    }

    // Gives `n` bytes of `row`, or as many as the limit leaves (then the output is TooLarge).
    bool Give(const uint8_t* row, size_t n, std::string_view& piece) {
        if (n > limit - given) {
            n = static_cast<size_t>(limit - given);
            End(StreamStatus::TooLarge);
        }
        if (n == 0) return false;
        piece = std::string_view(reinterpret_cast<const char*>(row), n);
        given += n;
        return true;
    }

    void End(StreamStatus final) {
        ended = true;
        status = final;
    }

    // The predictor's next row; false at the end.
    bool NextRow(std::string_view& piece) {
        const size_t need = predictor >= 10 ? rowBytes + 1 : rowBytes;
        bool more = true;
        while (rawFill < need) {
            if (pending.empty() && !(more = RawNext(pending))) break;
            const size_t take = std::min(need - rawFill, pending.size());
            std::memcpy(raw.data() + rawFill, pending.data(), take);
            rawFill += take;
            pending.remove_prefix(take);
        }
        size_t n = predictor >= 10 ? rawFill - (rawFill > 0 ? 1 : 0) : rawFill;
        if (!more) {
            // The filter's output ended: a short last row is kept as far as it goes.
            if (rawStatus == StreamStatus::Corrupt) {
                End(StreamStatus::Corrupt);
                return false;
            }
            if (rawFill > 0) predictorStatus = StreamStatus::Salvaged;
            End(Worse(rawStatus, predictorStatus));
            if (n == 0) return false;
        }
        rawFill = 0;
        uint8_t* row = raw.data();
        if (predictor >= 10) {
            row = rows[current].data();
            const uint8_t* up = rows[current ^ 1].data();
            if (!UnfilterBytes(raw[0], raw.data() + 1, up, n, bpp, row)) {
                // An unknown filter type: the row as it stands.
                std::memcpy(row, raw.data() + 1, n);
                predictorStatus = StreamStatus::Salvaged;
                if (ended) status = Worse(rawStatus, predictorStatus);
            }
            current ^= 1;
        } else {
            UnpredictTiffRow(row, n, params.colors, params.bitsPerComponent, params.columns);
        }
        return Give(row, n, piece);
    }
};

Decoder::Decoder() = default;
Decoder::~Decoder() = default;

void Decoder::Start(Kind kind, const Params& params, std::string_view in, size_t limit) {
    if (!state_) state_ = std::make_unique<State>();
    State& s = *state_;
    s.kind = kind;
    s.params = params;
    s.in = in;
    s.limit = limit;
    s.wholeGiven = false;
    s.rawStatus = StreamStatus::Ok;
    s.predictor = 0;
    s.rawFill = 0;
    s.current = 0;
    s.pending = std::string_view();
    s.predictorStatus = StreamStatus::Ok;
    s.given = 0;
    s.ended = false;
    s.status = StreamStatus::Ok;
    switch (kind) {
    case Kind::Ascii85: s.rawStatus = Ascii85(in, s.buffer, limit); return;
    case Kind::AsciiHex: s.rawStatus = AsciiHex(in, s.buffer, limit); return;
    case Kind::RunLength: s.runLength.Start(in, limit); return;
    case Kind::Flate:
    case Kind::Lzw: break;
    default: s.rawStatus = StreamStatus::Unsupported; return;
    }
    // As Decode: predictor 2 is TIFF, 10 and above PNG, whose rows carry a filter byte each.
    const bool png = params.predictor >= 10;
    const bool tiff = params.predictor == 2;
    size_t decodeLimit = limit;
    if (png || tiff) {
        if (!RowShape(params, s.rowBytes, s.bpp)) {
            s.End(StreamStatus::Corrupt);
            return;
        }
        s.predictor = png ? 10 : 2;
        if (png) decodeLimit = SaturatingAdd(SaturatingAdd(limit, limit / s.rowBytes), s.rowBytes + 1);
    }
    size_t rawLimit = decodeLimit;
    if (kind == Kind::Flate) {
        rawLimit = std::min(decodeLimit, SaturatingAdd(SaturatingMul(in.size(), kMaxFlateRatio), kFlateAllowance));
        s.inflate.Start(in, rawLimit);
    } else {
        s.lzw.Start(in, decodeLimit, params.earlyChange);
    }
    if (s.predictor != 0) {
        // Rows as long as /Columns says, or as all the data the filter can give, when less.
        const size_t row = std::min(s.rowBytes, rawLimit);
        s.raw.assign(row + 1, 0);
        if (png) {
            s.rows[0].assign(row, 0);
            s.rows[1].assign(row, 0);
        }
    }
}

bool Decoder::Next(std::string_view& piece) {
    if (!state_) return false;
    State& s = *state_;
    if (s.ended) return false;
    if (s.predictor != 0) return s.NextRow(piece);
    if (s.RawNext(piece)) {
        s.given += piece.size();
        return true;
    }
    s.End(s.rawStatus);
    return false;
}

StreamStatus Decoder::Status() const { return state_ ? state_->status : StreamStatus::Ok; }

void Chain::Start(const Stage* stages, size_t count, std::string_view in, size_t limit) {
    limit_ = limit;
    streamed_ = false;
    before_ = StreamStatus::Ok;
    // The last filter decoded: the last general-purpose one before any other kind.
    size_t last = count;
    for (size_t i = 0; i < count; i++) {
        const Kind kind = stages[i].kind;
        if (kind == Kind::Crypt) continue;  // handled before (a Crypt filter comes first)
        if (kind != Kind::Flate && kind != Kind::Lzw && kind != Kind::Ascii85 && kind != Kind::AsciiHex &&
            kind != Kind::RunLength) {
            before_ = StreamStatus::Unsupported;
            break;
        }
        last = i;
    }
    std::string_view data = in;
    bool decodedAny = false;
    int buffer = 0;
    for (size_t i = 0; i < last && last < count; i++) {
        if (stages[i].kind == Kind::Crypt) continue;
        const StreamStatus step = Decode(stages[i].kind, stages[i].params, data, whole_[buffer], limit);
        data = whole_[buffer];
        buffer ^= 1;
        decodedAny = true;
        if (step == StreamStatus::Corrupt) {
            input_ = std::string_view();
            before_ = StreamStatus::Corrupt;
            Restart();
            return;
        }
        before_ = Worse(before_, step);
        if (step == StreamStatus::TooLarge) {
            input_ = data;
            Restart();
            return;
        }
    }
    if (last < count) {
        last_ = stages[last];
        streamed_ = true;
        input_ = data;
    } else if (!decodedAny && data.size() > limit) {
        // No filter: the data itself, cut at the limit.
        input_ = data.substr(0, limit);
        before_ = Worse(before_, StreamStatus::TooLarge);
    } else {
        input_ = data;
    }
    Restart();
}

void Chain::Restart() {
    given_ = false;
    ended_ = false;
    size_ = 0;
    status_ = before_;
    if (streamed_) decoder_.Start(last_.kind, last_.params, input_, limit_);
}

bool Chain::Next(std::string_view& piece) {
    if (ended_) return false;
    if (streamed_) {
        if (decoder_.Next(piece)) {
            size_ += piece.size();
            return true;
        }
        status_ = Worse(before_, decoder_.Status());
    } else if (!given_) {
        given_ = true;
        if (!input_.empty()) {
            piece = input_;
            size_ += piece.size();
            return true;
        }
    }
    ended_ = true;
    return false;
}

} // namespace RayoPdf::Filters
