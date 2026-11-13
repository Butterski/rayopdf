#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write OmniParseBench's split manifests: the rows of manifest.parquet whose pages are the .pdf
pages of each split of selection.json (written by `tools/eval.py fetch --only omni`), as
manifest-dev.parquet and manifest-val.parquet beside it, in the manifest's row order. Runs in
rayopdf-eval-omni:1 (pyarrow).

Usage: omni_split.py --dataset DIR
"""

import argparse
import json
import os
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dataset", required=True, help="data/omni, with manifest.parquet and selection.json")
    args = parser.parse_args()
    with open(os.path.join(args.dataset, "selection.json"), encoding="utf-8") as f:
        selection = json.load(f)
    table = pq.read_table(os.path.join(args.dataset, "manifest.parquet"))
    for split, pages in selection["splits"].items():
        wanted = pa.array(pages["pdf"], type=pa.string()).cast(table.schema.field("page_id").type)
        rows = table.filter(pc.is_in(table["page_id"], value_set=wanted))
        if rows.num_rows != len(pages["pdf"]):
            print(f"omni_split: {split}: {rows.num_rows} manifest rows for {len(pages['pdf'])} pages")
            return 1
        pq.write_table(rows, os.path.join(args.dataset, f"manifest-{split}.parquet"))
        print(f"omni_split: {split}: {rows.num_rows} rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
