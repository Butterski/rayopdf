#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Text-layer census over zipped PDF collections (OCR-need evidence).

Usage (inside rayopdf-eval-producers:1, --network none):
    python3 /opt/eval/census/census.py --zips /data --out /out [--jobs 6] [--limit N]

Every PDF member of every ``*.zip`` under ``--zips`` (sorted by zip name, then
member name) is extracted to a unique temp file, examined with poppler's
``pdfinfo``, ``pdffonts``, ``pdftotext`` and ``pdfimages`` (first 50 pages at
most, 60 s per tool call) and deleted again. Each examined page is classified
(first match wins):

1. ``image_only``      < 50 non-space chars and image coverage >= 0.8
2. ``empty_or_vector`` < 50 non-space chars
3. ``broken``          any of docling's four broken-text-layer checks
4. ``text_over_image`` image coverage >= 0.9
5. ``text``

Writes ``census.jsonl`` (one line per file, sorted by file) and
``summary.json`` to ``--out``. Thresholds are listed in ``THRESHOLDS`` and are
copied into ``summary.json``.
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
import unicodedata
import zipfile

MAX_PAGES = 50
TOOL_TIMEOUT_S = 60
TOOLS = ("zip", "pdfinfo", "pdffonts", "pdftotext", "pdfimages")
CLASSES = ("image_only", "empty_or_vector", "broken", "text_over_image", "text")
SCRIPTS = ("latin", "cyrillic", "greek", "cjk", "arabic", "hebrew", "indic", "other", "unknown")

THRESHOLDS = {
    "max_pages": MAX_PAGES,
    "tool_timeout_s": TOOL_TIMEOUT_S,
    "min_chars": 50,
    "image_only_coverage": 0.8,
    "text_over_image_coverage": 0.9,
    "junk_char_fraction": 0.03,
    "junk_line_fraction": 0.10,
    "junk_min_lines": 3,
    "accented_latin_fraction": 0.50,
    "accented_min_latin_letters": 50,
    "vowel_share_max": 0.25,
    "vowel_min_letters": 200,
    "vowel_word_len": [3, 20],
    "script_min_letters": 20,
    "image_types_counted": ["image", "stencil"],
}

# Producer family table: case-insensitive, first match wins, Producer tried
# before Creator. `tex` is matched as `(?<!i)tex(?!t)` so that "iText" and
# "Text" do not land in the TeX family.
FAMILIES = (
    ("ms-office", r"word|microsoft"),
    ("libreoffice", r"libreoffice|openoffice"),
    ("tex", r"pdftex|xetex|luatex|dvipdfm|(?<!i)tex(?!t)"),
    ("adobe", r"indesign|illustrator|acrobat|distiller|adobe"),
    ("chromium", r"skia|chrom"),
    ("apple", r"quartz|mac os"),
    ("ghostscript", r"ghostscript"),
    ("itext", r"itext"),
    ("scanner-ocr", r"abbyy|finereader|omnipage|readiris|scan|canon|xerox|ricoh|kodak|epson"
                    r"|konica|kyocera|brother|fujitsu|paperport"),
)
FAMILY_RES = tuple((name, re.compile(pat, re.IGNORECASE)) for name, pat in FAMILIES)

LATIN_WORD_RE = re.compile(r"[A-Za-z\u00C0-\u024F]+")
VOWELS = frozenset("aeiouy")
PAGE_SIZE_RE = re.compile(r"^Page\s+(\d+)\s+size:\s+([\d.]+)\s+x\s+([\d.]+)")
DOC_SIZE_RE = re.compile(r"^Page size:\s+([\d.]+)\s+x\s+([\d.]+)")
TYPE3_RE = re.compile(r"(?:^|\s)Type 3(?:\s|$)")


# ---------------------------------------------------------------- page text

def is_junk(ch):
    """U+FFFD, private use, C0/C1 controls except tab, newline, form feed."""
    o = ord(ch)
    if o == 0xFFFD:
        return True
    if o < 0x20:
        return ch not in "\t\n\f"
    if 0x80 <= o <= 0x9F:
        return True
    return 0xE000 <= o <= 0xF8FF or 0xF0000 <= o <= 0xFFFFD or 0x100000 <= o <= 0x10FFFD


def line_junk(line):
    """Junk chars on one line; a lone junk bullet starting the line is not counted."""
    stripped = line.lstrip()
    n = sum(1 for ch in stripped if is_junk(ch))
    if n and is_junk(stripped[0]) and (len(stripped) == 1 or stripped[1].isspace()):
        n -= 1
    return n


def is_latin_letter(ch):
    return ch.isalpha() and (ch.isascii() or 0x00C0 <= ord(ch) <= 0x024F)


def broken_checks(text):
    """Return the names of docling's broken-text-layer checks that fire on `text`."""
    fired = []
    chars = sum(1 for ch in text if not ch.isspace())
    lines = [ln for ln in text.split("\n") if ln.strip()]
    per_line = [line_junk(ln) for ln in lines]
    junk = sum(per_line)
    if chars and junk / chars >= THRESHOLDS["junk_char_fraction"]:
        fired.append("junk_chars")
    junk_lines = sum(1 for n in per_line if n)
    if (lines and junk_lines >= THRESHOLDS["junk_min_lines"]
            and junk_lines / len(lines) >= THRESHOLDS["junk_line_fraction"]):
        fired.append("junk_lines")
    latin = 0
    accented = 0
    for ch in text:
        if is_latin_letter(ch):
            latin += 1
            if ord(ch) >= 0x00C0:
                accented += 1
    if (latin >= THRESHOLDS["accented_min_latin_letters"]
            and accented / latin >= THRESHOLDS["accented_latin_fraction"]):
        fired.append("accented_latin")
    lo, hi = THRESHOLDS["vowel_word_len"]
    letters = 0
    vowels = 0
    for word in LATIN_WORD_RE.findall(text):
        if lo <= len(word) <= hi:
            letters += len(word)
            for ch in word:
                base = unicodedata.normalize("NFD", ch)[0].lower()
                if base in VOWELS:
                    vowels += 1
    if letters >= THRESHOLDS["vowel_min_letters"] and vowels / letters < THRESHOLDS["vowel_share_max"]:
        fired.append("vowel_share")
    return fired


def classify_page(text, coverage):
    """Page class per THRESHOLDS; first match wins."""
    chars = sum(1 for ch in text if not ch.isspace())
    if chars < THRESHOLDS["min_chars"]:
        return "image_only" if coverage >= THRESHOLDS["image_only_coverage"] else "empty_or_vector"
    if broken_checks(text):
        return "broken"
    if coverage >= THRESHOLDS["text_over_image_coverage"]:
        return "text_over_image"
    return "text"


def char_script(ch):
    o = ord(ch)
    if ch.isascii() or 0x00C0 <= o <= 0x024F or 0x1E00 <= o <= 0x1EFF:
        return "latin"
    if 0x0400 <= o <= 0x052F:
        return "cyrillic"
    if 0x0370 <= o <= 0x03FF or 0x1F00 <= o <= 0x1FFF:
        return "greek"
    if (0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF or 0xF900 <= o <= 0xFAFF
            or 0x20000 <= o <= 0x3134F or 0x3040 <= o <= 0x30FF or 0x31F0 <= o <= 0x31FF
            or 0xFF66 <= o <= 0xFF9F or 0xAC00 <= o <= 0xD7AF or 0x1100 <= o <= 0x11FF
            or 0x3130 <= o <= 0x318F):
        return "cjk"
    if (0x0600 <= o <= 0x06FF or 0x0750 <= o <= 0x077F or 0x08A0 <= o <= 0x08FF
            or 0xFB50 <= o <= 0xFDFF or 0xFE70 <= o <= 0xFEFF):
        return "arabic"
    if 0x0590 <= o <= 0x05FF or 0xFB1D <= o <= 0xFB4F:
        return "hebrew"
    if 0x0900 <= o <= 0x0DFF:
        return "indic"
    return "other"


def script_counts(text):
    counts = {}
    for ch in text:
        if ch.isalpha():
            s = char_script(ch)
            counts[s] = counts.get(s, 0) + 1
    return counts


def dominant(counts):
    if sum(counts.values()) < THRESHOLDS["script_min_letters"]:
        return None
    # Ties resolve by SCRIPTS order for determinism.
    return max(SCRIPTS[:-1], key=lambda s: (counts.get(s, 0), -SCRIPTS.index(s)))


def page_scripts(texts):
    per_page = [script_counts(t) for t in texts]
    total = {}
    for c in per_page:
        for k, v in c.items():
            total[k] = total.get(k, 0) + v
    doc = dominant(total) or "unknown"
    return [dominant(c) or doc for c in per_page]


def family_of(producer, creator):
    for field in (producer, creator):
        if field:
            for name, rx in FAMILY_RES:
                if rx.search(field):
                    return name
    if not producer and not creator:
        return "unknown"
    return "other"


# ---------------------------------------------------------------- tool output

def run_tool(args):
    """Run one poppler call. Returns (returncode|None on timeout, stdout bytes, stderr bytes)."""
    try:
        p = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, timeout=TOOL_TIMEOUT_S)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as exc:
        return None, exc.stdout or b"", exc.stderr or b""
    except OSError as exc:
        return -1, b"", str(exc).encode()


def failure_reason(rc, stderr):
    if rc is None:
        return "timeout"
    lines = [ln for ln in stderr.decode("utf-8", "replace").splitlines() if ln.strip()]
    tail = lines[-1].strip()[:200] if lines else ""
    return f"exit {rc}: {tail}" if tail else f"exit {rc}"


def parse_pdfinfo(out):
    info = {"sizes": {}, "doc_size": None}
    for line in out.decode("utf-8", "replace").splitlines():
        m = PAGE_SIZE_RE.match(line)
        if m:
            info["sizes"][int(m.group(1))] = float(m.group(2)) * float(m.group(3))
            continue
        m = DOC_SIZE_RE.match(line)
        if m:
            info["doc_size"] = float(m.group(1)) * float(m.group(2))
            continue
        key, sep, value = line.partition(":")
        if sep:
            info.setdefault(key.strip(), value.strip())
    return info


def parse_pdffonts(out):
    type3 = False
    non_unicode = False
    for line in out.decode("utf-8", "replace").splitlines()[2:]:
        tokens = line.split()
        if len(tokens) < 7:
            continue
        # Columns from the right: emb sub uni obj gen; type sits left of them.
        if tokens[-3] == "no":
            non_unicode = True
        if TYPE3_RE.search(" ".join(tokens[:-5])):
            type3 = True
    return type3, non_unicode


def parse_pdfimages(out):
    """Image area in pt^2 per page from `pdfimages -list`."""
    area = {}
    counted = THRESHOLDS["image_types_counted"]
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
        area[page] = area.get(page, 0.0) + (w / xppi * 72.0) * (h / yppi * 72.0)
    return area


# ---------------------------------------------------------------- per file

def empty_record():
    return {"ok": False, "failures": {}, "pages": None, "examined": 0, "producer": None,
            "creator": None, "family": None, "pdf_version": None, "encrypted": None,
            "tagged": None, "type3": None, "non_unicode_font": None,
            "page_classes": [], "page_scripts": []}


def examine_pdf(path):
    """Examine one PDF on disk; returns the record without the `file` key."""
    rec = empty_record()
    rc, out, err = run_tool(["pdfinfo", "-isodates", "-f", "1", "-l", str(MAX_PAGES), path])
    if rc != 0:
        rec["failures"]["pdfinfo"] = failure_reason(rc, err)
        if b"password" in err.lower():
            rec["encrypted"] = True
        return rec
    info = parse_pdfinfo(out)
    try:
        pages = int(info.get("Pages", ""))
    except ValueError:
        rec["failures"]["pdfinfo"] = "no Pages line"
        return rec
    rec["ok"] = True
    rec["pages"] = pages
    rec["producer"] = info.get("Producer", "")
    rec["creator"] = info.get("Creator", "")
    rec["family"] = family_of(rec["producer"], rec["creator"])
    rec["pdf_version"] = info.get("PDF version")
    rec["encrypted"] = info.get("Encrypted", "no").startswith("yes")
    rec["tagged"] = info.get("Tagged", "no").startswith("yes")
    n = min(pages, MAX_PAGES)
    if n <= 0:
        return rec
    rng = ["-f", "1", "-l", str(n)]

    rc, out, err = run_tool(["pdffonts", *rng, path])
    if rc == 0:
        rec["type3"], rec["non_unicode_font"] = parse_pdffonts(out)
    else:
        rec["failures"]["pdffonts"] = failure_reason(rc, err)

    rc, out, err = run_tool(["pdfimages", "-list", *rng, path])
    if rc == 0:
        image_area = parse_pdfimages(out)
    else:
        rec["failures"]["pdfimages"] = failure_reason(rc, err)
        image_area = {}

    rc, out, err = run_tool(["pdftotext", "-enc", "UTF-8", *rng, path, "-"])
    if rc != 0:
        rec["failures"]["pdftotext"] = failure_reason(rc, err)
    # Only pages terminated by a form feed are complete; the rest are not examined.
    texts = out.decode("utf-8", "replace").split("\f")[:-1][:n]

    for i, text in enumerate(texts, start=1):
        size = info["sizes"].get(i, info["doc_size"])
        coverage = min(1.0, image_area.get(i, 0.0) / size) if size else 0.0
        rec["page_classes"].append(classify_page(text, coverage))
    rec["page_scripts"] = page_scripts(texts)
    rec["examined"] = len(texts)
    return rec


_zip_cache = {}


def examine_member(task):
    zip_path, member = task
    label = f"{os.path.basename(zip_path)}:{member}"
    fd, tmp = tempfile.mkstemp(prefix="census-", suffix=".pdf")
    try:
        try:
            zf = _zip_cache.get(zip_path)
            if zf is None:
                zf = _zip_cache[zip_path] = zipfile.ZipFile(zip_path)
            with zf.open(member) as src, os.fdopen(fd, "wb") as dst:
                fd = None
                shutil.copyfileobj(src, dst, 1 << 20)
        except Exception as exc:  # corrupt member, CRC error, disk full
            if fd is not None:
                os.close(fd)
            rec = empty_record()
            rec["failures"]["zip"] = f"{type(exc).__name__}: {exc}"[:200]
            return {"file": label, **rec}
        return {"file": label, **examine_pdf(tmp)}
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


# ---------------------------------------------------------------- summary

def pct(n, d):
    return round(100.0 * n / d, 4) if d else None


def class_pcts(recs):
    pages = {c: 0 for c in CLASSES}
    docs = {c: 0 for c in CLASSES}
    examined_docs = 0
    for r in recs:
        if not r["page_classes"]:
            continue
        examined_docs += 1
        for c in r["page_classes"]:
            pages[c] += 1
        for c in set(r["page_classes"]):
            docs[c] += 1
    total = sum(pages.values())
    return (total, examined_docs,
            {c: pct(pages[c], total) for c in CLASSES},
            {c: pct(docs[c], examined_docs) for c in CLASSES})


def summarize(recs):
    processed = [r for r in recs if r["ok"]]
    failures = {t: 0 for t in TOOLS}
    for r in recs:
        for t in r["failures"]:
            failures[t] += 1
    total_pages, examined_docs, page_pct, doc_pct = class_pcts(processed)
    by_family = {}
    for fam in sorted({r["family"] for r in processed}):
        frs = [r for r in processed if r["family"] == fam]
        fp, _, fpp, fdp = class_pcts(frs)
        by_family[fam] = {"docs": len(frs), "pages": fp, "page_class_pct": fpp, "doc_class_pct": fdp}
    need = {}
    for r in processed:
        for c, s in zip(r["page_classes"], r["page_scripts"]):
            if c in ("image_only", "broken"):
                need[s] = need.get(s, 0) + 1
    n_need = sum(need.values())
    return {
        "schema": 1,
        "files_total": len(recs),
        "files_processed": len(processed),
        "docs_examined": examined_docs,
        "password_required": sum(1 for r in recs if not r["ok"] and r["encrypted"]),
        "failures": failures,
        "pages_total": sum(r["pages"] for r in processed),
        "pages_examined": total_pages,
        "page_class_pct": page_pct,
        "doc_class_pct": doc_pct,
        "by_family": by_family,
        "tagged_pct": pct(sum(1 for r in processed if r["tagged"]), len(processed)),
        "encrypted_pct": pct(sum(1 for r in processed if r["encrypted"]), len(processed)),
        "type3_pct": pct(sum(1 for r in processed if r["type3"]), len(processed)),
        "non_unicode_font_pct": pct(sum(1 for r in processed if r["non_unicode_font"]), len(processed)),
        "ocr_need_script_mix": {s: pct(need[s], n_need) for s in SCRIPTS if s in need},
        "R_upper": round(n_need / total_pages, 6) if total_pages else None,
        "thresholds": THRESHOLDS,
        "notes": {
            "doc_class_pct": "percent of docs with >= 1 examined page having >= 1 page of the class",
            "flags_pct": "tagged/encrypted/type3/non_unicode_font percent of files_processed; "
                         "fonts from pages 1..min(pages, max_pages)",
            "R_upper": "(image_only + broken) / pages_examined",
        },
    }


def list_tasks(zips_dir, limit):
    tasks = []
    for name in sorted(os.listdir(zips_dir)):
        if not name.lower().endswith(".zip"):
            continue
        path = os.path.join(zips_dir, name)
        with zipfile.ZipFile(path) as zf:
            members = sorted(i.filename for i in zf.infolist()
                             if not i.is_dir() and i.filename.lower().endswith(".pdf"))
        tasks.extend((path, m) for m in members)
    return tasks[:limit] if limit else tasks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zips", required=True, help="directory holding *.zip files of PDFs")
    ap.add_argument("--out", required=True, help="output directory for census.jsonl and summary.json")
    ap.add_argument("--jobs", type=int, default=6, help="worker processes (default 6)")
    ap.add_argument("--limit", type=int, default=0, help="examine only the first N PDF members")
    args = ap.parse_args()

    tasks = list_tasks(args.zips, args.limit)
    if not tasks:
        print(f"census: no PDF members in {args.zips}/*.zip", file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    recs = []
    with multiprocessing.Pool(max(1, args.jobs)) as pool:
        for i, rec in enumerate(pool.imap_unordered(examine_member, tasks, chunksize=1), start=1):
            recs.append(rec)
            if i % 50 == 0 or i == len(tasks):
                print(f"census: {i}/{len(tasks)}", file=sys.stderr, flush=True)
    recs.sort(key=lambda r: r["file"])
    with open(os.path.join(args.out, "census.jsonl"), "w", encoding="utf-8") as f:
        for r in recs:
            f.write(json.dumps(r, ensure_ascii=False, sort_keys=False) + "\n")
    summary = summarize(recs)
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"census: {summary['files_processed']}/{summary['files_total']} files processed, "
          f"{summary['pages_examined']} pages examined, R_upper={summary['R_upper']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
