#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the table fixtures of the table-gap model (plan 5.4): 400 Markdown files, each one GFM
table between the corpus's sentinel sentences, with 3-8 columns and 4-20 body rows of integers,
decimals with thousands separators, percentages, short words or 2-4 word phrases, GFM column
alignments, and every cell text unique in its table. Beside each, <id>.cells.json lists the cells
(row 0 the header, col 0 the first column, text); manifest.json lists the fixtures in
render.py's form. Deterministic (seed 0). Output is never committed.

Usage (tools/eval.py corpus --suite gentables runs it in the producers image):
  python3 eval/fixtures/gen_tables.py --out benchmark-output/eval/gentables/md [--count 400]
"""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

WORDS = ("amber", "basin", "cedar", "delta", "ember", "fjord", "grove", "harbor", "island", "jade", "kelp",
         "lagoon", "meadow", "nectar", "orchid", "prairie", "quartz", "ridge", "summit", "tundra", "upland",
         "valley", "willow", "yarrow", "zenith", "anchor", "beacon", "canyon", "dune", "estuary", "falcon",
         "glacier", "heron", "inlet", "juniper", "kestrel", "lichen", "marsh", "nimbus", "oasis", "pebble",
         "quarry", "reef", "spruce", "thicket", "umber", "vista", "wharf", "yew", "zephyr", "alder", "bluff",
         "cove", "dell", "eddy", "fern", "gully", "hollow", "isle", "knoll", "ledge", "moor", "notch", "orbit",
         "plateau", "rapids", "shoal", "tor", "vale", "weir")
HEADS = ("Region", "Site", "Count", "Share", "Total", "Name", "Note", "Rate", "Index", "Value", "Area",
         "Stock", "Price", "Growth", "Label", "Status", "Weight", "Score", "Change", "Group")
KINDS = ("integer", "decimal", "percent", "word", "phrase")


def cell(kind: str, rng: random.Random) -> str:
    if kind == "integer":
        return str(rng.randint(0, 99999))
    if kind == "decimal":
        return f"{rng.uniform(0, 999999):,.2f}"
    if kind == "percent":
        return f"{rng.uniform(0, 100):.1f}%"
    if kind == "word":
        return rng.choice(WORDS).capitalize()
    return " ".join(rng.choice(WORDS) for _ in range(rng.randint(2, 4))).capitalize()


def table(rng: random.Random) -> tuple[list[list[str]], list[str]]:
    """Rows (the header first) and the column alignments of one table."""
    columns = rng.randint(3, 8)
    rows = rng.randint(4, 20)
    kinds = [rng.choice(("word", "phrase"))] + [rng.choice(KINDS) for _ in range(columns - 1)]
    used: set[str] = set()
    header = []
    for name in rng.sample(HEADS, columns):
        header.append(name)
        used.add(name)
    body = []
    for _ in range(rows):
        row = []
        for kind in kinds:
            for attempt in range(1000):
                text = cell(kind, rng)
                # Past the vocabulary of one table: the word with a number ("Cedar 12").
                if attempt >= 50:
                    text += f" {rng.randint(2, 999)}"
                if text not in used:
                    break
            else:
                raise SystemExit("gen_tables: no unique cell text")
            used.add(text)
            row.append(text)
        body.append(row)
    aligns = [(":---:" if rng.random() < 0.3 else "---:") if kind in ("integer", "decimal", "percent")
              else rng.choice((":---", ":---:", "---")) for kind in kinds]
    return [header] + body, aligns


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--out", type=Path, required=True, help="fixture folder (manifest.json, md/)")
    parser.add_argument("--count", type=int, default=400)
    args = parser.parse_args()
    rng = random.Random(0)
    (args.out / "md").mkdir(parents=True, exist_ok=True)
    fixtures = []
    for n in range(1, args.count + 1):
        fid = f"t{n:03d}-table"
        tag = fid.split("-", 1)[0].upper()
        rows, aligns = table(rng)
        lines = [f"Sentinel start {tag}.", "", "| " + " | ".join(rows[0]) + " |", "| " + " | ".join(aligns) + " |"]
        lines += ["| " + " | ".join(r) + " |" for r in rows[1:]]
        lines += ["", f"Sentinel end {tag}.", ""]
        (args.out / "md" / f"{fid}.md").write_text("\n".join(lines), encoding="utf-8")
        cells = [{"row": r, "col": c, "text": t} for r, row in enumerate(rows) for c, t in enumerate(row)]
        (args.out / "md" / f"{fid}.cells.json").write_text(json.dumps(cells, indent=1) + "\n", encoding="utf-8")
        fixtures.append({"id": fid, "file": f"md/{fid}.md", "exclude": []})
    (args.out / "manifest.json").write_text(json.dumps({"schema": 1, "fixtures": fixtures}, indent=1) + "\n",
                                            encoding="utf-8")
    print(f"gen_tables: {len(fixtures)} fixtures in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
