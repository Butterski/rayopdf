// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of src/text/bidi.h: the input as code points (three bytes each, any value). As a
// logical paragraph it goes through ResolveLevels with each paragraph direction and LogicalToVisual;
// as a visual line through VisualToLogical with and without its code points. Levels stay within
// UAX #9's depth (126 at most after I1-I2) and every order is a permutation of the kept items.
#include "text/bidi.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace RayoPdf::Text;

namespace {

void CheckPermutation(const std::vector<uint32_t>& order, size_t count, std::vector<uint8_t>& seen) {
    seen.assign(count, 0);
    for (const uint32_t i : order) {
        if (i >= count || seen[i]++) __builtin_trap();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint32_t> cps;
    std::vector<Bidi::Class> classes;
    for (size_t i = 0; i + 3 <= size && classes.size() < 4096; i += 3) {
        const uint32_t cp = static_cast<uint32_t>(data[i]) << 16 | static_cast<uint32_t>(data[i + 1]) << 8 |
                            static_cast<uint32_t>(data[i + 2]);
        const Bidi::Class c = Bidi::ClassOf(cp);
        if (static_cast<uint8_t>(c) > static_cast<uint8_t>(Bidi::Class::PDI)) __builtin_trap();
        if (Bidi::Mirror(cp) > 0x10FFFF && cp <= 0x10FFFF) __builtin_trap();
        cps.push_back(cp);
        classes.push_back(c);
    }
    std::vector<uint32_t> order;
    std::vector<uint8_t> levels;
    std::vector<uint8_t> seen;
    for (int direction = -1; direction <= 1; ++direction) {
        const uint8_t paragraph = Bidi::ResolveLevels(classes.data(), cps.data(), classes.size(), direction, levels);
        if (paragraph > 1 || levels.size() != classes.size()) __builtin_trap();
        size_t kept = 0;
        for (const uint8_t l : levels) {
            if (l == Bidi::kRemovedLevel) continue;
            if (l > 126) __builtin_trap();
            ++kept;
        }
        Bidi::LogicalToVisual(levels.data(), levels.size(), order);
        if (order.size() != kept) __builtin_trap();
        CheckPermutation(order, classes.size(), seen);

        for (const bool withCodePoints : {false, true}) {
            if (withCodePoints) {
                Bidi::VisualToLogical(classes.data(), cps.data(), classes.size(), order, &levels, direction);
            } else {
                Bidi::VisualToLogical(classes.data(), classes.size(), order, &levels, direction);
            }
            if (order.size() != classes.size() || levels.size() != classes.size()) __builtin_trap();
            CheckPermutation(order, classes.size(), seen);
            for (const uint8_t l : levels) {
                if (l > 126) __builtin_trap();
            }
        }
    }
    return 0;
}
