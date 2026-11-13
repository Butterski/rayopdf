# SPDX-License-Identifier: Apache-2.0
"""Train the table-gap model (plan 5.4, M9) in the train image.

Inputs per rendered table fixture (tools/eval.py corpus --suite gentables, then train-data --kind
table-gaps): train/table-gaps/<producer>/<id>.jsonl (`rayopdf --features table-gaps`: every
candidate column gap of a borderless run, with whether the rules made it a table),
<id>.words.html beside it (pdftotext -bbox: the page's words and boxes) and the fixture's
gentables/md/md/<id>.cells.json (its cells by row and column).

Labels: words are matched to cells by exact text (a word that occurs in one cell of the table
only); a gap is a column boundary (1) when, in at least 80 % of the rows crossing it (rows with
matched words on both sides on its page), the nearest matched words left and right of it belong to
different columns. Gaps crossed by fewer than two such rows are left out. Validation: the fixtures
whose SHA-256 of id falls in the lowest 20 %; training: the rest. LightGBM as edge_rows.py.
Writes <out>/table_gaps.json and <out>/table_gaps.meta.json (with τ_hi, the lowest threshold with
precision >= 0.98 for boundaries, and τ_lo, the highest with precision >= 0.98 for non-boundaries
among the gaps the rules chose).

Usage (tools/eval.py train runs it):
  python3 /opt/eval/train/table_gaps.py --fixtures /fixtures --features /features --out /out
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
from pathlib import Path

import lightgbm
import numpy

FEATURES = 12
PARAMS = {"objective": "binary", "num_leaves": 15, "max_depth": 5, "learning_rate": 0.1, "min_data_in_leaf": 50,
          "bagging_fraction": 1.0, "bagging_freq": 0, "feature_fraction": 1.0, "deterministic": True,
          "force_row_wise": True, "num_threads": 1, "seed": 0, "verbose": -1}
ROUNDS, PATIENCE = 60, 10
PAGE = re.compile(r'<page width="([\d.]+)" height="([\d.]+)">(.*?)</page>', re.S)
WORD = re.compile(r'<word xMin="([\d.]+)" yMin="([\d.]+)" xMax="([\d.]+)" yMax="([\d.]+)">(.*?)</word>', re.S)


def words_by_page(path: Path, cells: list[dict]) -> dict[int, list[tuple[float, float, int, int]]]:
    """Matched words per page (1-based): (x0, x1, row, col)."""
    owners: dict[str, set[tuple[int, int]]] = {}
    for c in cells:
        for token in c["text"].split():
            owners.setdefault(token, set()).add((c["row"], c["col"]))
    out: dict[int, list[tuple[float, float, int, int]]] = {}
    for n, page in enumerate(PAGE.finditer(path.read_text(encoding="utf-8", errors="replace")), 1):
        for w in WORD.finditer(page.group(3)):
            owner = owners.get(html.unescape(w.group(5)))
            if owner and len(owner) == 1:
                row, col = next(iter(owner))
                out.setdefault(n, []).append((float(w.group(1)), float(w.group(3)), row, col))
    return out


def label(gap: tuple[float, float], words: list[tuple[float, float, int, int]]) -> int | None:
    rows: dict[int, list[tuple[float, float, int, int]]] = {}
    for w in words:
        rows.setdefault(w[2], []).append(w)
    crossing = boundary = 0
    for row in rows.values():
        left = [w for w in row if w[1] <= gap[0] + 0.5]
        right = [w for w in row if w[0] >= gap[1] - 0.5]
        if not left or not right:
            continue
        crossing += 1
        boundary += max(left, key=lambda w: w[1])[3] != min(right, key=lambda w: w[0])[3]
    if crossing < 2:
        return None
    return 1 if boundary >= 0.8 * crossing else 0


def validation(fid: str) -> bool:
    return int(hashlib.sha256(fid.encode()).hexdigest(), 16) % 5 == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--fixtures", type=Path, required=True, help="gentables/md (md/<id>.cells.json)")
    parser.add_argument("--features", type=Path, required=True, help="train/table-gaps (<producer>/<id>.jsonl)")
    parser.add_argument("--out", type=Path, required=True, help="folder for the model files")
    args = parser.parse_args()

    sets = {"train": ([], [], [], []), "val": ([], [], [], [])}
    for path in sorted(args.features.glob("*/*.jsonl")):
        fid = path.stem
        words_path = path.with_suffix(".words.html")
        cells_path = args.fixtures / "md" / f"{fid}.cells.json"
        if not words_path.is_file() or not cells_path.is_file():
            continue
        words = words_by_page(words_path, json.loads(cells_path.read_text(encoding="utf-8")))
        xs, ys, rules, keys = sets["val" if validation(fid) else "train"]
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines()):
            record = json.loads(line)
            y = label((record["box"][0], record["box"][1]), words.get(record["page"], []))
            if y is None:
                continue
            xs.append(record["f"])
            ys.append(y)
            rules.append(record["rules"])
            keys.append(f"{path.parent.name}/{fid}:{n}")
    xt, yt = numpy.asarray(sets["train"][0], dtype=numpy.float64), numpy.asarray(sets["train"][1])
    xv, yv = numpy.asarray(sets["val"][0], dtype=numpy.float64), numpy.asarray(sets["val"][1])
    rv, kv = numpy.asarray(sets["val"][2]), sets["val"][3]
    if xt.shape[1:] != (FEATURES,) or xv.shape[1:] != (FEATURES,):
        raise SystemExit(f"table_gaps: expected {FEATURES} features a gap")
    train = lightgbm.Dataset(xt, label=yt, params={"verbose": -1})
    val = lightgbm.Dataset(xv, label=yv, reference=train, params={"verbose": -1})
    booster = lightgbm.train(PARAMS, train, num_boost_round=ROUNDS, valid_sets=[val], valid_names=["val"],
                             callbacks=[lightgbm.early_stopping(PATIENCE, verbose=False)])
    raw = booster.predict(xv, raw_score=True, num_iteration=booster.best_iteration)
    p = 1 / (1 + numpy.exp(-raw))
    truth = yv.astype(bool)
    grid = numpy.round(numpy.arange(0.01, 1.0, 0.01), 2)
    tau_hi = next((float(t) for t in grid if (p >= t).any() and truth[p >= t].mean() >= 0.98), 1.0)
    chosen = rv.astype(bool)
    tau_lo = next((float(t) for t in grid[::-1] if (chosen & (p <= t)).any() and
                   (~truth[chosen & (p <= t)]).mean() >= 0.98), 0.0)
    model = booster.dump_model(num_iteration=booster.best_iteration)
    versions = json.loads(Path("/opt/eval/versions.json").read_text(encoding="utf-8"))
    manifest = (args.fixtures / "manifest.json").read_bytes()
    meta = {
        "schema": 1, "model": "table_gaps", "features": FEATURES,
        "fixtures_sha256": hashlib.sha256(manifest).hexdigest(),
        "parameters": {**PARAMS, "num_boost_round": ROUNDS, "early_stopping_rounds": PATIENCE},
        "best_iteration": int(booster.best_iteration), "versions": versions,
        "tau_hi": tau_hi, "tau_lo": tau_lo,
        "metrics": {
            "train_gaps": int(len(yt)), "train_boundaries": int(yt.sum()),
            "val_gaps": int(len(yv)), "val_boundaries": int(yv.sum()),
            "val_accuracy_at_0_5": round(float(((p >= 0.5) == truth).mean()), 6),
            "val_rules_accuracy": round(float((chosen == truth).mean()), 6),
            "val_binary_logloss": round(float(booster.best_score["val"]["binary_logloss"]), 6),
        },
        "checks": [{"key": kv[i], "features": [float(v) for v in xv[i]], "raw": float(raw[i])}
                   for i in range(min(64, len(kv)))],
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "table_gaps.json").write_text(json.dumps(model, sort_keys=True) + "\n", encoding="utf-8")
    (args.out / "table_gaps.meta.json").write_text(json.dumps(meta, sort_keys=True, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(meta["metrics"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
