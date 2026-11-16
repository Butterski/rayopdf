#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""OmniParseBench adapter: RayoPDF's JSON output (--format=json) as `opb grade` predictions.

Usage: omni_predict.py --pred DIR --ids FILE --out DIR

For each page id of --ids (one a line), --pred/<id>.md holds RayoPDF's JSON document of the page;
--out/<id>.json gets {"pages": [{"html": H, "blocks": B}]}, the two outputs OmniParseBench's tests
read (omni_parse_bench.views):

- H, the page as HTML: headings <hN>, paragraphs and footnotes <p>, runs of list items <ul> or <ol>
  by `ordered` (an <ol> starts at its first item's printed number, and a new one opens where the
  numbers do not go on by one, so the numbers the grader draws are those of the page), code <pre>,
  tables <table> from their cells (header rows <th>, row_span and col_span as rowspan and colspan);
  images and comments are left out.
- B, one block per output block but comments: its box on the page as fractions of the page with
  the origin at the top left, and its label of views.LABELS (heading, table, list, code, footnote,
  figure for an image, text for the rest), with its text.

Text comes from each block's Markdown without its markup (md_text): emphasis delimiters, links
(their text kept), images' syntax, <sup>/<sub> tags, heading marks, list markers, code fences and
backslash escapes. Formulas of --math=latex stay as they are, their underscores, asterisks and
backslashes being LaTeX: $$…$$, and $…$ by pandoc's rule (the opening dollar followed by a
non-space, the closing one preceded by a non-space and followed by no digit; a dollar after a
backslash is text). A missing or unreadable output gives an empty page, which fails its tests.
Runs in rayopdf-eval-omni:1 (standard library only).
"""

import argparse
import html
import json
import os
import re

LABEL = {"heading": "heading", "table": "table", "list_item": "list", "code": "code", "footnote": "footnote",
         "image": "figure", "paragraph": "text"}
LINK = re.compile(r"!?\[([^\]]*)\]\((?:<[^>]*>|[^)\s]*)\)")
TAGS = re.compile(r"</?(?:sup|sub)>")
EMPHASIS = re.compile(r"(\*\*|__|\*|_|~~|`)")
ESCAPE = re.compile(r"\\([!-/:-@\[-`{-~])")
HEADING = re.compile(r"^#{1,6} ")
MARKER = re.compile(r"^(?:[-*+]|\d{1,9}[.)])(?: \[[ xX]\])? ")
NUMBER = re.compile(r"(\d{1,9})[.)] ")
NOTE = re.compile(r"^\[\^(\d+)\]: ")
MATH = re.compile(r"(?<!\\)\$\$.+?(?<!\\)\$\$|(?<![\\$])\$(?![\s$])[^$\n]*?(?<![\s\\])\$(?!\d)")


def md_text(markdown):
    """The text of a block's Markdown, its markup taken out."""
    lines = []
    for line in markdown.split("\n"):
        if line.startswith("```"):
            continue
        line = HEADING.sub("", line)
        line = MARKER.sub("", line)
        line = NOTE.sub(r"\1 ", line)
        lines.append(line)
    text = "\n".join(lines)
    held = []
    hold = lambda kept: held.append(kept) or f"\ue000{len(held) - 1}\ue001"
    text = MATH.sub(lambda m: hold(m.group(0)), text)
    text = LINK.sub(r"\1", text)
    text = TAGS.sub("", text)
    text = ESCAPE.sub(lambda m: hold(m.group(1)), text)
    text = EMPHASIS.sub("", text)
    text = re.sub("\ue000(\\d+)\ue001", lambda m: held[int(m.group(1))], text)
    return text.strip()


def table_html(block):
    rows = {}
    for cell in block.get("cells", []):
        rows.setdefault(cell["row"], []).append(cell)
    header = block.get("header_rows", 1)
    out = ["<table>"]
    for row in sorted(rows):
        out.append("<tr>")
        for cell in sorted(rows[row], key=lambda c: c["col"]):
            tag = "th" if row < header else "td"
            spans = ""
            if cell.get("col_span", 1) > 1:
                spans += f' colspan="{cell["col_span"]}"'
            if cell.get("row_span", 1) > 1:
                spans += f' rowspan="{cell["row_span"]}"'
            out.append(f"<{tag}{spans}>{html.escape(md_text(cell.get('text', '')))}</{tag}>")
        out.append("</tr>")
    out.append("</table>")
    return "".join(out)


def page_html(blocks):
    out = []
    open_list = None  # "ul" or "ol" while list items run
    next_number = None  # in an <ol>, the number the grader draws for its next item
    for block in blocks:
        kind = block["type"]
        if kind != "list_item" and open_list:
            out.append(f"</{open_list}>")
            open_list = None
        if kind == "list_item":
            tag = "ol" if block.get("ordered") else "ul"
            printed = NUMBER.match(block["markdown"]) if tag == "ol" else None
            number = int(printed[1]) if printed else None
            if open_list != tag or (number is not None and number != next_number):
                if open_list:
                    out.append(f"</{open_list}>")
                out.append(f'<ol start="{number}">' if number is not None else f"<{tag}>")
                open_list = tag
                next_number = number if number is not None else 1
            if tag == "ol":
                next_number += 1
            out.append(f"<li>{html.escape(md_text(block['markdown']))}</li>")
        elif kind == "heading":
            level = max(1, min(6, block.get("level", 1)))
            out.append(f"<h{level}>{html.escape(md_text(block['markdown']))}</h{level}>")
        elif kind in ("paragraph", "footnote"):
            out.append(f"<p>{html.escape(md_text(block['markdown']))}</p>")
        elif kind == "code":
            out.append(f"<pre>{html.escape(md_text(block['markdown']))}</pre>")
        elif kind == "table":
            out.append(table_html(block))
    if open_list:
        out.append(f"</{open_list}>")
    return "\n".join(out)


def page_blocks(document):
    page = document["pages"][0]
    number, width, height = page["page"], float(page["width"]), float(page["height"])
    out = []
    for block in document["blocks"]:
        label = LABEL.get(block["type"])
        boxes = [b["bbox"] for b in block.get("boxes", []) if b["page"] == number]
        if not label or not boxes or width <= 0 or height <= 0:
            continue
        x0 = min(b[0] for b in boxes); y0 = min(b[1] for b in boxes)
        x1 = max(b[2] for b in boxes); y1 = max(b[3] for b in boxes)
        clamp = lambda v: min(1.0, max(0.0, v))
        bbox = [clamp(x0 / width), clamp(1 - y1 / height), clamp(x1 / width), clamp(1 - y0 / height)]
        out.append({"label": label, "bbox": bbox, "text": md_text(block.get("markdown", ""))})
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pred", required=True, help="folder of RayoPDF's <id>.md (JSON documents)")
    parser.add_argument("--ids", required=True, help="the page ids, one a line")
    parser.add_argument("--out", required=True, help="the predictions/ folder to write")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(args.ids, encoding="utf-8") as f:
        ids = [line.strip() for line in f if line.strip()]
    read = 0
    for page_id in ids:
        prediction = {"html": "", "blocks": []}
        try:
            with open(os.path.join(args.pred, f"{page_id}.md"), encoding="utf-8") as f:
                document = json.load(f)
            prediction = {"html": page_html(document["blocks"]), "blocks": page_blocks(document)}
            read += 1
        except (OSError, ValueError, KeyError, IndexError, TypeError):
            pass
        with open(os.path.join(args.out, f"{page_id}.json"), "w", encoding="utf-8") as f:
            json.dump({"pages": [prediction]}, f, ensure_ascii=False)
    print(f"omni_predict: {read} of {len(ids)} pages read")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
