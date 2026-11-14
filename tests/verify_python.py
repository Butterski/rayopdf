#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks of the Python package (python/rayopdf) over a built librayopdf.so: Markdown, the JSON
document, chunks, the error classes, and the library's ABI (with `objdump -T`: no glibc symbol
version above 2.28, the manylinux_2_28 wheel's promise, and only rayopdf_* functions exported).
The package is copied with the library into a temporary folder and imported from there; the PDF
is built by the script.

Usage:
  python3 tests/verify_python.py --library build/wheel/librayopdf.so [--objdump objdump]
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAX_GLIBC = (2, 28)


def one_page_pdf(text: str) -> bytes:
    """A one-page PDF showing `text` in Helvetica."""
    content = zlib.compress(b"BT /F1 12 Tf 72 700 Td (" + text.encode("latin-1") + b") Tj ET")
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >>"
        b" /Contents 4 0 R >>",
        b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(content) + content + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]
    out = bytearray(b"%PDF-1.7\n")
    offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    out += b"".join(b"%010d 00000 n \n" % offset for offset in offsets)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref)
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--library", required=True, help="the built librayopdf.so")
    parser.add_argument("--objdump", default="objdump", help="objdump of the toolchain")
    args = parser.parse_args()
    failures: list[str] = []

    def check(condition: bool, what: str) -> None:
        if not condition:
            failures.append(what)
            print(f"FAIL {what}", file=sys.stderr)

    symbols = subprocess.run([args.objdump, "-T", args.library], capture_output=True, text=True, check=True).stdout
    versions = {tuple(int(n) for n in m) for m in re.findall(r"GLIBC_(\d+)\.(\d+)", symbols)}
    check(bool(versions) and max(versions) <= MAX_GLIBC,
          f"glibc symbol versions at most {MAX_GLIBC}: {sorted(versions)[-1:] if versions else 'none'}")
    exported = sorted(line.split()[-1] for line in symbols.splitlines()
                      if " DF " in line and "*UND*" not in line)
    check(exported == ["rayopdf_convert", "rayopdf_output_free", "rayopdf_version"],
          f"exports exactly the C ABI: {exported}")

    with tempfile.TemporaryDirectory() as work:
        package = Path(work) / "rayopdf"
        shutil.copytree(REPO / "python" / "rayopdf", package)
        (package / "_lib").mkdir()
        shutil.copy(args.library, package / "_lib" / "librayopdf.so")
        sys.path.insert(0, work)
        import rayopdf

        version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
        check(rayopdf.version() == version, f"version() is VERSION: {rayopdf.version()}")
        pdf_path = Path(work) / "hello.pdf"
        pdf_path.write_bytes(one_page_pdf("Hello from Python"))
        result = rayopdf.convert(pdf_path)
        check("Hello from Python" in result.text and result.pages == 1 and result.data is None and
              isinstance(result.report, dict) and "glyphs" in result.report, "a path converts to Markdown")
        result = rayopdf.convert(pdf_path.read_bytes(), format="json", workers=2)
        check(isinstance(result.data, dict) and result.data.get("schema") == 1 and result.text.startswith('{"rayopdf":'),
              "format=json parses, schema 1")
        result = rayopdf.convert(pdf_path, format="chunks", chunk_chars=256, plain=True, quotes="ascii", pages=(1, None))
        check(isinstance(result.data, list) and len(result.data) == 1 and
              "Hello from Python" in result.data[0]["markdown"] and result.data[0]["pages"] == [1, 1],
              "format=chunks gives a list of chunks")
        try:
            rayopdf.convert(b"not a pdf")
            check(False, "b'not a pdf' raises CorruptError")
        except rayopdf.CorruptError as error:
            check(error.status == 10 and isinstance(error, rayopdf.RayoPdfError) and str(error) != "",
                  "CorruptError carries status 10 and a message")
        try:
            rayopdf.convert(pdf_path, math="tex")
            check(False, "math='tex' raises UsageError")
        except rayopdf.UsageError as error:
            check(error.status == 2 and "--math" in error.message, f"UsageError names the flag: {error.message}")
        try:
            rayopdf.convert(Path(work) / "missing.pdf")
            check(False, "a missing file raises")
        except FileNotFoundError:
            pass
        for module in ("rayopdf.integrations.langchain", "rayopdf.integrations.llama_index"):
            try:
                __import__(module)
            except ImportError as error:
                check("pip install" in str(error), f"{module} names what to install: {error}")
    if failures:
        print(f"{len(failures)} check(s) failed", file=sys.stderr)
        return 1
    print("python: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
