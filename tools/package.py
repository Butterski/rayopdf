#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build RayoPDF's release archives from already-built binaries, reproducibly.

The layout mirrors RayoMD's releases: one archive per platform, holding one folder of the
archive's name with the binary, README.md (the user part of the repository's README: status,
usage and licence, with links made absolute), LICENSE, NOTICE, VERSION and third_party/licenses/.
Every entry has the same fixed time (SOURCE_DATE_EPOCH, else the time of the HEAD commit), owner
0 and fixed permissions, and the entries are sorted, so the same binaries give the same bytes.
SHA256SUMS lists the archives' SHA-256 in `sha256sum -c` form.

  rayopdf-VERSION-linux-x64-static.tar.gz   build/static/rayopdf (fully static, -static-pie)
  rayopdf-VERSION-windows-x64.zip           build/windows/rayopdf.exe (MinGW-w64, static)

With --ocr (or --only ocr-linux|ocr-windows), the OCR add-on's archives as well. Their folder has
the core archive's name, so unpacking one over the other puts rayopdf-ocr beside rayopdf, where
the core finds it; it holds the add-on (the ncnn build), rayopdf-ocr-models/<tier>/ (the default
tier, or --ocr-tiers: config.txt, dict.txt, the ncnn network files, and a models.txt listing just
those) and the same documents:

  rayopdf-ocr-VERSION-linux-x64-static.tar.gz   build/ocr/addon/rayopdf-ocr (static-pie)
  rayopdf-ocr-VERSION-windows-x64.zip           build/ocr-windows/addon/rayopdf-ocr.exe

Build the binaries first:
  python3 tools/dev.py build --config static
  python3 tools/dev.py --image rayopdf-build:2 build --config windows
  python3 tools/dev.py build --config ocr --target rayopdf-ocr        # in rayopdf-ocr:1
  python3 tools/dev.py build --config ocr-windows --target rayopdf-ocr
  python3 tools/dev.py --image rayopdf-ocr:1 run -- sh -c 'rm -rf build/ocr-models && cp -r /opt/ocr/models build/ocr-models'

Usage:
  python3 tools/package.py [--linux build/static/rayopdf] [--windows build/windows/rayopdf.exe]
                           [--out build/dist] [--only linux|windows|ocr-linux|ocr-windows]
                           [--ocr] [--ocr-linux build/ocr/addon/rayopdf-ocr]
                           [--ocr-windows build/ocr-windows/addon/rayopdf-ocr.exe]
                           [--ocr-models build/ocr-models] [--ocr-tiers small]
                           [--repository URL]
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
import re
import struct
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

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
    """The title, its lead line and README_SECTIONS of README.md, relative links made absolute."""
    text = (REPO / "README.md").read_text(encoding="utf-8")
    parts = re.split(r"(?m)^(?=## )", text)
    head = parts[0]
    kept = [p for p in parts[1:] if p.splitlines()[0][3:].strip() in README_SECTIONS]
    missing = set(README_SECTIONS) - {p.splitlines()[0][3:].strip() for p in kept}
    if missing:
        raise SystemExit(f"package: README.md has no section {sorted(missing)}")
    body = head + "".join(kept)

    def absolute(match: re.Match) -> str:
        target = match.group(2)
        # Web links, anchors and the files the archive holds itself stay as they are.
        if re.match(r"^[a-z][a-z0-9+.-]*:|^#", target) or target in ("LICENSE", "NOTICE", "VERSION") or \
                target.startswith("third_party/licenses/"):
            return match.group(0)
        return f"{match.group(1)}({repository}/blob/v{version}/{target})"

    body = re.sub(r"(\]\s*)\(([^)\s]+)\)", absolute, body)
    note = f"\nThis is the README of the RayoPDF {version} release; the full one is at {repository}.\n"
    return (body.rstrip("\n") + "\n" + note).encode("utf-8")


def documents(version: str, repository: str) -> list[tuple[str, bytes, int]]:
    """(name in the folder, bytes, mode) of everything beside the binary."""
    files = [("README.md", readme_excerpt(version, repository), 0o644)]
    for name in ("LICENSE", "NOTICE", "VERSION"):
        files.append((name, (REPO / name).read_bytes(), 0o644))
    for path in sorted((REPO / "third_party" / "licenses").iterdir()):
        if path.is_file():
            files.append((f"third_party/licenses/{path.name}", path.read_bytes(), 0o644))
    return files


def check_linux(binary: Path, version: str, program: str = "rayopdf") -> None:
    data = binary.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise SystemExit(f"package: {binary} is not a 64-bit little-endian ELF file")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)
    if e_machine != 62:
        raise SystemExit(f"package: {binary} is not an x86-64 binary")
    # A static-pie binary keeps PT_DYNAMIC for its own relocations, but has no PT_INTERP.
    for i in range(e_phnum):
        if struct.unpack_from("<I", data, e_phoff + i * e_phentsize)[0] == 3:  # PT_INTERP
            raise SystemExit(f"package: {binary} has an interpreter: build it with --config static")
    if e_type != 3:
        raise SystemExit(f"package: {binary} is not position-independent (-static-pie)")
    if sys.platform.startswith("linux") and os.uname().machine == "x86_64":
        out = subprocess.run([str(binary), "--version"], capture_output=True, text=True).stdout.strip()
        # rayopdf-ocr appends its engine in parentheses: "rayopdf-ocr 0.0.9 (pp-ocrv6 small, ...)".
        if out.split(" (")[0] != f"{program} {version}":
            raise SystemExit(f"package: {binary} --version says {out!r}, VERSION says {version}")


# Network files of the add-on's runtime in a tier folder (besides config.txt and dict.txt). Releases
# ship the ncnn build (docs/development/ocr-engine.md); the ONNX Runtime variant is not packaged.
OCR_RUNTIME_FILES = ("det.ncnn.param", "det.ncnn.bin", "rec.ncnn.param", "rec.ncnn.bin")


def ocr_models(folder: Path, tiers: list[str]) -> list[tuple[str, bytes, int]]:
    """rayopdf-ocr-models/<tier>/ entries: the runtime's files, checked against and listed in models.txt."""
    entries = []
    for tier in tiers:
        manifest = {}
        for line in (folder / tier / "models.txt").read_text(encoding="ascii").splitlines():
            digest, size, name = line.split()
            manifest[name] = (digest, int(size))
        kept = []
        for name in ("config.txt", "dict.txt") + OCR_RUNTIME_FILES:
            data = (folder / tier / name).read_bytes()
            digest, size = manifest[name]
            if hashlib.sha256(data).hexdigest() != digest or len(data) != size:
                raise SystemExit(f"package: {folder / tier / name} differs from its models.txt")
            entries.append((f"rayopdf-ocr-models/{tier}/{name}", data, 0o644))
            kept.append(f"{digest} {size} {name}\n")
        entries.append((f"rayopdf-ocr-models/{tier}/models.txt", "".join(kept).encode("ascii"), 0o644))
    return entries


def check_windows(binary: Path) -> None:
    data = binary.read_bytes()
    if data[:2] != b"MZ":
        raise SystemExit(f"package: {binary} is not a Windows executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 4)[0] != 0x8664:
        raise SystemExit(f"package: {binary} is not an x86-64 PE executable")
    # Only system DLLs: the MinGW runtime (libstdc++, libgcc, winpthread) must be linked in.
    for dll in (b"libstdc++-6.dll", b"libgcc_s_seh-1.dll", b"libwinpthread-1.dll"):
        if dll in data:
            raise SystemExit(f"package: {binary} imports {dll.decode()}: build it with --config windows")


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
    parser.add_argument("--windows", default="build/windows/rayopdf.exe", help="the windows-x64 binary")
    parser.add_argument("--out", default="build/dist", help="folder for the archives and SHA256SUMS")
    parser.add_argument("--only", choices=("linux", "windows", "ocr-linux", "ocr-windows"),
                        help="package one archive")
    parser.add_argument("--ocr", action="store_true", help="also package the OCR add-on's archives")
    parser.add_argument("--ocr-linux", default="build/ocr/addon/rayopdf-ocr", help="the linux add-on binary")
    parser.add_argument("--ocr-windows", default="build/ocr-windows/addon/rayopdf-ocr.exe", help="the windows add-on")
    parser.add_argument("--ocr-models", default="build/ocr-models", help="the model folder (/opt/ocr/models)")
    parser.add_argument("--ocr-tiers", default="small", help="comma-separated tiers to ship (default small)")
    parser.add_argument("--repository", default=REPOSITORY_URL, help="where the README's links point")
    args = parser.parse_args()

    version = (REPO / "VERSION").read_text(encoding="utf-8").strip()
    mtime = fixed_time()
    docs = documents(version, args.repository.rstrip("/"))
    out = (REPO / args.out) if not Path(args.out).is_absolute() else Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    made = []
    if args.only in (None, "linux"):
        binary = REPO / args.linux if not Path(args.linux).is_absolute() else Path(args.linux)
        check_linux(binary, version)
        name = f"rayopdf-{version}-linux-x64-static"
        tar_gz(out / f"{name}.tar.gz", name, [("rayopdf", binary.read_bytes(), 0o755)] + docs, mtime)
        made.append(out / f"{name}.tar.gz")
    if args.only in (None, "windows"):
        binary = REPO / args.windows if not Path(args.windows).is_absolute() else Path(args.windows)
        check_windows(binary)
        name = f"rayopdf-{version}-windows-x64"
        zip_archive(out / f"{name}.zip", name, [("rayopdf.exe", binary.read_bytes(), 0o755)] + docs, mtime)
        made.append(out / f"{name}.zip")
    wanted = {"ocr-linux", "ocr-windows"} if args.ocr and args.only is None else {args.only}
    if wanted & {"ocr-linux", "ocr-windows"}:
        folder = REPO / args.ocr_models if not Path(args.ocr_models).is_absolute() else Path(args.ocr_models)
        models = ocr_models(folder, [t for t in args.ocr_tiers.split(",") if t])
    if "ocr-linux" in wanted:
        binary = REPO / args.ocr_linux if not Path(args.ocr_linux).is_absolute() else Path(args.ocr_linux)
        check_linux(binary, version, "rayopdf-ocr")
        name = f"rayopdf-{version}-linux-x64-static"
        tar_gz(out / f"rayopdf-ocr-{version}-linux-x64-static.tar.gz", name,
               [("rayopdf-ocr", binary.read_bytes(), 0o755)] + models + docs, mtime)
        made.append(out / f"rayopdf-ocr-{version}-linux-x64-static.tar.gz")
    if "ocr-windows" in wanted:
        binary = REPO / args.ocr_windows if not Path(args.ocr_windows).is_absolute() else Path(args.ocr_windows)
        check_windows(binary)
        name = f"rayopdf-{version}-windows-x64"
        zip_archive(out / f"rayopdf-ocr-{version}-windows-x64.zip", name,
                    [("rayopdf-ocr.exe", binary.read_bytes(), 0o755)] + models + docs, mtime)
        made.append(out / f"rayopdf-ocr-{version}-windows-x64.zip")

    # SHA256SUMS covers every archive of this version in the folder, also one packaged earlier.
    archives = sorted(p for p in out.iterdir()
                      if p.name.startswith((f"rayopdf-{version}-", f"rayopdf-ocr-{version}-")) and
                      p.name.endswith((".tar.gz", ".zip")))
    sums = "".join(f"{sha256(p)}  {p.name}\n" for p in archives)
    (out / "SHA256SUMS").write_text(sums, encoding="ascii")
    for path in made:
        print(f"{path.relative_to(REPO) if path.is_relative_to(REPO) else path}  {path.stat().st_size} bytes")
    print(sums, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
