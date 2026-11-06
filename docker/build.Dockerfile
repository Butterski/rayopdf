# SPDX-License-Identifier: Apache-2.0
# Build image of RayoPDF: GCC 13 for release builds, Clang 18 for AddressSanitizer, UBSan and
# libFuzzer, CMake and Ninja. tools/dev.py builds it as rayopdf-build:1 and runs every build in it.
FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build clang-18 lld-18 libclang-rt-18-dev llvm-18 \
      python3 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && ln -s /usr/bin/clang-18 /usr/local/bin/clang \
 && ln -s /usr/bin/clang++-18 /usr/local/bin/clang++ \
 && ln -s /usr/bin/llvm-symbolizer-18 /usr/local/bin/llvm-symbolizer \
 && mkdir -p /opt/build \
 && { gcc --version | head -1; clang --version | head -1; cmake --version | head -1; \
      echo "ninja $(ninja --version)"; python3 --version; } > /opt/build/versions.txt \
 && chmod -R a+rX /opt/build
