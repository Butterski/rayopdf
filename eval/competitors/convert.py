#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Shared competitor adapter: convert listed PDFs to Markdown with one tool.

Runs inside a rayopdf-eval-<tool>:1 image (see eval/docker/tool-*.Dockerfile).

Usage:
  convert.py --tool T --in /in --list /in/list.txt --out /out
             [--timeout S] [--warmup 0|1] [--repeat N] [--cmd TEMPLATE]

For each id in the list (path relative to --in, without ".pdf") converts
/in/<id>.pdf to /out/<id>.md and appends a record to /out/results.jsonl:
  {"id","status":"ok|error|timeout|empty","ms":float|null,"error":str|null}
ms is the median over --repeat runs; the first repeat's output is kept.
/out/meta.json records the tool, the timing kind and /opt/eval/versions.json.
The opendataloader tool converts the whole list in one JVM and also writes
/out/batch.json {"total_ms","docs","pages":null}; its per-document ms is null.

In-process tools (markitdown, pymupdf, edgeparse, docling, docling-ocr) run in one
persistent worker process, timed there around the conversion call only. A document that
runs past --timeout kills the worker; the next document starts a fresh one, so
a tool's stuck threads cannot spoil the documents after it. docling-ocr is docling with
its default OCR (it runs in the docling image); tesseract renders pages with pdftoppm
and writes tesseract's plain text. kordoc runs one Node process a document
(/opt/kordoc/run.mjs: `parse()` and its Markdown, as kordoc's own benchmark driver does) with
its OCR off; kordoc-ocr is kordoc's default, which reads pages without text and large images
with the OCR models baked into the image (it runs in the kordoc image).
"""

import argparse
import concurrent.futures
import json
import multiprocessing
import os
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

TIMING = {
    "pdftotext": "fresh-process",
    "markitdown": "in-process",
    "pymupdf": "in-process",
    "opendataloader": "batch",
    "liteparse": "fresh-process",
    "edgeparse": "in-process",
    "docling": "in-process",
    "docling-ocr": "in-process",
    "tesseract": "fresh-process",
    "kordoc": "fresh-process",
    "kordoc-ocr": "fresh-process",
    "cli": "fresh-process",
}
ERR_KEEP = 500


def Tail(text):
    if text is None:
        return None
    if isinstance(text, bytes):
        text = text.decode("utf-8", "replace")
    text = text.strip()
    return text[-ERR_KEEP:] if text else None


class SubprocessError(Exception):
    pass


def RunCommand(argv, timeout, cwd=None):
    """Runs argv with captured output; returns elapsed ms. Raises on failure."""
    t0 = time.perf_counter()
    try:
        r = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        raise TimeoutError("timeout after %ss" % timeout)
    ms = (time.perf_counter() - t0) * 1000.0
    if r.returncode != 0:
        raise SubprocessError("exit %d: %s" % (r.returncode, Tail(r.stdout) or ""))
    return ms


# ---- per-tool converters: (pdf_path, md_path, timeout) -> ms -------------------


class InProcess:
    """Wraps a text-returning conversion callable; runs inside the worker process."""

    def __init__(self, fn):
        self.fn = fn

    def __call__(self, pdf, md, timeout=None):
        t0 = time.perf_counter()
        text = self.fn(pdf)
        ms = (time.perf_counter() - t0) * 1000.0
        with open(md, "w", encoding="utf-8") as f:
            f.write(text or "")
        return ms


def _Worker(tool, cmd, conn):
    """Worker process of an in-process tool: imports it once, then converts each job sent."""
    conv, api = MAKERS[tool](argparse.Namespace(cmd=cmd))
    conn.send(api)
    while True:
        job = conn.recv()
        if job is None:
            return
        try:
            conn.send((conv(*job), None))
        except Exception as e:  # reported per document by the parent
            conn.send((None, Tail("%s: %s" % (type(e).__name__, e))))


class Isolated:
    """Runs an in-process tool in a persistent, killable worker process."""

    def __init__(self, tool, cmd):
        self.tool, self.cmd, self.proc = tool, cmd, None
        self.api = self._Start()

    def _Start(self):
        ctx = multiprocessing.get_context("spawn")
        self.conn, child = ctx.Pipe()
        self.proc = ctx.Process(target=_Worker, args=(self.tool, self.cmd, child))
        self.proc.start()
        child.close()
        try:
            return self.conn.recv()
        except EOFError:
            self._Stop()
            raise RuntimeError("%s worker failed to start" % self.tool)

    def _Stop(self):
        self.proc.kill()
        self.proc.join()
        code, self.proc = self.proc.exitcode, None
        return code

    def __call__(self, pdf, md, timeout):
        if self.proc is None:
            self._Start()
        self.conn.send((pdf, md))
        if not self.conn.poll(timeout):
            self._Stop()
            raise TimeoutError("timeout after %ss" % timeout)
        try:
            ms, error = self.conn.recv()
        except EOFError:
            raise RuntimeError("worker died (exit %s)" % self._Stop())
        if error is not None:
            raise RuntimeError(error)
        return ms

    def Close(self):
        if self.proc is not None:
            self.conn.send(None)
            self.proc.join()
            self.proc = None


def MakeMarkitdown(args):
    from markitdown import MarkItDown
    conv = MarkItDown()
    return InProcess(lambda p: conv.convert(p).text_content), "markitdown.MarkItDown"


def MakePymupdf(args):
    import pymupdf
    if hasattr(pymupdf.Document, "to_markdown"):
        return InProcess(lambda p: pymupdf.open(p).to_markdown()), "pymupdf.Document.to_markdown"
    import pymupdf4llm
    return InProcess(lambda p: pymupdf4llm.to_markdown(p)), "pymupdf4llm.to_markdown"


def MakeEdgeparse(args):
    import edgeparse
    # edgeparse 0.3.2: convert() returns the Markdown string itself (no .markdown attribute).
    return InProcess(lambda p: edgeparse.convert(p, format="markdown")), "edgeparse.convert(format=markdown)"


def MakeDocling(args, ocr=False):
    try:
        from docling.datamodel.base_models import InputFormat
        from docling.datamodel.pipeline_options import PdfPipelineOptions
        from docling.document_converter import DocumentConverter, PdfFormatOption
        # docling-ocr keeps docling's default OCR options (OcrAutoOptions: RapidOCR with the
        # models downloaded into /models at image build), which OCR bitmap regions only.
        opts = PdfPipelineOptions(artifacts_path="/models", do_ocr=ocr)
        conv = DocumentConverter(format_options={InputFormat.PDF: PdfFormatOption(pipeline_options=opts)})
    except (ImportError, TypeError, AttributeError) as e:
        if ocr:
            raise SystemExit("docling API unavailable for docling-ocr: %s" % e)
        sys.stderr.write("docling API unavailable (%s); using CLI fallback\n" % e)
        return DoclingCli, "docling CLI (fallback)"
    api = "docling.DocumentConverter" + (" (do_ocr=True, default OcrAutoOptions)" if ocr else "")
    return InProcess(lambda p: conv.convert(p).document.export_to_markdown()), api


def MakeDoclingOcr(args):
    return MakeDocling(args, ocr=True)


def DoclingCli(pdf, md, timeout=None):
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        ms = RunCommand(["docling", pdf, "--to", "md", "--no-ocr", "--device", "cpu",
                         "--artifacts-path", "/models", "--output", tmp], timeout)
        src = os.path.join(tmp, os.path.splitext(os.path.basename(pdf))[0] + ".md")
        shutil.move(src, md)
    return ms


def MakePdftotext(args):
    return (lambda pdf, md, t: RunCommand(["pdftotext", "-enc", "UTF-8", pdf, md], t)), "pdftotext"


def MakeLiteparse(args):
    def Conv(pdf, md, t):
        return RunCommand(["lit", "parse", pdf, "--format", "markdown", "--no-ocr",
                           "--image-mode", "off", "-q", "-o", md], t)
    return Conv, "lit parse"


TESSERACT_DPI = 300


def MakeTesseract(args):
    """pdftoppm renders every page at 300 dpi gray; tesseract (--psm 1: page segmentation with
    orientation and script detection, eng) reads each page in its own single-threaded process
    (OMP_THREAD_LIMIT=1), OMP_NUM_THREADS pages at a time, and writes plain text; pages are
    joined by blank lines. Pages are independent, so the text does not depend on the job count."""
    os.environ["OMP_THREAD_LIMIT"] = "1"
    jobs = max(1, int(os.environ.get("OMP_NUM_THREADS", "1") or "1"))

    def Conv(pdf, md, t):
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            start = time.perf_counter()
            RunCommand(["pdftoppm", "-r", str(TESSERACT_DPI), "-gray", "-png", pdf,
                        os.path.join(tmp, "p")], t)
            pages = sorted((n for n in os.listdir(tmp) if n.endswith(".png")),
                           key=lambda n: int(n.rsplit("-", 1)[1][:-4]))
            left = None if t is None else max(1.0, t - (time.perf_counter() - start))

            def Page(n):
                RunCommand(["tesseract", os.path.join(tmp, pages[n]), os.path.join(tmp, "out%d" % n),
                            "-l", "eng", "--psm", "1", "--dpi", str(TESSERACT_DPI)], left)
                with open(os.path.join(tmp, "out%d.txt" % n), encoding="utf-8", errors="replace") as f:
                    return f.read().replace("\f", "").strip("\n")

            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                texts = list(pool.map(Page, range(len(pages))))
            with open(md, "w", encoding="utf-8") as f:
                f.write("\n\n".join(texts) + "\n")
            return (time.perf_counter() - start) * 1000.0
    return Conv, "pdftoppm -r 300 -gray + tesseract -l eng --psm 1 per page (plain text)"


def MakeKordoc(args, ocr=False):
    os.environ["KORDOC_OFFLINE"] = "1"
    # The OCR models are only found where KORDOC_MODEL_CACHE points; `kordoc` also passes
    # ocr: false, so an empty cache only makes that doubly sure.
    os.environ["KORDOC_MODEL_CACHE"] = "/opt/kordoc/models" if ocr else "/tmp/kordoc-no-models"
    mode = "default" if ocr else "off"

    def Conv(pdf, md, t):
        return RunCommand(["node", "/opt/kordoc/run.mjs", pdf, md, mode], t)
    return Conv, "kordoc parse() %s" % ("default (OCR models present)" if ocr else "{ ocr: false }")


def MakeKordocOcr(args):
    return MakeKordoc(args, ocr=True)


def MakeCli(args):
    if not args.cmd:
        raise SystemExit("--cmd is required for --tool cli")
    os.environ["PATH"] = "/opt/tool:" + os.environ.get("PATH", "")
    template = shlex.split(args.cmd)

    def Conv(pdf, md, t):
        argv = [a.replace("{in}", pdf).replace("{out}", md) for a in template]
        return RunCommand(argv, t)
    return Conv, "cli: " + args.cmd


MAKERS = {
    "pdftotext": MakePdftotext,
    "markitdown": MakeMarkitdown,
    "pymupdf": MakePymupdf,
    "liteparse": MakeLiteparse,
    "edgeparse": MakeEdgeparse,
    "docling": MakeDocling,
    "docling-ocr": MakeDoclingOcr,
    "tesseract": MakeTesseract,
    "kordoc": MakeKordoc,
    "kordoc-ocr": MakeKordocOcr,
    "cli": MakeCli,
}


# ---- driver --------------------------------------------------------------------


def ReadList(path):
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def Paths(args, doc_id):
    md = os.path.join(args.out, doc_id + ".md")
    os.makedirs(os.path.dirname(md), exist_ok=True)
    return os.path.join(args.inp, doc_id + ".pdf"), md


def Status(md):
    try:
        with open(md, "rb") as f:
            return "ok" if f.read().strip() else "empty"
    except OSError:
        return None


def ConvertOne(conv, args, doc_id):
    pdf, md = Paths(args, doc_id)
    times = []
    try:
        for i in range(args.repeat):
            target = md if i == 0 else md + ".repeat.tmp"
            times.append(conv(pdf, target, args.timeout))
            if i > 0 and os.path.exists(target):
                os.remove(target)
    except TimeoutError as e:
        return {"id": doc_id, "status": "timeout", "ms": None, "error": Tail(str(e))}
    except Exception as e:  # a failing document never aborts the run
        return {"id": doc_id, "status": "error", "ms": None,
                "error": Tail("%s: %s" % (type(e).__name__, e))}
    status = Status(md)
    if status is None:
        return {"id": doc_id, "status": "error", "ms": None, "error": "no output written"}
    return {"id": doc_id, "status": status, "ms": statistics.median(times), "error": None}


def RunBatch(args, ids, results):
    """opendataloader: one JVM over the whole list; inputs flattened to unique names."""
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        src = os.path.join(tmp, "in")
        os.makedirs(src)
        for n, doc_id in enumerate(ids):
            shutil.copyfile(os.path.join(args.inp, doc_id + ".pdf"), os.path.join(src, "d%06d.pdf" % n))
        if args.warmup and ids:
            wsrc = os.path.join(tmp, "warm")
            os.makedirs(wsrc)
            shutil.copyfile(os.path.join(src, "d000000.pdf"), os.path.join(wsrc, "d000000.pdf"))
            try:
                RunCommand(["opendataloader-pdf", wsrc, "-o", os.path.join(tmp, "warm-out"),
                            "-f", "markdown", "--image-output", "off", "-q"], args.timeout)
            except Exception:
                pass
        totals, error = [], None
        timeout = args.timeout * max(1, len(ids))
        for i in range(args.repeat):
            outdir = os.path.join(tmp, "out%d" % i)
            try:
                totals.append(RunCommand(["opendataloader-pdf", src, "-o", outdir, "-f", "markdown",
                                          "--image-output", "off", "-q"], timeout))
            except Exception as e:
                error = Tail("%s: %s" % (type(e).__name__, e))
                break
        produced = {}
        for root, _, files in os.walk(os.path.join(tmp, "out0")):
            for name in files:
                if name.endswith(".md"):
                    produced[name[:-3]] = os.path.join(root, name)
        for n, doc_id in enumerate(ids):
            _, md = Paths(args, doc_id)
            got = produced.get("d%06d" % n)
            if got is None:
                rec = {"id": doc_id, "status": "error", "ms": None, "error": error or "no output written"}
            else:
                shutil.move(got, md)
                rec = {"id": doc_id, "status": Status(md), "ms": None, "error": None}
            results.write(json.dumps(rec) + "\n")
        with open(os.path.join(args.out, "batch.json"), "w") as f:
            json.dump({"total_ms": statistics.median(totals) if totals else None,
                       "docs": len(ids), "pages": None, "error": error}, f)
    return "opendataloader-pdf CLI batch"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True, choices=sorted(TIMING))
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--list", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--warmup", type=int, choices=(0, 1), default=0)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--cmd")
    args = ap.parse_args()
    if args.repeat < 1:
        ap.error("--repeat must be >= 1")

    ids = ReadList(args.list)
    os.makedirs(args.out, exist_ok=True)
    try:
        with open("/opt/eval/versions.json", encoding="utf-8") as f:
            versions = json.load(f)
    except (OSError, ValueError):
        versions = None

    counts = {}
    with open(os.path.join(args.out, "results.jsonl"), "w", encoding="utf-8") as results:
        if args.tool == "opendataloader":
            api = RunBatch(args, ids, results)
        else:
            if TIMING[args.tool] == "in-process":
                conv = Isolated(args.tool, args.cmd)
                api = conv.api
            else:
                conv, api = MAKERS[args.tool](args)
            if args.warmup and ids:
                with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
                    try:
                        conv(os.path.join(args.inp, ids[0] + ".pdf"), os.path.join(tmp, "w.md"), args.timeout)
                    except Exception:
                        pass
            for doc_id in ids:
                rec = ConvertOne(conv, args, doc_id)
                counts[rec["status"]] = counts.get(rec["status"], 0) + 1
                results.write(json.dumps(rec) + "\n")
                results.flush()
            if isinstance(conv, Isolated):
                conv.Close()
    with open(os.path.join(args.out, "meta.json"), "w", encoding="utf-8") as f:
        json.dump({"tool": args.tool, "timing": TIMING[args.tool], "api": api,
                   "repeat": args.repeat, "warmup": args.warmup, "timeout": args.timeout,
                   "versions": versions}, f, indent=1)
    sys.stderr.write("%s: %d ids, %s\n" % (args.tool, len(ids), json.dumps(counts)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
