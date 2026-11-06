// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of Filters::Decode: the first bytes choose the filter, the predictor and its
// image shape and the output limit; the rest is the encoded data.
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
    const StreamStatus status = Filters::Decode(
        kind, params, std::string_view(reinterpret_cast<const char*>(data) + 5, size - 5), out, limit);
    // Unsupported hands the input back as it is; every decoder stays within the limit.
    if (status != StreamStatus::Unsupported && out.size() > limit) __builtin_trap();
    return 0;
}
