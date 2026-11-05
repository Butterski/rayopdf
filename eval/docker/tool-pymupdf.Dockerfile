# SPDX-License-Identifier: Apache-2.0
FROM python:3.13-slim
RUN apt-get update && apt-get install -y --no-install-recommends time && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir pymupdf==2.1 \
 && (python3 -c 'import pymupdf, sys; sys.exit(0 if hasattr(pymupdf.Document, "to_markdown") else 1)' \
     || pip install --no-cache-dir pymupdf4llm==2.1)
COPY competitors/convert.py /opt/eval/convert.py
RUN python3 - <<'EOF'
import json, subprocess
v = {}
r = subprocess.run("python3 -c 'import pymupdf; print(pymupdf.VersionBind)'", shell=True, capture_output=True, text=True)
v['pymupdf'] = ((r.stdout or '') + (r.stderr or '')).strip().splitlines()[0] if (r.stdout or r.stderr).strip() else None
out = subprocess.run(['python3', '-m', 'pip', 'freeze'], capture_output=True, text=True, check=True).stdout
v['pip'] = {l.split('==')[0]: l.split('==')[1] for l in out.splitlines() if '==' in l}
json.dump(v, open('/opt/eval/versions.json', 'w'), indent=1, sort_keys=True)
EOF
RUN chmod -R a+rX /opt/eval
