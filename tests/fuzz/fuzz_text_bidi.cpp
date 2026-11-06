// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of src/text/bidi.h: the input as code points (three bytes each, any value) whose
// classes form a visual line, reordered with each paragraph direction; the result must be a
// permutation with levels 0-2 (no explicit embeddings).
#include "text/bidi.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace RayoPdf::Text;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<Bidi::Class> classes;
    for (size_t i = 0; i + 3 <= size && classes.size() < 4096; i += 3) {
        const uint32_t cp = static_cast<uint32_t>(data[i]) << 16 | static_cast<uint32_t>(data[i + 1]) << 8 |
                            static_cast<uint32_t>(data[i + 2]);
        const Bidi::Class c = Bidi::ClassOf(cp);
        if (static_cast<uint8_t>(c) > static_cast<uint8_t>(Bidi::Class::PDI)) __builtin_trap();
        if (Bidi::Mirror(cp) > 0x10FFFF && cp <= 0x10FFFF) __builtin_trap();
        classes.push_back(c);
    }
    std::vector<uint32_t> order;
    std::vector<uint8_t> levels;
    std::vector<uint8_t> seen;
    for (int direction = -1; direction <= 1; ++direction) {
        Bidi::VisualToLogical(classes.data(), classes.size(), order, &levels, direction);
        if (order.size() != classes.size() || levels.size() != classes.size()) __builtin_trap();
        seen.assign(classes.size(), 0);
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] >= classes.size() || seen[order[i]]++ || levels[i] > 2) __builtin_trap();
        }
    }
    return 0;
}
