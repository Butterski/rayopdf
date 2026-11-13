// SPDX-License-Identifier: Apache-2.0
// Robust statistics fitted per document in place of fixed thresholds (docs/research/layout-math.md
// §2): Fisher's exact 1-D clustering with the number of clusters chosen by BIC. Deterministic:
// weights are integers, sums run in index order, and the same input gives the same result on every
// thread count and platform.
#pragma once

#include <cstddef>
#include <cstdint>

namespace RayoPdf::Robust {

constexpr size_t kMaxClusterPoints = 64;
constexpr int kMaxClusters = 8;

// Fisher's exact 1-D k-means (Fisher 1958; Jenks natural breaks) of the `n` points `x`, sorted
// ascending, with integer weights `w` (at least 1), for each k up to `maxK`, and of those the k of
// the smallest BIC = -2 ln L + (3k - 1) ln n (Schwarz 1978): L the likelihood of the weighted
// points under the clusters as Gaussians, each with its weight share, mean and variance, the
// variance at least `varianceFloor`. Ties go to the smaller k. Writes point i's cluster to
// cluster[i], 0 for the smallest values, and returns k; returns 0 (nothing written) when n is 0 or
// past kMaxClusterPoints, maxK is not in [1, kMaxClusters], or the points are not sorted, finite
// and of positive weight.
int FisherClusters(const float* x, const uint32_t* w, size_t n, int maxK, float varianceFloor, uint8_t* cluster);

} // namespace RayoPdf::Robust
