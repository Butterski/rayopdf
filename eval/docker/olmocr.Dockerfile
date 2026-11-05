# SPDX-License-Identifier: Apache-2.0
# olmOCR-bench scorer (plan step 5). Build context: eval/.
FROM python:3.11-slim

ENV PLAYWRIGHT_BROWSERS_PATH=/opt/ms-playwright \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PYTHONDONTWRITEBYTECODE=1

# olmocr 0.4.27 lists numpy only under its "elo" extra, but olmocr.bench.tests imports it.
RUN pip install --no-cache-dir "olmocr[bench]==0.4.27" numpy==2.4.6 \
 && playwright install --with-deps chromium \
 && rm -rf /var/lib/apt/lists/* /root/.cache \
 && chmod -R a+rX /opt/ms-playwright

RUN mkdir -p /opt/eval \
 && python - <<'EOF' \
 && chmod -R a+rX /opt/eval
import json, subprocess
from importlib.metadata import version
pip = {}
for line in subprocess.run(["pip", "freeze"], capture_output=True, text=True, check=True).stdout.splitlines():
    if "==" in line:
        name, ver = line.split("==", 1)
        pip[name] = ver
out = {
    "olmocr": version("olmocr"),
    "playwright": version("playwright"),
    "python": subprocess.run(["python", "--version"], capture_output=True, text=True).stdout.strip(),
    "pip": pip,
}
json.dump(out, open("/opt/eval/versions.json", "w"), indent=1, sort_keys=True)
EOF
