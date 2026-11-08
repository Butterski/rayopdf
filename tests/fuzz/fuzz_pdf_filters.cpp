// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of Filters::Decode: the first bytes choose the filter, the predictor and its
// image shape and the output limit; the rest is the encoded data. Filters::Decoder must give the
// same bytes and status a piece at a time.
#include "pdf/filters.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf;
    if (size < 5) return 0;
    static const Filters::Kind kKinds[] = {Filters::Kind::Flate, Filters::Kind::Lzw, Filters::Kind::Ascii85,
                                           Filters::Kind::AsciiHex, Filters::Kind::RunLength, Filters::Kind::Dct};
    static const int kPredictors[] = {1, 2, 10, 11, 12, 13, 14, 15};
    static const int kBits[] = {1, 2, 4, 8, 16, 3};
    const Filters::Kind kind = kKinds[data[0] % 6];
    Filters::Params params;
    params.predictor = kPredictors[data[1] & 7];
    params.colors = 1 + ((data[1] >> 3) & 3);
    params.bitsPerComponent = kBits[data[2] % 6];
    params.columns = 1 + data[3];
    params.earlyChange = data[2] >> 7;
    // Limits from a few bytes to 16 MiB.
    const size_t limit = size_t(1) << (4 + data[4] % 21);
    std::string out;
    const std::string_view in(reinterpret_cast<const char*>(data) + 5, size - 5);
    const StreamStatus status = Filters::Decode(kind, params, in, out, limit);
    // Unsupported hands the input back as it is; every decoder stays within the limit.
    if (status != StreamStatus::Unsupported && out.size() > limit) __builtin_trap();
    Filters::Decoder decoder;
    decoder.Start(kind, params, in, limit);
    size_t at = 0;
    std::string_view piece;
    while (decoder.Next(piece)) {
        if (piece.empty() || piece.size() > out.size() - at || out.compare(at, piece.size(), piece) != 0) {
            __builtin_trap();
        }
        at += piece.size();
    }
    if (at != out.size() || decoder.Status() != status) __builtin_trap();
    return 0;
}
