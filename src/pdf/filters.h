// SPDX-License-Identifier: Apache-2.0
// Stream filters (ISO 32000-1 section 7.4): the general-purpose ones decoded here, image-only ones
// recognised and left to the image writer. Every decoder is bounded by an output limit, never
// reads past its input, and salvages: data decoded before a corrupt or truncated point is kept.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace RayoPdf {

enum class StreamStatus : uint8_t {
    Ok,
    Salvaged,     // decoding stopped at corrupt or truncated data; the output before it is kept
    Unsupported,  // a filter not decoded here (an image filter, Brotli, an unknown name); the
                  // output holds the data as it stands before that filter
    TooLarge,     // the output would pass the limit; the output is cut at the limit
    Corrupt,      // nothing usable
    NotStream,    // the value is no stream
};

namespace Inflate {
class Output;
}

namespace Filters {

enum class Kind : uint8_t {
    Flate,      // FlateDecode, Fl
    Lzw,        // LZWDecode, LZW
    Ascii85,    // ASCII85Decode, A85
    AsciiHex,   // ASCIIHexDecode, AHx
    RunLength,  // RunLengthDecode, RL
    Crypt,      // Crypt (handled by the document's security handler)
    Dct,        // DCTDecode, DCT (image)
    Jpx,        // JPXDecode (image)
    Jbig2,      // JBIG2Decode (image)
    Ccitt,      // CCITTFaxDecode, CCF (image)
    Brotli,     // BrotliDecode (PDF 2.0 errata; not decoded yet)
    Unknown,
};

// The filter a /Filter name (full or abbreviated) names.
Kind KindOf(std::string_view name);
// True for the image-only filters (Dct, Jpx, Jbig2, Ccitt).
bool IsImageFilter(Kind kind);

// /DecodeParms of Flate and LZW: the predictor (1 none, 2 TIFF, 10-15 PNG) and its image shape.
struct Params {
    int predictor = 1;
    int colors = 1;            // 1 to 32
    int bitsPerComponent = 8;  // 1, 2, 4, 8 or 16
    int columns = 1;
    int earlyChange = 1;       // LZW only
};

// Decodes `in` with one filter into `out` (replaced), at most `limit` bytes. Image filters,
// Crypt, Brotli and Unknown return Unsupported with `out` = `in`.
StreamStatus Decode(Kind kind, const Params& params, std::string_view in, std::string& out, size_t limit);
// Decode(Flate, params of no predictor, in, ·, limit) onto `out` from its byte `at` on (the bytes
// before kept): `length` bytes, the same bytes and status (no bytes when Corrupt).
StreamStatus DecodeFlateInto(std::string_view in, Inflate::Output& out, size_t at, size_t limit, size_t& length);

// Decode's output a piece at a time: the same bytes in order and the same status, holding only
// what the filter needs (Flate's 32 KB window and a piece, LZW's table and a piece, a predictor's
// two rows) instead of the whole output. ASCII85 and ASCIIHex, which never give more bytes than
// they read but for ASCII85's "z", are decoded whole and given as one piece.
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Starts Decode(kind, params, in, ·, limit); `in` stays alive and unchanged while it is read.
    // Called again, starts again.
    void Start(Kind kind, const Params& params, std::string_view in, size_t limit);
    // The next piece of the output (at least one byte), valid until the next call; false once the
    // output is complete.
    bool Next(std::string_view& piece);
    // Decode's status, once Next has returned false.
    StreamStatus Status() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// One filter of a stream's /Filter with its /DecodeParms.
struct Stage {
    Kind kind = Kind::Unknown;
    Params params;
};

// A stream's filters applied in turn as Document::StreamData runs them: Crypt skipped, the first
// filter that is not general-purpose (an image filter, Brotli, an unknown name) stops decoding
// (Unsupported; the data as that filter would read it), a Corrupt filter ends it with nothing, a
// TooLarge one with its own output; data no filter decodes is cut at the limit (TooLarge). Every
// filter but the last one decoded runs whole (their output is the next one's encoded input); the
// last gives its output a piece at a time (Decoder). The status is the worst of the filters'.
class Chain {
public:
    // Starts decoding `in` (alive and unchanged while it is read) through `count` stages, at most
    // `limit` bytes each. Called again, starts again.
    void Start(const Stage* stages, size_t count, std::string_view in, size_t limit);
    // From the first byte of the output again (the filters before the last one are not run again).
    void Restart();
    // The next piece (at least one byte), valid until the next call; false once the output is
    // complete.
    bool Next(std::string_view& piece);
    // Once Next has returned false: the status of the decoding.
    StreamStatus Status() const { return status_; }
    // The bytes given so far.
    uint64_t Size() const { return size_; }

private:
    Decoder decoder_;
    std::string whole_[2];
    Stage last_;
    std::string_view input_;  // the last filter's input, or the output when no filter streams it
    size_t limit_ = 0;
    bool streamed_ = false;   // the last filter runs through decoder_
    bool given_ = false;      // the output that is not streamed has been given
    bool ended_ = false;
    StreamStatus before_ = StreamStatus::Ok;  // the status of everything but the streamed filter
    StreamStatus status_ = StreamStatus::Ok;
    uint64_t size_ = 0;
};

} // namespace Filters
} // namespace RayoPdf
