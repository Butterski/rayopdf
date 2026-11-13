// SPDX-License-Identifier: Apache-2.0
#include "layout/features.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace RayoPdf::Layout {

void TextShares(std::string_view text, float& digitShare, float& capitalShare, bool& endsInDigit) {
    size_t bytes = 0, digits = 0, letters = 0, capitals = 0;
    char last = 0;
    for (const char c : text) {
        if (c == ' ') continue;
        bytes++;
        last = c;
        if (c >= '0' && c <= '9') digits++;
        if (c >= 'A' && c <= 'Z') letters++, capitals++;
        if (c >= 'a' && c <= 'z') letters++;
    }
    digitShare = bytes ? float(digits) / float(bytes) : 0;
    capitalShare = letters ? float(capitals) / float(letters) : 0;
    endsInDigit = last >= '0' && last <= '9';
}

float MedianDeviation(std::vector<float>& values) {
    if (values.empty()) return 0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    const float median = *middle;
    for (float& v : values) v = std::fabs(v - median);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

} // namespace RayoPdf::Layout
