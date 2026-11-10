# SPDX-License-Identifier: Apache-2.0
# Measuring image of tools/bench.py and the performance campaign (docs/development/performance.md):
# the build image (same glibc and libstdc++ as the binaries under test, and its compiler for the
# timers under tools/bench/) plus valgrind (callgrind) and perf. Build it after `tools/dev.py image`:
# `docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker`. perf comes from linux-tools of
# the image's kernel series; call the versioned binary under /usr/lib/linux-tools/*/perf, which
# works against a newer host kernel (and needs a privileged container). libdeflate and zlib
# are reference decoders for decoder benchmarks and differential checks only (roadmap D2: test
# oracles), never linked into RayoPDF.
FROM rayopdf-build:2

RUN apt-get update \
 && apt-get install -y --no-install-recommends valgrind linux-tools-generic time binutils libdeflate-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/* \
 && { valgrind --version; ls /usr/lib/linux-tools/; } >> /opt/build/versions.txt
