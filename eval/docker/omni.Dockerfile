# SPDX-License-Identifier: Apache-2.0
# OmniParseBench scorer image: Datalab's omni-parse-bench (Apache-2.0) and its unit-test grader
# (`opb grade`), with pyarrow for the dataset's manifests. The repository's eval/scorer is mounted
# over /opt/eval/scorer at run time (omni_split.py, omni_predict.py).
# Build: docker build -f eval/docker/omni.Dockerfile -t rayopdf-eval-omni:1 eval
FROM python:3.13-slim@sha256:09a17cad02668ae61ece136b4fd36ac3849e14f670ebedb637ea3e15580525bc

RUN pip install --no-cache-dir omni-parse-bench==1.5.0 omni-bench-core==0.14.1

COPY scorer/ /opt/eval/scorer/

RUN python3 - <<'EOF' && chmod -R a+rX /opt/eval
import json, subprocess
pip = {}
for line in subprocess.run(["pip", "freeze"], capture_output=True, text=True, check=True).stdout.splitlines():
    if "==" in line:
        name, version = line.split("==", 1)
        pip[name] = version
versions = {
    "python": subprocess.run(["python3", "--version"], capture_output=True, text=True, check=True).stdout.strip(),
    "pip": pip,
}
with open("/opt/eval/versions.json", "w") as f:
    json.dump(versions, f, indent=1, sort_keys=True)
EOF
