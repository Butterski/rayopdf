# syntax=docker/dockerfile:1
# SPDX-License-Identifier: Apache-2.0
# Build image of the OCR add-on rayopdf-ocr (docs/development/ocr-engine.md): the build image plus
# the inference runtime ncnn, built from pinned source for Linux (GCC, position-independent, for
# the -static-pie add-on) and Windows (MinGW-w64), and the PP-OCRv6 models converted for it.
# tools/dev.py builds it (`image --name ocr`, tag rayopdf-ocr:2) and runs the `ocr` and
# `ocr-windows` configs in it. Nothing here is linked into the core `rayopdf`.
#
#   /opt/ocr/ncnn/{linux,windows}/   ncnn 20261020 (BSD-3-Clause), static, no OpenMP, no Vulkan
#   /opt/ocr/models/<tier>/          PP-OCRv6 tiny/small (Apache-2.0), docker/ocr_models.py
#   /opt/ocr/models/scripts/<script>/ the script pack: PP-OCRv5 cyrillic, korean, arabic,
#                                    devanagari, tamil and PP-OCRv3 kannada recognisers (Apache-2.0),
#                                    half-precision weights
#   /opt/ocr/models/orient/          PP-LCNet_x0_25_textline_ori (Apache-2.0)
#   /opt/ocr/versions.json           every pinned input and its SHA-256

# ---- Kannada: PaddleOCR publishes its Kannada recogniser (PP-OCRv3) in Paddle format only ----
FROM python:3.11-slim AS kannada
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1=14.2.0-19 && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir paddlepaddle==3.3.1 paddle2onnx==2.1.0
RUN mkdir -p /opt/ka/model && cd /opt/ka/model && python3 - <<'EOF'
import hashlib, urllib.request
base = "https://huggingface.co/PaddlePaddle/ka_PP-OCRv3_mobile_rec/resolve/fd08732bad5fb532cf883cb42a75ed139a72ab18/"
for name, digest in (("inference.json", "3be0576413bd881ed088dbaf7174c7a32fda9d000fa94cbf687727347fb123cc"),
                     ("inference.pdiparams", "5abb4f1c00e5fab314b06d55f69b137ee79bef3f59b547166a48e93bd530ff12"),
                     ("inference.yml", "948eb2cdc413b7754a3c41e1b4902fbab5478f0bb0f1b9d814f5b08c7bb51765")):
    data = urllib.request.urlopen(base + name, timeout=300).read()
    assert hashlib.sha256(data).hexdigest() == digest, name
    open(name, "wb").write(data)
EOF
# docker/ocr_models.py checks the converted model's SHA-256.
RUN paddle2onnx --model_dir /opt/ka/model --model_filename inference.json --params_filename inference.pdiparams \
      --save_file /opt/ka/ka.onnx --opset_version 14 \
 && cp /opt/ka/model/inference.yml /opt/ka/inference.yml && pip freeze > /opt/ka/pip-freeze.txt

# ---- Models: download, check, convert to ncnn through pnnx ----
FROM python:3.11-slim AS models
RUN pip install --no-cache-dir onnx==1.23.2 numpy==2.4.6 pyyaml==6.0.3 \
      protobuf==7.36.2 ml_dtypes==0.6.0 packaging==26.3 typing_extensions==4.16.0
RUN python3 - <<'EOF'
import hashlib, io, os, urllib.request, zipfile
url = "https://github.com/pnnx/pnnx/releases/download/20261020/pnnx-20261020-linux.zip"
data = urllib.request.urlopen(url, timeout=300).read()
assert hashlib.sha256(data).hexdigest() == "6cfff631178e02e62542e72bf14e0045dfce62095bbc1f633176e9c0778ab8a6", url
zipfile.ZipFile(io.BytesIO(data)).extract("pnnx-20261020-linux/pnnx", "/opt")
os.chmod("/opt/pnnx-20261020-linux/pnnx", 0o755)
EOF
COPY ocr_models.py /opt/ocr/ocr_models.py
COPY --from=kannada /opt/ka/ka.onnx /opt/ka/inference.yml /opt/ka/pip-freeze.txt /opt/ka/
RUN python3 /opt/ocr/ocr_models.py --out /opt/ocr/models --pnnx /opt/pnnx-20261020-linux/pnnx --work /tmp/models \
      --kannada /opt/ka \
 && rm -rf /tmp/models

# ---- Sources ----
FROM rayopdf-build:2 AS sources
RUN mkdir -p /src3 && cd /src3 && python3 - <<'EOF'
import hashlib, io, urllib.request, zipfile
url = "https://github.com/Tencent/ncnn/releases/download/20261020/ncnn-20261020-full-source.zip"
data = urllib.request.urlopen(url, timeout=600).read()
assert hashlib.sha256(data).hexdigest() == "699d9acc67489eee0993a551ca1ebc247bbf0f4e33babe1dec810e6a9a52b988", url
zipfile.ZipFile(io.BytesIO(data)).extractall("ncnn")
EOF

# ---- ncnn, Linux and Windows ----
# Only the layers the networks use are built (plus the ones ncnn's own layers create inside:
# padding, packing, casts, activations, ...), which roughly halves the add-on executable.
FROM sources AS ncnn
COPY --from=models /opt/ocr/models /tmp/models
RUN python3 - <<'EOF' > /tmp/ncnn-layers.cmake
import glob, re
used = {line.split()[0] for path in glob.glob("/tmp/models/**/*.ncnn.param", recursive=True)
        for line in open(path).read().splitlines()[2:] if line.split()}
used |= {"Padding", "Packing", "Cast", "Flatten", "Crop", "Slice", "Eltwise", "Split", "Concat", "Reshape",
         "Permute", "ReLU", "LeakyReLU", "Clip", "Sigmoid", "Mish", "HardSwish", "Swish", "TanH", "Softmax",
         "Gemm", "MatMul", "InnerProduct", "Convolution", "ConvolutionDepthWise", "Deconvolution",
         "DeconvolutionDepthWise", "Pooling", "BinaryOp", "UnaryOp", "Dequantize", "Requantize", "Quantize"}
names = re.findall(r"^ncnn_add_layer\((\w+)", open("/src3/ncnn/src/CMakeLists.txt").read(), re.M)
print(" ".join(f"-DWITH_LAYER_{n.lower()}=OFF" for n in names if n not in used))
EOF
RUN set -e; \
    common="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DNCNN_SHARED_LIB=OFF \
      -DNCNN_VULKAN=OFF -DNCNN_OPENMP=OFF -DNCNN_SIMPLEOCV=OFF -DNCNN_PIXEL=OFF -DNCNN_PIXEL_ROTATE=OFF \
      -DNCNN_PIXEL_AFFINE=OFF -DNCNN_PIXEL_DRAWING=OFF -DNCNN_INT8=OFF -DNCNN_BF16=OFF \
      -DNCNN_DISABLE_RTTI=ON -DNCNN_DISABLE_EXCEPTION=ON -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF \
      -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF -DNCNN_PYTHON=OFF $(cat /tmp/ncnn-layers.cmake)"; \
    cmake -S /src3/ncnn -B /tmp/ncnn-linux $common -DCMAKE_INSTALL_PREFIX=/opt/ocr/ncnn/linux; \
    cmake --build /tmp/ncnn-linux -j 6 && cmake --install /tmp/ncnn-linux; \
    cp /tmp/ncnn-layers.cmake /opt/ocr/ncnn/layers-off.txt; \
    cmake -S /src3/ncnn -B /tmp/ncnn-windows $common -DCMAKE_INSTALL_PREFIX=/opt/ocr/ncnn/windows \
      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix; \
    cmake --build /tmp/ncnn-windows -j 6 && cmake --install /tmp/ncnn-windows; \
    cp /src3/ncnn/LICENSE.txt /opt/ocr/ncnn/LICENSE.txt; rm -rf /tmp/ncnn-linux /tmp/ncnn-windows

# ---- The image ----
FROM rayopdf-build:2
COPY --from=ncnn /opt/ocr/ncnn /opt/ocr/ncnn
COPY --from=models /opt/ocr/models /opt/ocr/models
COPY --from=models /opt/ka/pip-freeze.txt /opt/ocr/kannada-converter-pip-freeze.txt
RUN python3 - <<'EOF'
import glob, json, os
versions = {
    "ncnn": {"version": "20261020", "source": "ncnn-20261020-full-source.zip",
             "sha256": "699d9acc67489eee0993a551ca1ebc247bbf0f4e33babe1dec810e6a9a52b988"},
    "pnnx": {"version": "20261020", "sha256": "6cfff631178e02e62542e72bf14e0045dfce62095bbc1f633176e9c0778ab8a6"},
    "kannada converter": open("/opt/ocr/kannada-converter-pip-freeze.txt").read().split(),
    "models": {},
}
folders = ["tiny", "small", "orient"] + sorted(
    os.path.relpath(p, "/opt/ocr/models") for p in glob.glob("/opt/ocr/models/scripts/*"))
for folder in folders:
    versions["models"][folder] = {line.split()[2]: line.split()[0]
                                  for line in open(f"/opt/ocr/models/{folder}/models.txt")}
json.dump(versions, open("/opt/ocr/versions.json", "w"), indent=1, sort_keys=True)
EOF
RUN chmod -R a+rX /opt/ocr
