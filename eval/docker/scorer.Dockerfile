# SPDX-License-Identifier: Apache-2.0
# Scorer image: opendataloader-bench evaluator, construct scorer, dataset fetching.
# Build: docker build -f eval/docker/scorer.Dockerfile -t rayopdf-eval-scorer:1 eval
FROM python:3.13-slim

ARG PANDOC_VERSION=3.13

RUN apt-get update \
 && apt-get install -y --no-install-recommends git git-lfs curl time ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 -o /tmp/pandoc.tar.gz \
      "https://github.com/jgm/pandoc/releases/download/${PANDOC_VERSION}/pandoc-${PANDOC_VERSION}-linux-amd64.tar.gz" \
 && tar -xzf /tmp/pandoc.tar.gz -C /opt \
 && ln -s "/opt/pandoc-${PANDOC_VERSION}/bin/pandoc" /usr/local/bin/pandoc \
 && rm -f /tmp/pandoc.tar.gz \
 && pip install --no-cache-dir \
      apted==1.0.3 rapidfuzz==3.14.3 lxml==6.1.0 beautifulsoup4==4.14.3 huggingface_hub==2.4.0

COPY scorer/ /opt/eval/scorer/

RUN python3 - <<'EOF' && chmod -R a+rX /opt/eval
import json, subprocess
def first(cmd):
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return (out.stdout or out.stderr).splitlines()[0].strip()
pip = {}
for line in subprocess.run(["pip", "freeze"], capture_output=True, text=True, check=True).stdout.splitlines():
    if "==" in line:
        name, ver = line.split("==", 1)
        pip[name] = ver
versions = {
    "python": first(["python3", "--version"]),
    "pandoc": first(["pandoc", "--version"]),
    "git": first(["git", "--version"]),
    "git-lfs": first(["git", "lfs", "version"]),
    "pip": pip,
}
with open("/opt/eval/versions.json", "w") as f:
    json.dump(versions, f, indent=1, sort_keys=True)
EOF
