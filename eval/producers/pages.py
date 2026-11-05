#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Count pages of every PDF under a directory with pdfinfo.

Usage: pages.py --in /in --out /out/odl-pages.json [--jobs 6]

Writes {"<path relative to --in, without .pdf>": pages} for every *.pdf found recursively;
the value is null when pdfinfo fails or reports no page count.
"""

import argparse
import concurrent.futures
import json
import os
import subprocess


def count_pages(path):
    try:
        proc = subprocess.run(["pdfinfo", path], capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        return None
    if proc.returncode != 0:
        return None
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        key, _, value = line.partition(":")
        if key.strip() == "Pages" and value.strip().isdigit():
            return int(value.strip())
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--in", dest="indir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args()

    rel_ids = []
    for dirpath, dirnames, filenames in os.walk(args.indir):
        dirnames.sort()
        for name in sorted(filenames):
            if name.lower().endswith(".pdf"):
                rel = os.path.relpath(os.path.join(dirpath, name), args.indir)
                rel_ids.append(rel[:-4])
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(args.jobs, 1)) as pool:
        counts = list(pool.map(lambda r: count_pages(os.path.join(args.indir, r + ".pdf")), rel_ids))
    result = dict(sorted(zip(rel_ids, counts)))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"{len(result)} PDFs, {sum(1 for c in counts if c is None)} without page count")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
