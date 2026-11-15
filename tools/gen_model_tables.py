#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate src/layout/model_data.cpp, the core's small models (roadmap M9), from eval/models/.

Reads every trained model eval/models/<name>.json (LightGBM's dump_model() of a binary
classifier, written by eval/train/<name>.py) with its <name>.meta.json, and writes per model:

- its nodes, tree after tree, each tree in preorder (a child after its parent): split feature
  (int16_t, -1 for a leaf), threshold (double: features[f] <= threshold goes left), left and right
  child (uint16_t, indices inside the tree) and leaf value (double);
- the offsets of its trees in the node array, its base score (0: LightGBM folds the initial score
  into the first tree), its feature count;
- the decision thresholds of its meta file (edge rows: τ_p, the meta's "tau_precision", the
  smallest threshold of precision 0.90 on doclaynet-dev, as a probability and as the raw score
  log(τ / (1 - τ)) the engine compares with, and the row size limit);
- the meta file's 64 check vectors (features rounded to float, as the engine passes them) with
  LightGBM's raw scores, which tests/test_layout_model.cpp compares RawScore with.

Only the plain numerical split is accepted: decision_type "<=", missing_type "None" (features
are never NaN, so default_left never applies), no categorical splits, objective "binary sigmoid:1",
one tree an iteration. Anything else stops the generator. src/layout/model.h declares the layout
and the models; a model name the header does not declare stops it too (MODELS below).

Usage:
  python3 tools/gen_model_tables.py            # write src/layout/model_data.cpp
  python3 tools/gen_model_tables.py --check    # exit 1 when the committed file differs
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MODELS_DIR = REPO / "eval" / "models"
OUTPUT = REPO / "src" / "layout" / "model_data.cpp"

# Model file name -> (C++ name in src/layout/model.h, meta thresholds it needs: the probability
# threshold the engine uses first). The edge-row model removes furniture rows
# (layout/reading_order.cpp) at τ_p ("tau_precision"), not at the F1-best "tau".
MODELS = {
    "edge_rows": ("EdgeRow", ("tau_precision", "size_limit")),
}
CHECKS = 64
MAX_NODES = 65535


def double(value: float) -> str:
    """A C++ literal of exactly `value` (the shortest decimal that reads back as the same double)."""
    if not math.isfinite(value):
        raise SystemExit(f"gen_model_tables: a value is not finite: {value}")
    text = repr(float(value))
    return text if any(c in text for c in ".e") else text + ".0"


def single(value: float) -> str:
    """A C++ float literal of `value` rounded to single precision."""
    rounded = struct.unpack("<f", struct.pack("<f", float(value)))[0]
    return double(rounded) + "f"


def flatten(name: str, tree: dict, features: int) -> list[tuple[int, float, int, int, float]]:
    """A tree's nodes in preorder: (feature, threshold, left, right, leaf value)."""
    nodes: list[list] = []

    def visit(node: dict, depth: int) -> int:
        if depth > 64:
            raise SystemExit(f"gen_model_tables: {name}: a tree deeper than 64")
        index = len(nodes)
        if "leaf_value" in node:
            nodes.append([-1, 0.0, 0, 0, float(node["leaf_value"])])
            return index
        if node.get("decision_type") != "<=" or node.get("missing_type") != "None":
            raise SystemExit(f"gen_model_tables: {name}: a split other than '<=' without missing values: "
                             f"{node.get('decision_type')} {node.get('missing_type')}")
        feature = int(node["split_feature"])
        if not 0 <= feature < features:
            raise SystemExit(f"gen_model_tables: {name}: split feature {feature} outside 0-{features - 1}")
        nodes.append([feature, float(node["threshold"]), 0, 0, 0.0])
        nodes[index][2] = visit(node["left_child"], depth + 1)
        nodes[index][3] = visit(node["right_child"], depth + 1)
        return index

    visit(tree, 0)
    if len(nodes) > MAX_NODES:
        raise SystemExit(f"gen_model_tables: {name}: a tree of {len(nodes)} nodes (at most {MAX_NODES})")
    return [tuple(n) for n in nodes]


def model_tables(name: str, cpp: str, needs: tuple[str, ...]) -> list[str]:
    dump_path, meta_path = MODELS_DIR / f"{name}.json", MODELS_DIR / f"{name}.meta.json"
    dump_bytes = dump_path.read_bytes()
    dump, meta = json.loads(dump_bytes), json.loads(meta_path.read_text(encoding="utf-8"))
    features = int(meta["features"])
    if dump.get("objective") != "binary sigmoid:1" or dump.get("num_class") != 1 or \
            dump.get("num_tree_per_iteration") != 1 or dump.get("average_output"):
        raise SystemExit(f"gen_model_tables: {name}: not a plain LightGBM binary model "
                         f"({dump.get('objective')}, {dump.get('num_class')} classes)")
    if int(dump["max_feature_idx"]) + 1 != features:
        raise SystemExit(f"gen_model_tables: {name}: {int(dump['max_feature_idx']) + 1} features, the meta says {features}")
    trees = []
    for tree in dump["tree_info"]:
        if tree.get("num_cat", 0) != 0:
            raise SystemExit(f"gen_model_tables: {name}: categorical splits")
        trees.append(flatten(name, tree["tree_structure"], features))
    checks = meta["checks"]
    if len(checks) != CHECKS or any(len(c["features"]) != features for c in checks):
        raise SystemExit(f"gen_model_tables: {name}: expected {CHECKS} check vectors of {features} features")
    for key in needs:
        if not isinstance(meta.get(key), (int, float)):
            raise SystemExit(f"gen_model_tables: {name}: the meta file has no number '{key}'")

    out = [f"// {name}: {len(trees)} trees, {sum(len(t) for t in trees)} nodes, {features} features; "
           f"eval/models/{name}.json SHA-256",
           f"// {hashlib.sha256(dump_bytes).hexdigest()}, LightGBM {meta['versions']['pip']['lightgbm']}, "
           f"training selection SHA-256",
           f"// {meta['selection_sha256']}.",
           "namespace {", "",
           f"constexpr TreeNode k{cpp}Nodes[] = {{"]
    offsets = [0]
    for t, nodes in enumerate(trees):
        out.append(f"    // tree {t}")
        for feature, threshold, left, right, value in nodes:
            out.append(f"    {{{double(threshold)}, {double(value)}, {feature}, {left}, {right}}},")
        offsets.append(offsets[-1] + len(nodes))
    out.append("};")
    out.append(f"constexpr uint32_t k{cpp}Trees[] = {{")
    for i in range(0, len(offsets), 12):
        out.append("    " + ", ".join(str(o) for o in offsets[i:i + 12]) + ",")
    out.append("};")
    out.append(f"constexpr float k{cpp}CheckFeatures[] = {{")
    for check in checks:
        out.append("    " + ", ".join(single(v) for v in check["features"]) + ",")
    out.append("};")
    out.append(f"constexpr double k{cpp}CheckScores[] = {{")
    for i in range(0, CHECKS, 4):
        out.append("    " + ", ".join(double(c["raw"]) for c in checks[i:i + 4]) + ",")
    out.append("};")
    out.append("")
    out.append("} // namespace")
    out.append("")
    out.append(f"const TreeModel k{cpp}Model = {{k{cpp}Nodes, k{cpp}Trees, {len(trees)}, {features}, 0.0,")
    out.append(f"                                k{cpp}CheckFeatures, k{cpp}CheckScores, {CHECKS}}};")
    if needs[0].startswith("tau"):
        tau = float(meta[needs[0]])
        if not 0 < tau < 1:
            raise SystemExit(f"gen_model_tables: {name}: {needs[0]} {tau} outside (0, 1)")
        out.append(f"const double k{cpp}Tau = {double(tau)};  // the meta's \"{needs[0]}\"")
        out.append(f"const double k{cpp}RawTau = {double(math.log(tau / (1 - tau)))};  // log(tau / (1 - tau))")
    if "size_limit" in needs:
        out.append(f"const double k{cpp}SizeLimit = {double(float(meta['size_limit']))};")
    out.append("")
    return out


def generate() -> str:
    names = sorted(p.name[:-len(".json")] for p in MODELS_DIR.glob("*.json") if not p.name.endswith(".meta.json"))
    out = ["// SPDX-License-Identifier: Apache-2.0",
           "// Generated by tools/gen_model_tables.py from the trained models of eval/models/ (roadmap M9; the",
           "// edge-row model learnt from DocLayNet, CDLA-Permissive-1.0: third_party/licenses/doclaynet.txt).",
           "// Do not edit.",
           '#include "layout/model.h"',
           "",
           "namespace RayoPdf::Layout {",
           ""]
    for name in names:
        if name not in MODELS:
            raise SystemExit(f"gen_model_tables: eval/models/{name}.json is not a model of src/layout/model.h "
                             "(add it there and to MODELS)")
        cpp, needs = MODELS[name]
        out.extend(model_tables(name, cpp, needs))
    missing = sorted(set(MODELS) - set(names))
    if missing:
        raise SystemExit(f"gen_model_tables: src/layout/model.h declares models without a dump: {missing}")
    out.append("} // namespace RayoPdf::Layout")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--check", action="store_true", help="exit 1 when the committed file differs")
    args = parser.parse_args()
    text = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != text:
            print(f"{OUTPUT} is out of date", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT} ({len(text.encode('utf-8'))} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
