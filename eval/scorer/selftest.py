#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for constructs.py: checks that the metrics react correctly to known mutations.

Usage:
    selftest.py --fixtures /fixtures/md

Uses the ground-truth fixtures c01-headings, c04-lists and c07-links. Mutated predictions are
generated programmatically into a temporary directory. Cases:

1. identity: GT vs itself (all three fixtures) -> every F1 and attribute accuracy is 1.0.
2. empty: GT vs empty files (all three fixtures) -> every recall is 0.
3. heading-level: c01-headings with every heading level changed -> heading F1 1.0, level
   accuracy 0. ATX headings of level 1-5 get one more '#', setext headings become ATX one
   level deeper; level-6 headings become level 5, because a seventh '#' would stop being a
   heading at all and the case is about a level change on a kept heading.
4. link-url: c07-links with exactly one inline link's URL changed -> link F1 1.0, URL
   accuracy (n-1)/n.
5. list-flatten: c04-lists with every nested list item moved to depth 1 -> list-item F1 1.0,
   depth accuracy < 1.0.

Prints one ``PASS <case>`` or ``FAIL <case>: <detail>`` line per case; exit 0 only if all pass.
"""

import argparse
import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import constructs  # noqa: E402

FIXTURES = ("c01-headings.md", "c04-lists.md", "c07-links.md")
FENCE = re.compile(r"^\s*(```|~~~)")
ATX = re.compile(r"^(#{1,6})(\s|$)")
SETEXT = re.compile(r"^(=+|-+)\s*$")
LIST_ITEM = re.compile(r"^\s+(?=(?:[-*+]|\d{1,9}[.)])(?:\s|$))")
INLINE_LINK = re.compile(r"(\]\()(https?://[^\s)]+)")


def unfenced(lines):
    """Yield (index, line) for lines outside fenced code blocks."""
    fenced = False
    for i, line in enumerate(lines):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            yield i, line


def shift_headings(text):
    lines = text.split("\n")
    out = list(lines)
    drop = set()
    for i, line in unfenced(lines):
        m = ATX.match(line)
        if m:
            level = len(m.group(1))
            new = level + 1 if level < 6 else 5
            out[i] = "#" * new + line[level:]
        elif SETEXT.match(line) and i > 0 and lines[i - 1].strip() and not ATX.match(lines[i - 1]):
            level = 1 if line.lstrip().startswith("=") else 2
            out[i - 1] = "#" * (level + 1) + " " + lines[i - 1].strip()
            drop.add(i)
    return "\n".join(line for i, line in enumerate(out) if i not in drop)


def change_one_url(text):
    m = INLINE_LINK.search(text)
    if not m:
        raise ValueError("no inline http(s) link to mutate")
    url = m.group(2)
    new = url.replace("://", "://changed.", 1)
    return text[:m.start(2)] + new + text[m.end(2):]


def flatten_lists(text):
    lines = text.split("\n")
    out = list(lines)
    for i, line in unfenced(lines):
        out[i] = LIST_ITEM.sub("", line)
    return "\n".join(out)


def write(dirpath, name, text):
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, name), "w", encoding="utf-8") as f:
        f.write(text)


def check_all(totals, key, want):
    bad = []
    for cat, d in totals["categories"].items():
        if d[key] is not None and d[key] != want:
            bad.append(f"{cat}.{key}={d[key]}")
        if key == "f1":
            for name, a in d["attr"].items():
                if a["acc"] is not None and a["acc"] != want:
                    bad.append(f"{name}.acc={a['acc']}")
    if key == "recall":
        for sect in ("math",):
            r = totals[sect]["recall"]
            if r is not None and r != want:
                bad.append(f"{sect}.recall={r}")
        r = totals["image"]["count_recall"]
        if r is not None and r != want:
            bad.append(f"image.count_recall={r}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--fixtures", required=True, help="directory with the GT fixture .md files")
    args = ap.parse_args()

    texts = {}
    for name in FIXTURES:
        with open(os.path.join(args.fixtures, name), encoding="utf-8") as f:
            texts[name] = f.read()

    tmp = tempfile.mkdtemp(prefix="selftest-")
    results = []
    try:
        def run(case, gt_names, mutate):
            gt_dir = os.path.join(tmp, case, "gt")
            pred_dir = os.path.join(tmp, case, "pred")
            for name in gt_names:
                write(gt_dir, name, texts[name])
                write(pred_dir, name, mutate(texts[name]))
            return constructs.score_dirs(gt_dir, pred_dir)["totals"]

        # 1. identity
        t = run("identity", FIXTURES, lambda s: s)
        bad = check_all(t, "f1", 1.0)
        results.append(("identity", bad))

        # 2. empty
        t = run("empty", FIXTURES, lambda s: "")
        bad = check_all(t, "recall", 0.0)
        results.append(("empty", bad))

        # 3. heading levels shifted
        t = run("heading-level", ("c01-headings.md",), shift_headings)
        h = t["categories"]["heading"]
        bad = []
        if h["gt"] == 0:
            bad.append("no GT headings")
        if h["f1"] != 1.0:
            bad.append(f"heading.f1={h['f1']} (want 1.0)")
        acc = h["attr"]["heading.level"]["acc"]
        if acc != 0.0:
            bad.append(f"heading.level.acc={acc} (want 0.0)")
        results.append(("heading-level", bad))

        # 4. one link URL changed
        t = run("link-url", ("c07-links.md",), change_one_url)
        lk = t["categories"]["link"]
        n = lk["gt"]
        bad = []
        if n < 2:
            bad.append(f"too few GT links ({n})")
        if lk["f1"] != 1.0:
            bad.append(f"link.f1={lk['f1']} (want 1.0)")
        acc = lk["attr"]["link.url"]["acc"]
        want = (n - 1) / n if n else None
        if acc is None or want is None or abs(acc - want) > 1e-9:
            bad.append(f"link.url.acc={acc} (want {want})")
        results.append(("link-url", bad))

        # 5. lists flattened to depth 1
        t = run("list-flatten", ("c04-lists.md",), flatten_lists)
        li = t["categories"]["list_item"]
        bad = []
        if li["f1"] != 1.0:
            bad.append(f"list_item.f1={li['f1']} (want 1.0)")
        acc = li["attr"]["list_item.depth"]["acc"]
        if acc is None or not acc < 1.0:
            bad.append(f"list_item.depth.acc={acc} (want < 1.0)")
        results.append(("list-flatten", bad))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    failed = 0
    for case, bad in results:
        if bad:
            failed += 1
            print(f"FAIL {case}: {'; '.join(bad)}")
        else:
            print(f"PASS {case}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
