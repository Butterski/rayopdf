// SPDX-License-Identifier: Apache-2.0
// Core tests of src/common/robust: Fisher's clustering with BIC (codes 2100-2149).
#include "check.h"
#include "common/robust.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace RayoPdf;

// Heading sizes in log space: 10, 12 and 14.4 pt (weights 50, 5 and 2 characters) are three
// clusters; 13.98, 14.00 and 14.02 pt are one (a variance floor of 0.0004, sizes 2 % apart).
void TestFisherClusters() {
    const float sizes[3] = {std::log(10.0f), std::log(12.0f), std::log(14.4f)};
    const uint32_t weights[3] = {50, 5, 2};
    uint8_t cluster[3] = {};
    RAYOPDF_CHECK(Robust::FisherClusters(sizes, weights, 3, 6, 0.0004f, cluster) == 3 && cluster[0] == 0 &&
                      cluster[1] == 1 && cluster[2] == 2,
                  2103);
    const float close[3] = {std::log(13.98f), std::log(14.0f), std::log(14.02f)};
    const uint32_t ones[3] = {1, 1, 1};
    uint8_t one[3] = {9, 9, 9};
    RAYOPDF_CHECK(Robust::FisherClusters(close, ones, 3, 6, 0.0004f, one) == 1 && one[0] == 0 && one[1] == 0 &&
                      one[2] == 0,
                  2104);
    // Unsorted points are refused.
    const float unsorted[2] = {2.0f, 1.0f};
    RAYOPDF_CHECK(Robust::FisherClusters(unsorted, ones, 2, 6, 0.0004f, one) == 0, 2106);
}

} // namespace

int main() {
    TestFisherClusters();
    std::puts("common robust tests passed");
    return 0;
}
