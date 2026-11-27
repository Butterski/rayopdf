#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks of the Python package (python/rayopdf) over a built librayopdf.so or librayopdf.dylib:
Markdown, the JSON document, chunks, the error classes, the Haystack component against a stub
haystack package, and the library's ABI: a library of this machine's architecture (ELF x86-64 or
aarch64, Mach-O arm64 or x86_64) exporting only the rayopdf_* functions; an ELF one (read with
`objdump -T`) with no glibc symbol version above 2.28, the manylinux_2_28 wheels' promise.
The package is copied with the library into a temporary folder and imported from there; the PDF
is built by the script.

Usage:
  python3 tests/verify_python.py --library build/wheel/librayopdf.so [--objdump objdump]
  python3 tests/verify_python.py --library build/macos-lib/librayopdf.dylib   # macOS: Mach-O, read with nm
"""

from __future__ import annotations

import argparse
import importlib
import logging
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAX_GLIBC = (2, 28)
# ELF e_machine of the architectures the wheels are built for, by platform.machine().
ELF_MACHINES = {"x86_64": 62, "aarch64": 183}
# Mach-O CPU types by platform.machine() (macOS).
MACHO_CPUS = {"arm64": 0x0100000C, "x86_64": 0x01000007}


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


# A stand-in for the part of haystack-ai's API that rayopdf.integrations.haystack uses.
HAYSTACK_STUB = '''
import dataclasses
from typing import Any, Dict, Optional


@dataclasses.dataclass
class Document:
    content: Optional[str] = None
    meta: Dict[str, Any] = dataclasses.field(default_factory=dict)


class _Component:
    def __call__(self, cls):
        cls.__haystack_component__ = True
        return cls

    @staticmethod
    def output_types(**types):
        def wrap(run):
            run.__haystack_output_types__ = types
            return run
        return wrap


component = _Component()


def default_to_dict(obj, **init_parameters):
    return {"type": f"{type(obj).__module__}.{type(obj).__name__}", "init_parameters": init_parameters}


def default_from_dict(cls, data):
    return cls(**data.get("init_parameters", {}))
'''
HAYSTACK_DATACLASSES_STUB = '''
import dataclasses
from typing import Any, Dict, Optional


@dataclasses.dataclass
class ByteStream:
    data: bytes
    meta: Dict[str, Any] = dataclasses.field(default_factory=dict)
    mime_type: Optional[str] = None
'''


def check_haystack(work: Path, pdf_path: Path, check) -> None:
    """RayoPdfToDocument against the stub haystack package: paths and ByteStreams, meta for every
    source or one a source, failed sources skipped with a warning, chunks, to_dict/from_dict."""
    (work / "haystack" / "dataclasses").mkdir(parents=True)
    (work / "haystack" / "__init__.py").write_text(HAYSTACK_STUB, encoding="utf-8")
    (work / "haystack" / "dataclasses" / "__init__.py").write_text(HAYSTACK_DATACLASSES_STUB, encoding="utf-8")
    importlib.invalidate_caches()
    from haystack.dataclasses import ByteStream
    from rayopdf import UsageError
    from rayopdf.integrations.haystack import RayoPdfToDocument

    warnings: list[logging.LogRecord] = []
    handler = logging.Handler()
    handler.emit = warnings.append
    logging.getLogger("rayopdf.integrations.haystack").addHandler(handler)
    sources = [pdf_path, ByteStream(data=pdf_path.read_bytes(), meta={"file_path": "memory.pdf"}),
               str(work / "missing.pdf"), ByteStream(data=b"not a pdf")]
    documents = RayoPdfToDocument().run(sources=sources, meta={"batch": 7})["documents"]
    check(len(documents) == 2 and all("Hello from Python" in d.content for d in documents) and
          documents[0].meta == {"file_path": str(pdf_path), "batch": 7, "pages": 1} and
          documents[1].meta == {"file_path": "memory.pdf", "batch": 7, "pages": 1},
          f"haystack: one Document a PDF, its meta: {[d.meta for d in documents]}")
    check(len(warnings) == 2 and all(w.levelno == logging.WARNING for w in warnings),
          f"haystack: each failed source skipped with a warning: {len(warnings)}")
    converter = RayoPdfToDocument(format="chunks", chunk_chars=256)
    documents = converter.run(sources=[pdf_path], meta=[{"n": 1}])["documents"]
    check(len(documents) == 1 and "Hello from Python" in documents[0].content and
          documents[0].meta == {"file_path": str(pdf_path), "n": 1, "chunk": 0, "pages": [1, 1], "heading_path": []},
          f"haystack: format=chunks gives a Document a chunk: {[d.meta for d in documents]}")
    data = converter.to_dict()
    again = RayoPdfToDocument.from_dict(data)
    check(data["init_parameters"] == {"format": "chunks", "chunk_chars": 256} and again.format == "chunks" and
          again.options == {"chunk_chars": 256}, f"haystack: to_dict and from_dict: {data}")
    for bad in ("json", "inspect"):
        try:
            RayoPdfToDocument(format=bad)
            check(False, f"haystack: format={bad} raises ValueError")
        except ValueError:
            pass
    try:
        RayoPdfToDocument(math="tex").run(sources=[pdf_path])
        check(False, "haystack: an invalid option raises UsageError")
    except UsageError as error:
        check(error.status == 2, f"haystack: UsageError: {error}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--library", required=True, help="the built librayopdf.so or librayopdf.dylib")
    parser.add_argument("--objdump", default="objdump", help="objdump of the toolchain (ELF libraries)")
    args = parser.parse_args()
    failures: list[str] = []

    def check(condition: bool, what: str) -> None:
        if not condition:
            failures.append(what)
            print(f"FAIL {what}", file=sys.stderr)

    data = Path(args.library).read_bytes()
    if data[:4] == b"\xcf\xfa\xed\xfe":  # Mach-O, 64-bit little-endian: librayopdf.dylib
        name = "librayopdf.dylib"
        check(struct.unpack_from("<I", data, 4)[0] == MACHO_CPUS.get(platform.machine()),
              f"a Mach-O library of this machine ({platform.machine()})")
        symbols = subprocess.run(["nm", "-gU", args.library], capture_output=True, text=True, check=True).stdout
        exported = sorted(line.split()[-1].removeprefix("_") for line in symbols.splitlines() if line.strip())
    else:
        name = "librayopdf.so"
        check(data[:4] == b"\x7fELF" and len(data) >= 20 and
              struct.unpack_from("<H", data, 18)[0] == ELF_MACHINES.get(platform.machine()),
              f"an ELF library of this machine ({platform.machine()})")
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
        shutil.copy(args.library, package / "_lib" / name)
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
        result = rayopdf.convert(pdf_path, format="json", json_detail="words")
        lines = [line for block in result.data["blocks"] for line in block.get("lines", [])]
        check(any(line["text"] == "Hello from Python" and [w["text"] for w in line["words"]] == ["Hello", "from", "Python"]
                  for line in lines), "json_detail=words gives the lines and their words")
        result = rayopdf.convert(pdf_path, format="chunks", chunk_chars=256, plain=True, quotes="ascii",
                                 small_caps="keep", figure_text="drop", tables="pipe-latex", pages=(1, None))
        check(isinstance(result.data, list) and len(result.data) == 1 and
              "Hello from Python" in result.data[0]["markdown"] and result.data[0]["pages"] == [1, 1],
              "format=chunks gives a list of chunks")
        result = rayopdf.convert(pdf_path, format="chunks", chunk_overlap=20, chunk_by="page")
        check(len(result.data) == 1 and result.data[0]["overlap"] == 0 and
              "Hello from Python" in result.data[0]["markdown"], "chunk_overlap and chunk_by pass through")
        result = rayopdf.convert(pdf_path, format="text")
        check(result.text == "Hello from Python\n" and result.data is None, "format=text gives the plain text")
        result = rayopdf.convert(pdf_path, format="html")
        check(result.text.startswith("<!DOCTYPE html>") and "<p>Hello from Python</p>" in result.text and
              result.data is None, "format=html gives an HTML document")
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
        for module in ("rayopdf.integrations.langchain", "rayopdf.integrations.llama_index",
                       "rayopdf.integrations.haystack"):
            try:
                __import__(module)
            except ImportError as error:
                check("pip install" in str(error), f"{module} names what to install: {error}")
        check_haystack(Path(work), pdf_path, check)
    if failures:
        print(f"{len(failures)} check(s) failed", file=sys.stderr)
        return 1
    print("python: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
