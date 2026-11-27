#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure RayoPDF's speed the way docs/development/performance.md and the version records do.

Every measurement but `hashes` runs in Docker, without network, on the CPUs given: RayoPDF's
binaries in the measuring image rayopdf-perf:1 (docker/perf.Dockerfile: the build image plus
valgrind and perf),
its WebAssembly module in rayopdf-wasm:1 (docker/wasm.Dockerfile, Node.js 22), other tools in
their evaluation images rayopdf-eval-<tool>:1 (`tools/eval.py images`). Binaries
and documents are mounted read-only, outputs go to a RAM disk inside the container, and
RAYOPDF_OCR names nothing, so an installed OCR add-on is never found. Give static binaries
(`tools/dev.py build --config static`) or binaries built in rayopdf-build:2; compare binaries
kept as files, never a build tree that changes under the measurement. A time is worth quoting
only from a CPU nothing else runs on (its sibling thread idle too); `identical` and `instr`
do not depend on the load.

Usage:
  python3 tools/bench.py warm --bin NAME=PATH [--bin ...] --dir DIR [--dir ...]
                              [--opt=--images=none ...] [--runs 3] [--rounds 2] [--cpu 3]
      In-process speed. Per document `rayopdf --bench DOC TMP RUNS OPTIONS` (one conversion
      untimed, then RUNS timed in the same process) and its fastest run; the rounds alternate the
      binaries and a document keeps its best time per binary. Prints ms/page median, p95 and mean
      over the documents, pages/s (pages / sum of the times) and the change against the first
      binary.

  python3 tools/bench.py fresh --bin NAME=PATH [--bin ...] --dir DIR [--dir ...]
                               [--opt=--images=none ...] [--runs 11] [--warmup 2] [--rounds 12]
                               [--cpu 3] [--per-doc]
      Fresh-process time. tools/bench/fresh_timer.c (posix_spawn to wait4, built in the image
      for the run) starts `rayopdf --convert DOC OUT OPTIONS` WARMUP times untimed and RUNS times
      timed per document; a round runs every binary once, and the order rotates from round to
      round. Prints the median over the rounds of the per-round median document time, its range,
      the paired change against the first binary (median of the per-round changes), p95, mean,
      CPU time and page faults. --per-doc keeps each document's time, CPU time and faults (the
      median over the rounds of its median of RUNS) per binary in --json, and prints the ten
      documents slowest for the first binary.

  python3 tools/bench.py identical --a PATH --b PATH --input DIR [--input ...]
                                   [--set NAME=OPTIONS ...] [--a-opt=OPTION ...] [--b-opt=OPTION ...]
                                   [--jobs N] [--cpus LIST] [--b-emulator PROGRAM] [--image TAG]
      Output identity. Every PDF under the inputs (subfolders too) is converted by both binaries
      with each option set (`--convert DOC out.md --report OPTIONS`; default sets:
      none=--images=none and images=, which writes the image files); the exit code, the --report
      record without its "ms" and every file written are compared. --a-opt and --b-opt add options
      to one side only, after the set's, so that one binary can be compared with itself, for
      example serial against page threads (--a-opt=--workers=1 --b-opt=--workers=4). Prints the
      counts and the first differences; exit code 1 when anything differs or a conversion ran into
      --timeout.
      --b-emulator runs B through that program in the container, --image names the container's
      image (default rayopdf-perf:1): a static Linux arm64 binary against the x64 one with
      `--b-emulator qemu-aarch64-static --image rayopdf-cross:1` (`tools/dev.py image --name cross`).

  python3 tools/bench.py hashes --bin PATH --dir DIR [--dir ...] [--set NAME=OPTIONS ...] [--jobs N]
                                --json FILE
  python3 tools/bench.py hashes --compare A.json B.json [--show 20]
      Output identity across machines. On this host, without Docker (macOS runners have none):
      every PDF under the folders converted by the binary with each option set, as `identical`
      does, and the exit code, the SHA-256 of the --report record without "ms" and "input" (the
      path differs between machines) and of every file written kept in --json; --compare prints
      the outputs that differ between the files of two platforms and exits with 1 when any does.

  python3 tools/bench.py instr --bin NAME=PATH [--bin ...] --doc PDF [--doc ...]
                               [--opt=--images=none ...] [--n 5] [--cpus LIST]
      Instructions per conversion under callgrind: (I(--bench DOC TMP 2N) - I(--bench DOC TMP N)) / N,
      the same on every run to about 0.01 %, and the change against the first binary.

  python3 tools/bench.py tools --bin PATH --doc NAME=PDF [--doc ...] [--cpus LIST ...]
                               [--only TOOL ...]
      RayoPDF and the other tools, each through the command line its package installs (TOOLS), as
      a fresh process from start to exit: tools/bench/cmdbench.c gives the median of N runs after
      warm-up runs and the peak resident memory. Each tool runs in its own image with as many
      threads as the CPU list has CPUs (default: --cpus 3; repeat --cpus for more tables).
      Prints a table with the times, the factor against RayoPDF and the peak RSS.

  python3 tools/bench.py pages --bin PATH --list FILE|--dir DIR [...] [--workers "0 1 2 4"]
                               [--cpus 2,3,4,5] [--runs 15] [--warmup 2]
      Documents of several pages on several cores: the fresh wall time of
      `rayopdf --convert DOC OUT --images=none --workers=N` per document and worker count
      (0: no --workers option, the default), summed over the documents, and the ratios against
      one worker. --list names a file with one PDF path a line.

  python3 tools/bench.py serve --bin NAME=PATH [--bin ...] --dir DIR|--list FILE [...]
                               [--opt=--images=none ...] [--rounds 2] [--cpu 3] [--timeout 300]
      One `rayopdf --serve --json OPTIONS` process a binary and round over every PDF under the
      folders (subfolders too) and of the lists: the requests `{"id":N,"input":DOC}` one at a time
      in their order, the answers carrying the output. Per request the wall time from writing the
      request to reading the whole answer, and the resident memory after it (VmRSS of
      /proc/PID/status); per session the peak (VmHWM), the final VmRSS and the minor faults. The
      rounds alternate the binaries; a document keeps its median time per binary. Prints the sum,
      median, p95 and mean ms a request, requests/s, the change of the sum against the first
      binary, peak and final RSS and faults a request. The first round's answers are then checked
      against `BIN --convert DOC OUT --report OPTIONS`: exit code, the report record (without
      "id", "output" and "ms") and the output (Markdown and HTML as bytes, JSON and chunks as
      parsed values); prints "answers identical: N of M" a binary and its first differences, and
      exits with 1 when anything differs.

  python3 tools/bench.py wasm --module NAME=DIR [--module ...] --dir DIR [--dir ...]
                              [--opt=--images=none ...] [--runs 3] [--rounds 2] [--cpu 3]
                              [--native BIN] [--jobs N] [--cpus LIST]
      The WebAssembly module (DIR holds rayopdf-core.mjs and rayopdf-core.wasm of
      `tools/dev.py build --config wasm`) in process, as the npm package calls it:
      tools/bench/wasm_bench.mjs converts every PDF under the folders (subfolders too) with
      `--format=markdown --ocr=off OPTIONS` in one Node.js process, one untimed conversion and then
      RUNS timed ones a document, keeping the fastest; the rounds alternate the modules and a
      document keeps its best time per module. Prints what `warm` prints. --native also runs
      `BIN --convert DOC OUT.md --ocr=off OPTIONS` per document in the same container and compares
      the exit code with the module's status and, when both are 0, the output's bytes with the
      text of the module's untimed conversion; prints "identical: N of M" a module and its first
      five differences, and exits with 1 when anything differs. --runs 0 takes no time (one
      round); only then may --jobs N split the documents over N processes on the CPUs of --cpus.

Every command takes --json FILE (the whole result, with date, machine and parameters). The
first line printed says what was measured, where and when; quote it with the numbers.

  docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker    # once, after `tools/dev.py image`
"""

from __future__ import annotations

import argparse
import concurrent.futures
import filecmp
import hashlib
import json
import os
import platform
import re
import select
import shlex
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PERF_IMAGE = "rayopdf-perf:1"
WASM_IMAGE = "rayopdf-wasm:1"  # Emscripten's Node.js, for `wasm` (docker/wasm.Dockerfile)
SOURCES = "/opt/bench"  # tools/ in the measuring container
SCRATCH = "/out"        # its RAM disk
NO_OCR = "RAYOPDF_OCR=/nonexistent/rayopdf-ocr"

# pymupdf has no command line that writes Markdown: three lines as the harness converts with it
# (pymupdf4llm installed, its layout analysis on).
PYMUPDF = ("import sys, pymupdf\n"
           "with open('/tmp/o.md', 'w', encoding='utf-8') as out:\n"
           "    out.write(pymupdf.open(sys.argv[1]).to_markdown())\n")
# `tools`: image, timed runs, warm-up runs and the command; {in} is the document, {bin} RayoPDF's
# binary, {threads} the number of CPUs of the run.
TOOLS = {
    "rayopdf": (PERF_IMAGE, 40, 3, ["{bin}", "--convert", "{in}", "/tmp/o.md", "--images=none"]),
    "pdftotext": ("rayopdf-eval-pdftotext:1", 20, 2, ["pdftotext", "-enc", "UTF-8", "{in}", "/tmp/o.txt"]),
    "liteparse": ("rayopdf-eval-liteparse:1", 20, 2,
                  ["lit", "parse", "{in}", "--format", "markdown", "--no-ocr", "--image-mode", "off", "-q",
                   "-o", "/tmp/o.md"]),
    "edgeparse": ("rayopdf-eval-edgeparse:1", 20, 2, ["edgeparse", "{in}", "-o", "/tmp/out", "-f", "markdown"]),
    "opendataloader": ("rayopdf-eval-opendataloader:1", 10, 1,
                       ["opendataloader-pdf", "{in}", "-o", "/tmp/out", "-f", "markdown", "--image-output", "off",
                        "-q"]),
    "markitdown": ("rayopdf-eval-markitdown:1", 10, 1, ["markitdown", "{in}", "-o", "/tmp/o.md"]),
    "pymupdf": ("rayopdf-eval-pymupdf:1", 5, 1, ["python3", "-c", PYMUPDF, "{in}"]),
    "docling": ("rayopdf-eval-docling:1", 3, 1,
                ["docling", "convert", "{in}", "--to", "md", "--no-ocr", "--artifacts-path", "/models", "--output",
                 "/tmp/out", "--device", "cpu", "--image-export-mode", "placeholder", "--num-threads", "{threads}"]),
    # kordoc's command line reads without OCR unless asked (--ocr); KORDOC_OFFLINE (below) keeps it
    # from fetching anything, as the harness runs it.
    "kordoc": ("rayopdf-eval-kordoc:1", 10, 1, ["/opt/kordoc/node_modules/.bin/kordoc", "{in}", "-o", "/tmp/o.md"]),
}
REPORT_MS = re.compile(rb'"ms":[0-9.]+')
REPORT_INPUT = re.compile(rb'"input":"(?:[^"\\]|\\.)*",?')


def log(text: str) -> None:
    print(text, file=sys.stderr, flush=True)


def scope() -> dict:
    """When and where: every number is quoted with it (AGENTS.md rule 6)."""
    machine = platform.machine()
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as cpuinfo:
            machine = next(line.split(":", 1)[1].strip() for line in cpuinfo if line.startswith("model name"))
    except (OSError, StopIteration):
        pass
    return {"date": time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime()), "machine": machine,
            "kernel": platform.release()}


def table(header: list[str], rows: list[list[str]]) -> str:
    """A Markdown table with padded columns."""
    widths = [max(len(row[i]) for row in [header] + rows) for i in range(len(header))]
    lines = ["| " + " | ".join(cell.ljust(width) for cell, width in zip(row, widths)) + " |"
             for row in [header] + rows]
    lines.insert(1, "|" + "|".join("-" * (width + 2) for width in widths) + "|")
    return "\n".join(lines)


def percent(new: float, base: float) -> str:
    return f"{(new / base - 1) * 100:+.1f} %" if base else ""


def rotated(items: list, by: int) -> list:
    by %= len(items)
    return items[by:] + items[:by]


# ---- host side: arguments, containers ----------------------------------------------------------

def named_files(values: list[str], what: str) -> list[tuple[str, str]]:
    """NAME=PATH arguments as (name, resolved path)."""
    out = []
    for value in values:
        name, sep, path = value.partition("=")
        if not sep or not name:
            raise SystemExit(f"bench: {what} takes NAME=PATH, got {value!r}")
        out.append((name, existing_file(path)))
    if len({name for name, _ in out}) != len(out):
        raise SystemExit(f"bench: two {what} arguments have the same name")
    return out


def existing_file(path: str) -> str:
    resolved = Path(path).resolve()
    if not resolved.is_file():
        raise SystemExit(f"bench: {path} is not a file")
    return str(resolved)


def documents(directories: list[str], lists: list[str] = (), recursive: bool = False) -> list[tuple[str, str]]:
    """(label, resolved path) of the PDFs of the folders and list files; the label is folder/name."""
    found: list[Path] = []
    for directory in directories:
        if not Path(directory).is_dir():
            raise SystemExit(f"bench: {directory} is not a folder")
        found += sorted(Path(directory).rglob("*.pdf") if recursive else Path(directory).glob("*.pdf"))
    for name in lists:
        found += [Path(line.strip()) for line in Path(name).read_text(encoding="utf-8").splitlines() if line.strip()]
    if not found:
        raise SystemExit("bench: no PDF to measure")
    return [(f"{path.absolute().parent.name}/{path.name}", existing_file(str(path))) for path in found]


def read_only_mounts(paths: list[str]) -> list[str]:
    """docker -v arguments that show the files' folders in the container under their own paths."""
    kept: list[str] = []
    for folder in sorted({str(Path(path).parent) for path in paths}):
        if not any(folder == k or folder.startswith(k.rstrip("/") + "/") for k in kept):
            kept.append(folder)
    return [arg for folder in kept for arg in ("-v", f"{folder}:{folder}:ro")]


def require_images(images: list[str]) -> None:
    missing = [image for image in images
               if subprocess.run(["docker", "image", "inspect", image], capture_output=True).returncode != 0]
    if missing:
        hint = ("`python3 tools/dev.py image`, then `docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker`"
                if PERF_IMAGE in missing else "`python3 tools/dev.py image --name wasm`"
                if WASM_IMAGE in missing else "`python3 tools/dev.py image --name cross`"
                if "rayopdf-cross:1" in missing else "`python3 tools/eval.py images`")
        raise SystemExit(f"bench: no image {', '.join(missing)}; build with {hint}")


def docker_run(image: str, cpus: str | None, options: list[str], command: list[str],
               stdin: str | None = None) -> subprocess.CompletedProcess:
    """`command` in a container of `image`: no network, the calling user, pinned to `cpus`. Its
    standard output is returned; its standard error goes to ours."""
    cmd = ["docker", "run", "--rm", "--init", "--network", "none", "-u", f"{os.getuid()}:{os.getgid()}",
           "-e", "HOME=/tmp", "-e", NO_OCR]
    if cpus:
        cmd += ["--cpuset-cpus", cpus]
    if stdin is not None:
        cmd.append("-i")
    return subprocess.run(cmd + options + [image] + command, input=stdin, stdout=subprocess.PIPE, text=True)


def measure_inside(job: dict, cpus: str | None, files: list[str], image: str = PERF_IMAGE) -> dict:
    """Run `job` by this script's inside half in `image` (default the measuring image); its result."""
    require_images([image])
    options = ["-e", "PYTHONDONTWRITEBYTECODE=1", "--tmpfs", f"{SCRATCH}:exec",
               "-v", f"{REPO / 'tools'}:{SOURCES}:ro"] + read_only_mounts(files)
    proc = docker_run(image, cpus, options, ["python3", f"{SOURCES}/bench.py", "inside"], stdin=json.dumps(job))
    if proc.returncode != 0 or not proc.stdout.strip():
        raise SystemExit(f"bench: the measuring container failed (exit code {proc.returncode})")
    return json.loads(proc.stdout.splitlines()[-1])


def finish(args, result: dict, text: str) -> None:
    print(text)
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")


def headline(what: str, where: dict) -> str:
    return f"{what}; {where['machine']}, Linux {where['kernel']}, {where['date']}"


# ---- warm ----------------------------------------------------------------------------------------

def inside_warm(job: dict) -> dict:
    tmp = f"{SCRATCH}/warm"
    os.makedirs(tmp)
    bins, docs = job["bins"], job["docs"]
    best: dict[str, dict[str, float]] = {name: {} for name, _ in bins}
    failed: dict[str, set] = {name: set() for name, _ in bins}
    pages: dict[str, int] = {}
    for r in range(job["rounds"]):
        for label, doc in docs:
            for name, path in rotated(bins, r):
                res = subprocess.run([path, "--bench", doc, tmp, str(job["runs"])] + job["opts"],
                                     capture_output=True, text=True)
                if res.returncode != 0:
                    failed[name].add(label)
                    continue
                fields = dict(pair.split("=", 1) for pair in res.stdout.split())
                pages[label] = int(fields["pages"])
                best[name][label] = min(float(fields["best_ms"]), best[name].get(label, float("inf")))
        log(f"warm: round {r + 1} of {job['rounds']}")
    return {"rows": {name: speed_row([(best[name][label], pages[label]) for label, _ in docs if label in best[name]],
                                     len(failed[name]), [label for label, _ in docs if label in best[name]])
                     for name, _ in bins}}


def speed_row(times: list[tuple[float, int]], failed: int, labels: list[str]) -> dict:
    """`warm` and `wasm`'s figures of the documents' best times (ms, pages), labelled `labels`."""
    row = {"documents": len(times), "failed": failed}
    if times:
        per_page = sorted(ms / max(1, pages) for ms, pages in times)
        total_ms = sum(ms for ms, _ in times)
        total_pages = sum(pages for _, pages in times)
        row.update({"pages": total_pages, "ms_page_median": statistics.median(per_page),
                    "ms_page_p95": per_page[len(per_page) * 95 // 100], "ms_page_mean": total_ms / total_pages,
                    "pages_per_s": total_pages / total_ms * 1000, "total_ms": total_ms,
                    "document_ms": {label: ms for label, (ms, _) in zip(labels, times)}})
    return row


def speed_table(what: str, rows: dict[str, dict]) -> str:
    """The rows of speed_row as a table, the first row the base of the pages/s change."""
    lines, first = [], next(iter(rows.values()))
    for name, row in rows.items():
        if not row["documents"]:
            lines.append([name, "0", str(row["failed"]), "", "", "", "", "", ""])
            continue
        lines.append([name, str(row["documents"]), str(row["failed"]), str(row["pages"]),
                      f"{row['ms_page_median']:.3f}", f"{row['ms_page_p95']:.3f}", f"{row['ms_page_mean']:.3f}",
                      f"{row['pages_per_s']:,.0f}",
                      percent(row["pages_per_s"], first.get("pages_per_s", 0)) if row is not first else ""])
    return table([what, "documents", "failed", "pages", "ms/page median", "p95", "mean", "pages/s",
                  f"pages/s against {next(iter(rows))}"], lines)


def cmd_warm(args) -> int:
    bins, docs = named_files(args.bin, "--bin"), documents(args.dir)
    job = {"task": "warm", "bins": bins, "docs": docs, "opts": args.opt, "runs": args.runs, "rounds": args.rounds}
    result = measure_inside(job, str(args.cpu), [path for _, path in bins + docs])
    result.update({"command": "warm", "scope": scope(), "cpu": args.cpu, "image": PERF_IMAGE,
                   "folders": args.dir, "options": args.opt, "runs": args.runs, "rounds": args.rounds})
    text = headline(f"warm: {len(docs)} documents, options [{' '.join(args.opt)}], --bench best of {args.runs}, "
                    f"{args.rounds} alternating rounds, CPU {args.cpu}, {PERF_IMAGE}", result["scope"])
    text += "\n\n" + speed_table("binary", {name: result["rows"][name] for name, _ in bins})
    finish(args, result, text)
    return 0


# ---- fresh ---------------------------------------------------------------------------------------

def build_timer(name: str) -> str:
    """tools/bench/<name>.c compiled onto the container's RAM disk."""
    out = f"{SCRATCH}/{name}"
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-o", out, f"{SOURCES}/bench/{name}.c"], check=True)
    return out


def inside_fresh(job: dict) -> dict:
    timer = build_timer("fresh_timer")
    rounds = []
    for r in range(job["rounds"]):
        for name, path in rotated(job["bins"], r):
            res = subprocess.run([timer] + (["--per-file"] if job["per_doc"] else []) +
                                 [str(job["runs"]), str(job["warmup"]), f"{SCRATCH}/o.md", path] + job["opts"] +
                                 ["--"] + [doc for _, doc in job["docs"]], stdout=subprocess.PIPE, text=True)
            if res.returncode != 0:
                raise SystemExit(f"fresh_timer failed for {name}")
            record = json.loads(res.stdout)
            rounds.append({"round": r, "binary": name, **record})
            log(f"fresh: round {r + 1} of {job['rounds']}, {name}: {record['wall_ms']['median']:.4f} ms")
    return {"rounds": rounds}


def cmd_fresh(args) -> int:
    bins, docs = named_files(args.bin, "--bin"), documents(args.dir)
    job = {"task": "fresh", "bins": bins, "docs": docs, "opts": args.opt, "runs": args.runs, "warmup": args.warmup,
           "rounds": args.rounds, "per_doc": args.per_doc}
    result = measure_inside(job, str(args.cpu), [path for _, path in bins + docs])
    result.update({"command": "fresh", "scope": scope(), "cpu": args.cpu, "image": PERF_IMAGE, "folders": args.dir,
                   "options": args.opt, "runs": args.runs, "warmup": args.warmup, "rows": {}})

    def over_rounds(name: str, key: str, figure: str) -> list[float]:
        return [r[key][figure] for r in result["rounds"] if r["binary"] == name]

    base = over_rounds(bins[0][0], "wall_ms", "median")
    rows = []
    for name, _ in bins:
        wall = over_rounds(name, "wall_ms", "median")
        row = {"ms_median": statistics.median(wall), "ms_median_least": min(wall), "ms_median_most": max(wall),
               "paired_change": statistics.median(w / b - 1 for w, b in zip(wall, base)),
               "failures": sum(r["failures"] for r in result["rounds"] if r["binary"] == name)}
        for key, short in (("wall_ms", "ms"), ("cpu_ms", "cpu_ms"), ("minflt", "faults")):
            for figure in ("median", "p95", "mean"):
                row[f"{short}_{figure}"] = statistics.median(over_rounds(name, key, figure))
        if args.per_doc:
            per_round = [r["per_file"] for r in result["rounds"] if r["binary"] == name]
            row["docs"] = {doc: {short: statistics.median(files[i][key] for files in per_round)
                                 for key, short in (("wall_ms", "ms"), ("cpu_ms", "cpu_ms"), ("minflt", "faults"))}
                           for i, (_, doc) in enumerate(docs)}
        result["rows"][name] = row
        rows.append([name, f"{row['ms_median']:.3f} ({row['ms_median_least']:.3f}-{row['ms_median_most']:.3f})",
                     f"{row['paired_change'] * 100:+.1f} %" if name != bins[0][0] else "",
                     f"{row['ms_p95']:.2f}", f"{row['ms_mean']:.2f}", f"{row['cpu_ms_median']:.3f}",
                     f"{row['faults_median']:.0f}, {row['faults_p95']:.0f}, {row['faults_mean']:.0f}",
                     str(row["failures"])])
    text = headline(f"fresh: {len(docs)} documents, options [{' '.join(args.opt)}], {args.runs} runs a document "
                    f"after {args.warmup}, {args.rounds} rounds in rotated order, CPU {args.cpu}, {PERF_IMAGE}",
                    result["scope"])
    text += "\n\n" + table(["binary", "ms a document, median (range over the rounds)", f"paired against {bins[0][0]}",
                            "p95", "mean", "CPU ms median", "page faults median, p95, mean", "failed runs"], rows)
    if args.per_doc:
        first = result["rows"][bins[0][0]]["docs"]
        slowest = sorted(docs, key=lambda d: -first[d[1]]["ms"])[:10]
        header = ["document"]
        for name, _ in bins:
            header += [f"{name} ms", f"{name} faults"]
        lines = [[label] + [cell for name, _ in bins
                            for cell in (f"{result['rows'][name]['docs'][doc]['ms']:.3f}",
                                         f"{result['rows'][name]['docs'][doc]['faults']:.0f}")]
                 for label, doc in slowest]
        text += f"\n\nThe ten documents slowest for {bins[0][0]} (median over the rounds):\n\n" + table(header, lines)
    finish(args, result, text)
    return 0


# ---- identical -----------------------------------------------------------------------------------

def tree(folder: str) -> list[str]:
    return sorted(str(path.relative_to(folder)) for path in Path(folder).rglob("*") if path.is_file())


def inside_identical(job: dict) -> dict:
    def convert(binary: str, doc: str, opts: list[str], folder: str, emulator: list[str]) -> tuple[object, bytes]:
        os.makedirs(folder)
        try:
            res = subprocess.run(emulator + [binary, "--convert", doc, "out.md", "--report"] + opts, cwd=folder,
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=job["timeout"])
        except subprocess.TimeoutExpired:
            return "timeout", b""
        return res.returncode, REPORT_MS.sub(b"", res.stdout)

    def compare(index: int, doc: str, opts: list[str]) -> list[str]:
        folder = f"{SCRATCH}/identical/{index}"
        a, b = (convert(job[side], doc, opts + job[f"{side}_opts"], f"{folder}/{side}",
                        job.get(f"{side}_emulator", []))
                for side in ("a", "b"))
        what = []
        if "timeout" in (a[0], b[0]):
            what.append("timeout")
        if a[0] != b[0]:
            what.append(f"exit code {a[0]} against {b[0]}")
        if a[1] != b[1]:
            what.append("--report record")
        files_a, files_b = tree(f"{folder}/a"), tree(f"{folder}/b")
        what += [f"{name} only from A" for name in files_a if name not in files_b]
        what += [f"{name} only from B" for name in files_b if name not in files_a]
        what += [name for name in files_a if name in files_b and
                 not filecmp.cmp(f"{folder}/a/{name}", f"{folder}/b/{name}", shallow=False)]
        shutil.rmtree(folder)
        return what

    work = [(label, doc, name, opts) for name, opts in job["sets"] for label, doc in job["docs"]]
    jobs = job["jobs"] or len(os.sched_getaffinity(0))
    differences = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futures = [pool.submit(compare, i, doc, opts) for i, (_, doc, _, opts) in enumerate(work)]
        for i, ((label, _, name, _), future) in enumerate(zip(work, futures), 1):
            what = future.result()
            if what:
                differences.append({"set": name, "document": label, "differs": what})
            if i % 500 == 0:
                log(f"identical: {i} of {len(work)} compared, {len(differences)} differ")
    return {"comparisons": len(work), "differ": len(differences), "differences": differences}


def option_sets(values: list[str] | None) -> list[tuple[str, list[str]]]:
    """--set NAME=OPTIONS arguments as (name, options); default none=--images=none and images=."""
    sets = []
    for value in values or ["none=--images=none", "images="]:
        name, sep, options = value.partition("=")
        if not sep or not name:
            raise SystemExit(f"bench: --set takes NAME=OPTIONS, got {value!r}")
        sets.append((name, shlex.split(options)))
    return sets


def cmd_identical(args) -> int:
    a, b = existing_file(args.a), existing_file(args.b)
    docs = documents(args.input, recursive=True)
    sets = option_sets(args.set)
    job = {"task": "identical", "a": a, "b": b, "a_opts": args.a_opt, "b_opts": args.b_opt, "docs": docs,
           "sets": sets, "jobs": args.jobs, "timeout": args.timeout,
           "b_emulator": [args.b_emulator] if args.b_emulator else []}
    result = measure_inside(job, args.cpus, [a, b] + [path for _, path in docs], args.image)
    result.update({"command": "identical", "scope": scope(), "a": a, "b": b, "a_opts": args.a_opt,
                   "b_opts": args.b_opt, "inputs": args.input,
                   "sets": {name: options for name, options in sets}, "documents": len(docs)})
    sides = "".join(f", {side} adds {' '.join(opts)}" for side, opts in (("A", args.a_opt), ("B", args.b_opt))
                    if opts)
    text = (f"identical: {result['comparisons']} comparisons ({len(docs)} documents, option sets "
            f"{', '.join(name for name, _ in sets)}{sides}"
            f"{f'; B through {args.b_emulator} in {args.image}' if args.b_emulator else ''}) of exit code, --report "
            f"record and output files: {result['differ']} differ")
    for difference in result["differences"][:args.show]:
        text += f"\n  {difference['set']} {difference['document']}: {'; '.join(difference['differs'][:6])}"
    if result["differ"] > args.show:
        text += f"\n  ... and {result['differ'] - args.show} more (--json FILE has all)"
    finish(args, result, text)
    return 1 if result["differ"] else 0


# ---- hashes --------------------------------------------------------------------------------------

def output_hashes(binary: str, doc: str, opts: list[str], folder: Path, timeout: int) -> dict:
    """Exit code, SHA-256 of the --report record without "ms" and "input" (the document's path,
    which differs between machines) and of every file one conversion writes."""
    folder.mkdir(parents=True)
    env = dict(os.environ, RAYOPDF_OCR=NO_OCR.split("=", 1)[1])
    try:
        res = subprocess.run([binary, "--convert", doc, "out.md", "--report"] + opts, cwd=folder, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
        code, report = res.returncode, REPORT_INPUT.sub(b"", REPORT_MS.sub(b"", res.stdout))
    except subprocess.TimeoutExpired:
        code, report = "timeout", b""
    files = {name: hashlib.sha256((folder / name).read_bytes()).hexdigest() for name in tree(str(folder))}
    shutil.rmtree(folder)
    return {"exit": code, "report": hashlib.sha256(report).hexdigest(), "files": files}


def compare_hashes(paths: list[str], show: int) -> int:
    """The documents whose outputs differ between the hashes files of two platforms."""
    a, b = (json.loads(Path(path).read_text(encoding="utf-8")) for path in paths)
    lines, differ, compared = [], 0, 0
    for name in a["hashes"]:
        left, right = a["hashes"][name], b["hashes"].get(name, {})
        for label in sorted(set(left) | set(right)):
            compared += 1
            if left.get(label) != right.get(label):
                differ += 1
                what = ("only in one" if label not in left or label not in right else
                        "; ".join(k for k in ("exit", "report") if left[label][k] != right[label][k]) or
                        ", ".join(sorted(f for f in set(left[label]["files"]) | set(right[label]["files"])
                                         if left[label]["files"].get(f) != right[label]["files"].get(f))))
                if differ <= show:
                    lines.append(f"  {name} {label}: {what}")
    print(f"hashes: {paths[0]} ({a['platform']}) against {paths[1]} ({b['platform']}): {compared} outputs "
          f"compared, {differ} differ" + "".join("\n" + line for line in lines) +
          (f"\n  ... and {differ - show} more" if differ > show else ""))
    return 1 if differ else 0


def cmd_hashes(args) -> int:
    if args.compare:
        return compare_hashes(args.compare, args.show)
    if not args.bin or not args.dir or not args.json:
        raise SystemExit("bench: hashes needs --bin, --dir and --json (or --compare A B)")
    binary = existing_file(args.bin)
    docs = documents(args.dir, recursive=True)
    sets = option_sets(args.set)
    work = [(name, opts, label, doc) for name, opts in sets for label, doc in docs]
    with tempfile.TemporaryDirectory() as scratch, \
            concurrent.futures.ThreadPoolExecutor(args.jobs or os.cpu_count() or 1) as pool:
        futures = [pool.submit(output_hashes, binary, doc, opts, Path(scratch) / str(i), args.timeout)
                   for i, (_, opts, _, doc) in enumerate(work)]
        hashes: dict[str, dict] = {name: {} for name, _ in sets}
        for (name, _, label, _), future in zip(work, futures):
            hashes[name][label] = future.result()
    failed = sum(1 for entries in hashes.values() for entry in entries.values() if entry["exit"] == "timeout")
    result = {"command": "hashes", "scope": scope(), "platform": f"{platform.system()} {platform.machine()}",
              "bin": binary, "inputs": args.dir, "sets": {name: opts for name, opts in sets},
              "documents": len(docs), "timeouts": failed, "hashes": hashes}
    finish(args, result, f"hashes: {len(work)} outputs of {binary} ({result['platform']}; {len(docs)} documents, "
                         f"option sets {', '.join(name for name, _ in sets)}), {failed} timed out")
    return 1 if failed else 0


# ---- instr ---------------------------------------------------------------------------------------

def inside_instr(job: dict) -> dict:
    tmp = f"{SCRATCH}/instr"
    os.makedirs(tmp)
    rows = []
    for label, doc in job["docs"]:
        row = {"document": label}
        for name, path in job["bins"]:
            counts = []
            for n in (job["n"], 2 * job["n"]):
                res = subprocess.run(["valgrind", "--tool=callgrind", "--callgrind-out-file=/dev/null", path,
                                      "--bench", doc, tmp, str(n)] + job["opts"], capture_output=True, text=True)
                found = re.search(r"Collected : (\d+)", res.stderr)
                if res.returncode != 0 or not found:
                    raise SystemExit(f"callgrind failed for {name} on {label}:\n{res.stderr[-2000:]}")
                counts.append(int(found.group(1)))
            row[name] = (counts[1] - counts[0]) / job["n"]
        rows.append(row)
        log(f"instr: {label}")
    return {"rows": rows}


def cmd_instr(args) -> int:
    bins = named_files(args.bin, "--bin")
    docs = [(f"{Path(doc).absolute().parent.name}/{Path(doc).name}", existing_file(doc)) for doc in args.doc]
    job = {"task": "instr", "bins": bins, "docs": docs, "opts": args.opt, "n": args.n}
    result = measure_inside(job, args.cpus, [path for _, path in bins + docs])
    result.update({"command": "instr", "scope": scope(), "image": PERF_IMAGE, "options": args.opt, "n": args.n})
    rows = []
    for row in result["rows"]:
        cells = [row["document"]]
        for name, _ in bins:
            cells.append(f"{row[name]:,.0f}")
            if name != bins[0][0]:
                cells.append(f"{(row[name] / row[bins[0][0]] - 1) * 100:+.2f} %")
        rows.append(cells)
    header = ["document"]
    for name, _ in bins:
        header += [name] if name == bins[0][0] else [name, "change"]
    text = headline(f"instr: instructions a conversion (callgrind, --bench N={args.n} and {2 * args.n}), options "
                    f"[{' '.join(args.opt)}], {PERF_IMAGE}", result["scope"])
    finish(args, result, text + "\n\n" + table(header, rows))
    return 0


# ---- tools ---------------------------------------------------------------------------------------

def cmd_tools(args) -> int:
    binary = existing_file(args.bin)
    docs = named_files(args.doc, "--doc")
    unknown = [name for name in args.only or [] if name not in TOOLS]
    if unknown:
        raise SystemExit(f"bench: unknown tool {', '.join(unknown)}; known: {', '.join(TOOLS)}")
    names = [name for name in TOOLS if not args.only or name in args.only]
    require_images(sorted({PERF_IMAGE} | {TOOLS[name][0] for name in names}))
    where = scope()
    result = {"command": "tools", "scope": where, "tools": {name: TOOLS[name][3] for name in names}, "rows": []}
    text = []
    with tempfile.TemporaryDirectory(prefix="rayopdf-bench-") as work:
        # One static timer for every image.
        built = docker_run(PERF_IMAGE, None, ["-v", f"{REPO / 'tools'}:{SOURCES}:ro", "-v", f"{work}:/work"],
                           ["cc", "-O2", "-Wall", "-Wextra", "-static", "-o", "/work/cmdbench",
                            f"{SOURCES}/bench/cmdbench.c"])
        if built.returncode != 0:
            raise SystemExit("bench: cmdbench did not build")
        for cpus in args.cpus or ["3"]:
            threads = len(cpu_list(cpus))
            for name in names:
                image, runs, warmup, command = TOOLS[name]
                for doc, path in docs:
                    argv = [part.replace("{in}", "/in/doc.pdf").replace("{bin}", "/opt/rayopdf/" + Path(binary).name)
                            .replace("{threads}", str(threads)) for part in command]
                    options = ["--tmpfs", "/tmp:exec,size=2g", "--entrypoint", "/opt/cmdbench",
                               "-v", f"{work}/cmdbench:/opt/cmdbench:ro", "-v", f"{path}:/in/doc.pdf:ro",
                               "-v", f"{Path(binary).parent}:/opt/rayopdf:ro", "-e", "HF_HUB_OFFLINE=1",
                               "-e", "KORDOC_OFFLINE=1"]
                    for variable in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                                     "RAYON_NUM_THREADS"):
                        options += ["-e", f"{variable}={threads}"]
                    proc = docker_run(image, cpus, options, [str(runs), str(warmup), "--"] + argv)
                    row = {"tool": name, "document": doc, "cpus": cpus, "threads": threads, "warmup": warmup}
                    try:
                        row.update(json.loads(proc.stdout.splitlines()[-1]))
                    except (IndexError, ValueError):
                        row["error"] = f"exit code {proc.returncode}"
                    result["rows"].append(row)
                    log(f"tools: {cpus} {name} {doc}: {row.get('median_ms', row.get('error'))}")
            text.append(tools_table(result["rows"], names, [doc for doc, _ in docs], cpus, threads, where))
    finish(args, result, "\n\n".join(text))
    return 0


def cpu_list(cpus: str) -> list[int]:
    """The CPUs of a cpuset such as 0-2,4."""
    out: list[int] = []
    for part in cpus.split(","):
        first, _, last = part.partition("-")
        out += range(int(first), int(last or first) + 1)
    return out


def tools_table(rows: list[dict], names: list[str], docs: list[str], cpus: str, threads: int, where: dict) -> str:
    by = {(row["tool"], row["document"]): row for row in rows if row["cpus"] == cpus}
    header = ["tool", "runs after warm-up"]
    for doc in docs:
        header += [f"{doc}, ms", "x RayoPDF", "peak RSS, MiB"]
    lines = []
    for name in names:
        cells = [name, f"{TOOLS[name][1]} after {TOOLS[name][2]}"]
        for doc in docs:
            row, ours = by[(name, doc)], by.get(("rayopdf", doc), {})
            if "median_ms" not in row or row["failures"]:
                cells += [row.get("error", f"{row.get('failures')} runs failed"), "", ""]
                continue
            factor = row["median_ms"] / ours["median_ms"] if ours.get("median_ms") else None
            cells += [f"{row['median_ms']:,.2f}" if row["median_ms"] < 100 else f"{row['median_ms']:,.0f}",
                      "" if factor is None else f"{factor:,.1f}" if factor < 10 else f"{factor:,.0f}",
                      f"{max(row['max_rss_kib'], row['max_rss_desc_kib']) / 1024:,.0f}"]
        lines.append(cells)
    head = headline(f"tools: each tool's own command line as a fresh process, start to exit, CPUs {cpus} "
                    f"({threads} thread{'s' if threads > 1 else ''}), no network, output to a RAM disk", where)
    return head + "\n\n" + table(header, lines)


# ---- pages ---------------------------------------------------------------------------------------

def inside_pages(job: dict) -> dict:
    timer = build_timer("cmdbench")
    rows = []
    for label, doc in job["docs"]:
        for count in job["workers"]:
            command = [job["bin"], "--convert", doc, f"{SCRATCH}/o.md", "--images=none"]
            if count:
                command.append(f"--workers={count}")
            res = subprocess.run([timer, str(job["runs"]), str(job["warmup"]), "--"] + command,
                                 stdout=subprocess.PIPE, text=True)
            if res.returncode != 0:
                raise SystemExit(f"cmdbench failed on {label}")
            rows.append({"document": label, "workers": count, **json.loads(res.stdout)})
        log(f"pages: {label}")
    return {"rows": rows}


def cmd_pages(args) -> int:
    binary = existing_file(args.bin)
    docs = documents(args.dir or [], args.list or [])
    counts = [int(n) for n in args.workers.split()]
    job = {"task": "pages", "bin": binary, "docs": docs, "workers": counts, "runs": args.runs, "warmup": args.warmup}
    result = measure_inside(job, args.cpus, [binary] + [path for _, path in docs])
    result.update({"command": "pages", "scope": scope(), "cpus": args.cpus, "image": PERF_IMAGE, "runs": args.runs,
                   "warmup": args.warmup, "sums": {}})
    by: dict[str, dict[int, float]] = {}
    failures = 0
    for row in result["rows"]:
        by.setdefault(row["document"], {})[row["workers"]] = row["median_ms"]
        failures += row["failures"]
    rows = []
    for count in counts:
        total = sum(times[count] for times in by.values())
        result["sums"][str(count)] = total
        cells = ["default" if count == 0 else str(count), f"{total:.1f}", "", "", "", ""]
        if 1 in counts and count != 1:
            ratios = [times[count] / times[1] for times in by.values()]
            cells[2:] = [f"{total / sum(times[1] for times in by.values()):.3f}",
                         f"{statistics.median(ratios):.3f}", f"{min(ratios):.3f}", f"{max(ratios):.3f}"]
        rows.append(cells)
    text = headline(f"pages: {len(docs)} documents, fresh --convert --images=none, median of {args.runs} runs a "
                    f"document after {args.warmup}, CPUs {args.cpus}, {PERF_IMAGE}", result["scope"])
    text += "\n\n" + table(["--workers", "sum, ms", "sum against 1", "median ratio", "best", "worst"], rows)
    if failures:
        text += f"\n{failures} runs did not exit with 0"
    finish(args, result, text)
    return 0


# ---- serve ---------------------------------------------------------------------------------------

ANSWER_KEYS = ("markdown", "json", "chunks", "html")


def canonical(kind: str, value) -> str:
    """An answer as a digest: text by its bytes, JSON and chunks by their parsed values."""
    text = value.encode("utf-8", "surrogateescape") if kind in ("markdown", "html") else \
        json.dumps(value, sort_keys=True, ensure_ascii=False).encode("utf-8", "surrogateescape")
    return hashlib.sha256(text).hexdigest()


def record_fields(record: dict) -> dict:
    """What the check compares of a serve answer or a --report record."""
    return {key: value for key, value in record.items() if key not in ("id", "output", "ms") + ANSWER_KEYS}


def serve_session(path: str, docs: list[str], opts: list[str], timeout: int, answers: list | None) -> dict:
    """One `--serve --json` process over the documents, one request at a time in their order. With
    `answers`, each answer's code, record fields and output digest are appended to it."""
    proc = subprocess.Popen([path, "--serve", "--json"] + opts, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    fd, pending = proc.stdout.fileno(), bytearray()

    def answer(what: str) -> bytes:
        deadline = time.monotonic() + timeout
        while True:
            end = pending.find(b"\n")
            if end >= 0:
                line = bytes(pending[:end])
                del pending[:end + 1]
                return line
            if not select.select([fd], [], [], max(0.0, deadline - time.monotonic()))[0]:
                proc.kill()
                raise SystemExit(f"bench: --serve gave no answer for {what} in {timeout} s")
            chunk = os.read(fd, 1 << 20)
            if not chunk:
                raise SystemExit(f"bench: --serve ended at {what} (exit code {proc.wait()})")
            pending.extend(chunk)

    def status(key: str) -> int:
        with open(f"/proc/{proc.pid}/status", encoding="ascii") as lines:
            return next((int(line.split()[1]) for line in lines if line.startswith(key)), 0)

    answer("the first line")
    ms, rss, failed = [], [], 0
    for i, doc in enumerate(docs):
        request = json.dumps({"id": i, "input": doc}).encode("utf-8") + b"\n"
        start = time.perf_counter_ns()
        proc.stdin.write(request)
        proc.stdin.flush()
        line = answer(doc)
        ms.append((time.perf_counter_ns() - start) / 1e6)
        rss.append(status("VmRSS:"))
        record = json.loads(line.decode("utf-8", "surrogateescape"))
        failed += record.get("code") != 0
        if answers is not None:
            kind = next((key for key in ANSWER_KEYS if key in record), None)
            answers.append({"code": record.get("code"), "fields": record_fields(record), "kind": kind,
                            "digest": canonical(kind, record[kind]) if kind else None})
    with open(f"/proc/{proc.pid}/stat", encoding="ascii") as stat:
        faults = int(stat.read().rsplit(")", 1)[1].split()[7])
    peak = status("VmHWM:")
    proc.stdin.write(b"quit\n")
    proc.stdin.close()
    code = proc.wait(timeout)
    return {"ms": ms, "rss_kib": rss, "peak_kib": peak, "final_kib": rss[-1] if rss else 0, "faults": faults,
            "failed": failed, "exit_code": code}


def inside_serve(job: dict) -> dict:
    docs = [doc for _, doc in job["docs"]]
    sessions: dict[str, list[dict]] = {name: [] for name, _ in job["bins"]}
    answers: dict[str, list[dict]] = {name: [] for name, _ in job["bins"]}
    for r in range(job["rounds"]):
        for name, path in rotated(job["bins"], r):
            sessions[name].append(serve_session(path, docs, job["opts"], job["timeout"],
                                                answers[name] if r == 0 else None))
            log(f"serve: round {r + 1} of {job['rounds']}, {name}: {sum(sessions[name][-1]['ms']):.1f} ms")
    checks = {}
    out = f"{SCRATCH}/serve"
    for name, path in job["bins"]:
        differences = []
        for i, ((label, doc), served) in enumerate(zip(job["docs"], answers[name])):
            shutil.rmtree(out, ignore_errors=True)
            os.makedirs(out)
            try:
                res = subprocess.run([path, "--convert", doc, f"{out}/out", "--report"] + job["opts"],
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=job["timeout"])
            except subprocess.TimeoutExpired:
                differences.append({"document": label, "differs": ["--convert timed out"]})
                continue
            what = []
            if res.returncode != served["code"]:
                what.append(f"exit code {res.returncode} against the answer's {served['code']}")
            report = res.stdout.decode("utf-8", "surrogateescape").strip()
            if not report or record_fields(json.loads(report)) != served["fields"]:
                what.append("record")
            if res.returncode == 0 and served["kind"]:
                data = Path(f"{out}/out").read_bytes()
                if served["kind"] in ("markdown", "html"):
                    value = data.decode("utf-8", "surrogateescape")
                elif served["kind"] == "json":
                    value = json.loads(data.decode("utf-8", "surrogateescape"))
                else:
                    value = [json.loads(line) for line in data.decode("utf-8", "surrogateescape").splitlines()]
                if canonical(served["kind"], value) != served["digest"]:
                    what.append(served["kind"])
            if what:
                differences.append({"document": label, "differs": what})
            if (i + 1) % 500 == 0:
                log(f"serve: {name}, {i + 1} of {len(docs)} answers checked")
        checks[name] = {"identical": len(docs) - len(differences), "differences": differences}
    return {"sessions": sessions, "checks": checks}


def cmd_serve(args) -> int:
    bins, docs = named_files(args.bin, "--bin"), documents(args.dir or [], args.list or [], recursive=True)
    job = {"task": "serve", "bins": bins, "docs": docs, "opts": args.opt, "rounds": args.rounds,
           "timeout": args.timeout}
    result = measure_inside(job, str(args.cpu), [path for _, path in bins + docs])
    result.update({"command": "serve", "scope": scope(), "cpu": args.cpu, "image": PERF_IMAGE,
                   "folders": args.dir, "lists": args.list, "options": args.opt, "rounds": args.rounds,
                   "documents": [label for label, _ in docs], "rows": {}})
    rows, base, differ = [], None, 0
    for name, _ in bins:
        sessions = result["sessions"][name]
        per_doc = [statistics.median(times) for times in zip(*(session["ms"] for session in sessions))]
        ordered = sorted(per_doc)
        total = sum(per_doc)
        row = {"requests": len(per_doc), "failed": max(session["failed"] for session in sessions), "sum_ms": total,
               "median_ms": statistics.median(ordered), "p95_ms": ordered[len(ordered) * 95 // 100],
               "mean_ms": total / len(per_doc), "requests_per_s": len(per_doc) / total * 1000,
               "peak_mib": max(session["peak_kib"] for session in sessions) / 1024,
               "final_mib": statistics.median(session["final_kib"] for session in sessions) / 1024,
               "faults_per_request": statistics.median(session["faults"] for session in sessions) / len(per_doc),
               "document_ms": per_doc}
        result["rows"][name] = row
        base = base or row
        rows.append([name, str(row["requests"]), str(row["failed"]), f"{total:.1f}", f"{row['median_ms']:.3f}",
                     f"{row['p95_ms']:.3f}", f"{row['mean_ms']:.3f}", f"{row['requests_per_s']:,.0f}",
                     percent(total, base["sum_ms"]) if row is not base else "", f"{row['peak_mib']:.1f}",
                     f"{row['final_mib']:.1f}", f"{row['faults_per_request']:.1f}"])
        differ += len(result["checks"][name]["differences"])
    text = headline(f"serve: {len(docs)} requests, --serve --json [{' '.join(args.opt)}], one session a binary "
                    f"and round, {args.rounds} alternating rounds, CPU {args.cpu}, {PERF_IMAGE}", result["scope"])
    text += "\n\n" + table(["binary", "requests", "failed", "sum, ms", "median ms", "p95", "mean", "requests/s",
                            f"sum against {bins[0][0]}", "peak RSS, MiB", "final RSS, MiB", "faults a request"], rows)
    for name, _ in bins:
        check = result["checks"][name]
        text += f"\n{name}: answers identical to --convert's: {check['identical']} of {len(docs)}"
        for difference in check["differences"][:args.show]:
            text += f"\n  {difference['document']}: {'; '.join(difference['differs'])}"
    finish(args, result, text)
    return 1 if differ else 0


# ---- wasm ----------------------------------------------------------------------------------------

def inside_wasm(job: dict) -> dict:
    docs, jobs, native = job["docs"], job["jobs"], job["native"]
    paths = [doc for _, doc in docs]
    out_root = f"{SCRATCH}/wasm"

    def run_module(name: str, folder: str, out: str | None) -> list[dict]:
        """wasm_bench.mjs over every document: `jobs` Node.js processes, process j taking the
        documents j, j + jobs, ... (its outputs under OUT/j)."""
        procs = []
        for j in range(min(jobs, len(paths))):
            command = ["node", f"{SOURCES}/bench/wasm_bench.mjs", "--module", f"{folder}/rayopdf-core.mjs",
                       "--runs", str(job["runs"])] + (["--out", f"{out}/{j}"] if out else [])
            for opt in job["opts"]:
                command += ["--opt", opt]
            procs.append(subprocess.Popen(command + ["--"] + paths[j::jobs], stdout=subprocess.PIPE, text=True))
        records: list[dict] = [{}] * len(paths)
        for j, proc in enumerate(procs):
            stdout, _ = proc.communicate()
            if proc.returncode != 0 or not stdout.strip():
                raise SystemExit(f"wasm_bench.mjs failed for {name} (exit code {proc.returncode})")
            records[j::jobs] = json.loads(stdout.splitlines()[-1])["docs"]
        return records

    best: dict[str, list] = {name: [None] * len(docs) for name, _ in job["modules"]}
    status: dict[str, list] = {}
    pages = [0] * len(docs)
    for r in range(job["rounds"]):
        for name, folder in rotated(job["modules"], r):
            records = run_module(name, folder, f"{out_root}/{name}" if r == 0 and native else None)
            if r == 0:
                status[name] = [record["status"] for record in records]
            for i, record in enumerate(records):
                if record["status"] == 0:
                    pages[i] = record["pages"]
                if record["best_ms"] is not None:
                    best[name][i] = min(record["best_ms"], best[name][i] or float("inf"))
            log(f"wasm: round {r + 1} of {job['rounds']}, {name}")
    result: dict = {"rows": {}, "identity": {}}
    if job["runs"]:
        for name, _ in job["modules"]:
            done = [i for i in range(len(docs)) if best[name][i] is not None]
            result["rows"][name] = speed_row([(best[name][i], pages[i]) for i in done],
                                             sum(1 for code in status[name] if code != 0),
                                             [docs[i][0] for i in done])
    if native:
        os.makedirs(f"{out_root}/native")

        def convert(i: int) -> object:
            try:
                return subprocess.run([native, "--convert", paths[i], f"{out_root}/native/{i:05d}.md", "--ocr=off"] +
                                      job["opts"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                      timeout=job["timeout"]).returncode
            except subprocess.TimeoutExpired:
                return "timeout"

        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            codes = list(pool.map(convert, range(len(docs))))
        for name, _ in job["modules"]:
            differences = []
            for i, code in enumerate(codes):
                got = status[name][i]
                if got != code:
                    differences.append({"document": docs[i][0], "differs": f"status {got} against exit code {code}"})
                elif code == 0 and not filecmp.cmp(f"{out_root}/{name}/{i % jobs}/{i // jobs:05d}.md",
                                                   f"{out_root}/native/{i:05d}.md", shallow=False):
                    differences.append({"document": docs[i][0], "differs": "output"})
            result["identity"][name] = {"compared": len(docs), "identical": len(docs) - len(differences),
                                        "differences": differences}
    return result


def cmd_wasm(args) -> int:
    modules = [(name, str(Path(core).parent))
               for name, core in named_files([f"{value}/rayopdf-core.mjs" for value in args.module], "--module")]
    for _, folder in modules:
        existing_file(f"{folder}/rayopdf-core.wasm")
    native = existing_file(args.native) if args.native else None
    if args.jobs > 1 and args.runs:
        raise SystemExit("bench: --jobs takes --runs 0: times are taken in one process on --cpu")
    rounds = args.rounds if args.runs else 1
    docs = documents(args.dir, recursive=True)
    cpus = str(args.cpu) if args.jobs == 1 else args.cpus
    job = {"task": "wasm", "modules": modules, "docs": docs, "opts": args.opt, "runs": args.runs, "rounds": rounds,
           "jobs": args.jobs, "native": native, "timeout": args.timeout}
    files = [f"{folder}/rayopdf-core.mjs" for _, folder in modules] + [path for _, path in docs] + \
        ([native] if native else [])
    result = measure_inside(job, cpus, files, image=WASM_IMAGE)
    result.update({"command": "wasm", "scope": scope(), "cpus": cpus, "image": WASM_IMAGE, "folders": args.dir,
                   "modules": dict(modules), "native": native, "options": args.opt, "runs": args.runs,
                   "rounds": rounds})
    timing = (f"best of {args.runs} in one Node.js process, {rounds} alternating rounds" if args.runs
              else "no timing")
    text = headline(f"wasm: {len(docs)} documents, options [{' '.join(args.opt)}], {timing}, "
                    f"CPU{'s' if args.jobs > 1 else ''} {cpus}, {WASM_IMAGE}", result["scope"])
    if args.runs:
        text += "\n\n" + speed_table("module", result["rows"])
    differ = 0
    for name, identity in result["identity"].items():
        differ += len(identity["differences"])
        text += (f"\n\n{name} against {native} --convert: identical: {identity['identical']} of "
                 f"{identity['compared']}")
        for difference in identity["differences"][:5]:
            text += f"\n  {difference['document']}: {difference['differs']}"
    finish(args, result, text)
    return 1 if differ else 0


# ---- command line --------------------------------------------------------------------------------

INSIDE = {"warm": inside_warm, "fresh": inside_fresh, "identical": inside_identical, "instr": inside_instr,
          "pages": inside_pages, "serve": inside_serve, "wasm": inside_wasm}


def cmd_inside(_args) -> int:
    """The half that runs in the measuring container: a job as JSON on standard input, its result
    as one line of JSON on standard output."""
    job = json.load(sys.stdin)
    print(json.dumps(INSIDE[job["task"]](job)))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    sub = parser.add_subparsers(dest="command", required=True,
                                metavar="{warm,fresh,identical,hashes,instr,tools,pages,serve,wasm}")

    def add(name: str, func, text: str | None):
        p = sub.add_parser(name, help=text)
        p.set_defaults(func=func)
        p.add_argument("--json", metavar="FILE", help="write the whole result as JSON")
        return p

    def binaries_and_options(p) -> None:
        p.add_argument("--bin", action="append", required=True, metavar="NAME=PATH",
                       help="a binary under a name; the first is the base of the changes")
        p.add_argument("--opt", action="append", default=[], metavar="OPTION",
                       help="an option of the conversion, as --opt=--images=none")

    cpus_default = os.environ.get("RAYOPDF_DOCKER_CPUS")
    p = add("warm", cmd_warm, "in-process speed: ms/page and pages/s over a folder")
    binaries_and_options(p)
    p.add_argument("--dir", action="append", required=True, help="a folder of PDFs")
    p.add_argument("--runs", type=int, default=3, help="timed conversions a process (default 3)")
    p.add_argument("--rounds", type=int, default=2, help="rounds over the documents (default 2)")
    p.add_argument("--cpu", type=int, default=3, help="the CPU to measure on (default 3)")

    p = add("fresh", cmd_fresh, "fresh-process time a document over a folder")
    binaries_and_options(p)
    p.add_argument("--dir", action="append", required=True, help="a folder of PDFs")
    p.add_argument("--runs", type=int, default=11, help="timed runs a document (default 11)")
    p.add_argument("--warmup", type=int, default=2, help="untimed runs a document first (default 2)")
    p.add_argument("--rounds", type=int, default=12, help="rounds, the binaries' order rotated (default 12)")
    p.add_argument("--cpu", type=int, default=3, help="the CPU to measure on (default 3)")
    p.add_argument("--per-doc", action="store_true",
                   help="keep every document's figures in --json and print the ten slowest")

    p = add("identical", cmd_identical, "two binaries write the same output")
    p.add_argument("--a", required=True, help="one binary")
    p.add_argument("--b", required=True, help="the other binary")
    p.add_argument("--input", action="append", required=True, help="a folder of PDFs, subfolders included")
    p.add_argument("--set", action="append", metavar="NAME=OPTIONS",
                   help="an option set under a name (default: none=--images=none and images=)")
    p.add_argument("--a-opt", action="append", default=[], metavar="OPTION",
                   help="an option for --a only, after the set's (repeat for more; write --a-opt=--workers=1)")
    p.add_argument("--b-opt", action="append", default=[], metavar="OPTION",
                   help="an option for --b only, after the set's (repeat for more)")
    p.add_argument("--jobs", type=int, default=0, help="comparisons at once (default: one a CPU)")
    p.add_argument("--cpus", default=cpus_default, help="cpuset of the container (default: $RAYOPDF_DOCKER_CPUS)")
    p.add_argument("--timeout", type=int, default=300, help="seconds a conversion may take (default 300)")
    p.add_argument("--show", type=int, default=20, help="differences to print (default 20)")
    p.add_argument("--b-emulator", metavar="PROGRAM", help="run B through this program (qemu-aarch64-static)")
    p.add_argument("--image", default=PERF_IMAGE, help=f"the container's image (default {PERF_IMAGE})")

    p = add("hashes", cmd_hashes, "SHA-256 of every output of one binary, on this host (no Docker)")
    p.add_argument("--bin", help="the binary")
    p.add_argument("--dir", action="append", help="a folder of PDFs, subfolders included")
    p.add_argument("--set", action="append", metavar="NAME=OPTIONS",
                   help="an option set under a name (default: none=--images=none and images=)")
    p.add_argument("--jobs", type=int, default=0, help="conversions at once (default: one a CPU)")
    p.add_argument("--timeout", type=int, default=300, help="seconds a conversion may take (default 300)")
    p.add_argument("--compare", nargs=2, metavar=("A.json", "B.json"),
                   help="compare the --json files of two runs instead (exit 1 when an output differs)")
    p.add_argument("--show", type=int, default=20, help="differences to print with --compare (default 20)")

    p = add("instr", cmd_instr, "instructions a conversion under callgrind")
    binaries_and_options(p)
    p.add_argument("--doc", action="append", required=True, help="a PDF")
    p.add_argument("--n", type=int, default=5, help="conversions between the two counts (default 5)")
    p.add_argument("--cpus", default=cpus_default, help="cpuset of the container (default: $RAYOPDF_DOCKER_CPUS)")

    p = add("tools", cmd_tools, "RayoPDF against the other tools' command lines")
    p.add_argument("--bin", required=True, help="RayoPDF's binary (static)")
    p.add_argument("--doc", action="append", required=True, metavar="NAME=PDF", help="a document under a name")
    p.add_argument("--cpus", action="append", metavar="LIST",
                   help="cpuset of a run, threads = its CPUs (default 3; repeat for more tables)")
    p.add_argument("--only", action="append", metavar="TOOL", help=f"only this tool (of {', '.join(TOOLS)})")

    p = add("pages", cmd_pages, "multi-page documents per --workers count")
    p.add_argument("--bin", required=True, help="RayoPDF's binary")
    p.add_argument("--list", action="append", metavar="FILE", help="a file with one PDF path a line")
    p.add_argument("--dir", action="append", help="a folder of PDFs")
    p.add_argument("--workers", default="0 1 2 4", help='worker counts, 0 for the default (default "0 1 2 4")')
    p.add_argument("--cpus", default="2,3,4,5", help="cpuset of the container (default 2,3,4,5)")
    p.add_argument("--runs", type=int, default=15, help="timed runs a document and count (default 15)")
    p.add_argument("--warmup", type=int, default=2, help="untimed runs first (default 2)")

    p = add("serve", cmd_serve, "one --serve process over documents: time a request, memory, answers checked")
    binaries_and_options(p)
    p.add_argument("--dir", action="append", help="a folder of PDFs, subfolders included")
    p.add_argument("--list", action="append", metavar="FILE", help="a file with one PDF path a line")
    p.add_argument("--rounds", type=int, default=2, help="sessions a binary, alternating (default 2)")
    p.add_argument("--cpu", type=int, default=3, help="the CPU to measure on (default 3)")
    p.add_argument("--timeout", type=int, default=300, help="seconds a request may take (default 300)")
    p.add_argument("--show", type=int, default=5, help="differences to print a binary (default 5)")

    p = add("wasm", cmd_wasm, "the WebAssembly module's speed, and its output against a native binary")
    p.add_argument("--module", action="append", required=True, metavar="NAME=DIR",
                   help="a folder with rayopdf-core.mjs and .wasm under a name; the first is the base")
    p.add_argument("--dir", action="append", required=True, help="a folder of PDFs, subfolders included")
    p.add_argument("--opt", action="append", default=[], metavar="OPTION",
                   help="an option of the conversion, as --opt=--images=none")
    p.add_argument("--runs", type=int, default=3, help="timed conversions a document, 0 for none (default 3)")
    p.add_argument("--rounds", type=int, default=2, help="rounds over the documents (default 2)")
    p.add_argument("--cpu", type=int, default=3, help="the CPU to measure on (default 3)")
    p.add_argument("--native", metavar="BIN", help="compare every output with BIN --convert's")
    p.add_argument("--jobs", type=int, default=1,
                   help="Node.js processes at once, on --cpus; more than 1 takes --runs 0 (default 1)")
    p.add_argument("--cpus", default=cpus_default, help="cpuset with --jobs (default: $RAYOPDF_DOCKER_CPUS)")
    p.add_argument("--timeout", type=int, default=300, help="seconds a native conversion may take (default 300)")

    sub.add_parser("inside").set_defaults(func=cmd_inside)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
