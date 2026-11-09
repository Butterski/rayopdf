#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate src/hot.ld, the GNU ld script that groups the code a fresh conversion runs.

A one-shot `rayopdf --convert` faults its code in by 64 KiB windows of the executable (the
kernel's fault-around). Release builds put every function in a section of its own
(-ffunction-sections); this script finds the sections a set of documents executes and writes a
linker script that places them, most used first, in one output section before .text:

  SECTIONS { .text.hot : { *(.text._ZN...) ... *libc.a:malloc.o(.text) ... } } INSERT BEFORE .text;

CMake passes the script to GNU ld for Release builds of the command line on Linux (the release
and the static config). Placement changes no output byte. A name that no longer exists matches
nothing and is ignored, so a stale list is harmless; it only stops helping. Regenerate it after
larger changes to the code every conversion runs (the method of RayoMD's scripts/hot_sections.py
at ef106ca, Apache-2.0, same author):

  python3 tools/dev.py build --config release
  python3 tools/dev.py build --config static
  python3 tools/dev.py --image rayopdf-perf:1 run -- \\
      python3 tools/hot_sections.py --build-dir build/release --build-dir build/static

For each build directory the script relinks rayopdf from its objects with a link map and without
the current hot.ld (the link command of the Ninja build, `ninja -t commands rayopdf`), runs
`valgrind --tool=callgrind --dump-instr=yes` on a fresh `--convert ... --images=none` of every
document, and maps each executed instruction to the input section that holds it. Sections are
ordered by how many conversions ran them, then by their place in the default layout. Archive
members without function sections (glibc's in the static build) and generic section names of
single objects are named with their file.

Documents: from the evaluation harness's corpus (--corpus, default benchmark-output/eval, made by
tools/eval.py): every tenth odl-bench PDF and four synthetic fixtures of eight producers, or those
given with --doc. Needs valgrind, ninja and the compiler the build directories were configured
with (the rayopdf-perf:1 image has them); Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The default documents, relative to the corpus folder.
ODL_IDS = [f"010300000{n:05d}" for n in range(2, 200, 10)]
PRODUCERS = ["chromium", "libreoffice", "libreoffice-tagged", "rayomd", "rayomd-pdfua", "rayomd-tech", "typst",
             "xelatex"]
FIXTURES = ["c01-headings", "c02-paragraphs", "c06-tables", "d02-business-report"]

# Section names that more than one object uses: only with their file they name one section.
GENERIC = {".text", ".text.unlikely", ".text.hot", ".text.startup", ".text.exit"}


def default_documents(corpus: Path) -> list[Path]:
    docs = [corpus / "data" / "odl-bench" / "pdfs" / f"{n}.pdf" for n in ODL_IDS]
    docs += [corpus / "corpus" / p / f"{f}.pdf" for p in PRODUCERS for f in FIXTURES]
    missing = [str(d) for d in docs if not d.is_file()]
    if missing:
        raise SystemExit(f"hot_sections: {len(missing)} documents missing, first {missing[0]} "
                         "(make the corpus with tools/eval.py, or give --corpus or --doc)")
    return docs


def parse_map(path: Path) -> list[tuple[str, int, int, str]]:
    """Input sections of the executable's code: (name, address, size, file)."""
    sections = []
    pending = None
    started = False
    for line in path.read_text(errors="replace").splitlines():
        if not started:
            started = line.startswith("Linker script and memory map")
            continue
        if pending is not None:
            m = re.match(r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", line)
            if m:
                sections.append((pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip()))
            pending = None
            continue
        m = re.match(r" (\.text\S*)\s*$", line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r" (\.text\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", line)
        if m:
            sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()))
    return [s for s in sections if s[2] > 0]


def pattern(name: str, file: str) -> str:
    """The input section description of one section in a GNU ld script."""
    m = re.match(r"(.*/)?([^/]+\.a)\(([^)]+)\)$", file)
    if m:
        archive = re.sub(r"-\d+(\.\d+)*\.a$", "-*.a", m.group(2))  # libm-2.39.a: any glibc
        return f"*{archive}:{m.group(3)}({name})"
    if name in GENERIC:
        return f"*{os.path.basename(file)}({name})"
    return f"*({name})"


def executed_addresses(callgrind_out: Path, binary: Path) -> tuple[set[int], int | None]:
    """The instruction addresses the profiled binary itself executed, and main's lowest one."""
    addresses = set()
    in_binary = False
    current_fn = ""
    main_low = None
    for line in callgrind_out.read_text(errors="replace").splitlines():
        if line.startswith("ob="):
            in_binary = Path(line[3:].strip()).name == binary.name
        elif line.startswith("fn="):
            current_fn = line[3:].strip()
        elif in_binary and line.startswith("0x"):
            address = int(line.split()[0], 16)
            addresses.add(address)
            if current_fn == "main" and (main_low is None or address < main_low):
                main_low = address
    return addresses, main_low


def symbol_address(binary: Path, name: str) -> int:
    out = subprocess.run(["nm", "--defined-only", str(binary)], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name and parts[1] in "Tt":
            return int(parts[0], 16)
    raise SystemExit(f"hot_sections: no symbol {name} in {binary}")


def link_command(build: Path) -> list[str]:
    """The command that links rayopdf in `build`, as Ninja runs it (`: && c++ ... && :`)."""
    out = subprocess.run(["ninja", "-C", str(build), "-t", "commands", "rayopdf"], capture_output=True, text=True,
                         check=True).stdout
    lines = [line for line in out.splitlines() if re.search(r"(^|\s)-o rayopdf(\s|$)", line)]
    if not lines:
        raise SystemExit(f"hot_sections: no link command of rayopdf in {build}")
    words = shlex.split(lines[-1])
    while words and words[0] in (":", "&&"):
        words.pop(0)
    if "&&" in words:
        words = words[:words.index("&&")]
    return words


def relink(build: Path, work: Path) -> tuple[Path, Path]:
    args = [a for a in link_command(build) if a != "-s" and not re.match(r"-Wl,-T,.*hot\.ld$", a)]
    out = args.index("-o")
    binary, map_file = work / f"{build.name}-rayopdf", work / f"{build.name}.map"
    args[out + 1] = str(binary)
    args.insert(1, f"-Wl,-Map={map_file}")
    subprocess.run(args, cwd=build, check=True)
    return binary, map_file


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build-dir", action="append", required=True, type=Path,
                        help="a configured and built Release tree (Ninja); repeat for the release and the static build")
    parser.add_argument("--corpus", type=Path, default=ROOT / "benchmark-output" / "eval",
                        help="the evaluation harness's output folder, for the default documents")
    parser.add_argument("--doc", action="append", type=Path, help="a PDF to convert (repeatable; replaces the default)")
    parser.add_argument("--out", type=Path, default=ROOT / "src" / "hot.ld")
    parser.add_argument("--valgrind", default="valgrind")
    args = parser.parse_args()

    docs = args.doc or default_documents(args.corpus)
    uses: dict[str, int] = {}
    order: dict[str, tuple[int, int]] = {}
    with tempfile.TemporaryDirectory(prefix="rayopdf-hot-") as temp:
        work = Path(temp)
        for index, build in enumerate(args.build_dir):
            build = build.resolve()
            binary, map_file = relink(build, work)
            sections = sorted(parse_map(map_file), key=lambda s: s[1])
            starts = [s[1] for s in sections]
            main_link = symbol_address(binary, "main")
            for doc in docs:
                out = work / "cg.out"
                subprocess.run([args.valgrind, "--tool=callgrind", "--dump-instr=yes", "--compress-pos=no",
                                "--compress-strings=no", f"--callgrind-out-file={out}", str(binary), "--convert",
                                str(doc.resolve()), str(work / "out.md"), "--images=none"],
                               cwd=work, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                addresses, main_low = executed_addresses(out, binary)
                if main_low is None:
                    raise SystemExit(f"hot_sections: main not found in the profile of {doc}")
                bias = main_low - main_link  # where the loader put a PIE
                hit = set()
                for address in addresses:
                    address -= bias
                    lo, hi = 0, len(starts)
                    while lo < hi:
                        mid = (lo + hi) // 2
                        if starts[mid] <= address:
                            lo = mid + 1
                        else:
                            hi = mid
                    if lo and address < sections[lo - 1][1] + sections[lo - 1][2]:
                        hit.add(lo - 1)
                for at in hit:
                    name, address, _, file = sections[at]
                    key = pattern(name, file)
                    uses[key] = uses.get(key, 0) + 1
                    order.setdefault(key, (index, address))
                print(f"{build.name}: {doc.name}: {len(hit)} sections", file=sys.stderr)

    ranked = sorted(uses, key=lambda key: (-uses[key], order[key]))
    lines = [
        "/* The code fresh conversions run, grouped before .text so that they fault in fewer pages.",
        "   Generated by tools/hot_sections.py; regenerate it with that script, never by hand.",
        "   Names that no longer exist match nothing. */",
        "SECTIONS",
        "{",
        "  .text.hot :",
        "  {",
    ]
    lines += [f"    {key}" for key in ranked]
    lines += ["  }", "}", "INSERT BEFORE .text;", ""]
    args.out.write_text("\n".join(lines))
    print(f"{args.out}: {len(ranked)} sections", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
