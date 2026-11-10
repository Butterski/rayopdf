#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the repository's own rules (AGENTS.md) that need no build.

Usage:
  python3 tools/check_repo.py            # exit 1 and one line per finding when a rule is broken

Checks, over the files git tracks:
  - no generated material is tracked (build trees, benchmark output, PDFs, binaries, models);
  - every own Python, shell, Dockerfile, C and C++ file carries its SPDX line (first line, or the
    line after a shebang or a Dockerfile syntax directive);
  - README.md has the H2 sections tools/package.py copies, and its Status names VERSION;
  - every record under docs/benchmarks/ is linked from docs/benchmarks/README.md;
  - relative links in the Markdown documents point at tracked files;
  - every fuzz target is named tests/fuzz/fuzz_<module>_<name>.cpp for a module under src/.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPDX = "SPDX-License-Identifier: Apache-2.0"
GENERATED = re.compile(r"(^build/|^benchmark-output/|\.(pdf|exe|dll|so|a|o|onnx|param|bin|png|jpg|jp2|gcda)$|^rayopdf$)")
README_SECTIONS = ("Status", "Usage", "License")


def tracked() -> list[str]:
    out = subprocess.run(["git", "-C", str(REPO), "ls-files"], capture_output=True, text=True, check=True).stdout
    return [line for line in out.splitlines() if line]


def spdx_missing(path: str) -> bool:
    name = Path(path).name
    if path.startswith("third_party/"):
        return False
    if name.endswith((".py", ".sh")) or name.endswith(".Dockerfile") or name == "Dockerfile":
        marker = "# " + SPDX
    elif name.endswith((".cpp", ".h", ".c")):
        marker = "// " + SPDX
    else:
        return False
    lines = (REPO / path).read_text(encoding="utf-8", errors="replace").splitlines()[:2]
    if lines and lines[0].startswith(("#!", "# syntax=")):  # a shebang or a Dockerfile directive
        lines = lines[1:]
    return not lines or lines[0].strip() != marker


def markdown_links(text: str) -> list[str]:
    """Targets of [text](target) outside fenced blocks and code spans."""
    text = re.sub(r"(?ms)^```.*?^```", "", text)
    text = re.sub(r"`[^`\n]*`", "", text)
    return re.findall(r"\]\(([^)\s]+)\)", text)


def main() -> int:
    argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter).parse_args()
    files = tracked()
    known = set(files)
    directories = {str(Path(f).parent) for f in files} | {str(p) for f in files for p in Path(f).parents}
    findings: list[str] = []

    for path in files:
        if GENERATED.search(path):
            findings.append(f"{path}: generated material is tracked")
        if spdx_missing(path):
            findings.append(f"{path}: no SPDX line")

    readme = (REPO / "README.md").read_text(encoding="utf-8")
    sections = {line[3:].strip() for line in readme.splitlines() if line.startswith("## ")}
    for section in README_SECTIONS:
        if section not in sections:
            findings.append(f"README.md: no section '{section}' (tools/package.py copies it)")
    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    status = readme.split("## Status", 1)[-1].split("\n## ", 1)[0]
    if version not in status:
        findings.append(f"README.md: the Status section does not name version {version}")

    index = (REPO / "docs/benchmarks/README.md").read_text(encoding="utf-8")
    for path in files:
        if path.startswith("docs/benchmarks/") and path.endswith(".md") and Path(path).name != "README.md":
            if f"({Path(path).name})" not in index:
                findings.append(f"{path}: not linked from docs/benchmarks/README.md")

    for path in files:
        if not path.endswith(".md") or path.startswith(("third_party/", "eval/fixtures/")):
            continue
        for target in markdown_links((REPO / path).read_text(encoding="utf-8")):
            if re.match(r"^[a-z][a-z0-9+.-]*:|^#", target):
                continue
            local = target.split("#", 1)[0]
            resolved = (Path(path).parent / local).as_posix() if local else path
            parts: list[str] = []
            for part in resolved.split("/"):
                if part == "..":
                    if parts:
                        parts.pop()
                elif part not in ("", "."):
                    parts.append(part)
            resolved = "/".join(parts)
            if resolved and resolved not in known and resolved not in directories:
                findings.append(f"{path}: link to {target} points at nothing tracked")

    modules = {Path(f).parts[1] for f in files if f.startswith("src/") and len(Path(f).parts) > 2}
    for path in files:
        if path.startswith("tests/fuzz/fuzz_") and path.endswith(".cpp"):
            match = re.match(r"tests/fuzz/fuzz_([a-z0-9]+)_[a-z0-9_]+\.cpp$", path)
            if not match or match.group(1) not in modules:
                findings.append(f"{path}: not named fuzz_<module>_<name>.cpp for a module under src/")

    for finding in findings:
        print(finding)
    print(f"check_repo: {len(files)} tracked files, {len(findings)} findings")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
