#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and test RayoPDF inside the rayopdf-build image (docker/build.Dockerfile).

Nothing is installed on the host: the compilers, CMake and Ninja live in the image, the
repository is mounted at /src, and builds go to build/<name>/ (git-ignored).

Usage:
  python3 tools/dev.py [--image TAG] image [--name build|cross|ocr|wheel|wasm]  # build an image (default: build)
  python3 tools/dev.py [--image TAG] build [--config CONFIG] [--dir NAME] [--target T ...]
  python3 tools/dev.py [--image TAG] test [--config CONFIG] [--dir NAME] [-- CTEST ARGS]
  python3 tools/dev.py [--image TAG] run [--mount HOST:CTR] -- COMMAND ...

Configs: release (GCC, -O2), debug (GCC, -O0 -g), asan (Clang, ASan + UBSan, -O1 -g),
tsan (Clang, ThreadSanitizer, -O1 -g), static (GCC release, fully static -static-pie binary for
the evaluation harness's `cli` tool and the linux-x64-static release), fuzz (Clang, libFuzzer
targets of tests/fuzz), windows (MinGW-w64 GCC release, a static rayopdf.exe for the windows-x64
release and rayopdf.dll, the C ABI's shared library, for the win_amd64 wheel; `test` runs the
core tests, the CLI verifier and tests/c_api_smoke.c under Wine), wheel (GCC release in the
rayopdf-wheel:1 image, docker/wheel.Dockerfile: manylinux_2_28 of the machine's architecture,
x86_64 or aarch64, so librayopdf.so needs no glibc newer than 2.28; tests including
tests/verify_python.py), wasm (Emscripten release in the
rayopdf-wasm:1 image, docker/wasm.Dockerfile, configured through emcmake: rayopdf-core.mjs and
rayopdf-core.wasm for the npm package; `test` runs tests/verify_wasm.mjs under Node.js), arm64
(GCC 13.3 for Linux aarch64 in the rayopdf-cross:1 image, docker/cross.Dockerfile: the fully
static -static-pie binary of the linux-arm64-static release and librayopdf.a of its lib archive;
`test` runs the core tests and the CLI verifier under qemu-aarch64-static, except the two tests
that start the stand-in OCR add-on, layout_ocr and layout_glyph: a process under qemu-user can
start no aarch64 program; they run natively on an aarch64 machine), and the OCR add-on's configs,
which run in the rayopdf-ocr:2 image (docker/ocr.Dockerfile: the ncnn runtime and the models)
unless --image says otherwise: ocr (static release with the add-on rayopdf-ocr, tests including
tests/verify_ocr.py with the image's models), ocr-windows (MinGW-w64 rayopdf-ocr.exe, tests under
Wine). --dir defaults to the config's name. Builds run with
--network none; RAYOPDF_DOCKER_CPUS (a cpuset such as 0-2,4-8) keeps the container off the CPUs a
timing run uses.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IMAGE = "rayopdf-build:2"
IMAGE_TAG = IMAGE  # --image, or the config's image
# Images `image --name` builds: Dockerfile under docker/ and tag.
IMAGES = {"build": ("build.Dockerfile", IMAGE), "cross": ("cross.Dockerfile", "rayopdf-cross:1"),
          "ocr": ("ocr.Dockerfile", "rayopdf-ocr:2"), "wheel": ("wheel.Dockerfile", "rayopdf-wheel:1"),
          "wasm": ("wasm.Dockerfile", "rayopdf-wasm:1")}
# Configs that build in another image than the build image.
CONFIG_IMAGES = {"arm64": "rayopdf-cross:1", "ocr": "rayopdf-ocr:2", "ocr-windows": "rayopdf-ocr:2",
                 "wheel": "rayopdf-wheel:1", "wasm": "rayopdf-wasm:1"}
# Configs whose configure step runs through a wrapper (Emscripten's emcmake: its toolchain file).
CONFIG_WRAPPERS = {"wasm": "emcmake"}
# ctest arguments of a config's `test`: under qemu-user the tests that start an add-on are left out.
CONFIG_CTEST = {"arm64": ["-E", "^layout_(ocr|glyph)$"]}
OCR_MODELS = "/opt/ocr/models"  # in rayopdf-ocr:2

# MinGW's linker writes the link time into the PE header unless told not to; without it two builds
# of one source differ in four bytes, and so would the release archives (tools/package.py).
PE_NO_TIMESTAMP = "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--no-insert-timestamp"
PE_DLL_NO_TIMESTAMP = "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-insert-timestamp"  # rayopdf.dll, the wheel's

CONFIGS = {
    "release": ["-DCMAKE_BUILD_TYPE=Release"],
    "debug": ["-DCMAKE_BUILD_TYPE=Debug"],
    "asan": ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_C_COMPILER=clang",
             "-DCMAKE_CXX_COMPILER=clang++", "-DRAYOPDF_SANITIZE=ON"],
    "static": ["-DCMAKE_BUILD_TYPE=Release", "-DRAYOPDF_STATIC=ON", "-DRAYOPDF_TESTS=OFF"],
    "fuzz": ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_C_COMPILER=clang",
             "-DCMAKE_CXX_COMPILER=clang++", "-DRAYOPDF_FUZZ=ON", "-DRAYOPDF_TESTS=OFF"],
    "tsan": ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_C_COMPILER=clang",
             "-DCMAKE_CXX_COMPILER=clang++", "-DRAYOPDF_TSAN=ON"],
    "windows": ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=Windows",
                "-DCMAKE_SYSTEM_PROCESSOR=x86_64", "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix",
                "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix",
                "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres", "-DCMAKE_CROSSCOMPILING_EMULATOR=wine",
                PE_NO_TIMESTAMP, PE_DLL_NO_TIMESTAMP, "-DRAYOPDF_SHARED=ON"],
    "wheel": ["-DCMAKE_BUILD_TYPE=Release", "-DRAYOPDF_SHARED=ON"],
    "wasm": ["-DCMAKE_BUILD_TYPE=Release"],
    "arm64": ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
              "-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc-13", "-DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-13",
              "-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-aarch64-static", "-DRAYOPDF_STATIC=ON"],
    "ocr": ["-DCMAKE_BUILD_TYPE=Release", "-DRAYOPDF_STATIC=ON", "-DRAYOPDF_OCR_ADDON=ON",
            "-DRAYOPDF_OCR_RUNTIME_DIR=/opt/ocr/ncnn/linux", f"-DRAYOPDF_OCR_MODELS={OCR_MODELS}"],
    "ocr-windows": ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=Windows",
                    "-DCMAKE_SYSTEM_PROCESSOR=x86_64", "-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix",
                    "-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix",
                    "-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres", "-DCMAKE_CROSSCOMPILING_EMULATOR=wine",
                    PE_NO_TIMESTAMP, "-DRAYOPDF_OCR_ADDON=ON",
                    "-DRAYOPDF_OCR_RUNTIME_DIR=/opt/ocr/ncnn/windows", f"-DRAYOPDF_OCR_MODELS={OCR_MODELS}"],
}


def docker(argv: list[str], mounts: list[str] | None = None, workdir: str = "/src", tsan: bool = False) -> int:
    cmd = ["docker", "run", "--rm", "--init", "-u", f"{os.getuid()}:{os.getgid()}",
           "-e", "HOME=/tmp", "-e", "LANG=C.UTF-8", "-e", "WINEDEBUG=-all", "--network", "none",
           "-v", f"{REPO}:/src", "-w", workdir]
    cpus = os.environ.get("RAYOPDF_DOCKER_CPUS")
    if cpus:
        cmd += ["--cpuset-cpus", cpus]
    if tsan:
        # ThreadSanitizer re-executes itself without address randomization (personality(2)), which
        # Docker's default seccomp profile refuses on kernels with high mmap entropy.
        cmd += ["--security-opt", "seccomp=unconfined"]
    for mount in mounts or []:
        host, _, ctr = mount.partition(":")
        cmd += ["-v", f"{Path(host).resolve()}:{ctr}"]
    cmd += [IMAGE_TAG] + argv
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def build_dir(args) -> str:
    return f"build/{args.dir or args.config}"


def cmd_image(args) -> int:
    dockerfile, tag = IMAGES[args.name]
    return subprocess.run(["docker", "build", "-f", str(REPO / "docker" / dockerfile),
                           "-t", IMAGE_TAG if args.image_given else tag, str(REPO / "docker")]).returncode


def configure_and_build(args) -> int:
    out = build_dir(args)
    configure = f"cmake -S . -B {out} -G Ninja {' '.join(CONFIGS[args.config])}"
    if args.config in CONFIG_WRAPPERS:
        configure = f"{CONFIG_WRAPPERS[args.config]} {configure}"
    script = f"{configure} >/dev/null && cmake --build {out}"
    if args.target:
        script += " --target " + " ".join(args.target)
    return docker(["sh", "-ec", script])


def cmd_build(args) -> int:
    return configure_and_build(args)


def cmd_test(args) -> int:
    code = configure_and_build(args)
    if code:
        return code
    extra = " ".join(shlex.quote(a) for a in CONFIG_CTEST.get(args.config, []) + (args.ctest or []))
    return docker(["sh", "-ec", f"ctest --test-dir {build_dir(args)} --output-on-failure {extra}"],
                  tsan=args.config == "tsan")


def cmd_run(args) -> int:
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        raise SystemExit("run needs a command after --")
    return docker(command, mounts=args.mount, tsan=args.tsan)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--image", help=f"image tag (default {IMAGE}, or the config's image)")
    sub = parser.add_subparsers(dest="command_name", required=True)
    p = sub.add_parser("image", help="build an image (tag: --image)")
    p.add_argument("--name", choices=sorted(IMAGES), default="build", help="which image (default build)")
    p.set_defaults(func=cmd_image)
    for name, func, text in (("build", cmd_build, "configure and build"),
                             ("test", cmd_test, "build, then run ctest")):
        p = sub.add_parser(name, help=text)
        p.add_argument("--config", choices=sorted(CONFIGS), default="release")
        p.add_argument("--dir", help="build directory name under build/ (default: the config)")
        p.add_argument("--target", nargs="+", help="build only these targets")
        if name == "test":
            p.add_argument("ctest", nargs=argparse.REMAINDER, help="-- extra ctest arguments")
        p.set_defaults(func=func)
    p = sub.add_parser("run", help="run a command in the build image (repository at /src)")
    p.add_argument("--mount", action="append", help="extra mount HOST:CONTAINER")
    p.add_argument("--tsan", action="store_true", help="let ThreadSanitizer binaries run (seccomp unconfined)")
    p.add_argument("command", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_run)
    args = parser.parse_args()
    if getattr(args, "ctest", None) and args.ctest[:1] == ["--"]:
        args.ctest = args.ctest[1:]
    global IMAGE_TAG
    args.image_given = args.image is not None
    IMAGE_TAG = args.image or CONFIG_IMAGES.get(getattr(args, "config", None), IMAGE)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
