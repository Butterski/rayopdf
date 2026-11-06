#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and test RayoPDF inside the rayopdf-build:1 image (docker/build.Dockerfile).

Nothing is installed on the host: the compilers, CMake and Ninja live in the image, the
repository is mounted at /src, and builds go to build/<name>/ (git-ignored).

Usage:
  python3 tools/dev.py image                       # build rayopdf-build:1
  python3 tools/dev.py build [--config release|debug|asan|static|fuzz] [--dir NAME] [--target T ...]
  python3 tools/dev.py test [--config ...] [--dir NAME] [-- CTEST ARGS]
  python3 tools/dev.py run [--mount HOST:CTR] -- COMMAND ...

Configs: release (GCC, -O2), debug (GCC, -O0 -g), asan (Clang, ASan + UBSan, -O1 -g),
static (GCC release, fully static -static-pie binary for the evaluation harness's `cli` tool),
fuzz (Clang, libFuzzer targets of tests/fuzz). --dir defaults to the config's name.
Builds run with --network none.
"""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IMAGE = "rayopdf-build:1"

CONFIGS = {
    "release": ["-DCMAKE_BUILD_TYPE=Release"],
    "debug": ["-DCMAKE_BUILD_TYPE=Debug"],
    "asan": ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_C_COMPILER=clang",
             "-DCMAKE_CXX_COMPILER=clang++", "-DRAYOPDF_SANITIZE=ON"],
    "static": ["-DCMAKE_BUILD_TYPE=Release", "-DRAYOPDF_STATIC=ON", "-DRAYOPDF_TESTS=OFF"],
    "fuzz": ["-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_C_COMPILER=clang",
             "-DCMAKE_CXX_COMPILER=clang++", "-DRAYOPDF_FUZZ=ON", "-DRAYOPDF_TESTS=OFF"],
}


def docker(argv: list[str], mounts: list[str] | None = None, workdir: str = "/src") -> int:
    cmd = ["docker", "run", "--rm", "--init", "-u", f"{os.getuid()}:{os.getgid()}",
           "-e", "HOME=/tmp", "--network", "none", "-v", f"{REPO}:/src", "-w", workdir]
    for mount in mounts or []:
        host, _, ctr = mount.partition(":")
        cmd += ["-v", f"{Path(host).resolve()}:{ctr}"]
    cmd += [IMAGE] + argv
    print("$ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def build_dir(args) -> str:
    return f"build/{args.dir or args.config}"


def cmd_image(args) -> int:
    return subprocess.run(["docker", "build", "-f", str(REPO / "docker" / "build.Dockerfile"),
                           "-t", IMAGE, str(REPO / "docker")]).returncode


def configure_and_build(args) -> int:
    out = build_dir(args)
    script = (f"cmake -S . -B {out} -G Ninja {' '.join(CONFIGS[args.config])} >/dev/null && "
              f"cmake --build {out}")
    if args.target:
        script += " --target " + " ".join(args.target)
    return docker(["sh", "-ec", script])


def cmd_build(args) -> int:
    return configure_and_build(args)


def cmd_test(args) -> int:
    code = configure_and_build(args)
    if code:
        return code
    extra = " ".join(args.ctest) if args.ctest else ""
    return docker(["sh", "-ec", f"ctest --test-dir {build_dir(args)} --output-on-failure {extra}"])


def cmd_run(args) -> int:
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        raise SystemExit("run needs a command after --")
    return docker(command, mounts=args.mount)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    sub = parser.add_subparsers(dest="command_name", required=True)
    p = sub.add_parser("image", help="build the rayopdf-build:1 image")
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
    p.add_argument("command", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_run)
    args = parser.parse_args()
    if getattr(args, "ctest", None) and args.ctest[:1] == ["--"]:
        args.ctest = args.ctest[1:]
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
