# SPDX-License-Identifier: Apache-2.0
# OpenJPEG (BSD-2-Clause), the reference JPEG 2000 decoder of RayoPDF's JPX decoder
# (src/image/jpx.h) and the encoder of its test vectors, built from the pinned v2.5.4 release
# without LittleCMS (no ICC conversion), twice:
# - /usr/local: opj_decompress, opj_compress, opj_dump and jpx_ref built as distributions build
#   them (CMAKE_BUILD_TYPE None, -O2): strict IEEE arithmetic, the reference for the samples.
# - /opt/openjpeg-release: OpenJPEG's own Release configuration (-O3 -ffast-math) and
#   jpx_ref_fast linked to it, the reference for speed (its 9/7 samples may differ by 1).
# jpx_ref (openjpeg/jpx_ref.c) writes the library's decoded components as int32 planes;
# make_jpx_vectors.py makes the test vectors of tests/test_image_jpx.cpp.
# Built by hand, not by tools/eval.py images:
#   docker build -f eval/docker/openjpeg.Dockerfile -t rayopdf-eval-openjpeg:1 eval/docker
FROM debian:12-slim

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl cmake make gcc \
        libc6-dev python3 time \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /tmp/openjpeg.tar.gz https://github.com/uclouvain/openjpeg/archive/refs/tags/v2.5.4.tar.gz \
    && echo "a695fbe19c0165f295a8531b1e4e855cd94d0875d2f88ec4b61080677e27188a  /tmp/openjpeg.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/openjpeg.tar.gz -C /tmp \
    && for config in None:/usr/local Release:/opt/openjpeg-release; do \
        cmake -S /tmp/openjpeg-2.5.4 -B /tmp/opj-build -DCMAKE_BUILD_TYPE=${config%%:*} -DCMAKE_C_FLAGS=-O2 \
            -DBUILD_SHARED_LIBS=OFF -DBUILD_CODEC=ON -DBUILD_THIRDPARTY=OFF \
            -DCMAKE_DISABLE_FIND_PACKAGE_LCMS2=ON -DCMAKE_DISABLE_FIND_PACKAGE_LCMS=ON \
            -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON -DCMAKE_DISABLE_FIND_PACKAGE_TIFF=ON \
            -DCMAKE_INSTALL_PREFIX=${config#*:} \
        && cmake --build /tmp/opj-build -j4 && cmake --install /tmp/opj-build && rm -rf /tmp/opj-build; \
    done \
    && rm -rf /tmp/openjpeg.tar.gz /tmp/openjpeg-2.5.4

COPY openjpeg/jpx_ref.c /opt/eval/jpx_ref.c
RUN gcc -O2 -o /usr/local/bin/jpx_ref /opt/eval/jpx_ref.c -I/usr/local/include/openjpeg-2.5 \
        /usr/local/lib/libopenjp2.a -lm -lpthread \
    && gcc -O2 -o /usr/local/bin/jpx_ref_fast /opt/eval/jpx_ref.c -I/opt/openjpeg-release/include/openjpeg-2.5 \
        /opt/openjpeg-release/lib/libopenjp2.a -lm -lpthread \
    && printf '{"openjpeg": "%s", "source_sha256": "a695fbe19c0165f295a8531b1e4e855cd94d0875d2f88ec4b61080677e27188a"}\n' \
        "$(opj_decompress -h 2>&1 | grep -o 'v2\.[0-9]*\.[0-9]*' | head -1)" > /opt/eval/versions.json \
    && chmod -R a+rX /opt/eval
