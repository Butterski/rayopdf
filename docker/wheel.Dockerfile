# SPDX-License-Identifier: Apache-2.0
# Wheel image of RayoPDF: manylinux_2_28 (AlmaLinux 8, glibc 2.28) with its GCC toolset (GCC 14),
# CMake, Python and objdump, plus Ninja. Libraries built here need no glibc newer than 2.28, which
# the manylinux_2_28_x86_64 wheel promises (tests/verify_python.py checks it). tools/dev.py builds
# it (`image --name wheel`, tag rayopdf-wheel:1) and runs the `wheel` config in it.
FROM quay.io/pypa/manylinux_2_28_x86_64@sha256:9555ba6c479fa69a2aa80ad1dc57154a60d562b19df159361d9379a02beb1e36

RUN /opt/python/cp312-cp312/bin/python -m pip install --no-cache-dir ninja==1.13.2 \
 && ln -s /opt/python/cp312-cp312/bin/ninja /usr/local/bin/ninja \
 && mkdir -p /opt/build \
 && { gcc --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"; \
      python3 --version; objdump --version | head -1; ldd --version | head -1; } > /opt/build/versions.txt \
 && chmod -R a+rX /opt/build
