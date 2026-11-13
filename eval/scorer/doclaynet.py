#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""DocLayNet scorer: the roles RayoPDF's JSON output gives the text cells of DocLayNet pages.

Usage:
    doclaynet.py --coco COCO.json --pages DIR --pred DIR --out F.json [--per-page]

Each page of COCO.json (images with precedence 0 whose PDF is in --pages) is scored with its text
cells (--pages/<stem>.json, DocLayNet's "cells": text and a box [x, y, w, h] in the 1025 px square
COCO image) and the converter's output --pred/<stem>.md (RayoPDF's --format=json document; a
missing or unreadable output keeps nothing). A COCO box or cell maps to PDF points with the page
size W, H of the output (else of the cells' metadata): x0 = x W/1025, x1 = (x + w) W/1025,
y1 = H - y H/1025, y0 = H - (y + h) H/1025.

Gold role of a cell: the category of the smallest COCO box of its page that holds the cell's
centre (none: the cell is not scored). Groups: furniture = Page-header, Page-footer; heading =
Section-header, Title; list = List-item; footnote = Footnote; table = Table; Picture is not
scored; Text, Caption and Formula are body text. A cell weighs its non-space characters.

A cell is kept when its centre lies in the box (on its page) of an output block that is not an
image; its predicted role is the type of the smallest such block (heading, list_item, footnote,
table; any other type is body text).

Metrics, pooled over the pages and weighted by characters:
  text_kept     kept share of the cells that are not furniture
  furniture_f1  F1 of furniture (positive: a furniture cell; predicted positive: not kept)
  heading_f1, list_f1, footnote_f1, table_f1   F1 of the role over the kept cells

Output: {"schema": 1, "pages": N, "outputs": N with a readable output, "metrics": {...},
"counts": {...}} and, with --per-page (development data only), "pages_detail": {stem: {counts and
the page's metrics, null where undefined}}.
"""

import argparse
import json
import os
import sys

COCO_SIZE = 1025.0
GROUPS = {"Page-header": "furniture", "Page-footer": "furniture", "Section-header": "heading",
          "Title": "heading", "List-item": "list", "Footnote": "footnote", "Table": "table",
          "Text": "body", "Caption": "body", "Formula": "body"}
ROLES = ("heading", "list", "footnote", "table")
PREDICTED = {"heading": "heading", "list_item": "list", "footnote": "footnote", "table": "table"}
COUNT_KEYS = ["body", "body_kept", "furniture", "furniture_dropped", "dropped"] + [
    f"{role}_{part}" for role in ROLES for part in ("gold", "pred", "both")]


def to_points(box, width, height):
    """A COCO-space [x, y, w, h] as PDF points (x0, y0, x1, y1), origin at the lower left."""
    x, y, w, h = box
    return (x * width / COCO_SIZE, height - (y + h) * height / COCO_SIZE,
            (x + w) * width / COCO_SIZE, height - y * height / COCO_SIZE)


def inside(x, y, box):
    return box[0] <= x <= box[2] and box[1] <= y <= box[3]


def area(box):
    return (box[2] - box[0]) * (box[3] - box[1])


def read_output(path):
    """The output's page size and its blocks' (type, box) on its first page, or None."""
    try:
        with open(path, encoding="utf-8") as f:
            document = json.load(f)
        page = document["pages"][0]
        number = page["page"]
        blocks = []
        for block in document["blocks"]:
            if block["type"] == "image":
                continue
            for box in block["boxes"]:
                if box["page"] == number:
                    blocks.append((block["type"], tuple(box["bbox"])))
        return float(page["width"]), float(page["height"]), blocks
    except (OSError, ValueError, KeyError, IndexError, TypeError):
        return None


def score_page(cells, annotations, categories, output, fallback_size):
    """The page's counts (COUNT_KEYS)."""
    counts = dict.fromkeys(COUNT_KEYS, 0)
    width, height, blocks = output if output else (*fallback_size, [])
    golds = [(GROUPS.get(categories.get(a["category_id"])), to_points(a["bbox"], width, height))
             for a in annotations]
    for cell in cells:
        weight = sum(1 for ch in cell.get("text", "") if not ch.isspace())
        if weight == 0:
            continue
        x0, y0, x1, y1 = to_points(cell["bbox"], width, height)
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        holding = [(area(box), group) for group, box in golds if inside(cx, cy, box)]
        if not holding:
            continue
        gold = min(holding, key=lambda item: item[0])[1]
        if gold is None:  # Picture, or a category the groups do not name
            continue
        found = [(area(box), kind) for kind, box in blocks if inside(cx, cy, box)]
        kept = bool(found)
        predicted = PREDICTED.get(min(found, key=lambda item: item[0])[1], "body") if kept else None
        if gold == "furniture":
            counts["furniture"] += weight
            if not kept:
                counts["furniture_dropped"] += weight
        else:
            counts["body"] += weight
            if kept:
                counts["body_kept"] += weight
        if not kept:
            counts["dropped"] += weight
            continue
        for role in ROLES:
            counts[f"{role}_gold"] += weight if gold == role else 0
            counts[f"{role}_pred"] += weight if predicted == role else 0
            counts[f"{role}_both"] += weight if gold == role and predicted == role else 0
    return counts


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def metrics(counts):
    out = {"text_kept": ratio(counts["body_kept"], counts["body"]),
           "furniture_f1": ratio(2 * counts["furniture_dropped"], counts["furniture"] + counts["dropped"])}
    for role in ROLES:
        out[f"{role}_f1"] = ratio(2 * counts[f"{role}_both"], counts[f"{role}_gold"] + counts[f"{role}_pred"])
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--coco", required=True)
    parser.add_argument("--pages", required=True, help="folder of the pages' <stem>.pdf and <stem>.json")
    parser.add_argument("--pred", required=True, help="folder of the converter's <stem>.md (JSON)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--per-page", action="store_true", help="per-page counts (development data only)")
    args = parser.parse_args()

    with open(args.coco, encoding="utf-8") as f:
        coco = json.load(f)
    categories = {c["id"]: c["name"] for c in coco["categories"]}
    annotations = {}
    for a in coco["annotations"]:
        annotations.setdefault(a["image_id"], []).append(a)
    totals = dict.fromkeys(COUNT_KEYS, 0)
    detail = {}
    pages = outputs = 0
    for image in sorted(coco["images"], key=lambda im: im["file_name"]):
        stem = os.path.splitext(image["file_name"])[0]
        if image.get("precedence", 0) != 0 or not os.path.isfile(os.path.join(args.pages, f"{stem}.pdf")):
            continue
        with open(os.path.join(args.pages, f"{stem}.json"), encoding="utf-8") as f:
            page = json.load(f)
        meta = page["metadata"]
        output = read_output(os.path.join(args.pred, f"{stem}.md"))
        counts = score_page(page["cells"], annotations.get(image["id"], []), categories, output,
                            (float(meta["original_width"]), float(meta["original_height"])))
        pages += 1
        outputs += output is not None
        for key, value in counts.items():
            totals[key] += value
        if args.per_page:
            detail[stem] = {"counts": counts, "metrics": metrics(counts), "output": output is not None}
    if not pages:
        print("doclaynet: no page of the COCO file has its PDF in --pages", file=sys.stderr)
        return 1
    result = {"schema": 1, "pages": pages, "outputs": outputs, "metrics": metrics(totals), "counts": totals}
    if args.per_page:
        result["pages_detail"] = detail
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, sort_keys=True)
        f.write("\n")
    print(json.dumps({"pages": pages, "outputs": outputs, "metrics": result["metrics"]}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
