#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the `multiscript` OCR set: image-only PDFs of original prose in nine scripts.

Usage: multiscript.py --fixtures /eval/fixtures/multiscript --out /out [--jobs 6]

Each <script>.md under --fixtures (latin, cyrillic, hangul, arabic, hebrew, devanagari, kannada,
tamil, mixed; UTF-8, NFC, a title line and paragraphs separated by blank lines) is typeset with
Pillow and libraqm (HarfBuzz shaping) onto gray A4 pages at the item's dpi: 2.2 cm margins, Noto
fonts at 11 pt (the title at 14 pt), line pitch 1.35 times the font size, 0.6 body line between
paragraphs, greedy wrapping on spaces by measured width. Arabic and Hebrew paragraphs run right
to left and are right-aligned; in mixed.md every paragraph takes the font and direction of its
dominant script. Characters outside a script font's own Unicode blocks (digits and punctuation
in Arabic, Hebrew and Indic text, which the Noto Hebrew faces lack) are set in the Noto Latin
face of the same style. FriBidi (libfribidi through ctypes, the library libraqm uses) gives every
character its bidi embedding level; runs of one level and one font are put in visual order by
rule L2 of the Unicode bidi algorithm and shaped one at a time, in their own direction.

Items, ids <script>-<variant>:
  s300-sans-clean, s300-serif-clean, s200-sans-clean   typeset at that dpi in that style
  s300-sans-noise      scan.py's degrade('noise'), seeded with the SHA-256 of the id
  rot90, rot180, rot270   the s300-sans-clean pages turned clockwise by that angle; the page size
                          follows the turned image (no /Rotate key)
The serif style is Noto Serif (Noto Serif CJK KR for Hangul); Arabic, which has no Noto Serif
face, uses Noto Naskh Arabic. A script without a serif face gets no serif item.

Every item writes
  OUT/pdf/<id>.pdf        one 8-bit gray Flate image per page (scan.py's writer), page size =
                          pixels / dpi * 72 pt
  OUT/pgm/<id>/p<N>.pgm   binary PGM of exactly the image on page N
  OUT/gt/<id>.md          the fixture, byte for byte
  OUT/gt/<id>/p<N>.txt    the text on page N: one line per paragraph, or per part of a paragraph
                          split at the line break where the page ended, in logical order
and is valid when `pdfimages -list` shows one gray 8-bit Flate image per page with the page's
pixel size at the item's dpi. OUT/multiscript.json lists the items, the font files (with
SHA-256) and the Pillow, libraqm, HarfBuzz, FriBidi and FreeType versions. The output is
byte-identical across runs. Runs inside rayopdf-eval-producers:1 with no network; scan.py must
sit next to this file.
"""

import argparse
import concurrent.futures
import ctypes
import functools
import hashlib
import json
import os
import re
import shutil
import subprocess
import unicodedata

import numpy as np
import PIL
from PIL import Image, ImageDraw, ImageFont, features

from scan import build_pdf, degrade, encode_flate, seed_of

SCRIPTS = ("latin", "cyrillic", "hangul", "arabic", "hebrew", "devanagari", "kannada", "tamil",
           "mixed")
NOTO = "/usr/share/fonts/noto/"
NOTO_CJK = "/usr/share/fonts/noto-cjk/"
# (font file, face index in a collection) per script and style; face 1 of the CJK collections is KR.
FONTS = {
    "latin": {"sans": (NOTO + "NotoSans-Regular.ttf", 0),
              "serif": (NOTO + "NotoSerif-Regular.ttf", 0)},
    "cyrillic": {"sans": (NOTO + "NotoSans-Regular.ttf", 0),
                 "serif": (NOTO + "NotoSerif-Regular.ttf", 0)},
    "hangul": {"sans": (NOTO_CJK + "NotoSansCJK-Regular.ttc", 1),
               "serif": (NOTO_CJK + "NotoSerifCJK-Regular.ttc", 1)},
    "arabic": {"sans": (NOTO + "NotoSansArabic-Regular.ttf", 0),
               "serif": (NOTO + "NotoNaskhArabic-Regular.ttf", 0)},
    "hebrew": {"sans": (NOTO + "NotoSansHebrew-Regular.ttf", 0),
               "serif": (NOTO + "NotoSerifHebrew-Regular.ttf", 0)},
    "devanagari": {"sans": (NOTO + "NotoSansDevanagari-Regular.ttf", 0),
                   "serif": (NOTO + "NotoSerifDevanagari-Regular.ttf", 0)},
    "kannada": {"sans": (NOTO + "NotoSansKannada-Regular.ttf", 0),
                "serif": (NOTO + "NotoSerifKannada-Regular.ttf", 0)},
    "tamil": {"sans": (NOTO + "NotoSansTamil-Regular.ttf", 0),
              "serif": (NOTO + "NotoSerifTamil-Regular.ttf", 0)},
}
# Unicode blocks set in the script's own font; every other character except the space and the
# zero-width (non-)joiners uses the Latin face. Scripts not listed set everything in their font.
OWN_BLOCKS = {
    "arabic": ((0x0600, 0x06FF), (0x0750, 0x077F), (0x08A0, 0x08FF), (0xFB50, 0xFDFF),
               (0xFE70, 0xFEFF)),
    "hebrew": ((0x0590, 0x05FF), (0xFB1D, 0xFB4F)),
    "devanagari": ((0x0900, 0x097F), (0xA8E0, 0xA8FF)),
    "kannada": ((0x0C80, 0x0CFF),),
    "tamil": ((0x0B80, 0x0BFF), (0x11FC0, 0x11FFF)),
}
# Letters that decide a mixed.md paragraph's script; anything else counts as Latin.
SCRIPT_BLOCKS = {
    "cyrillic": ((0x0400, 0x052F),),
    "hangul": ((0x1100, 0x11FF), (0x3130, 0x318F), (0xAC00, 0xD7AF)),
    **OWN_BLOCKS,
}
RTL_SCRIPTS = ("arabic", "hebrew")
KEEP_IN_RUN = " \u200c\u200d"

PAGE_MM = (210.0, 297.0)
MARGIN_CM = 2.2
BODY_PT, TITLE_PT = 11.0, 14.0
LINE_PITCH = 1.35          # line advance, in font sizes
BASELINE = 1.0             # baseline below the top of a line box, in font sizes
PARA_GAP = 0.6             # extra space before a paragraph, in body line advances
RENDERS = (("sans", 300), ("serif", 300), ("sans", 200))
ROTATIONS = ((90, Image.Transpose.ROTATE_270), (180, Image.Transpose.ROTATE_180),
             (270, Image.Transpose.ROTATE_90))
TIMEOUT_S = 300

# ---- bidi ---------------------------------------------------------------------------------

_FRIBIDI = ctypes.CDLL("libfribidi.so.0")
_FRIBIDI.fribidi_log2vis.restype = ctypes.c_int8
_FRIBIDI.fribidi_log2vis.argtypes = [
    ctypes.POINTER(ctypes.c_uint32), ctypes.c_int, ctypes.POINTER(ctypes.c_uint32),
    ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int8)]
FRIBIDI_PAR_LTR, FRIBIDI_PAR_RTL = 0x110, 0x111


def bidi_levels(text, rtl):
    """Embedding level of every character of one line (FriBidi, paragraph direction given)."""
    n = len(text)
    chars = (ctypes.c_uint32 * n)(*map(ord, text))
    base = ctypes.c_uint32(FRIBIDI_PAR_RTL if rtl else FRIBIDI_PAR_LTR)
    visual = (ctypes.c_uint32 * n)()
    levels = (ctypes.c_int8 * n)()
    if not _FRIBIDI.fribidi_log2vis(chars, n, ctypes.byref(base), visual, None, None, levels):
        raise RuntimeError("fribidi_log2vis failed")
    return list(levels)


# ---- typesetting --------------------------------------------------------------------------


@functools.lru_cache(maxsize=None)
def load_font(path, index, size_px):
    return ImageFont.truetype(path, size_px, index=index, layout_engine=ImageFont.Layout.RAQM)


def in_blocks(cp, blocks):
    return any(lo <= cp <= hi for lo, hi in blocks)


def paragraph_script(text):
    counts = {}
    for c in text:
        if unicodedata.category(c)[0] in "LM":
            cp = ord(c)
            script = next((s for s, b in SCRIPT_BLOCKS.items() if in_blocks(cp, b)), "latin")
            counts[script] = counts.get(script, 0) + 1
    return max(sorted(counts), key=counts.get) if counts else "latin"


class Paragraph:
    def __init__(self, text, script, size_pt):
        self.text = text
        self.script = script
        self.rtl = script in RTL_SCRIPTS
        self.size_pt = size_pt


def read_fixture(path, script):
    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")
    if not unicodedata.is_normalized("NFC", text):
        raise ValueError(f"{path} is not NFC")
    blocks = [" ".join(b.split()) for b in re.split(r"\n[ \t]*\n", text)]
    blocks = [b for b in blocks if b]
    paras = [Paragraph(b, paragraph_script(b) if script == "mixed" else script,
                       TITLE_PT if i == 0 else BODY_PT) for i, b in enumerate(blocks)]
    return raw, paras


def visual_runs(text, para, fonts):
    """[(font, text, direction, width)] of one line, left to right."""
    levels = bidi_levels(text, para.rtl)
    own = OWN_BLOCKS.get(para.script)
    keys = [0 if own is None or c in KEEP_IN_RUN or in_blocks(ord(c), own) else 1 for c in text]
    runs = []
    start = 0
    for i in range(1, len(text) + 1):
        if i == len(text) or levels[i] != levels[start] or keys[i] != keys[start]:
            runs.append((levels[start], keys[start], text[start:i]))
            start = i
    # Rule L2: from the highest level down to the lowest odd one, reverse every maximal sequence
    # of runs at that level or above.
    lowest_odd = min(levels) | 1
    for level in range(max(levels), lowest_odd - 1, -1):
        i = 0
        while i < len(runs):
            if runs[i][0] < level:
                i += 1
                continue
            j = i
            while j < len(runs) and runs[j][0] >= level:
                j += 1
            runs[i:j] = runs[i:j][::-1]
            i = j
    out = []
    for level, key, run in runs:
        direction = "rtl" if level % 2 else "ltr"
        out.append((fonts[key], run, direction, fonts[key].getlength(run, direction=direction)))
    return out


def wrap(para, fonts, width):
    """Greedy: the longest prefix of words whose measured width fits, then the next line."""
    words = para.text.split(" ")
    lines = []
    line = words[0]
    for word in words[1:]:
        candidate = line + " " + word
        if sum(r[3] for r in visual_runs(candidate, para, fonts)) <= width:
            line = candidate
        else:
            lines.append(line)
            line = word
    lines.append(line)
    return lines


def typeset(paras, style, dpi):
    """(page images, page texts) of the paragraphs in one style at one dpi."""
    page_w = round(PAGE_MM[0] / 25.4 * dpi)
    page_h = round(PAGE_MM[1] / 25.4 * dpi)
    margin = round(MARGIN_CM / 2.54 * dpi)
    width = page_w - 2 * margin
    body_advance = BODY_PT * dpi / 72.0 * LINE_PITCH
    pages = [[]]            # per page: (paragraph index, line text, runs, x, baseline)
    y = float(margin)
    for index, para in enumerate(paras):
        size = para.size_pt * dpi / 72.0
        advance = size * LINE_PITCH
        fonts = (load_font(*FONTS[para.script][style], size),
                 load_font(*FONTS["latin"][style], size))
        if pages[-1]:
            y += PARA_GAP * body_advance
        for line in wrap(para, fonts, width):
            if pages[-1] and y + advance > page_h - margin:
                pages.append([])
                y = float(margin)
            runs = visual_runs(line, para, fonts)
            line_w = sum(r[3] for r in runs)
            if line_w > width:
                raise RuntimeError(f"word wider than the text block: {line[:40]!r}")
            x = page_w - margin - line_w if para.rtl else float(margin)
            pages[-1].append((index, line, runs, x, y + BASELINE * size))
            y += advance
    images, texts = [], []
    for lines in pages:
        img = Image.new("L", (page_w, page_h), 255)
        draw = ImageDraw.Draw(img)
        for _, _, runs, x, baseline in lines:
            for font, run, direction, run_w in runs:
                draw.text((x, baseline), run, font=font, fill=0, anchor="ls", direction=direction)
                x += run_w
        images.append(img)
        parts = []
        for index, line, _, _, _ in lines:
            if parts and parts[-1][0] == index:
                parts[-1][1].append(line)
            else:
                parts.append((index, [line]))
        texts.append("".join(" ".join(words) + "\n" for _, words in parts))
    # The page texts must give back every paragraph exactly.
    joined = {}
    for lines in pages:
        for index, line, _, _, _ in lines:
            joined.setdefault(index, []).append(line)
    if [" ".join(joined[i]) for i in range(len(paras))] != [p.text for p in paras]:
        raise RuntimeError("page texts do not reproduce the paragraphs")
    return images, texts


# ---- items --------------------------------------------------------------------------------


def write_pgm(img, path):
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % img.size + img.tobytes())


def validate(pdf_path, images, dpi):
    proc = subprocess.run(["pdfimages", "-list", pdf_path], capture_output=True,
                          timeout=TIMEOUT_S)
    if proc.returncode != 0:
        return "pdfimages failed: " + proc.stderr.decode("utf-8", "replace")[-200:]
    rows = [line.split() for line in proc.stdout.decode("utf-8", "replace").splitlines()[2:]
            if line.strip()]
    if len(rows) != len(images):
        return f"pdfimages lists {len(rows)} images for {len(images)} pages"
    for n, (row, img) in enumerate(zip(rows, images), 1):
        want = [str(n), "image", str(img.size[0]), str(img.size[1]), "gray", "1", "8", "image"]
        if len(row) < 14 or [row[0]] + row[2:9] != want or row[12:14] != [str(dpi)] * 2:
            return f"page {n}: pdfimages row {' '.join(row)}"
    return None


def write_item(out, item_id, images, texts, dpi, fixture_raw):
    """Writes the item's files; returns the validation failure or None."""
    pgm_dir = os.path.join(out, "pgm", item_id)
    gt_dir = os.path.join(out, "gt", item_id)
    for d in (pgm_dir, gt_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    pages = []
    for n, (img, text) in enumerate(zip(images, texts), 1):
        entries, data = encode_flate(img, "gray")
        pages.append((img.size[0], img.size[1], entries, data))
        write_pgm(img, os.path.join(pgm_dir, f"p{n}.pgm"))
        with open(os.path.join(gt_dir, f"p{n}.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    pdf_path = os.path.join(out, "pdf", item_id + ".pdf")
    with open(pdf_path, "wb") as f:
        f.write(build_pdf(pages, dpi, None))
    with open(os.path.join(out, "gt", item_id + ".md"), "wb") as f:
        f.write(fixture_raw)
    return validate(pdf_path, images, dpi)


def run_job(job):
    """One typesetting (script, style, dpi) and the items made from it."""
    script, style, dpi, fixture, out = job
    variants = [(f"s{dpi}-{style}-clean", "clean", 0, None)]
    if (style, dpi) == ("sans", 300):
        variants.append(("s300-sans-noise", "noise", 0, None))
        variants += [(f"rot{deg}", "clean", deg, op) for deg, op in ROTATIONS]
    records = []
    try:
        raw, paras = read_fixture(fixture, script)
        images, texts = typeset(paras, style, dpi)
    except (OSError, ValueError, RuntimeError) as e:
        images, texts, failure = None, None, f"{type(e).__name__}: {e}"[:300]
    for variant, degradation, rotation, op in variants:
        item_id = f"{script}-{variant}"
        record = {"id": item_id, "script": script, "font": style, "dpi": dpi,
                  "degradation": degradation, "rotation": rotation, "pages": None,
                  "pdf": f"pdf/{item_id}.pdf", "gt": f"gt/{item_id}.md", "valid": False,
                  "reason": None}
        if images is None:
            record["reason"] = failure
            records.append(record)
            continue
        pages = images
        if degradation == "noise":
            rng = np.random.default_rng(seed_of(item_id))
            pages = [degrade(img, "noise", dpi, rng) for img in images]
        elif op is not None:
            pages = [img.transpose(op) for img in images]
        try:
            record["reason"] = write_item(out, item_id, pages, texts, dpi, raw)
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as e:
            record["reason"] = f"{type(e).__name__}: {e}"[:300]
        record["pages"] = len(pages)
        record["valid"] = record["reason"] is None
        records.append(record)
    return records


def versions():
    proc = subprocess.run(["pacman", "-Q", "python-pillow", "libraqm", "harfbuzz", "fribidi",
                           "freetype2", "python-numpy", "zlib", "poppler", "noto-fonts",
                           "noto-fonts-cjk"], capture_output=True, text=True)
    packages = dict(line.split(None, 1) for line in proc.stdout.splitlines() if " " in line)
    files = sorted({path for styles in FONTS.values() for path, _ in styles.values()})
    font_sha = {}
    for path in files:
        with open(path, "rb") as f:
            font_sha[os.path.basename(path)] = hashlib.sha256(f.read()).hexdigest()
    return {
        "pillow": PIL.__version__,
        "raqm": features.version_feature("raqm"),
        "freetype2": features.version_module("freetype2"),
        "pacman": packages,
        "fonts": {script: {style: os.path.basename(path) + (f"#{index}" if index else "")
                           for style, (path, index) in styles.items()}
                  for script, styles in FONTS.items()},
        "font_sha256": font_sha,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fixtures", required=True, help="directory with <script>.md")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args()

    for sub in ("pdf", "pgm", "gt"):
        os.makedirs(os.path.join(args.out, sub), exist_ok=True)
    jobs = []
    for script in SCRIPTS:
        fixture = os.path.join(args.fixtures, script + ".md")
        _, paras = read_fixture(fixture, script)
        for style, dpi in RENDERS:
            if all(style in FONTS[p.script] for p in paras):
                jobs.append((script, style, dpi, fixture, args.out))
    items = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for records in pool.map(run_job, jobs):
            for r in records:
                print(f"{r['id']}: {r['pages']} page(s) {'ok' if r['valid'] else r['reason']}",
                      flush=True)
            items += records
    items.sort(key=lambda r: r["id"])
    with open(os.path.join(args.out, "multiscript.json"), "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "versions": versions(), "items": items}, f, indent=1,
                  ensure_ascii=False)
        f.write("\n")
    valid = sum(1 for r in items if r["valid"])
    print(f"multiscript.json: {valid}/{len(items)} valid", flush=True)
    return 0 if valid == len(items) else 1


if __name__ == "__main__":
    raise SystemExit(main())
