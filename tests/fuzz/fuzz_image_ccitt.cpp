// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the CCITTFaxDecode decoder (src/image/ccitt.h). The first 8 bytes are the
// parameters: /K (signed byte), flags (1 /EndOfLine, 2 /EncodedByteAlign, 4 /EndOfBlock,
// 8 /BlackIs1), /Columns - 1 (16 bits, big-endian), /Rows (16 bits), /DamagedRowsBeforeError, and
// a row limit (0: none, else 4 x the byte); the rest is the coded data. A pixel limit keeps one
// input fast. Checks the output's shape: whole rows within the limits, black pad bits.
#include "image/ccitt.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf::Image;
    if (size < 8) return 0;
    Ccitt::Params params;
    params.k = static_cast<int8_t>(data[0]);
    params.endOfLine = (data[1] & 1) != 0;
    params.encodedByteAlign = (data[1] & 2) != 0;
    params.endOfBlock = (data[1] & 4) != 0;
    params.blackIs1 = (data[1] & 8) != 0;
    params.columns = (uint32_t(data[2]) << 8 | data[3]) + 1;
    params.rows = uint32_t(data[4]) << 8 | data[5];
    params.damagedRowsBeforeError = data[6];
    const uint32_t maxRows = data[7] == 0 ? 0xFFFFFFFFu : uint32_t(data[7]) * 4;
    const uint64_t maxPixels = uint64_t(1) << 22;
    std::string rows;
    Ccitt::Info info;
    Ccitt::Decode(data + 8, size - 8, params, maxRows, maxPixels, rows, info);
    const size_t stride = (size_t(params.columns) + 7) / 8;
    if (rows.size() != size_t(info.rows) * stride || info.rows > maxRows ||
        uint64_t(info.rows) * params.columns > maxPixels || info.bytesUsed > size - 8) {
        __builtin_trap();
    }
    if (params.columns % 8 != 0) {
        const uint8_t pad = static_cast<uint8_t>(0xFF >> (params.columns % 8));
        for (uint32_t y = 0; y < info.rows; y++) {
            const uint8_t last = static_cast<uint8_t>(rows[size_t(y) * stride + stride - 1]) & pad;
            if (last != (params.blackIs1 ? pad : 0)) __builtin_trap();
        }
    }
    return 0;
}
