# SPDX-License-Identifier: Apache-2.0
# kordoc (https://github.com/chrisryugj/kordoc, MIT) with its optional PDF and OCR dependencies.
# The text OCR models (PP-OCRv5 korean det + rec + dictionary) are fetched at build time into
# /opt/kordoc/models; `kordoc-ocr` runs point KORDOC_MODEL_CACHE there, `kordoc` runs pass
# ocr: false. Every run is offline (KORDOC_OFFLINE=1, --network none).
FROM node:22-bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends python3 time \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/kordoc
ENV ONNXRUNTIME_NODE_INSTALL_CUDA=skip
RUN npm init -y > /dev/null && npm install --no-audit --no-fund --save-exact kordoc@4.24.66
RUN KORDOC_MODEL_CACHE=/opt/kordoc/models node node_modules/kordoc/dist/cli.js check-ocr-models
COPY competitors/kordoc_run.mjs /opt/kordoc/run.mjs
COPY competitors/convert.py /opt/eval/convert.py
RUN node - <<'EOF'
const { execSync } = require("child_process")
const fs = require("fs")
const tree = JSON.parse(execSync("npm ls --all --json", { cwd: "/opt/kordoc" }).toString())
const resolved = {}
const walk = (deps) => {
  for (const [name, d] of Object.entries(deps || {})) {
    if (d.version) resolved[name] = d.version
    walk(d.dependencies)
  }
}
walk(tree.dependencies)
const models = fs.readdirSync("/opt/kordoc/models/ppocr").sort()
fs.writeFileSync("/opt/eval/versions.json", JSON.stringify({
  kordoc: require("/opt/kordoc/node_modules/kordoc/package.json").version,
  node: process.version, npm: resolved, ocr_models: models,
}, null, 1))
EOF
RUN chmod -R a+rX /opt/eval /opt/kordoc
