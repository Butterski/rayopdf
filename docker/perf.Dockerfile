# SPDX-License-Identifier: Apache-2.0
# Measuring image of the performance campaign (docs/development/performance.md): the build image
# (same glibc and libstdc++ as the binaries under test) plus valgrind (callgrind) and perf. Used
# through the agent skills' dkrun.sh: `dkrun.sh --image rayopdf-perf:1 [--perf] --cpus 3 -- ...`.
# perf comes from linux-tools of the image's kernel series; the scripts call the versioned binary
# under /usr/lib/linux-tools/*/perf, which works against a newer host kernel.
FROM rayopdf-build:2

RUN apt-get update \
 && apt-get install -y --no-install-recommends valgrind linux-tools-generic time binutils \
 && rm -rf /var/lib/apt/lists/* \
 && { valgrind --version; ls /usr/lib/linux-tools/; } >> /opt/build/versions.txt
