# SPDX-License-Identifier: Apache-2.0
# Training image of the small models (roadmap M9, eval/train/): LightGBM and NumPy on Python 3.13.
# The repository's eval/train is mounted over /opt/eval/train at run time; training runs with
# --network none, one thread and a fixed seed, so two runs give the same model bytes.
# Build: docker build -f eval/docker/train.Dockerfile -t rayopdf-eval-train:1 eval
FROM python:3.13-slim@sha256:09a17cad02668ae61ece136b4fd36ac3849e14f670ebedb637ea3e15580525bc

RUN apt-get update \
 && apt-get install -y --no-install-recommends libgomp1 \
 && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir lightgbm==4.7.0 numpy==2.5.4

RUN python3 - <<'EOF' && chmod -R a+rX /opt/eval
import json, os, subprocess
pip = {}
for line in subprocess.run(["pip", "freeze"], capture_output=True, text=True, check=True).stdout.splitlines():
    if "==" in line:
        name, version = line.split("==", 1)
        pip[name] = version
versions = {
    "python": subprocess.run(["python3", "--version"], capture_output=True, text=True, check=True).stdout.strip(),
    "pip": pip,
}
os.makedirs("/opt/eval", exist_ok=True)
with open("/opt/eval/versions.json", "w") as f:
    json.dump(versions, f, indent=1, sort_keys=True)
EOF
