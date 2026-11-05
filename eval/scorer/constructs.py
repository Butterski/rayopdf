#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Construct scorer: Markdown-fidelity metrics between ground-truth and predicted Markdown.

Usage:
    constructs.py --gt-dir G --pred-dir P --out F.json [--jobs N]

Every ``*.md`` under G is paired with the file at the same relative path under P; a missing
prediction counts as an empty document. Both sides are parsed with ``pandoc -f gfm -t json``
and walked into typed items (headings, list items, code blocks, quote paragraphs, alerts,
links, strong/emph/strike/inline code, sup/sub, footnotes, images, math). Per category the
GT x prediction pairs are scored with ``rapidfuzz.fuzz.ratio`` on normalised text, pairs
>= 90 (>= 95 for code blocks) are matched greedily one-to-one by descending score, and
precision/recall/F1 plus attribute accuracies on the matched pairs are reported. Math is
recall-only (``fuzz.partial_ratio`` of the GT math's Unicode rendering, from pandoc's plain
writer or, where that falls back to TeX, the text of pandoc's MathML, against the plain-text
rendering of the prediction, >= 80). Images report count recall, alt match rate and the
rate of predicted image targets that exist relative to the prediction file.

Output: ``{"schema":1,"docs":{relpath: D},"totals": D}``; ``totals`` is the micro-average.
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import unicodedata
import urllib.parse

from bs4 import BeautifulSoup
from rapidfuzz import fuzz

READER = "gfm+footnotes+tex_math_dollars+alerts+task_lists+strikeout+pipe_tables+autolink_bare_uris"
CATEGORIES = (
    "heading", "list_item", "code_block", "quote_para", "alert", "link", "strong", "emph",
    "strike", "code_inline", "sup", "sub", "footnote",
)
ATTRS = {
    "heading": ("heading.level",),
    "list_item": ("list_item.depth", "list_item.ordered", "list_item.task"),
    "code_block": ("code_block.exact",),
    "alert": ("alert.type",),
    "link": ("link.url",),
}
ALERT_TYPES = ("note", "tip", "important", "warning", "caution")
MATCH_THRESHOLD = 90.0
CODE_MATCH_THRESHOLD = 95.0
MATH_THRESHOLD = 80.0
MATH_SEP = "RAYOPDFMATHSEPARATOR"
TASK_MARKS = {"\u2610": "unchecked", "\u2612": "checked"}
RAW_TAG = re.compile(r"^<\s*(/?)\s*(sup|sub)\b[^>]*>$", re.IGNORECASE)

PANDOC_TIMEOUT = 120.0


# ----------------------------------------------------------------------------- text helpers

def norm(text):
    """NFKC, collapse whitespace, strip."""
    return " ".join(unicodedata.normalize("NFKC", text).split())


def norm_code(text):
    """Code-block normalisation: expand tabs to 4, rstrip lines, drop trailing blank lines."""
    lines = [line.expandtabs(4).rstrip() for line in text.split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def norm_url(url):
    """Lowercase scheme and host, strip one trailing '/'."""
    url = url.strip()
    try:
        parts = urllib.parse.urlsplit(url)
        url = urllib.parse.urlunsplit(
            (parts.scheme.lower(), parts.netloc.lower(), parts.path, parts.query, parts.fragment))
    except ValueError:
        pass
    if url.endswith("/"):
        url = url[:-1]
    return url


def inline_text(inlines):
    out = []
    for node in inlines:
        t = node.get("t")
        c = node.get("c")
        if t == "Str":
            out.append(c)
        elif t in ("Space", "SoftBreak", "LineBreak"):
            out.append(" ")
        elif t in ("Code", "Math"):
            out.append(c[1])
        elif t in ("Emph", "Strong", "Strikeout", "Underline", "Superscript", "Subscript",
                   "SmallCaps"):
            out.append(inline_text(c))
        elif t in ("Link", "Image", "Span"):
            out.append(inline_text(c[1]))
        elif t == "Quoted":
            q = '"' if c[0].get("t") == "DoubleQuote" else "'"
            out.append(q + inline_text(c[1]) + q)
        elif t == "Cite":
            out.append(inline_text(c[1]))
        # RawInline and Note contribute no text.
    return "".join(out)


def block_text(blocks):
    parts = []
    for block in blocks:
        t = block.get("t")
        c = block.get("c")
        if t in ("Para", "Plain", "Header"):
            parts.append(inline_text(c if t != "Header" else c[2]))
        elif t == "LineBlock":
            parts.extend(inline_text(line) for line in c)
        elif t == "CodeBlock":
            parts.append(c[1])
        elif t in ("BlockQuote",):
            parts.append(block_text(c))
        elif t == "Div":
            parts.append(block_text(c[1]))
        elif t in ("BulletList",):
            parts.extend(block_text(item) for item in c)
        elif t == "OrderedList":
            parts.extend(block_text(item) for item in c[1])
        elif t == "DefinitionList":
            for term, defs in c:
                parts.append(inline_text(term))
                parts.extend(block_text(d) for d in defs)
        elif t == "Figure":
            parts.append(block_text(c[2]))
        elif t == "Table":
            parts.append(block_text(list(iter_table_blocks(c))))
    return " ".join(p for p in parts if p)


def iter_table_blocks(c):
    """Yield every block inside a pandoc Table (caption, head, bodies, foot)."""
    _attr, caption, _specs, head, bodies, foot = c
    yield from caption[1]
    rows = list(head[1])
    for body in bodies:
        rows.extend(body[2])
        rows.extend(body[3])
    rows.extend(foot[1])
    for row in rows:
        for cell in row[1]:
            yield from cell[4]


# ----------------------------------------------------------------------------- AST walker

class Collector:
    def __init__(self):
        self.items = {cat: [] for cat in CATEGORIES}
        self.images = []  # (alt, target)
        self.math = []  # Math nodes (pandoc JSON)
        self.api = [1, 23, 1]
        self.doc = None  # parsed pandoc JSON document

    def add(self, cat, text, **attrs):
        self.items[cat].append({"text": norm(text), **attrs})

    # blocks ------------------------------------------------------------------
    def blocks(self, blocks, quote=False, list_depth=0):
        for block in blocks:
            self.block(block, quote, list_depth)

    def block(self, block, quote, list_depth):
        t = block.get("t")
        c = block.get("c")
        if t in ("Para", "Plain"):
            if quote:
                self.add("quote_para", inline_text(c))
            self.inlines(c)
        elif t == "Header":
            self.add("heading", inline_text(c[2]), level=c[0])
            self.inlines(c[2])
        elif t == "CodeBlock":
            self.add("code_block", c[1], code=norm_code(c[1]))
        elif t == "BlockQuote":
            self.blocks(c, True, list_depth)
        elif t in ("BulletList", "OrderedList"):
            ordered = t == "OrderedList"
            for item in (c[1] if ordered else c):
                self.list_item(item, ordered, list_depth + 1)
        elif t == "Div":
            classes = c[0][1]
            alert = next((k for k in classes if k in ALERT_TYPES), None)
            if alert is not None:
                body = [b for b in c[1]
                        if not (b.get("t") == "Div" and "title" in b["c"][0][1])]
                self.add("alert", block_text(body), type=alert)
                self.blocks(body, False, list_depth)
            else:
                self.blocks(c[1], quote, list_depth)
        elif t == "LineBlock":
            for line in c:
                self.inlines(line)
        elif t == "DefinitionList":
            for term, defs in c:
                self.inlines(term)
                for d in defs:
                    self.blocks(d, quote, list_depth)
        elif t == "Figure":
            self.blocks(c[1][1], quote, list_depth)
            self.blocks(c[2], quote, list_depth)
        elif t == "Table":
            self.blocks(list(iter_table_blocks(c)), quote, list_depth)
        # RawBlock, HorizontalRule: nothing.

    def list_item(self, blocks, ordered, depth):
        task = "none"
        text = ""
        if blocks:
            first = blocks[0]
            if first.get("t") in ("Plain", "Para"):
                inl = first["c"]
                if inl and inl[0].get("t") == "Str" and inl[0]["c"] in TASK_MARKS:
                    task = TASK_MARKS[inl[0]["c"]]
                    inl = inl[1:]
                    if inl and inl[0].get("t") == "Space":
                        inl = inl[1:]
                text = inline_text(inl)
            else:
                text = block_text([first])
        self.add("list_item", text, depth=depth, ordered=ordered, task=task)
        self.blocks(blocks, False, depth)

    # inlines -----------------------------------------------------------------
    def inlines(self, inlines):
        for i, node in enumerate(inlines):
            t = node.get("t")
            c = node.get("c")
            if t == "Link":
                self.add("link", inline_text(c[1]), url=c[2][0])
                self.inlines(c[1])
            elif t == "Image":
                self.images.append((norm(inline_text(c[1])), c[2][0]))
            elif t == "Strong":
                self.add("strong", inline_text(c))
                self.inlines(c)
            elif t == "Emph":
                self.add("emph", inline_text(c))
                self.inlines(c)
            elif t == "Strikeout":
                self.add("strike", inline_text(c))
                self.inlines(c)
            elif t == "Code":
                self.add("code_inline", c[1])
            elif t == "Superscript":
                self.add("sup", inline_text(c))
                self.inlines(c)
            elif t == "Subscript":
                self.add("sub", inline_text(c))
                self.inlines(c)
            elif t == "RawInline":
                self.raw_tag(inlines, i)
            elif t == "Note":
                self.add("footnote", block_text(c))
                self.blocks(c)
            elif t == "Math":
                self.math.append(node)
            elif t in ("Underline", "SmallCaps", "Span"):
                self.inlines(c if t != "Span" else c[1])
            elif t == "Quoted":
                self.inlines(c[1])
            elif t == "Cite":
                self.inlines(c[1])

    def raw_tag(self, inlines, i):
        fmt, raw = inlines[i]["c"]
        if fmt.lower() != "html":
            return
        m = RAW_TAG.match(raw.strip())
        if not m or m.group(1):
            return
        name = m.group(2).lower()
        for j in range(i + 1, len(inlines)):
            node = inlines[j]
            if node.get("t") != "RawInline" or node["c"][0].lower() != "html":
                continue
            m2 = RAW_TAG.match(node["c"][1].strip())
            if m2 and m2.group(2).lower() == name:
                if m2.group(1):
                    self.add(name, inline_text(inlines[i + 1:j]))
                return


# ----------------------------------------------------------------------------- pandoc

def run_pandoc(args, data):
    try:
        proc = subprocess.run(["pandoc", *args], input=data.encode("utf-8"),
                              capture_output=True, timeout=PANDOC_TIMEOUT)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.decode("utf-8", errors="replace")


def read_text(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def parse(text):
    """Return a Collector for Markdown text; malformed input yields an empty collector."""
    col = Collector()
    if not text.strip():
        return col
    out = run_pandoc(["-f", READER, "-t", "json"], text)
    if out is None:
        return col
    try:
        doc = json.loads(out)
        col.api = doc.get("pandoc-api-version", col.api)
        col.doc = doc
        col.blocks(doc.get("blocks", []))
    except (ValueError, TypeError, KeyError, IndexError, AttributeError):
        return Collector()
    return col


def math_texts(math_nodes, api):
    """Unicode rendering of each Math node.

    One `pandoc -f json -t plain` call renders all nodes (one Para each). pandoc's plain writer
    falls back to raw TeX for constructs it cannot linearise (\\frac, \\int with limits,
    matrices); those nodes instead get the text content of pandoc's MathML rendering
    (annotation excluded), from one `pandoc -f json -t html --math-method=mathml` call.
    """
    if not math_nodes:
        return []
    tex = [norm(node["c"][1]) for node in math_nodes]

    def render(nodes, writer):
        blocks = []
        for k, node in enumerate(nodes):
            if k:
                blocks.append({"t": "Para", "c": [{"t": "Str", "c": MATH_SEP}]})
            blocks.append({"t": "Para", "c": [node]})
        doc = {"pandoc-api-version": api, "meta": {}, "blocks": blocks}
        return run_pandoc(["-f", "json", *writer, "--wrap=none"], json.dumps(doc))

    out = render(math_nodes, ["-t", "plain"])
    parts = out.split(MATH_SEP) if out is not None else []
    if len(parts) != len(math_nodes):
        parts = ["$" + t + "$" for t in tex]
    texts = [norm(p) for p in parts]
    failed = [k for k, t in enumerate(texts) if t.startswith("$") or "\\" in t]
    if failed:
        html = render([math_nodes[k] for k in failed], ["-t", "html", "--math-method=mathml"])
        maths = BeautifulSoup(html or "", "html.parser").find_all("math")
        if len(maths) == len(failed):
            for k, m in zip(failed, maths):
                for ann in m.find_all(["annotation", "annotation-xml"]):
                    ann.decompose()
                texts[k] = norm(m.get_text(""))
        else:
            for k in failed:
                texts[k] = tex[k]
    return texts


def prediction_plain(col):
    """Plain-text rendering of a parsed prediction (`pandoc -f json -t plain`).

    Math nodes in the prediction are first replaced by their math_texts() rendering, so math
    the prediction writes as TeX is linearised exactly like the GT side.
    """
    for node, text in zip(col.math, math_texts(col.math, col.api)):
        node["t"] = "Str"
        node["c"] = text
    return run_pandoc(["-f", "json", "-t", "plain", "--wrap=none"], json.dumps(col.doc))


# ----------------------------------------------------------------------------- scoring

def rate(num, den):
    return num / den if den else None


def match(gt, pred, threshold):
    """Greedy one-to-one matching by descending fuzz.ratio; returns [(gi, pi)]."""
    pairs = []
    for gi, g in enumerate(gt):
        for pi, p in enumerate(pred):
            s = fuzz.ratio(g["text"], p["text"], score_cutoff=threshold)
            if s >= threshold:
                pairs.append((-s, gi, pi))
    pairs.sort()
    used_g, used_p, out = set(), set(), []
    for _s, gi, pi in pairs:
        if gi in used_g or pi in used_p:
            continue
        used_g.add(gi)
        used_p.add(pi)
        out.append((gi, pi))
    return out


def attr_values(name, g, p):
    if name == "heading.level":
        return g["level"] == p["level"]
    if name == "list_item.depth":
        return g["depth"] == p["depth"]
    if name == "list_item.ordered":
        return g["ordered"] == p["ordered"]
    if name == "list_item.task":
        return g["task"] == p["task"]
    if name == "alert.type":
        return g["type"] == p["type"]
    if name == "link.url":
        return norm_url(g["url"]) == norm_url(p["url"])
    if name == "code_block.exact":
        return g["code"] == p["code"]
    raise KeyError(name)


def finish_category(d):
    d["precision"] = rate(d["matched"], d["pred"])
    d["recall"] = rate(d["matched"], d["gt"])
    # F1 = 2*matched/(gt+pred): 0 when only one side has items, null when both are empty.
    d["f1"] = rate(2 * d["matched"], d["gt"] + d["pred"])
    for a in d["attr"].values():
        a["acc"] = rate(a["correct"], a["total"])


def finish(doc):
    for d in doc["categories"].values():
        finish_category(d)
    m = doc["math"]
    m["recall"] = rate(m["found"], m["gt"])
    im = doc["image"]
    im["count_recall"] = rate(min(im["pred"], im["gt"]), im["gt"])
    im["alt_match"] = rate(im["alt_matched"], im["gt"])
    im["file_exists"] = rate(im["files_exist"], im["pred"])
    return doc


def empty_doc():
    return {
        "categories": {
            cat: {"gt": 0, "pred": 0, "matched": 0,
                  "attr": {a: {"correct": 0, "total": 0} for a in ATTRS.get(cat, ())}}
            for cat in CATEGORIES
        },
        "math": {"gt": 0, "found": 0},
        "image": {"gt": 0, "pred": 0, "alt_matched": 0, "files_exist": 0},
    }


def score_pair(gt_path, pred_path):
    gt_text = read_text(gt_path)
    pred_text = read_text(pred_path) if os.path.isfile(pred_path) else ""
    gt = parse(gt_text)
    pred = parse(pred_text)
    doc = empty_doc()
    for cat in CATEGORIES:
        g_items, p_items = gt.items[cat], pred.items[cat]
        d = doc["categories"][cat]
        d["gt"], d["pred"] = len(g_items), len(p_items)
        thr = CODE_MATCH_THRESHOLD if cat == "code_block" else MATCH_THRESHOLD
        pairs = match(g_items, p_items, thr)
        d["matched"] = len(pairs)
        for name in ATTRS.get(cat, ()):
            a = d["attr"][name]
            a["total"] = len(pairs)
            a["correct"] = sum(1 for gi, pi in pairs if attr_values(name, g_items[gi], p_items[pi]))
    # math (recall only)
    gt_math = math_texts(gt.math, gt.api)
    doc["math"]["gt"] = len(gt_math)
    if gt_math and pred.doc is not None:
        plain = norm(prediction_plain(pred) or "")
        if plain:
            doc["math"]["found"] = sum(
                1 for g in gt_math if fuzz.partial_ratio(g, plain) >= MATH_THRESHOLD)
    # images
    im = doc["image"]
    im["gt"], im["pred"] = len(gt.images), len(pred.images)
    g_alts = [{"text": alt} for alt, _ in gt.images]
    p_alts = [{"text": alt} for alt, _ in pred.images]
    im["alt_matched"] = len(match(g_alts, p_alts, MATCH_THRESHOLD))
    base = os.path.dirname(os.path.abspath(pred_path))
    exists = 0
    for _alt, target in pred.images:
        target = urllib.parse.unquote(target.split("#", 1)[0].split("?", 1)[0])
        if target and "://" not in target and not target.startswith("data:"):
            if os.path.isfile(os.path.join(base, target)):
                exists += 1
    im["files_exist"] = exists
    return finish(doc)


def totals(docs):
    tot = empty_doc()
    for doc in docs:
        for cat, d in doc["categories"].items():
            t = tot["categories"][cat]
            for k in ("gt", "pred", "matched"):
                t[k] += d[k]
            for name, a in d["attr"].items():
                t["attr"][name]["correct"] += a["correct"]
                t["attr"][name]["total"] += a["total"]
        for k in ("gt", "found"):
            tot["math"][k] += doc["math"][k]
        for k in ("gt", "pred", "alt_matched", "files_exist"):
            tot["image"][k] += doc["image"][k]
    return finish(tot)


def score_dirs(gt_dir, pred_dir, jobs=None):
    rels = []
    for root, _dirs, files in os.walk(gt_dir):
        for name in files:
            if name.endswith(".md"):
                rels.append(os.path.relpath(os.path.join(root, name), gt_dir))
    rels.sort()
    jobs = max(1, jobs or os.cpu_count() or 1)
    args = [(os.path.join(gt_dir, r), os.path.join(pred_dir, r)) for r in rels]
    if jobs == 1 or len(args) <= 1:
        results = [score_pair(g, p) for g, p in args]
    else:
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(score_pair, *zip(*args)))
    docs = {rel.replace(os.sep, "/"): res for rel, res in zip(rels, results)}
    return {"schema": 1, "docs": docs, "totals": totals(results)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--gt-dir", required=True)
    ap.add_argument("--pred-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=len(os.sched_getaffinity(0)))
    args = ap.parse_args()
    if not os.path.isdir(args.gt_dir):
        print(f"constructs.py: no such directory: {args.gt_dir}", file=sys.stderr)
        return 2
    result = score_dirs(args.gt_dir, args.pred_dir, args.jobs)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, sort_keys=True)
        f.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
