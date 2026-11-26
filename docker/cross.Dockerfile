# SPDX-License-Identifier: Apache-2.0
# Cross image of RayoPDF: GCC 13.3 for Linux aarch64 (the same GCC release as rayopdf-build:2's)
# with glibc 2.39's aarch64 development files, qemu-user to run what it builds, and Clang 18 with
# libc++ for the x86-64 `libcxx` config (the C++ library of macOS and of the WebAssembly build,
# on Linux). The base is pinned by digest and every package by version; a version the Ubuntu
# archive no longer serves stops the build. tools/dev.py builds it (`image --name cross`, tag
# rayopdf-cross:1) and runs the `arm64` and `libcxx` configs in it.
FROM ubuntu:24.04@sha256:562111b88caa79c71391c0c4d3c2114a7d5c35915c20561fd893cb1e07929e6b

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      g++-13-aarch64-linux-gnu=13.3.0-6ubuntu2~24.04.1cross1 \
      libc6-dev-arm64-cross=2.39-0ubuntu8cross1 \
      qemu-user-static=1:8.2.2+ds-0ubuntu1.18 \
      clang-18=1:18.1.3-1ubuntu1 libc++-18-dev=1:18.1.3-1ubuntu1 libc++abi-18-dev=1:18.1.3-1ubuntu1 \
      cmake=3.28.3-1build7 ninja-build=1.11.1-2 python3=3.12.3-0ubuntu2.1 \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /opt/build \
 && { aarch64-linux-gnu-g++-13 --version | head -1; clang-18 --version | head -1; \
      qemu-aarch64-static --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"; \
      python3 --version; } > /opt/build/versions.txt \
 && chmod -R a+rX /opt/build
# Dynamically linked aarch64 programs (the core tests) find their loader and libraries here.
ENV QEMU_LD_PREFIX=/usr/aarch64-linux-gnu
