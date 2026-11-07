#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fuzz RayoPDF's parsers with libFuzzer inside the rayopdf-build:2 image.

Every target tests/fuzz/fuzz_<module>_<name>.cpp is built with Clang, ASan and UBSan
(`tools/dev.py build --config fuzz`). `run` fuzzes the targets side by side, one CPU each, and
keeps each target's corpus in build/fuzz-corpus/<target>/ between runs; crashes, leaks and
timeouts land in build/fuzz-artifacts/<target>/. Minimized reproducers of fixed findings are
committed under tests/fuzz/regressions/<target>/ and `replay` runs every target over them.

Usage:
  python3 tools/fuzz.py seeds                                   # seed the corpora with real PDFs
  python3 tools/fuzz.py run --seconds 600 [--targets a,b] [--cpus 0,1,2,4] [--dir fuzz]
  python3 tools/fuzz.py replay [--dir fuzz]

Seeds: the PDFs of the development suites (synthetic corpus without the weasyprint holdout, odl)
and the first 300 PDFs of the SafeDocs sample, each at most 2 MB, for the targets that read whole
PDFs (fuzz_pdf_document, fuzz_layout_document); the other targets start from their own inputs.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IMAGE = "rayopdf-build:2"
CORPUS = REPO / "build" / "fuzz-corpus"
ARTIFACTS = REPO / "build" / "fuzz-artifacts"
REGRESSIONS = REPO / "tests" / "fuzz" / "regressions"
EVAL = REPO / "benchmark-output" / "eval"
PDF_TARGETS = ("fuzz_pdf_document", "fuzz_layout_document")
DEFAULT_CPUS = "0,1,2,4,6,7,8,10"
SEED_LIMIT = 2 << 20


def targets() -> list[str]:
    return sorted(p.stem for p in (REPO / "tests" / "fuzz").glob("fuzz_*.cpp"))


def build(directory: str) -> None:
    code = subprocess.run([sys.executable, str(REPO / "tools" / "dev.py"), "build", "--config", "fuzz",
                           "--dir", directory]).returncode
    if code:
        raise SystemExit("fuzz build failed")


def docker(args: list[str], cpu: str | None = None, log: Path | None = None,
           name: str | None = None) -> subprocess.Popen:
    """A container running `args`; its output goes to `log` (a pipe would fill and stall it)."""
    cmd = ["docker", "run", "--rm", "--init", "-u", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp",
           "--network", "none", "--memory=4g", "-v", f"{REPO}:/src", "-w", "/src"]
    if cpu is not None:
        cmd.append(f"--cpuset-cpus={cpu}")
    if name is not None:
        cmd += ["--name", name]
    if log is None:
        return subprocess.Popen(cmd + [IMAGE] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    with log.open("w") as out:
        return subprocess.Popen(cmd + [IMAGE] + args, stdout=out, stderr=subprocess.STDOUT)


def cmd_seeds(args) -> int:
    pdfs: list[tuple[str, bytes]] = []
    for path in sorted((EVAL / "corpus").glob("*/*.pdf")):
        if path.parent.name != "weasyprint" and path.stat().st_size <= SEED_LIMIT:
            pdfs.append((f"synthetic-{path.parent.name}-{path.stem}.pdf", path.read_bytes()))
    for path in sorted((EVAL / "data" / "odl-bench" / "pdfs").glob("*.pdf")):
        if path.stat().st_size <= SEED_LIMIT:
            pdfs.append((f"odl-{path.name}", path.read_bytes()))
    archive = EVAL / "data" / "safedocs" / "0000.zip"
    if archive.is_file():
        with zipfile.ZipFile(archive) as z:
            names = [n for n in sorted(z.namelist()) if n.lower().endswith(".pdf")][:300]
            for name in names:
                if z.getinfo(name).file_size <= SEED_LIMIT:
                    pdfs.append((f"safedocs-{Path(name).name}", z.read(name)))
    for target in PDF_TARGETS:
        directory = CORPUS / target
        directory.mkdir(parents=True, exist_ok=True)
        for name, data in pdfs:
            (directory / name).write_bytes(data)
    print(f"{len(pdfs)} seed PDFs for {', '.join(PDF_TARGETS)}")
    return 0


def cmd_run(args) -> int:
    build(args.dir)
    names = args.targets.split(",") if args.targets else targets()
    free = args.cpus.split(",")
    queue = list(names)
    running: dict[str, tuple[subprocess.Popen, float, str]] = {}
    results: dict[str, dict] = {}
    try:
        while queue or running:
            while queue and free:
                target = queue.pop(0)
                cpu = free.pop(0)
                for directory in (CORPUS / target, ARTIFACTS / target):
                    directory.mkdir(parents=True, exist_ok=True)
                argv = [f"build/{args.dir}/{target}", f"-max_total_time={args.seconds}", "-timeout=10",
                        "-rss_limit_mb=2048", "-print_final_stats=1",
                        f"-artifact_prefix=build/fuzz-artifacts/{target}/", f"build/fuzz-corpus/{target}"]
                log = ARTIFACTS / f"{target}.log"
                running[target] = (docker(argv, cpu, log, f"rayopdf-fuzz-{target}"), time.time(), cpu)
                print(f"started {target} on CPU {cpu}", flush=True)
            for target, (proc, started, cpu) in list(running.items()):
                if proc.poll() is None:
                    continue
                output = (ARTIFACTS / f"{target}.log").read_text(errors="replace")
                # "stat::number_of_executed_units: 2213521": the key runs to the last colon.
                stats = {line.rsplit(":", 1)[0].strip(): line.rsplit(":", 1)[1].strip()
                         for line in output.splitlines() if line.startswith("stat::")}
                artifacts = sorted(p.name for p in (ARTIFACTS / target).iterdir())
                results[target] = {"exit": proc.returncode, "seconds": round(time.time() - started),
                                   "runs": stats.get("stat::number_of_executed_units"),
                                   "peak_rss_mb": stats.get("stat::peak_rss_mb"), "artifacts": artifacts}
                status = "clean" if proc.returncode == 0 and not artifacts else "FINDINGS"
                print(f"{target}: {status} {json.dumps(results[target])}", flush=True)
                if status != "clean":
                    print("\n".join(output.splitlines()[-40:]))
                del running[target]
                free.append(cpu)
            time.sleep(1)
    except KeyboardInterrupt:
        for target in running:
            subprocess.run(["docker", "kill", f"rayopdf-fuzz-{target}"], capture_output=True)
        raise
    summary = REPO / "build" / f"{args.dir}-summary.json"
    summary.write_text(json.dumps(results, indent=2) + "\n")
    bad = [t for t, r in results.items() if r["exit"] != 0 or r["artifacts"]]
    print(f"{len(results) - len(bad)}/{len(results)} targets clean; summary in {summary.relative_to(REPO)}")
    return 1 if bad else 0


def cmd_replay(args) -> int:
    build(args.dir)
    failed = 0
    for target in targets():
        directory = REGRESSIONS / target
        inputs = sorted(p for p in directory.glob("*") if p.is_file()) if directory.is_dir() else []
        if not inputs:
            continue
        proc = docker([f"build/{args.dir}/{target}", "-runs=0"] + [str(p.relative_to(REPO)) for p in inputs])
        output, _ = proc.communicate()
        ok = proc.returncode == 0
        failed += not ok
        print(f"{target}: {len(inputs)} inputs {'ok' if ok else 'FAILED'}")
        if not ok:
            print("\n".join(output.splitlines()[-40:]))
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("seeds", help="seed the PDF targets' corpora").set_defaults(func=cmd_seeds)
    p = sub.add_parser("run", help="fuzz targets side by side")
    p.add_argument("--seconds", type=int, required=True, help="time per target")
    p.add_argument("--targets", help="comma-separated target names (default: all)")
    p.add_argument("--cpus", default=DEFAULT_CPUS, help=f"CPUs to use, one target each (default {DEFAULT_CPUS})")
    p.add_argument("--dir", default="fuzz", help="build directory under build/ (default: fuzz)")
    p.set_defaults(func=cmd_run)
    p = sub.add_parser("replay", help="run every target over tests/fuzz/regressions/")
    p.add_argument("--dir", default="fuzz", help="build directory under build/ (default: fuzz)")
    p.set_defaults(func=cmd_replay)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
