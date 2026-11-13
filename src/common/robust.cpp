// SPDX-License-Identifier: Apache-2.0
#include "common/robust.h"

#include <algorithm>
#include <cmath>

namespace RayoPdf::Robust {

int FisherClusters(const float* x, const uint32_t* w, size_t n, int maxK, float varianceFloor, uint8_t* cluster) {
    if (n == 0 || n > kMaxClusterPoints || maxK < 1 || maxK > kMaxClusters) return 0;
    for (size_t i = 0; i < n; ++i)
        if (w[i] == 0 || !std::isfinite(x[i]) || (i > 0 && !(x[i - 1] <= x[i]))) return 0;
    // Prefix sums of the weights, weighted values and squares; values relative to the first, so
    // the squares stay small.
    double weight[kMaxClusterPoints + 1] = {}, sum[kMaxClusterPoints + 1] = {}, squares[kMaxClusterPoints + 1] = {};
    for (size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(x[i]) - x[0];
        weight[i + 1] = weight[i] + w[i];
        sum[i + 1] = sum[i] + w[i] * v;
        squares[i + 1] = squares[i] + w[i] * v * v;
    }
    // The weighted sum of squared deviations of points [a, b).
    const auto deviations = [&](size_t a, size_t b) {
        const double ww = weight[b] - weight[a], s = sum[b] - sum[a];
        return std::max(0.0, squares[b] - squares[a] - s * s / ww);
    };
    // cost[k][i]: the least deviations of points [0, i) in k clusters; from[k][i]: where the last
    // of them starts (ties to the earliest).
    const int top = static_cast<int>(std::min<size_t>(static_cast<size_t>(maxK), n));
    double cost[kMaxClusters + 1][kMaxClusterPoints + 1];
    uint8_t from[kMaxClusters + 1][kMaxClusterPoints + 1] = {};
    for (size_t i = 0; i <= n; ++i) cost[0][i] = i == 0 ? 0 : INFINITY;
    for (int k = 1; k <= top; ++k) {
        for (size_t i = 0; i <= n; ++i) {
            cost[k][i] = INFINITY;
            for (size_t j = static_cast<size_t>(k - 1); j < i; ++j) {
                if (cost[k - 1][j] == INFINITY) continue;
                const double c = cost[k - 1][j] + deviations(j, i);
                if (c < cost[k][i]) cost[k][i] = c, from[k][i] = static_cast<uint8_t>(j);
            }
        }
    }
    const double total = weight[n];
    const double floor = std::max(static_cast<double>(varianceFloor), 1e-12);
    constexpr double kTwoPi = 6.283185307179586;
    int bestK = 0;
    double bestBic = 0;
    for (int k = 1; k <= top; ++k) {
        double logLikelihood = 0;
        size_t end = n;
        for (int c = k; c >= 1; --c) {
            const size_t start = from[c][end];
            const double ww = weight[end] - weight[start];
            const double variance = std::max(deviations(start, end) / ww, floor);
            logLikelihood += ww * std::log(ww / total) - ww / 2 * std::log(kTwoPi * variance) - ww / 2;
            end = start;
        }
        const double bic = -2 * logLikelihood + (3 * k - 1) * std::log(static_cast<double>(n));
        if (bestK == 0 || bic < bestBic) bestK = k, bestBic = bic;
    }
    size_t end = n;
    for (int c = bestK; c >= 1; --c) {
        const size_t start = from[c][end];
        for (size_t i = start; i < end; ++i) cluster[i] = static_cast<uint8_t>(c - 1);
        end = start;
    }
    return bestK;
}

} // namespace RayoPdf::Robust
