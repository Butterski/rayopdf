#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""RayoPDF's text of every census page, judged by the census's broken-text-layer checks.

For each PDF the census examined (census.jsonl, `examined` > 0), the zip member is extracted to a
temp file and one `rayopdf --serve --json` process converts each examined page alone
(`--pages=N-N --images=none`, Markdown returned in the answer). `<!-- rayopdf: ... -->` comments
are removed, then the page is classified with census.py's own definitions: `no_text` below
THRESHOLDS["min_chars"] non-space characters, else `broken` if census.broken_checks() fires,
else `text`. The checks run on the Markdown as written, as the round-6 measurement did: Markdown
joins a paragraph into one line where pdftotext writes one line per printed line, so the
junk-lines check (>= 10 % of lines) is stricter on RayoPDF's text; the per-check names are kept
so that effect can be counted.

Usage (inside rayopdf-build:2, --network none; tools/ocr_gate.py collect-rayopdf runs it):
    python3 rayopdf_pages.py --bin /opt/tool/rayopdf --census /census/census.jsonl \
        --zips /data --out /out/rayopdf-pages.jsonl [--jobs 6] [--timeout 600]

One JSON line per page, sorted by file and page: {"file", "page", "status", "class", "checks",
"glyphs", "unmapped", "guessed", "unmapped_comment"}.
"""

import argparse
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import census  # noqa: E402  (same directory, stdlib only)

COMMENT = re.compile(r"<!-- rayopdf: (.*?) -->", re.S)
BIN = None
TIMEOUT = 600


def judge(markdown):
    comments = COMMENT.findall(markdown)
    text = COMMENT.sub("", markdown)
    chars = sum(1 for ch in text if not ch.isspace())
    if chars < census.THRESHOLDS["min_chars"]:
        return "no_text", [], comments
    checks = census.broken_checks(text)
    return ("broken" if checks else "text"), checks, comments


def convert_pages(path, pages):
    requests = "".join(json.dumps({"id": str(n), "input": path, "args": [f"--pages={n}-{n}", "--images=none"]}) + "\n"
                       for n in range(1, pages + 1)) + "quit\n"
    try:
        p = subprocess.run([BIN, "--serve", "--json"], input=requests.encode(), capture_output=True,
                           timeout=TIMEOUT)
        # Only "\n" ends an answer: str.splitlines() would also split at U+2028, U+0085 and other
        # separators that the Markdown inside the JSON string may hold unescaped.
        lines = p.stdout.decode("utf-8", "replace").split("\n")[1:]
    except subprocess.TimeoutExpired:
        lines = []
    answers = {}
    for line in lines:
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        answers[rec.get("id")] = rec
    return answers


def examine(task):
    zip_path, member, label, pages = task
    fd, tmp = tempfile.mkstemp(prefix="rayopdf-", suffix=".pdf")
    try:
        with zipfile.ZipFile(zip_path) as zf, zf.open(member) as src, os.fdopen(fd, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        answers = convert_pages(tmp, pages)
    finally:
        os.unlink(tmp)
    out = []
    for n in range(1, pages + 1):
        rec = answers.get(str(n))
        if rec is None:
            out.append({"file": label, "page": n, "status": "no answer", "class": "no_text", "checks": [],
                        "glyphs": 0, "unmapped": 0, "guessed": 0, "unmapped_comment": False})
            continue
        cls, checks, comments = judge(rec.get("markdown") or "")
        out.append({"file": label, "page": n, "status": rec.get("status"), "class": cls, "checks": checks,
                    "glyphs": rec.get("glyphs", 0), "unmapped": rec.get("unmapped_glyphs", 0),
                    "guessed": rec.get("guessed_glyphs", 0),
                    "unmapped_comment": any("unmapped" in c for c in comments)})
    return out


def init(binary, timeout):
    global BIN, TIMEOUT
    BIN, TIMEOUT = binary, timeout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="rayopdf binary")
    ap.add_argument("--census", required=True, help="census.jsonl (files and examined page counts)")
    ap.add_argument("--zips", required=True, help="directory holding the census's *.zip files")
    ap.add_argument("--out", required=True, help="output JSONL file")
    ap.add_argument("--jobs", type=int, default=6, help="worker processes (default 6)")
    ap.add_argument("--timeout", type=int, default=600, help="seconds per PDF (all its pages)")
    args = ap.parse_args()
    tasks = []
    with open(args.census, encoding="utf-8") as f:
        for line in f:
            rec = json.loads(line)
            if rec.get("examined"):
                zip_name, member = rec["file"].split(":", 1)
                tasks.append((os.path.join(args.zips, zip_name), member, rec["file"], rec["examined"]))
    rows = []
    with multiprocessing.Pool(max(1, args.jobs), initializer=init, initargs=(args.bin, args.timeout)) as pool:
        for i, out in enumerate(pool.imap_unordered(examine, tasks, chunksize=1), start=1):
            rows += out
            if i % 100 == 0 or i == len(tasks):
                print(f"rayopdf_pages: {i}/{len(tasks)}", file=sys.stderr, flush=True)
    rows.sort(key=lambda r: (r["file"], r["page"]))
    with open(args.out, "w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
