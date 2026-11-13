#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""RayoPDF evaluation harness driver.

Every third-party tool runs inside the Docker images built from eval/docker/. This script only
builds those images, lays out files on the host, starts containers and writes the report.

Usage:
  python3 tools/eval.py all [--resume]
  python3 tools/eval.py images [--only producers,scorer,olmocr,readoc,omni,pdftotext,...]
  python3 tools/eval.py fetch [--only odl,olmocr,readoc,readoc-dev,readoc-val,readoc-dev2,safedocs,doclaynet,omni]
  python3 tools/eval.py corpus [--suite synthetic|scanned|multiscript] [--producers a,b] [--only ids] [--jobs 6]
  python3 tools/eval.py convert --tool T --suite S [--cmd TEMPLATE | --profile default|declared] [--bin-dir DIR]
  python3 tools/eval.py score --selftest
  python3 tools/eval.py score --suite S [--tools a,b]
  python3 tools/eval.py speed --tool T [--cmd TEMPLATE] [--bin-dir DIR]
  python3 tools/eval.py census [--jobs 8] [--limit N]
  python3 tools/eval.py report

Every subcommand takes --root (default benchmark-output/eval). Tools: pdftotext, markitdown,
pymupdf, opendataloader, liteparse, edgeparse, docling, docling-ocr, tesseract, kordoc (OCR off),
kordoc-ocr (kordoc's default, OCR models present), cli. Suites: synthetic, odl, olmocr,
olmocr-math, readoc, readoc-dev, readoc-val, readoc-dev2, doclaynet-dev, doclaynet-val,
omni-dev, omni-val, scanned, olmocr-scans, multiscript. olmocr-math is olmOCR's arxiv_math
category alone, converted and scored for cli only (its declared profile is --math=latex).
doclaynet-dev and doclaynet-val (`fetch --only doclaynet`: DocLayNet's val and test pages) score
the roles of the pages' text cells in cli's JSON output, for cli only; doclaynet-val is aggregate
only. omni-dev and omni-val (`images --only omni`, then `fetch --only omni`: OmniParseBench's
.pdf pages in two halves) are graded by its unit tests on cli's JSON output, for cli only;
omni-val is aggregate only. The scanned suite is built from the synthetic corpus by `corpus
--suite scanned`; olmocr-scans comes with `fetch --only olmocr`; multiscript (image-only pages in
9 scripts) is rendered by `corpus --suite multiscript`. Four disjoint READoc sets, numbers of one
order per subset (arXiv, GitHub): readoc is the holdout (1-150, 300 documents, aggregate scores
only, once per release candidate); readoc-dev is the development split (151-300, `fetch --only
readoc-dev`), scored per document too (scores/readoc-dev/<tool>-<subset>-docs.json); readoc-val is
the validation split (301-600, 600 documents, `fetch --only readoc-val`), aggregate scores only,
checked at every quality landing; readoc-dev2 is a second development split (601-900, up to 600
documents, `fetch --only readoc-dev2`), scored per document like readoc-dev.
The cli tool scores any converter binary: --bin-dir DIR is mounted at /opt/tool and
--cmd is a template with {in} and {out}, for example --cmd 'rayopdf --convert {in} {out}'.
--profile default runs `rayopdf --convert {in} {out}`; --profile declared adds the suite's
options from eval/profiles.json (each benchmark's declared profile, roadmap D6).
RAYOPDF_EVAL_CPUS replaces the work cpuset (default 0-2,4,6-8,10), so that several conversions
can run side by side on disjoint CPUs; it must leave the timing CPU (3) out.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_ROOT = REPO / "benchmark-output" / "eval"

WORK_CPUS = os.environ.get("RAYOPDF_EVAL_CPUS") or "0-2,4,6-8,10"
TIMING_CPU = "3"
GIB = 1024 ** 3

TOOLS = ["pdftotext", "markitdown", "pymupdf", "opendataloader", "liteparse", "edgeparse",
         "docling", "docling-ocr", "tesseract", "kordoc", "kordoc-ocr", "cli"]
TEXT_LAYER_TOOLS = ["pdftotext", "markitdown", "pymupdf", "opendataloader", "liteparse",
                    "edgeparse"]
# OCR reference tools; `all` runs them on the scanned suites only (kordoc-ocr on odl as well,
# where kordoc reports its result with OCR on) and does not time them.
OCR_TOOLS = ["docling-ocr", "tesseract", "kordoc-ocr"]
OCR_TOOL_SUITES = {"kordoc-ocr": ["odl"]}  # suites besides the scanned ones
# Tools that run in another tool's image (same install and models, different options).
TOOL_IMAGE = {"docling-ocr": "docling", "kordoc-ocr": "kordoc"}
# readoc-dev: 300 READoc documents the holdout (readoc) did not take; development only.
# readoc-val: 600 further READoc documents; validation, aggregate only.
# readoc-dev2: up to 600 READoc documents after those; a second development split.
# olmocr-math: olmOCR's arxiv_math category alone, for cli in its declared profile (--math=latex).
# doclaynet-dev, doclaynet-val: DocLayNet pages (its val and test splits) scored by the roles of
# their text cells (eval/scorer/doclaynet.py); development and validation (aggregate only).
# omni-dev, omni-val: OmniParseBench's PDF pages, halves of its pages by the SHA-256 of their id,
# graded by its unit tests (`opb grade`); development and validation (aggregate only).
TEXT_SUITES = ["synthetic", "odl", "olmocr", "olmocr-math", "readoc", "readoc-dev", "readoc-val",
               "readoc-dev2", "doclaynet-dev", "doclaynet-val", "omni-dev", "omni-val"]
# Suites converted and scored for cli alone (`all` converts no cli run and skips them): only cli
# writes the boxes the DocLayNet and OmniParseBench scorers read.
CLI_SUITES = ["olmocr-math", "doclaynet-dev", "doclaynet-val", "omni-dev", "omni-val"]
# multiscript: image-only pages in Cyrillic, Hangul, Arabic, Hebrew, Devanagari, Kannada, Tamil,
# Latin and a mix, upright and turned (eval/producers/multiscript.py); development only.
SCAN_SUITES = ["scanned", "olmocr-scans", "multiscript"]
SUITES = TEXT_SUITES + SCAN_SUITES
IMAGES = ["producers", "scorer", "olmocr", "readoc", "omni"] + [t for t in TOOLS if t not in TOOL_IMAGE]
DATASETS = ["odl", "olmocr", "readoc", "readoc-dev", "readoc-val", "readoc-dev2", "safedocs",
            "doclaynet", "omni"]
# Each benchmark's declared profile: the cli options it is measured with (roadmap D6).
PROFILES = REPO / "eval" / "profiles.json"

ODL_URL = "https://github.com/opendataloader-project/opendataloader-bench"
ODL_COMMIT = "7af1d8f4d0c09f51ea1a5c6ba5f66e993286d109"
OLMOCR_DATASET = "allenai/olmOCR-bench"
OLMOCR_REVISION = "54a96a6fb6a2bd3b297e59869491db4d3625b711"
OLMOCR_CATEGORIES = ["arxiv_math", "headers_footers", "long_tiny_text", "multi_column", "tables"]
OLMOCR_JSONL = {"arxiv_math": "arxiv_math", "headers_footers": "headers_footers",
                "long_tiny_text": "long_tiny_text", "multi_column": "multi_column",
                "tables": "table_tests"}
OLMOCR_SCAN_CATEGORIES = ["old_scans", "old_scans_math"]
OLMOCR_SCAN_JSONL = {"old_scans": "old_scans", "old_scans_math": "old_scans_math"}
READOC_URL = "https://github.com/icip-cas/READoc"
READOC_COMMIT = "afa047715184ac319046ce0230a6109cd270e6fc"
READOC_DATASET = "lazyc/READoc"
READOC_REVISION = "782bf954e4d9a31ae2bcfe14ff59f1f4b2467592"
READOC_SUBSETS = ["arxiv", "github"]
READOC_PER_SUBSET = 150
# The development split: the next documents of each subset in the holdout's order.
READOC_DEV_PER_SUBSET = 150
# The validation split: the documents after the development split's in the same order.
READOC_VAL_PER_SUBSET = 300
# The second development split: the documents after the validation split's in the same order
# (fewer when a subset ends first).
READOC_DEV2_PER_SUBSET = 300
# Per suite: the folders of the PDFs and of their ground truth under data/READoc-data.
READOC_DIRS = {"readoc": ("sel", "sel-gt"), "readoc-dev": ("dev", "dev-gt"),
               "readoc-val": ("val", "val-gt"), "readoc-dev2": ("dev2", "dev2-gt")}
# Suites whose per-document outputs and scores are never looked at (AGENTS.md rule 7): READoc's
# are scored by suite_e2e.py's aggregate alone, DocLayNet's without per-page values.
AGGREGATE_ONLY = ["readoc", "readoc-val", "doclaynet-val", "omni-val"]
SAFEDOCS_URL = ("https://digitalcorpora.s3.amazonaws.com/corpora/files/"
                "CC-MAIN-2021-31-PDF-UNTRUNCATED/zipfiles/0000-0999/0000.zip")
SAFEDOCS_SIZE = 1_266_879_273
# DocLayNet 1.0.0 (CDLA-Permissive-1.0): the core archive holds the COCO annotations (and page
# images, not read), the extra one each page's PDF and its text cells (JSON/<stem>.json, boxes in
# the 1025 px COCO image). Pinned by S3 ETag and size (2026-11-13); read by range requests.
DOCLAYNET_URL = "https://codait-cos-dax.s3.us.cloud-object-storage.appdomain.cloud/dax-doclaynet/1.0.0/"
DOCLAYNET_ZIPS = {"core": ("DocLayNet_core.zip", "52242220284e12538949c5b06eb2777c-448", 30_012_083_650),
                  "extra": ("DocLayNet_extra.zip", "56a63e0408a7076bceaac038ab9842ca-120", 8_008_878_198)}
# Our splits from DocLayNet's: development its val, validation its test, and a training set of
# its train pages (never a suite; eval/train/).
DOCLAYNET_SPLITS = {"dev": "val", "val": "test", "train": "train"}
DOCLAYNET_TRAIN = 20_000
# OmniParseBench (Datalab; its tests CC BY 4.0, each source's licence in licenses/README.md),
# pinned to the dataset's revision on 2026-11-13. Its .png pages (scans and photos) are for the
# bundle's records; the core suites take its .pdf pages.
OMNI_DATASET = "datalab-to/omni_parse_bench"
OMNI_REVISION = "d70182b9f901d1660a27e60bf6be6d6e6ea83013"
OMNI_MAX_BYTES = 3 * GIB
# Bytes each dataset needs while it is fetched (READoc holds one 1.2 GB zip until its selection
# is extracted).
DATASET_NEED = {"odl": GIB // 2, "olmocr": GIB // 2, "readoc": 3 * GIB // 2,
                "readoc-dev": 3 * GIB // 2, "readoc-val": 3 * GIB // 2,
                "readoc-dev2": 3 * GIB // 2, "safedocs": 3 * GIB // 2, "doclaynet": 5 * GIB,
                "omni": 3 * GIB}
FULL_RUN_NEED = 25 * GIB

SPEED_SYNTHETIC = [f"{p}/{f}" for p in ("rayomd", "xelatex", "libreoffice")
                   for f in ("d03-user-manual", "d05-book-chapter")]
CLASSES = ["renderer", "office", "academic", "book"]
# eval/producers/scan.py's codecs and degradations, in report order.
CODECS_SCANNED = ("jpeg", "ccitt", "jbig2", "jpx", "flate")
DEGRADATIONS_SCANNED = ("clean", "noise", "skew", "blur")
CONSTRUCT_CATEGORIES = ["heading", "list_item", "code_block", "quote_para", "alert", "link",
                        "strong", "emph", "strike", "code_inline", "sup", "sub", "footnote"]
CONSTRUCT_ATTRS = ["heading.level", "list_item.depth", "list_item.ordered", "list_item.task",
                   "alert.type", "link.url", "code_block.exact"]


# --------------------------------------------------------------------------------------------
# helpers


def log(message: str) -> None:
    print(f"[eval {datetime.datetime.now():%H:%M:%S}] {message}", flush=True)


def run(cmd: list[str], *, check: bool = True, capture: bool = False,
        stdout_path: Path | None = None) -> subprocess.CompletedProcess:
    log("$ " + " ".join(cmd))
    if stdout_path is not None:
        with stdout_path.open("w", encoding="utf-8") as fh:
            proc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, text=True)
    elif capture:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    else:
        proc = subprocess.run(cmd)
    if check and proc.returncode != 0:
        detail = (proc.stderr or "")[-2000:] if capture else ""
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}\n{detail}")
    return proc


def tag(name: str) -> str:
    return f"rayopdf-eval-{name}:1"


def dockerfile(name: str) -> Path:
    stem = f"tool-{name}" if name in TOOLS else name
    return REPO / "eval" / "docker" / f"{stem}.Dockerfile"


def tool_tag(tool: str) -> str:
    return tag(TOOL_IMAGE.get(tool, tool))


def docker_cmd(image: str, argv: list[str], *, mounts: list[tuple[Path | str, str, bool]],
               network: bool = False, timing: bool = False, env: dict[str, str] | None = None,
               hf_offline: bool = True, workdir: str | None = None) -> list[str]:
    """Build the shared `docker run` invocation (see docs/development/evaluation.md)."""
    cmd = ["docker", "run", "--rm", "--init", "-u", f"{os.getuid()}:{os.getgid()}",
           "-e", "HOME=/tmp", "-e", "XDG_CACHE_HOME=/tmp/.cache", "-e", "TEXMFVAR=/tmp/texmf-var",
           "-e", "HF_HOME=/tmp/hf"]
    if hf_offline:
        cmd += ["-e", "HF_HUB_OFFLINE=1"]
    cmd += ["--tmpfs", "/tmp:exec,size=4g", "--memory=8g"]
    if not network:
        cmd += ["--network", "none"]
    if timing:
        cmd += [f"--cpuset-cpus={TIMING_CPU}", "--cpus=1"]
    else:
        cmd += [f"--cpuset-cpus={WORK_CPUS}"]
    for key, value in (env or {}).items():
        cmd += ["-e", f"{key}={value}"]
    for host, ctr, read_only in mounts:
        cmd += ["-v", f"{Path(host).resolve()}:{ctr}" + (":ro" if read_only else "")]
    if workdir:
        cmd += ["-w", workdir]
    return cmd + [image] + argv


def read_json(path: Path, default=None):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def read_jsonl(path: Path) -> list[dict]:
    rows = []
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                rows.append(json.loads(line))
    except OSError:
        pass
    return rows


def reset_dir(path: Path) -> Path:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    return path


def free_bytes(path: Path) -> int:
    probe = path
    while not probe.exists():
        probe = probe.parent
    return shutil.disk_usage(probe).free


def preflight(path: Path, need: int, what: str) -> None:
    free = free_bytes(path)
    if free < need:
        raise SystemExit(f"pre-flight: {what} needs {need / GIB:.1f} GiB free on the filesystem of "
                         f"{path}, only {free / GIB:.1f} GiB is free. Free space yourself; this "
                         "script never prunes Docker images.")


def median(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def p95(values: list[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, -(-95 * len(ordered) // 100) - 1)]


def mean(values) -> float | None:
    values = [v for v in values if v is not None]
    return sum(values) / len(values) if values else None


# --------------------------------------------------------------------------------------------
# suites


def corpus_items(root: Path) -> list[dict]:
    data = read_json(root / "corpus" / "corpus.json")
    if not data:
        raise SystemExit(f"{root / 'corpus' / 'corpus.json'} is missing; run `corpus` first")
    return data["items"]


def scanned_items(root: Path) -> list[dict]:
    data = read_json(root / "scanned" / "scanned.json")
    if not data:
        raise SystemExit(f"{root / 'scanned' / 'scanned.json'} is missing; run "
                         "`corpus --suite scanned` first")
    return data["items"]


def multiscript_items(root: Path) -> list[dict]:
    data = read_json(root / "multiscript" / "multiscript.json")
    if not data:
        raise SystemExit(f"{root / 'multiscript' / 'multiscript.json'} is missing; run "
                         "`corpus --suite multiscript` first")
    return data["items"]


def suite_root(root: Path, suite: str) -> Path:
    return {
        "multiscript": root / "multiscript" / "pdf",
        "synthetic": root / "corpus",
        "odl": root / "data" / "odl-bench" / "pdfs",
        "olmocr": root / "data" / "olmOCR-bench" / "bench_data" / "pdfs",
        "olmocr-math": root / "data" / "olmOCR-bench" / "bench_data" / "pdfs",
        "readoc": root / "data" / "READoc-data" / READOC_DIRS["readoc"][0],
        "readoc-dev": root / "data" / "READoc-data" / READOC_DIRS["readoc-dev"][0],
        "readoc-val": root / "data" / "READoc-data" / READOC_DIRS["readoc-val"][0],
        "readoc-dev2": root / "data" / "READoc-data" / READOC_DIRS["readoc-dev2"][0],
        "scanned": root / "scanned",
        "olmocr-scans": root / "data" / "olmOCR-bench" / "bench_data" / "pdfs",
        "doclaynet-dev": root / "data" / "doclaynet" / "dev",
        "doclaynet-val": root / "data" / "doclaynet" / "val",
        "omni-dev": root / "data" / "omni" / "pages",
        "omni-val": root / "data" / "omni" / "pages",
    }[suite]


def pdf_ids(base: Path, sub: str = "") -> list[str]:
    folder = base / sub if sub else base
    if not folder.is_dir():
        return []
    return sorted(str(p.relative_to(base).with_suffix("")) for p in folder.glob("*.pdf"))


def suite_ids(root: Path, suite: str) -> list[str]:
    base = suite_root(root, suite)
    if suite == "synthetic":
        return [item["id"] for item in corpus_items(root) if item["valid"]]
    if suite == "scanned":
        return [item["id"] for item in scanned_items(root) if item["valid"]]
    if suite == "multiscript":
        return [item["id"] for item in multiscript_items(root) if item["valid"]]
    if suite in ("odl", "doclaynet-dev", "doclaynet-val"):
        ids = pdf_ids(base)
    elif suite in ("omni-dev", "omni-val"):
        selection = read_json(root / "data" / "omni" / "selection.json", {})
        ids = sorted(selection.get("splits", {}).get(suite.split("-", 1)[1], {}).get("pdf", []))
    elif suite == "olmocr":
        ids = [i for cat in OLMOCR_CATEGORIES for i in pdf_ids(base, cat)]
    elif suite == "olmocr-math":
        ids = pdf_ids(base, "arxiv_math")
    elif suite == "olmocr-scans":
        ids = [i for cat in OLMOCR_SCAN_CATEGORIES for i in pdf_ids(base, cat)]
    else:
        ids = [i for sub in READOC_SUBSETS for i in pdf_ids(base, sub)]
    if not ids:
        raise SystemExit(f"suite {suite}: no PDFs under {base}; run `fetch` first")
    return ids


def write_list(path: Path, ids: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{i}\n" for i in ids), encoding="utf-8")


def tool_timeout(tool: str) -> int:
    # cli gets the OCR tools' limit: with the OCR add-on beside it, it OCRs scanned pages too.
    return {"docling": 600, "docling-ocr": 1800, "tesseract": 600, "kordoc-ocr": 600,
            "cli": 600}.get(tool, 120)


def odl_pages(root: Path) -> dict[str, int | None]:
    """Page counts of the odl PDFs, computed once with pdfinfo in the producers image."""
    path = root / "data" / "odl-pages.json"
    pages = read_json(path)
    if pages is None:
        src = suite_root(root, "odl")
        run(docker_cmd(tag("producers"), ["python3", "/opt/eval/producers/pages.py", "--in", "/in",
                                          "--out", "/out/odl-pages.json"],
                       mounts=[(src, "/in", True), (root / "data", "/out", False)]))
        pages = read_json(path, {})
    return pages


# --------------------------------------------------------------------------------------------
# images


def cmd_images(args) -> int:
    root: Path = args.root
    names = args.only.split(",") if args.only else IMAGES
    unknown = [n for n in names if n not in IMAGES]
    if unknown:
        raise SystemExit(f"unknown image(s): {', '.join(unknown)}")
    record = read_json(root / "images.json", {"images": {}})
    for name in names:
        run(["docker", "build", "-f", str(dockerfile(name)), "-t", tag(name), str(REPO / "eval")])
        info = json.loads(run(["docker", "image", "inspect", tag(name)], capture=True).stdout)[0]
        versions = run(["docker", "run", "--rm", "--network", "none", tag(name), "cat",
                        "/opt/eval/versions.json"], capture=True).stdout
        record["images"][name] = {"tag": tag(name), "id": info["Id"], "size": info["Size"],
                                  "created": info["Created"], "versions": json.loads(versions)}
        write_json(root / "images.json", record)
    record["date"] = datetime.date.today().isoformat()
    write_json(root / "images.json", record)
    log(f"images.json: {len(record['images'])} images")
    return 0


# --------------------------------------------------------------------------------------------
# fetch


def fetch_in_container(data: Path, script: str) -> None:
    run(docker_cmd(tag("scorer"), ["sh", "-ec", script], mounts=[(data, "/data", False)],
                   network=True, hf_offline=False, workdir="/data",
                   env={"HF_HUB_DISABLE_PROGRESS_BARS": "1"}))


OLMOCR_PDFS = 1403


def dataset_present(data: Path, name: str) -> bool:
    if name == "odl":
        repo = data / "odl-bench"
        return (len(list((repo / "pdfs").glob("*.pdf"))) == 200
                and (repo / "src" / "evaluator.py").is_file())
    if name == "doclaynet":
        return (data / "doclaynet" / "selection.json").is_file()
    if name == "omni":
        return all((data / "omni" / f).is_file()
                   for f in ("selection.json", "manifest-dev.parquet", "manifest-val.parquet"))
    if name == "olmocr":
        bench = data / "olmOCR-bench" / "bench_data"
        return (len(list((bench / "pdfs").glob("*/*.pdf"))) == OLMOCR_PDFS
                and all((bench / f"{j}.jsonl").is_file() for j in OLMOCR_JSONL.values()))
    if name == "readoc":
        return ((data / "READoc-data" / "selection.json").is_file()
                and (data / "READoc" / ".git").is_dir())
    if name in ("readoc-dev", "readoc-val", "readoc-dev2"):
        selection = {"readoc-dev": "selection-dev.json", "readoc-val": "selection-val.json",
                     "readoc-dev2": "selection-dev2.json"}[name]
        return ((data / "READoc-data" / selection).is_file()
                and (data / "READoc" / ".git").is_dir())
    target = data / "safedocs" / "0000.zip"
    return target.is_file() and target.stat().st_size == SAFEDOCS_SIZE


def fetch_odl(data: Path) -> None:
    fetch_in_container(data, f"""
        [ -d odl-bench/.git ] || git clone --no-checkout {ODL_URL} odl-bench
        git -C odl-bench -c advice.detachedHead=false checkout {ODL_COMMIT}
        git -C odl-bench lfs install --local
        git -C odl-bench lfs pull
    """)


def fetch_olmocr(data: Path) -> None:
    fetch_in_container(data, f"hf download {OLMOCR_DATASET} --repo-type dataset "
                             f"--revision {OLMOCR_REVISION} --local-dir olmOCR-bench")


def readoc_clone(data: Path) -> None:
    fetch_in_container(data, f"""
        [ -d READoc/.git ] || git clone --no-checkout {READOC_URL} READoc
        git -C READoc -c advice.detachedHead=false checkout {READOC_COMMIT}
    """)


def readoc_subset(data: Path, sub: str, first: int, count: int, suite: str,
                  before: list[str] | None = None) -> dict:
    """Fetch one READoc subset and extract documents first+1..first+count of its order.

    The order: ground-truth stems sorted by SHA-256 of the stem, those without a same-stem PDF
    left out. `before` is the earlier sets' selections in order (the holdout, then the
    development split, then the validation split): the documents before `first` must be
    exactly it and none of the extracted ones may be in it. Only one zip (up to 1.2 GB) is on
    disk at a time; it is deleted again.
    """
    base = data / "READoc-data"
    gt_dir = base / f"{sub}_ground_truth"
    include = f"--include {sub}.zip"
    if not list(gt_dir.glob("*.md")):
        include += f" --include '{sub}_ground_truth/*'"
    fetch_in_container(data, f"hf download {READOC_DATASET} --repo-type dataset "
                             f"--revision {READOC_REVISION} {include} --local-dir READoc-data")
    pdf_name, gt_name = READOC_DIRS[suite]
    stems = sorted(p.stem for p in gt_dir.glob("*.md"))
    try:
        with zipfile.ZipFile(base / f"{sub}.zip") as archive:
            members = [m for m in archive.namelist()
                       if m.lower().endswith(".pdf") and not m.startswith("__MACOSX/")]
            exact = {Path(m).stem: m for m in members}
            lower = {Path(m).stem.lower(): m for m in members}
            paired_by = "stem" if all(s in exact for s in stems) else "lowercased stem"
            ranked = []
            for stem in sorted(stems, key=lambda s: hashlib.sha256(s.encode()).hexdigest()):
                member = exact.get(stem) or lower.get(stem.lower())
                if member is not None:
                    ranked.append((stem, member))
            chosen = ranked[first:first + count]
            if before is not None:
                if [stem for stem, _ in ranked[:first]] != before:
                    raise SystemExit(f"{suite}/{sub}: the first {first} documents of the order are "
                                     "not the earlier sets' selections")
                both = sorted({stem for stem, _ in chosen} & set(before))
                if both:
                    raise SystemExit(f"{suite}/{sub}: {len(both)} selected document(s) are in an "
                                     f"earlier set: {', '.join(both[:5])}")
            out_pdf = reset_dir(base / pdf_name / sub)
            out_gt = reset_dir(base / gt_name / sub)
            for stem, member in chosen:
                with archive.open(member) as src, (out_pdf / f"{stem}.pdf").open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                shutil.copyfile(gt_dir / f"{stem}.md", out_gt / f"{stem}.md")
    finally:
        (base / f"{sub}.zip").unlink(missing_ok=True)
    log(f"{suite}/{sub}: {len(chosen)} selected of {len(stems)} GT files")
    return {"gt_files": len(stems), "pdf_members": len(members), "paired_by": paired_by,
            "paired": len(ranked), "selected": [stem for stem, _ in chosen]}


def fetch_readoc(data: Path) -> None:
    sel = data / "READoc-data" / "selection.json"
    readoc_clone(data)
    if sel.is_file():
        log("readoc: selection present")
        return
    selection = {"dataset": READOC_DATASET, "revision": READOC_REVISION,
                 "rule": f"GT stems sorted by SHA-256 of the stem; first {READOC_PER_SUBSET} "
                         "with a same-stem PDF", "subsets": {}}
    for sub in READOC_SUBSETS:
        selection["subsets"][sub] = readoc_subset(data, sub, 0, READOC_PER_SUBSET, "readoc")
    write_json(sel, selection)
    shutil.rmtree(data / "READoc-data" / ".cache", ignore_errors=True)


def fetch_readoc_dev(data: Path) -> None:
    """The development split: the documents after the holdout's in the same order."""
    base = data / "READoc-data"
    holdout = read_json(base / "selection.json")
    if not holdout:
        raise SystemExit("readoc-dev: the split is defined against the holdout's selection.json; "
                         "run `fetch --only readoc` first")
    readoc_clone(data)
    first, last = READOC_PER_SUBSET + 1, READOC_PER_SUBSET + READOC_DEV_PER_SUBSET
    selection = {"dataset": READOC_DATASET, "revision": READOC_REVISION,
                 "rule": "GT stems sorted by SHA-256 of the stem; of those with a same-stem PDF, "
                         f"numbers {first}-{last} (numbers 1-{READOC_PER_SUBSET} are the holdout "
                         "of selection.json)",
                 "role": "development", "subsets": {}}
    for sub in READOC_SUBSETS:
        selection["subsets"][sub] = readoc_subset(
            data, sub, READOC_PER_SUBSET, READOC_DEV_PER_SUBSET, "readoc-dev",
            before=holdout["subsets"][sub]["selected"])
    write_json(base / "selection-dev.json", selection)
    shutil.rmtree(base / ".cache", ignore_errors=True)


def fetch_readoc_val(data: Path) -> None:
    """The validation split: the documents after the development split's in the same order."""
    base = data / "READoc-data"
    holdout = read_json(base / "selection.json")
    dev = read_json(base / "selection-dev.json")
    if not holdout or not dev:
        raise SystemExit("readoc-val: the split follows selection.json and selection-dev.json; "
                         "run `fetch --only readoc,readoc-dev` first")
    readoc_clone(data)
    taken = READOC_PER_SUBSET + READOC_DEV_PER_SUBSET
    first, last = taken + 1, taken + READOC_VAL_PER_SUBSET
    selection = {"dataset": READOC_DATASET, "revision": READOC_REVISION,
                 "rule": "GT stems sorted by SHA-256 of the stem; of those with a same-stem PDF, "
                         f"numbers {first}-{last} (numbers 1-{READOC_PER_SUBSET} are the holdout "
                         f"of selection.json, {READOC_PER_SUBSET + 1}-{taken} the development "
                         "split of selection-dev.json)",
                 "role": "validation", "subsets": {}}
    for sub in READOC_SUBSETS:
        before = holdout["subsets"][sub]["selected"] + dev["subsets"][sub]["selected"]
        selection["subsets"][sub] = readoc_subset(
            data, sub, taken, READOC_VAL_PER_SUBSET, "readoc-val", before=before)
    write_json(base / "selection-val.json", selection)
    shutil.rmtree(base / ".cache", ignore_errors=True)


def fetch_readoc_dev2(data: Path) -> None:
    """The second development split: the documents after the validation split's in the same
    order."""
    base = data / "READoc-data"
    earlier = [read_json(base / name) for name in
               ("selection.json", "selection-dev.json", "selection-val.json")]
    if not all(earlier):
        raise SystemExit("readoc-dev2: the split follows selection.json, selection-dev.json and "
                         "selection-val.json; run `fetch --only readoc,readoc-dev,readoc-val` "
                         "first")
    readoc_clone(data)
    taken = READOC_PER_SUBSET + READOC_DEV_PER_SUBSET + READOC_VAL_PER_SUBSET
    first, last = taken + 1, taken + READOC_DEV2_PER_SUBSET
    selection = {"dataset": READOC_DATASET, "revision": READOC_REVISION,
                 "rule": "GT stems sorted by SHA-256 of the stem; of those with a same-stem PDF, "
                         f"numbers {first}-{last} (1-{READOC_PER_SUBSET} holdout, "
                         f"{READOC_PER_SUBSET + 1}-{READOC_PER_SUBSET + READOC_DEV_PER_SUBSET} "
                         f"development, {READOC_PER_SUBSET + READOC_DEV_PER_SUBSET + 1}-{taken} "
                         "validation)",
                 "role": "development", "subsets": {}}
    for sub in READOC_SUBSETS:
        before = [stem for chosen in earlier for stem in chosen["subsets"][sub]["selected"]]
        selection["subsets"][sub] = readoc_subset(
            data, sub, taken, READOC_DEV2_PER_SUBSET, "readoc-dev2", before=before)
    write_json(base / "selection-dev2.json", selection)
    shutil.rmtree(base / ".cache", ignore_errors=True)


def fetch_safedocs(data: Path) -> None:
    target = data / "safedocs" / "0000.zip"
    target.parent.mkdir(parents=True, exist_ok=True)
    fetch_in_container(data, f"curl -fsSL --retry 3 -o safedocs/0000.zip {SAFEDOCS_URL}")
    size = target.stat().st_size
    if size != SAFEDOCS_SIZE:
        raise SystemExit(f"safedocs: {target} has {size} bytes, expected {SAFEDOCS_SIZE}")


class RangeFile(io.RawIOBase):
    """A seekable, read-only file over an HTTP URL that answers range requests. A read outside
    the buffer fetches `readahead` bytes from the position (more when the read asks for more) in
    one `Range: bytes=a-b` request, tried up to 3 times; the answer's Content-Range must name that
    range. Reads in ascending order are served from the buffer until they pass its end."""

    def __init__(self, url: str, size: int, readahead: int = 1 << 20) -> None:
        super().__init__()
        self.url, self.size, self.readahead = url, size, readahead
        self.position = 0
        self.start, self.buffer = 0, b""
        self.requests = 0

    def readable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def tell(self) -> int:
        return self.position

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        self.position = {io.SEEK_SET: 0, io.SEEK_CUR: self.position, io.SEEK_END: self.size}[whence] + offset
        return self.position

    def fetch(self, first: int, last: int) -> bytes:
        request = urllib.request.Request(self.url, headers={"Range": f"bytes={first}-{last}"})
        for attempt in range(3):
            try:
                with urllib.request.urlopen(request, timeout=300) as answer:
                    data = answer.read()
                    named = answer.headers.get("Content-Range")
                if named != f"bytes {first}-{last}/{self.size}" or len(data) != last - first + 1:
                    raise OSError(f"asked for bytes {first}-{last}, got {named} with {len(data)} bytes")
                self.requests += 1
                return data
            except (OSError, http.client.HTTPException) as exc:
                if attempt == 2:
                    raise SystemExit(f"{self.url}: {exc}") from None
                log(f"range request failed ({exc}); trying again")
                time.sleep(10 * (attempt + 1))
        raise AssertionError("unreachable")

    def readinto(self, target) -> int:
        view = memoryview(target).cast("B")
        if self.position >= self.size or not len(view):
            return 0
        end = min(self.size, self.position + len(view))
        if not (self.start <= self.position and end <= self.start + len(self.buffer)):
            last = min(self.size, max(end, self.position + self.readahead)) - 1
            self.buffer, self.start = self.fetch(self.position, last), self.position
        data = self.buffer[self.position - self.start:end - self.start]
        view[:len(data)] = data
        self.position += len(data)
        return len(data)


def doclaynet_archive(name: str, readahead: int) -> zipfile.ZipFile:
    """One of DocLayNet's archives, read in place: its pins checked by a HEAD request, then its
    central directory read by range requests (the archive is not downloaded)."""
    file, etag, size = DOCLAYNET_ZIPS[name]
    url = DOCLAYNET_URL + file
    with urllib.request.urlopen(urllib.request.Request(url, method="HEAD"), timeout=60) as answer:
        headers = answer.headers
    for field, found, expected in (("ETag", (headers.get("ETag") or "").strip('"'), etag),
                                   ("Content-Length", headers.get("Content-Length"), str(size)),
                                   ("Accept-Ranges", headers.get("Accept-Ranges"), "bytes")):
        if found != expected:
            raise SystemExit(f"DocLayNet changed upstream: {field} of {file} is {found}, pinned {expected}")
    return zipfile.ZipFile(RangeFile(url, size, readahead))


def copy_member(archive: zipfile.ZipFile, member: str, target: Path) -> str:
    """Extracts `member` to `target`; its SHA-256."""
    digest = hashlib.sha256()
    with archive.open(member) as src, target.open("wb") as dst:
        while chunk := src.read(1 << 20):
            digest.update(chunk)
            dst.write(chunk)
    return digest.hexdigest()


def fetch_doclaynet(data: Path) -> None:
    """DocLayNet without downloading its archives (38 GB): the COCO annotations from the core
    archive into data/doclaynet/coco/, then the PDF and text cells (JSON) of each selected page
    from the extra archive into data/doclaynet/<split>/, in the order the archive holds them so
    that one request serves neighbours. Selected: every page of DocLayNet's val split (our dev)
    and test split (our val) and the DOCLAYNET_TRAIN train pages with the smallest SHA-256 of
    their file name, each with precedence 0 (the page's first annotation)."""
    out = data / "doclaynet"
    coco = out / "coco"
    coco.mkdir(parents=True, exist_ok=True)
    # 32 MiB a request: a request takes seconds to start, and the members read are dense.
    core = doclaynet_archive("core", 32 << 20)
    selection = {"schema": 1, "dataset": "DocLayNet 1.0.0", "licence": "CDLA-Permissive-1.0",
                 "archives": {name: {"file": file, "etag": etag, "size": size}
                              for name, (file, etag, size) in DOCLAYNET_ZIPS.items()},
                 "rule": (f"COCO images with precedence 0: dev = DocLayNet's val split, val = its test split, "
                          f"train = the {DOCLAYNET_TRAIN} train images with the smallest SHA-256 of "
                          "file_name; each page's PDF/<stem>.pdf and JSON/<stem>.json"),
                 "roles": {"dev": "development", "val": "validation, aggregate only",
                           "train": "training data only, never a suite"},
                 "coco": {}, "counts": {}, "sha256": {}}
    stems: dict[str, list[str]] = {}
    for split, theirs in DOCLAYNET_SPLITS.items():
        target = coco / f"{theirs}.json"
        selection["coco"][theirs] = copy_member(core, f"COCO/{theirs}.json", target)
        images = [im["file_name"] for im in read_json(target)["images"] if im.get("precedence", 0) == 0]
        if split == "train":
            images = sorted(images, key=lambda f: hashlib.sha256(f.encode()).hexdigest())[:DOCLAYNET_TRAIN]
        stems[split] = sorted(Path(f).stem for f in images)
        log(f"doclaynet {split}: {len(stems[split])} pages of DocLayNet's {theirs} split")
    core.close()
    extra = doclaynet_archive("extra", 32 << 20)
    members = []
    for split, names in stems.items():
        (out / split).mkdir(parents=True, exist_ok=True)
        for stem in names:
            for kind, suffix in (("PDF", "pdf"), ("JSON", "json")):
                info = extra.getinfo(f"{kind}/{stem}.{suffix}")
                members.append((info.header_offset, info.filename, out / split / f"{stem}.{suffix}"))
    for count, (_, member, target) in enumerate(sorted(members), 1):
        selection["sha256"][str(target.relative_to(out))] = copy_member(extra, member, target)
        if count % 5000 == 0:
            log(f"doclaynet: {count}/{len(members)} files")
    extra.close()
    selection["counts"] = {split: len(names) for split, names in stems.items()}
    write_json(out / "selection.json", selection)
    log(f"doclaynet: {selection['counts']}")


def omni_listing() -> list[dict]:
    """The files of OmniParseBench at OMNI_REVISION, from the Hub's tree listing (paged)."""
    url = (f"https://huggingface.co/api/datasets/{OMNI_DATASET}/tree/{OMNI_REVISION}"
           "?recursive=1")
    files: list[dict] = []
    while url:
        with urllib.request.urlopen(url, timeout=120) as answer:
            page = json.loads(answer.read())
            link = answer.headers.get("Link") or ""
        files += [entry for entry in page if entry.get("type") == "file"]
        found = re.search(r'<([^>]+)>;\s*rel="next"', link)
        url = found.group(1) if found else None
    return files


def omni_download(entry: dict, target: Path) -> str:
    """One file of the dataset into `target` (kept when a former fetch left it whole); its SHA-256,
    checked against the listing's size and LFS hash when it has one. Tried up to 8 times: a 429 or
    5xx answer waits for its Retry-After (else 30 s more each time), other errors 10 s more each
    time."""
    url = f"https://huggingface.co/datasets/{OMNI_DATASET}/resolve/{OMNI_REVISION}/{entry['path']}"
    lfs = (entry.get("lfs") or {}).get("oid")
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and target.stat().st_size == entry.get("size"):
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        if not lfs or lfs == digest:
            return digest
    for attempt in range(8):
        try:
            digest = hashlib.sha256()
            size = 0
            with urllib.request.urlopen(url, timeout=300) as answer, target.open("wb") as out:
                while chunk := answer.read(1 << 20):
                    digest.update(chunk)
                    out.write(chunk)
                    size += len(chunk)
            if size != entry.get("size", size) or (lfs and lfs != digest.hexdigest()):
                raise OSError(f"{entry['path']}: {size} bytes, SHA-256 {digest.hexdigest()}, not as listed")
            return digest.hexdigest()
        except (OSError, http.client.HTTPException) as exc:
            if attempt == 7:
                raise SystemExit(f"omni: {entry['path']}: {exc}") from None
            wait = 10 * (attempt + 1)
            if isinstance(exc, urllib.error.HTTPError) and (exc.code == 429 or exc.code >= 500):
                after = exc.headers.get("Retry-After", "") if exc.headers else ""
                wait = int(after) if after.isdigit() else 30 * (attempt + 1)
            time.sleep(wait)
    raise AssertionError("unreachable")


def fetch_omni(data: Path) -> None:
    """OmniParseBench at OMNI_REVISION: its manifest, licence notes, pages and gold tests into
    data/omni/ (refused past OMNI_MAX_BYTES), downloaded 4 at a time; selection.json with the
    revision, every file's SHA-256 and the splits: page ids sorted by their SHA-256, the first half
    dev, the rest val (each split's .pdf pages are its suite, its .png pages bundle-only). Then
    eval/scorer/omni_split.py writes each split's manifest of .pdf pages (manifest-dev.parquet,
    manifest-val.parquet) in the omni image: `images --only omni` comes first."""
    out = data / "omni"
    out.mkdir(parents=True, exist_ok=True)
    wanted = [entry for entry in omni_listing()
              if entry["path"] in ("manifest.parquet", "licenses/README.md")
              or (entry["path"].startswith("pages/") and entry["path"].endswith((".pdf", ".png")))
              or (entry["path"].startswith("gold/") and entry["path"].endswith(".json"))]
    total = sum(entry.get("size", 0) for entry in wanted)
    if total > OMNI_MAX_BYTES:
        raise SystemExit(f"omni: the files take {total} bytes, past the {OMNI_MAX_BYTES} allowed")
    log(f"omni: {len(wanted)} files, {total / 2**20:.0f} MiB")
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        digests = dict(zip((entry["path"] for entry in wanted),
                           pool.map(lambda entry: omni_download(entry, out / entry["path"]), wanted)))
    pages = {Path(path).stem: Path(path).suffix for path in digests if path.startswith("pages/")}
    ids = sorted(pages, key=lambda page: hashlib.sha256(page.encode()).hexdigest())
    half = (len(ids) + 1) // 2
    splits = {}
    for split, chosen in (("dev", ids[:half]), ("val", ids[half:])):
        splits[split] = {"pdf": sorted(p for p in chosen if pages[p] == ".pdf"),
                         "png": sorted(p for p in chosen if pages[p] == ".png")}
    write_json(out / "selection.json", {
        "schema": 1, "dataset": OMNI_DATASET, "revision": OMNI_REVISION,
        "rule": "page ids sorted by the SHA-256 of the id: the first half dev, the rest val; a split's "
                ".pdf pages are its suite (omni-dev, omni-val), its .png pages bundle-only",
        "roles": {"dev": "development", "val": "validation, aggregate only"},
        "counts": {split: {kind: len(v) for kind, v in s.items()} for split, s in splits.items()},
        "splits": splits, "sha256": dict(sorted(digests.items()))})
    run(docker_cmd(tag("omni"), ["python3", "/opt/eval/scorer/omni_split.py", "--dataset", "/data"],
                   mounts=[(REPO / "eval" / "scorer", "/opt/eval/scorer", True), (out, "/data", False)]))
    log(f"omni: {json.dumps(read_json(out / 'selection.json')['counts'])}")


def cmd_fetch(args) -> int:
    data: Path = args.root / "data"
    data.mkdir(parents=True, exist_ok=True)
    names = args.only.split(",") if args.only else DATASETS
    unknown = [n for n in names if n not in DATASETS]
    if unknown:
        raise SystemExit(f"unknown dataset(s): {', '.join(unknown)}")
    missing = [n for n in names if not dataset_present(data, n)]
    for name in names:
        if name not in missing:
            log(f"{name}: present")
    if missing:
        preflight(data, sum(DATASET_NEED[n] for n in missing) + 2 * GIB, "fetch")
    for name in missing:
        log(f"fetch {name}")
        {"odl": fetch_odl, "olmocr": fetch_olmocr, "readoc": fetch_readoc,
         "readoc-dev": fetch_readoc_dev, "readoc-val": fetch_readoc_val,
         "readoc-dev2": fetch_readoc_dev2, "safedocs": fetch_safedocs, "doclaynet": fetch_doclaynet,
         "omni": fetch_omni}[name](data)
    return 0


# --------------------------------------------------------------------------------------------
# corpus, convert


def cmd_corpus(args) -> int:
    if args.suite == "scanned":
        return corpus_scanned(args)
    if args.suite == "multiscript":
        return corpus_multiscript(args)
    out = args.root / "corpus"
    out.mkdir(parents=True, exist_ok=True)
    argv = ["python3", "/opt/eval/producers/render.py", "--fixtures", "/fixtures", "--out", "/out",
            "--jobs", str(args.jobs)]
    if args.producers:
        argv += ["--producers", args.producers]
    if args.only:
        argv += ["--only", args.only]
    run(docker_cmd(tag("producers"), argv,
                   mounts=[(REPO / "eval" / "fixtures", "/fixtures", True), (out, "/out", False)]))
    items = corpus_items(args.root)
    valid = sum(1 for i in items if i["valid"])
    log(f"corpus: {valid}/{len(items)} valid")
    return 0


def corpus_scanned(args) -> int:
    """Image-only PDFs made from the valid synthetic items (eval/producers/scan.py)."""
    corpus_items(args.root)
    out = args.root / "scanned"
    out.mkdir(parents=True, exist_ok=True)
    argv = ["python3", "/opt/eval/producers/scan.py", "--corpus", "/corpus", "--out", "/out",
            "--jobs", str(args.jobs)]
    if args.only:
        argv += ["--only", args.only]
    run(docker_cmd(tag("producers"), argv,
                   mounts=[(args.root / "corpus", "/corpus", True), (out, "/out", False)]))
    items = scanned_items(args.root)
    valid = sum(1 for i in items if i["valid"])
    log(f"scanned: {valid}/{len(items)} valid, {sum(i['pages'] or 0 for i in items if i['valid'])}"
        " pages")
    return 0 if valid == len(items) else 1


def corpus_multiscript(args) -> int:
    """Image-only pages of text in nine scripts, upright and turned (eval/producers/multiscript.py).
    The repository's producers folder replaces the image's copy (the generator imports scan.py)."""
    out = args.root / "multiscript"
    out.mkdir(parents=True, exist_ok=True)
    argv = ["python3", "/opt/eval/producers/multiscript.py", "--fixtures", "/fixtures", "--out", "/out",
            "--jobs", str(args.jobs)]
    run(docker_cmd(tag("producers"), argv,
                   mounts=[(REPO / "eval" / "producers", "/opt/eval/producers", True),
                           (REPO / "eval" / "fixtures" / "multiscript", "/fixtures", True), (out, "/out", False)]))
    items = multiscript_items(args.root)
    valid = sum(1 for i in items if i["valid"])
    log(f"multiscript: {valid}/{len(items)} valid, {sum(i['pages'] or 0 for i in items if i['valid'])} pages")
    return 0 if valid == len(items) else 1


def tool_argv(tool: str, list_path: str, out: str, extra: list[str], cmd: str | None) -> list[str]:
    argv = ["python3", "/opt/eval/convert.py", "--tool", tool, "--in", "/in", "--list", list_path,
            "--out", out, "--timeout", str(tool_timeout(tool))] + extra
    if tool == "cli":
        if not cmd:
            raise SystemExit("--tool cli needs --cmd")
        argv += ["--cmd", cmd]
    return argv


def tool_mounts(tool: str, bin_dir: str | None) -> list[tuple[Path, str, bool]]:
    # The repository's adapter replaces the image's copy, so an adapter change needs no rebuild
    # of the (multi-GB) tool images.
    mounts = [(REPO / "eval" / "competitors" / "convert.py", "/opt/eval/convert.py", True)]
    if tool in ("kordoc", "kordoc-ocr"):
        mounts.append((REPO / "eval" / "competitors" / "kordoc_run.mjs", "/opt/kordoc/run.mjs", True))
    if tool == "cli" and bin_dir:
        mounts.append((Path(bin_dir), "/opt/tool", True))
    return mounts


def profile_cmd(suite: str, profile: str) -> str:
    """The cli command of `suite` in `profile`: `rayopdf --convert {in} {out}`, and in the
    declared profile the suite's options from eval/profiles.json after it."""
    cmd = "rayopdf --convert {in} {out}"
    if profile == "default":
        return cmd
    declared = read_json(PROFILES, {}).get("declared", {})
    if suite not in declared:
        raise SystemExit(f"no declared profile for {suite}")
    return f"{cmd} {declared[suite]}".rstrip()


def cmd_convert(args) -> int:
    root: Path = args.root
    cmd = args.cmd
    if args.profile:
        if args.tool != "cli":
            raise SystemExit("--profile is for --tool cli")
        cmd = profile_cmd(args.suite, args.profile)
    elif args.tool == "cli" and not cmd:
        raise SystemExit("--tool cli needs --cmd or --profile")
    ids = suite_ids(root, args.suite)
    list_dir = root / "suites" / args.suite
    write_list(list_dir / "list.txt", ids)
    out = reset_dir(root / "runs" / args.tool / args.suite)
    argv = tool_argv(args.tool, "/list/list.txt", "/out", [], cmd)
    mounts = [(suite_root(root, args.suite), "/in", True), (list_dir, "/list", True),
              (out, "/out", False)] + tool_mounts(args.tool, args.bin_dir)
    run(docker_cmd(tool_tag(args.tool), argv, mounts=mounts, env={"OMP_NUM_THREADS": "4"}))
    rows = read_jsonl(out / "results.jsonl")
    counts: dict[str, int] = {}
    for row in rows:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
    log(f"convert {args.tool}/{args.suite}: {len(rows)}/{len(ids)} records {counts}")
    return 0 if len(rows) == len(ids) else 1


def run_complete(root: Path, tool: str, suite: str) -> bool:
    lst = root / "suites" / suite / "list.txt"
    res = root / "runs" / tool / suite / "results.jsonl"
    if not (lst.is_file() and res.is_file()):
        return False
    expected = [l for l in lst.read_text(encoding="utf-8").splitlines() if l]
    return len(read_jsonl(res)) == len(expected)


# --------------------------------------------------------------------------------------------
# score


def available_tools(root: Path, suite: str, only: str | None) -> list[str]:
    tools = only.split(",") if only else TOOLS
    return [t for t in tools if (root / "runs" / t / suite / "results.jsonl").is_file()]


def odl_evaluator(gt: str, pred_root: str, engine: str | None, output: str) -> list[str]:
    argv = ["python3", "/odl/src/evaluator.py", "--ground-truth-dir", gt, "--prediction-root",
            pred_root, "--output-filename", output, "--log-level", "WARNING"]
    return argv + (["--engine", engine] if engine else [])


def score_selftest(args) -> int:
    proc = run(docker_cmd(tag("scorer"), ["python3", "/opt/eval/scorer/selftest.py", "--fixtures",
                                          "/fixtures/md"],
                          mounts=[(REPO / "eval" / "fixtures", "/fixtures", True)]), check=False)
    return proc.returncode


def score_fixture_suite(root: Path, suite: str, items: list[dict], tools: list[str]) -> None:
    """Score a suite whose ground truth is the fixture Markdown (synthetic, scanned).

    odl's evaluator needs flat file names (`<id with / as __>.md`); constructs.py pairs files
    by relative path, so its ground truth mirrors the ids.
    """
    work = root / "scores" / suite / "work"
    gt_flat = reset_dir(work / "gt")
    gt_nested = reset_dir(work / "gt-nested")
    for item in items:
        src = REPO / "eval" / "fixtures" / "md" / f"{item['fixture']}.md"
        shutil.copyfile(src, gt_flat / f"{item['id'].replace('/', '__')}.md")
        dst = gt_nested / f"{item['id']}.md"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
    odl = root / "data" / "odl-bench"
    for tool in tools:
        runs = root / "runs" / tool / suite
        pred = reset_dir(work / "pred" / tool / "markdown")
        for item in items:
            src = runs / f"{item['id']}.md"
            if src.is_file():
                shutil.copyfile(src, pred / f"{item['id'].replace('/', '__')}.md")
        script = (" ".join(odl_evaluator("/work/gt", "/work/pred", tool, "evaluation.json"))
                  + " && python3 /opt/eval/scorer/constructs.py --gt-dir /work/gt-nested"
                  + f" --pred-dir /pred --out /work/constructs-{tool}.json")
        run(docker_cmd(tag("scorer"), ["sh", "-ec", script],
                       mounts=[(work, "/work", False), (runs, "/pred", True), (odl, "/odl", True)]))
        evaluation = read_json(work / "pred" / tool / "evaluation.json", {})
        odl_docs = {d["document_id"]: d["scores"] for d in evaluation.get("documents", [])}
        constructs = read_json(work / f"constructs-{tool}.json", {}).get("docs", {})
        results = {r["id"]: r for r in read_jsonl(runs / "results.jsonl")}
        joined = []
        for item in items:
            res = results.get(item["id"], {})
            joined.append({**item, "status": res.get("status", "missing"), "ms": res.get("ms"),
                           "odl": odl_docs.get(item["id"].replace("/", "__")),
                           "constructs": constructs.get(f"{item['id']}.md")})
        write_json(root / "scores" / suite / f"{tool}.json",
                   {"schema": 1, "tool": tool, "items": joined})
        log(f"{suite}/{tool}: scored {len(joined)} items")


def score_odl(root: Path, tools: list[str], force: bool) -> None:
    odl = root / "data" / "odl-bench"
    gt = "/odl/ground-truth/markdown"
    work = root / "scores" / "odl" / "work"
    for tool in tools:
        pred = reset_dir(work / tool / "markdown")
        for src in (root / "runs" / tool / "odl").glob("*.md"):
            shutil.copyfile(src, pred / src.name)
        run(docker_cmd(tag("scorer"), odl_evaluator(gt, "/work", tool, "evaluation.json"),
                       mounts=[(work, "/work", False), (odl, "/odl", True)]))
    stored = root / "scores" / "odl" / "stored"
    if force or not list(stored.glob("*/evaluation.rescored.json")):
        shutil.rmtree(stored, ignore_errors=True)
        shutil.copytree(odl / "prediction", stored)
        run(docker_cmd(tag("scorer"), odl_evaluator(gt, "/stored", None, "evaluation.rescored.json"),
                       mounts=[(stored, "/stored", False), (odl, "/odl", True)]))


def score_olmocr(root: Path, tools: list[str], suite: str, categories: list[str] = OLMOCR_CATEGORIES) -> None:
    """olmOCR-bench scorer over `categories` of the born-digital suites (olmocr; olmocr-math with
    arxiv_math alone), or over the scanned categories (olmocr-scans)."""
    if suite == "olmocr-scans":
        categories, jsonls = OLMOCR_SCAN_CATEGORIES, OLMOCR_SCAN_JSONL
    else:
        jsonls = {c: OLMOCR_JSONL[c] for c in categories}
    dataset = root / "data" / "olmOCR-bench" / "bench_data"
    out = root / "scores" / suite
    out.mkdir(parents=True, exist_ok=True)
    for tool in tools:
        work = reset_dir(out / "work" / tool)
        bench = work / "bench_data"
        # The benchmark expects a candidate file for every PDF under pdfs/, so pdfs/ holds only
        # this suite's categories.
        (bench / "pdfs").mkdir(parents=True)
        for category in categories:
            (bench / "pdfs" / category).symlink_to(f"/data/bench_data/pdfs/{category}")
        for jsonl in jsonls.values():
            shutil.copyfile(dataset / f"{jsonl}.jsonl", bench / f"{jsonl}.jsonl")
        runs = root / "runs" / tool / suite
        for doc_id in suite_ids(root, suite):
            src = runs / f"{doc_id}.md"
            category, name = doc_id.split("/", 1)
            dst = bench / tool / category / f"{name}_pg1_repeat1.md"
            dst.parent.mkdir(parents=True, exist_ok=True)
            # A failed conversion scores as empty output, and so does whitespace-only output:
            # olmOCR's text-presence test passes on a lone newline (11 of 12 tests of
            # long_tiny_text/10a_pg1, an image-only page), which would reward empty output.
            data = src.read_bytes() if src.is_file() else b""
            dst.write_bytes(data if data.strip() else b"")
        argv = ["python", "-m", "olmocr.bench.benchmark", "--dir", "/work/bench_data",
                "--candidate", tool, "--skip_baseline"]
        run(docker_cmd(tag("olmocr"), argv,
                       mounts=[(work, "/work", False), (dataset.parent, "/data", True)]),
            check=False, stdout_path=out / f"{tool}.log")
        shutil.rmtree(work)


def score_readoc(root: Path, tools: list[str], suite: str) -> int:
    """READoc's evaluator on each subset of the holdout, a development or the validation split.

    The holdout (readoc) and the validation split (readoc-val) get `suite_e2e.py`'s aggregate
    file and nothing else. The development splits (readoc-dev, readoc-dev2) are scored by
    eval/scorer/readoc_docs.py, which runs the same evaluator document by document in parallel
    and writes the same aggregate file plus `<tool>-<subset>-docs.json` with every document's
    metrics. Returns the number of (tool, subset) pairs left without a result.
    """
    data = root / "data"
    gt_name = READOC_DIRS[suite][1]
    out = root / "scores" / suite
    out.mkdir(parents=True, exist_ok=True)
    failed = 0
    for tool in tools:
        work = reset_dir(out / "work" / tool)
        runs = root / "runs" / tool / suite
        for sub in READOC_SUBSETS:
            (work / sub).mkdir()
            for gt in (data / "READoc-data" / gt_name / sub).glob("*.md"):
                src = runs / sub / gt.name
                (work / sub / gt.name).write_bytes(src.read_bytes() if src.is_file() else b"")
            mounts = [(data, "/data", True), (work, "/work", True), (out, "/out", False)]
            if suite in AGGREGATE_ONLY:
                argv = ["python", "/data/READoc/src/evaluation/suite_e2e.py",
                        "--gold_dir", f"/data/READoc-data/{gt_name}/{sub}",
                        "--pred_dir", f"/work/{sub}", "--result_json", f"/out/{tool}-{sub}.json"]
            else:
                for stale in (f"{tool}-{sub}.json", f"{tool}-{sub}-docs.json"):
                    (out / stale).unlink(missing_ok=True)
                argv = ["python", "/opt/eval/scorer/readoc_docs.py",
                        "--evaluator", "/data/READoc/src/evaluation",
                        "--gold-dir", f"/data/READoc-data/{gt_name}/{sub}",
                        "--pred-dir", f"/work/{sub}", "--result-json", f"/out/{tool}-{sub}.json",
                        "--docs-json", f"/out/{tool}-{sub}-docs.json"]
                mounts.append((REPO / "eval" / "scorer", "/opt/eval/scorer", True))
            run(docker_cmd(tag("readoc"), argv, mounts=mounts, workdir="/data/READoc"),
                check=False, stdout_path=out / f"{tool}-{sub}.log")
            if not (out / f"{tool}-{sub}.json").is_file():
                failed += 1
                log(f"{suite}/{tool}/{sub}: not scored, see {out / f'{tool}-{sub}.log'}")
        shutil.rmtree(work)
    return failed


def score_doclaynet(root: Path, tools: list[str], suite: str) -> None:
    """The roles each tool's JSON output gives DocLayNet's text cells (eval/scorer/doclaynet.py);
    per-page counts for the development split only."""
    split = suite.split("-", 1)[1]
    out = root / "scores" / suite
    out.mkdir(parents=True, exist_ok=True)
    for tool in tools:
        argv = ["python3", "/opt/eval/scorer/doclaynet.py", "--coco", f"/data/coco/{DOCLAYNET_SPLITS[split]}.json",
                "--pages", f"/data/{split}", "--pred", "/pred", "--out", f"/out/{tool}.json"]
        if suite not in AGGREGATE_ONLY:
            argv.append("--per-page")
        run(docker_cmd(tag("scorer"), argv,
                       mounts=[(REPO / "eval" / "scorer", "/opt/eval/scorer", True),
                               (root / "data" / "doclaynet", "/data", True),
                               (root / "runs" / tool / suite, "/pred", True), (out, "/out", False)]))
        log(f"{suite}/{tool}: scored")


def score_omni(root: Path, tools: list[str], suite: str) -> int:
    """OmniParseBench's unit tests on each tool's JSON output (eval/scorer/omni_predict.py, then
    `opb grade` with the split's manifest): scores/<suite>/<tool>.json with the headline score and
    the text, tables and layout families; for omni-dev also the test lines (<tool>-tests.jsonl).
    Returns the number of tools left without a score."""
    split = suite.split("-", 1)[1]
    out = root / "scores" / suite
    out.mkdir(parents=True, exist_ok=True)
    ids = suite_ids(root, suite)
    failed = 0
    for tool in tools:
        work = reset_dir(out / "work" / tool)
        write_list(work / "ids.txt", ids)
        script = ("python3 /opt/eval/scorer/omni_predict.py --pred /pred --ids /work/ids.txt "
                  "--out /work/run/predictions && "
                  f"opb grade /work/run --dataset /data --manifest manifest-{split}.parquet > /work/grade.json")
        run(docker_cmd(tag("omni"), ["sh", "-ec", script],
                       mounts=[(REPO / "eval" / "scorer", "/opt/eval/scorer", True),
                               (root / "data" / "omni", "/data", True),
                               (root / "runs" / tool / suite, "/pred", True), (work, "/work", False)]),
            check=False, stdout_path=out / f"{tool}.log")
        summary = read_json(work / "run" / "unit_tests" / "summary.json")
        if not summary or summary.get("score") is None:
            failed += 1
            log(f"{suite}/{tool}: not scored, see {out / f'{tool}.log'}")
            continue
        families = summary.get("families", {})
        metrics = {"headline": summary["score"]}
        for family in ("text", "tables", "layout"):
            metrics[family] = families.get(family, {}).get("score")
        write_json(out / f"{tool}.json", {"schema": 1, "pages": len(ids), "metrics": metrics,
                                          "families": families})
        if suite not in AGGREGATE_ONLY:
            tests = work / "run" / "unit_tests" / "scores.jsonl"
            if tests.is_file():
                shutil.copyfile(tests, out / f"{tool}-tests.jsonl")
        shutil.rmtree(work)
        log(f"{suite}/{tool}: scored {metrics}")
    return failed


def score_multiscript(root: Path, tools: list[str]) -> None:
    """CER and word F1 of each tool's Markdown against the item's text (eval/scorer/ocr_text.py)."""
    out = root / "scores" / "multiscript"
    out.mkdir(parents=True, exist_ok=True)
    for tool in tools:
        argv = ["python3", "/opt/eval/scorer/ocr_text.py", "--gt-dir", "/gt", "--pred-dir", "/pred",
                "--out", f"/out/{tool}.json"]
        run(docker_cmd(tag("scorer"), argv,
                       mounts=[(REPO / "eval" / "scorer", "/opt/eval/scorer", True),
                               (root / "multiscript" / "gt", "/gt", True),
                               (root / "runs" / tool / "multiscript", "/pred", True), (out, "/out", False)]),
            stdout_path=out / f"{tool}.md")
        log(f"multiscript/{tool}: scored")


def cmd_score(args) -> int:
    if args.selftest:
        return score_selftest(args)
    if not args.suite:
        raise SystemExit("score needs --selftest or --suite")
    tools = available_tools(args.root, args.suite, args.tools)
    if not tools and args.suite != "odl":
        raise SystemExit(f"no runs for suite {args.suite}; run `convert` first")
    if args.suite == "synthetic":
        score_fixture_suite(args.root, "synthetic",
                            [i for i in corpus_items(args.root) if i["valid"]], tools)
    elif args.suite == "scanned":
        score_fixture_suite(args.root, "scanned",
                            [i for i in scanned_items(args.root) if i["valid"]], tools)
    elif args.suite == "odl":
        score_odl(args.root, tools, args.force)
    elif args.suite == "multiscript":
        score_multiscript(args.root, tools)
    elif args.suite in ("olmocr", "olmocr-scans"):
        score_olmocr(args.root, tools, args.suite)
    elif args.suite == "olmocr-math":
        score_olmocr(args.root, tools, args.suite, ["arxiv_math"])
    elif args.suite in ("doclaynet-dev", "doclaynet-val"):
        score_doclaynet(args.root, tools, args.suite)
    elif args.suite in ("omni-dev", "omni-val"):
        return 1 if score_omni(args.root, tools, args.suite) else 0
    else:
        return 1 if score_readoc(args.root, tools, args.suite) else 0
    return 0


# --------------------------------------------------------------------------------------------
# speed


def cmd_speed(args) -> int:
    root: Path = args.root
    tool = args.tool
    pages_odl = odl_pages(root)
    corpus = {i["id"]: i for i in corpus_items(root)}
    ids = [f"odl/{i}" for i in sorted(pages_odl)]
    ids += [f"synthetic/{i}" for i in SPEED_SYNTHETIC if corpus.get(i, {}).get("valid")]
    pages = {f"odl/{k}": v for k, v in pages_odl.items()}
    pages.update({f"synthetic/{k}": v["pages"] for k, v in corpus.items()})
    cold_id = next(i for i in ids if pages.get(i) == 1)
    out = reset_dir(root / "speed" / tool)
    write_list(out / "list.txt", ids)
    write_list(out / "cold.txt", [cold_id])
    mounts = [(suite_root(root, "odl"), "/in/odl", True),
              (suite_root(root, "synthetic"), "/in/synthetic", True),
              (out, "/out", False)] + tool_mounts(tool, args.bin_dir)
    env = {k: "1" for k in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                            "RAYON_NUM_THREADS")}
    warm = ["/usr/bin/time", "-f", "%M", "-o", "/out/warm-time.txt"] + tool_argv(
        tool, "/out/list.txt", "/out/warm", ["--warmup", "1", "--repeat", "3"], args.cmd)
    run(docker_cmd(tool_tag(tool), warm, mounts=mounts, timing=True, env=env), check=False)
    one = " ".join(shlex_quote(a) for a in tool_argv(tool, "/out/cold.txt", "/out/cold-$i", [],
                                                       args.cmd))
    one = one.replace("'/out/cold-$i'", '"/out/cold-$i"')
    cold = ("for i in 1 2 3 4 5; do /usr/bin/time -f '%e %M' -o /out/cold-$i.txt " + one
            + " >/dev/null 2>&1 || true; done")
    run(docker_cmd(tool_tag(tool), ["sh", "-c", cold], mounts=mounts, timing=True, env=env),
        check=False)

    rows = read_jsonl(out / "warm" / "results.jsonl")
    meta = read_json(out / "warm" / "meta.json", {})
    timing_kind = meta.get("timing")
    per_doc = {}
    ok_ms, ok_pages, ms_per_page = 0.0, 0, []
    for row in rows:
        n = pages.get(row["id"])
        per_doc[row["id"]] = {"status": row["status"], "ms": row.get("ms"), "pages": n}
        if row["status"] == "ok" and row.get("ms") is not None and n:
            ok_ms += row["ms"]
            ok_pages += n
            ms_per_page.append(row["ms"] / n)
    batch = read_json(out / "warm" / "batch.json")
    if batch and batch.get("total_ms"):
        ok_docs = [r["id"] for r in rows if r["status"] in ("ok", "empty")]
        ok_pages = sum(pages.get(i) or 0 for i in ok_docs)
        ok_ms = batch["total_ms"]
    cold_s, cold_rss = [], []
    for i in range(1, 6):
        parts = (out / f"cold-{i}.txt").read_text().split() if (out / f"cold-{i}.txt").is_file() else []
        if len(parts) >= 2 and re.fullmatch(r"[\d.]+", parts[-2]) and parts[-1].isdigit():
            cold_s.append(float(parts[-2]))
            cold_rss.append(int(parts[-1]))
    warm_rss = None
    if (out / "warm-time.txt").is_file():
        last = (out / "warm-time.txt").read_text().split()
        warm_rss = int(last[-1]) if last and last[-1].isdigit() else None
    result = {
        "schema": 1, "tool": tool, "date": datetime.date.today().isoformat(),
        "timing": timing_kind, "cpu": f"cpuset {TIMING_CPU}, --cpus=1",
        "docs": len(ids), "records": len(rows),
        "ok": sum(1 for r in rows if r["status"] == "ok"),
        "failed": sum(1 for r in rows if r["status"] in ("error", "timeout")),
        "empty": sum(1 for r in rows if r["status"] == "empty"),
        "pages": ok_pages, "total_ms": ok_ms,
        "pages_per_s": (ok_pages / (ok_ms / 1000)) if ok_ms else None,
        "ms_per_page_median": median(ms_per_page), "ms_per_page_p95": p95(ms_per_page),
        "cold_start_s": median(cold_s), "cold_start_runs": len(cold_s),
        "cold_start_peak_rss_kib": max(cold_rss) if cold_rss else None,
        "peak_rss_kib": warm_rss, "cold_id": cold_id, "per_doc": per_doc,
    }
    write_json(root / "speed" / f"{tool}.json", result)
    log(f"speed {tool}: {result['pages_per_s']} pages/s, cold {result['cold_start_s']} s, "
        f"rss {warm_rss} KiB")
    return 0


def shlex_quote(arg: str) -> str:
    import shlex
    return shlex.quote(arg)


# --------------------------------------------------------------------------------------------
# census


def cmd_census(args) -> int:
    data = args.root / "data" / "safedocs"
    if not list(data.glob("*.zip")):
        raise SystemExit(f"no zips in {data}; run `fetch --only safedocs` first")
    out = reset_dir(args.root / "census")
    argv = ["python3", "/opt/eval/census/census.py", "--zips", "/data", "--out", "/out",
            "--jobs", str(args.jobs)]
    if args.limit:
        argv += ["--limit", str(args.limit)]
    run(docker_cmd(tag("producers"), argv, mounts=[(data, "/data", True), (out, "/out", False)]))
    summary = read_json(out / "summary.json", {})
    log(f"census: {summary.get('files_processed')} files, R_upper={summary.get('R_upper')}")
    return 0


# --------------------------------------------------------------------------------------------
# report


def fmt(value, digits: int = 3) -> str:
    if value is None:
        return "–"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def table(header: list[str], rows: list[list]) -> list[str]:
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(fmt(c) if not isinstance(c, str) else c.replace("|", "\\|")
                              for c in row) + " |"
            for row in rows]
    return out + [""]


def tool_versions(images: dict) -> dict[str, str]:
    out = {}
    for name, info in images.get("images", {}).items():
        versions = info.get("versions", {})
        flat = [f"{k} {v}" for k, v in versions.items() if isinstance(v, str)]
        out[name] = "; ".join(flat)[:400]
    return out


def construct_pool(docs: list[dict]) -> dict:
    """Micro-average construct counts over documents (missing docs count as empty output)."""
    cats = {c: {"gt": 0, "pred": 0, "matched": 0} for c in CONSTRUCT_CATEGORIES}
    attrs = {a: [0, 0] for a in CONSTRUCT_ATTRS}
    math = [0, 0]
    image = [0, 0, 0, 0]
    for d in docs:
        if not d:
            continue
        for cat, v in d.get("categories", {}).items():
            if cat in cats:
                for k in ("gt", "pred", "matched"):
                    cats[cat][k] += v.get(k, 0)
            for name, a in (v.get("attr") or {}).items():
                key = name if "." in name else f"{cat}.{name}"
                if key in attrs:
                    attrs[key][0] += a.get("correct", 0)
                    attrs[key][1] += a.get("total", 0)
        m = d.get("math") or {}
        math[0] += m.get("found", 0)
        math[1] += m.get("gt", 0)
        im = d.get("image") or {}
        image[0] += im.get("gt", 0)
        image[1] += min(im.get("pred", 0), im.get("gt", 0))
        image[2] += im.get("alt_matched", 0)
        image[3] += im.get("files_exist", 0)

    def f1(c):
        if c["gt"] == 0 and c["pred"] == 0:
            return None
        p = c["matched"] / c["pred"] if c["pred"] else 0.0
        r = c["matched"] / c["gt"] if c["gt"] else 0.0
        return 2 * p * r / (p + r) if p + r else 0.0

    total = {k: sum(c[k] for c in cats.values()) for k in ("gt", "pred", "matched")}
    return {"f1": {c: f1(v) for c, v in cats.items()}, "all": f1(total),
            "attr": {a: (v[0] / v[1] if v[1] else None) for a, v in attrs.items()},
            "math_recall": math[0] / math[1] if math[1] else None,
            "image_recall": image[1] / image[0] if image[0] else None}


def odl_means(items: list[dict]) -> dict:
    return {m: mean((i.get("odl") or {}).get(m) if (i.get("odl") or {}) else
                    (0.0 if m == "nid" else None) for i in items) for m in ("nid", "teds", "mhs")}


def report_synthetic(root: Path) -> list[str]:
    lines = ["## 3. Synthetic", ""]
    scored = {t: read_json(root / "scores" / "synthetic" / f"{t}.json")
              for t in TOOLS if (root / "scores" / "synthetic" / f"{t}.json").is_file()}
    if not scored:
        return lines + ["Not scored.", ""]
    lines += ["NID/TEDS/MHS: opendataloader-bench evaluator means over documents where the metric "
              "applies. Construct F1: micro-averaged over documents. Development items exclude "
              "the weasyprint holdout. \"overall\" = macro-average over the 4 classes.", ""]
    for tagged in (False, True):
        label = "tagged" if tagged else "untagged"
        rows = []
        for tool, data in scored.items():
            dev = [i for i in data["items"] if not i["holdout"] and bool(i["tagged"]) == tagged]
            if not dev:
                continue
            row = [tool, str(len(dev))]
            per_class = {}
            for cls in CLASSES:
                group = [i for i in dev if i["class"] == cls]
                per_class[cls] = odl_means(group) if group else None
            for metric in ("nid", "teds", "mhs"):
                vals = [per_class[c][metric] for c in CLASSES
                        if per_class[c] and per_class[c][metric] is not None]
                row.append(mean(vals))
            for cls in CLASSES:
                row.append(per_class[cls]["nid"] if per_class[cls] else None)
            row.append(construct_pool([i["constructs"] for i in dev])["all"])
            rows.append(row)
        if rows:
            lines += [f"### 3.{1 if not tagged else 2} Development, {label} items", ""]
            lines += table(["tool", "items", "NID overall", "TEDS overall", "MHS overall",
                            "NID renderer", "NID office", "NID academic", "NID book",
                            "construct F1 (all)"], rows)
    for tagged in (False, True):
        label = "tagged" if tagged else "untagged"
        rows, arows = [], []
        for tool, data in scored.items():
            dev = [i for i in data["items"] if not i["holdout"] and bool(i["tagged"]) == tagged]
            if not dev:
                continue
            pool = construct_pool([i["constructs"] for i in dev])
            rows.append([tool] + [pool["f1"][c] for c in CONSTRUCT_CATEGORIES])
            arows.append([tool] + [pool["attr"][a] for a in CONSTRUCT_ATTRS]
                         + [pool["math_recall"], pool["image_recall"]])
        if rows:
            lines += [f"### Construct F1 per category, development {label}", ""]
            lines += table(["tool"] + CONSTRUCT_CATEGORIES, rows)
            lines += [f"### Attribute accuracy, math and image recall, development {label}", ""]
            lines += table(["tool"] + CONSTRUCT_ATTRS + ["math recall", "image recall"], arows)
    rows = []
    for tool, data in scored.items():
        hold = [i for i in data["items"] if i["holdout"]]
        if hold:
            m = odl_means(hold)
            rows.append([tool, str(len(hold)), m["nid"], m["teds"], m["mhs"],
                         construct_pool([i["constructs"] for i in hold])["all"]])
    if rows:
        lines += ["### Holdout (weasyprint), aggregate only", ""]
        lines += table(["tool", "items", "NID", "TEDS", "MHS", "construct F1 (all)"], rows)
    producers = sorted({(i["producer"], bool(i["tagged"])) for d in scored.values()
                        for i in d["items"]})
    rows = []
    for producer, tagged in producers:
        row = [producer, "yes" if tagged else "no"]
        for tool, data in scored.items():
            group = [i for i in data["items"] if i["producer"] == producer
                     and bool(i["tagged"]) == tagged]
            m = odl_means(group)
            f = construct_pool([i["constructs"] for i in group])["all"]
            row.append(f"{fmt(m['nid'], 2)} / {fmt(f, 2)}")
        rows.append(row)
    lines += ["### Per producer: NID / construct F1 (all)", ""]
    lines += table(["producer", "tagged"] + list(scored), rows)
    return lines


def report(root: Path) -> str:
    images = read_json(root / "images.json", {"images": {}})
    lines = [f"# RayoPDF evaluation report", "",
             f"Generated {datetime.datetime.now():%Y-%m-%d %H:%M} by `tools/eval.py report`.", ""]

    # 1. setup
    cpu = next((l.split(":", 1)[1].strip() for l in Path("/proc/cpuinfo").read_text().splitlines()
                if l.startswith("model name")), "unknown")
    docker = subprocess.run(["docker", "version", "--format", "{{.Server.Version}}"],
                            capture_output=True, text=True).stdout.strip()
    lines += ["## 1. Setup", "", f"- Date: {datetime.date.today().isoformat()}",
              f"- CPU: {cpu} ({os.cpu_count()} logical CPUs); work cpuset `{WORK_CPUS}`, "
              f"timing cpuset `{TIMING_CPU}` with `--cpus=1`", f"- Docker: {docker}",
              f"- opendataloader-bench `{ODL_COMMIT}`; olmOCR-bench `{OLMOCR_REVISION}`; "
              f"READoc code `{READOC_COMMIT}`, data `{READOC_REVISION}`; SafeDocs "
              f"CC-MAIN-2021-31-PDF-UNTRUNCATED `0000.zip`", ""]
    rows = [[name, info.get("id", "")[7:19], f"{info.get('size', 0) / 1e9:.2f} GB",
             tool_versions(images).get(name, "")] for name, info in images["images"].items()]
    lines += table(["image", "id", "size", "versions"], rows)

    # 2. corpus validity
    lines += ["## 2. Corpus validity", ""]
    corpus = read_json(root / "corpus" / "corpus.json")
    if corpus:
        items = corpus["items"]
        producers = sorted({i["producer"] for i in items})
        fixtures = sorted({i["fixture"] for i in items})
        by = {(i["producer"], i["fixture"]): i for i in items}
        rows = []
        for f in fixtures:
            row = [f]
            for p in producers:
                i = by.get((p, f))
                row.append("" if i is None else ("ok" if i["valid"] else "**x**"))
            rows.append(row)
        valid = sum(1 for i in items if i["valid"])
        lines += [f"{valid} of {len(items)} items valid; blank = excluded by the manifest.", ""]
        lines += table(["fixture"] + producers, rows)
        bad = [[i["id"], i.get("reason") or ""] for i in items if not i["valid"]]
        if bad:
            lines += table(["invalid item", "reason"], bad)
        tagged = sorted({i["producer"] for i in items if i.get("tagged")})
        lines += [f"Producers with tagged output: {', '.join(tagged) or 'none'}.", ""]
    else:
        lines += ["Not run.", ""]

    # 3. synthetic
    lines += report_synthetic(root)

    # 4. odl
    lines += ["## 4. opendataloader-bench (200 documents)", ""]
    rows = []
    for tool in TOOLS:
        ev = read_json(root / "scores" / "odl" / "work" / tool / "evaluation.json")
        if ev:
            s, m = ev["metrics"]["score"], ev["metrics"]
            rows.append([tool, s["overall_mean"], s["nid_mean"], s["nid_s_mean"], s["teds_mean"],
                         s["teds_s_mean"], s["mhs_mean"], s["mhs_s_mean"],
                         str(m.get("missing_predictions"))])
    if rows:
        lines += ["Local runs (reproduced):", ""]
        lines += table(["tool", "overall", "NID", "NID-S", "TEDS", "TEDS-S", "MHS", "MHS-S",
                        "missing"], rows)
    rows = []
    for d in sorted((root / "scores" / "odl" / "stored").glob("*")):
        new = read_json(d / "evaluation.rescored.json")
        old = read_json(d / "evaluation.json")
        if not new:
            continue
        ns = new["metrics"]["score"]
        os_ = (old or {}).get("metrics", {}).get("score", {})
        diff = (abs(ns["overall_mean"] - os_["overall_mean"])
                if os_.get("overall_mean") is not None and ns["overall_mean"] is not None else None)
        differs = [k.replace("_mean", "") for k in ("nid_mean", "teds_mean", "mhs_mean")
                   if ns.get(k) is not None and os_.get(k) is not None
                   and abs(ns[k] - os_[k]) > 0.001]
        rows.append([d.name, os_.get("overall_mean"), ns["overall_mean"], diff, ns["nid_mean"],
                     ns["teds_mean"], ns["mhs_mean"], ", ".join(differs) or "none"])
    if rows:
        lines += ["Stored engine predictions shipped with the benchmark, rescored in our scorer "
                  "image against the pinned ground truth (their conversions were not run here). "
                  "A stored evaluation.json older than the benchmark's last ground-truth change "
                  "differs from the rescore in the metrics that change touched.", ""]
        lines += table(["engine", "stored overall", "rescored overall", "abs. diff", "NID", "TEDS",
                        "MHS", "metrics differing by > 0.001"], rows)

    # 5. olmOCR
    lines += ["## 5. olmOCR-bench (born-digital categories)", ""]
    rows, verbatim = [], []
    cats = list(OLMOCR_JSONL.values())
    for tool in TOOLS:
        path = root / "scores" / "olmocr" / f"{tool}.log"
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        rates = parse_olmocr_log(text)
        if not rates:
            verbatim += [f"`{tool}`: summary could not be parsed; its final block:", "", "```",
                         olmocr_summary_block(text), "```", ""]
            continue
        bd = [rates.get(c) for c in cats if c != "arxiv_math"]
        rows.append([tool] + [rates.get(c) for c in cats]
                    + [mean(bd) if all(v is not None for v in bd) else None])
    if rows:
        lines += ["Pass rate per JSONL (tests passed / tests); last column = mean of the 4 "
                  "born-digital categories excluding arxiv_math. Scanned categories "
                  "(`old_scans*`) are left to the M8 OCR gate.", ""]
        lines += table(["tool"] + cats + ["born-digital mean (excl. arxiv_math)"], rows)
    lines += verbatim
    if not rows and not verbatim:
        lines += ["Not scored.", ""]

    # 6. READoc
    lines += ["## 6. READoc-300 (holdout, aggregate only)", ""]
    lines += report_readoc(root, "readoc")

    # 7. speed
    lines += ["## 7. Speed and memory", "",
              "Single pinned core (`--cpuset-cpus=3 --cpus=1`, thread env vars = 1). 200 odl PDFs + "
              "6 multi-page synthetic PDFs; warm = median of 3 repeats after 1 warm-up; "
              "pages/s = pages of successful documents / sum of their medians; cold start = "
              "median wall time of 5 fresh adapter processes on one 1-page PDF; peak RSS = "
              "max RSS of the whole warm run.", ""]
    rows = []
    for tool in TOOLS:
        s = read_json(root / "speed" / f"{tool}.json")
        if s:
            rows.append([tool, s["timing"], s["pages_per_s"], s["ms_per_page_median"],
                         s["ms_per_page_p95"], s["cold_start_s"],
                         None if s["peak_rss_kib"] is None else s["peak_rss_kib"] / 1024,
                         f"{s['ok']}/{s['docs']}"])
    lines += table(["tool", "timing", "pages/s", "ms/page median", "ms/page p95",
                    "cold start s", "peak RSS MiB", "ok docs"], rows) if rows else ["Not run.", ""]

    # 8. failures
    lines += ["## 8. Failures", ""]
    rows = []
    for tool in TOOLS:
        for suite in SUITES:
            res = read_jsonl(root / "runs" / tool / suite / "results.jsonl")
            if not res:
                continue
            c = {k: sum(1 for r in res if r["status"] == k) for k in ("error", "timeout", "empty")}
            rows.append([tool, suite, str(len(res)), str(c["error"]), str(c["timeout"]),
                         str(c["empty"])])
    lines += table(["tool", "suite", "docs", "error", "timeout", "empty"], rows)
    for entry in read_json(root / "all-failures.json", []):
        lines.append(f"- not run: `{entry['step']}`: {entry['reason']}")
    lines.append("")

    # 9. census
    lines += ["## 9. Text-layer census (SafeDocs CC-MAIN-2021-31 0000.zip)", ""]
    summary = read_json(root / "census" / "summary.json")
    if summary:
        lines += [f"- files: {summary['files_total']} total, {summary['files_processed']} opened by "
                  f"pdfinfo; pages examined: {summary['pages_examined']} (≤ 50 per file)",
                  f"- tool failures: {json.dumps(summary['failures'])}",
                  f"- tagged {fmt(summary['tagged_pct'], 1)} %, encrypted "
                  f"{fmt(summary['encrypted_pct'], 1)} %, Type 3 font "
                  f"{fmt(summary['type3_pct'], 1)} %, non-Unicode font "
                  f"{fmt(summary['non_unicode_font_pct'], 1)} % of processed files",
                  f"- **R_upper = {fmt(summary['R_upper'] * 100, 2)} %** of examined pages are "
                  "image_only or broken (upper bound of the OCR need before RayoPDF's own "
                  "Unicode repair)", ""]
        classes = list(summary["page_class_pct"])
        lines += table(["scope", "docs", "pages"] + [f"{c} % pages" for c in classes],
                       [["all", str(summary["files_processed"]), str(summary["pages_examined"])]
                        + [summary["page_class_pct"][c] for c in classes]]
                       + [[fam, str(v["docs"]), str(v["pages"])]
                          + [v["page_class_pct"].get(c) for c in classes]
                          for fam, v in sorted(summary["by_family"].items(),
                                               key=lambda kv: -kv[1]["docs"])])
        lines += table(["class", "% of documents with ≥ 1 such page"],
                       [[c, v] for c, v in summary["doc_class_pct"].items()])
        lines += table(["script", "% of image_only + broken pages"],
                       [[k, v] for k, v in sorted(summary["ocr_need_script_mix"].items(),
                                                  key=lambda kv: -kv[1])])
    else:
        lines += ["Not run.", ""]
    lines += report_scanned(root)
    lines += report_olmocr_scans(root)
    lines += report_multiscript(root)
    lines += ["## 13. READoc development split (readoc-dev; not the holdout)", "",
              f"{READOC_DEV_PER_SUBSET} documents per subset that follow the holdout's in its "
              "selection order (`data/READoc-data/selection-dev.json`); none is in the holdout. "
              "Development data: each document's scores are in "
              "`scores/readoc-dev/<tool>-<subset>-docs.json`.", ""]
    lines += report_readoc(root, "readoc-dev")
    lines += ["## 14. READoc validation split (readoc-val; validation, aggregate only)", "",
              f"{READOC_VAL_PER_SUBSET} documents per subset that follow the development split's "
              "in the same order (`data/READoc-data/selection-val.json`); none is in the holdout "
              "or the development split. Quality landings are checked against it; like the "
              "holdout, only aggregate scores are written and looked at.", ""]
    lines += report_readoc(root, "readoc-val")
    lines += ["## 15. READoc second development split (readoc-dev2; not the holdout)", "",
              f"Up to {READOC_DEV2_PER_SUBSET} documents per subset that follow the validation "
              "split's in the same order (`data/READoc-data/selection-dev2.json`); none is in the "
              "holdout, the development or the validation split. Development data: each "
              "document's scores are in `scores/readoc-dev2/<tool>-<subset>-docs.json`.", ""]
    lines += report_readoc(root, "readoc-dev2")
    lines += report_olmocr_math(root)
    lines += report_doclaynet(root)
    lines += report_omni(root)
    return "\n".join(lines) + "\n"


def report_omni(root: Path) -> list[str]:
    """Section 18: OmniParseBench's unit tests on its PDF pages."""
    keys = ["headline", "text", "tables", "layout"]
    lines = ["## 18. OmniParseBench", "",
             f"`{OMNI_DATASET}` at `{OMNI_REVISION[:12]}`, its .pdf pages halved by the SHA-256 of their "
             "id (`data/omni/selection.json`), graded by `opb grade` (omni-parse-bench 1.5.0): the share "
             "of each family's tests passed and their mean, the headline. Only cli writes the blocks the "
             "layout tests read, so only cli is scored. omni-val is validation data: aggregate only.", ""]
    rows = []
    for suite in ("omni-dev", "omni-val"):
        for tool in TOOLS:
            result = read_json(root / "scores" / suite / f"{tool}.json")
            if result:
                rows.append([suite, tool, str(result["pages"])] + [result["metrics"].get(k) for k in keys])
    return lines + (table(["suite", "tool", "pages"] + keys, rows) if rows else ["Not scored.", ""])


def report_doclaynet(root: Path) -> list[str]:
    """Section 17: the DocLayNet suites' role metrics (eval/scorer/doclaynet.py)."""
    keys = ["text_kept", "furniture_f1", "heading_f1", "list_f1", "footnote_f1", "table_f1"]
    lines = ["## 17. DocLayNet roles", "",
             "Character-weighted roles of DocLayNet's text cells in the JSON output "
             "(`eval/scorer/doclaynet.py`): the kept share of body text, furniture left out, and "
             "headings, list items, footnotes and tables among the kept cells. Only cli writes "
             "boxes, so only cli is scored. doclaynet-val is validation data: aggregate only.", ""]
    rows = []
    for suite in ("doclaynet-dev", "doclaynet-val"):
        for tool in TOOLS:
            result = read_json(root / "scores" / suite / f"{tool}.json")
            if result:
                rows.append([suite, tool, str(result["pages"])] + [result["metrics"].get(k) for k in keys])
    return lines + (table(["suite", "tool", "pages"] + keys, rows) if rows else ["Not scored.", ""])


def report_olmocr_math(root: Path) -> list[str]:
    """olmOCR's arxiv_math category converted by cli in its declared profile (eval/profiles.json)."""
    declared = read_json(PROFILES, {}).get("declared", {}).get("olmocr-math", "")
    lines = ["## 16. olmOCR arxiv_math in its declared mode", "",
             f"cli with `{declared}` (its declared profile, `eval/profiles.json`); the other tools' "
             "arxiv_math pass rates are in section 5.", ""]
    path = root / "scores" / "olmocr-math" / "cli.log"
    if not path.is_file():
        return lines + ["Not scored.", ""]
    text = path.read_text(encoding="utf-8", errors="replace")
    rate = parse_olmocr_log(text).get("arxiv_math")
    if rate is None:
        return lines + ["The summary could not be parsed; its final block:", "", "```", olmocr_summary_block(text),
                        "```", ""]
    return lines + table(["tool", "arxiv_math"], [["cli", rate]])


def report_readoc(root: Path, suite: str) -> list[str]:
    """The evaluator's aggregate per subset and tool (scores/<suite>/<tool>-<subset>.json)."""
    lines = []
    for sub in READOC_SUBSETS:
        keys: list[str] = []
        results = {}
        for tool in TOOLS:
            res = read_json(root / "scores" / suite / f"{tool}-{sub}.json")
            if res is None:
                continue
            flat = flatten_numbers(res)
            results[tool] = flat
            keys += [k for k in flat if k not in keys]
        if results:
            lines += [f"### {sub}", ""]
            lines += table(["metric"] + list(results),
                           [[k] + [results[t].get(k) for t in results] for k in keys])
    return lines or ["Not scored.", ""]


def report_multiscript(root: Path) -> list[str]:
    lines = ["## 12. Multiscript suite (image-only pages in nine scripts, upright and turned)", "",
             "CER pooled / word F1 per script (eval/scorer/ocr_text.py; development only, no holdout).", ""]
    scored = {t: read_json(root / "scores" / "multiscript" / f"{t}.json")
              for t in TOOLS if (root / "scores" / "multiscript" / f"{t}.json").is_file()}
    if not scored:
        return lines + ["Not run.", ""]
    scripts = sorted({s for res in scored.values() for s in res["scripts"]})
    rows = []
    for tool, res in scored.items():
        cells = [res["scripts"].get(s) for s in scripts] + [res["totals"]]
        rows.append([tool] + [f"{c['pooled_cer']:.4f} / {c['word_f1']:.4f}" if c else "–" for c in cells])
    return lines + table(["tool"] + scripts + ["all"], rows)


def report_scanned(root: Path) -> list[str]:
    lines = ["## 10. Scanned suite (image-only PDFs of the synthetic items)", ""]
    scored = {t: read_json(root / "scores" / "scanned" / f"{t}.json")
              for t in TOOLS if (root / "scores" / "scanned" / f"{t}.json").is_file()}
    data = read_json(root / "scanned" / "scanned.json")
    if data:
        items = data["items"]
        valid = [i for i in items if i["valid"]]
        lines += [f"{len(valid)} of {len(items)} items valid, "
                  f"{sum(i['pages'] for i in valid)} pages; ground truth = the fixture Markdown; "
                  "items made from the weasyprint producer are holdout (aggregate only). "
                  "Metrics as in section 3 (missing or failed output scores NID 0).", ""]
        combos = {}
        for i in valid:
            key = (i["dpi"], i["codec"])
            combos[key] = combos.get(key, 0) + 1
        lines += table(["dpi"] + list(CODECS_SCANNED),
                       [[str(d)] + [str(combos.get((d, c), 0)) for c in CODECS_SCANNED]
                        for d in (300, 200)])
    if not scored:
        return lines + ["Not scored.", ""]
    rows, codec_rows, deg_rows, f1_rows, hold_rows = [], [], [], [], []
    for tool, res in scored.items():
        dev = [i for i in res["items"] if not i["holdout"]]
        hold = [i for i in res["items"] if i["holdout"]]
        m = odl_means(dev)
        pool = construct_pool([i["constructs"] for i in dev])
        ok = sum(1 for i in dev if i["status"] == "ok")
        timed = [i for i in dev if i.get("ms") is not None and i["pages"]]
        s_per_page = (sum(i["ms"] for i in timed) / 1000 / sum(i["pages"] for i in timed)
                      if timed else None)
        rows.append([tool, str(len(dev)), f"{ok}", m["nid"], m["teds"], m["mhs"], pool["all"],
                     pool["f1"]["heading"], pool["attr"]["heading.level"],
                     odl_means([i for i in dev if i["dpi"] == 300])["nid"],
                     odl_means([i for i in dev if i["dpi"] == 200])["nid"], s_per_page])
        codec_rows.append([tool] + [odl_means([i for i in dev if i["codec"] == c])["nid"]
                                    for c in CODECS_SCANNED])
        deg_rows.append([tool] + [odl_means([i for i in dev if i["degradation"] == d])["nid"]
                                  for d in DEGRADATIONS_SCANNED])
        f1_rows.append([tool] + [pool["f1"][c] for c in CONSTRUCT_CATEGORIES])
        if hold:
            hm = odl_means(hold)
            hold_rows.append([tool, str(len(hold)), hm["nid"], hm["teds"], hm["mhs"],
                              construct_pool([i["constructs"] for i in hold])["all"]])
    lines += ["### 10.1 Development items", "",
              "s/page: conversion time of the quality run over its pages (work cpuset shared with "
              "other jobs, `OMP_NUM_THREADS=4`); a rough cost indication, not a speed result.", ""]
    lines += table(["tool", "items", "ok", "NID", "TEDS", "MHS", "construct F1 (all)",
                    "heading F1", "heading level acc.", "NID 300 dpi", "NID 200 dpi", "s/page"],
                   rows)
    lines += ["### 10.2 NID per codec, development", ""]
    lines += table(["tool"] + list(CODECS_SCANNED), codec_rows)
    lines += ["### 10.3 NID per degradation, development", ""]
    lines += table(["tool"] + list(DEGRADATIONS_SCANNED), deg_rows)
    lines += ["### 10.4 Construct F1 per category, development", ""]
    lines += table(["tool"] + CONSTRUCT_CATEGORIES, f1_rows)
    if hold_rows:
        lines += ["### 10.5 Holdout (weasyprint sources), aggregate only", ""]
        lines += table(["tool", "items", "NID", "TEDS", "MHS", "construct F1 (all)"], hold_rows)
    return lines


def report_olmocr_scans(root: Path) -> list[str]:
    lines = ["## 11. olmOCR-bench scans (old_scans, old_scans_math)", ""]
    rows, verbatim = [], []
    cats = list(OLMOCR_SCAN_JSONL.values())
    for tool in TOOLS:
        path = root / "scores" / "olmocr-scans" / f"{tool}.log"
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        rates = parse_olmocr_log(text)
        if not rates:
            verbatim += [f"`{tool}`: summary could not be parsed; its final block:", "", "```",
                         olmocr_summary_block(text), "```", ""]
            continue
        vals = [rates.get(c) for c in cats]
        rows.append([tool] + vals + [mean(vals) if all(v is not None for v in vals) else None])
    if rows:
        lines += ["Pass rate per JSONL (tests passed / tests), olmOCR scorer as in section 5. "
                  "Empty output passes the 70 `absent` tests of old_scans (floor 0.133); "
                  "old_scans_math has only `math` tests.", ""]
        lines += table(["tool"] + cats + ["mean"], rows)
    lines += verbatim
    if not rows and not verbatim:
        lines += ["Not scored.", ""]
    return lines


def flatten_numbers(data, prefix: str = "") -> dict[str, float]:
    out = {}
    if isinstance(data, dict):
        for k, v in data.items():
            out.update(flatten_numbers(v, f"{prefix}{k}." if not isinstance(v, (int, float))
                                       else f"{prefix}{k}"))
    elif isinstance(data, (int, float)) and not isinstance(data, bool):
        out[prefix.rstrip(".")] = float(data)
    return out


OLMOCR_RESULT = re.compile(r"^\s+(\S+)\.jsonl\s*:\s*([\d.]+)% \((\d+)/(\d+) tests\)", re.M)


def parse_olmocr_log(text: str) -> dict[str, float]:
    """Per-JSONL pass rates (fractions 0-1) from the benchmark's "Results by JSONL file" block."""
    summary = text[text.rfind("Final Summary"):] if "Final Summary" in text else ""
    return {m.group(1): int(m.group(3)) / int(m.group(4))
            for m in OLMOCR_RESULT.finditer(summary) if int(m.group(4))}


def olmocr_summary_block(text: str) -> str:
    return text[text.rfind("Final Summary"):].strip() if "Final Summary" in text else text[-2000:]


def cmd_report(args) -> int:
    text = report(args.root)
    (args.root / "report.md").write_text(text, encoding="utf-8")
    log(f"wrote {args.root / 'report.md'}")
    return 0


# --------------------------------------------------------------------------------------------
# all


def images_current(root: Path) -> bool:
    """True when images.json records every image under the id it has now."""
    recorded = read_json(root / "images.json", {}).get("images", {})
    for name in IMAGES:
        proc = subprocess.run(["docker", "image", "inspect", "-f", "{{.Id}}", tag(name)],
                              capture_output=True, text=True)
        if proc.returncode or recorded.get(name, {}).get("id") != proc.stdout.strip():
            return False
    return True


def footprint(root: Path) -> int:
    """Bytes already used by this harness (rayopdf-eval images + --root)."""
    total = 0
    for name in IMAGES:
        proc = subprocess.run(["docker", "image", "inspect", "-f", "{{.Size}}", tag(name)],
                              capture_output=True, text=True)
        if proc.returncode == 0 and proc.stdout.strip().isdigit():
            total += int(proc.stdout.strip())
    for path in root.rglob("*") if root.is_dir() else []:
        if path.is_file() and not path.is_symlink():
            total += path.stat().st_size
    return total


def cmd_all(args) -> int:
    root: Path = args.root
    root.mkdir(parents=True, exist_ok=True)
    preflight(root, max(0, FULL_RUN_NEED - footprint(root)), "a full run (images + data)")
    failures: list[dict] = []

    def step(name: str, func, ns: argparse.Namespace) -> None:
        log(f"=== {name}")
        try:
            code = func(ns)
        except SystemExit as exc:
            code = exc.code if isinstance(exc.code, int) else 1
            if not isinstance(exc.code, int):
                print(exc.code, file=sys.stderr)
        if code:
            failures.append({"step": name, "reason": f"exit {code}"})
            write_json(root / "all-failures.json", failures)

    def ns(**kw) -> argparse.Namespace:
        base = {"root": root, "only": None, "producers": None, "jobs": 6, "cmd": None, "profile": None,
                "bin_dir": None, "selftest": False, "suite": None, "tools": None, "force": False,
                "limit": None, "tool": None}
        base.update(kw)
        return argparse.Namespace(**base)

    write_json(root / "all-failures.json", [])
    if args.resume and images_current(root):
        log("=== images: every image is built and recorded, skipped (--resume)")
    else:
        step("images", cmd_images, ns())
    step("fetch", cmd_fetch, ns())
    if args.resume and (root / "corpus" / "corpus.json").is_file():
        log("=== corpus: corpus.json present, skipped (--resume)")
    else:
        step("corpus", cmd_corpus, ns(suite="synthetic"))
    if args.resume and (root / "scanned" / "scanned.json").is_file():
        log("=== corpus --suite scanned: scanned.json present, skipped (--resume)")
    else:
        step("corpus --suite scanned", cmd_corpus, ns(suite="scanned"))
    if args.resume and (root / "multiscript" / "multiscript.json").is_file():
        log("=== corpus --suite multiscript: multiscript.json present, skipped (--resume)")
    else:
        step("corpus --suite multiscript", cmd_corpus, ns(suite="multiscript"))
    step("score --selftest", cmd_score, ns(selftest=True))
    # The third-party tools; cli runs are made apart (with a binary and a profile), and the suites
    # of cli alone (CLI_SUITES) are scored here only when one is there.
    for tool in TOOLS[:-1]:
        # The OCR reference tools run on the scanned suites (and those OCR_TOOL_SUITES adds).
        for suite in OCR_TOOL_SUITES.get(tool, []) + SCAN_SUITES if tool in OCR_TOOLS else SUITES:
            if suite in CLI_SUITES:
                continue
            if args.resume and run_complete(root, tool, suite):
                log(f"=== convert {tool}/{suite}: complete, skipped (--resume)")
                continue
            step(f"convert --tool {tool} --suite {suite}", cmd_convert, ns(tool=tool, suite=suite))
    for suite in SUITES:
        if suite in CLI_SUITES and not available_tools(root, suite, None):
            continue
        step(f"score --suite {suite}", cmd_score, ns(suite=suite))
    for tool in TOOLS[:-1]:
        if tool in OCR_TOOLS:
            continue
        if args.resume and (root / "speed" / f"{tool}.json").is_file():
            log(f"=== speed {tool}: present, skipped (--resume)")
            continue
        step(f"speed --tool {tool}", cmd_speed, ns(tool=tool))
    if not (args.resume and (root / "census" / "summary.json").is_file()):
        step("census", cmd_census, ns(jobs=8))
    step("report", cmd_report, ns())
    log(f"all: {len(failures)} failed step(s)")
    return 1 if failures else 0


# --------------------------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    sub = parser.add_subparsers(dest="command", required=True)

    def add(name: str, func, help_text: str) -> argparse.ArgumentParser:
        p = sub.add_parser(name, help=help_text)
        p.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                       help="output root (default: benchmark-output/eval)")
        p.set_defaults(func=func)
        return p

    p = add("all", cmd_all, "run every step in order and write the report")
    p.add_argument("--resume", action="store_true",
                   help="skip conversions, speed runs and the census whose outputs are complete")
    p = add("images", cmd_images, "build the Docker images and record their versions")
    p.add_argument("--only", help=f"comma-separated subset of: {', '.join(IMAGES)}")
    p = add("fetch", cmd_fetch, "download the public benchmarks (network)")
    p.add_argument("--only", help=f"comma-separated subset of: {', '.join(DATASETS)}")
    p = add("corpus", cmd_corpus, "render the synthetic corpus, or build the scanned suite from it")
    p.add_argument("--suite", choices=["synthetic", "scanned", "multiscript"], default="synthetic")
    p.add_argument("--producers", help="synthetic only: comma-separated producer ids")
    p.add_argument("--only", help="comma-separated fixture ids (synthetic) or source item ids "
                                  "<producer>/<fixture> (scanned)")
    p.add_argument("--jobs", type=int, default=6)
    p = add("convert", cmd_convert, "convert one suite with one tool")
    p.add_argument("--tool", required=True, choices=TOOLS)
    p.add_argument("--suite", required=True, choices=SUITES)
    command = p.add_mutually_exclusive_group()
    command.add_argument("--cmd", help="cli only: command template with {in} and {out}")
    command.add_argument("--profile", choices=["default", "declared"],
                         help="cli only: `rayopdf --convert {in} {out}`, with the suite's options from "
                              "eval/profiles.json for declared")
    p.add_argument("--bin-dir", help="cli only: host directory mounted at /opt/tool")
    p = add("score", cmd_score, "score conversions of one suite, or run the scorer self-test")
    p.add_argument("--selftest", action="store_true")
    p.add_argument("--suite", choices=SUITES)
    p.add_argument("--tools", help="comma-separated tools (default: every tool with a run)")
    p.add_argument("--force", action="store_true", help="odl: rescore stored predictions again")
    p = add("speed", cmd_speed, "single-core speed and memory of one tool")
    p.add_argument("--tool", required=True, choices=TOOLS)
    p.add_argument("--cmd", help="cli only: command template with {in} and {out}")
    p.add_argument("--bin-dir", help="cli only: host directory mounted at /opt/tool")
    p = add("census", cmd_census, "text-layer census of the SafeDocs sample")
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--limit", type=int, help="process only the first N PDFs")
    add("report", cmd_report, "write report.md from all recorded outputs")
    args = parser.parse_args()
    args.root = args.root.resolve()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
