// SPDX-License-Identifier: Apache-2.0
// Forked from RayoMD src/core/inflate.h at ef106ca (Apache-2.0, same author).
// A zlib (RFC 1950) and raw DEFLATE (RFC 1951) decoder for PDF's FlateDecode (internal to the pdf
// module; Filters::Decode is its interface). Unlike RayoMD's, which needs the exact output size,
// a valid header and its Adler-32, this one is built for damaged input (algorithms §1.3):
// - the output size is unknown: the buffer grows geometrically up to the limit;
// - a missing or invalid zlib header means raw deflate; a preset-dictionary flag is skipped over;
// - the Adler-32 is not checked (writers get it wrong; the deflate data carries its own end);
// - salvage: the output decoded before corrupt or truncated data is kept, never a byte decoded
//   from bits past the end of the input.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace RayoPdf::Inflate {

enum class Result : uint8_t {
    Ok,        // the final block ended
    Salvaged,  // corrupt or truncated data after some output; `out` holds the output before it
    TooLarge,  // more than `limit` bytes; `out` holds the first `limit`
    Corrupt,   // corrupt or truncated before any output; `out` is empty
};

// Inflates `in` into `out` (replaced), at most `limit` bytes.
Result Inflate(std::string_view in, std::string& out, size_t limit);

// Inflate's output a piece at a time, holding the 32 KB window and one piece (kPiece bytes) of
// output instead of all of it: the same bytes in order, and the same Result.
class Stream {
public:
    static constexpr size_t kPiece = size_t(224) << 10;

    Stream();
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Starts inflating `in` (alive and unchanged while it is read), at most `limit` bytes; again
    // from the start when called again.
    void Start(std::string_view in, size_t limit);
    // The next piece of output (at least one byte), valid until the next call; false once the
    // output is complete.
    bool Next(const uint8_t*& data, size_t& size);
    // How the output ended, once Next has returned false: Inflate's Result for `in` and `limit`.
    Result Finish() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace RayoPdf::Inflate
