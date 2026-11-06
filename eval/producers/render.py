#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render the synthetic Markdown fixtures to PDF with every producer and validate them.

Usage: render.py --fixtures /fixtures --out /out [--producers a,b] [--only ids] [--jobs 6]

/fixtures holds manifest.json and md/<id>.md. For each manifest fixture x producer
(minus exclusions) the fixture is copied to a temp dir together with img/{chart,icon,photo}.png,
the producer runs there with a 300 s timeout, and the PDF is written to
/out/<producer>/<fixture>.pdf. An item is valid when the producer exits 0, `qpdf --check`
exits 0 or 3, and the whitespace-collapsed `pdftotext -enc UTF-8` text contains both
sentinel sentences. /out/corpus.json lists every item; items rendered by earlier runs that
this run did not touch are kept. Runs inside rayopdf-eval-producers:1 with HOME=/tmp and
no network.
"""

import argparse
import concurrent.futures
import hashlib
import itertools
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_images import write_images  # noqa: E402

TIMEOUT_S = 300
PRODUCERS = (
    "rayomd", "rayomd-pdfua", "rayomd-tech", "xelatex", "xelatex-2col", "xelatex-eisvogel", "typst",
    "chromium", "weasyprint", "libreoffice", "libreoffice-tagged", "beamer",
)
EISVOGEL = "/opt/eval/templates/eisvogel.latex"
SLIDES_FIXTURE = "d06-slides"
SLIDES_PRODUCERS = ("beamer", "rayomd")
LO_TAGGED_FILTER = 'pdf:writer_pdf_Export:{"UseTaggedPDF":{"type":"boolean","value":"true"}}'

_worker_local = threading.local()
_worker_counter = itertools.count()


def worker_index():
    if not hasattr(_worker_local, "index"):
        _worker_local.index = next(_worker_counter)
    return _worker_local.index


def pandoc_std(src):
    return ["pandoc", src, "-f", "gfm", "-s", "--resource-path=."]


def producer_steps(producer, fixture_id, workdir, out, worker):
    """Return the list of commands for one producer; `out` is the PDF path to create."""
    src = f"{fixture_id}.md"
    if producer in ("rayomd", "rayomd-pdfua"):
        cmd = ["rayomd", "--export", src, out, "native", "modern", "normal", "--no-image-cache"]
        return [cmd + (["--pdfua"] if producer == "rayomd-pdfua" else [])]
    if producer == "rayomd-tech":
        return [["rayomd", "--export", src, out, "native", "tech", "compact", "--page-size=letter",
                 "--page-numbers", "--footnotes=page", "--compress", "--no-image-cache"]]
    if producer in ("xelatex", "xelatex-2col"):
        cmd = pandoc_std(src) + ["-o", out, "--pdf-engine=xelatex",
                                 "-V", "mainfont=DejaVu Sans", "-V", "monofont=DejaVu Sans Mono",
                                 "-V", "CJKmainfont=Noto Sans CJK SC", "-V", "geometry:margin=2.5cm"]
        if producer == "xelatex-2col":
            cmd += ["-V", "classoption=twocolumn", "-V", "fontsize=10pt", "-V", "geometry:margin=1.8cm"]
        return [cmd]
    if producer == "xelatex-eisvogel":
        # READoc's GitHub pipeline (pandoc + the Eisvogel template); listings-style code blocks.
        return [pandoc_std(src) + ["-o", out, "--pdf-engine=xelatex", f"--template={EISVOGEL}",
                                   "--syntax-highlighting=idiomatic",
                                   "-V", "mainfont=DejaVu Sans", "-V", "monofont=DejaVu Sans Mono",
                                   "-V", "CJKmainfont=Noto Sans CJK SC"]]
    if producer == "typst":
        return [pandoc_std(src) + ["-o", out, "--pdf-engine=typst", "-V", "mainfont=DejaVu Sans"]]
    if producer == "chromium":
        return [
            pandoc_std(src) + ["--mathml", "--embed-resources", "--metadata", f"title={fixture_id}",
                               "-o", "page.html"],
            ["chromium", "--headless", "--no-sandbox", "--disable-gpu", "--disable-dev-shm-usage",
             f"--user-data-dir=/tmp/chrome-{worker}", "--no-pdf-header-footer",
             f"--print-to-pdf={out}", f"file://{workdir}/page.html"],
        ]
    if producer == "weasyprint":
        return [
            pandoc_std(src) + ["--metadata", f"title={fixture_id}", "-o", "page.html"],
            ["weasyprint", "page.html", out],
        ]
    if producer in ("libreoffice", "libreoffice-tagged"):
        target = LO_TAGGED_FILTER if producer == "libreoffice-tagged" else "pdf"
        return [
            ["pandoc", src, "-f", "gfm", "--resource-path=.", "-o", "doc.docx"],
            ["soffice", f"-env:UserInstallation=file:///tmp/lo-{worker}", "--headless",
             "--convert-to", target, "--outdir", ".", "doc.docx"],
            ["mv", "doc.pdf", out],
        ]
    if producer == "beamer":
        return [["pandoc", src, "-f", "gfm", "-t", "beamer", "--pdf-engine=xelatex",
                 "-V", "mainfont=DejaVu Sans", "-o", out]]
    raise ValueError(f"unknown producer {producer}")


def run(cmd, cwd, timeout):
    """Run cmd in its own process group; kill the whole group on timeout."""
    proc = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, start_new_session=True)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        out, err = proc.communicate()
        return None, out.decode("utf-8", "replace"), err.decode("utf-8", "replace")
    return proc.returncode, out.decode("utf-8", "replace"), err.decode("utf-8", "replace")


def last_line(*texts):
    """Last non-empty line of the first non-empty text; a TeX `! ...` error line wins."""
    for text in texts:
        lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
        if lines:
            tex_errors = [ln for ln in lines if ln.startswith("! ")]
            return (tex_errors[-1] if tex_errors else lines[-1])[:300]
    return ""


def doc_class(fixture_id, producer):
    prefix = fixture_id.split("-", 1)[0]
    if prefix == "d02":
        return "office"
    if prefix == "d04":
        return "academic"
    if prefix in ("d03", "d05", "d06"):
        return "book"
    if producer.startswith("libreoffice"):
        return "office"
    if producer == "xelatex-2col":
        return "academic"
    return "renderer"


def pdfinfo(path):
    code, out, _ = run(["pdfinfo", path], None, 60)
    info = {}
    if code == 0:
        for line in out.splitlines():
            key, sep, value = line.partition(":")
            if sep:
                info[key.strip()] = value.strip()
    return info


def sentinels(fixture_id):
    tag = fixture_id.split("-", 1)[0].upper()
    return f"Sentinel start {tag}.", f"Sentinel end {tag}."


def validate(fixture_id, pdf):
    """Return (reason or None)."""
    code, out, err = run(["qpdf", "--check", pdf], None, 120)
    if code not in (0, 3):
        return f"qpdf --check exit {code}: {last_line(err, out)}"
    code, out, err = run(["pdftotext", "-enc", "UTF-8", pdf, "-"], None, 120)
    if code != 0:
        return f"pdftotext exit {code}: {last_line(err)}"
    text = " ".join(out.split())
    missing = [s for s in sentinels(fixture_id) if s not in text]
    if missing:
        return "sentinel missing: " + " / ".join(missing)
    return None


def render_item(fixture, producer, fixtures_dir, img_dir, out_dir):
    fixture_id = fixture["id"]
    worker = worker_index()
    final = os.path.join(out_dir, producer, f"{fixture_id}.pdf")
    if os.path.exists(final):
        os.remove(final)
    item = {
        "id": f"{producer}/{fixture_id}", "fixture": fixture_id, "producer": producer,
        "class": doc_class(fixture_id, producer), "tagged": None, "pages": None,
        "sha256": None, "valid": False, "reason": None, "holdout": producer == "weasyprint",
    }
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix=f"render-{worker}-") as work:
        shutil.copyfile(fixture["path"], os.path.join(work, f"{fixture_id}.md"))
        shutil.copytree(img_dir, os.path.join(work, "img"))
        tmp_pdf = os.path.join(work, "out.pdf")
        reason = None
        for cmd in producer_steps(producer, fixture_id, work, tmp_pdf, worker):
            remaining = TIMEOUT_S - (time.monotonic() - started)
            code, out, err = run(cmd, work, max(remaining, 1))
            if code is None:
                reason = f"{cmd[0]} timeout after {TIMEOUT_S} s"
                break
            if code != 0:
                reason = f"{cmd[0]} exit {code}: {last_line(err, out)}"
                break
        if reason is None and not os.path.isfile(tmp_pdf):
            reason = f"no PDF produced: {last_line(err, out)}"
        if os.path.isfile(tmp_pdf):
            os.makedirs(os.path.dirname(final), exist_ok=True)
            shutil.copyfile(tmp_pdf, final)
    if os.path.isfile(final):
        with open(final, "rb") as f:
            item["sha256"] = hashlib.sha256(f.read()).hexdigest()
        info = pdfinfo(final)
        if info.get("Pages", "").isdigit():
            item["pages"] = int(info["Pages"])
        if "Tagged" in info:
            item["tagged"] = info["Tagged"].lower().startswith("yes")
        if reason is None:
            reason = validate(fixture_id, final)
    item["valid"] = reason is None
    item["reason"] = reason
    item["seconds"] = round(time.monotonic() - started, 2)
    return item


def load_fixtures(fixtures_dir):
    with open(os.path.join(fixtures_dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    fixtures = []
    for entry in manifest["fixtures"]:
        candidates = [os.path.join(fixtures_dir, entry["file"]),
                      os.path.join(fixtures_dir, "md", os.path.basename(entry["file"]))]
        path = next((p for p in candidates if os.path.isfile(p)), None)
        fixtures.append({"id": entry["id"], "file": entry["file"], "path": path,
                         "exclude": set(entry.get("exclude", []))})
    return fixtures


def wanted(fixture, producer):
    if producer in fixture["exclude"]:
        return False
    if fixture["id"] == SLIDES_FIXTURE:
        return producer in SLIDES_PRODUCERS
    return producer != "beamer"


def read_versions():
    try:
        with open("/opt/eval/versions.json", encoding="utf-8") as f:
            return json.load(f)
    except OSError:
        return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--producers", help="comma-separated subset of: " + ",".join(PRODUCERS))
    ap.add_argument("--only", help="comma-separated fixture ids")
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args()

    producers = PRODUCERS
    if args.producers:
        producers = tuple(p for p in args.producers.split(",") if p)
        unknown = [p for p in producers if p not in PRODUCERS]
        if unknown:
            ap.error(f"unknown producers: {', '.join(unknown)}")
    fixtures = load_fixtures(args.fixtures)
    if args.only:
        only = set(args.only.split(","))
        missing = only - {f["id"] for f in fixtures}
        if missing:
            ap.error(f"unknown fixtures: {', '.join(sorted(missing))}")
        fixtures = [f for f in fixtures if f["id"] in only]
    absent = [f"{f['id']} ({f['file']})" for f in fixtures if f["path"] is None]
    if absent:
        ap.error(f"fixture files not found: {', '.join(absent)}")

    jobs = [(f, p) for f in fixtures for p in producers if wanted(f, p)]
    os.makedirs(args.out, exist_ok=True)
    started = time.monotonic()
    items = []
    with tempfile.TemporaryDirectory(prefix="render-img-") as img_dir:
        write_images(img_dir)
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(args.jobs, 1)) as pool:
            futures = [pool.submit(render_item, f, p, args.fixtures, img_dir, args.out) for f, p in jobs]
            for fut in concurrent.futures.as_completed(futures):
                item = fut.result()
                items.append(item)
                status = "ok" if item["valid"] else f"INVALID ({item['reason']})"
                print(f"[{len(items)}/{len(jobs)}] {item['id']} {item['seconds']}s {status}", flush=True)

    corpus_path = os.path.join(args.out, "corpus.json")
    done = {i["id"] for i in items}
    manifest_ids = {f"{p}/{f['id']}" for f in load_fixtures(args.fixtures) for p in PRODUCERS if wanted(f, p)}
    try:
        with open(corpus_path, encoding="utf-8") as f:
            previous = json.load(f).get("items", [])
    except (OSError, ValueError):
        previous = []
    for old in previous:
        if old["id"] not in done and old["id"] in manifest_ids and (
                not old.get("valid") or os.path.isfile(os.path.join(args.out, old["id"] + ".pdf"))):
            items.append(old)
    items.sort(key=lambda i: i["id"])
    for i in items:
        i.pop("seconds", None)
    with open(corpus_path, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "versions": read_versions(), "items": items}, f, indent=1, ensure_ascii=False)
        f.write("\n")
    valid = sum(1 for i in items if i["valid"])
    print(f"rendered {len(jobs)} items in {time.monotonic() - started:.1f} s; "
          f"corpus.json: {valid}/{len(items)} valid", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
