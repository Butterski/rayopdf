// SPDX-License-Identifier: Apache-2.0
#include "layout/model.h"

namespace RayoPdf::Layout {

double RawScore(const TreeModel& model, const float* features) {
    // LightGBM's order: the trees' outputs added one after another in double precision.
    double score = model.base;
    for (uint32_t t = 0; t < model.treeCount; ++t) {
        const TreeNode* tree = model.nodes + model.trees[t];
        uint32_t i = 0;
        // Each step goes to a later node of the tree (preorder), so the walk ends at a leaf.
        while (tree[i].feature >= 0)
            i = double(features[tree[i].feature]) <= tree[i].threshold ? tree[i].left : tree[i].right;
        score += tree[i].value;
    }
    return score;
}

} // namespace RayoPdf::Layout
