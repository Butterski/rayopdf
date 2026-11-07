// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of src/text/repair.h: the input as font text (any bytes, mostly invalid UTF-8)
// repaired under every context and counted into map statistics; the repair of repaired text
// changes nothing more (it is idempotent).
#include "text/repair.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace RayoPdf::Text;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    Repair::MapStats stats;
    std::string out, again;
    for (int bits = 0; bits < 8; ++bits) {
        Repair::Context context;
        context.symbol = (bits & 1) != 0;
        context.wingdings = (bits & 2) != 0;
        context.thai = (bits & 4) != 0;
        if (Repair::RepairText(text, context, out)) {
            if (out.size() > text.size() * 3 + 4) __builtin_trap();
            if (Repair::RepairText(out, context, again) && again != out) __builtin_trap();
        }
        Repair::Count(text.substr(0, 64), stats);
        Repair::SymbolCodePoint(size > 1 ? (data[0] << 8 | data[1]) + 0xE000u : 0xF0B7u, context);
    }
    Repair::Untrusted(stats);
    if (stats.junk > stats.entries) __builtin_trap();
    return 0;
}
