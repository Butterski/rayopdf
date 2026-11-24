# SPDX-License-Identifier: Apache-2.0
"""RayoPDF: PDF to Markdown in the calling process.

The package wraps the converter's C ABI (include/rayopdf/rayopdf_c.h) with ctypes; the library,
librayopdf.so or rayopdf.dll, lies in the package's _lib folder and needs nothing else installed.

    import rayopdf

    result = rayopdf.convert("paper.pdf")              # a path, or the PDF's bytes
    print(result.text)                                 # the Markdown
    chunks = rayopdf.convert("paper.pdf", format="chunks").data
    try:
        rayopdf.convert(b"not a pdf")
    except rayopdf.CorruptError as error:
        print(error.status, error)

The options are those of `rayopdf --convert`; a call converts on `workers` page threads and
releases the GIL while it runs. LangChain and LlamaIndex loaders: rayopdf.integrations.
"""

from __future__ import annotations

import ctypes
import json
import os
import sys
import threading
from pathlib import Path
from typing import Any, List, Optional, Tuple, Union

__all__ = [
    "convert",
    "version",
    "Result",
    "RayoPdfError",
    "UsageError",
    "UnreadableError",
    "CorruptError",
    "PasswordRequiredError",
    "UnwritableError",
    "UnsupportedEncryptionError",
]


class RayoPdfError(Exception):
    """A conversion that failed: `status` is the command line's exit code, `message` why."""

    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


class UsageError(RayoPdfError):
    """Status 2: an option with an invalid value."""


class UnreadableError(RayoPdfError):
    """Status 3: the input cannot be read."""


class CorruptError(RayoPdfError):
    """Status 10: not a PDF, corrupt beyond repair, or past a document limit."""


class PasswordRequiredError(RayoPdfError):
    """Status 11: encrypted, and the password does not open it."""


class UnwritableError(RayoPdfError):
    """Status 12: an output (an image file) could not be written."""


class UnsupportedEncryptionError(RayoPdfError):
    """Status 13: encrypted with a handler RayoPDF does not implement."""


_ERRORS = {2: UsageError, 3: UnreadableError, 10: CorruptError, 11: PasswordRequiredError, 12: UnwritableError,
           13: UnsupportedEncryptionError}


class Result:
    """A conversion's output.

    text: the Markdown (or the JSON, JSON Lines or HTML of `format`);
    data: the JSON object for format="json", the list of chunks for format="chunks", else None;
    report: the --report counters (a dict);
    pages: the pages converted.
    """

    __slots__ = ("text", "data", "report", "pages")

    def __init__(self, text: str, data: Any, report: dict, pages: int) -> None:
        self.text = text
        self.data = data
        self.report = report
        self.pages = pages

    def __repr__(self) -> str:
        return f"Result(pages={self.pages}, text={len(self.text)} characters)"


class _Output(ctypes.Structure):
    _fields_ = [("status", ctypes.c_int), ("pages", ctypes.c_int), ("text", ctypes.c_void_p),
                ("text_size", ctypes.c_size_t), ("report", ctypes.c_void_p), ("error", ctypes.c_void_p)]


_library = None
_library_lock = threading.Lock()


def _lib() -> ctypes.CDLL:
    global _library
    with _library_lock:
        if _library is None:
            name = "rayopdf.dll" if sys.platform == "win32" else "librayopdf.so"
            library = ctypes.CDLL(str(Path(__file__).resolve().parent / "_lib" / name))
            library.rayopdf_convert.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_char_p),
                                                ctypes.c_size_t, ctypes.POINTER(_Output)]
            library.rayopdf_convert.restype = ctypes.c_int
            library.rayopdf_output_free.argtypes = [ctypes.POINTER(_Output)]
            library.rayopdf_output_free.restype = None
            library.rayopdf_version.argtypes = []
            library.rayopdf_version.restype = ctypes.c_char_p
            _library = library
        return _library


def version() -> str:
    """The library's version, "MAJOR.MINOR.PATCH"."""
    return _lib().rayopdf_version().decode("ascii")


def _pages_flag(pages: Union[int, str, Tuple[Optional[int], Optional[int]]]) -> str:
    if isinstance(pages, bool):
        raise TypeError("pages takes an int, a string \"A-B\" or a tuple (first, last)")
    if isinstance(pages, int):
        return str(pages)
    if isinstance(pages, str):
        return pages
    if isinstance(pages, (tuple, list)) and len(pages) == 2:
        first, last = pages
        return f"{'' if first is None else int(first)}-{'' if last is None else int(last)}"
    raise TypeError("pages takes an int, a string \"A-B\" or a tuple (first, last)")


def convert(source: Union[str, os.PathLike, bytes, bytearray, memoryview], *, password: Optional[str] = None,
            pages: Union[None, int, str, Tuple[Optional[int], Optional[int]]] = None,
            images: Union[None, str, os.PathLike] = None, format: str = "markdown", math: str = "unicode",
            page_markers: bool = False, front_matter: bool = False, furniture: str = "drop", plain: bool = False,
            tables: str = "auto", quotes: str = "keep", small_caps: str = "lower", figure_text: str = "keep",
            chunk_chars: Optional[int] = None, structure: bool = True, annotations: bool = True, exact: bool = True, ocr: str = "off",
            ocr_addon: Union[None, str, os.PathLike] = None, workers: int = 1) -> Result:
    """Converts a PDF (a path, or its bytes) as `rayopdf --convert` does.

    format: markdown, json, chunks or inspect; math: unicode or latex; furniture: drop or keep;
    tables: auto, html, pipe or pipe-latex; quotes: keep or ascii; small_caps: lower or keep (headings in small
    capitals in the source's case or as printed); figure_text: keep or drop (a figure's own text, its
    tick labels and legends, as paragraphs or left out); ocr: off, auto or force (the rayopdf-ocr add-on,
    found as the command line finds it, or `ocr_addon`); images: a folder the image files are
    written to, linked as FOLDER/NAME (default: no images); pages: 3, "2-5", (2, None); workers:
    page threads, 1 to 64.
    Raises a RayoPdfError subclass when the conversion fails.
    """
    if isinstance(source, (bytes, bytearray, memoryview)):
        pdf = bytes(source)
    else:
        with open(os.fspath(source), "rb") as file:
            pdf = file.read()
    args = [f"--format={format}", f"--math={math}", f"--furniture={furniture}", f"--tables={tables}",
            f"--quotes={quotes}", f"--small-caps={small_caps}", f"--figure-text={figure_text}", f"--ocr={ocr}",
            f"--workers={int(workers)}"]
    if password is not None:
        args.append(f"--password={password}")
    if pages is not None:
        args.append(f"--pages={_pages_flag(pages)}")
    if images is not None:
        args.append(f"--images={os.fspath(images)}")
    if chunk_chars is not None:
        args.append(f"--chunk-chars={int(chunk_chars)}")
    if ocr_addon is not None:
        args.append(f"--ocr-addon={os.fspath(ocr_addon)}")
    for flag, on in (("--page-markers", page_markers), ("--front-matter", front_matter), ("--plain", plain),
                     ("--no-structure", not structure), ("--no-annotations", not annotations),
                     ("--no-exact", not exact)):
        if on:
            args.append(flag)
    encoded = [a.encode("utf-8") for a in args]
    argv = (ctypes.c_char_p * len(encoded))(*encoded)
    library = _lib()
    out = _Output()
    status = library.rayopdf_convert(pdf, len(pdf), argv, len(encoded), ctypes.byref(out))
    try:
        if status != 0:
            message = ctypes.string_at(out.error).decode("utf-8", "replace") if out.error else ""
            raise _ERRORS.get(status, RayoPdfError)(status, message)
        text = ctypes.string_at(out.text, out.text_size).decode("utf-8") if out.text_size else ""
        report = json.loads(ctypes.string_at(out.report).decode("utf-8")) if out.report else {}
        pages_converted = out.pages
    finally:
        library.rayopdf_output_free(ctypes.byref(out))
    data: Any = None
    if format == "json":
        data = json.loads(text)
    elif format == "chunks":
        data = [json.loads(line) for line in text.splitlines() if line]
    return Result(text, data, report, pages_converted)

