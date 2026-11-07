#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-page census class and per-image page coverage, for the OCR/ML gate.

The census (census.py) keeps only each page's class. The gate also needs the area of every image
on a page, to bound the text-bearing image regions on digital pages, and the census classes of
olmOCR-bench's scanned categories. This script re-runs the census's own poppler calls and
classifier (imported from census.py in the same directory) and keeps those numbers.

Usage (inside rayopdf-eval-producers:1, --network none; tools/ocr_gate.py collect runs it):
    python3 pages.py --zips /data --out /out/safedocs-pages.jsonl [--jobs 6]
    python3 pages.py --pdfs /scans/old_scans /scans/old_scans_math --out /out/old-scans-pages.jsonl

One JSON line per PDF, sorted by file: {"file", "ok", "pages": [{"class", "coverage", "images",
"script"}]}. "coverage" is the census's capped image coverage of the page, "images" the area of
each counted image (types image and stencil) as a fraction of the page area, uncapped, rounded to
4 digits. At most the first 50 pages are examined, as in the census.
"""

import argparse
import json
import multiprocessing
import os
import shutil
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census  # noqa: E402  (same directory, stdlib only)


def image_fractions(out, sizes, doc_size):
    """Per-page list of image area fractions from `pdfimages -list` output."""
    per_page = {}
    counted = census.THRESHOLDS["image_types_counted"]
    for line in out.decode("utf-8", "replace").splitlines():
        t = line.split()
        if len(t) < 14 or not t[0].isdigit() or t[2] not in counted:
            continue
        try:
            page, w, h, xppi, yppi = int(t[0]), int(t[3]), int(t[4]), float(t[12]), float(t[13])
        except ValueError:
            continue
        if xppi <= 0 or yppi <= 0:
            continue
        size = sizes.get(page, doc_size)
        if not size:
            continue
        per_page.setdefault(page, []).append((w / xppi * 72.0) * (h / yppi * 72.0) / size)
    return per_page


def examine(path):
    rc, out, _ = census.run_tool(["pdfinfo", "-isodates", "-f", "1", "-l", str(census.MAX_PAGES), path])
    if rc != 0:
        return {"ok": False, "pages": []}
    info = census.parse_pdfinfo(out)
    try:
        pages = int(info.get("Pages", ""))
    except ValueError:
        return {"ok": False, "pages": []}
    n = min(pages, census.MAX_PAGES)
    if n <= 0:
        return {"ok": True, "pages": []}
    rng = ["-f", "1", "-l", str(n)]
    rc, out, _ = census.run_tool(["pdfimages", "-list", *rng, path])
    images = image_fractions(out, info["sizes"], info["doc_size"]) if rc == 0 else {}
    _, out, _ = census.run_tool(["pdftotext", "-enc", "UTF-8", *rng, path, "-"])
    texts = out.decode("utf-8", "replace").split("\f")[:-1][:n]
    scripts = census.page_scripts(texts)
    recs = []
    for i, text in enumerate(texts, start=1):
        fractions = images.get(i, [])
        coverage = min(1.0, sum(fractions))
        recs.append({"class": census.classify_page(text, coverage), "coverage": round(coverage, 4),
                     "images": [round(f, 4) for f in fractions], "script": scripts[i - 1]})
    return {"ok": True, "pages": recs}


def examine_task(task):
    kind, base, name = task
    if kind == "file":
        return {"file": name, **examine(os.path.join(base, name))}
    fd, tmp = tempfile.mkstemp(prefix="gate-", suffix=".pdf")
    try:
        try:
            with zipfile.ZipFile(base) as zf, zf.open(name) as src, os.fdopen(fd, "wb") as dst:
                fd = None
                shutil.copyfileobj(src, dst, 1 << 20)
        except Exception:  # corrupt member: the census records it as a zip failure
            if fd is not None:
                os.close(fd)
            return {"file": f"{os.path.basename(base)}:{name}", "ok": False, "pages": []}
        return {"file": f"{os.path.basename(base)}:{name}", **examine(tmp)}
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def list_tasks(zips, pdfs):
    tasks = []
    if zips:
        for name in sorted(os.listdir(zips)):
            if name.lower().endswith(".zip"):
                path = os.path.join(zips, name)
                with zipfile.ZipFile(path) as zf:
                    tasks += [("zip", path, i.filename) for i in sorted(zf.infolist(), key=lambda i: i.filename)
                              if not i.is_dir() and i.filename.lower().endswith(".pdf")]
    for root in pdfs or []:
        parent = os.path.dirname(os.path.normpath(root))
        for dirpath, _, files in os.walk(root):
            tasks += [("file", parent, os.path.relpath(os.path.join(dirpath, f), parent))
                      for f in sorted(files) if f.lower().endswith(".pdf")]
    return tasks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zips", help="directory holding *.zip files of PDFs")
    ap.add_argument("--pdfs", nargs="*", help="directories of PDFs (walked recursively)")
    ap.add_argument("--out", required=True, help="output JSONL file")
    ap.add_argument("--jobs", type=int, default=6, help="worker processes (default 6)")
    args = ap.parse_args()
    tasks = list_tasks(args.zips, args.pdfs)
    if not tasks:
        print("pages: no PDFs", file=sys.stderr)
        return 1
    recs = []
    with multiprocessing.Pool(max(1, args.jobs)) as pool:
        for i, rec in enumerate(pool.imap_unordered(examine_task, tasks, chunksize=1), start=1):
            recs.append(rec)
            if i % 100 == 0 or i == len(tasks):
                print(f"pages: {i}/{len(tasks)}", file=sys.stderr, flush=True)
    recs.sort(key=lambda r: r["file"])
    with open(args.out, "w", encoding="utf-8") as f:
        for r in recs:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
