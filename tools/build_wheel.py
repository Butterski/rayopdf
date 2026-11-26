#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the Python package's wheels from an already-built shared library, reproducibly.

A wheel holds python/rayopdf/ (the ctypes binding and its LangChain and LlamaIndex loaders), the
library in rayopdf/_lib/, and in its .dist-info folder METADATA (Requires-Python >= 3.9, extras
langchain and llama-index), WHEEL, RECORD (SHA-256 of every file), LICENSE, NOTICE and the core
archives' third_party/licenses/. Entries are sorted and carry one fixed time (SOURCE_DATE_EPOCH,
else the time of the HEAD commit), so the same library gives the same bytes.

  rayopdf-VERSION-py3-none-manylinux_2_28_x86_64.whl    build/wheel/librayopdf.so
  rayopdf-VERSION-py3-none-manylinux_2_28_aarch64.whl   build/wheel-arm64/librayopdf.so
  rayopdf-VERSION-py3-none-macosx_11_0_arm64.whl        build/macos-arm64-lib/librayopdf.dylib
  rayopdf-VERSION-py3-none-macosx_10_15_x86_64.whl      build/macos-x64-lib/librayopdf.dylib
  rayopdf-VERSION-py3-none-win_amd64.whl                build/windows/rayopdf.dll

Build the libraries first:
  python3 tools/dev.py build --config wheel      # in rayopdf-wheel:1 (manylinux_2_28)
  python3 tools/dev.py build --config wheel --dir wheel-arm64   # the same on an arm64 machine
  python3 tools/dev.py build --config macos-lib --dir macos-arm64-lib --target rayopdf_c   # on an arm64 Mac
  python3 tools/dev.py build --config windows

tools/package.py builds the same wheels (parts wheel-linux, wheel-linux-arm64, wheel-macos-arm64,
wheel-macos-x64 and wheel-windows) beside the release archives and lists them in SHA256SUMS.

Usage:
  python3 tools/build_wheel.py --platform linux|linux-arm64|macos-arm64|macos-x64|windows [--library PATH]
                               [--out build/dist]
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# Platform: (wheel platform tag, library file name in rayopdf/_lib/, default library path).
PLATFORMS = {
    "linux": ("manylinux_2_28_x86_64", "librayopdf.so", "build/wheel/librayopdf.so"),
    "linux-arm64": ("manylinux_2_28_aarch64", "librayopdf.so", "build/wheel-arm64/librayopdf.so"),
    "macos-arm64": ("macosx_11_0_arm64", "librayopdf.dylib", "build/macos-arm64-lib/librayopdf.dylib"),
    "macos-x64": ("macosx_10_15_x86_64", "librayopdf.dylib", "build/macos-x64-lib/librayopdf.dylib"),
    "windows": ("win_amd64", "rayopdf.dll", "build/windows/rayopdf.dll"),
}
SUMMARY = "PDF to Markdown, JSON and chunks for retrieval, in process: RayoPDF's converter"
EXTRAS = {"langchain": "langchain-core", "llama-index": "llama-index-core"}


def metadata(version: str, readme: bytes, repository: str) -> bytes:
    lines = ["Metadata-Version: 2.1", "Name: rayopdf", f"Version: {version}", f"Summary: {SUMMARY}",
             f"Home-page: {repository}", "License: Apache-2.0", "Requires-Python: >=3.9",
             "Classifier: License :: OSI Approved :: Apache Software License",
             "Classifier: Programming Language :: Python :: 3",
             "Classifier: Topic :: Text Processing :: Markup :: Markdown"]
    for extra, requirement in EXTRAS.items():
        lines.append(f"Provides-Extra: {extra}")
        lines.append(f'Requires-Dist: {requirement}; extra == "{extra}"')
    lines.append("Description-Content-Type: text/markdown")
    return ("\n".join(lines) + "\n\n").encode("utf-8") + readme


def record_hash(data: bytes) -> str:
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode("ascii")


def build(library: Path, platform: str, version: str, out: Path, documents: list[tuple[str, bytes, int]],
          mtime: int, repository: str) -> Path:
    """Writes the wheel of `platform` (a key of PLATFORMS) for `library` into `out` and returns its
    path. `documents` are (name, bytes, mode) as tools/package.py's documents() gives them, README.md
    first: README.md becomes the description, the rest the licence files."""
    tag, name, _ = PLATFORMS[platform]
    info = f"rayopdf-{version}.dist-info"
    files: dict[str, tuple[bytes, int]] = {}
    for path in sorted((REPO / "python" / "rayopdf").rglob("*.py")):
        files[path.relative_to(REPO / "python").as_posix()] = (path.read_bytes(), 0o644)
    files[f"rayopdf/_lib/{name}"] = (library.read_bytes(), 0o755)
    readme = b""
    for doc, data, mode in documents:
        if doc == "README.md":
            readme = data
        elif doc != "VERSION":  # METADATA carries the version
            files[f"{info}/{doc}"] = (data, mode)
    files[f"{info}/METADATA"] = (metadata(version, readme, repository), 0o644)
    wheel = (f"Wheel-Version: 1.0\nGenerator: rayopdf tools/build_wheel.py\nRoot-Is-Purelib: false\n"
             f"Tag: py3-none-{tag}\n")
    files[f"{info}/WHEEL"] = (wheel.encode("ascii"), 0o644)
    record = "".join(f"{n},{record_hash(d)},{len(d)}\n" for n, (d, _) in sorted(files.items()))
    record += f"{info}/RECORD,,\n"
    files[f"{info}/RECORD"] = (record.encode("utf-8"), 0o644)

    path = out / f"rayopdf-{version}-py3-none-{tag}.whl"
    stamp = time.gmtime(max(mtime, 315532800))[:6]  # a zip time starts in 1980
    # The package first and RECORD last, as wheel installers expect; sorted within.
    order = sorted(n for n in files if not n.startswith(info)) + \
        sorted(n for n in files if n.startswith(info) and n != f"{info}/RECORD") + [f"{info}/RECORD"]
    with zipfile.ZipFile(path, "w") as archive:
        for entry in order:
            data, mode = files[entry]
            zinfo = zipfile.ZipInfo(entry, date_time=stamp)
            zinfo.create_system = 3
            zinfo.external_attr = (0o100000 | mode) << 16
            zinfo.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(zinfo, data, compresslevel=9)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    parser.add_argument("--library", help="the shared library (default: the platform's build output)")
    parser.add_argument("--out", default="build/dist", help="folder for the wheel")
    args = parser.parse_args()
    # The archives' documents and fixed time, as the release archives have them.
    import package  # tools/package.py, beside this file
    library = Path(args.library or PLATFORMS[args.platform][2])
    library = library if library.is_absolute() else REPO / library
    if args.platform == "linux":
        package.check_shared_linux(library)
    elif args.platform == "linux-arm64":
        package.check_shared_linux(library, 183)
    elif args.platform.startswith("macos-"):
        package.check_macho(library, args.platform.removeprefix("macos-"), True)
    else:
        package.check_windows(library, dll=True)
    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    out = Path(args.out) if Path(args.out).is_absolute() else REPO / args.out
    out.mkdir(parents=True, exist_ok=True)
    path = build(library, args.platform, version, out, package.documents(version, package.REPOSITORY_URL, "core"),
                 package.fixed_time(), package.REPOSITORY_URL)
    print(f"{path}  {path.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
