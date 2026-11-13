// SPDX-License-Identifier: Apache-2.0
// Small models compiled into the core (roadmap M9): gradient-boosted decision trees trained with
// LightGBM by eval/train/*.py, whose dumps (eval/models/) tools/gen_model_tables.py writes into
// layout/model_data.cpp. No file is read at run time; a score is a sum over a few hundred nodes.
#pragma once

#include <cstdint>

namespace RayoPdf::Layout {

// A node of a tree: a split sends features[feature] <= threshold to `left`, else to `right` (the
// numerical split of LightGBM; features are never NaN); a leaf (feature -1) holds `value`. Children
// are indices inside the node's tree and follow their parent (preorder), so a walk ends.
struct TreeNode {
    double threshold;
    double value;
    int16_t feature;
    uint16_t left, right;
};

struct TreeModel {
    const TreeNode* nodes;
    const uint32_t* trees;  // tree t's nodes are nodes[trees[t], trees[t + 1])
    uint32_t treeCount;
    uint32_t featureCount;
    double base;  // added to the sum of the leaves
    // Feature vectors (checkCount x featureCount) with LightGBM's raw scores for them, from the
    // model's meta file, against which tests check RawScore.
    const float* checkFeatures;
    const double* checkScores;
    uint32_t checkCount;
};

// The model's raw score (log-odds) of a feature vector of model.featureCount values: the base plus
// the leaf each tree reaches.
double RawScore(const TreeModel& model, const float* features);

// Edge rows as page furniture (layout/features.h's kEdgeRowFeatures features; used by
// RemoveRunningLines): a row the rules keep in the outer tenth of a page, at most
// kEdgeRowSizeLimit body sizes, whose probability sigmoid(RawScore) reaches kEdgeRowTau (raw score
// at least kEdgeRowRawTau = log(tau / (1 - tau))) is furniture, unless the page would keep fewer
// than three other lines (the rules' guard of sparse pages) or the row reads as a caption. Its
// lines stay text for every decision about the page and only the paragraphs and headings they
// alone make are left out (kLineEdgeModel). kEdgeRowTau is the smallest threshold whose precision
// over such rows of doclaynet-dev reaches 0.98 (the meta's "tau_precision").
extern const TreeModel kEdgeRowModel;
extern const double kEdgeRowTau;
extern const double kEdgeRowRawTau;
extern const double kEdgeRowSizeLimit;

} // namespace RayoPdf::Layout
