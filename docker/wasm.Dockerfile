# SPDX-License-Identifier: Apache-2.0
# WebAssembly image of RayoPDF: Emscripten 4.0.23 (its Clang, system libraries already built into
# the world-writable cache, and Node.js 22 for the tests), CMake, plus Ninja. tools/dev.py builds
# it (`image --name wasm`, tag rayopdf-wasm:1) and runs the `wasm` config in it.
FROM emscripten/emsdk:4.0.23@sha256:86537645c51e44899812d29820ee3b64b96c321ebb2aba4416a04ceeb1bcde62

RUN apt-get update \
 && apt-get install -y --no-install-recommends ninja-build \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /opt/build \
 && { emcc --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"; \
      echo "node $(node --version)"; python3 --version; } > /opt/build/versions.txt \
 && chmod -R a+rX /opt/build
