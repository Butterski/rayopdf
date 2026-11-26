#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build RayoPDF's release archives from already-built binaries, reproducibly.

The layout mirrors RayoMD's releases: one archive per platform, holding one folder of the
archive's name with the binary, README.md (the user part of the repository's README: status,
usage and licence, with links made absolute), LICENSE, NOTICE, VERSION and, under
third_party/licenses/, the licences of what that archive holds (LICENSES: the core's data tables,
its edge-row model's training data and zlib-ng's checksum kernels in the core archives, ncnn and
the models in the add-on's, the script pack's models in its own).
Every entry has the same fixed time (SOURCE_DATE_EPOCH, else the time of the HEAD commit), owner
0 and fixed permissions, and the entries are sorted, so the same binaries give the same bytes.
SHA256SUMS lists the archives' SHA-256 in `sha256sum -c` form.

  rayopdf-VERSION-linux-x64-static.tar.gz   build/static/rayopdf (fully static, -static-pie)
  rayopdf-VERSION-linux-arm64-static.tar.gz build/arm64/rayopdf (the same for aarch64, cross-built)
  rayopdf-VERSION-windows-x64.zip           build/windows/rayopdf.exe (MinGW-w64, static)

The C++ API's archives (--only lib-linux|lib-linux-arm64|lib-windows) hold, in a folder of the archive's name,
include/rayopdf/rayopdf.h, lib/librayopdf.a (every module the command line links, merged by the
build's rayopdf_bundle target), example.cpp and the core archives' documents and licences:

  rayopdf-lib-VERSION-linux-x64.tar.gz      build/static/librayopdf.a (GCC, x86-64 Linux)
  rayopdf-lib-VERSION-linux-arm64.tar.gz    build/arm64/librayopdf.a (GCC, aarch64 Linux)
  rayopdf-lib-VERSION-windows-x64.zip       build/windows/librayopdf.a (MinGW-w64)

The Python package's wheels (--only wheel-linux|wheel-linux-arm64|wheel-windows; tools/build_wheel.py writes them):
python/rayopdf with the C ABI's shared library in rayopdf/_lib/, the core archives' documents
in the .dist-info folder:

  rayopdf-VERSION-py3-none-manylinux_2_28_x86_64.whl    build/wheel/librayopdf.so (glibc <= 2.28)
  rayopdf-VERSION-py3-none-manylinux_2_28_aarch64.whl   build/wheel-arm64/librayopdf.so (the same, built
                                                        natively on an arm64 machine)
  rayopdf-VERSION-py3-none-win_amd64.whl                build/windows/rayopdf.dll (MinGW-w64)

The npm package (--only npm), in npm pack's layout (one folder package/): packaging/npm's
package.json (its version set from VERSION), rayopdf.js and rayopdf.d.ts, the WebAssembly module
and the core archives' documents:

  rayopdf-VERSION.tgz                       build/wasm/rayopdf-core.mjs and rayopdf-core.wasm

With --ocr (or --only ocr-linux|ocr-windows), the OCR add-on's archives as well. Their folder has
the core archive's name, so unpacking one over the other puts rayopdf-ocr beside rayopdf, where
the core finds it; it holds the add-on, rayopdf-ocr-models/<tier>/ (the default tier and the tiny
first pass, or --ocr-tiers: config.txt, dict.txt, the ncnn network files, and a models.txt listing
just those), rayopdf-ocr-models/orient/ (the text-line orientation classifier),
the same documents and the add-on's licences:

  rayopdf-ocr-VERSION-linux-x64-static.tar.gz   build/ocr/addon/rayopdf-ocr (static-pie)
  rayopdf-ocr-VERSION-windows-x64.zip           build/ocr-windows/addon/rayopdf-ocr.exe

and the optional script pack (or --only ocr-scripts-linux|ocr-scripts-windows): the recognisers of
rayopdf-ocr-models/scripts/<script>/ (Cyrillic, Hangul, Arabic, Devanagari, Kannada, Tamil), in
the same folder again, so that it unpacks over the add-on; the add-on loads them on first use:

  rayopdf-ocr-scripts-VERSION-linux-x64-static.tar.gz
  rayopdf-ocr-scripts-VERSION-windows-x64.zip

Build the binaries first:
  python3 tools/dev.py build --config static
  python3 tools/dev.py build --config arm64                           # in rayopdf-cross:1
  python3 tools/dev.py --image rayopdf-build:2 build --config windows
  python3 tools/dev.py build --config wheel                           # in rayopdf-wheel:1
  python3 tools/dev.py build --config wasm                            # in rayopdf-wasm:1
  python3 tools/dev.py build --config ocr --target rayopdf-ocr        # in rayopdf-ocr:2
  python3 tools/dev.py build --config ocr-windows --target rayopdf-ocr
  python3 tools/dev.py --image rayopdf-ocr:2 run -- sh -c 'rm -rf build/ocr-models && cp -r /opt/ocr/models build/ocr-models'

Usage:
  python3 tools/package.py [--linux build/static/rayopdf] [--linux-arm64 build/arm64/rayopdf]
                           [--windows build/windows/rayopdf.exe]
                           [--lib-linux build/static/librayopdf.a] [--lib-linux-arm64 build/arm64/librayopdf.a]
                           [--lib-windows build/windows/librayopdf.a]
                           [--wheel-linux build/wheel/librayopdf.so] [--wheel-linux-arm64 build/wheel-arm64/librayopdf.so]
                           [--wheel-windows build/windows/rayopdf.dll]
                           [--npm build/wasm/rayopdf-core.mjs] [--out build/dist]
                           [--only linux|linux-arm64|windows|lib-linux|lib-linux-arm64|lib-windows|wheel-linux|
                                   wheel-linux-arm64|wheel-windows|npm|ocr-linux|ocr-windows|ocr-scripts-linux|
                                   ocr-scripts-windows]
                           [--ocr] [--ocr-linux build/ocr/addon/rayopdf-ocr]
                           [--ocr-windows build/ocr-windows/addon/rayopdf-ocr.exe]
                           [--ocr-models build/ocr-models] [--ocr-tiers tiny,small]
                           [--repository URL]
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

import build_wheel  # tools/build_wheel.py, beside this file

REPO = Path(__file__).resolve().parent.parent
REPOSITORY_URL = "https://github.com/Butterski/rayopdf"
# The sections of README.md a release carries; building and evaluating need the repository.
README_SECTIONS = ("Status", "Usage", "License")


def fixed_time() -> int:
    if os.environ.get("SOURCE_DATE_EPOCH"):
        return int(os.environ["SOURCE_DATE_EPOCH"])
    try:
        out = subprocess.run(["git", "-C", str(REPO), "log", "-1", "--format=%ct"], capture_output=True,
                             text=True, check=True).stdout.strip()
        return int(out)
    except (OSError, subprocess.CalledProcessError, ValueError):
        raise SystemExit("package: set SOURCE_DATE_EPOCH (no git commit to take the time from)")


def readme_excerpt(version: str, repository: str) -> bytes:
    """The title, its lead line and README_SECTIONS of README.md, relative links made absolute:
    Markdown links and HTML href attributes to the tagged file on the repository, HTML src
    attributes (the mascot) to its raw bytes, so that the page shows them on PyPI too."""
    text = (REPO / "README.md").read_text(encoding="utf-8")
    parts = re.split(r"(?m)^(?=## )", text)
    head = parts[0]
    kept = [p for p in parts[1:] if p.splitlines()[0][3:].strip() in README_SECTIONS]
    missing = set(README_SECTIONS) - {p.splitlines()[0][3:].strip() for p in kept}
    if missing:
        raise SystemExit(f"package: README.md has no section {sorted(missing)}")
    body = head + "".join(kept)

    def kept_as_is(target: str) -> bool:
        # Web links, anchors and the files the archive holds itself stay as they are.
        return bool(re.match(r"^[a-z][a-z0-9+.-]*:|^#", target)) or target in ("LICENSE", "NOTICE", "VERSION") or \
            target.startswith("third_party/licenses/")

    def absolute(match: re.Match) -> str:
        target = match.group(2)
        if kept_as_is(target):
            return match.group(0)
        return f"{match.group(1)}({repository}/blob/v{version}/{target})"

    def attribute(match: re.Match) -> str:
        name, target = match.group(1), match.group(2)
        if kept_as_is(target):
            return match.group(0)
        if name == "href":
            return f'href="{repository}/blob/v{version}/{target}"'
        github = "https://github.com/"
        if repository.startswith(github):
            return f'src="https://raw.githubusercontent.com/{repository[len(github):]}/v{version}/{target}"'
        return f'src="{repository}/raw/v{version}/{target}"'

    body = re.sub(r"(\]\s*)\(([^)\s]+)\)", absolute, body)
    body = re.sub(r'\b(src|href)="([^"\s]+)"', attribute, body)
    note = f"\nThis is the README of the RayoPDF {version} release; the full one is at {repository}.\n"
    return (body.rstrip("\n") + "\n" + note).encode("utf-8")


# third_party/licenses/ by the archive that holds the licensed material (NOTICE names each):
# rayopdf carries the data tables generated into src/text and src/layout (among them the edge-row
# model of src/layout/model_data.cpp, trained on DocLayNet); rayopdf-ocr links ncnn
# and none of those tables, and ships the PP-OCRv6 tiers and the orientation classifier; the
# script pack holds PaddleOCR recognisers only.
LICENSES = {
    "core": ("agl-aglfn.txt", "cmap-resources.txt", "doclaynet.txt", "katex.txt", "unicode.txt", "zlib-ng.txt"),
    "ocr": ("ncnn.txt", "paddleocr-scripts.txt", "pp-ocrv6.txt"),
    "ocr-scripts": ("paddleocr-scripts.txt",),
}


def documents(version: str, repository: str, part: str) -> list[tuple[str, bytes, int]]:
    """(name in the folder, bytes, mode) of everything beside the binary and the models of an archive
    of `part` (a key of LICENSES)."""
    folder = REPO / "third_party" / "licenses"
    unassigned = {p.name for p in folder.iterdir() if p.is_file()} - {n for names in LICENSES.values() for n in names}
    if unassigned:
        raise SystemExit(f"package: {sorted(unassigned)} under third_party/licenses/ belong to no archive (LICENSES)")
    files = [("README.md", readme_excerpt(version, repository), 0o644)]
    for name in ("LICENSE", "NOTICE", "VERSION"):
        files.append((name, (REPO / name).read_bytes(), 0o644))
    for name in LICENSES[part]:
        files.append((f"third_party/licenses/{name}", (folder / name).read_bytes(), 0o644))
    return files


# ELF e_machine of the Linux archives: (name, `uname -m` of a host that runs it, the config that builds it).
ELF_MACHINES = {62: ("x86-64", "x86_64", "static"), 183: ("aarch64", "aarch64", "arm64")}


def check_linux(binary: Path, version: str, program: str = "rayopdf", machine: int = 62) -> None:
    name, host, config = ELF_MACHINES[machine]
    data = binary.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit(f"package: {binary} is not a 64-bit little-endian ELF file")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    if e_machine != machine:
        raise SystemExit(f"package: {binary} is not an {name} binary")
    # A static-pie binary keeps PT_DYNAMIC for its own relocations, but has no PT_INTERP.
    for i in range(e_phnum):
        if struct.unpack_from("<I", data, e_phoff + i * e_phentsize)[0] == 3:  # PT_INTERP
            raise SystemExit(f"package: {binary} has an interpreter: build it with --config {config}")
    if e_type != 3:
        raise SystemExit(f"package: {binary} is not position-independent (-static-pie)")
    if sys.platform.startswith("linux") and os.uname().machine == host:
        out = subprocess.run([str(binary), "--version"], capture_output=True, text=True).stdout.strip()
        # rayopdf-ocr appends its engine in parentheses: "rayopdf-ocr 0.0.9 (pp-ocrv6 small, ...)".
        if out.split(" (")[0] != f"{program} {version}":
            raise SystemExit(f"package: {binary} --version says {out!r}, VERSION says {version}")


# Network files of the add-on's runtime (ncnn) in a tier folder (besides config.txt and dict.txt).
OCR_RUNTIME_FILES = ("det.ncnn.param", "det.ncnn.bin", "rec.ncnn.param", "rec.ncnn.bin")
# The orientation classifier's and the script pack's network files.
OCR_ORIENT_FILES = ("cls.ncnn.param", "cls.ncnn.bin")
OCR_SCRIPT_FILES = ("rec.ncnn.param", "rec.ncnn.bin")


def ocr_folder(folder: Path, sub: str, files: tuple) -> list[tuple[str, bytes, int]]:
    """rayopdf-ocr-models/<sub>/ entries: config.txt, dict.txt and `files`, checked against and listed
    in models.txt."""
    manifest = {}
    for line in (folder / sub / "models.txt").read_text(encoding="ascii").splitlines():
        digest, size, name = line.split()
        manifest[name] = (digest, int(size))
    entries, kept = [], []
    for name in ("config.txt", "dict.txt") + files:
        data = (folder / sub / name).read_bytes()
        digest, size = manifest[name]
        if hashlib.sha256(data).hexdigest() != digest or len(data) != size:
            raise SystemExit(f"package: {folder / sub / name} differs from its models.txt")
        entries.append((f"rayopdf-ocr-models/{sub}/{name}", data, 0o644))
        kept.append(f"{digest} {size} {name}\n")
    entries.append((f"rayopdf-ocr-models/{sub}/models.txt", "".join(kept).encode("ascii"), 0o644))
    return entries


def ocr_models(folder: Path, tiers: list[str]) -> list[tuple[str, bytes, int]]:
    """The add-on's models: the tiers' folders and the orientation classifier's."""
    entries = []
    for tier in tiers:
        entries += ocr_folder(folder, tier, OCR_RUNTIME_FILES)
    return entries + ocr_folder(folder, "orient", OCR_ORIENT_FILES)


def ocr_scripts(folder: Path) -> list[tuple[str, bytes, int]]:
    """The script pack: every recogniser folder of rayopdf-ocr-models/scripts/."""
    entries = []
    for sub in sorted(p.name for p in (folder / "scripts").iterdir() if p.is_dir()):
        entries += ocr_folder(folder, f"scripts/{sub}", OCR_SCRIPT_FILES)
    if not entries:
        raise SystemExit(f"package: {folder / 'scripts'} holds no recogniser")
    return entries


def check_windows(binary: Path, dll: bool = False) -> None:
    data = binary.read_bytes()
    if data[:2] != b"MZ":
        raise SystemExit(f"package: {binary} is not a Windows executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 4)[0] != 0x8664:
        raise SystemExit(f"package: {binary} is not an x86-64 PE executable")
    if dll != bool(struct.unpack_from("<H", data, pe + 22)[0] & 0x2000):  # IMAGE_FILE_DLL
        raise SystemExit(f"package: {binary} is {'not ' if dll else ''}a DLL")
    # Only system DLLs: the MinGW runtime (libstdc++, libgcc, winpthread) must be linked in.
    for runtime in (b"libstdc++-6.dll", b"libgcc_s_seh-1.dll", b"libwinpthread-1.dll"):
        if runtime in data:
            raise SystemExit(f"package: {binary} imports {runtime.decode()}: build it with --config windows")


# The manylinux_2_28 wheel's promise: no glibc symbol version newer than this.
MAX_GLIBC = (2, 28)


def check_shared_linux(library: Path, machine: int = 62) -> None:
    """librayopdf.so: an ELF shared object of `machine` needing no glibc newer than MAX_GLIBC and no
    libstdc++ or libgcc_s (both linked in)."""
    data = library.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit(f"package: {library} is not a 64-bit little-endian ELF file")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != 3 or e_machine != machine:
        raise SystemExit(f"package: {library} is not an {ELF_MACHINES[machine][0]} shared object")
    versions = {(int(a), int(b)) for a, b in re.findall(rb"GLIBC_(\d+)\.(\d+)", data)}
    if not versions or max(versions) > MAX_GLIBC:
        raise SystemExit(f"package: {library} needs glibc {max(versions, default=None)}: build it with --config wheel")
    for name in (b"libstdc++.so", b"libgcc_s.so"):
        if name in data:
            raise SystemExit(f"package: {library} needs {name.decode()}: build it with --config wheel")


def check_library(library: Path, windows: bool, machine: int = 62) -> None:
    """A static library whose objects are ELF of `machine` (Linux) or x86-64 COFF (MinGW-w64)."""
    data = library.read_bytes()
    if data[:8] != b"!<arch>\n":
        raise SystemExit(f"package: {library} is not a static library (ar archive)")
    at = 8
    while at + 60 <= len(data):
        name = data[at:at + 16].rstrip()
        size = int(data[at + 48:at + 58])
        member = data[at + 60:at + 60 + size]
        if name not in (b"/", b"//", b"/SYM64/"):  # symbol and long-name tables
            if windows:
                ok = member[:2] == b"\x64\x86"
            else:
                ok = member[:4] == b"\x7fELF" and struct.unpack_from("<H", member, 18)[0] == machine
            if not ok:
                platform = "an x86-64 COFF" if windows else f"an {ELF_MACHINES[machine][0]} ELF"
                config = "windows" if windows else ELF_MACHINES[machine][2]
                raise SystemExit(f"package: {library} holds {name.decode(errors='replace')}, not {platform} object: "
                                 f"build it with --config {config}")
            return
        at += 60 + size + (size & 1)
    raise SystemExit(f"package: {library} holds no object")


def lib_entries(library: Path, docs: list[tuple[str, bytes, int]]) -> list[tuple[str, bytes, int]]:
    """The C++ API archive's entries: the public header, the library, the example and `docs`."""
    return [("include/rayopdf/rayopdf.h", (REPO / "include" / "rayopdf" / "rayopdf.h").read_bytes(), 0o644),
            ("lib/librayopdf.a", library.read_bytes(), 0o644),
            ("example.cpp", (REPO / "src" / "api" / "example" / "example.cpp").read_bytes(), 0o644)] + docs


def check_wasm(module: Path) -> None:
    """rayopdf-core.mjs and beside it rayopdf-core.wasm, a WebAssembly binary (magic \\0asm, version 1)."""
    wasm = module.with_suffix(".wasm")
    if not module.is_file() or b"rayopdf-core.wasm" not in module.read_bytes():
        raise SystemExit(f"package: {module} is not the module factory of rayopdf-core.wasm: build it with --config wasm")
    if wasm.read_bytes()[:8] != b"\0asm\x01\0\0\0":
        raise SystemExit(f"package: {wasm} is not a WebAssembly binary: build it with --config wasm")


def npm_entries(module: Path, version: str, docs: list[tuple[str, bytes, int]]) -> list[tuple[str, bytes, int]]:
    """The npm package's entries: packaging/npm's files (package.json with `version`), the module and `docs`."""
    folder = REPO / "packaging" / "npm"
    manifest = json.loads((folder / "package.json").read_text(encoding="utf-8"))
    manifest["version"] = version
    return [("package.json", (json.dumps(manifest, indent=2) + "\n").encode("utf-8"), 0o644),
            ("rayopdf.js", (folder / "rayopdf.js").read_bytes(), 0o644),
            ("rayopdf.d.ts", (folder / "rayopdf.d.ts").read_bytes(), 0o644),
            ("rayopdf-core.mjs", module.read_bytes(), 0o644),
            ("rayopdf-core.wasm", module.with_suffix(".wasm").read_bytes(), 0o644)] + docs


def tar_gz(path: Path, folder: str, entries: list[tuple[str, bytes, int]], mtime: int) -> None:
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        names = sorted({folder} | {f"{folder}/{n}" for n, _, _ in entries} |
                       {f"{folder}/{p}" for n, _, _ in entries for p in parents(n)})
        content = {f"{folder}/{n}": (data, mode) for n, data, mode in entries}
        for name in names:
            info = tarfile.TarInfo(name)
            info.mtime = mtime
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if name in content:
                data, mode = content[name]
                info.size = len(data)
                info.mode = mode
                tar.addfile(info, io.BytesIO(data))
            else:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                tar.addfile(info)
    with open(path, "wb") as out:
        with gzip.GzipFile(filename="", mode="wb", fileobj=out, compresslevel=9, mtime=mtime) as gz:
            gz.write(raw.getvalue())


def zip_archive(path: Path, folder: str, entries: list[tuple[str, bytes, int]], mtime: int) -> None:
    stamp = time.gmtime(max(mtime, 315532800))[:6]  # a zip time starts in 1980
    names = sorted({f"{folder}/"} | {f"{folder}/{p}/" for n, _, _ in entries for p in parents(n)} |
                   {f"{folder}/{n}" for n, _, _ in entries})
    content = {f"{folder}/{n}": (data, mode) for n, data, mode in entries}
    with zipfile.ZipFile(path, "w") as archive:
        for name in names:
            info = zipfile.ZipInfo(name, date_time=stamp)
            info.create_system = 3
            if name in content:
                data, mode = content[name]
                info.external_attr = (0o100000 | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data, compresslevel=9)
            else:
                info.external_attr = (0o040755 << 16) | 0x10
                archive.writestr(info, b"")


def parents(name: str) -> list[str]:
    parts = name.split("/")[:-1]
    return ["/".join(parts[:i + 1]) for i in range(len(parts))]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--linux", default="build/static/rayopdf", help="the linux-x64-static binary")
    parser.add_argument("--linux-arm64", default="build/arm64/rayopdf", help="the linux-arm64-static binary")
    parser.add_argument("--windows", default="build/windows/rayopdf.exe", help="the windows-x64 binary")
    parser.add_argument("--out", default="build/dist", help="folder for the archives and SHA256SUMS")
    parser.add_argument("--only", choices=("linux", "linux-arm64", "windows", "lib-linux", "lib-linux-arm64",
                                           "lib-windows", "wheel-linux", "wheel-linux-arm64", "wheel-windows", "npm",
                                           "ocr-linux", "ocr-windows", "ocr-scripts-linux", "ocr-scripts-windows"),
                        help="package one archive")
    parser.add_argument("--lib-linux", default="build/static/librayopdf.a", help="the linux-x64 C++ API library")
    parser.add_argument("--lib-linux-arm64", default="build/arm64/librayopdf.a",
                        help="the linux-arm64 C++ API library")
    parser.add_argument("--lib-windows", default="build/windows/librayopdf.a", help="the windows-x64 C++ API library")
    parser.add_argument("--wheel-linux", default="build/wheel/librayopdf.so",
                        help="the manylinux_2_28 wheel's shared library (--config wheel)")
    parser.add_argument("--wheel-linux-arm64", default="build/wheel-arm64/librayopdf.so",
                        help="the manylinux_2_28_aarch64 wheel's shared library (--config wheel on arm64)")
    parser.add_argument("--wheel-windows", default="build/windows/rayopdf.dll", help="the win_amd64 wheel's DLL")
    parser.add_argument("--npm", default="build/wasm/rayopdf-core.mjs",
                        help="the npm package's WebAssembly module (rayopdf-core.wasm beside it; --config wasm)")
    parser.add_argument("--ocr", action="store_true", help="also package the OCR add-on's archives and script pack")
    parser.add_argument("--ocr-linux", default="build/ocr/addon/rayopdf-ocr", help="the linux add-on binary")
    parser.add_argument("--ocr-windows", default="build/ocr-windows/addon/rayopdf-ocr.exe", help="the windows add-on")
    parser.add_argument("--ocr-models", default="build/ocr-models", help="the model folder (/opt/ocr/models)")
    parser.add_argument("--ocr-tiers", default="tiny,small",
                        help="comma-separated tiers to ship (default tiny,small: small with its tiny first pass)")
    parser.add_argument("--repository", default=REPOSITORY_URL, help="where the README's links point")
    args = parser.parse_args()

    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    mtime = fixed_time()
    docs = {part: documents(version, args.repository.rstrip("/"), part) for part in LICENSES}
    out = (REPO / args.out) if not Path(args.out).is_absolute() else Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    made = []
    for part, arch, given, machine in (("linux", "x64", args.linux, 62), ("linux-arm64", "arm64", args.linux_arm64, 183)):
        if args.only in (None, part):
            binary = REPO / given if not Path(given).is_absolute() else Path(given)
            check_linux(binary, version, machine=machine)
            name = f"rayopdf-{version}-linux-{arch}-static"
            tar_gz(out / f"{name}.tar.gz", name, [("rayopdf", binary.read_bytes(), 0o755)] + docs["core"], mtime)
            made.append(out / f"{name}.tar.gz")
    if args.only in (None, "windows"):
        binary = REPO / args.windows if not Path(args.windows).is_absolute() else Path(args.windows)
        check_windows(binary)
        name = f"rayopdf-{version}-windows-x64"
        zip_archive(out / f"{name}.zip", name, [("rayopdf.exe", binary.read_bytes(), 0o755)] + docs["core"], mtime)
        made.append(out / f"{name}.zip")
    for part, arch, given, machine in (("lib-linux", "x64", args.lib_linux, 62),
                                       ("lib-linux-arm64", "arm64", args.lib_linux_arm64, 183)):
        if args.only in (None, part):
            library = REPO / given if not Path(given).is_absolute() else Path(given)
            check_library(library, False, machine)
            name = f"rayopdf-lib-{version}-linux-{arch}"
            tar_gz(out / f"{name}.tar.gz", name, lib_entries(library, docs["core"]), mtime)
            made.append(out / f"{name}.tar.gz")
    if args.only in (None, "lib-windows"):
        library = REPO / args.lib_windows if not Path(args.lib_windows).is_absolute() else Path(args.lib_windows)
        check_library(library, True)
        name = f"rayopdf-lib-{version}-windows-x64"
        zip_archive(out / f"{name}.zip", name, lib_entries(library, docs["core"]), mtime)
        made.append(out / f"{name}.zip")
    for part, platform, given in (("wheel-linux", "linux", args.wheel_linux),
                                  ("wheel-linux-arm64", "linux-arm64", args.wheel_linux_arm64),
                                  ("wheel-windows", "windows", args.wheel_windows)):
        if args.only in (None, part):
            library = REPO / given if not Path(given).is_absolute() else Path(given)
            if platform == "linux":
                check_shared_linux(library)
            elif platform == "linux-arm64":
                check_shared_linux(library, 183)
            else:
                check_windows(library, dll=True)
            made.append(build_wheel.build(library, platform, version, out, docs["core"], mtime,
                                          args.repository.rstrip("/")))
    if args.only in (None, "npm"):
        module = REPO / args.npm if not Path(args.npm).is_absolute() else Path(args.npm)
        check_wasm(module)
        tar_gz(out / f"rayopdf-{version}.tgz", "package", npm_entries(module, version, docs["core"]), mtime)
        made.append(out / f"rayopdf-{version}.tgz")
    ocr = {"ocr-linux", "ocr-windows", "ocr-scripts-linux", "ocr-scripts-windows"}
    wanted = ocr if args.ocr and args.only is None else {args.only}
    if wanted & ocr:
        folder = REPO / args.ocr_models if not Path(args.ocr_models).is_absolute() else Path(args.ocr_models)
    if wanted & {"ocr-linux", "ocr-windows"}:
        models = ocr_models(folder, [t for t in args.ocr_tiers.split(",") if t])
    if wanted & {"ocr-scripts-linux", "ocr-scripts-windows"}:
        scripts = ocr_scripts(folder)
    if "ocr-linux" in wanted:
        binary = REPO / args.ocr_linux if not Path(args.ocr_linux).is_absolute() else Path(args.ocr_linux)
        check_linux(binary, version, "rayopdf-ocr")
        name = f"rayopdf-{version}-linux-x64-static"
        tar_gz(out / f"rayopdf-ocr-{version}-linux-x64-static.tar.gz", name,
               [("rayopdf-ocr", binary.read_bytes(), 0o755)] + models + docs["ocr"], mtime)
        made.append(out / f"rayopdf-ocr-{version}-linux-x64-static.tar.gz")
    if "ocr-windows" in wanted:
        binary = REPO / args.ocr_windows if not Path(args.ocr_windows).is_absolute() else Path(args.ocr_windows)
        check_windows(binary)
        name = f"rayopdf-{version}-windows-x64"
        zip_archive(out / f"rayopdf-ocr-{version}-windows-x64.zip", name,
                    [("rayopdf-ocr.exe", binary.read_bytes(), 0o755)] + models + docs["ocr"], mtime)
        made.append(out / f"rayopdf-ocr-{version}-windows-x64.zip")
    if "ocr-scripts-linux" in wanted:
        tar_gz(out / f"rayopdf-ocr-scripts-{version}-linux-x64-static.tar.gz", f"rayopdf-{version}-linux-x64-static",
               scripts + docs["ocr-scripts"], mtime)
        made.append(out / f"rayopdf-ocr-scripts-{version}-linux-x64-static.tar.gz")
    if "ocr-scripts-windows" in wanted:
        zip_archive(out / f"rayopdf-ocr-scripts-{version}-windows-x64.zip", f"rayopdf-{version}-windows-x64",
                    scripts + docs["ocr-scripts"], mtime)
        made.append(out / f"rayopdf-ocr-scripts-{version}-windows-x64.zip")

    # SHA256SUMS covers every archive of this version in the folder, also one packaged earlier.
    archives = sorted(p for p in out.iterdir()
                      if (p.name.startswith((f"rayopdf-{version}-", f"rayopdf-lib-{version}-", f"rayopdf-ocr-{version}-",
                                             f"rayopdf-ocr-scripts-{version}-")) and
                          p.name.endswith((".tar.gz", ".zip", ".whl"))) or p.name == f"rayopdf-{version}.tgz")
    sums = "".join(f"{sha256(p)}  {p.name}\n" for p in archives)
    (out / "SHA256SUMS").write_text(sums, encoding="ascii")
    for path in made:
        print(f"{path.relative_to(REPO) if path.is_relative_to(REPO) else path}  {path.stat().st_size} bytes")
    print(sums, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
