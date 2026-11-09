#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""OCR text scorer: character error rate and word F1 of predictions against plain-text ground truth.

Usage:
    ocr_text.py --gt-dir G --pred-dir P --out F.json [--per-page]

Every ``<id>.md`` under G (or, with --per-page, every ``<id>/p<N>.txt``) is paired with the file
at the same relative path under P; a missing prediction counts as empty text. Ids are
``<script>-<variant>`` as made by eval/producers/multiscript.py.

Both sides are normalised the same way: HTML comments are dropped; Markdown markup is removed
(leading ``#``, ``>``, list markers ``- * + 1.``, thematic breaks and table rule rows, table pipes,
paired ``*``/``_`` emphasis, image and link syntax keeping the alt or link text, backslash
escapes keeping the escaped character); zero-width (non-)joiners dropped; then NFKC and whitespace
collapsed to single spaces.

Per item: CER = Levenshtein(pred, gt) / len(gt) (rapidfuzz), and word precision, recall and F1
from the multiset overlap of whitespace tokens with punctuation stripped from both token ends.
Aggregates per script (the id prefix) and per variant group (clean, serif, s200, noise, rot90,
rot180, rot270) and over everything: mean CER, pooled CER (sum of edits / sum of GT characters)
and pooled word precision, recall and F1.

Output: ``{"schema":1,"per_page":bool,"items":{relpath: M},"scripts":{s: A},"groups":{g: A},
"totals": A}``; the same aggregates are printed as Markdown tables.
"""

import argparse
import collections
import json
import os
import re
import sys
import unicodedata

from rapidfuzz.distance import Levenshtein

COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
ESCAPE = re.compile(r"\\([!-/:-@\[-`{-~])")
ESCAPE_BASE = 0xF0000            # escaped characters wait in a private-use plane meanwhile
RULE_ROW = re.compile(r"^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$")
THEMATIC_BREAK = re.compile(r"^\s*([-*_])(\s*\1){2,}\s*$")
HEADING = re.compile(r"^\s*#{1,6}(\s+|$)")
QUOTE = re.compile(r"^\s*(>\s?)+")
LIST_MARKER = re.compile(r"^\s*([-*+]|\d{1,9}[.)])\s+")
IMAGE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")
EMPHASIS_STAR = re.compile(r"(\*{1,3})(?=\S)(.+?)(?<=\S)\1")
EMPHASIS_UNDERSCORE = re.compile(r"(?<!\w)(_{1,3})(?=\S)(.+?)(?<=\S)\1(?!\w)")
PAGE_FILE = re.compile(r"^p\d+\.txt$")
GROUPS = ("clean", "serif", "s200", "noise", "rot90", "rot180", "rot270")


def strip_markdown_line(line):
    if RULE_ROW.match(line) or THEMATIC_BREAK.match(line):
        return ""
    line = HEADING.sub("", line)
    line = QUOTE.sub("", line)
    line = LIST_MARKER.sub("", line)
    line = IMAGE.sub(r"\1", line)
    line = LINK.sub(r"\1", line)
    for _ in range(3):           # nested emphasis, e.g. ***x*** or **_x_**
        line = EMPHASIS_STAR.sub(r"\2", line)
        line = EMPHASIS_UNDERSCORE.sub(r"\2", line)
    return line.replace("|", " ")


def normalise(text):
    text = COMMENT.sub(" ", text)
    text = ESCAPE.sub(lambda m: chr(ESCAPE_BASE + ord(m.group(1))), text)
    text = "\n".join(strip_markdown_line(line) for line in text.split("\n"))
    text = re.sub("[\U000F0000-\U000F007F]", lambda m: chr(ord(m.group(0)) - ESCAPE_BASE), text)
    # Zero-width joiners and non-joiners only steer shaping (Persian, Indic): no OCR emits them.
    text = text.replace("\u200c", "").replace("\u200d", "")
    return " ".join(unicodedata.normalize("NFKC", text).split())


def words(text):
    out = []
    for token in text.split(" "):
        start, end = 0, len(token)
        while start < end and unicodedata.category(token[start]).startswith("P"):
            start += 1
        while end > start and unicodedata.category(token[end - 1]).startswith("P"):
            end -= 1
        if start < end:
            out.append(token[start:end])
    return out


def score(gt_text, pred_text):
    gt, pred = normalise(gt_text), normalise(pred_text)
    edits = Levenshtein.distance(pred, gt)
    gt_words, pred_words = collections.Counter(words(gt)), collections.Counter(words(pred))
    matched = sum((gt_words & pred_words).values())
    n_gt, n_pred = sum(gt_words.values()), sum(pred_words.values())
    return {"cer": edits / len(gt) if gt else float(bool(pred)), "edits": edits,
            "gt_chars": len(gt), "pred_chars": len(pred), "gt_words": n_gt,
            "pred_words": n_pred, "matched_words": matched,
            **prf(matched, n_gt, n_pred)}


def prf(matched, n_gt, n_pred):
    return {"precision": matched / n_pred if n_pred else float(not n_gt),
            "recall": matched / n_gt if n_gt else float(not n_pred),
            "f1": 2 * matched / (n_gt + n_pred) if n_gt + n_pred else 1.0}


def aggregate(items):
    if not items:
        return {"n": 0}
    edits = sum(m["edits"] for m in items)
    chars = sum(m["gt_chars"] for m in items)
    matched = sum(m["matched_words"] for m in items)
    n_gt = sum(m["gt_words"] for m in items)
    n_pred = sum(m["pred_words"] for m in items)
    return {"n": len(items), "missing": sum(m["missing"] for m in items),
            "mean_cer": sum(m["cer"] for m in items) / len(items),
            "pooled_cer": edits / chars if chars else 0.0, "edits": edits, "gt_chars": chars,
            "matched_words": matched, "gt_words": n_gt, "pred_words": n_pred,
            **{"word_" + k: v for k, v in prf(matched, n_gt, n_pred).items()}}


def item_id(relpath, per_page):
    return relpath.split("/")[0] if per_page else relpath[:-len(".md")]


def variant_group(iid):
    variant = iid.split("-", 1)[1] if "-" in iid else iid
    if variant.startswith("rot"):
        return variant
    if "serif" in variant:
        return "serif"
    if variant.startswith("s200"):
        return "s200"
    if variant.endswith("noise"):
        return "noise"
    return "clean"


def gt_files(gt_dir, per_page):
    out = []
    for root, _, files in os.walk(gt_dir):
        rel_root = os.path.relpath(root, gt_dir).replace(os.sep, "/")
        for name in files:
            if per_page:
                if rel_root != "." and "/" not in rel_root and PAGE_FILE.match(name):
                    out.append(f"{rel_root}/{name}")
            elif rel_root == "." and name.endswith(".md"):
                out.append(name)
    return sorted(out)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def table(title, rows):
    lines = [f"| {title} | n | missing | mean CER | pooled CER | word P | word R | word F1 |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for name, a in rows:
        if a["n"]:
            lines.append(f"| {name} | {a['n']} | {a['missing']} | {a['mean_cer']:.4f} | "
                         f"{a['pooled_cer']:.4f} | {a['word_precision']:.4f} | "
                         f"{a['word_recall']:.4f} | {a['word_f1']:.4f} |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gt-dir", required=True)
    ap.add_argument("--pred-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-page", action="store_true",
                    help="score <id>/p<N>.txt pages instead of <id>.md documents")
    args = ap.parse_args()

    rels = gt_files(args.gt_dir, args.per_page)
    if not rels:
        print("no ground-truth files found", file=sys.stderr)
        return 1
    items = {}
    for rel in rels:
        pred_path = os.path.join(args.pred_dir, rel)
        missing = not os.path.isfile(pred_path)
        m = score(read(os.path.join(args.gt_dir, rel)), "" if missing else read(pred_path))
        m["missing"] = missing
        items[rel] = m
    by_script, by_group = collections.defaultdict(list), collections.defaultdict(list)
    for rel, m in items.items():
        iid = item_id(rel, args.per_page)
        by_script[iid.split("-", 1)[0]].append(m)
        by_group[variant_group(iid)].append(m)
    scripts = {s: aggregate(by_script[s]) for s in sorted(by_script)}
    group_order = [g for g in GROUPS if g in by_group] + sorted(set(by_group) - set(GROUPS))
    groups = {g: aggregate(by_group[g]) for g in group_order}
    totals = aggregate(list(items.values()))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "per_page": args.per_page, "items": items, "scripts": scripts,
                   "groups": groups, "totals": totals}, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(table("script", list(scripts.items()) + [("**all**", totals)]))
    print()
    print(table("variant", list(groups.items()) + [("**all**", totals)]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
