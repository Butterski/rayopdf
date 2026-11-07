# SPDX-License-Identifier: Apache-2.0
# Build image of RayoPDF: GCC 13 for release builds, Clang 18 for AddressSanitizer, UBSan,
# ThreadSanitizer and libFuzzer, MinGW-w64 (GCC 13, POSIX thread model) for the Windows build,
# Wine to run the Windows binaries, CMake and Ninja. tools/dev.py builds it (`image`, tag
# rayopdf-build:2) and runs every build in it.
FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build clang-18 lld-18 libclang-rt-18-dev llvm-18 \
      g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix wine64 \
      python3 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/bin/clang-18 /usr/local/bin/clang \
 && ln -s /usr/bin/clang++-18 /usr/local/bin/clang++ \
 && ln -s /usr/bin/llvm-symbolizer-18 /usr/local/bin/llvm-symbolizer \
 && printf '%s\n' '#!/bin/sh' \
      '# Wine for the Windows build: a prefix of the calling user under /tmp (which root owns, so' \
      '# Wine would refuse to make one in it directly), and the 64-bit loader by its real path.' \
      'set -e' \
      'base="/tmp/wine-$(id -u)"' \
      'mkdir -p "$base"' \
      'export WINEPREFIX="${WINEPREFIX:-$base/prefix}"' \
      'export WINEDEBUG="${WINEDEBUG:--all}"' \
      'exec /usr/lib/wine/wine64 "$@"' > /usr/local/bin/wine \
 && chmod 755 /usr/local/bin/wine \
 && mkdir -p /opt/build \
 && { gcc --version | head -1; clang --version | head -1; x86_64-w64-mingw32-g++-posix --version | head -1; \
      wine --version; cmake --version | head -1; echo "ninja $(ninja --version)"; python3 --version; } \
      > /opt/build/versions.txt \
 && chmod -R a+rX /opt/build
