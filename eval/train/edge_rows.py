# SPDX-License-Identifier: Apache-2.0
"""Train the edge-row furniture model (roadmap M9) in the train image.

Labels: a row examined by the furniture rules (`rayopdf --features edge-rows`, written by
`tools/eval.py train-data --kind edge-rows` into train/edge-rows/<split>/<stem>.jsonl) is
furniture (1) when at least half of its box lies in DocLayNet's Page-header and Page-footer boxes,
mapped from the 1025 px COCO image to PDF points as eval/scorer/doclaynet.py maps them. Trained on
the DocLayNet train selection; doclaynet-dev (DocLayNet's val split) only stops the boosting early
and chooses the thresholds over the rows the model may take (kept by the rules, at most
SIZE_LIMIT body sizes, not ending a sentence): τ (best furniture F1) and τ_p (the smallest
threshold whose precision reaches PRECISION, 0.90, the one the engine uses). The holdout
doclaynet-val is never read.

LightGBM binary: num_leaves 15, max_depth 5, learning_rate 0.1, up to 60 trees with early
stopping after 10 rounds on dev, min_data_in_leaf 50, no bagging or feature sampling,
deterministic, row-wise, one thread, seed 0: two runs give the same bytes. Writes
<out>/edge_rows.json (booster.dump_model()) and <out>/edge_rows.meta.json (selection SHA-256,
parameters, versions, metrics, 64 check vectors with their raw scores, τ and τ_p).

Usage (tools/eval.py train runs it):
  python3 /opt/eval/train/edge_rows.py --data /data/doclaynet --features /features --out /out
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import lightgbm
import numpy

FEATURES = 23
FURNITURE = ("Page-header", "Page-footer")
PARAMS = {"objective": "binary", "num_leaves": 15, "max_depth": 5, "learning_rate": 0.1, "min_data_in_leaf": 50,
          "bagging_fraction": 1.0, "bagging_freq": 0, "feature_fraction": 1.0, "deterministic": True,
          "force_row_wise": True, "num_threads": 1, "seed": 0, "verbose": -1}
ROUNDS, PATIENCE = 60, 10
# The model may remove only rows at most this many body sizes. Larger rows are often titles and
# headings: over doclaynet-dev's rows the rules keep, the model's precision at τ_p is 0.98 from
# 1.35 to 1.6 body sizes, 0.89 from 1.6 to 2 and 0.69 from 2 to 3.
SIZE_LIMIT = 1.6
ENDS_SENTENCE = 16  # nor rows that end a sentence (this feature 1)
PRECISION = 0.90  # τ_p's least precision over the rows the model may take (9 of 10 furniture)


def furniture_boxes(coco: dict) -> dict[str, list[list[float]]]:
    """COCO boxes of furniture by file stem."""
    ids = {c["id"] for c in coco["categories"] if c["name"] in FURNITURE}
    stems = {image["id"]: Path(image["file_name"]).stem for image in coco["images"]}
    out: dict[str, list[list[float]]] = {stem: [] for stem in stems.values()}
    for a in coco["annotations"]:
        if a["category_id"] in ids:
            out[stems[a["image_id"]]].append(a["bbox"])
    return out


def load(split: str, data: Path, features: Path, boxes: dict) -> tuple[list, list, list, list]:
    """Feature vectors, labels, rule flags and (stem, line) keys of a split, in stem order."""
    xs, ys, rules, keys = [], [], [], []
    for path in sorted((features / split).glob("*.jsonl")):
        stem = path.stem
        meta = json.loads((data / split / f"{stem}.json").read_text(encoding="utf-8"))["metadata"]
        w, h = float(meta["original_width"]), float(meta["original_height"])
        sx, sy = w / float(meta["coco_width"]), h / float(meta["coco_height"])
        pdf_boxes = [(x * sx, h - (y + bh) * sy, (x + bw) * sx, h - y * sy) for x, y, bw, bh in boxes.get(stem, [])]
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines()):
            record = json.loads(line)
            x0, y0, x1, y1 = record["box"]
            area = max(x1 - x0, 0.0) * max(y1 - y0, 0.0)
            inside = 0.0
            for a0, b0, a1, b1 in pdf_boxes:
                inside += max(min(x1, a1) - max(x0, a0), 0.0) * max(min(y1, b1) - max(y0, b0), 0.0)
            xs.append(record["f"])
            ys.append(1 if area > 0 and inside >= 0.5 * area else 0)
            rules.append(record["rules"])
            keys.append(f"{stem}:{n}")
    return xs, ys, rules, keys


def f1(truth: numpy.ndarray, predicted: numpy.ndarray) -> float:
    tp = float(numpy.sum(truth & predicted))
    fp = float(numpy.sum(~truth & predicted))
    fn = float(numpy.sum(truth & ~predicted))
    return 2 * tp / (2 * tp + fp + fn) if tp else 0.0


def precision_recall(truth: numpy.ndarray, predicted: numpy.ndarray) -> tuple[float, float]:
    tp = float(numpy.sum(truth & predicted))
    positive, relevant = float(numpy.sum(predicted)), float(numpy.sum(truth))
    return (tp / positive if positive else 0.0), (tp / relevant if relevant else 0.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--data", type=Path, required=True, help="data/doclaynet")
    parser.add_argument("--features", type=Path, required=True, help="train/edge-rows (train/, dev/)")
    parser.add_argument("--out", type=Path, required=True, help="folder for the model files")
    args = parser.parse_args()

    coco = args.data / "coco"
    train_boxes = furniture_boxes(json.loads((coco / "train.json").read_text(encoding="utf-8")))
    dev_boxes = furniture_boxes(json.loads((coco / "val.json").read_text(encoding="utf-8")))
    xt, yt, _, _ = load("train", args.data, args.features, train_boxes)
    xd, yd, rd, kd = load("dev", args.data, args.features, dev_boxes)
    xt, yt = numpy.asarray(xt, dtype=numpy.float64), numpy.asarray(yt)
    xd, yd, rd = numpy.asarray(xd, dtype=numpy.float64), numpy.asarray(yd), numpy.asarray(rd)
    if xt.shape[1:] != (FEATURES,) or xd.shape[1:] != (FEATURES,):
        raise SystemExit(f"edge_rows: expected {FEATURES} features a row")

    train = lightgbm.Dataset(xt, label=yt, params={"verbose": -1})
    dev = lightgbm.Dataset(xd, label=yd, reference=train, params={"verbose": -1})
    booster = lightgbm.train(PARAMS, train, num_boost_round=ROUNDS, valid_sets=[dev], valid_names=["dev"],
                             callbacks=[lightgbm.early_stopping(PATIENCE, verbose=False)])
    raw = booster.predict(xd, raw_score=True, num_iteration=booster.best_iteration)
    probability = 1 / (1 + numpy.exp(-raw))

    # τ: the threshold that maximises furniture F1 over the dev rows the rules keep that the model
    # may take, as the engine uses it: rows at most SIZE_LIMIT body sizes that do not end a
    # sentence (feature ENDS_SENTENCE).
    truth = yd.astype(bool)
    kept = (rd == 0) & (xd[:, 3] <= SIZE_LIMIT) & (xd[:, ENDS_SENTENCE] == 0)
    best_tau, best_f1 = 1.0, -1.0
    for tau in numpy.round(numpy.arange(0.50, 0.995, 0.01), 2):
        score = f1(truth[kept], probability[kept] >= tau)
        if score > best_f1:
            best_tau, best_f1 = float(tau), score
    # τ_p: the smallest threshold (steps of 0.001) at which at least PRECISION of the kept rows the
    # model would remove are furniture.
    tau_p = None
    for tau in numpy.round(numpy.arange(0.500, 0.9995, 0.001), 3):
        precision, recall = precision_recall(truth[kept], probability[kept] >= tau)
        if precision >= PRECISION:
            tau_p = float(tau)
            break
    if tau_p is None:
        raise SystemExit(f"edge_rows: no threshold below 1 reaches precision {PRECISION} on dev")
    rules_only = rd.astype(bool)
    combined = rules_only | (kept & (probability >= best_tau))
    combined_p = rules_only | (kept & (probability >= tau_p))

    model = booster.dump_model(num_iteration=booster.best_iteration)
    selection = (args.data / "selection.json").read_bytes()
    versions = json.loads(Path("/opt/eval/versions.json").read_text(encoding="utf-8"))
    checks = [{"key": kd[i], "features": [float(v) for v in xd[i]], "raw": float(raw[i])}
              for i in range(min(64, len(kd)))]
    meta = {
        "schema": 1,
        "model": "edge_rows",
        "features": FEATURES,
        "selection_sha256": hashlib.sha256(selection).hexdigest(),
        "parameters": {**PARAMS, "num_boost_round": ROUNDS, "early_stopping_rounds": PATIENCE},
        "best_iteration": int(booster.best_iteration),
        "versions": versions,
        "tau": best_tau,
        "tau_precision": tau_p,
        "precision_target": PRECISION,
        "size_limit": SIZE_LIMIT,
        "metrics": {
            "train_rows": int(len(yt)), "train_furniture": int(yt.sum()),
            "dev_rows": int(len(yd)), "dev_furniture": int(yd.sum()),
            "dev_kept_f1_at_tau": round(best_f1, 6),
            "dev_kept_precision_at_tau_precision": round(precision, 6),
            "dev_kept_recall_at_tau_precision": round(recall, 6),
            "dev_kept_f1_at_tau_precision": round(f1(truth[kept], probability[kept] >= tau_p), 6),
            "dev_rules_f1": round(f1(truth, rules_only), 6),
            "dev_rules_plus_model_f1": round(f1(truth, combined), 6),
            "dev_rules_plus_model_f1_at_tau_precision": round(f1(truth, combined_p), 6),
            "dev_binary_logloss": round(float(booster.best_score["dev"]["binary_logloss"]), 6),
        },
        "checks": checks,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "edge_rows.json").write_text(json.dumps(model, sort_keys=True) + "\n", encoding="utf-8")
    (args.out / "edge_rows.meta.json").write_text(json.dumps(meta, sort_keys=True, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(meta["metrics"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
