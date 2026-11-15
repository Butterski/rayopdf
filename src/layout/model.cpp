// SPDX-License-Identifier: Apache-2.0
#include "layout/model.h"

namespace RayoPdf::Layout {

namespace {

// The leaf tree t of `model` gives `features`.
double Leaf(const TreeModel& model, uint32_t t, const float* features) {
    const TreeNode* tree = model.nodes + model.trees[t];
    uint32_t i = 0;
    // Each step goes to a later node of the tree (preorder), so the walk ends at a leaf.
    while (tree[i].feature >= 0) i = features[tree[i].feature] <= tree[i].threshold ? tree[i].left : tree[i].right;
    return model.leaves[tree[i].left];
}

} // namespace

double RawScore(const TreeModel& model, const float* features) {
    // LightGBM's order: the trees' outputs added one after another in double precision.
    double score = model.base;
    for (uint32_t t = 0; t < model.treeCount; ++t) score += Leaf(model, t, features);
    return score;
}

bool ScoreReaches(const TreeModel& model, const float* features, double raw) {
    // The margin covers the rounding of adding the leaves in another order than the bounds were
    // summed in (a few ulps of numbers below 100).
    constexpr double kMargin = 1e-6;
    double score = model.base;
    for (uint32_t t = 0; t < model.treeCount; ++t) {
        if (score + model.upper[t] < raw - kMargin) return false;
        if (score + model.lower[t] >= raw + kMargin) return true;
        score += Leaf(model, t, features);
    }
    return score >= raw;
}

} // namespace RayoPdf::Layout
